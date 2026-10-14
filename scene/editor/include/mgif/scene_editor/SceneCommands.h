#pragma once

#include <mgif/scene_editor/EditorObjectId.h>
#include <mgif/scene_editor/EditorFaultInjection.h>
#include <mgif/scene_editor/EditorHistoryPolicy.h>
#include <mgif/scene_editor/EditorLimits.h>
#include <mgif/scene_editor/GltfDocument.h>
#include <mgif/scene_editor/SceneChangeSet.h>
#include <mgif/scene_editor/GltfValidator.h>
#include <mgif/scene_editor/StructuralDelta.h>

#include <array>
#include <compare>
#include <cstdint>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <variant>
#include <vector>

namespace demo::editor {

enum class MutationStatus : std::uint8_t
{
	applied = 0,
	rejected,
	domainNoop
};

enum class MutationErrorCode : std::uint16_t
{
	none = 0,
	invalidId,
	staleId,
	crossDocument,
	staleRevision,
	staleTransaction,
	staleOccurrence,
	unsupportedKind,
	primitiveHasNoName,
	invalidName,
	invalidTransform,
	matrixAmbiguity,
	cycle,
	multiParent,
	multiScenePolicy,
	lastScene,
	uniqueRootDeleteNoop,
	noOp,
	invalidReference,
	invalidExtension,
	limitExceeded,
	validationFailed,
	allocationFailed,
	internalError,
	sceneRootTransformInvalid,
	sceneRootTransformFrozen,
	snapshotBudgetExceeded,
	generationExhausted,
	revisionExhausted,
	sceneActivationEpochExhausted,
	hierarchyEpochExhausted,
	externalStateSerialExhausted,
	invalidSnapshot,
};

struct MutationToken final
{
	EditorDocumentAuthority authority;
	std::uint64_t revision{0};
	std::uint64_t serial{0};

	[[nodiscard]] constexpr bool valid() const noexcept
	{
		return authority.valid() && serial != 0;
	}

	auto operator<=>(const MutationToken&) const = default;
};

struct MutationGuard final
{
	std::optional<std::uint64_t> expectedRevision;
	std::optional<MutationToken> expectedToken;
};

struct MutationRemapEntry final
{
	EditorObjectId oldId;
	EditorObjectId newId;
};

struct MutationRemap final
{
	std::vector<MutationRemapEntry> scenes;
	std::vector<MutationRemapEntry> nodes;
	std::vector<MutationRemapEntry> primitives;
	std::vector<MutationRemapEntry> meshes;
	std::vector<MutationRemapEntry> materials;
	std::vector<MutationRemapEntry> textures;
	std::vector<MutationRemapEntry> images;
	std::vector<MutationRemapEntry> samplers;
	std::vector<MutationRemapEntry> accessors;
	std::vector<MutationRemapEntry> bufferViews;
	std::vector<MutationRemapEntry> buffers;
	std::vector<MutationRemapEntry> skins;
	std::vector<MutationRemapEntry> animations;
	std::vector<MutationRemapEntry> cameras;
	std::vector<MutationRemapEntry> lights;

	[[nodiscard]] const std::vector<MutationRemapEntry>& forKind(EditorObjectKind kind) const noexcept;
};

struct SceneRootStateTransition final
{
	EditorDocumentAuthority authority{};
	SceneId scene{};
	std::uint64_t documentRevision{0};
	std::uint64_t sceneActivationEpoch{0};
	std::uint64_t baseExternalStateSerial{0};
	std::uint64_t targetExternalStateSerial{0};
	SceneRootTransform before{};
	SceneRootTransform after{};

	bool operator==(const SceneRootStateTransition&) const = default;
};

struct MutationResult final
{
	MutationStatus status{MutationStatus::rejected};
	MutationErrorCode error{MutationErrorCode::internalError};
	std::string message;
	std::uint64_t beforeRevision{0};
	std::uint64_t afterRevision{0};
	GltfValidationReport validation;
	std::vector<EditorObjectId> changedIds;
	MutationRemap remap;
	MutationToken transactionToken;
	SceneMutationEvidence changes;
	std::optional<FullRebuildReason> fullSnapshotReason;
	std::optional<SceneActivationResult> sceneActivationTransition;
	std::optional<SceneRootStateTransition> rootStateTransition;
	HistoryPolicy historyPolicy{HistoryPolicy::unclassified};

	[[nodiscard]] bool applied() const noexcept { return status == MutationStatus::applied; }
	[[nodiscard]] bool rejected() const noexcept { return status == MutationStatus::rejected; }
	[[nodiscard]] bool domainNoop() const noexcept { return status == MutationStatus::domainNoop; }
};

struct NodeTransform final
{
	std::array<double, 3> translation{0.0, 0.0, 0.0};
	std::array<double, 4> rotation{0.0, 0.0, 0.0, 1.0};
	std::array<double, 3> scale{1.0, 1.0, 1.0};

	auto operator<=>(const NodeTransform&) const = default;
};

struct MaterialProperties final
{
	std::array<double, 4> baseColorFactor{1.0, 1.0, 1.0, 1.0};
	double metallicFactor{1.0};
	double roughnessFactor{1.0};
	std::array<double, 3> emissiveFactor{0.0, 0.0, 0.0};
	double alphaCutoff{0.5};
	std::string alphaMode{"OPAQUE"};
	bool doubleSided{false};
};

enum class MaterialTextureSlot : std::uint8_t
{
	baseColor,
	metallicRoughness,
	normal,
	occlusion,
	emissive,
};

struct MaterialTextureProperties final
{
	std::optional<TextureId> texture;
	std::uint32_t texCoord{0};
	// Normal uses scale, occlusion uses strength, and the other slots require 1.
	double scalar{1.0};

	auto operator<=>(const MaterialTextureProperties&) const = default;
};

// TinyGLTF stays private to the implementation. This lossless MGIF value tree
// is the public edit/query representation for material extension objects.
struct MaterialExtensionValue final
{
	using Array = std::vector<MaterialExtensionValue>;
	using Object = std::vector<std::pair<std::string, MaterialExtensionValue>>;
	using Storage = std::variant<std::monostate, bool, std::int32_t, double,
		std::string, Array, Object>;

	Storage value;

	bool operator==(const MaterialExtensionValue& other) const;
};

struct CameraProperties final
{
	std::string type{"perspective"};
	double yfov{0.7853981633974483};
	double znear{0.01};
	double zfar{0.0};
	double aspectRatio{0.0};
	double xmag{1.0};
	double ymag{1.0};
};

struct LightProperties final
{
	std::string type{"point"};
	std::array<double, 3> color{1.0, 1.0, 1.0};
	double intensity{1.0};
	double range{0.0};
	double innerConeAngle{0.0};
	double outerConeAngle{0.7853981633974483};

	auto operator<=>(const LightProperties&) const = default;
};

enum class SnapshotInspectionStatus : std::uint8_t
{
	complete = 0,
	invalidSnapshot,
	allocationFailed,
};

// A read-only, TinyGLTF-free copy of one complete immutable snapshot record.
// MGIFSRC1 begins with magic[8], version:u32, kind:u8, sectionCount:u32. Its
// ordered META, REFS, and RECD sections each use tag:u32 + byteLength:u64 +
// payload. All integers are fixed-width little-endian, doubles retain exact
// IEEE-754 bits, and strings, binary values, and collections are u64-delimited.
// META contains authority and the stable target ID; each stable ID is encoded
// as kind:u8 + slot:u32 + generation:u32 + authority:u64. REFS contains
// source-index/stable-ID evidence for known references; RECD contains every
// retained material or light field with explicit value-type tags. Inspection
// may allocate only the returned copy; snapshots cache no canonical bytes.
// A caller retaining the copy in a command must inspect before publication;
// validation, execution, and restore never inspect. Snapshot equality and live
// validation traverse the same complete semantic fields directly, compare
// exact IEEE-754 bits and stable references, and allocate nothing. Any
// non-complete inspection status has an empty canonicalBytes vector.
struct SnapshotCanonicalRecord final
{
	static constexpr std::uint32_t kSchemaVersion = 1u;

	std::uint32_t schemaVersion{kSchemaVersion};
	EditorObjectKind kind{EditorObjectKind::invalid};
	SnapshotInspectionStatus status{SnapshotInspectionStatus::invalidSnapshot};
	std::vector<std::uint8_t> canonicalBytes;

	[[nodiscard]] bool complete() const noexcept
	{
		return schemaVersion == kSchemaVersion
			&& (kind == EditorObjectKind::material || kind == EditorObjectKind::light)
			&& status == SnapshotInspectionStatus::complete
			&& !canonicalBytes.empty();
	}
};

class MaterialSnapshot final
{
public:
	MaterialSnapshot() noexcept = default;
	[[nodiscard]] bool valid() const noexcept;
	[[nodiscard]] MaterialId id() const noexcept;
	[[nodiscard]] EditorDocumentAuthority authority() const noexcept;
	[[nodiscard]] std::string_view name() const noexcept;
	[[nodiscard]] std::uint64_t snapshotBytes() const noexcept;
	[[nodiscard]] bool operator==(const MaterialSnapshot&) const noexcept;
	[[nodiscard]] SnapshotCanonicalRecord inspectCanonicalRecord() const noexcept;

private:
	struct Impl;
	std::shared_ptr<const Impl> m_impl;
	friend class SceneCommands;
};

class LightSnapshot final
{
public:
	LightSnapshot() noexcept = default;
	[[nodiscard]] bool valid() const noexcept;
	[[nodiscard]] LightId id() const noexcept;
	[[nodiscard]] EditorDocumentAuthority authority() const noexcept;
	[[nodiscard]] std::string_view name() const noexcept;
	[[nodiscard]] std::uint64_t snapshotBytes() const noexcept;
	[[nodiscard]] bool operator==(const LightSnapshot&) const noexcept;
	[[nodiscard]] SnapshotCanonicalRecord inspectCanonicalRecord() const noexcept;

private:
	struct Impl;
	std::shared_ptr<const Impl> m_impl;
	friend class SceneCommands;
};

struct MaterialTextureEdit final
{
	MaterialTextureSlot slot;
	MaterialTextureProperties value;
};

struct MaterialExtensionEdit final
{
  std::string name;
  std::optional<MaterialExtensionValue> value;
};

// A single authored material transaction assembled from several Inspector
// fields. Keeping all scalar/color values together avoids committing field A
// synchronously when the user moves directly to field B.
struct MaterialDraftProperties final
{
  MaterialProperties properties;
  MaterialTextureProperties normal;
  MaterialTextureProperties occlusion;
};

using MaterialEdit = std::variant<
	MaterialProperties,
	MaterialDraftProperties,
	MaterialTextureEdit,
	MaterialExtensionEdit>;

enum class PreparedValueStatus : std::uint8_t
{
	matches = 0,
	invalidSnapshot,
	foreignAuthority,
	staleId,
	staleTransaction,
	valueMismatch,
};

struct PreparedValueValidation final
{
	PreparedValueStatus status{PreparedValueStatus::invalidSnapshot};
	MutationErrorCode error{MutationErrorCode::invalidSnapshot};
	[[nodiscard]] bool matches() const noexcept
	{
		return status == PreparedValueStatus::matches;
	}
};

enum class SceneOccurrencePolicy : std::uint8_t
{
	clickedScene,
	allScenes,
};

// A D20 occurrence is revision-bound and records the clicked scene plus the
// direct parent observed there. clickedScene isolates shared parent paths before
// editing that occurrence; allScenes applies one structurally consistent edit
// to the complete occurrence set. Both routes snapshot every scene and reject
// stale, cyclic, or ambiguous/multiparent input before publication.
struct SceneNodeOccurrence final
{
	SceneId scene;
	NodeId node;
	std::optional<NodeId> parent;
	std::uint64_t revision{0};

	auto operator<=>(const SceneNodeOccurrence&) const = default;
};

struct AddChildNodeRequest final
{
	SceneId scene;
	NodeId parent;
	std::string name;
};

struct AddLightRequest final
{
	SceneId scene;
	std::optional<NodeId> parent;
	std::string nodeName;
	std::string lightName;
	LightProperties properties;
};

struct DuplicateNodeRequest final
{
	SceneNodeOccurrence source;
	std::optional<SceneNodeOccurrence> parent;
	SceneOccurrencePolicy policy{SceneOccurrencePolicy::clickedScene};
};

struct DeleteNodeRequest final
{
	SceneNodeOccurrence occurrence;
	SceneOccurrencePolicy policy{SceneOccurrencePolicy::clickedScene};
};

struct ReparentNodeRequest final
{
	SceneNodeOccurrence source;
	std::optional<SceneNodeOccurrence> parent;
	SceneOccurrencePolicy policy{SceneOccurrencePolicy::clickedScene};
	// Explicit destination scene for a root drop.  Older callers that omit this
	// trailing field retain the historical source-scene behavior.
	SceneId targetScene{};
};

using StructuralRequest = std::variant<
	AddChildNodeRequest,
	AddLightRequest,
	DuplicateNodeRequest,
	DeleteNodeRequest,
	ReparentNodeRequest>;

class PreparedStructuralMutation final
{
public:
	PreparedStructuralMutation(PreparedStructuralMutation&&) noexcept;
	PreparedStructuralMutation& operator=(PreparedStructuralMutation&&) noexcept;
	~PreparedStructuralMutation();

	PreparedStructuralMutation(const PreparedStructuralMutation&) = delete;
	PreparedStructuralMutation& operator=(const PreparedStructuralMutation&) = delete;

	[[nodiscard]] bool valid() const noexcept;
	[[nodiscard]] std::uint64_t snapshotBytes() const noexcept;
	[[nodiscard]] HistoryPolicy historyPolicy() const noexcept;

private:
	struct Impl;
	explicit PreparedStructuralMutation(std::unique_ptr<Impl>) noexcept;
	std::unique_ptr<Impl> m_impl;
	friend class SceneCommands;
};

struct StructuralPrepareResult final
{
	std::unique_ptr<PreparedStructuralMutation> prepared;
	MutationErrorCode error{MutationErrorCode::none};
	HistoryPolicy historyPolicy{HistoryPolicy::unclassified};
	std::string message;

	[[nodiscard]] bool succeeded() const noexcept
	{
		return prepared != nullptr && prepared->valid();
	}
};

struct StructuralMutationResult final
{
	MutationResult mutation;
	std::unique_ptr<StructuralDelta> delta;

	[[nodiscard]] bool hasDelta() const noexcept;
};

class SceneCommands final
{
public:
	explicit SceneCommands(GltfDocument& document,
	                       EditorLimits limits = kDefaultEditorLimits,
	                       EditorFaultInjector* faultInjector = nullptr) noexcept;

	[[nodiscard]] MutationToken token() const noexcept;
	[[nodiscard]] MutationResult query(EditorObjectId id,
	                                  const MutationGuard& guard = {}) const;
	[[nodiscard]] std::optional<SceneNodeOccurrence> captureNodeOccurrence(
		SceneId scene, NodeId node) const;
	[[nodiscard]] StructuralPrepareResult prepareStructural(
		StructuralRequest request,
		const MutationGuard& guard = {}) const;
	[[nodiscard]] StructuralMutationResult commitStructural(
		std::unique_ptr<PreparedStructuralMutation> prepared,
		const MutationGuard& guard = {});
	[[nodiscard]] StructuralMutationResult executeStructural(
		StructuralRequest request,
		const MutationGuard& guard = {});
	[[nodiscard]] MutationResult restoreStructural(
		const StructuralDelta& delta,
		StructuralState target,
		const MutationGuard& guard = {});

	[[nodiscard]] MutationResult addScene(std::string name,
	                                     const MutationGuard& guard = {});
	[[nodiscard]] MutationResult deleteScene(SceneId scene,
	                                        const MutationGuard& guard = {});
	[[nodiscard]] MutationResult addNode(SceneId scene,
	                                    std::string name = {},
	                                    std::optional<NodeId> parent = std::nullopt,
	                                    const MutationGuard& guard = {});
	[[nodiscard]] MutationResult duplicateNode(NodeId node,
	                                           std::optional<NodeId> parent = std::nullopt,
	                                           const MutationGuard& guard = {});
	[[nodiscard]] MutationResult deleteNode(NodeId node,
	                                        const MutationGuard& guard = {});
	[[nodiscard]] MutationResult reparentNode(NodeId node,
	                                          std::optional<NodeId> parent,
	                                          const MutationGuard& guard = {});
	[[nodiscard]] MutationResult duplicateNodeAt(
		const SceneNodeOccurrence& source,
		std::optional<SceneNodeOccurrence> parent,
		SceneOccurrencePolicy policy,
		const MutationGuard& guard = {});
	[[nodiscard]] MutationResult deleteNodeAt(
		const SceneNodeOccurrence& occurrence,
		SceneOccurrencePolicy policy,
		const MutationGuard& guard = {});
	[[nodiscard]] MutationResult reparentNodeAt(
		const SceneNodeOccurrence& source,
		std::optional<SceneNodeOccurrence> parent,
		SceneOccurrencePolicy policy,
		const MutationGuard& guard = {});
	// Root drops have no parent occurrence to carry the destination scene.  Keep
	// this overload separate so legacy callers retain source-scene semantics.
	[[nodiscard]] MutationResult reparentNodeAt(
		const SceneNodeOccurrence& source,
		std::optional<SceneNodeOccurrence> parent,
		SceneOccurrencePolicy policy,
		SceneId targetScene,
		const MutationGuard& guard = {});
	[[nodiscard]] MutationResult rename(EditorObjectId id,
	                                    std::string name,
	                                    const MutationGuard& guard = {});
	[[nodiscard]] MutationResult setNodeTransform(NodeId node,
	                                              const NodeTransform& transform,
	                                              const MutationGuard& guard = {});
	[[nodiscard]] MutationResult attachMesh(NodeId node,
	                                        MeshId mesh,
	                                        const MutationGuard& guard = {});
	[[nodiscard]] MutationResult attachCamera(NodeId node,
	                                          std::optional<CameraId> camera,
	                                          const MutationGuard& guard = {});
	[[nodiscard]] MutationResult attachLight(NodeId node,
	                                         std::optional<LightId> light,
	                                         const MutationGuard& guard = {});
	[[nodiscard]] MutationResult setVisibility(EditorObjectId id,
	                                           bool visible,
	                                           const MutationGuard& guard = {});
	[[nodiscard]] MutationResult removeVisibility(NodeId node,
	                                              const MutationGuard& guard = {});

	[[nodiscard]] MutationResult setMaterial(MaterialId material,
	                                        const MaterialProperties& properties,
	                                        const MutationGuard& guard = {});
	[[nodiscard]] MutationResult setMaterialTexture(
		MaterialId material,
		MaterialTextureSlot slot,
		const MaterialTextureProperties& properties,
		const MutationGuard& guard = {});
	[[nodiscard]] MutationResult setMaterialExtension(
		MaterialId material,
		std::string name,
		std::optional<MaterialExtensionValue> value,
		const MutationGuard& guard = {});
	[[nodiscard]] std::optional<NodeTransform> nodeTransform(NodeId node) const noexcept;
	[[nodiscard]] std::optional<MaterialSnapshot> materialSnapshot(MaterialId material) const;
	[[nodiscard]] std::optional<LightSnapshot> lightSnapshot(LightId light) const;
	[[nodiscard]] std::optional<MaterialSnapshot> prepareMaterialSnapshot(
		MaterialId material, const MaterialEdit& edit) const;
	[[nodiscard]] std::optional<LightSnapshot> prepareLightSnapshot(
		LightId light, const LightProperties& properties) const;
	[[nodiscard]] MutationResult setMaterialSnapshot(
		MaterialId material,
		const MaterialSnapshot& snapshot,
		const MutationGuard& guard = {});
	[[nodiscard]] MutationResult setLightSnapshot(
		LightId light,
		const LightSnapshot& snapshot,
		const MutationGuard& guard = {});
	[[nodiscard]] PreparedValueValidation validateNodeTransform(
		NodeId node,
		const NodeTransform& value,
		MutationToken expected) const noexcept;
	[[nodiscard]] PreparedValueValidation validateMaterialSnapshot(
		MaterialId material,
		const MaterialSnapshot& value,
		MutationToken expected) const noexcept;
	[[nodiscard]] PreparedValueValidation validateLightSnapshot(
		LightId light,
		const LightSnapshot& value,
		MutationToken expected) const noexcept;
	[[nodiscard]] std::optional<MaterialProperties> materialProperties(MaterialId material) const;
	[[nodiscard]] std::optional<MaterialTextureProperties> materialTexture(
		MaterialId material, MaterialTextureSlot slot) const;
	[[nodiscard]] std::optional<MaterialExtensionValue> materialExtension(
		MaterialId material, std::string_view name) const;
	[[nodiscard]] std::optional<MeshId> nodeMesh(NodeId node) const;
	[[nodiscard]] std::vector<PrimitiveId> meshPrimitives(MeshId mesh) const;
	[[nodiscard]] std::optional<MaterialId> primitiveMaterial(
		PrimitiveId primitive) const;
	[[nodiscard]] std::size_t meshReferenceCount(MeshId mesh) const noexcept;
	[[nodiscard]] std::size_t materialReferenceCount(MaterialId material) const noexcept;
	[[nodiscard]] MutationResult duplicateMaterial(
		MaterialId material, const MutationGuard& guard = {});
	[[nodiscard]] MutationResult assignPrimitiveMaterial(PrimitiveId primitive,
	                                                    std::optional<MaterialId> material,
	                                                    const MutationGuard& guard = {});
	[[nodiscard]] MutationResult duplicatePrimitive(PrimitiveId primitive,
	                                               const MutationGuard& guard = {});
	// Node-aware R09 operations preserve the selected occurrence while other
	// nodes keep their existing shared mesh. Split copies the complete mesh and
	// the selected primitive's material; Merge discovers an eligible shared
	// geometry-equivalent mesh and removes only an orphaned source mesh.
	[[nodiscard]] MutationResult splitPrimitive(NodeId node,
	                                           PrimitiveId primitive,
	                                           const MutationGuard& guard = {});
	[[nodiscard]] MutationResult mergeMeshes(NodeId node,
	                                         const MutationGuard& guard = {});
	// Compatibility entry points are meaningful only when the source mesh has a
	// single node reference, so the selected node can be inferred unambiguously.
	[[nodiscard]] MutationResult splitPrimitive(PrimitiveId primitive,
	                                           const MutationGuard& guard = {});
	[[nodiscard]] MutationResult mergeMeshes(MeshId target,
	                                         MeshId source,
	                                         const MutationGuard& guard = {});
	[[nodiscard]] MutationResult setCamera(CameraId camera,
	                                      const CameraProperties& properties,
	                                      const MutationGuard& guard = {});
	[[nodiscard]] MutationResult setCameraFromView(
		CameraId camera,
		NodeId node,
		const CameraProperties& properties,
		const NodeTransform& transform,
		const MutationGuard& guard = {});
	[[nodiscard]] MutationResult setLight(LightId light,
	                                     const LightProperties& properties,
	                                     const MutationGuard& guard = {});
	[[nodiscard]] MutationResult createLight(
		SceneId scene,
		std::optional<NodeId> parent,
		std::string nodeName,
		std::string lightName,
		const LightProperties& properties,
		const MutationGuard& guard = {});
	[[nodiscard]] MutationResult deleteLight(
		LightId light, const MutationGuard& guard = {});
	[[nodiscard]] std::optional<CameraProperties> cameraProperties(CameraId camera) const;
	[[nodiscard]] std::optional<LightProperties> lightProperties(LightId light) const;
	[[nodiscard]] MutationResult setPrimitiveVariant(PrimitiveId primitive,
	                                                std::uint32_t variant,
	                                                std::optional<MaterialId> material,
	                                                const MutationGuard& guard = {});
	[[nodiscard]] std::optional<MaterialId> primitiveVariantMaterial(
		PrimitiveId primitive, std::uint32_t variant) const;
	[[nodiscard]] std::optional<NodeId> cameraNode(CameraId camera) const noexcept;
	[[nodiscard]] std::optional<NodeId> lightNode(LightId light) const noexcept;
	[[nodiscard]] std::optional<SkinId> nodeSkin(NodeId node) const noexcept;
	[[nodiscard]] SceneRootTransform sceneRootTransform(SceneId scene) const noexcept
	{
		return m_document.sceneRootTransform(scene);
	}
	[[nodiscard]] SceneRootTransform sceneRootBaseline(SceneId scene) const noexcept
	{
		return m_document.sceneRootBaseline(scene);
	}
	[[nodiscard]] SceneRootTransformState sceneRootTransformState(SceneId scene) const noexcept
	{
		return m_document.sceneRootTransformState(scene);
	}
	[[nodiscard]] MutationResult previewSceneRootTransform(
		SceneId scene, const SceneRootTransform& transform,
		const MutationGuard& guard = {});
	[[nodiscard]] MutationResult resetSceneRootTransform(
		SceneId scene, const MutationGuard& guard = {});

	// Material variants are document-level extension data. Playback sampling
	// is intentionally absent here: animation evaluation remains authoritative
	// in the document projection/runtime layer.

private:
	GltfDocument& m_document;
	EditorLimits m_limits;
	EditorFaultInjector* m_faultInjector{nullptr};
	std::uint64_t m_serial{0};
};

} // namespace demo::editor
