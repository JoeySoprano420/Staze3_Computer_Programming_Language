# Build Staze C++23 Compiler 0.8.0 on Windows x86-64

## Requirements

- Windows x86-64;
- CMake;
- a C++23-capable host compiler (Visual Studio/MSVC or another CMake-supported C++23 toolchain);
- Clang capable of targeting `x86_64-pc-windows-msvc`;
- `lld-link`.

Compiler 0.8 does **not** require MASM. The Staze backend emits LLVM IR, invokes Clang to produce a Windows x86-64 COFF object, and invokes `lld-link` to produce a PE32+ executable.

## Build the compiler

From a Visual Studio x64 developer command prompt:

```bat
build_windows_x64.bat
```

Equivalent explicit commands:

```bat
cmake -S . -B build -A x64
cmake --build build --config Release
```

The compiler is then:

```text
build\Release\stazec.exe
```

## Compile a 0.8 transaction program

```bat
build\Release\stazec.exe examples\transaction_observable_rollback.stz3 -o rollback.exe
```

## Compile a structured-task program

```bat
build\Release\stazec.exe examples\parallel_shared_task.stz3 -o parallel_shared_task.exe
```

## Detached SIR-C backend

```bat
build\Release\stazec.exe examples\parallel_shared_task.stz3 --check --emit-sir-c parallel_shared_task.sirc
build\Release\stazec.exe --from-sir-c parallel_shared_task.sirc -o parallel_shared_task.exe
```

## Build representative examples

```bat
compile_examples.bat
```

## Run the conformance suite

```bat
run_tests.bat
```

## Tool-path overrides

If Clang or `lld-link` is not on `PATH`:

```bat
set STAZE_CLANG=C:\path\to\clang.exe
set STAZE_LLD_LINK=C:\path\to\lld-link.exe
```
