#include "staze/semantic.hpp"
#include "staze/diagnostic.hpp"

#include <algorithm>
#include <charconv>
#include <cctype>
#include <functional>
#include <limits>
#include <sstream>
#include <unordered_set>

namespace staze {

std::string cardinality_name(Cardinality c) {
    switch (c) {
        case Cardinality::One: return "One";
        case Cardinality::ZeroOrOne: return "ZeroOrOne";
        case Cardinality::Many: return "Many";
    }
    return "?";
}

static std::string base_type_name(const SemType& t) {
    switch (t.base) {
        case BaseType::Unit: return "unit";
        case BaseType::Bool: return "bool";
        case BaseType::I32: return "i32";
        case BaseType::I64: return "i64";
        case BaseType::U32: return "u32";
        case BaseType::U64: return "u64";
        case BaseType::Text: return "text";
        case BaseType::Nominal: return t.nominal;
    }
    return "?";
}

std::string type_name(const SemType& t) {
    const auto b = base_type_name(t);
    if (t.cardinality == Cardinality::ZeroOrOne) return "optional<" + b + ">";
    if (t.cardinality == Cardinality::Many) return "list<" + b + ">";
    return b;
}

bool is_exact_one(const SemType& t) { return t.cardinality == Cardinality::One; }
bool is_integer(const SemType& t) {
    if (!is_exact_one(t)) return false;
    return t.base == BaseType::I32 || t.base == BaseType::I64 || t.base == BaseType::U32 || t.base == BaseType::U64;
}
bool is_signed_integer(const SemType& t) { return is_exact_one(t) && (t.base == BaseType::I32 || t.base == BaseType::I64); }
unsigned integer_bits(const SemType& t) {
    if (!is_exact_one(t)) return 0;
    return (t.base == BaseType::I32 || t.base == BaseType::U32) ? 32u :
           (t.base == BaseType::I64 || t.base == BaseType::U64) ? 64u : 0u;
}

SemType SemanticAnalyzer::resolve_type(const TypeRef& t) {
    auto scalar = [&](BaseType b) { return SemType{b, {}, Cardinality::One}; };
    if (t.name == "unit") return scalar(BaseType::Unit);
    if (t.name == "bool") return scalar(BaseType::Bool);
    if (t.name == "i32") return scalar(BaseType::I32);
    if (t.name == "i64") return scalar(BaseType::I64);
    if (t.name == "u32") return scalar(BaseType::U32);
    if (t.name == "u64") return scalar(BaseType::U64);
    if (t.name == "text") return scalar(BaseType::Text);
    if (t.name == "optional" || t.name == "list") {
        if (t.args.size() != 1) throw CompileError(t.where, t.name + " requires exactly one element type in Compiler 0.8");
        auto inner = resolve_type(t.args[0]);
        if (inner.cardinality != Cardinality::One) throw CompileError(t.where, "nested cardinality containers are not canonical in Compiler 0.8");
        inner.cardinality = t.name == "optional" ? Cardinality::ZeroOrOne : Cardinality::Many;
        return inner;
    }
    return {BaseType::Nominal, t.name, Cardinality::One};
}

static void merge_faults(std::set<std::string>& a, const std::set<std::string>& b) { a.insert(b.begin(), b.end()); }

static bool fits_int(std::int64_t v, const SemType& t) {
    if (!is_exact_one(t)) return false;
    switch (t.base) {
        case BaseType::I32: return v >= std::numeric_limits<std::int32_t>::min() && v <= std::numeric_limits<std::int32_t>::max();
        case BaseType::I64: return true;
        case BaseType::U32: return v >= 0 && static_cast<std::uint64_t>(v) <= std::numeric_limits<std::uint32_t>::max();
        case BaseType::U64: return v >= 0;
        default: return false;
    }
}

static std::pair<std::int64_t, std::optional<SemType>> parse_int_literal(const std::string& s, SourceLocation w) {
    std::size_t cut = 0;
    while (cut < s.size() && std::isdigit(static_cast<unsigned char>(s[cut]))) ++cut;
    std::int64_t v = 0;
    auto [ptr, ec] = std::from_chars(s.data(), s.data() + cut, v);
    if (ec != std::errc{} || ptr != s.data() + cut) throw CompileError(w, "integer literal exceeds Compiler-0.7 64-bit parser range");
    std::optional<SemType> suffix;
    auto tail = s.substr(cut);
    if (!tail.empty()) {
        if (tail == "i32") suffix = SemType{BaseType::I32, {}, Cardinality::One};
        else if (tail == "i64") suffix = SemType{BaseType::I64, {}, Cardinality::One};
        else if (tail == "u32") suffix = SemType{BaseType::U32, {}, Cardinality::One};
        else if (tail == "u64") suffix = SemType{BaseType::U64, {}, Cardinality::One};
        else throw CompileError(w, "Compiler 0.8 executable integer suffixes are i32/i64/u32/u64");
    }
    return {v, suffix};
}

static std::string prefix_before_last_dot(const std::string& s) {
    const auto p = s.rfind('.');
    return p == std::string::npos ? std::string{} : s.substr(0, p);
}
static std::string suffix_after_last_dot(const std::string& s) {
    const auto p = s.rfind('.');
    return p == std::string::npos ? s : s.substr(p + 1);
}

SSLProgram SemanticAnalyzer::analyze(const Program& p) const {
    if (p.staze_version != 3) throw CompileError({}, "internal: non-v3 AST");
    SSLProgram ssl;
    ssl.module_name = p.module_name;
    ssl.fault_codes = {{"ArithmeticOverflow", 1}, {"DivideByZero", 2}, {"IoFailure", 3}};
    ssl.fault_facts["ArithmeticOverflow"] = {1, {}};
    ssl.fault_facts["DivideByZero"] = {2, {}};
    ssl.fault_facts["IoFailure"] = {3, {}};

    std::vector<std::string> userfaults;
    std::unordered_set<std::string> faultnames{"ArithmeticOverflow", "DivideByZero", "IoFailure"};
    for (const auto& f : p.faults) {
        if (!faultnames.insert(f.name).second) throw CompileError(f.where, "duplicate/reserved fault identity '" + f.name + "'");
        userfaults.push_back(f.name);
    }
    std::sort(userfaults.begin(), userfaults.end());
    int code = 100;
    for (const auto& name : userfaults) ssl.fault_codes[name] = code++;
    for (const auto& f : p.faults) {
        FaultFacts ff;
        ff.code = ssl.fault_codes.at(f.name);
        std::unordered_set<std::string> names;
        for (const auto& fld : f.fields) {
            if (!names.insert(fld.name).second) throw CompileError(f.where, "duplicate fault payload field '" + fld.name + "'");
            ff.payload.push_back({fld.name, resolve_type(fld.type), false, false});
        }
        ssl.fault_facts[f.name] = ff;
        std::ostringstream os; os << "fault " << f.name << " code=" << ff.code << " payload=(";
        for (std::size_t i = 0; i < ff.payload.size(); ++i) { if (i) os << ','; os << ff.payload[i].name << ':' << type_name(ff.payload[i].type); }
        os << ')'; ssl.facts_text.push_back(os.str());
    }

    std::unordered_set<std::string> top_names;
    for (const auto& d : p.datasets) {
        if (!top_names.insert(d.name).second) throw CompileError(d.where, "duplicate top-level declaration '" + d.name + "'");
        DatasetFacts df;
        std::unordered_set<std::string> fields;
        for (const auto& f : d.fields) {
            if (!fields.insert(f.name).second) throw CompileError(f.where, "duplicate dataset field '" + f.name + "'");
            if (f.is_key) {
                if (df.key) throw CompileError(f.where, "dataset may declare only one key in Compiler 0.8");
                df.key = f.name; df.entity = true;
            }
            df.fields.push_back({f.name, resolve_type(f.type), f.is_key, f.revisable});
        }
        for (const auto& i : d.indexes) {
            if (!fields.contains(i)) throw CompileError(d.where, "index names unknown field '" + i + "'");
            df.indexes.push_back(i);
        }
        ssl.datasets[d.name] = df;
        ssl.facts_text.push_back("dataset " + d.name + " entity=" + (df.entity ? "true" : "false") + (df.key ? " key=" + *df.key : ""));
    }

    for (const auto& c : p.choices) {
        if (!top_names.insert(c.name).second) throw CompileError(c.where, "duplicate top-level declaration '" + c.name + "'");
        ChoiceFacts cf;
        std::unordered_set<std::string> cases;
        for (const auto& arm : c.cases) {
            if (!cases.insert(arm.name).second) throw CompileError(arm.where, "duplicate choice case '" + arm.name + "'");
            ChoiceCaseFacts af; af.name = arm.name;
            std::unordered_set<std::string> fields;
            for (const auto& f : arm.fields) {
                if (!fields.insert(f.name).second) throw CompileError(f.where, "duplicate choice payload field '" + f.name + "'");
                af.fields.push_back({f.name, resolve_type(f.type), false, false});
            }
            cf.cases.push_back(std::move(af));
        }
        if (cf.cases.empty()) throw CompileError(c.where, "choice must declare at least one case");
        ssl.choices[c.name] = cf;
        ssl.facts_text.push_back("choice " + c.name + " cases=" + std::to_string(cf.cases.size()));
    }

    for (const auto& r : p.relations) {
        if (!top_names.insert(r.name).second) throw CompileError(r.where, "duplicate top-level declaration '" + r.name + "'");
        if (!ssl.datasets.contains(r.source_type.name) || !ssl.datasets.contains(r.target_type.name))
            throw CompileError(r.where, "Compiler 0.8 relation endpoints must name declared datasets");
        static const std::unordered_set<std::string> cards{"zero", "zero_or_one", "one", "many"};
        if (!cards.contains(r.source_cardinality) || !cards.contains(r.target_cardinality))
            throw CompileError(r.where, "relation must declare source/target cardinality as zero/zero_or_one/one/many");
        static const std::unordered_set<std::string> owns{"none", "establish", "transfer"};
        if (!owns.contains(r.ownership)) throw CompileError(r.where, "relation ownership must be none/establish/transfer");
        ssl.relations[r.name] = {r.source_type.name, r.target_type.name, r.source_cardinality, r.target_cardinality,
                                 r.ownership, r.unique_pair, r.reverse_index};
        ssl.facts_text.push_back("relation " + r.name + " " + r.source_type.name + "->" + r.target_type.name +
                                 " cardinality=" + r.source_cardinality + "/" + r.target_cardinality + " ownership=" + r.ownership);
    }

    for (const auto& pool : p.pools) {
        if (!top_names.insert(pool.name).second) throw CompileError(pool.where, "duplicate top-level declaration '" + pool.name + "'");
        PoolFacts pf;
        std::unordered_set<std::string> fieldnames;
        for (const auto& f : pool.fields) {
            if (!fieldnames.insert(f.name).second) throw CompileError(f.where, "duplicate pool field '" + f.name + "'");
            pf.fields.push_back({f.name, resolve_type(f.type), false, f.revisable});
        }
        for (const auto& d : pool.directives) {
            if (d.name == "lifetime") { if (pf.lifetime) throw CompileError(d.where, "duplicate pool lifetime directive"); if (!d.argument) throw CompileError(d.where, "@lifetime requires an argument"); pf.lifetime = *d.argument; }
            else if (d.name == "reclaim") { if (pf.reclaim) throw CompileError(d.where, "duplicate pool reclaim directive"); if (!d.argument) throw CompileError(d.where, "@reclaim requires an argument"); pf.reclaim = *d.argument; }
            else if (d.name == "ownership") {
                if (!d.argument) throw CompileError(d.where, "@ownership requires unique or shared");
                if (*d.argument != "unique" && *d.argument != "shared") throw CompileError(d.where, "@ownership must be unique or shared");
                pf.ownership = *d.argument;
            } else if (d.name == "arena" || d.name == "heap" || d.name == "stack" || d.name == "static") {
                if (pf.allocation) throw CompileError(d.where, "multiple explicit pool allocation directives");
                pf.allocation = d.name;
            } else throw CompileError(d.where, "Compiler 0.8 recognizes pool directives @lifetime, @reclaim, @ownership, @arena, @heap, @stack, @static");
        }
        if (pf.lifetime) {
            static const std::unordered_set<std::string> lifetimes{"call", "frame", "scope", "transaction", "static"};
            if (!lifetimes.contains(*pf.lifetime)) throw CompileError(pool.where, "Compiler 0.8 pool lifetime must be call/frame/scope/transaction/static");
        }
        if (pf.reclaim) {
            static const std::unordered_set<std::string> reclaims{"manual", "scope", "end", "call"};
            if (!reclaims.contains(*pf.reclaim)) throw CompileError(pool.where, "Compiler 0.8 pool reclaim must be manual/scope/end/call");
        }
        if (pf.ownership == "shared" && (!pf.allocation || *pf.allocation != "heap"))
            throw CompileError(pool.where, "@ownership(shared) requires explicit @heap storage in Compiler 0.8");
        ssl.pools[pool.name] = pf;
        ssl.facts_text.push_back("pool " + pool.name + " lifetime=" + pf.lifetime.value_or("unspecified") +
                                 " allocation=" + pf.allocation.value_or("free") + " reclaim=" + pf.reclaim.value_or("policy") + " ownership=" + pf.ownership);
    }

    std::unordered_map<std::string, const InstructionDecl*> decls;
    for (const auto& fn : p.instructions) {
        if (!decls.emplace(fn.name, &fn).second) throw CompileError(fn.where, "duplicate instruction family '" + fn.name + "'");
        InstructionFacts facts;
        facts.result_type = resolve_type(fn.result_type);
        facts.result_borrow_mode = fn.result_borrow_mode;
        facts.result_borrow_from = fn.result_borrow_from.value_or("");
        if (fn.result_borrow_from) {
            auto pit = std::find_if(fn.params.begin(), fn.params.end(), [&](const auto& p){ return p.name == *fn.result_borrow_from; });
            if (pit == fn.params.end()) throw CompileError(fn.where, "result borrow contract names unknown parameter '" + *fn.result_borrow_from + "'");
            const auto required = fn.result_borrow_mode == "unique-mut" ? std::string("borrow_mut") : std::string("borrow");
            if (pit->authority != required) throw CompileError(fn.where, "result borrow contract requires source parameter authority '" + required + "'");
            if (resolve_type(pit->type) != facts.result_type) throw CompileError(fn.where, "result borrow contract currently requires result type to equal source parameter type");
        }
        facts.declared_faults.insert(fn.faults.begin(), fn.faults.end());
        for (const auto& f : fn.faults) if (!ssl.fault_codes.contains(f)) throw CompileError(fn.where, "instruction declares unknown fault '" + f + "'");
        ssl.instructions.emplace(fn.name, std::move(facts));
    }

    if (!decls.contains("main")) throw CompileError({}, "hosted executable requires public instruction main[] -> i32 or unit");
    const auto& main = *decls.at("main");
    if (!main.is_public) throw CompileError(main.where, "main must be public");
    if (!main.params.empty()) throw CompileError(main.where, "Compiler 0.8 executable entry supports main[] only");
    const auto main_type = resolve_type(main.result_type);
    if (main_type.cardinality != Cardinality::One || (main_type.base != BaseType::I32 && main_type.base != BaseType::Unit))
        throw CompileError(main.where, "hosted main must return exact-One i32 or unit");

    // Resource-family classification is semantic, not a target layout decision.
    // It tells SSL whether a value carries a lifetime obligation. Concrete byte
    // layout remains entirely downstream in SIR-C.
    std::function<std::string(const SemType&, std::set<std::string>&)> resource_family_inner;
    resource_family_inner = [&](const SemType& t, std::set<std::string>& seen) -> std::string {
        SemType one = t; one.cardinality = Cardinality::One;
        std::string base;
        if (one.base == BaseType::Nominal) {
            if (auto pit = ssl.pools.find(one.nominal); pit != ssl.pools.end()) {
                if (pit->second.allocation && *pit->second.allocation == "heap")
                    base = (pit->second.ownership == "shared" ? "shared-pool:" : "heap-pool:") + one.nominal;
                else if (seen.insert(one.nominal).second) {
                    for (const auto& f : pit->second.fields) {
                        if (!resource_family_inner(f.type, seen).empty()) { base = "composite:" + one.nominal; break; }
                    }
                    seen.erase(one.nominal);
                }
            } else if (auto dit = ssl.datasets.find(one.nominal); dit != ssl.datasets.end()) {
                if (seen.insert(one.nominal).second) {
                    for (const auto& f : dit->second.fields) {
                        if (!resource_family_inner(f.type, seen).empty()) { base = "composite:" + one.nominal; break; }
                    }
                    seen.erase(one.nominal);
                }
            } else if (auto cit = ssl.choices.find(one.nominal); cit != ssl.choices.end()) {
                if (seen.insert(one.nominal).second) {
                    for (const auto& arm : cit->second.cases) for (const auto& f : arm.fields) {
                        if (!resource_family_inner(f.type, seen).empty()) { base = "composite:" + one.nominal; break; }
                    }
                    seen.erase(one.nominal);
                }
            }
        }
        if (base.empty()) return {};
        if (t.cardinality == Cardinality::ZeroOrOne) return "optional:" + base;
        if (t.cardinality == Cardinality::Many) return "many:" + base;
        return base;
    };
    auto resource_family_for_type = [&](const SemType& t) { std::set<std::string> seen; return resource_family_inner(t, seen); };

    struct EnvVal {
        SemType type;
        bool revisable{false};
        std::set<std::string> authorities{"read"};
        std::optional<std::int64_t> constant_int;
        std::optional<bool> constant_bool;
        std::optional<std::string> constant_text;
        bool moved{false};
        bool released{false};
        std::string ownership{"value"};
        std::string resource_family;
        std::string borrow_root;
        std::string borrow_mode{"none"};
        std::set<std::string> moved_fields;
        std::set<std::string> active_shared_borrows;
        std::string active_mut_borrow;
    };

    for (const auto& fn : p.instructions) {
        auto& ff = ssl.instructions.at(fn.name);
        std::unordered_map<std::string, EnvVal> env;
        std::set<std::string> exposed;
        int loop_depth = 0;
        int transaction_depth = 0;
        int parallel_depth = 0;
        int task_depth = 0;

        for (const auto& param : fn.params) {
            auto t = resolve_type(param.type);
            if (t.base == BaseType::Text) throw CompileError(param.where, "Compiler 0.8 native parameter ABI does not yet admit text");
            std::set<std::string> auth{"read"};
            std::string ownership = "parameter";
            const auto family = resource_family_for_type(t);
            if (param.authority == "revise") auth.insert("revise");
            else if (param.authority == "move") { auth.insert("move"); auth.insert("send"); ownership = family.empty() ? "parameter" : "owned"; }
            else if (param.authority == "borrow") ownership = "borrow-shared";
            else if (param.authority == "borrow_mut") { auth.insert("revise"); ownership = "borrow-mut"; }
            else if (param.authority == "shared") { auth.insert("share"); auth.insert("send"); ownership = "shared"; }
            else if (param.authority == "send") { auth.insert("move"); auth.insert("send"); ownership = "sent"; }
            else if (param.authority != "read") auth.insert(param.authority);
            EnvVal ev; ev.type=t; ev.revisable=auth.contains("revise"); ev.authorities=auth; ev.ownership=ownership; ev.resource_family=family;
            if (!env.emplace(param.name, std::move(ev)).second) throw CompileError(param.where, "duplicate parameter '" + param.name + "'");
            ff.locals[param.name] = {t, auth.contains("revise"), auth, ownership, family};
        }

        struct FieldResolution {
            SemType type;
            bool revisable{false};
            std::set<std::string> authorities{"read"};
            std::string root_name;
            std::string owner_type;
            std::string field_name;
            std::string relative_path;
            std::string resource_family;
        };
        auto resolve_field_path = [&](const std::string& path, SourceLocation where) -> std::optional<FieldResolution> {
            const auto dot = path.find('.');
            if (dot == std::string::npos) return std::nullopt;
            const auto root_name = path.substr(0, dot);
            auto root_it = env.find(root_name);
            if (root_it == env.end()) return std::nullopt;
            if (root_it->second.moved) throw CompileError(where, "binding '" + root_name + "' was moved and may no longer be accessed");
            if (root_it->second.released) throw CompileError(where, "binding '" + root_name + "' was released and may no longer be accessed");
            if (!root_it->second.active_mut_borrow.empty()) throw CompileError(where, "binding '" + root_name + "' is exclusively borrowed by '" + root_it->second.active_mut_borrow + "'");
            SemType current = root_it->second.type;
            std::size_t start = dot + 1;
            FieldResolution out; out.root_name = root_name; out.authorities = {"read"}; out.relative_path = path.substr(dot + 1);
            while (start <= path.size()) {
                const auto next = path.find('.', start);
                const auto field_name = path.substr(start, next == std::string::npos ? std::string::npos : next - start);
                if (current.base != BaseType::Nominal || current.cardinality != Cardinality::One)
                    throw CompileError(where, "field access requires an exact-One nominal aggregate before '." + field_name + "'");
                const std::vector<FieldFacts>* fields = nullptr;
                if (auto it = ssl.datasets.find(current.nominal); it != ssl.datasets.end()) fields = &it->second.fields;
                else if (auto it = ssl.pools.find(current.nominal); it != ssl.pools.end()) fields = &it->second.fields;
                else throw CompileError(where, "type '" + current.nominal + "' does not expose addressable fields in Compiler 0.8");
                auto fit = std::find_if(fields->begin(), fields->end(), [&](const auto& f){ return f.name == field_name; });
                if (fit == fields->end()) throw CompileError(where, "unknown field '" + field_name + "' on '" + current.nominal + "'");
                out.owner_type = current.nominal; out.field_name = field_name; out.type = fit->type;
                out.revisable = fit->revisable && root_it->second.authorities.contains("revise");
                current = fit->type;
                if (next == std::string::npos) break;
                start = next + 1;
            }
            for (const auto& moved : root_it->second.moved_fields) {
                const bool same_or_below = out.relative_path == moved || out.relative_path.rfind(moved + ".", 0) == 0;
                const bool ancestor = moved.rfind(out.relative_path + ".", 0) == 0;
                if (same_or_below || ancestor) throw CompileError(where, "field path '" + path + "' intersects partially moved subobject '" + root_name + "." + moved + "'");
            }
            if (out.revisable) out.authorities.insert("revise");
            out.resource_family = resource_family_for_type(out.type);
            return out;
        };

        std::function<ExprFacts(const Expr&, std::optional<SemType>)> expr;
        expr = [&](const Expr& e, std::optional<SemType> expected) -> ExprFacts {
            ExprFacts out;
            std::visit([&](const auto& n) {
                using T = std::decay_t<decltype(n)>;
                if constexpr (std::is_same_v<T, IntExpr>) {
                    auto [v, suf] = parse_int_literal(n.literal, e.where);
                    SemType t = suf.value_or(expected.value_or(SemType{BaseType::I32, {}, Cardinality::One}));
                    if (!is_integer(t)) throw CompileError(e.where, "integer literal requires an exact-One integer context");
                    if (!fits_int(v, t)) throw CompileError(e.where, "integer literal is not representable as " + type_name(t));
                    out.type = t; out.constant_int = v;
                } else if constexpr (std::is_same_v<T, BoolExpr>) {
                    out.type = {BaseType::Bool, {}, Cardinality::One}; out.constant_bool = n.value;
                } else if constexpr (std::is_same_v<T, TextExpr>) {
                    out.type = {BaseType::Text, {}, Cardinality::One}; out.constant_text = n.value;
                } else if constexpr (std::is_same_v<T, NameExpr>) {
                    auto it = env.find(n.name);
                    if (it != env.end()) {
                        if (it->second.moved) throw CompileError(e.where, "binding '" + n.name + "' was moved and may no longer be read");
                        if (it->second.released) throw CompileError(e.where, "binding '" + n.name + "' was released and may no longer be read");
                        if (!it->second.active_mut_borrow.empty()) throw CompileError(e.where, "binding '" + n.name + "' is exclusively borrowed by '" + it->second.active_mut_borrow + "'");
                        if (!it->second.moved_fields.empty()) throw CompileError(e.where, "binding '" + n.name + "' is partially moved and cannot be read as a whole until the moved field is restored");
                        if (parallel_depth > 0 && !it->second.resource_family.empty() && it->second.ownership != "shared" && it->second.ownership != "sent" && it->second.ownership != "borrow-shared")
                            throw CompileError(e.where, "resource '" + n.name + "' enters parallel execution without an explicit shared/send capability transition");
                        out.type = it->second.type; out.authorities = it->second.authorities; out.ownership = it->second.ownership; out.resource_family = it->second.resource_family; out.borrow_root = it->second.borrow_root; out.borrow_mode = it->second.borrow_mode;
                        out.constant_int = it->second.constant_int; out.constant_bool = it->second.constant_bool; out.constant_text = it->second.constant_text;
                    } else if (auto field = resolve_field_path(n.name, e.where)) {
                        out.type = field->type; out.authorities = field->authorities; out.resource_family = field->resource_family;
                        if (!field->resource_family.empty()) { out.ownership = "borrow-shared"; out.borrow_root = field->root_name; out.borrow_mode = "shared-read"; out.authorities = {"read"}; }
                    } else throw CompileError(e.where, "unknown binding or field path '" + n.name + "'");
                } else if constexpr (std::is_same_v<T, UnaryExpr>) {
                    auto a = expr(*n.operand, expected); out.type = a.type; out.live_faults = a.live_faults; out.authorities = a.authorities;
                    if (n.op == UnaryOp::Not) {
                        if (a.type.base != BaseType::Bool || a.type.cardinality != Cardinality::One) throw CompileError(e.where, "'not' requires exact-One bool");
                        out.type = {BaseType::Bool, {}, Cardinality::One};
                    } else {
                        if (!is_signed_integer(a.type)) throw CompileError(e.where, "Compiler 0.8 unary '-' requires exact-One signed integer");
                        out.live_faults.insert("ArithmeticOverflow");
                    }
                } else if constexpr (std::is_same_v<T, BinaryExpr>) {
                    if (n.op == BinaryOp::And || n.op == BinaryOp::Or) {
                        auto a = expr(*n.lhs, SemType{BaseType::Bool, {}, Cardinality::One});
                        auto b = expr(*n.rhs, SemType{BaseType::Bool, {}, Cardinality::One});
                        if (a.type.base != BaseType::Bool || a.type.cardinality != Cardinality::One || b.type != a.type)
                            throw CompileError(e.where, "boolean operator requires exact-One bool operands");
                        out.type = a.type; merge_faults(out.live_faults, a.live_faults); merge_faults(out.live_faults, b.live_faults);
                    } else {
                        auto a = expr(*n.lhs, expected); auto b = expr(*n.rhs, a.type);
                        merge_faults(out.live_faults, a.live_faults); merge_faults(out.live_faults, b.live_faults);
                        if (a.type != b.type) throw CompileError(e.where, "binary operands must resolve to the same type");
                        if (n.op == BinaryOp::Add || n.op == BinaryOp::Sub || n.op == BinaryOp::Mul || n.op == BinaryOp::Div || n.op == BinaryOp::Mod) {
                            if (!is_integer(a.type)) throw CompileError(e.where, "Compiler 0.8 arithmetic supports exact-One integer types");
                            out.type = a.type;
                            if (n.op == BinaryOp::Add || n.op == BinaryOp::Sub || n.op == BinaryOp::Mul) out.live_faults.insert("ArithmeticOverflow");
                            else { out.live_faults.insert("DivideByZero"); if (is_signed_integer(a.type)) out.live_faults.insert("ArithmeticOverflow"); }
                        } else {
                            if (a.type.cardinality != Cardinality::One || !(is_integer(a.type) || a.type.base == BaseType::Bool))
                                throw CompileError(e.where, "comparison is implemented only for exact-One bool/integer values in Compiler 0.8");
                            out.type = {BaseType::Bool, {}, Cardinality::One};
                        }
                    }
                } else if constexpr (std::is_same_v<T, CallExpr>) {
                    if (n.callee == "stdout.write_text") {
                        if (n.args.size() != 1) throw CompileError(e.where, "stdout.write_text expects one text argument");
                        auto a = expr(n.args[0], SemType{BaseType::Text, {}, Cardinality::One});
                        if (a.type.base != BaseType::Text || a.type.cardinality != Cardinality::One || !a.constant_text)
                            throw CompileError(n.args[0].where, "Compiler 0.8 stdout.write_text requires a compile-time text literal/binding");
                        out.type = {BaseType::Unit, {}, Cardinality::One}; out.live_faults = a.live_faults; out.live_faults.insert("IoFailure");
                    } else if (n.callee == "some") {
                        if (n.args.size() != 1) throw CompileError(e.where, "some(value) expects exactly one value");
                        std::optional<SemType> elem;
                        if (expected) {
                            if (expected->cardinality != Cardinality::ZeroOrOne) throw CompileError(e.where, "some(...) requires an optional<T> context when a contextual type is supplied");
                            elem = *expected; elem->cardinality = Cardinality::One;
                        }
                        auto a = expr(n.args[0], elem); if (a.type.cardinality != Cardinality::One) throw CompileError(e.where, "some(...) payload must have cardinality One");
                        if(!a.resource_family.empty() && a.ownership!="shared" && !a.explicit_move) throw CompileError(n.args[0].where,"resource-bearing optional payload requires explicit move(...)");
                        if(a.ownership=="shared" && !a.explicit_retain) throw CompileError(n.args[0].where,"shared optional payload requires explicit retain(...)");
                        out.type = a.type; out.type.cardinality = Cardinality::ZeroOrOne; out.live_faults = a.live_faults;out.resource_family=a.resource_family.empty()?std::string{}:"optional:"+a.resource_family;out.ownership=out.resource_family.empty()?"value":"owned";out.authorities=out.resource_family.empty()?std::set<std::string>{"read"}:std::set<std::string>{"read","move","send"};
                    } else if (n.callee == "none") {
                        if (!n.args.empty()) throw CompileError(e.where, "none() takes no arguments");
                        if (!expected || expected->cardinality != Cardinality::ZeroOrOne) throw CompileError(e.where, "none() requires an optional<T> contextual type");
                        out.type = *expected; out.resource_family=resource_family_for_type(out.type); out.ownership=out.resource_family.empty()?"value":"owned"; if(!out.resource_family.empty())out.authorities={"read","move","send"};
                    } else if (n.callee == "many") {
                        SemType element;
                        if (expected) {
                            if (expected->cardinality != Cardinality::Many) throw CompileError(e.where, "many(...) requires a list<T> context when contextual type is supplied");
                            element = *expected; element.cardinality = Cardinality::One;
                        } else {
                            if (n.args.empty()) throw CompileError(e.where, "empty many() requires an explicit list<T> contextual type");
                            element = expr(n.args[0], std::nullopt).type;
                            if (element.cardinality != Cardinality::One) throw CompileError(e.where, "many(...) elements must have cardinality One");
                        }
                        for (const auto& aexpr : n.args) {
                            auto a = expr(aexpr, element); if (a.type != element) throw CompileError(aexpr.where, "many(...) element type mismatch");
                            if(!a.resource_family.empty() && a.ownership!="shared" && !a.explicit_move) throw CompileError(aexpr.where,"resource-bearing Many element requires explicit move(...)");
                            if(a.ownership=="shared" && !a.explicit_retain) throw CompileError(aexpr.where,"shared Many element requires explicit retain(...)");
                            merge_faults(out.live_faults, a.live_faults);
                        }
                        out.type = element; out.type.cardinality = Cardinality::Many;out.resource_family=resource_family_for_type(out.type);out.ownership=out.resource_family.empty()?"value":"owned";if(!out.resource_family.empty())out.authorities={"read","move","send"};
                    } else if (n.callee == "dynamic_many") {
                        if (!expected || expected->cardinality != Cardinality::Many) throw CompileError(e.where, "dynamic_many(capacity) requires a list<T> contextual type");
                        if (n.args.size() != 1) throw CompileError(e.where, "dynamic_many(capacity) expects exactly one u64 capacity");
                        auto cap = expr(n.args[0], SemType{BaseType::U64, {}, Cardinality::One});
                        if (cap.type != SemType{BaseType::U64, {}, Cardinality::One}) throw CompileError(n.args[0].where, "dynamic_many capacity must be u64");
                        if (!ssl.fault_facts.contains("AllocationFailure")) throw CompileError(e.where, "dynamic_many requires a declared fault AllocationFailure");
                        out.type = *expected; out.live_faults = cap.live_faults; out.live_faults.insert("AllocationFailure"); out.live_faults.insert("ArithmeticOverflow");
                        out.authorities = {"read", "move", "send", "revise"}; out.ownership="owned";out.resource_family="dynamic-many"; ff.effects.insert("memory.allocate");
                    } else if (n.callee == "many.push") {
                        if (transaction_depth > 0) throw CompileError(e.where, "Compiler 0.8 reference transactions do not yet permit dynamic Many mutation; use a value created outside the transaction or defer the push");
                        if (n.args.size() != 2) throw CompileError(e.where, "many.push(list, value) expects two arguments");
                        auto* named = std::get_if<NameExpr>(&n.args[0].node); if (!named) throw CompileError(n.args[0].where, "many.push requires a named revisable list binding");
                        auto it = env.find(named->name); if (it == env.end()) throw CompileError(n.args[0].where, "unknown many.push target '" + named->name + "'");
                        if (it->second.type.cardinality != Cardinality::Many) throw CompileError(n.args[0].where, "many.push target must be list<T>");
                        if (!it->second.authorities.contains("revise")) throw CompileError(n.args[0].where, "many.push requires revise authority on the list binding");
                        auto listv = expr(n.args[0], it->second.type); SemType element = it->second.type; element.cardinality = Cardinality::One;
                        auto item = expr(n.args[1], element); if (item.type != element) throw CompileError(n.args[1].where, "many.push element type mismatch");
                        merge_faults(out.live_faults, listv.live_faults); merge_faults(out.live_faults, item.live_faults);
                        if (!ssl.fault_facts.contains("AllocationFailure")) throw CompileError(e.where, "many.push requires a declared fault AllocationFailure");
                        out.live_faults.insert("AllocationFailure"); out.live_faults.insert("ArithmeticOverflow");
                        out.type = {BaseType::Unit, {}, Cardinality::One}; ff.effects.insert("memory.reallocate");
                    } else if (n.callee == "many.length") {
                        if (n.args.size() != 1) throw CompileError(e.where, "many.length(list) expects one argument");
                        auto listv = expr(n.args[0], std::nullopt); if (listv.type.cardinality != Cardinality::Many) throw CompileError(n.args[0].where, "many.length requires list<T>");
                        out.type = {BaseType::U64, {}, Cardinality::One}; out.live_faults = listv.live_faults;
                    } else if (n.callee == "borrow" || n.callee == "borrow_mut") {
                        if (n.args.size() != 1) throw CompileError(e.where, n.callee + "(value) expects one addressable value");
                        auto* named = std::get_if<NameExpr>(&n.args[0].node);
                        if (!named) throw CompileError(n.args[0].where, n.callee + " requires a named binding or field path so provenance can be proven");
                        std::string root = named->name;
                        bool field_revisable = false;
                        if (auto dot = root.find('.'); dot != std::string::npos) {
                            auto f = resolve_field_path(root, n.args[0].where); if (!f) throw CompileError(n.args[0].where, "unknown borrow field path '" + root + "'");
                            root = f->root_name; field_revisable = f->revisable;
                        }
                        auto rit = env.find(root); if (rit == env.end()) throw CompileError(n.args[0].where, "unknown borrow root '" + root + "'");
                        if (rit->second.moved || rit->second.released) throw CompileError(n.args[0].where, "borrow root '" + root + "' is no longer live");
                        if (n.callee == "borrow_mut") {
                            if (!rit->second.active_mut_borrow.empty() || !rit->second.active_shared_borrows.empty()) throw CompileError(n.args[0].where, "borrow_mut requires exclusive access; root '" + root + "' already has an active borrow");
                            if (!rit->second.authorities.contains("revise")) throw CompileError(n.args[0].where, "borrow_mut requires revise authority on root '" + root + "'");
                            if (named->name.find('.') != std::string::npos && !field_revisable) throw CompileError(n.args[0].where, "borrow_mut of field requires a revisable field");
                        } else if (!rit->second.active_mut_borrow.empty()) throw CompileError(n.args[0].where, "borrow requires shared-read access but root '" + root + "' has an active mutable borrow");
                        auto a = expr(n.args[0], expected); out.type = a.type; out.live_faults = a.live_faults; out.resource_family = a.resource_family.empty() ? resource_family_for_type(a.type) : a.resource_family;
                        out.borrow_root = root; out.borrow_mode = n.callee == "borrow_mut" ? "unique-mut" : "shared-read";
                        out.ownership = n.callee == "borrow_mut" ? "borrow-mut" : "borrow-shared";
                        out.authorities = n.callee == "borrow_mut" ? std::set<std::string>{"read","revise"} : std::set<std::string>{"read"};
                    } else if (n.callee == "retain" || n.callee == "share") {
                        if (n.args.size()!=1) throw CompileError(e.where, n.callee + "(value) expects one shared resource binding");
                        auto* named=std::get_if<NameExpr>(&n.args[0].node); if(!named || named->name.find('.')!=std::string::npos) throw CompileError(n.args[0].where, n.callee + " requires a whole named shared binding");
                        auto it=env.find(named->name); if(it==env.end()) throw CompileError(n.args[0].where,"unknown shared source '"+named->name+"'");
                        if(it->second.ownership!="shared" || !it->second.authorities.contains("share")) throw CompileError(n.args[0].where,n.callee+" requires explicit shared ownership");
                        auto a=expr(n.args[0],expected);out.type=a.type;out.live_faults=a.live_faults;out.ownership="shared";out.resource_family=a.resource_family;out.authorities={"read","share","send"};out.explicit_retain=true;ff.effects.insert("shared.retain");
                    } else if (n.callee == "release") {
                        if (transaction_depth > 0) throw CompileError(e.where, "Compiler 0.8 reference transactions cannot release an existing resource because destruction is not reversibly reconstructible");
                        if(n.args.size()!=1)throw CompileError(e.where,"release(value) expects one named resource or borrow");
                        auto*named=std::get_if<NameExpr>(&n.args[0].node);if(!named||named->name.find('.')!=std::string::npos)throw CompileError(n.args[0].where,"release currently requires a whole named binding");
                        auto it=env.find(named->name);if(it==env.end())throw CompileError(n.args[0].where,"unknown release target '"+named->name+"'");
                        if(it->second.released||it->second.moved)throw CompileError(n.args[0].where,"release target is already inactive");
                        // Record ordinary expression facts for the release operand as well. DLE is a detached
                        // consumer of SSL facts and must never have to rediscover the operand's type/ownership.
                        auto released_value = expr(n.args[0], it->second.type);
                        out.live_faults = released_value.live_faults;
                        if(it->second.ownership=="borrow-shared"||it->second.ownership=="borrow-mut"){
                            auto owner=env.find(it->second.borrow_root);if(owner!=env.end()){owner->second.active_shared_borrows.erase(named->name);if(owner->second.active_mut_borrow==named->name)owner->second.active_mut_borrow.clear();}
                            ff.effects.insert("borrow.end");
                        } else {
                            if(it->second.resource_family.empty())throw CompileError(n.args[0].where,"release requires a resource-bearing value");
                            if(it->second.resource_family.rfind("heap-pool:",0)==0||it->second.resource_family.rfind("shared-pool:",0)==0){auto pos=it->second.resource_family.find(':');auto pn=it->second.resource_family.substr(pos+1);auto pp=ssl.pools.find(pn);if(pp!=ssl.pools.end()&&pp->second.reclaim.value_or("scope")!="manual")throw CompileError(n.args[0].where,"explicit release of pool resource currently requires @reclaim(manual); automatic lifetimes remain path-owned");}
                            else throw CompileError(n.args[0].where,"explicit release currently targets manual heap/shared pool resources; composite/dynamic release remains lifetime-driven");
                            if(!it->second.active_mut_borrow.empty()||!it->second.active_shared_borrows.empty())throw CompileError(n.args[0].where,"release is illegal while borrows of the resource remain active");
                            ff.effects.insert(it->second.ownership=="shared"?"shared.release":"resource.release");
                        }
                        it->second.released=true;out.type={BaseType::Unit,{},Cardinality::One};out.ownership="value";
                    } else if (n.callee == "send") {
                        if(n.args.size()!=1)throw CompileError(e.where,"send(value) expects one named owned/shared resource");
                        auto*named=std::get_if<NameExpr>(&n.args[0].node);if(!named||named->name.find('.')!=std::string::npos)throw CompileError(n.args[0].where,"send requires a whole named binding");
                        auto it=env.find(named->name);if(it==env.end())throw CompileError(n.args[0].where,"unknown send source '"+named->name+"'");
                        if(!it->second.authorities.contains("send"))throw CompileError(n.args[0].where,"send requires send authority");
                        if(!it->second.active_mut_borrow.empty()||!it->second.active_shared_borrows.empty())throw CompileError(n.args[0].where,"send is illegal while borrows remain active");
                        auto a=expr(n.args[0],expected);out.type=a.type;out.live_faults=a.live_faults;out.authorities={"read","move","send"};out.ownership="sent";out.resource_family=a.resource_family;out.explicit_move=true;it->second.moved=true;ff.effects.insert("ownership.send");
                    } else if (n.callee == "move") {
                        if (n.args.size() != 1) throw CompileError(e.where, "move(value) expects one binding or addressable field");
                        auto* named = std::get_if<NameExpr>(&n.args[0].node); if (!named) throw CompileError(n.args[0].where, "Compiler 0.8 move(...) requires a named binding/field so move-state can be proven");
                        if(named->name.find('.')!=std::string::npos){
                            auto f=resolve_field_path(named->name,n.args[0].where);if(!f)throw CompileError(n.args[0].where,"unknown field move source");auto rit=env.find(f->root_name);
                            if(rit==env.end()||!rit->second.authorities.contains("move"))throw CompileError(n.args[0].where,"field move requires move authority on aggregate root");
                            if(!rit->second.active_mut_borrow.empty()||!rit->second.active_shared_borrows.empty())throw CompileError(n.args[0].where,"field move is illegal while aggregate borrows remain active");
                            if(f->resource_family.empty())throw CompileError(n.args[0].where,"partial move currently requires a resource-bearing field");
                            auto a=expr(n.args[0],expected);out.type=a.type;out.live_faults=a.live_faults;out.authorities={"read","move","send"};out.ownership="owned";out.resource_family=f->resource_family;out.explicit_move=true;rit->second.moved_fields.insert(f->relative_path);
                        } else {
                            auto it = env.find(named->name); if (it == env.end()) throw CompileError(n.args[0].where, "unknown move source '" + named->name + "'");
                            if (it->second.moved) throw CompileError(n.args[0].where, "binding '" + named->name + "' was already moved");
                            if (!it->second.authorities.contains("move")) throw CompileError(n.args[0].where, "move requires move authority");
                            if(!it->second.active_mut_borrow.empty()||!it->second.active_shared_borrows.empty())throw CompileError(n.args[0].where,"move is illegal while borrows remain active");
                            auto a = expr(n.args[0], expected); out.type = a.type; out.live_faults = a.live_faults; out.authorities = a.authorities;out.ownership=a.ownership=="shared"?"shared":"owned";out.resource_family=a.resource_family;out.explicit_move=true; it->second.moved = true;
                        }
                    } else if (ssl.datasets.contains(n.callee)) {
                        const auto& ds = ssl.datasets.at(n.callee);
                        if (n.args.size() != ds.fields.size()) throw CompileError(e.where, "dataset aggregate constructor '" + n.callee + "' expects " + std::to_string(ds.fields.size()) + " fields");
                        for (std::size_t i = 0; i < n.args.size(); ++i) {
                            auto a = expr(n.args[i], ds.fields[i].type); if (a.type != ds.fields[i].type) throw CompileError(n.args[i].where, "dataset field type mismatch for '" + ds.fields[i].name + "'");
                            if(!a.resource_family.empty()&&a.ownership!="shared"&&!a.explicit_move)throw CompileError(n.args[i].where,"resource-bearing dataset field '"+ds.fields[i].name+"' requires explicit move(...)");
                            if(a.ownership=="shared"&&!a.explicit_retain)
                                throw CompileError(n.args[i].where,"shared dataset field '"+ds.fields[i].name+"' requires explicit retain(...)");
                            merge_faults(out.live_faults, a.live_faults);
                        }
                        out.type = {BaseType::Nominal, n.callee, Cardinality::One}; out.authorities = {"read", "move", "send"}; out.resource_family=resource_family_for_type(out.type); out.ownership=out.resource_family.empty()?"value":"owned";
                    } else if (const auto owner = prefix_before_last_dot(n.callee); !owner.empty() && ssl.choices.contains(owner)) {
                        const auto arm_name = suffix_after_last_dot(n.callee); const auto& ch = ssl.choices.at(owner);
                        auto it = std::find_if(ch.cases.begin(), ch.cases.end(), [&](const auto& a){ return a.name == arm_name; });
                        if (it == ch.cases.end()) throw CompileError(e.where, "unknown choice case '" + n.callee + "'");
                        if (n.args.size() != it->fields.size()) throw CompileError(e.where, "choice constructor '" + n.callee + "' payload arity mismatch");
                        for (std::size_t i = 0; i < n.args.size(); ++i) { auto a = expr(n.args[i], it->fields[i].type); if (a.type != it->fields[i].type) throw CompileError(n.args[i].where, "choice payload type mismatch");if(!a.resource_family.empty()&&a.ownership!="shared"&&!a.explicit_move)throw CompileError(n.args[i].where,"resource-bearing choice payload requires explicit move(...)");if(a.ownership=="shared"&&!a.explicit_retain)throw CompileError(n.args[i].where,"shared choice payload requires explicit retain(...)"); merge_faults(out.live_faults, a.live_faults); }
                        out.type = {BaseType::Nominal, owner, Cardinality::One}; out.authorities = {"read", "move", "send"}; out.resource_family=resource_family_for_type(out.type); out.ownership=out.resource_family.empty()?"value":"owned";
                    } else if (const auto owner = prefix_before_last_dot(n.callee); !owner.empty() && ssl.pools.contains(owner) && suffix_after_last_dot(n.callee) == "alloc") {
                        const auto& pool = ssl.pools.at(owner);
                        if (n.args.size() != pool.fields.size()) throw CompileError(e.where, "pool allocation '" + n.callee + "' field arity mismatch");
                        for (std::size_t i = 0; i < n.args.size(); ++i) { auto a = expr(n.args[i], pool.fields[i].type); if (a.type != pool.fields[i].type) throw CompileError(n.args[i].where, "pool field type mismatch");if(!a.resource_family.empty()&&a.ownership!="shared"&&!a.explicit_move)throw CompileError(n.args[i].where,"resource-bearing pool field requires explicit move(...)");if(a.ownership=="shared"&&!a.explicit_retain)throw CompileError(n.args[i].where,"shared pool field requires explicit retain(...)"); merge_faults(out.live_faults, a.live_faults); }
                        out.type = {BaseType::Nominal, owner, Cardinality::One}; out.resource_family=resource_family_for_type(out.type);
                        out.ownership=pool.ownership=="shared"?"shared":"owned";
                        out.authorities = pool.ownership=="shared" ? std::set<std::string>{"read","revise","share","send"} : std::set<std::string>{"read","revise","move","send"}; ff.effects.insert("memory.allocate");
                        if (pool.allocation && *pool.allocation == "heap") {
                            if (!ssl.fault_facts.contains("AllocationFailure"))
                                throw CompileError(e.where, "@heap pool allocation requires a declared fault AllocationFailure so allocation failure is explicit");
                            out.live_faults.insert("AllocationFailure");
                        }
                    } else if (const auto owner = prefix_before_last_dot(n.callee); !owner.empty() && ssl.relations.contains(owner) && (suffix_after_last_dot(n.callee) == "link" || suffix_after_last_dot(n.callee) == "unlink")) {
                        if (task_depth > 0) throw CompileError(e.where, "Compiler 0.8 deterministic task profile does not permit relation mutation inside a task; route relation state through a later synchronized profile");
                        const auto& rel = ssl.relations.at(owner); if (n.args.size() != 2) throw CompileError(e.where, "relation link/unlink expects source,target");
                        auto a = expr(n.args[0], SemType{BaseType::Nominal, rel.source, Cardinality::One}); auto b = expr(n.args[1], SemType{BaseType::Nominal, rel.target, Cardinality::One});
                        if (a.type.nominal != rel.source || b.type.nominal != rel.target) throw CompileError(e.where, "relation endpoint type mismatch");
                        merge_faults(out.live_faults, a.live_faults); merge_faults(out.live_faults, b.live_faults); out.type = {BaseType::Unit, {}, Cardinality::One};
                        ff.effects.insert(suffix_after_last_dot(n.callee) == "link" ? "relation.link" : "relation.unlink");
                    } else {
                        auto di = decls.find(n.callee); if (di == decls.end()) throw CompileError(e.where, "unknown instruction or constructor '" + n.callee + "'");
                        const auto& callee = *di->second; if (callee.params.size() != n.args.size()) throw CompileError(e.where, "argument count mismatch calling '" + n.callee + "'");
                        std::vector<ExprFacts> actuals; actuals.reserve(n.args.size());
                        for (std::size_t i = 0; i < n.args.size(); ++i) {
                            auto want = resolve_type(callee.params[i].type); auto got = expr(n.args[i], want); if (got.type != want) throw CompileError(n.args[i].where, "argument type mismatch: expected " + type_name(want) + ", got " + type_name(got.type));
                            const auto mode=callee.params[i].authority;
                            if(mode=="borrow" && got.borrow_mode!="shared-read") throw CompileError(n.args[i].where,"parameter '"+callee.params[i].name+"' requires explicit borrow(...)");
                            if(mode=="borrow_mut" && got.borrow_mode!="unique-mut") throw CompileError(n.args[i].where,"parameter '"+callee.params[i].name+"' requires explicit borrow_mut(...)");
                            if(mode=="move" && !got.explicit_move) throw CompileError(n.args[i].where,"move parameter '"+callee.params[i].name+"' requires explicit move(...)");
                            if(mode=="shared" && got.ownership!="shared") throw CompileError(n.args[i].where,"shared parameter '"+callee.params[i].name+"' requires a shared value");
                            if(mode=="send" && got.ownership!="sent") throw CompileError(n.args[i].where,"send parameter '"+callee.params[i].name+"' requires explicit send(...)");
                            merge_faults(out.live_faults, got.live_faults); actuals.push_back(std::move(got));
                        }
                        out.type = resolve_type(callee.result_type); out.live_faults.insert(callee.faults.begin(), callee.faults.end()); out.resource_family=resource_family_for_type(out.type);
                        if(callee.result_borrow_from){auto pit=std::find_if(callee.params.begin(),callee.params.end(),[&](const auto&p){return p.name==*callee.result_borrow_from;});auto idx=static_cast<std::size_t>(std::distance(callee.params.begin(),pit));out.borrow_mode=callee.result_borrow_mode;out.borrow_root=actuals.at(idx).borrow_root;out.ownership=callee.result_borrow_mode=="unique-mut"?"borrow-mut":"borrow-shared";out.authorities=callee.result_borrow_mode=="unique-mut"?std::set<std::string>{"read","revise"}:std::set<std::string>{"read"};}

                    }
                }
            }, e.node);

            for (const auto& h : e.handlers) {
                if (!out.live_faults.contains(h.fault_name)) throw CompileError(h.where, "handler names fault '" + h.fault_name + "' that this expression does not expose");
                out.live_faults.erase(h.fault_name);
                if (h.kind == HandlerKind::Bypass) {
                    auto repl = expr(*h.replacement, out.type); if (repl.type != out.type) throw CompileError(h.where, "bypass replacement type must be " + type_name(out.type)); merge_faults(out.live_faults, repl.live_faults);
                } else {
                    // NC-3 delete algebra: Unit is omission/continue, One is illegal,
                    // ZeroOrOne becomes none, Many becomes empty contribution.
                    if (out.type.cardinality == Cardinality::One && out.type.base != BaseType::Unit)
                        throw CompileError(h.where, "delete is illegal for exact-One non-unit results");
                }
            }
            ssl.expressions[e.id] = out;
            return out;
        };

        std::function<bool(const Block&)> block;
        block = [&](const Block& b) -> bool {
            bool dead = false;
            for (const auto& s : b) {
                if (dead) throw CompileError(std::visit([](const auto& x){ return x.where; }, s.node), "unreachable statement after definite control transfer");
                std::visit([&](const auto& st) {
                    using T = std::decay_t<decltype(st)>;
                    if constexpr (std::is_same_v<T, LocalDecl>) {
                        auto got = expr(st.value, st.type ? std::optional<SemType>{resolve_type(*st.type)} : std::nullopt);
                        SemType t = st.type ? resolve_type(*st.type) : got.type;
                        if (got.type != t) throw CompileError(st.where, "binding initializer type mismatch");
                        std::set<std::string> auth = got.authorities; auth.insert("read"); if (st.revisable) auth.insert("revise");
                        EnvVal ev;ev.type=t;ev.revisable=st.revisable;ev.authorities=auth;ev.constant_int=got.constant_int;ev.constant_bool=got.constant_bool;ev.constant_text=got.constant_text;ev.ownership=got.ownership;ev.resource_family=got.resource_family.empty()?resource_family_for_type(t):got.resource_family;ev.borrow_root=got.borrow_root;ev.borrow_mode=got.borrow_mode;
                        if (!env.emplace(st.name, std::move(ev)).second) throw CompileError(st.where, "':=' creates a new binding; '" + st.name + "' already exists");
                        if(!got.borrow_root.empty()){auto owner=env.find(got.borrow_root);if(owner==env.end())throw CompileError(st.where,"borrow root disappeared before binding creation");if(got.borrow_mode=="unique-mut"){if(!owner->second.active_mut_borrow.empty()||!owner->second.active_shared_borrows.empty())throw CompileError(st.where,"mutable borrow conflicts with active borrow");owner->second.active_mut_borrow=st.name;}else if(got.borrow_mode=="shared-read"){if(!owner->second.active_mut_borrow.empty())throw CompileError(st.where,"shared borrow conflicts with mutable borrow");owner->second.active_shared_borrows.insert(st.name);}}
                        ff.locals[st.name] = {t, st.revisable, auth, got.ownership, got.resource_family.empty()?resource_family_for_type(t):got.resource_family}; merge_faults(exposed, got.live_faults);
                    } else if constexpr (std::is_same_v<T, SetStmt>) {
                        if (task_depth > 0) throw CompileError(st.where, "Compiler 0.8 task captures are immutable; set is not permitted inside a task region");
                        auto it = env.find(st.target);
                        if (it != env.end()) {
                            if (!it->second.authorities.contains("revise")) throw CompileError(st.where, "set requires revise authority");
                            auto v = expr(st.value, it->second.type); if (v.type != it->second.type) throw CompileError(st.where, "set replacement type mismatch"); merge_faults(exposed, v.live_faults);
                            it->second.constant_int.reset(); it->second.constant_bool.reset(); it->second.constant_text.reset();
                        } else if (auto field = resolve_field_path(st.target, st.where)) {
                            if (!field->revisable) throw CompileError(st.where, "set of field '" + st.target + "' requires a revisable field and revise authority on its aggregate");
                            auto v = expr(st.value, field->type); if (v.type != field->type) throw CompileError(st.where, "field set replacement type mismatch"); merge_faults(exposed, v.live_faults);
                        } else throw CompileError(st.where, "unknown set target '" + st.target + "'");
                        ff.effects.insert("revise");
                    } else if constexpr (std::is_same_v<T, ReviseStmt>) {
                        if (task_depth > 0) throw CompileError(st.where, "Compiler 0.8 task captures are immutable; revise is not permitted inside a task region");
                        auto it = env.find(st.target);
                        SemType target_type;
                        if (it != env.end()) {
                            if (!it->second.authorities.contains("revise")) throw CompileError(st.where, "revise requires revise authority");
                            target_type = it->second.type;
                        } else if (auto field = resolve_field_path(st.target, st.where)) {
                            if (!field->revisable) throw CompileError(st.where, "revise of field '" + st.target + "' requires a revisable field and revise authority on its aggregate");
                            target_type = field->type;
                        } else throw CompileError(st.where, "unknown revise target '" + st.target + "'");
                        if (!is_integer(target_type)) throw CompileError(st.where, "numeric revise requires exact-One integer");
                        auto d = expr(st.delta, target_type); if (d.type != target_type) throw CompileError(st.where, "revision delta type mismatch");
                        merge_faults(exposed, d.live_faults); exposed.insert("ArithmeticOverflow"); ff.effects.insert("revise");
                    } else if constexpr (std::is_same_v<T, IfStmt>) {
                        auto c = expr(st.condition, SemType{BaseType::Bool, {}, Cardinality::One}); if (c.type.base != BaseType::Bool || c.type.cardinality != Cardinality::One) throw CompileError(st.where, "if condition must be exact-One bool");
                        merge_faults(exposed, c.live_faults); bool tr = block(st.then_body), er = !st.else_body.empty() && block(st.else_body); dead = tr && er;
                    } else if constexpr (std::is_same_v<T, WhileStmt>) {
                        if (transaction_depth > 0) throw CompileError(st.where, "Compiler 0.8 reference transactions require statically bounded single-pass bodies; while is not permitted inside transaction");
                        auto c = expr(st.condition, SemType{BaseType::Bool, {}, Cardinality::One}); if (c.type.base != BaseType::Bool || c.type.cardinality != Cardinality::One) throw CompileError(st.where, "while condition must be exact-One bool");
                        merge_faults(exposed, c.live_faults); ++loop_depth; block(st.body); --loop_depth;
                    } else if constexpr (std::is_same_v<T, LoopStmt>) {
                        if (transaction_depth > 0) throw CompileError(st.where, "Compiler 0.8 reference transactions require statically bounded single-pass bodies; loop is not permitted inside transaction");
                        ++loop_depth; block(st.body); --loop_depth;
                    } else if constexpr (std::is_same_v<T, TransactionStmt>) {
                        if (transaction_depth > 0) throw CompileError(st.where, "Compiler 0.8 nested transactions are not yet part of the reference transaction profile");
                        ++transaction_depth; ff.effects.insert("transaction"); block(st.body); --transaction_depth;
                     } else if constexpr (std::is_same_v<T, ParallelStmt>) {
                        ++parallel_depth; ff.effects.insert("concurrency.parallel"); block(st.body); --parallel_depth;
                    } else if constexpr (std::is_same_v<T, TaskStmt>) {
                        if (parallel_depth == 0) throw CompileError(st.where, "task requires an enclosing parallel region");
                        ++task_depth; ff.effects.insert("concurrency.task"); block(st.body); --task_depth;
                    } else if constexpr (std::is_same_v<T, BreakStmt> || std::is_same_v<T, ContinueStmt>) {
                        if (loop_depth == 0) throw CompileError(st.where, "break/continue requires an enclosing loop");
                        if (transaction_depth > 0) throw CompileError(st.where, "Compiler 0.8 transaction blocks may exit only by fallthrough or fault");
                        if (task_depth > 0) throw CompileError(st.where, "Compiler 0.8 task blocks may not break/continue across the task boundary");
                        dead = true;
                    } else if constexpr (std::is_same_v<T, ReturnStmt>) {
                        if (transaction_depth > 0) throw CompileError(st.where, "Compiler 0.8 transaction blocks may not return across an uncommitted transaction boundary");
                        if (task_depth > 0) throw CompileError(st.where, "Compiler 0.8 task blocks may not return across the structured join boundary");
                        if (ff.result_type.base == BaseType::Unit && ff.result_type.cardinality == Cardinality::One) {
                            if (st.value) throw CompileError(st.where, "unit instruction return must omit a value");
                        } else {
                            if (!st.value) throw CompileError(st.where, "non-unit instruction must return a value");
                            auto v = expr(*st.value, ff.result_type); if (v.type != ff.result_type) throw CompileError(st.where, "return type mismatch: expected " + type_name(ff.result_type) + ", got " + type_name(v.type)); merge_faults(exposed, v.live_faults);
                            if(ff.result_borrow_mode!="none") { if(v.borrow_mode!=ff.result_borrow_mode) throw CompileError(st.where,"borrow result contract requires return of matching explicit borrow mode"); if(v.borrow_root!=ff.result_borrow_from) throw CompileError(st.where,"returned borrow provenance must originate from parameter '"+ff.result_borrow_from+"'"); }
                            else if(v.borrow_mode!="none") throw CompileError(st.where,"borrowed result requires @borrow_from(...) or @borrow_mut_from(...) contract");
                            if(v.ownership=="shared" && !v.explicit_retain) throw CompileError(st.where,"shared resource return requires explicit retain(...) so the caller receives an independent ownership obligation");
                        }
                        dead = true;
                    } else if constexpr (std::is_same_v<T, PerformStmt>) {
                        auto v = expr(st.value, std::nullopt); if (v.type.base != BaseType::Unit || v.type.cardinality != Cardinality::One) throw CompileError(st.where, "perform may discard only exact-One unit results"); merge_faults(exposed, v.live_faults);
                    } else if constexpr (std::is_same_v<T, FaultStmt>) {
                        auto fi = ssl.fault_facts.find(st.fault_name); if (fi == ssl.fault_facts.end()) throw CompileError(st.where, "unknown fault identity '" + st.fault_name + "'");
                        if (st.payload.size() != fi->second.payload.size()) throw CompileError(st.where, "fault payload arity mismatch for '" + st.fault_name + "'");
                        for (std::size_t i = 0; i < st.payload.size(); ++i) { auto v = expr(st.payload[i], fi->second.payload[i].type); if (v.type != fi->second.payload[i].type) throw CompileError(st.payload[i].where, "fault payload type mismatch for field '" + fi->second.payload[i].name + "'"); merge_faults(exposed, v.live_faults); }
                        exposed.insert(st.fault_name); dead = true;
                    } else if constexpr (std::is_same_v<T, ExprStmt>) {
                        auto v = expr(st.value, std::nullopt); if (v.type.base != BaseType::Unit || v.type.cardinality != Cardinality::One) throw CompileError(st.where, "non-unit expression result may not be silently discarded"); merge_faults(exposed, v.live_faults);
                    }
                }, s.node);
            }
            return dead;
        };

        const bool returns = block(fn.body);
        if (!(ff.result_type.base == BaseType::Unit && ff.result_type.cardinality == Cardinality::One) && !returns)
            throw CompileError(fn.where, "non-unit instruction does not definitely return on all top-level paths");
        for (const auto& f : exposed) if (!ff.declared_faults.contains(f)) throw CompileError(fn.where, "instruction body exposes fault '" + f + "' but it is absent from faults [...]");

        std::ostringstream fx; fx << "instruction " << fn.name << " result=" << type_name(ff.result_type) << " effects=[";
        bool first = true; for (const auto& e : ff.effects) { if (!first) fx << ','; first = false; fx << e; }
        fx << "] faults=["; first = true; for (const auto& f : ff.declared_faults) { if (!first) fx << ','; first = false; fx << f; } fx << ']';
        ssl.facts_text.push_back(fx.str());
        (void)transaction_depth; (void)parallel_depth; (void)task_depth;
    }
    return ssl;
}

} // namespace staze
