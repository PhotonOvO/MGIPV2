#pragma once

#include "../common/Common.h"
#include "../shaders/shader_io.h"
#include "UploadUtils.h"

#include <string>
#include <vector>

namespace demo {

class GPUDrivenLightResources
{
public:
  struct CreateInfo
  {
    uint32_t maxPointLights{256};
    uint32_t maxSpotLights{128};
    uint32_t frameCount{1};
  };

  struct ClusterStats
  {
    uint32_t maxOccupancy{0};
    uint32_t overflowClusterCount{0};
    uint32_t appendedLightReferences{0};
    uint32_t testedPointLights{0};
    uint32_t testedSpotLights{0};
  };

  struct Diagnostics
  {
    uint32_t clusterGridX{shaderio::LClusterGridSizeX};
    uint32_t clusterGridY{shaderio::LClusterGridSizeY};
    uint32_t clusterGridZ{shaderio::LClusterGridSizeZ};
    uint32_t clusterCount{shaderio::LClusterCount};
    uint32_t maxLightsPerCluster{shaderio::LMaxLightsPerCluster};
    uint32_t maxPointLights{0};
    uint32_t maxSpotLights{0};
    uint32_t activePointLights{0};
    uint32_t activeSpotLights{0};
    uint32_t maxOccupancy{0};
    uint32_t overflowClusterCount{0};
    uint32_t appendedLightReferences{0};
    uint64_t clusterMemoryBytes{0};
    uint64_t lightMemoryBytes{0};
    bool     initialized{false};
    bool     clusteredDescriptorsReady{false};
    bool     lightingDescriptorsReady{false};
  };

  GPUDrivenLightResources() = default;
  ~GPUDrivenLightResources() { assert(m_device == VK_NULL_HANDLE && "Missing deinit()"); }

  void init(VkDevice device, VmaAllocator allocator, const CreateInfo& createInfo);
  void deinit();

  void updateLights(uint32_t frameIndex,
                    const std::vector<shaderio::LightData>& pointLights,
                    const std::vector<shaderio::LightData>& spotLights);
  void updateUniforms(uint32_t frameIndex,
                      const shaderio::LightingUniforms& lightingUniforms,
                      const shaderio::LightCoarseCullingUniforms& coarseUniforms,
                      const shaderio::ClusteredLightUniforms& clusteredUniforms);

  [[nodiscard]] VkBuffer getPointLightBuffer(uint32_t frameIndex) const;
  [[nodiscard]] VkBuffer getSpotLightBuffer(uint32_t frameIndex) const;
  [[nodiscard]] VkBuffer getPointCoarseBoundsBuffer(uint32_t frameIndex) const;
  [[nodiscard]] VkBuffer getSpotCoarseBoundsBuffer(uint32_t frameIndex) const;
  [[nodiscard]] VkBuffer getCoarseUniformBuffer(uint32_t frameIndex) const;
  [[nodiscard]] VkBuffer getLightingUniformBuffer(uint32_t frameIndex) const;
  [[nodiscard]] VkBuffer getClusteredUniformBuffer(uint32_t frameIndex) const;
  [[nodiscard]] VkBuffer getClusterCountsBuffer(uint32_t frameIndex) const;
  [[nodiscard]] VkBuffer getClusterIndicesBuffer(uint32_t frameIndex) const;
  [[nodiscard]] VkBuffer getClusterStatsBuffer(uint32_t frameIndex) const;
  [[nodiscard]] uint32_t getActivePointLightCount() const { return m_activePointLights; }
  [[nodiscard]] uint32_t getActiveSpotLightCount() const { return m_activeSpotLights; }
  [[nodiscard]] uint32_t getMaxPointLights() const { return m_maxPointLights; }
  [[nodiscard]] uint32_t getMaxSpotLights() const { return m_maxSpotLights; }
  [[nodiscard]] uint32_t getFrameCount() const { return static_cast<uint32_t>(m_frames.size()); }
  [[nodiscard]] Diagnostics getDiagnostics() const;
  void cacheClusterStats(uint32_t frameIndex);

private:
  struct FrameResources
  {
    utils::Buffer pointLightBuffer{};
    utils::Buffer spotLightBuffer{};
    utils::Buffer pointCoarseBoundsBuffer{};
    utils::Buffer spotCoarseBoundsBuffer{};
    utils::Buffer lightingUniformBuffer{};
    utils::Buffer coarseUniformBuffer{};
    utils::Buffer clusteredUniformBuffer{};
    utils::Buffer clusterCountsBuffer{};
    utils::Buffer clusterIndicesBuffer{};
    utils::Buffer clusterStatsBuffer{};
  };

  [[nodiscard]] utils::Buffer createStorageBuffer(VkDeviceSize size, VmaMemoryUsage usage, VmaAllocationCreateFlags flags = {}) const;
  [[nodiscard]] utils::Buffer createUniformBuffer(VkDeviceSize size, VmaMemoryUsage usage, VmaAllocationCreateFlags flags = {}) const;
  void destroyBuffer(utils::Buffer& buffer) const;
  void updateMappedBuffer(utils::Buffer& buffer, const void* data, VkDeviceSize size) const;

  VkDevice     m_device{VK_NULL_HANDLE};
  VmaAllocator m_allocator{nullptr};
  std::vector<FrameResources> m_frames;
  uint32_t m_maxPointLights{256};
  uint32_t m_maxSpotLights{128};
  uint32_t m_activePointLights{0};
  uint32_t m_activeSpotLights{0};
  ClusterStats m_lastClusterStats{};
};

}  // namespace demo
