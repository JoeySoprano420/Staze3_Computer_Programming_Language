# STZ-3.0 Grounding Notes for Compiler 0.3

> **0.4 status:** Rich Semantic SSA now carries cardinality, fault payloads, resource regions/provenance, capabilities and effect-token ordering. Unsupported STZ-3 behavior remains deliberately gated rather than inferred from C++/LLVM.


This milestone is intentionally shaped by the ratified STZ-3.0 standard rather than by C/C++ convenience.

Key rules implemented or preserved in architecture:

1. `=` is equality, never mutation.
2. `:=` creates a binding.
3. `set` and `revise ... by ...` remain separate effects.
4. A local/parameter `revise` qualifier grants replacement/relative-revision authority; it does not grant every other capability.
5. Safe integer arithmetic has checked overflow semantics independent of debug/release mode.
6. Division by zero is a typed fault; signed minimum divided by `-1` is overflow.
7. Fault sets are closed typed outcomes rather than hidden language exceptions.
8. `bypass` substitutes a continuation/value.
9. `delete` follows result cardinality and is illegal for an exact-One result by itself.
10. Datasets are semantic record schemas whose ordinary in-memory layout is not fixed by source order.
11. Relation identity/cardinality/ownership are explicit; an English relation name does not imply ownership.
12. Pools are semantic lifetime/storage/responsibility regions; a pool need not become one runtime object.
13. SSL is the authoritative resolved semantic model.
14. DLE chooses only legal realizations.
15. SIR is independently verified before native backend production.
16. LLVM is a backend mechanism, not the definition of Staze semantics.

Compiler 0.3 implements a subset of these rules and creates explicit phase boundaries for the rest. Unsupported Standard behavior must be added deliberately rather than inherited from the C++ host or LLVM backend.
