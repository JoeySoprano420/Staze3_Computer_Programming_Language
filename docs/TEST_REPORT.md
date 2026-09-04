# Compiler 0.5.0 Validation Report

## Clean build

A clean Release configuration/build was performed after all 0.5 source changes.

Compiler flags on this host include:

```text
-Wall -Wextra -Wpedantic
```

Result:

```text
C++ compiler warnings: 0
```

The generated LLVM-to-COFF command suppresses Clang's irrelevant module-triple override warning while still explicitly targeting `x86_64-pc-windows-msvc`.

## Automated suite

```text
34 / 34 tests passed
0 failed
```

The suite covers the previous detached semantic compiler checks plus 0.5 representation tests.

### 0.5 positive checks

- heap allocation semantic/SIR verification;
- native typed fault payload verification;
- rich aggregate caller-storage verification;
- source → rich cardinality PE;
- detached rich-cardinality SIR-C → PE;
- source → rich resources PE;
- source → heap allocation PE;
- source → typed fault payload PE;
- source → rich caller-storage PE.

### 0.5 negative checks

- `@heap @reclaim(scope)` is rejected because automatic heap reclamation is not yet implemented;
- a SIR-C 3.0 value plan with zero concrete storage size is rejected;
- the 0.4-era effect-token corruption test was regenerated as SIR-C 3.0 and still rejects;
- the missing-move-authority corruption test was regenerated as SIR-C 3.0 and still rejects.

## PE validation

The following generated files were independently identified by the host `file` utility as Windows x86-64 PE32+ console executables:

```text
rich_cardinality.exe
rich_resources.exe
rich_caller_storage.exe
heap_allocation.exe
fault_payload_native.exe
```

All report:

```text
PE32+ executable for MS Windows ... x86-64
```

## Detached SIR-C equivalence

For two rich programs, compilation from source and a second invocation starting from serialized SIR-C produced byte-identical PE files.

```text
rich_cardinality
SHA-256 34b5c7dd7f8bde1cd735127c4bdbcb116c18281c8e1f1b09faaeb328f943113f

rich_caller_storage
SHA-256 e5dd8291b0953dd76ba0ad12cec634f390f9babf9d85aedac7c37c54d76d9f00
```

This demonstrates that the backend does not need source, tokens, AST, SSL, or SIR-S once canonical SIR-C 3.0 exists.

## Native heap proof

`heap_allocation.exe` imports and uses:

```text
KERNEL32.dll
GetProcessHeap
HeapAlloc
```

The generated LLVM checks the returned pointer for null and branches to the SIR `AllocationFailure` target.

## Native typed fault payload proof

`fault_payload_native.ll` contains:

```llvm
store i32 42, ptr %fault_out, align 4
```

This is the typed `Missing(value:i32)` payload being written into caller-owned native fault storage before status propagation.

## Caller-storage ABI proof

`rich_caller_storage.ll` contains a rich-return function shaped as:

```llvm
define i32 @stz_MakePair(ptr %result_out, ptr %fault_out, i64 %arg0, i64 %arg1)
```

and a rich-parameter function shaped as:

```llvm
define { i32, i64 } @stz_Consume(ptr %fault_out, ptr %arg0)
```

The caller allocates the aggregate storage, passes it to `MakePair`, then passes the resulting pointer to `Consume`.

## Runtime-execution boundary

The current development host is Linux. The generated files were validated structurally as PE32+ and through object/import/LLVM inspection, but they were not executed by a native Windows loader in this validation run. The report therefore does not claim Windows runtime execution.

## SIR-S semantic invariance against Compiler 0.4

Compiler 0.4 was rebuilt from the previous release package and the same existing rich fixtures were lowered again. Their SIR-S artifacts were compared byte-for-byte with Compiler 0.5.

```text
rich_cardinality.sirs
0.4 SHA-256 c4c3bec1caace83e633557f40974127b5db4aaac98ebb4abc3b85eb96a026556
0.5 SHA-256 c4c3bec1caace83e633557f40974127b5db4aaac98ebb4abc3b85eb96a026556
byte-identical: YES

rich_resources.sirs
0.4 SHA-256 c850f387462ad97dcb206b51e785f96427bcaee9afdd90b732cac49c2a4400b9
0.5 SHA-256 c850f387462ad97dcb206b51e785f96427bcaee9afdd90b732cac49c2a4400b9
byte-identical: YES
```

For the pre-existing rich language subset, the representation milestone therefore changed SIR-C/native realization while leaving serialized SIR-S meaning unchanged.
