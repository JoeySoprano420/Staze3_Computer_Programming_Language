# Roadmap after Compiler 0.6.0

Compiler 0.6 closes the first unique-resource lifetime loop: allocation, provenance, path-sensitive cleanup, transfer, caller re-ownership, dynamic Many replacement, and detached verification.

A strong next milestone is **Compiler 0.7 — Deep Ownership, Borrow Graphs & Resource Composition**:

- explicit cross-boundary borrow lifetime contracts;
- mutable/unique/shared borrow modes;
- nested aggregate resource fields and recursive cleanup plans;
- partial moves and field moves;
- resource-containing choices/optionals/Many values;
- structured manual release operations;
- shared ownership/reference-count contracts where explicitly selected;
- richer escape/region inference;
- transaction-aware ownership transfer and rollback obligations;
- concurrency send/share capabilities and race-proof resource transitions;
- production-grade lifetime diagnostics with provenance paths.

The governing constraint remains: these extensions must enrich SIR-S semantics and SIR-C proofs without making LLVM, C++, or a hidden runtime the source of language meaning.
