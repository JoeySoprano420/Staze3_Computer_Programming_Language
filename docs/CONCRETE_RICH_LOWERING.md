# Compiler 0.5 — Concrete Rich Representation Lowering

## 1. Why this layer exists

Compiler 0.4 deliberately stopped rich values before native representation. That was necessary: `optional<T>`, `Many<T>`, datasets, choices, pools, borrows and typed faults are semantic concepts, not permission for the C++ bootstrap compiler to secretly choose `std::optional`, `std::vector`, `std::variant`, `shared_ptr`, or `malloc`.

Compiler 0.5 adds the missing target step while preserving that separation.

```text
SIR-S 2.x              SIR-C 3.x                    LLVM
semantic contract  →   concrete target contract  →  mechanical realization
```

No AST or SSL pointer exists in either detached SIR artifact.

## 2. Target contract

Current target profile:

```text
target triple      x86_64-pc-windows-msvc
pointer width      64
object format      COFF-x86-64
executable format  PE32+
```

SIR-C contains target-wide representation strings plus machine-readable layout and per-value records.

## 3. Concrete placement

| Semantic situation | 0.5 placement | Reason |
|---|---|---|
| exact-One scalar | register | no address identity required |
| fixed rich local | stack | bounded target-known storage |
| rich parameter | caller | caller already owns/materializes bytes |
| rich function result | caller result storage | avoids hidden return-object ABI |
| `@arena` pool allocation | pool-arena | honors explicit pool placement |
| `@heap` pool allocation | heap | explicit native heap request |
| borrow/move/revision view | alias | preserves underlying concrete storage |

Placement does not change ownership/provenance meaning. Those remain semantic SIR facts.

## 4. Concrete layout algorithm

Every layout records at least:

```text
kind
size
alignment
payload_offset
element_size
element_alignment
capacity
field_offsets[]
```

`align_up(offset, alignment)` is used before every field/payload with stricter alignment.

### Scalars

```text
unit/bool  size 1 align 1
i32/u32    size 4 align 4
i64/u64    size 8 align 8
text view  size 16 align 8
```

### ZeroOrOne<T>

Logical layout:

```text
byte 0      present tag
padding     to alignof(T)
payload     T
```

Concrete size is rounded up to the maximum alignment.

### Many<T>

The current implementation uses a bounded inline representation when the compiler can determine a finite constructor/phi capacity:

```text
u64 length
padding if required
T elements[capacity]
```

Phi-connected Many values are capacity-promoted to the maximum required capacity so every incoming edge agrees on one concrete layout.

This is intentionally not yet a dynamically growing vector contract.

### Dataset / pool record

Fields stay in declared order. For each field:

```text
offset = align_up(offset, field.align)
field_offsets.push(offset)
offset += field.size
```

Final size is rounded to record alignment.

### Choice

```text
i32 tag
padding to max payload alignment
payload bytes sized for largest case
```

The semantic choice case/tag mapping remains in SIR-S/SIR-C schema records; the layout only states physical storage.

## 5. Caller storage ABI

A rich result is not hidden in the scalar result tuple.

```text
scalar result:
    {status:i32, payload:u64} fn(fault_out:ptr, args...)

rich result:
    status:i32 fn(result_out:ptr, fault_out:ptr, args...)
```

Rich arguments are passed as `ptr`. Scalar arguments remain scalar boxed values in the bootstrap native convention.

`examples/rich_caller_storage.stz3` proves this by returning a dataset aggregate from one Staze instruction and passing it into another.

## 6. Fault payload ABI

SIR-C computes the maximum typed payload size/alignment required by the program's declared fault schemas.

The caller owns a fault buffer for the active call chain. A direct fault:

1. writes each typed payload field into the buffer using its concrete field layout;
2. returns the nonzero fault status;
3. leaves the payload bytes untouched during propagation.

This gives native payload transport without C++ exceptions or cross-language unwinding.

## 7. Escape analysis

The planner constructs a direct-use map for every SSA value.

A rich value is initially escaping when it:

- is returned from the function;
- is transported as a fault payload;
- crosses a rich call boundary.

Escape status then propagates backward through storage-preserving aliases:

- borrow;
- move;
- revision markers;
- phi joins.

This is deliberately conservative.

## 8. Scalar replacement

0.5 implements a narrow proof:

A dataset/choice construction can be marked `scalar_replaced` only when:

- it does not escape; and
- every direct use is one of the current semantic-only borrow/move/revision/relation operations.

Pool allocations are never scalar-replaced.

The backend reads the serialized flag. It does not repeat escape analysis.

## 9. Stack allocation discipline

Fixed stack/pool-arena storage is hoisted to the LLVM function entry block. This avoids accidental dynamic stack probes caused by materializing fixed-size rich values in arbitrary CFG blocks.

Each alloca uses the exact SIR-C size/alignment.

## 10. Explicit heap allocation

For `@heap` pools, LLVM lowering emits:

```text
heap = GetProcessHeap()
ptr  = HeapAlloc(heap, 0, concrete_size)
null = ptr == null
branch null -> AllocationFailure handler
       else -> normal continuation
```

This makes allocation failure part of the Staze fault graph.

A program cannot use an `@heap` pool without declaring the `AllocationFailure` fault identity needed by this milestone.

### Reclamation boundary

0.5 accepts heap pools only with `@reclaim(manual)`. It does not pretend that scope/lifetime reclamation is implemented merely because `HeapFree` exists. Automatic cleanup requires path-sensitive ownership/lifetime cleanup insertion and is a later milestone.

## 11. Native lowerings now admitted

- optional.some / optional.none;
- Many constructors and delete-to-empty recovery;
- dataset aggregates;
- choice/tagged union constructors;
- arena-pool allocations;
- heap-pool allocations;
- borrow/move aliases;
- rich phi values;
- rich caller results and rich parameters;
- typed fault payload writes;
- existing scalar/control/fault/effect operations.

## 12. What remains semantic-only or partial

Relation link/unlink, transaction and parallel operations are represented and effect-ordered but still have no full storage/rollback/scheduler runtime. Aggregate field projection and general address-sensitive borrow operations also remain later work.

This is deliberate: a native representation layer should implement only behavior whose failure, lifetime, authority and ownership consequences are already specified.
