# Expandability: how Staze grows without bloating the compiler core

This document explains the expansion model and why Staze can evolve indefinitely while keeping a tiny, stable compiler core.

1. Expandability through the Semantic Lattice (SSL)

Staze treats semantic facts as the source of truth, not syntax. That means you can add:

- New relationships
- New node/link/child/branch semantics
- New categories
- New rulesets
- New derivative types
- New cascades
- New pool policies

…without changing the compiler core. The compiler reasons about semantic facts; as long as a feature can be expressed as facts, the engine can reason about it. This is the same reason SQL, spreadsheets, documents, GPU kernels, and game entities can coexist inside Staze without separate mini‑languages.

2. Expandability through Definition‑Driven Language Construction

The compiler is not a giant monolith of handwritten feature logic. Instead, it loads definitions that describe:

- syntax forms
- semantic meaning
- constraints
- lowering rules
- target rules
- diagnostics
- relationships

Adding a new language feature becomes adding a new "module of meaning" rather than editing the C++ source. You can define:

- a new range type
- a new derivative policy
- a new cascade form
- a new dataset rule
- a new pool directive
- a new lowering rule
- a new semantic relationship

The core stays small and stable.

3. Expandability through the Two‑Generation Rule

If the compiler doesn't yet understand a feature: generation G adds support for it; G+1 can use it internally; G+2 can rewrite the compiler using it. This prevents bootstrap deadlocks and allows safe evolution.

4. Expandability through Deductive Lowering

Lowering is driven by proven facts, not syntax. You define what a feature means, what obligations it creates, and what rules can satisfy those obligations. The compiler proves a legal implementation and lowers to constructs LLVM already understands.

5. Expandability through SIR (Staze Instruction IR)

SIR is a semantic buffer zone. New SIR instructions or behaviors can be added through definitions and verified before lowering. This enables new semantic operations, authority instructions, relationship operations, dataset ops, etc., safely at the IR level.

6. Expandability through Contexts

Contexts let you add entire domains without changing the language core. Examples: GPU, physics, database, cryptography, document, spreadsheet, simulation contexts. Each context can define default types, units, pool policies, execution policies, and domain‑specific rules.

7. Expandability through Pools & Authority Models

Pools are semantic authority boundaries. You can define new concurrency models, authority types, access policies, lifetime rules, and storage directives. This opens Staze to distributed systems, GPU memory, secure enclaves, real‑time systems, simulation engines, and multi‑tenant servers without changing the core.

8. Expandability through Cross‑References

Cross‑references unify databases, spreadsheets, documents, game entities, network packets, schemas, and assets. New reference types and lookup semantics can be added through definitions.

9. Expandability through the Seed Engine

The seed engine is intentionally tiny: it understands values, types, names, references, rules, patterns and basic instructions. Everything else is built on top of it, so the seed rarely needs to grow.

10. Expandability through Self‑Hosting

When Staze is self‑hosted it can rewrite, extend, optimize and redefine itself. Combined with the two‑generation rule and definition‑driven construction, this is the ultimate path to safe, perpetual evolution.

## The core answer

Staze is expandable because its semantics are defined rather than hard‑coded. The compiler is a reasoning engine, lowering is proof‑driven, features are definitions, and bootstrap architecture avoids deadlocks. That is why Staze can grow into many domains without fragmenting.

-- end --




## *** ##



# Definition module: new_range
# Template for adding a "range" feature via definitions.
# Sections:
#  - metadata: name, version, dependencies
#  - syntax: grammar snippets or token forms
#  - semantics: facts introduced into the SSL
#  - constraints: verifier-level rules
#  - lowering: deductive lowering patterns to SIR
#  - runtime: optional runtime shims / required helpers
#  - diagnostics: error messages and explanations

[metadata]
name = "example.new_range"
version = "0.1"
description = "Adds a lightweight 'range' value and literal form that lowers to an iterator pair"
requires = []

[syntax]
# This is illustrative pseudo-grammar. The compiler's definition loader reads
# a project-specific dialect for grammar fragments. Provide token forms and
# examples for maintainers.
# Grammar:
#   range_literal := '[' expression '..' expression ']'
#   range_expr    := identifier ':' range_literal   // variable binding with type

examples = ["let r : [0..10]", "for x in [0..n] { ... }"]

[semantics]
# Semantic facts introduced into the SSL. These are high-level facts the
# semantic engine will consume.
# - type 'Range<T>' as a builtin composite: (start:T,end:T,step:int)
# - 'range_literal' produces a value of Range<T>

facts = [
  "type Range<T> = { start: T, end: T, step: Int }",
  "value_construction(range_literal) -> produces Range<T>"
]

[constraints]
# Verifier constraints (expressed in the project's rule language) — examples:
# - start and end must be integer-compatible when used in integer contexts
# - step != 0 if specified

rules = [
  "if expression in range_literal has type U and U is numeric -> range_literal has type Range<U>",
  "if step is provided -> assert step != 0 else default step = 1"
]

[lowering]
# Deductive lowering patterns. These are rules that prove how high-level facts
# can be implemented in terms of existing primitives. They are written in the
# lowering DSL; the compiler will use proof search to apply them.
# Example lowering: iterate over Range<T> -> synthesize a basic loop using
# existing 'for' lowering primitives or explicit CFG/phi tokens in SIR.

patterns = [
  "LowerRangeLiteral: range_literal(start,end) -> allocate Range{start,end,step}\n",
  "LowerForOverRange: for x in range_expr(body) -> \n  create loop with phi x = start; compare x < end; body; x = x + step;" 
]

[target]
# Target rules to guide emission: how Range maps to runtime helpers or inline code.
# For targets that lack special support, lower to scalar arithmetic and branch
# operations; otherwise call runtime 'range_iter' helper.

rules = [
  "If target supports inline loop emission -> emit loop using scalar ops",
  "Else emit calls to runtime.range_iter(range, &next, &has_next)"
]

[runtime]
# If your feature requires runtime helpers, list them here and provide C or
# Staze implementations in the runtime/ or libs/ directories. These will be
# linked into the final binary via the normal build pipeline.

helpers = [
  {name = "runtime.range_iter", lang = "c", path = "runtime/range_iter.c", desc = "optional C helper to iterate a Range"}
]

[diagnostics]
# Error templates referenced by verifier rules.
messages = {
  "ERR_RANGE_STEP_ZERO" = "range step cannot be zero",
  "ERR_RANGE_NON_NUMERIC" = "range bounds must be numeric types"
}

# End of definition template




## *** ##



# Definition module: gpu_context
# Adds a GPU execution context with default types and pool policy for device memory

[metadata]
name = "context.gpu"
version = "0.1"
description = "Introduce a GPU context with device memory pools and execution policy"
requires = []

[syntax]
# Example usage:
# context gpu {
#   default_pool device_mem
#   default_execution 'gpu'
# }

examples = ["context gpu { default_pool device_mem; default_execution 'gpu' }"]

[semantics]
facts = [
  "context gpu { default_pool = device_mem; default_execution = 'gpu' }",
  "pool device_mem is Pool<device>"
]

[constraints]
rules = [
  "pool device_mem must be allocated via device allocator when used in gpu context",
  "functions in gpu context must be marked with 'gpu_kernel' attribute"
]

[lowering]
patterns = [
  "LowerGpuKernel: function marked gpu_kernel -> emit kernel entry/exit and special calling convention",
  "LowerDeviceAlloc: pool_alloc(device_mem, size) -> call runtime.device_alloc(size)"
]

[target]
rules = [
  "emit device memory loads/stores as normal loads/stores but mark with 'address_space=1' metadata for LLVM",
  "if target has NVPTX backend -> prefer kernel launch intrinsics"
]

[runtime]
helpers = [
  {name = "runtime.device_alloc", lang = "c", path = "runtime/device_alloc.c", desc = "allocates device memory on GPU"}
]

[diagnostics]
messages = {
  "ERR_GPU_KERNEL_MISSING_ATTR" = "GPU function must be annotated with 'gpu_kernel'"
}

# End




## *** ##



