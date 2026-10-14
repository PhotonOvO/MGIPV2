#include <mgif/scene_editor/SceneProjection.h>

#include "SceneProjectionDetail.h"

#include <mgif/scene_editor/GltfSemanticCompare.h>
#include <mgif/scene_editor/SceneChangeCanonicalizer.h>

#include "../../loader/GltfLoader.h"
#include "../../SceneAssetBuilder.h"

#include <algorithm>
#include <array>
#include <bit>
#include <cmath>
#include <exception>
#include <limits>
#include <tuple>
#include <type_traits>
#include <utility>

#include <glm/gtc/matrix_transform.hpp>
#include <glm/gtc/quaternion.hpp>
#include <glm/gtc/type_ptr.hpp>

namespace demo::editor {
namespace {

class ProjectionEncoder final {
public:
  template <typename T> void integer(T value) {
    using U = std::make_unsigned_t<T>;
    std::uint64_t bits = static_cast<std::uint64_t>(static_cast<U>(value));
    for (std::size_t index = 0; index < sizeof(T); ++index) {
      m_bytes.push_back(static_cast<std::uint8_t>(bits & U{0xff}));
      bits >>= 8u;
    }
  }

  void boolean(bool value) { integer<std::uint8_t>(value ? 1u : 0u); }
  void real(float value) { integer(std::bit_cast<std::uint32_t>(value)); }
  void real(double value) { integer(std::bit_cast<std::uint64_t>(value)); }

  void string(const std::string &value) {
    integer<std::uint64_t>(value.size());
    m_bytes.insert(m_bytes.end(), value.begin(), value.end());
  }

  void bytes(const std::vector<std::uint8_t> &value) {
    integer<std::uint64_t>(value.size());
    m_bytes.insert(m_bytes.end(), value.begin(), value.end());
  }

  void bytes(const ImmutableByteBuffer& value) {
    integer<std::uint64_t>(value.size());
    flush();
    if (!value.empty()) m_parts.push_back(value);
  }

  void raw(std::span<const std::uint8_t> value) {
    m_bytes.insert(m_bytes.end(), value.begin(), value.end());
  }

  [[nodiscard]] ImmutableByteBuffer take() && {
    flush();
    return ImmutableByteBuffer::concatenate(std::move(m_parts));
  }

private:
  void flush() {
    if (!m_bytes.empty()) {
      m_parts.emplace_back(std::move(m_bytes));
      m_bytes = {};
    }
  }
  std::vector<std::uint8_t> m_bytes;
  std::vector<ImmutableByteBuffer> m_parts;
};

template <typename T, typename Function>
void collection(ProjectionEncoder &encoder, const std::vector<T> &values,
                Function &&write) {
  encoder.integer<std::uint64_t>(values.size());
  for (const T &value : values) {
    write(value);
  }
}

void vector3(ProjectionEncoder &encoder, const glm::vec3 &value) {
  encoder.real(value.x);
  encoder.real(value.y);
  encoder.real(value.z);
}

void vector4(ProjectionEncoder &encoder, const glm::vec4 &value) {
  encoder.real(value.x);
  encoder.real(value.y);
  encoder.real(value.z);
  encoder.real(value.w);
}

void quaternion(ProjectionEncoder &encoder, const glm::quat &value) {
  encoder.real(value.x);
  encoder.real(value.y);
  encoder.real(value.z);
  encoder.real(value.w);
}

void matrix4(ProjectionEncoder &encoder, const glm::mat4 &value) {
  const float *data = glm::value_ptr(value);
  for (std::size_t index = 0; index < 16u; ++index) {
    encoder.real(data[index]);
  }
}

void stableId(ProjectionEncoder &encoder, EditorObjectId id) {
  encoder.integer(static_cast<std::uint8_t>(id.kind));
  encoder.integer(id.authority.value);
  encoder.integer(id.slot);
  encoder.integer(id.generation);
}

[[nodiscard]] std::string digestHex(const SceneDigest &digest) {
  static constexpr char digits[] = "0123456789abcdef";
  std::string result;
  result.reserve(digest.size() * 2u);
  for (std::uint8_t byte : digest) {
    result.push_back(digits[byte >> 4u]);
    result.push_back(digits[byte & 0x0fu]);
  }
  return result;
}

[[nodiscard]] ImmutableByteBuffer canonicalProjectionBytes(
    const SceneAsset &asset,
    const std::vector<SceneRuntimeMappingEntry> &mappingEntries,
    const std::vector<SceneRuntimeMappingAlias> &mappingAliases,
    EditorDocumentAuthority authority, const SceneDigest &documentDigest,
    std::uint64_t revision, std::optional<StableSceneId> activeScene,
    std::uint64_t activationEpoch, std::uint64_t rootSerial,
    std::uint64_t mappingEpoch, std::optional<std::uint32_t> activeVariant,
    std::optional<std::uint32_t> activeAnimation, double animationTime,
    bool animationLoop) {
  ProjectionEncoder encoder;
  constexpr std::array<std::uint8_t, 8> magic{'M', 'G', 'I', 'F',
                                              'P', 'R', 'J', '2'};
  encoder.raw(magic);
  encoder.integer<std::uint32_t>(2u);
  encoder.integer(authority.value);
  encoder.integer(revision);
  encoder.boolean(activeScene.has_value());
  if (activeScene) {
    stableId(encoder, editorObjectId(*activeScene));
  }
  encoder.integer(activationEpoch);
  encoder.integer(rootSerial);
  encoder.integer(mappingEpoch);
  encoder.boolean(activeVariant.has_value());
  if (activeVariant) {
    encoder.integer(*activeVariant);
  }
  encoder.boolean(activeAnimation.has_value());
  if (activeAnimation) {
    encoder.integer(*activeAnimation);
    encoder.real(animationTime);
    encoder.boolean(animationLoop);
  }
  encoder.raw(documentDigest);
  encoder.string(asset.name);
  encoder.string(asset.sourcePath);
  encoder.string(asset.documentSemanticHash);
  encoder.integer(asset.documentRevision);
  vector3(encoder, asset.sceneRootTransform.translation);
  quaternion(encoder, asset.sceneRootTransform.rotation);
  vector3(encoder, asset.sceneRootTransform.scale);
  matrix4(encoder, asset.sceneRootTransform.matrix);
  encoder.boolean(asset.sceneRootTransform.frozen);
  encoder.integer(asset.sceneRootTransform.hierarchyEpoch);

  collection(encoder, asset.meshes, [&](const SceneMesh &mesh) {
    encoder.integer(mesh.vertexOffset);
    encoder.integer(mesh.vertexCount);
    encoder.integer(mesh.indexOffset);
    encoder.integer(mesh.indexCount);
    encoder.integer(mesh.materialIndex);
    vector3(encoder, mesh.localBoundsMin);
    vector3(encoder, mesh.localBoundsMax);
    encoder.integer(mesh.alphaMode);
    encoder.real(mesh.alphaCutoff);
    matrix4(encoder, mesh.exportTransform);
    encoder.integer(mesh.sourceNodeIndex);
    encoder.integer(mesh.sourceMeshIndex);
    encoder.integer(mesh.sourcePrimitiveIndex);
    encoder.integer(mesh.runtimeIdentity);
    encoder.boolean(mesh.localVisible);
    encoder.boolean(mesh.effectiveVisible);
  });
  collection(encoder, asset.primitives, [&](const ScenePrimitive &primitive) {
    encoder.integer(primitive.meshIndex);
    encoder.integer(primitive.nodeIndex);
    encoder.integer(primitive.documentMeshIndex);
    encoder.integer(primitive.documentPrimitiveIndex);
    encoder.integer(primitive.materialIndex);
    encoder.boolean(primitive.localVisible);
    encoder.boolean(primitive.effectiveVisible);
    encoder.integer(primitive.runtimeIdentity);
    collection(encoder, primitive.variants,
               [&](const ScenePrimitiveVariant &variant) {
                 encoder.integer(variant.materialIndex);
                 collection(encoder, variant.variantIndices,
                            [&](std::uint32_t index) { encoder.integer(index); });
               });
    encoder.string(primitive.extensionsJson);
    encoder.string(primitive.extrasJson);
  });
  collection(encoder, asset.materials, [&](const SceneMaterial &material) {
    vector4(encoder, material.baseColorFactor);
    encoder.real(material.metallicFactor);
    encoder.real(material.roughnessFactor);
    encoder.real(material.normalScale);
    encoder.real(material.occlusionStrength);
    vector3(encoder, material.emissiveFactor);
    encoder.integer(material.baseColorTexture);
    encoder.integer(material.normalTexture);
    encoder.integer(material.metallicRoughnessTexture);
    encoder.integer(material.occlusionTexture);
    encoder.integer(material.emissiveTexture);
    encoder.integer(material.alphaMode);
    encoder.real(material.alphaCutoff);
    encoder.integer(material.materialWorkflow);
    encoder.boolean(material.doubleSided);
    encoder.string(material.name);
    encoder.string(material.extensionsJson);
    encoder.string(material.extrasJson);
  });
  collection(encoder, asset.textures, [&](const SceneTexture &texture) {
    encoder.integer(texture.width);
    encoder.integer(texture.height);
    encoder.integer(texture.mipLevels);
    encoder.integer(
        static_cast<std::underlying_type_t<decltype(texture.format)>>(
            texture.format));
    encoder.integer(texture.payloadOffset);
    encoder.integer(texture.payloadSize);
    encoder.boolean(texture.isKtx2);
    encoder.string(texture.uri);
  });
  collection(encoder, asset.nodes, [&](const SceneNode &node) {
    encoder.string(node.name);
    encoder.integer(node.parent);
    collection(encoder, node.children,
               [&](std::uint32_t child) { encoder.integer(child); });
    collection(encoder, node.meshRefs,
               [&](std::uint32_t mesh) { encoder.integer(mesh); });
    vector3(encoder, node.translation);
    quaternion(encoder, node.rotation);
    vector3(encoder, node.scale);
    matrix4(encoder, node.localTransform);
    matrix4(encoder, node.worldTransform);
    encoder.integer(node.sourceNodeIndex);
    encoder.integer(node.runtimeIdentity);
    encoder.boolean(node.localVisible);
    encoder.boolean(node.effectiveVisible);
    encoder.string(node.extensionsJson);
    encoder.string(node.extrasJson);
  });
  collection(encoder, asset.cameras, [&](const SceneCamera &camera) {
    encoder.string(camera.name);
    encoder.integer(camera.nodeIndex);
    encoder.integer(static_cast<std::underlying_type_t<SceneCameraType>>(
        camera.type));
    encoder.real(camera.aspectRatio);
    encoder.real(camera.verticalFieldOfViewRadians);
    encoder.real(camera.nearPlane);
    encoder.real(camera.farPlane);
    encoder.real(camera.horizontalMagnification);
    encoder.real(camera.verticalMagnification);
    encoder.integer(camera.sourceCameraIndex);
    encoder.integer(camera.runtimeIdentity);
  });
  collection(encoder, asset.lights, [&](const SceneLight &light) {
    encoder.string(light.name);
    encoder.integer(light.nodeIndex);
    encoder.integer(
        static_cast<std::underlying_type_t<SceneLightType>>(light.type));
    encoder.boolean(light.enabled);
    vector3(encoder, light.color);
    encoder.real(light.intensity);
    encoder.real(light.range);
    encoder.real(light.innerConeAngle);
    encoder.real(light.outerConeAngle);
    encoder.integer(light.sourceLightIndex);
    encoder.integer(light.runtimeIdentity);
  });
  collection(encoder, asset.rootNodes,
             [&](std::uint32_t node) { encoder.integer(node); });
  encoder.bytes(asset.sceneRootTransformApplyToRoot);
  collection(encoder, asset.dependencies, [&](const SceneDependency &dependency) {
    encoder.string(dependency.relativePath);
    encoder.integer(dependency.fileSize);
    encoder.integer(dependency.writeTimeTicks);
    encoder.integer(dependency.pathHash);
  });
  collection(encoder, asset.materialVariantNames,
             [&](const std::string &name) { encoder.string(name); });
  collection(encoder, asset.animations, [&](const SceneAnimation &animation) {
    encoder.string(animation.name);
    collection(encoder, animation.samplers,
               [&](const SceneAnimationSampler &sampler) {
                 encoder.integer(sampler.inputAccessor);
                 encoder.integer(sampler.outputAccessor);
                 encoder.string(sampler.interpolation);
               });
    collection(encoder, animation.channels,
               [&](const SceneAnimationChannel &channel) {
                 encoder.integer(channel.documentNodeIndex);
                 encoder.integer(channel.projectedNodeIndex);
                 encoder.integer(channel.samplerIndex);
                 encoder.string(channel.path);
               });
  });
  encoder.bytes(asset.vertexPayload);
  encoder.bytes(asset.indexPayload);
  encoder.bytes(asset.texturePayload);
  collection(encoder, asset.meshletPayloads,
             [&](const SceneAsset::MeshletPayload &payload) {
               encoder.bytes(payload.meshletData);
               encoder.bytes(payload.indexData);
               encoder.bytes(payload.vertexData);
             });
  collection(encoder, mappingEntries,
             [&](const SceneRuntimeMappingEntry &entry) {
               stableId(encoder, entry.documentId);
               encoder.integer(entry.sceneAssetIndex);
               encoder.integer(entry.projectionIdentity);
             });
  collection(encoder, mappingAliases,
             [&](const SceneRuntimeMappingAlias &alias) {
               stableId(encoder, alias.documentId);
               encoder.integer(static_cast<std::uint8_t>(alias.kind));
               encoder.integer(alias.sceneAssetIndex);
             });
  return std::move(encoder).take();
}

[[nodiscard]] bool finalizeProjection(
    const GltfDocument &document, SceneProjectionResult &result,
    std::vector<SceneRuntimeMappingEntry> mappingEntries,
    std::vector<SceneRuntimeMappingAlias> mappingAliases) {
  const std::uint64_t rootSerial =
      result.sceneRootTransform.externalStateSerial;
  std::sort(mappingAliases.begin(), mappingAliases.end(),
            [](const SceneRuntimeMappingAlias &left,
               const SceneRuntimeMappingAlias &right) {
              const auto leftKey = std::tuple{
                  static_cast<std::uint8_t>(left.kind), left.sceneAssetIndex,
                  static_cast<std::uint8_t>(left.documentId.kind),
                  left.documentId.authority.value, left.documentId.slot,
                  left.documentId.generation};
              const auto rightKey = std::tuple{
                  static_cast<std::uint8_t>(right.kind), right.sceneAssetIndex,
                  static_cast<std::uint8_t>(right.documentId.kind),
                  right.documentId.authority.value, right.documentId.slot,
                  right.documentId.generation};
              return leftKey < rightKey;
            });
  ImmutableByteBuffer bytes = canonicalProjectionBytes(
      result.asset, mappingEntries, mappingAliases,
      document.documentAuthority(), result.documentDigest,
      result.documentRevision, result.activeScene, result.sceneActivationEpoch,
      rootSerial, result.mappingEpoch, result.activeVariant,
      result.activeAnimation, result.animationTime, result.animationLoop);
  ProjectionEncoder bindingEncoder;
  collection(bindingEncoder, result.textureBindings,
             [&](const SceneTextureProjectionBinding &binding) {
               stableId(bindingEncoder, editorObjectId(binding.textureId));
               stableId(bindingEncoder, editorObjectId(binding.sourceImageId));
               bindingEncoder.integer(binding.sourceImageIndex);
               bindingEncoder.integer(binding.sceneAssetIndex);
               bindingEncoder.integer(binding.colorSceneAssetIndex);
               bindingEncoder.integer(binding.linearSceneAssetIndex);
               bindingEncoder.integer(binding.samplerIndex);
               bindingEncoder.boolean(binding.samplerId.has_value());
               if (binding.samplerId) {
                 stableId(bindingEncoder, editorObjectId(*binding.samplerId));
               }
             });
  collection(bindingEncoder, result.samplerBindings,
             [&](const SceneSamplerProjectionBinding &binding) {
               stableId(bindingEncoder, editorObjectId(binding.samplerId));
               bindingEncoder.integer(binding.textureUseCount);
             });
  ImmutableByteBuffer bindingBytes =
      std::move(bindingEncoder).take();
  bytes = ImmutableByteBuffer::concatenate({std::move(bytes), std::move(bindingBytes)});
  result.projectionDigest = SceneChangeCanonicalizer::computeSha256Parts(bytes.chunks());

  SceneRuntimeMappingFence fence;
  fence.authority = document.documentAuthority();
  fence.appliedDocumentRevision = result.documentRevision;
  fence.activeScene = result.activeScene;
  fence.sceneActivationEpoch = result.sceneActivationEpoch;
  fence.rootExternalStateSerial = rootSerial;
  fence.projectionDigest = result.projectionDigest;
  fence.mappingEpoch = result.mappingEpoch;
  std::string mappingError;
  auto mapping = SceneRuntimeMapping::create(
      std::move(fence), std::move(mappingEntries), std::move(mappingAliases),
      &mappingError);
  if (!mapping) {
    result.error = mappingError.empty()
                       ? "failed to create immutable projection mapping"
                       : std::move(mappingError);
    return false;
  }
  result.mapping = std::move(*mapping);
  if (!result.mapping.validate(document, result.asset, &mappingError)) {
    result.error = mappingError.empty() ? "projection mapping invariant failed"
                                        : std::move(mappingError);
    return false;
  }

  result.occurrenceNodes.clear();
  result.primitiveOccurrences.clear();
  const std::optional<SceneId> occurrenceScene =
      result.activeScene
          ? SceneId::from(editorObjectId(*result.activeScene))
          : std::nullopt;
  if (occurrenceScene) {
    struct PendingNode final {
      std::uint32_t index{0};
      std::uint32_t depth{0};
    };
    std::vector<PendingNode> pending;
    pending.reserve(result.asset.nodes.size());
    for (auto root = result.asset.rootNodes.rbegin();
         root != result.asset.rootNodes.rend(); ++root) {
      pending.push_back({*root, 0u});
    }
    std::vector<std::uint8_t> visited(result.asset.nodes.size(), 0u);
    while (!pending.empty()) {
      const PendingNode current = pending.back();
      pending.pop_back();
      if (current.index >= result.asset.nodes.size() ||
          visited[current.index] != 0u) {
        result.error = "active scene node topology is not a canonical tree";
        return false;
      }
      visited[current.index] = 1u;
      const std::optional<EditorObjectId> rawNode =
          result.mapping.documentIdAt(EditorObjectKind::node, current.index);
      const std::optional<NodeId> node =
          rawNode ? NodeId::from(*rawNode) : std::nullopt;
      if (!node) {
        result.error = "active scene node occurrence has no stable identity";
        return false;
      }
      std::optional<NodeId> parent;
      const int parentIndex = result.asset.nodes[current.index].parent;
      if (parentIndex >= 0) {
        const auto rawParent = result.mapping.documentIdAt(
            EditorObjectKind::node, static_cast<std::uint32_t>(parentIndex));
        parent = rawParent ? NodeId::from(*rawParent) : std::nullopt;
        if (!parent) {
          result.error = "active scene node parent has no stable identity";
          return false;
        }
      }
      result.occurrenceNodes.push_back(
          SceneProjectionNodeOccurrence{*node, parent, current.depth});
      const auto &children = result.asset.nodes[current.index].children;
      for (auto child = children.rbegin(); child != children.rend(); ++child) {
        pending.push_back({*child, current.depth + 1u});
      }
    }
    if (std::any_of(visited.begin(), visited.end(),
                    [](std::uint8_t value) { return value == 0u; })) {
      result.error =
          "active scene node topology omits one or more projected nodes";
      return false;
    }

    result.primitiveOccurrences.reserve(result.asset.primitives.size());
    for (std::uint32_t primitiveIndex = 0;
         primitiveIndex < result.asset.primitives.size(); ++primitiveIndex) {
      const ScenePrimitive &primitive =
          result.asset.primitives[primitiveIndex];
      if (primitive.nodeIndex >= result.asset.nodes.size() ||
          primitive.nodeIndex >= visited.size()) {
        result.error =
            "active scene primitive occurrence has an invalid node coordinate";
        return false;
      }
      const auto rawNode = result.mapping.documentIdAt(
          EditorObjectKind::node, primitive.nodeIndex);
      const auto rawPrimitive = result.mapping.documentIdAt(
          EditorObjectKind::primitive, primitiveIndex);
      const std::optional<NodeId> node =
          rawNode ? NodeId::from(*rawNode) : std::nullopt;
      const std::optional<PrimitiveId> primitiveId =
          rawPrimitive ? PrimitiveId::from(*rawPrimitive) : std::nullopt;
      if (!node || !primitiveId || visited[primitive.nodeIndex] == 0u) {
        result.error =
            "active scene primitive occurrence has no canonical node identity";
        return false;
      }
      std::uint32_t ordinal = 0u;
      for (const SceneProjectionPrimitiveOccurrence &previous :
           result.primitiveOccurrences) {
        if (previous.key.scene == *occurrenceScene &&
            previous.key.node == *node &&
            previous.key.primitive == *primitiveId) {
          ++ordinal;
        }
      }
      result.primitiveOccurrences.push_back(
          SceneProjectionPrimitiveOccurrence{
              PrimitiveOccurrenceKey{*occurrenceScene, *node, *primitiveId,
                                     ordinal},
              primitiveIndex, primitive.effectiveVisible});
    }
  }

  result.immutableSnapshot.authority = document.documentAuthority();
  result.immutableSnapshot.documentRevision = result.documentRevision;
  result.immutableSnapshot.sceneActivationEpoch = result.sceneActivationEpoch;
  result.immutableSnapshot.rootExternalStateSerial = rootSerial;
  result.immutableSnapshot.activeScene = result.activeScene;
  result.immutableSnapshot.documentDigest = result.documentDigest;
  result.immutableSnapshot.projectionDigest = result.projectionDigest;
  result.immutableSnapshot.contentDigest = result.projectionDigest;
  result.immutableSnapshot.bytes = std::move(bytes);
  return true;
}

[[nodiscard]] bool sameRootState(const SceneRootTransformState &left,
                                 const SceneRootTransformState &right) noexcept {
  return left.baseline == right.baseline && left.current == right.current &&
         left.hierarchyEpoch == right.hierarchyEpoch &&
         left.externalStateSerial == right.externalStateSerial &&
         left.frozen == right.frozen;
}

[[nodiscard]] std::optional<StableSceneId>
activeSceneFor(const GltfDocument &document) noexcept {
  const std::optional<SceneId> activeScene = document.activeSceneId();
  return activeScene
             ? std::optional<StableSceneId>{stableSceneId(activeScene->untyped())}
             : std::nullopt;
}

[[nodiscard]] bool sameObjectMembership(
    const GltfDocument &document,
    const SceneRuntimeMapping &mapping) noexcept {
  constexpr std::size_t kindCount =
      static_cast<std::size_t>(EditorObjectKind::count);
  std::size_t expectedEntries = 0u;
  for (std::size_t rawKind = 1u; rawKind < kindCount; ++rawKind) {
    const auto kind = static_cast<EditorObjectKind>(rawKind);
    const std::size_t count = document.objectCount(kind);
    expectedEntries += count;
    for (std::size_t index = 0u; index < count; ++index) {
      if (!mapping.entry(document.idAt(kind, index))) {
        return false;
      }
    }
  }
  return expectedEntries == mapping.entries().size();
}

[[nodiscard]] bool animationPoseMatchesProjection(
    const SceneProjectionResult &previous,
    const SceneProjectionOptions &options) noexcept {
  if (options.activeAnimation != previous.activeAnimation ||
      options.animationTime != previous.animationTime ||
      options.animationLoop != previous.animationLoop) {
    return false;
  }

  if (!options.activeAnimation) {
    return !options.animationPose;
  }

  // A runtime pose is only safe for the name-only path when its identity and
  // every supplied local component are represented by the prior projection.
  // Rejecting duplicate or malformed samples keeps the fast path from making
  // assumptions about the order-dependent full pose application.
  if (!options.animationPose ||
      options.animationPose->animationIndex != *options.activeAnimation ||
      options.animationPose->time != options.animationTime ||
      options.animationPose->loop != options.animationLoop ||
      !std::isfinite(options.animationPose->time)) {
    return false;
  }

  for (std::size_t sampleIndex = 0u;
       sampleIndex < options.animationPose->transforms.size();
       ++sampleIndex) {
    const AnimationTransformSample &sample =
        options.animationPose->transforms[sampleIndex];
    for (std::size_t priorIndex = 0u; priorIndex < sampleIndex;
         ++priorIndex) {
      if (options.animationPose->transforms[priorIndex].nodeIndex ==
          sample.nodeIndex) {
        return false;
      }
    }

    const auto projected = std::find_if(
        previous.asset.nodes.begin(), previous.asset.nodes.end(),
        [&](const SceneNode &node) {
          return node.sourceNodeIndex == sample.nodeIndex;
        });
    if (projected == previous.asset.nodes.end()) {
      return false;
    }
    if (sample.translation) {
      for (std::size_t component = 0u; component < 3u; ++component) {
        if (!std::isfinite((*sample.translation)[component]) ||
            projected->translation[component] !=
                static_cast<float>((*sample.translation)[component])) {
          return false;
        }
      }
    }
    if (sample.scale) {
      for (std::size_t component = 0u; component < 3u; ++component) {
        if (!std::isfinite((*sample.scale)[component]) ||
            projected->scale[component] !=
                static_cast<float>((*sample.scale)[component])) {
          return false;
        }
      }
    }
    if (sample.rotation) {
      double lengthSquared = 0.0;
      for (const double value : *sample.rotation) {
        if (!std::isfinite(value)) {
          return false;
        }
        lengthSquared += value * value;
      }
      if (!std::isfinite(lengthSquared) || lengthSquared <= 1.0e-20) {
        return false;
      }
      const glm::quat expected(
          static_cast<float>((*sample.rotation)[3]),
          static_cast<float>((*sample.rotation)[0]),
          static_cast<float>((*sample.rotation)[1]),
          static_cast<float>((*sample.rotation)[2]));
      const glm::quat normalized = glm::normalize(expected);
      if (!std::isfinite(normalized.x) || !std::isfinite(normalized.y) ||
          !std::isfinite(normalized.z) || !std::isfinite(normalized.w) ||
          projected->rotation != normalized) {
        return false;
      }
    }
  }
  return true;
}

[[nodiscard]] std::optional<std::vector<SceneRuntimeMappingEntry>>
canonicalMappingEntryOrder(const GltfDocument &document,
                            const SceneRuntimeMapping &mapping) {
  constexpr std::size_t kindCount =
      static_cast<std::size_t>(EditorObjectKind::count);
  std::vector<SceneRuntimeMappingEntry> ordered;
  ordered.reserve(mapping.entries().size());
  for (std::size_t rawKind = 1u; rawKind < kindCount; ++rawKind) {
    const auto kind = static_cast<EditorObjectKind>(rawKind);
    for (std::size_t index = 0u; index < document.objectCount(kind); ++index) {
      const auto entry = mapping.entry(document.idAt(kind, index));
      if (!entry) {
        return std::nullopt;
      }
      ordered.push_back(*entry);
    }
  }
  return ordered;
}

[[nodiscard]] bool previousEnvelopeIsCoherent(
    const SceneProjectionResult &previous) noexcept {
  return previous.succeeded() && previous.mapping.authority().valid() &&
         previous.mapping.authority() == previous.immutableSnapshot.authority &&
         previous.documentRevision == previous.mapping.appliedDocumentRevision() &&
         previous.documentRevision == previous.immutableSnapshot.documentRevision &&
         previous.sceneActivationEpoch == previous.mapping.sceneActivationEpoch() &&
         previous.sceneActivationEpoch ==
             previous.immutableSnapshot.sceneActivationEpoch &&
         previous.activeScene == previous.mapping.activeScene() &&
         previous.activeScene == previous.immutableSnapshot.activeScene &&
         previous.sceneRootTransform.externalStateSerial ==
             previous.mapping.rootExternalStateSerial() &&
         previous.sceneRootTransform.externalStateSerial ==
             previous.immutableSnapshot.rootExternalStateSerial &&
         previous.mappingEpoch == previous.mapping.mappingEpoch() &&
         previous.documentDigest == previous.immutableSnapshot.documentDigest &&
         previous.projectionDigest == previous.mapping.projectionDigest() &&
          previous.projectionDigest ==
              previous.immutableSnapshot.projectionDigest &&
          previous.projectionDigest == previous.immutableSnapshot.contentDigest &&
          previous.asset.documentRevision == previous.documentRevision &&
          previous.asset.documentSemanticHash == previous.semanticHash &&
          sceneDigestPresent(previous.documentDigest) &&
          sceneDigestPresent(previous.projectionDigest) &&
          !previous.immutableSnapshot.bytes.empty();
}

[[nodiscard]] SceneRootTransformMetadata
toFloat(const SceneRootTransform &transform) {
  SceneRootTransformMetadata metadata;
  metadata.translation =
      glm::vec3(static_cast<float>(transform.translation[0]),
                static_cast<float>(transform.translation[1]),
                static_cast<float>(transform.translation[2]));
  metadata.rotation =
      glm::quat(static_cast<float>(transform.rotation[3]),
                static_cast<float>(transform.rotation[0]),
                static_cast<float>(transform.rotation[1]),
                static_cast<float>(transform.rotation[2]));
  metadata.scale = glm::vec3(static_cast<float>(transform.scale[0]),
                             static_cast<float>(transform.scale[1]),
                             static_cast<float>(transform.scale[2]));
  metadata.matrix = glm::translate(glm::mat4(1.0f), metadata.translation) *
                    glm::mat4_cast(metadata.rotation) *
                    glm::scale(glm::mat4(1.0f), metadata.scale);
  return metadata;
}

[[nodiscard]] bool applyAnimationPose(
    SceneAsset &asset, const std::vector<AnimationTransformSample> &samples,
    std::string &error) {
  for (const AnimationTransformSample &sample : samples) {
    if (sample.translation) {
      for (const double value : *sample.translation) {
        if (!std::isfinite(value)) {
          error = "animation translation contains a non-finite value";
          return false;
        }
      }
    }
    if (sample.scale) {
      for (const double value : *sample.scale) {
        if (!std::isfinite(value)) {
          error = "animation scale contains a non-finite value";
          return false;
        }
      }
    }
    if (sample.rotation) {
      double lengthSquared = 0.0;
      for (const double value : *sample.rotation) {
        if (!std::isfinite(value)) {
          error = "animation rotation contains a non-finite value";
          return false;
        }
        lengthSquared += value * value;
      }
      if (!std::isfinite(lengthSquared) || lengthSquared <= 1.0e-20) {
        error = "animation rotation has zero length";
        return false;
      }
    }

    bool found = false;
    for (SceneNode &node : asset.nodes) {
      if (node.sourceNodeIndex != sample.nodeIndex)
        continue;
      found = true;
      if (sample.translation) {
        node.translation = glm::vec3(
            static_cast<float>((*sample.translation)[0]),
            static_cast<float>((*sample.translation)[1]),
            static_cast<float>((*sample.translation)[2]));
      }
      if (sample.scale) {
        node.scale = glm::vec3(static_cast<float>((*sample.scale)[0]),
                               static_cast<float>((*sample.scale)[1]),
                               static_cast<float>((*sample.scale)[2]));
      }
      if (sample.rotation) {
        glm::quat rotation(
            static_cast<float>((*sample.rotation)[3]),
            static_cast<float>((*sample.rotation)[0]),
            static_cast<float>((*sample.rotation)[1]),
            static_cast<float>((*sample.rotation)[2]));
        const float length = glm::length(rotation);
        if (!std::isfinite(length) || length <= 1.0e-6F) {
          error = "animation rotation cannot be normalized";
          return false;
        }
        node.rotation = glm::normalize(rotation);
      }
      node.localTransform = glm::translate(glm::mat4(1.0F), node.translation) *
                            glm::mat4_cast(node.rotation) *
                            glm::scale(glm::mat4(1.0F), node.scale);
    }
    if (!found) {
      error = "animation channel targets a node outside the active projection";
      return false;
    }
  }

  // Projection nodes are emitted in parent-before-child order. Recompute in
  // that order so deep scenes do not depend on recursive traversal here.
  for (std::size_t index = 0; index < asset.nodes.size(); ++index) {
    SceneNode &node = asset.nodes[index];
    if (node.parent < 0) {
      node.worldTransform = node.localTransform;
    } else if (static_cast<std::size_t>(node.parent) < index) {
      node.worldTransform = asset.nodes[static_cast<std::size_t>(node.parent)]
                                .worldTransform * node.localTransform;
    } else {
      error = "animation projection contains an invalid parent order";
      return false;
    }
  }
  for (std::size_t nodeIndex = 0; nodeIndex < asset.nodes.size(); ++nodeIndex) {
    for (const std::uint32_t meshIndex : asset.nodes[nodeIndex].meshRefs) {
      if (meshIndex >= asset.meshes.size()) {
        error = "animation projection contains an invalid mesh reference";
        return false;
      }
      asset.meshes[meshIndex].exportTransform =
          asset.nodes[nodeIndex].worldTransform;
    }
  }
  return true;
}

} // namespace

SceneProjectionOwnerCapability SceneProjectionOwnerCapability::fromDocument(
    const GltfDocument& document, const SceneAsset& asset,
    const SceneRuntimeMapping& mapping, std::string* error) noexcept {
  try {
    std::string mappingError;
    if (!mapping.validate(document, asset, &mappingError)) {
      if (error != nullptr) {
        *error = mappingError.empty()
                     ? "projection owner mapping validation failed"
                     : std::move(mappingError);
      }
      return {};
    }

    auto state = std::make_shared<State>();
    state->mapping = mapping;
    const auto append = [&](EditorObjectKind kind, const auto& values,
                            std::vector<Association>& destination,
                            auto sourceIndex, auto runtimeIdentity) {
      destination.reserve(values.size());
      for (std::size_t index = 0; index < values.size(); ++index) {
        if (index > std::numeric_limits<std::uint32_t>::max()) {
          return false;
        }
        const std::optional<EditorObjectId> owner = mapping.documentIdAt(
            kind, static_cast<std::uint32_t>(index));
        if (!owner || owner->kind != kind || !document.contains(*owner)) {
          return false;
        }
        const std::optional<std::size_t> source = document.indexOf(*owner);
        if (!source || *source > std::numeric_limits<std::uint32_t>::max()) {
          return false;
        }
        const std::uint64_t identity =
            SceneRuntimeMapping::deriveProjectionIdentity(*owner);
        if (sourceIndex(values[index]) != static_cast<std::uint32_t>(*source) ||
            runtimeIdentity(values[index]) != identity) {
          return false;
        }
        const std::optional<SceneRuntimeMappingEntry> entry =
            mapping.entry(*owner);
        if (!entry ||
            entry->sceneAssetIndex == SceneRuntimeMapping::kInvalidIndex ||
            entry->projectionIdentity != identity ||
            entry->runtimeIndex != identity) {
          return false;
        }
        destination.push_back(Association{
            kind, static_cast<std::uint32_t>(index),
            static_cast<std::uint32_t>(*source), stableSceneId(*owner),
            identity});
      }
      return true;
    };

    if (!append(
            EditorObjectKind::camera, asset.cameras,
            state->cameraAssociations,
            [](const SceneCamera& value) noexcept {
              return value.sourceCameraIndex;
            },
            [](const SceneCamera& value) noexcept {
              return value.runtimeIdentity;
            }) ||
        !append(
            EditorObjectKind::light, asset.lights,
            state->lightAssociations,
            [](const SceneLight& value) noexcept {
              return value.sourceLightIndex;
            },
            [](const SceneLight& value) noexcept {
              return value.runtimeIdentity;
            })) {
      if (error != nullptr) {
        *error = "projection owner camera/light association is invalid";
      }
      return {};
    }
    if (error != nullptr) {
      error->clear();
    }
    return SceneProjectionOwnerCapability{std::move(state)};
  } catch (const std::bad_alloc&) {
    if (error != nullptr) {
      *error = "projection owner capability allocation failed";
    }
  } catch (...) {
    if (error != nullptr) {
      *error = "projection owner capability creation failed";
    }
  }
  return {};
}

bool SceneProjection::sealOwner(const GltfDocument& document,
                                SceneProjectionResult& result) noexcept {
  std::string capabilityError;
  result.ownerCapability = SceneProjectionOwnerCapability::fromDocument(
      document, result.asset, result.mapping, &capabilityError);
  if (result.ownerCapability.valid()) {
    return true;
  }
  result.error = capabilityError.empty()
                     ? "failed to bind immutable projection owner capability"
                     : std::move(capabilityError);
  return false;
}

SceneProjectionResult
SceneProjection::project(const GltfDocument &document,
                         const SceneProjectionOptions &options) {
  SceneProjectionResult result;
  try {
    if (options.baseMappingEpoch ==
        std::numeric_limits<std::uint64_t>::max()) {
      result.error = "projection mapping epoch is exhausted";
      return result;
    }
    result.mappingEpoch = options.baseMappingEpoch + 1u;
    result.activeVariant = options.activeVariant;
    result.activeAnimation = options.activeAnimation;
    result.animationTime = options.animationTime;
    result.animationLoop = options.animationLoop;
    result.documentRevision = document.revision();
    result.sceneActivationEpoch = document.sceneActivationEpoch();
    result.documentDigest = document.documentDigest();
    if (!sceneDigestPresent(result.documentDigest)) {
      result.error = "failed to compute the document SHA-256 digest";
      return result;
    }
    result.semanticHash = options.computeSemanticHash
                              ? GltfSemanticCompare::digest(document)
                              : std::string{};
    const std::optional<SceneId> activeScene = document.activeSceneId();
    result.activeScene = activeScene
                             ? std::optional<StableSceneId>{
                                   stableSceneId(activeScene->untyped())}
                             : std::nullopt;
    result.sceneRootTransform = activeScene
                                    ? document.sceneRootTransformState(*activeScene)
                                    : SceneRootTransformState{};

    GltfLoader decoder;
    GltfModel model;
    if (!decoder.decode(document, model)) {
      result.error = decoder.getLastError();
      if (result.error.empty()) {
        result.error = "failed to decode the document authority";
      }
      return result;
    }

    if (options.activeVariant &&
        *options.activeVariant >= model.materialVariantNames.size()) {
      result.error = "active material variant is outside the document variant set";
      return result;
    }

    SceneAssetBuilder::BuildOptions buildOptions;
    buildOptions.buildMeshlets = options.buildMeshlets;
    buildOptions.keepRawVertexData = options.keepRawVertexData;
    buildOptions.activeVariant = options.activeVariant;
    result.asset = SceneAssetBuilder::build(model, buildOptions);
    result.asset.documentSemanticHash = result.semanticHash;
    result.asset.documentRevision = result.documentRevision;
    result.asset.sceneRootTransform = toFloat(result.sceneRootTransform.current);
    result.asset.sceneRootTransform.frozen = result.sceneRootTransform.frozen;
    result.asset.sceneRootTransform.hierarchyEpoch =
        result.sceneRootTransform.hierarchyEpoch;
    result.asset.sceneRootTransformApplyToRoot.assign(
        result.asset.rootNodes.size(), 1u);

    if (options.animationPose) {
      if (options.animationPose->animationIndex >= model.animations.size()) {
        result.error = "active animation is outside the document animation set";
        return result;
      }
      std::string poseError;
      if (!applyAnimationPose(result.asset, options.animationPose->transforms,
                              poseError)) {
        result.error = poseError.empty() ? "failed to apply animation pose"
                                         : std::move(poseError);
        return result;
      }
      result.animationTime = options.animationPose->time;
    }

    if(model.textures.size() != document.objectCount(EditorObjectKind::texture)
       || model.samplers.size() != document.objectCount(EditorObjectKind::sampler)
       || model.images.size() < document.objectCount(EditorObjectKind::image))
    {
      result.error = "decoded texture/image/sampler counts do not match the document authority";
      return result;
    }
    result.textureBindings.reserve(model.textures.size());
    result.samplerBindings.reserve(model.samplers.size());
    for(std::size_t samplerIndex = 0; samplerIndex < model.samplers.size(); ++samplerIndex)
    {
      const EditorObjectId samplerId =
          document.idAt(EditorObjectKind::sampler, samplerIndex);
      if(!samplerId.valid())
      {
        result.error = "decoded sampler identity is invalid";
        return result;
      }
      result.samplerBindings.push_back(
          SceneSamplerProjectionBinding{stableSceneId(samplerId), 0u});
    }
    for(std::size_t textureIndex = 0; textureIndex < model.textures.size(); ++textureIndex)
    {
      const GltfTextureData& source = model.textures[textureIndex];
      const EditorObjectId textureId =
          document.idAt(EditorObjectKind::texture, textureIndex);
      if(!textureId.valid()
         || source.resolvedImageIndex < 0
         || static_cast<std::size_t>(source.resolvedImageIndex)
                >= document.objectCount(EditorObjectKind::image)
         || source.canonicalImageIndex < 0
         || static_cast<std::size_t>(source.canonicalImageIndex)
                >= result.asset.textures.size())
      {
        result.error = "decoded texture binding has an invalid image projection";
        return result;
      }
      const EditorObjectId imageId = document.idAt(
          EditorObjectKind::image,
          static_cast<std::size_t>(source.resolvedImageIndex));
      if(!imageId.valid())
      {
        result.error = "decoded texture binding has an invalid source image identity";
        return result;
      }
      const auto sceneIndex = [](int value) {
        return value >= 0 ? static_cast<std::uint32_t>(value)
                          : SceneRuntimeMapping::kInvalidIndex;
      };
      SceneTextureProjectionBinding binding;
      binding.textureId = stableSceneId(textureId);
      binding.sourceImageId = stableSceneId(imageId);
      binding.sourceImageIndex = static_cast<std::uint32_t>(source.resolvedImageIndex);
      binding.sceneAssetIndex = static_cast<std::uint32_t>(source.canonicalImageIndex);
      binding.colorSceneAssetIndex = sceneIndex(source.colorImageIndex);
      binding.linearSceneAssetIndex = sceneIndex(source.linearImageIndex);
      binding.samplerIndex = source.samplerIndex;
      if(source.samplerIndex >= 0
         && static_cast<std::size_t>(source.samplerIndex)
                < result.samplerBindings.size())
      {
        binding.samplerId = result.samplerBindings[
            static_cast<std::size_t>(source.samplerIndex)].samplerId;
        ++result.samplerBindings[static_cast<std::size_t>(source.samplerIndex)]
              .textureUseCount;
      }
      result.textureBindings.push_back(std::move(binding));
    }

    for (std::size_t root = 0; root < result.asset.rootNodes.size(); ++root) {
      if (result.asset.rootNodes[root] >= result.asset.nodes.size()) {
        result.error = "projected scene root index is out of bounds";
        return result;
      }
      const std::size_t sourceIndex =
          result.asset.nodes[result.asset.rootNodes[root]].sourceNodeIndex;
      const NodeId rootId = NodeId::from(
                                document.idAt(EditorObjectKind::node,
                                              sourceIndex))
                                .value_or(NodeId{});
      if (activeScene && rootId.valid() &&
          document.sceneRootNodeFrozen(*activeScene, rootId)) {
        result.asset.sceneRootTransformApplyToRoot[root] = 0u;
      }
    }

    constexpr std::size_t kindCount =
        static_cast<std::size_t>(EditorObjectKind::count);
    std::array<std::size_t, kindCount> entryOffsets{};
    std::vector<SceneRuntimeMappingEntry> mappingEntries;
    std::vector<SceneRuntimeMappingAlias> mappingAliases;
    std::size_t totalEntries = 0;
    for (std::size_t kind = 1; kind < kindCount; ++kind) {
      entryOffsets[kind] = totalEntries;
      totalEntries +=
          document.objectCount(static_cast<EditorObjectKind>(kind));
    }
    mappingEntries.reserve(totalEntries);
    for (std::size_t kind = 1; kind < kindCount; ++kind) {
      const auto type = static_cast<EditorObjectKind>(kind);
      for (std::size_t index = 0; index < document.objectCount(type); ++index) {
        mappingEntries.push_back(SceneRuntimeMappingEntry{
            document.idAt(type, index), SceneRuntimeMapping::kInvalidIndex,
            SceneRuntimeMapping::kInvalidProjectionIdentity,
            SceneRuntimeMapping::kInvalidRuntimeIndex});
      }
    }
    const auto bindProjected = [&](EditorObjectId id,
                                   std::uint32_t assetIndex) -> bool {
      const auto sourceIndex = document.indexOf(id);
      if (!sourceIndex) {
        return false;
      }
      const std::size_t entryIndex =
          entryOffsets[static_cast<std::size_t>(id.kind)] + *sourceIndex;
      if (entryIndex >= mappingEntries.size() ||
          mappingEntries[entryIndex].documentId != id) {
        return false;
      }
      auto &entry = mappingEntries[entryIndex];
      const std::uint64_t identity =
          SceneRuntimeMapping::deriveProjectionIdentity(id);
      if (entry.sceneAssetIndex == SceneRuntimeMapping::kInvalidIndex) {
        entry.sceneAssetIndex = assetIndex;
        entry.projectionIdentity = identity;
        entry.runtimeIndex = identity;
      } else {
        if (entry.sceneAssetIndex == assetIndex) {
          return false;
        }
        mappingAliases.push_back(
            SceneRuntimeMappingAlias{id, id.kind, assetIndex});
      }
      return entry.projectionIdentity == identity &&
             entry.runtimeIndex == identity;
    };

    for (std::size_t index = 0; index < result.asset.nodes.size(); ++index) {
      const GltfNodeData &source = model.nodes[index];
      if (source.documentNodeIndex < 0) {
        result.error = "projected node source identity is invalid";
        return result;
      }
      const NodeId id = NodeId::from(document.idAt(
                                      EditorObjectKind::node,
                                      static_cast<std::size_t>(
                                          source.documentNodeIndex)))
                            .value_or(NodeId{});
      if (!id.valid() || !bindProjected(id.untyped(),
                                        static_cast<std::uint32_t>(index))) {
        result.error = "failed to bind projected node identity";
        return result;
      }
      result.asset.nodes[index].runtimeIdentity =
          SceneRuntimeMapping::deriveProjectionIdentity(id.untyped());
    }
    for (std::size_t index = 0; index < result.asset.primitives.size();
         ++index) {
      ScenePrimitive &primitive = result.asset.primitives[index];
      const PrimitiveId id = document
                                 .primitiveIdAt(PrimitiveAddress{
                                     primitive.documentMeshIndex,
                                     primitive.documentPrimitiveIndex})
                                 .value_or(PrimitiveId{});
      if (!id.valid() ||
          !bindProjected(id.untyped(), static_cast<std::uint32_t>(index))) {
        result.error = "failed to bind projected primitive identity";
        return result;
      }
      primitive.runtimeIdentity =
          SceneRuntimeMapping::deriveProjectionIdentity(id.untyped());
    }
    for (std::size_t index = 0; index < result.asset.meshes.size(); ++index) {
      SceneMesh &mesh = result.asset.meshes[index];
      const MeshId id =
          MeshId::from(document.idAt(EditorObjectKind::mesh,
                                     mesh.sourceMeshIndex))
              .value_or(MeshId{});
      if (!id.valid() ||
          !bindProjected(id.untyped(), static_cast<std::uint32_t>(index))) {
        result.error = "failed to bind projected mesh identity";
        return result;
      }
      mesh.runtimeIdentity =
          SceneRuntimeMapping::deriveProjectionIdentity(id.untyped());
    }
    const auto bindOrdered = [&](EditorObjectKind kind, std::size_t count) {
      for (std::size_t index = 0; index < count; ++index) {
        const EditorObjectId id = document.idAt(kind, index);
        if (!id.valid() ||
            !bindProjected(id, static_cast<std::uint32_t>(index))) {
          return false;
        }
      }
      return true;
    };
    if (!bindOrdered(EditorObjectKind::material,
                     result.asset.materials.size()) ||
        !bindOrdered(EditorObjectKind::animation,
                     result.asset.animations.size())) {
      result.error = "failed to bind ordered projection identity";
      return result;
    }
    for(std::size_t index = 0; index < result.asset.textures.size(); ++index)
    {
      const SceneTexture& texture = result.asset.textures[index];
      if(texture.sourceImageIndex >= document.objectCount(EditorObjectKind::image))
      {
        result.error = "projected texture payload has an invalid source image";
        return result;
      }
      const EditorObjectId imageId = document.idAt(
          EditorObjectKind::image, texture.sourceImageIndex);
      if(!imageId.valid()
         || !bindProjected(imageId, static_cast<std::uint32_t>(index)))
      {
        result.error = "failed to bind projected image identity";
        return result;
      }
    }
    for (std::size_t index = 0; index < result.asset.cameras.size(); ++index) {
      if (index >= model.cameraDocumentIndices.size() ||
          model.cameraDocumentIndices[index] < 0) {
        result.error = "projected camera source identity is missing";
        return result;
      }
      const std::size_t sourceIndex = static_cast<std::size_t>(
          model.cameraDocumentIndices[index]);
      if (sourceIndex > std::numeric_limits<std::uint32_t>::max()) {
        result.error = "projected camera source identity exceeds asset limits";
        return result;
      }
      const EditorObjectId id = document.idAt(
          EditorObjectKind::camera, sourceIndex);
      if (!id.valid() ||
          !bindProjected(id, static_cast<std::uint32_t>(index))) {
        result.error = "failed to bind projected camera identity";
        return result;
      }
      SceneCamera &camera = result.asset.cameras[index];
      camera.sourceCameraIndex = static_cast<std::uint32_t>(sourceIndex);
      camera.runtimeIdentity =
          SceneRuntimeMapping::deriveProjectionIdentity(id);
    }
    for (std::size_t index = 0; index < result.asset.lights.size(); ++index) {
      if (index >= model.lightDocumentIndices.size() ||
          model.lightDocumentIndices[index] < 0) {
        result.error = "projected light source identity is missing";
        return result;
      }
      const std::size_t sourceIndex = static_cast<std::size_t>(
          model.lightDocumentIndices[index]);
      if (sourceIndex > std::numeric_limits<std::uint32_t>::max()) {
        result.error = "projected light source identity exceeds asset limits";
        return result;
      }
      const EditorObjectId id = document.idAt(
          EditorObjectKind::light, sourceIndex);
      if (!id.valid() ||
          !bindProjected(id, static_cast<std::uint32_t>(index))) {
        result.error = "failed to bind projected light identity";
        return result;
      }
      SceneLight &light = result.asset.lights[index];
      light.sourceLightIndex = static_cast<std::uint32_t>(sourceIndex);
      light.runtimeIdentity =
          SceneRuntimeMapping::deriveProjectionIdentity(id);
    }

    if (!finalizeProjection(document, result, std::move(mappingEntries),
                            std::move(mappingAliases))) {
      return result;
    }
    if (!sealOwner(document, result)) {
      return result;
    }
    return result;
  } catch (const std::bad_alloc &) {
    result.error = "projection allocation failed";
  } catch (const std::exception &exception) {
    result.error = std::string("projection failed: ") + exception.what();
  } catch (...) {
    result.error = "projection failed with an unknown error";
  }
  return result;
}

std::optional<SceneProjectionResult> detail::reprojectMaterialParameters(
    const GltfDocument& document, const SceneProjectionResult& previous,
    const SceneMutationEvidence& evidence,
    const SceneProjectionOptions& options) noexcept {
  try {
    if (!previousEnvelopeIsCoherent(previous) ||
        !previous.ownerCapability.validateMapping(previous.mapping) ||
        !evidence.structuralEvidenceComplete || evidence.fullSnapshot ||
        !evidence.stableIdRemap.empty() || evidence.exactChanges.empty() ||
        evidence.exactChanges.size() > 3u ||
        !evidence.sourceMutation.mutationToken.valid() ||
        previous.mapping.authority() != document.documentAuthority() ||
        previous.documentRevision == std::numeric_limits<std::uint64_t>::max() ||
        previous.documentRevision + 1u != document.revision() ||
        evidence.sourceMutation.baseDocumentRevision != previous.documentRevision ||
        evidence.sourceMutation.targetDocumentRevision != document.revision() ||
        evidence.sourceMutation.baseDocumentDigest != previous.documentDigest ||
        options.baseMappingEpoch != previous.mappingEpoch ||
        previous.mappingEpoch == std::numeric_limits<std::uint64_t>::max() ||
        document.sceneActivationEpoch() != previous.sceneActivationEpoch ||
        activeSceneFor(document) != previous.activeScene ||
        options.activeVariant != previous.activeVariant ||
        options.activeAnimation || previous.activeAnimation ||
        options.animationPose || !options.computeSemanticHash ||
        !options.includeInvisible || options.buildMeshlets ||
        options.keepRawVertexData ||
        !sameObjectMembership(document, previous.mapping)) {
      return std::nullopt;
    }
    const auto activeScene = document.activeSceneId();
    const auto rootState = activeScene ? document.sceneRootTransformState(*activeScene)
                                       : SceneRootTransformState{};
    if (!sameRootState(rootState, previous.sceneRootTransform)) return std::nullopt;

    const SceneFieldChange* parameterChange = nullptr;
    const SceneFieldChange* normalChange = nullptr;
    const SceneFieldChange* occlusionChange = nullptr;
    EditorObjectId target{};
    for (const SceneFieldChange& change : evidence.exactChanges) {
      if (change.target.kind != EditorObjectKind::material ||
          change.target.authority != document.documentAuthority() ||
          change.action != SceneChangeAction::Update ||
          !change.beforeValue || !change.afterValue)
        return std::nullopt;
      const EditorObjectId id = editorObjectId(change.target);
      if (target.valid() && target != id) return std::nullopt;
      target = id;
      if (change.changeClass == SceneChangeClass::MaterialParameters &&
          change.field == SceneField::MaterialParameters) {
        if (parameterChange) return std::nullopt;
        parameterChange = &change;
      } else if (change.changeClass == SceneChangeClass::MaterialBinding &&
                 change.field == SceneField::MaterialTextureBinding) {
        const auto* binding = std::get_if<SceneTextureBindingValue>(
            &*change.afterValue);
        if (!binding) return std::nullopt;
        if (binding->semantic == SceneTextureSemantic::Normal) {
          if (normalChange) return std::nullopt;
          normalChange = &change;
        } else if (binding->semantic == SceneTextureSemantic::Occlusion) {
          if (occlusionChange) return std::nullopt;
          occlusionChange = &change;
        } else {
          return std::nullopt;
        }
      } else {
        return std::nullopt;
      }
    }
    const auto material = MaterialId::from(target);
    const auto index = previous.mapping.sceneAssetIndex(target);
    if (!material || !index || *index >= previous.asset.materials.size())
      return std::nullopt;
    const SceneMaterial& prior = previous.asset.materials[*index];
    // Spec-gloss overrides the metallic-roughness factors during glTF decode.
    if (prior.materialWorkflow != 0 || document.materialHasExtension(
            *material, "KHR_materials_pbrSpecularGlossiness"))
      return std::nullopt;

    SceneMaterialParametersValue afterParameters;
    if (parameterChange) {
      const auto* before = std::get_if<SceneMaterialParametersValue>(
          &*parameterChange->beforeValue);
      const auto* after = std::get_if<SceneMaterialParametersValue>(
          &*parameterChange->afterValue);
      if (!before || !after ||
          before->alphaMode != after->alphaMode ||
          before->doubleSided != after->doubleSided ||
          document.materialParameters(*material) != *after)
        return std::nullopt;
      const auto toColor = [](const std::array<double, 4>& value) {
        return glm::vec4(static_cast<float>(value[0]),
                         static_cast<float>(value[1]),
                         static_cast<float>(value[2]),
                         static_cast<float>(value[3]));
      };
      const auto toEmissive = [](const std::array<double, 3>& value) {
        return glm::vec3(static_cast<float>(value[0]),
                         static_cast<float>(value[1]),
                         static_cast<float>(value[2]));
      };
      if (prior.baseColorFactor != toColor(before->baseColorFactor) ||
          prior.metallicFactor != static_cast<float>(before->metallicFactor) ||
          prior.roughnessFactor != static_cast<float>(before->roughnessFactor) ||
          prior.alphaMode != static_cast<int32_t>(before->alphaMode) ||
          prior.doubleSided != before->doubleSided ||
          prior.alphaCutoff != (before->alphaMode == SceneAlphaMode::Mask
              ? static_cast<float>(before->alphaCutoff) : 0.5f))
        return std::nullopt;
      if (document.materialHasExtension(*material,
              "KHR_materials_emissive_strength")) {
        if (before->emissiveFactor != after->emissiveFactor)
          return std::nullopt;
      } else if (prior.emissiveFactor != toEmissive(before->emissiveFactor)) {
        return std::nullopt;
      }
      afterParameters = *after;
    }

    const auto scalarAfter = [&](const SceneFieldChange* change,
                                 SceneTextureSemantic semantic,
                                 int32_t projectedTexture,
                                 float projectedScalar) -> std::optional<float> {
      if (!change) return projectedScalar;
      const auto* before = std::get_if<SceneTextureBindingValue>(
          &*change->beforeValue);
      const auto* after = std::get_if<SceneTextureBindingValue>(
          &*change->afterValue);
      if (!before || !after || before->semantic != semantic ||
          after->semantic != semantic || before->scalar == after->scalar)
        return std::nullopt;
      auto expected = *before;
      expected.scalar = after->scalar;
      if (expected != *after ||
          document.materialTextureBinding(*material, semantic) != *after ||
          (projectedTexture >= 0) != before->texture.has_value() ||
          projectedScalar != (projectedTexture >= 0
              ? static_cast<float>(before->scalar) : 1.0f))
        return std::nullopt;
      if (projectedTexture >= 0) {
        const auto binding = std::find_if(
            previous.textureBindings.begin(), previous.textureBindings.end(),
            [&](const SceneTextureProjectionBinding& entry) {
              return entry.textureId == *before->texture;
            });
        if (binding == previous.textureBindings.end() ||
            binding->samplerId != before->sampler ||
            binding->linearSceneAssetIndex !=
                static_cast<std::uint32_t>(projectedTexture) ||
            static_cast<std::size_t>(projectedTexture) >=
                previous.asset.textures.size() ||
            previous.asset.textures[projectedTexture].sourceImageIndex !=
                binding->sourceImageIndex)
          return std::nullopt;
      } else if (before->sampler) {
        return std::nullopt;
      }
      return projectedTexture >= 0
          ? static_cast<float>(after->scalar) : 1.0f;
    };
    const auto normalScale = scalarAfter(normalChange,
        SceneTextureSemantic::Normal, prior.normalTexture, prior.normalScale);
    const auto occlusionStrength = scalarAfter(occlusionChange,
        SceneTextureSemantic::Occlusion, prior.occlusionTexture,
        prior.occlusionStrength);
    if (!normalScale || !occlusionStrength) return std::nullopt;

    const SceneDigest digest = document.documentDigest();
    if (digest != evidence.sourceMutation.targetDocumentDigest ||
        !sceneDigestPresent(digest)) {
      return std::nullopt;
    }

    SceneProjectionResult result = previous;
    result.documentRevision = document.revision();
    result.documentDigest = digest;
    result.semanticHash = GltfSemanticCompare::digest(document);
    result.mappingEpoch = previous.mappingEpoch + 1u;
    result.asset.documentRevision = result.documentRevision;
    result.asset.documentSemanticHash = result.semanticHash;
    SceneMaterial& updated = result.asset.materials[*index];
    if (parameterChange) {
      const auto& color = afterParameters.baseColorFactor;
      updated.baseColorFactor = glm::vec4(
          static_cast<float>(color[0]), static_cast<float>(color[1]),
          static_cast<float>(color[2]), static_cast<float>(color[3]));
      updated.metallicFactor = static_cast<float>(afterParameters.metallicFactor);
      updated.roughnessFactor = static_cast<float>(afterParameters.roughnessFactor);
      if (!document.materialHasExtension(*material,
              "KHR_materials_emissive_strength")) {
        const auto& emissive = afterParameters.emissiveFactor;
        updated.emissiveFactor = glm::vec3(
            static_cast<float>(emissive[0]), static_cast<float>(emissive[1]),
            static_cast<float>(emissive[2]));
      }
      updated.alphaCutoff = afterParameters.alphaMode == SceneAlphaMode::Mask
          ? static_cast<float>(afterParameters.alphaCutoff) : 0.5f;
      if (updated.alphaCutoff != prior.alphaCutoff) {
        if (result.asset.primitives.size() != result.asset.meshes.size())
          return std::nullopt;
        for (std::size_t meshIndex = 0;
             meshIndex < result.asset.meshes.size(); ++meshIndex) {
          if (result.asset.primitives[meshIndex].materialIndex ==
                  static_cast<int32_t>(*index)) {
            SceneMesh& mesh = result.asset.meshes[meshIndex];
            if (mesh.alphaCutoff != prior.alphaCutoff)
              return std::nullopt;
            mesh.alphaCutoff = updated.alphaCutoff;
          }
        }
      }
    }
    updated.normalScale = *normalScale;
    updated.occlusionStrength = *occlusionStrength;
    auto entries = canonicalMappingEntryOrder(document, previous.mapping);
    if (!entries ||
        !finalizeProjection(
            document, result, std::move(*entries),
            std::vector<SceneRuntimeMappingAlias>(
                previous.mapping.aliases().begin(),
                previous.mapping.aliases().end())) ||
        !SceneProjection::sealOwner(document, result)) {
      return std::nullopt;
    }
    return result;
  } catch (...) {
    return std::nullopt;
  }
}

std::optional<SceneProjectionResult> detail::reprojectNodeNameOnly(
    const GltfDocument &document, const SceneProjectionResult &previous,
    NodeId node, const SceneProjectionOptions &options,
    const detail::NodeNameProjectionDigestCache &cached) noexcept {
  try {
    const bool basicEligible =
        node.valid() && node.authority() == document.documentAuthority() &&
        document.contains(node.untyped()) && previousEnvelopeIsCoherent(previous) &&
        previous.mapping.authority() == document.documentAuthority() &&
        previous.documentRevision != std::numeric_limits<std::uint64_t>::max() &&
        document.revision() == previous.documentRevision + 1u &&
        options.baseMappingEpoch == previous.mappingEpoch &&
        options.baseMappingEpoch != std::numeric_limits<std::uint64_t>::max() &&
        options.activeVariant == previous.activeVariant &&
        animationPoseMatchesProjection(previous, options) &&
        options.computeSemanticHash &&
        options.includeInvisible && !options.buildMeshlets && !options.keepRawVertexData &&
        document.sceneActivationEpoch() == previous.sceneActivationEpoch &&
        activeSceneFor(document) == previous.activeScene &&
        sameObjectMembership(document, previous.mapping) &&
        previous.asset.sourcePath == document.sourcePath().string() &&
        previous.asset.name == document.sourcePath().filename().string();
    if (!basicEligible) {
      return std::nullopt;
    }

    const std::optional<SceneId> activeScene = document.activeSceneId();
    const SceneRootTransformState rootState =
        activeScene ? document.sceneRootTransformState(*activeScene)
                    : SceneRootTransformState{};
    if (!sameRootState(rootState, previous.sceneRootTransform)) {
      return std::nullopt;
    }

    const auto nodeIndex = document.indexOf(node.untyped());
    if (!nodeIndex) {
      return std::nullopt;
    }
    const std::string_view nextName = document.objectName(node.untyped());
    const std::string projectedNextName =
        nextName.empty() ? "Node " + std::to_string(*nodeIndex)
                         : std::string(nextName);
    bool targetSeen = false;
    std::optional<std::string_view> priorTargetName;
    for (const SceneNode &projected : previous.asset.nodes) {
      if (projected.sourceNodeIndex >=
          document.objectCount(EditorObjectKind::node)) {
        return std::nullopt;
      }
      const EditorObjectId projectedId = document.idAt(
          EditorObjectKind::node, projected.sourceNodeIndex);
      if (!projectedId.valid()) {
        return std::nullopt;
      }
      const std::string_view authorityName = document.objectName(projectedId);
      if (projected.sourceNodeIndex == *nodeIndex) {
        targetSeen = true;
        if (projected.name == projectedNextName ||
            (priorTargetName && projected.name != *priorTargetName)) {
          return std::nullopt;
        }
        priorTargetName = projected.name;
      } else if (projected.name != authorityName) {
        return std::nullopt;
      }
    }
    if (!targetSeen || !priorTargetName) {
      return std::nullopt;
    }

    SceneProjectionResult result = previous;
    result.error.clear();
    result.mappingEpoch = options.baseMappingEpoch + 1u;
    result.documentRevision = document.revision();
    result.sceneActivationEpoch = document.sceneActivationEpoch();
    result.activeScene = activeSceneFor(document);
    result.sceneRootTransform = rootState;
    result.activeVariant = options.activeVariant;
    result.activeAnimation = options.activeAnimation;
    result.animationTime = options.animationTime;
    result.animationLoop = options.animationLoop;
    // optional::value_or evaluates its fallback eagerly. Keep the expensive
    // full-document digest off the validated cache-hit path.
    if (cached.documentDigest) {
      if (!sceneDigestPresent(*cached.documentDigest)) {
        return std::nullopt;
      }
      result.documentDigest = *cached.documentDigest;
    } else {
      result.documentDigest = document.documentDigest();
    }
    if (!sceneDigestPresent(result.documentDigest)) {
      return std::nullopt;
    }
    if (cached.semanticHash) {
      if (cached.semanticHash->empty()) {
        return std::nullopt;
      }
      result.semanticHash = *cached.semanticHash;
    } else {
      result.semanticHash = GltfSemanticCompare::digest(document);
    }
    if (result.semanticHash.empty()) {
      return std::nullopt;
    }
    result.asset.documentSemanticHash = result.semanticHash;
    result.asset.documentRevision = result.documentRevision;
    result.asset.sceneRootTransform = toFloat(rootState.current);
    result.asset.sceneRootTransform.frozen = rootState.frozen;
    result.asset.sceneRootTransform.hierarchyEpoch = rootState.hierarchyEpoch;
    for (SceneNode &projected : result.asset.nodes) {
      if (projected.sourceNodeIndex == *nodeIndex) {
        projected.name = projectedNextName;
      }
    }

    const auto orderedEntries =
        canonicalMappingEntryOrder(document, previous.mapping);
    if (!orderedEntries) {
      return std::nullopt;
    }
    std::vector<SceneRuntimeMappingEntry> mappingEntries = *orderedEntries;
    std::vector<SceneRuntimeMappingAlias> mappingAliases(
        previous.mapping.aliases().begin(), previous.mapping.aliases().end());
    if (!finalizeProjection(document, result, std::move(mappingEntries),
                            std::move(mappingAliases))) {
      return std::nullopt;
    }
    if (!SceneProjection::sealOwner(document, result)) {
      return std::nullopt;
    }
    return result;
  } catch (...) {
    return std::nullopt;
  }
}

bool SceneProjection::projectInto(const GltfDocument &document,
                                  SceneAsset &asset,
                                  SceneRuntimeMapping &mapping,
                                  std::string *error,
                                  const SceneProjectionOptions &options) {
  SceneProjectionResult result = project(document, options);
  if (!result) {
    if (error != nullptr) {
      *error = result.error;
    }
    return false;
  }
  asset = std::move(result.asset);
  mapping = std::move(result.mapping);
  if (error != nullptr) {
    error->clear();
  }
  return true;
}

std::string SceneProjection::digest(const SceneAsset &asset,
                                    const SceneRuntimeMapping &mapping) {
  (void)asset;
  return digestHex(mapping.projectionDigest());
}

} // namespace demo::editor
