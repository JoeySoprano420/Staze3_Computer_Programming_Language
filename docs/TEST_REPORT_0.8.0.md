# Staze C++23 Compiler 0.8.0 — Final Validation Report

## Release result

The exact hardened 0.8.0 source/test tree was configured and compiled from a fresh external build directory.

```text
Host C++ compiler : GNU C++ 14.2.0
Language mode      : C++23
Configuration      : Release
Warning policy     : -Wall -Wextra -Wpedantic
Compiler warnings  : 0
Compiler errors    : 0
CTest               : 170 / 170 passed
CTest failures      : 0
```

The raw configure/build/test transcripts are shipped as:

- `CONFIGURE_OUTPUT_0.8.0.txt`
- `BUILD_OUTPUT_0.8.0.txt`
- `CTEST_OUTPUT_0.8.0.txt`

## 0.8 positive native specimens

The clean compiler generated source-driven and detached-SIR-C Windows x86-64 PE32+ executables for:

1. `transaction_field_undo`
2. `transaction_observable_rollback`
3. `transaction_resource_abort`
4. `transaction_shared_abort`
5. `parallel_tasks`
6. `parallel_shared_task`
7. `parallel_send_task`

`file(1)` identifies all fourteen source/detached executables as PE32+ Microsoft Windows console x86-64 files.

## Detached-backend equivalence

For all seven 0.8 native specimens:

```text
source -> SIR-C -> LLVM
```

and

```text
standalone .sirc -> LLVM
```

produce byte-identical LLVM IR.

The source-built and detached-built PE files may differ in link timestamp/metadata, so the release also extracts and compares their `.text` sections. All seven source/detached pairs have byte-identical machine-code `.text` sections.

Exact hashes are in `DETACHED_EQUIVALENCE_0.8.0.csv` and `MACHINE_CODE_EQUIVALENCE_0.8.0.txt`.

## Detached SIR-S concretization proof

Two representative graphs are also checked through the earlier boundary:

- `transaction_observable_rollback.sirs`
- `parallel_shared_task.sirs`

For both, direct source concretization and fresh-process-style `.sirs -> SIR-C` concretization produce byte-identical SIR-C files.

## Executable transaction evidence

`transaction_observable_rollback` proves externally relevant field rollback:

- old field bytes are copied into concrete undo storage;
- the undo record is armed only after the snapshot;
- the transaction mutation executes;
- the abort path tests the active bit;
- old bytes are copied back before fault propagation;
- the record is disarmed after restoration.

`transaction_resource_abort` proves an allocation created in a transaction can be destroyed on abort using `HeapFree` and a concrete resource ownership slot.

`transaction_shared_abort` proves shared-retain compensation through the atomic shared reference-count helper and abort-side shared release.

See `NATIVE_EVIDENCE_0.8.0.txt` and the generated LLVM files under `dist/0.8/`.

## Executable structured-task evidence

The 0.8 task specimens prove:

- explicit semantic `parallel` and `task` regions;
- SIR-S task-capture records;
- concrete SIR-C `taskplan` and `ctaskcapture` records;
- source/deattached verifier correspondence;
- deterministic structured native control flow;
- shared-read and send capture modes.

The reference profile executes tasks in declared order and joins them before parallel exit; it does not claim OS-thread execution.

## Negative/corruption coverage

The full 170-test suite retains earlier semantic/ownership/resource rejection tests and adds 0.8-specific failures including:

- task outside parallel region;
- mutation inside the immutable-capture task profile;
- loop inside statically bounded transaction;
- irreversible explicit release inside transaction;
- corrupted undo kind;
- corrupted undo field offset;
- corrupted concrete TaskEnd identity;
- corrupted concrete task capture;
- **task capture that outlives the captured value's lifetime region**.

The final lifetime attack is a standalone malicious `.sirs` artifact. It preserves otherwise valid task metadata but places the captured value in an unrelated sibling lifetime region. The detached semantic verifier rejects it with:

```text
SIR verifier: task capture outlives captured value lifetime
```

## PE inspection

The release includes `PE_INSPECTION_0.8.0.txt`.

Resource-transaction executables import the expected KERNEL32 allocation APIs:

```text
GetProcessHeap
HeapAlloc
HeapFree
ExitProcess
```

The environment used for this build does not include Wine or a Windows loader, so the report distinguishes **successful native PE generation and structural/machine-code validation** from actually launching the PE on Windows. The same sources are intended to run on a Windows x86-64 system after normal compilation/linking.

## Reference-profile boundaries

0.8 is complete for its declared transaction/task reference profile. It deliberately does not claim:

- nested/dynamic transaction logs;
- transaction loops/repeated dynamic undo entries;
- dynamic-Many mutation inside transactions;
- a persistent native relation table;
- OS-thread parallel scheduling;
- complete STZ-3 standard-library/runtime conformance.

Those are explicitly deferred rather than silently approximated.
