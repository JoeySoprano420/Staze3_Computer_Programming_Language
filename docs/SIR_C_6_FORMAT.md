# SIR-C 6.0 — Transaction Undo & Structured Task Concrete Format

SIR-C 6.0 extends the 5.x concrete ownership/resource format. It remains a standalone, line-oriented canonical artifact consumed by the independent verifier and LLVM backend.

## Header

```text
SIR-C 6 0
module <hex-name>
target <profile> <triple> <pointer-bits> <object> <exe> <cc> <aggregate-contract> <cardinality-contract> <fault-payload-contract>
```

## Concrete function header

```text
concretefn <hex-name>
           <result-storage>
           <result-kind>
           <result-transfer>
           <result-size> <result-align>
           <fault-payload-size> <fault-payload-align>
           <value-count>
           <resource-count>
           <component-count>
           <cleanup-count>
           <rollback-count>
           <undo-count>
           <task-count>
           <task-capture-count>
```

The 6.0 additions are `undo-count`, `task-count`, and `task-capture-count`.

## Executable undo record

```text
undo <id>
     <transaction-region>
     <source-op>
     <kind>
     <target-value>
     <aux-value>
     <byte-offset>
     <size>
     <align>
     <hex-semantic-name>
```

Undo kinds currently serialized:

```text
restore-field
relation-unlink
relation-link
restore-owner
destroy-resource
shared-release
```

For every semantic rollback obligation, SIR-C must contain exactly one matching concrete undo plan. The verifier checks source opcode, target/aux values, offsets/layout, resource identity and semantic name as appropriate.

`relation-link` / `relation-unlink` remain representation records for the typed relation effect model; the Windows 0.8 reference provider does not maintain a native persistent relation table, so those inverse records do not mutate native table bytes in this target profile.

## Structured task plan

```text
taskplan <id> <parallel-region> <task-region> <begin-op> <end-op>
```

Requirements:

- nonzero unique identity;
- task region exists and is of kind `task`;
- declared parallel region exists and is the structured ancestor;
- `begin-op` is `TaskBegin` in the task region;
- `end-op` is `TaskEnd` in the same task region;
- every semantic task region has exactly one concrete task plan.

## Concrete task capture

```text
ctaskcapture <id> <parallel-region> <task-region> <value> <provenance> <hex-mode>
```

The verifier requires exact correspondence with the SIR-S task-capture record and independently verifies the embedded semantic graph, including lifetime containment.

## Canonical detached loop

```text
SIR-C bytes
   -> deserialize
   -> independent SIR-C verifier
   -> LLVM emitter
```

The backend does not consult AST, SSL, DLE or target-neutral lowering state.
