# Roadmap after Compiler 0.8.0

Compiler 0.8 closes the first executable transaction/task loop: semantic rollback obligations and task captures survive detached SIR, become concrete SIR-C 6.0 plans, are independently verified, and reach native Windows x86-64 code.

A strong next milestone is **Compiler 0.9 — Dynamic Transactions, Native Relation Storage & Concurrent Scheduling**:

- dynamic/bounded transaction undo logs for loops and repeated mutations;
- nested transaction savepoints and commit/abort composition;
- dynamic-Many mutation under transactional ownership;
- concrete native relation storage with key identity, uniqueness and cardinality enforcement;
- reversible native relation link/unlink;
- a verified concurrent worker scheduler for eligible task graphs;
- task result/join values and fault aggregation;
- race/conflict analysis across mutable/shared captures;
- send/share realization across actual worker boundaries;
- stronger SABI-3 interop for borrowed/shared/task values;
- richer diagnostics for transaction/task proof failures.

The governing constraint remains unchanged: SIR-S defines meaning, SIR-C proves a target realization, and LLVM implements the verified plan rather than becoming the language's semantic authority.
