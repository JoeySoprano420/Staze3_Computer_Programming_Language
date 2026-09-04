# STZ-3 Compiler 0.6.0 — Implementation Status

## Implemented in this milestone

| Capability | Status |
|---|---|
| Path-sensitive resource cleanup | Implemented |
| HeapFree proof/discharge | Implemented |
| `@heap @reclaim(scope)` | Implemented for supported unique-resource pools |
| Arena/stack lifetime markers | Implemented |
| Static pool placement | Implemented |
| Dynamic `Many<T>` descriptor | Implemented |
| Checked dynamic Many growth | Implemented |
| Old-buffer reclamation on growth | Implemented |
| Addressable aggregate fields | Implemented |
| Field read/write/address | Implemented |
| Subobject provenance | Implemented |
| Borrow lifetime narrowing | Implemented + verifier enforced |
| Borrowed result across function boundary | Rejected pending explicit lifetime ABI |
| Explicit owned heap result transfer | Implemented |
| Explicit dynamic Many result transfer | Implemented |
| Caller resource re-ownership | Implemented |
| Missing transfer corruption rejection | Implemented |
| Detached SIR-C → native backend | Implemented |

## Earlier supported compiler slices retained

Typed bindings/parameters, checked integer arithmetic, control flow, `set`/`revise`, closed faults, typed fault payloads, `bypass`/`delete` cardinality handling, datasets, choices, pools, relations, authority, provenance, effect-token SSA, transactions/parallel region markers, rich representation lowering, COFF/PE generation.

## Not claimed complete

General shared ownership/reference counting, tracing GC, arbitrary cross-boundary borrowed references, fully dynamic generalized container library, nested resource destructors, complete transaction rollback/durability, production concurrency scheduler, complete STZ-3 standard-library/runtime coverage, and all optimizer passes.
