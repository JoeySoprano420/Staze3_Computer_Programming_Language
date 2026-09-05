# STZ-3 Compiler 0.8.0 — Implementation Status

## 0.8 milestone: implemented and executable

| Capability | Status |
|---|---|
| Canonical SIR-S 5.x transaction/task semantics | Implemented |
| Detached SIR-S serialize/deserialize/verify | Implemented |
| SIR-C 6.0 executable undo plans | Implemented |
| SIR-C 6.0 structured task plans | Implemented |
| Detached SIR-C serialize/deserialize/verify | Implemented |
| Field-mutation snapshot/restore | Implemented |
| Resource-created-in-transaction destruction | Implemented |
| Shared-retain compensation on abort | Implemented |
| Ownership move/send restoration plan | Implemented |
| Reverse-order abort execution | Implemented |
| Commit disarm | Implemented |
| Cross-call observable field rollback | Implemented + native PE fixture |
| `parallel { task { ... } }` parsing/semantic regions | Implemented |
| `copy-read` task capture | Implemented |
| `shared-read` task capture | Implemented |
| `send` task capture | Implemented |
| Task/parallel nesting proof | Implemented |
| Task-capture provenance/authority proof | Implemented |
| Task-capture lifetime-containment proof | Implemented + malicious detached-SIR rejection |
| Concrete TaskBegin/TaskEnd plan proof | Implemented |
| Deterministic structured reference scheduler | Implemented |
| Source-vs-detached SIR-C LLVM equivalence | Implemented/tested |
| LLVM -> x86-64 COFF -> PE32+ | Implemented/tested |

## Retained and regression-tested from earlier milestones

- typed bindings and parameters;
- checked integer arithmetic and explicit overflow faults;
- `if`, loops, `set`, `revise`;
- closed faults, typed fault payloads, `bypass`, `delete`;
- `One` / `ZeroOrOne` / `Many` semantic cardinality;
- aggregates and choices;
- pools, lifetime regions, provenance, authority and effect-token SSA;
- stack/register/caller/arena/heap placement;
- native optional/Many/aggregate/choice layouts;
- dynamic Many growth and reclamation;
- path-sensitive cleanup and `HeapFree` proof;
- addressable aggregate fields and subobject provenance;
- explicit owned resource transfer;
- shared ownership with atomic retain/release;
- cross-boundary borrow contracts;
- partial field moves and resource-composition proof;
- `send` capability and transaction rollback obligations;
- independent SIR-C backend entry.

## Exact 0.8 reference-profile boundaries

These are deliberate profile boundaries, not hidden partial implementations:

- structured tasks execute deterministically in declared order; 0.8 does not claim an OS-thread scheduler;
- transactions are statically bounded/single-pass: no nested transaction, loop, irreversible explicit release, or dynamic-Many mutation inside a transaction;
- relation operations are typed/effect-ordered SIR operations, but the Windows 0.8 reference target does not provide a persistent/native relation table yet;
- text parameters are not admitted by the current native ABI;
- explicit nested `shadow` syntax remains reserved;
- this is a production-style compiler milestone, not a claim of complete STZ-3 language/standard-library conformance.
