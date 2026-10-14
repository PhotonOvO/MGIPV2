#pragma once

#include "../rhi/RHITypes.h"
#include "../common/ImmutableByteBuffer.h"
#include "SceneCamera.h"
#include "SceneLight.h"

#include <glm/gtc/quaternion.hpp>

#include <cstdint>
#include <limits>
#include <span>
#include <cassert>
#include <string>
#include <vector>

namespace demo {

struct SceneRootTransformMetadata {
  glm::vec3 translation = glm::vec3(0.0f);
  glm::quat rotation = glm::quat(1.0f, 0.0f, 0.0f, 0.0f);
  glm::vec3 scale = glm::vec3(1.0f);
  glm::mat4 matrix = glm::mat4(1.0f);
  bool frozen = false;
  uint64_t hierarchyEpoch = 0;
};

struct ScenePrimitiveVariant {
  int32_t materialIndex = -1;
  std::vector<uint32_t> variantIndices;
};

struct ScenePrimitive {
  uint32_t meshIndex = 0;
  uint32_t nodeIndex = 0;
  uint32_t documentMeshIndex = 0;
  uint32_t documentPrimitiveIndex = 0;
  int32_t materialIndex = -1;
  bool localVisible = true;
  bool effectiveVisible = true;
  uint64_t runtimeIdentity = 0;
  std::vector<ScenePrimitiveVariant> variants;
  std::string extensionsJson;
  std::string extrasJson;
};

struct SceneAnimationSampler {
  int32_t inputAccessor = -1;
  int32_t outputAccessor = -1;
  std::string interpolation;
};

struct SceneAnimationChannel {
  int32_t documentNodeIndex = -1;
  int32_t projectedNodeIndex = -1;
  int32_t samplerIndex = -1;
  std::string path;
};

struct SceneAnimation {
  std::string name;
  std::vector<SceneAnimationSampler> samplers;
  std::vector<SceneAnimationChannel> channels;
};

// ---------------------------------------------------------------------------
// SceneAsset: renderer-agnostic scene representation
//
// Goals:
//   - Decoupled from glTF (no GltfModel, GltfMeshData, etc.)
//   - Vertex data is already interleaved (48-byte stride) for zero-copy upload
//   - Texture payload is GPU-ready (KTX2 / raw pixels) without runtime conversion
//   - Node hierarchy stores local transform components (T/R/S) only; world transforms
//     are rebuilt at runtime so that editing/scene-graph animation works.
// ---------------------------------------------------------------------------

struct SceneMesh {
  uint32_t vertexOffset = 0;   // byte offset into SceneAsset::vertexPayload
  uint32_t vertexCount = 0;
  uint32_t indexOffset = 0;    // byte offset into SceneAsset::indexPayload
  uint32_t indexCount = 0;
  uint32_t materialIndex = 0;

  glm::vec3 localBoundsMin = glm::vec3(0.0f);
  glm::vec3 localBoundsMax = glm::vec3(0.0f);

  int32_t alphaMode = 0;       // 0=OPAQUE, 1=MASK, 2=BLEND
  float   alphaCutoff = 0.5f;

  // Pre-computed world transform at export time (used for initial bounds / priority)
  glm::mat4 exportTransform = glm::mat4(1.0f);

  // Source identity and runtime handle are derived projection metadata.
  uint32_t sourceNodeIndex = 0;
  uint32_t sourceMeshIndex = 0;
  uint32_t sourcePrimitiveIndex = 0;
  uint64_t runtimeIdentity = 0;
  bool localVisible = true;
  bool effectiveVisible = true;
};

struct SceneMaterial {
  glm::vec4 baseColorFactor = glm::vec4(1.0f);
  float     metallicFactor = 1.0f;
  float     roughnessFactor = 1.0f;
  float     normalScale = 1.0f;
  float     occlusionStrength = 1.0f;
  glm::vec3 emissiveFactor = glm::vec3(0.0f);
  int32_t   _padding = 0;

  int32_t baseColorTexture = -1;
  int32_t normalTexture = -1;
  int32_t metallicRoughnessTexture = -1;
  int32_t occlusionTexture = -1;
  int32_t emissiveTexture = -1;

  int32_t alphaMode = 0;       // 0=OPAQUE, 1=MASK, 2=BLEND
  float   alphaCutoff = 0.5f;
  int32_t materialWorkflow = 0; // 0=metallic-roughness, 1=specular-glossiness

  bool    doubleSided = false;
  std::string name;
  std::string extensionsJson;
  std::string extrasJson;
};

struct SceneTexture {
  uint32_t width = 0;
  uint32_t height = 0;
  uint32_t mipLevels = 1;
  rhi::TextureFormat format = rhi::TextureFormat::undefined;

  // Payload location inside SceneAsset::texturePayload
  uint64_t payloadOffset = 0;
  uint64_t payloadSize = 0;

  bool isKtx2 = false;         // true = KTX2/BasisU compressed payload
  std::string uri;             // original URI for debugging
  uint32_t sourceImageIndex = std::numeric_limits<uint32_t>::max();
};

struct SceneDependency {
  std::string relativePath;
  uint64_t fileSize = 0;
  int64_t writeTimeTicks = 0;
  uint64_t pathHash = 0;
};

struct SceneNode {
  std::string name;
  int32_t     parent = -1;
  std::vector<uint32_t> children;

  // References into SceneAsset::meshes
  std::vector<uint32_t> meshRefs;

  glm::vec3 translation = glm::vec3(0.0f);
  glm::quat rotation = glm::quat(1.0f, 0.0f, 0.0f, 0.0f);
  glm::vec3 scale = glm::vec3(1.0f);

  // Cached local transform (rebuilt from T/R/S)
  glm::mat4 localTransform = glm::mat4(1.0f);

  // Cached world transform (rebuilt by TransformSystem)
  glm::mat4 worldTransform = glm::mat4(1.0f);

  uint32_t sourceNodeIndex = 0;
  uint64_t runtimeIdentity = 0;
  bool localVisible = true;
  bool effectiveVisible = true;
  std::string extensionsJson;
  std::string extrasJson;
};

struct SceneAsset {
  std::string name;
  std::string sourcePath;

  std::vector<SceneMesh>     meshes;
  std::vector<ScenePrimitive> primitives;
  std::vector<SceneMaterial> materials;
  std::vector<SceneTexture>  textures;
  std::vector<SceneNode>     nodes;
  std::vector<SceneCamera>   cameras;
  std::vector<SceneLight>    lights;
  std::vector<uint32_t>      rootNodes;
  std::vector<uint8_t>       sceneRootTransformApplyToRoot;
  std::vector<SceneDependency> dependencies;
  std::vector<std::string> materialVariantNames;
  std::vector<SceneAnimation> animations;

  // Semantic identity of the source document at projection time. This is
  // metadata only; the mutable document remains the save authority.
  std::string documentSemanticHash;
  uint64_t documentRevision = 0;

  // Runtime-only scene transform metadata. It is intentionally not part of
  // the glTF semantic authority or save format.
  SceneRootTransformMetadata sceneRootTransform;

  // --- GPU-ready bulk payloads --------------------------------------------

  // Interleaved vertex data: 48-byte stride
  //   float3 position
  //   float3 normal
  //   float2 texCoord
  //   float4 tangent
  ImmutableByteBuffer vertexPayload;

  // Index data: uint32_t per index
  ImmutableByteBuffer indexPayload;

  // Texture data: concatenated payloads, each texture's data lives at
  // texture[i].payloadOffset with size texture[i].payloadSize
  ImmutableByteBuffer texturePayload;

  // --- Meshlet data (optional, populated by builder when enabled) ---------
  struct MeshletPayload {
    std::vector<uint8_t> meshletData;     // shaderio::Meshlet array
    std::vector<uint8_t> indexData;       // meshlet-local index data
    std::vector<uint8_t> vertexData;      // meshlet-local vertex data
  };
  std::vector<MeshletPayload> meshletPayloads;  // per mesh, same order as meshes

  // Convenience helpers
  [[nodiscard]] const uint8_t* vertexDataPtr() const { return vertexPayload.data(); }
  [[nodiscard]] const uint8_t* indexDataPtr() const { return indexPayload.data(); }
  [[nodiscard]] const uint8_t* textureDataPtr() const { return texturePayload.data(); }

  [[nodiscard]] size_t totalVertexBytes() const { return vertexPayload.size(); }
  [[nodiscard]] size_t totalIndexBytes() const { return indexPayload.size(); }
  [[nodiscard]] size_t totalTextureBytes() const { return texturePayload.size(); }
};

// Input for MeshPool that is already interleaved (zero-copy upload path)
struct SceneMeshData {
  std::span<const uint8_t> interleavedVertexData;  // 48-byte stride
  uint32_t                 vertexCount = 0;
  std::span<const uint32_t> indices;
  glm::mat4                transform = glm::mat4(1.0f);
  int32_t                  materialIndex = -1;
};

}  // namespace demo
