# Staze C++23 Compiler 0.7.0 — Deep Ownership & SIR-C 5.0

Compiler 0.7.0 extends the detached Staze compiler architecture with cross-boundary borrow contracts, mutable/shared borrow modes, shared heap ownership, explicit retain/release, send-capability transitions, partial field moves, concrete resource composition, transaction rollback obligations, and SIR-C 5.0.

## Compiler architecture

```text
.stz3
  -> Lexer / Parser / AST
  -> SSL-3
  -> DLE-3
  -> canonical SIR-S 4.x
  -> serialize / deserialize / independent verify
  -> SIR-C 5.0
  -> serialize / deserialize / independent verify
  -> LLVM
  -> x86-64 COFF
  -> Windows PE32+
```

The backend can also start directly from a standalone `.sirc` artifact. In that mode it never constructs source tokens, AST, SSL, DLE, or SIR-S objects.

## 0.7 ownership features

- `@borrow_from(parameter)` shared-read returned borrow contract;
- `@borrow_mut_from(parameter)` unique mutable returned borrow contract;
- `borrow` / `borrow_mut` parameter authority modes;
- explicit borrow graph and cross-boundary borrow edges;
- `retain(...)` / `share(...)` for shared ownership;
- `release(...)` for explicit manual resource/shared release;
- `@ownership(shared)` heap pools with atomic refcount header;
- shared result transfer ABI;
- `send(...)` ownership transition;
- partial resource field move (`move(owner.field)`);
- concrete resource-component records with `absorbed_by` proof;
- transaction rollback obligation serialization;
- provenance-chain verification diagnostics.

## SIR-C 5 proof loop

The final 0.7 acceptance loop is clean:

```text
compiler warnings: 0
compiler errors:   0
CTest:             113 / 113 passed
```

Eight new 0.7 programs are built both from source and again from their detached SIR-C. For every pair, generated LLVM is byte-identical, and both paths produce valid Windows x86-64 PE32+ executables.

Four deliberately corrupted SIR-C 5 artifacts are also rejected before LLVM: bad shared ownership, broken component absorption, rollback mismatch, and corrupted cross-boundary borrow metadata.

See:
- `docs/SIR_C_5_LOOP_REPORT.md`
- `docs/SIR_C_5_FORMAT.md`
- `docs/TEST_REPORT_0.7.0.md`
- `docs/CTEST_OUTPUT_0.7.0.txt`
- `docs/BUILD_OUTPUT_0.7.0.txt`

## Build

```bat
cmake -S . -B build -A x64
cmake --build build --config Release
build\Release\stazec.exe examples\shared_return.stz3 -o shared_return.exe
```

The native linker path uses Clang's Windows target support plus `lld-link`; `STAZE_CLANG` and `STAZE_LLD_LINK` can override executable paths.

## Important boundary

Compiler 0.7 implements and verifies transaction rollback **obligations**, but does not claim a general runtime rollback/undo engine yet. Resource composition uses verified flattened child obligations rather than a hidden universal destructor runtime.
