#include "GPUSceneRegistry.h"

#include <algorithm>
#include <cstring>

namespace demo {

namespace {

utils::Buffer createBuffer(VkDevice device,
                           VmaAllocator allocator,
                           VkDeviceSize size,
                           VkBufferUsageFlags2KHR usage,
                           VmaMemoryUsage memoryUsage,
                           VmaAllocationCreateFlags flags)
{
  const VkBufferUsageFlags2CreateInfoKHR usageInfo{
      .sType = VK_STRUCTURE_TYPE_BUFFER_USAGE_FLAGS_2_CREATE_INFO_KHR,
      .usage = usage | VK_BUFFER_USAGE_2_SHADER_DEVICE_ADDRESS_BIT_KHR,
  };
  const VkBufferCreateInfo bufferInfo{
      .sType       = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO,
      .pNext       = &usageInfo,
      .size        = size,
      .usage       = 0,
      .sharingMode = VK_SHARING_MODE_EXCLUSIVE,
  };

  VmaAllocationCreateInfo allocInfo{.flags = flags, .usage = memoryUsage};
  if((allocInfo.flags & (VMA_ALLOCATION_CREATE_HOST_ACCESS_SEQUENTIAL_WRITE_BIT
                         | VMA_ALLOCATION_CREATE_HOST_ACCESS_RANDOM_BIT)) != 0)
  {
    allocInfo.flags |= VMA_ALLOCATION_CREATE_MAPPED_BIT;
  }

  utils::Buffer     buffer{};
  VmaAllocationInfo allocationInfo{};
  VK_CHECK(vmaCreateBuffer(allocator, &bufferInfo, &allocInfo, &buffer.buffer, &buffer.allocation, &allocationInfo));
  buffer.mapped = allocationInfo.pMappedData;

  const VkBufferDeviceAddressInfo addressInfo{.sType = VK_STRUCTURE_TYPE_BUFFER_DEVICE_ADDRESS_INFO, .buffer = buffer.buffer};
  buffer.address = vkGetBufferDeviceAddress(device, &addressInfo);
  return buffer;
}

glm::vec4 packRow(const glm::mat4& matrix, int row)
{
  return glm::vec4(matrix[0][row], matrix[1][row], matrix[2][row], matrix[3][row]);
}

}  // namespace

void GPUSceneRegistry::init(VkDevice device, VmaAllocator allocator)
{
  m_device = device;
  m_allocator = allocator;
}

void GPUSceneRegistry::deinit()
{
  clear();
  destroyBuffer(m_updateBuffer);
  destroyBuffer(m_objectBuffer);
  destroyBuffer(m_cullObjectBuffer);
  m_capacity = 0;
  m_device = VK_NULL_HANDLE;
  m_allocator = nullptr;
}

void GPUSceneRegistry::clear()
{
  m_slots.assign(1, ObjectSlot{});
  m_freeList.clear();
  m_denseSlotIds.clear();
  m_gpuObjects.clear();
  m_cullObjects.clear();
  m_dirty = true;
}

uint32_t GPUSceneRegistry::registerObject(const GPUSceneRegistrationDesc& desc)
{
  uint32_t objectID = 0;
  if(!m_freeList.empty())
  {
    objectID = m_freeList.back();
    m_freeList.pop_back();
  }
  else
  {
    objectID = static_cast<uint32_t>(m_slots.size());
    m_slots.push_back(ObjectSlot{});
  }

  ObjectSlot& slot = m_slots[objectID];
  slot.occupied = true;
  slot.denseIndex = static_cast<uint32_t>(m_gpuObjects.size());
  slot.desc = desc;
  slot.gpuObject = packSceneObject(desc);
  slot.cullObject = packCullObject(desc);

  m_denseSlotIds.push_back(objectID);
  m_gpuObjects.push_back(slot.gpuObject);
  m_cullObjects.push_back(slot.cullObject);
  m_dirty = true;
  return objectID;
}

void GPUSceneRegistry::removeObject(uint32_t objectID)
{
  if(objectID == 0 || objectID >= m_slots.size())
  {
    return;
  }

  ObjectSlot& slot = m_slots[objectID];
  if(!slot.occupied)
  {
    return;
  }

  const uint32_t denseIndex = slot.denseIndex;
  const uint32_t lastDenseIndex = static_cast<uint32_t>(m_gpuObjects.size() - 1u);
  if(denseIndex != lastDenseIndex)
  {
    m_gpuObjects[denseIndex] = m_gpuObjects[lastDenseIndex];
    m_cullObjects[denseIndex] = m_cullObjects[lastDenseIndex];
    const uint32_t movedObjectID = m_denseSlotIds[lastDenseIndex];
    m_denseSlotIds[denseIndex] = movedObjectID;
    m_slots[movedObjectID].denseIndex = denseIndex;
  }

  m_gpuObjects.pop_back();
  m_cullObjects.pop_back();
  m_denseSlotIds.pop_back();

  slot = {};
  m_freeList.push_back(objectID);
  m_dirty = true;
}

void GPUSceneRegistry::updateTransform(uint32_t objectID, const glm::mat4& newTransform, const glm::vec4& newBoundsSphere)
{
  if(objectID == 0 || objectID >= m_slots.size())
  {
    return;
  }

  ObjectSlot& slot = m_slots[objectID];
  if(!slot.occupied)
  {
    return;
  }

  slot.desc.transform = newTransform;
  slot.desc.boundsSphere = newBoundsSphere;
  rebuildPackedObject(objectID);
  m_dirty = true;
}

void GPUSceneRegistry::syncToGpu(VkCommandBuffer cmd)
{
  if(!m_dirty)
  {
    return;
  }

  const uint32_t objectCount = static_cast<uint32_t>(m_gpuObjects.size());
  if(objectCount == 0)
  {
    m_dirty = false;
    return;
  }

  ensureCapacity(objectCount);

  const VkDeviceSize sceneBytes = sizeof(shaderio::GPUSceneObject) * static_cast<VkDeviceSize>(objectCount);
  std::memcpy(m_updateBuffer.mapped, m_gpuObjects.data(), static_cast<size_t>(sceneBytes));
  VK_CHECK(vmaFlushAllocation(m_allocator, m_updateBuffer.allocation, 0, sceneBytes));

  const VkBufferCopy2 sceneCopy{
      .sType     = VK_STRUCTURE_TYPE_BUFFER_COPY_2,
      .srcOffset = 0,
      .dstOffset = 0,
      .size      = sceneBytes,
  };
  const VkCopyBufferInfo2 sceneCopyInfo{
      .sType       = VK_STRUCTURE_TYPE_COPY_BUFFER_INFO_2,
      .srcBuffer   = m_updateBuffer.buffer,
      .dstBuffer   = m_objectBuffer.buffer,
      .regionCount = 1,
      .pRegions    = &sceneCopy,
  };
  vkCmdCopyBuffer2(cmd, &sceneCopyInfo);

  const VkDeviceSize cullBytes = sizeof(shaderio::GPUCullObject) * static_cast<VkDeviceSize>(objectCount);
  std::memcpy(m_updateBuffer.mapped, m_cullObjects.data(), static_cast<size_t>(cullBytes));
  VK_CHECK(vmaFlushAllocation(m_allocator, m_updateBuffer.allocation, 0, cullBytes));

  const VkBufferCopy2 cullCopy{
      .sType     = VK_STRUCTURE_TYPE_BUFFER_COPY_2,
      .srcOffset = 0,
      .dstOffset = 0,
      .size      = cullBytes,
  };
  const VkCopyBufferInfo2 cullCopyInfo{
      .sType       = VK_STRUCTURE_TYPE_COPY_BUFFER_INFO_2,
      .srcBuffer   = m_updateBuffer.buffer,
      .dstBuffer   = m_cullObjectBuffer.buffer,
      .regionCount = 1,
      .pRegions    = &cullCopy,
  };
  vkCmdCopyBuffer2(cmd, &cullCopyInfo);

  const std::array<VkBufferMemoryBarrier2, 2> barriers{{
      VkBufferMemoryBarrier2{
          .sType         = VK_STRUCTURE_TYPE_BUFFER_MEMORY_BARRIER_2,
          .srcStageMask  = VK_PIPELINE_STAGE_2_TRANSFER_BIT,
          .srcAccessMask = VK_ACCESS_2_TRANSFER_WRITE_BIT,
          .dstStageMask  = VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT | VK_PIPELINE_STAGE_2_VERTEX_SHADER_BIT,
          .dstAccessMask = VK_ACCESS_2_SHADER_STORAGE_READ_BIT,
          .buffer        = m_objectBuffer.buffer,
          .offset        = 0,
          .size          = sceneBytes,
      },
      VkBufferMemoryBarrier2{
          .sType         = VK_STRUCTURE_TYPE_BUFFER_MEMORY_BARRIER_2,
          .srcStageMask  = VK_PIPELINE_STAGE_2_TRANSFER_BIT,
          .srcAccessMask = VK_ACCESS_2_TRANSFER_WRITE_BIT,
          .dstStageMask  = VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT | VK_PIPELINE_STAGE_2_VERTEX_SHADER_BIT,
          .dstAccessMask = VK_ACCESS_2_SHADER_STORAGE_READ_BIT,
          .buffer        = m_cullObjectBuffer.buffer,
          .offset        = 0,
          .size          = cullBytes,
      },
  }};
  const VkDependencyInfo dependencyInfo{
      .sType                    = VK_STRUCTURE_TYPE_DEPENDENCY_INFO,
      .bufferMemoryBarrierCount = static_cast<uint32_t>(barriers.size()),
      .pBufferMemoryBarriers    = barriers.data(),
  };
  vkCmdPipelineBarrier2(cmd, &dependencyInfo);

  m_dirty = false;
}

void GPUSceneRegistry::ensureCapacity(uint32_t requiredCount)
{
  if(requiredCount <= m_capacity)
  {
    return;
  }

  const uint32_t newCapacity = std::max(requiredCount, std::max(64u, m_capacity * 2u));
  destroyBuffer(m_objectBuffer);
  destroyBuffer(m_cullObjectBuffer);
  destroyBuffer(m_updateBuffer);

  m_objectBuffer = createBuffer(m_device,
                                m_allocator,
                                sizeof(shaderio::GPUSceneObject) * static_cast<VkDeviceSize>(newCapacity),
                                VK_BUFFER_USAGE_2_STORAGE_BUFFER_BIT_KHR | VK_BUFFER_USAGE_2_TRANSFER_DST_BIT_KHR,
                                VMA_MEMORY_USAGE_AUTO_PREFER_DEVICE,
                                0);
  m_cullObjectBuffer = createBuffer(m_device,
                                    m_allocator,
                                    sizeof(shaderio::GPUCullObject) * static_cast<VkDeviceSize>(newCapacity),
                                    VK_BUFFER_USAGE_2_STORAGE_BUFFER_BIT_KHR | VK_BUFFER_USAGE_2_TRANSFER_DST_BIT_KHR,
                                    VMA_MEMORY_USAGE_AUTO_PREFER_DEVICE,
                                    0);
  m_updateBuffer = createBuffer(m_device,
                                m_allocator,
                                sizeof(shaderio::GPUSceneObject) * static_cast<VkDeviceSize>(newCapacity),
                                VK_BUFFER_USAGE_2_TRANSFER_SRC_BIT_KHR,
                                VMA_MEMORY_USAGE_CPU_TO_GPU,
                                VMA_ALLOCATION_CREATE_HOST_ACCESS_SEQUENTIAL_WRITE_BIT);
  m_capacity = newCapacity;
}

void GPUSceneRegistry::rebuildPackedObject(uint32_t objectID)
{
  ObjectSlot& slot = m_slots[objectID];
  slot.gpuObject = packSceneObject(slot.desc);
  slot.cullObject = packCullObject(slot.desc);
  m_gpuObjects[slot.denseIndex] = slot.gpuObject;
  m_cullObjects[slot.denseIndex] = slot.cullObject;
}

void GPUSceneRegistry::destroyBuffer(utils::Buffer& buffer)
{
  if(buffer.buffer != VK_NULL_HANDLE)
  {
    vmaDestroyBuffer(m_allocator, buffer.buffer, buffer.allocation);
    buffer = {};
  }
}

shaderio::GPUSceneObject GPUSceneRegistry::packSceneObject(const GPUSceneRegistrationDesc& desc)
{
  shaderio::GPUSceneObject object{};
  object.worldMatrixRows[0] = packRow(desc.transform, 0);
  object.worldMatrixRows[1] = packRow(desc.transform, 1);
  object.worldMatrixRows[2] = packRow(desc.transform, 2);
  object.boundsSphere = desc.boundsSphere;
  object.materialIndex = desc.materialIndex;
  object.meshIndex = desc.meshIndex;
  object.flags = desc.flags;
  return object;
}

shaderio::GPUCullObject GPUSceneRegistry::packCullObject(const GPUSceneRegistrationDesc& desc)
{
  return shaderio::GPUCullObject{
      .sphereCenterRadius = desc.boundsSphere,
      .indexCount         = desc.indexCount,
      .firstIndex         = desc.firstIndex,
      .vertexOffset       = desc.vertexOffset,
      .flags              = desc.flags,
  };
}

}  // namespace demo
