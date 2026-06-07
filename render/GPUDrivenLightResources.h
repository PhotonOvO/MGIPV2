#pragma once

#include "../rhi/vulkan/internal/VulkanCommon.h"
#include "../common/Handles.h"
#include "../rhi/RHIDevice.h"

#include <cassert>
#include <cstdint>
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
  ~GPUDrivenLightResources() { assert(m_device == nullptr && "Missing deinit()"); }

  void init(rhi::Device& device, const CreateInfo& createInfo);
  void deinit();

  void updateLights(uint32_t frameIndex,
                    const std::vector<shaderio::LightData>& pointLights,
                    const std::vector<shaderio::LightData>& spotLights);
  void updateUniforms(uint32_t frameIndex,
                      const shaderio::LightingUniforms& lightingUniforms,
                      const shaderio::LightCoarseCullingUniforms& coarseUniforms,
                      const shaderio::ClusteredLightUniforms& clusteredUniforms);

  [[nodiscard]] rhi::BufferHandle getPointLightBuffer(uint32_t frameIndex) const;
  [[nodiscard]] rhi::BufferHandle getSpotLightBuffer(uint32_t frameIndex) const;
  [[nodiscard]] rhi::BufferHandle getPointCoarseBoundsBuffer(uint32_t frameIndex) const;
  [[nodiscard]] rhi::BufferHandle getSpotCoarseBoundsBuffer(uint32_t frameIndex) const;
  [[nodiscard]] rhi::BufferHandle getCoarseUniformBuffer(uint32_t frameIndex) const;
  [[nodiscard]] rhi::BufferHandle getLightingUniformBuffer(uint32_t frameIndex) const;
  [[nodiscard]] rhi::BufferHandle getClusteredUniformBuffer(uint32_t frameIndex) const;
  [[nodiscard]] rhi::BufferHandle getClusterCountsBuffer(uint32_t frameIndex) const;
  [[nodiscard]] rhi::BufferHandle getClusterIndicesBuffer(uint32_t frameIndex) const;
  [[nodiscard]] rhi::BufferHandle getClusterStatsBuffer(uint32_t frameIndex) const;
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
    rhi::BufferHandle pointLightBuffer{};
    rhi::BufferHandle spotLightBuffer{};
    rhi::BufferHandle pointCoarseBoundsBuffer{};
    rhi::BufferHandle spotCoarseBoundsBuffer{};
    rhi::BufferHandle lightingUniformBuffer{};
    rhi::BufferHandle coarseUniformBuffer{};
    rhi::BufferHandle clusteredUniformBuffer{};
    rhi::BufferHandle clusterCountsBuffer{};
    rhi::BufferHandle clusterIndicesBuffer{};
    rhi::BufferHandle clusterStatsBuffer{};
  };

  [[nodiscard]] rhi::BufferHandle createStorageBuffer(uint64_t size, rhi::MemoryUsage usage) const;
  [[nodiscard]] rhi::BufferHandle createUniformBuffer(uint64_t size, rhi::MemoryUsage usage) const;
  void destroyBuffer(rhi::BufferHandle& buffer) const;
  void updateMappedBuffer(rhi::BufferHandle buffer, const void* data, uint64_t size) const;

  rhi::Device* m_device{nullptr};
  std::vector<FrameResources> m_frames;
  uint32_t m_maxPointLights{256};
  uint32_t m_maxSpotLights{128};
  uint32_t m_activePointLights{0};
  uint32_t m_activeSpotLights{0};
  ClusterStats m_lastClusterStats{};
};

}  // namespace demo
