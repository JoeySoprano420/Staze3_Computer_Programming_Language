# Build Compiler 0.5 on Windows x86-64

Use a C++23-capable compiler, CMake, Clang capable of emitting the `x86_64-pc-windows-msvc` target, and `lld-link`.

```bat
cmake -S . -B build -A x64
cmake --build build --config Release
```

Then:

```bat
build\Release\stazec.exe examples\rich_caller_storage.stz3 -o rich_caller_storage.exe
```

The Staze compiler itself generates LLVM IR internally, asks Clang for a Windows x86-64 COFF object, creates the required KERNEL32 import library, and invokes `lld-link` to produce PE32+.

Environment overrides:

```text
STAZE_CLANG
STAZE_LLD_LINK
```

No MASM step is required by Compiler 0.5.
