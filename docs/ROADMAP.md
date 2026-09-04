# Roadmap after Compiler 0.5.0

Compiler 0.5 establishes **Concrete Rich Representation Lowering** without changing SIR-S meaning.

## Compiler 0.6 — Lifetime/Resource Realization

Primary goals:

1. path-sensitive cleanup insertion;
2. `HeapFree` scheduling from proven ownership/lifetime/reclaim policy;
3. automatic scope/resource cleanup without C++ RAII leakage into Staze semantics;
4. dynamic/growing `Many<T>` with explicit capacity/reallocation faults;
5. aggregate field projection/revision and addressable subobjects;
6. richer borrow/reference address operations with provenance and lifetime verification;
7. pool reset/reclaim operations;
8. escape-to-heap only when SIR/DLE proves it is required;
9. native representation for richer fault payloads containing aggregates/cardinality values.

## Compiler 0.7 — Transaction and Relation Runtime

- concrete relation stores and indexes;
- transactional mutation log/effect tokens;
- commit/abort/rollback boundaries;
- compensation rules for external effects;
- dataset query/update primitives;
- cardinality invariant enforcement at mutation boundaries.

## Compiler 0.8 — Concurrency Realization

- parallel region dependency analysis;
- data-race proof using authority/provenance facts;
- deterministic join semantics;
- scheduler/executor abstraction with explicit failure;
- atomic/synchronization primitives in SIR-C.

## Later production work

- stable SIR binary encoding and version compatibility;
- independent verifier executable/process;
- full target packs beyond Windows x86-64;
- LLVM backend as replaceable implementation rather than semantic authority;
- full STZ-3 definitions, packages, modules, representation and ABI surfaces;
- self-hosted compiler stages.
