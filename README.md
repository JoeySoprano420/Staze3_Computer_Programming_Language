# Staze C++23 Compiler 0.8.0 — Transactional State & Structured Concurrency Realization

Compiler 0.8.0 is the next executable STZ-3 compiler milestone built on the detached semantic/concrete IR boundary established in 0.3–0.7.

The defining 0.8 change is that transaction rollback and structured task regions are no longer merely semantic annotations. They are represented in canonical SIR-S, concretized into verifier-backed SIR-C 6.0 plans, serialized/deserialized as standalone artifacts, independently verified, and realized by the Windows x86-64 LLVM/COFF/PE backend.

## Architecture

```text
.stz3
  -> Lexer / Parser / AST
  -> SSL-3 milestone facts
  -> DLE-3 milestone lowering
  -> canonical SIR-S 5.x
       * SSA / CFG / phi
       * faults + payloads
       * effect-token SSA
       * lifetime / provenance / ownership / authority
       * borrow + ownership graphs
       * transaction regions + rollback obligations
       * parallel/task regions + capture contracts
  -> serialize
  -> deserialize into a fresh graph
  -> independent SIR-S verification
  -> SIR-C 6.0 target concretization
       * storage/layout/ABI plans
       * cleanup/resource obligations
       * executable transaction undo plans
       * structured task plans + concrete captures
  -> serialize
  -> deserialize into a fresh graph
  -> independent SIR-C verification
  -> LLVM IR
  -> x86-64 COFF
  -> Windows PE32+ .exe
```

The backend can also begin directly from `.sirs` or `.sirc`. A `.sirc -> LLVM -> COFF -> PE` invocation never constructs source tokens, AST, SSL, DLE, or SIR-S objects.

## 0.8 transaction realization

A transaction is represented by a semantic region plus rollback obligations. SIR-C 6.0 resolves each obligation into a concrete undo action. The reference backend reserves undo state in the function frame, arms actions only after the corresponding effect occurs, executes armed actions in reverse order on `transaction.abort`, and disarms them on commit.

Native undo actions in the 0.8 reference profile include:

- field-byte restoration;
- destruction of a resource created inside the transaction;
- shared-retain compensation;
- ownership restoration for move/send/field-move transitions.

The semantic IR also retains relation inverse obligations. The current Windows reference relation provider remains effect-ordered/abstract, so relation-link/unlink have no native table state in 0.8 and therefore no observable bytes to restore. This is explicitly documented rather than presented as a native relation database implementation.

Reference transactions are deliberately **single-pass and statically bounded**. Nested transactions, `while`/`loop`, irreversible explicit `release`, and dynamic-Many mutation inside a transaction are rejected by the 0.8 profile rather than accepted without a sound undo-log model.

## 0.8 structured task realization

```stz3
parallel
{
    task
    {
        // immutable captures are inferred and recorded
    }
}
```

SIR-S records every task region and every captured SSA value. Capture modes are explicit:

- `copy-read` for immutable non-resource values;
- `shared-read` for explicitly shared resources;
- `send` for ownership transfer into the task region.

The verifier checks capture mode, provenance, authority, task/parallel nesting, and lifetime containment. A task capture is invalid if the task region outlives the captured value's region.

SIR-C 6.0 binds each semantic task region to exact `TaskBegin` / `TaskEnd` operations and preserves each capture contract in a concrete task plan.

The Windows x86-64 **reference task scheduler is deterministic structured execution**: task regions execute in source-declared order and all tasks are joined before the enclosing parallel region exits. This is a fully executable profile, but it deliberately does not claim OS-thread parallelism. The detached IR preserves the information required for a later genuinely concurrent scheduler.

## Retained 0.7 ownership model

0.8 retains and verifies:

- `@borrow_from(parameter)` and `@borrow_mut_from(parameter)`;
- shared-read and unique-mut borrow graphs;
- partial resource field moves;
- explicit `retain` / `share` / `release`;
- `@ownership(shared)` heap pools with native atomic refcounting;
- `send(...)` ownership transitions;
- resource-composition records;
- path-sensitive cleanup and lifetime realization;
- dynamic `Many<T>` allocation/growth/reclamation;
- owned/shared result transfer ABIs;
- detached SIR-C native generation.

## Build

### CMake

```bat
cmake -S . -B build -A x64
cmake --build build --config Release
build\Release\stazec.exe examples\transaction_observable_rollback.stz3 -o rollback.exe
```

### Detached backend

```bat
stazec examples\parallel_shared_task.stz3 --check --emit-sir-c parallel_shared_task.sirc
stazec --from-sir-c parallel_shared_task.sirc -o parallel_shared_task.exe
```

Environment overrides:

```text
STAZE_CLANG      path/name of clang
STAZE_LLD_LINK   path/name of lld-link
```

## Release validation

The final package contains the exact source/test/documentation tree used for the 0.8.0 release validation, plus the generated release evidence under `dist/` and raw clean build/CTest logs under `docs/`.

See:

- `docs/TRANSACTION_STRUCTURED_CONCURRENCY.md`
- `docs/SIR_C_6_FORMAT.md`
- `docs/IMPLEMENTATION_STATUS.md`
- `docs/TEST_REPORT_0.8.0.md`
- `docs/CTEST_OUTPUT_0.8.0.txt`
- `docs/BUILD_OUTPUT_0.8.0.txt`

## Final 0.8.0 validation

```text
clean Release build : PASS
C++ warnings        : 0
C++ errors          : 0
CTest               : 170 / 170 PASS
0.8 LLVM equivalence: 7 / 7 PASS
0.8 .text equivalence: 7 / 7 PASS
SIR-S -> SIR-C equivalence: 2 / 2 PASS
PE32+ generation    : PASS for all 0.8 native acceptance specimens
```

The final malicious lifetime fixture also passes as a rejection test: a detached task capture cannot outlive the captured SSA value's lifetime region.
