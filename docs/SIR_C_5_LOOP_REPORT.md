# Compiler 0.7.0 — SIR-C 5 Serializer / Backend / Verifier Loop

Status: **CLEAN / PASSING**

This report records the proof boundary requested for Compiler 0.7.0: target-neutral SIR-S is concretized into SIR-C 5.0, serialized, deserialized into a fresh graph, independently verified, and consumed by the LLVM/COFF/PE backend without frontend state.

## Pipeline proven

```text
.stz3
  -> AST / SSL / DLE
  -> SIR-S 4.x
  -> SIR-C 5.0
  -> serialize .sirc
  -> deserialize fresh SIR-C
  -> independent SIR-C verifier
  -> LLVM IR
  -> x86-64 COFF
  -> PE32+ .exe

.sirc
  -> deserialize fresh SIR-C
  -> independent SIR-C verifier
  -> LLVM IR
  -> x86-64 COFF
  -> PE32+ .exe
```

The source-driven and detached SIR-C paths emit byte-identical LLVM IR for every Compiler 0.7 acceptance fixture listed below.

## 0.7 acceptance fixtures

1. `borrow_cross_boundary.stz3` — shared-read borrowed result ABI (`@borrow_from`).
2. `borrow_mut_cross_boundary.stz3` — unique mutable borrowed result ABI (`@borrow_mut_from`).
3. `shared_scope.stz3` — automatic shared ownership and refcount cleanup.
4. `shared_manual_release.stz3` — explicit shared retain/release under manual reclaim.
5. `shared_return.stz3` — shared ownership transfer across an instruction boundary.
6. `send_resource.stz3` — explicit send-capability ownership transition.
7. `partial_field_move.stz3` — field resource extraction plus resource-component metadata.
8. `transaction_send.stz3` — transaction-scoped ownership transfer with serialized rollback obligation.

All eight source paths and all eight detached SIR-C paths produce structurally valid Windows x86-64 PE32+ executables.

## SIR-C 5 records exercised

SIR-C 5 now round-trips:

- result transfer kinds: `borrow-ref`, `shared-ref`, heap object, dynamic Many;
- shared-resource flags;
- concrete cleanup kinds including `shared-release`;
- resource `absorbed_by` linkage;
- resource-component records and guards;
- transaction rollback actions;
- borrow graph metadata embedded in the semantic function graph;
- ownership/provenance/resource-family metadata;
- concrete value storage and alias information.

Resource composition is no longer a zero-count placeholder. For example, the partial-field-move fixture contains a concrete component tying a `Holder.cell` byte offset to the absorbed heap child obligation, and the verifier requires a corresponding SIR-S ownership edge.

## Adversarial verification

Four new malformed SIR-C fixtures are permanently tested:

- `invalid_sirc07_bad_shared_flag.sirc` — removes the shared bit from a `SharedRelease` obligation; rejected.
- `invalid_sirc07_bad_component_link.sirc` — breaks resource `absorbed_by` linkage; rejected.
- `invalid_sirc07_bad_rollback.sirc` — changes the concrete rollback source operation; rejected.
- `invalid_sirc07_bad_cross_borrow.sirc` — corrupts cross-boundary borrow metadata; rejected.

These failures occur during detached verification, before LLVM code generation.

## Clean build result

The exact tree represented by this report was configured as a clean CMake **Release** build with Clang 17 and `-O0 -DNDEBUG` for bounded compile time in this environment. The compiler's warning policy remained enabled (`-Wall -Wextra -Wpedantic`).

```text
compiler warnings: 0
compiler errors:   0
CTest:             113 / 113 passed
failed tests:      0
```

The optimization override affects only how the C++ host compiler builds `stazec`; it does not change Staze SIR verification rules or the Windows target backend pipeline exercised by the tests.

## Current semantic boundary

Compiler 0.7 proves the SIR-C 5 serialization/backend/verifier loop for the implemented deep-ownership subset. Transaction rollback *obligations* are serialized and verified, but a general runtime transaction-abort/undo engine is still a later milestone. Likewise, resource-component records currently preserve and verify flattened child cleanup obligations rather than introducing a universal destructor runtime.
