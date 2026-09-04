# Staze C++23 Compiler 0.6.0 — Lifetime & Resource Realization

Compiler 0.6 makes Staze resource lifetimes executable. SIR-S remains the semantic contract; SIR-C 4.x now records the concrete resource obligations, cleanup edges, ownership transfers, field addresses, dynamic collection state, and storage realization that the Windows x86-64 backend must obey.

## Canonical pipeline

```text
.stz3 → AST → SSL-3 → DLE-3 → canonical SIR-S
     → serialize/deserialize → independent SIR-S verification
     → SIR-C 4.x resource/representation realization
     → serialize/deserialize → independent SIR-C verification
     → LLVM IR → x86-64 COFF → PE32+ .exe
```

LLVM is downstream of resource proof. It does not decide whether an owned allocation must be freed, transferred, or retained.

## 0.6 capabilities

- path-sensitive cleanup insertion on normal, fault, branch, `break`, `continue`, and transaction-region exits;
- proven `HeapFree` for automatic `@heap @reclaim(scope)` resources;
- automatic arena/stack lifetime end realization;
- dynamic `Many<T>` as `{data,length,capacity}` with checked growth and replacement of the same ownership obligation;
- addressable aggregate fields with serialized byte offsets;
- field load/store and field-address operations;
- subobject provenance and borrowed field addresses;
- borrow lifetime narrowing to the shortest valid owner/lexical region;
- explicit owned-resource transfer across instruction returns/calls;
- caller re-ownership of transferred heap objects and dynamic `Many<T>` buffers;
- static pool placement with no dynamic cleanup;
- independent corruption checks for cleanup, transfer, provenance, authority, effect tokens, and representation plans.

## Ownership-transfer rule

Returning an owned resource requires an explicit `move(...)`. A resource return is not represented as “skip the free.” SIR-S declares an `owned-transfer` result contract; SIR-C records a transfer discharge on the callee success edge; the native ABI passes ownership-bearing state to caller storage; the caller installs a fresh cleanup obligation.

```text
callee obligation ──move/transfer──► caller obligation
       │                                 │
 success: no free                    later lifetime exit
       │                                 │
       └────────────────────────────────►HeapFree/current-buffer cleanup
```

Borrow returns remain deliberately rejected until Staze has an explicit cross-boundary borrow-lifetime ABI.

## Build

```bat
cmake -S . -B build -A x64
cmake --build build --config Release
build\Release\stazec.exe examples\owned_heap_transfer.stz3 -o owned_heap_transfer.exe
```

On environments with Clang/lld-link, the bootstrap PE backend can also be exercised directly. `STAZE_CLANG` and `STAZE_LLD_LINK` override tool locations.

## Detached compilation

```bat
stazec examples\owned_heap_transfer.stz3 --check --emit-sir-c owned.sirc
stazec --from-sir-c owned.sirc -o owned.exe
```

The detached invocation creates no lexer, parser, AST, SSL, DLE, or SIR-S object.

## Validation

The clean Release build was compiled with `-Wall -Wextra -Wpedantic`: **0 warnings, 0 errors**. The expanded conformance suite passes **61/61 tests**. See `docs/TEST_REPORT.md` and `docs/CTEST_OUTPUT_0.6.0.txt`.

## Important current boundary

Compiler 0.6 supports automatic unique-resource cleanup and explicit ownership transfer for the implemented heap-pool and dynamic-Many families. It does not yet claim general shared ownership, reference counting, tracing GC, arbitrary cross-boundary borrowed references, complete transaction rollback, or a universal destructor model.
