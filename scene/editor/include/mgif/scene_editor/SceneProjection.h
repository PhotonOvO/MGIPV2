#pragma once

#include <mgif/scene_editor/SceneRuntimeMapping.h>
#include <mgif/scene_editor/GltfDocument.h>
#include <mgif/scene_editor/AnimationPlayback.h>
#include <mgif/scene_editor/EditorSelectionTypes.h>

#include "../../../../../scene/SceneAsset.h"

#include <cstdint>
#include <limits>
#include <memory>
#include <new>
#include <optional>
#include <string>
#include <vector>

namespace demo::editor {

struct SceneAnimationPose final {
	std::uint32_t animationIndex{0};
	double time{0.0};
	bool loop{true};
	std::vector<AnimationTransformSample> transforms;
};

struct SceneProjectionOptions final
{
	bool computeSemanticHash{true};
	bool includeInvisible{true};
	bool buildMeshlets{false};
	bool keepRawVertexData{false};
	// Runtime-only material variant selection.  This never mutates the
	// GltfDocument authority or participates in document history.
	std::optional<std::uint32_t> activeVariant;
	// Runtime-only animation pose selection. The index is the document
	// animation index; time is sampled without mutating document authority.
	std::optional<std::uint32_t> activeAnimation;
	double animationTime{0.0};
	bool animationLoop{true};
	std::optional<SceneAnimationPose> animationPose;
	std::uint64_t baseMappingEpoch{0};
};

// Opaque authority carried with a renderer-facing projection.  The public
// SceneAsset and SceneRuntimeMapping values are intentionally mutable at their
// submission boundaries, so their integrity encodings cannot be used as the
// trust root.  This capability owns an immutable copy of the projection-owner
// associations derived from the live GltfDocument and can only be constructed
// by SceneProjection (or the explicitly test-gated synthetic fixture helper).
class SceneProjectionOwnerCapability final
{
private:
	struct Association final
	{
		EditorObjectKind kind{EditorObjectKind::invalid};
		std::uint32_t sceneAssetIndex{std::numeric_limits<std::uint32_t>::max()};
		std::uint32_t sourceIndex{std::numeric_limits<std::uint32_t>::max()};
		StableSceneId stableId{};
		std::uint64_t runtimeIdentity{0};

		bool operator==(const Association&) const = default;
	};

	struct State final
	{
		SceneRuntimeMapping mapping;
		std::vector<Association> cameraAssociations;
		std::vector<Association> lightAssociations;
	};

public:
	SceneProjectionOwnerCapability() = default;

	[[nodiscard]] bool valid() const noexcept
	{
		return static_cast<bool>(m_state);
	}
	[[nodiscard]] bool sameSeal(
		const SceneProjectionOwnerCapability& other) const noexcept
	{
		return m_state && m_state == other.m_state;
	}
	[[nodiscard]] bool sameOwner(
		const SceneProjectionOwnerCapability& other) const noexcept
	{
		return m_state && other.m_state &&
			m_state->mapping.authority() == other.m_state->mapping.authority();
	}
	[[nodiscard]] bool validateMapping(
		const SceneRuntimeMapping& mapping,
		std::string* error = nullptr) const noexcept
	{
		if (!m_state)
		{
			if (error != nullptr)
			{
				try { *error = "projection owner capability is absent"; }
				catch (...) {}
			}
			return false;
		}
		if (!(mapping == m_state->mapping))
		{
			if (error != nullptr)
			{
				try { *error = "projection mapping differs from its owner capability"; }
				catch (...) {}
			}
			return false;
		}
		if (error != nullptr)
		{
			try { error->clear(); }
			catch (...) {}
		}
		return true;
	}

	// Renderer mapping epochs are allocated by the adapter, not the document
	// projection. Rebinding is permitted only when every owner association and
	// every document-side lifecycle coordinate is unchanged.
	[[nodiscard]] SceneProjectionOwnerCapability bindMapping(
		const SceneRuntimeMapping& mapping,
		std::string* error = nullptr) const noexcept
	{
		try
		{
			if (!m_state)
			{
				if (error != nullptr) *error = "projection owner capability is absent";
				return {};
			}
			const SceneRuntimeMappingFence& source = m_state->mapping.fence();
			const SceneRuntimeMappingFence& target = mapping.fence();
			if (m_state->mapping.entries() != mapping.entries() ||
				m_state->mapping.aliases() != mapping.aliases() ||
				source.authority != target.authority ||
				source.appliedDocumentRevision != target.appliedDocumentRevision ||
				source.activeScene != target.activeScene ||
				source.sceneActivationEpoch != target.sceneActivationEpoch ||
				source.rootExternalStateSerial != target.rootExternalStateSerial ||
				source.projectionDigest != target.projectionDigest ||
				target.mappingEpoch == 0u)
			{
				if (error != nullptr)
					*error = "renderer mapping cannot be rebound to this projection owner";
				return {};
			}
			auto state = std::make_shared<State>(*m_state);
			state->mapping = mapping;
			if (error != nullptr) error->clear();
			return SceneProjectionOwnerCapability{std::move(state)};
		}
		catch (const std::bad_alloc&)
		{
			if (error != nullptr)
			{
				try { *error = "projection owner rebinding allocation failed"; }
				catch (...) {}
			}
		}
		catch (...)
		{
			if (error != nullptr)
			{
				try { *error = "projection owner rebinding failed"; }
				catch (...) {}
			}
		}
		return {};
	}

	// Validate the submitted asset and nested projection mapping against the
	// immutable owner snapshot.  This performs no renderer/RHI work and is safe
	// to call before any transaction state is touched.
	[[nodiscard]] bool validate(const SceneAsset& asset,
	                           const SceneRuntimeMapping& mapping,
	                           std::string* error = nullptr) const noexcept
	{
		const auto setError = [error](const char* message) noexcept
		{
			if (error == nullptr)
			{
				return;
			}
			try
			{
				*error = message;
			}
			catch (...)
			{
				// Validation remains a rejection even if diagnostics cannot allocate.
			}
		};

		if (!m_state)
		{
			setError("projection owner capability is absent");
			return false;
		}
		if (!validateMapping(mapping, error)) return false;

		const auto validateAssociations = [&](EditorObjectKind kind,
		                                     const auto& expected,
		                                     const auto& submitted,
		                                     auto sourceIndex,
		                                     auto runtimeIdentity) noexcept
		{
			if (submitted.size() != expected.size())
			{
				setError("camera/light owner cardinality differs from its capability");
				return false;
			}
			for (std::size_t index = 0; index < expected.size(); ++index)
			{
				const Association& association = expected[index];
				if (association.kind != kind ||
					association.sceneAssetIndex != index)
				{
					setError("camera/light capability coordinate is invalid");
					return false;
				}
				const std::optional<EditorObjectId> owner =
					mapping.documentIdAt(kind,
						static_cast<std::uint32_t>(index));
				if (!owner || stableSceneId(*owner) != association.stableId)
				{
					setError("camera/light projection owner differs from its capability");
					return false;
				}
					const std::optional<SceneRuntimeMappingEntry> entry =
						mapping.entry(*owner);
					if (!entry ||
						entry->sceneAssetIndex == SceneRuntimeMapping::kInvalidIndex ||
						entry->projectionIdentity != association.runtimeIdentity ||
					entry->runtimeIndex != association.runtimeIdentity)
				{
					setError("camera/light mapping coordinate differs from its capability");
					return false;
				}
				if (sourceIndex(submitted[index]) != association.sourceIndex ||
					runtimeIdentity(submitted[index]) != association.runtimeIdentity)
				{
					setError("camera/light source identity differs from its owner capability");
					return false;
				}
			}
			return true;
		};

		if (!validateAssociations(
			EditorObjectKind::camera, m_state->cameraAssociations,
			asset.cameras,
			[](const SceneCamera& value) noexcept { return value.sourceCameraIndex; },
			[](const SceneCamera& value) noexcept { return value.runtimeIdentity; }) ||
			!validateAssociations(
				EditorObjectKind::light, m_state->lightAssociations,
				asset.lights,
				[](const SceneLight& value) noexcept { return value.sourceLightIndex; },
				[](const SceneLight& value) noexcept { return value.runtimeIdentity; }))
		{
			return false;
		}
		if (error != nullptr)
		{
			try
			{
				 error->clear();
			}
			catch (...)
			{
			}
		}
		return true;
	}

#if defined(MGIF_RENDERER_OWNER_TESTING)
	// Synthetic renderer fixtures do not own a GltfDocument.  Keep their seal
	// available only to test translation units; production code has no public
	// constructor for caller-controlled authority data.
	[[nodiscard]] static SceneProjectionOwnerCapability makeForTesting(
		const SceneAsset& asset, const SceneRuntimeMapping& mapping,
		std::string* error = nullptr) noexcept
	{
		try
		{
			auto state = std::make_shared<State>();
			state->mapping = mapping;
			const auto append = [&](EditorObjectKind kind, const auto& values,
			                        std::vector<Association>& destination,
			                        auto sourceIndex, auto runtimeIdentity)
			{
				 destination.reserve(values.size());
				 for (std::size_t index = 0; index < values.size(); ++index)
				 {
					 const auto owner = mapping.documentIdAt(
						kind, static_cast<std::uint32_t>(index));
					 if (!owner)
					 {
						 return false;
					 }
					 destination.push_back(Association{
						 kind, static_cast<std::uint32_t>(index),
						 sourceIndex(values[index]), stableSceneId(*owner),
						 runtimeIdentity(values[index])});
				 }
				 return true;
			};
			if (!append(EditorObjectKind::camera, asset.cameras,
			             state->cameraAssociations,
			             [](const SceneCamera& value) noexcept {
				             return value.sourceCameraIndex;
			             },
			             [](const SceneCamera& value) noexcept {
				             return value.runtimeIdentity;
			             }) ||
				!append(EditorObjectKind::light, asset.lights,
				        state->lightAssociations,
				        [](const SceneLight& value) noexcept {
				        return value.sourceLightIndex;
				        },
				        [](const SceneLight& value) noexcept {
				        return value.runtimeIdentity;
				        }))
			{
				if (error != nullptr) *error = "synthetic projection owner is incomplete";
				return {};
			}
			if (error != nullptr) error->clear();
			return SceneProjectionOwnerCapability{std::move(state)};
		}
		catch (const std::bad_alloc&)
		{
			if (error != nullptr) *error = "synthetic projection owner allocation failed";
		}
		catch (...)
		{
			if (error != nullptr) *error = "synthetic projection owner creation failed";
		}
		return {};
	}
#endif


private:
	explicit SceneProjectionOwnerCapability(std::shared_ptr<const State> state) noexcept
		: m_state(std::move(state))
	{
	}

	// Issuance is restricted to SceneProjection. The submitted asset and mapping
	// remain public transport values, but a submitting caller cannot mint a new
	// seal after coherently rewriting those values.
	[[nodiscard]] static SceneProjectionOwnerCapability fromDocument(
		const GltfDocument& document, const SceneAsset& asset,
		const SceneRuntimeMapping& mapping, std::string* error = nullptr) noexcept;

	std::shared_ptr<const State> m_state;

	friend class SceneProjection;
};

// Renderer-facing image payloads are indexed by SceneAsset coordinates, while
// editor commands address glTF texture and sampler objects.  Keep that
// relationship explicit so a non-identity texture->image layout, BasisU
// source, shared image, or color-space duplicate cannot fall back to a stable
// ID slot by accident.
struct SceneTextureProjectionBinding final
{
	StableSceneId textureId{};
	StableSceneId sourceImageId{};
	std::uint32_t sourceImageIndex{std::numeric_limits<std::uint32_t>::max()};
	std::uint32_t sceneAssetIndex{std::numeric_limits<std::uint32_t>::max()};
	std::uint32_t colorSceneAssetIndex{std::numeric_limits<std::uint32_t>::max()};
	std::uint32_t linearSceneAssetIndex{std::numeric_limits<std::uint32_t>::max()};
	std::int32_t samplerIndex{-1};
	std::optional<StableSceneId> samplerId;

	bool operator==(const SceneTextureProjectionBinding&) const = default;
};

struct SceneSamplerProjectionBinding final
{
	StableSceneId samplerId{};
	std::uint32_t textureUseCount{0};

	bool operator==(const SceneSamplerProjectionBinding&) const = default;
};

struct SceneProjectionNodeOccurrence final
{
	NodeId node;
	std::optional<NodeId> parent;
	std::uint32_t depth{0};

	auto operator<=>(const SceneProjectionNodeOccurrence&) const = default;
};

struct SceneProjectionPrimitiveOccurrence final
{
	PrimitiveOccurrenceKey key;
	std::uint32_t scenePrimitiveIndex{0};
	bool visible{false};

	auto operator<=>(const SceneProjectionPrimitiveOccurrence&) const = default;
};

struct SceneProjectionResult final
{
	SceneAsset asset;
	SceneRuntimeMapping mapping;
	SceneProjectionOwnerCapability ownerCapability;
	std::string semanticHash;
	std::string error;
	std::uint64_t documentRevision{0};
	std::uint64_t sceneActivationEpoch{0};
	std::uint64_t mappingEpoch{0};
	std::optional<StableSceneId> activeScene;
	SceneDigest documentDigest{};
	SceneDigest projectionDigest{};
	ImmutableSceneSnapshotRef immutableSnapshot;
	SceneRootTransformState sceneRootTransform{};
	std::vector<SceneTextureProjectionBinding> textureBindings;
	std::vector<SceneSamplerProjectionBinding> samplerBindings;
	std::vector<SceneProjectionNodeOccurrence> occurrenceNodes;
	std::vector<SceneProjectionPrimitiveOccurrence> primitiveOccurrences;
	std::optional<std::uint32_t> activeVariant;
	std::optional<std::uint32_t> activeAnimation;
	double animationTime{0.0};
	bool animationLoop{true};

	[[nodiscard]] bool succeeded() const noexcept
	{
		return error.empty() && ownerCapability.valid();
	}
	[[nodiscard]] explicit operator bool() const noexcept
	{
		return succeeded();
	}
};

namespace detail {
struct NodeNameProjectionDigestCache;

[[nodiscard]] std::optional<SceneProjectionResult>
reprojectMaterialParameters(const GltfDocument& document,
                      const SceneProjectionResult& previous,
                      const SceneMutationEvidence& evidence,
                      const SceneProjectionOptions& options) noexcept;

[[nodiscard]] std::optional<SceneProjectionResult>
reprojectNodeNameOnly(const GltfDocument& document,
	                  const SceneProjectionResult& previous,
	                  NodeId node,
	                  const SceneProjectionOptions& options,
	                  const NodeNameProjectionDigestCache& cached) noexcept;
} // namespace detail

class SceneProjection final
{
public:
	// Build a renderer-facing projection from the current document scene. The
	// document is read-only for the entire operation and remains the semantic
	// authority after return.
	[[nodiscard]] static SceneProjectionResult project(
		const GltfDocument& document,
		const SceneProjectionOptions& options = {});

	[[nodiscard]] static SceneProjectionResult build(
		const GltfDocument& document,
		const SceneProjectionOptions& options = {})
	{
		return project(document, options);
	}

	[[nodiscard]] static bool projectInto(
		const GltfDocument& document,
		SceneAsset& asset,
		SceneRuntimeMapping& mapping,
		std::string* error = nullptr,
		const SceneProjectionOptions& options = {});

	// Stable digest for the derived payload and mapping. It is intentionally
	// separate from GltfSemanticCompare::digest(document).
	[[nodiscard]] static std::string digest(
		const SceneAsset& asset,
		const SceneRuntimeMapping& mapping);

private:
	[[nodiscard]] static bool sealOwner(
		const GltfDocument& document, SceneProjectionResult& result) noexcept;

	friend std::optional<SceneProjectionResult>
	detail::reprojectMaterialParameters(const GltfDocument& document,
		const SceneProjectionResult& previous, const SceneMutationEvidence& evidence,
		const SceneProjectionOptions& options) noexcept;

	friend std::optional<SceneProjectionResult>
	detail::reprojectNodeNameOnly(
		const GltfDocument& document,
		const SceneProjectionResult& previous,
		NodeId node,
		const SceneProjectionOptions& options,
		const detail::NodeNameProjectionDigestCache& cached) noexcept;
};

} // namespace demo::editor
