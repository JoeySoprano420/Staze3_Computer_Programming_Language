#include "staze/llvm_x64.hpp"
#include "staze/diagnostic.hpp"

#include <algorithm>
#include <cctype>
#include <cstdint>
#include <limits>
#include <map>
#include <set>
#include <tuple>
#include <sstream>
#include <string>
#include <unordered_map>
#include <vector>

namespace staze {
namespace {

std::string safe_name(std::string s) {
    for (char& c : s) if (!std::isalnum(static_cast<unsigned char>(c)) && c != '_') c = '_';
    return s;
}

std::string llvm_ty(SirType t) {
    switch (t) {
        case SirType::Bool: return "i1";
        case SirType::I32:
        case SirType::U32: return "i32";
        case SirType::I64:
        case SirType::U64: return "i64";
        case SirType::Unit: return "i8";
        case SirType::Text:
        case SirType::Nominal: return "ptr";
    }
    return "i8";
}

std::string escape_bytes(const std::string& s) {
    std::ostringstream o;
    const char* hex = "0123456789ABCDEF";
    for (unsigned char c : s) {
        if (c >= 32 && c <= 126 && c != '"' && c != '\\') o << static_cast<char>(c);
        else o << '\\' << hex[c >> 4] << hex[c & 15];
    }
    return o.str();
}

struct Def {
    SirType type{SirType::Unit};
    SirCardinality cardinality{SirCardinality::One};
    std::string nominal;
    const SirInstruction* op{nullptr};
    bool parameter{false};
    std::size_t parameter_index{0};
};

const SirConcreteFunction& concrete_function(const SirCProgram& p, const std::string& name) {
    auto it = std::find_if(p.concrete_functions.begin(), p.concrete_functions.end(), [&](const auto& x){ return x.name == name; });
    if (it == p.concrete_functions.end()) throw CompileError({}, "LLVM backend: missing concrete function plan for '" + name + "'");
    return *it;
}

const SirConcreteLayout& concrete_layout(const SirCProgram& p, const std::string& name) {
    auto it = std::find_if(p.concrete_layouts.begin(), p.concrete_layouts.end(), [&](const auto& x){ return x.name == name; });
    if (it == p.concrete_layouts.end()) throw CompileError({}, "LLVM backend: missing concrete layout for '" + name + "'");
    return *it;
}

const SirPoolSchema* pool_schema(const SirCProgram& p, const std::string& name) {
    auto it=std::find_if(p.pools.begin(),p.pools.end(),[&](const auto&x){return x.name==name;});return it==p.pools.end()?nullptr:&*it;
}
const SirFaultIdentity* fault_schema(const SirCProgram& p, const std::string& name) {
    auto it=std::find_if(p.faults.begin(),p.faults.end(),[&](const auto&x){return x.name==name;});return it==p.faults.end()?nullptr:&*it;
}

std::uint32_t align_up(std::uint32_t v, std::uint32_t a) {
    if (a <= 1) return v;
    return (v + a - 1u) & ~(a - 1u);
}

class FunctionEmitter {
public:
    FunctionEmitter(const SirCProgram& program,
                    const SirFunction& fn,
                    const std::map<std::string, std::size_t>& strings)
        : program_(program), fn_(fn), concrete_(concrete_function(program, fn.name)), strings_(strings) {
        for (std::size_t i = 0; i < fn.parameters.size(); ++i)
            defs_[fn.parameters[i].value] = Def{fn.parameters[i].type, fn.parameters[i].cardinality, fn.parameters[i].nominal_type, nullptr, true, i};
        for (const auto& b : fn.blocks)
            for (const auto& op : b.instructions)
                defs_[op.result] = Def{op.type, op.cardinality, op.nominal_type, &op, false, 0};
        for (const auto& cv : concrete_.values) concrete_values_[cv.value] = &cv;
        for (const auto& vs : fn_.values) value_semantics_[vs.value] = &vs;
        for (const auto& pr : fn_.provenances) provenances_[pr.id] = &pr;
        for (const auto& r : concrete_.resources) {
            resources_[r.id] = &r;
            resource_by_value_[r.value] = &r;
            resource_by_provenance_[r.provenance] = &r;
        }
        for (const auto& u : concrete_.undo_actions) {
            undo_by_source_[u.source_op].push_back(&u);
            undo_by_transaction_[u.transaction_region].push_back(&u);
            if (u.kind == SirUndoKind::DestroyResource || u.kind == SirUndoKind::SharedRelease)
                undo_tracked_values_.insert(u.target_value);
        }
    }

    std::string emit() {
        out_ << "define " << function_return_type() << " @stz_" << safe_name(fn_.name) << "(";
        bool first = true;
        if (rich_result()) { out_ << "ptr %result_out"; first = false; }
        if (!first) out_ << ", ";
        out_ << "ptr %fault_out"; first = false;
        for (std::size_t i = 0; i < fn_.parameters.size(); ++i) {
            out_ << ", " << (is_rich(fn_.parameters[i].value) ? "ptr" : "i64") << " %arg" << i;
        }
        out_ << ") {\nprologue:\n";

        // Fixed-size stack/caller temporaries are hoisted to the prologue.  This
        // keeps the Windows x64 frame static and avoids backend-invented dynamic
        // alloca/probe behavior for ordinary rich SSA values.
        for (const auto& p : concrete_.values) {
            if (p.scalar_replaced) continue;
            if (p.storage == SirStorageClass::Stack || p.storage == SirStorageClass::PoolArena) {
                out_ << "  " << value_name(p.value) << " = alloca [" << p.size << " x i8], align " << p.align << "\n";
                preallocated_[p.value] = true;
            }
        }

        // Resource ownership slots are the runtime realization of SIR-C cleanup obligations.
        // Null means this static allocation site currently owns no heap buffer.
        for (const auto& r : concrete_.resources) {
            const bool tracked_for_undo = undo_tracked_values_.contains(r.value);
            if (!r.automatic && !tracked_for_undo) continue;
            if (r.cleanup == SirCleanupKind::HeapFree || r.cleanup == SirCleanupKind::ManyBufferFree || r.cleanup == SirCleanupKind::SharedRelease) {
                out_ << "  " << resource_slot(r.id) << " = alloca ptr, align 8\n";
                out_ << "  store ptr null, ptr " << resource_slot(r.id) << ", align 8\n";
            }
        }

        // Transaction undo storage is concrete SIR-C state.  Active bits make
        // branch-sensitive rollback precise; restore-field records additionally
        // reserve exact target-sized snapshot bytes.
        for (const auto& u : concrete_.undo_actions) {
            out_ << "  " << undo_active(u.id) << " = alloca i1, align 1\n";
            out_ << "  store i1 false, ptr " << undo_active(u.id) << ", align 1\n";
            if (u.kind == SirUndoKind::RestoreField)
                out_ << "  " << undo_bytes(u.id) << " = alloca [" << u.size << " x i8], align " << u.align << "\n";
        }

        for (std::size_t i = 0; i < fn_.parameters.size(); ++i) {
            const auto& p = fn_.parameters[i];
            const auto name = value_name(p.value);
            if (is_rich(p.value)) {
                out_ << "  " << name << " = getelementptr i8, ptr %arg" << i << ", i64 0\n";
                continue;
            }
            switch (p.type) {
                case SirType::I64:
                case SirType::U64: out_ << "  " << name << " = add i64 %arg" << i << ", 0\n"; break;
                case SirType::I32:
                case SirType::U32: out_ << "  " << name << " = trunc i64 %arg" << i << " to i32\n"; break;
                case SirType::Bool: out_ << "  " << name << " = trunc i64 %arg" << i << " to i1\n"; break;
                case SirType::Unit: out_ << "  " << name << " = add i8 0, 0\n"; break;
                case SirType::Text:
                case SirType::Nominal: throw CompileError({}, "LLVM backend: concrete plan marked rich parameter as register");
            }
        }
        out_ << "  br label %" << block_name(fn_.entry) << "\n";
        for (const auto& b : fn_.blocks) emit_block(b);
        emit_cleanup_edge_blocks();
        out_ << "}\n\n";
        return out_.str();
    }

private:
    std::string block_name(SirBlockId id) const { return "b" + std::to_string(id); }
    std::string value_name(SirValueId id) const { return "%v" + std::to_string(id); }
    std::string temp(SirOpId id, const std::string& suffix) const { return "%o" + std::to_string(id) + "." + suffix; }
    std::string local_label(SirBlockId bid, SirOpId op, const std::string& suffix) const { return "b" + std::to_string(bid) + ".o" + std::to_string(op) + "." + suffix; }

    bool rich_result() const { return concrete_.result_storage == SirStorageClass::Caller; }
    bool transferred_result() const { return concrete_.result_transfer != SirResultTransferKind::None; }
    std::string function_return_type() const { return rich_result() ? "i32" : "{ i32, i64 }"; }

    const SirConcreteValue& cv(SirValueId id) const {
        auto it = concrete_values_.find(id);
        if (it == concrete_values_.end()) throw CompileError({}, "LLVM backend: missing concrete value plan for %" + std::to_string(id));
        return *it->second;
    }
    bool is_rich(SirValueId id) const { return cv(id).storage != SirStorageClass::Register; }
    SirType value_type(SirValueId id) const {
        auto it=defs_.find(id);if(it==defs_.end())throw CompileError({},"LLVM backend: undefined SIR SSA value");return it->second.type;
    }

    int fault_code(const std::string& name) const {
        for (const auto& f : program_.faults) if (f.name == name) return f.code;
        throw CompileError({}, "LLVM backend: missing fault code for '" + name + "'");
    }
    SirBlockId fault_target(const SirInstruction& op, const std::string& name) const {
        for (const auto& e : op.fault_edges) if (e.fault == name) return e.target;
        throw CompileError({}, "LLVM backend: SIR operation lacks explicit fault edge for '" + name + "'");
    }

    std::string scalar(SirValueId id) const {
        auto it = defs_.find(id);
        if (it == defs_.end()) throw CompileError({}, "LLVM backend: undefined SIR SSA value");
        if (is_rich(id)) throw CompileError({}, "LLVM backend: rich SSA value requested as scalar");
        const auto& d = it->second;
        if (d.parameter) return value_name(id);
        const auto& op = *d.op;
        switch (op.opcode) {
            case SirOpcode::ConstUnit: return "0";
            case SirOpcode::ConstInt: return std::to_string(op.int_value);
            case SirOpcode::ConstBool: return op.bool_value ? "1" : "0";
            case SirOpcode::RevisionMarker:
            case SirOpcode::MoveValue:
            case SirOpcode::SendValue:
            case SirOpcode::Borrow:
            case SirOpcode::BorrowMut: return scalar(op.operands.at(0));
            case SirOpcode::ConstText: throw CompileError({}, "LLVM backend: text value requested as scalar");
            default: return value_name(id);
        }
    }

    std::string static_name(SirValueId id) const { return "@.stz.static." + safe_name(fn_.name) + ".v" + std::to_string(id); }
    std::string resource_slot(std::uint32_t id) const { return "%resource." + std::to_string(id) + ".slot"; }
    std::string undo_active(std::uint32_t id) const { return "%undo." + std::to_string(id) + ".active"; }
    std::string undo_bytes(std::uint32_t id) const { return "%undo." + std::to_string(id) + ".bytes"; }
    std::string undo_label(SirOpId abort_op,std::uint32_t id,const std::string& suffix) const { return "undo.o"+std::to_string(abort_op)+".u"+std::to_string(id)+"."+suffix; }
    std::string cleanup_label(SirBlockId from, SirBlockId to, SirOpId source) const {
        return "cleanup.b" + std::to_string(from) + ".to" + std::to_string(to) + ".op" + std::to_string(source);
    }
    std::string rich_ptr(SirValueId id) const {
        const auto& p=cv(id);
        if(p.scalar_replaced) return "null";
        if(p.storage==SirStorageClass::Static) return static_name(id);
        return value_name(id);
    }

    std::pair<std::size_t, std::size_t> text_constant(SirValueId id) const {
        auto it = defs_.find(id);
        if (it == defs_.end() || !it->second.op) throw CompileError({}, "LLVM backend: write_text operand is not a constant text SIR value");
        const auto* op = it->second.op;
        if (op->opcode == SirOpcode::RevisionMarker) return text_constant(op->operands.at(0));
        if (op->opcode != SirOpcode::ConstText) throw CompileError({}, "LLVM backend: write_text requires a constant text value in Compiler 0.8");
        auto si = strings_.find(op->text_value);
        if (si == strings_.end()) throw CompileError({}, "LLVM backend: text constant was not interned");
        return {si->second, op->text_value.size()};
    }

    std::string box(SirValueId id) {
        const auto t = value_type(id);
        const auto v = scalar(id);
        if (t == SirType::I64 || t == SirType::U64) return v;
        if (t == SirType::Unit) return "0";
        const auto r = "%box" + std::to_string(box_counter_++);
        if (t == SirType::I32) out_ << "  " << r << " = sext i32 " << v << " to i64\n";
        else if (t == SirType::U32 || t == SirType::Bool) out_ << "  " << r << " = zext " << llvm_ty(t) << " " << v << " to i64\n";
        else throw CompileError({}, "LLVM backend: scalar ABI cannot box rich/text values");
        return r;
    }

    void unbox_to(SirValueId result, SirType t, const std::string& payload) {
        if (t == SirType::Unit) return;
        if (t == SirType::I64 || t == SirType::U64) out_ << "  " << value_name(result) << " = add i64 " << payload << ", 0\n";
        else if (t == SirType::I32 || t == SirType::U32) out_ << "  " << value_name(result) << " = trunc i64 " << payload << " to i32\n";
        else if (t == SirType::Bool) out_ << "  " << value_name(result) << " = trunc i64 " << payload << " to i1\n";
        else throw CompileError({}, "LLVM backend: scalar ABI cannot unbox rich/text values");
    }

    void emit_alloca(SirValueId id) {
        const auto& p=cv(id); if(p.scalar_replaced) return;
        if (preallocated_.contains(id)) return;
        out_ << "  " << value_name(id) << " = alloca [" << p.size << " x i8], align " << p.align << "\n";
        preallocated_[id] = true;
    }
    std::string byte_ptr(const std::string& base, std::uint32_t offset, const std::string& name) {
        if(offset==0) return base;
        out_ << "  " << name << " = getelementptr i8, ptr " << base << ", i64 " << offset << "\n";
        return name;
    }
    void emit_memcpy(const std::string& dst, const std::string& src, std::uint32_t bytes) {
        if(bytes==0 || dst=="null" || src=="null") return;
        out_ << "  call void @llvm.memcpy.p0.p0.i64(ptr " << dst << ", ptr " << src << ", i64 " << bytes << ", i1 false)\n";
    }
    void store_value(const std::string& base, std::uint32_t offset, SirValueId value, std::uint32_t bytes, std::uint32_t align, const std::string& stem) {
        auto ptr=byte_ptr(base,offset,stem+".ptr");
        if(is_rich(value)) { emit_memcpy(ptr,rich_ptr(value),std::min(bytes,cv(value).size)); return; }
        const auto t=value_type(value);
        out_ << "  store " << llvm_ty(t) << ' ' << scalar(value) << ", ptr " << ptr << ", align " << std::max<std::uint32_t>(1,std::min<std::uint32_t>(align,8)) << "\n";
    }

    const SirConcreteResource* resource_for_value(SirValueId id) const {
        auto si=value_semantics_.find(id); if(si==value_semantics_.end()) return nullptr;
        auto prov=si->second->provenance;
        std::set<SirProvenanceId> seen;
        while(prov && seen.insert(prov).second){
            auto ri=resource_by_provenance_.find(prov); if(ri!=resource_by_provenance_.end()) return ri->second;
            auto pi=provenances_.find(prov); if(pi==provenances_.end()) break; prov=pi->second->parent;
        }
        return nullptr;
    }
    std::vector<const SirCleanupAction*> cleanup_actions(SirBlockId from, SirBlockId to, SirOpId source, bool fault_exit=false) const {
        std::vector<const SirCleanupAction*> out;
        for(const auto& a:concrete_.cleanup_actions) if(a.from_block==from&&a.to_block==to&&a.source_op==source&&a.fault_exit==fault_exit) out.push_back(&a);
        return out;
    }
    std::string edge_label(SirBlockId from, SirBlockId to, SirOpId source=0) const {
        return cleanup_actions(from,to,source).empty()?block_name(to):cleanup_label(from,to,source);
    }
    std::string fault_edge_label(const SirBlock& b,const SirInstruction& op,const std::string& fault) const {
        return edge_label(b.id,fault_target(op,fault),op.id);
    }
    void emit_cleanup_action(const SirCleanupAction& a) {
        auto ri=resources_.find(a.resource_id);if(ri==resources_.end())throw CompileError({},"LLVM backend: cleanup action names unknown resource");
        const auto& r=*ri->second;
        if(r.cleanup==SirCleanupKind::HeapFree||r.cleanup==SirCleanupKind::ManyBufferFree){
            out_<<"  %cleanup.r"<<r.id<<".p."<<cleanup_counter_<<" = load ptr, ptr "<<resource_slot(r.id)<<", align 8\n";
            if(!a.transfer) out_<<"  call void @staze_heap_free_if_nonnull(ptr %cleanup.r"<<r.id<<".p."<<cleanup_counter_<<")\n";
            out_<<"  store ptr null, ptr "<<resource_slot(r.id)<<", align 8\n";
            ++cleanup_counter_;
        }else if(r.cleanup==SirCleanupKind::SharedRelease){
            out_<<"  %cleanup.r"<<r.id<<".p."<<cleanup_counter_<<" = load ptr, ptr "<<resource_slot(r.id)<<", align 8\n";
            if(!a.transfer) out_<<"  call void @staze_shared_release_if_nonnull(ptr %cleanup.r"<<r.id<<".p."<<cleanup_counter_<<")\n";
            out_<<"  store ptr null, ptr "<<resource_slot(r.id)<<", align 8\n";
            ++cleanup_counter_;
        }else if(r.cleanup==SirCleanupKind::LifetimeEnd){
            if(a.transfer)throw CompileError({},"LLVM backend: stack/arena lifetime cannot be transferred across instruction boundary");
            const auto p=rich_ptr(r.value);
            if(p!="null") out_<<"  call void @llvm.lifetime.end.p0(i64 "<<r.size<<", ptr "<<p<<")\n";
        }
    }
    void emit_cleanup_list(const std::vector<const SirCleanupAction*>& actions){for(const auto* a:actions)emit_cleanup_action(*a);}
    void emit_cleanup_edge_blocks(){
        std::set<std::tuple<SirBlockId,SirBlockId,SirOpId>> done;
        for(const auto& a:concrete_.cleanup_actions){
            if(a.to_block==0xffffffffu)continue;
            auto key=std::make_tuple(a.from_block,a.to_block,a.source_op);if(!done.insert(key).second)continue;
            out_<<cleanup_label(a.from_block,a.to_block,a.source_op)<<":\n";
            emit_cleanup_list(cleanup_actions(a.from_block,a.to_block,a.source_op));
            out_<<"  br label %"<<block_name(a.to_block)<<"\n";
        }
    }

    std::string normal_exit_label(const SirBlock& b) const {
        for (auto it = b.instructions.rbegin(); it != b.instructions.rend(); ++it)
            if (!it->fault_edges.empty()) return local_label(b.id, it->id, "after");
        return block_name(b.id);
    }

    std::string phi_pred_label(SirBlockId pred,SirBlockId target) const {
        if(!cleanup_actions(pred,target,0).empty()) return cleanup_label(pred,target,0);
        return normal_exit_label(fn_.blocks.at(pred));
    }
    void emit_phi(const SirBlock& current,const SirInstruction& op) {
        if(is_rich(op.result)) {
            out_ << "  " << value_name(op.result) << " = phi ptr ";
            for(std::size_t i=0;i<op.phi_inputs.size();++i){if(i)out_<<", ";const auto&in=op.phi_inputs[i];out_<<"[ "<<rich_ptr(in.value)<<", %"<<phi_pred_label(in.predecessor,current.id)<<" ]";}out_<<"\n";
            return;
        }
        out_ << "  " << value_name(op.result) << " = phi " << llvm_ty(op.type) << ' ';
        for (std::size_t i = 0; i < op.phi_inputs.size(); ++i) {
            if (i) out_ << ", ";
            const auto& in = op.phi_inputs[i];
            out_ << "[ " << scalar(in.value) << ", %" << phi_pred_label(in.predecessor,current.id) << " ]";
        }
        out_ << "\n";
    }

    void emit_faulting_overflow(const SirBlock& b, const SirInstruction& op, const char* stem) {
        const auto ty = llvm_ty(op.type); const auto sign = sir_is_signed_integer(op.type) ? "s" : "u";
        const auto intr = std::string("@llvm.") + sign + stem + ".with.overflow.i" + std::to_string(sir_integer_bits(op.type));
        out_ << "  " << temp(op.id,"pair") << " = call { " << ty << ", i1 } " << intr << "(" << ty << ' ' << scalar(op.operands[0]);
        if(op.opcode==SirOpcode::CheckedNeg) out_ << ")\n"; else out_ << ", " << ty << ' ' << scalar(op.operands[1]) << ")\n";
        out_ << "  " << value_name(op.result) << " = extractvalue { " << ty << ", i1 } " << temp(op.id,"pair") << ", 0\n";
        out_ << "  " << temp(op.id,"overflow") << " = extractvalue { " << ty << ", i1 } " << temp(op.id,"pair") << ", 1\n";
        out_ << "  br i1 " << temp(op.id,"overflow") << ", label %" << fault_edge_label(b,op,"ArithmeticOverflow") << ", label %" << local_label(b.id,op.id,"after") << "\n";
        out_ << local_label(b.id,op.id,"after") << ":\n";
    }

    void emit_checked_neg(const SirBlock& b, const SirInstruction& op) {
        const auto ty=llvm_ty(op.type); const auto intr=std::string("@llvm.ssub.with.overflow.i")+std::to_string(sir_integer_bits(op.type));
        out_<<"  "<<temp(op.id,"pair")<<" = call { "<<ty<<", i1 } "<<intr<<"("<<ty<<" 0, "<<ty<<' '<<scalar(op.operands[0])<<")\n";
        out_<<"  "<<value_name(op.result)<<" = extractvalue { "<<ty<<", i1 } "<<temp(op.id,"pair")<<", 0\n";
        out_<<"  "<<temp(op.id,"overflow")<<" = extractvalue { "<<ty<<", i1 } "<<temp(op.id,"pair")<<", 1\n";
        out_<<"  br i1 "<<temp(op.id,"overflow")<<", label %"<<fault_edge_label(b,op,"ArithmeticOverflow")<<", label %"<<local_label(b.id,op.id,"after")<<"\n";
        out_<<local_label(b.id,op.id,"after")<<":\n";
    }

    void emit_divmod(const SirBlock& b, const SirInstruction& op, bool mod) {
        const auto ty=llvm_ty(op.type);const auto zero=temp(op.id,"zero"),nonzero=local_label(b.id,op.id,"nonzero"),after=local_label(b.id,op.id,"after");
        out_<<"  "<<zero<<" = icmp eq "<<ty<<' '<<scalar(op.operands[1])<<", 0\n";
        out_<<"  br i1 "<<zero<<", label %"<<fault_edge_label(b,op,"DivideByZero")<<", label %"<<nonzero<<"\n";
        out_<<nonzero<<":\n";
        if(sir_is_signed_integer(op.type)){
            const std::string minv=sir_integer_bits(op.type)==32?std::to_string(std::numeric_limits<std::int32_t>::min()):std::to_string(std::numeric_limits<std::int64_t>::min());
            out_<<"  "<<temp(op.id,"ismin")<<" = icmp eq "<<ty<<' '<<scalar(op.operands[0])<<", "<<minv<<"\n";
            out_<<"  "<<temp(op.id,"isneg1")<<" = icmp eq "<<ty<<' '<<scalar(op.operands[1])<<", -1\n";
            out_<<"  "<<temp(op.id,"bad")<<" = and i1 "<<temp(op.id,"ismin")<<", "<<temp(op.id,"isneg1")<<"\n";
            out_<<"  br i1 "<<temp(op.id,"bad")<<", label %"<<fault_edge_label(b,op,"ArithmeticOverflow")<<", label %"<<after<<"\n";
        }else out_<<"  br label %"<<after<<"\n";
        out_<<after<<":\n";const char*inst=sir_is_signed_integer(op.type)?(mod?"srem":"sdiv"):(mod?"urem":"udiv");
        out_<<"  "<<value_name(op.result)<<" = "<<inst<<' '<<ty<<' '<<scalar(op.operands[0])<<", "<<scalar(op.operands[1])<<"\n";
    }

    void emit_optional(const SirInstruction& op, bool some) {
        emit_alloca(op.result); const auto& p=cv(op.result); if(p.scalar_replaced)return;
        out_<<"  store i8 "<<(some?1:0)<<", ptr "<<rich_ptr(op.result)<<", align 1\n";
        if(some) store_value(rich_ptr(op.result),p.payload_offset,op.operands.at(0),p.element_size,cv(op.operands.at(0)).align,temp(op.id,"optional.payload"));
    }

    void emit_many(const SirInstruction& op) {
        emit_alloca(op.result); const auto&p=cv(op.result); if(p.scalar_replaced)return;
        out_<<"  store i64 "<<op.operands.size()<<", ptr "<<rich_ptr(op.result)<<", align 8\n";
        for(std::size_t i=0;i<op.operands.size();++i) store_value(rich_ptr(op.result),p.payload_offset+static_cast<std::uint32_t>(i)*p.element_size,op.operands[i],p.element_size,cv(op.operands[i]).align,temp(op.id,"many."+std::to_string(i)));
    }

    void emit_record_fields(const SirInstruction& op, const std::vector<std::uint32_t>& offsets) {
        const auto&p=cv(op.result); if(p.scalar_replaced)return; emit_alloca(op.result);
        if(offsets.size()!=op.operands.size())throw CompileError({},"LLVM backend: concrete field-offset arity mismatch");
        for(std::size_t i=0;i<op.operands.size();++i)store_value(rich_ptr(op.result),offsets[i],op.operands[i],cv(op.operands[i]).size,cv(op.operands[i]).align,temp(op.id,"field."+std::to_string(i)));
    }

    void emit_aggregate(const SirInstruction& op) {
        const auto&l=concrete_layout(program_,op.semantic_name);emit_record_fields(op,l.field_offsets);
    }

    void emit_choice(const SirInstruction& op) {
        const auto&p=cv(op.result);if(p.scalar_replaced)return;emit_alloca(op.result);
        out_<<"  store i32 "<<op.tag<<", ptr "<<rich_ptr(op.result)<<", align 4\n";
        std::uint32_t off=p.payload_offset;
        for(std::size_t i=0;i<op.operands.size();++i){const auto&ov=cv(op.operands[i]);off=align_up(off,ov.align);store_value(rich_ptr(op.result),off,op.operands[i],ov.size,ov.align,temp(op.id,"choice."+std::to_string(i)));off+=ov.size;}
    }

    void mark_resource_pointer(SirValueId value,const std::string& ptr) {
        const auto* r=resource_for_value(value);if(!r||(!r->automatic&&!undo_tracked_values_.contains(r->value)))return;
        if(r->cleanup==SirCleanupKind::HeapFree||r->cleanup==SirCleanupKind::ManyBufferFree||r->cleanup==SirCleanupKind::SharedRelease)
            out_<<"  store ptr "<<ptr<<", ptr "<<resource_slot(r->id)<<", align 8\n";
    }
    std::vector<const SirConcreteUndo*> undo_for_source(SirOpId op) const {
        auto it=undo_by_source_.find(op);if(it==undo_by_source_.end())return {};return it->second;
    }
    void activate_simple_undo(SirOpId source) {
        for(const auto* u:undo_for_source(source)) if(u->kind!=SirUndoKind::RestoreField)
            out_<<"  store i1 true, ptr "<<undo_active(u->id)<<", align 1\n";
    }
    void snapshot_field_undo(const SirInstruction& op,const std::string& root,std::uint32_t off) {
        for(const auto* u:undo_for_source(op.id)) if(u->kind==SirUndoKind::RestoreField){
            const auto src=byte_ptr(root,off,"%undo."+std::to_string(u->id)+".src");
            emit_memcpy(undo_bytes(u->id),src,u->size);
            out_<<"  store i1 true, ptr "<<undo_active(u->id)<<", align 1\n";
        }
    }
    void clear_transaction_undo(SirRegionId region) {
        auto it=undo_by_transaction_.find(region);if(it==undo_by_transaction_.end())return;
        for(const auto* u:it->second) out_<<"  store i1 false, ptr "<<undo_active(u->id)<<", align 1\n";
    }
    void emit_undo_action(const SirConcreteUndo& u,SirOpId abort_op) {
        const auto active="%undo.o"+std::to_string(abort_op)+".u"+std::to_string(u.id)+".isactive";
        const auto doit=undo_label(abort_op,u.id,"do"),done=undo_label(abort_op,u.id,"done");
        out_<<"  "<<active<<" = load i1, ptr "<<undo_active(u.id)<<", align 1\n";
        out_<<"  br i1 "<<active<<", label %"<<doit<<", label %"<<done<<"\n"<<doit<<":\n";
        if(u.kind==SirUndoKind::RestoreField){
            auto dst=byte_ptr(rich_ptr(u.target_value),u.byte_offset,"%undo.o"+std::to_string(abort_op)+".u"+std::to_string(u.id)+".dst");
            emit_memcpy(dst,undo_bytes(u.id),u.size);
        }else if(u.kind==SirUndoKind::DestroyResource||u.kind==SirUndoKind::SharedRelease){
            const auto* r=resource_for_value(u.target_value);if(!r)throw CompileError({},"LLVM backend: transaction undo resource lacks concrete resource obligation");
            const auto pv="%undo.o"+std::to_string(abort_op)+".u"+std::to_string(u.id)+".p";
            out_<<"  "<<pv<<" = load ptr, ptr "<<resource_slot(r->id)<<", align 8\n";
            if(r->cleanup==SirCleanupKind::SharedRelease||u.kind==SirUndoKind::SharedRelease)out_<<"  call void @staze_shared_release_if_nonnull(ptr "<<pv<<")\n";
            else out_<<"  call void @staze_heap_free_if_nonnull(ptr "<<pv<<")\n";
            out_<<"  store ptr null, ptr "<<resource_slot(r->id)<<", align 8\n";
        }else if(u.kind==SirUndoKind::RestoreOwner){
            auto oi=defs_.find(u.aux_value);
            if(u.aux_value&&oi!=defs_.end()&&oi->second.op&&oi->second.op->opcode==SirOpcode::FieldMove){
                const auto fp=byte_ptr(rich_ptr(u.target_value),u.byte_offset,"%undo.o"+std::to_string(abort_op)+".u"+std::to_string(u.id)+".field");
                out_<<"  store ptr "<<rich_ptr(u.aux_value)<<", ptr "<<fp<<", align 8\n";
            }
            // Whole-value move/send are representation aliases in the reference
            // backend, so restoring semantic ownership requires no byte mutation.
        }else if(u.kind==SirUndoKind::RelationLink||u.kind==SirUndoKind::RelationUnlink){
            // The 0.8 reference relation provider is intentionally abstract:
            // relation operations are effect-ordered but hold no native table yet.
            // Clearing this verified inverse action is therefore the concrete no-op.
        }
        out_<<"  store i1 false, ptr "<<undo_active(u.id)<<", align 1\n";
        out_<<"  br label %"<<done<<"\n"<<done<<":\n";
    }
    void emit_transaction_abort(const SirInstruction& op) {
        auto it=undo_by_transaction_.find(op.region);if(it==undo_by_transaction_.end())return;
        auto actions=it->second;std::sort(actions.begin(),actions.end(),[](const auto*a,const auto*b){return std::tie(a->source_op,a->id)>std::tie(b->source_op,b->id);});
        for(const auto* u:actions)emit_undo_action(*u,op.id);
    }

    void emit_pool_alloc(const SirBlock& b, const SirInstruction& op) {
        const auto&l=concrete_layout(program_,op.semantic_name);const auto&p=cv(op.result);const auto*ps=pool_schema(program_,op.semantic_name);if(!ps)throw CompileError({},"LLVM backend: missing pool schema");
        if(p.storage==SirStorageClass::Heap){
            out_<<"  "<<temp(op.id,"heap")<<" = call ptr @GetProcessHeap()\n";
            const bool shared = ps->ownership == "shared";
            const auto alloc_bytes = static_cast<std::uint64_t>(p.size) + (shared ? 8u : 0u);
            out_<<"  "<<temp(op.id,"base")<<" = call ptr @HeapAlloc(ptr "<<temp(op.id,"heap")<<", i32 0, i64 "<<alloc_bytes<<")\n";
            out_<<"  "<<temp(op.id,"null")<<" = icmp eq ptr "<<temp(op.id,"base")<<", null\n";
            const auto after=local_label(b.id,op.id,"after");
            out_<<"  br i1 "<<temp(op.id,"null")<<", label %"<<fault_edge_label(b,op,"AllocationFailure")<<", label %"<<after<<"\n";
            out_<<after<<":\n";
            if(shared){
                out_<<"  store i64 1, ptr "<<temp(op.id,"base")<<", align 8\n";
                out_<<"  "<<value_name(op.result)<<" = getelementptr i8, ptr "<<temp(op.id,"base")<<", i64 8\n";
            }else{
                out_<<"  "<<value_name(op.result)<<" = getelementptr i8, ptr "<<temp(op.id,"base")<<", i64 0\n";
            }
            mark_resource_pointer(op.result,rich_ptr(op.result));
        }else if(p.storage==SirStorageClass::Static){
            // Static storage is emitted at module scope; the allocation operation initializes it.
        }else{
            emit_alloca(op.result);
            if(const auto* r=resource_for_value(op.result);r&&r->automatic&&r->cleanup==SirCleanupKind::LifetimeEnd)
                out_<<"  call void @llvm.lifetime.start.p0(i64 "<<r->size<<", ptr "<<rich_ptr(op.result)<<")\n";
        }
        for(std::size_t i=0;i<op.operands.size();++i)store_value(rich_ptr(op.result),l.field_offsets.at(i),op.operands[i],cv(op.operands[i]).size,cv(op.operands[i]).align,temp(op.id,"pool."+std::to_string(i)));
        activate_simple_undo(op.id);
    }

    std::uint32_t field_offset(const SirInstruction& op) const {
        auto pos=op.semantic_name.find('.');if(pos==std::string::npos)throw CompileError({},"LLVM backend: field op lacks owner.field identity");
        const auto owner=op.semantic_name.substr(0,pos);const auto& l=concrete_layout(program_,owner);if(op.tag>=l.field_offsets.size())throw CompileError({},"LLVM backend: field ordinal outside concrete layout");return l.field_offsets[op.tag];
    }
    void emit_field_load(const SirInstruction& op) {
        const auto root=rich_ptr(op.operands.at(0));const auto off=cv(op.result).byte_offset;
        const auto ptr=temp(op.id,"field.ptr");out_<<"  "<<ptr<<" = getelementptr i8, ptr "<<root<<", i64 "<<off<<"\n";
        if(is_rich(op.result)) out_<<"  "<<value_name(op.result)<<" = getelementptr i8, ptr "<<ptr<<", i64 0\n";
        else out_<<"  "<<value_name(op.result)<<" = load "<<llvm_ty(op.type)<<", ptr "<<ptr<<", align "<<std::max<std::uint32_t>(1,std::min<std::uint32_t>(cv(op.result).align,8))<<"\n";
    }
    void emit_field_address(const SirInstruction& op) {
        const auto root=rich_ptr(op.operands.at(0));out_<<"  "<<value_name(op.result)<<" = getelementptr i8, ptr "<<root<<", i64 "<<cv(op.result).byte_offset<<"\n";
    }
    void emit_field_store(const SirInstruction& op) {
        const auto root=rich_ptr(op.operands.at(0));const auto off=field_offset(op);const auto value=op.operands.at(1);
        snapshot_field_undo(op,root,off);
        store_value(root,off,value,cv(value).size,cv(value).align,temp(op.id,"field.store"));
        out_<<"  "<<value_name(op.result)<<" = add i8 0, 0\n";
    }

    void emit_field_move(const SirInstruction& op) {
        const auto root = rich_ptr(op.operands.at(0));
        const auto off = field_offset(op);
        const auto fieldp = temp(op.id, "field.ptr");
        out_ << "  " << fieldp << " = getelementptr i8, ptr " << root << ", i64 " << off << "\n";
        const auto& plan = cv(op.result);
        if (plan.kind == SirConcreteKind::BorrowRef && plan.size == 8) {
            out_ << "  " << value_name(op.result) << " = load ptr, ptr " << fieldp << ", align 8\n";
            out_ << "  store ptr null, ptr " << fieldp << ", align 8\n";
            mark_resource_pointer(op.result, value_name(op.result));
            activate_simple_undo(op.id);
            return;
        }
        throw CompileError({}, "LLVM backend: Compiler 0.8 partial field move currently requires a pointer-bearing resource field");
    }

    void emit_shared_retain(const SirInstruction& op) {
        const auto src = rich_ptr(op.operands.at(0));
        out_ << "  call void @staze_shared_retain_if_nonnull(ptr " << src << ")\n";
        out_ << "  " << value_name(op.result) << " = getelementptr i8, ptr " << src << ", i64 0\n";
        mark_resource_pointer(op.result, value_name(op.result));
        activate_simple_undo(op.id);
    }

    void clear_resource_slot_for_value(SirValueId value) {
        const auto* r = resource_for_value(value);
        if (r && (r->automatic || undo_tracked_values_.contains(r->value)) && (r->cleanup == SirCleanupKind::HeapFree || r->cleanup == SirCleanupKind::ManyBufferFree || r->cleanup == SirCleanupKind::SharedRelease))
            out_ << "  store ptr null, ptr " << resource_slot(r->id) << ", align 8\n";
    }

    void emit_resource_release(const SirInstruction& op) {
        const auto source = op.operands.at(0);
        const auto* sem = value_semantics_.at(source);
        if (sem->borrow_mode != "none" || sem->ownership == "borrow" || sem->ownership == "borrow-shared" || sem->ownership == "borrow-mut") {
            out_ << "  " << value_name(op.result) << " = add i8 0, 0\n";
            return;
        }
        const auto ptr = rich_ptr(source);
        if (sem->ownership == "shared" || sem->resource_family.rfind("shared-pool:", 0) == 0)
            out_ << "  call void @staze_shared_release_if_nonnull(ptr " << ptr << ")\n";
        else if (sem->resource_family.rfind("heap-pool:", 0) == 0)
            out_ << "  call void @staze_heap_free_if_nonnull(ptr " << ptr << ")\n";
        else
            throw CompileError({}, "LLVM backend: resource.release has no supported native resource family");
        clear_resource_slot_for_value(source);
        out_ << "  " << value_name(op.result) << " = add i8 0, 0\n";
    }

    void emit_many_dynamic(const SirBlock& b,const SirInstruction& op) {
        emit_alloca(op.result);const auto&p=cv(op.result);const auto cap=scalar(op.operands.at(0));
        const auto pair=temp(op.id,"bytes.pair"),bytes=temp(op.id,"bytes"),ov=temp(op.id,"bytes.ov"),alloc=local_label(b.id,op.id,"alloc"),after=local_label(b.id,op.id,"after");
        out_<<"  "<<pair<<" = call { i64, i1 } @llvm.umul.with.overflow.i64(i64 "<<cap<<", i64 "<<p.element_size<<")\n";
        out_<<"  "<<bytes<<" = extractvalue { i64, i1 } "<<pair<<", 0\n";
        out_<<"  "<<ov<<" = extractvalue { i64, i1 } "<<pair<<", 1\n";
        out_<<"  br i1 "<<ov<<", label %"<<fault_edge_label(b,op,"ArithmeticOverflow")<<", label %"<<alloc<<"\n"<<alloc<<":\n";
        out_<<"  "<<temp(op.id,"cap.zero")<<" = icmp eq i64 "<<cap<<", 0\n";
        out_<<"  "<<temp(op.id,"alloc.bytes")<<" = select i1 "<<temp(op.id,"cap.zero")<<", i64 "<<std::max<std::uint32_t>(1,p.element_size)<<", i64 "<<bytes<<"\n";
        out_<<"  "<<temp(op.id,"heap")<<" = call ptr @GetProcessHeap()\n";
        out_<<"  "<<temp(op.id,"data")<<" = call ptr @HeapAlloc(ptr "<<temp(op.id,"heap")<<", i32 0, i64 "<<temp(op.id,"alloc.bytes")<<")\n";
        out_<<"  "<<temp(op.id,"null")<<" = icmp eq ptr "<<temp(op.id,"data")<<", null\n";
        out_<<"  br i1 "<<temp(op.id,"null")<<", label %"<<fault_edge_label(b,op,"AllocationFailure")<<", label %"<<after<<"\n"<<after<<":\n";
        out_<<"  store ptr "<<temp(op.id,"data")<<", ptr "<<rich_ptr(op.result)<<", align 8\n";
        auto lp=byte_ptr(rich_ptr(op.result),8,temp(op.id,"len.ptr"));out_<<"  store i64 0, ptr "<<lp<<", align 8\n";
        auto cp=byte_ptr(rich_ptr(op.result),16,temp(op.id,"cap.ptr"));out_<<"  store i64 "<<cap<<", ptr "<<cp<<", align 8\n";
        mark_resource_pointer(op.result,temp(op.id,"data"));
    }
    void emit_many_length(const SirInstruction& op){auto lp=byte_ptr(rich_ptr(op.operands.at(0)),8,temp(op.id,"len.ptr"));out_<<"  "<<value_name(op.result)<<" = load i64, ptr "<<lp<<", align 8\n";}
    void emit_many_push(const SirBlock& b,const SirInstruction& op){
        const auto list=op.operands.at(0),item=op.operands.at(1);const auto& p=cv(list);const auto base=rich_ptr(list);
        auto lp=byte_ptr(base,8,temp(op.id,"len.ptr")),cp=byte_ptr(base,16,temp(op.id,"cap.ptr"));
        out_<<"  "<<temp(op.id,"data")<<" = load ptr, ptr "<<base<<", align 8\n";
        out_<<"  "<<temp(op.id,"len")<<" = load i64, ptr "<<lp<<", align 8\n";
        out_<<"  "<<temp(op.id,"cap")<<" = load i64, ptr "<<cp<<", align 8\n";
        out_<<"  "<<temp(op.id,"room")<<" = icmp ult i64 "<<temp(op.id,"len")<<", "<<temp(op.id,"cap")<<"\n";
        const auto grow=local_label(b.id,op.id,"grow"),append=local_label(b.id,op.id,"append"),after=local_label(b.id,op.id,"after");
        out_<<"  br i1 "<<temp(op.id,"room")<<", label %"<<append<<", label %"<<grow<<"\n"<<grow<<":\n";
        out_<<"  "<<temp(op.id,"cap.zero")<<" = icmp eq i64 "<<temp(op.id,"cap")<<", 0\n";
        out_<<"  "<<temp(op.id,"grow.pair")<<" = call { i64, i1 } @llvm.umul.with.overflow.i64(i64 "<<temp(op.id,"cap")<<", i64 2)\n";
        out_<<"  "<<temp(op.id,"grow.double")<<" = extractvalue { i64, i1 } "<<temp(op.id,"grow.pair")<<", 0\n";
        out_<<"  "<<temp(op.id,"grow.ov0")<<" = extractvalue { i64, i1 } "<<temp(op.id,"grow.pair")<<", 1\n";
        out_<<"  "<<temp(op.id,"newcap")<<" = select i1 "<<temp(op.id,"cap.zero")<<", i64 4, i64 "<<temp(op.id,"grow.double")<<"\n";
        out_<<"  "<<temp(op.id,"grow.ov")<<" = select i1 "<<temp(op.id,"cap.zero")<<", i1 false, i1 "<<temp(op.id,"grow.ov0")<<"\n";
        const auto bytescheck=local_label(b.id,op.id,"bytescheck");out_<<"  br i1 "<<temp(op.id,"grow.ov")<<", label %"<<fault_edge_label(b,op,"ArithmeticOverflow")<<", label %"<<bytescheck<<"\n"<<bytescheck<<":\n";
        out_<<"  "<<temp(op.id,"bytes.pair")<<" = call { i64, i1 } @llvm.umul.with.overflow.i64(i64 "<<temp(op.id,"newcap")<<", i64 "<<p.element_size<<")\n";
        out_<<"  "<<temp(op.id,"bytes")<<" = extractvalue { i64, i1 } "<<temp(op.id,"bytes.pair")<<", 0\n";
        out_<<"  "<<temp(op.id,"bytes.ov")<<" = extractvalue { i64, i1 } "<<temp(op.id,"bytes.pair")<<", 1\n";
        const auto allocate=local_label(b.id,op.id,"allocate");out_<<"  br i1 "<<temp(op.id,"bytes.ov")<<", label %"<<fault_edge_label(b,op,"ArithmeticOverflow")<<", label %"<<allocate<<"\n"<<allocate<<":\n";
        out_<<"  "<<temp(op.id,"heap")<<" = call ptr @GetProcessHeap()\n";
        out_<<"  "<<temp(op.id,"newdata")<<" = call ptr @HeapAlloc(ptr "<<temp(op.id,"heap")<<", i32 0, i64 "<<temp(op.id,"bytes")<<")\n";
        out_<<"  "<<temp(op.id,"null")<<" = icmp eq ptr "<<temp(op.id,"newdata")<<", null\n";
        const auto copy=local_label(b.id,op.id,"copy");out_<<"  br i1 "<<temp(op.id,"null")<<", label %"<<fault_edge_label(b,op,"AllocationFailure")<<", label %"<<copy<<"\n"<<copy<<":\n";
        out_<<"  "<<temp(op.id,"used.bytes")<<" = mul i64 "<<temp(op.id,"len")<<", "<<p.element_size<<"\n";
        out_<<"  call void @llvm.memcpy.p0.p0.i64(ptr "<<temp(op.id,"newdata")<<", ptr "<<temp(op.id,"data")<<", i64 "<<temp(op.id,"used.bytes")<<", i1 false)\n";
        out_<<"  call void @staze_heap_free_if_nonnull(ptr "<<temp(op.id,"data")<<")\n";
        out_<<"  store ptr "<<temp(op.id,"newdata")<<", ptr "<<base<<", align 8\n";
        out_<<"  store i64 "<<temp(op.id,"newcap")<<", ptr "<<cp<<", align 8\n";
        mark_resource_pointer(list,temp(op.id,"newdata"));
        out_<<"  br label %"<<append<<"\n"<<append<<":\n";
        out_<<"  "<<temp(op.id,"append.data")<<" = load ptr, ptr "<<base<<", align 8\n";
        out_<<"  "<<temp(op.id,"append.len")<<" = load i64, ptr "<<lp<<", align 8\n";
        out_<<"  "<<temp(op.id,"append.off")<<" = mul i64 "<<temp(op.id,"append.len")<<", "<<p.element_size<<"\n";
        out_<<"  "<<temp(op.id,"append.ptr")<<" = getelementptr i8, ptr "<<temp(op.id,"append.data")<<", i64 "<<temp(op.id,"append.off")<<"\n";
        store_value(temp(op.id,"append.ptr"),0,item,p.element_size,cv(item).align,temp(op.id,"append.value"));
        out_<<"  "<<temp(op.id,"nextlen")<<" = add i64 "<<temp(op.id,"append.len")<<", 1\n";
        out_<<"  store i64 "<<temp(op.id,"nextlen")<<", ptr "<<lp<<", align 8\n";
        out_<<"  "<<value_name(op.result)<<" = add i8 0, 0\n";
        out_<<"  br label %"<<after<<"\n"<<after<<":\n";
    }

    void emit_alias(const SirInstruction& op) {
        if(!is_rich(op.result)) { activate_simple_undo(op.id); return; }
        const auto src=rich_ptr(op.operands.at(0));const auto off=cv(op.result).byte_offset;
        if(src=="null") out_<<"  "<<value_name(op.result)<<" = inttoptr i64 0 to ptr\n";
        else out_<<"  "<<value_name(op.result)<<" = getelementptr i8, ptr "<<src<<", i64 "<<off<<"\n";
        activate_simple_undo(op.id);
    }

    void emit_call(const SirBlock& b, const SirInstruction& op) {
        const auto& callee=concrete_function(program_,op.callee);const bool rr=callee.result_storage==SirStorageClass::Caller;
        std::string transfer_slot;
        if(rr){
            if(callee.result_transfer==SirResultTransferKind::HeapObject || callee.result_transfer==SirResultTransferKind::BorrowRef || callee.result_transfer==SirResultTransferKind::SharedRef){transfer_slot=temp(op.id,"transfer.slot");out_<<"  "<<transfer_slot<<" = alloca ptr, align 8\n  store ptr null, ptr "<<transfer_slot<<", align 8\n";}
            else emit_alloca(op.result);
        }
        std::vector<std::string> args;
        for(auto v:op.operands) args.push_back(is_rich(v)?rich_ptr(v):box(v));
        if(rr){
            const bool pointer_transfer = callee.result_transfer==SirResultTransferKind::HeapObject || callee.result_transfer==SirResultTransferKind::BorrowRef || callee.result_transfer==SirResultTransferKind::SharedRef;
            const auto outptr=pointer_transfer?transfer_slot:rich_ptr(op.result);
            out_<<"  "<<temp(op.id,"status")<<" = call i32 @stz_"<<safe_name(op.callee)<<"(ptr "<<outptr<<", ptr %fault_out";
            for (std::size_t i = 0; i < args.size(); ++i) out_ << ", " << (is_rich(op.operands[i]) ? "ptr " : "i64 ") << args[i];
            out_ << ")\n";
        }else{
            out_<<"  "<<temp(op.id,"call")<<" = call { i32, i64 } @stz_"<<safe_name(op.callee)<<"(ptr %fault_out";
            for (std::size_t i = 0; i < args.size(); ++i) out_ << ", " << (is_rich(op.operands[i]) ? "ptr " : "i64 ") << args[i];
            out_ << ")\n";
            out_<<"  "<<temp(op.id,"status")<<" = extractvalue { i32, i64 } "<<temp(op.id,"call")<<", 0\n";
            out_<<"  "<<temp(op.id,"payload")<<" = extractvalue { i32, i64 } "<<temp(op.id,"call")<<", 1\n";
        }
        auto materialize_transfer=[&](){
            if(!rr)return;
            if(callee.result_transfer==SirResultTransferKind::HeapObject || callee.result_transfer==SirResultTransferKind::BorrowRef || callee.result_transfer==SirResultTransferKind::SharedRef){out_<<"  "<<value_name(op.result)<<" = load ptr, ptr "<<transfer_slot<<", align 8\n";mark_resource_pointer(op.result,value_name(op.result));}
            else if(callee.result_transfer==SirResultTransferKind::DynamicMany){auto dp=rich_ptr(op.result);out_<<"  "<<temp(op.id,"transfer.data")<<" = load ptr, ptr "<<dp<<", align 8\n";mark_resource_pointer(op.result,temp(op.id,"transfer.data"));}
        };
        if(op.fault_edges.empty()){if(!rr)unbox_to(op.result,op.type,temp(op.id,"payload"));else materialize_transfer();return;}
        const auto after=local_label(b.id,op.id,"after"),invalid=local_label(b.id,op.id,"invalid_status");
        out_<<"  switch i32 "<<temp(op.id,"status")<<", label %"<<invalid<<" [\n    i32 0, label %"<<after<<"\n";
        for (const auto& e : op.fault_edges) out_ << "    i32 " << fault_code(e.fault) << ", label %" << edge_label(b.id,e.target,op.id) << "\n";
        out_ << "  ]\n" << invalid << ":\n  unreachable\n" << after << ":\n";
        if(!rr)unbox_to(op.result,op.type,temp(op.id,"payload"));else materialize_transfer();
    }

    void emit_write_text(const SirBlock& b, const SirInstruction& op) {
        const auto[index,length]=text_constant(op.operands.at(0));const auto fault=fault_edge_label(b,op,"IoFailure"),handle_ok=local_label(b.id,op.id,"handle_ok"),after=local_label(b.id,op.id,"after");
        out_<<"  "<<temp(op.id,"handle")<<" = call ptr @GetStdHandle(i32 -11)\n";
        out_<<"  "<<temp(op.id,"handle_i")<<" = ptrtoint ptr "<<temp(op.id,"handle")<<" to i64\n";
        out_<<"  "<<temp(op.id,"bad_minus1")<<" = icmp eq i64 "<<temp(op.id,"handle_i")<<", -1\n";
        out_<<"  "<<temp(op.id,"bad_zero")<<" = icmp eq i64 "<<temp(op.id,"handle_i")<<", 0\n";
        out_<<"  "<<temp(op.id,"bad_handle")<<" = or i1 "<<temp(op.id,"bad_minus1")<<", "<<temp(op.id,"bad_zero")<<"\n";
        out_<<"  br i1 "<<temp(op.id,"bad_handle")<<", label %"<<fault<<", label %"<<handle_ok<<"\n"<<handle_ok<<":\n";
        out_<<"  "<<temp(op.id,"ptr")<<" = getelementptr ["<<length<<" x i8], ptr @.str"<<index<<", i64 0, i64 0\n";
        out_<<"  "<<temp(op.id,"ok")<<" = call i32 @WriteFile(ptr "<<temp(op.id,"handle")<<", ptr "<<temp(op.id,"ptr")<<", i32 "<<length<<", ptr @.staze_io_bytes, ptr null)\n";
        out_<<"  "<<temp(op.id,"failed")<<" = icmp eq i32 "<<temp(op.id,"ok")<<", 0\n";
        out_<<"  br i1 "<<temp(op.id,"failed")<<", label %"<<fault<<", label %"<<after<<"\n"<<after<<":\n";
    }

    void emit_instruction(const SirBlock& b, const SirInstruction& op) {
        switch(op.opcode){
            case SirOpcode::Phi: emit_phi(b,op); break;
            case SirOpcode::ConstUnit:case SirOpcode::ConstInt:case SirOpcode::ConstBool:case SirOpcode::ConstText: break;
            case SirOpcode::RevisionMarker:case SirOpcode::MoveValue:case SirOpcode::SendValue:case SirOpcode::Borrow:case SirOpcode::BorrowMut: emit_alias(op); break;
            case SirOpcode::OptionalSome: emit_optional(op,true); break;
            case SirOpcode::OptionalNone: emit_optional(op,false); break;
            case SirOpcode::ManyMake: emit_many(op); break;
            case SirOpcode::AggregateMake: emit_aggregate(op); break;
            case SirOpcode::ChoiceMake: emit_choice(op); break;
            case SirOpcode::PoolAlloc: emit_pool_alloc(b,op); break;
            case SirOpcode::FieldLoad: emit_field_load(op); break;
            case SirOpcode::FieldAddress: emit_field_address(op); break;
            case SirOpcode::FieldStore: emit_field_store(op); break;
            case SirOpcode::FieldMove: emit_field_move(op); break;
            case SirOpcode::ManyDynamic: emit_many_dynamic(b,op); break;
            case SirOpcode::ManyPush: emit_many_push(b,op); break;
            case SirOpcode::ManyLength: emit_many_length(op); break;
            case SirOpcode::ResourceRelease: emit_resource_release(op); break;
            case SirOpcode::SharedRetain: emit_shared_retain(op); break;
            case SirOpcode::RelationLink:case SirOpcode::RelationUnlink: activate_simple_undo(op.id); break;
            case SirOpcode::TransactionBegin: break;
            case SirOpcode::TransactionCommit: clear_transaction_undo(op.region); break;
            case SirOpcode::TransactionAbort: emit_transaction_abort(op); break;
            case SirOpcode::ParallelBegin:case SirOpcode::ParallelEnd:case SirOpcode::TaskBegin:case SirOpcode::TaskEnd: break;
            case SirOpcode::Not: out_<<"  "<<value_name(op.result)<<" = xor i1 "<<scalar(op.operands[0])<<", true\n"; break;
            case SirOpcode::CheckedNeg: emit_checked_neg(b,op); break;
            case SirOpcode::CheckedAdd: emit_faulting_overflow(b,op,"add"); break;
            case SirOpcode::CheckedSub: emit_faulting_overflow(b,op,"sub"); break;
            case SirOpcode::CheckedMul: emit_faulting_overflow(b,op,"mul"); break;
            case SirOpcode::CheckedDiv: emit_divmod(b,op,false); break;
            case SirOpcode::CheckedMod: emit_divmod(b,op,true); break;
            case SirOpcode::CompareEq:case SirOpcode::CompareNe:case SirOpcode::CompareLt:case SirOpcode::CompareLe:case SirOpcode::CompareGt:case SirOpcode::CompareGe:{
                const auto ot=value_type(op.operands[0]);std::string pred;if(op.opcode==SirOpcode::CompareEq)pred="eq";else if(op.opcode==SirOpcode::CompareNe)pred="ne";else if(op.opcode==SirOpcode::CompareLt)pred=sir_is_signed_integer(ot)?"slt":"ult";else if(op.opcode==SirOpcode::CompareLe)pred=sir_is_signed_integer(ot)?"sle":"ule";else if(op.opcode==SirOpcode::CompareGt)pred=sir_is_signed_integer(ot)?"sgt":"ugt";else pred=sir_is_signed_integer(ot)?"sge":"uge";
                out_<<"  "<<value_name(op.result)<<" = icmp "<<pred<<' '<<llvm_ty(ot)<<' '<<scalar(op.operands[0])<<", "<<scalar(op.operands[1])<<"\n";break;}
            case SirOpcode::Call: emit_call(b,op); break;
            case SirOpcode::WriteText: emit_write_text(b,op); break;
        }
    }

    void write_fault_payload(const SirTerminator& t) {
        if(t.fault_payload_passthrough||t.fault_payload.empty())return;
        const auto*fs=fault_schema(program_,t.fault);if(!fs)throw CompileError({},"LLVM backend: missing fault schema");
        std::uint32_t off=0;
        for(std::size_t i=0;i<t.fault_payload.size();++i){const auto v=t.fault_payload[i];const auto&p=cv(v);off=align_up(off,p.align);store_value("%fault_out",off,v,p.size,p.align,"%fault.store."+std::to_string(fault_store_counter_++));off+=p.size;}
    }

    void emit_return(const SirBlock& b,const SirTerminator& t) {
        if(rich_result()){
            if (!t.value) throw CompileError({}, "LLVM backend: rich result missing return value");
            const auto src = rich_ptr(*t.value);
            if(concrete_.result_transfer==SirResultTransferKind::HeapObject || concrete_.result_transfer==SirResultTransferKind::BorrowRef || concrete_.result_transfer==SirResultTransferKind::SharedRef) out_<<"  store ptr "<<src<<", ptr %result_out, align 8\n";
            else emit_memcpy("%result_out", src, std::min(concrete_.result_size, cv(*t.value).size));
            emit_cleanup_list(cleanup_actions(b.id,0xffffffffu,0,false));
            out_ << "  ret i32 0\n";
            return;
        }
        std::string payload="0";if(t.value)payload=box(*t.value);
        emit_cleanup_list(cleanup_actions(b.id,0xffffffffu,0,false));
        const auto a="%ret.status."+std::to_string(return_counter_),bb="%ret.value."+std::to_string(return_counter_++);
        out_<<"  "<<a<<" = insertvalue { i32, i64 } poison, i32 0, 0\n"<<"  "<<bb<<" = insertvalue { i32, i64 } "<<a<<", i64 "<<payload<<", 1\n"<<"  ret { i32, i64 } "<<bb<<"\n";
    }
    void emit_fault_return(const SirBlock& b,const SirTerminator& t) {
        write_fault_payload(t);
        emit_cleanup_list(cleanup_actions(b.id,0xffffffffu,0,true));
        if(rich_result()){out_<<"  ret i32 "<<fault_code(t.fault)<<"\n";return;}
        const auto a="%fault.status."+std::to_string(return_counter_),bb="%fault.value."+std::to_string(return_counter_++);
        out_<<"  "<<a<<" = insertvalue { i32, i64 } poison, i32 "<<fault_code(t.fault)<<", 0\n"<<"  "<<bb<<" = insertvalue { i32, i64 } "<<a<<", i64 0, 1\n"<<"  ret { i32, i64 } "<<bb<<"\n";
    }

    void emit_block(const SirBlock& b) {
        out_<<block_name(b.id)<<":\n";for(const auto&op:b.instructions)emit_instruction(b,op);const auto&t=b.terminator;
        switch(t.kind){
            case SirTerminatorKind::Br:out_<<"  br label %"<<edge_label(b.id,t.target,0)<<"\n";break;
            case SirTerminatorKind::CondBr:out_<<"  br i1 "<<scalar(t.condition)<<", label %"<<edge_label(b.id,t.true_target,0)<<", label %"<<edge_label(b.id,t.false_target,0)<<"\n";break;
            case SirTerminatorKind::Return:emit_return(b,t);break;
            case SirTerminatorKind::FaultReturn:emit_fault_return(b,t);break;
            case SirTerminatorKind::Unreachable:out_<<"  unreachable\n";break;
        }
    }

    const SirCProgram& program_; const SirFunction& fn_; const SirConcreteFunction& concrete_; const std::map<std::string,std::size_t>& strings_;
    std::unordered_map<SirValueId,Def> defs_;
    std::unordered_map<SirValueId,const SirConcreteValue*> concrete_values_;
    std::unordered_map<SirValueId,const SirValueSemantics*> value_semantics_;
    std::unordered_map<SirProvenanceId,const SirProvenance*> provenances_;
    std::unordered_map<std::uint32_t,const SirConcreteResource*> resources_;
    std::unordered_map<SirValueId,const SirConcreteResource*> resource_by_value_;
    std::unordered_map<SirProvenanceId,const SirConcreteResource*> resource_by_provenance_;
    std::unordered_map<SirOpId,std::vector<const SirConcreteUndo*>> undo_by_source_;
    std::unordered_map<SirRegionId,std::vector<const SirConcreteUndo*>> undo_by_transaction_;
    std::set<SirValueId> undo_tracked_values_;
    std::unordered_map<SirValueId,bool> preallocated_; std::ostringstream out_;
    std::size_t box_counter_{0},return_counter_{0},fault_store_counter_{0},cleanup_counter_{0};
};

} // namespace

std::string LlvmWindowsX64Emitter::emit(const SirCProgram& p) const {
    SirVerifier{}.verify(p);
    std::map<std::string,std::size_t> strings;for(const auto&fn:p.functions)for(const auto&b:fn.blocks)for(const auto&op:b.instructions)if(op.opcode==SirOpcode::ConstText)strings.emplace(op.text_value,0);std::size_t si=0;for(auto&[_,id]:strings)id=si++;
    std::ostringstream o;o<<"; Generated from standalone SIR-C 6.x by Staze C++23 Compiler 0.8\n; AST/SSL/SIR-S are not visible to this backend. Concrete storage/layout decisions came from serialized SIR-C.\n";
    o<<"target triple = \""<<p.target_triple<<"\"\n\n";
    for(const auto&[s,id]:strings)o<<"@.str"<<id<<" = private unnamed_addr constant ["<<s.size()<<" x i8] c\""<<escape_bytes(s)<<"\", align 1\n";
    for(const auto& cf:p.concrete_functions)for(const auto& v:cf.values)if(v.storage==SirStorageClass::Static&&!v.scalar_replaced)
        o<<"@.stz.static."<<safe_name(cf.name)<<".v"<<v.value<<" = internal global ["<<v.size<<" x i8] zeroinitializer, align "<<v.align<<"\n";
    o<<"@.staze_io_bytes = internal global i32 0, align 4\n\n";
    o<<"declare dllimport ptr @GetStdHandle(i32)\ndeclare dllimport i32 @WriteFile(ptr, ptr, i32, ptr, ptr)\ndeclare dllimport void @ExitProcess(i32)\n";
    o<<"declare dllimport ptr @GetProcessHeap()\ndeclare dllimport ptr @HeapAlloc(ptr, i32, i64)\ndeclare dllimport i32 @HeapFree(ptr, i32, ptr)\n";
    o<<"declare void @llvm.memcpy.p0.p0.i64(ptr, ptr, i64, i1 immarg)\n";
    o<<"declare void @llvm.lifetime.start.p0(i64, ptr nocapture)\n";
    o<<"declare void @llvm.lifetime.end.p0(i64, ptr nocapture)\n";
    o<<"declare { i32, i1 } @llvm.sadd.with.overflow.i32(i32, i32)\ndeclare { i32, i1 } @llvm.ssub.with.overflow.i32(i32, i32)\ndeclare { i32, i1 } @llvm.smul.with.overflow.i32(i32, i32)\n";
    o<<"declare { i64, i1 } @llvm.sadd.with.overflow.i64(i64, i64)\ndeclare { i64, i1 } @llvm.ssub.with.overflow.i64(i64, i64)\ndeclare { i64, i1 } @llvm.smul.with.overflow.i64(i64, i64)\n";
    o<<"declare { i32, i1 } @llvm.uadd.with.overflow.i32(i32, i32)\ndeclare { i32, i1 } @llvm.usub.with.overflow.i32(i32, i32)\ndeclare { i32, i1 } @llvm.umul.with.overflow.i32(i32, i32)\n";
    o<<"declare { i64, i1 } @llvm.uadd.with.overflow.i64(i64, i64)\ndeclare { i64, i1 } @llvm.usub.with.overflow.i64(i64, i64)\ndeclare { i64, i1 } @llvm.umul.with.overflow.i64(i64, i64)\n\n";
    o<<"define internal void @staze_heap_free_if_nonnull(ptr %p) {\nentry:\n  %isnull = icmp eq ptr %p, null\n  br i1 %isnull, label %done, label %free\nfree:\n  %heap = call ptr @GetProcessHeap()\n  %ok = call i32 @HeapFree(ptr %heap, i32 0, ptr %p)\n  br label %done\ndone:\n  ret void\n}\n\n";
    o<<"define internal void @staze_shared_retain_if_nonnull(ptr %obj) {\nentry:\n  %isnull = icmp eq ptr %obj, null\n  br i1 %isnull, label %done, label %retain\nretain:\n  %base = getelementptr i8, ptr %obj, i64 -8\n  %old = atomicrmw add ptr %base, i64 1 monotonic\n  br label %done\ndone:\n  ret void\n}\n\n";
    o<<"define internal void @staze_shared_release_if_nonnull(ptr %obj) {\nentry:\n  %isnull = icmp eq ptr %obj, null\n  br i1 %isnull, label %done, label %release\nrelease:\n  %base = getelementptr i8, ptr %obj, i64 -8\n  %old = atomicrmw sub ptr %base, i64 1 acq_rel\n  %last = icmp eq i64 %old, 1\n  br i1 %last, label %free, label %done\nfree:\n  %heap = call ptr @GetProcessHeap()\n  %ok = call i32 @HeapFree(ptr %heap, i32 0, ptr %base)\n  br label %done\ndone:\n  ret void\n}\n\n";
    for(const auto&fn:p.functions)o<<FunctionEmitter(p,fn,strings).emit();
    const SirFunction* main = nullptr;
    for (const auto& fn : p.functions) {
        if (fn.name == "main") { main = &fn; break; }
    }
    if (!main) throw CompileError({}, "LLVM backend: no main instruction in SIR-C");
    if (!main->parameters.empty()) throw CompileError({}, "LLVM backend: executable main must have no parameters");
    const std::string main_name = "main";
    const auto& cm = concrete_function(p, main_name);
    if (cm.result_storage != SirStorageClass::Register)
        throw CompileError({}, "LLVM backend: executable main must return an exact-One scalar/unit value");
    o<<"define void @staze_entry() {\nentry:\n";
    o<<"  %faultbuf = alloca ["<<cm.fault_payload_size<<" x i8], align "<<cm.fault_payload_align<<"\n";
    o<<"  %r = call { i32, i64 } @stz_main(ptr %faultbuf)\n  %status = extractvalue { i32, i64 } %r, 0\n  %payload = extractvalue { i32, i64 } %r, 1\n  %ok = icmp eq i32 %status, 0\n  br i1 %ok, label %success, label %failure\nsuccess:\n";
    if(main->result_type==SirType::Unit)o<<"  call void @ExitProcess(i32 0)\n";else o<<"  %exit = trunc i64 %payload to i32\n  call void @ExitProcess(i32 %exit)\n";
    o<<"  unreachable\nfailure:\n  call void @ExitProcess(i32 1)\n  unreachable\n}\n";return o.str();
}

} // namespace staze
