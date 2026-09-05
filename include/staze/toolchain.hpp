#pragma once
#include <filesystem>
namespace staze {
struct BuildOptions { bool keep_intermediates{false}; };
class WindowsPeToolchain {
public:
    void build(const std::filesystem::path& llvm_ir,
               const std::filesystem::path& exe,
               const BuildOptions& options = {}) const;
};
}
