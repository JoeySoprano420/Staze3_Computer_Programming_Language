#include "staze/toolchain.hpp"
#include "staze/diagnostic.hpp"
#include <cstdlib>
#include <fstream>
#include <sstream>
#include <string>

namespace staze {
namespace {
std::string q(const std::filesystem::path& p) {
#ifdef _WIN32
    return "\"" + p.string() + "\"";
#else
    std::string s = p.string(), out = "'";
    for (char c : s) {
        if (c == '\'') out += "'\\''";
        else out += c;
    }
    return out + "'";
#endif
}
std::string qstr(const std::string& s) {
#ifdef _WIN32
    return "\"" + s + "\"";
#else
    std::string out = "'";
    for (char c : s) {
        if (c == '\'') out += "'\\''";
        else out += c;
    }
    return out + "'";
#endif
}
std::string env_or(const char* name, const char* fallback) {
    if (const char* v = std::getenv(name); v && *v) return v;
    return fallback;
}
void run(const std::string& cmd, const char* what) {
    const int rc = std::system(cmd.c_str());
    if (rc != 0) throw CompileError({}, std::string(what) + " failed (exit code " + std::to_string(rc) + "): " + cmd);
}
}

void WindowsPeToolchain::build(const std::filesystem::path& llvm_ir,
                               const std::filesystem::path& exe,
                               const BuildOptions& options) const {
    namespace fs = std::filesystem;
    if (!fs::exists(llvm_ir)) throw CompileError({}, "toolchain: LLVM IR file does not exist: " + llvm_ir.string());
    if (!exe.parent_path().empty()) fs::create_directories(exe.parent_path());

    const auto clang = env_or("STAZE_CLANG", "clang");
    const auto lld   = env_or("STAZE_LLD_LINK", "lld-link");
    const fs::path obj = exe.parent_path() / (exe.stem().string() + ".staze.obj");
    const fs::path def = exe.parent_path() / (exe.stem().string() + ".staze-kernel32.def");
    const fs::path lib = exe.parent_path() / (exe.stem().string() + ".staze-kernel32.lib");

    {
        std::ofstream f(def, std::ios::binary);
        if (!f) throw CompileError({}, "toolchain: cannot create " + def.string());
        f << "LIBRARY KERNEL32.dll\nEXPORTS\n  GetStdHandle\n  WriteFile\n  ExitProcess\n  GetProcessHeap\n  HeapAlloc\n  HeapFree\n";
    }

    // LLVM IR -> Windows x86-64 COFF object.
    run(qstr(clang) + " -target x86_64-pc-windows-msvc -O2 -Wno-override-module -mno-stack-arg-probe -c " + q(llvm_ir) + " -o " + q(obj),
        "LLVM/COFF compilation");

    // Create a tiny import library from the platform DLL export contract, then PE-link.
    run(qstr(lld) + " /lib /def:" + q(def) + " /machine:x64 /out:" + q(lib),
        "KERNEL32 import-library generation");
    run(qstr(lld) + " /subsystem:console /entry:staze_entry /machine:x64 /nodefaultlib /dynamicbase /nxcompat /out:" + q(exe) + " " + q(obj) + " " + q(lib),
        "PE32+ link");

    if (!fs::exists(exe) || fs::file_size(exe) == 0) throw CompileError({}, "toolchain: linker reported success but no executable was produced");

    if (!options.keep_intermediates) {
        std::error_code ec;
        fs::remove(obj, ec); fs::remove(def, ec); fs::remove(lib, ec);
    }
}

} // namespace staze
