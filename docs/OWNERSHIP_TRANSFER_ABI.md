# Compiler 0.6 — Owned Resource Transfer ABI

## Heap pool result

Semantic result contract:

```text
ownership = owned-transfer
resource  = heap-pool:<PoolName>
```

Concrete SIR-C result:

```text
transfer = heap-object
storage  = caller
size     = 8
align    = 8
```

The caller supplies a pointer-sized result slot. On successful return the callee stores the owned heap pointer into that slot and performs a transfer discharge that clears its local ownership slot without `HeapFree`. The caller loads the pointer and installs it into its own resource slot. On every callee fault exit, the callee retains responsibility and performs its own cleanup.

## Dynamic Many<T> result

Concrete transfer uses caller-owned 24-byte descriptor storage. The callee copies `{data,length,capacity}` into the caller result area, discharges its local obligation by transfer, and returns success. The caller loads the descriptor's data pointer into its new resource slot. Reallocation updates this same caller obligation.

## Why explicit move is mandatory

Copying an aggregate or descriptor is not itself proof that resource responsibility moved. `return move(resource)` is the semantic signal that permits the ownership contract. Returning the resource without `move` is rejected before target lowering.

## What is not supported yet

Borrowed-reference results require a separate lifetime ABI and are rejected. Shared ownership/refcount transfer is not part of Compiler 0.6.
