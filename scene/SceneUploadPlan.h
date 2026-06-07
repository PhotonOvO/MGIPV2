#pragma once

#include "../common/Common.h"
#include "../rhi/vulkan/internal/VulkanCommon.h"

#include <array>
#include <cstdint>
#include <string>
#include <vector>

namespace demo {

constexpr uint32_t kSceneUploadVertexStride = 48;

enum class TexturePayloadKind : uint32_t {
  RawRgba8,
  Ktx2Container,
};

struct MeshUploadPlan {
  uint32_t meshIndex{0};
  uint64_t vertexSrcOffset{0};
  uint64_t vertexByteSize{0};
  uint64_t indexSrcOffset{0};
  uint64_t indexByteSize{0};
  uint32_t vertexCount{0};
  uint32_t indexCount{0};
  uint32_t vertexStride{kSceneUploadVertexStride};
  VkIndexType indexType{VK_INDEX_TYPE_UINT32};
  glm::vec3 localBoundsMin{0.0f};
  glm::vec3 localBoundsMax{0.0f};
};

struct TextureUploadPlan {
  uint32_t textureIndex{0};
  VkFormat format{VK_FORMAT_UNDEFINED};
  uint32_t width{0};
  uint32_t height{0};
  uint32_t mipLevels{1};
  TexturePayloadKind payloadKind{TexturePayloadKind::RawRgba8};
  uint64_t payloadSrcOffset{0};
  uint64_t payloadByteSize{0};
};

struct MaterialCreatePlan {
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
};

struct SceneDrawInstance {
  uint32_t instanceIndex{0};
  uint32_t nodeIndex{0};
  uint32_t meshIndex{0};
  uint32_t materialIndex{0};
  glm::mat4 worldTransform{1.0f};
};

struct InstanceBuildPlan {
  std::vector<SceneDrawInstance> instances;
};

struct InstanceCullRecord {
  uint32_t instanceIndex{0};
  uint32_t meshIndex{0};
  glm::vec3 worldBoundsMin{0.0f};
  glm::vec3 worldBoundsMax{0.0f};
  glm::vec4 boundingSphere{0.0f};
};

struct DescriptorUpdatePlan {
  uint32_t materialIndex{0};
  std::array<int32_t, 5> textureIndices{{-1, -1, -1, -1, -1}};
};

enum class SceneDrawBucket : uint32_t {
  Opaque,
  AlphaMask,
  Transparent,
};

struct DrawCommandBuildPlan {
  uint32_t instanceIndex{0};
  uint32_t meshIndex{0};
  uint32_t materialIndex{0};
  SceneDrawBucket bucket{SceneDrawBucket::Opaque};
};

struct SceneUploadPlan {
  std::vector<MeshUploadPlan> meshes;
  std::vector<TextureUploadPlan> textures;
  std::vector<MaterialCreatePlan> materials;
  TransformBuildPlan transforms;
  InstanceBuildPlan instances;
  std::vector<InstanceCullRecord> cullRecords;
  std::vector<DescriptorUpdatePlan> descriptorUpdates;
  std::vector<DrawCommandBuildPlan> drawCommands;
};

struct SceneUploadPlanValidationResult {
  bool        valid{true};
  std::string error;
};

}  // namespace demo
