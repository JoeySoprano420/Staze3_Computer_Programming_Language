# Growing Compiler 0.3 into the Full STZ-3 Architecture

> **0.4 status:** The roadmap in this document began at the 0.3 detached-SIR milestone. Rich Semantic SSA is now implemented; see `RICH_SEMANTIC_SSA.md` and the current `ROADMAP.md`.


The permanent rule is:

> **Never solve a semantic problem in the backend that belongs in SSL, DLE, SIR or the verifier.**

## Phase A — syntax/provenance

The parser records what the programmer wrote. It does not decide machine representation.

## Phase B — canonical SSL-3

The current `SSLProgram` is still a milestone semantic model. It must continue expanding into versioned, canonical, provenance-bearing facts for identity, type, cardinality, faults, effects, authority, ownership, lifetime, pools, relations, rules/invariants, representation boundaries and target-independent obligations.

## Phase C — DLE-3

DLE converts proven semantic obligations into executable semantics. It may select only realizations legal under SSL facts.

## Phase D — SIR-S — implemented detached graph foundation

Compiler 0.3 now builds a real standalone graph containing:

```text
SSA values
basic blocks
phi nodes
control-flow terminators
explicit named fault edges
effect annotations
copied semantic facts
```

No target register, Windows ABI detail, LLVM object or AST pointer belongs here.

## Phase E — independent SIR-S verification — implemented foundation

The verifier requires SSA definition uniqueness, dominance, exact phi predecessor coverage, call signature consistency, closed call fault edges, type-correct branches/returns and valid CFG/fault targets.

## Phase F — SIR-C — implemented detached target boundary

SIR-C deep-copies verified SIR-S and adds target-concrete facts. Current Windows bootstrap facts are:

```text
x86_64-pc-windows-msvc
64-bit pointers
COFF-x86-64 objects
PE32+ executables
current status-i32/payload-u64 scalar call contract
```

SIR-C is serializable and independently reloadable.

## Phase G — independent SIR-C verification — implemented foundation

The verifier operates on deserialized SIR-C with no AST or SSL. A deliberately corrupted `.sirc` fixture is rejected by the test suite.

## Phase H — native backend

```text
verified standalone SIR-C
    ↓
LLVM IR
    ↓
Clang target x86_64-pc-windows-msvc
    ↓
COFF object
    ↓
lld-link
    ↓
PE32+
```

`LlvmWindowsX64Emitter` consumes only `sir.hpp` data.

## Remaining production expansion

The next layers should add full aggregate/cardinality SSA, fault payload values, memory/provenance/lifetime operations, authority/capability values, pools and cleanup, dataset/relation execution, transactions/concurrency, proof references, full SABI-3 and a separately shipped verifier executable.

## Permanent separation

```text
WHAT STAZE MEANS                 SSL
WHAT LEGAL MECHANICS EXIST      DLE
WHAT WILL EXECUTE                SIR-S
WHAT TARGET REALIZATION EXISTS  SIR-C
IS THE GRAPH VALID?              independent verifier
HOW DOES x86-64 ENCODE IT?       LLVM/backend/linker
```
