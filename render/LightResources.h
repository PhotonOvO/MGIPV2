#pragma once

#include "../rhi/RHIDevice.h"
#include "ShaderInterop.h"

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
  ~LightResources() { assert(m_backendDeviceToken == 0 && "Missing deinit()"); }

  void init(rhi::Device& device, uintptr_t backendAllocatorToken, const CreateInfo& createInfo);
  void deinit();

  void updatePointLights(uint32_t frameIndex, const std::vector<shaderio::LightData>& lights);
  void updateSpotLights(uint32_t frameIndex, const std::vector<shaderio::LightData>& lights);
  void updateCoarseCullingUniforms(uint32_t frameIndex, const shaderio::LightCoarseCullingUniforms& uniforms);

  [[nodiscard]] rhi::BufferHandle getPointLightBuffer(uint32_t frameIndex) const;
  [[nodiscard]] rhi::BufferHandle getSpotLightBuffer(uint32_t frameIndex) const;
  [[nodiscard]] rhi::BufferHandle getPointCoarseBoundsBuffer(uint32_t frameIndex) const;
  [[nodiscard]] rhi::BufferHandle getSpotCoarseBoundsBuffer(uint32_t frameIndex) const;
  [[nodiscard]] rhi::BufferHandle getCoarseCullingUniformBuffer(uint32_t frameIndex) const;
  [[nodiscard]] uint32_t getMaxPointLights() const { return m_maxPointLights; }
  [[nodiscard]] uint32_t getMaxSpotLights() const { return m_maxSpotLights; }
  [[nodiscard]] uint32_t getFrameCount() const { return static_cast<uint32_t>(m_frames.size()); }

private:
  struct FrameResources
  {
    rhi::BufferHandle pointLightBuffer{};
    rhi::BufferHandle spotLightBuffer{};
    rhi::BufferHandle pointCoarseBoundsBuffer{};
    rhi::BufferHandle spotCoarseBoundsBuffer{};
    rhi::BufferHandle coarseCullingUniformBuffer{};
  };

  rhi::Device* m_rhiDevice{nullptr};
  uintptr_t m_backendDeviceToken{0};
  uintptr_t m_backendAllocatorToken{0};

  std::vector<FrameResources> m_frames;
  uint32_t m_maxPointLights{256};
  uint32_t m_maxSpotLights{128};
};

}  // namespace demo
