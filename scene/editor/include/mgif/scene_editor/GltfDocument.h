#pragma once

#include <mgif/scene_editor/EditorObjectId.h>
#include <mgif/scene_editor/SceneChangeSet.h>

#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <memory>
#include <optional>
#include <utility>
#include <array>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace tinygltf {
class Model;
struct Material;
}

namespace demo {
class GltfLoader;
}

namespace demo::editor {

class GltfDocumentIO;
class GltfSemanticCompare;
class GltfValidator;
class SceneCommands;
class EditorStateInspector;
#if defined(MGIF_EDITOR_TESTING)
class EditorStateTestAccess;
#endif

namespace detail {
class GltfDocumentAccess;
class SceneCommandsAccess;
class SceneCompactorAccess;
class SceneMergerAccess;

struct MutationSceneRootStatePlan final
{
	EditorObjectId scene;
	std::array<double, 3> baselineTranslation{0.0, 0.0, 0.0};
	std::array<double, 4> baselineRotation{0.0, 0.0, 0.0, 1.0};
	std::array<double, 3> baselineScale{1.0, 1.0, 1.0};
	std::array<double, 3> currentTranslation{0.0, 0.0, 0.0};
	std::array<double, 4> currentRotation{0.0, 0.0, 0.0, 1.0};
	std::array<double, 3> currentScale{1.0, 1.0, 1.0};
	std::uint64_t expectedExternalStateSerial{0};
	std::uint64_t targetExternalStateSerial{0};
	bool frozen{false};
	bool preserveExternalOnMismatch{false};
	std::vector<EditorObjectId> frozenRootIds;
};

// Internal CPU-only plan used by SceneCommands.  A valid ID preserves the
// logical object identity; an invalid entry asks GltfDocument to allocate a
// fresh slot/generation during the no-throw commit.
struct MutationRegistryPlan final
{
	using IdsByKind = std::array<std::vector<EditorObjectId>,
		static_cast<std::size_t>(EditorObjectKind::count)>;
	IdsByKind idsByKind;
	std::vector<PrimitiveAddress> primitiveAddresses;
	SceneId currentScene;
	bool hierarchyChanged{false};
	// Structural history restoration is the only caller allowed to request an
	// exact ID from a matching tombstone. Ordinary allocation stays append-only.
	bool allowResurrection{false};
	// Immutable history-owned structural deltas own their root-runtime
	// transition. Every other hierarchy publication advances the external
	// state serial for each affected scene.
	bool retainExternalStateSerial{false};
	// Candidate node indices, grouped by candidate scene index. GltfDocument
	// resolves them to stable IDs while preparing the next registry.
	std::vector<std::vector<std::size_t>> frozenSceneRootIndicesByScene;
	std::vector<MutationSceneRootStatePlan> sceneRootStates;
};

enum class MutationCommitStatus : std::uint8_t
{
	applied,
	allocationFailed,
	prePublishFailed,
	generationExhausted,
	revisionExhausted,
	sceneActivationEpochExhausted,
	hierarchyEpochExhausted,
	externalStateSerialExhausted,
	internalError,
};

using MutationPrePublishCallback = void (*)(const MutationRegistryPlan&, void* context);
}

enum class GltfDocumentFormat : std::uint8_t
{
	ascii,
	binary
};

enum class GltfDependencyKind : std::uint8_t
{
	buffer,
	image
};

struct GltfDependency final
{
	GltfDependencyKind kind{GltfDependencyKind::buffer};
	std::size_t objectIndex{0};
	std::string uri;
	std::filesystem::path resolvedPath;
	std::uint64_t byteSize{0};
};

struct PrimitiveGeometryStats final
{
	std::uint32_t vertexCount{0};
	std::uint32_t indexCount{0};

	auto operator<=>(const PrimitiveGeometryStats&) const = default;
};

// Scene-level transform state is editor-owned runtime state. glTF has no
// scene transform field, so preview/reset never rewrite the TinyGLTF
// authority or participate in semantic revision/dirty tracking. Structural
// node edits freeze an active preview into the candidate node matrices and
// rebuild this state for the new hierarchy.
struct SceneRootTransform final
{
	std::array<double, 3> translation{0.0, 0.0, 0.0};
	std::array<double, 4> rotation{0.0, 0.0, 0.0, 1.0};
	std::array<double, 3> scale{1.0, 1.0, 1.0};

	bool operator==(const SceneRootTransform&) const = default;
};

struct SceneRootTransformState final
{
	SceneRootTransform baseline{};
	SceneRootTransform current{};
	std::uint64_t hierarchyEpoch{0};
	std::uint64_t externalStateSerial{0};
	bool frozen{false};
};

enum class SceneActivationStatus : std::uint8_t
{
	Changed = 0,
	Unchanged,
	InvalidScene,
	EpochExhausted,
};

struct SceneActivationResult final
{
	SceneActivationStatus status{SceneActivationStatus::InvalidScene};
	std::optional<SceneId> beforeScene;
	std::optional<SceneId> afterScene;
	std::uint64_t baseEpoch{0};
	std::uint64_t targetEpoch{0};

	[[nodiscard]] bool changed() const noexcept
	{
		return status == SceneActivationStatus::Changed;
	}
};

class GltfDocument final
{
public:
	~GltfDocument();

	GltfDocument(GltfDocument&&) noexcept;
	GltfDocument& operator=(GltfDocument&&) noexcept;

	GltfDocument(const GltfDocument&) = delete;
	GltfDocument& operator=(const GltfDocument&) = delete;

	[[nodiscard]] const std::filesystem::path& sourcePath() const noexcept;
	[[nodiscard]] GltfDocumentFormat format() const noexcept;
	[[nodiscard]] std::span<const GltfDependency> dependencies() const noexcept;

	[[nodiscard]] std::uint64_t revision() const noexcept;
	[[nodiscard]] std::uint64_t cleanRevision() const noexcept;
	[[nodiscard]] bool dirty() const noexcept;
	[[nodiscard]] EditorDocumentAuthority documentAuthority() const noexcept;
	[[nodiscard]] SceneDigest documentDigest() const;

	[[nodiscard]] std::optional<std::size_t> defaultSceneIndex() const noexcept;
	[[nodiscard]] SceneId currentSceneId() const noexcept;
	[[nodiscard]] std::optional<SceneId> activeSceneId() const noexcept;
	[[nodiscard]] std::optional<std::size_t> currentSceneIndex() const noexcept;
	[[nodiscard]] std::uint64_t sceneActivationEpoch() const noexcept;
	[[nodiscard]] SceneRootTransform sceneRootTransform(SceneId scene) const noexcept;
	[[nodiscard]] SceneRootTransform sceneRootBaseline(SceneId scene) const noexcept;
	[[nodiscard]] SceneRootTransformState sceneRootTransformState(SceneId scene) const noexcept;
	[[nodiscard]] bool sceneRootNodeFrozen(SceneId scene, NodeId node) const noexcept;
	[[nodiscard]] std::vector<NodeId> frozenSceneRootNodeIds(SceneId scene) const;

	// Current-scene selection is runtime state. It never changes the glTF
	// default scene, document revision, or clean revision.
	[[nodiscard]] SceneActivationResult
	activateCurrentScene(std::optional<SceneId> scene) noexcept;
	[[nodiscard]] bool selectCurrentScene(SceneId scene) noexcept;
#if defined(MGIF_EDITOR_TESTING)
	[[nodiscard]] bool seedSceneActivationEpochForTesting(
		std::uint64_t value) noexcept;
#endif

	[[nodiscard]] std::size_t objectCount(EditorObjectKind kind) const noexcept;
	[[nodiscard]] EditorObjectId idAt(EditorObjectKind kind, std::size_t index) const noexcept;
	[[nodiscard]] std::optional<std::size_t> indexOf(EditorObjectId id) const noexcept;
	[[nodiscard]] bool contains(EditorObjectId id) const noexcept;
	[[nodiscard]] bool objectVisible(EditorObjectId id) const noexcept;
	[[nodiscard]] bool objectHasVisibility(EditorObjectId id) const noexcept;
	[[nodiscard]] std::optional<PrimitiveAddress> primitiveAddress(PrimitiveId id) const noexcept;
	[[nodiscard]] std::optional<PrimitiveId> primitiveIdAt(PrimitiveAddress address) const noexcept;
	[[nodiscard]] std::optional<PrimitiveGeometryStats>
	primitiveGeometryStats(PrimitiveId id) const noexcept;
	[[nodiscard]] std::string_view objectName(EditorObjectId id) const noexcept;
	[[nodiscard]] std::optional<std::array<double, 4>> materialBaseColor(MaterialId material) const noexcept;
	[[nodiscard]] std::optional<SceneMaterialParametersValue> materialParameters(MaterialId material) const noexcept;
	[[nodiscard]] std::optional<SceneTextureBindingValue> materialTextureBinding(
		MaterialId material, SceneTextureSemantic semantic) const noexcept;
	[[nodiscard]] bool materialHasExtension(MaterialId material, std::string_view name) const noexcept;
	[[nodiscard]] std::vector<NodeId> sceneRootNodeIds(SceneId scene) const;
	[[nodiscard]] std::vector<NodeId> nodeChildIds(NodeId node) const;
	[[nodiscard]] std::vector<NodeId> sceneNodeParentPath(
		SceneId scene, NodeId node) const;
	[[nodiscard]] bool hasNodeOccurrence(
		SceneId scene,
		NodeId node,
		std::optional<NodeId> directParent) const noexcept;
	[[nodiscard]] bool ownsPrimitiveOccurrence(
		SceneId scene,
		NodeId node,
		MeshId mesh,
		PrimitiveId primitive) const noexcept;

	// These read-only views make preservation testable without exposing mutable
	// TinyGLTF state. Stable editor IDs are never written into any JSON value.
	[[nodiscard]] std::string_view documentExtensionsJson() const noexcept;
	[[nodiscard]] std::string_view documentExtrasJson() const noexcept;
	[[nodiscard]] std::string_view assetVersion() const noexcept;
	[[nodiscard]] std::string_view assetMinVersion() const noexcept;
	[[nodiscard]] std::string_view assetGenerator() const noexcept;
	[[nodiscard]] std::string_view assetCopyright() const noexcept;
	[[nodiscard]] std::vector<std::string> documentExtensionNames() const;
	[[nodiscard]] std::vector<std::string> assetExtensionNames() const;
	[[nodiscard]] std::string_view assetExtensionsJson() const noexcept;
	[[nodiscard]] std::string_view assetExtrasJson() const noexcept;
	[[nodiscard]] std::string_view objectExtensionsJson(EditorObjectId id) const noexcept;
	[[nodiscard]] std::string_view objectExtrasJson(EditorObjectId id) const noexcept;

private:
	struct Impl;

	GltfDocument(std::unique_ptr<tinygltf::Model> model,
	             std::filesystem::path sourcePath,
	             GltfDocumentFormat format,
	             std::vector<GltfDependency> dependencies);

	[[nodiscard]] const tinygltf::Model& authorityModel() const noexcept;
	[[nodiscard]] tinygltf::Model& mutableAuthorityModel() noexcept;
	[[nodiscard]] std::unique_ptr<tinygltf::Model> takeAuthorityModel() noexcept;
	[[nodiscard]] std::string semanticRepresentation(std::string& error) const;
	// The semantic digest pair is cached per authority revision.  Projection
	// and the node-name transaction both request these values repeatedly while
	// the underlying TinyGLTF model is unchanged.
	[[nodiscard]] std::string semanticDigest() const;
	void invalidateSemanticDigestCache(bool preservePayloads = false,
	                                   bool preserveMaterialMetadata = false) const noexcept;
	[[nodiscard]] static std::string semanticRepresentation(
		const tinygltf::Model& model, std::string& error);
	[[nodiscard]] static SceneDigest documentDigest(
		const tinygltf::Model& model);
	[[nodiscard]] bool advanceRevision() noexcept;
	[[nodiscard]] detail::MutationCommitStatus resolvePreparedMutationIds(
		detail::MutationRegistryPlan& plan) const noexcept;
	[[nodiscard]] detail::MutationCommitStatus commitMutation(
		std::unique_ptr<tinygltf::Model> model,
		detail::MutationRegistryPlan& plan,
		detail::MutationPrePublishCallback prePublish,
		void* prePublishContext) noexcept;
	// A rename-only publication keeps the validated authority model in place
	// and changes one node name under the same pre-publication transaction
	// boundary.  SceneCommands proves that the registry/topology plan is
	// unchanged before calling this internal helper.
	[[nodiscard]] detail::MutationCommitStatus commitNodeNameMutation(
		std::size_t nodeIndex, std::string name,
		detail::MutationRegistryPlan& plan,
		detail::MutationPrePublishCallback prePublish,
		void* prePublishContext) noexcept;
	// A material-properties-only publication preserves every object registry
	// and scene topology entry. Keep the validated authority model in place and
	// update the scalar/color properties under the same pre-publication boundary.
	[[nodiscard]] detail::MutationCommitStatus commitMaterialPropertiesMutation(
		std::size_t materialIndex, const tinygltf::Material& record,
		detail::MutationRegistryPlan& plan,
		detail::MutationPrePublishCallback prePublish,
		void* prePublishContext) noexcept;
	[[nodiscard]] detail::MutationCommitStatus setSceneRootTransform(
		SceneId scene, const SceneRootTransform& transform) noexcept;
	[[nodiscard]] detail::MutationCommitStatus resetSceneRootTransform(SceneId scene) noexcept;
	void acceptSavedAuthority(std::unique_ptr<tinygltf::Model> model,
	                          std::filesystem::path sourcePath,
	                          GltfDocumentFormat format,
	                          std::vector<GltfDependency> dependencies) noexcept;

	std::unique_ptr<Impl> m_impl;

	friend class GltfDocumentIO;
	friend class GltfSemanticCompare;
	friend class GltfValidator;
	friend class SceneCommands;
	// GltfLoader is the sole renderer-facing decoder. It needs a read-only
	// typed view of TinyGLTF to project this authority without reparsing source
	// bytes; a public adapter would expose the third-party model in this API.
	friend class ::demo::GltfLoader;
	friend class detail::GltfDocumentAccess;
	friend class detail::SceneCommandsAccess;
	friend class detail::SceneCompactorAccess;
	friend class detail::SceneMergerAccess;
	friend class EditorStateInspector;
#if defined(MGIF_EDITOR_TESTING)
	friend class EditorStateTestAccess;
#endif
};

} // namespace demo::editor
