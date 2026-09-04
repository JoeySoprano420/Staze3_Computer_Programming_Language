# Staze → Machine: the exact mental model

> **Compiler 0.4 update:** Rich source semantics now survive as independent SIR value/cardinality/region/provenance/capability/effect records before native realization.


> **Compiler 0.3 update:** The source-to-machine principle below is unchanged, but the current backend no longer ends in a MASM source file. It emits LLVM IR and automatically produces Windows x86-64 COFF + PE32+. See `PE_PIPELINE.md`.


A CPU does **not** understand `staze`, `module`, `instruction`, `perform`, or `return`.
It does not learn these words subconsciously. It executes encoded machine instructions.
The compiler is the translator that turns Staze's defined meaning into those instructions.

For this Compiler-0:

```text
Staze source
   ↓ characters
Lexer
   ↓ tokens
Parser
   ↓ AST
Semantic analyzer
   ↓ resolved program
Windows-x64 MASM emitter
   ↓ .asm
ml64.exe
   ↓ COFF .obj machine code + relocations
link.exe
   ↓ PE32+ .exe
Windows loader
   ↓ maps code + resolves Kernel32 imports
x86-64 CPU
   ↓ executes binary opcodes
```

## The important distinction

The compiler does **not** assign a private CPU opcode to every Staze keyword.

For example, `perform` has no x86 opcode named `PERFORM`.
Instead:

1. The lexer recognizes the source word `perform` as `TokenKind::KwPerform`.
2. The parser recognizes the pattern `perform stdout.write_text(...)`.
3. Semantic analysis proves the argument is currently a text value.
4. The backend lowers the operation to the Windows service call `WriteFile`.
5. MASM encodes instructions such as `mov`, `lea`, and `call` into x86-64 bytes.

Likewise, `:=` is not an opcode. It means **create a binding**. In this stage, immutable
text bindings are compile-time constants, so they disappear entirely from runtime code.
That disappearance is a feature: the machine receives only what is still required to execute.

## Keyword meanings implemented in Compiler-0

| Staze form | Compiler meaning | Runtime consequence |
|---|---|---|
| `staze 3` | Select language contract version 3 | None; compile-time only |
| `module X` | Give program/module semantic identity | Comment/metadata in Compiler-0 |
| `use Std.IO::{stdout}` | Make stdout service name available | No runtime import object yet; Compiler-0 recognizes stdout directly |
| `public` | Export/entry visibility | Requires `main` to be externally visible |
| `instruction` | Declare executable behavior | Creates an executable native procedure |
| `[]` | Parameter list | Compiler-0 requires it to be empty |
| `-> i32` | Result contract | Native return value is placed in `EAX` |
| `faults [IoFailure]` | Declared fault algebra | Validated; full fault lowering is a later stage |
| `name := "text"` | Create immutable binding | Constant-propagated; usually zero runtime instructions |
| `perform stdout.write_text(x)` | Authorized output effect | Lowers to Win32 `GetStdHandle` + `WriteFile` calls |
| `return N` | Complete instruction with i32 result | `mov eax, N` then `ret` |

## Why RCX, RDX, R8, and R9 appear

On Windows x86-64, the Microsoft x64 calling convention passes the first four integer/pointer
arguments in RCX, RDX, R8, and R9. A caller also reserves 32 bytes of shadow space. The generated
assembly follows that rule when calling Kernel32.

`WriteFile(handle, bytes, length, &written, nullptr)` therefore becomes conceptually:

```asm
mov rcx, [stdout_handle] ; argument 1
lea rdx, msg             ; argument 2
mov r8d, msg_len         ; argument 3
lea r9, bytes_written    ; argument 4
mov qword ptr [rsp+32],0 ; argument 5
call WriteFile
```

MASM then converts each x86-64 instruction into its binary encoding and records relocations for
symbols whose final addresses are chosen by the linker.
