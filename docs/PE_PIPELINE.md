# Windows x86-64: Detached SIR-C to COFF to PE32+

## Source command

```bat
stazec app.stz3 -o app.exe
```

## Backend-only command

```bat
stazec --from-sir-c app.sirc -o app.exe
```

The second form is the architectural proof: no Staze lexer/parser/AST/SSL is needed by native code generation.

## Internally

```text
standalone verified SIR-C
  -> LLVM IR
  -> clang -target x86_64-pc-windows-msvc
  -> x86-64 COFF .obj
  -> lld-link
  -> PE32+ .exe
```

The driver currently creates a minimal KERNEL32 import library for `GetStdHandle`, `WriteFile` and `ExitProcess`.

## Inspection outputs

```text
--emit-sir-s file.sirs
--emit-sir-c file.sirc
--emit-sir file.sir
--emit-llvm file.ll
--keep-intermediates
```

`.exe` remains the ordinary final product.
