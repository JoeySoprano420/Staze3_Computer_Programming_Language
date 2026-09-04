# Staze C++23 Compiler 0.5.0 — Concrete Rich Representation Lowering

**Platform target:** Windows x86-64 / PE32+  
**Host implementation:** C++23  
**Native path:** detached SIR-C 3.0 → LLVM IR → x86-64 COFF → PE32+  
**Status:** implementation milestone toward full STZ-3, not a claim of complete language coverage.

Compiler 0.5 keeps Compiler 0.4's SIR-S meaning intact and moves physical decisions into target concretization. SIR-C 3.0 now records the concrete representation plan that the LLVM backend must obey.

## The architectural rule

```text
SIR-S
    says what the Staze program means

        ↓ TargetConcretizer / DLE realization

SIR-C 3.0
    says how that meaning is represented on Windows x86-64

        ↓ independent verifier

LLVM backend
    realizes the already-selected representation

        ↓

COFF → PE32+ .exe
```

LLVM is deliberately not asked to invent Staze cardinality, ownership, aggregate, fault, pool, or allocation semantics.

## New in 0.5

### Concrete storage classes

SIR-C can choose and serialize:

- `register` — exact-One scalar values.
- `stack` — fixed-size rich temporaries/local materializations.
- `caller` — rich parameters and caller-provided rich return storage.
- `pool-arena` — values allocated from explicit arena-style Staze pools.
- `heap` — explicit `@heap` pool allocations.
- `alias` — borrow/move/revision aliases preserving underlying storage/provenance.

A `static` enum is reserved in the representation model, but full static-storage lowering is not claimed in 0.5.

### Concrete rich layouts

- `ZeroOrOne<T>` → `{ present: u8, padding, payload: T }`.
- compiler-bounded `Many<T>` → `{ length: u64, inline_elements[N] }`.
- datasets/pool records → explicit size, alignment, and field-offset tables.
- choices → explicit `i32` tag plus aligned maximum payload storage.
- text remains a 16-byte target view contract.

Cardinality remains semantic in SIR-S. These layouts are SIR-C choices, not language definitions.

### Native rich ABI

Scalar functions retain the status/payload scalar convention plus hidden fault storage.

Rich-return functions use caller storage:

```text
status:i32 fn(result_out:ptr, fault_out:ptr, ...)
```

Rich parameters are passed by address. The included `rich_caller_storage.stz3` fixture exercises both directions.

### Native typed fault payloads

Every call chain receives caller-owned fault payload storage sized/aligned from the closed fault schemas. A direct typed fault writes its payload into that storage before returning nonzero status. Propagation does not overwrite the buffer.

Example `fault Missing { value: i32 }` with `fault Missing(42)` becomes a real native `store i32 42` into `%fault_out`.

### Escape analysis and scalar replacement

Target concretization builds use sets and marks rich values escaping through returns, rich calls, and aliases. Escape information propagates through `borrow`, `move`, revision aliases, and phi values.

A deliberately narrow scalar-replacement rule currently eliminates nonescaping dataset/choice constructions whose only remaining uses are semantic borrow/move/relation operations. Escaping values remain materialized.

### Explicit heap allocation failure

An `@heap` pool allocation exposes `AllocationFailure` as an ordinary closed Staze fault. Native lowering uses `GetProcessHeap` / `HeapAlloc`, checks the returned pointer, and follows the SIR fault edge when allocation returns null.

Compiler 0.5 does **not** invent automatic heap reclamation. Therefore `@heap` is temporarily accepted only with `@reclaim(manual)`. Automatic scope/lifetime reclamation belongs to the next resource-runtime milestone.

## Example commands

Build the compiler:

```bat
cmake -S . -B build -A x64
cmake --build build --config Release
```

Compile Staze directly to PE32+:

```bat
build\Release\stazec.exe examples\rich_cardinality.stz3 -o rich_cardinality.exe
```

Inspect the concrete plan without building:

```bat
build\Release\stazec.exe examples\rich_resources.stz3 --check --emit-sir-c rich_resources.sirc
```

Compile a detached concrete artifact, with no source/AST/SSL/DLE present:

```bat
build\Release\stazec.exe --from-sir-c rich_resources.sirc -o rich_resources.exe
```

## Current native examples

- `rich_cardinality.stz3` — optional + Many + delete recovery reaches PE.
- `rich_resources.stz3` — dataset, choice, borrow/move, arena pool, relations/transaction markers reach PE.
- `rich_caller_storage.stz3` — rich aggregate return + rich aggregate parameter reaches PE.
- `heap_allocation.stz3` — explicit heap placement + AllocationFailure reaches PE.
- `fault_payload_native.stz3` — typed fault payload transport reaches PE.
- all prior scalar/control/effect fixtures continue to build.

## Deliberate 0.5 boundaries

The following are not falsely claimed as complete:

- dynamically growing/unknown-length `Many<T>` allocation;
- automatic heap reclamation / `HeapFree` scheduling from lifetime proof;
- full field projection/mutation for arbitrary aggregates;
- general borrow-address dereference machinery;
- native runtime relation storage;
- transaction rollback/durability machinery;
- real parallel worker scheduling/data-race proof;
- production SIR ABI stability;
- full STZ-3 language surface.

These are next-stage work, not silently delegated to C++ containers or a hidden runtime.

## Documents

- `docs/CONCRETE_RICH_LOWERING.md` — full 0.5 architecture and layout rules.
- `docs/SIR_C_3_FORMAT.md` — concrete artifact records and boundary.
- `docs/IMPLEMENTATION_STATUS.md` — precise support matrix.
- `docs/TEST_REPORT.md` — clean-build and conformance results.
- `docs/ROADMAP.md` — next implementation milestone.
- `docs/RICH_SEMANTIC_SSA.md` — retained 0.4 semantic foundation.

## Core principle

> **SIR-S preserves meaning. SIR-C chooses representation. The verifier checks the choice. LLVM only realizes it.**
