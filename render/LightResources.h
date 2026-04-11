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
    uint32_t maxLights{256};
    uint32_t maxLightsPerTile{32};
  };

  LightResources() = default;
  ~LightResources() { assert(m_device == VK_NULL_HANDLE && "Missing deinit()"); }

  void init(rhi::Device& device, VmaAllocator allocator, const CreateInfo& createInfo);
  void deinit();

  // Update light data buffers (requires staging buffer upload)
  // Call this before light culling compute pass
  void updateLights(VkCommandBuffer cmd, const std::vector<shaderio::LightData>& lights, const shaderio::LightListUniforms& uniforms);

  [[nodiscard]] VkBuffer getLightBuffer() const { return m_lightBuffer.buffer; }
  [[nodiscard]] VkBuffer getLightUniformsBuffer() const { return m_lightUniformsBuffer.buffer; }
  [[nodiscard]] VkBuffer getTileLightIndexBuffer() const { return m_tileLightIndexBuffer.buffer; }
  [[nodiscard]] uint64_t getLightBufferAddress() const;
  [[nodiscard]] uint64_t getLightUniformsBufferAddress() const;
  [[nodiscard]] uint64_t getTileLightIndexBufferAddress() const;
  [[nodiscard]] uint32_t getMaxLights() const { return m_maxLights; }
  [[nodiscard]] uint32_t getMaxLightsPerTile() const { return m_maxLightsPerTile; }

private:
  VkDevice m_device{VK_NULL_HANDLE};
  VmaAllocator m_allocator{nullptr};

  utils::Buffer m_lightBuffer{};          // Light data array (SSBO for compute read)
  utils::Buffer m_lightUniformsBuffer{};  // Light list uniforms (UBO)
  utils::Buffer m_tileLightIndexBuffer{}; // Per-tile light index list (compute output)

  uint32_t m_maxLights{256};
  uint32_t m_maxLightsPerTile{32};

  // Staging buffers for deferred deletion after GPU sync
  std::vector<utils::Buffer> m_stagingBuffers;
};

}  // namespace demo