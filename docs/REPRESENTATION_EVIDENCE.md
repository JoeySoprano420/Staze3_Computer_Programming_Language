# Compiler 0.5 — Representation Evidence Matrix

| Requested behavior | Fixture | Evidence |
|---|---|---|
| register placement | all scalar fixtures | exact-One scalar `cvalue ... register scalar` |
| stack rich placement | `rich_cardinality`, `rich_caller_storage` | rich temporary `cvalue ... stack ...` |
| caller storage | `rich_caller_storage` | rich parameter is `caller`; `MakePair` result storage is caller |
| pool/arena storage | `rich_resources` | `Frame` allocation is `pool-arena aggregate` |
| explicit heap storage | `heap_allocation` | `HeapCell` is `heap aggregate`; native `HeapAlloc` |
| optional lowering | `rich_cardinality` | `optional` concrete kind + native tag/payload storage |
| Many lowering | `rich_cardinality` | `many-inline` with length/capacity/element size |
| aggregate lowering | `rich_resources`, `rich_caller_storage` | serialized field offsets and native stores/memcpy |
| choice lowering | `rich_resources` | `choice` tag + payload layout |
| native fault payload | `fault_payload_native` | `store i32 42, ptr %fault_out` |
| escape analysis | `rich_caller_storage` | call-crossing aggregate marked escaping, not scalar-replaced |
| scalar replacement | `rich_resources` | three nonescaping dataset/choice values marked scalar-replaced |
| allocation failure | `heap_allocation` | null check branches to `AllocationFailure` |
| detached rich backend | `rich_cardinality_from_sirc.exe` | byte-identical to source-built PE |
| SIR-S unchanged | 0.4 vs 0.5 rich fixtures | byte-identical serialized SIR-S |
