#pragma once

#include "../common/Common.h"
#include "../rhi/RHIDevice.h"

#include <glm/gtc/quaternion.hpp>

#include <cstdint>
#include <string>
#include <vector>

namespace demo {

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
};

struct SceneTexture {
  uint32_t width = 0;
  uint32_t height = 0;
  uint32_t mipLevels = 1;
  VkFormat format = VK_FORMAT_UNDEFINED;

  // Payload location inside SceneAsset::texturePayload
  uint64_t payloadOffset = 0;
  uint64_t payloadSize = 0;

  bool isKtx2 = false;         // true = KTX2/BasisU compressed payload
  std::string uri;             // original URI for debugging
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
};

struct SceneAsset {
  std::string name;
  std::string sourcePath;

  std::vector<SceneMesh>     meshes;
  std::vector<SceneMaterial> materials;
  std::vector<SceneTexture>  textures;
  std::vector<SceneNode>     nodes;
  std::vector<uint32_t>      rootNodes;
  std::vector<SceneDependency> dependencies;

  // --- GPU-ready bulk payloads --------------------------------------------

  // Interleaved vertex data: 48-byte stride
  //   float3 position
  //   float3 normal
  //   float2 texCoord
  //   float4 tangent
  std::vector<uint8_t> vertexPayload;

  // Index data: uint32_t per index
  std::vector<uint8_t> indexPayload;

  // Texture data: concatenated payloads, each texture's data lives at
  // texture[i].payloadOffset with size texture[i].payloadSize
  std::vector<uint8_t> texturePayload;

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
