# Compiler 0.7.0 Validation Report

## Clean compiler build

- CMake build type: Release
- Host compiler: Clang 17
- Host optimization override: `-O0 -DNDEBUG` (bounded compile-time environment)
- Warning policy: `-Wall -Wextra -Wpedantic`
- Compiler warnings: **0**
- Compiler errors: **0**

## CTest

```text
113 / 113 passed
0 failed
```

The suite contains the complete frozen Compiler 0.6 regression suite plus Compiler 0.7 SIR-C 5 tests for cross-boundary borrow, mutable borrow, shared retain/release, shared return, send, partial field move/resource composition, transaction rollback metadata, detached backend builds, LLVM equivalence, and malicious SIR-C rejection.

See `CTEST_OUTPUT_0.7.0.txt` for the complete run.
