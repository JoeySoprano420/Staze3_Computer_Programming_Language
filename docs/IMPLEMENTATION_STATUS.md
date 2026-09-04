# STZ-3 Compiler 0.5.0 — Implementation Status

| Area | Status | Notes |
|---|---|---|
| Lexer/parser | Milestone subset | Existing 0.4 grammar plus 0.5 heap constraints |
| SSL/DLE | Active | SIR-S meaning unchanged; concrete selection added after SIR-S |
| Canonical SIR-S | Implemented milestone | Detached semantic SSA/CFG/fault/effect/resource graph |
| Canonical SIR-C | **3.0 implemented** | Detached concrete Windows x86-64 representation plan |
| register scalar placement | Implemented | exact-One scalar values |
| rich stack placement | Implemented | fixed-size rich temporaries |
| caller rich return storage | Implemented | hidden result pointer |
| rich parameters | Implemented | passed by address |
| pool/arena placement | Implemented | explicit `@arena` pool values |
| explicit heap placement | Implemented | `GetProcessHeap` + `HeapAlloc` |
| automatic heap reclaim | **Not yet** | 0.5 requires `@reclaim(manual)` for `@heap` |
| allocation failure | Implemented milestone | null pointer routes to `AllocationFailure` SIR edge |
| optional layout | Implemented | tag + aligned payload |
| Many layout | Implemented for compiler-bounded capacity | length + inline elements; phi capacity promotion |
| dynamic/growing Many | Not yet | needs explicit allocator/reallocation semantics |
| dataset aggregate layout | Implemented | size/alignment/field offsets |
| choice layout | Implemented | i32 tag + max aligned payload |
| rich phi | Implemented | pointer phi over agreed concrete representation |
| typed fault payload transport | Implemented | caller-owned byte buffer |
| escape analysis | Implemented conservative milestone | return/fault/call escape + alias propagation |
| scalar replacement | Implemented narrow milestone | nonescaping aggregate/choice semantic-only uses |
| provenance/authority/effect SSA | Retained and verified | from 0.4 |
| relation runtime storage | Not yet | semantic/effect operation only |
| transaction rollback/durability | Not yet | semantic/effect regions only |
| parallel scheduler | Not yet | semantic/effect regions only |
| LLVM backend | Implemented for stated 0.5 subset | consumes SIR-C concrete plans |
| x86-64 COFF | Implemented | generated with Clang target backend |
| PE32+ link | Implemented | `lld-link`, KERNEL32 import library |

## Executed placement fixtures

- Register: every scalar arithmetic/control program.
- Stack: rich cardinality and aggregate temporaries.
- Caller: `rich_caller_storage.stz3` and rich return functions in `rich_cardinality.stz3`.
- Pool-arena: `rich_resources.stz3` / `Frame`.
- Heap: `heap_allocation.stz3` / `HeapCell`.
