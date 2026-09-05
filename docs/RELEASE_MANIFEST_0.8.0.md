# Compiler 0.8.0 Release Manifest

## Milestone

**Transactional State & Structured Concurrency Realization**

## Canonical IR versions

- SIR-S: 5.x
- SIR-C: 6.0

## Primary release evidence

- `dist/0.8/transaction_observable_rollback.sirs`
- `dist/0.8/transaction_observable_rollback.sirc`
- `dist/0.8/transaction_observable_rollback.ll`
- `dist/0.8/transaction_observable_rollback.exe`
- `dist/0.8/parallel_shared_task.sirs`
- `dist/0.8/parallel_shared_task.sirc`
- `dist/0.8/parallel_shared_task.ll`
- `dist/0.8/parallel_shared_task.exe`

Every 0.8 native acceptance specimen also has `source.*` and `detached.*` artifacts for equivalence testing.

## Validation

- clean C++23 Release build;
- zero compiler warnings under `-Wall -Wextra -Wpedantic`;
- 170/170 CTest tests passed;
- 7/7 source-vs-detached LLVM pairs byte-identical;
- 7/7 source-vs-detached PE `.text` sections byte-identical;
- 2/2 representative SIR-S-to-SIR-C detached concretization pairs byte-identical;
- all generated native acceptance executables identified as Windows x86-64 PE32+.
