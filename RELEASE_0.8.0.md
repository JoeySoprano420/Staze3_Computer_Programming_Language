# Staze C++23 Compiler 0.8.0 — Finalized Release

Milestone: **Transactional State & Structured Concurrency Realization**.

Canonical semantic IR: **SIR-S 5.x**.
Canonical Windows x86-64 concrete IR: **SIR-C 6.0**.

Final validation:

```text
C++23 clean Release build : PASS
-Wall -Wextra -Wpedantic : 0 warnings
CTest                     : 170 / 170 passed
Source/detached LLVM       : 7 / 7 byte-identical
Source/detached PE .text   : 7 / 7 byte-identical
Representative .sirs->.sirc: 2 / 2 byte-identical
Windows x86-64 PE32+ output: PASS
```

See `docs/TEST_REPORT_0.8.0.md` for the full evidence and explicit reference-profile boundaries.
