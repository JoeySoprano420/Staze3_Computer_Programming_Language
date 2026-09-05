#pragma once
#include <cstddef>
#include <string>

namespace staze {

enum class TokenKind {
    End, Identifier, Integer, String, BuiltinType, ReservedKeyword,
    KwStaze, KwModule, KwUse, KwAs, KwPublic, KwPrivate,
    KwInstruction, KwFaults, KwFault, KwPerform, KwReturn,
    KwSet, KwRevise, KwBy, KwIf, KwElse, KwWhile, KwDo, KwLoop,
    KwBreak, KwContinue, KwShadow,
    KwDataset, KwKey, KwRule, KwIndex,
    KwRelation, KwPool, KwChoice, KwTransaction, KwParallel, KwTask, KwDelegate, KwAccess,
    KwRead, KwMove, KwInsert, KwRemove, KwLink, KwUnlink,
    KwTrue, KwFalse, KwAnd, KwOr, KwNot,
    KwBypass, KwDelete, KwUsing,
    ColonEqual, Arrow, DoubleColon, Ellipsis,
    Equal, NotEqual, Plus, Minus, Star, Slash, Percent,
    Less, LessEqual, Greater, GreaterEqual,
    Dot, Comma, Colon, At,
    LBracket, RBracket, LBrace, RBrace, LParen, RParen, Semicolon
};

struct SourceLocation { std::size_t line{1}; std::size_t column{1}; };
struct Token { TokenKind kind{TokenKind::End}; std::string text; SourceLocation location; };
const char* token_kind_name(TokenKind kind);

} // namespace staze
