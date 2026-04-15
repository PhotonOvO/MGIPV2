#pragma once

#include "../common/Common.h"
#include "../rhi/RHIDevice.h"
#include "../shaders/shader_io.h"

#include <vector>

namespace demo {

class LightResources
{
public:
  struct CreateInfo
  {
    uint32_t maxPointLights{256};
    uint32_t maxSpotLights{128};
    uint32_t frameCount{1};
  };

  LightResources() = default;
  ~LightResources() { assert(m_device == VK_NULL_HANDLE && "Missing deinit()"); }

  void init(rhi::Device& device, VmaAllocator allocator, const CreateInfo& createInfo);
  void deinit();

  void updatePointLights(uint32_t frameIndex, const std::vector<shaderio::LightData>& lights);
  void updateSpotLights(uint32_t frameIndex, const std::vector<shaderio::LightData>& lights);
  void updateCoarseCullingUniforms(uint32_t frameIndex, const shaderio::LightCoarseCullingUniforms& uniforms);

  [[nodiscard]] VkBuffer getPointLightBuffer(uint32_t frameIndex) const;
  [[nodiscard]] VkBuffer getSpotLightBuffer(uint32_t frameIndex) const;
  [[nodiscard]] VkBuffer getPointCoarseBoundsBuffer(uint32_t frameIndex) const;
  [[nodiscard]] VkBuffer getSpotCoarseBoundsBuffer(uint32_t frameIndex) const;
  [[nodiscard]] VkBuffer getCoarseCullingUniformBuffer(uint32_t frameIndex) const;
  [[nodiscard]] uint32_t getMaxPointLights() const { return m_maxPointLights; }
  [[nodiscard]] uint32_t getMaxSpotLights() const { return m_maxSpotLights; }
  [[nodiscard]] uint32_t getFrameCount() const { return static_cast<uint32_t>(m_frames.size()); }

private:
  struct FrameResources
  {
    utils::Buffer pointLightBuffer{};
    utils::Buffer spotLightBuffer{};
    utils::Buffer pointCoarseBoundsBuffer{};
    utils::Buffer spotCoarseBoundsBuffer{};
    utils::Buffer coarseCullingUniformBuffer{};
  };

  VkDevice m_device{VK_NULL_HANDLE};
  VmaAllocator m_allocator{nullptr};

  std::vector<FrameResources> m_frames;
  uint32_t m_maxPointLights{256};
  uint32_t m_maxSpotLights{128};
};

}  // namespace demo
