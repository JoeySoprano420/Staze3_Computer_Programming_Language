# Compiler 0.4 — Rich Semantic SSA Architecture

## 1. Purpose

Compiler 0.3 answered:

> Can SIR exist without the AST?

Compiler 0.4 answers the harder question:

> Can the detached graph carry enough Staze meaning that resource, fault, cardinality, authority and effect legality can be verified after the frontend is gone?

For the implemented slice, yes.

SIR-S 2.0 is now a canonical owned semantic graph. SIR-C 2.0 is derived only from that graph and adds target contracts without reaching backward into source structures.

## 2. Value identity

Every runtime semantic value has two related records.

The SSA operation defines the computation:

```text
%result = opcode operands...
```

The value-semantics record defines the non-incidental Staze properties:

```text
value %result
  type        = i32 | bool | unit | text | nominal(Name)
  cardinality = One | ZeroOrOne | Many
  region      = lifetime-region ID
  provenance  = provenance ID or 0
  ownership   = value | parameter | owned | borrow | moved ...
  authorities = { read, move, revise, ... }
```

The verifier requires every SSA result to have exactly one matching semantic record.

## 3. Cardinality

Cardinality is carried independently of base type:

```text
SirTypeDesc {
    SirType type;
    SirCardinality cardinality;
    nominal_type;
}
```

Therefore these are different descriptors:

```text
i32 / One
i32 / ZeroOrOne
i32 / Many
```

without requiring SIR-S to say whether `ZeroOrOne` is later represented by a tag byte, sentinel, pair, register convention or optimized-away proof.

### Current constructors

```text
some(x)  -> optional.some x -> T / ZeroOrOne
none()   -> optional.none   -> T / ZeroOrOne
many(...) -> many.make      -> T / Many
```

## 4. Delete algebra

Deletion is resolved before target representation.

Given a fault-producing expression `E`:

```text
E : Unit / One
  delete F
  => recovery value = Unit / One

E : T / One, T != Unit
  delete F
  => compile-time error

E : T / ZeroOrOne
  delete F
  => optional.none<T>

E : T / Many
  delete F
  => many.make<T>()
```

The success and recovery paths join through normal SSA phi nodes.

This preserves the STZ distinction between **removing a failed contribution** and inventing an arbitrary replacement value.

## 5. Fault identities and payloads

A serialized fault identity contains:

```text
code
name
payload field count
payload field names + full descriptors
```

An explicit fault terminator contains either:

```text
concrete payload SSA values
```

or a marked pass-through payload when forwarding an already-originated fault whose payload has not been destructured in this milestone.

The verifier checks:

- known fault identity;
- fault belongs to the instruction's closed outward fault set;
- concrete payload arity;
- exact payload descriptor match;
- pass-through faults do not simultaneously invent concrete payload values.

## 6. Aggregate SSA

Datasets now become serialized schemas.

For:

```stz3
dataset Person
{
    key id: i32
    age: i32
}
```

SIR records:

```text
dataset Person
  field id  : i32 / One / key
  field age : i32 / One
```

Construction:

```stz3
p := Person(1, 30)
```

becomes:

```text
%1 = const.int 1
%2 = const.int 30
%3 = aggregate.make Person %1 %2
```

`%3` is nominal `Person / One` and receives a provenance identity.

## 7. Choice SSA

A `choice` is a tagged semantic union schema.

```stz3
choice WorkState
{
    Idle
    Busy(job: i32)
}
```

is serialized with stable case order/tags in this milestone:

```text
0 Idle
1 Busy(job: i32 / One)
```

Construction:

```text
%state = choice.make WorkState tag=1 %job
```

The verifier checks that the tag exists and the payload exactly matches that case's schema.

## 8. Lifetime regions

A function owns a root region. Rich constructs create nested regions where the semantic model requires them.

Current kinds:

```text
function
pool
transaction
parallel
```

A region records:

```text
ID
parent ID
name
kind
lifetime
allocation policy
reclaim policy
```

Values reference regions by ID. Provenance records also reference regions. The verifier rejects unknown parents, out-of-order parent construction and references to nonexistent regions.

## 9. Provenance

Provenance identifies the origin of resource-bearing nominal values independently from their SSA version.

Current provenance origins include:

```text
aggregate.<Dataset>
choice.<Choice>.<Case>
pool.alloc.<Pool>
```

Borrow and move operations preserve provenance rather than creating a fictitious new underlying resource identity.

## 10. Authority capabilities

Value authority is explicit serialized state.

Current exercised capabilities:

```text
read
move
revise
```

The verifier enforces:

```text
borrow(source)       requires source.read
move(source)         requires source.move
revise.relative      result carries revise
revise.replace       result carries revise
```

The frontend additionally tracks moved bindings and rejects subsequent reads.

This is intentionally only the beginning of the complete STZ-3 authority algebra; field/path authority, delegation, relation authority and cross-instruction authority transfer remain later work.

## 11. Effect-token SSA

Ordinary SSA answers:

> Which value does this computation consume?

Effect-token SSA answers:

> Which semantic effects are known to have happened before this operation?

A pure operation has no effect token transition.

An effectful operation has:

```text
effect_in  = !n
effect_out = !n+1
effects    = semantic effect names
```

Examples:

```text
ownership.move
memory.allocate
relation.link
transaction.begin
transaction.commit
parallel.begin
parallel.end
revise.relative
revise.replace
io.write
```

### Block entry tokens

Each basic block defines one unique effect-entry token.

A non-entry block stores an effect-phi input for **each exact incoming CFG edge**.

The edge identity is:

```text
(predecessor block, source operation)
```

where source operation `0` means the predecessor terminator and a nonzero source operation identifies a fault edge leaving that operation.

This prevents a fault path from inheriting effects that occur *after* the faulting operation in the same block.

The verifier recomputes the expected token at every edge and requires an exact match.

## 12. Ownership operations

### Borrow

```stz3
observer := borrow(alice)
```

SIR:

```text
%observer = borrow %alice
```

Properties:

```text
same type/cardinality
same provenance
ownership = borrow
read capability required and retained
```

### Move

```stz3
transferred := move(bob)
```

SIR:

```text
%transferred = move %bob
  effect ownership.move
```

Properties:

```text
same type/cardinality
same provenance
source requires move capability
result ownership = moved
semantic binding bob becomes unavailable for further reads
```

## 13. Pools

Pool declarations now survive as schemas with:

```text
name
lifetime
allocation mode
reclaim mode
field descriptors
field revision capability
```

For:

```stz3
pool Frame @lifetime(call) @arena @reclaim(scope)
{
    tick: revise i32
}
```

and:

```stz3
frame := Frame.alloc(0)
```

SIR includes:

```text
pool region Frame
provenance pool.alloc.Frame
%frame = pool.alloc Frame %initial_tick
  effect memory.allocate
```

The current LLVM backend intentionally does not invent physical arena implementation or allocation-failure behavior yet.

## 14. Relations

Relation schemas now include endpoints, cardinalities, ownership policy, unique-pair and reverse-index facts.

Operations such as:

```stz3
perform knows.link(alice, transferred)
```

become:

```text
relation.link knows %alice %transferred
  effect relation.link
```

The verifier checks relation identity and endpoint nominal types independently of source syntax.

A concrete relation-state provider, cardinality storage enforcement and transactional rollback are next-stage runtime/backend work.

## 15. Transactions

```stz3
transaction
{
    ...
}
```

creates a nested lifetime/effect region and explicit operations:

```text
transaction.begin
...
transaction.commit
```

They participate in effect-token order.

Compiler 0.4 does **not** yet claim durable/rollback transaction machinery. The graph now provides the place where such proof and implementation must attach instead of allowing backend invention.

## 16. Parallel regions

```stz3
parallel
{
    ...
}
```

creates:

```text
parallel.begin
...
parallel.end
```

inside an explicit parallel semantic region and effect-token chain.

Compiler 0.4 does not yet lower this into OS threads or a task runtime. Conflict analysis, deterministic joins and scheduling belong to the next concurrency milestone.

## 17. SIR-S → SIR-C

Target concretization copies only verified SIR-S meaning and adds target facts, currently including:

```text
target profile  = windows-x86_64-bootstrap
target triple   = x86_64-pc-windows-msvc
pointer bits    = 64
object format   = COFF-x86-64
executable      = PE32+
aggregate rep   = rich-indirect-v1
cardinality rep = One=scalar; ZeroOrOne/Many=rich-indirect-v1
```

No AST/SSL lookup is permitted.

Both SIR-S and SIR-C use canonical line-oriented format version **2.0** in this milestone.

## 18. Verification boundary

The independent verifier now checks all prior SSA/CFG rules plus:

- fault payload schemas and concrete payload values;
- One/ZeroOrOne/Many descriptor consistency;
- aggregate schema/arity/field descriptors;
- choice tag and payload descriptors;
- region identity/parenting;
- provenance identity and region membership;
- exactly one semantic record per SSA value;
- nonempty and nonduplicated capability sets;
- borrow/move provenance and capability laws;
- revision capability laws;
- pool identity and allocation provenance;
- relation endpoint descriptors;
- transaction/parallel effect markers;
- effect-entry token uniqueness;
- exact effect-token chains;
- exact edge-level effect-phi coverage;
- target representation contracts in SIR-C.

## 19. Native lowering boundary

The phrase **executable semantics** in 0.4 means the graph states the operation, inputs, outputs, lifetime/provenance/capability state, control/fault edges and effect order precisely enough for independent validation and later lowering.

It does not mean every rich operation has already been assigned a final Windows heap/stack/arena byte layout.

That remaining job is deliberately separated into Compiler 0.5 so DLE/SIR-C can choose representations without changing SIR-S meaning.

## 20. Architectural result

The pipeline is now capable of saying:

```text
SOURCE says:
    delete this optional fault
    borrow this resource
    move that one
    allocate from this pool
    link these entities
    enter this transaction

SIR-S records:
    exact value/cardinality/provenance/authority/effect/fault graph

VERIFIER proves:
    graph is internally legal without reading source

SIR-C states:
    target representation contract

BACKEND may only:
    realize that verified contract
```

That is the intended direction of STZ-3: rich meaning survives until it has been proven safe to erase.
