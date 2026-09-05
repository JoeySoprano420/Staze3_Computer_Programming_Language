# Compiler 0.8 — Transactional State & Structured Concurrency Realization

## 1. Design rule

SIR-S states semantic obligations. SIR-C states the target-specific mechanism that realizes those obligations. LLVM is not allowed to invent transaction or task semantics.

## 2. Transaction path

```text
transaction source
  -> transaction region
  -> effectful SIR operation
  -> rollback obligation
  -> SIR-C undo action
  -> independent correspondence verification
  -> native undo state
```

### 2.1 Field restoration

Before a transaction-scoped `field.store`, the native backend copies the exact old field bytes into statically allocated undo storage and arms one undo bit. Abort restores those bytes; commit clears the active bit.

### 2.2 Created resources

A heap/shared resource created inside a transaction is armed only after successful allocation/retain. If the transaction aborts, the inverse action releases/frees that acquired resource. If the transaction commits, the undo record is disarmed and normal lifetime cleanup remains authoritative.

### 2.3 Ownership transitions

Move/send/field-move transitions create `restore-owner` obligations. Whole-value alias-style ownership restoration requires no byte copy in the current representation; pointer-bearing field moves restore the removed pointer into its exact field offset.

### 2.4 Reverse order

Abort executes active concrete undo actions in reverse `(source_op, undo_id)` order. This preserves LIFO inverse ordering for the statically bounded reference transaction profile.

## 3. Why transactions are single-pass in 0.8

The 0.8 concrete plan reserves a fixed amount of undo state. A loop inside a transaction could execute one semantic mutation an unbounded number of times, which would require a dynamic undo log. The compiler therefore rejects such source rather than silently reusing one snapshot incorrectly.

Nested transactions, irreversible release and dynamic-Many mutation are rejected for the same reason: their correct inverse semantics require a stronger log/commit model than the fixed 0.8 plan.

## 4. Structured task graph

Each `task` creates a child task region under a `parallel` region. Task boundaries are first-class SIR instructions (`TaskBegin`/`TaskEnd`). Captures are synthesized from SSA uses crossing into the task.

Capture modes:

```text
copy-read    immutable non-resource value
shared-read  shared resource reference
send         transferred resource authority
```

## 5. Lifetime proof

For every task capture:

```text
task_region must be within captured_value.region
```

The semantic verifier checks this after deserialization. The release suite contains a malicious `.sirs` artifact that preserves otherwise-valid task metadata but moves the captured value to an unrelated sibling lifetime region; it is rejected before target concretization.

## 6. Concrete task plan

SIR-C records:

```text
taskplan
  id
  parallel_region
  task_region
  begin_op
  end_op

ctaskcapture
  id
  parallel_region
  task_region
  value
  provenance
  mode
```

The concrete verifier requires one plan per semantic task region and exact agreement with SIR-S capture records.

## 7. Reference scheduler

The Windows x86-64 0.8 reference target executes task regions deterministically in declaration order and does not leave the enclosing parallel region until every task has completed. This yields an executable structured-concurrency profile without making a host C++ threading library the definition of Staze concurrency.

A later backend may schedule verified tasks concurrently while preserving the same SIR-S capture/ownership contracts.
