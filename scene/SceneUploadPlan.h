#pragma once

#include "../common/Common.h"
#include "../rhi/RHITypes.h"

#include <mgif/scene_editor/SceneChangeSet.h>

#include <array>
#include <compare>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <optional>
#include <string>
#include <vector>

namespace demo {

struct SceneAssetView;

namespace editor {
class SceneRuntimeMapping;
}

constexpr uint32_t kSceneUploadVertexStride = 48;

// A renderer drawable is an occurrence of one document primitive on one
// document node. Neither stable ID is unique by itself: a node may contain
// multiple primitives and one primitive may be referenced by multiple nodes.
struct SceneDrawableSourceIdentity {
  editor::StableSceneId sceneId{};
  editor::StableSceneId primitiveId{};
  editor::StableSceneId nodeId{};
  uint32_t instanceOrdinal{0};

  [[nodiscard]] bool valid() const noexcept {
    return sceneId.valid() &&
           sceneId.kind == editor::EditorObjectKind::scene &&
           primitiveId.valid() &&
           primitiveId.kind == editor::EditorObjectKind::primitive &&
           nodeId.valid() && nodeId.kind == editor::EditorObjectKind::node &&
           sceneId.authority == primitiveId.authority &&
           primitiveId.authority == nodeId.authority;
  }

  auto operator<=>(const SceneDrawableSourceIdentity&) const = default;
};

struct SceneUploadPlanHeader {
  editor::EditorDocumentAuthority authority{};
  uint64_t documentRevision{0};
  std::optional<editor::StableSceneId> activeScene;
  uint64_t sceneActivationEpoch{0};
  uint64_t rootExternalStateSerial{0};
  editor::SceneDigest projectionDigest{};
  uint64_t mappingEpoch{0};

  [[nodiscard]] bool valid() const noexcept {
    const bool activeSceneValid =
        !activeScene ||
        (activeScene->valid() &&
         activeScene->kind == editor::EditorObjectKind::scene &&
         activeScene->authority == authority);
    return authority.valid() && activeSceneValid &&
           editor::sceneDigestPresent(projectionDigest) && mappingEpoch != 0u;
  }

  [[nodiscard]] bool emptyLegacyEnvelope() const noexcept {
    return !authority.valid() && documentRevision == 0u && !activeScene &&
           sceneActivationEpoch == 0u && rootExternalStateSerial == 0u &&
           !editor::sceneDigestPresent(projectionDigest) && mappingEpoch == 0u;
  }

  bool operator==(const SceneUploadPlanHeader&) const = default;
};

enum class TexturePayloadKind : uint32_t {
  RawRgba8,
  Ktx2Container,
};

struct MeshUploadPlan {
  // Transactional plans bind this to the source primitive, not the glTF mesh.
  editor::StableSceneId sourceId{};
  uint32_t meshIndex{0};
  uint64_t vertexSrcOffset{0};
  uint64_t vertexByteSize{0};
  uint64_t indexSrcOffset{0};
  uint64_t indexByteSize{0};
  uint32_t vertexCount{0};
  uint32_t indexCount{0};
  uint32_t vertexStride{kSceneUploadVertexStride};
  rhi::IndexFormat indexType{rhi::IndexFormat::uint32};
  glm::vec3 localBoundsMin{0.0f};
  glm::vec3 localBoundsMax{0.0f};
  uint32_t primitiveIndex{std::numeric_limits<uint32_t>::max()};
  editor::StableSceneId nodeSourceId{};
  editor::StableSceneId sceneSourceId{};
  uint32_t instanceOrdinal{0};

  [[nodiscard]] SceneDrawableSourceIdentity sourceIdentity() const noexcept {
    return {sceneSourceId, sourceId, nodeSourceId, instanceOrdinal};
  }
};

struct TextureUploadPlan {
  editor::StableSceneId sourceId{};
  uint32_t textureIndex{0};
  rhi::TextureFormat format{rhi::TextureFormat::undefined};
  uint32_t width{0};
  uint32_t height{0};
  uint32_t mipLevels{1};
  TexturePayloadKind payloadKind{TexturePayloadKind::RawRgba8};
  uint64_t payloadSrcOffset{0};
  uint64_t payloadByteSize{0};
};

struct MaterialCreatePlan {
  editor::StableSceneId sourceId{};
  uint32_t materialIndex{0};
  glm::vec4 baseColorFactor{1.0f};
  float metallicFactor{1.0f};
  float roughnessFactor{1.0f};
  float normalScale{1.0f};
  float occlusionStrength{1.0f};
  glm::vec3 emissiveFactor{0.0f};
  int32_t baseColorTexture{-1};
  int32_t normalTexture{-1};
  int32_t metallicRoughnessTexture{-1};
  int32_t occlusionTexture{-1};
  int32_t emissiveTexture{-1};
  int32_t alphaMode{0};
  float alphaCutoff{0.5f};
  int32_t materialWorkflow{0};
  bool doubleSided{false};
};

struct TransformBuildPlan {
  std::vector<glm::mat4> worldTransforms;
  std::vector<editor::StableSceneId> sourceIds;
};

struct SceneDrawInstance {
  // sourceId is the primitive half of the stable drawable occurrence key.
  editor::StableSceneId sourceId{};
  uint32_t instanceIndex{0};
  uint32_t nodeIndex{0};
  uint32_t meshIndex{0};
  uint32_t materialIndex{0};
  glm::mat4 worldTransform{1.0f};
  uint32_t primitiveIndex{std::numeric_limits<uint32_t>::max()};
  editor::StableSceneId nodeSourceId{};
  editor::StableSceneId sceneSourceId{};
  uint32_t instanceOrdinal{0};

  [[nodiscard]] SceneDrawableSourceIdentity sourceIdentity() const noexcept {
    return {sceneSourceId, sourceId, nodeSourceId, instanceOrdinal};
  }
};

struct InstanceBuildPlan {
  std::vector<SceneDrawInstance> instances;
};

struct InstanceCullRecord {
  editor::StableSceneId sourceId{};
  uint32_t instanceIndex{0};
  uint32_t meshIndex{0};
  glm::vec3 worldBoundsMin{0.0f};
  glm::vec3 worldBoundsMax{0.0f};
  glm::vec4 boundingSphere{0.0f};
  editor::StableSceneId nodeSourceId{};
  editor::StableSceneId sceneSourceId{};
  uint32_t instanceOrdinal{0};

  [[nodiscard]] SceneDrawableSourceIdentity sourceIdentity() const noexcept {
    return {sceneSourceId, sourceId, nodeSourceId, instanceOrdinal};
  }
};

struct DescriptorUpdatePlan {
  editor::StableSceneId sourceId{};
  uint32_t materialIndex{0};
  std::array<int32_t, 5> textureIndices{{-1, -1, -1, -1, -1}};
};

enum class SceneDrawBucket : uint32_t {
  Opaque,
  AlphaMask,
  Transparent,
};

struct DrawCommandBuildPlan {
  editor::StableSceneId sourceId{};
  uint32_t instanceIndex{0};
  uint32_t meshIndex{0};
  uint32_t materialIndex{0};
  SceneDrawBucket bucket{SceneDrawBucket::Opaque};
  editor::StableSceneId nodeSourceId{};
  editor::StableSceneId sceneSourceId{};
  uint32_t instanceOrdinal{0};

  [[nodiscard]] SceneDrawableSourceIdentity sourceIdentity() const noexcept {
    return {sceneSourceId, sourceId, nodeSourceId, instanceOrdinal};
  }
};

enum class SceneRequiredResourceKind : uint8_t {
  Mesh = 0,
  Texture = 1,
  Material = 2,
};

struct SceneRequiredResource {
  editor::StableSceneId sourceId{};
  SceneRequiredResourceKind kind{SceneRequiredResourceKind::Mesh};
  bool placeholderAllowed{false};
  editor::StableSceneId nodeSourceId{};
  editor::StableSceneId sceneSourceId{};
  uint32_t instanceOrdinal{0};

  [[nodiscard]] SceneDrawableSourceIdentity sourceIdentity() const noexcept {
    return {sceneSourceId, sourceId, nodeSourceId, instanceOrdinal};
  }

  bool operator==(const SceneRequiredResource&) const = default;
};

struct SceneUploadPlan {
  SceneUploadPlanHeader header;
  std::vector<MeshUploadPlan> meshes;
  std::vector<TextureUploadPlan> textures;
  std::vector<MaterialCreatePlan> materials;
  TransformBuildPlan transforms;
  InstanceBuildPlan instances;
  std::vector<InstanceCullRecord> cullRecords;
  std::vector<DescriptorUpdatePlan> descriptorUpdates;
  std::vector<DrawCommandBuildPlan> drawCommands;
  std::vector<SceneRequiredResource> requiredResources;
};

enum class SceneUploadPlanError : uint8_t {
  None = 0,
  InvalidAsset = 1,
  InvalidEnvelope = 2,
  EnvelopeMismatch = 3,
  MissingStableId = 4,
  StableIdMismatch = 5,
  DuplicateStableId = 6,
  InvalidRecord = 7,
  PayloadRange = 8,
  MissingRequiredResource = 9,
  PlaceholderNotAllowed = 10,
};

struct SceneUploadPlanValidationResult {
  static constexpr size_t kNoRecord = std::numeric_limits<size_t>::max();

  bool valid{true};
  SceneUploadPlanError code{SceneUploadPlanError::None};
  std::optional<editor::StableSceneId> sourceId;
  size_t recordIndex{kNoRecord};
  std::string error;
};

// Binds transient candidate coordinates to the immutable document projection.
// The operation mutates only the caller-owned plan and never invents an ID for
// a missing projection entry.
[[nodiscard]] SceneUploadPlanValidationResult bindSceneUploadPlanProjection(
    const SceneAssetView& asset,
    const editor::SceneRuntimeMapping& mapping,
    SceneUploadPlan& plan);

}  // namespace demo
