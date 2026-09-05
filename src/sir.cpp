#include "staze/sir.hpp"
#include "staze/ast.hpp"
#include "staze/diagnostic.hpp"
#include "staze/semantic.hpp"

#include <algorithm>
#include <charconv>
#include <cctype>
#include <cstdint>
#include <functional>
#include <map>
#include <set>
#include <sstream>
#include <string_view>
#include <type_traits>
#include <unordered_map>
#include <unordered_set>

namespace staze {
namespace {

[[noreturn]] void fail(const std::string& message) { throw CompileError({}, message); }
SirLocation loc(const SourceLocation& w) { return {static_cast<std::uint32_t>(w.line), static_cast<std::uint32_t>(w.column)}; }

SirType to_sir_type(const SemType& t) {
    switch (t.base) {
        case BaseType::Unit: return SirType::Unit;
        case BaseType::Bool: return SirType::Bool;
        case BaseType::I32: return SirType::I32;
        case BaseType::I64: return SirType::I64;
        case BaseType::U32: return SirType::U32;
        case BaseType::U64: return SirType::U64;
        case BaseType::Text: return SirType::Text;
        case BaseType::Nominal: return SirType::Nominal;
    }
    fail("SIR: unknown semantic type");
}
SirCardinality to_sir_cardinality(Cardinality c) {
    switch (c) {
        case Cardinality::One: return SirCardinality::One;
        case Cardinality::ZeroOrOne: return SirCardinality::ZeroOrOne;
        case Cardinality::Many: return SirCardinality::Many;
    }
    fail("SIR: unknown semantic cardinality");
}
SirTypeDesc to_desc(const SemType& t) { return {to_sir_type(t), to_sir_cardinality(t.cardinality), t.base == BaseType::Nominal ? t.nominal : std::string{}}; }

std::string hex_encode(std::string_view s) {
    static constexpr char h[] = "0123456789abcdef";
    std::string out; out.reserve(s.size() * 2);
    for (unsigned char c : s) { out.push_back(h[c >> 4]); out.push_back(h[c & 15]); }
    return out.empty() ? "-" : out;
}
int hex_digit(char c) {
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return 10 + c - 'a';
    if (c >= 'A' && c <= 'F') return 10 + c - 'A';
    return -1;
}
std::string hex_decode(const std::string& s) {
    if (s == "-") return {};
    if (s.size() % 2) fail("SIR deserialize: malformed hex string");
    std::string out; out.reserve(s.size() / 2);
    for (std::size_t i = 0; i < s.size(); i += 2) {
        const int a = hex_digit(s[i]), b = hex_digit(s[i + 1]);
        if (a < 0 || b < 0) fail("SIR deserialize: malformed hex digit");
        out.push_back(static_cast<char>((a << 4) | b));
    }
    return out;
}
template<class T> T parse_num(const std::string& s, const char* what) {
    T value{}; const auto [p, ec] = std::from_chars(s.data(), s.data() + s.size(), value);
    if (ec != std::errc{} || p != s.data() + s.size()) fail(std::string("SIR deserialize: invalid ") + what + " '" + s + "'");
    return value;
}
std::vector<std::string> split_ws(const std::string& line) {
    std::istringstream in(line); std::vector<std::string> out; std::string s;
    while (in >> s) out.push_back(std::move(s));
    return out;
}

std::string before_last_dot(const std::string& s) {
    const auto p = s.rfind('.'); return p == std::string::npos ? std::string{} : s.substr(0, p);
}
std::string after_last_dot(const std::string& s) {
    const auto p = s.rfind('.'); return p == std::string::npos ? s : s.substr(p + 1);
}

struct Binding {
    SirTypeDesc desc;
    SirValueId value{0};
    bool revisable{false};
    SirRegionId region{0};
    SirProvenanceId provenance{0};
    std::vector<std::string> authorities{"read"};
    std::string ownership{"value"};
    std::string resource_family;
    std::string borrow_mode{"none"};
};
using Environment = std::map<std::string, Binding>;
using FaultRouting = std::map<std::string, SirBlockId>;
struct EdgeEnvironment { SirBlockId predecessor{0}; Environment env; };
struct LoopContext {
    SirBlockId header{0}; SirBlockId exit{0};
    std::vector<EdgeEnvironment> continues; std::vector<EdgeEnvironment> breaks;
};

std::vector<std::string> sorted_unique(std::vector<std::string> v) {
    std::sort(v.begin(), v.end()); v.erase(std::unique(v.begin(), v.end()), v.end()); return v;
}
std::vector<std::string> intersect_auth(std::vector<std::string> a, const std::vector<std::string>& b) {
    a = sorted_unique(std::move(a)); auto bb = sorted_unique(b); std::vector<std::string> out;
    std::set_intersection(a.begin(), a.end(), bb.begin(), bb.end(), std::back_inserter(out));
    if (out.empty()) out.push_back("read");
    return out;
}

class FunctionLowerer {
public:
    FunctionLowerer(const Program& program, const SSLProgram& ssl, const InstructionDecl& source, const InstructionFacts& facts)
        : program_(program), ssl_(ssl), source_(source), facts_(facts) {
        out_.name = source.name; out_.is_public = source.is_public;
        const auto rd = to_desc(facts.result_type); out_.result_type = rd.type; out_.result_cardinality = rd.cardinality; out_.result_nominal_type = rd.nominal;
        out_.result_borrow_mode = facts.result_borrow_mode; out_.result_borrow_from = facts.result_borrow_from;
        out_.declared_faults.assign(facts.declared_faults.begin(), facts.declared_faults.end());
        out_.effects.assign(facts.effects.begin(), facts.effects.end());
        std::sort(out_.declared_faults.begin(), out_.declared_faults.end()); std::sort(out_.effects.begin(), out_.effects.end());

        out_.root_region = new_region("fn." + source.name, "function", 0, "call", "compiler", "scope");
        region_stack_.push_back(out_.root_region);
        out_.entry = new_block("entry"); current_ = out_.entry;

        for (const auto& f : out_.declared_faults) {
            const auto b = new_block("fault.return." + f);
            SirTerminator t; t.kind = SirTerminatorKind::FaultReturn; t.fault = f; t.fault_payload_passthrough = true; t.where = loc(source.where);
            block(b).terminator = std::move(t); default_faults_[f] = b;
        }

        for (const auto& p : source.params) {
            const auto it = facts.locals.find(p.name); if (it == facts.locals.end()) throw CompileError(p.where, "DLE: parameter lacks SSL facts");
            const auto d = to_desc(it->second.type); const auto v = next_value_++;
            out_.parameters.push_back({p.name, p.authority, it->second.ownership, d.type, d.cardinality, d.nominal, v});
            auto auth = std::vector<std::string>(it->second.authorities.begin(), it->second.authorities.end()); auth = sorted_unique(std::move(auth));
            SirProvenanceId param_prov = 0;
            if (!it->second.resource_family.empty() || it->second.ownership.rfind("borrow", 0) == 0 || it->second.ownership == "shared" || it->second.ownership == "sent" || it->second.ownership == "owned") {
                const std::string borrow_mode = it->second.ownership == "borrow-mut" ? "unique-mut" : (it->second.ownership == "borrow-shared" ? "shared-read" : "none");
                param_prov = new_provenance(out_.root_region, "param." + p.name, it->second.ownership, false, 0, {}, it->second.resource_family, borrow_mode);
            }
            register_value(v, d, out_.root_region, param_prov, auth, it->second.ownership, it->second.resource_family,
                           it->second.ownership == "borrow-mut" ? "unique-mut" : (it->second.ownership == "borrow-shared" ? "shared-read" : "none"));
            env_[p.name] = Binding{d, v, it->second.revisable, out_.root_region, param_prov, auth, it->second.ownership, it->second.resource_family, it->second.ownership == "borrow-mut" ? "unique-mut" : (it->second.ownership == "borrow-shared" ? "shared-read" : "none")};
        }
    }

    SirFunction run() {
        const bool terminated = lower_block(source_.body);
        if (!terminated) {
            if (out_.result_type == SirType::Unit && out_.result_cardinality == SirCardinality::One) {
                SirTerminator t; t.kind = SirTerminatorKind::Return; t.where = loc(source_.where); set_term(std::move(t));
            } else {
                SirTerminator t; t.kind = SirTerminatorKind::Unreachable; t.where = loc(source_.where); set_term(std::move(t));
            }
        }
        synthesize_task_captures();
        synthesize_effect_ssa();
        std::sort(out_.values.begin(), out_.values.end(), [](const auto& a, const auto& b){ return a.value < b.value; });
        std::sort(out_.regions.begin(), out_.regions.end(), [](const auto& a, const auto& b){ return a.id < b.id; });
        std::sort(out_.provenances.begin(), out_.provenances.end(), [](const auto& a, const auto& b){ return a.id < b.id; });
        return std::move(out_);
    }

private:
    SirRegionId new_region(std::string name, std::string kind, SirRegionId parent,
                           std::string lifetime, std::string allocation, std::string reclaim) {
        const SirRegionId id = next_region_++;
        out_.regions.push_back({id, parent, std::move(name), std::move(kind), std::move(lifetime), std::move(allocation), std::move(reclaim)});
        return id;
    }
    bool region_inside(SirRegionId child, SirRegionId ancestor) const {
        if (!ancestor) return true;
        std::set<SirRegionId> seen;
        while (child && seen.insert(child).second) {
            if (child == ancestor) return true;
            auto it = std::find_if(out_.regions.begin(), out_.regions.end(), [&](const auto& r){ return r.id == child; });
            if (it == out_.regions.end()) break;
            child = it->parent;
        }
        return false;
    }
    SirRegionId borrow_region(SirRegionId owner_region, SourceLocation where) const {
        const auto lexical = region_stack_.back();
        if (region_inside(lexical, owner_region)) return lexical;
        if (region_inside(owner_region, lexical)) return owner_region;
        throw CompileError(where, "DLE: borrow lifetime is unrelated to owner lifetime");
    }
    SirRegionId ensure_pool_region(const std::string& pool) {
        const auto& pf = ssl_.pools.at(pool);
        const auto lifetime = pf.lifetime.value_or("call");
        if (lifetime == "call" || lifetime == "frame" || lifetime == "static") return out_.root_region;
        if (lifetime == "scope") return region_stack_.back();
        if (lifetime == "transaction") {
            for (auto it = region_stack_.rbegin(); it != region_stack_.rend(); ++it) {
                auto rit = std::find_if(out_.regions.begin(), out_.regions.end(), [&](const auto& r){ return r.id == *it; });
                if (rit != out_.regions.end() && rit->kind == "transaction") return *it;
            }
            throw CompileError(source_.where, "DLE: @lifetime(transaction) allocation requires an enclosing transaction region");
        }
        return out_.root_region;
    }
    SirProvenanceId new_provenance(SirRegionId region, std::string origin, std::string ownership, bool pinned = false,
                                     SirProvenanceId parent = 0, std::string subobject = {},
                                     std::string resource_family = {}, std::string borrow_mode = "none") {
        const auto id = next_provenance_++;
        SirProvenance p; p.id=id; p.region=region; p.parent=parent; p.origin=std::move(origin); p.ownership=std::move(ownership);
        p.subobject=std::move(subobject); p.resource_family=std::move(resource_family); p.borrow_mode=std::move(borrow_mode); p.pinned=pinned;
        out_.provenances.push_back(std::move(p));
        return id;
    }
    void register_value(SirValueId id, const SirTypeDesc& d, SirRegionId region, SirProvenanceId provenance,
                        std::vector<std::string> authorities, std::string ownership,
                        std::string resource_family = {}, std::string borrow_mode = "none") {
        authorities = sorted_unique(std::move(authorities));
        SirValueSemantics v; v.value=id; v.type=d.type; v.cardinality=d.cardinality; v.nominal_type=d.nominal; v.region=region; v.provenance=provenance;
        v.authorities=std::move(authorities); v.ownership=std::move(ownership); v.resource_family=std::move(resource_family); v.borrow_mode=std::move(borrow_mode);
        out_.values.push_back(std::move(v));
    }
    void add_borrow_edge(SirProvenanceId owner, SirProvenanceId borrower, SirRegionId region, std::string mode,
                         bool cross_boundary=false, std::string source_parameter={}) {
        if(!owner || !borrower) throw CompileError(source_.where, "DLE: borrow graph requires concrete owner/borrower provenance");
        out_.borrow_edges.push_back({next_borrow_edge_++, owner, borrower, region, std::move(mode), cross_boundary, std::move(source_parameter)});
    }
    void add_ownership_edge(SirProvenanceId owner, SirProvenanceId child, SirRegionId region, std::string role,
                            std::string path, std::uint32_t ordinal) {
        if(!owner || !child || owner==child) return;
        out_.ownership_edges.push_back({next_ownership_edge_++, owner, child, region, std::move(role), std::move(path), ordinal});
    }
    SirRegionId active_transaction_region() const {
        for(auto it=region_stack_.rbegin();it!=region_stack_.rend();++it){
            auto rr=std::find_if(out_.regions.begin(),out_.regions.end(),[&](const auto&r){return r.id==*it;});
            if(rr!=out_.regions.end()&&rr->kind=="transaction")return *it;
        }
        return 0;
    }
    void add_rollback_obligation(SirProvenanceId provenance, SirOpId source_op, std::string action) {
        const auto tx=active_transaction_region();
        if(!tx)return;
        out_.rollback_obligations.push_back({next_rollback_++,tx,provenance,source_op,std::move(action)});
    }
    std::string resource_family_for_desc(const SirTypeDesc& d, std::set<std::string>* external_seen=nullptr) const {
        std::set<std::string> local; auto& seen=external_seen?*external_seen:local;
        if(d.cardinality==SirCardinality::Many){SirTypeDesc e=d;e.cardinality=SirCardinality::One;auto inner=resource_family_for_desc(e,&seen);return inner.empty()?std::string{}:"many:"+inner;}
        if(d.cardinality==SirCardinality::ZeroOrOne){SirTypeDesc e=d;e.cardinality=SirCardinality::One;auto inner=resource_family_for_desc(e,&seen);return inner.empty()?std::string{}:"optional:"+inner;}
        if(d.type!=SirType::Nominal || !seen.insert(d.nominal).second) return {};
        if(auto p=ssl_.pools.find(d.nominal);p!=ssl_.pools.end()){
            if(p->second.allocation && *p->second.allocation=="heap") return (p->second.ownership=="shared"?"shared-pool:":"heap-pool:")+d.nominal;
            for(const auto& f:p->second.fields){auto x=to_desc(f.type);if(!resource_family_for_desc(x,&seen).empty())return "composite:"+d.nominal;}return {};
        }
        if(auto ds=ssl_.datasets.find(d.nominal);ds!=ssl_.datasets.end()){for(const auto&f:ds->second.fields){auto x=to_desc(f.type);if(!resource_family_for_desc(x,&seen).empty())return "composite:"+d.nominal;}return {};}
        if(auto c=ssl_.choices.find(d.nominal);c!=ssl_.choices.end()){for(const auto&a:c->second.cases)for(const auto&f:a.fields){auto x=to_desc(f.type);if(!resource_family_for_desc(x,&seen).empty())return "composite:"+d.nominal;}return {};}
        return {};
    }
    const SirValueSemantics& value_sem(SirValueId id) const {
        auto it = std::find_if(out_.values.begin(), out_.values.end(), [&](const auto& v){ return v.value == id; });
        if (it == out_.values.end()) throw CompileError(source_.where, "DLE internal: missing value semantics");
        return *it;
    }

    SirBlockId new_block(std::string label, SirRegionId explicit_region = 0) {
        SirBlock b; b.id = static_cast<SirBlockId>(out_.blocks.size()); b.label = std::move(label);
        b.region = explicit_region ? explicit_region : (region_stack_.empty() ? 0 : region_stack_.back());
        b.terminator.kind = SirTerminatorKind::Unreachable;
        out_.blocks.push_back(std::move(b)); return out_.blocks.back().id;
    }
    SirBlock& block(SirBlockId id) { if (id >= out_.blocks.size()) throw CompileError(source_.where, "DLE internal: invalid block id"); return out_.blocks[id]; }
    const SirBlock& block(SirBlockId id) const { if (id >= out_.blocks.size()) throw CompileError(source_.where, "DLE internal: invalid block id"); return out_.blocks[id]; }
    bool has_term(SirBlockId id) const { return block(id).terminator.kind != SirTerminatorKind::Unreachable; }
    void set_term(SirTerminator t) { if (has_term(current_)) throw CompileError(source_.where, "DLE internal: block already terminated"); block(current_).terminator = std::move(t); }
    void branch(SirBlockId target, SirLocation where = {}) { SirTerminator t; t.kind = SirTerminatorKind::Br; t.target = target; t.where = where; set_term(std::move(t)); }

    SirValueId append(SirOpcode opcode, SirTypeDesc desc, std::vector<SirValueId> operands, SirLocation where,
                      std::vector<SirFaultEdge> faults = {}, std::vector<std::string> effects = {},
                      std::string semantic_name = {}, std::uint32_t tag = 0,
                      SirRegionId region = 0, SirProvenanceId provenance = 0,
                      std::vector<std::string> authorities = {"read"}, std::string ownership = "value",
                      std::string resource_family = {}, std::string borrow_mode = "none") {
        SirInstruction op; op.id = next_op_++; op.opcode = opcode; op.result = next_value_++; op.type = desc.type; op.cardinality = desc.cardinality; op.nominal_type = desc.nominal;
        op.operands = std::move(operands); op.fault_edges = std::move(faults); op.effects = sorted_unique(std::move(effects)); op.semantic_name = std::move(semantic_name); op.tag = tag;
        op.region = region ? region : region_stack_.back(); op.provenance = provenance; op.where = where;
        block(current_).instructions.push_back(std::move(op));
        register_value(block(current_).instructions.back().result, desc, block(current_).instructions.back().region, provenance, std::move(authorities), std::move(ownership), std::move(resource_family), std::move(borrow_mode));
        return block(current_).instructions.back().result;
    }
    SirValueId const_unit(SirLocation w) { return append(SirOpcode::ConstUnit, {SirType::Unit, SirCardinality::One, {}}, {}, w); }
    SirValueId const_bool(bool v, SirLocation w) { const auto id = append(SirOpcode::ConstBool, {SirType::Bool, SirCardinality::One, {}}, {}, w); block(current_).instructions.back().bool_value = v; return id; }
    SirValueId const_int(std::int64_t v, SirTypeDesc d, SirLocation w) { const auto id = append(SirOpcode::ConstInt, d, {}, w); block(current_).instructions.back().int_value = v; return id; }
    SirValueId const_text(const std::string& v, SirLocation w) { const auto id = append(SirOpcode::ConstText, {SirType::Text, SirCardinality::One, {}}, {}, w); block(current_).instructions.back().text_value = v; return id; }
    SirValueId optional_none(SirTypeDesc d, SirLocation w) { return append(SirOpcode::OptionalNone, d, {}, w); }
    SirValueId many_empty(SirTypeDesc d, SirLocation w) { return append(SirOpcode::ManyMake, d, {}, w); }

    SirInstruction& find_op(SirBlockId b, SirOpId id) { for (auto& op : block(b).instructions) if (op.id == id) return op; throw CompileError(source_.where, "DLE internal: operation not found"); }
    const ExprFacts& expr_facts(const Expr& e) const { auto it = ssl_.expressions.find(e.id); if (it == ssl_.expressions.end()) throw CompileError(e.where, "DLE: expression lacks SSL facts"); return it->second; }
    SirTypeDesc expr_desc(const Expr& e) const { return to_desc(expr_facts(e).type); }

    struct FieldPathInfo {
        std::string root_name;
        std::string owner_type;
        std::string field_name;
        Binding root;
        SirTypeDesc desc;
        bool revisable{false};
        std::size_t ordinal{0};
    };
    FieldPathInfo resolve_field_path(const std::string& path, SourceLocation where) const {
        const auto dot = path.find('.');
        if (dot == std::string::npos || path.find('.', dot + 1) != std::string::npos)
            throw CompileError(where, "Compiler 0.8 addressable field paths currently require exactly root.field");
        const auto root_name = path.substr(0, dot), field_name = path.substr(dot + 1);
        auto bit = env_.find(root_name); if (bit == env_.end()) throw CompileError(where, "DLE: unknown aggregate root '" + root_name + "'");
        if (bit->second.desc.type != SirType::Nominal || bit->second.desc.cardinality != SirCardinality::One)
            throw CompileError(where, "DLE: field root must be exact-One nominal aggregate");
        const auto owner = bit->second.desc.nominal;
        const std::vector<FieldFacts>* fields = nullptr;
        if (auto it = ssl_.datasets.find(owner); it != ssl_.datasets.end()) fields = &it->second.fields;
        else if (auto it = ssl_.pools.find(owner); it != ssl_.pools.end()) fields = &it->second.fields;
        else throw CompileError(where, "DLE: type '" + owner + "' has no addressable field schema");
        auto fit = std::find_if(fields->begin(), fields->end(), [&](const auto& f){ return f.name == field_name; });
        if (fit == fields->end()) throw CompileError(where, "DLE: unknown field '" + field_name + "' on '" + owner + "'");
        const auto ordinal = static_cast<std::size_t>(std::distance(fields->begin(), fit));
        return {root_name, owner, field_name, bit->second, to_desc(fit->type), fit->revisable, ordinal};
    }
    SirValueId lower_field_load(const std::string& path, SourceLocation where) {
        const auto f = resolve_field_path(path, where);
        const auto prov = new_provenance(f.root.region, "subobject.load." + f.owner_type + "." + f.field_name,
                                         "subobject", false, f.root.provenance, f.field_name);
        std::vector<std::string> auth{"read"}; if (f.revisable && std::find(f.root.authorities.begin(),f.root.authorities.end(),"revise")!=f.root.authorities.end()) auth.push_back("revise");
        return append(SirOpcode::FieldLoad, f.desc, {f.root.value}, loc(where), {}, {}, f.owner_type + "." + f.field_name,
                      static_cast<std::uint32_t>(f.ordinal), f.root.region, prov, auth, "subobject-value");
    }
    SirValueId lower_field_address(const std::string& path, SourceLocation where, bool mut=false) {
        const auto f = resolve_field_path(path, where);
        const auto br = borrow_region(f.root.region, where);
        const auto mode=mut?std::string("unique-mut"):std::string("shared-read");
        const auto own=mut?std::string("borrow-mut"):std::string("borrow-shared");
        const auto family=resource_family_for_desc(f.desc);
        const auto prov = new_provenance(br, "subobject.address." + f.owner_type + "." + f.field_name,
                                         own, false, f.root.provenance, f.field_name, family, mode);
        add_borrow_edge(f.root.provenance,prov,br,mode);
        return append(SirOpcode::FieldAddress, f.desc, {f.root.value}, loc(where), {}, {}, f.owner_type + "." + f.field_name,
                      static_cast<std::uint32_t>(f.ordinal), br, prov, mut?std::vector<std::string>{"read","revise"}:std::vector<std::string>{"read"}, own, family, mode);
    }

    std::vector<SirFaultEdge> route_faults(const std::vector<std::string>& names, const FaultRouting& routing, SourceLocation where) const {
        std::vector<SirFaultEdge> out; for (const auto& n : names) { auto it = routing.find(n); if (it == routing.end()) throw CompileError(where, "DLE: no fault route for '" + n + "'"); out.push_back({n, it->second}); }
        std::sort(out.begin(), out.end(), [](const auto& a, const auto& b){ return a.fault < b.fault; }); return out;
    }

    Binding binding_for_value(const Binding& base, SirValueId value) const {
        Binding b = base; b.value = value; const auto& v = value_sem(value); b.region = v.region; b.provenance = v.provenance; b.authorities = v.authorities; b.ownership = v.ownership; return b;
    }

    Environment merge_environments(SirBlockId join, const Environment& baseline, const std::vector<EdgeEnvironment>& incoming, SirLocation where) {
        Environment result = baseline;
        if (incoming.empty()) return result;
        for (const auto& [name, base] : baseline) {
            std::vector<SirPhiIncoming> vals; vals.reserve(incoming.size());
            bool all_same = true; SirValueId first = 0; bool first_set = false;
            Binding merged = base;
            for (const auto& edge : incoming) {
                auto it = edge.env.find(name); if (it == edge.env.end()) throw CompileError(source_.where, "DLE internal: missing binding at merge");
                if (it->second.desc != base.desc) throw CompileError(source_.where, "DLE internal: binding type changed across control flow");
                vals.push_back({edge.predecessor, it->second.value});
                if (!first_set) { first = it->second.value; first_set = true; merged = it->second; }
                else { if (it->second.value != first) all_same = false; merged.authorities = intersect_auth(merged.authorities, it->second.authorities); if (merged.provenance != it->second.provenance) merged.provenance = 0; if (merged.region != it->second.region) merged.region = out_.root_region; }
            }
            if (all_same) { result[name] = merged; continue; }
            SirInstruction op; op.id = next_op_++; op.opcode = SirOpcode::Phi; op.result = next_value_++; op.type = base.desc.type; op.cardinality = base.desc.cardinality; op.nominal_type = base.desc.nominal; op.phi_inputs = std::move(vals); op.region = merged.region; op.provenance = merged.provenance; op.where = where;
            block(join).instructions.push_back(std::move(op));
            register_value(block(join).instructions.back().result, base.desc, merged.region, merged.provenance, merged.authorities, merged.ownership, merged.resource_family, merged.borrow_mode);
            merged.value = block(join).instructions.back().result; result[name] = merged;
        }
        return result;
    }

    SirValueId lower_expr(const Expr& e, const FaultRouting& outer) {
        if (e.handlers.empty()) return lower_raw_expr(e, outer);
        FaultRouting routing = outer; std::vector<std::pair<const FaultHandler*, SirBlockId>> handler_blocks;
        for (const auto& h : e.handlers) {
            const auto b = new_block("fault.handle." + h.fault_name + "." + std::to_string(next_handler_++)); routing[h.fault_name] = b; handler_blocks.push_back({&h, b});
        }
        const auto result_desc = expr_desc(e); const auto value = lower_raw_expr(e, routing); const auto normal_pred = current_;
        const auto done = new_block("fault.done." + std::to_string(next_handler_++)); branch(done, loc(e.where));
        std::vector<SirPhiIncoming> incoming; if (!(result_desc.type == SirType::Unit && result_desc.cardinality == SirCardinality::One)) incoming.push_back({normal_pred, value});
        for (const auto& [h, b] : handler_blocks) {
            current_ = b; SirValueId hv = 0;
            if (h->kind == HandlerKind::Bypass) hv = lower_expr(*h->replacement, outer);
            else if (result_desc.cardinality == SirCardinality::ZeroOrOne) hv = optional_none(result_desc, loc(h->where));
            else if (result_desc.cardinality == SirCardinality::Many) hv = many_empty(result_desc, loc(h->where));
            else hv = const_unit(loc(h->where));
            const auto pred = current_; branch(done, loc(h->where));
            if (!(result_desc.type == SirType::Unit && result_desc.cardinality == SirCardinality::One)) incoming.push_back({pred, hv});
        }
        current_ = done;
        if (result_desc.type == SirType::Unit && result_desc.cardinality == SirCardinality::One) return const_unit(loc(e.where));
        SirInstruction op; op.id = next_op_++; op.opcode = SirOpcode::Phi; op.result = next_value_++; op.type = result_desc.type; op.cardinality = result_desc.cardinality; op.nominal_type = result_desc.nominal; op.phi_inputs = std::move(incoming); op.region = region_stack_.back(); op.where = loc(e.where);
        block(current_).instructions.push_back(std::move(op)); register_value(block(current_).instructions.back().result, result_desc, region_stack_.back(), 0, {"read"}, "value"); return block(current_).instructions.back().result;
    }

    SirValueId lower_raw_expr(const Expr& e, const FaultRouting& routing) {
        const auto sf = expr_facts(e); const auto desc = to_desc(sf.type);
        return std::visit([&](const auto& n) -> SirValueId {
            using T = std::decay_t<decltype(n)>;
            if constexpr (std::is_same_v<T, IntExpr>) {
                if (!sf.constant_int) throw CompileError(e.where, "DLE: integer literal lacks constant SSL value");
                return const_int(*sf.constant_int, desc, loc(e.where));
            } else if constexpr (std::is_same_v<T, BoolExpr>) {
                return const_bool(n.value, loc(e.where));
            } else if constexpr (std::is_same_v<T, TextExpr>) {
                return const_text(n.value, loc(e.where));
            } else if constexpr (std::is_same_v<T, NameExpr>) {
                auto it = env_.find(n.name); if (it != env_.end()) return it->second.value;
                if (n.name.find('.') != std::string::npos) {
                    if(sf.borrow_mode=="shared-read"||sf.borrow_mode=="unique-mut") return lower_field_address(n.name,e.where,sf.borrow_mode=="unique-mut");
                    return lower_field_load(n.name, e.where);
                }
                throw CompileError(e.where, "DLE: unknown SSA binding '" + n.name + "'");
            } else if constexpr (std::is_same_v<T, UnaryExpr>) {
                const auto a = lower_expr(*n.operand, routing);
                if (n.op == UnaryOp::Not) return append(SirOpcode::Not, desc, {a}, loc(e.where));
                return append(SirOpcode::CheckedNeg, desc, {a}, loc(e.where), route_faults({"ArithmeticOverflow"}, routing, e.where));
            } else if constexpr (std::is_same_v<T, BinaryExpr>) {
                if (n.op == BinaryOp::And || n.op == BinaryOp::Or) {
                    const auto lhs = lower_expr(*n.lhs, routing); const auto lhs_pred = current_;
                    const auto shortcut = const_bool(n.op == BinaryOp::Or, loc(e.where));
                    const auto rhs_b = new_block("logic.rhs." + std::to_string(next_logic_++)); const auto done = new_block("logic.done." + std::to_string(next_logic_++));
                    SirTerminator t; t.kind = SirTerminatorKind::CondBr; t.condition = lhs;
                    if (n.op == BinaryOp::And) { t.true_target = rhs_b; t.false_target = done; } else { t.true_target = done; t.false_target = rhs_b; }
                    t.where = loc(e.where); set_term(std::move(t));
                    current_ = rhs_b; const auto rhs = lower_expr(*n.rhs, routing); const auto rhs_pred = current_; branch(done, loc(e.where)); current_ = done;
                    SirInstruction op; op.id = next_op_++; op.opcode = SirOpcode::Phi; op.result = next_value_++; op.type = SirType::Bool; op.cardinality = SirCardinality::One; op.phi_inputs = {{lhs_pred, shortcut}, {rhs_pred, rhs}}; op.region = region_stack_.back(); op.where = loc(e.where);
                    block(current_).instructions.push_back(std::move(op)); register_value(block(current_).instructions.back().result, {SirType::Bool, SirCardinality::One, {}}, region_stack_.back(), 0, {"read"}, "value"); return block(current_).instructions.back().result;
                }
                const auto a = lower_expr(*n.lhs, routing); const auto b = lower_expr(*n.rhs, routing);
                switch (n.op) {
                    case BinaryOp::Add: return append(SirOpcode::CheckedAdd, desc, {a,b}, loc(e.where), route_faults({"ArithmeticOverflow"}, routing, e.where));
                    case BinaryOp::Sub: return append(SirOpcode::CheckedSub, desc, {a,b}, loc(e.where), route_faults({"ArithmeticOverflow"}, routing, e.where));
                    case BinaryOp::Mul: return append(SirOpcode::CheckedMul, desc, {a,b}, loc(e.where), route_faults({"ArithmeticOverflow"}, routing, e.where));
                    case BinaryOp::Div: {
                        std::vector<std::string> fs{"DivideByZero"}; if (sf.type.base == BaseType::I32 || sf.type.base == BaseType::I64) fs.push_back("ArithmeticOverflow"); return append(SirOpcode::CheckedDiv, desc, {a,b}, loc(e.where), route_faults(fs, routing, e.where));
                    }
                    case BinaryOp::Mod: {
                        std::vector<std::string> fs{"DivideByZero"}; if (sf.type.base == BaseType::I32 || sf.type.base == BaseType::I64) fs.push_back("ArithmeticOverflow"); return append(SirOpcode::CheckedMod, desc, {a,b}, loc(e.where), route_faults(fs, routing, e.where));
                    }
                    case BinaryOp::Equal: return append(SirOpcode::CompareEq, desc, {a,b}, loc(e.where));
                    case BinaryOp::NotEqual: return append(SirOpcode::CompareNe, desc, {a,b}, loc(e.where));
                    case BinaryOp::Less: return append(SirOpcode::CompareLt, desc, {a,b}, loc(e.where));
                    case BinaryOp::LessEqual: return append(SirOpcode::CompareLe, desc, {a,b}, loc(e.where));
                    case BinaryOp::Greater: return append(SirOpcode::CompareGt, desc, {a,b}, loc(e.where));
                    case BinaryOp::GreaterEqual: return append(SirOpcode::CompareGe, desc, {a,b}, loc(e.where));
                    case BinaryOp::And: case BinaryOp::Or: break;
                }
            } else if constexpr (std::is_same_v<T, CallExpr>) {
                if (n.callee == "stdout.write_text") {
                    const auto a = lower_expr(n.args.at(0), routing); return append(SirOpcode::WriteText, desc, {a}, loc(e.where), route_faults({"IoFailure"}, routing, e.where), {"io.write"});
                }
                if (n.callee == "some") {
                    const auto a = lower_expr(n.args.at(0), routing);
                    const auto family = sf.resource_family.empty() ? resource_family_for_desc(desc) : sf.resource_family;
                    if (family.empty()) return append(SirOpcode::OptionalSome, desc, {a}, loc(e.where));
                    const auto prov = new_provenance(region_stack_.back(), "optional.some", "owned", false, 0, {}, family);
                    const auto id = append(SirOpcode::OptionalSome, desc, {a}, loc(e.where), {}, {}, {}, 0,
                                           region_stack_.back(), prov, {"read","move","send"}, "owned", family);
                    const auto& av = value_sem(a);
                    if (av.provenance && !av.resource_family.empty()) add_ownership_edge(prov,av.provenance,region_stack_.back(),"optional","payload",0);
                    return id;
                }
                if (n.callee == "none") {
                    const auto family = sf.resource_family.empty() ? resource_family_for_desc(desc) : sf.resource_family;
                    if (family.empty()) return append(SirOpcode::OptionalNone, desc, {}, loc(e.where));
                    const auto prov = new_provenance(region_stack_.back(), "optional.none", "owned", false, 0, {}, family);
                    return append(SirOpcode::OptionalNone, desc, {}, loc(e.where), {}, {}, {}, 0,
                                  region_stack_.back(), prov, {"read","move","send"}, "owned", family);
                }
                if (n.callee == "many") {
                    std::vector<SirValueId> args; for (const auto& x : n.args) args.push_back(lower_expr(x, routing));
                    const auto family = sf.resource_family.empty() ? resource_family_for_desc(desc) : sf.resource_family;
                    if (family.empty()) return append(SirOpcode::ManyMake, desc, std::move(args), loc(e.where));
                    const auto prov = new_provenance(region_stack_.back(), "many.inline", "owned", false, 0, {}, family);
                    const auto id = append(SirOpcode::ManyMake, desc, args, loc(e.where), {}, {}, {}, 0,
                                           region_stack_.back(), prov, {"read","move","send"}, "owned", family);
                    for (std::size_t i=0;i<args.size();++i) { const auto& av=value_sem(args[i]); if(av.provenance&&!av.resource_family.empty()) add_ownership_edge(prov,av.provenance,region_stack_.back(),"many",std::to_string(i),static_cast<std::uint32_t>(i)); }
                    return id;
                }
                if (n.callee == "dynamic_many") {
                    const auto cap = lower_expr(n.args.at(0), routing);
                    const auto prov = new_provenance(region_stack_.back(), "many.dynamic", "owned", false, 0, {}, "dynamic-many");
                    return append(SirOpcode::ManyDynamic, desc, {cap}, loc(e.where),
                                  route_faults({"AllocationFailure","ArithmeticOverflow"}, routing, e.where),
                                  {"memory.allocate"}, "dynamic_many", 0, region_stack_.back(), prov,
                                  {"read","move","send","revise"}, "owned", "dynamic-many");
                }
                if (n.callee == "many.push") {
                    const auto list = lower_expr(n.args.at(0), routing); const auto item = lower_expr(n.args.at(1), routing);
                    return append(SirOpcode::ManyPush, {SirType::Unit,SirCardinality::One,{}}, {list,item}, loc(e.where),
                                  route_faults({"AllocationFailure","ArithmeticOverflow"}, routing, e.where),
                                  {"memory.reallocate"}, "many.push");
                }
                if (n.callee == "many.length") {
                    const auto list = lower_expr(n.args.at(0), routing);
                    return append(SirOpcode::ManyLength, {SirType::U64,SirCardinality::One,{}}, {list}, loc(e.where), {}, {}, "many.length");
                }
                if (n.callee == "borrow" || n.callee == "borrow_mut") {
                    const bool mut=n.callee=="borrow_mut";
                    if (const auto* named = std::get_if<NameExpr>(&n.args.at(0).node); named && named->name.find('.') != std::string::npos)
                        return lower_field_address(named->name, n.args.at(0).where, mut);
                    const auto a = lower_expr(n.args.at(0), routing); const auto& vs = value_sem(a);
                    const auto br = borrow_region(vs.region, e.where);const auto mode=mut?std::string("unique-mut"):std::string("shared-read");const auto own=mut?std::string("borrow-mut"):std::string("borrow-shared");
                    const auto prov=new_provenance(br,"borrow."+std::to_string(a),own,false,vs.provenance,{},vs.resource_family,mode);add_borrow_edge(vs.provenance,prov,br,mode);
                    return append(mut?SirOpcode::BorrowMut:SirOpcode::Borrow, desc, {a}, loc(e.where), {}, {}, {}, 0, br, prov, mut?std::vector<std::string>{"read","revise"}:std::vector<std::string>{"read"}, own, vs.resource_family, mode);
                }
                if (n.callee == "retain" || n.callee == "share") {
                    const auto a=lower_expr(n.args.at(0),routing);const auto&vs=value_sem(a);const auto prov=new_provenance(vs.region,"shared.retain."+std::to_string(a),"shared",false,vs.provenance,{},vs.resource_family);
                    const auto id=append(SirOpcode::SharedRetain,desc,{a},loc(e.where),{}, {"shared.retain"},{},0,vs.region,prov,{"read","share","send"},"shared",vs.resource_family);
                    add_rollback_obligation(prov,block(current_).instructions.back().id,"shared-release");
                    return id;
                }
                if (n.callee == "release") {
                    const auto a=lower_expr(n.args.at(0),routing);const auto&vs=value_sem(a);
                    const auto effect=(vs.ownership=="borrow-shared"||vs.ownership=="borrow-mut")?std::string("borrow.end"):(vs.ownership=="shared"?std::string("shared.release"):std::string("resource.release"));
                    return append(SirOpcode::ResourceRelease,{SirType::Unit,SirCardinality::One,{}},{a},loc(e.where),{}, {effect},effect,0,vs.region,vs.provenance,{"read"},"value");
                }
                if (n.callee == "send") {
                    const auto a=lower_expr(n.args.at(0),routing);const auto&vs=value_sem(a);const auto id=append(SirOpcode::SendValue,desc,{a},loc(e.where),{}, {"ownership.send"},{},0,vs.region,vs.provenance,{"read","move","send"},"sent",vs.resource_family);
                    if(vs.provenance)add_rollback_obligation(vs.provenance,block(current_).instructions.back().id,"restore-owner");
                    return id;
                }
                if (n.callee == "move") {
                    if(const auto*named=std::get_if<NameExpr>(&n.args.at(0).node);named&&named->name.find('.')!=std::string::npos){
                        const auto f=resolve_field_path(named->name,n.args.at(0).where);const auto child=new_provenance(f.root.region,"field.move."+f.owner_type+"."+f.field_name,"owned",false,f.root.provenance,f.field_name,resource_family_for_desc(f.desc));add_ownership_edge(f.root.provenance,child,f.root.region,"field",f.field_name,static_cast<std::uint32_t>(f.ordinal));
                        const auto id=append(SirOpcode::FieldMove,desc,{f.root.value},loc(e.where),{}, {"ownership.move","field.invalidate"},f.owner_type+"."+f.field_name,static_cast<std::uint32_t>(f.ordinal),f.root.region,child,{"read","move","send"},"owned",resource_family_for_desc(f.desc));
                        add_rollback_obligation(child,block(current_).instructions.back().id,"restore-owner");
                        return id;
                    }
                    const auto a = lower_expr(n.args.at(0), routing); const auto& vs = value_sem(a);
                    const auto id=append(SirOpcode::MoveValue, desc, {a}, loc(e.where), {}, {"ownership.move"}, {}, 0, vs.region, vs.provenance, vs.authorities, "moved",vs.resource_family);
                    if(vs.provenance)add_rollback_obligation(vs.provenance,block(current_).instructions.back().id,"restore-owner");
                    return id;
                }
                if (ssl_.datasets.contains(n.callee)) {
                    std::vector<SirValueId> args; for (const auto& x : n.args) args.push_back(lower_expr(x, routing));
                    const auto family=sf.resource_family.empty()?resource_family_for_desc(desc):sf.resource_family;const auto own=family.empty()?std::string("value"):std::string("owned");
                    const auto prov = new_provenance(region_stack_.back(), "aggregate." + n.callee, own,false,0,{},family);
                    const auto id=append(SirOpcode::AggregateMake, desc, args, loc(e.where), {}, {}, n.callee, 0, region_stack_.back(), prov, {"read","move","send"}, own,family);
                    const auto&ds=ssl_.datasets.at(n.callee);for(std::size_t i=0;i<args.size();++i){const auto&av=value_sem(args[i]);if(av.provenance&&!av.resource_family.empty())add_ownership_edge(prov,av.provenance,region_stack_.back(),"field",ds.fields[i].name,static_cast<std::uint32_t>(i));}return id;
                }
                const auto owner = before_last_dot(n.callee), member = after_last_dot(n.callee);
                if (!owner.empty() && ssl_.choices.contains(owner)) {
                    std::vector<SirValueId> args; for (const auto& x : n.args) args.push_back(lower_expr(x, routing));
                    const auto& ch = ssl_.choices.at(owner); auto it = std::find_if(ch.cases.begin(), ch.cases.end(), [&](const auto& c){ return c.name == member; });
                    const auto tag = static_cast<std::uint32_t>(std::distance(ch.cases.begin(), it));const auto family=sf.resource_family.empty()?resource_family_for_desc(desc):sf.resource_family;const auto own=family.empty()?std::string("value"):std::string("owned"); const auto prov = new_provenance(region_stack_.back(), "choice." + n.callee, own,false,0,{},family);
                    const auto id=append(SirOpcode::ChoiceMake, desc, args, loc(e.where), {}, {}, owner, tag, region_stack_.back(), prov, {"read","move","send"}, own,family);for(std::size_t i=0;i<args.size();++i){const auto&av=value_sem(args[i]);if(av.provenance&&!av.resource_family.empty())add_ownership_edge(prov,av.provenance,region_stack_.back(),"choice",it->fields[i].name,static_cast<std::uint32_t>(i));}return id;
                }
                if (!owner.empty() && ssl_.pools.contains(owner) && member == "alloc") {
                    std::vector<SirValueId> args; for (const auto& x : n.args) args.push_back(lower_expr(x, routing)); const auto region = ensure_pool_region(owner);const auto& pf = ssl_.pools.at(owner);const auto family=(pf.allocation&&*pf.allocation=="heap")?((pf.ownership=="shared"?std::string("shared-pool:"):std::string("heap-pool:"))+owner):resource_family_for_desc(desc);const auto own=pf.ownership=="shared"?std::string("shared"):std::string("owned"); const auto prov = new_provenance(region, "pool.alloc." + owner, own,false,0,{},family);
                    std::vector<SirFaultEdge> fs;
                    if (pf.allocation && *pf.allocation == "heap") fs = route_faults({"AllocationFailure"}, routing, e.where);
                    const auto id=append(SirOpcode::PoolAlloc, desc, args, loc(e.where), std::move(fs), {"memory.allocate"}, owner, 0, region, prov, pf.ownership=="shared"?std::vector<std::string>{"read","revise","share","send"}:std::vector<std::string>{"read","revise","move","send"}, own,family);
                    for(std::size_t i=0;i<args.size();++i){const auto&av=value_sem(args[i]);if(av.provenance&&!av.resource_family.empty())add_ownership_edge(prov,av.provenance,region,"field",pf.fields[i].name,static_cast<std::uint32_t>(i));}
                    if(pf.allocation&&*pf.allocation=="heap")add_rollback_obligation(prov,block(current_).instructions.back().id,"destroy-created");
                    return id;
                }
                if (!owner.empty() && ssl_.relations.contains(owner) && (member == "link" || member == "unlink")) {
                    std::vector<SirValueId> args; for (const auto& x : n.args) args.push_back(lower_expr(x, routing));
                    const auto id=append(member == "link" ? SirOpcode::RelationLink : SirOpcode::RelationUnlink, desc, std::move(args), loc(e.where), {}, {member == "link" ? "relation.link" : "relation.unlink"}, owner);
                    add_rollback_obligation(0,block(current_).instructions.back().id,member=="link"?"relation-unlink":"relation-link");
                    return id;
                }
                std::vector<SirValueId> args; for (const auto& x : n.args) args.push_back(lower_expr(x, routing));
                auto fi = ssl_.instructions.find(n.callee); if (fi == ssl_.instructions.end()) throw CompileError(e.where, "DLE: unknown call target");
                std::vector<std::string> fs(fi->second.declared_faults.begin(), fi->second.declared_faults.end());
                SirProvenanceId result_prov=0;std::string result_own="value",result_family=fi->second.result_type.base==BaseType::Nominal?resource_family_for_desc(desc):std::string{},result_bm="none";
                if(fi->second.result_borrow_mode!="none"){auto callee_decl=std::find_if(program_.instructions.begin(),program_.instructions.end(),[&](const auto&f){return f.name==n.callee;});if(callee_decl==program_.instructions.end())throw CompileError(e.where,"DLE: missing callee declaration");auto spi=std::find_if(callee_decl->params.begin(),callee_decl->params.end(),[&](const auto&p){return p.name==fi->second.result_borrow_from;});if(spi==callee_decl->params.end())throw CompileError(e.where,"DLE: borrow result source parameter missing");auto idx=static_cast<std::size_t>(std::distance(callee_decl->params.begin(),spi));const auto&src=value_sem(args.at(idx));const auto br=borrow_region(src.region,e.where);result_bm=fi->second.result_borrow_mode;result_own=result_bm=="unique-mut"?"borrow-mut":"borrow-shared";result_family=src.resource_family;result_prov=new_provenance(br,"call.borrow."+n.callee,result_own,false,src.provenance,{},result_family,result_bm);add_borrow_edge(src.provenance,result_prov,br,result_bm,true,fi->second.result_borrow_from);}
                const auto id = append(SirOpcode::Call, desc, args, loc(e.where), route_faults(fs, routing, e.where),{}, {},0,region_stack_.back(),result_prov,result_bm=="unique-mut"?std::vector<std::string>{"read","revise"}:std::vector<std::string>{"read"},result_own,result_family,result_bm); block(current_).instructions.back().callee = n.callee; return id;
            }
            throw CompileError(e.where, "DLE: unsupported expression");
        }, e.node);
    }

    bool lower_if(const IfStmt& st) {
        const Environment before = env_; const auto condition = lower_expr(st.condition, default_faults_);
        const auto parent = region_stack_.back();
        const auto then_r = new_region("if.then.region." + std::to_string(next_region_scope_++), "lexical", parent, "scope", "inherited", "scope");
        const auto else_r = new_region("if.else.region." + std::to_string(next_region_scope_++), "lexical", parent, "scope", "inherited", "scope");
        const auto then_b = new_block("if.then." + std::to_string(next_control_++), then_r);
        const auto else_b = new_block("if.else." + std::to_string(next_control_++), else_r);
        const auto join_b = new_block("if.join." + std::to_string(next_control_++), parent);
        SirTerminator ct; ct.kind = SirTerminatorKind::CondBr; ct.condition = condition; ct.true_target = then_b; ct.false_target = else_b; ct.where = loc(st.where); set_term(std::move(ct));
        std::vector<EdgeEnvironment> incoming;
        current_ = then_b; env_ = before; region_stack_.push_back(then_r); const bool td = lower_block(st.then_body); region_stack_.pop_back();
        if (!td) { const auto p = current_; branch(join_b, loc(st.where)); incoming.push_back({p, env_}); }
        current_ = else_b; env_ = before; region_stack_.push_back(else_r); bool ed = false; if (!st.else_body.empty()) ed = lower_block(st.else_body); region_stack_.pop_back();
        if (!ed) { const auto p = current_; branch(join_b, loc(st.where)); incoming.push_back({p, env_}); }
        current_ = join_b;
        if (incoming.empty()) { block(join_b).terminator.kind = SirTerminatorKind::Unreachable; env_ = before; return true; }
        env_ = merge_environments(join_b, before, incoming, loc(st.where)); return false;
    }

    bool lower_while(const WhileStmt& st) {
        const Environment before = env_; const auto preheader = current_; const auto parent = region_stack_.back();
        const auto body_r = new_region("while.body.region." + std::to_string(next_region_scope_++), "lexical", parent, "scope", "inherited", "scope");
        const auto header = new_block("while.header." + std::to_string(next_control_++), parent);
        const auto body_b = new_block("while.body." + std::to_string(next_control_++), body_r);
        const auto exit_b = new_block("while.exit." + std::to_string(next_control_++), parent); branch(header, loc(st.where));
        current_ = header; Environment header_env = before; std::map<std::string,SirOpId> phis;
        for (const auto& [name,b] : before) {
            SirInstruction op; op.id = next_op_++; op.opcode = SirOpcode::Phi; op.result = next_value_++; op.type = b.desc.type; op.cardinality = b.desc.cardinality; op.nominal_type = b.desc.nominal; op.phi_inputs = {{preheader,b.value}}; op.region = b.region; op.provenance = b.provenance; op.where = loc(st.where);
            phis[name] = op.id; block(header).instructions.push_back(std::move(op)); register_value(block(header).instructions.back().result,b.desc,b.region,b.provenance,b.authorities,b.ownership,b.resource_family,b.borrow_mode); header_env[name].value = block(header).instructions.back().result;
        }
        env_ = header_env; const auto cond = lower_expr(st.condition, default_faults_); const auto cond_pred = current_; const auto cond_env = env_;
        SirTerminator ct; ct.kind = SirTerminatorKind::CondBr; ct.condition = cond; ct.true_target = body_b; ct.false_target = exit_b; ct.where = loc(st.where); set_term(std::move(ct));
        LoopContext loop{header,exit_b,{},{}}; loops_.push_back(&loop); current_ = body_b; env_ = header_env; region_stack_.push_back(body_r); const bool dead = lower_block(st.body); region_stack_.pop_back();
        if (!dead) { const auto p=current_; branch(header,loc(st.where)); loop.continues.push_back({p,env_}); } loops_.pop_back();
        for (const auto& [name,opid] : phis) { auto& op=find_op(header,opid); for (const auto& edge:loop.continues) op.phi_inputs.push_back({edge.predecessor,edge.env.at(name).value}); }
        std::vector<EdgeEnvironment> exits{{cond_pred,cond_env}}; exits.insert(exits.end(),loop.breaks.begin(),loop.breaks.end()); current_=exit_b; env_=merge_environments(exit_b,before,exits,loc(st.where)); return false;
    }

    bool lower_loop(const LoopStmt& st) {
        const Environment before=env_; const auto preheader=current_; const auto parent=region_stack_.back();
        const auto body_r = new_region("loop.body.region." + std::to_string(next_region_scope_++), "lexical", parent, "scope", "inherited", "scope");
        const auto header=new_block("loop.header."+std::to_string(next_control_++), parent); const auto body_b=new_block("loop.body."+std::to_string(next_control_++), body_r); const auto exit_b=new_block("loop.exit."+std::to_string(next_control_++), parent); branch(header,loc(st.where));
        current_=header; Environment header_env=before; std::map<std::string,SirOpId> phis;
        for(const auto&[name,b]:before){SirInstruction op;op.id=next_op_++;op.opcode=SirOpcode::Phi;op.result=next_value_++;op.type=b.desc.type;op.cardinality=b.desc.cardinality;op.nominal_type=b.desc.nominal;op.phi_inputs={{preheader,b.value}};op.region=b.region;op.provenance=b.provenance;op.where=loc(st.where);phis[name]=op.id;block(header).instructions.push_back(std::move(op));register_value(block(header).instructions.back().result,b.desc,b.region,b.provenance,b.authorities,b.ownership,b.resource_family,b.borrow_mode);header_env[name].value=block(header).instructions.back().result;}
        LoopContext loop{header,exit_b,{},{}};loops_.push_back(&loop); current_=body_b; env_=header_env; region_stack_.push_back(body_r); const bool dead=lower_block(st.body); region_stack_.pop_back(); if(!dead){const auto p=current_;branch(header,loc(st.where));loop.continues.push_back({p,env_});}loops_.pop_back();
        for(const auto&[name,opid]:phis){auto&op=find_op(header,opid);for(const auto&edge:loop.continues)op.phi_inputs.push_back({edge.predecessor,edge.env.at(name).value});}
        current_=exit_b;if(!loop.breaks.empty()){env_=merge_environments(exit_b,before,loop.breaks,loc(st.where));return false;}env_=before;block(exit_b).terminator.kind=SirTerminatorKind::Unreachable;return true;
    }

    bool lower_region_block(const Block& body, SourceLocation where, bool transaction) {
        const auto parent = region_stack_.back();
        const auto id = new_region((transaction ? "transaction." : "parallel.") + std::to_string(next_region_scope_++),
                                   transaction ? "transaction" : "parallel", parent, "scope", "inherited", "scope");
        const auto body_b = new_block((transaction ? "transaction.body." : "parallel.body.") + std::to_string(next_control_++), id);
        const auto done_b = new_block((transaction ? "transaction.done." : "parallel.done.") + std::to_string(next_control_++), parent);

        // Transactions interpose explicit abort blocks on every outward fault route.
        // Nested transactions naturally chain: an inner abort branches to the outer
        // transaction's abort route, so undo executes inner-to-outer.
        FaultRouting saved_faults;
        if (transaction) {
            saved_faults = default_faults_;
            FaultRouting txn_faults;
            const auto saved_current = current_;
            for (const auto& [fault, outer_target] : saved_faults) {
                const auto ab = new_block("transaction.abort." + fault + "." + std::to_string(next_control_++), id);
                current_ = ab;
                (void)append(SirOpcode::TransactionAbort, {SirType::Unit,SirCardinality::One,{}}, {}, loc(where), {},
                             {"transaction.abort"}, fault, 0, id);
                branch(outer_target, loc(where));
                txn_faults[fault] = ab;
            }
            current_ = saved_current;
            default_faults_ = std::move(txn_faults);
        }

        (void)append(transaction ? SirOpcode::TransactionBegin : SirOpcode::ParallelBegin,
                     {SirType::Unit,SirCardinality::One,{}}, {}, loc(where), {},
                     {transaction ? "transaction.begin" : "parallel.begin"}, {}, 0, id);
        branch(body_b, loc(where));
        current_ = body_b;
        region_stack_.push_back(id);
        const bool dead = lower_block(body);
        if (!dead) {
            (void)append(transaction ? SirOpcode::TransactionCommit : SirOpcode::ParallelEnd,
                         {SirType::Unit,SirCardinality::One,{}}, {}, loc(where), {},
                         {transaction ? "transaction.commit" : "parallel.end"}, {}, 0, id);
            branch(done_b, loc(where));
        }
        region_stack_.pop_back();
        if (transaction) default_faults_ = std::move(saved_faults);
        if (dead) { block(done_b).terminator.kind = SirTerminatorKind::Unreachable; return true; }
        current_ = done_b;
        return false;
    }

    bool lower_task_block(const TaskStmt& st) {
        // Find the nearest enclosing parallel semantic region.
        SirRegionId parallel_region = 0;
        for (auto it = region_stack_.rbegin(); it != region_stack_.rend(); ++it) {
            auto ri = std::find_if(out_.regions.begin(), out_.regions.end(), [&](const auto& r){ return r.id == *it; });
            if (ri != out_.regions.end() && ri->kind == "parallel") { parallel_region = *it; break; }
        }
        if (!parallel_region) throw CompileError(st.where, "DLE: task requires an enclosing parallel region");

        const auto task_region = new_region("task." + std::to_string(next_region_scope_++), "task", parallel_region,
                                            "scope", "structured", "join");
        const auto body_b = new_block("task.body." + std::to_string(next_control_++), task_region);
        const auto done_b = new_block("task.done." + std::to_string(next_control_++), parallel_region);
        (void)append(SirOpcode::TaskBegin, {SirType::Unit,SirCardinality::One,{}}, {}, loc(st.where), {},
                     {"concurrency.task.begin"}, {}, 0, task_region);
        branch(body_b, loc(st.where));

        const auto outer_env = env_;
        current_ = body_b;
        region_stack_.push_back(task_region);
        const bool dead = lower_block(st.body);
        if (!dead) {
            (void)append(SirOpcode::TaskEnd, {SirType::Unit,SirCardinality::One,{}}, {}, loc(st.where), {},
                         {"concurrency.task.end"}, {}, 0, task_region);
            branch(done_b, loc(st.where));
        }
        region_stack_.pop_back();
        // Task-local bindings never leak across the structured join. The semantic
        // pass has already validated move/send source-state across sibling tasks.
        env_ = outer_env;
        if (dead) { block(done_b).terminator.kind = SirTerminatorKind::Unreachable; return true; }
        current_ = done_b;
        return false;
    }

    bool lower_block(const Block& body) {
        for (const auto& stmt : body) {
            if (has_term(current_)) return true;
            const bool transfer = std::visit([&](const auto& st)->bool {
                using T=std::decay_t<decltype(st)>;
                if constexpr(std::is_same_v<T,LocalDecl>){const auto value=lower_expr(st.value,default_faults_);const auto d=expr_desc(st.value);auto it=facts_.locals.find(st.name);std::vector<std::string> auth{"read"};if(it!=facts_.locals.end())auth.assign(it->second.authorities.begin(),it->second.authorities.end());auth=sorted_unique(std::move(auth));auto vsi=std::find_if(out_.values.begin(),out_.values.end(),[&](const auto&v){return v.value==value;});if(vsi==out_.values.end())throw CompileError(st.where,"DLE internal: local binding value lacks SSA semantics");for(const auto&a:auth)if(std::find(vsi->authorities.begin(),vsi->authorities.end(),a)==vsi->authorities.end())vsi->authorities.push_back(a);vsi->authorities=sorted_unique(std::move(vsi->authorities));const auto vs=*vsi;env_[st.name]=Binding{d,value,st.revisable,vs.region,vs.provenance,auth,vs.ownership,vs.resource_family,vs.borrow_mode};return false;}
                else if constexpr(std::is_same_v<T,SetStmt>){
                    auto it=env_.find(st.target);
                    if(it!=env_.end()){
                        const auto replacement=lower_expr(st.value,default_faults_);
                        const auto marked=append(SirOpcode::RevisionMarker,it->second.desc,{replacement},loc(st.where),{}, {"revise.replace"}, {},0,it->second.region,it->second.provenance,it->second.authorities,it->second.ownership);
                        it->second.value=marked; return false;
                    }
                    const auto f=resolve_field_path(st.target,st.where); const auto replacement=lower_expr(st.value,default_faults_);
                    (void)append(SirOpcode::FieldStore,{SirType::Unit,SirCardinality::One,{}},{f.root.value,replacement},loc(st.where),{}, {"memory.field.write"},f.owner_type+"."+f.field_name,static_cast<std::uint32_t>(f.ordinal),f.root.region,f.root.provenance,{"read","revise"},"effect");
                    add_rollback_obligation(f.root.provenance,block(current_).instructions.back().id,"restore-field");
                    return false;
                }
                else if constexpr(std::is_same_v<T,ReviseStmt>){
                    auto it=env_.find(st.target);
                    if(it!=env_.end()){
                        const auto delta=lower_expr(st.delta,default_faults_);const auto revised=append(SirOpcode::CheckedAdd,it->second.desc,{it->second.value,delta},loc(st.where),route_faults({"ArithmeticOverflow"},default_faults_,st.where),{"revise.relative"},{},0,it->second.region,it->second.provenance,it->second.authorities,it->second.ownership);it->second.value=revised;return false;
                    }
                    const auto f=resolve_field_path(st.target,st.where); const auto cur=lower_field_load(st.target,st.where); const auto delta=lower_expr(st.delta,default_faults_);
                    const auto revised=append(SirOpcode::CheckedAdd,f.desc,{cur,delta},loc(st.where),route_faults({"ArithmeticOverflow"},default_faults_,st.where),{"revise.relative"},{},0,f.root.region,value_sem(cur).provenance,{"read","revise"},"subobject-value");
                    (void)append(SirOpcode::FieldStore,{SirType::Unit,SirCardinality::One,{}},{f.root.value,revised},loc(st.where),{}, {"memory.field.write"},f.owner_type+"."+f.field_name,static_cast<std::uint32_t>(f.ordinal),f.root.region,f.root.provenance,{"read","revise"},"effect");
                    add_rollback_obligation(f.root.provenance,block(current_).instructions.back().id,"restore-field");
                    return false;
                }
                else if constexpr(std::is_same_v<T,IfStmt>) return lower_if(st);
                else if constexpr(std::is_same_v<T,WhileStmt>) return lower_while(st);
                else if constexpr(std::is_same_v<T,LoopStmt>) return lower_loop(st);
                else if constexpr(std::is_same_v<T,TransactionStmt>) return lower_region_block(st.body,st.where,true);
                else if constexpr(std::is_same_v<T,ParallelStmt>) return lower_region_block(st.body,st.where,false);
                else if constexpr(std::is_same_v<T,TaskStmt>) return lower_task_block(st);
                else if constexpr(std::is_same_v<T,BreakStmt>){if(loops_.empty())throw CompileError(st.where,"DLE: break without loop");auto*loop=loops_.back();const auto p=current_;loop->breaks.push_back({p,env_});branch(loop->exit,loc(st.where));return true;}
                else if constexpr(std::is_same_v<T,ContinueStmt>){if(loops_.empty())throw CompileError(st.where,"DLE: continue without loop");auto*loop=loops_.back();const auto p=current_;loop->continues.push_back({p,env_});branch(loop->header,loc(st.where));return true;}
                else if constexpr(std::is_same_v<T,ReturnStmt>){SirTerminator t;t.kind=SirTerminatorKind::Return;t.where=loc(st.where);if(st.value)t.value=lower_expr(*st.value,default_faults_);set_term(std::move(t));return true;}
                else if constexpr(std::is_same_v<T,PerformStmt>){(void)lower_expr(st.value,default_faults_);return false;}
                else if constexpr(std::is_same_v<T,FaultStmt>){auto fi=ssl_.fault_facts.find(st.fault_name);if(fi==ssl_.fault_facts.end())throw CompileError(st.where,"DLE: unknown fault");
                    // Explicit faults abort every active transaction before propagation.
                    for(auto it=region_stack_.rbegin();it!=region_stack_.rend();++it){auto rr=std::find_if(out_.regions.begin(),out_.regions.end(),[&](const auto&r){return r.id==*it;});if(rr!=out_.regions.end()&&rr->kind=="transaction")
                        (void)append(SirOpcode::TransactionAbort,{SirType::Unit,SirCardinality::One,{}},{},loc(st.where),{}, {"transaction.abort"},st.fault_name,0,*it);}
                    SirTerminator t;t.kind=SirTerminatorKind::FaultReturn;t.fault=st.fault_name;t.where=loc(st.where);for(const auto&x:st.payload)t.fault_payload.push_back(lower_expr(x,default_faults_));set_term(std::move(t));return true;}
                else if constexpr(std::is_same_v<T,ExprStmt>){(void)lower_expr(st.value,default_faults_);return false;}
                return false;
            }, stmt.node);
            if (transfer) return true;
        }
        return has_term(current_);
    }

    struct ControlEdge { SirBlockId from{0}; SirBlockId to{0}; SirOpId source_op{0}; SirEffectTokenId token{0}; };
    void synthesize_task_captures() {
        auto region_kind = [&](SirRegionId id) -> std::string {
            auto it=std::find_if(out_.regions.begin(),out_.regions.end(),[&](const auto&r){return r.id==id;});
            return it==out_.regions.end()?std::string{}:it->kind;
        };
        auto parallel_parent = [&](SirRegionId task) -> SirRegionId {
            SirRegionId cur=task;std::set<SirRegionId> seen;
            while(cur&&seen.insert(cur).second){auto it=std::find_if(out_.regions.begin(),out_.regions.end(),[&](const auto&r){return r.id==cur;});if(it==out_.regions.end())break;if(it->kind=="parallel")return it->id;cur=it->parent;}return 0;
        };
        std::map<std::pair<SirRegionId,SirValueId>,std::string> captures;
        for(const auto& tr:out_.regions){
            if(tr.kind!="task")continue;
            const auto pr=parallel_parent(tr.id);if(!pr)throw CompileError(source_.where,"DLE internal: task region lacks parallel parent");
            for(const auto& b:out_.blocks){
                if(!region_inside(b.region,tr.id))continue;
                for(const auto& op:b.instructions){
                    for(std::size_t oi=0;oi<op.operands.size();++oi){
                        const auto v=op.operands[oi];const auto& vs=value_sem(v);
                        if(region_inside(vs.region,tr.id))continue;
                        std::string mode;
                        if(op.opcode==SirOpcode::SendValue&&oi==0)mode="send";
                        else if(vs.ownership=="sent")mode="send";
                        else if(vs.ownership=="shared")mode="shared-read";
                        else if(vs.resource_family.empty()&&std::find(vs.authorities.begin(),vs.authorities.end(),"revise")==vs.authorities.end())mode="copy-read";
                        else throw CompileError(source_.where,"DLE: task capture of resource/mutable value requires explicit send(...) or shared ownership");
                        auto key=std::make_pair(tr.id,v);auto it=captures.find(key);if(it==captures.end()||mode=="send")captures[key]=mode;
                    }
                }
            }
            for(const auto& [key,mode]:captures){
                if(key.first!=tr.id) continue;
                const auto& vs=value_sem(key.second);
                out_.task_captures.push_back({next_task_capture_++,pr,tr.id,key.second,vs.provenance,mode});
            }
        }
        std::sort(out_.task_captures.begin(),out_.task_captures.end(),[](const auto&a,const auto&b){return a.id<b.id;});
        (void)region_kind;
    }

    void synthesize_effect_ssa() {
        SirEffectTokenId next = 1;
        for (auto& b : out_.blocks) {
            b.effect_entry = next++;
            SirEffectTokenId cur = b.effect_entry;
            for (auto& op : b.instructions) {
                if (!op.effects.empty()) { op.effect_in = cur; op.effect_out = next++; cur = op.effect_out; }
            }
            b.effect_exit = cur; b.terminator.effect_token = cur;
        }
        out_.effect_root = block(out_.entry).effect_entry;
        std::vector<ControlEdge> edges;
        for (const auto& b : out_.blocks) {
            SirEffectTokenId cur = b.effect_entry;
            for (const auto& op : b.instructions) {
                if (!op.effects.empty()) cur = op.effect_out;
                for (const auto& f : op.fault_edges) edges.push_back({b.id,f.target,op.id,cur});
            }
            switch (b.terminator.kind) {
                case SirTerminatorKind::Br: edges.push_back({b.id,b.terminator.target,0,b.effect_exit}); break;
                case SirTerminatorKind::CondBr: edges.push_back({b.id,b.terminator.true_target,0,b.effect_exit}); edges.push_back({b.id,b.terminator.false_target,0,b.effect_exit}); break;
                default: break;
            }
        }
        for (auto& b : out_.blocks) {
            if (b.id == out_.entry) continue;
            for (const auto& e : edges) if (e.to == b.id) b.effect_phi_inputs.push_back({e.from,e.source_op,e.token});
            std::sort(b.effect_phi_inputs.begin(),b.effect_phi_inputs.end(),[](const auto&a,const auto&b){if(a.predecessor!=b.predecessor)return a.predecessor<b.predecessor;return a.source_op<b.source_op;});
        }
    }

    const Program& program_; const SSLProgram& ssl_; const InstructionDecl& source_; const InstructionFacts& facts_;
    SirFunction out_; Environment env_; FaultRouting default_faults_; SirBlockId current_{0}; SirValueId next_value_{1}; SirOpId next_op_{1};
    SirRegionId next_region_{1}; SirProvenanceId next_provenance_{1}; std::uint32_t next_control_{0},next_handler_{0},next_logic_{0},next_region_scope_{0};
    std::uint32_t next_borrow_edge_{1}, next_ownership_edge_{1}, next_rollback_{1}, next_task_capture_{1};
    std::vector<LoopContext*> loops_; std::vector<SirRegionId> region_stack_;
};

struct DefSite { SirBlockId block{0}; int instruction_index{-1}; SirTypeDesc desc; };

struct EdgeKey {
    SirBlockId from{0}; SirBlockId to{0}; SirOpId source_op{0};
    bool operator==(const EdgeKey&) const = default;
    bool operator<(const EdgeKey& x) const { if(from!=x.from)return from<x.from;if(to!=x.to)return to<x.to;return source_op<x.source_op; }
};

std::map<SirBlockId,std::set<SirBlockId>> predecessors(const SirFunction& fn) {
    std::map<SirBlockId,std::set<SirBlockId>> pred; for(const auto&b:fn.blocks)pred[b.id];
    auto add=[&](SirBlockId from,SirBlockId to){if(!pred.contains(to))fail("SIR verifier: edge targets unknown block "+std::to_string(to));pred[to].insert(from);};
    for(const auto&b:fn.blocks){for(const auto&op:b.instructions)for(const auto&e:op.fault_edges)add(b.id,e.target);switch(b.terminator.kind){case SirTerminatorKind::Br:add(b.id,b.terminator.target);break;case SirTerminatorKind::CondBr:add(b.id,b.terminator.true_target);add(b.id,b.terminator.false_target);break;default:break;}}
    return pred;
}
std::set<EdgeKey> control_edges(const SirFunction& fn) {
    std::set<EdgeKey> out;
    for(const auto&b:fn.blocks){for(const auto&op:b.instructions)for(const auto&e:op.fault_edges)out.insert({b.id,e.target,op.id});switch(b.terminator.kind){case SirTerminatorKind::Br:out.insert({b.id,b.terminator.target,0});break;case SirTerminatorKind::CondBr:out.insert({b.id,b.terminator.true_target,0});out.insert({b.id,b.terminator.false_target,0});break;default:break;}}
    return out;
}
std::map<SirBlockId,std::set<SirBlockId>> dominators(const SirFunction& fn,const std::map<SirBlockId,std::set<SirBlockId>>&pred){std::set<SirBlockId>all;for(const auto&b:fn.blocks)all.insert(b.id);std::map<SirBlockId,std::set<SirBlockId>>dom;for(const auto&b:fn.blocks)dom[b.id]=b.id==fn.entry?std::set<SirBlockId>{b.id}:all;bool changed=true;while(changed){changed=false;for(const auto&b:fn.blocks){if(b.id==fn.entry)continue;std::set<SirBlockId>next;const auto&ps=pred.at(b.id);if(ps.empty())next={b.id};else{auto it=ps.begin();next=dom.at(*it++);for(;it!=ps.end();++it){std::set<SirBlockId>x;std::set_intersection(next.begin(),next.end(),dom.at(*it).begin(),dom.at(*it).end(),std::inserter(x,x.begin()));next=std::move(x);}next.insert(b.id);}if(next!=dom[b.id]){dom[b.id]=std::move(next);changed=true;}}}return dom;}

SirTypeDesc op_desc(const SirInstruction& op){return{op.type,op.cardinality,op.nominal_type};}
SirTypeDesc param_desc(const SirParameter&p){return{p.type,p.cardinality,p.nominal_type};}
SirTypeDesc value_desc(const SirValueSemantics&v){return{v.type,v.cardinality,v.nominal_type};}
bool desc_integer(const SirTypeDesc&d){return d.cardinality==SirCardinality::One&&sir_is_integer(d.type);}


const SirValueSemantics& find_value_sem(const SirFunction& fn, SirValueId id) {
    auto it = std::find_if(fn.values.begin(), fn.values.end(), [&](const auto& v){ return v.value == id; });
    if (it == fn.values.end()) fail("SIR verifier: missing value semantics for %" + std::to_string(id));
    return *it;
}

const SirDatasetSchema* find_dataset(const std::vector<SirDatasetSchema>& xs, const std::string& name) {
    auto it = std::find_if(xs.begin(), xs.end(), [&](const auto& x){ return x.name == name; }); return it == xs.end() ? nullptr : &*it;
}
const SirChoiceSchema* find_choice(const std::vector<SirChoiceSchema>& xs, const std::string& name) {
    auto it = std::find_if(xs.begin(), xs.end(), [&](const auto& x){ return x.name == name; }); return it == xs.end() ? nullptr : &*it;
}
const SirPoolSchema* find_pool(const std::vector<SirPoolSchema>& xs, const std::string& name) {
    auto it = std::find_if(xs.begin(), xs.end(), [&](const auto& x){ return x.name == name; }); return it == xs.end() ? nullptr : &*it;
}
const SirRelationSchema* find_relation(const std::vector<SirRelationSchema>& xs, const std::string& name) {
    auto it = std::find_if(xs.begin(), xs.end(), [&](const auto& x){ return x.name == name; }); return it == xs.end() ? nullptr : &*it;
}
const SirFieldSchema* find_addressable_field(const std::vector<SirDatasetSchema>& datasets,
                                             const std::vector<SirPoolSchema>& pools,
                                             const std::string& owner, const std::string& field) {
    if (const auto* d = find_dataset(datasets, owner)) {
        auto it=std::find_if(d->fields.begin(),d->fields.end(),[&](const auto& f){return f.name==field;}); if(it!=d->fields.end())return &*it;
    }
    if (const auto* p = find_pool(pools, owner)) {
        auto it=std::find_if(p->fields.begin(),p->fields.end(),[&](const auto& f){return f.name==field;}); if(it!=p->fields.end())return &*it;
    }
    return nullptr;
}

void verify_schema_type(const SirTypeDesc& d, const std::string& where) {
    if (d.type == SirType::Nominal && d.nominal.empty()) fail("SIR verifier: nominal type lacks identity in " + where);
    if (d.type != SirType::Nominal && !d.nominal.empty()) fail("SIR verifier: non-nominal type carries nominal identity in " + where);
    if (d.type == SirType::Unit && d.cardinality != SirCardinality::One) fail("SIR verifier: unit may only have cardinality One in " + where);
}

void verify_common(const std::string& module,
                   const std::vector<SirFaultIdentity>& faults,
                   const std::vector<SirDatasetSchema>& datasets,
                   const std::vector<SirChoiceSchema>& choices,
                   const std::vector<SirRelationSchema>& relations,
                   const std::vector<SirPoolSchema>& pools,
                   const std::vector<SirFunction>& functions) {
    if (module.empty()) fail("SIR verifier: module identity is empty");

    std::set<std::string> fault_names; std::set<std::int32_t> fault_codes;
    std::map<std::string,const SirFaultIdentity*> fault_map;
    for (const auto& f : faults) {
        if (!fault_names.insert(f.name).second) fail("SIR verifier: duplicate fault identity '" + f.name + "'");
        if (!fault_codes.insert(f.code).second) fail("SIR verifier: duplicate fault code");
        std::set<std::string> fields;
        for (const auto& fld : f.payload) {
            if (!fields.insert(fld.name).second) fail("SIR verifier: duplicate fault payload field");
            verify_schema_type({fld.type,fld.cardinality,fld.nominal_type}, "fault payload");
        }
        fault_map[f.name] = &f;
    }

    std::set<std::string> type_names;
    for (const auto& d : datasets) {
        if (!type_names.insert(d.name).second) fail("SIR verifier: duplicate aggregate type '" + d.name + "'");
        std::set<std::string> fields;
        for (const auto& f : d.fields) { if (!fields.insert(f.name).second) fail("SIR verifier: duplicate dataset field"); verify_schema_type({f.type,f.cardinality,f.nominal_type}, "dataset field"); }
        if (!d.key.empty() && !fields.contains(d.key)) fail("SIR verifier: dataset key names unknown field");
        for (const auto& i : d.indexes) if (!fields.contains(i)) fail("SIR verifier: dataset index names unknown field");
    }
    for (const auto& c : choices) {
        if (!type_names.insert(c.name).second) fail("SIR verifier: duplicate aggregate/choice type '" + c.name + "'");
        std::set<std::string> cases; std::set<std::uint32_t> tags;
        for (const auto& arm : c.cases) {
            if (!cases.insert(arm.name).second || !tags.insert(arm.tag).second) fail("SIR verifier: duplicate choice case/tag");
            for (const auto& f : arm.fields) verify_schema_type({f.type,f.cardinality,f.nominal_type}, "choice payload");
        }
        if (c.cases.empty()) fail("SIR verifier: choice has no cases");
    }
    for (const auto& p : pools) {
        if (!type_names.insert(p.name).second) fail("SIR verifier: duplicate pool/aggregate type '" + p.name + "'");
        for (const auto& f : p.fields) verify_schema_type({f.type,f.cardinality,f.nominal_type}, "pool field");
    }
    std::set<std::string> relation_names;
    for (const auto& r : relations) {
        if (!relation_names.insert(r.name).second) fail("SIR verifier: duplicate relation '" + r.name + "'");
        if (!find_dataset(datasets,r.source) || !find_dataset(datasets,r.target)) fail("SIR verifier: relation endpoints must name dataset schemas");
    }

    std::map<std::string,const SirFunction*> fmap;
    for (const auto& fn : functions) {
        if (!fmap.emplace(fn.name,&fn).second) fail("SIR verifier: duplicate function '" + fn.name + "'");
    }

    for (const auto& fn : functions) {
        verify_schema_type({fn.result_type,fn.result_cardinality,fn.result_nominal_type}, "function result");
        if(fn.result_ownership!="value"&&fn.result_ownership!="owned-transfer"&&fn.result_ownership!="borrowed"&&fn.result_ownership!="shared-transfer")fail("SIR verifier: unknown function result ownership contract");
        if((fn.result_ownership=="owned-transfer"||fn.result_ownership=="shared-transfer")&&fn.result_resource.empty())fail("SIR verifier: resource-transfer result requires resource identity");
        if(fn.result_ownership=="borrowed"){
            if(fn.result_borrow_mode!="shared-read"&&fn.result_borrow_mode!="unique-mut")fail("SIR verifier: borrowed result requires shared-read/unique-mut contract");
            if(fn.result_borrow_from.empty())fail("SIR verifier: borrowed result requires a source parameter");
        }else if(fn.result_borrow_mode!="none"||!fn.result_borrow_from.empty())fail("SIR verifier: non-borrowed result may not carry borrow contract metadata");
        if(fn.result_ownership=="value"&&!fn.result_resource.empty())fail("SIR verifier: ordinary value result may not carry resource-transfer identity");
        if (fn.blocks.empty()) fail("SIR verifier: function has no blocks");
        if (fn.entry >= fn.blocks.size()) fail("SIR verifier: invalid entry block");
        for (std::size_t i=0;i<fn.blocks.size();++i) if (fn.blocks[i].id != i) fail("SIR verifier: block IDs must be canonical and contiguous");

        std::set<SirRegionId> region_ids;
        std::map<SirRegionId,SirRegionId> region_parent;
        for (const auto& r : fn.regions) {
            if (!r.id || !region_ids.insert(r.id).second) fail("SIR verifier: invalid/duplicate region id");
            if (r.parent && !region_ids.contains(r.parent)) fail("SIR verifier: regions must be serialized parent-before-child");
            region_parent[r.id]=r.parent;
        }
        if (!region_ids.contains(fn.root_region)) fail("SIR verifier: function root region missing");
        auto region_within=[&](SirRegionId child,SirRegionId ancestor){
            if(!ancestor)return true;
            std::set<SirRegionId> seen;
            while(child&&seen.insert(child).second){if(child==ancestor)return true;auto it=region_parent.find(child);if(it==region_parent.end())break;child=it->second;}
            return false;
        };
        std::set<SirProvenanceId> prov_ids;
        std::map<SirProvenanceId,const SirProvenance*> prov_map;
        for (const auto& p : fn.provenances) {
            if (!p.id || !prov_ids.insert(p.id).second) fail("SIR verifier: invalid/duplicate provenance id");
            if (!region_ids.contains(p.region)) fail("SIR verifier: provenance references unknown lifetime region");
            if (p.parent && !prov_ids.contains(p.parent)) fail("SIR verifier: subobject provenance parent must be serialized before child");
            if (p.parent && p.subobject.empty() && p.borrow_mode=="none" && p.ownership!="shared") {
                // Parent-linked provenance is also used for borrow/share lineage; only ordinary
                // subobject children require a concrete subobject name.
            }
            if (!p.parent && !p.subobject.empty()) fail("SIR verifier: root provenance may not claim a subobject path (provenance "+std::to_string(p.id)+", subobject='"+p.subobject+"')");
            if(p.parent){const auto* parent=prov_map.at(p.parent);if(!region_within(p.region,parent->region))fail("SIR verifier: subobject/borrow provenance lifetime may not outlive parent provenance");}
            prov_map[p.id]=&p;
        }
        auto provenance_path=[&](SirProvenanceId id){
            std::ostringstream o; std::set<SirProvenanceId> seen; bool first=true;
            while(id&&seen.insert(id).second){auto it=prov_map.find(id);if(it==prov_map.end())break;const auto&pr=*it->second;if(!first)o<<" <- ";first=false;o<<"prov#"<<pr.id<<"["<<pr.origin;if(!pr.subobject.empty())o<<"."<<pr.subobject;o<<", region="<<pr.region<<", ownership="<<pr.ownership<<"]";id=pr.parent;}
            return o.str();
        };
        std::set<std::uint32_t> borrow_ids;
        for(const auto&e:fn.borrow_edges){
            if(!e.id||!borrow_ids.insert(e.id).second)fail("SIR verifier: duplicate/zero borrow-edge identity");
            if(!prov_map.contains(e.owner)||!prov_map.contains(e.borrower))fail("SIR verifier: borrow edge references unknown provenance");
            const auto&owner=*prov_map.at(e.owner);const auto&borrower=*prov_map.at(e.borrower);
            if(e.mode!="shared-read"&&e.mode!="unique-mut")fail("SIR verifier: borrow edge has unknown mode");
            if(borrower.parent!=e.owner)fail("SIR verifier: borrow provenance must descend from owner; chain: "+provenance_path(e.borrower));
            if(borrower.region!=e.region||borrower.borrow_mode!=e.mode)fail("SIR verifier: borrow edge/provenance metadata disagree; chain: "+provenance_path(e.borrower));
            if(!region_within(e.region,owner.region))fail("SIR verifier: borrow lifetime outlives owner; chain: "+provenance_path(e.borrower));
            if(e.cross_boundary&&e.source_parameter.empty())fail("SIR verifier: cross-boundary borrow edge lacks source parameter");
            if(!e.cross_boundary&&!e.source_parameter.empty())fail("SIR verifier: local borrow edge may not name a cross-boundary source parameter");
        }
        std::set<std::uint32_t> ownership_ids;std::map<SirProvenanceId,std::vector<SirProvenanceId>> ownership_graph;
        for(const auto&e:fn.ownership_edges){
            if(!e.id||!ownership_ids.insert(e.id).second)fail("SIR verifier: duplicate/zero ownership-edge identity");
            if(!prov_map.contains(e.owner)||!prov_map.contains(e.child)||e.owner==e.child)fail("SIR verifier: ownership edge references invalid provenance");
            if(!region_ids.contains(e.region)||!region_within(prov_map.at(e.child)->region,prov_map.at(e.owner)->region))fail("SIR verifier: owned child lifetime is incompatible with owner; child chain: "+provenance_path(e.child));
            if(e.role!="field"&&e.role!="optional"&&e.role!="choice"&&e.role!="many")fail("SIR verifier: unknown resource-composition ownership role");
            ownership_graph[e.owner].push_back(e.child);
        }
        std::function<void(SirProvenanceId,std::set<SirProvenanceId>&,std::set<SirProvenanceId>&)> ownership_dfs;
        ownership_dfs=[&](SirProvenanceId at,std::set<SirProvenanceId>&active,std::set<SirProvenanceId>&done){if(done.contains(at))return;if(!active.insert(at).second)fail("SIR verifier: recursive resource ownership graph contains a cycle at "+provenance_path(at));for(auto ch:ownership_graph[at])ownership_dfs(ch,active,done);active.erase(at);done.insert(at);};
        {std::set<SirProvenanceId> active,done;for(const auto&[root,_]:ownership_graph)ownership_dfs(root,active,done);}
        std::set<std::uint32_t> rollback_ids;
        for(const auto&r:fn.rollback_obligations){
            if(!r.id||!rollback_ids.insert(r.id).second)fail("SIR verifier: duplicate/zero rollback obligation identity");
            auto ri=std::find_if(fn.regions.begin(),fn.regions.end(),[&](const auto&x){return x.id==r.transaction_region;});if(ri==fn.regions.end()||ri->kind!="transaction")fail("SIR verifier: rollback obligation must name a transaction region");
            const bool relation_action=r.action=="relation-link"||r.action=="relation-unlink";
            if(!relation_action && !prov_map.contains(r.provenance))fail("SIR verifier: rollback obligation references unknown provenance");
            if(relation_action && r.provenance!=0)fail("SIR verifier: relation rollback obligation must not masquerade as a resource provenance");
            if(r.action!="restore-owner"&&r.action!="destroy-created"&&r.action!="shared-release"&&r.action!="restore-field"&&r.action!="relation-link"&&r.action!="relation-unlink")fail("SIR verifier: unknown rollback obligation action");
        }
        std::set<std::uint32_t> capture_ids;std::set<std::pair<SirRegionId,SirValueId>> capture_pairs;
        for(const auto&c:fn.task_captures){
            if(!c.id||!capture_ids.insert(c.id).second)fail("SIR verifier: duplicate/zero task capture identity");
            if(!capture_pairs.insert({c.task_region,c.value}).second)fail("SIR verifier: duplicate task capture for the same value");
            auto tr=std::find_if(fn.regions.begin(),fn.regions.end(),[&](const auto&r){return r.id==c.task_region;});
            auto pr=std::find_if(fn.regions.begin(),fn.regions.end(),[&](const auto&r){return r.id==c.parallel_region;});
            if(tr==fn.regions.end()||tr->kind!="task")fail("SIR verifier: task capture must name a task region");
            if(pr==fn.regions.end()||pr->kind!="parallel")fail("SIR verifier: task capture must name a parallel parent region");
            SirRegionId cur=tr->parent;std::set<SirRegionId>seen;bool nested=false;while(cur&&seen.insert(cur).second){if(cur==c.parallel_region){nested=true;break;}auto it=std::find_if(fn.regions.begin(),fn.regions.end(),[&](const auto&r){return r.id==cur;});if(it==fn.regions.end())break;cur=it->parent;}if(!nested)fail("SIR verifier: task capture task region is not nested beneath its declared parallel region");
            const auto&vs=find_value_sem(fn,c.value);if(vs.provenance!=c.provenance)fail("SIR verifier: task capture provenance disagrees with captured SSA value");
            if(!region_within(c.task_region,vs.region))fail("SIR verifier: task capture outlives captured value lifetime");
            if(c.mode=="copy-read"){if(!vs.resource_family.empty()||std::find(vs.authorities.begin(),vs.authorities.end(),"revise")!=vs.authorities.end())fail("SIR verifier: copy-read task capture must be immutable and non-resource");}
            else if(c.mode=="shared-read"){if(vs.ownership!="shared")fail("SIR verifier: shared-read task capture requires shared ownership");}
            else if(c.mode=="send"){if(std::find(vs.authorities.begin(),vs.authorities.end(),"send")==vs.authorities.end())fail("SIR verifier: send task capture lacks send authority");}
            else fail("SIR verifier: unknown task capture mode");
        }
        for (const auto& b : fn.blocks) if (!region_ids.contains(b.region)) fail("SIR verifier: block references unknown lifetime region");

        std::set<std::string> declared_faults(fn.declared_faults.begin(),fn.declared_faults.end());
        for(const auto&f:declared_faults) if(!fault_map.contains(f)) fail("SIR verifier: function declares unknown fault '"+f+"'");

        const auto pred = predecessors(fn); const auto dom = dominators(fn,pred); const auto edges = control_edges(fn);
        std::map<SirValueId,DefSite> defs;
        std::map<SirValueId,const SirInstruction*> defops;
        for (const auto& p : fn.parameters) {
            if (!p.value || defs.contains(p.value)) fail("SIR verifier: duplicate/zero parameter SSA value");
            verify_schema_type(param_desc(p), "parameter"); defs[p.value] = {fn.entry,-1,param_desc(p)};
        }
        for (const auto& b : fn.blocks) {
            bool seen_non_phi=false;
            for (std::size_t i=0;i<b.instructions.size();++i) {
                const auto& op=b.instructions[i];
                if (!op.id || !op.result || defs.contains(op.result)) fail("SIR verifier: operation IDs/results must be nonzero and SSA-unique");
                verify_schema_type(op_desc(op), "operation result");
                if(op.opcode==SirOpcode::Phi){if(seen_non_phi)fail("SIR verifier: phi operations must lead block");}else seen_non_phi=true;
                defs[op.result]={b.id,static_cast<int>(i),op_desc(op)};defops[op.result]=&op;
            }
        }
        std::set<SirValueId> sem_ids;
        for (const auto& v : fn.values) {
            if (!v.value || !sem_ids.insert(v.value).second) fail("SIR verifier: duplicate value-semantics record");
            auto di=defs.find(v.value); if(di==defs.end()) fail("SIR verifier: value-semantics record references undefined value");
            if(di->second.desc!=value_desc(v)) fail("SIR verifier: value-semantics type/cardinality disagrees with SSA definition");
            if(!region_ids.contains(v.region)) fail("SIR verifier: value references unknown lifetime region");
            if(v.provenance && !prov_ids.contains(v.provenance)) fail("SIR verifier: value references unknown provenance");
            if(v.authorities.empty()) fail("SIR verifier: value authority set may not be empty");
            std::set<std::string> unique_authorities(v.authorities.begin(), v.authorities.end());
            if(unique_authorities.size()!=v.authorities.size()) fail("SIR verifier: duplicate value authority capability");
        }
        if(sem_ids.size()!=defs.size()) fail("SIR verifier: every SSA value must have exactly one semantic record");
        for(const auto&p:fn.parameters){
            const auto&vs=find_value_sem(fn,p.value);
            if(p.authority=="borrow"){if(vs.borrow_mode!="shared-read"||(vs.ownership!="borrow-shared"&&vs.ownership!="borrow"))fail("SIR verifier: borrow parameter lacks shared-read borrow semantics");}
            else if(p.authority=="borrow_mut"){if(vs.borrow_mode!="unique-mut"||vs.ownership!="borrow-mut"||std::find(vs.authorities.begin(),vs.authorities.end(),"revise")==vs.authorities.end())fail("SIR verifier: borrow_mut parameter lacks unique mutable-borrow semantics");}
            else if(p.authority=="shared"){if(vs.ownership!="shared"||std::find(vs.authorities.begin(),vs.authorities.end(),"share")==vs.authorities.end())fail("SIR verifier: shared parameter lacks shared ownership capability");}
            else if(p.authority=="send"){if(vs.ownership!="sent"||std::find(vs.authorities.begin(),vs.authorities.end(),"send")==vs.authorities.end())fail("SIR verifier: send parameter lacks send ownership capability");}
        }


        std::function<std::string(SirValueId,std::set<SirValueId>&)> semantic_resource_of;
        semantic_resource_of=[&](SirValueId id,std::set<SirValueId>&seen)->std::string{if(!seen.insert(id).second)return {};auto oi=defops.find(id);if(oi==defops.end())return {};const auto&op=*oi->second;if(op.opcode==SirOpcode::PoolAlloc){const auto*ps=find_pool(pools,op.semantic_name);return ps&&ps->allocation=="heap"?((ps->ownership=="shared"?"shared-pool:":"heap-pool:")+op.semantic_name):std::string{};}if(op.opcode==SirOpcode::ManyDynamic)return "dynamic-many";if((op.opcode==SirOpcode::MoveValue||op.opcode==SirOpcode::SendValue||op.opcode==SirOpcode::SharedRetain||op.opcode==SirOpcode::RevisionMarker)&&!op.operands.empty())return semantic_resource_of(op.operands[0],seen);if(op.opcode==SirOpcode::Call){auto ci=fmap.find(op.callee);return ci==fmap.end()?std::string{}:ci->second->result_resource;}if(op.opcode==SirOpcode::Phi){std::string k;for(const auto&i:op.phi_inputs){auto local=seen;auto x=semantic_resource_of(i.value,local);if(x.empty())return {};if(k.empty())k=x;else if(k!=x)fail("SIR verifier: phi merges incompatible resource transfer families");}return k;}return {};};
        for(const auto&b:fn.blocks)if(b.terminator.kind==SirTerminatorKind::Return&&b.terminator.value){const auto&rv=find_value_sem(fn,*b.terminator.value);if(fn.result_ownership=="borrowed"||fn.result_ownership=="shared-transfer")continue;std::set<SirValueId> seen;auto rk=semantic_resource_of(*b.terminator.value,seen);if(!rk.empty()){if(fn.result_ownership!="owned-transfer"||fn.result_resource!=rk)fail("SIR verifier: returned resource disagrees with function ownership-transfer contract");if(rv.ownership!="moved"&&rv.ownership!="sent")fail("SIR verifier: owned resource must cross instruction boundary through explicit move/send");}else if(fn.result_ownership=="owned-transfer")fail("SIR verifier: owned-transfer function has a non-resource return path");}

        auto require_def=[&](SirValueId id){auto it=defs.find(id);if(it==defs.end())fail("SIR verifier: use of undefined SSA value %"+std::to_string(id));return it->second;};
        auto dominates_use=[&](SirValueId id,SirBlockId use_block,int use_index){const auto d=require_def(id);if(d.block==use_block){if(d.instruction_index>=use_index&&d.instruction_index!=-1)fail("SIR verifier: use-before-definition in block");}else if(!dom.at(use_block).contains(d.block))fail("SIR verifier: SSA definition does not dominate use");};

        std::set<SirEffectTokenId> effect_defs;
        for (const auto& b : fn.blocks) {
            if(!b.effect_entry||!effect_defs.insert(b.effect_entry).second)fail("SIR verifier: block effect-entry token must be unique/nonzero");
            if(b.id==fn.entry){if(!b.effect_phi_inputs.empty())fail("SIR verifier: entry block may not have effect phi inputs");if(fn.effect_root!=b.effect_entry)fail("SIR verifier: effect root must equal entry token");}
            else {
                std::set<EdgeKey> want; for(const auto&e:edges)if(e.to==b.id)want.insert(e);
                std::set<EdgeKey> got;
                for(const auto&i:b.effect_phi_inputs){got.insert({i.predecessor,b.id,i.source_op});if(!i.token)fail("SIR verifier: zero effect phi token");}
                if(want!=got)fail("SIR verifier: effect-token phi inputs must match every control/fault edge exactly");
            }
            SirEffectTokenId current=b.effect_entry;
            for(const auto&op:b.instructions){
                if(op.effects.empty()){if(op.effect_in||op.effect_out)fail("SIR verifier: pure op may not advance effect-token SSA");}
                else{if(op.effect_in!=current||!op.effect_out||!effect_defs.insert(op.effect_out).second)fail("SIR verifier: effectful op has broken effect-token chain");current=op.effect_out;}
            }
            if(b.effect_exit!=current||b.terminator.effect_token!=current)fail("SIR verifier: block/terminator effect token mismatch");
        }
        // Verify each effect-phi incoming token is the token at the exact source edge.
        for(const auto&b:fn.blocks) for(const auto&i:b.effect_phi_inputs){
            const auto&src=fn.blocks.at(i.predecessor); SirEffectTokenId expected=src.effect_entry;
            if(i.source_op==0) expected=src.effect_exit;
            else {
                bool found=false; SirEffectTokenId cur=src.effect_entry;
                for(const auto&op:src.instructions){if(!op.effects.empty())cur=op.effect_out;if(op.id==i.source_op){expected=cur;found=true;break;}}
                if(!found)fail("SIR verifier: effect phi source_op does not exist in predecessor");
            }
            if(expected!=i.token)fail("SIR verifier: effect phi uses wrong edge token");
        }

        for (const auto& b : fn.blocks) {
            for (std::size_t oi=0;oi<b.instructions.size();++oi) {
                const auto& op=b.instructions[oi]; const auto d=op_desc(op);
                if(op.opcode==SirOpcode::Phi){
                    std::set<SirBlockId> got; for(const auto&in:op.phi_inputs){if(!pred.at(b.id).contains(in.predecessor))fail("SIR verifier: phi incoming block is not CFG predecessor");if(!got.insert(in.predecessor).second)fail("SIR verifier: duplicate phi predecessor");const auto def=require_def(in.value);if(def.desc!=d)fail("SIR verifier: phi incoming type/cardinality mismatch");if(def.block!=in.predecessor&&!dom.at(in.predecessor).contains(def.block))fail("SIR verifier: phi incoming definition does not dominate predecessor");}
                    if(got!=pred.at(b.id))fail("SIR verifier: phi must have one incoming value per predecessor");
                } else {
                    for(const auto v:op.operands)dominates_use(v,b.id,static_cast<int>(oi));
                }
                for(const auto&fe:op.fault_edges){if(!fault_map.contains(fe.fault))fail("SIR verifier: operation references unknown fault '"+fe.fault+"'");if(fe.target>=fn.blocks.size())fail("SIR verifier: fault edge target invalid");}

                auto operand_desc=[&](std::size_t i){return require_def(op.operands.at(i)).desc;};
                switch(op.opcode){
                    case SirOpcode::Phi: break;
                    case SirOpcode::ConstUnit: if(d!=SirTypeDesc{SirType::Unit,SirCardinality::One,{}}||!op.operands.empty())fail("SIR verifier: const.unit malformed"); break;
                    case SirOpcode::ConstInt: if(!desc_integer(d)||!op.operands.empty())fail("SIR verifier: const.int must be exact-One integer"); break;
                    case SirOpcode::ConstBool: if(d!=SirTypeDesc{SirType::Bool,SirCardinality::One,{}})fail("SIR verifier: const.bool malformed"); break;
                    case SirOpcode::ConstText: if(d!=SirTypeDesc{SirType::Text,SirCardinality::One,{}})fail("SIR verifier: const.text malformed"); break;
                    case SirOpcode::OptionalSome: if(d.cardinality!=SirCardinality::ZeroOrOne||op.operands.size()!=1){fail("SIR verifier: optional.some malformed");}else{auto x=operand_desc(0);x.cardinality=SirCardinality::ZeroOrOne;if(x!=d)fail("SIR verifier: optional.some element type mismatch");} break;
                    case SirOpcode::OptionalNone: if(d.cardinality!=SirCardinality::ZeroOrOne||!op.operands.empty())fail("SIR verifier: optional.none malformed"); break;
                    case SirOpcode::ManyMake: if(d.cardinality!=SirCardinality::Many)fail("SIR verifier: many.make result must be Many");else for(std::size_t i=0;i<op.operands.size();++i){auto x=operand_desc(i);x.cardinality=SirCardinality::Many;if(x!=d)fail("SIR verifier: many.make element type mismatch");} break;
                    case SirOpcode::AggregateMake: {
                        if(d.type!=SirType::Nominal||d.cardinality!=SirCardinality::One||d.nominal!=op.semantic_name)fail("SIR verifier: aggregate.make result identity mismatch");
                        const auto* ds=find_dataset(datasets,op.semantic_name); if(!ds)fail("SIR verifier: aggregate.make names unknown dataset schema"); if(ds->fields.size()!=op.operands.size())fail("SIR verifier: aggregate.make arity mismatch");
                        for(std::size_t i=0;i<op.operands.size();++i)if(operand_desc(i)!=SirTypeDesc{ds->fields[i].type,ds->fields[i].cardinality,ds->fields[i].nominal_type})fail("SIR verifier: aggregate field type mismatch");
                        break;
                    }
                    case SirOpcode::ChoiceMake: {
                        if(d.type!=SirType::Nominal||d.cardinality!=SirCardinality::One||d.nominal!=op.semantic_name)fail("SIR verifier: choice.make result identity mismatch");
                        const auto* ch=find_choice(choices,op.semantic_name); if(!ch)fail("SIR verifier: choice.make names unknown choice"); auto it=std::find_if(ch->cases.begin(),ch->cases.end(),[&](const auto&c){return c.tag==op.tag;}); if(it==ch->cases.end())fail("SIR verifier: choice.make tag invalid"); if(it->fields.size()!=op.operands.size())fail("SIR verifier: choice payload arity mismatch");
                        for(std::size_t i=0;i<op.operands.size();++i)if(operand_desc(i)!=SirTypeDesc{it->fields[i].type,it->fields[i].cardinality,it->fields[i].nominal_type})fail("SIR verifier: choice payload type mismatch");
                        break;
                    }
                    case SirOpcode::PoolAlloc: {
                        const auto* ps=find_pool(pools,op.semantic_name); if(!ps)fail("SIR verifier: pool.alloc names unknown pool"); if(d.type!=SirType::Nominal||d.nominal!=ps->name||d.cardinality!=SirCardinality::One)fail("SIR verifier: pool.alloc result type mismatch"); if(ps->fields.size()!=op.operands.size())fail("SIR verifier: pool.alloc field arity mismatch");
                        const auto& vs=find_value_sem(fn,op.result); if(!vs.provenance)fail("SIR verifier: pool.alloc must create provenance");
                        if(ps->allocation=="heap"){
                            std::set<std::string> names;for(const auto&e:op.fault_edges)names.insert(e.fault);
                            if(names!=std::set<std::string>{"AllocationFailure"})fail("SIR verifier: @heap pool allocation must expose exactly AllocationFailure");
                        } else if(!op.fault_edges.empty()) fail("SIR verifier: non-heap pool allocation may not invent allocation fault edges in Compiler 0.8");
                        break;
                    }
                    case SirOpcode::FieldLoad: case SirOpcode::FieldAddress: {
                        if(op.operands.size()!=1)fail("SIR verifier: field projection requires one aggregate operand");
                        const auto root=operand_desc(0); if(root.type!=SirType::Nominal||root.cardinality!=SirCardinality::One)fail("SIR verifier: field root must be exact-One nominal");
                        const auto owner=before_last_dot(op.semantic_name), field=after_last_dot(op.semantic_name);
                        if(owner!=root.nominal)fail("SIR verifier: field owner does not match root nominal type");
                        const auto*fs=find_addressable_field(datasets,pools,owner,field);if(!fs)fail("SIR verifier: field projection names unknown field");
                        if(d!=SirTypeDesc{fs->type,fs->cardinality,fs->nominal_type})fail("SIR verifier: field projection result descriptor mismatch");
                        const auto&src=find_value_sem(fn,op.operands[0]);const auto&dst=find_value_sem(fn,op.result);
                        auto pit=std::find_if(fn.provenances.begin(),fn.provenances.end(),[&](const auto&p){return p.id==dst.provenance;});
                        if(pit==fn.provenances.end()||pit->parent!=src.provenance||pit->subobject!=field)fail("SIR verifier: field projection lacks correct subobject provenance");
                        if(op.opcode==SirOpcode::FieldAddress){
                            SirRegionId expected=0;
                            if(region_within(b.region,src.region))expected=b.region;
                            else if(region_within(src.region,b.region))expected=src.region;
                            else fail("SIR verifier: field borrow region is unrelated to owner lifetime");
                            if(dst.region!=expected||op.region!=expected||pit->region!=expected)fail("SIR verifier: field borrow is not narrowed to the shortest lexical/owner lifetime");
                            const auto expected_mode=dst.borrow_mode.empty()?std::string("none"):dst.borrow_mode;
                            if(dst.ownership!="borrow"&&dst.ownership!="borrow-shared"&&dst.ownership!="borrow-mut")fail("SIR verifier: field.address must produce a borrow");
                            if(!fn.borrow_edges.empty()){auto bei=std::find_if(fn.borrow_edges.begin(),fn.borrow_edges.end(),[&](const auto&e){return e.owner==src.provenance&&e.borrower==dst.provenance;});if(bei==fn.borrow_edges.end())fail("SIR verifier: field.address lacks borrow-graph edge; chain: "+provenance_path(dst.provenance));if(expected_mode!="none"&&bei->mode!=expected_mode)fail("SIR verifier: field.address borrow mode disagrees with borrow graph");}
                        }
                        break;
                    }
                    case SirOpcode::FieldStore: {
                        if(d!=SirTypeDesc{SirType::Unit,SirCardinality::One,{}}||op.operands.size()!=2)fail("SIR verifier: field.store malformed");
                        const auto root=operand_desc(0);if(root.type!=SirType::Nominal||root.cardinality!=SirCardinality::One)fail("SIR verifier: field.store root must be nominal");
                        const auto owner=before_last_dot(op.semantic_name),field=after_last_dot(op.semantic_name);const auto*fs=find_addressable_field(datasets,pools,owner,field);
                        if(!fs||owner!=root.nominal||operand_desc(1)!=SirTypeDesc{fs->type,fs->cardinality,fs->nominal_type})fail("SIR verifier: field.store descriptor mismatch");
                        if(!fs->revisable)fail("SIR verifier: field.store targets a non-revisable field");
                        const auto&outv=find_value_sem(fn,op.result);if(std::find(outv.authorities.begin(),outv.authorities.end(),"revise")==outv.authorities.end())fail("SIR verifier: field.store lacks verified revise capability");
                        const auto&rootv=find_value_sem(fn,op.operands[0]);const auto&stored=find_value_sem(fn,op.operands[1]);
                        if(stored.ownership=="borrow"&&!region_within(rootv.region,stored.region))fail("SIR verifier: storing borrow would let destination outlive borrow lifetime");
                        break;
                    }
                    case SirOpcode::ManyDynamic: {
                        if(d.cardinality!=SirCardinality::Many||op.operands.size()!=1||operand_desc(0)!=SirTypeDesc{SirType::U64,SirCardinality::One,{}})fail("SIR verifier: many.dynamic malformed");
                        std::set<std::string>got;for(const auto&e:op.fault_edges)got.insert(e.fault);if(got!=std::set<std::string>{"AllocationFailure","ArithmeticOverflow"})fail("SIR verifier: many.dynamic must expose AllocationFailure and ArithmeticOverflow");
                        if(find_value_sem(fn,op.result).provenance==0)fail("SIR verifier: many.dynamic requires owned provenance");
                        break;
                    }
                    case SirOpcode::ManyPush: {
                        if(d!=SirTypeDesc{SirType::Unit,SirCardinality::One,{}}||op.operands.size()!=2)fail("SIR verifier: many.push malformed");
                        const auto list=operand_desc(0);if(list.cardinality!=SirCardinality::Many)fail("SIR verifier: many.push first operand must be Many");
                        auto elem=list;elem.cardinality=SirCardinality::One;if(operand_desc(1)!=elem)fail("SIR verifier: many.push element descriptor mismatch");
                        const auto&lv=find_value_sem(fn,op.operands[0]);if(std::find(lv.authorities.begin(),lv.authorities.end(),"revise")==lv.authorities.end())fail("SIR verifier: many.push list lacks revise authority");
                        std::set<std::string>got;for(const auto&e:op.fault_edges)got.insert(e.fault);if(got!=std::set<std::string>{"AllocationFailure","ArithmeticOverflow"})fail("SIR verifier: many.push must expose AllocationFailure and ArithmeticOverflow");
                        break;
                    }
                    case SirOpcode::ManyLength:
                        if(d!=SirTypeDesc{SirType::U64,SirCardinality::One,{}}||op.operands.size()!=1||operand_desc(0).cardinality!=SirCardinality::Many)fail("SIR verifier: many.length malformed");
                        break;
                    case SirOpcode::Borrow: case SirOpcode::BorrowMut: {
                        const bool mut=op.opcode==SirOpcode::BorrowMut;
                        if(op.operands.size()!=1||operand_desc(0)!=d)fail("SIR verifier: borrow must preserve type/cardinality");
                        const auto&src=find_value_sem(fn,op.operands[0]); const auto&dst=find_value_sem(fn,op.result);
                        SirRegionId expected=0;
                        if(region_within(b.region,src.region))expected=b.region;
                        else if(region_within(src.region,b.region))expected=src.region;
                        else fail("SIR verifier: borrow region is unrelated to owner lifetime; source "+provenance_path(src.provenance));
                        if(dst.region!=expected||op.region!=expected)fail("SIR verifier: borrow is not narrowed to the shortest lexical/owner lifetime; chain: "+provenance_path(dst.provenance));
                        if(std::find(src.authorities.begin(),src.authorities.end(),"read")==src.authorities.end())fail("SIR verifier: borrow source lacks read authority");
                        if(mut&&std::find(src.authorities.begin(),src.authorities.end(),"revise")==src.authorities.end())fail("SIR verifier: borrow_mut source lacks revise authority");
                        const auto want_mode=mut?std::string("unique-mut"):std::string("shared-read");
                        if(dst.borrow_mode!=want_mode)fail("SIR verifier: borrow result mode marker missing");
                        if(mut?dst.ownership!="borrow-mut":(dst.ownership!="borrow-shared"&&dst.ownership!="borrow"))fail("SIR verifier: borrow result ownership marker missing");
                        if(std::find(dst.authorities.begin(),dst.authorities.end(),"read")==dst.authorities.end())fail("SIR verifier: borrow result lacks read authority");
                        if(!fn.borrow_edges.empty()){auto bei=std::find_if(fn.borrow_edges.begin(),fn.borrow_edges.end(),[&](const auto&e){return e.owner==src.provenance&&e.borrower==dst.provenance&&e.mode==want_mode;});if(bei==fn.borrow_edges.end())fail("SIR verifier: borrow operation lacks matching borrow-graph edge; chain: "+provenance_path(dst.provenance));}
                        break;
                    }
                    case SirOpcode::MoveValue: {
                        if(op.operands.size()!=1||operand_desc(0)!=d)fail("SIR verifier: move must preserve type/cardinality");
                        const auto&src=find_value_sem(fn,op.operands[0]); const auto&dst=find_value_sem(fn,op.result);
                        if(src.provenance!=dst.provenance)fail("SIR verifier: move must preserve provenance");
                        if(std::find(src.authorities.begin(),src.authorities.end(),"move")==src.authorities.end())fail("SIR verifier: move source lacks move authority");
                        if(dst.ownership!="moved")fail("SIR verifier: move result ownership marker missing");
                        break;
                    }
                    case SirOpcode::FieldMove: {
                        if (op.operands.size() != 1) fail("SIR verifier: field.move requires one aggregate root");
                        const auto root = operand_desc(0);
                        if (root.type != SirType::Nominal || root.cardinality != SirCardinality::One) fail("SIR verifier: field.move root must be exact-One nominal");
                        const auto owner = before_last_dot(op.semantic_name);
                        const auto field = after_last_dot(op.semantic_name);
                        const auto* fs = find_addressable_field(datasets, pools, owner, field);
                        if (!fs || owner != root.nominal || d != SirTypeDesc{fs->type, fs->cardinality, fs->nominal_type}) fail("SIR verifier: field.move descriptor mismatch");
                        const auto& src = find_value_sem(fn, op.operands[0]);
                        const auto& dst = find_value_sem(fn, op.result);
                        if (std::find(src.authorities.begin(), src.authorities.end(), "move") == src.authorities.end()) fail("SIR verifier: partial move root lacks move authority");
                        if (dst.ownership != "owned" || dst.resource_family.empty()) fail("SIR verifier: field.move must produce owned resource semantics");
                        if (!dst.provenance || !prov_map.contains(dst.provenance) || prov_map.at(dst.provenance)->parent != src.provenance) fail("SIR verifier: field.move provenance does not descend from aggregate owner; chain: " + provenance_path(dst.provenance));
                        break;
                    }
                    case SirOpcode::ResourceRelease: {
                        if (d != SirTypeDesc{SirType::Unit, SirCardinality::One, {}} || op.operands.size() != 1) fail("SIR verifier: resource.release malformed");
                        const auto& src = find_value_sem(fn, op.operands[0]);
                        if (src.resource_family.empty() && src.borrow_mode == "none") fail("SIR verifier: resource.release requires a resource/borrow operand");
                        if (op.effects.empty()) fail("SIR verifier: resource.release must advance effect-token SSA");
                        break;
                    }
                    case SirOpcode::SharedRetain: {
                        if (op.operands.size() != 1 || operand_desc(0) != d) fail("SIR verifier: shared.retain must preserve descriptor");
                        const auto& src = find_value_sem(fn, op.operands[0]);
                        const auto& dst = find_value_sem(fn, op.result);
                        if (src.ownership != "shared" || std::find(src.authorities.begin(), src.authorities.end(), "share") == src.authorities.end()) fail("SIR verifier: shared.retain source lacks shared/share capability");
                        if (dst.ownership != "shared" || dst.resource_family.rfind("shared-pool:", 0) != 0) fail("SIR verifier: shared.retain result lacks shared resource identity");
                        if (!dst.provenance || !prov_map.contains(dst.provenance) || prov_map.at(dst.provenance)->parent != src.provenance) fail("SIR verifier: retained share requires child provenance; chain: " + provenance_path(dst.provenance));
                        break;
                    }
                    case SirOpcode::SendValue: {
                        if (op.operands.size() != 1 || operand_desc(0) != d) fail("SIR verifier: send must preserve descriptor");
                        const auto& src = find_value_sem(fn, op.operands[0]);
                        const auto& dst = find_value_sem(fn, op.result);
                        if (std::find(src.authorities.begin(), src.authorities.end(), "send") == src.authorities.end()) fail("SIR verifier: send source lacks send capability");
                        if (dst.ownership != "sent") fail("SIR verifier: send result ownership marker missing");
                        if (src.provenance != dst.provenance) fail("SIR verifier: send must preserve provenance");
                        break;
                    }
                    case SirOpcode::Not: if(op.operands.size()!=1||d!=SirTypeDesc{SirType::Bool,SirCardinality::One,{}}||operand_desc(0)!=d)fail("SIR verifier: not requires bool"); break;
                    case SirOpcode::CheckedNeg: case SirOpcode::CheckedAdd: case SirOpcode::CheckedSub: case SirOpcode::CheckedMul: case SirOpcode::CheckedDiv: case SirOpcode::CheckedMod:
                        if(!desc_integer(d))fail("SIR verifier: checked arithmetic result must be exact-One integer");
                        for(auto v:op.operands)if(require_def(v).desc!=d)fail("SIR verifier: checked arithmetic operand type mismatch");
                        if(std::find(op.effects.begin(),op.effects.end(),"revise.relative")!=op.effects.end()){const auto&vs=find_value_sem(fn,op.result);if(std::find(vs.authorities.begin(),vs.authorities.end(),"revise")==vs.authorities.end())fail("SIR verifier: revise.relative result lacks revise capability");}
                        break;
                    case SirOpcode::CompareEq: case SirOpcode::CompareNe: case SirOpcode::CompareLt: case SirOpcode::CompareLe: case SirOpcode::CompareGt: case SirOpcode::CompareGe:
                        if(d!=SirTypeDesc{SirType::Bool,SirCardinality::One,{}}||op.operands.size()!=2||operand_desc(0)!=operand_desc(1)) fail("SIR verifier: comparison malformed");
                        break;
                    case SirOpcode::Call: {
                        auto it=fmap.find(op.callee);if(it==fmap.end())fail("SIR verifier: call targets unknown function '"+op.callee+"'");const auto&callee=*it->second;if(callee.parameters.size()!=op.operands.size())fail("SIR verifier: call arity mismatch");
                        for(std::size_t i=0;i<op.operands.size();++i){if(operand_desc(i)!=param_desc(callee.parameters[i]))fail("SIR verifier: call argument descriptor mismatch");const auto&av=find_value_sem(fn,op.operands[i]);const auto mode=callee.parameters[i].authority;if(mode=="borrow"&&av.borrow_mode!="shared-read")fail("SIR verifier: borrow parameter requires explicit shared-read borrow argument");if(mode=="borrow_mut"&&av.borrow_mode!="unique-mut")fail("SIR verifier: borrow_mut parameter requires explicit unique-mut borrow argument");if(mode=="move"&&av.ownership!="moved")fail("SIR verifier: move parameter requires moved argument");if(mode=="shared"&&av.ownership!="shared")fail("SIR verifier: shared parameter requires shared argument");if(mode=="send"&&av.ownership!="sent")fail("SIR verifier: send parameter requires sent argument");}
                        if (d != SirTypeDesc{callee.result_type, callee.result_cardinality, callee.result_nominal_type}) fail("SIR verifier: call result descriptor mismatch");
                        const auto& callv = find_value_sem(fn, op.result);
                        if(callee.result_ownership=="owned-transfer"){if(!callv.provenance||callv.ownership!="owned"||op.provenance!=callv.provenance)fail("SIR verifier: resource-returning call must create fresh caller-owned provenance");}
                        else if(callee.result_ownership=="shared-transfer"){if(!callv.provenance||callv.ownership!="shared"||op.provenance!=callv.provenance)fail("SIR verifier: shared-returning call must create caller-owned shared provenance");}
                        else if(callee.result_ownership=="borrowed"){if(!callv.provenance||callv.borrow_mode!=callee.result_borrow_mode)fail("SIR verifier: borrowed call result lacks declared borrow semantics");auto spi=std::find_if(callee.parameters.begin(),callee.parameters.end(),[&](const auto&p){return p.name==callee.result_borrow_from;});if(spi==callee.parameters.end())fail("SIR verifier: borrowed callee result source is missing");auto idx=static_cast<std::size_t>(std::distance(callee.parameters.begin(),spi));const auto&src=find_value_sem(fn,op.operands[idx]);auto bei=std::find_if(fn.borrow_edges.begin(),fn.borrow_edges.end(),[&](const auto&e){return e.owner==src.provenance&&e.borrower==callv.provenance&&e.cross_boundary&&e.source_parameter==callee.result_borrow_from;});if(bei==fn.borrow_edges.end())fail("SIR verifier: borrowed call result lacks cross-boundary borrow edge; chain: "+provenance_path(callv.provenance));}
                        std::set<std::string>got;for(const auto&e:op.fault_edges)got.insert(e.fault);std::set<std::string>want(callee.declared_faults.begin(),callee.declared_faults.end());if(got!=want)fail("SIR verifier: call must expose callee's exact closed fault set");break;
                    }
                    case SirOpcode::WriteText: if(d!=SirTypeDesc{SirType::Unit,SirCardinality::One,{}}||op.operands.size()!=1||operand_desc(0)!=SirTypeDesc{SirType::Text,SirCardinality::One,{}})fail("SIR verifier: write_text malformed"); break;
                    case SirOpcode::RelationLink: case SirOpcode::RelationUnlink: {
                        if(d!=SirTypeDesc{SirType::Unit,SirCardinality::One,{}}||op.operands.size()!=2) fail("SIR verifier: relation op malformed");
                        const auto*r=find_relation(relations,op.semantic_name);
                        if(!r) fail("SIR verifier: relation op names unknown relation");
                        if(operand_desc(0)!=SirTypeDesc{SirType::Nominal,SirCardinality::One,r->source}||operand_desc(1)!=SirTypeDesc{SirType::Nominal,SirCardinality::One,r->target}) fail("SIR verifier: relation endpoint descriptor mismatch");
                        break;
                    }
                    case SirOpcode::TransactionBegin: case SirOpcode::TransactionCommit: case SirOpcode::TransactionAbort: case SirOpcode::ParallelBegin: case SirOpcode::ParallelEnd: case SirOpcode::TaskBegin: case SirOpcode::TaskEnd:
                        if(d!=SirTypeDesc{SirType::Unit,SirCardinality::One,{}}||!op.operands.empty()||op.effects.empty()) fail("SIR verifier: region marker malformed");
                        if((op.opcode==SirOpcode::TaskBegin||op.opcode==SirOpcode::TaskEnd)){auto rr=std::find_if(fn.regions.begin(),fn.regions.end(),[&](const auto&r){return r.id==op.region;});if(rr==fn.regions.end()||rr->kind!="task")fail("SIR verifier: task marker must name a task region");}
                        break;
                    case SirOpcode::RevisionMarker:
                        if(op.operands.size()!=1||operand_desc(0)!=d)fail("SIR verifier: revision marker must preserve descriptor");
                        if(std::find(op.effects.begin(),op.effects.end(),"revise.replace")!=op.effects.end()){const auto&vs=find_value_sem(fn,op.result);if(std::find(vs.authorities.begin(),vs.authorities.end(),"revise")==vs.authorities.end())fail("SIR verifier: revise.replace result lacks revise capability");}
                        break;
                }
            }

            const auto&t=b.terminator;
            if(t.condition)dominates_use(t.condition,b.id,static_cast<int>(b.instructions.size()));
            if(t.value)dominates_use(*t.value,b.id,static_cast<int>(b.instructions.size()));
            for(auto v:t.fault_payload)dominates_use(v,b.id,static_cast<int>(b.instructions.size()));
            switch(t.kind){
                case SirTerminatorKind::Br: if(t.target>=fn.blocks.size())fail("SIR verifier: invalid br target"); break;
                case SirTerminatorKind::CondBr: if(t.true_target>=fn.blocks.size()||t.false_target>=fn.blocks.size())fail("SIR verifier: invalid condbr target");if(require_def(t.condition).desc!=SirTypeDesc{SirType::Bool,SirCardinality::One,{}})fail("SIR verifier: condbr requires exact-One bool");break;
                case SirTerminatorKind::Return: {
                    SirTypeDesc want{fn.result_type,fn.result_cardinality,fn.result_nominal_type};
                    if(want==SirTypeDesc{SirType::Unit,SirCardinality::One,{}}){if(t.value)fail("SIR verifier: unit return carries value");}
                    else{if(!t.value||require_def(*t.value).desc!=want)fail("SIR verifier: return descriptor mismatch");}
                    if(t.value){const auto&rv=find_value_sem(fn,*t.value);const bool borrowed=rv.borrow_mode!="none"||rv.ownership=="borrow"||rv.ownership=="borrow-shared"||rv.ownership=="borrow-mut";if(borrowed){if(fn.result_ownership!="borrowed")fail("SIR verifier: borrowed value may not escape the function without an explicit cross-boundary lifetime contract; chain: "+provenance_path(rv.provenance));if(rv.borrow_mode!=fn.result_borrow_mode)fail("SIR verifier: returned borrow mode disagrees with function result contract");auto pi=std::find_if(fn.parameters.begin(),fn.parameters.end(),[&](const auto&p){return p.name==fn.result_borrow_from;});if(pi==fn.parameters.end())fail("SIR verifier: result borrow source parameter not found");const auto&pv=find_value_sem(fn,pi->value);SirProvenanceId cur=rv.provenance;std::set<SirProvenanceId>seen;bool reaches=false;while(cur&&seen.insert(cur).second){if(cur==pv.provenance){reaches=true;break;}auto itp=prov_map.find(cur);if(itp==prov_map.end())break;cur=itp->second->parent;}if(!reaches)fail("SIR verifier: returned borrow provenance does not reach declared source parameter; chain: "+provenance_path(rv.provenance));}}
                    break;
                }
                case SirTerminatorKind::FaultReturn: {
                    auto it=fault_map.find(t.fault);if(it==fault_map.end())fail("SIR verifier: fault.return names unknown fault");if(!declared_faults.contains(t.fault))fail("SIR verifier: fault.return escapes undeclared fault");
                    if(t.fault_payload_passthrough){if(!t.fault_payload.empty())fail("SIR verifier: passthrough fault may not carry concrete payload values");}
                    else {if(t.fault_payload.size()!=it->second->payload.size())fail("SIR verifier: fault payload arity mismatch");for(std::size_t i=0;i<t.fault_payload.size();++i)if(require_def(t.fault_payload[i]).desc!=SirTypeDesc{it->second->payload[i].type,it->second->payload[i].cardinality,it->second->payload[i].nominal_type})fail("SIR verifier: fault payload descriptor mismatch");}
                    break;
                }
                case SirTerminatorKind::Unreachable: break;
            }
        }
    }
}

struct LineReader {
    explicit LineReader(std::string_view text):in_(std::string(text)){}
    bool next(std::vector<std::string>& w){std::string line;while(std::getline(in_,line)){++line_;if(line.empty())continue;w=split_ws(line);if(!w.empty())return true;}return false;}
    [[noreturn]] void error(const std::string&m)const{fail("SIR deserialize line "+std::to_string(line_)+": "+m);}
    std::istringstream in_; std::size_t line_{0};
};

void serialize_field(std::ostringstream&o,const char*tag,const SirFieldSchema&f){o<<tag<<' '<<hex_encode(f.name)<<' '<<sir_type_name(f.type)<<' '<<sir_cardinality_name(f.cardinality)<<' '<<hex_encode(f.nominal_type)<<' '<<(f.key?1:0)<<' '<<(f.revisable?1:0)<<'\n';}
SirFieldSchema parse_field(const std::vector<std::string>&w,const char*tag){if(w.size()!=7||w[0]!=tag)fail(std::string("SIR deserialize: malformed ")+tag);return{hex_decode(w[1]),sir_type_from_name(w[2]),sir_cardinality_from_name(w[3]),hex_decode(w[4]),parse_num<int>(w[5],"key")!=0,parse_num<int>(w[6],"revisable")!=0};}

void serialize_schemas(std::ostringstream&o,const std::vector<SirFaultIdentity>&faults,const std::vector<SirDatasetSchema>&datasets,const std::vector<SirChoiceSchema>&choices,const std::vector<SirRelationSchema>&relations,const std::vector<SirPoolSchema>&pools){
    for(const auto&f:faults){o<<"fault "<<f.code<<' '<<hex_encode(f.name)<<' '<<f.payload.size()<<'\n';for(const auto&x:f.payload)serialize_field(o,"ffield",x);}
    for(const auto&d:datasets){o<<"dataset "<<hex_encode(d.name)<<' '<<hex_encode(d.key)<<' '<<d.fields.size()<<' '<<d.indexes.size()<<'\n';for(const auto&f:d.fields)serialize_field(o,"dfield",f);for(const auto&i:d.indexes)o<<"index "<<hex_encode(i)<<'\n';}
    for(const auto&c:choices){o<<"choice "<<hex_encode(c.name)<<' '<<c.cases.size()<<'\n';for(const auto&a:c.cases){o<<"case "<<a.tag<<' '<<hex_encode(a.name)<<' '<<a.fields.size()<<'\n';for(const auto&f:a.fields)serialize_field(o,"cfield",f);}}
    for(const auto&r:relations)o<<"relation "<<hex_encode(r.name)<<' '<<hex_encode(r.source)<<' '<<hex_encode(r.target)<<' '<<hex_encode(r.source_cardinality)<<' '<<hex_encode(r.target_cardinality)<<' '<<hex_encode(r.ownership)<<' '<<(r.unique_pair?1:0)<<' '<<(r.reverse_index?1:0)<<'\n';
    for(const auto&p:pools){o<<"pool "<<hex_encode(p.name)<<' '<<hex_encode(p.lifetime)<<' '<<hex_encode(p.allocation)<<' '<<hex_encode(p.reclaim)<<' '<<hex_encode(p.ownership)<<' '<<p.fields.size()<<'\n';for(const auto&f:p.fields)serialize_field(o,"pfield",f);}
}

void serialize_function(std::ostringstream&o,const SirFunction&fn){
    o<<"function "<<hex_encode(fn.name)<<' '<<(fn.is_public?1:0)<<' '<<sir_type_name(fn.result_type)<<' '<<sir_cardinality_name(fn.result_cardinality)<<' '<<hex_encode(fn.result_nominal_type)<<' '<<fn.entry<<' '<<fn.root_region<<' '<<fn.effect_root<<' '<<fn.declared_faults.size()<<' '<<fn.effects.size()<<' '<<hex_encode(fn.result_ownership)<<' '<<hex_encode(fn.result_resource)<<' '<<hex_encode(fn.result_borrow_mode)<<' '<<hex_encode(fn.result_borrow_from)<<'\n';
    for(const auto&f:fn.declared_faults) o<<"declfault "<<hex_encode(f)<<'\n';
    for(const auto&e:fn.effects) o<<"fneffect "<<hex_encode(e)<<'\n';
    for(const auto&p:fn.parameters)o<<"param "<<p.value<<' '<<sir_type_name(p.type)<<' '<<sir_cardinality_name(p.cardinality)<<' '<<hex_encode(p.nominal_type)<<' '<<hex_encode(p.name)<<' '<<hex_encode(p.authority)<<' '<<hex_encode(p.ownership)<<'\n';
    for(const auto&r:fn.regions)o<<"region "<<r.id<<' '<<r.parent<<' '<<hex_encode(r.name)<<' '<<hex_encode(r.kind)<<' '<<hex_encode(r.lifetime)<<' '<<hex_encode(r.allocation)<<' '<<hex_encode(r.reclaim)<<'\n';
    for(const auto&p:fn.provenances)o<<"provenance "<<p.id<<' '<<p.region<<' '<<p.parent<<' '<<hex_encode(p.origin)<<' '<<hex_encode(p.ownership)<<' '<<hex_encode(p.subobject)<<' '<<hex_encode(p.resource_family)<<' '<<hex_encode(p.borrow_mode)<<' '<<(p.pinned?1:0)<<'\n';
    for(const auto&e:fn.borrow_edges)o<<"borrowedge "<<e.id<<' '<<e.owner<<' '<<e.borrower<<' '<<e.region<<' '<<hex_encode(e.mode)<<' '<<(e.cross_boundary?1:0)<<' '<<hex_encode(e.source_parameter)<<'\n';
    for(const auto&e:fn.ownership_edges)o<<"ownershipedge "<<e.id<<' '<<e.owner<<' '<<e.child<<' '<<e.region<<' '<<hex_encode(e.role)<<' '<<hex_encode(e.path)<<' '<<e.ordinal<<'\n';
    for(const auto&e:fn.task_captures)o<<"taskcapture "<<e.id<<' '<<e.parallel_region<<' '<<e.task_region<<' '<<e.value<<' '<<e.provenance<<' '<<hex_encode(e.mode)<<'\n';
    for(const auto&e:fn.rollback_obligations)o<<"rollback "<<e.id<<' '<<e.transaction_region<<' '<<e.provenance<<' '<<e.source_op<<' '<<hex_encode(e.action)<<'\n';
    for(const auto&v:fn.values){o<<"value "<<v.value<<' '<<sir_type_name(v.type)<<' '<<sir_cardinality_name(v.cardinality)<<' '<<hex_encode(v.nominal_type)<<' '<<v.region<<' '<<v.provenance<<' '<<hex_encode(v.ownership)<<' '<<hex_encode(v.resource_family)<<' '<<hex_encode(v.borrow_mode)<<' '<<v.authorities.size()<<'\n';for(const auto&a:v.authorities)o<<"authority "<<hex_encode(a)<<'\n';}
    for(const auto&b:fn.blocks){
        o<<"block "<<b.id<<' '<<hex_encode(b.label)<<' '<<b.region<<' '<<b.effect_entry<<' '<<b.effect_exit<<' '<<b.effect_phi_inputs.size()<<'\n';
        for(const auto&e:b.effect_phi_inputs)o<<"effectphi "<<e.predecessor<<' '<<e.source_op<<' '<<e.token<<'\n';
        for(const auto&op:b.instructions){
            o<<"op "<<op.id<<' '<<sir_opcode_name(op.opcode)<<' '<<op.result<<' '<<sir_type_name(op.type)<<' '<<sir_cardinality_name(op.cardinality)<<' '<<hex_encode(op.nominal_type)<<' '<<op.region<<' '<<op.provenance<<' '<<op.effect_in<<' '<<op.effect_out<<' '<<op.int_value<<' '<<(op.bool_value?1:0)<<' '<<hex_encode(op.text_value)<<' '<<hex_encode(op.callee)<<' '<<hex_encode(op.semantic_name)<<' '<<op.tag<<' '<<op.where.line<<' '<<op.where.column<<' '<<op.operands.size()<<' '<<op.phi_inputs.size()<<' '<<op.fault_edges.size()<<' '<<op.effects.size()<<'\n';
            for(auto v:op.operands) o<<"operand "<<v<<'\n';
            for(const auto&x:op.phi_inputs) o<<"phi "<<x.predecessor<<' '<<x.value<<'\n';
            for(const auto&x:op.fault_edges) o<<"faultedge "<<hex_encode(x.fault)<<' '<<x.target<<'\n';
            for(const auto&e:op.effects) o<<"effect "<<hex_encode(e)<<'\n';
        }
        const auto&t=b.terminator;o<<"term "<<sir_terminator_name(t.kind)<<' '<<t.target<<' '<<t.true_target<<' '<<t.false_target<<' '<<t.condition<<' '<<(t.value?1:0)<<' '<<(t.value?*t.value:0)<<' '<<hex_encode(t.fault)<<' '<<t.fault_payload.size()<<' '<<(t.fault_payload_passthrough?1:0)<<' '<<t.effect_token<<' '<<t.where.line<<' '<<t.where.column<<'\n';for(auto v:t.fault_payload)o<<"faultarg "<<v<<'\n';
        o<<"endblock\n";
    }
    o<<"endfunction\n";
}

SirFunction parse_function(LineReader&r,const std::vector<std::string>&head){
    if(head.size()!=11&&head.size()!=13&&head.size()!=15) r.error("malformed function header");
    SirFunction fn;fn.name=hex_decode(head[1]);fn.is_public=parse_num<int>(head[2],"public")!=0;fn.result_type=sir_type_from_name(head[3]);fn.result_cardinality=sir_cardinality_from_name(head[4]);fn.result_nominal_type=hex_decode(head[5]);fn.entry=parse_num<SirBlockId>(head[6],"entry");fn.root_region=parse_num<SirRegionId>(head[7],"root region");fn.effect_root=parse_num<SirEffectTokenId>(head[8],"effect root");const auto nf=parse_num<std::size_t>(head[9],"fault count"),ne=parse_num<std::size_t>(head[10],"effect count");if(head.size()>=13){fn.result_ownership=hex_decode(head[11]);fn.result_resource=hex_decode(head[12]);}if(head.size()==15){fn.result_borrow_mode=hex_decode(head[13]);fn.result_borrow_from=hex_decode(head[14]);}std::vector<std::string>w;
    for(std::size_t i=0;i<nf;++i){if(!r.next(w)||w.size()!=2||w[0]!="declfault")r.error("expected declfault");fn.declared_faults.push_back(hex_decode(w[1]));}
    for(std::size_t i=0;i<ne;++i){if(!r.next(w)||w.size()!=2||w[0]!="fneffect")r.error("expected fneffect");fn.effects.push_back(hex_decode(w[1]));}
    while(r.next(w)){
        if(w[0]=="param"){if(w.size()!=7&&w.size()!=8)r.error("malformed param");SirParameter p;p.value=parse_num<SirValueId>(w[1],"param value");p.type=sir_type_from_name(w[2]);p.cardinality=sir_cardinality_from_name(w[3]);p.nominal_type=hex_decode(w[4]);p.name=hex_decode(w[5]);p.authority=hex_decode(w[6]);if(w.size()==8)p.ownership=hex_decode(w[7]);fn.parameters.push_back(std::move(p));}
        else if(w[0]=="region"){if(w.size()!=8)r.error("malformed region");fn.regions.push_back({parse_num<SirRegionId>(w[1],"region"),parse_num<SirRegionId>(w[2],"parent"),hex_decode(w[3]),hex_decode(w[4]),hex_decode(w[5]),hex_decode(w[6]),hex_decode(w[7])});}
        else if(w[0]=="provenance"){if(w.size()!=8&&w.size()!=10)r.error("malformed provenance");SirProvenance p;p.id=parse_num<SirProvenanceId>(w[1],"provenance");p.region=parse_num<SirRegionId>(w[2],"region");p.parent=parse_num<SirProvenanceId>(w[3],"parent provenance");p.origin=hex_decode(w[4]);p.ownership=hex_decode(w[5]);p.subobject=hex_decode(w[6]);if(w.size()==10){p.resource_family=hex_decode(w[7]);p.borrow_mode=hex_decode(w[8]);p.pinned=parse_num<int>(w[9],"pinned")!=0;}else p.pinned=parse_num<int>(w[7],"pinned")!=0;fn.provenances.push_back(std::move(p));}
        else if(w[0]=="borrowedge"){if(w.size()!=8)r.error("malformed borrowedge");fn.borrow_edges.push_back({parse_num<std::uint32_t>(w[1],"borrow edge"),parse_num<SirProvenanceId>(w[2],"borrow owner"),parse_num<SirProvenanceId>(w[3],"borrower"),parse_num<SirRegionId>(w[4],"borrow region"),hex_decode(w[5]),parse_num<int>(w[6],"cross boundary")!=0,hex_decode(w[7])});}
        else if(w[0]=="ownershipedge"){if(w.size()!=8)r.error("malformed ownershipedge");fn.ownership_edges.push_back({parse_num<std::uint32_t>(w[1],"ownership edge"),parse_num<SirProvenanceId>(w[2],"owner"),parse_num<SirProvenanceId>(w[3],"child"),parse_num<SirRegionId>(w[4],"ownership region"),hex_decode(w[5]),hex_decode(w[6]),parse_num<std::uint32_t>(w[7],"ordinal")});}
        else if(w[0]=="taskcapture"){if(w.size()!=7)r.error("malformed taskcapture");fn.task_captures.push_back({parse_num<std::uint32_t>(w[1],"task capture"),parse_num<SirRegionId>(w[2],"parallel region"),parse_num<SirRegionId>(w[3],"task region"),parse_num<SirValueId>(w[4],"captured value"),parse_num<SirProvenanceId>(w[5],"capture provenance"),hex_decode(w[6])});}
        else if(w[0]=="rollback"){if(w.size()!=6)r.error("malformed rollback");fn.rollback_obligations.push_back({parse_num<std::uint32_t>(w[1],"rollback"),parse_num<SirRegionId>(w[2],"transaction region"),parse_num<SirProvenanceId>(w[3],"rollback provenance"),parse_num<SirOpId>(w[4],"rollback op"),hex_decode(w[5])});}
        else if(w[0]=="value"){
            if(w.size()!=9&&w.size()!=11) r.error("malformed value");
            SirValueSemantics v;v.value=parse_num<SirValueId>(w[1],"value");v.type=sir_type_from_name(w[2]);v.cardinality=sir_cardinality_from_name(w[3]);v.nominal_type=hex_decode(w[4]);v.region=parse_num<SirRegionId>(w[5],"value region");v.provenance=parse_num<SirProvenanceId>(w[6],"value provenance");v.ownership=hex_decode(w[7]);std::size_t count_index=8;if(w.size()==11){v.resource_family=hex_decode(w[8]);v.borrow_mode=hex_decode(w[9]);count_index=10;}const auto n=parse_num<std::size_t>(w[count_index],"authority count");for(std::size_t i=0;i<n;++i){if(!r.next(w)||w.size()!=2||w[0]!="authority")r.error("expected authority");v.authorities.push_back(hex_decode(w[1]));}fn.values.push_back(std::move(v));
        }
        else if(w[0]=="block"){
            if(w.size()!=7) r.error("malformed block");
            SirBlock b;b.id=parse_num<SirBlockId>(w[1],"block");b.label=hex_decode(w[2]);b.region=parse_num<SirRegionId>(w[3],"block region");b.effect_entry=parse_num<SirEffectTokenId>(w[4],"effect entry");b.effect_exit=parse_num<SirEffectTokenId>(w[5],"effect exit");const auto eph=parse_num<std::size_t>(w[6],"effect phi count");for(std::size_t i=0;i<eph;++i){if(!r.next(w)||w.size()!=4||w[0]!="effectphi")r.error("expected effectphi");b.effect_phi_inputs.push_back({parse_num<SirBlockId>(w[1],"pred"),parse_num<SirOpId>(w[2],"source op"),parse_num<SirEffectTokenId>(w[3],"token")});}
            while(r.next(w)){
                if(w[0]=="op"){
                    if(w.size()!=23) r.error("malformed op");
                    SirInstruction op;op.id=parse_num<SirOpId>(w[1],"op id");op.opcode=sir_opcode_from_name(w[2]);op.result=parse_num<SirValueId>(w[3],"result");op.type=sir_type_from_name(w[4]);op.cardinality=sir_cardinality_from_name(w[5]);op.nominal_type=hex_decode(w[6]);op.region=parse_num<SirRegionId>(w[7],"op region");op.provenance=parse_num<SirProvenanceId>(w[8],"op provenance");op.effect_in=parse_num<SirEffectTokenId>(w[9],"effect in");op.effect_out=parse_num<SirEffectTokenId>(w[10],"effect out");op.int_value=parse_num<std::int64_t>(w[11],"int");op.bool_value=parse_num<int>(w[12],"bool")!=0;op.text_value=hex_decode(w[13]);op.callee=hex_decode(w[14]);op.semantic_name=hex_decode(w[15]);op.tag=parse_num<std::uint32_t>(w[16],"tag");op.where.line=parse_num<std::uint32_t>(w[17],"line");op.where.column=parse_num<std::uint32_t>(w[18],"column");const auto no=parse_num<std::size_t>(w[19],"operand count"),np=parse_num<std::size_t>(w[20],"phi count"),nfault=parse_num<std::size_t>(w[21],"fault edge count"),nfx=parse_num<std::size_t>(w[22],"effect count");
                    for(std::size_t i=0;i<no;++i){if(!r.next(w)||w.size()!=2||w[0]!="operand")r.error("expected operand");op.operands.push_back(parse_num<SirValueId>(w[1],"operand"));}
                    for(std::size_t i=0;i<np;++i){if(!r.next(w)||w.size()!=3||w[0]!="phi")r.error("expected phi");op.phi_inputs.push_back({parse_num<SirBlockId>(w[1],"phi pred"),parse_num<SirValueId>(w[2],"phi value")});}
                    for(std::size_t i=0;i<nfault;++i){if(!r.next(w)||w.size()!=3||w[0]!="faultedge")r.error("expected faultedge");op.fault_edges.push_back({hex_decode(w[1]),parse_num<SirBlockId>(w[2],"fault target")});}
                    for(std::size_t i=0;i<nfx;++i){if(!r.next(w)||w.size()!=2||w[0]!="effect")r.error("expected effect");op.effects.push_back(hex_decode(w[1]));}
                    b.instructions.push_back(std::move(op));
                }
                else if(w[0]=="term"){
                    if(w.size()!=14) r.error("malformed terminator");
                    SirTerminator t;t.kind=sir_terminator_from_name(w[1]);t.target=parse_num<SirBlockId>(w[2],"target");t.true_target=parse_num<SirBlockId>(w[3],"true target");t.false_target=parse_num<SirBlockId>(w[4],"false target");t.condition=parse_num<SirValueId>(w[5],"condition");if(parse_num<int>(w[6],"has value"))t.value=parse_num<SirValueId>(w[7],"return value");t.fault=hex_decode(w[8]);const auto n=parse_num<std::size_t>(w[9],"fault payload count");t.fault_payload_passthrough=parse_num<int>(w[10],"passthrough")!=0;t.effect_token=parse_num<SirEffectTokenId>(w[11],"term effect");t.where.line=parse_num<std::uint32_t>(w[12],"line");t.where.column=parse_num<std::uint32_t>(w[13],"column");for(std::size_t i=0;i<n;++i){if(!r.next(w)||w.size()!=2||w[0]!="faultarg")r.error("expected faultarg");t.fault_payload.push_back(parse_num<SirValueId>(w[1],"faultarg"));}b.terminator=std::move(t);
                }
                else if(w[0]=="endblock")break;else r.error("unexpected record inside block: "+w[0]);
            }
            fn.blocks.push_back(std::move(b));
        }
        else if(w[0]=="endfunction")break;else r.error("unknown function record '"+w[0]+"'");
    }
    return fn;
}


std::uint32_t align_up_u32(std::uint32_t v, std::uint32_t a) {
    if (a <= 1) return v;
    return (v + a - 1u) & ~(a - 1u);
}

struct ConcreteShape {
    SirConcreteKind kind{SirConcreteKind::Scalar};
    std::uint32_t size{0};
    std::uint32_t align{1};
    std::uint32_t payload_offset{0};
    std::uint32_t element_size{0};
    std::uint32_t element_align{1};
    std::uint32_t capacity{0};
    std::vector<std::uint32_t> field_offsets;
};

ConcreteShape primitive_shape(SirType t) {
    ConcreteShape out;
    switch (t) {
        case SirType::Unit:
            out.kind = SirConcreteKind::Scalar; out.size = 1; out.align = 1; return out;
        case SirType::Bool:
            out.kind = SirConcreteKind::Scalar; out.size = 1; out.align = 1; return out;
        case SirType::I32:
        case SirType::U32:
            out.kind = SirConcreteKind::Scalar; out.size = 4; out.align = 4; return out;
        case SirType::I64:
        case SirType::U64:
            out.kind = SirConcreteKind::Scalar; out.size = 8; out.align = 8; return out;
        case SirType::Text:
            out.kind = SirConcreteKind::TextView; out.size = 16; out.align = 8; return out;
        case SirType::Nominal:
            break;
    }
    fail("target concretization: nominal type requires a schema layout");
}

ConcreteShape desc_shape(const SirTypeDesc& d,
                         std::uint32_t many_capacity,
                         const std::vector<SirDatasetSchema>& datasets,
                         const std::vector<SirChoiceSchema>& choices,
                         const std::vector<SirPoolSchema>& pools);

bool indirect_heap_resource(const SirTypeDesc& d, const std::vector<SirPoolSchema>& pools) {
    if (d.cardinality != SirCardinality::One || d.type != SirType::Nominal) return false;
    const auto* p = find_pool(pools, d.nominal);
    return p && p->allocation == "heap";
}

ConcreteShape member_shape(const SirTypeDesc& d,
                           const std::vector<SirDatasetSchema>& datasets,
                           const std::vector<SirChoiceSchema>& choices,
                           const std::vector<SirPoolSchema>& pools) {
    if (indirect_heap_resource(d,pools)) {
        ConcreteShape out; out.kind=SirConcreteKind::BorrowRef; out.size=8; out.align=8; return out;
    }
    return desc_shape(d,0,datasets,choices,pools);
}

ConcreteShape record_shape(const std::vector<SirFieldSchema>& fields,
                           const std::vector<SirDatasetSchema>& datasets,
                           const std::vector<SirChoiceSchema>& choices,
                           const std::vector<SirPoolSchema>& pools) {
    ConcreteShape out; out.kind = SirConcreteKind::Aggregate; out.align = 1; out.size = 0;
    for (const auto& f : fields) {
        auto sh = member_shape({f.type,f.cardinality,f.nominal_type}, datasets, choices, pools);
        out.align = std::max(out.align, sh.align);
        out.size = align_up_u32(out.size, sh.align);
        out.field_offsets.push_back(out.size);
        out.size += sh.size;
    }
    out.size = align_up_u32(std::max<std::uint32_t>(out.size,1), out.align);
    return out;
}

ConcreteShape nominal_shape(const std::string& name,
                            const std::vector<SirDatasetSchema>& datasets,
                            const std::vector<SirChoiceSchema>& choices,
                            const std::vector<SirPoolSchema>& pools) {
    if (const auto* d = find_dataset(datasets,name)) return record_shape(d->fields,datasets,choices,pools);
    if (const auto* p = find_pool(pools,name)) return record_shape(p->fields,datasets,choices,pools);
    if (const auto* c = find_choice(choices,name)) {
        std::uint32_t payload_size=0,payload_align=1;
        for (const auto& arm : c->cases) {
            auto sh=record_shape(arm.fields,datasets,choices,pools);
            payload_size=std::max(payload_size,sh.size); payload_align=std::max(payload_align,sh.align);
        }
        ConcreteShape out; out.kind=SirConcreteKind::Choice; out.align=std::max<std::uint32_t>(4,payload_align);
        out.payload_offset=align_up_u32(4,payload_align); out.size=align_up_u32(out.payload_offset+payload_size,out.align);
        out.field_offsets={0,out.payload_offset}; return out;
    }
    fail("target concretization: unknown nominal layout '"+name+"'");
}

ConcreteShape desc_shape(const SirTypeDesc& d,
                         std::uint32_t many_capacity,
                         const std::vector<SirDatasetSchema>& datasets,
                         const std::vector<SirChoiceSchema>& choices,
                         const std::vector<SirPoolSchema>& pools) {
    SirTypeDesc elem=d; elem.cardinality=SirCardinality::One;
    ConcreteShape base = d.type==SirType::Nominal ? nominal_shape(d.nominal,datasets,choices,pools) : primitive_shape(d.type);
    if (d.cardinality==SirCardinality::One) return base;
    if (indirect_heap_resource(elem,pools)) { base.kind=SirConcreteKind::BorrowRef; base.size=8; base.align=8; base.payload_offset=0; base.element_size=0; base.element_align=1; base.capacity=0; }
    if (d.cardinality==SirCardinality::ZeroOrOne) {
        ConcreteShape out; out.kind=SirConcreteKind::Optional; out.align=std::max<std::uint32_t>(1,base.align);
        out.payload_offset=align_up_u32(1,base.align); out.element_size=base.size; out.element_align=base.align;
        out.size=align_up_u32(out.payload_offset+base.size,out.align); return out;
    }
    ConcreteShape out; out.kind=SirConcreteKind::ManyInline; out.align=std::max<std::uint32_t>(8,base.align);
    out.payload_offset=align_up_u32(8,base.align); out.element_size=base.size; out.element_align=base.align; out.capacity=many_capacity;
    out.size=align_up_u32(out.payload_offset + many_capacity*base.size,out.align); return out;
}

const SirInstruction* find_def_op(const SirFunction& fn, SirValueId value) {
    for (const auto& b:fn.blocks) for (const auto& op:b.instructions) if(op.result==value) return &op;
    return nullptr;
}

std::uint32_t many_capacity_for_value(const SirFunction& fn, SirValueId value, std::map<SirValueId,std::uint32_t>& memo, std::set<SirValueId>& active) {
    if (auto it=memo.find(value);it!=memo.end()) return it->second;
    if (!active.insert(value).second) return 0;
    std::uint32_t cap=0;
    if (const auto* op=find_def_op(fn,value)) {
        if(op->opcode==SirOpcode::ManyMake) cap=static_cast<std::uint32_t>(op->operands.size());
        else if(op->opcode==SirOpcode::Phi) for(const auto& in:op->phi_inputs) cap=std::max(cap,many_capacity_for_value(fn,in.value,memo,active));
        else if(op->opcode==SirOpcode::RevisionMarker||op->opcode==SirOpcode::MoveValue||op->opcode==SirOpcode::SendValue||op->opcode==SirOpcode::Borrow||op->opcode==SirOpcode::BorrowMut||op->opcode==SirOpcode::SharedRetain) cap=many_capacity_for_value(fn,op->operands.at(0),memo,active);
    }
    active.erase(value); memo[value]=cap; return cap;
}

bool desc_is_register_scalar(const SirTypeDesc& d) {
    return d.cardinality==SirCardinality::One && d.type!=SirType::Nominal && d.type!=SirType::Text;
}

std::uint32_t fault_payload_bytes(const SirFaultIdentity& f,
                                  const std::vector<SirDatasetSchema>& datasets,
                                  const std::vector<SirChoiceSchema>& choices,
                                  const std::vector<SirPoolSchema>& pools,
                                  std::uint32_t* align_out=nullptr) {
    std::uint32_t size=0,align=1;
    for(const auto& fld:f.payload){auto sh=desc_shape({fld.type,fld.cardinality,fld.nominal_type},0,datasets,choices,pools);align=std::max(align,sh.align);size=align_up_u32(size,sh.align)+sh.size;}
    size=align_up_u32(std::max<std::uint32_t>(size,1),align); if(align_out)*align_out=align; return size;
}

void parse_top_schema(LineReader&r,std::vector<std::string>&w,std::vector<SirFaultIdentity>&faults,std::vector<SirDatasetSchema>&datasets,std::vector<SirChoiceSchema>&choices,std::vector<SirRelationSchema>&relations,std::vector<SirPoolSchema>&pools){
    if(w[0]=="fault"){if(w.size()!=4)r.error("malformed fault");SirFaultIdentity f;f.code=parse_num<std::int32_t>(w[1],"fault code");f.name=hex_decode(w[2]);const auto n=parse_num<std::size_t>(w[3],"payload count");for(std::size_t i=0;i<n;++i){if(!r.next(w))r.error("missing fault field");f.payload.push_back(parse_field(w,"ffield"));}faults.push_back(std::move(f));}
    else if(w[0]=="dataset"){if(w.size()!=5)r.error("malformed dataset");SirDatasetSchema d;d.name=hex_decode(w[1]);d.key=hex_decode(w[2]);const auto nf=parse_num<std::size_t>(w[3],"field count"),ni=parse_num<std::size_t>(w[4],"index count");for(std::size_t i=0;i<nf;++i){if(!r.next(w))r.error("missing dataset field");d.fields.push_back(parse_field(w,"dfield"));}for(std::size_t i=0;i<ni;++i){if(!r.next(w)||w.size()!=2||w[0]!="index")r.error("expected index");d.indexes.push_back(hex_decode(w[1]));}datasets.push_back(std::move(d));}
    else if(w[0]=="choice"){if(w.size()!=3)r.error("malformed choice");SirChoiceSchema c;c.name=hex_decode(w[1]);const auto nc=parse_num<std::size_t>(w[2],"case count");for(std::size_t i=0;i<nc;++i){if(!r.next(w)||w.size()!=4||w[0]!="case")r.error("expected case");SirChoiceCaseSchema a;a.tag=parse_num<std::uint32_t>(w[1],"tag");a.name=hex_decode(w[2]);const auto nf=parse_num<std::size_t>(w[3],"case field count");for(std::size_t j=0;j<nf;++j){if(!r.next(w))r.error("missing choice field");a.fields.push_back(parse_field(w,"cfield"));}c.cases.push_back(std::move(a));}choices.push_back(std::move(c));}
    else if(w[0]=="relation"){if(w.size()!=9)r.error("malformed relation");relations.push_back({hex_decode(w[1]),hex_decode(w[2]),hex_decode(w[3]),hex_decode(w[4]),hex_decode(w[5]),hex_decode(w[6]),parse_num<int>(w[7],"unique")!=0,parse_num<int>(w[8],"reverse")!=0});}
    else if(w[0]=="pool"){if(w.size()!=6&&w.size()!=7)r.error("malformed pool");SirPoolSchema p;p.name=hex_decode(w[1]);p.lifetime=hex_decode(w[2]);p.allocation=hex_decode(w[3]);p.reclaim=hex_decode(w[4]);std::size_t count=5;if(w.size()==7){p.ownership=hex_decode(w[5]);count=6;}const auto n=parse_num<std::size_t>(w[count],"pool field count");for(std::size_t i=0;i<n;++i){if(!r.next(w))r.error("missing pool field");p.fields.push_back(parse_field(w,"pfield"));}pools.push_back(std::move(p));}
    else r.error("not a schema record");
}

} // namespace


std::string sir_type_name(SirType t){switch(t){case SirType::Unit:return"unit";case SirType::Bool:return"bool";case SirType::I32:return"i32";case SirType::I64:return"i64";case SirType::U32:return"u32";case SirType::U64:return"u64";case SirType::Text:return"text";case SirType::Nominal:return"nominal";}return"?";}
SirType sir_type_from_name(const std::string&n){if(n=="unit")return SirType::Unit;if(n=="bool")return SirType::Bool;if(n=="i32")return SirType::I32;if(n=="i64")return SirType::I64;if(n=="u32")return SirType::U32;if(n=="u64")return SirType::U64;if(n=="text")return SirType::Text;if(n=="nominal")return SirType::Nominal;fail("SIR deserialize: unknown type '"+n+"'");}
std::string sir_cardinality_name(SirCardinality c){switch(c){case SirCardinality::One:return"One";case SirCardinality::ZeroOrOne:return"ZeroOrOne";case SirCardinality::Many:return"Many";}return"?";}
SirCardinality sir_cardinality_from_name(const std::string&n){if(n=="One")return SirCardinality::One;if(n=="ZeroOrOne")return SirCardinality::ZeroOrOne;if(n=="Many")return SirCardinality::Many;fail("SIR deserialize: unknown cardinality '"+n+"'");}
std::string sir_storage_class_name(SirStorageClass s){switch(s){case SirStorageClass::Register:return"register";case SirStorageClass::Stack:return"stack";case SirStorageClass::Caller:return"caller";case SirStorageClass::PoolArena:return"pool-arena";case SirStorageClass::Heap:return"heap";case SirStorageClass::Static:return"static";case SirStorageClass::Alias:return"alias";}return"?";}
SirStorageClass sir_storage_class_from_name(const std::string&n){if(n=="register")return SirStorageClass::Register;if(n=="stack")return SirStorageClass::Stack;if(n=="caller")return SirStorageClass::Caller;if(n=="pool-arena")return SirStorageClass::PoolArena;if(n=="heap")return SirStorageClass::Heap;if(n=="static")return SirStorageClass::Static;if(n=="alias")return SirStorageClass::Alias;fail("SIR deserialize: unknown storage class '"+n+"'");}
std::string sir_concrete_kind_name(SirConcreteKind k){switch(k){case SirConcreteKind::Scalar:return"scalar";case SirConcreteKind::TextView:return"text-view";case SirConcreteKind::Optional:return"optional";case SirConcreteKind::ManyInline:return"many-inline";case SirConcreteKind::ManyDynamic:return"many-dynamic";case SirConcreteKind::Aggregate:return"aggregate";case SirConcreteKind::Choice:return"choice";case SirConcreteKind::BorrowRef:return"borrow-ref";case SirConcreteKind::Opaque:return"opaque";}return"?";}
SirConcreteKind sir_concrete_kind_from_name(const std::string&n){if(n=="scalar")return SirConcreteKind::Scalar;if(n=="text-view")return SirConcreteKind::TextView;if(n=="optional")return SirConcreteKind::Optional;if(n=="many-inline")return SirConcreteKind::ManyInline;if(n=="many-dynamic")return SirConcreteKind::ManyDynamic;if(n=="aggregate")return SirConcreteKind::Aggregate;if(n=="choice")return SirConcreteKind::Choice;if(n=="borrow-ref")return SirConcreteKind::BorrowRef;if(n=="opaque")return SirConcreteKind::Opaque;fail("SIR deserialize: unknown concrete kind '"+n+"'");}
std::string sir_cleanup_kind_name(SirCleanupKind k){switch(k){case SirCleanupKind::HeapFree:return"heap-free";case SirCleanupKind::ManyBufferFree:return"many-buffer-free";case SirCleanupKind::LifetimeEnd:return"lifetime-end";case SirCleanupKind::SharedRelease:return"shared-release";case SirCleanupKind::CompositeRelease:return"composite-release";}return"?";}
SirCleanupKind sir_cleanup_kind_from_name(const std::string&n){if(n=="heap-free")return SirCleanupKind::HeapFree;if(n=="many-buffer-free")return SirCleanupKind::ManyBufferFree;if(n=="lifetime-end")return SirCleanupKind::LifetimeEnd;if(n=="shared-release")return SirCleanupKind::SharedRelease;if(n=="composite-release")return SirCleanupKind::CompositeRelease;fail("SIR deserialize: unknown cleanup kind '"+n+"'");}
std::string sir_result_transfer_kind_name(SirResultTransferKind k){switch(k){case SirResultTransferKind::None:return"none";case SirResultTransferKind::HeapObject:return"heap-object";case SirResultTransferKind::DynamicMany:return"dynamic-many";case SirResultTransferKind::BorrowRef:return"borrow-ref";case SirResultTransferKind::SharedRef:return"shared-ref";}return"?";}
SirResultTransferKind sir_result_transfer_kind_from_name(const std::string&n){if(n=="none")return SirResultTransferKind::None;if(n=="heap-object")return SirResultTransferKind::HeapObject;if(n=="dynamic-many")return SirResultTransferKind::DynamicMany;if(n=="borrow-ref")return SirResultTransferKind::BorrowRef;if(n=="shared-ref")return SirResultTransferKind::SharedRef;fail("SIR deserialize: unknown result transfer kind '"+n+"'");}
std::string sir_undo_kind_name(SirUndoKind k){switch(k){case SirUndoKind::RestoreField:return"restore-field";case SirUndoKind::RelationUnlink:return"relation-unlink";case SirUndoKind::RelationLink:return"relation-link";case SirUndoKind::RestoreOwner:return"restore-owner";case SirUndoKind::DestroyResource:return"destroy-resource";case SirUndoKind::SharedRelease:return"shared-release";}return"?";}
SirUndoKind sir_undo_kind_from_name(const std::string&n){if(n=="restore-field")return SirUndoKind::RestoreField;if(n=="relation-unlink")return SirUndoKind::RelationUnlink;if(n=="relation-link")return SirUndoKind::RelationLink;if(n=="restore-owner")return SirUndoKind::RestoreOwner;if(n=="destroy-resource")return SirUndoKind::DestroyResource;if(n=="shared-release")return SirUndoKind::SharedRelease;fail("SIR deserialize: unknown undo kind '"+n+"'");}
std::string sir_component_guard_name(SirComponentGuard g){switch(g){case SirComponentGuard::Always:return"always";case SirComponentGuard::OptionalPresent:return"optional-present";case SirComponentGuard::ChoiceTag:return"choice-tag";case SirComponentGuard::ManyElements:return"many-elements";}return"?";}
SirComponentGuard sir_component_guard_from_name(const std::string&n){if(n=="always")return SirComponentGuard::Always;if(n=="optional-present")return SirComponentGuard::OptionalPresent;if(n=="choice-tag")return SirComponentGuard::ChoiceTag;if(n=="many-elements")return SirComponentGuard::ManyElements;fail("SIR deserialize: unknown component guard '"+n+"'");}
bool sir_is_integer(SirType t){return t==SirType::I32||t==SirType::I64||t==SirType::U32||t==SirType::U64;}
bool sir_is_signed_integer(SirType t){return t==SirType::I32||t==SirType::I64;}
unsigned sir_integer_bits(SirType t){return(t==SirType::I32||t==SirType::U32)?32u:(t==SirType::I64||t==SirType::U64)?64u:0u;}

std::string sir_opcode_name(SirOpcode op){
    switch(op){
        case SirOpcode::Phi:return"phi";case SirOpcode::ConstUnit:return"const.unit";case SirOpcode::ConstInt:return"const.int";case SirOpcode::ConstBool:return"const.bool";case SirOpcode::ConstText:return"const.text";
        case SirOpcode::OptionalSome:return"optional.some";case SirOpcode::OptionalNone:return"optional.none";case SirOpcode::ManyMake:return"many.make";case SirOpcode::AggregateMake:return"aggregate.make";case SirOpcode::ChoiceMake:return"choice.make";case SirOpcode::PoolAlloc:return"pool.alloc";case SirOpcode::FieldLoad:return"field.load";case SirOpcode::FieldAddress:return"field.address";case SirOpcode::FieldStore:return"field.store";case SirOpcode::FieldMove:return"field.move";case SirOpcode::ManyDynamic:return"many.dynamic";case SirOpcode::ManyPush:return"many.push";case SirOpcode::ManyLength:return"many.length";case SirOpcode::Borrow:return"borrow";case SirOpcode::BorrowMut:return"borrow.mut";case SirOpcode::MoveValue:return"move";case SirOpcode::ResourceRelease:return"resource.release";case SirOpcode::SharedRetain:return"shared.retain";case SirOpcode::SendValue:return"send";
        case SirOpcode::Not:return"not";case SirOpcode::CheckedNeg:return"neg.checked";case SirOpcode::CheckedAdd:return"add.checked";case SirOpcode::CheckedSub:return"sub.checked";case SirOpcode::CheckedMul:return"mul.checked";case SirOpcode::CheckedDiv:return"div.checked";case SirOpcode::CheckedMod:return"mod.checked";
        case SirOpcode::CompareEq:return"cmp.eq";case SirOpcode::CompareNe:return"cmp.ne";case SirOpcode::CompareLt:return"cmp.lt";case SirOpcode::CompareLe:return"cmp.le";case SirOpcode::CompareGt:return"cmp.gt";case SirOpcode::CompareGe:return"cmp.ge";
        case SirOpcode::Call:return"call";case SirOpcode::WriteText:return"io.stdout.write_text";case SirOpcode::RelationLink:return"relation.link";case SirOpcode::RelationUnlink:return"relation.unlink";
        case SirOpcode::TransactionBegin:return"transaction.begin";case SirOpcode::TransactionCommit:return"transaction.commit";case SirOpcode::TransactionAbort:return"transaction.abort";case SirOpcode::ParallelBegin:return"parallel.begin";case SirOpcode::ParallelEnd:return"parallel.end";case SirOpcode::TaskBegin:return"task.begin";case SirOpcode::TaskEnd:return"task.end";case SirOpcode::RevisionMarker:return"revision.marker";
    }return"?";
}
SirOpcode sir_opcode_from_name(const std::string&n){
    static const std::map<std::string,SirOpcode>t{{"phi",SirOpcode::Phi},{"const.unit",SirOpcode::ConstUnit},{"const.int",SirOpcode::ConstInt},{"const.bool",SirOpcode::ConstBool},{"const.text",SirOpcode::ConstText},{"optional.some",SirOpcode::OptionalSome},{"optional.none",SirOpcode::OptionalNone},{"many.make",SirOpcode::ManyMake},{"aggregate.make",SirOpcode::AggregateMake},{"choice.make",SirOpcode::ChoiceMake},{"pool.alloc",SirOpcode::PoolAlloc},{"field.load",SirOpcode::FieldLoad},{"field.address",SirOpcode::FieldAddress},{"field.store",SirOpcode::FieldStore},{"field.move",SirOpcode::FieldMove},{"many.dynamic",SirOpcode::ManyDynamic},{"many.push",SirOpcode::ManyPush},{"many.length",SirOpcode::ManyLength},{"borrow",SirOpcode::Borrow},{"borrow.mut",SirOpcode::BorrowMut},{"move",SirOpcode::MoveValue},{"resource.release",SirOpcode::ResourceRelease},{"shared.retain",SirOpcode::SharedRetain},{"send",SirOpcode::SendValue},{"not",SirOpcode::Not},{"neg.checked",SirOpcode::CheckedNeg},{"add.checked",SirOpcode::CheckedAdd},{"sub.checked",SirOpcode::CheckedSub},{"mul.checked",SirOpcode::CheckedMul},{"div.checked",SirOpcode::CheckedDiv},{"mod.checked",SirOpcode::CheckedMod},{"cmp.eq",SirOpcode::CompareEq},{"cmp.ne",SirOpcode::CompareNe},{"cmp.lt",SirOpcode::CompareLt},{"cmp.le",SirOpcode::CompareLe},{"cmp.gt",SirOpcode::CompareGt},{"cmp.ge",SirOpcode::CompareGe},{"call",SirOpcode::Call},{"io.stdout.write_text",SirOpcode::WriteText},{"relation.link",SirOpcode::RelationLink},{"relation.unlink",SirOpcode::RelationUnlink},{"transaction.begin",SirOpcode::TransactionBegin},{"transaction.commit",SirOpcode::TransactionCommit},{"transaction.abort",SirOpcode::TransactionAbort},{"parallel.begin",SirOpcode::ParallelBegin},{"parallel.end",SirOpcode::ParallelEnd},{"task.begin",SirOpcode::TaskBegin},{"task.end",SirOpcode::TaskEnd},{"revision.marker",SirOpcode::RevisionMarker}};
    auto it=t.find(n);if(it==t.end())fail("SIR deserialize: unknown opcode '"+n+"'");return it->second;
}
std::string sir_terminator_name(SirTerminatorKind k){switch(k){case SirTerminatorKind::Br:return"br";case SirTerminatorKind::CondBr:return"condbr";case SirTerminatorKind::Return:return"return";case SirTerminatorKind::FaultReturn:return"fault.return";case SirTerminatorKind::Unreachable:return"unreachable";}return"?";}
SirTerminatorKind sir_terminator_from_name(const std::string&n){if(n=="br")return SirTerminatorKind::Br;if(n=="condbr")return SirTerminatorKind::CondBr;if(n=="return")return SirTerminatorKind::Return;if(n=="fault.return")return SirTerminatorKind::FaultReturn;if(n=="unreachable")return SirTerminatorKind::Unreachable;fail("SIR deserialize: unknown terminator '"+n+"'");}

SirSProgram DeductiveLowerer::lower(const Program&p,const SSLProgram&ssl)const{
    SirSProgram out;out.module_name=ssl.module_name;
    for(const auto&[name,ff]:ssl.fault_facts){SirFaultIdentity f;f.name=name;f.code=ff.code;for(const auto&x:ff.payload){auto d=to_desc(x.type);f.payload.push_back({x.name,d.type,d.cardinality,d.nominal,x.key,x.revisable});}out.faults.push_back(std::move(f));}
    std::sort(out.faults.begin(),out.faults.end(),[](const auto&a,const auto&b){return a.code==b.code?a.name<b.name:a.code<b.code;});
    for(const auto&[name,d]:ssl.datasets){SirDatasetSchema s;s.name=name;s.key=d.key.value_or("");s.indexes=d.indexes;for(const auto&f:d.fields){auto x=to_desc(f.type);s.fields.push_back({f.name,x.type,x.cardinality,x.nominal,f.key,f.revisable});}out.datasets.push_back(std::move(s));}
    for(const auto&[name,c]:ssl.choices){SirChoiceSchema s;s.name=name;std::uint32_t tag=0;for(const auto&a:c.cases){SirChoiceCaseSchema ca;ca.name=a.name;ca.tag=tag++;for(const auto&f:a.fields){auto x=to_desc(f.type);ca.fields.push_back({f.name,x.type,x.cardinality,x.nominal,f.key,f.revisable});}s.cases.push_back(std::move(ca));}out.choices.push_back(std::move(s));}
    for(const auto&[name,r]:ssl.relations)out.relations.push_back({name,r.source,r.target,r.source_cardinality,r.target_cardinality,r.ownership,r.unique_pair,r.reverse_index});
    for(const auto&[name,pf]:ssl.pools){SirPoolSchema s;s.name=name;s.lifetime=pf.lifetime.value_or("unspecified");s.allocation=pf.allocation.value_or("free");s.reclaim=pf.reclaim.value_or("policy");s.ownership=pf.ownership;for(const auto&f:pf.fields){auto x=to_desc(f.type);s.fields.push_back({f.name,x.type,x.cardinality,x.nominal,f.key,f.revisable});}out.pools.push_back(std::move(s));}
    auto byname=[](const auto&a,const auto&b){return a.name<b.name;};std::sort(out.datasets.begin(),out.datasets.end(),byname);std::sort(out.choices.begin(),out.choices.end(),byname);std::sort(out.relations.begin(),out.relations.end(),byname);std::sort(out.pools.begin(),out.pools.end(),byname);
    out.semantic_facts=ssl.facts_text;std::sort(out.semantic_facts.begin(),out.semantic_facts.end());
    for(const auto&fn:p.instructions){auto it=ssl.instructions.find(fn.name);if(it==ssl.instructions.end())throw CompileError(fn.where,"DLE: instruction lacks SSL facts");out.functions.push_back(FunctionLowerer(p,ssl,fn,it->second).run());}
    std::sort(out.functions.begin(),out.functions.end(),[](const auto&a,const auto&b){return a.name<b.name;});

    // Infer whole-program ownership-transfer contracts before target lowering.  A
    // resource may cross an instruction boundary only through an explicit move.
    // This is a semantic SIR-S fact; SIR-C later chooses the transport ABI.
    std::map<std::string,SirFunction*> fmap;for(auto&fn:out.functions)fmap[fn.name]=&fn;
    auto op_for=[&](const SirFunction&fn,SirValueId id)->const SirInstruction*{for(const auto&b:fn.blocks)for(const auto&op:b.instructions)if(op.result==id)return &op;return nullptr;};
    std::function<std::string(const SirFunction&,SirValueId,std::set<SirValueId>&)> resource_of;
    resource_of=[&](const SirFunction&fn,SirValueId id,std::set<SirValueId>&seen)->std::string{
        if(!seen.insert(id).second)return {};
        const auto*op=op_for(fn,id);if(!op)return {};
        if(op->opcode==SirOpcode::PoolAlloc){const auto*ps=find_pool(out.pools,op->semantic_name);if(ps&&ps->allocation=="heap")return (ps->ownership=="shared"?"shared-pool:":"heap-pool:")+op->semantic_name;return {};}
        if(op->opcode==SirOpcode::ManyDynamic)return "dynamic-many";
        if((op->opcode==SirOpcode::MoveValue||op->opcode==SirOpcode::SendValue||op->opcode==SirOpcode::SharedRetain||op->opcode==SirOpcode::RevisionMarker)&&!op->operands.empty())return resource_of(fn,op->operands[0],seen);
        if(op->opcode==SirOpcode::Call){auto it=fmap.find(op->callee);return it==fmap.end()?std::string{}:it->second->result_resource;}
        if(op->opcode==SirOpcode::Phi){std::string k;for(const auto&i:op->phi_inputs){auto local=seen;auto x=resource_of(fn,i.value,local);if(x.empty())return {};if(k.empty())k=x;else if(k!=x)throw CompileError({},"DLE: phi merges incompatible owned resource families");}return k;}
        return {};
    };
    std::function<bool(const SirFunction&,SirValueId,std::set<SirValueId>&)> explicit_transfer;
    explicit_transfer=[&](const SirFunction&fn,SirValueId id,std::set<SirValueId>&seen){
        if(!seen.insert(id).second)return false;
        const auto*op=op_for(fn,id);if(!op)return false;
        if(op->opcode==SirOpcode::MoveValue||op->opcode==SirOpcode::SendValue)return true;
        if(op->opcode==SirOpcode::Phi){if(op->phi_inputs.empty())return false;for(const auto&i:op->phi_inputs){auto local=seen;if(!explicit_transfer(fn,i.value,local))return false;}return true;}
        return false;
    };
    bool summary_changed=true;
    while(summary_changed){summary_changed=false;for(auto&fn:out.functions){
        if(fn.result_borrow_mode!="none"){
            auto pit=std::find_if(fn.parameters.begin(),fn.parameters.end(),[&](const auto&p){return p.name==fn.result_borrow_from;});
            if(pit==fn.parameters.end()) throw CompileError({},"DLE: result borrow source parameter is missing");
            const auto& pvs=find_value_sem(fn,pit->value);
            const std::string ownership="borrowed";
            const std::string result_resource=pvs.resource_family;
            if(fn.result_ownership!=ownership||fn.result_resource!=result_resource){fn.result_ownership=ownership;fn.result_resource=result_resource;summary_changed=true;}
            continue;
        }
        std::string result_resource;bool saw_resource=false;
        for(const auto&b:fn.blocks)if(b.terminator.kind==SirTerminatorKind::Return&&b.terminator.value){
            const auto& rv=find_value_sem(fn,*b.terminator.value);
            if(rv.ownership=="shared"){if(fn.result_ownership!="shared-transfer"||fn.result_resource!=rv.resource_family){fn.result_ownership="shared-transfer";fn.result_resource=rv.resource_family;summary_changed=true;}saw_resource=true;result_resource=rv.resource_family;continue;}
            std::set<SirValueId> seen;auto r=resource_of(fn,*b.terminator.value,seen);if(r.empty())continue;saw_resource=true;if(result_resource.empty())result_resource=r;else if(result_resource!=r)throw CompileError({},"DLE: function returns incompatible owned resource families");std::set<SirValueId> moved;if(!explicit_transfer(fn,*b.terminator.value,moved))throw CompileError({},"DLE: owned resource return requires explicit move(...) at the instruction boundary");}
        if(fn.result_ownership=="shared-transfer") continue;
        const auto ownership=saw_resource?std::string("owned-transfer"):std::string("value");
        if(fn.result_ownership!=ownership||fn.result_resource!=result_resource){fn.result_ownership=ownership;fn.result_resource=result_resource;summary_changed=true;}
    }}

    // Give every resource-returning call a fresh caller-local provenance.  The
    // resource identity is transferred semantically; the callee's provenance ID
    // itself never leaks into the caller's function-local namespace.
    for(auto&fn:out.functions){SirProvenanceId next=1;for(const auto&pr:fn.provenances)next=std::max(next,pr.id+1);
        auto value_mut=[&](SirValueId id)->SirValueSemantics&{auto it=std::find_if(fn.values.begin(),fn.values.end(),[&](const auto&v){return v.value==id;});if(it==fn.values.end())throw CompileError({},"DLE internal: missing mutable value semantics");return *it;};
        for(auto&b:fn.blocks)for(auto&op:b.instructions)if(op.opcode==SirOpcode::Call){auto ci=fmap.find(op.callee);if(ci!=fmap.end()&&(ci->second->result_ownership=="owned-transfer"||ci->second->result_ownership=="shared-transfer")){auto&v=value_mut(op.result);if(!v.provenance){const auto prov=next++;SirProvenance pr;pr.id=prov;pr.region=op.region;pr.origin="call.return."+op.callee;pr.ownership=ci->second->result_ownership=="shared-transfer"?"shared":"owned";pr.resource_family=ci->second->result_resource;const auto caller_ownership = pr.ownership;fn.provenances.push_back(std::move(pr));op.provenance=prov;v.provenance=prov;v.ownership=caller_ownership;v.resource_family=ci->second->result_resource;v.authorities=ci->second->result_ownership=="shared-transfer"?std::vector<std::string>{"read","share","send"}:std::vector<std::string>{"move","read","revise","send"};}}}
        bool aliases=true;while(aliases){aliases=false;for(auto&b:fn.blocks)for(auto&op:b.instructions){
            if(op.operands.empty())continue;
            if(op.opcode==SirOpcode::MoveValue||op.opcode==SirOpcode::SendValue||op.opcode==SirOpcode::Borrow||op.opcode==SirOpcode::BorrowMut||op.opcode==SirOpcode::RevisionMarker){auto&dst=value_mut(op.result);const auto&src=find_value_sem(fn,op.operands[0]);
                if((op.opcode==SirOpcode::MoveValue||op.opcode==SirOpcode::SendValue||op.opcode==SirOpcode::RevisionMarker)&&src.provenance&&dst.provenance!=src.provenance){dst.provenance=src.provenance;op.provenance=src.provenance;aliases=true;}
                if(op.opcode==SirOpcode::MoveValue)dst.ownership="moved";else if(op.opcode==SirOpcode::SendValue)dst.ownership="sent";else if(op.opcode==SirOpcode::Borrow)dst.ownership="borrow-shared";else if(op.opcode==SirOpcode::BorrowMut)dst.ownership="borrow-mut";else dst.ownership=src.ownership;}
            else if(op.opcode==SirOpcode::FieldLoad||op.opcode==SirOpcode::FieldAddress||op.opcode==SirOpcode::FieldMove){const auto&src=find_value_sem(fn,op.operands[0]);auto&dst=value_mut(op.result);if(src.provenance&&dst.provenance){auto pi=std::find_if(fn.provenances.begin(),fn.provenances.end(),[&](const auto&pr){return pr.id==dst.provenance;});if(pi!=fn.provenances.end()&&pi->parent==0){pi->parent=src.provenance;aliases=true;}}}
        }}
        {std::vector<SirProvenance> ordered;std::set<SirProvenanceId> emitted;while(ordered.size()<fn.provenances.size()){bool progress=false;for(const auto&pr:fn.provenances){if(emitted.contains(pr.id))continue;if(!pr.parent||emitted.contains(pr.parent)){ordered.push_back(pr);emitted.insert(pr.id);progress=true;}}if(!progress)throw CompileError({},"DLE: cyclic provenance graph after ownership-transfer stitching");}fn.provenances=std::move(ordered);}
    }
    out.format_minor=1;
    return out;
}

SirCProgram TargetConcretizer::concretize(const SirSProgram&s,std::string profile,std::string triple)const{
    if(profile.empty()||triple.empty())throw CompileError({},"target concretization: target profile/triple must be explicit");
    if(triple!="x86_64-pc-windows-msvc"||profile!="windows-x86_64-bootstrap")throw CompileError({},"target concretization: Compiler 0.8 supports windows-x86_64-bootstrap / x86_64-pc-windows-msvc");

    SirVerifier{}.verify(s);
    SirCProgram o; o.format_major=6; o.format_minor=0; o.module_name=s.module_name;
    o.target_profile=std::move(profile); o.target_triple=std::move(triple); o.pointer_bits=64;
    o.object_format="COFF-x86-64"; o.executable_format="PE32+";
    o.calling_convention="scalar={status:i32,payload:u64}+hidden-fault-out; rich=status:i32+caller-result+hidden-fault-out; Microsoft-x64 external";
    o.aggregate_representation="byte-layout-v3: explicit size/alignment/field offsets; addressable fields use target byte offsets; aggregates may be scalar-replaced only when no material address/escape is required";
    o.cardinality_representation="One=register-or-address; ZeroOrOne={present:u8,payload}; ManyInline={length:u64,inline-elements[N]}; ManyDynamic={data:ptr,length:u64,capacity:u64}";
    o.fault_payload_representation="caller-owned byte buffer; callee writes typed payload before nonzero status; cleanup edges preserve payload/status while discharging owned resources";
    o.faults=s.faults;o.datasets=s.datasets;o.choices=s.choices;o.relations=s.relations;o.pools=s.pools;o.semantic_facts=s.semantic_facts;o.functions=s.functions;

    for(const auto&d:s.datasets){auto sh=nominal_shape(d.name,s.datasets,s.choices,s.pools);o.concrete_layouts.push_back({d.name,sh.kind,sh.size,sh.align,sh.payload_offset,sh.element_size,sh.element_align,sh.capacity,sh.field_offsets});}
    for(const auto&c:s.choices){auto sh=nominal_shape(c.name,s.datasets,s.choices,s.pools);o.concrete_layouts.push_back({c.name,sh.kind,sh.size,sh.align,sh.payload_offset,sh.element_size,sh.element_align,sh.capacity,sh.field_offsets});}
    for(const auto&p:s.pools){auto sh=nominal_shape(p.name,s.datasets,s.choices,s.pools);o.concrete_layouts.push_back({p.name,sh.kind,sh.size,sh.align,sh.payload_offset,sh.element_size,sh.element_align,sh.capacity,sh.field_offsets});}
    std::sort(o.concrete_layouts.begin(),o.concrete_layouts.end(),[](const auto&a,const auto&b){return a.name<b.name;});

    auto find_layout=[&](const std::string& name)->const SirConcreteLayout*{
        auto it=std::find_if(o.concrete_layouts.begin(),o.concrete_layouts.end(),[&](const auto& l){return l.name==name;});
        return it==o.concrete_layouts.end()?nullptr:&*it;
    };
    auto split_field=[](const std::string& n)->std::pair<std::string,std::string>{
        auto pos=n.find('.'); if(pos==std::string::npos)return {n,{}}; return {n.substr(0,pos),n.substr(pos+1)};
    };
    auto region_inside=[](const SirFunction& fn,SirRegionId child,SirRegionId ancestor){
        if(!ancestor)return true;
        std::set<SirRegionId> seen;
        while(child&&seen.insert(child).second){
            if(child==ancestor)return true;
            auto it=std::find_if(fn.regions.begin(),fn.regions.end(),[&](const auto&r){return r.id==child;});
            if(it==fn.regions.end()) break;
            child=it->parent;
        }
        return false;
    };
    auto block_by_id=[](const SirFunction& fn,SirBlockId id)->const SirBlock*{
        auto it=std::find_if(fn.blocks.begin(),fn.blocks.end(),[&](const auto&b){return b.id==id;});return it==fn.blocks.end()?nullptr:&*it;
    };
    auto successors=[](const SirBlock& b){
        std::vector<SirBlockId> v;
        if(b.terminator.kind==SirTerminatorKind::Br)v.push_back(b.terminator.target);
        else if(b.terminator.kind==SirTerminatorKind::CondBr){v.push_back(b.terminator.true_target);v.push_back(b.terminator.false_target);}
        for(const auto& op:b.instructions)for(const auto& fe:op.fault_edges)v.push_back(fe.target);
        std::sort(v.begin(),v.end());v.erase(std::unique(v.begin(),v.end()),v.end());return v;
    };
    auto block_in_cycle=[&](const SirFunction& fn,SirBlockId start){
        std::set<SirBlockId> visiting;
        std::function<bool(SirBlockId)> reach=[&](SirBlockId at){
            const auto* b=block_by_id(fn,at); if(!b)return false;
            for(auto nx:successors(*b)){
                if(nx==start)return true;
                if(visiting.insert(nx).second&&reach(nx))return true;
            }
            return false;
        };
        visiting.insert(start); return reach(start);
    };

    std::uint32_t global_fault_size=1,global_fault_align=1;
    for(const auto&f:s.faults){std::uint32_t a=1;auto z=fault_payload_bytes(f,s.datasets,s.choices,s.pools,&a);global_fault_size=std::max(global_fault_size,z);global_fault_align=std::max(global_fault_align,a);}
    global_fault_size=align_up_u32(global_fault_size,global_fault_align);
    std::map<std::string,const SirFunction*> semantic_functions;for(const auto&f:s.functions)semantic_functions[f.name]=&f;

    for(const auto&fn:s.functions){
        SirConcreteFunction cf; cf.name=fn.name; cf.fault_payload_size=global_fault_size; cf.fault_payload_align=global_fault_align;
        std::map<SirValueId,std::vector<SirOpcode>> users;
        std::set<SirValueId> escapes;
        std::map<SirValueId,const SirInstruction*> defs;
        std::map<SirValueId,SirBlockId> defblocks;
        for(const auto&b:fn.blocks){
            for(const auto&op:b.instructions){
                defs[op.result]=&op; defblocks[op.result]=b.id;
                for(auto v:op.operands)users[v].push_back(op.opcode);
                for(const auto&pi:op.phi_inputs)users[pi.value].push_back(SirOpcode::Phi);
                if(op.opcode==SirOpcode::Call) for(auto v:op.operands) escapes.insert(v);
                if(op.opcode==SirOpcode::FieldAddress&& !op.operands.empty()) escapes.insert(op.operands[0]);
            }
            if(b.terminator.value)escapes.insert(*b.terminator.value);
            for(auto v:b.terminator.fault_payload)escapes.insert(v);
        }
        bool changed=true;
        while(changed){changed=false;for(const auto&b:fn.blocks)for(const auto&op:b.instructions)if(escapes.contains(op.result)){
            if(op.opcode==SirOpcode::Borrow||op.opcode==SirOpcode::BorrowMut||op.opcode==SirOpcode::MoveValue||op.opcode==SirOpcode::SendValue||op.opcode==SirOpcode::SharedRetain||op.opcode==SirOpcode::RevisionMarker||op.opcode==SirOpcode::FieldAddress||op.opcode==SirOpcode::FieldLoad||op.opcode==SirOpcode::FieldMove){if(!op.operands.empty()&&escapes.insert(op.operands.at(0)).second)changed=true;}
            if(op.opcode==SirOpcode::Phi)for(const auto&i:op.phi_inputs)if(escapes.insert(i.value).second)changed=true;
        }}

        std::map<SirValueId,std::uint32_t> capmemo; std::set<SirValueId> active;
        std::map<SirValueId,std::uint32_t> promoted_caps;
        for(const auto&vs:fn.values) if(vs.cardinality==SirCardinality::Many){active.clear();promoted_caps[vs.value]=many_capacity_for_value(fn,vs.value,capmemo,active);}
        bool cap_changed=true;
        while(cap_changed){cap_changed=false;for(const auto&b:fn.blocks)for(const auto&op:b.instructions)if(op.opcode==SirOpcode::Phi&&op.cardinality==SirCardinality::Many){std::uint32_t m=promoted_caps[op.result];for(const auto&i:op.phi_inputs)m=std::max(m,promoted_caps[i.value]);if(promoted_caps[op.result]!=m){promoted_caps[op.result]=m;cap_changed=true;}for(const auto&i:op.phi_inputs)if(promoted_caps[i.value]!=m){promoted_caps[i.value]=m;cap_changed=true;}}}

        std::set<SirValueId> dynamic_many;
        for(const auto&b:fn.blocks)for(const auto&op:b.instructions){
            if(op.opcode==SirOpcode::ManyDynamic)dynamic_many.insert(op.result);
            if(op.opcode==SirOpcode::Call){auto ci=semantic_functions.find(op.callee);if(ci!=semantic_functions.end()&&ci->second->result_resource=="dynamic-many")dynamic_many.insert(op.result);}
        }
        bool dm_changed=true;
        while(dm_changed){dm_changed=false;for(const auto&b:fn.blocks)for(const auto&op:b.instructions){
            if(op.opcode==SirOpcode::Phi&&op.cardinality==SirCardinality::Many){bool any=dynamic_many.contains(op.result);for(const auto&i:op.phi_inputs)any=any||dynamic_many.contains(i.value);if(any){if(dynamic_many.insert(op.result).second)dm_changed=true;for(const auto&i:op.phi_inputs)if(dynamic_many.insert(i.value).second)dm_changed=true;}}
            if((op.opcode==SirOpcode::MoveValue||op.opcode==SirOpcode::SendValue||op.opcode==SirOpcode::Borrow||op.opcode==SirOpcode::BorrowMut||op.opcode==SirOpcode::SharedRetain||op.opcode==SirOpcode::RevisionMarker)&&op.cardinality==SirCardinality::Many&&!op.operands.empty()&&dynamic_many.contains(op.operands[0]))if(dynamic_many.insert(op.result).second)dm_changed=true;
        }}

        std::uint32_t result_cap=0;
        bool result_dynamic=false;
        if(fn.result_cardinality==SirCardinality::Many) for(const auto&b:fn.blocks) if(b.terminator.kind==SirTerminatorKind::Return&&b.terminator.value){result_cap=std::max(result_cap,promoted_caps[*b.terminator.value]);result_dynamic=result_dynamic||dynamic_many.contains(*b.terminator.value);}
        auto result_shape=desc_shape({fn.result_type,fn.result_cardinality,fn.result_nominal_type},result_cap,s.datasets,s.choices,s.pools);
        if(result_dynamic){result_shape.kind=SirConcreteKind::ManyDynamic;result_shape.size=24;result_shape.align=8;result_shape.payload_offset=0;result_shape.capacity=0;}
        cf.result_kind=result_shape.kind; cf.result_size=result_shape.size; cf.result_align=result_shape.align;
        cf.result_storage=desc_is_register_scalar({fn.result_type,fn.result_cardinality,fn.result_nominal_type})?SirStorageClass::Register:SirStorageClass::Caller;
        if(fn.result_ownership=="borrowed"){cf.result_transfer=SirResultTransferKind::BorrowRef;cf.result_storage=SirStorageClass::Caller;cf.result_kind=SirConcreteKind::BorrowRef;cf.result_size=8;cf.result_align=8;}
        else if(fn.result_ownership=="shared-transfer"){cf.result_transfer=SirResultTransferKind::SharedRef;cf.result_storage=SirStorageClass::Caller;cf.result_kind=SirConcreteKind::BorrowRef;cf.result_size=8;cf.result_align=8;}
        else if(fn.result_resource.rfind("heap-pool:",0)==0){cf.result_transfer=SirResultTransferKind::HeapObject;cf.result_storage=SirStorageClass::Caller;cf.result_size=8;cf.result_align=8;}
        else if(fn.result_resource=="dynamic-many"){cf.result_transfer=SirResultTransferKind::DynamicMany;cf.result_storage=SirStorageClass::Caller;cf.result_kind=SirConcreteKind::ManyDynamic;cf.result_size=24;cf.result_align=8;}

        for(const auto&vs:fn.values){
            active.clear(); const auto cap=vs.cardinality==SirCardinality::Many?promoted_caps[vs.value]:0;
            auto sh=desc_shape({vs.type,vs.cardinality,vs.nominal_type},cap,s.datasets,s.choices,s.pools);
            if(dynamic_many.contains(vs.value)){auto elem=desc_shape({vs.type,SirCardinality::One,vs.nominal_type},0,s.datasets,s.choices,s.pools);sh.kind=SirConcreteKind::ManyDynamic;sh.size=24;sh.align=8;sh.payload_offset=0;sh.element_size=elem.size;sh.element_align=elem.align;sh.capacity=0;}
            SirConcreteValue cv; cv.value=vs.value; cv.kind=sh.kind; cv.size=sh.size; cv.align=sh.align;cv.payload_offset=sh.payload_offset;cv.element_size=sh.element_size;cv.capacity=sh.capacity;cv.escapes=escapes.contains(vs.value);
            const auto* op=defs.contains(vs.value)?defs[vs.value]:nullptr;
            const auto pit=std::find_if(fn.parameters.begin(),fn.parameters.end(),[&](const auto&p){return p.value==vs.value;});
            if(op&&(op->opcode==SirOpcode::FieldAddress||op->opcode==SirOpcode::FieldLoad||op->opcode==SirOpcode::FieldMove)){
                auto [owner,field]=split_field(op->semantic_name);(void)field;const auto* l=find_layout(owner);if(!l||op->tag>=l->field_offsets.size())fail("target concretization: addressable field lacks concrete offset");
                cv.byte_offset=l->field_offsets[op->tag];cv.alias_of=op->operands.at(0);
                if(op->opcode==SirOpcode::FieldAddress){cv.storage=SirStorageClass::Alias;cv.kind=SirConcreteKind::BorrowRef;cv.size=8;cv.align=8;}
                else if(op->opcode==SirOpcode::FieldMove){cv.storage=SirStorageClass::Alias;if(vs.resource_family.rfind("heap-pool:",0)==0||vs.resource_family.rfind("shared-pool:",0)==0){cv.kind=SirConcreteKind::BorrowRef;cv.size=8;cv.align=8;}}
                else if(desc_is_register_scalar({vs.type,vs.cardinality,vs.nominal_type}))cv.storage=SirStorageClass::Register;
                else cv.storage=SirStorageClass::Alias;
            }
            else if(desc_is_register_scalar({vs.type,vs.cardinality,vs.nominal_type})) cv.storage=SirStorageClass::Register;
            else if(pit!=fn.parameters.end() && (vs.borrow_mode!="none" || vs.resource_family.rfind("heap-pool:",0)==0 || vs.resource_family.rfind("shared-pool:",0)==0)) { cv.storage=SirStorageClass::Alias; cv.kind=SirConcreteKind::BorrowRef; cv.size=8; cv.align=8; }
            else if(pit!=fn.parameters.end()) cv.storage=SirStorageClass::Caller;
            else if(op&&(op->opcode==SirOpcode::Borrow||op->opcode==SirOpcode::BorrowMut||op->opcode==SirOpcode::MoveValue||op->opcode==SirOpcode::SendValue||op->opcode==SirOpcode::SharedRetain||op->opcode==SirOpcode::RevisionMarker)) {cv.storage=SirStorageClass::Alias;cv.alias_of=op->operands.at(0);if(op->opcode==SirOpcode::Borrow||op->opcode==SirOpcode::BorrowMut)cv.kind=SirConcreteKind::BorrowRef;}
            else if(op&&op->opcode==SirOpcode::Phi) cv.storage=SirStorageClass::Alias;
            else if(op&&op->opcode==SirOpcode::ManyDynamic) cv.storage=SirStorageClass::Stack;
            else if(op&&op->opcode==SirOpcode::Call){auto ci=semantic_functions.find(op->callee);if(ci!=semantic_functions.end()&&(ci->second->result_resource.rfind("heap-pool:",0)==0||ci->second->result_resource.rfind("shared-pool:",0)==0||ci->second->result_ownership=="borrowed"||ci->second->result_ownership=="shared-transfer")){cv.storage=SirStorageClass::Alias;cv.kind=SirConcreteKind::BorrowRef;cv.size=8;cv.align=8;}else cv.storage=SirStorageClass::Stack;}
            else if(op&&op->opcode==SirOpcode::PoolAlloc){const auto* ps=find_pool(s.pools,op->semantic_name);if(ps&&ps->allocation=="heap")cv.storage=SirStorageClass::Heap;else if(ps&&ps->allocation=="arena")cv.storage=SirStorageClass::PoolArena;else if(ps&&ps->allocation=="static")cv.storage=SirStorageClass::Static;else cv.storage=SirStorageClass::Stack;}
            else cv.storage=SirStorageClass::Stack;

            if((sh.kind==SirConcreteKind::Aggregate||sh.kind==SirConcreteKind::Choice)&&!cv.escapes&&op&&(op->opcode==SirOpcode::AggregateMake||op->opcode==SirOpcode::ChoiceMake)){
                bool runtime=false; for(auto u:users[vs.value]) if(u!=SirOpcode::Borrow&&u!=SirOpcode::BorrowMut&&u!=SirOpcode::MoveValue&&u!=SirOpcode::SendValue&&u!=SirOpcode::SharedRetain&&u!=SirOpcode::RevisionMarker&&u!=SirOpcode::RelationLink&&u!=SirOpcode::RelationUnlink) runtime=true;
                cv.scalar_replaced=!runtime;
            }
            cf.values.push_back(cv);
        }
        std::sort(cf.values.begin(),cf.values.end(),[](const auto&a,const auto&b){return a.value<b.value;});

        auto concrete_of=[&](SirValueId id)->const SirConcreteValue*{auto it=std::find_if(cf.values.begin(),cf.values.end(),[&](const auto&v){return v.value==id;});return it==cf.values.end()?nullptr:&*it;};
        auto value_prov=[&](SirValueId id){return find_value_sem(fn,id).provenance;};
        std::uint32_t next_resource=1;
        for(const auto&b:fn.blocks)for(const auto&op:b.instructions){
            if(op.opcode==SirOpcode::PoolAlloc){
                const auto* ps=find_pool(s.pools,op.semantic_name);const auto* cv=concrete_of(op.result);if(!ps||!cv)fail("target concretization: pool allocation lacks pool/concrete plan");
                SirConcreteResource r;r.id=next_resource++;r.value=op.result;r.provenance=value_prov(op.result);r.lifetime_region=op.region?op.region:fn.root_region;r.size=cv->size;r.align=cv->align;r.automatic=ps->reclaim!="manual";r.dynamic_many=false;r.resource_family=find_value_sem(fn,op.result).resource_family;
                if(cv->storage==SirStorageClass::Heap){if(ps->ownership=="shared"){r.cleanup=SirCleanupKind::SharedRelease;r.shared=true;}else r.cleanup=SirCleanupKind::HeapFree;}else r.cleanup=SirCleanupKind::LifetimeEnd;
                if(cv->storage!=SirStorageClass::Static)cf.resources.push_back(r);
            }else if(op.opcode==SirOpcode::ManyDynamic){
                const auto* cv=concrete_of(op.result);if(!cv)fail("target concretization: dynamic Many lacks concrete plan");
                SirConcreteResource r;r.id=next_resource++;r.value=op.result;r.provenance=value_prov(op.result);r.lifetime_region=op.region?op.region:fn.root_region;r.cleanup=SirCleanupKind::ManyBufferFree;r.size=cv->size;r.align=cv->align;r.automatic=true;r.dynamic_many=true;r.resource_family="dynamic-many";cf.resources.push_back(r);
            }else if(op.opcode==SirOpcode::SharedRetain){
                const auto*cv=concrete_of(op.result);if(!cv)fail("target concretization: retained shared reference lacks concrete plan");SirConcreteResource r;r.id=next_resource++;r.value=op.result;r.provenance=value_prov(op.result);r.lifetime_region=op.region?op.region:fn.root_region;r.size=8;r.align=8;r.cleanup=SirCleanupKind::SharedRelease;r.shared=true;r.resource_family=find_value_sem(fn,op.result).resource_family;r.automatic=true;if(r.resource_family.rfind("shared-pool:",0)==0){const auto pool=r.resource_family.substr(std::string("shared-pool:").size());const auto*ps=find_pool(s.pools,pool);if(ps)r.automatic=ps->reclaim!="manual";}cf.resources.push_back(r);
            }else if(op.opcode==SirOpcode::Call){
                auto ci=semantic_functions.find(op.callee);if(ci!=semantic_functions.end()&&(ci->second->result_ownership=="owned-transfer"||ci->second->result_ownership=="shared-transfer")){const auto*cv=concrete_of(op.result);if(!cv)fail("target concretization: transferred call result lacks concrete plan");SirConcreteResource r;r.id=next_resource++;r.value=op.result;r.provenance=value_prov(op.result);r.lifetime_region=op.region?op.region:fn.root_region;r.size=cv->size;r.align=cv->align;r.automatic=true;r.resource_family=ci->second->result_resource;if(ci->second->result_ownership=="shared-transfer"){r.cleanup=SirCleanupKind::SharedRelease;r.shared=true;r.size=8;r.align=8;}else if(ci->second->result_resource=="dynamic-many"){r.cleanup=SirCleanupKind::ManyBufferFree;r.dynamic_many=true;}else if(ci->second->result_resource.rfind("heap-pool:",0)==0){r.cleanup=SirCleanupKind::HeapFree;const auto pool=ci->second->result_resource.substr(std::string("heap-pool:").size());const auto*ps=find_pool(s.pools,pool);r.automatic=!ps||ps->reclaim!="manual";}else fail("target concretization: unknown transferred resource family");cf.resources.push_back(r);}
            }
        }

        // Materialize SIR-S ownership composition into the concrete SIR-C plan. Compiler 0.8
        // deliberately keeps child cleanup obligations flattened (the child still owns its exact
        // HeapFree/SharedRelease/ManyBufferFree proof), while `absorbed_by` + `component` records
        // describe where that obligation is physically embedded inside its owning rich value.
        // This makes composition independently serializable/verifiable without asking LLVM to
        // rediscover aggregate ownership from byte layout.
        auto cleanup_for_family=[&](const std::string& family)->std::optional<SirCleanupKind>{
            if(family.rfind("shared-pool:",0)==0)return SirCleanupKind::SharedRelease;
            if(family.rfind("heap-pool:",0)==0)return SirCleanupKind::HeapFree;
            if(family=="dynamic-many")return SirCleanupKind::ManyBufferFree;
            return std::nullopt;
        };
        auto resource_by_prov=[&](SirProvenanceId prov)->SirConcreteResource*{
            auto it=std::find_if(cf.resources.begin(),cf.resources.end(),[&](auto&r){return r.provenance==prov;});
            return it==cf.resources.end()?nullptr:&*it;
        };
        std::uint32_t next_component=1;
        for(const auto&b:fn.blocks)for(const auto&op:b.instructions){
            if(op.opcode!=SirOpcode::AggregateMake&&op.opcode!=SirOpcode::PoolAlloc&&op.opcode!=SirOpcode::OptionalSome&&op.opcode!=SirOpcode::ChoiceMake&&op.opcode!=SirOpcode::ManyMake)continue;
            const auto& owner_vs=find_value_sem(fn,op.result);if(!owner_vs.provenance)continue;
            const auto* owner_cv=concrete_of(op.result);if(!owner_cv)fail("target concretization: resource-composition owner lacks concrete value");
            for(std::size_t i=0;i<op.operands.size();++i){
                const auto& child_vs=find_value_sem(fn,op.operands[i]);if(!child_vs.provenance||child_vs.resource_family.empty())continue;
                auto* child_resource=resource_by_prov(child_vs.provenance);if(!child_resource)continue; // borrowed/non-owning child, or already abstract-only composition
                auto ck=cleanup_for_family(child_resource->resource_family);if(!ck)continue;
                SirResourceComponent c;c.id=next_component++;c.owner_value=op.result;c.owner_provenance=owner_vs.provenance;c.cleanup=*ck;
                c.type_name=child_vs.nominal_type.empty()?sir_type_name(child_vs.type):child_vs.nominal_type;
                if(op.opcode==SirOpcode::AggregateMake||op.opcode==SirOpcode::PoolAlloc){
                    const auto* l=find_layout(op.semantic_name);if(!l||i>=l->field_offsets.size())fail("target concretization: resource field lacks concrete owner offset");
                    c.guard=SirComponentGuard::Always;c.byte_offset=l->field_offsets[i];
                    if(op.opcode==SirOpcode::AggregateMake){const auto* ds=find_dataset(s.datasets,op.semantic_name);if(!ds||i>=ds->fields.size())fail("target concretization: dataset resource component lacks field schema");c.path=ds->fields[i].name;}
                    else {const auto* ps=find_pool(s.pools,op.semantic_name);if(!ps||i>=ps->fields.size())fail("target concretization: pool resource component lacks field schema");c.path=ps->fields[i].name;}
                }else if(op.opcode==SirOpcode::OptionalSome){
                    c.guard=SirComponentGuard::OptionalPresent;c.byte_offset=owner_cv->payload_offset;c.path="payload";
                }else if(op.opcode==SirOpcode::ChoiceMake){
                    const auto* ch=find_choice(s.choices,op.semantic_name);if(!ch||op.tag>=ch->cases.size()||i>=ch->cases[op.tag].fields.size())fail("target concretization: choice resource component lacks case schema");
                    auto arm=record_shape(ch->cases[op.tag].fields,s.datasets,s.choices,s.pools);if(i>=arm.field_offsets.size())fail("target concretization: choice resource payload lacks concrete offset");
                    c.guard=SirComponentGuard::ChoiceTag;c.tag=op.tag;c.byte_offset=owner_cv->payload_offset+arm.field_offsets[i];c.path=ch->cases[op.tag].name+"."+ch->cases[op.tag].fields[i].name;
                }else { // ManyMake
                    c.guard=SirComponentGuard::ManyElements;c.byte_offset=owner_cv->payload_offset;c.element_stride=owner_cv->element_size;c.path="elements";
                }
                // Require a corresponding semantic ownership edge before concrete composition may exist.
                const auto edge=std::find_if(fn.ownership_edges.begin(),fn.ownership_edges.end(),[&](const auto&e){return e.owner==owner_vs.provenance&&e.child==child_vs.provenance;});
                if(edge==fn.ownership_edges.end())fail("target concretization: concrete resource component lacks SIR-S ownership edge");
                child_resource->absorbed_by=c.id;cf.resource_components.push_back(std::move(c));
            }
        }
        std::sort(cf.resource_components.begin(),cf.resource_components.end(),[](const auto&a,const auto&b){return a.id<b.id;});

        auto resource_returned=[&](const SirConcreteResource&r,const SirBlock&b){
            if((fn.result_ownership!="owned-transfer"&&fn.result_ownership!="shared-transfer")||b.terminator.kind!=SirTerminatorKind::Return||!b.terminator.value)return false;
            const auto& vs=find_value_sem(fn,*b.terminator.value);
            return vs.provenance==r.provenance && ((fn.result_ownership=="owned-transfer"&&vs.ownership=="moved")||(fn.result_ownership=="shared-transfer"&&vs.ownership=="shared"));
        };
        auto add_action=[&](const SirConcreteResource&r,SirBlockId from,SirBlockId to,SirOpId source,bool fault_exit,bool transfer,std::string reason){
            auto exists=std::any_of(cf.cleanup_actions.begin(),cf.cleanup_actions.end(),[&](const auto&a){return a.resource_id==r.id&&a.from_block==from&&a.to_block==to&&a.source_op==source&&a.fault_exit==fault_exit&&a.transfer==transfer;});
            if(!exists)cf.cleanup_actions.push_back({r.id,from,to,source,fault_exit,transfer,std::move(reason)});
        };
        for(const auto&r:cf.resources)if(r.automatic){
            const auto defb=defblocks.contains(r.value)?defblocks[r.value]:fn.entry;
            if(r.lifetime_region!=block_by_id(fn,defb)->region && block_in_cycle(fn,defb))
                throw CompileError({},"target concretization: repeating automatic resource allocation with a lifetime wider than its loop is not yet representable by a single cleanup obligation; narrow the resource to @lifetime(scope) or use @reclaim(manual)");
            for(const auto&b:fn.blocks){
                if(!region_inside(fn,b.region,r.lifetime_region))continue;
                for(const auto&op:b.instructions)for(const auto&fe:op.fault_edges){const auto* tb=block_by_id(fn,fe.target);if(tb&&!region_inside(fn,tb->region,r.lifetime_region))add_action(r,b.id,fe.target,op.id,false,false,"fault-edge-region-exit");}
                if(b.terminator.kind==SirTerminatorKind::Br){const auto* tb=block_by_id(fn,b.terminator.target);if(tb&&!region_inside(fn,tb->region,r.lifetime_region))add_action(r,b.id,b.terminator.target,0,false,false,"branch-region-exit");}
                else if(b.terminator.kind==SirTerminatorKind::CondBr){
                    const auto* t=block_by_id(fn,b.terminator.true_target);const auto* f=block_by_id(fn,b.terminator.false_target);
                    if(t&&!region_inside(fn,t->region,r.lifetime_region))add_action(r,b.id,b.terminator.true_target,0,false,false,"branch-region-exit");
                    if(f&&!region_inside(fn,f->region,r.lifetime_region))add_action(r,b.id,b.terminator.false_target,0,false,false,"branch-region-exit");
                }else if(b.terminator.kind==SirTerminatorKind::Return){if(resource_returned(r,b))add_action(r,b.id,0xffffffffu,0,false,true,"ownership-transfer");else add_action(r,b.id,0xffffffffu,0,false,false,"function-return");}
                else if(b.terminator.kind==SirTerminatorKind::FaultReturn)add_action(r,b.id,0xffffffffu,0,true,false,"fault-return");
            }
        }
        for(const auto& rb:fn.rollback_obligations) cf.rollback_actions.push_back({rb.id,rb.transaction_region,rb.provenance,rb.source_op,rb.action});
        std::sort(cf.rollback_actions.begin(),cf.rollback_actions.end(),[](const auto&a,const auto&b){return a.id<b.id;});

        // SIR-C 6.0: executable transaction undo plans are derived entirely from
        // the already-verified SIR-S rollback obligations.  LLVM consumes these
        // records; it does not rediscover transaction semantics from source/AST.
        std::map<SirOpId,const SirInstruction*> op_by_id;
        for(const auto& b:fn.blocks) for(const auto& op:b.instructions) op_by_id[op.id]=&op;
        for(const auto& rb:fn.rollback_obligations){
            auto oi=op_by_id.find(rb.source_op);
            if(oi==op_by_id.end()) fail("target concretization: rollback obligation names unknown source operation");
            const auto& op=*oi->second;
            SirConcreteUndo u;u.id=rb.id;u.transaction_region=rb.transaction_region;u.source_op=rb.source_op;
            if(rb.action=="restore-field"){
                if(op.opcode!=SirOpcode::FieldStore||op.operands.size()<2) fail("target concretization: restore-field requires FieldStore source operation");
                u.kind=SirUndoKind::RestoreField;u.target_value=op.operands[0];u.aux_value=op.operands[1];u.semantic_name=op.semantic_name;
                auto pos=op.semantic_name.find('.');if(pos==std::string::npos) fail("target concretization: restore-field lacks owner.field identity");
                const auto owner=op.semantic_name.substr(0,pos);const auto* l=find_layout(owner);
                if(!l||op.tag>=l->field_offsets.size()) fail("target concretization: restore-field lacks concrete field offset");
                u.byte_offset=l->field_offsets[op.tag];const auto* fcv=concrete_of(op.operands[1]);if(!fcv) fail("target concretization: restore-field replacement lacks concrete plan");u.size=fcv->size;u.align=fcv->align;
            }else if(rb.action=="relation-unlink"){
                if(op.opcode!=SirOpcode::RelationLink||op.operands.size()<2) fail("target concretization: relation-unlink undo requires RelationLink source");
                u.kind=SirUndoKind::RelationUnlink;u.target_value=op.operands[0];u.aux_value=op.operands[1];u.semantic_name=op.semantic_name;
            }else if(rb.action=="relation-link"){
                if(op.opcode!=SirOpcode::RelationUnlink||op.operands.size()<2) fail("target concretization: relation-link undo requires RelationUnlink source");
                u.kind=SirUndoKind::RelationLink;u.target_value=op.operands[0];u.aux_value=op.operands[1];u.semantic_name=op.semantic_name;
            }else if(rb.action=="restore-owner"){
                if(op.opcode!=SirOpcode::MoveValue&&op.opcode!=SirOpcode::SendValue&&op.opcode!=SirOpcode::FieldMove) fail("target concretization: restore-owner undo requires ownership transfer source");
                if(op.operands.empty()) fail("target concretization: restore-owner source lacks operand");
                u.kind=SirUndoKind::RestoreOwner;u.target_value=op.operands[0];
                if(op.opcode==SirOpcode::FieldMove){
                    u.aux_value=op.result;u.semantic_name=op.semantic_name;auto pos=op.semantic_name.find('.');if(pos==std::string::npos)fail("target concretization: field-move restore-owner lacks owner.field identity");const auto* l=find_layout(op.semantic_name.substr(0,pos));if(!l||op.tag>=l->field_offsets.size())fail("target concretization: field-move restore-owner lacks concrete offset");u.byte_offset=l->field_offsets[op.tag];u.size=8;u.align=8;
                }
            }else if(rb.action=="destroy-created"){
                if(op.opcode!=SirOpcode::PoolAlloc||!op.result) fail("target concretization: destroy-created undo requires pool allocation source");
                u.kind=SirUndoKind::DestroyResource;u.target_value=op.result;const auto* cv=concrete_of(op.result);if(cv){u.size=cv->size;u.align=cv->align;}u.semantic_name=op.semantic_name;
            }else if(rb.action=="shared-release"){
                if(op.opcode!=SirOpcode::SharedRetain||!op.result) fail("target concretization: shared-release undo requires SharedRetain source");
                u.kind=SirUndoKind::SharedRelease;u.target_value=op.result;u.size=8;u.align=8;
            }else fail("target concretization: unsupported rollback action '"+rb.action+"'");
            cf.undo_actions.push_back(std::move(u));
        }
        std::sort(cf.undo_actions.begin(),cf.undo_actions.end(),[](const auto&a,const auto&b){return a.id<b.id;});

        // SIR-C 6.0: deterministic structured-task plan.  Semantic capture
        // legality remains in SIR-S; this layer binds task regions to exact
        // begin/end op identities for the target backend.
        std::map<SirRegionId,const SirRegion*> regions_by_id;for(const auto&r:fn.regions)regions_by_id[r.id]=&r;
        auto parallel_ancestor=[&](SirRegionId rid)->SirRegionId{
            std::set<SirRegionId> seen;while(rid&&seen.insert(rid).second){auto ri=regions_by_id.find(rid);if(ri==regions_by_id.end())break;if(ri->second->kind=="parallel")return rid;rid=ri->second->parent;}return 0;
        };
        std::uint32_t next_task_plan=1;
        for(const auto&r:fn.regions)if(r.kind=="task"){
            SirOpId begin=0,end=0;for(const auto&b:fn.blocks)for(const auto&op:b.instructions)if(op.region==r.id){if(op.opcode==SirOpcode::TaskBegin)begin=op.id;else if(op.opcode==SirOpcode::TaskEnd)end=op.id;}
            auto par=parallel_ancestor(r.parent);if(!begin||!end||!par)fail("target concretization: task region lacks structured begin/end or parallel ancestor");
            cf.tasks.push_back({next_task_plan++,par,r.id,begin,end});
        }
        for(const auto&c:fn.task_captures)cf.task_captures.push_back({c.id,c.parallel_region,c.task_region,c.value,c.provenance,c.mode});
        std::sort(cf.tasks.begin(),cf.tasks.end(),[](const auto&a,const auto&b){return a.id<b.id;});
        std::sort(cf.task_captures.begin(),cf.task_captures.end(),[](const auto&a,const auto&b){return a.id<b.id;});
        std::sort(cf.resources.begin(),cf.resources.end(),[](const auto&a,const auto&b){return a.id<b.id;});
        std::sort(cf.cleanup_actions.begin(),cf.cleanup_actions.end(),[](const auto&a,const auto&b){return std::tie(a.from_block,a.to_block,a.source_op,a.resource_id,a.fault_exit)<std::tie(b.from_block,b.to_block,b.source_op,b.resource_id,b.fault_exit);});
        o.concrete_functions.push_back(std::move(cf));
    }
    std::sort(o.concrete_functions.begin(),o.concrete_functions.end(),[](const auto&a,const auto&b){return a.name<b.name;});
    return o;
}

void SirVerifier::verify(const SirSProgram&sir)const{
    if(sir.format_major<3||sir.format_major>5)throw CompileError({},"SIR-S verifier: unsupported format major (Compiler 0.8 accepts SIR-S 3.x/4.x/5.x)");
    verify_common(sir.module_name,sir.faults,sir.datasets,sir.choices,sir.relations,sir.pools,sir.functions);
}
void SirVerifier::verify(const SirCProgram&sir)const{
    if(sir.format_major<3||sir.format_major>6)throw CompileError({},"SIR-C verifier: unsupported format major (Compiler 0.8 accepts SIR-C 3.x/4.x/5.x/6.x)");
    if(sir.target_triple!="x86_64-pc-windows-msvc"||sir.target_profile!="windows-x86_64-bootstrap"||sir.pointer_bits!=64||sir.object_format!="COFF-x86-64"||sir.executable_format!="PE32+")throw CompileError({},"SIR-C verifier: target concrete representation contract mismatch");
    if(sir.aggregate_representation.empty()||sir.cardinality_representation.empty()||sir.fault_payload_representation.empty())throw CompileError({},"SIR-C verifier: concrete representation contract missing");
    verify_common(sir.module_name,sir.faults,sir.datasets,sir.choices,sir.relations,sir.pools,sir.functions);
    std::map<std::string,const SirConcreteLayout*> layoutmap;
    for(const auto&l:sir.concrete_layouts){
        if(l.name.empty()||layoutmap.contains(l.name))throw CompileError({},"SIR-C verifier: duplicate/empty concrete layout");
        if(!l.size||!l.align||(l.align&(l.align-1)))throw CompileError({},"SIR-C verifier: concrete layout size/alignment invalid");
        if(l.payload_offset>l.size)throw CompileError({},"SIR-C verifier: concrete payload offset outside layout");
        for(auto off:l.field_offsets)if(off>=l.size)throw CompileError({},"SIR-C verifier: concrete field offset outside layout");
        layoutmap[l.name]=&l;
    }
    std::map<std::string,const SirFunction*> fmap;for(const auto&fn:sir.functions)fmap[fn.name]=&fn;
    std::map<std::string,const SirConcreteFunction*> cfmap;for(const auto&x:sir.concrete_functions)cfmap[x.name]=&x;
    std::set<std::string> plans;
    for (const auto& cf : sir.concrete_functions) {
        if (!plans.insert(cf.name).second) throw CompileError({}, "SIR-C verifier: duplicate concrete function plan");
        auto fi = fmap.find(cf.name); if (fi == fmap.end()) throw CompileError({}, "SIR-C verifier: concrete plan names unknown function");
        const auto& fn=*fi->second;
        SirResultTransferKind expected_transfer=SirResultTransferKind::None;
        if(fn.result_ownership=="borrowed")expected_transfer=SirResultTransferKind::BorrowRef;
        else if(fn.result_ownership=="shared-transfer")expected_transfer=SirResultTransferKind::SharedRef;
        else if(fn.result_resource.rfind("heap-pool:",0)==0)expected_transfer=SirResultTransferKind::HeapObject;
        else if(fn.result_resource=="dynamic-many")expected_transfer=SirResultTransferKind::DynamicMany;
        if(cf.result_transfer!=expected_transfer)throw CompileError({},"SIR-C verifier: concrete result-transfer mode disagrees with SIR-S ownership/borrow contract");
        if((expected_transfer==SirResultTransferKind::HeapObject||expected_transfer==SirResultTransferKind::DynamicMany)&&fn.result_ownership!="owned-transfer")throw CompileError({},"SIR-C verifier: owned concrete result lacks semantic owned-transfer contract");
        if(expected_transfer==SirResultTransferKind::BorrowRef&&fn.result_ownership!="borrowed")throw CompileError({},"SIR-C verifier: borrow-ref ABI lacks semantic borrowed result contract");
        if(expected_transfer==SirResultTransferKind::SharedRef&&fn.result_ownership!="shared-transfer")throw CompileError({},"SIR-C verifier: shared-ref ABI lacks semantic shared-transfer contract");
        if (!cf.result_size || !cf.result_align || !cf.fault_payload_size || !cf.fault_payload_align) throw CompileError({}, "SIR-C verifier: zero concrete result/fault layout");
        if((cf.result_transfer==SirResultTransferKind::HeapObject||cf.result_transfer==SirResultTransferKind::BorrowRef||cf.result_transfer==SirResultTransferKind::SharedRef)&&(cf.result_size!=8||cf.result_align!=8))throw CompileError({},"SIR-C verifier: pointer transfer ABI must use one pointer-sized caller slot");
        if(cf.result_transfer==SirResultTransferKind::DynamicMany&&(cf.result_size!=24||cf.result_align!=8))throw CompileError({},"SIR-C verifier: dynamic-Many transfer ABI must use a 24-byte descriptor");
        std::set<SirValueId> ids;std::map<SirValueId,const SirConcreteValue*> cvmap;
        for (const auto& cv : cf.values) {
            if (!cv.value || !ids.insert(cv.value).second) throw CompileError({}, "SIR-C verifier: duplicate/zero concrete value plan");
            if (!cv.size || !cv.align || (cv.align & (cv.align - 1))) throw CompileError({}, "SIR-C verifier: concrete value size/alignment invalid");
            if (cv.payload_offset > cv.size) throw CompileError({}, "SIR-C verifier: concrete value payload offset outside storage");
            if (cv.storage == SirStorageClass::Alias && cv.alias_of == cv.value) throw CompileError({}, "SIR-C verifier: concrete alias may not alias itself");
            if(cv.kind==SirConcreteKind::ManyDynamic&&(cv.size!=24||cv.align!=8||cv.element_size==0))throw CompileError({},"SIR-C verifier: dynamic Many descriptor must be {ptr,u64,u64} with known element size");
            cvmap[cv.value]=&cv;
        }
        if (ids.size() != fn.values.size()) throw CompileError({}, "SIR-C verifier: every semantic SSA value requires one concrete value plan");
        for (const auto& v : fn.values) if (!ids.contains(v.value)) throw CompileError({}, "SIR-C verifier: semantic SSA value missing concrete representation plan");

        std::map<SirValueId,const SirInstruction*> defs;std::map<SirOpId,std::pair<SirBlockId,const SirInstruction*>> opmap;std::map<SirBlockId,const SirBlock*> bmap;
        for(const auto&b:fn.blocks){bmap[b.id]=&b;for(const auto&op:b.instructions){defs[op.result]=&op;opmap[op.id]={b.id,&op};}}
        for(const auto&b:fn.blocks)for(const auto&op:b.instructions){
            if(op.opcode==SirOpcode::FieldLoad||op.opcode==SirOpcode::FieldAddress||op.opcode==SirOpcode::FieldMove){
                const auto* cv=cvmap.at(op.result);auto pos=op.semantic_name.find('.');if(pos==std::string::npos)throw CompileError({},"SIR-C verifier: addressable field lacks owner.field identity");
                auto owner=op.semantic_name.substr(0,pos);auto li=layoutmap.find(owner);if(li==layoutmap.end()||op.tag>=li->second->field_offsets.size())throw CompileError({},"SIR-C verifier: addressable field has no concrete nominal offset");
                if(cv->byte_offset!=li->second->field_offsets[op.tag])throw CompileError({},"SIR-C verifier: addressable field byte offset does not match nominal layout");
                if((op.opcode==SirOpcode::FieldAddress||op.opcode==SirOpcode::FieldMove)&&(cv->storage!=SirStorageClass::Alias||cv->alias_of!=op.operands.at(0)))throw CompileError({},"SIR-C verifier: field address/move must retain concrete root association");
            }
            if(op.opcode==SirOpcode::ManyDynamic&&cvmap.at(op.result)->kind!=SirConcreteKind::ManyDynamic)throw CompileError({},"SIR-C verifier: many.dynamic requires ManyDynamic concrete representation");
            if(op.opcode==SirOpcode::ManyPush){const auto* list=cvmap.at(op.operands.at(0));if(list->kind!=SirConcreteKind::ManyDynamic)throw CompileError({},"SIR-C verifier: many.push currently requires a dynamic Many representation");}
        }

        std::set<std::uint32_t> resource_ids;std::map<std::uint32_t,const SirConcreteResource*> resources;
        for(const auto&r:cf.resources){
            if(!r.id||!resource_ids.insert(r.id).second)throw CompileError({},"SIR-C verifier: duplicate/zero resource identity");
            if(!cvmap.contains(r.value))throw CompileError({},"SIR-C verifier: resource names unknown concrete value");
            const auto& vs=find_value_sem(fn,r.value);if(vs.provenance!=r.provenance||!r.provenance)throw CompileError({},"SIR-C verifier: resource provenance mismatch");
            if(!r.lifetime_region||std::none_of(fn.regions.begin(),fn.regions.end(),[&](const auto&x){return x.id==r.lifetime_region;}))throw CompileError({},"SIR-C verifier: resource lifetime region is unknown");
            if(!r.size||!r.align||(r.align&(r.align-1)))throw CompileError({},"SIR-C verifier: resource size/alignment invalid");
            const auto di=defs.find(r.value);if(di==defs.end())throw CompileError({},"SIR-C verifier: resource value lacks defining operation");
            const auto* op=di->second;const auto* cv=cvmap.at(r.value);
            if(r.cleanup==SirCleanupKind::HeapFree){bool ok=op->opcode==SirOpcode::PoolAlloc&&cv->storage==SirStorageClass::Heap;if(op->opcode==SirOpcode::Call){auto ci=cfmap.find(op->callee);ok=ci!=cfmap.end()&&ci->second->result_transfer==SirResultTransferKind::HeapObject&&cv->storage==SirStorageClass::Alias&&cv->kind==SirConcreteKind::BorrowRef&&cv->size==8&&cv->align==8;}if(!ok)throw CompileError({},"SIR-C verifier: HeapFree obligation must own a heap pool allocation or transferred heap result");}
            if(r.cleanup==SirCleanupKind::ManyBufferFree){bool ok=op->opcode==SirOpcode::ManyDynamic&&cv->kind==SirConcreteKind::ManyDynamic&&r.dynamic_many;if(op->opcode==SirOpcode::Call){auto ci=cfmap.find(op->callee);ok=ci!=cfmap.end()&&ci->second->result_transfer==SirResultTransferKind::DynamicMany&&cv->kind==SirConcreteKind::ManyDynamic&&r.dynamic_many;}if(!ok)throw CompileError({},"SIR-C verifier: ManyBufferFree obligation must own a dynamic Many buffer or transferred dynamic-Many result");}
            if(r.cleanup==SirCleanupKind::SharedRelease){bool ok=false;if(op->opcode==SirOpcode::PoolAlloc){const auto*ps=find_pool(sir.pools,op->semantic_name);ok=ps&&ps->allocation=="heap"&&ps->ownership=="shared";}else if(op->opcode==SirOpcode::SharedRetain)ok=true;else if(op->opcode==SirOpcode::Call){auto ci=cfmap.find(op->callee);ok=ci!=cfmap.end()&&ci->second->result_transfer==SirResultTransferKind::SharedRef;}if(!ok||!r.shared||r.resource_family.rfind("shared-pool:",0)!=0)throw CompileError({},"SIR-C verifier: shared-release obligation must own a shared heap reference");}
            if(r.cleanup==SirCleanupKind::LifetimeEnd&&(op->opcode!=SirOpcode::PoolAlloc||(cv->storage!=SirStorageClass::Stack&&cv->storage!=SirStorageClass::PoolArena)))throw CompileError({},"SIR-C verifier: lifetime-end resource must name stack/arena pool storage");
            if(r.cleanup==SirCleanupKind::CompositeRelease)throw CompileError({},"SIR-C verifier: top-level CompositeRelease resource is reserved; Compiler 0.8 uses verified flattened child obligations plus resource-component records");
            resources[r.id]=&r;
        }
        std::set<std::tuple<std::uint32_t,SirBlockId,SirBlockId,SirOpId,bool>> actions;
        std::map<std::uint32_t,std::size_t> action_count;
        for(const auto&a:cf.cleanup_actions){
            if(!resources.contains(a.resource_id)||!resources[a.resource_id]->automatic)throw CompileError({},"SIR-C verifier: cleanup action names missing/nonautomatic resource");
            if(!bmap.contains(a.from_block))throw CompileError({},"SIR-C verifier: cleanup action source block is unknown");
            if(a.to_block!=0xffffffffu&&!bmap.contains(a.to_block))throw CompileError({},"SIR-C verifier: cleanup action target block is unknown");
            auto key=std::make_tuple(a.resource_id,a.from_block,a.to_block,a.source_op,a.fault_exit);if(!actions.insert(key).second)throw CompileError({},"SIR-C verifier: duplicate cleanup action");
            const auto* b=bmap[a.from_block];
            if(a.transfer){if(a.source_op||a.fault_exit||a.to_block!=0xffffffffu||b->terminator.kind!=SirTerminatorKind::Return||!b->terminator.value)throw CompileError({},"SIR-C verifier: ownership transfer must occur on a normal function return");const auto&rv=find_value_sem(fn,*b->terminator.value);const bool owned_ok=fn.result_ownership=="owned-transfer"&&(rv.ownership=="moved"||rv.ownership=="sent");const bool shared_ok=fn.result_ownership=="shared-transfer"&&rv.ownership=="shared";if((!owned_ok&&!shared_ok)||rv.provenance!=resources[a.resource_id]->provenance)throw CompileError({},"SIR-C verifier: transfer action is not proven by returned ownership provenance");}
            if(a.source_op){if(a.transfer)throw CompileError({},"SIR-C verifier: fault edge cannot transfer ownership");auto oi=opmap.find(a.source_op);if(oi==opmap.end()||oi->second.first!=a.from_block)throw CompileError({},"SIR-C verifier: cleanup fault action names op outside source block");bool edge=false;for(const auto&fe:oi->second.second->fault_edges)edge=edge||fe.target==a.to_block;if(!edge)throw CompileError({},"SIR-C verifier: cleanup fault action does not correspond to a fault edge");}
            else if(a.to_block==0xffffffffu){if(a.fault_exit&&b->terminator.kind!=SirTerminatorKind::FaultReturn)throw CompileError({},"SIR-C verifier: fault-exit cleanup must precede fault.return");if(!a.fault_exit&&b->terminator.kind!=SirTerminatorKind::Return)throw CompileError({},"SIR-C verifier: normal-exit cleanup/transfer must precede return");}
            else {bool edge=(b->terminator.kind==SirTerminatorKind::Br&&b->terminator.target==a.to_block)||(b->terminator.kind==SirTerminatorKind::CondBr&&(b->terminator.true_target==a.to_block||b->terminator.false_target==a.to_block));if(!edge)throw CompileError({},"SIR-C verifier: cleanup branch action does not correspond to CFG edge");}
            ++action_count[a.resource_id];
        }
        auto region_inside_c=[&](SirRegionId child,SirRegionId ancestor){if(!ancestor)return true;std::set<SirRegionId>seen;while(child&&seen.insert(child).second){if(child==ancestor)return true;auto it=std::find_if(fn.regions.begin(),fn.regions.end(),[&](const auto&x){return x.id==child;});if(it==fn.regions.end())break;child=it->parent;}return false;};
        auto has_action=[&](std::uint32_t rid,SirBlockId from,SirBlockId to,SirOpId source,bool fault,bool transfer){return std::any_of(cf.cleanup_actions.begin(),cf.cleanup_actions.end(),[&](const auto&a){return a.resource_id==rid&&a.from_block==from&&a.to_block==to&&a.source_op==source&&a.fault_exit==fault&&a.transfer==transfer;});};
        for(const auto&r:cf.resources){
            if(!r.automatic){if(action_count[r.id])throw CompileError({},"SIR-C verifier: manual resource may not receive automatic cleanup actions");continue;}
            if(!action_count[r.id])throw CompileError({},"SIR-C verifier: automatic resource has no proven cleanup/transfer path");
            for(const auto&b:fn.blocks){if(!region_inside_c(b.region,r.lifetime_region))continue;
                for(const auto&op:b.instructions)for(const auto&fe:op.fault_edges){const auto*tb=bmap.at(fe.target);if(!region_inside_c(tb->region,r.lifetime_region)&&!has_action(r.id,b.id,fe.target,op.id,false,false))throw CompileError({},"SIR-C verifier: missing cleanup on fault edge leaving resource lifetime");}
                if(b.terminator.kind==SirTerminatorKind::Br){const auto*tb=bmap.at(b.terminator.target);if(!region_inside_c(tb->region,r.lifetime_region)&&!has_action(r.id,b.id,b.terminator.target,0,false,false))throw CompileError({},"SIR-C verifier: missing cleanup on branch leaving resource lifetime");}
                else if(b.terminator.kind==SirTerminatorKind::CondBr){for(auto target:{b.terminator.true_target,b.terminator.false_target}){const auto*tb=bmap.at(target);if(!region_inside_c(tb->region,r.lifetime_region)&&!has_action(r.id,b.id,target,0,false,false))throw CompileError({},"SIR-C verifier: missing cleanup on conditional branch leaving resource lifetime");}}
                else if(b.terminator.kind==SirTerminatorKind::Return){bool transfer=false;if(b.terminator.value){const auto&rv=find_value_sem(fn,*b.terminator.value);transfer=((fn.result_ownership=="owned-transfer"&&(rv.ownership=="moved"||rv.ownership=="sent"))||(fn.result_ownership=="shared-transfer"&&rv.ownership=="shared"))&&rv.provenance==r.provenance;}if(!has_action(r.id,b.id,0xffffffffu,0,false,transfer))throw CompileError({},transfer?"SIR-C verifier: returned owned resource lacks transfer discharge":"SIR-C verifier: return path lacks resource cleanup");}
                else if(b.terminator.kind==SirTerminatorKind::FaultReturn){if(!has_action(r.id,b.id,0xffffffffu,0,true,false))throw CompileError({},"SIR-C verifier: fault-return path lacks resource cleanup");}
            }
        }
        std::set<std::uint32_t> component_ids;
        for(const auto&c:cf.resource_components){
            if(!c.id||!component_ids.insert(c.id).second)throw CompileError({},"SIR-C verifier: duplicate/zero resource-component identity");
            if(!cvmap.contains(c.owner_value))throw CompileError({},"SIR-C verifier: resource component names unknown owner value");
            const auto&ov=find_value_sem(fn,c.owner_value);if(ov.provenance!=c.owner_provenance||!c.owner_provenance)throw CompileError({},"SIR-C verifier: resource component owner provenance mismatch");
            if(c.path.empty())throw CompileError({},"SIR-C verifier: resource component requires a stable semantic path");
            if(c.cleanup==SirCleanupKind::LifetimeEnd||c.cleanup==SirCleanupKind::CompositeRelease)throw CompileError({},"SIR-C verifier: resource component cleanup must name a destroyable child resource kind");
            if(c.guard==SirComponentGuard::ManyElements&&c.element_stride==0)throw CompileError({},"SIR-C verifier: ManyElements component requires element stride");
            auto child=std::find_if(cf.resources.begin(),cf.resources.end(),[&](const auto&r){return r.absorbed_by==c.id;});
            if(child==cf.resources.end())throw CompileError({},"SIR-C verifier: resource component has no absorbed child obligation");
            if(child->cleanup!=c.cleanup)throw CompileError({},"SIR-C verifier: component cleanup kind disagrees with absorbed child resource");
            auto oe=std::find_if(fn.ownership_edges.begin(),fn.ownership_edges.end(),[&](const auto&e){return e.owner==c.owner_provenance&&e.child==child->provenance;});
            if(oe==fn.ownership_edges.end())throw CompileError({},"SIR-C verifier: resource component lacks matching SIR-S ownership edge");
            if(c.guard==SirComponentGuard::Always&&oe->path!=c.path)throw CompileError({},"SIR-C verifier: concrete field component path disagrees with semantic ownership path");
        }
        for(const auto&r:cf.resources)if(r.absorbed_by&&!component_ids.contains(r.absorbed_by))throw CompileError({},"SIR-C verifier: resource absorbed_by names unknown component");
        std::set<std::uint32_t> concrete_rb_ids;
        for(const auto&r:cf.rollback_actions){
            if(!r.id||!concrete_rb_ids.insert(r.id).second)throw CompileError({},"SIR-C verifier: duplicate/zero concrete rollback identity");
            auto sr=std::find_if(fn.rollback_obligations.begin(),fn.rollback_obligations.end(),[&](const auto&x){return x.id==r.id;});if(sr==fn.rollback_obligations.end()||sr->transaction_region!=r.transaction_region||sr->provenance!=r.provenance||sr->source_op!=r.source_op||sr->action!=r.action)throw CompileError({},"SIR-C verifier: concrete rollback action disagrees with SIR-S transaction obligation");
            if(!opmap.contains(r.source_op))throw CompileError({},"SIR-C verifier: rollback action source operation does not exist");
        }
        if(concrete_rb_ids.size()!=fn.rollback_obligations.size())throw CompileError({},"SIR-C verifier: every semantic rollback obligation requires one concrete rollback action");

        if(sir.format_major>=6){
            // Executable undo must be a one-to-one concrete realization of the
            // semantic rollback graph.  A detached SIR-C file therefore cannot
            // weaken, retarget, or silently omit transaction restoration.
            std::set<std::uint32_t> undo_ids;
            for(const auto&u:cf.undo_actions){
                if(!u.id||!undo_ids.insert(u.id).second)throw CompileError({},"SIR-C verifier: duplicate/zero concrete undo identity");
                auto rb=std::find_if(fn.rollback_obligations.begin(),fn.rollback_obligations.end(),[&](const auto&x){return x.id==u.id;});
                if(rb==fn.rollback_obligations.end()||rb->transaction_region!=u.transaction_region||rb->source_op!=u.source_op)throw CompileError({},"SIR-C verifier: concrete undo disagrees with SIR-S rollback identity");
                auto oi=opmap.find(u.source_op);if(oi==opmap.end())throw CompileError({},"SIR-C verifier: concrete undo source operation does not exist");const auto&op=*oi->second.second;
                auto expected_kind=[&](){if(rb->action=="restore-field")return SirUndoKind::RestoreField;if(rb->action=="relation-unlink")return SirUndoKind::RelationUnlink;if(rb->action=="relation-link")return SirUndoKind::RelationLink;if(rb->action=="restore-owner")return SirUndoKind::RestoreOwner;if(rb->action=="destroy-created")return SirUndoKind::DestroyResource;if(rb->action=="shared-release")return SirUndoKind::SharedRelease;throw CompileError({},"SIR-C verifier: unknown semantic rollback action");};
                if(u.kind!=expected_kind())throw CompileError({},"SIR-C verifier: concrete undo kind disagrees with semantic rollback action");
                if(u.kind==SirUndoKind::RestoreField){
                    if(op.opcode!=SirOpcode::FieldStore||op.operands.size()<2||u.target_value!=op.operands[0]||u.aux_value!=op.operands[1])throw CompileError({},"SIR-C verifier: restore-field undo does not match FieldStore operands");
                    auto pos=op.semantic_name.find('.');if(pos==std::string::npos)throw CompileError({},"SIR-C verifier: restore-field lacks owner.field identity");auto li=layoutmap.find(op.semantic_name.substr(0,pos));if(li==layoutmap.end()||op.tag>=li->second->field_offsets.size()||u.byte_offset!=li->second->field_offsets[op.tag])throw CompileError({},"SIR-C verifier: restore-field undo byte offset disagrees with concrete layout");
                    auto ac=cvmap.find(op.operands[1]);if(ac==cvmap.end()||u.size!=ac->second->size||u.align!=ac->second->align||!u.size||!u.align||(u.align&(u.align-1)))throw CompileError({},"SIR-C verifier: restore-field undo size/alignment disagrees with stored value");
                    if(u.semantic_name!=op.semantic_name)throw CompileError({},"SIR-C verifier: restore-field semantic identity mismatch");
                }else if(u.kind==SirUndoKind::RelationUnlink){
                    if(op.opcode!=SirOpcode::RelationLink||op.operands.size()<2||u.target_value!=op.operands[0]||u.aux_value!=op.operands[1]||u.semantic_name!=op.semantic_name)throw CompileError({},"SIR-C verifier: relation-unlink undo disagrees with RelationLink source");
                }else if(u.kind==SirUndoKind::RelationLink){
                    if(op.opcode!=SirOpcode::RelationUnlink||op.operands.size()<2||u.target_value!=op.operands[0]||u.aux_value!=op.operands[1]||u.semantic_name!=op.semantic_name)throw CompileError({},"SIR-C verifier: relation-link undo disagrees with RelationUnlink source");
                }else if(u.kind==SirUndoKind::RestoreOwner){
                    if((op.opcode!=SirOpcode::MoveValue&&op.opcode!=SirOpcode::SendValue&&op.opcode!=SirOpcode::FieldMove)||op.operands.empty()||u.target_value!=op.operands[0])throw CompileError({},"SIR-C verifier: restore-owner undo disagrees with ownership-transfer source");
                    if(op.opcode==SirOpcode::FieldMove){auto pos=op.semantic_name.find('.');if(pos==std::string::npos)throw CompileError({},"SIR-C verifier: field-move restore-owner lacks owner.field identity");auto li=layoutmap.find(op.semantic_name.substr(0,pos));if(li==layoutmap.end()||op.tag>=li->second->field_offsets.size()||u.aux_value!=op.result||u.byte_offset!=li->second->field_offsets[op.tag]||u.size!=8||u.align!=8)throw CompileError({},"SIR-C verifier: field-move restore-owner concrete restoration mismatch");}
                }else if(u.kind==SirUndoKind::DestroyResource){
                    if(op.opcode!=SirOpcode::PoolAlloc||u.target_value!=op.result||!cvmap.contains(u.target_value))throw CompileError({},"SIR-C verifier: destroy-created undo disagrees with PoolAlloc source");
                }else if(u.kind==SirUndoKind::SharedRelease){
                    if(op.opcode!=SirOpcode::SharedRetain||u.target_value!=op.result)throw CompileError({},"SIR-C verifier: shared-release undo disagrees with SharedRetain source");
                }
            }
            if(undo_ids.size()!=fn.rollback_obligations.size())throw CompileError({},"SIR-C verifier: every SIR-S rollback obligation requires one executable undo plan");

            std::map<SirRegionId,const SirRegion*> rmap;for(const auto&r:fn.regions)rmap[r.id]=&r;
            auto parallel_parent=[&](SirRegionId rid){std::set<SirRegionId>seen;while(rid&&seen.insert(rid).second){auto ri=rmap.find(rid);if(ri==rmap.end())break;if(ri->second->kind=="parallel")return rid;rid=ri->second->parent;}return SirRegionId{0};};
            std::set<SirRegionId> semantic_tasks;for(const auto&r:fn.regions)if(r.kind=="task")semantic_tasks.insert(r.id);
            std::set<std::uint32_t> task_ids;std::set<SirRegionId> planned_task_regions;
            for(const auto&t:cf.tasks){
                if(!t.id||!task_ids.insert(t.id).second||!planned_task_regions.insert(t.task_region).second)throw CompileError({},"SIR-C verifier: duplicate/zero concrete task plan");
                if(!semantic_tasks.contains(t.task_region)||!rmap.contains(t.parallel_region)||rmap[t.parallel_region]->kind!="parallel"||parallel_parent(rmap[t.task_region]->parent)!=t.parallel_region)throw CompileError({},"SIR-C verifier: concrete task region/parallel parent mismatch");
                auto bi=opmap.find(t.begin_op),ei=opmap.find(t.end_op);if(bi==opmap.end()||ei==opmap.end()||bi->second.second->opcode!=SirOpcode::TaskBegin||ei->second.second->opcode!=SirOpcode::TaskEnd||bi->second.second->region!=t.task_region||ei->second.second->region!=t.task_region)throw CompileError({},"SIR-C verifier: concrete task begin/end operation mismatch");
            }
            if(planned_task_regions!=semantic_tasks)throw CompileError({},"SIR-C verifier: every semantic task region requires exactly one concrete task plan");

            std::set<std::uint32_t> ccap_ids;
            if(cf.task_captures.size()!=fn.task_captures.size())throw CompileError({},"SIR-C verifier: concrete task capture count disagrees with SIR-S");
            for(const auto&c:cf.task_captures){
                if(!c.id||!ccap_ids.insert(c.id).second)throw CompileError({},"SIR-C verifier: duplicate/zero concrete task capture");
                auto sc=std::find_if(fn.task_captures.begin(),fn.task_captures.end(),[&](const auto&x){return x.id==c.id;});
                if(sc==fn.task_captures.end()||sc->parallel_region!=c.parallel_region||sc->task_region!=c.task_region||sc->value!=c.value||sc->provenance!=c.provenance||sc->mode!=c.mode)throw CompileError({},"SIR-C verifier: concrete task capture disagrees with SIR-S capture contract");
                if(!planned_task_regions.contains(c.task_region))throw CompileError({},"SIR-C verifier: concrete task capture names unplanned task region");
            }
        }
    }
    if(plans.size()!=sir.functions.size())throw CompileError({},"SIR-C verifier: every function requires one concrete representation plan");
}

std::string serialize_sir_s(const SirSProgram&sir){SirVerifier{}.verify(sir);std::ostringstream o;o<<"SIR-S "<<sir.format_major<<' '<<sir.format_minor<<'\n'<<"module "<<hex_encode(sir.module_name)<<'\n';serialize_schemas(o,sir.faults,sir.datasets,sir.choices,sir.relations,sir.pools);for(const auto&f:sir.semantic_facts)o<<"fact "<<hex_encode(f)<<'\n';for(const auto&fn:sir.functions)serialize_function(o,fn);o<<"end\n";return o.str();}
std::string serialize_sir_c(const SirCProgram&sir){
    SirVerifier{}.verify(sir);std::ostringstream o;o<<"SIR-C "<<sir.format_major<<' '<<sir.format_minor<<'\n'<<"module "<<hex_encode(sir.module_name)<<'\n';
    o<<"target "<<hex_encode(sir.target_profile)<<' '<<hex_encode(sir.target_triple)<<' '<<sir.pointer_bits<<' '<<hex_encode(sir.object_format)<<' '<<hex_encode(sir.executable_format)<<' '<<hex_encode(sir.calling_convention)<<' '<<hex_encode(sir.aggregate_representation)<<' '<<hex_encode(sir.cardinality_representation)<<' '<<hex_encode(sir.fault_payload_representation)<<'\n';
    serialize_schemas(o,sir.faults,sir.datasets,sir.choices,sir.relations,sir.pools);
    for(const auto&l:sir.concrete_layouts){o<<"layout "<<hex_encode(l.name)<<' '<<sir_concrete_kind_name(l.kind)<<' '<<l.size<<' '<<l.align<<' '<<l.payload_offset<<' '<<l.element_size<<' '<<l.element_align<<' '<<l.capacity<<' '<<l.field_offsets.size()<<'\n';for(auto x:l.field_offsets)o<<"loffset "<<x<<'\n';}
    for(const auto&cf:sir.concrete_functions){
        o<<"concretefn "<<hex_encode(cf.name)<<' '<<sir_storage_class_name(cf.result_storage)<<' '<<sir_concrete_kind_name(cf.result_kind)<<' '<<sir_result_transfer_kind_name(cf.result_transfer)<<' '<<cf.result_size<<' '<<cf.result_align<<' '<<cf.fault_payload_size<<' '<<cf.fault_payload_align<<' '<<cf.values.size()<<' '<<cf.resources.size()<<' '<<cf.resource_components.size()<<' '<<cf.cleanup_actions.size()<<' '<<cf.rollback_actions.size()<<' '<<cf.undo_actions.size()<<' '<<cf.tasks.size()<<' '<<cf.task_captures.size()<<'\n';
        for(const auto&v:cf.values)o<<"cvalue "<<v.value<<' '<<sir_storage_class_name(v.storage)<<' '<<sir_concrete_kind_name(v.kind)<<' '<<v.size<<' '<<v.align<<' '<<v.payload_offset<<' '<<v.element_size<<' '<<v.capacity<<' '<<v.byte_offset<<' '<<v.alias_of<<' '<<(v.escapes?1:0)<<' '<<(v.scalar_replaced?1:0)<<'\n';
        for(const auto&r:cf.resources)o<<"resource "<<r.id<<' '<<r.value<<' '<<r.provenance<<' '<<r.lifetime_region<<' '<<sir_cleanup_kind_name(r.cleanup)<<' '<<r.size<<' '<<r.align<<' '<<(r.automatic?1:0)<<' '<<(r.dynamic_many?1:0)<<' '<<(r.shared?1:0)<<' '<<r.absorbed_by<<' '<<hex_encode(r.resource_family)<<'\n';
        for(const auto&c:cf.resource_components)o<<"component "<<c.id<<' '<<c.owner_value<<' '<<c.owner_provenance<<' '<<sir_cleanup_kind_name(c.cleanup)<<' '<<sir_component_guard_name(c.guard)<<' '<<c.byte_offset<<' '<<c.tag<<' '<<c.element_stride<<' '<<hex_encode(c.type_name)<<' '<<hex_encode(c.path)<<'\n';
        for(const auto&a:cf.cleanup_actions)o<<"cleanup "<<a.resource_id<<' '<<a.from_block<<' '<<a.to_block<<' '<<a.source_op<<' '<<(a.fault_exit?1:0)<<' '<<(a.transfer?1:0)<<' '<<hex_encode(a.reason)<<'\n';
        for(const auto&r:cf.rollback_actions)o<<"rollback "<<r.id<<' '<<r.transaction_region<<' '<<r.provenance<<' '<<r.source_op<<' '<<hex_encode(r.action)<<'\n';
        for(const auto&u:cf.undo_actions)o<<"undo "<<u.id<<' '<<u.transaction_region<<' '<<u.source_op<<' '<<sir_undo_kind_name(u.kind)<<' '<<u.target_value<<' '<<u.aux_value<<' '<<u.byte_offset<<' '<<u.size<<' '<<u.align<<' '<<hex_encode(u.semantic_name)<<'\n';
        for(const auto&t:cf.tasks)o<<"taskplan "<<t.id<<' '<<t.parallel_region<<' '<<t.task_region<<' '<<t.begin_op<<' '<<t.end_op<<'\n';
        for(const auto&c:cf.task_captures)o<<"ctaskcapture "<<c.id<<' '<<c.parallel_region<<' '<<c.task_region<<' '<<c.value<<' '<<c.provenance<<' '<<hex_encode(c.mode)<<'\n';
    }
    for (const auto& f : sir.semantic_facts) o << "fact " << hex_encode(f) << '\n';
    for (const auto& fn : sir.functions) serialize_function(o, fn);
    o << "end\n";
    return o.str();
}

SirSProgram deserialize_sir_s(const std::string&text){LineReader r(text);std::vector<std::string>w;if(!r.next(w)||w.size()!=3||w[0]!="SIR-S")r.error("expected SIR-S header");SirSProgram o;o.format_major=parse_num<std::uint32_t>(w[1],"format major");o.format_minor=parse_num<std::uint32_t>(w[2],"format minor");while(r.next(w)){if(w[0]=="module"){if(w.size()!=2)r.error("malformed module");o.module_name=hex_decode(w[1]);}else if(w[0]=="fault"||w[0]=="dataset"||w[0]=="choice"||w[0]=="relation"||w[0]=="pool")parse_top_schema(r,w,o.faults,o.datasets,o.choices,o.relations,o.pools);else if(w[0]=="fact"){if(w.size()!=2)r.error("malformed fact");o.semantic_facts.push_back(hex_decode(w[1]));}else if(w[0]=="function")o.functions.push_back(parse_function(r,w));else if(w[0]=="end")break;else r.error("unknown top-level record '"+w[0]+"'");}SirVerifier{}.verify(o);return o;}
SirCProgram deserialize_sir_c(const std::string&text){
    LineReader r(text);std::vector<std::string>w;if(!r.next(w)||w.size()!=3||w[0]!="SIR-C")r.error("expected SIR-C header");SirCProgram o;o.format_major=parse_num<std::uint32_t>(w[1],"format major");o.format_minor=parse_num<std::uint32_t>(w[2],"format minor");
    while(r.next(w)){
        if(w[0]=="module"){if(w.size()!=2)r.error("malformed module");o.module_name=hex_decode(w[1]);}
        else if(w[0]=="target"){if(w.size()!=10)r.error("malformed target");o.target_profile=hex_decode(w[1]);o.target_triple=hex_decode(w[2]);o.pointer_bits=parse_num<std::uint32_t>(w[3],"pointer bits");o.object_format=hex_decode(w[4]);o.executable_format=hex_decode(w[5]);o.calling_convention=hex_decode(w[6]);o.aggregate_representation=hex_decode(w[7]);o.cardinality_representation=hex_decode(w[8]);o.fault_payload_representation=hex_decode(w[9]);}
        else if(w[0]=="fault"||w[0]=="dataset"||w[0]=="choice"||w[0]=="relation"||w[0]=="pool")parse_top_schema(r,w,o.faults,o.datasets,o.choices,o.relations,o.pools);
        else if(w[0]=="layout"){if(w.size()!=10)r.error("malformed concrete layout");SirConcreteLayout l;l.name=hex_decode(w[1]);l.kind=sir_concrete_kind_from_name(w[2]);l.size=parse_num<std::uint32_t>(w[3],"layout size");l.align=parse_num<std::uint32_t>(w[4],"layout align");l.payload_offset=parse_num<std::uint32_t>(w[5],"payload offset");l.element_size=parse_num<std::uint32_t>(w[6],"element size");l.element_align=parse_num<std::uint32_t>(w[7],"element align");l.capacity=parse_num<std::uint32_t>(w[8],"capacity");auto n=parse_num<std::size_t>(w[9],"field offset count");for(std::size_t i=0;i<n;++i){if(!r.next(w)||w.size()!=2||w[0]!="loffset")r.error("expected loffset");l.field_offsets.push_back(parse_num<std::uint32_t>(w[1],"field offset"));}o.concrete_layouts.push_back(std::move(l));}
        else if(w[0]=="concretefn"){
            if(w.size()!=11&&w.size()!=12&&w.size()!=14&&w.size()!=17)r.error("malformed concrete function");
            const bool v6=w.size()==17;const bool v5=w.size()==14||v6;
            SirConcreteFunction cf;cf.name=hex_decode(w[1]);cf.result_storage=sir_storage_class_from_name(w[2]);cf.result_kind=sir_concrete_kind_from_name(w[3]);std::size_t k=4;if(w.size()==12||v5){cf.result_transfer=sir_result_transfer_kind_from_name(w[k++]);}cf.result_size=parse_num<std::uint32_t>(w[k++],"result size");cf.result_align=parse_num<std::uint32_t>(w[k++],"result align");cf.fault_payload_size=parse_num<std::uint32_t>(w[k++],"fault payload size");cf.fault_payload_align=parse_num<std::uint32_t>(w[k++],"fault payload align");
            auto nv=parse_num<std::size_t>(w[k++],"concrete value count"),nr=parse_num<std::size_t>(w[k++],"resource count");std::size_t nc=0,na=0,nrb=0,nu=0,nt=0,ntc=0;if(v5){nc=parse_num<std::size_t>(w[k++],"component count");na=parse_num<std::size_t>(w[k++],"cleanup count");nrb=parse_num<std::size_t>(w[k++],"rollback count");if(v6){nu=parse_num<std::size_t>(w[k++],"undo count");nt=parse_num<std::size_t>(w[k++],"task count");ntc=parse_num<std::size_t>(w[k++],"task capture count");}}else na=parse_num<std::size_t>(w[k++],"cleanup count");
            for(std::size_t i=0;i<nv;++i){if(!r.next(w)||w.size()!=13||w[0]!="cvalue")r.error("expected cvalue");SirConcreteValue v;v.value=parse_num<SirValueId>(w[1],"concrete value");v.storage=sir_storage_class_from_name(w[2]);v.kind=sir_concrete_kind_from_name(w[3]);v.size=parse_num<std::uint32_t>(w[4],"value size");v.align=parse_num<std::uint32_t>(w[5],"value align");v.payload_offset=parse_num<std::uint32_t>(w[6],"payload offset");v.element_size=parse_num<std::uint32_t>(w[7],"element size");v.capacity=parse_num<std::uint32_t>(w[8],"capacity");v.byte_offset=parse_num<std::uint32_t>(w[9],"byte offset");v.alias_of=parse_num<SirValueId>(w[10],"alias");v.escapes=parse_num<int>(w[11],"escapes")!=0;v.scalar_replaced=parse_num<int>(w[12],"scalar replaced")!=0;cf.values.push_back(v);}
            for(std::size_t i=0;i<nr;++i){if(!r.next(w)||(w.size()!=10&&w.size()!=13)||w[0]!="resource")r.error("expected resource");SirConcreteResource x;x.id=parse_num<std::uint32_t>(w[1],"resource id");x.value=parse_num<SirValueId>(w[2],"resource value");x.provenance=parse_num<SirProvenanceId>(w[3],"resource provenance");x.lifetime_region=parse_num<SirRegionId>(w[4],"resource lifetime region");x.cleanup=sir_cleanup_kind_from_name(w[5]);x.size=parse_num<std::uint32_t>(w[6],"resource size");x.align=parse_num<std::uint32_t>(w[7],"resource align");x.automatic=parse_num<int>(w[8],"automatic")!=0;x.dynamic_many=parse_num<int>(w[9],"dynamic many")!=0;if(w.size()==13){x.shared=parse_num<int>(w[10],"shared")!=0;x.absorbed_by=parse_num<std::uint32_t>(w[11],"absorbed by");x.resource_family=hex_decode(w[12]);}cf.resources.push_back(x);}
            for(std::size_t i=0;i<nc;++i){if(!r.next(w)||w.size()!=11||w[0]!="component")r.error("expected component");SirResourceComponent c;c.id=parse_num<std::uint32_t>(w[1],"component id");c.owner_value=parse_num<SirValueId>(w[2],"component owner value");c.owner_provenance=parse_num<SirProvenanceId>(w[3],"component owner provenance");c.cleanup=sir_cleanup_kind_from_name(w[4]);c.guard=sir_component_guard_from_name(w[5]);c.byte_offset=parse_num<std::uint32_t>(w[6],"component byte offset");c.tag=parse_num<std::uint32_t>(w[7],"component tag");c.element_stride=parse_num<std::uint32_t>(w[8],"component stride");c.type_name=hex_decode(w[9]);c.path=hex_decode(w[10]);cf.resource_components.push_back(std::move(c));}
            for(std::size_t i=0;i<na;++i){if(!r.next(w)||(w.size()!=7&&w.size()!=8)||w[0]!="cleanup")r.error("expected cleanup");SirCleanupAction a;a.resource_id=parse_num<std::uint32_t>(w[1],"cleanup resource");a.from_block=parse_num<SirBlockId>(w[2],"cleanup source block");a.to_block=parse_num<SirBlockId>(w[3],"cleanup target block");a.source_op=parse_num<SirOpId>(w[4],"cleanup source op");a.fault_exit=parse_num<int>(w[5],"cleanup fault exit")!=0;if(w.size()==8){a.transfer=parse_num<int>(w[6],"cleanup transfer")!=0;a.reason=hex_decode(w[7]);}else a.reason=hex_decode(w[6]);cf.cleanup_actions.push_back(std::move(a));}
            for(std::size_t i=0;i<nrb;++i){if(!r.next(w)||w.size()!=6||w[0]!="rollback")r.error("expected rollback");cf.rollback_actions.push_back({parse_num<std::uint32_t>(w[1],"rollback id"),parse_num<SirRegionId>(w[2],"transaction region"),parse_num<SirProvenanceId>(w[3],"rollback provenance"),parse_num<SirOpId>(w[4],"rollback source op"),hex_decode(w[5])});}
            for(std::size_t i=0;i<nu;++i){if(!r.next(w)||w.size()!=11||w[0]!="undo")r.error("expected undo");SirConcreteUndo u;u.id=parse_num<std::uint32_t>(w[1],"undo id");u.transaction_region=parse_num<SirRegionId>(w[2],"undo transaction region");u.source_op=parse_num<SirOpId>(w[3],"undo source op");u.kind=sir_undo_kind_from_name(w[4]);u.target_value=parse_num<SirValueId>(w[5],"undo target value");u.aux_value=parse_num<SirValueId>(w[6],"undo aux value");u.byte_offset=parse_num<std::uint32_t>(w[7],"undo byte offset");u.size=parse_num<std::uint32_t>(w[8],"undo size");u.align=parse_num<std::uint32_t>(w[9],"undo align");u.semantic_name=hex_decode(w[10]);cf.undo_actions.push_back(std::move(u));}
            for(std::size_t i=0;i<nt;++i){if(!r.next(w)||w.size()!=6||w[0]!="taskplan")r.error("expected taskplan");cf.tasks.push_back({parse_num<std::uint32_t>(w[1],"task id"),parse_num<SirRegionId>(w[2],"task parallel region"),parse_num<SirRegionId>(w[3],"task region"),parse_num<SirOpId>(w[4],"task begin op"),parse_num<SirOpId>(w[5],"task end op")});}
            for(std::size_t i=0;i<ntc;++i){if(!r.next(w)||w.size()!=7||w[0]!="ctaskcapture")r.error("expected ctaskcapture");cf.task_captures.push_back({parse_num<std::uint32_t>(w[1],"concrete task capture id"),parse_num<SirRegionId>(w[2],"capture parallel region"),parse_num<SirRegionId>(w[3],"capture task region"),parse_num<SirValueId>(w[4],"capture value"),parse_num<SirProvenanceId>(w[5],"capture provenance"),hex_decode(w[6])});}
            o.concrete_functions.push_back(std::move(cf));
        }
        else if(w[0]=="fact"){if(w.size()!=2)r.error("malformed fact");o.semantic_facts.push_back(hex_decode(w[1]));}
        else if(w[0]=="function")o.functions.push_back(parse_function(r,w));else if(w[0]=="end")break;else r.error("unknown top-level record '"+w[0]+"'");
    }
    SirVerifier{}.verify(o);return o;
}

} // namespace staze
