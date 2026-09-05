#pragma once
#include "staze/token.hpp"
#include <memory>
#include <optional>
#include <string>
#include <variant>
#include <vector>

namespace staze {

struct TypeRef {
    std::string name;
    std::vector<TypeRef> args;
    SourceLocation where;
};

enum class UnaryOp { Negate, Not };
enum class BinaryOp { Add, Sub, Mul, Div, Mod, Equal, NotEqual, Less, LessEqual, Greater, GreaterEqual, And, Or };

enum class HandlerKind { Bypass, Delete };
struct Expr;
struct FaultHandler {
    HandlerKind kind{HandlerKind::Bypass};
    std::string fault_name;
    std::shared_ptr<Expr> replacement;
    SourceLocation where;
};

struct IntExpr { std::string literal; };
struct BoolExpr { bool value{false}; };
struct TextExpr { std::string value; };
struct NameExpr { std::string name; };
struct UnaryExpr { UnaryOp op; std::shared_ptr<Expr> operand; };
struct BinaryExpr { BinaryOp op; std::shared_ptr<Expr> lhs, rhs; };
struct CallExpr { std::string callee; std::vector<Expr> args; };

struct Expr {
    std::size_t id{0};
    SourceLocation where;
    std::variant<IntExpr, BoolExpr, TextExpr, NameExpr, UnaryExpr, BinaryExpr, CallExpr> node;
    std::vector<FaultHandler> handlers;
};

struct LocalDecl { std::string name; std::optional<TypeRef> type; bool revisable{false}; Expr value; SourceLocation where; };
struct SetStmt { std::string target; Expr value; SourceLocation where; };
struct ReviseStmt { std::string target; Expr delta; SourceLocation where; };
struct IfStmt;
struct WhileStmt;
struct LoopStmt;
struct TransactionStmt;
struct ParallelStmt;
struct TaskStmt;
struct BreakStmt { SourceLocation where; };
struct ContinueStmt { SourceLocation where; };
struct ReturnStmt { std::optional<Expr> value; SourceLocation where; };
struct PerformStmt { Expr value; SourceLocation where; };
struct FaultStmt { std::string fault_name; std::vector<Expr> payload; SourceLocation where; };
struct ExprStmt { Expr value; SourceLocation where; };

struct Statement;
using Block = std::vector<Statement>;
struct IfStmt { Expr condition; Block then_body; Block else_body; SourceLocation where; };
struct WhileStmt { Expr condition; Block body; SourceLocation where; };
struct LoopStmt { Block body; SourceLocation where; };
struct TransactionStmt { Block body; SourceLocation where; };
struct ParallelStmt { Block body; SourceLocation where; };
struct TaskStmt { Block body; SourceLocation where; };

struct Statement {
    std::variant<LocalDecl, SetStmt, ReviseStmt, IfStmt, WhileStmt, LoopStmt,
                 TransactionStmt, ParallelStmt, TaskStmt, BreakStmt, ContinueStmt,
                 ReturnStmt, PerformStmt, FaultStmt, ExprStmt> node;
};

struct Parameter { std::string name; std::string authority{"read"}; TypeRef type; SourceLocation where; };
struct InstructionDecl {
    bool is_public{false};
    std::string name;
    std::vector<Parameter> params;
    TypeRef result_type;
    // Cross-boundary borrow contract. Empty means ordinary value/owned result.
    // @borrow_from(name) and @borrow_mut_from(name) bind a returned borrow lifetime
    // to a specific parameter rather than permitting an unbounded borrowed escape.
    std::string result_borrow_mode{"none"};
    std::optional<std::string> result_borrow_from;
    std::vector<std::string> faults;
    Block body;
    SourceLocation where;
};

struct FaultField { std::string name; TypeRef type; };
struct FaultDecl { std::string name; std::vector<FaultField> fields; SourceLocation where; };

struct DatasetField {
    bool is_key{false};
    bool revisable{false};
    std::string name;
    TypeRef type;
    std::optional<Expr> default_value;
    SourceLocation where;
};
struct DatasetRule { std::string field; std::string predicate; SourceLocation where; };
struct DatasetDecl {
    std::string name;
    std::vector<DatasetField> fields;
    std::vector<DatasetRule> rules;
    std::vector<std::string> indexes;
    SourceLocation where;
};

struct ChoiceField { std::string name; TypeRef type; SourceLocation where; };
struct ChoiceCase { std::string name; std::vector<ChoiceField> fields; SourceLocation where; };
struct ChoiceDecl { std::string name; std::vector<ChoiceCase> cases; SourceLocation where; };

struct RelationDecl {
    std::string name;
    TypeRef source_type;
    TypeRef target_type;
    std::string source_cardinality;
    std::string target_cardinality;
    bool unique_pair{false};
    bool reverse_index{false};
    std::string ownership{"none"};
    SourceLocation where;
};

struct PoolDirective { std::string name; std::optional<std::string> argument; SourceLocation where; };
struct PoolField { std::string name; bool revisable{false}; TypeRef type; SourceLocation where; };
struct PoolDecl {
    std::string name;
    std::vector<PoolDirective> directives;
    std::vector<PoolField> fields;
    SourceLocation where;
};

struct Program {
    int staze_version{0};
    std::string module_name;
    std::vector<std::string> imports;
    std::vector<FaultDecl> faults;
    std::vector<DatasetDecl> datasets;
    std::vector<ChoiceDecl> choices;
    std::vector<RelationDecl> relations;
    std::vector<PoolDecl> pools;
    std::vector<InstructionDecl> instructions;
};

} // namespace staze
