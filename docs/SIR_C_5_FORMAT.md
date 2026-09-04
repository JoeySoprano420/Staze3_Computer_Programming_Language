# SIR-C 5.0 — Deep Ownership Concrete Artifact

SIR-C 5.0 extends SIR-C 4.x resource realization without changing SIR-S semantic meaning.

## New concrete records

### `concretefn`
Carries the concrete result ABI plus counts for values, resources, resource components, cleanup actions, and rollback actions.

### `resource`
Adds:
- shared ownership flag;
- `absorbed_by` resource-component identity;
- serialized resource-family identity.

### `component`
Represents a destroyable child resource embedded in a rich owner:
- owner SSA value and provenance;
- cleanup kind;
- guard (`always`, `optional-present`, `choice-tag`, `many-elements`);
- concrete byte offset;
- choice tag or Many stride when relevant;
- type/path identity.

A component is valid only when a concrete child resource points back through `absorbed_by`, and a matching SIR-S ownership edge connects the owner provenance to the child provenance.

### `rollback`
Carries the concrete realization of a semantic transaction rollback obligation:
- rollback identity;
- transaction region;
- provenance;
- source operation;
- action.

The SIR-C verifier requires a one-to-one match with SIR-S rollback obligations.

## Result transfer ABI

SIR-C 5.0 recognizes:
- no transfer;
- owned heap-object transfer;
- dynamic-Many transfer;
- borrowed-reference transfer;
- shared-reference transfer.

Borrowed and shared references are passed as pointer-sized caller-storage results rather than copying aggregate bytes.

## Backend rule

LLVM is a consumer of SIR-C 5.0. It is not permitted to infer ownership, lifetime, refcount, component, or rollback meaning from AST/SSL state.
