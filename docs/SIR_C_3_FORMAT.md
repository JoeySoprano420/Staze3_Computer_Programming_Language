# SIR-C 3.0 — Concrete Representation Artifact

SIR-C 3.0 extends detached semantic SIR with a target representation plan. The artifact is canonical plaintext in this bootstrap compiler and is independently deserialized and verified before LLVM emission.

## Header

```text
SIR-C 3 0
module <hex-name>
target <profile> <triple> <pointer-bits> <object> <exe> <calling-convention> <aggregate-contract> <cardinality-contract> <fault-payload-contract>
```

## Concrete nominal layout

```text
layout <hex-name> <kind> <size> <align> <payload-offset> <element-size> <element-align> <capacity> <field-count>
loffset <byte-offset>
...
```

Kinds currently include scalar, text-view, optional, many-inline, aggregate, choice, borrow-ref and opaque.

## Concrete function plan

```text
concretefn <hex-name> <result-storage> <result-kind> <result-size> <result-align> <fault-size> <fault-align> <value-count>
```

This makes caller-result ABI and fault-buffer requirements explicit before LLVM.

## Concrete SSA value plan

```text
cvalue <ssa-id> <storage> <kind> <size> <align> <payload-offset> <element-size> <capacity> <alias-of> <escapes> <scalar-replaced>
```

Storage values are:

```text
register
stack
caller
pool-arena
heap
static
alias
```

The backend is required to consume these records. It is not permitted to reinterpret SIR-S by reaching back into the AST/SSL.

## Verification

The 0.5 independent verifier checks, among other existing SIR rules:

- SIR-C major version 3;
- exact Windows x86-64 target contract;
- unique/nonempty concrete nominal layouts;
- nonzero power-of-two alignments;
- field/payload offsets remain within storage;
- every semantic function has one concrete function plan;
- every semantic SSA value has exactly one concrete value plan;
- aliases do not self-alias;
- concrete size/alignment cannot be zero.

A corrupted fixture with zero concrete storage size is included and rejected by the test suite.
