# SIR-C 4.x — Resource Realization Artifact

SIR-C 4.x extends concrete representation lowering with independently verifiable resource realization.

Important records include:

```text
concretefn <name> <storage> <kind> <result-transfer> ...
resource <id> <value> <provenance> <lifetime-region> <cleanup-kind> <size> <align> <automatic> <dynamic-many>
cleanup <resource-id> <from-block> <to-block> <source-op> <fault-exit> <transfer> <reason>
```

`cleanup.transfer=0` means the resource is destroyed/reclaimed at the edge. `cleanup.transfer=1` means responsibility crosses the function return boundary and the callee must not destroy the resource.

The artifact remains canonical plaintext in this bootstrap implementation, is serialized/deserialized before LLVM, and is independently verified.
