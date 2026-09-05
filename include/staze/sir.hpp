#pragma once
#include <cstdint>
#include <optional>
#include <string>
#include <utility>
#include <vector>

namespace staze {

struct Program;
struct SSLProgram;

using SirValueId = std::uint32_t;
using SirBlockId = std::uint32_t;
using SirOpId = std::uint32_t;
using SirEffectTokenId = std::uint32_t;
using SirRegionId = std::uint32_t;
using SirProvenanceId = std::uint32_t;

struct SirLocation {
    std::uint32_t line{0};
    std::uint32_t column{0};
    bool operator==(const SirLocation&) const = default;
};

enum class SirType { Unit, Bool, I32, I64, U32, U64, Text, Nominal };
enum class SirCardinality { One, ZeroOrOne, Many };

std::string sir_type_name(SirType type);
SirType sir_type_from_name(const std::string& name);
std::string sir_cardinality_name(SirCardinality cardinality);
SirCardinality sir_cardinality_from_name(const std::string& name);
bool sir_is_integer(SirType type);
bool sir_is_signed_integer(SirType type);
unsigned sir_integer_bits(SirType type);

struct SirTypeDesc {
    SirType type{SirType::Unit};
    SirCardinality cardinality{SirCardinality::One};
    std::string nominal;
    bool operator==(const SirTypeDesc&) const = default;
};

enum class SirOpcode {
    Phi,
    ConstUnit,
    ConstInt,
    ConstBool,
    ConstText,
    OptionalSome,
    OptionalNone,
    ManyMake,
    AggregateMake,
    ChoiceMake,
    PoolAlloc,
    FieldLoad,
    FieldAddress,
    FieldStore,
    FieldMove,
    ManyDynamic,
    ManyPush,
    ManyLength,
    Borrow,
    BorrowMut,
    MoveValue,
    ResourceRelease,
    SharedRetain,
    SendValue,
    Not,
    CheckedNeg,
    CheckedAdd,
    CheckedSub,
    CheckedMul,
    CheckedDiv,
    CheckedMod,
    CompareEq,
    CompareNe,
    CompareLt,
    CompareLe,
    CompareGt,
    CompareGe,
    Call,
    WriteText,
    RelationLink,
    RelationUnlink,
    TransactionBegin,
    TransactionCommit,
    TransactionAbort,
    ParallelBegin,
    ParallelEnd,
    TaskBegin,
    TaskEnd,
    RevisionMarker
};

std::string sir_opcode_name(SirOpcode opcode);
SirOpcode sir_opcode_from_name(const std::string& name);

struct SirPhiIncoming {
    SirBlockId predecessor{0};
    SirValueId value{0};
    bool operator==(const SirPhiIncoming&) const = default;
};
struct SirEffectPhiIncoming {
    SirBlockId predecessor{0};
    SirOpId source_op{0}; // 0 = block terminator edge; nonzero = fault edge emitted by this op
    SirEffectTokenId token{0};
    bool operator==(const SirEffectPhiIncoming&) const = default;
};
struct SirFaultEdge {
    std::string fault;
    SirBlockId target{0};
    bool operator==(const SirFaultEdge&) const = default;
};

struct SirInstruction {
    SirOpId id{0};
    SirOpcode opcode{SirOpcode::ConstInt};
    SirValueId result{0};
    SirType type{SirType::Unit};
    SirCardinality cardinality{SirCardinality::One};
    std::string nominal_type;
    std::vector<SirValueId> operands;
    std::vector<SirPhiIncoming> phi_inputs;
    std::vector<SirFaultEdge> fault_edges;
    std::vector<std::string> effects;
    SirEffectTokenId effect_in{0};
    SirEffectTokenId effect_out{0};
    SirRegionId region{0};
    SirProvenanceId provenance{0};
    std::int64_t int_value{0};
    bool bool_value{false};
    std::string text_value;
    std::string callee;
    std::string semantic_name;
    std::uint32_t tag{0};
    SirLocation where;
};

enum class SirTerminatorKind { Br, CondBr, Return, FaultReturn, Unreachable };
std::string sir_terminator_name(SirTerminatorKind kind);
SirTerminatorKind sir_terminator_from_name(const std::string& name);

struct SirTerminator {
    SirTerminatorKind kind{SirTerminatorKind::Unreachable};
    SirBlockId target{0};
    SirBlockId true_target{0};
    SirBlockId false_target{0};
    SirValueId condition{0};
    std::optional<SirValueId> value;
    std::string fault;
    std::vector<SirValueId> fault_payload;
    bool fault_payload_passthrough{false};
    SirEffectTokenId effect_token{0};
    SirLocation where;
};

struct SirBlock {
    SirBlockId id{0};
    std::string label;
    SirRegionId region{0};
    SirEffectTokenId effect_entry{0};
    SirEffectTokenId effect_exit{0};
    std::vector<SirEffectPhiIncoming> effect_phi_inputs;
    std::vector<SirInstruction> instructions;
    SirTerminator terminator;
};

struct SirParameter {
    std::string name;
    std::string authority;
    std::string ownership{"value"};
    SirType type{SirType::Unit};
    SirCardinality cardinality{SirCardinality::One};
    std::string nominal_type;
    SirValueId value{0};
};

struct SirValueSemantics {
    SirValueId value{0};
    SirType type{SirType::Unit};
    SirCardinality cardinality{SirCardinality::One};
    std::string nominal_type;
    SirRegionId region{0};
    SirProvenanceId provenance{0};
    std::vector<std::string> authorities;
    std::string ownership{"value"};
    std::string resource_family;
    std::string borrow_mode{"none"};
};

struct SirRegion {
    SirRegionId id{0};
    SirRegionId parent{0};
    std::string name;
    std::string kind;
    std::string lifetime;
    std::string allocation;
    std::string reclaim;
};

struct SirProvenance {
    SirProvenanceId id{0};
    SirRegionId region{0};
    SirProvenanceId parent{0};
    std::string origin;
    std::string ownership;
    std::string subobject;
    std::string resource_family;
    std::string borrow_mode{"none"};
    bool pinned{false};
};

struct SirBorrowEdge {
    std::uint32_t id{0};
    SirProvenanceId owner{0};
    SirProvenanceId borrower{0};
    SirRegionId region{0};
    std::string mode;              // shared-read | unique-mut
    bool cross_boundary{false};
    std::string source_parameter;
};

struct SirOwnershipEdge {
    std::uint32_t id{0};
    SirProvenanceId owner{0};
    SirProvenanceId child{0};
    SirRegionId region{0};
    std::string role;              // field | optional | choice | many
    std::string path;
    std::uint32_t ordinal{0};
};

struct SirTaskCapture {
    std::uint32_t id{0};
    SirRegionId parallel_region{0};
    SirRegionId task_region{0};
    SirValueId value{0};
    SirProvenanceId provenance{0};
    std::string mode;              // copy-read | shared-read | send
};

struct SirRollbackObligation {
    std::uint32_t id{0};
    SirRegionId transaction_region{0};
    SirProvenanceId provenance{0};
    SirOpId source_op{0};
    std::string action;            // restore-owner | destroy-created | shared-release
};

struct SirFunction {
    std::string name;
    bool is_public{false};
    SirType result_type{SirType::Unit};
    SirCardinality result_cardinality{SirCardinality::One};
    std::string result_nominal_type;
    // Semantic ownership contract for values crossing the instruction boundary.
    // "value" means ordinary value transport. "owned-transfer" means the result
    // carries a resource obligation into the caller. result_resource identifies
    // the semantic resource family (for example heap-pool:Cell or dynamic-many).
    std::string result_ownership{"value"};
    std::string result_resource;
    std::string result_borrow_mode{"none"};
    std::string result_borrow_from;
    std::vector<SirParameter> parameters;
    std::vector<std::string> declared_faults;
    std::vector<std::string> effects;
    SirBlockId entry{0};
    SirRegionId root_region{0};
    SirEffectTokenId effect_root{0};
    std::vector<SirRegion> regions;
    std::vector<SirProvenance> provenances;
    std::vector<SirBorrowEdge> borrow_edges;
    std::vector<SirOwnershipEdge> ownership_edges;
    std::vector<SirTaskCapture> task_captures;
    std::vector<SirRollbackObligation> rollback_obligations;
    std::vector<SirValueSemantics> values;
    std::vector<SirBlock> blocks;
};

struct SirFieldSchema {
    std::string name;
    SirType type{SirType::Unit};
    SirCardinality cardinality{SirCardinality::One};
    std::string nominal_type;
    bool key{false};
    bool revisable{false};
};
struct SirFaultIdentity {
    std::string name;
    std::int32_t code{0};
    std::vector<SirFieldSchema> payload;
    bool operator==(const SirFaultIdentity&) const = default;
};
struct SirDatasetSchema {
    std::string name;
    std::vector<SirFieldSchema> fields;
    std::string key;
    std::vector<std::string> indexes;
};
struct SirChoiceCaseSchema { std::string name; std::uint32_t tag{0}; std::vector<SirFieldSchema> fields; };
struct SirChoiceSchema { std::string name; std::vector<SirChoiceCaseSchema> cases; };
struct SirRelationSchema {
    std::string name, source, target, source_cardinality, target_cardinality, ownership;
    bool unique_pair{false};
    bool reverse_index{false};
};
struct SirPoolSchema {
    std::string name, lifetime, allocation, reclaim, ownership{"unique"};
    std::vector<SirFieldSchema> fields;
};

struct SirSProgram {
    std::uint32_t format_major{5};
    std::uint32_t format_minor{0};
    std::string module_name;
    std::vector<SirFaultIdentity> faults;
    std::vector<SirDatasetSchema> datasets;
    std::vector<SirChoiceSchema> choices;
    std::vector<SirRelationSchema> relations;
    std::vector<SirPoolSchema> pools;
    std::vector<std::string> semantic_facts;
    std::vector<SirFunction> functions;
};


enum class SirStorageClass { Register, Stack, Caller, PoolArena, Heap, Static, Alias };
enum class SirConcreteKind { Scalar, TextView, Optional, ManyInline, ManyDynamic, Aggregate, Choice, BorrowRef, Opaque };

std::string sir_storage_class_name(SirStorageClass storage);
SirStorageClass sir_storage_class_from_name(const std::string& name);
std::string sir_concrete_kind_name(SirConcreteKind kind);
SirConcreteKind sir_concrete_kind_from_name(const std::string& name);

struct SirConcreteLayout {
    std::string name;
    SirConcreteKind kind{SirConcreteKind::Opaque};
    std::uint32_t size{0};
    std::uint32_t align{1};
    std::uint32_t payload_offset{0};
    std::uint32_t element_size{0};
    std::uint32_t element_align{1};
    std::uint32_t capacity{0};
    std::vector<std::uint32_t> field_offsets;
};

struct SirConcreteValue {
    SirValueId value{0};
    SirStorageClass storage{SirStorageClass::Register};
    SirConcreteKind kind{SirConcreteKind::Scalar};
    std::uint32_t size{0};
    std::uint32_t align{1};
    std::uint32_t payload_offset{0};
    std::uint32_t element_size{0};
    std::uint32_t capacity{0};
    std::uint32_t byte_offset{0};
    SirValueId alias_of{0};
    bool escapes{false};
    bool scalar_replaced{false};
};


enum class SirCleanupKind { HeapFree, ManyBufferFree, LifetimeEnd, SharedRelease, CompositeRelease };
std::string sir_cleanup_kind_name(SirCleanupKind kind);
SirCleanupKind sir_cleanup_kind_from_name(const std::string& name);

enum class SirResultTransferKind { None, HeapObject, DynamicMany, BorrowRef, SharedRef };
std::string sir_result_transfer_kind_name(SirResultTransferKind kind);
SirResultTransferKind sir_result_transfer_kind_from_name(const std::string& name);

struct SirConcreteResource {
    std::uint32_t id{0};
    SirValueId value{0};
    SirProvenanceId provenance{0};
    SirRegionId lifetime_region{0};
    SirCleanupKind cleanup{SirCleanupKind::HeapFree};
    std::uint32_t size{0};
    std::uint32_t align{1};
    bool automatic{false};
    bool dynamic_many{false};
    bool shared{false};
    std::uint32_t absorbed_by{0};
    std::string resource_family;
};

struct SirCleanupAction {
    std::uint32_t resource_id{0};
    SirBlockId from_block{0};
    SirBlockId to_block{0xffffffffu}; // UINT32_MAX = function exit
    SirOpId source_op{0};             // nonzero = fault edge produced by this op
    bool fault_exit{false};
    // Transfer discharges the callee obligation without destroying the resource.
    // The caller receives a new obligation under its own lifetime region.
    bool transfer{false};
    std::string reason;
};

enum class SirComponentGuard { Always, OptionalPresent, ChoiceTag, ManyElements };
std::string sir_component_guard_name(SirComponentGuard guard);
SirComponentGuard sir_component_guard_from_name(const std::string& name);

struct SirResourceComponent {
    std::uint32_t id{0};
    SirValueId owner_value{0};
    SirProvenanceId owner_provenance{0};
    SirCleanupKind cleanup{SirCleanupKind::HeapFree};
    SirComponentGuard guard{SirComponentGuard::Always};
    std::uint32_t byte_offset{0};
    std::uint32_t tag{0};
    std::uint32_t element_stride{0};
    std::string type_name;
    std::string path;
};

enum class SirUndoKind { RestoreField, RelationUnlink, RelationLink, RestoreOwner, DestroyResource, SharedRelease };
std::string sir_undo_kind_name(SirUndoKind kind);
SirUndoKind sir_undo_kind_from_name(const std::string& name);

struct SirConcreteUndo {
    std::uint32_t id{0};
    SirRegionId transaction_region{0};
    SirOpId source_op{0};
    SirUndoKind kind{SirUndoKind::RestoreOwner};
    SirValueId target_value{0};
    SirValueId aux_value{0};
    std::uint32_t byte_offset{0};
    std::uint32_t size{0};
    std::uint32_t align{1};
    std::string semantic_name;
};

struct SirConcreteTask {
    std::uint32_t id{0};
    SirRegionId parallel_region{0};
    SirRegionId task_region{0};
    SirOpId begin_op{0};
    SirOpId end_op{0};
};

struct SirConcreteTaskCapture {
    std::uint32_t id{0};
    SirRegionId parallel_region{0};
    SirRegionId task_region{0};
    SirValueId value{0};
    SirProvenanceId provenance{0};
    std::string mode;
};

struct SirConcreteRollback {
    std::uint32_t id{0};
    SirRegionId transaction_region{0};
    SirProvenanceId provenance{0};
    SirOpId source_op{0};
    std::string action;
};

struct SirConcreteFunction {
    std::string name;
    SirStorageClass result_storage{SirStorageClass::Register};
    SirConcreteKind result_kind{SirConcreteKind::Scalar};
    SirResultTransferKind result_transfer{SirResultTransferKind::None};
    std::uint32_t result_size{0};
    std::uint32_t result_align{1};
    std::uint32_t fault_payload_size{0};
    std::uint32_t fault_payload_align{1};
    std::vector<SirConcreteValue> values;
    std::vector<SirConcreteResource> resources;
    std::vector<SirResourceComponent> resource_components;
    std::vector<SirCleanupAction> cleanup_actions;
    std::vector<SirConcreteRollback> rollback_actions;
    std::vector<SirConcreteUndo> undo_actions;
    std::vector<SirConcreteTask> tasks;
    std::vector<SirConcreteTaskCapture> task_captures;
};

struct SirCProgram {
    std::uint32_t format_major{6};
    std::uint32_t format_minor{0};
    std::string module_name;
    std::string target_profile;
    std::string target_triple;
    std::uint32_t pointer_bits{0};
    std::string object_format;
    std::string executable_format;
    std::string calling_convention;
    std::string aggregate_representation;
    std::string cardinality_representation;
    std::string fault_payload_representation;
    std::vector<SirConcreteLayout> concrete_layouts;
    std::vector<SirConcreteFunction> concrete_functions;
    std::vector<SirFaultIdentity> faults;
    std::vector<SirDatasetSchema> datasets;
    std::vector<SirChoiceSchema> choices;
    std::vector<SirRelationSchema> relations;
    std::vector<SirPoolSchema> pools;
    std::vector<std::string> semantic_facts;
    std::vector<SirFunction> functions;
};

class DeductiveLowerer {
public:
    SirSProgram lower(const Program& program, const SSLProgram& ssl) const;
};
class TargetConcretizer {
public:
    SirCProgram concretize(const SirSProgram& sir_s, std::string target_profile, std::string target_triple) const;
};
class SirVerifier {
public:
    void verify(const SirSProgram& sir) const;
    void verify(const SirCProgram& sir) const;
};

std::string serialize_sir_s(const SirSProgram& sir);
std::string serialize_sir_c(const SirCProgram& sir);
SirSProgram deserialize_sir_s(const std::string& text);
SirCProgram deserialize_sir_c(const std::string& text);
inline std::string sir_text(const SirSProgram& sir) { return serialize_sir_s(sir); }
inline std::string sirc_text(const SirCProgram& sir) { return serialize_sir_c(sir); }

} // namespace staze
