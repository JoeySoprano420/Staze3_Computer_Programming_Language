#pragma once
#include "staze/sir.hpp"
#include <string>
namespace staze {
class LlvmWindowsX64Emitter {
public:
    std::string emit(const SirCProgram& program) const;
};
} // namespace staze
