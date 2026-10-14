#include <mgif/scene_editor/SceneChangeCanonicalizer.h>
#include <mgif/scene_editor/detail/BitwiseCast.h>

#include <algorithm>
#include <array>
#include <bit>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <limits>
#include <map>
#include <set>
#include <stdexcept>
#include <string>
#include <tuple>
#include <type_traits>
#include <utility>

#if defined(_WIN32)
#ifndef NOMINMAX
#define NOMINMAX
#endif
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#include <bcrypt.h>
#include <wincrypt.h>
#endif

namespace demo::editor {
namespace {

#if defined(_WIN32)
// Full projection snapshots include texture bytes. Use the platform SHA-256
// implementation so Debug builds do not hash those bytes in an unoptimized
// C++ compression loop on every editor preview. The digest format is unchanged.
BCRYPT_ALG_HANDLE nativeSha256Algorithm() noexcept {
  struct Algorithm final {
    BCRYPT_ALG_HANDLE handle{nullptr};
    Algorithm() noexcept {
      if (BCryptOpenAlgorithmProvider(&handle, BCRYPT_SHA256_ALGORITHM,
                                      nullptr, 0) < 0)
        handle = nullptr;
    }
    ~Algorithm() { if (handle) BCryptCloseAlgorithmProvider(handle, 0); }
  };
  static const Algorithm algorithm;
  return algorithm.handle;
}

struct NativeHash final {
  NativeHash() = default;
  NativeHash(const NativeHash&) = delete;
  NativeHash& operator=(const NativeHash&) = delete;
  BCRYPT_HASH_HANDLE handle{nullptr};
  ~NativeHash() { if (handle) BCryptDestroyHash(handle); }
};

bool appendNativeSha256(BCRYPT_HASH_HANDLE handle,
                       std::span<const std::span<const std::uint8_t>> parts) noexcept {
  for (auto bytes : parts) {
    while (!bytes.empty()) {
      const auto count = static_cast<ULONG>(
          std::min(bytes.size(), std::size_t{16 * 1024 * 1024}));
      if (BCryptHashData(handle, const_cast<PUCHAR>(bytes.data()), count, 0) < 0)
        return false;
      bytes = bytes.subspan(count);
    }
  }
  return true;
}

bool nativeSha256(std::span<const std::span<const std::uint8_t>> parts,
                  SceneDigest& digest) noexcept {
  const auto algorithm = nativeSha256Algorithm();
  if (!algorithm) return false;
  NativeHash hash;
  if (BCryptCreateHash(algorithm, &hash.handle, nullptr, 0, nullptr, 0, 0) < 0 ||
      !appendNativeSha256(hash.handle, parts)) return false;
  return BCryptFinishHash(hash.handle, digest.data(),
                          static_cast<ULONG>(digest.size()), 0) >= 0;
}
#endif

constexpr double kPi = 3.14159265358979323846264338327950288;

SceneChangeValidation
failure(SceneChangeValidationError error, std::string message,
        std::size_t changeIndex = SceneChangeValidation::kNoIndex,
        std::size_t sourceIndex = SceneChangeValidation::kNoIndex) {
  return SceneChangeValidation{error, changeIndex, sourceIndex,
                               std::move(message)};
}

SceneCanonicalizationResult failedResult(SceneChangeValidation validation) {
  SceneCanonicalizationResult result;
  result.validation = std::move(validation);
  return result;
}

template <typename Enum> constexpr auto enumValue(Enum value) noexcept {
  return static_cast<std::underlying_type_t<Enum>>(value);
}

template <typename Enum>
constexpr bool enumAtMost(Enum value, Enum maximum) noexcept {
  return enumValue(value) <= enumValue(maximum);
}

bool checkedIncrement(std::uint64_t base, std::uint64_t target) noexcept {
  return base != std::numeric_limits<std::uint64_t>::max() &&
         target == base + 1u;
}

bool finite(double value) noexcept { return std::isfinite(value); }

template <std::size_t Size>
bool allFinite(const std::array<double, Size> &values) noexcept {
  return std::all_of(values.begin(), values.end(),
                     [](double value) { return finite(value); });
}

bool inUnitRange(double value) noexcept {
  return finite(value) && value >= 0.0 && value <= 1.0;
}

bool exactDoubleEqual(double left, double right) noexcept {
  return detail::bitwiseCast<std::uint64_t>(left) ==
         detail::bitwiseCast<std::uint64_t>(right);
}

template <std::size_t Size>
bool exactDoubleArrayEqual(const std::array<double, Size> &left,
                           const std::array<double, Size> &right) noexcept {
  for (std::size_t index = 0; index < Size; ++index) {
    if (!exactDoubleEqual(left[index], right[index])) {
      return false;
    }
  }
  return true;
}

bool exactValueEqual(const SceneFieldValue &left,
                     const SceneFieldValue &right) {
  if (left.index() != right.index()) {
    return false;
  }
  return std::visit(
      [&right](const auto &leftValue) {
        using Value = std::decay_t<decltype(leftValue)>;
        const Value &rightValue = std::get<Value>(right);
        if constexpr (std::is_same_v<Value, SceneNodeTransformValue>) {
          if (leftValue.value.index() != rightValue.value.index()) {
            return false;
          }
          if (std::holds_alternative<SceneTrsValue>(leftValue.value)) {
            const auto &leftTrs = std::get<SceneTrsValue>(leftValue.value);
            const auto &rightTrs = std::get<SceneTrsValue>(rightValue.value);
            return exactDoubleArrayEqual(leftTrs.translation,
                                         rightTrs.translation) &&
                   exactDoubleArrayEqual(leftTrs.rotation, rightTrs.rotation) &&
                   exactDoubleArrayEqual(leftTrs.scale, rightTrs.scale);
          }
          return exactDoubleArrayEqual(
              std::get<SceneMatrixValue>(leftValue.value).columnMajor,
              std::get<SceneMatrixValue>(rightValue.value).columnMajor);
        } else if constexpr (std::is_same_v<Value,
                                            SceneMaterialParametersValue>) {
          return exactDoubleArrayEqual(leftValue.baseColorFactor,
                                       rightValue.baseColorFactor) &&
                 exactDoubleEqual(leftValue.metallicFactor,
                                  rightValue.metallicFactor) &&
                 exactDoubleEqual(leftValue.roughnessFactor,
                                  rightValue.roughnessFactor) &&
                 exactDoubleArrayEqual(leftValue.emissiveFactor,
                                       rightValue.emissiveFactor) &&
                 exactDoubleEqual(leftValue.alphaCutoff,
                                  rightValue.alphaCutoff) &&
                 leftValue.alphaMode == rightValue.alphaMode &&
                 leftValue.doubleSided == rightValue.doubleSided;
        } else if constexpr (std::is_same_v<Value, SceneTextureBindingValue>) {
          return leftValue.semantic == rightValue.semantic &&
                 leftValue.texture == rightValue.texture &&
                 leftValue.sampler == rightValue.sampler &&
                 leftValue.uvSet == rightValue.uvSet &&
                 exactDoubleArrayEqual(leftValue.uvOffset,
                                       rightValue.uvOffset) &&
                 exactDoubleArrayEqual(leftValue.uvScale, rightValue.uvScale) &&
                 exactDoubleEqual(leftValue.uvRotation,
                                  rightValue.uvRotation) &&
                 exactDoubleEqual(leftValue.scalar, rightValue.scalar);
        } else if constexpr (std::is_same_v<Value, SceneCameraValue>) {
          return leftValue.projection == rightValue.projection &&
                 exactDoubleEqual(leftValue.yfov, rightValue.yfov) &&
                 exactDoubleEqual(leftValue.znear, rightValue.znear) &&
                 exactDoubleEqual(leftValue.zfar, rightValue.zfar) &&
                 exactDoubleEqual(leftValue.aspectRatio,
                                  rightValue.aspectRatio) &&
                 exactDoubleEqual(leftValue.xmag, rightValue.xmag) &&
                 exactDoubleEqual(leftValue.ymag, rightValue.ymag);
        } else if constexpr (std::is_same_v<Value, SceneLightValue>) {
          return leftValue.type == rightValue.type &&
                 exactDoubleArrayEqual(leftValue.color, rightValue.color) &&
                 exactDoubleEqual(leftValue.intensity, rightValue.intensity) &&
                 exactDoubleEqual(leftValue.range, rightValue.range) &&
                 exactDoubleEqual(leftValue.innerConeAngle,
                                  rightValue.innerConeAngle) &&
                 exactDoubleEqual(leftValue.outerConeAngle,
                                  rightValue.outerConeAngle);
        } else {
          return leftValue == rightValue;
        }
      },
      left);
}

bool exactOptionalValueEqual(const std::optional<SceneFieldValue> &left,
                             const std::optional<SceneFieldValue> &right) {
  return left.has_value() == right.has_value() &&
         (!left || exactValueEqual(*left, *right));
}

bool exactChangeEqual(const SceneFieldChange &left,
                      const SceneFieldChange &right) {
  return left.target == right.target && left.changeClass == right.changeClass &&
         left.action == right.action && left.field == right.field &&
         left.mutationSerial == right.mutationSerial &&
         exactOptionalValueEqual(left.beforeValue, right.beforeValue) &&
         exactOptionalValueEqual(left.afterValue, right.afterValue);
}

bool validKind(EditorObjectKind kind) noexcept {
  return kind > EditorObjectKind::invalid && kind < EditorObjectKind::count;
}

bool validActiveScene(const std::optional<StableSceneId> &scene,
                      EditorDocumentAuthority authority) noexcept {
  return !scene || (scene->valid() && scene->kind == EditorObjectKind::scene &&
                    scene->authority == authority);
}

bool optionalIdValidFor(const std::optional<StableSceneId> &id,
                        EditorDocumentAuthority authority,
                        EditorObjectKind kind) noexcept {
  return !id || (id->valid() && id->authority == authority && id->kind == kind);
}

SceneChangeClass expectedClass(SceneField field) noexcept {
  switch (field) {
  case SceneField::NodeLocalTransform:
  case SceneField::SceneRootTransform:
    return SceneChangeClass::Transform;
  case SceneField::NodeLocalVisibility:
    return SceneChangeClass::Visibility;
  case SceneField::MaterialParameters:
  case SceneField::MaterialExtension:
    return SceneChangeClass::MaterialParameters;
  case SceneField::MaterialTextureBinding:
  case SceneField::PrimitiveMaterialBinding:
    return SceneChangeClass::MaterialBinding;
  case SceneField::CameraParameters:
  case SceneField::CameraAttachment:
    return SceneChangeClass::Camera;
  case SceneField::LightParameters:
  case SceneField::LightAttachment:
    return SceneChangeClass::Light;
  case SceneField::HierarchyParent:
  case SceneField::HierarchyAttachment:
    return SceneChangeClass::Hierarchy;
  case SceneField::TopologyMetadata:
  case SceneField::GeometryBufferRange:
  case SceneField::TexturePayload:
    return SceneChangeClass::Topology;
  }
  return SceneChangeClass::FullRebuild;
}

bool targetKindMatchesField(EditorObjectKind kind, SceneField field) noexcept {
  switch (field) {
  case SceneField::NodeLocalTransform:
  case SceneField::NodeLocalVisibility:
  case SceneField::HierarchyParent:
  case SceneField::HierarchyAttachment:
    return kind == EditorObjectKind::node;
  case SceneField::SceneRootTransform:
    return kind == EditorObjectKind::scene;
  case SceneField::MaterialParameters:
  case SceneField::MaterialExtension:
  case SceneField::MaterialTextureBinding:
    return kind == EditorObjectKind::material;
  case SceneField::PrimitiveMaterialBinding:
    return kind == EditorObjectKind::primitive;
  case SceneField::CameraParameters:
  case SceneField::CameraAttachment:
    return kind == EditorObjectKind::camera;
  case SceneField::LightParameters:
  case SceneField::LightAttachment:
    return kind == EditorObjectKind::light;
  case SceneField::GeometryBufferRange:
    return kind == EditorObjectKind::buffer ||
           kind == EditorObjectKind::bufferView ||
           kind == EditorObjectKind::accessor ||
           kind == EditorObjectKind::mesh ||
           kind == EditorObjectKind::primitive;
  case SceneField::TexturePayload:
    return kind == EditorObjectKind::image || kind == EditorObjectKind::texture;
  case SceneField::TopologyMetadata:
    return validKind(kind);
  }
  return false;
}

SceneChangeValidation validateSourceChain(const SceneChangeHeader &header,
                                          bool required) {
  if (header.sourceMutations.empty()) {
    if (required) {
      return failure(
          SceneChangeValidationError::InvalidSourceMutationChain,
          "document mutation requires a complete source-mutation chain");
    }
    return {};
  }

  if (header.targetDocumentRevision < header.baseDocumentRevision) {
    return failure(
        header.baseDocumentRevision ==
                    std::numeric_limits<std::uint64_t>::max() &&
                header.targetDocumentRevision == 0u
            ? SceneChangeValidationError::MonotonicOverflow
            : SceneChangeValidationError::InvalidSourceMutationChain,
        "source-mutation chain moves the document revision backwards");
  }
  const std::uint64_t revisionDistance =
      header.targetDocumentRevision - header.baseDocumentRevision;
  if (revisionDistance !=
      static_cast<std::uint64_t>(header.sourceMutations.size())) {
    return failure(
        SceneChangeValidationError::InvalidSourceMutationChain,
        "source-mutation chain does not contain every adjacent revision");
  }

  std::uint64_t expectedRevision = header.baseDocumentRevision;
  SceneDigest expectedDigest = header.baseDocumentDigest;
  std::uint64_t previousSerial = 0u;
  for (std::size_t index = 0; index < header.sourceMutations.size(); ++index) {
    const SourceMutationFence &source = header.sourceMutations[index];
    if (source.baseDocumentRevision != expectedRevision ||
        source.baseDocumentDigest != expectedDigest) {
      return failure(SceneChangeValidationError::InvalidSourceMutationChain,
                     "source-mutation revision or digest is discontinuous",
                     SceneChangeValidation::kNoIndex, index);
    }
    if (!checkedIncrement(source.baseDocumentRevision,
                          source.targetDocumentRevision)) {
      return failure(
          source.baseDocumentRevision ==
                  std::numeric_limits<std::uint64_t>::max()
              ? SceneChangeValidationError::MonotonicOverflow
              : SceneChangeValidationError::InvalidSourceMutationChain,
          "source-mutation revision is not a checked adjacent increment",
          SceneChangeValidation::kNoIndex, index);
    }
    if (index != 0u &&
        previousSerial == std::numeric_limits<std::uint64_t>::max()) {
      return failure(SceneChangeValidationError::MonotonicOverflow,
                     "source-mutation token serial overflows",
                     SceneChangeValidation::kNoIndex, index);
    }
    if (!source.mutationToken.valid() ||
        source.mutationToken.authority != header.authority ||
        source.mutationToken.revision != source.targetDocumentRevision) {
      return failure(SceneChangeValidationError::InvalidMutationToken,
                     "source-mutation token does not bind its target revision "
                     "and authority",
                     SceneChangeValidation::kNoIndex, index);
    }
    if (!sceneDigestPresent(source.baseDocumentDigest) ||
        !sceneDigestPresent(source.targetDocumentDigest)) {
      return failure(SceneChangeValidationError::InvalidContentHash,
                     "source-mutation document digest is absent",
                     SceneChangeValidation::kNoIndex, index);
    }
    if (index != 0u) {
      if (source.mutationToken.serial <= previousSerial) {
        return failure(SceneChangeValidationError::InvalidSourceMutationChain,
                       "source-mutation token serial is not strictly ordered",
                       SceneChangeValidation::kNoIndex, index);
      }
    }
    previousSerial = source.mutationToken.serial;
    expectedRevision = source.targetDocumentRevision;
    expectedDigest = source.targetDocumentDigest;
  }

  if (expectedRevision != header.targetDocumentRevision ||
      expectedDigest != header.targetDocumentDigest) {
    return failure(SceneChangeValidationError::InvalidSourceMutationChain,
                   "source-mutation chain does not end at the header target");
  }
  if (header.firstMutationSerial !=
          header.sourceMutations.front().mutationToken.serial ||
      header.lastMutationSerial !=
          header.sourceMutations.back().mutationToken.serial) {
    return failure(SceneChangeValidationError::InvalidMutationSerial,
                   "header mutation serial range does not match the complete "
                   "source chain");
  }
  return {};
}

SceneChangeValidation validateHeader(const SceneChangeHeader &header) {
  if (header.schemaVersion != kSceneChangeSchemaVersion) {
    return failure(SceneChangeValidationError::InvalidSchema,
                   "scene change schema version is unsupported");
  }
  if (!enumAtMost(header.domain, SceneChangeDomain::FullSnapshot)) {
    return failure(SceneChangeValidationError::InvalidDomain,
                   "scene change domain is invalid");
  }
  if (!header.authority.valid()) {
    return failure(SceneChangeValidationError::InvalidAuthority,
                   "scene change authority is invalid");
  }
  if (!validActiveScene(header.baseActiveScene, header.authority) ||
      !validActiveScene(header.targetActiveScene, header.authority)) {
    return failure(SceneChangeValidationError::InvalidActiveScene,
                   "active scene is not an optional valid scene identity for "
                   "this authority");
  }
  const bool zeroSerialFullSnapshot =
      header.domain == SceneChangeDomain::FullSnapshot &&
      header.firstMutationSerial == 0u && header.lastMutationSerial == 0u &&
      header.sourceMutations.empty() && !header.mutationToken;
  if ((!zeroSerialFullSnapshot && header.firstMutationSerial == 0u) ||
      header.lastMutationSerial < header.firstMutationSerial) {
    return failure(SceneChangeValidationError::InvalidMutationSerial,
                   "mutation serial range is empty or reversed");
  }
  if (!sceneDigestPresent(header.baseDocumentDigest) ||
      !sceneDigestPresent(header.targetDocumentDigest) ||
      !sceneDigestPresent(header.baseProjectionDigest) ||
      !sceneDigestPresent(header.targetProjectionDigest)) {
    return failure(SceneChangeValidationError::InvalidContentHash,
                   "document and projection digests are mandatory");
  }
  if (header.mutationToken) {
    if (!header.mutationToken->valid() ||
        header.mutationToken->authority != header.authority ||
        header.mutationToken->revision != header.targetDocumentRevision) {
      return failure(SceneChangeValidationError::InvalidMutationToken,
                     "header mutation token does not bind the target revision "
                     "and authority");
    }
  }

  switch (header.domain) {
  case SceneChangeDomain::DocumentMutation: {
    if (header.baseSceneActivationEpoch != header.targetSceneActivationEpoch ||
        header.baseActiveScene != header.targetActiveScene) {
      return failure(
          SceneChangeValidationError::InvalidEnvelope,
          "document mutation must not change scene activation state");
    }
    if (header.baseRootExternalStateSerial !=
            header.targetRootExternalStateSerial &&
        !checkedIncrement(header.baseRootExternalStateSerial,
                          header.targetRootExternalStateSerial)) {
      return failure(header.baseRootExternalStateSerial ==
                             std::numeric_limits<std::uint64_t>::max()
                         ? SceneChangeValidationError::MonotonicOverflow
                         : SceneChangeValidationError::InvalidEnvelope,
                     "document mutation root-state transition must be "
                     "unchanged or one checked step");
    }
    const SceneChangeValidation chain = validateSourceChain(header, true);
    if (!chain.valid()) {
      return chain;
    }
    if (header.sourceMutations.size() == 1u) {
      if (!header.mutationToken ||
          *header.mutationToken !=
              header.sourceMutations.front().mutationToken) {
        return failure(
            SceneChangeValidationError::InvalidMutationToken,
            "unbatched document mutation requires its exact header token");
      }
    } else if (header.mutationToken &&
               *header.mutationToken !=
                   header.sourceMutations.back().mutationToken) {
      return failure(
          SceneChangeValidationError::InvalidMutationToken,
          "batched header token, when present, must be the final source token");
    }
    break;
  }
  case SceneChangeDomain::SceneActivation:
    if (header.baseDocumentRevision != header.targetDocumentRevision ||
        header.baseDocumentDigest != header.targetDocumentDigest ||
        header.baseRootExternalStateSerial !=
            header.targetRootExternalStateSerial ||
        !checkedIncrement(header.baseSceneActivationEpoch,
                          header.targetSceneActivationEpoch) ||
        header.baseActiveScene == header.targetActiveScene ||
        header.mutationToken || !header.sourceMutations.empty() ||
        header.firstMutationSerial != header.lastMutationSerial) {
      return failure(
          header.baseSceneActivationEpoch ==
                  std::numeric_limits<std::uint64_t>::max()
              ? SceneChangeValidationError::MonotonicOverflow
              : SceneChangeValidationError::InvalidEnvelope,
          "scene activation envelope is not one isolated checked transition");
    }
    break;
  case SceneChangeDomain::SceneRootState:
    if (header.baseDocumentRevision != header.targetDocumentRevision ||
        header.baseDocumentDigest != header.targetDocumentDigest ||
        header.baseSceneActivationEpoch != header.targetSceneActivationEpoch ||
        header.baseActiveScene != header.targetActiveScene ||
        !checkedIncrement(header.baseRootExternalStateSerial,
                          header.targetRootExternalStateSerial) ||
        header.mutationToken || !header.sourceMutations.empty() ||
        header.firstMutationSerial != header.lastMutationSerial) {
      return failure(
          header.baseRootExternalStateSerial ==
                  std::numeric_limits<std::uint64_t>::max()
              ? SceneChangeValidationError::MonotonicOverflow
              : SceneChangeValidationError::InvalidEnvelope,
          "scene root-state envelope is not one isolated checked transition");
    }
    break;
  case SceneChangeDomain::FullSnapshot:
    if (header.targetDocumentRevision < header.baseDocumentRevision ||
        header.targetSceneActivationEpoch < header.baseSceneActivationEpoch ||
        header.targetRootExternalStateSerial <
            header.baseRootExternalStateSerial) {
      return failure(
          SceneChangeValidationError::InvalidEnvelope,
          "full snapshot target envelope moves a monotonic identity backwards");
    }
    if (header.mutationToken && header.sourceMutations.empty()) {
      return failure(SceneChangeValidationError::InvalidMutationToken,
                     "full snapshot mutation token requires its complete "
                     "source chain");
    }
    if (!header.sourceMutations.empty()) {
      const SceneChangeValidation chain = validateSourceChain(header, false);
      if (!chain.valid()) {
        return chain;
      }
      if (header.mutationToken &&
          *header.mutationToken !=
              header.sourceMutations.back().mutationToken) {
        return failure(SceneChangeValidationError::InvalidMutationToken,
                       "full snapshot header token does not match its final "
                       "source mutation");
      }
    }
    break;
  }
  return {};
}

SceneChangeValidation validateValue(SceneField field,
                                    const SceneFieldValue &value,
                                    EditorDocumentAuthority authority,
                                    std::size_t changeIndex) {
  if (!sceneFieldValueMatchesField(field, value)) {
    return failure(SceneChangeValidationError::InvalidFieldValue,
                   "field value tag does not match its field", changeIndex);
  }

  const auto references = sceneFieldValueReferences(value);
  for (const StableSceneId reference : references) {
    if (!reference.valid()) {
      return failure(SceneChangeValidationError::InvalidStableId,
                     "field value contains an invalid stable identity",
                     changeIndex);
    }
    if (reference.authority != authority) {
      return failure(SceneChangeValidationError::CrossAuthority,
                     "field value contains a cross-authority identity",
                     changeIndex);
    }
  }

  return std::visit(
      [&](const auto &typedValue) -> SceneChangeValidation {
        using Value = std::decay_t<decltype(typedValue)>;
        if constexpr (std::is_same_v<Value, SceneNodeTransformValue>) {
          if (std::holds_alternative<SceneTrsValue>(typedValue.value)) {
            const SceneTrsValue &trs =
                std::get<SceneTrsValue>(typedValue.value);
            const double quaternionLengthSquared =
                trs.rotation[0] * trs.rotation[0] +
                trs.rotation[1] * trs.rotation[1] +
                trs.rotation[2] * trs.rotation[2] +
                trs.rotation[3] * trs.rotation[3];
            if (!allFinite(trs.translation) || !allFinite(trs.rotation) ||
                !allFinite(trs.scale) || quaternionLengthSquared <= 0.0) {
              return failure(SceneChangeValidationError::InvalidRange,
                             "TRS value is non-finite or has a zero quaternion",
                             changeIndex);
            }
          } else if (!allFinite(std::get<SceneMatrixValue>(typedValue.value)
                                    .columnMajor)) {
            return failure(SceneChangeValidationError::InvalidRange,
                           "matrix value is non-finite", changeIndex);
          }
        } else if constexpr (std::is_same_v<Value,
                                            SceneMaterialParametersValue>) {
          if (!std::all_of(typedValue.baseColorFactor.begin(),
                           typedValue.baseColorFactor.end(), inUnitRange) ||
              !inUnitRange(typedValue.metallicFactor) ||
              !inUnitRange(typedValue.roughnessFactor) ||
              !std::all_of(typedValue.emissiveFactor.begin(),
                           typedValue.emissiveFactor.end(),
                           [](double component) {
                             return finite(component) && component >= 0.0;
                           }) ||
              !inUnitRange(typedValue.alphaCutoff) ||
              !enumAtMost(typedValue.alphaMode, SceneAlphaMode::Blend)) {
            return failure(
                SceneChangeValidationError::InvalidRange,
                "material parameters are outside the frozen finite ranges",
                changeIndex);
          }
        } else if constexpr (std::is_same_v<Value,
                                            SceneMaterialExtensionValue>) {
          if (typedValue.extensionName.empty() ||
              typedValue.extensionName.size() > 1024u ||
              typedValue.canonicalValueBytes.empty() ||
              typedValue.canonicalValueBytes.size() >
                  kMaximumSceneChangePayloadBytes) {
            return failure(
                SceneChangeValidationError::InvalidPayload,
                "material extension payload is empty or exceeds its bound",
                changeIndex);
          }
          if (!sceneDigestPresent(typedValue.contentDigest) ||
              SceneChangeCanonicalizer::computeSha256(
                  typedValue.canonicalValueBytes) != typedValue.contentDigest) {
            return failure(SceneChangeValidationError::InvalidContentHash,
                           "material extension content digest does not match",
                           changeIndex);
          }
        } else if constexpr (std::is_same_v<Value, SceneTextureBindingValue>) {
          if (!enumAtMost(typedValue.semantic,
                          SceneTextureSemantic::Emissive) ||
              !optionalIdValidFor(typedValue.texture, authority,
                                  EditorObjectKind::texture) ||
              !optionalIdValidFor(typedValue.sampler, authority,
                                  EditorObjectKind::sampler) ||
              (typedValue.sampler && !typedValue.texture) ||
              typedValue.uvSet > 15u || !allFinite(typedValue.uvOffset) ||
              !allFinite(typedValue.uvScale) ||
              !finite(typedValue.uvRotation) || !finite(typedValue.scalar) ||
              typedValue.scalar < 0.0) {
            return failure(SceneChangeValidationError::InvalidRange,
                           "texture binding identity, UV, or scalar is invalid",
                           changeIndex);
          }
        } else if constexpr (std::is_same_v<
                                 Value, ScenePrimitiveMaterialBindingValue>) {
          if (!optionalIdValidFor(typedValue.material, authority,
                                  EditorObjectKind::material)) {
            return failure(SceneChangeValidationError::InvalidStableId,
                           "primitive material binding is invalid",
                           changeIndex);
          }
        } else if constexpr (std::is_same_v<Value, SceneCameraValue>) {
          const bool common =
              enumAtMost(typedValue.projection,
                         SceneCameraProjection::Orthographic) &&
              finite(typedValue.znear) && typedValue.znear > 0.0 &&
              finite(typedValue.zfar) &&
              (typedValue.zfar == 0.0 || typedValue.zfar > typedValue.znear);
          const bool projectionValid =
              typedValue.projection == SceneCameraProjection::Perspective
                  ? finite(typedValue.yfov) && typedValue.yfov > 0.0 &&
                        typedValue.yfov < kPi &&
                        finite(typedValue.aspectRatio) &&
                        typedValue.aspectRatio >= 0.0
                  : finite(typedValue.xmag) && typedValue.xmag > 0.0 &&
                        finite(typedValue.ymag) && typedValue.ymag > 0.0;
          if (!common || !projectionValid) {
            return failure(SceneChangeValidationError::InvalidRange,
                           "camera parameters are invalid", changeIndex);
          }
        } else if constexpr (std::is_same_v<Value,
                                            SceneObjectAttachmentValue>) {
          if (!optionalIdValidFor(typedValue.node, authority,
                                  EditorObjectKind::node)) {
            return failure(SceneChangeValidationError::InvalidStableId,
                           "attachment node identity is invalid", changeIndex);
          }
        } else if constexpr (std::is_same_v<Value, SceneLightValue>) {
          if (!enumAtMost(typedValue.type, SceneLightType::Spot) ||
              !std::all_of(typedValue.color.begin(), typedValue.color.end(),
                           [](double component) {
                             return finite(component) && component >= 0.0;
                           }) ||
              !finite(typedValue.intensity) || typedValue.intensity < 0.0 ||
              !finite(typedValue.range) || typedValue.range < 0.0 ||
              !finite(typedValue.innerConeAngle) ||
              !finite(typedValue.outerConeAngle) ||
              typedValue.innerConeAngle < 0.0 ||
              typedValue.outerConeAngle < typedValue.innerConeAngle ||
              !(typedValue.outerConeAngle <= kPi * 0.5)) {
            return failure(SceneChangeValidationError::InvalidRange,
                           "light parameters are invalid", changeIndex);
          }
        } else if constexpr (std::is_same_v<Value, SceneHierarchyValue>) {
          if (!optionalIdValidFor(typedValue.parent, authority,
                                  EditorObjectKind::node)) {
            return failure(SceneChangeValidationError::InvalidStableId,
                           "hierarchy parent identity is invalid", changeIndex);
          }
        } else if constexpr (std::is_same_v<Value, SceneTopologyValue>) {
          if (!enumAtMost(typedValue.topologyKind,
                          SceneTopologyKind::Texture) ||
              !validKind(typedValue.objectKind) ||
              !sceneDigestPresent(typedValue.schemaDigest)) {
            return failure(SceneChangeValidationError::InvalidPayload,
                           "topology metadata is incomplete", changeIndex);
          }
        } else if constexpr (std::is_same_v<Value, SceneBufferRangeValue>) {
          if (!enumAtMost(typedValue.semantic, SceneBufferSemantic::Skin) ||
              typedValue.byteSize != typedValue.bytes.size() ||
              typedValue.bytes.empty() ||
              typedValue.bytes.size() > kMaximumSceneChangePayloadBytes) {
            return failure(SceneChangeValidationError::InvalidPayload,
                           "buffer range byte size is invalid", changeIndex);
          }
          if (typedValue.logicalOffset >
              std::numeric_limits<std::uint64_t>::max() - typedValue.byteSize) {
            return failure(SceneChangeValidationError::InvalidRange,
                           "buffer range overflows", changeIndex);
          }
          if (!sceneDigestPresent(typedValue.schemaDigest) ||
              !sceneDigestPresent(typedValue.contentDigest) ||
              SceneChangeCanonicalizer::computeSha256(typedValue.bytes) !=
                  typedValue.contentDigest) {
            return failure(SceneChangeValidationError::InvalidContentHash,
                           "buffer range hash is invalid", changeIndex);
          }
        } else if constexpr (std::is_same_v<Value, SceneTexturePayloadValue>) {
          if (typedValue.width == 0u || typedValue.height == 0u ||
              typedValue.depth == 0u || typedValue.mipCount == 0u ||
              typedValue.layerCount == 0u || typedValue.formatTag == 0u ||
              !enumAtMost(typedValue.colorSpace, SceneColorSpace::Srgb) ||
              typedValue.bytes.empty() ||
              typedValue.bytes.size() > kMaximumSceneChangePayloadBytes) {
            return failure(SceneChangeValidationError::InvalidPayload,
                           "texture payload schema or byte range is invalid",
                           changeIndex);
          }
          if (!sceneDigestPresent(typedValue.schemaDigest) ||
              !sceneDigestPresent(typedValue.contentDigest) ||
              SceneChangeCanonicalizer::computeSha256(typedValue.bytes) !=
                  typedValue.contentDigest) {
            return failure(SceneChangeValidationError::InvalidContentHash,
                           "texture payload hash is invalid", changeIndex);
          }
        }
        return {};
      },
      value);
}

bool serialBelongsToSource(const SceneChangeHeader &header,
                           std::uint64_t serial) noexcept {
  return std::any_of(header.sourceMutations.begin(),
                     header.sourceMutations.end(),
                     [serial](const SourceMutationFence &source) {
                       return source.mutationToken.serial == serial;
                     });
}

SceneChangeValidation validateChange(const SceneChangeHeader &header,
                                     const SceneFieldChange &change,
                                     std::size_t index) {
  if (!change.target.valid()) {
    return failure(SceneChangeValidationError::InvalidStableId,
                   "change target is invalid", index);
  }
  if (change.target.authority != header.authority) {
    return failure(SceneChangeValidationError::CrossAuthority,
                   "change target has a different authority", index);
  }
  if (!enumAtMost(change.changeClass, SceneChangeClass::FullRebuild) ||
      !enumAtMost(change.action, SceneChangeAction::Delete) ||
      !enumAtMost(change.field, SceneField::SceneRootTransform)) {
    return failure(SceneChangeValidationError::InvalidFieldValue,
                   "change enum value is invalid", index);
  }
  if (change.changeClass != expectedClass(change.field) ||
      change.changeClass == SceneChangeClass::FullRebuild ||
      !targetKindMatchesField(change.target.kind, change.field)) {
    return failure(SceneChangeValidationError::FieldClassMismatch,
                   "change class, field, and target kind do not match", index);
  }
  if (change.mutationSerial < header.firstMutationSerial ||
      change.mutationSerial > header.lastMutationSerial ||
      change.mutationSerial == 0u ||
      (header.domain == SceneChangeDomain::DocumentMutation &&
       !serialBelongsToSource(header, change.mutationSerial))) {
    return failure(SceneChangeValidationError::InvalidMutationSerial,
                   "change mutation serial is outside the exact source range",
                   index);
  }

  const bool presenceValid = (change.action == SceneChangeAction::Create &&
                              !change.beforeValue && change.afterValue) ||
                             (change.action == SceneChangeAction::Update &&
                              change.beforeValue && change.afterValue) ||
                             (change.action == SceneChangeAction::Delete &&
                              change.beforeValue && !change.afterValue);
  if (!presenceValid) {
    return failure(SceneChangeValidationError::InvalidActionValues,
                   "Create, Update, or Delete before/after presence is invalid",
                   index);
  }
  if (change.beforeValue) {
    const SceneChangeValidation validation = validateValue(
        change.field, *change.beforeValue, header.authority, index);
    if (!validation.valid()) {
      return validation;
    }
  }
  if (change.afterValue) {
    const SceneChangeValidation validation = validateValue(
        change.field, *change.afterValue, header.authority, index);
    if (!validation.valid()) {
      return validation;
    }
  }
  for (const auto &value : {change.beforeValue, change.afterValue}) {
    if (value) {
      const auto references = sceneFieldValueReferences(*value);
      if (std::binary_search(references.begin(), references.end(),
                             change.target)) {
        return failure(SceneChangeValidationError::NonClosedHierarchy,
                       "field value contains a self dependency", index);
      }
    }
  }
  if (change.field == SceneField::HierarchyParent) {
    const auto selfParent =
        [&change](const std::optional<SceneFieldValue> &value) {
          if (!value) {
            return false;
          }
          const SceneHierarchyValue &hierarchy =
              std::get<SceneHierarchyValue>(*value);
          return hierarchy.parent && *hierarchy.parent == change.target;
        };
    if (selfParent(change.beforeValue) || selfParent(change.afterValue)) {
      return failure(SceneChangeValidationError::NonClosedHierarchy,
                     "node cannot be its own hierarchy parent", index);
    }
  }
  if (change.field == SceneField::TopologyMetadata) {
    const auto kindMatches =
        [&change](const std::optional<SceneFieldValue> &value) {
          return !value || std::get<SceneTopologyValue>(*value).objectKind ==
                               change.target.kind;
        };
    if (!kindMatches(change.beforeValue) || !kindMatches(change.afterValue)) {
      return failure(SceneChangeValidationError::InvalidFieldValue,
                     "topology metadata kind does not match its stable target",
                     index);
    }
  }
  return {};
}

void normalizeValue(SceneFieldValue &value) {
  std::visit(
      [](auto &typedValue) {
        using Value = std::decay_t<decltype(typedValue)>;
        if constexpr (std::is_same_v<Value,
                                     ScenePrimitiveMaterialBindingValue>) {
          // A variant selection is input provenance. Canonical output is the
          // final primitive-material binding only.
          typedValue.sourceVariant.reset();
        } else if constexpr (std::is_same_v<Value, SceneHierarchyValue>) {
          std::sort(typedValue.attachments.begin(),
                    typedValue.attachments.end());
          typedValue.attachments.erase(
              std::unique(typedValue.attachments.begin(),
                          typedValue.attachments.end()),
              typedValue.attachments.end());
        } else if constexpr (std::is_same_v<Value, SceneTopologyValue>) {
          std::sort(typedValue.dependencies.begin(),
                    typedValue.dependencies.end());
          typedValue.dependencies.erase(
              std::unique(typedValue.dependencies.begin(),
                          typedValue.dependencies.end()),
              typedValue.dependencies.end());
        }
      },
      value);
}

void normalizeChange(SceneFieldChange &change) {
  if (change.beforeValue) {
    normalizeValue(*change.beforeValue);
  }
  if (change.afterValue) {
    normalizeValue(*change.afterValue);
  }
}

std::string valueSubkey(const SceneFieldValue &value) {
  return std::visit(
      [](const auto &typedValue) -> std::string {
        using Value = std::decay_t<decltype(typedValue)>;
        if constexpr (std::is_same_v<Value, SceneMaterialExtensionValue>) {
          return typedValue.extensionName;
        } else if constexpr (std::is_same_v<Value, SceneTextureBindingValue>) {
          return std::string(1,
                             static_cast<char>(enumValue(typedValue.semantic)));
        } else if constexpr (std::is_same_v<Value, SceneBufferRangeValue>) {
          std::string key(9, '\0');
          key[0] = static_cast<char>(enumValue(typedValue.semantic));
          for (std::size_t byte = 0; byte < 8u; ++byte) {
            key[byte + 1u] = static_cast<char>(
                (typedValue.logicalOffset >> (byte * 8u)) & 0xffu);
          }
          return key;
        }
        return {};
      },
      value);
}

struct FieldKey final {
  StableSceneId target;
  SceneField field{SceneField::NodeLocalTransform};
  std::string subkey;

  bool operator<(const FieldKey &other) const {
    return std::tie(target, field, subkey) <
           std::tie(other.target, other.field, other.subkey);
  }
};

std::optional<FieldKey> fieldKey(const SceneFieldChange &change) {
  const SceneFieldValue &primary =
      change.afterValue ? *change.afterValue : *change.beforeValue;
  const std::string subkey = valueSubkey(primary);
  if (change.beforeValue && change.afterValue &&
      valueSubkey(*change.beforeValue) != subkey) {
    return std::nullopt;
  }
  return FieldKey{change.target, change.field, subkey};
}

struct FoldedField final {
  std::optional<SceneFieldChange> change;
  std::optional<StableSceneId> canceledTarget;
};

std::variant<FoldedField, SceneChangeValidation>
foldField(std::vector<SceneFieldChange> changes) {
  std::sort(changes.begin(), changes.end(),
            [](const SceneFieldChange &left, const SceneFieldChange &right) {
              return left.mutationSerial < right.mutationSerial;
            });
  std::vector<SceneFieldChange> unique;
  for (SceneFieldChange &change : changes) {
    if (!unique.empty() &&
        unique.back().mutationSerial == change.mutationSerial) {
      if (!exactChangeEqual(unique.back(), change)) {
        return failure(
            SceneChangeValidationError::ConflictingWrite,
            "same field has conflicting writes at one mutation serial");
      }
      continue;
    }
    unique.push_back(std::move(change));
  }

  const SceneFieldChange &first = unique.front();
  const bool initiallyPresent = first.action != SceneChangeAction::Create;
  std::optional<SceneFieldValue> baseValue = first.beforeValue;
  std::optional<SceneFieldValue> currentValue;
  bool deleted = false;
  for (std::size_t index = 0; index < unique.size(); ++index) {
    const SceneFieldChange &change = unique[index];
    if (change.action == SceneChangeAction::Create) {
      if (currentValue || (index != 0u && !deleted)) {
        return failure(SceneChangeValidationError::DuplicateCreate,
                       "field is created while already present");
      }
      if (index != 0u && deleted) {
        return failure(SceneChangeValidationError::GenerationAmbiguity,
                       "delete/create at one stable generation is ambiguous");
      }
      currentValue = change.afterValue;
      deleted = false;
    } else {
      if (index == 0u) {
        currentValue = change.beforeValue;
      }
      if (!currentValue ||
          !exactValueEqual(*currentValue, *change.beforeValue)) {
        return failure(
            SceneChangeValidationError::DiscontinuousWriteChain,
            "field before value does not equal the preceding final value");
      }
      if (change.action == SceneChangeAction::Update) {
        currentValue = change.afterValue;
      } else {
        currentValue.reset();
        deleted = true;
      }
    }
  }

  SceneFieldChange folded = unique.back();
  folded.beforeValue = baseValue;
  folded.afterValue = currentValue;
  if (!initiallyPresent) {
    if (!currentValue) {
      return FoldedField{std::nullopt, folded.target};
    }
    folded.action = SceneChangeAction::Create;
    folded.beforeValue.reset();
  } else if (!currentValue) {
    folded.action = SceneChangeAction::Delete;
    folded.afterValue.reset();
  } else {
    if (exactValueEqual(*baseValue, *currentValue)) {
      return FoldedField{};
    }
    folded.action = SceneChangeAction::Update;
  }
  return FoldedField{std::move(folded), std::nullopt};
}

struct SlotKey final {
  EditorObjectKind kind{EditorObjectKind::invalid};
  EditorDocumentAuthority authority{};
  std::uint32_t slot{0};

  auto operator<=>(const SlotKey &) const = default;
};

SceneChangeValidation
validateGenerations(const std::vector<SceneFieldChange> &changes) {
  std::map<SlotKey, std::map<std::uint32_t, std::set<SceneChangeAction>>>
      generations;
  for (const SceneFieldChange &change : changes) {
    generations[{change.target.kind, change.target.authority,
                 change.target.slot}][change.target.generation]
        .insert(change.action);
  }
  for (const auto &[slot, byGeneration] : generations) {
    (void)slot;
    if (byGeneration.size() <= 1u) {
      continue;
    }
    if (byGeneration.size() != 2u) {
      return failure(
          SceneChangeValidationError::GenerationAmbiguity,
          "more than two generations of one slot occur in a change set");
    }
    auto oldGeneration = byGeneration.begin();
    auto newGeneration = std::next(oldGeneration);
    const bool oldDeletes =
        oldGeneration->second.size() == 1u &&
        oldGeneration->second.contains(SceneChangeAction::Delete);
    const bool newCreates =
        newGeneration->second.size() == 1u &&
        newGeneration->second.contains(SceneChangeAction::Create);
    if (!oldDeletes || !newCreates) {
      return failure(
          SceneChangeValidationError::GenerationAmbiguity,
          "generation change must be an old delete followed by a new create");
    }
  }
  return {};
}

bool containsId(const std::vector<StableSceneId> &ids, StableSceneId id) {
  return std::binary_search(ids.begin(), ids.end(), id);
}

std::vector<StableSceneId>
referencesOf(const std::optional<SceneFieldValue> &value) {
  return value ? sceneFieldValueReferences(*value)
               : std::vector<StableSceneId>{};
}

int dependencyPhase(const SceneFieldChange &change) {
  if (change.action == SceneChangeAction::Delete) {
    switch (change.field) {
    case SceneField::HierarchyParent:
    case SceneField::HierarchyAttachment:
    case SceneField::MaterialTextureBinding:
    case SceneField::PrimitiveMaterialBinding:
    case SceneField::CameraAttachment:
    case SceneField::LightAttachment:
      return 60;
    case SceneField::TopologyMetadata:
    case SceneField::GeometryBufferRange:
    case SceneField::TexturePayload:
      return 90;
    default:
      return 80;
    }
  }
  if (change.action == SceneChangeAction::Update) {
    const auto before = referencesOf(change.beforeValue);
    const auto after = referencesOf(change.afterValue);
    if (std::any_of(before.begin(), before.end(), [&after](StableSceneId id) {
          return !containsId(after, id);
        })) {
      return 55;
    }
  }
  switch (change.field) {
  case SceneField::TopologyMetadata:
  case SceneField::GeometryBufferRange:
  case SceneField::TexturePayload:
    return 10;
  case SceneField::MaterialParameters:
  case SceneField::MaterialExtension:
  case SceneField::CameraParameters:
  case SceneField::LightParameters:
    return 20;
  case SceneField::HierarchyParent:
  case SceneField::HierarchyAttachment:
    return 30;
  case SceneField::NodeLocalTransform:
  case SceneField::SceneRootTransform:
  case SceneField::NodeLocalVisibility:
    return 40;
  case SceneField::MaterialTextureBinding:
  case SceneField::PrimitiveMaterialBinding:
  case SceneField::CameraAttachment:
  case SceneField::LightAttachment:
    return 50;
  }
  return 50;
}

bool baseChangeLess(const SceneFieldChange &left,
                    const SceneFieldChange &right) {
  const auto leftKey = fieldKey(left);
  const auto rightKey = fieldKey(right);
  return std::tuple{dependencyPhase(left),
                    enumValue(left.target.kind),
                    left.target.authority.value,
                    left.target.slot,
                    left.target.generation,
                    enumValue(left.action),
                    enumValue(left.field),
                    leftKey ? leftKey->subkey : std::string{},
                    left.mutationSerial} <
         std::tuple{dependencyPhase(right),
                    enumValue(right.target.kind),
                    right.target.authority.value,
                    right.target.slot,
                    right.target.generation,
                    enumValue(right.action),
                    enumValue(right.field),
                    rightKey ? rightKey->subkey : std::string{},
                    right.mutationSerial};
}

std::variant<std::vector<SceneFieldChange>, SceneChangeValidation>
orderChanges(std::vector<SceneFieldChange> changes) {
  const std::size_t count = changes.size();
  std::vector<std::set<std::size_t>> outgoing(count);
  std::vector<std::size_t> indegree(count, 0u);
  std::map<StableSceneId, std::vector<std::size_t>> byTarget;
  for (std::size_t index = 0; index < count; ++index) {
    byTarget[changes[index].target].push_back(index);
  }
  const auto addEdge = [&](std::size_t before, std::size_t after) {
    if (before != after && outgoing[before].insert(after).second) {
      ++indegree[after];
    }
  };

  for (std::size_t index = 0; index < count; ++index) {
    const SceneFieldChange &change = changes[index];
    const auto beforeReferences = referencesOf(change.beforeValue);
    const auto afterReferences = referencesOf(change.afterValue);
    for (StableSceneId reference : afterReferences) {
      const auto found = byTarget.find(reference);
      if (found == byTarget.end()) {
        continue;
      }
      for (std::size_t dependency : found->second) {
        if (changes[dependency].action != SceneChangeAction::Delete) {
          addEdge(dependency, index);
        }
      }
    }
    for (StableSceneId reference : beforeReferences) {
      const auto found = byTarget.find(reference);
      if (found == byTarget.end()) {
        continue;
      }
      const bool detached = !containsId(afterReferences, reference);
      for (std::size_t dependency : found->second) {
        if (changes[dependency].action == SceneChangeAction::Delete) {
          addEdge(index, dependency);
        }
      }
      if (change.action == SceneChangeAction::Delete && !detached) {
        for (std::size_t dependency : found->second) {
          if (changes[dependency].action == SceneChangeAction::Delete) {
            addEdge(index, dependency);
          }
        }
      }
    }
  }

  std::map<StableSceneId, StableSceneId> finalParents;
  std::map<StableSceneId, StableSceneId> priorParents;
  for (const SceneFieldChange &change : changes) {
    if (change.field != SceneField::HierarchyParent) {
      continue;
    }
    if (change.beforeValue) {
      const auto &hierarchy =
          std::get<SceneHierarchyValue>(*change.beforeValue);
      if (hierarchy.parent) {
        priorParents.emplace(change.target, *hierarchy.parent);
      }
    }
    if (change.afterValue) {
      const auto &hierarchy = std::get<SceneHierarchyValue>(*change.afterValue);
      if (hierarchy.parent) {
        finalParents.emplace(change.target, *hierarchy.parent);
      }
    }
  }
  for (const auto &[child, parent] : finalParents) {
    const auto parentChanges = byTarget.find(parent);
    const auto childChanges = byTarget.find(child);
    if (parentChanges == byTarget.end() || childChanges == byTarget.end()) {
      continue;
    }
    for (std::size_t parentIndex : parentChanges->second) {
      for (std::size_t childIndex : childChanges->second) {
        const SceneChangeClass parentClass = changes[parentIndex].changeClass;
        const SceneChangeClass childClass = changes[childIndex].changeClass;
        if ((parentClass == SceneChangeClass::Transform &&
             childClass == SceneChangeClass::Transform) ||
            (parentClass == SceneChangeClass::Visibility &&
             childClass == SceneChangeClass::Visibility) ||
            changes[childIndex].field == SceneField::HierarchyParent) {
          addEdge(parentIndex, childIndex);
        }
      }
    }
  }
  for (const auto &[child, parent] : priorParents) {
    const auto parentChanges = byTarget.find(parent);
    const auto childChanges = byTarget.find(child);
    if (parentChanges == byTarget.end() || childChanges == byTarget.end()) {
      continue;
    }
    for (std::size_t parentIndex : parentChanges->second) {
      for (std::size_t childIndex : childChanges->second) {
        if (changes[parentIndex].action == SceneChangeAction::Delete &&
            changes[childIndex].action == SceneChangeAction::Delete) {
          addEdge(childIndex, parentIndex);
        }
      }
    }
  }

  // Preserve generation-changing recreate: retire the old generation before
  // creating any field of the new generation.
  for (std::size_t left = 0; left < count; ++left) {
    for (std::size_t right = 0; right < count; ++right) {
      if (changes[left].target.kind == changes[right].target.kind &&
          changes[left].target.authority == changes[right].target.authority &&
          changes[left].target.slot == changes[right].target.slot &&
          changes[left].target.generation < changes[right].target.generation &&
          changes[left].action == SceneChangeAction::Delete &&
          changes[right].action == SceneChangeAction::Create) {
        addEdge(left, right);
      }
    }
  }

  struct ReadyLess final {
    const std::vector<SceneFieldChange> *changes{};
    bool operator()(std::size_t left, std::size_t right) const {
      if (baseChangeLess((*changes)[left], (*changes)[right])) {
        return true;
      }
      if (baseChangeLess((*changes)[right], (*changes)[left])) {
        return false;
      }
      return left < right;
    }
  };
  std::set<std::size_t, ReadyLess> ready{ReadyLess{&changes}};
  for (std::size_t index = 0; index < count; ++index) {
    if (indegree[index] == 0u) {
      ready.insert(index);
    }
  }
  std::vector<SceneFieldChange> ordered;
  ordered.reserve(count);
  while (!ready.empty()) {
    const std::size_t index = *ready.begin();
    ready.erase(ready.begin());
    ordered.push_back(std::move(changes[index]));
    for (std::size_t dependent : outgoing[index]) {
      if (--indegree[dependent] == 0u) {
        ready.insert(dependent);
      }
    }
  }
  if (ordered.size() != count) {
    return failure(SceneChangeValidationError::DependencyCycle,
                   "derived scene work contains a dependency cycle");
  }
  return ordered;
}

SceneChangeValidation normalizeRemap(std::vector<StableIdRemapEvidence> &remap,
                                     EditorDocumentAuthority authority) {
  for (const StableIdRemapEvidence &entry : remap) {
    if (!entry.oldId && !entry.newId) {
      return failure(SceneChangeValidationError::IncompleteRemap,
                     "remap entry has neither an old nor a new identity");
    }
    for (const auto &id : {entry.oldId, entry.newId}) {
      if (id && (!id->valid() || id->authority != authority)) {
        return failure(id->valid()
                           ? SceneChangeValidationError::CrossAuthority
                           : SceneChangeValidationError::InvalidStableId,
                       "remap entry identity is invalid or cross-authority");
      }
    }
  }
  const auto key = [](const StableIdRemapEvidence &entry) {
    const StableSceneId primary = entry.oldId ? *entry.oldId : *entry.newId;
    const StableSceneId secondary = entry.newId ? *entry.newId : primary;
    return std::tuple{enumValue(primary.kind),
                      primary.authority.value,
                      primary.slot,
                      primary.generation,
                      entry.oldId.has_value(),
                      enumValue(secondary.kind),
                      secondary.authority.value,
                      secondary.slot,
                      secondary.generation,
                      entry.newId.has_value()};
  };
  std::sort(remap.begin(), remap.end(),
            [&](const auto &left, const auto &right) {
              return key(left) < key(right);
            });
  remap.erase(std::unique(remap.begin(), remap.end()), remap.end());
  std::map<StableSceneId, std::optional<StableSceneId>> byOld;
  std::map<StableSceneId, std::optional<StableSceneId>> byNew;
  for (const StableIdRemapEvidence &entry : remap) {
    if (entry.oldId) {
      const auto [iterator, inserted] =
          byOld.emplace(*entry.oldId, entry.newId);
      if (!inserted && iterator->second != entry.newId) {
        return failure(SceneChangeValidationError::IncompleteRemap,
                       "one old identity maps to multiple targets");
      }
    }
    if (entry.newId) {
      const auto [iterator, inserted] =
          byNew.emplace(*entry.newId, entry.oldId);
      if (!inserted && iterator->second != entry.oldId) {
        return failure(SceneChangeValidationError::IncompleteRemap,
                       "multiple old identities map to one target");
      }
    }
  }
  return {};
}

SceneChangeValidation
validateIncrementalRemap(const std::vector<SceneFieldChange> &changes,
                         const std::vector<StableIdRemapEvidence> &remap) {
  std::set<StableSceneId> created;
  std::set<StableSceneId> deleted;
  for (const SceneFieldChange &change : changes) {
    if (change.field != SceneField::TopologyMetadata) {
      continue;
    }
    if (change.action == SceneChangeAction::Create) {
      created.insert(change.target);
    } else if (change.action == SceneChangeAction::Delete) {
      deleted.insert(change.target);
    }
  }

  std::set<StableSceneId> coveredCreates;
  std::set<StableSceneId> coveredDeletes;
  for (const StableIdRemapEvidence &entry : remap) {
    if (entry.oldId) {
      if (!deleted.contains(*entry.oldId)) {
        return failure(SceneChangeValidationError::IncompleteRemap,
                       "remap old identity has no exact topology delete");
      }
      coveredDeletes.insert(*entry.oldId);
    }
    if (entry.newId) {
      if (!created.contains(*entry.newId)) {
        return failure(SceneChangeValidationError::IncompleteRemap,
                       "remap new identity has no exact topology create");
      }
      coveredCreates.insert(*entry.newId);
    }
    if (entry.oldId && entry.newId &&
        (entry.oldId->kind != entry.newId->kind ||
         entry.oldId->authority != entry.newId->authority)) {
      return failure(SceneChangeValidationError::IncompleteRemap,
                     "identity remap changes object kind or authority");
    }
  }
  if (created != coveredCreates || deleted != coveredDeletes) {
    return failure(SceneChangeValidationError::IncompleteRemap,
                   "every topology object create and delete requires complete "
                   "remap evidence");
  }
  return {};
}

SceneChangeValidation
validateFullSnapshot(const SceneChangeHeader &header,
                     FullSnapshotReference &fullSnapshot) {
  if (!enumAtMost(fullSnapshot.reason,
                  FullRebuildReason::ExplicitCallerRequest) ||
      !fullSnapshot.stableIdRemapComplete) {
    return failure(
        SceneChangeValidationError::InvalidFullSnapshot,
        "full snapshot reason or complete-remap declaration is invalid");
  }
  ImmutableSceneSnapshotRef &target = fullSnapshot.immutableTarget;
  if (target.schemaVersion != kImmutableSceneSnapshotSchemaVersion ||
      !enumAtMost(target.encoding,
                  ImmutableSceneSnapshotEncoding::CanonicalBinary) ||
      target.authority != header.authority ||
      target.documentRevision != header.targetDocumentRevision ||
      target.sceneActivationEpoch != header.targetSceneActivationEpoch ||
      target.rootExternalStateSerial != header.targetRootExternalStateSerial ||
      target.activeScene != header.targetActiveScene ||
      target.documentDigest != header.targetDocumentDigest ||
      target.projectionDigest != header.targetProjectionDigest ||
      target.bytes.empty() ||
      target.bytes.size() > kMaximumSceneChangePayloadBytes) {
    return failure(
        SceneChangeValidationError::InvalidFullSnapshot,
        "immutable target does not carry the complete target envelope");
  }
  if (!sceneDigestPresent(target.contentDigest) ||
      target.contentDigest != fullSnapshot.targetSnapshotDigest ||
      SceneChangeCanonicalizer::computeSha256(target.bytes) !=
          target.contentDigest) {
    return failure(
        SceneChangeValidationError::InvalidContentHash,
        "full snapshot content digest does not match its immutable bytes");
  }
  SceneChangeValidation remap =
      normalizeRemap(fullSnapshot.stableIdRemap, header.authority);
  if (!remap.valid()) {
    return remap;
  }
  return {};
}

class Encoder final {
public:
  void u8(std::uint8_t value) { m_bytes.push_back(value); }

  void u16(std::uint16_t value) {
    for (unsigned shift = 0; shift < 16u; shift += 8u) {
      u8(static_cast<std::uint8_t>((value >> shift) & 0xffu));
    }
  }

  void u32(std::uint32_t value) {
    for (unsigned shift = 0; shift < 32u; shift += 8u) {
      u8(static_cast<std::uint8_t>((value >> shift) & 0xffu));
    }
  }

  void u64(std::uint64_t value) {
    for (unsigned shift = 0; shift < 64u; shift += 8u) {
      u8(static_cast<std::uint8_t>((value >> shift) & 0xffu));
    }
  }

  void boolean(bool value) { u8(value ? 1u : 0u); }

  void f64(double value) { u64(detail::bitwiseCast<std::uint64_t>(value)); }

  void raw(std::span<const std::uint8_t> bytes) {
    m_bytes.insert(m_bytes.end(), bytes.begin(), bytes.end());
  }

  void bytes(std::span<const std::uint8_t> value) {
    u64(static_cast<std::uint64_t>(value.size()));
    raw(value);
  }

  void string(const std::string &value) {
    u64(static_cast<std::uint64_t>(value.size()));
    m_bytes.insert(m_bytes.end(), value.begin(), value.end());
  }

  [[nodiscard]] std::vector<std::uint8_t> take() { return std::move(m_bytes); }

private:
  std::vector<std::uint8_t> m_bytes;
};

void encodeDigest(Encoder &encoder, const SceneDigest &digest) {
  encoder.raw(digest);
}

void encodeId(Encoder &encoder, StableSceneId id) {
  encoder.u8(enumValue(id.kind));
  encoder.u64(id.authority.value);
  encoder.u32(id.slot);
  encoder.u32(id.generation);
}

void encodeOptionalId(Encoder &encoder,
                      const std::optional<StableSceneId> &id) {
  encoder.boolean(id.has_value());
  if (id) {
    encodeId(encoder, *id);
  }
}

void encodeToken(Encoder &encoder, const SceneMutationToken &token) {
  encoder.u64(token.authority.value);
  encoder.u64(token.revision);
  encoder.u64(token.serial);
}

void encodeValue(Encoder &encoder, const SceneFieldValue &value) {
  encoder.u8(enumValue(sceneFieldValueTag(value)));
  std::visit(
      [&encoder](const auto &typedValue) {
        using Value = std::decay_t<decltype(typedValue)>;
        if constexpr (std::is_same_v<Value, SceneNodeTransformValue>) {
          encoder.u8(static_cast<std::uint8_t>(typedValue.value.index()));
          std::visit(
              [&encoder](const auto &transform) {
                if constexpr (std::is_same_v<std::decay_t<decltype(transform)>,
                                             SceneTrsValue>) {
                  for (double component : transform.translation)
                    encoder.f64(component);
                  for (double component : transform.rotation)
                    encoder.f64(component);
                  for (double component : transform.scale)
                    encoder.f64(component);
                } else {
                  for (double component : transform.columnMajor)
                    encoder.f64(component);
                }
              },
              typedValue.value);
        } else if constexpr (std::is_same_v<Value, SceneVisibilityValue>) {
          encoder.boolean(typedValue.visible);
        } else if constexpr (std::is_same_v<Value,
                                            SceneMaterialParametersValue>) {
          for (double component : typedValue.baseColorFactor)
            encoder.f64(component);
          encoder.f64(typedValue.metallicFactor);
          encoder.f64(typedValue.roughnessFactor);
          for (double component : typedValue.emissiveFactor)
            encoder.f64(component);
          encoder.f64(typedValue.alphaCutoff);
          encoder.u8(enumValue(typedValue.alphaMode));
          encoder.boolean(typedValue.doubleSided);
        } else if constexpr (std::is_same_v<Value,
                                            SceneMaterialExtensionValue>) {
          encoder.string(typedValue.extensionName);
          encodeDigest(encoder, typedValue.contentDigest);
          encoder.bytes(typedValue.canonicalValueBytes);
        } else if constexpr (std::is_same_v<Value, SceneTextureBindingValue>) {
          encoder.u8(enumValue(typedValue.semantic));
          encodeOptionalId(encoder, typedValue.texture);
          encodeOptionalId(encoder, typedValue.sampler);
          encoder.u32(typedValue.uvSet);
          for (double component : typedValue.uvOffset)
            encoder.f64(component);
          for (double component : typedValue.uvScale)
            encoder.f64(component);
          encoder.f64(typedValue.uvRotation);
          encoder.f64(typedValue.scalar);
        } else if constexpr (std::is_same_v<
                                 Value, ScenePrimitiveMaterialBindingValue>) {
          encodeOptionalId(encoder, typedValue.material);
          encoder.boolean(typedValue.sourceVariant.has_value());
          if (typedValue.sourceVariant)
            encoder.u32(*typedValue.sourceVariant);
        } else if constexpr (std::is_same_v<Value, SceneCameraValue>) {
          encoder.u8(enumValue(typedValue.projection));
          encoder.f64(typedValue.yfov);
          encoder.f64(typedValue.znear);
          encoder.f64(typedValue.zfar);
          encoder.f64(typedValue.aspectRatio);
          encoder.f64(typedValue.xmag);
          encoder.f64(typedValue.ymag);
        } else if constexpr (std::is_same_v<Value,
                                            SceneObjectAttachmentValue>) {
          encodeOptionalId(encoder, typedValue.node);
        } else if constexpr (std::is_same_v<Value, SceneLightValue>) {
          encoder.u8(enumValue(typedValue.type));
          for (double component : typedValue.color)
            encoder.f64(component);
          encoder.f64(typedValue.intensity);
          encoder.f64(typedValue.range);
          encoder.f64(typedValue.innerConeAngle);
          encoder.f64(typedValue.outerConeAngle);
        } else if constexpr (std::is_same_v<Value, SceneHierarchyValue>) {
          encodeOptionalId(encoder, typedValue.parent);
          encoder.u64(
              static_cast<std::uint64_t>(typedValue.attachments.size()));
          for (StableSceneId attachment : typedValue.attachments)
            encodeId(encoder, attachment);
        } else if constexpr (std::is_same_v<Value, SceneTopologyValue>) {
          encoder.u8(enumValue(typedValue.topologyKind));
          encoder.u8(enumValue(typedValue.objectKind));
          encoder.u64(
              static_cast<std::uint64_t>(typedValue.dependencies.size()));
          for (StableSceneId dependency : typedValue.dependencies)
            encodeId(encoder, dependency);
          encodeDigest(encoder, typedValue.schemaDigest);
          encoder.u64(typedValue.elementCount);
        } else if constexpr (std::is_same_v<Value, SceneBufferRangeValue>) {
          encoder.u8(enumValue(typedValue.semantic));
          encoder.u64(typedValue.logicalOffset);
          encoder.u64(typedValue.byteSize);
          encodeDigest(encoder, typedValue.schemaDigest);
          encodeDigest(encoder, typedValue.contentDigest);
          encoder.bytes(typedValue.bytes);
        } else if constexpr (std::is_same_v<Value, SceneTexturePayloadValue>) {
          encoder.u32(typedValue.width);
          encoder.u32(typedValue.height);
          encoder.u32(typedValue.depth);
          encoder.u32(typedValue.mipCount);
          encoder.u32(typedValue.layerCount);
          encoder.u32(typedValue.formatTag);
          encoder.u8(enumValue(typedValue.colorSpace));
          encodeDigest(encoder, typedValue.schemaDigest);
          encodeDigest(encoder, typedValue.contentDigest);
          encoder.bytes(typedValue.bytes);
        }
      },
      value);
}

void encodeOptionalValue(Encoder &encoder,
                         const std::optional<SceneFieldValue> &value) {
  encoder.boolean(value.has_value());
  if (value) {
    encodeValue(encoder, *value);
  }
}

void encodeRemap(Encoder &encoder, const StableIdRemapEvidence &remap) {
  encodeOptionalId(encoder, remap.oldId);
  encodeOptionalId(encoder, remap.newId);
}

std::vector<std::uint8_t> serializeCanonical(const SceneChangeSet &changeSet) {
  Encoder encoder;
  constexpr std::array<std::uint8_t, 8> magic{'M', 'G', 'I', 'F',
                                              'S', 'C', 'N', '1'};
  encoder.raw(magic);
  const SceneChangeHeader &header = changeSet.header;
  encoder.u32(header.schemaVersion);
  encoder.u8(enumValue(header.domain));
  encoder.u64(header.authority.value);
  encoder.u64(header.baseDocumentRevision);
  encoder.u64(header.targetDocumentRevision);
  encoder.u64(header.baseSceneActivationEpoch);
  encoder.u64(header.targetSceneActivationEpoch);
  encoder.u64(header.baseRootExternalStateSerial);
  encoder.u64(header.targetRootExternalStateSerial);
  encodeOptionalId(encoder, header.baseActiveScene);
  encodeOptionalId(encoder, header.targetActiveScene);
  encoder.boolean(header.mutationToken.has_value());
  if (header.mutationToken)
    encodeToken(encoder, *header.mutationToken);
  encoder.u64(header.firstMutationSerial);
  encoder.u64(header.lastMutationSerial);
  encodeDigest(encoder, header.baseDocumentDigest);
  encodeDigest(encoder, header.targetDocumentDigest);
  encodeDigest(encoder, header.baseProjectionDigest);
  encodeDigest(encoder, header.targetProjectionDigest);
  // canonicalChangeSetDigest is intentionally excluded from its own preimage.
  encoder.u64(static_cast<std::uint64_t>(header.sourceMutations.size()));
  for (const SourceMutationFence &source : header.sourceMutations) {
    encoder.u64(source.baseDocumentRevision);
    encoder.u64(source.targetDocumentRevision);
    encodeToken(encoder, source.mutationToken);
    encodeDigest(encoder, source.baseDocumentDigest);
    encodeDigest(encoder, source.targetDocumentDigest);
  }
  encoder.u64(static_cast<std::uint64_t>(changeSet.canonicalChanges.size()));
  for (const SceneFieldChange &change : changeSet.canonicalChanges) {
    encodeId(encoder, change.target);
    encoder.u8(enumValue(change.changeClass));
    encoder.u8(enumValue(change.action));
    encoder.u16(enumValue(change.field));
    encoder.u64(change.mutationSerial);
    encodeOptionalValue(encoder, change.beforeValue);
    encodeOptionalValue(encoder, change.afterValue);
  }
  encoder.u64(static_cast<std::uint64_t>(changeSet.stableIdRemap.size()));
  for (const StableIdRemapEvidence &remap : changeSet.stableIdRemap) {
    encodeRemap(encoder, remap);
  }
  encoder.boolean(changeSet.fullSnapshot.has_value());
  if (changeSet.fullSnapshot) {
    const FullSnapshotReference &full = *changeSet.fullSnapshot;
    encoder.u8(enumValue(full.reason));
    encodeDigest(encoder, full.targetSnapshotDigest);
    const ImmutableSceneSnapshotRef &target = full.immutableTarget;
    encoder.u32(target.schemaVersion);
    encoder.u8(enumValue(target.encoding));
    encoder.u64(target.authority.value);
    encoder.u64(target.documentRevision);
    encoder.u64(target.sceneActivationEpoch);
    encoder.u64(target.rootExternalStateSerial);
    encodeOptionalId(encoder, target.activeScene);
    encodeDigest(encoder, target.documentDigest);
    encodeDigest(encoder, target.projectionDigest);
    encodeDigest(encoder, target.contentDigest);
    encoder.bytes(target.bytes);
    encoder.u64(static_cast<std::uint64_t>(full.stableIdRemap.size()));
    for (const StableIdRemapEvidence &remap : full.stableIdRemap) {
      encodeRemap(encoder, remap);
    }
    encoder.boolean(full.stableIdRemapComplete);
  }
  return encoder.take();
}

class Sha256State final {
public:
  void update(std::span<const std::uint8_t> bytes) noexcept {
    m_totalBytes += static_cast<std::uint64_t>(bytes.size());
    while (!bytes.empty()) {
      const std::size_t amount =
          std::min(bytes.size(), m_buffer.size() - m_bufferSize);
      std::memcpy(m_buffer.data() + m_bufferSize, bytes.data(), amount);
      m_bufferSize += amount;
      bytes = bytes.subspan(amount);
      if (m_bufferSize == m_buffer.size()) {
        transform(m_buffer.data());
        m_bufferSize = 0u;
      }
    }
  }

  SceneDigest finish() noexcept {
    const std::uint64_t bitLength = m_totalBytes * 8u;
    m_buffer[m_bufferSize++] = 0x80u;
    if (m_bufferSize > 56u) {
      std::fill(m_buffer.begin() + static_cast<std::ptrdiff_t>(m_bufferSize),
                m_buffer.end(), 0u);
      transform(m_buffer.data());
      m_bufferSize = 0u;
    }
    std::fill(m_buffer.begin() + static_cast<std::ptrdiff_t>(m_bufferSize),
              m_buffer.begin() + 56, 0u);
    for (std::size_t byte = 0; byte < 8u; ++byte) {
      m_buffer[63u - byte] =
          static_cast<std::uint8_t>(bitLength >> (byte * 8u));
    }
    transform(m_buffer.data());
    SceneDigest digest{};
    for (std::size_t word = 0; word < m_hash.size(); ++word) {
      for (std::size_t byte = 0; byte < 4u; ++byte) {
        digest[word * 4u + byte] =
            static_cast<std::uint8_t>(m_hash[word] >> ((3u - byte) * 8u));
      }
    }
    return digest;
  }

private:
  static constexpr std::array<std::uint32_t, 64> kRoundConstants{
      0x428a2f98u, 0x71374491u, 0xb5c0fbcfu, 0xe9b5dba5u, 0x3956c25bu,
      0x59f111f1u, 0x923f82a4u, 0xab1c5ed5u, 0xd807aa98u, 0x12835b01u,
      0x243185beu, 0x550c7dc3u, 0x72be5d74u, 0x80deb1feu, 0x9bdc06a7u,
      0xc19bf174u, 0xe49b69c1u, 0xefbe4786u, 0x0fc19dc6u, 0x240ca1ccu,
      0x2de92c6fu, 0x4a7484aau, 0x5cb0a9dcu, 0x76f988dau, 0x983e5152u,
      0xa831c66du, 0xb00327c8u, 0xbf597fc7u, 0xc6e00bf3u, 0xd5a79147u,
      0x06ca6351u, 0x14292967u, 0x27b70a85u, 0x2e1b2138u, 0x4d2c6dfcu,
      0x53380d13u, 0x650a7354u, 0x766a0abbu, 0x81c2c92eu, 0x92722c85u,
      0xa2bfe8a1u, 0xa81a664bu, 0xc24b8b70u, 0xc76c51a3u, 0xd192e819u,
      0xd6990624u, 0xf40e3585u, 0x106aa070u, 0x19a4c116u, 0x1e376c08u,
      0x2748774cu, 0x34b0bcb5u, 0x391c0cb3u, 0x4ed8aa4au, 0x5b9cca4fu,
      0x682e6ff3u, 0x748f82eeu, 0x78a5636fu, 0x84c87814u, 0x8cc70208u,
      0x90befffau, 0xa4506cebu, 0xbef9a3f7u, 0xc67178f2u};

  static std::uint32_t loadBigEndian(const std::uint8_t *bytes) noexcept {
    return (static_cast<std::uint32_t>(bytes[0]) << 24u) |
           (static_cast<std::uint32_t>(bytes[1]) << 16u) |
           (static_cast<std::uint32_t>(bytes[2]) << 8u) |
           static_cast<std::uint32_t>(bytes[3]);
  }

  void transform(const std::uint8_t *block) noexcept {
    std::array<std::uint32_t, 64> words{};
    for (std::size_t index = 0; index < 16u; ++index) {
      words[index] = loadBigEndian(block + index * 4u);
    }
    for (std::size_t index = 16u; index < words.size(); ++index) {
      const std::uint32_t s0 = std::rotr(words[index - 15u], 7) ^
                               std::rotr(words[index - 15u], 18) ^
                               (words[index - 15u] >> 3u);
      const std::uint32_t s1 = std::rotr(words[index - 2u], 17) ^
                               std::rotr(words[index - 2u], 19) ^
                               (words[index - 2u] >> 10u);
      words[index] = words[index - 16u] + s0 + words[index - 7u] + s1;
    }
    std::uint32_t a = m_hash[0];
    std::uint32_t b = m_hash[1];
    std::uint32_t c = m_hash[2];
    std::uint32_t d = m_hash[3];
    std::uint32_t e = m_hash[4];
    std::uint32_t f = m_hash[5];
    std::uint32_t g = m_hash[6];
    std::uint32_t h = m_hash[7];
    for (std::size_t index = 0; index < words.size(); ++index) {
      const std::uint32_t sum1 =
          std::rotr(e, 6) ^ std::rotr(e, 11) ^ std::rotr(e, 25);
      const std::uint32_t choose = (e & f) ^ (~e & g);
      const std::uint32_t temporary1 =
          h + sum1 + choose + kRoundConstants[index] + words[index];
      const std::uint32_t sum0 =
          std::rotr(a, 2) ^ std::rotr(a, 13) ^ std::rotr(a, 22);
      const std::uint32_t majority = (a & b) ^ (a & c) ^ (b & c);
      const std::uint32_t temporary2 = sum0 + majority;
      h = g;
      g = f;
      f = e;
      e = d + temporary1;
      d = c;
      c = b;
      b = a;
      a = temporary1 + temporary2;
    }
    m_hash[0] += a;
    m_hash[1] += b;
    m_hash[2] += c;
    m_hash[3] += d;
    m_hash[4] += e;
    m_hash[5] += f;
    m_hash[6] += g;
    m_hash[7] += h;
  }

  std::array<std::uint32_t, 8> m_hash{0x6a09e667u, 0xbb67ae85u, 0x3c6ef372u,
                                      0xa54ff53au, 0x510e527fu, 0x9b05688cu,
                                      0x1f83d9abu, 0x5be0cd19u};
  std::array<std::uint8_t, 64> m_buffer{};
  std::size_t m_bufferSize{0};
  std::uint64_t m_totalBytes{0};
};

} // namespace

struct SceneSha256Prefix::State final {
  Sha256State portable;
#if defined(_WIN32)
  NativeHash native;
#endif
};

SceneSha256Prefix SceneChangeCanonicalizer::captureSha256Prefix(
    std::span<const std::span<const std::uint8_t>> parts) noexcept {
  SceneSha256Prefix result;
  try {
    auto state = std::make_shared<SceneSha256Prefix::State>();
#if defined(_WIN32)
    const auto algorithm = nativeSha256Algorithm();
    if (algorithm && BCryptCreateHash(algorithm, &state->native.handle,
            nullptr, 0, nullptr, 0, 0) >= 0) {
      if (!appendNativeSha256(state->native.handle, parts)) return result;
    } else
#endif
    {
      for (const auto bytes : parts) state->portable.update(bytes);
    }
    result.m_state = std::move(state);
  } catch (...) {}
  return result;
}

SceneDigest SceneChangeCanonicalizer::computeSha256WithPrefix(
    const SceneSha256Prefix& prefix, std::span<const std::uint8_t> suffix) noexcept {
  if (!prefix.m_state) return {};
#if defined(_WIN32)
  if (prefix.m_state->native.handle) {
    NativeHash hash;
    SceneDigest digest{};
    const std::array parts{suffix};
    if (BCryptDuplicateHash(prefix.m_state->native.handle, &hash.handle, nullptr, 0, 0) < 0 ||
        !appendNativeSha256(hash.handle, parts) ||
        BCryptFinishHash(hash.handle, digest.data(), static_cast<ULONG>(digest.size()), 0) < 0)
      return {};
    return digest;
  }
#endif
  auto state = prefix.m_state->portable;
  state.update(suffix);
  return state.finish();
}

SceneCanonicalizationResult
SceneChangeCanonicalizer::canonicalize(SceneChangeSet input) {
  const SceneDigest suppliedDigest = input.header.canonicalChangeSetDigest;
  SceneChangeValidation validation = validateHeader(input.header);
  if (!validation.valid()) {
    return failedResult(std::move(validation));
  }

  if (input.header.domain == SceneChangeDomain::FullSnapshot) {
    if (!input.canonicalChanges.empty() || !input.fullSnapshot) {
      return failedResult(
          failure(SceneChangeValidationError::InvalidFullSnapshot,
                  "full snapshot domain requires one snapshot and no "
                  "incremental changes"));
    }
  } else if (input.fullSnapshot) {
    return failedResult(
        failure(SceneChangeValidationError::InvalidFullSnapshot,
                "incremental domain cannot carry a full snapshot"));
  }

  for (std::size_t index = 0; index < input.canonicalChanges.size(); ++index) {
    validation =
        validateChange(input.header, input.canonicalChanges[index], index);
    if (!validation.valid()) {
      return failedResult(std::move(validation));
    }
    normalizeChange(input.canonicalChanges[index]);
  }
  const auto isRootTransform = [](const SceneFieldChange &change) {
    return change.field == SceneField::SceneRootTransform;
  };
  const std::size_t rootTransformCount = static_cast<std::size_t>(
      std::count_if(input.canonicalChanges.begin(),
                    input.canonicalChanges.end(), isRootTransform));
  if (input.header.domain == SceneChangeDomain::SceneActivation &&
      !input.canonicalChanges.empty()) {
    return failedResult(failure(SceneChangeValidationError::InvalidEnvelope,
                                "scene activation carries identity and "
                                "projection fences, not field mutations"));
  }
  if (input.header.domain == SceneChangeDomain::SceneRootState) {
    if (rootTransformCount != 1u || input.canonicalChanges.size() != 1u ||
        !input.header.targetActiveScene) {
      return failedResult(failure(SceneChangeValidationError::InvalidEnvelope,
                                  "scene root-state transition requires one "
                                  "exact active-scene root value"));
    }
    const auto rootChange =
        std::find_if(input.canonicalChanges.begin(),
                     input.canonicalChanges.end(), isRootTransform);
    if (rootChange->target != *input.header.targetActiveScene) {
      return failedResult(
          failure(SceneChangeValidationError::InvalidActiveScene,
                  "scene root-state target does not match the active scene"));
    }
    if (rootChange->action != SceneChangeAction::Update) {
      return failedResult(failure(
          SceneChangeValidationError::InvalidActionValues,
          "scene root-state transition is a complete before/after update"));
    }
  }
  if (input.header.domain == SceneChangeDomain::DocumentMutation) {
    const bool rootSerialChanged = input.header.baseRootExternalStateSerial !=
                                   input.header.targetRootExternalStateSerial;
    if (rootSerialChanged != (rootTransformCount != 0u) ||
        rootTransformCount > 1u) {
      return failedResult(failure(SceneChangeValidationError::InvalidEnvelope,
                                  "document mutation root serial and exact "
                                  "frozen root value disagree"));
    }
    if (rootSerialChanged) {
      const auto rootChange =
          std::find_if(input.canonicalChanges.begin(),
                       input.canonicalChanges.end(), isRootTransform);
      if (!input.header.targetActiveScene ||
          rootChange->target != *input.header.targetActiveScene ||
          rootChange->action != SceneChangeAction::Update) {
        return failedResult(failure(
            SceneChangeValidationError::InvalidEnvelope,
            "document mutation frozen root must update the active scene"));
      }
    }
  }

  std::map<FieldKey, std::vector<SceneFieldChange>> grouped;
  for (SceneFieldChange &change : input.canonicalChanges) {
    const std::optional<FieldKey> key = fieldKey(change);
    if (!key) {
      return failedResult(
          failure(SceneChangeValidationError::InvalidFieldValue,
                  "field update changes its binding or range key"));
    }
    grouped[*key].push_back(std::move(change));
  }

  std::vector<SceneFieldChange> foldedChanges;
  std::vector<StableSceneId> canceledTargets;
  for (auto &[key, changes] : grouped) {
    (void)key;
    auto folded = foldField(std::move(changes));
    if (std::holds_alternative<SceneChangeValidation>(folded)) {
      return failedResult(std::get<SceneChangeValidation>(std::move(folded)));
    }
    FoldedField value = std::get<FoldedField>(std::move(folded));
    if (value.change) {
      foldedChanges.push_back(std::move(*value.change));
    }
    if (value.canceledTarget) {
      canceledTargets.push_back(*value.canceledTarget);
    }
  }
  std::sort(canceledTargets.begin(), canceledTargets.end());
  canceledTargets.erase(
      std::unique(canceledTargets.begin(), canceledTargets.end()),
      canceledTargets.end());
  for (const SceneFieldChange &change : foldedChanges) {
    for (StableSceneId reference : referencesOf(change.afterValue)) {
      if (std::binary_search(canceledTargets.begin(), canceledTargets.end(),
                             reference)) {
        return failedResult(failure(
            SceneChangeValidationError::NonClosedHierarchy,
            "surviving work references an unpublished create/delete target"));
      }
    }
  }

  validation = validateGenerations(foldedChanges);
  if (!validation.valid()) {
    return failedResult(std::move(validation));
  }
  auto ordered = orderChanges(std::move(foldedChanges));
  if (std::holds_alternative<SceneChangeValidation>(ordered)) {
    return failedResult(std::get<SceneChangeValidation>(std::move(ordered)));
  }
  input.canonicalChanges =
      std::get<std::vector<SceneFieldChange>>(std::move(ordered));

  const std::size_t canonicalRootTransformCount = static_cast<std::size_t>(
      std::count_if(input.canonicalChanges.begin(),
                    input.canonicalChanges.end(), isRootTransform));
  if ((input.header.domain == SceneChangeDomain::SceneRootState &&
       canonicalRootTransformCount != 1u) ||
      (input.header.domain == SceneChangeDomain::DocumentMutation &&
       (input.header.baseRootExternalStateSerial !=
        input.header.targetRootExternalStateSerial) !=
           (canonicalRootTransformCount == 1u))) {
    return failedResult(failure(
        SceneChangeValidationError::InvalidEnvelope,
        "canonical root value does not match the published root-state clock"));
  }

  validation = normalizeRemap(input.stableIdRemap, input.header.authority);
  if (!validation.valid()) {
    return failedResult(std::move(validation));
  }
  if (input.header.domain != SceneChangeDomain::FullSnapshot) {
    validation =
        validateIncrementalRemap(input.canonicalChanges, input.stableIdRemap);
    if (!validation.valid()) {
      return failedResult(std::move(validation));
    }
  }
  if (input.fullSnapshot) {
    validation = validateFullSnapshot(input.header, *input.fullSnapshot);
    if (!validation.valid()) {
      return failedResult(std::move(validation));
    }
    if (input.stableIdRemap.empty()) {
      input.stableIdRemap = input.fullSnapshot->stableIdRemap;
    } else if (input.stableIdRemap != input.fullSnapshot->stableIdRemap) {
      return failedResult(
          failure(SceneChangeValidationError::IncompleteRemap,
                  "change-set and full-snapshot remap evidence differ"));
    }
  }

  input.header.canonicalChangeSetDigest = {};
  std::vector<std::uint8_t> canonicalBytes = serializeCanonical(input);
  const SceneDigest computedDigest = computeSha256(canonicalBytes);
  if (sceneDigestPresent(suppliedDigest) && suppliedDigest != computedDigest) {
    return failedResult(failure(
        SceneChangeValidationError::CanonicalDigestMismatch,
        "supplied canonical change-set digest does not match canonical bytes"));
  }
  input.header.canonicalChangeSetDigest = computedDigest;

  SceneCanonicalizationResult result;
  result.changeSet = std::move(input);
  result.canonicalBytes = std::move(canonicalBytes);
  return result;
}

SceneCanonicalizationResult SceneChangeCanonicalizer::canonicalizeEvidence(
    SceneChangeHeader header, std::vector<SceneMutationEvidence> evidence,
    std::optional<FullSnapshotReference> fullSnapshot) {
  SceneChangeSet input;
  input.header = std::move(header);
  std::vector<SourceMutationFence> sourceMutations;
  bool incompleteStructuralEvidence = false;
  for (SceneMutationEvidence &item : evidence) {
    sourceMutations.push_back(item.sourceMutation);
    input.canonicalChanges.insert(
        input.canonicalChanges.end(),
        std::make_move_iterator(item.exactChanges.begin()),
        std::make_move_iterator(item.exactChanges.end()));
    input.stableIdRemap.insert(
        input.stableIdRemap.end(),
        std::make_move_iterator(item.stableIdRemap.begin()),
        std::make_move_iterator(item.stableIdRemap.end()));
    if (item.fullSnapshot) {
      if (fullSnapshot && *fullSnapshot != *item.fullSnapshot) {
        return failedResult(
            failure(SceneChangeValidationError::InvalidFullSnapshot,
                    "raw evidence contains conflicting full snapshots"));
      }
      fullSnapshot = std::move(item.fullSnapshot);
    }
    incompleteStructuralEvidence =
        incompleteStructuralEvidence || !item.structuralEvidenceComplete;
  }
  if (incompleteStructuralEvidence && !fullSnapshot) {
    return failedResult(failure(
        SceneChangeValidationError::InvalidFullSnapshot,
        "incomplete structural evidence requires an explicit full snapshot"));
  }
  if (fullSnapshot) {
    if (input.header.domain != SceneChangeDomain::FullSnapshot) {
      return failedResult(failure(
          SceneChangeValidationError::InvalidFullSnapshot,
          "explicit snapshot evidence requires a FullSnapshot envelope"));
    }
    input.canonicalChanges.clear();
  }
  if (!sourceMutations.empty()) {
    if (!input.header.sourceMutations.empty() &&
        input.header.sourceMutations != sourceMutations) {
      return failedResult(
          failure(SceneChangeValidationError::InvalidSourceMutationChain,
                  "header and raw evidence source chains differ"));
    }
    input.header.sourceMutations = std::move(sourceMutations);
  }
  if (fullSnapshot) {
    input.stableIdRemap.insert(input.stableIdRemap.end(),
                               fullSnapshot->stableIdRemap.begin(),
                               fullSnapshot->stableIdRemap.end());
    fullSnapshot->stableIdRemap = input.stableIdRemap;
    input.fullSnapshot = std::move(fullSnapshot);
  }
  return canonicalize(std::move(input));
}

SceneChangeValidation SceneChangeCanonicalizer::validateCanonical(
    const SceneChangeSet &changeSet,
    std::span<const std::uint8_t> canonicalBytes) {
  SceneCanonicalizationResult result = canonicalize(changeSet);
  if (!result.succeeded()) {
    return result.validation;
  }
  if (*result.changeSet != changeSet ||
      !std::equal(result.canonicalBytes.begin(), result.canonicalBytes.end(),
                  canonicalBytes.begin(), canonicalBytes.end())) {
    return failure(
        SceneChangeValidationError::CanonicalDigestMismatch,
        "change set or byte stream is not the unique canonical representation");
  }
  return {};
}

SceneDigest SceneChangeCanonicalizer::computeSha256(
    std::span<const std::uint8_t> bytes) noexcept {
  const std::array parts{bytes};
  return computeSha256Parts(parts);
}

SceneDigest SceneChangeCanonicalizer::computeSha256Parts(
    std::span<const std::span<const std::uint8_t>> parts) noexcept {
#if defined(_WIN32)
  SceneDigest digest{};
  if (nativeSha256(parts, digest)) return digest;
#endif
  Sha256State state;
  for (const auto bytes : parts) state.update(bytes);
  return state.finish();
}

std::string SceneChangeCanonicalizer::encodeBase64(std::span<const std::uint8_t> bytes) {
  static constexpr char alphabet[] =
      "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
  if (bytes.size() / 3u >= std::string{}.max_size() / 4u)
    throw std::length_error("Base64 input is too large");
  std::string encoded(((bytes.size() + 2u) / 3u) * 4u, '=');
#if defined(_WIN32)
  if (!bytes.empty() && bytes.size() <= std::numeric_limits<DWORD>::max() &&
      encoded.size() < std::numeric_limits<DWORD>::max()) {
    DWORD capacity = static_cast<DWORD>(encoded.size() + 1u);
    if (CryptBinaryToStringA(bytes.data(), static_cast<DWORD>(bytes.size()),
                            CRYPT_STRING_BASE64 | CRYPT_STRING_NOCRLF,
                            encoded.data(), &capacity))
      return encoded;
    std::fill(encoded.begin(), encoded.end(), '=');
  }
#endif
  char* output = encoded.data();
  const auto* input = bytes.data();
  for (std::size_t index = 0; index < bytes.size() / 3u; ++index) {
    const std::uint32_t value = (std::uint32_t{input[0]} << 16u)
        | (std::uint32_t{input[1]} << 8u) | input[2];
    output[0] = alphabet[(value >> 18u) & 0x3fu];
    output[1] = alphabet[(value >> 12u) & 0x3fu];
    output[2] = alphabet[(value >> 6u) & 0x3fu];
    output[3] = alphabet[value & 0x3fu];
    input += 3;
    output += 4;
  }
  const auto remainder = bytes.size() % 3u;
  if (remainder != 0u) {
    const std::uint32_t value = (std::uint32_t{input[0]} << 16u)
        | (remainder == 2u ? std::uint32_t{input[1]} << 8u : 0u);
    output[0] = alphabet[(value >> 18u) & 0x3fu];
    output[1] = alphabet[(value >> 12u) & 0x3fu];
    if (remainder == 2u) output[2] = alphabet[(value >> 6u) & 0x3fu];
  }
  return encoded;
}

} // namespace demo::editor
