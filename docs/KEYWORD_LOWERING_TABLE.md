# Staze v3 keyword/construct → compiler responsibility → machine consequence

> **Compiler 0.4 update:** Several resource/cardinality/transaction constructs now have executable SIR operations. Consult `IMPLEMENTATION_STATUS.md` for the exact supported subset.


> **Compiler 0.3 status note:** The table describes the full intended STZ-3 responsibilities. Only the subset listed in `IMPLEMENTATION_STATUS.md` is executable in the current compiler.


This is the rule that keeps the compiler intellectually clean:

> **A Staze word defines semantic responsibility. It does not automatically define an x86 opcode.**

The x86-64 CPU knows instructions such as `mov`, `add`, `cmp`, conditional branches, loads,
stores, SIMD operations, and `call`. Staze's vocabulary is higher-level. The compiler resolves
that vocabulary into a verified computation, and *then* a backend chooses legal machine
instructions.

The table below is therefore intentionally three-layered.

| Staze construct | What it means before code generation | Typical eventual machine consequence |
|---|---|---|
| `staze 3` | Select STZ-3 semantics | No runtime code |
| `module` | Module/namespace identity | Usually metadata/symbol naming only |
| `use` | Compile-time semantic access to exact names | Usually no runtime code |
| `public/private/package` | Visibility | Symbol/export metadata where required |
| `alias` | Same semantic type under another spelling | Usually no runtime code |
| `type ... distinct` | New nominal identity | Often no extra runtime storage |
| `choice` | Closed tagged-sum semantics | Tag + payload only if value survives lowering |
| `fault` / `faults` | Closed typed failure set | Status/tag/control-flow representation selected later |
| `signature` | First-class callable contract | Function pointer/context/table only when runtime value survives |
| `category` / `implements` | Compile-time capability/coherence facts | Often zero runtime code after specialization |
| `dataset` | Semantic record/entity schema | Layout selected only when representation becomes necessary/observable |
| `relation` | Typed edge semantics + cardinality/mutability/ownership rules | Could become pointers, indices, tables, DB queries, or disappear |
| `ruleset` / `rule` | Invariants/constraints | Proven rules disappear; unresolved rules become guards |
| `dictionary` | Deterministic mapping semantics | Hash/search/perfect-hash/table chosen by DLE |
| `pool` | Lifetime/responsibility region | Arena/stack/heap/static/custom realization if required |
| `label` | Semantic identity/directive target | Metadata/address/branch target depending context |
| `context` | Scoped semantic defaults/facts | Usually compile-time; runtime state only if observable |
| `unit` | Unit/dimensional identity or unit control type by context | Often zero-cost type information |
| `derivative` | Derived/recomputed semantic value | Inline expression, cache, incremental update, etc. if proven equivalent |
| `group/family/chain/web/nest` | Relational/constrained views | Tables/indices/pointers/loops—or no runtime object |
| `constant` | Compile-time constant declaration | Usually immediate bytes or no runtime storage |
| `instruction` | Primary executable semantic abstraction | Native function/body after verification |
| `task` | Structured async executable abstraction | State machine/task runtime only as required by selected profile |
| `cascade` | Ordered semantic composition | Calls may fuse/inline/disappear if legal |
| `foreign` | Explicit foreign boundary | Exact ABI call/representation bridge |
| `generate` / `compile` | Bounded compile-time generation/evaluation | No runtime code if fully evaluated |
| `test/property` | Verification/development declarations | Usually absent from release executable |
| `:=` | Create a new binding | Store/register/immediate—or zero instructions if folded |
| `=` | Equality, never mutation | Compare/test instructions or compile-time proof |
| `set` | Atomic semantic replacement at target invariant boundary | Compute + guard + store/commit |
| `revise ... by` | Relative change through unique revision semantics | Arithmetic + checked fault path + store, if runtime |
| `insert/remove` | Membership mutation | Container/index operations chosen by representation |
| `move` | Ownership transfer | Frequently zero instructions; invalidates old semantic owner |
| `link/unlink` | Relation-edge mutation | Edge/index/table mutation chosen by relation realization |
| `delegate` | Narrow authority transfer | Often zero runtime code unless capability is represented dynamically |
| `if/else` | Conditional control | Branch, select/predication, or compile-time elimination |
| `branch/when/otherwise` | Exhaustive pattern classification | Jump table/comparisons/tag tests/etc. |
| `for/while/do/loop` | Repetition | Branches, induction variables, vector loops, or unrolled/eliminated code |
| `break/continue/return` | Structured control transfer | Jump/epilog/return protocol |
| `perform` | Explicitly discard a `unit` result while performing effects | Whatever the invoked operation requires; `perform` itself has no opcode |
| `transaction` | Explicit commit/abort semantic region | Log/copy/lock/DB transaction/no extra machinery depending proven realization |
| `parallel` | Semantically permitted parallel execution | Threads/tasks/SIMD/GPU/sequential fallback only if contract allows equivalent fallback |
| `unsafe` | Admit explicitly sourced assumptions outside safe proof | No mandatory opcode; changes proof obligations, never permission to reinterpret semantics |
| `bypass F using X` | Substitute a valid continuation for fault `F` | Branch/select/error-status path; may inline away |
| `delete F` | Eliminate failed contribution according to cardinality | Skip/none/empty contribution/no-op; illegal under exact `One` unless repaired |
| `await` | Await task completion under task semantics | State machine/wait primitive/inlined completion path |
| `atomic` | Atomic access semantics | LOCKed x86 ops/fences or equivalent backend sequence |
| `@directive` | Versioned semantic/representation/tool directive | Depends entirely on directive; may be compile-time only |

## Why some powerful features generate *nothing*

Suppose SSL proves that a rule is always true. The correct machine implementation of that rule
check is **zero instructions**.

Suppose `:=` creates a constant used once. The compiler can substitute its value. Again: **zero
instructions for binding creation**.

Suppose `move` transfers responsibility for a value already sitting in the right register/storage.
Ownership changes semantically while the bits stay where they are. Again: possibly **zero
instructions**.

This is exactly why Staze must not define language features as one-to-one opcode aliases. Its
semantics stay rich long enough for DLE to prove which runtime machinery is unnecessary.
