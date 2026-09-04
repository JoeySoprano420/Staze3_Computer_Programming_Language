# Building Staze Compiler-0 in C++23 — hand-held from source text to x86-64

> **Compiler 0.4 update:** The tutorial’s basic frontend mental model still applies. The current compiler now inserts a Rich Semantic SSA layer; see `RICH_SEMANTIC_SSA.md`.


> **Compiler 0.3 note:** This tutorial began with the transparent Compiler-0 MASM path. The current executable pipeline supersedes that final backend step with verified standalone SIR-C → LLVM IR → COFF → PE32+. The early sections remain useful for understanding the frontend and machine-code mental model. See `PE_PIPELINE.md` for the current native path.


## 0. First: what we are actually building

You asked for a C++23 compiler that "explains to the machine" what Staze means.
That description is useful as an intuition, with one technical correction:

**the CPU does not learn Staze.**

A CPU is an extremely literal state-transition machine. It fetches bytes from an instruction
address, decodes those bytes according to the x86-64 instruction set, changes registers/memory,
and advances to the next instruction. It has no awareness of `instruction`, `dataset`, `fault`,
`perform`, `pool`, or `bypass`.

The compiler is what preserves the connection:

```text
Staze spelling
    ↓
Staze syntax
    ↓
Staze meaning
    ↓
proved/selected mechanics
    ↓
verified concrete operations
    ↓
x86-64 instructions + Windows ABI calls
    ↓
encoded bytes
```

Think of the compiler as a sequence of locked doors. Each door may remove information that the
next door no longer needs, but it must never invent a different meaning.

---

# 1. The five questions every compiler stage answers

## Lexer — "What words and symbols are here?"

Input:

```stz3
return 0
```

Output conceptually:

```text
KwReturn("return")
Integer("0")
```

The lexer does **not** decide what returning means.

## Parser — "What grammatical structure do these tokens form?"

It turns those tokens into something like:

```text
ReturnStmt(value = 0)
```

The parser still does not choose x86 instructions.

## Semantic resolver — "Is this legal Staze, and exactly what does it mean?"

Questions include:

```text
What instruction are we inside?
What result type is required?
Does 0 fit i32?
Is this path permitted to return here?
What faults/effects remain?
What bindings exist?
```

This is where Staze starts becoming meaning rather than syntax.

## Lowering/IR — "What concrete behavior satisfies that meaning?"

Production Staze will do this through SSL-3 → DLE-3 → SIR-S → SIR-C → verification.
Compiler-0 temporarily compresses those layers because the first executable milestone must remain
small enough to understand.

## Backend — "How does this verified behavior run on this target?"

For the supplied reference backend, the target is:

```text
Windows
x86-64
Microsoft x64 calling convention
PE/COFF executable
```

The backend emits MASM text. `ml64.exe` encodes it to x86-64 object code and `link.exe` creates the
PE executable.

---

# 2. Open the project and follow the data, not the filenames

Use this reading order:

```text
examples/hello.stz3
        ↓
include/staze/token.hpp
        ↓
src/lexer.cpp
        ↓
include/staze/ast.hpp
        ↓
src/parser.cpp
        ↓
src/semantic.cpp
        ↓
src/masm_x64.cpp
        ↓
src/main.cpp
```

That is the same order in which source meaning travels.

---

# 3. `token.hpp`: teach the compiler the *names of shapes*

A token kind is not machine code.

When C++ contains:

```cpp
enum class TokenKind {
    KwInstruction,
    KwPerform,
    KwReturn,
    ColonEqual,
    ...
};
```

we are giving the compiler implementation stable internal labels.

When the lexer sees the bytes for:

```text
perform
```

it records:

```text
kind = KwPerform
text = "perform"
line/column = where it occurred
```

Why keep the location? Because an error without provenance is miserable. Later semantic facts should
ultimately retain source spans all the way into SSL/SIR/debug information.

---

# 4. `lexer.cpp`: turn characters into facts about spelling

The lexer repeatedly asks:

```text
Is this whitespace?
A comment?
An identifier?
A reserved word?
A number?
A string?
A punctuation/operator token?
```

It must be deterministic. The exact same UTF-8 source and language version must produce the same
token stream.

For production Staze, this stage must eventually implement the complete STZ-3 lexical law:

- UTF-8 validation;
- optional BOM;
- ASCII case-sensitive semantic identifiers;
- nested `/* ... */` comments;
- all NC-3.0 reserved words;
- string/character escapes;
- numeric literal grammar;
- exact automatic statement termination;
- punctuation/operator tokenization.

Compiler-0 implements only the subset required by its published feature list. Unsupported Standard
syntax is rejected rather than guessed.

---

# 5. `ast.hpp`: store syntax without pretending syntax is semantics

The AST says things like:

```text
BindStmt(name="greeting", value="Hello")
WriteTextStmt(value=NameExpr("greeting"))
ReturnStmt(value=0)
```

This is deliberately still close to the source.

Production law: **the AST must not become the authoritative program model.** SSL-3 is authoritative.
Why? Because two different spellings may resolve to the same semantics, while identical-looking
syntax under different definitions/types may not.

---

# 6. `parser.cpp`: teach grammar, not machine implementation

For the canonical hello program, the parser recognizes:

```text
staze 3
module <qualified-name>
use ...
public instruction main[] -> i32
faults [...]
{
    statements
}
```

Then it recognizes the Compiler-0 statements:

```text
name := text-expression
perform stdout.write_text(text-expression)
return integer
```

Notice what is **not** in the parser:

```text
RCX
RDX
WriteFile
MOV
CALL
PE headers
```

That separation is crucial. If parsing `perform` directly emits `call WriteFile`, your frontend has
silently declared that all meanings of `perform` are Windows console writes. That would be wrong.

---

# 7. `semantic.cpp`: this is where the language begins enforcing itself

The semantic analyzer currently proves a tiny set of obligations:

```text
language version is exactly 3
entry instruction is public
entry is named main
main returns i32
fault names are from the supported Compiler-0 vocabulary
:= never recreates an existing binding
text names must exist before use
statements after return are rejected
main has an explicit return
```

Example:

```stz3
message := "one"
message := "two"
```

must fail.

Why?

Because `:=` is **creation**, not mutation. Silently turning the second line into replacement would
violate a permanent Staze law.

Production expansion of this layer becomes SSL-3. Instead of a few maps, the compiler records
monotonic semantic facts such as:

```text
identity
resolved type
cardinality
ownership
lifetime
borrow state
authority/effect set
fault set
invariants
representation observability
source provenance
```

A contradiction is not "resolved creatively." It is a compile error.

---

# 8. Constant propagation: watch Staze semantics disappear correctly

Consider:

```stz3
greeting := "Hello from Staze v3\n"
perform stdout.write_text(greeting)
```

At runtime, we do not need a mutable variable named `greeting`.

Compiler-0 resolves the name to the actual UTF-8 bytes during compilation. Therefore the generated
assembly contains a static byte sequence and never emits an instruction corresponding to `:=`.

This is the first tiny example of the Staze performance philosophy:

> preserve meaning long enough to prove machinery unnecessary, then erase the machinery.

---

# 9. `masm_x64.cpp`: cross the target boundary only after meaning is resolved

The backend receives something closer to:

```text
ResolvedProgram
    writes = ["Hello from Staze v3\n"]
    return_code = 0
```

It no longer needs to ask whether `greeting` existed or whether `:=` was legal. Those questions
belong to earlier stages.

For output, the reference backend uses Windows Kernel32:

```text
GetStdHandle(STD_OUTPUT_HANDLE)
WriteFile(...)
```

This is a **target realization**, not the definition of `Std.IO.stdout.write_text` for every target.
Linux, freestanding firmware, a test host, or another runtime profile can realize the same Staze
contract differently.

---

# 10. Why the generated assembly uses RCX/RDX/R8/R9

Windows x86-64 has a calling convention. For ordinary integer/pointer parameters:

```text
argument 1 → RCX
argument 2 → RDX
argument 3 → R8
argument 4 → R9
argument 5+ → stack
```

The caller also reserves 32 bytes of shadow space and preserves required stack alignment.

So a conceptual call:

```text
WriteFile(handle, bytes, count, &written, nullptr)
```

becomes:

```asm
mov rcx, qword ptr [stdout_handle]
lea rdx, msg_0
mov r8d, msg_0_len
lea r9, bytes_written
mov qword ptr [rsp+32], 0
call WriteFile
```

Now we are finally close to machine instructions.

---

# 11. Where the literal binary comes from

MASM translates each assembly instruction into its x86-64 encoding.

Conceptually:

```text
mov eax, 0
```

becomes an opcode plus encoded operand/immediate bytes.

A `call WriteFile` cannot contain the final absolute runtime address at assembly time. The object
file therefore contains machine code plus a **relocation** saying, in effect:

```text
"linker/loader: connect this call/reference to the symbol WriteFile"
```

`link.exe` produces a PE32+ image containing sections, import information, relocations, an entry
point, and the encoded code/data. The Windows loader maps the image and resolves imported APIs.

Only then does the CPU fetch the final instruction bytes from executable memory.

---

# 12. Do not memorize hex opcodes to understand a compiler

Hex is useful for inspection, but the deep model is:

```text
semantic contract
→ verified low-level operation
→ target instruction selection
→ encoding
```

The binary encoding is the **last representation**, not the source of meaning.

If you make hexadecimal opcode tables the semantic foundation of Staze, the language becomes tied
to one ISA and loses its own architecture. Instead, Staze defines behavior; target packs/backends
define how a particular processor realizes it.

---

# 13. How we grow this into the real STZ-3 compiler

Never add a new feature only by hacking the parser and emitter.

For every feature, add it in this order:

```text
1. Lexical/grammar recognition
2. AST/source provenance
3. Semantic identity and facts
4. Type/cardinality/effect/fault/authority rules
5. SSL-3 canonical representation
6. DLE-3 legal realization rules + proof obligations
7. SIR-S operation(s), if still semantically significant
8. Target concretization → SIR-C
9. Independent SIR-C verifier rules
10. Backend lowering
11. Positive tests
12. Negative tests
13. Boundary tests
14. Golden semantic/SIR artifacts
```

If step 10 is implemented before steps 3–9, the backend starts deciding language meaning. That is
precisely what Staze's architecture forbids.

---

# 14. The production pipeline we are working toward

```text
UTF-8 .stz3
   ↓
lexer + terminator resolver
   ↓
parser / source tree
   ↓
name + definition resolution (SDEF-3)
   ↓
type/effect/fault/authority/lifetime resolution
   ↓
canonical SSL-3
   ↓
DLE-3: prove legal realizations, then choose deterministically
   ↓
SIR-S
   ↓
target pack / representation concretization
   ↓
SIR-C
   ↓
INDEPENDENT verifier
   ↓
LLVM-compatible production backend
   ↓
LLVM machine pipeline
   ↓
COFF .obj
   ↓
PE linker
   ↓
.exe
```

The supplied direct MASM backend is intentionally useful as a microscope/reference path. It is not
being declared the final canonical production backend.

---

# 15. What to implement next

The highest-leverage next engineering milestone is **not** datasets or fancy lattice syntax.
It is a real small IR boundary.

Build this next:

```text
AST
 ↓
MiniSSL facts
 ↓
SIR-S v0
 ↓
SIR-C windows-x86_64 v0
 ↓
Verifier
 ↓
MASM backend
```

Once that exists, adding features stops coupling semantics directly to assembly. It creates the
same architectural shape the production compiler will keep.
