#pragma once
#include "staze/token.hpp"
#include <string_view>
#include <vector>
namespace staze {
class Lexer {
public:
    explicit Lexer(std::string_view source) : source_(source) {}
    std::vector<Token> lex_all();
private:
    bool at_end() const noexcept;
    char peek(std::size_t lookahead = 0) const noexcept;
    char advance();
    bool match(char expected);
    void skip_space_and_comments();
    Token lex_identifier_or_keyword();
    Token lex_integer();
    Token lex_string();
    Token make(TokenKind kind, std::string text, SourceLocation start) const;
    std::string_view source_;
    std::size_t pos_{0}, line_{1}, column_{1};
};
}
