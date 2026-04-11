#include "LightResources.h"

#include <cstring>

namespace demo {

void LightResources::init(rhi::Device& device, VmaAllocator allocator, const CreateInfo& createInfo)
{
  m_device = reinterpret_cast<VkDevice>(static_cast<uintptr_t>(device.getNativeDevice()));
  m_allocator = allocator;
  m_maxLights = createInfo.maxLights;
  m_maxLightsPerTile = createInfo.maxLightsPerTile;

  // Create light data buffer (storage buffer for compute read)
  const VkBufferCreateInfo lightBufferInfo{
      .sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO,
      .size = sizeof(shaderio::LightData) * m_maxLights,
      .usage = VK_BUFFER_USAGE_STORAGE_BUFFER_BIT | VK_BUFFER_USAGE_TRANSFER_DST_BIT | VK_BUFFER_USAGE_SHADER_DEVICE_ADDRESS_BIT,
      .sharingMode = VK_SHARING_MODE_EXCLUSIVE,
  };
  const VmaAllocationCreateInfo lightAllocInfo{.usage = VMA_MEMORY_USAGE_GPU_ONLY};
  VK_CHECK(vmaCreateBuffer(m_allocator, &lightBufferInfo, &lightAllocInfo,
      &m_lightBuffer.buffer, &m_lightBuffer.allocation, nullptr));

  // Get device address for light buffer
  const VkBufferDeviceAddressInfo lightAddrInfo{
      .sType = VK_STRUCTURE_TYPE_BUFFER_DEVICE_ADDRESS_INFO,
      .buffer = m_lightBuffer.buffer,
  };
  m_lightBuffer.address = vkGetBufferDeviceAddress(m_device, &lightAddrInfo);

  // Create light uniforms buffer (UBO for compute/graphic read)
  const VkBufferCreateInfo uniformBufferInfo{
      .sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO,
      .size = sizeof(shaderio::LightListUniforms),
      .usage = VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT | VK_BUFFER_USAGE_TRANSFER_DST_BIT | VK_BUFFER_USAGE_SHADER_DEVICE_ADDRESS_BIT,
      .sharingMode = VK_SHARING_MODE_EXCLUSIVE,
  };
  const VmaAllocationCreateInfo uniformAllocInfo{.usage = VMA_MEMORY_USAGE_GPU_ONLY};
  VK_CHECK(vmaCreateBuffer(m_allocator, &uniformBufferInfo, &uniformAllocInfo,
      &m_lightUniformsBuffer.buffer, &m_lightUniformsBuffer.allocation, nullptr));

  // Get device address for uniforms buffer
  const VkBufferDeviceAddressInfo uniformAddrInfo{
      .sType = VK_STRUCTURE_TYPE_BUFFER_DEVICE_ADDRESS_INFO,
      .buffer = m_lightUniformsBuffer.buffer,
  };
  m_lightUniformsBuffer.address = vkGetBufferDeviceAddress(m_device, &uniformAddrInfo);

  // Create tile light index buffer (compute output)
  // Size: (maxScreenWidth / TILE_SIZE_X) * (maxScreenHeight / TILE_SIZE_Y) * maxLightsPerTile * sizeof(uint32_t)
  // We allocate for max resolution 4K (256 * 256 tiles = 65536 tiles)
  const uint32_t maxTiles = 256 * 256;
  const VkBufferCreateInfo tileBufferInfo{
      .sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO,
      .size = maxTiles * m_maxLightsPerTile * sizeof(uint32_t),
      .usage = VK_BUFFER_USAGE_STORAGE_BUFFER_BIT | VK_BUFFER_USAGE_SHADER_DEVICE_ADDRESS_BIT,
      .sharingMode = VK_SHARING_MODE_EXCLUSIVE,
  };
  VK_CHECK(vmaCreateBuffer(m_allocator, &tileBufferInfo, &lightAllocInfo,
      &m_tileLightIndexBuffer.buffer, &m_tileLightIndexBuffer.allocation, nullptr));

  // Get device address for tile buffer
  const VkBufferDeviceAddressInfo tileAddrInfo{
      .sType = VK_STRUCTURE_TYPE_BUFFER_DEVICE_ADDRESS_INFO,
      .buffer = m_tileLightIndexBuffer.buffer,
  };
  m_tileLightIndexBuffer.address = vkGetBufferDeviceAddress(m_device, &tileAddrInfo);
}

void LightResources::deinit()
{
  // Free any remaining staging buffers
  for (auto& buffer : m_stagingBuffers)
  {
    if (buffer.buffer != VK_NULL_HANDLE)
    {
      vmaDestroyBuffer(m_allocator, buffer.buffer, buffer.allocation);
    }
  }
  m_stagingBuffers.clear();

  // Destroy main buffers
  if (m_lightBuffer.buffer != VK_NULL_HANDLE)
    vmaDestroyBuffer(m_allocator, m_lightBuffer.buffer, m_lightBuffer.allocation);
  if (m_lightUniformsBuffer.buffer != VK_NULL_HANDLE)
    vmaDestroyBuffer(m_allocator, m_lightUniformsBuffer.buffer, m_lightUniformsBuffer.allocation);
  if (m_tileLightIndexBuffer.buffer != VK_NULL_HANDLE)
    vmaDestroyBuffer(m_allocator, m_tileLightIndexBuffer.buffer, m_tileLightIndexBuffer.allocation);

  *this = LightResources{};
}

void LightResources::updateLights(VkCommandBuffer cmd, const std::vector<shaderio::LightData>& lights, const shaderio::LightListUniforms& uniforms)
{
  // Clamp light count to max
  const uint32_t lightCount = std::min(static_cast<uint32_t>(lights.size()), m_maxLights);
  const VkDeviceSize lightDataSize = sizeof(shaderio::LightData) * lightCount;
  const VkDeviceSize uniformDataSize = sizeof(shaderio::LightListUniforms);

  // Create staging buffer for light data
  VkBufferCreateInfo stagingInfo{
      .sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO,
      .size = lightDataSize + uniformDataSize,  // Combined staging for efficiency
      .usage = VK_BUFFER_USAGE_TRANSFER_SRC_BIT,
      .sharingMode = VK_SHARING_MODE_EXCLUSIVE,
  };

  VmaAllocationCreateInfo stagingAllocInfo{
      .flags = VMA_ALLOCATION_CREATE_HOST_ACCESS_SEQUENTIAL_WRITE_BIT,
      .usage = VMA_MEMORY_USAGE_CPU_ONLY,
  };

  utils::Buffer stagingBuffer{};
  VK_CHECK(vmaCreateBuffer(m_allocator, &stagingInfo, &stagingAllocInfo,
      &stagingBuffer.buffer, &stagingBuffer.allocation, nullptr));

  // Map and copy light data
  void* mappedData = nullptr;
  VK_CHECK(vmaMapMemory(m_allocator, stagingBuffer.allocation, &mappedData));

  // Copy light data first
  std::memcpy(mappedData, lights.data(), lightDataSize);
  // Copy uniforms after
  std::memcpy(static_cast<char*>(mappedData) + lightDataSize, &uniforms, uniformDataSize);

  vmaUnmapMemory(m_allocator, stagingBuffer.allocation);

  // Copy to GPU buffers
  if (lightCount > 0)
  {
    VkBufferCopy lightCopy{.size = lightDataSize};
    vkCmdCopyBuffer(cmd, stagingBuffer.buffer, m_lightBuffer.buffer, 1, &lightCopy);
  }

  VkBufferCopy uniformCopy{
      .srcOffset = lightDataSize,
      .size = uniformDataSize,
  };
  vkCmdCopyBuffer(cmd, stagingBuffer.buffer, m_lightUniformsBuffer.buffer, 1, &uniformCopy);

  // Store staging buffer for deferred deletion after GPU sync
  m_stagingBuffers.push_back(stagingBuffer);
}

uint64_t LightResources::getLightBufferAddress() const
{
  return m_lightBuffer.address;
}

uint64_t LightResources::getLightUniformsBufferAddress() const
{
  return m_lightUniformsBuffer.address;
}

uint64_t LightResources::getTileLightIndexBufferAddress() const
{
  return m_tileLightIndexBuffer.address;
}

}  // namespace demo