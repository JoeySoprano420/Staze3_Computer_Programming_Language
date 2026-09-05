#pragma once
#include "staze/token.hpp"
#include <stdexcept>
#include <string>

namespace staze {
class CompileError : public std::runtime_error {
public:
    CompileError(SourceLocation where, std::string message)
        : std::runtime_error(std::move(message)), where_(where) {}
    SourceLocation where() const noexcept { return where_; }
private:
    SourceLocation where_;
};
}
