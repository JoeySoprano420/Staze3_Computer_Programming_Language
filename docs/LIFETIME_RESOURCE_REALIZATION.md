# Compiler 0.6 — Lifetime & Resource Realization

## 1. Resource obligations

A concrete automatic resource is represented in SIR-C with an identity, semantic SSA value, provenance identity, lifetime region, cleanup kind, size/alignment, and automatic/manual policy. Cleanup is therefore a verified graph obligation rather than an emitter heuristic.

## 2. Path-sensitive cleanup

For every automatic resource, target concretization examines CFG and fault edges that leave the resource lifetime. It emits a `cleanup` action for each applicable edge. The independent SIR-C verifier reconstructs the required exits and rejects missing discharge actions.

Normal returns and fault returns are checked separately. Loop backedges, `continue`, and `break` matter when they leave an iteration/scope region. A wider-lifetime automatic allocation repeatedly created in a loop is rejected if it cannot be represented by one safe obligation.

## 3. Heap realization

Automatic heap resources use a pointer ownership slot initialized to null. Successful `HeapAlloc` installs the pointer. Cleanup loads the current pointer, calls guarded `HeapFree`, then clears the slot. Allocation failure is a normal Staze `AllocationFailure` fault edge.

The null initialization makes a cleanup obligation safe even on a path where allocation never completed.

## 4. Dynamic Many<T>

Dynamic `Many<T>` is concretized as a 24-byte descriptor on x86-64:

```text
0   data     : ptr
8   length   : u64
16  capacity : u64
```

Growth checks capacity arithmetic, allocates replacement storage, copies live elements, frees the old buffer, updates the descriptor, and replaces the pointer stored in the same resource obligation. Final cleanup therefore targets the newest live buffer.

## 5. Addressable fields and subobjects

Dataset/pool field offsets are established in SIR-C. `field.load`, `field.address`, and `field.store` use these offsets. A field-address borrow creates child provenance referencing the owner's provenance and a subobject path.

## 6. Borrow lifetime proof

For a borrow created in block region `L` from an owner in region `O`, the required borrow region is the shortest valid nested region:

```text
if L is inside O: borrow = L
if O is inside L: borrow = O
otherwise: invalid
```

The SIR-S verifier independently recomputes this rule for `borrow` and `field.address`. It checks the SSA value region, operation region, and child provenance region. A detached artifact that widens the borrow is rejected.

A borrowed value may not currently be returned from an instruction because no cross-boundary borrow-lifetime contract has yet been standardized.

## 7. Owned-resource transfer proof

A resource result must cross an instruction boundary through explicit `move(...)`. DLE classifies the transfer family (`heap-pool:<name>` or `dynamic-many`) and marks the function result as `owned-transfer`.

SIR-C then selects a concrete transfer mode:

- `heap-object`: caller result storage contains the ownership-bearing pointer;
- `dynamic-many`: caller result storage contains the 24-byte descriptor whose data pointer carries the buffer obligation.

The callee's success return receives a `transfer` discharge, not a cleanup discharge. Native lowering copies ownership-bearing state to caller storage and clears the callee ownership slot without freeing it. The caller loads that state and creates a new local resource obligation with fresh caller-local provenance.

Fault exits do not transfer and must still clean the callee-owned resource.

## 8. Independent proof boundary

The verifier rejects, among other cases:

- missing cleanup on an edge leaving an automatic resource lifetime;
- an unproven `HeapFree` resource;
- a success return of an owned resource without transfer discharge;
- a transfer discharge on a fault/ordinary branch edge;
- a resource return without `move` semantics;
- a call result missing caller-local ownership provenance;
- a field borrow whose lifetime was widened in serialized SIR;
- a borrowed value escaping through an instruction result.

## 9. Native realization

LLVM consumes SIR-C after serialization/deserialization and verification. It never receives AST or SSL state. For the two cross-boundary ownership fixtures, source-driven and detached-SIR-C paths generate byte-identical LLVM IR.
