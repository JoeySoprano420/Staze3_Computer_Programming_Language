#pragma once
#include "staze/ast.hpp"
#include <cstdint>
#include <optional>
#include <set>
#include <string>
#include <unordered_map>
#include <vector>

namespace staze {

enum class BaseType { Unit, Bool, I32, I64, U32, U64, Text, Nominal };
enum class Cardinality { One, ZeroOrOne, Many };

struct SemType {
    BaseType base{BaseType::Unit};
    std::string nominal;
    Cardinality cardinality{Cardinality::One};
    bool operator==(const SemType&) const = default;
};

std::string cardinality_name(Cardinality cardinality);
std::string type_name(const SemType& type);
bool is_integer(const SemType& type);
bool is_signed_integer(const SemType& type);
unsigned integer_bits(const SemType& type);
bool is_exact_one(const SemType& type);

struct ExprFacts {
    SemType type;
    std::set<std::string> live_faults;
    std::set<std::string> authorities{"read"};
    // Ownership/resource facts survive SSL so DLE does not reconstruct them from syntax.
    std::string ownership{"value"};
    std::string resource_family;
    std::string borrow_root;
    std::string borrow_mode{"none"};
    bool explicit_move{false};
    bool explicit_retain{false};
    std::optional<std::int64_t> constant_int;
    std::optional<bool> constant_bool;
    std::optional<std::string> constant_text;
};
struct LocalFacts { SemType type; bool revisable{false}; std::set<std::string> authorities{"read"}; std::string ownership{"value"}; std::string resource_family; };
struct InstructionFacts {
    SemType result_type;
    std::string result_borrow_mode{"none"};
    std::string result_borrow_from;
    std::set<std::string> declared_faults;
    std::unordered_map<std::string, LocalFacts> locals;
    std::set<std::string> effects;
};
struct FieldFacts { std::string name; SemType type; bool key{false}; bool revisable{false}; };
struct DatasetFacts { bool entity{false}; std::optional<std::string> key; std::vector<FieldFacts> fields; std::vector<std::string> indexes; };
struct ChoiceCaseFacts { std::string name; std::vector<FieldFacts> fields; };
struct ChoiceFacts { std::vector<ChoiceCaseFacts> cases; };
struct RelationFacts { std::string source, target, source_cardinality, target_cardinality, ownership; bool unique_pair{false}; bool reverse_index{false}; };
struct PoolFacts { std::optional<std::string> lifetime; std::optional<std::string> allocation; std::optional<std::string> reclaim; std::string ownership{"unique"}; std::vector<FieldFacts> fields; };
struct FaultFacts { std::int32_t code{0}; std::vector<FieldFacts> payload; };

struct SSLProgram {
    std::string module_name;
    std::unordered_map<std::size_t, ExprFacts> expressions;
    std::unordered_map<std::string, InstructionFacts> instructions;
    std::unordered_map<std::string, DatasetFacts> datasets;
    std::unordered_map<std::string, ChoiceFacts> choices;
    std::unordered_map<std::string, RelationFacts> relations;
    std::unordered_map<std::string, PoolFacts> pools;
    std::unordered_map<std::string, int> fault_codes;
    std::unordered_map<std::string, FaultFacts> fault_facts;
    std::vector<std::string> facts_text;
};

class SemanticAnalyzer {
public:
    SSLProgram analyze(const Program& program) const;
private:
    static SemType resolve_type(const TypeRef& type);
};

} // namespace staze
