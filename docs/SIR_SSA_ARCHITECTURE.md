# SIR-S / SIR-C 0.3 — Detached SSA Architecture

> **0.4 supersession note:** Compiler 0.4 retains the detached SSA foundation described here and extends it with cardinality, fault payloads, aggregates/choices, lifetime regions, provenance, capabilities, ownership operations, and explicit effect-token SSA. See `RICH_SEMANTIC_SSA.md`.


## 1. Non-negotiable boundary

The backend must not know how Staze source was spelled.

The frontend is allowed to know source syntax, token positions, AST structure and SSL expression IDs. SIR is not.

The only source-derived material retained by SIR is copied data that remains meaningful after the frontend is destroyed: module identity, source locations, semantic fact strings, fault identities, function signatures, SSA values, control-flow blocks, effects and fault edges.

No SIR structure contains a pointer or reference to an AST or SSL structure.

## 2. SIR-S

SIR-S is target-neutral executable semantics.

A function contains:

```text
identity
public/private flag
result type
parameters
closed outward fault set
effect summary
entry block
basic blocks
```

A basic block contains:

```text
zero or more phi nodes
zero or more operations
exactly one terminator
```

## 3. SSA values

`SirValueId` zero is reserved. Every parameter and operation result receives one unique nonzero value ID.

An ordinary operation may use a value only if its definition dominates the use. A phi input is interpreted on a predecessor edge and is checked against that predecessor.

## 4. Revision without mutable backend slots

Staze's semantic distinction remains visible:

```text
:=      establishes a binding
set     replaces under revise authority
revise  performs relative revision under revise authority
```

The SSA graph does not need a C-style stack slot merely because Staze source used `set`.

`set` produces a new SSA version through `revision.marker`; `revise by` produces a checked arithmetic value with `revise.relative` effect annotation.

This lets later DLE/optimization eliminate local revision machinery while preserving its semantic identity in SIR.

## 5. If joins

For a binding that differs between incoming paths:

```text
block 1:
    %5 = ...
    br 3

block 2:
    %8 = ...
    br 3

block 3:
    %9 = phi [1,%5] [2,%8]
```

The verifier requires the phi predecessor set to exactly equal the CFG predecessor set.

## 6. Loops

Loop-carried bindings are represented by header phis:

```text
preheader:
    %initial = ...
    br header

header:
    %x = phi [preheader,%initial] [backedge,%x.next]
    ... condition ...
    condbr body, exit

body:
    %x.next = ...
    br header
```

`continue` contributes another header incoming edge. `break` contributes an exit-edge environment; multiple exit versions are joined with exit phis.

## 7. Short-circuit boolean execution

`and` and `or` are not represented as eager binary arithmetic operations. DLE constructs CFG so the right-hand expression executes only when required, and a phi combines the shortcut value with the right-hand result.

## 8. Fault graph

A checked operation has explicit named edges, for example:

```text
%12 = div.checked %a %b
      fault DivideByZero      -> block 7
      fault ArithmeticOverflow -> block 8
```

A call's fault-edge set must exactly equal the callee's declared closed fault set.

An expression handler rewrites the route for the named fault before lowering the protected expression. Thus:

```stz3
Divide(a,b) bypass DivideByZero using fallback
```

becomes a call whose `DivideByZero` edge targets a handler block. The handler computes `fallback`; the normal and recovery values meet at a phi.

Uncaught declared faults terminate at `fault.return` blocks.

## 9. Effect graph

The current milestone stores effect annotations on operations. Ordering inside a basic block and CFG edges define execution order.

This is sufficient to make effects independent from AST syntax, but the later full SIR should introduce explicit effect tokens / regions where necessary for advanced reordering proofs, concurrency and transactions.

## 10. SIR-C

SIR-C is created only after SIR-S verification. It deep-copies the graph and adds target facts:

```text
target profile       windows-x86_64-bootstrap
target triple        x86_64-pc-windows-msvc
pointer width        64
object format        COFF-x86-64
executable format    PE32+
calling convention   current status-i32/payload-u64 internal contract
```

No source object is reattached during concretization.

## 11. Serialization

The canonical text format is line-oriented. User/source strings are hex-encoded. This avoids ambiguity from spaces, quotes, Unicode bytes and embedded line breaks.

Serialization is checked for byte stability after a deserialize/serialize round trip.

## 12. Backend isolation

`LlvmWindowsX64Emitter` includes `sir.hpp`, not `ast.hpp` or `semantic.hpp`.

It reconstructs all needed information from SIR-C:

- function signatures;
- value types;
- constants;
- phi nodes;
- control-flow edges;
- fault routing;
- effect-neutral execution operations;
- target identity;
- fault codes.

## 13. Separate-process proof

The command:

```text
stazec --from-sir-c program.sirc -o program.exe
```

starts at SIR-C. This path is used in the test suite and proves that PE generation does not require Staze source or frontend state.

## 14. What comes next

The next SIR expansion should focus on semantics that cannot be represented merely as scalar SSA:

1. aggregate and choice values;
2. cardinality values (`ZeroOrOne`, `Many`);
3. fault payload SSA values;
4. memory objects, provenance, borrows and lifetime regions;
5. authority/capability values;
6. pool allocation and deterministic cleanup operations;
7. dataset/relation operations;
8. transaction regions;
9. concurrency/parallel effect regions;
10. DLE proof references attached to representation-changing SIR-C operations.
