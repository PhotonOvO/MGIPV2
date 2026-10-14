#pragma once

#include <mgif/scene_editor/EditorObjectId.h>
#include "../../../../../common/ImmutableByteBuffer.h"

#include <array>
#include <compare>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>
#include <variant>
#include <vector>

namespace demo::editor {

using SceneDigest = std::array<std::uint8_t, 32>;

inline constexpr std::uint32_t kSceneChangeSchemaVersion = 1u;
inline constexpr std::uint32_t kImmutableSceneSnapshotSchemaVersion = 1u;
inline constexpr std::uint64_t kMaximumSceneChangePayloadBytes =
    256u * 1024u * 1024u;

[[nodiscard]] bool sceneDigestPresent(const SceneDigest &digest) noexcept;

struct StableSceneId final {
  EditorObjectKind kind{EditorObjectKind::invalid};
  EditorDocumentAuthority authority{};
  std::uint32_t slot{0};
  std::uint32_t generation{0};

  [[nodiscard]] constexpr bool valid() const noexcept {
    return EditorObjectId{kind, slot, generation, authority}.valid();
  }

  auto operator<=>(const StableSceneId &) const = default;
};

[[nodiscard]] constexpr StableSceneId
stableSceneId(EditorObjectId id) noexcept {
  return StableSceneId{id.kind, id.authority, id.slot, id.generation};
}

[[nodiscard]] constexpr EditorObjectId
editorObjectId(StableSceneId id) noexcept {
  return EditorObjectId{id.kind, id.slot, id.generation, id.authority};
}

// This value is a lossless, cycle-free transport of the document mutation
// token fields. Producer integration converts to and from its command token.
struct SceneMutationToken final {
  EditorDocumentAuthority authority{};
  std::uint64_t revision{0};
  std::uint64_t serial{0};

  [[nodiscard]] constexpr bool valid() const noexcept {
    return authority.valid() && serial != 0u;
  }

  auto operator<=>(const SceneMutationToken &) const = default;
};

enum class SceneChangeDomain : std::uint8_t {
  DocumentMutation = 0,
  SceneActivation = 1,
  SceneRootState = 2,
  FullSnapshot = 3,
};

enum class SceneChangeClass : std::uint8_t {
  Transform = 0,
  MaterialParameters = 1,
  MaterialBinding = 2,
  Camera = 3,
  Light = 4,
  Visibility = 5,
  Hierarchy = 6,
  Topology = 7,
  FullRebuild = 8,
};

enum class SceneChangeAction : std::uint8_t {
  Create = 0,
  Update = 1,
  Delete = 2,
};

enum class SceneField : std::uint16_t {
  NodeLocalTransform = 0,
  NodeLocalVisibility = 1,
  MaterialParameters = 2,
  MaterialExtension = 3,
  MaterialTextureBinding = 4,
  PrimitiveMaterialBinding = 5,
  CameraParameters = 6,
  CameraAttachment = 7,
  LightParameters = 8,
  LightAttachment = 9,
  HierarchyParent = 10,
  HierarchyAttachment = 11,
  TopologyMetadata = 12,
  GeometryBufferRange = 13,
  TexturePayload = 14,
  SceneRootTransform = 15,
};

enum class SceneValueTag : std::uint8_t {
  NodeTransform = 0,
  Visibility = 1,
  MaterialParameters = 2,
  MaterialExtension = 3,
  TextureBinding = 4,
  PrimitiveMaterialBinding = 5,
  Camera = 6,
  ObjectAttachment = 7,
  Light = 8,
  Hierarchy = 9,
  Topology = 10,
  BufferRange = 11,
  TexturePayload = 12,
};

struct SceneTrsValue final {
  std::array<double, 3> translation{0.0, 0.0, 0.0};
  std::array<double, 4> rotation{0.0, 0.0, 0.0, 1.0};
  std::array<double, 3> scale{1.0, 1.0, 1.0};

  bool operator==(const SceneTrsValue &) const = default;
};

struct SceneMatrixValue final {
  std::array<double, 16> columnMajor{1.0, 0.0, 0.0, 0.0, 0.0, 1.0, 0.0, 0.0,
                                     0.0, 0.0, 1.0, 0.0, 0.0, 0.0, 0.0, 1.0};

  bool operator==(const SceneMatrixValue &) const = default;
};

struct SceneNodeTransformValue final {
  using Storage = std::variant<SceneTrsValue, SceneMatrixValue>;
  Storage value{};

  bool operator==(const SceneNodeTransformValue &) const = default;
};

struct SceneVisibilityValue final {
  bool visible{true};

  bool operator==(const SceneVisibilityValue &) const = default;
};

enum class SceneAlphaMode : std::uint8_t {
  Opaque = 0,
  Mask = 1,
  Blend = 2,
};

struct SceneMaterialParametersValue final {
  std::array<double, 4> baseColorFactor{1.0, 1.0, 1.0, 1.0};
  double metallicFactor{1.0};
  double roughnessFactor{1.0};
  std::array<double, 3> emissiveFactor{0.0, 0.0, 0.0};
  double alphaCutoff{0.5};
  SceneAlphaMode alphaMode{SceneAlphaMode::Opaque};
  bool doubleSided{false};

  bool operator==(const SceneMaterialParametersValue &) const = default;
};

struct SceneMaterialExtensionValue final {
  std::string extensionName;
  std::vector<std::uint8_t> canonicalValueBytes;
  SceneDigest contentDigest{};

  bool operator==(const SceneMaterialExtensionValue &) const = default;
};

enum class SceneTextureSemantic : std::uint8_t {
  BaseColor = 0,
  MetallicRoughness = 1,
  Normal = 2,
  Occlusion = 3,
  Emissive = 4,
};

struct SceneTextureBindingValue final {
  SceneTextureSemantic semantic{SceneTextureSemantic::BaseColor};
  std::optional<StableSceneId> texture;
  std::optional<StableSceneId> sampler;
  std::uint32_t uvSet{0};
  std::array<double, 2> uvOffset{0.0, 0.0};
  std::array<double, 2> uvScale{1.0, 1.0};
  double uvRotation{0.0};
  double scalar{1.0};

  bool operator==(const SceneTextureBindingValue &) const = default;
};

struct ScenePrimitiveMaterialBindingValue final {
  std::optional<StableSceneId> material;
  std::optional<std::uint32_t> sourceVariant;

  bool operator==(const ScenePrimitiveMaterialBindingValue &) const = default;
};

enum class SceneCameraProjection : std::uint8_t {
  Perspective = 0,
  Orthographic = 1,
};

struct SceneCameraValue final {
  SceneCameraProjection projection{SceneCameraProjection::Perspective};
  double yfov{0.7853981633974483};
  double znear{0.01};
  double zfar{0.0};
  double aspectRatio{0.0};
  double xmag{1.0};
  double ymag{1.0};

  bool operator==(const SceneCameraValue &) const = default;
};

struct SceneObjectAttachmentValue final {
  std::optional<StableSceneId> node;

  bool operator==(const SceneObjectAttachmentValue &) const = default;
};

enum class SceneLightType : std::uint8_t {
  Directional = 0,
  Point = 1,
  Spot = 2,
};

struct SceneLightValue final {
  SceneLightType type{SceneLightType::Point};
  std::array<double, 3> color{1.0, 1.0, 1.0};
  double intensity{1.0};
  double range{0.0};
  double innerConeAngle{0.0};
  double outerConeAngle{0.7853981633974483};

  bool operator==(const SceneLightValue &) const = default;
};

struct SceneHierarchyValue final {
  std::optional<StableSceneId> parent;
  std::vector<StableSceneId> attachments;

  bool operator==(const SceneHierarchyValue &) const = default;
};

enum class SceneTopologyKind : std::uint8_t {
  Object = 0,
  Instance = 1,
  Geometry = 2,
  Texture = 3,
};

struct SceneTopologyValue final {
  SceneTopologyKind topologyKind{SceneTopologyKind::Object};
  EditorObjectKind objectKind{EditorObjectKind::invalid};
  std::vector<StableSceneId> dependencies;
  SceneDigest schemaDigest{};
  std::uint64_t elementCount{0};

  bool operator==(const SceneTopologyValue &) const = default;
};

enum class SceneBufferSemantic : std::uint8_t {
  Vertex = 0,
  Index = 1,
  Morph = 2,
  Skin = 3,
};

struct SceneBufferRangeValue final {
  SceneBufferSemantic semantic{SceneBufferSemantic::Vertex};
  std::uint64_t logicalOffset{0};
  std::uint64_t byteSize{0};
  SceneDigest schemaDigest{};
  SceneDigest contentDigest{};
  std::vector<std::uint8_t> bytes;

  bool operator==(const SceneBufferRangeValue &) const = default;
};

enum class SceneColorSpace : std::uint8_t {
  Linear = 0,
  Srgb = 1,
};

struct SceneTexturePayloadValue final {
  std::uint32_t width{0};
  std::uint32_t height{0};
  std::uint32_t depth{1};
  std::uint32_t mipCount{1};
  std::uint32_t layerCount{1};
  std::uint32_t formatTag{0};
  SceneColorSpace colorSpace{SceneColorSpace::Linear};
  SceneDigest schemaDigest{};
  SceneDigest contentDigest{};
  std::vector<std::uint8_t> bytes;

  bool operator==(const SceneTexturePayloadValue &) const = default;
};

using SceneFieldValue =
    std::variant<SceneNodeTransformValue, SceneVisibilityValue,
                 SceneMaterialParametersValue, SceneMaterialExtensionValue,
                 SceneTextureBindingValue, ScenePrimitiveMaterialBindingValue,
                 SceneCameraValue, SceneObjectAttachmentValue, SceneLightValue,
                 SceneHierarchyValue, SceneTopologyValue, SceneBufferRangeValue,
                 SceneTexturePayloadValue>;

[[nodiscard]] SceneValueTag
sceneFieldValueTag(const SceneFieldValue &value) noexcept;
[[nodiscard]] bool
sceneFieldValueMatchesField(SceneField field,
                            const SceneFieldValue &value) noexcept;
[[nodiscard]] std::vector<StableSceneId>
sceneFieldValueReferences(const SceneFieldValue &value);

struct SourceMutationFence final {
  std::uint64_t baseDocumentRevision{0};
  std::uint64_t targetDocumentRevision{0};
  SceneMutationToken mutationToken{};
  SceneDigest baseDocumentDigest{};
  SceneDigest targetDocumentDigest{};

  bool operator==(const SourceMutationFence &) const = default;
};

// The three clocks are independent. DocumentMutation alone carries a complete
// adjacent source chain. SceneActivation and SceneRootState each advance only
// their own checked clock and carry no document mutation token. Optional scene
// identity is the only representation of the no-active-scene state.
struct SceneChangeHeader final {
  std::uint32_t schemaVersion{kSceneChangeSchemaVersion};
  SceneChangeDomain domain{SceneChangeDomain::DocumentMutation};
  EditorDocumentAuthority authority{};
  std::uint64_t baseDocumentRevision{0};
  std::uint64_t targetDocumentRevision{0};
  std::uint64_t baseSceneActivationEpoch{0};
  std::uint64_t targetSceneActivationEpoch{0};
  std::uint64_t baseRootExternalStateSerial{0};
  std::uint64_t targetRootExternalStateSerial{0};
  std::optional<StableSceneId> baseActiveScene;
  std::optional<StableSceneId> targetActiveScene;
  std::optional<SceneMutationToken> mutationToken;
  std::uint64_t firstMutationSerial{0};
  std::uint64_t lastMutationSerial{0};
  SceneDigest baseDocumentDigest{};
  SceneDigest targetDocumentDigest{};
  SceneDigest baseProjectionDigest{};
  SceneDigest targetProjectionDigest{};
  SceneDigest canonicalChangeSetDigest{};
  std::vector<SourceMutationFence> sourceMutations;

  bool operator==(const SceneChangeHeader &) const = default;
};

// Create is absent/complete, Update is complete/complete, and Delete is
// complete/absent for beforeValue/afterValue. Values own all variable storage.
struct SceneFieldChange final {
  StableSceneId target{};
  SceneChangeClass changeClass{SceneChangeClass::Transform};
  SceneChangeAction action{SceneChangeAction::Update};
  SceneField field{SceneField::NodeLocalTransform};
  std::uint64_t mutationSerial{0};
  std::optional<SceneFieldValue> beforeValue;
  std::optional<SceneFieldValue> afterValue;

  bool operator==(const SceneFieldChange &) const = default;
};

// Missing oldId is create, missing newId is delete, and both present is remap.
// Both missing is invalid. Canonical order is the stable scene-key order.
struct StableIdRemapEvidence final {
  std::optional<StableSceneId> oldId;
  std::optional<StableSceneId> newId;

  bool operator==(const StableIdRemapEvidence &) const = default;
};

enum class FullRebuildReason : std::uint8_t {
  SceneSwitch = 0,
  GlobalMerge = 1,
  Compact = 2,
  StructuralTopology = 3,
  GeometrySchema = 4,
  TextureSchema = 5,
  AccelerationStructureTopology = 6,
  MappingMismatch = 7,
  MissedRevision = 8,
  DeviceRecovery = 9,
  UnsupportedIncrementalCapability = 10,
  ExplicitCallerRequest = 11,
};

enum class ImmutableSceneSnapshotEncoding : std::uint8_t {
  CanonicalBinary = 0,
};

struct ImmutableSceneSnapshotRef final {
  std::uint32_t schemaVersion{kImmutableSceneSnapshotSchemaVersion};
  ImmutableSceneSnapshotEncoding encoding{
      ImmutableSceneSnapshotEncoding::CanonicalBinary};
  EditorDocumentAuthority authority{};
  std::uint64_t documentRevision{0};
  std::uint64_t sceneActivationEpoch{0};
  std::uint64_t rootExternalStateSerial{0};
  std::optional<StableSceneId> activeScene;
  SceneDigest documentDigest{};
  SceneDigest projectionDigest{};
  SceneDigest contentDigest{};
  ImmutableByteBuffer bytes;

  bool operator==(const ImmutableSceneSnapshotRef &) const = default;
};

// The byte owner covers deferred consumption. Its digest and every target
// envelope identity are verified before a FullSnapshot can be canonicalized.
struct FullSnapshotReference final {
  FullRebuildReason reason{FullRebuildReason::ExplicitCallerRequest};
  SceneDigest targetSnapshotDigest{};
  ImmutableSceneSnapshotRef immutableTarget;
  std::vector<StableIdRemapEvidence> stableIdRemap;
  bool stableIdRemapComplete{false};

  bool operator==(const FullSnapshotReference &) const = default;
};

struct SceneMutationEvidence final {
  SourceMutationFence sourceMutation;
  std::vector<SceneFieldChange> exactChanges;
  std::vector<StableIdRemapEvidence> stableIdRemap;
  bool structuralEvidenceComplete{true};
  std::optional<FullSnapshotReference> fullSnapshot;

  bool operator==(const SceneMutationEvidence &) const = default;
};

struct SceneChangeSet final {
  SceneChangeHeader header;
  std::vector<SceneFieldChange> canonicalChanges;
  std::vector<StableIdRemapEvidence> stableIdRemap;
  std::optional<FullSnapshotReference> fullSnapshot;

  bool operator==(const SceneChangeSet &) const = default;
};

} // namespace demo::editor
