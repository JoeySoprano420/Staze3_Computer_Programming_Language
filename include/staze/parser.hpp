#pragma once
#include "staze/ast.hpp"
#include <vector>
namespace staze {
class Parser {
public:
    explicit Parser(std::vector<Token> tokens) : tokens_(std::move(tokens)) {}
    Program parse_program();
private:
    const Token& peek(std::size_t lookahead = 0) const;
    bool is(TokenKind kind) const;
    bool match(TokenKind kind);
    Token consume(TokenKind kind, const char* message);
    bool text_is(std::string_view text) const;
    Token consume_word(std::string_view word, const char* message);
    void optional_semi();
    std::string parse_qualified_name();
    std::string parse_semantic_name();
    std::string parse_use_decl();
    TypeRef parse_type();
    Parameter parse_parameter();
    std::vector<std::string> parse_faults();
    InstructionDecl parse_instruction();
    FaultDecl parse_fault_decl();
    DatasetDecl parse_dataset();
    ChoiceDecl parse_choice();
    RelationDecl parse_relation();
    PoolDecl parse_pool();
    Block parse_block();
    Statement parse_statement();
    Expr parse_expression();
    Expr parse_or(); Expr parse_and(); Expr parse_equality(); Expr parse_comparison();
    Expr parse_additive(); Expr parse_multiplicative(); Expr parse_unary(); Expr parse_postfix(); Expr parse_primary();
    Expr make_binary(BinaryOp op, Expr lhs, Expr rhs, SourceLocation where);
    std::size_t next_expr_id_{1};
    std::vector<Token> tokens_;
    std::size_t pos_{0};
};
}
