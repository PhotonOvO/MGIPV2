#include "VulkanCommandPool.h"

#include "internal/VulkanCommon.h"

namespace demo {
namespace rhi {
namespace vulkan {

namespace {

uint64_t toNativeU64(uintptr_t value)
{
  return static_cast<uint64_t>(value);
}

}  // namespace

VulkanCommandPool::~VulkanCommandPool()
{
  deinit();
}

void VulkanCommandPool::init(void* nativeDevice, QueueClass queueClass, uint32_t queueFamilyIndex)
{
  ASSERT(nativeDevice != nullptr, "VulkanCommandPool::init requires VkDevice");
  ASSERT(m_device == VK_NULL_HANDLE, "VulkanCommandPool::init already initialized");
  ASSERT(m_pool == VK_NULL_HANDLE, "VulkanCommandPool::init found stale VkCommandPool");

  m_device           = static_cast<VkDevice>(nativeDevice);
  m_queueClass       = queueClass;
  m_queueFamilyIndex = queueFamilyIndex;

  const VkCommandPoolCreateInfo createInfo{
      .sType            = VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO,
      .flags            = VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT,
      .queueFamilyIndex = queueFamilyIndex,
  };
  VK_CHECK(vkCreateCommandPool(m_device, &createInfo, nullptr, &m_pool));
  ASSERT(m_pool != VK_NULL_HANDLE, "VulkanCommandPool::init failed creating command pool");
}

void VulkanCommandPool::deinit()
{
  if(m_device == VK_NULL_HANDLE)
  {
    m_pool             = VK_NULL_HANDLE;
    m_queueClass       = QueueClass::graphics;
    m_queueFamilyIndex = ~0U;
    return;
  }

  if(m_pool != VK_NULL_HANDLE)
  {
    vkDestroyCommandPool(m_device, m_pool, nullptr);
    m_pool = VK_NULL_HANDLE;
  }

  m_device           = VK_NULL_HANDLE;
  m_queueClass       = QueueClass::graphics;
  m_queueFamilyIndex = ~0U;
}

void VulkanCommandPool::reset()
{
  ASSERT(m_device != VK_NULL_HANDLE, "VulkanCommandPool::reset requires VkDevice");
  ASSERT(m_pool != VK_NULL_HANDLE, "VulkanCommandPool::reset requires VkCommandPool");
  VK_CHECK(vkResetCommandPool(m_device, m_pool, 0));
}

VkCommandBuffer VulkanCommandPool::allocateNativeCommandBuffer()
{
  ASSERT(m_device != VK_NULL_HANDLE, "VulkanCommandPool::allocateNativeCommandBuffer requires VkDevice");
  ASSERT(m_pool != VK_NULL_HANDLE, "VulkanCommandPool::allocateNativeCommandBuffer requires VkCommandPool");

  VkCommandBuffer                   commandBuffer = VK_NULL_HANDLE;
  const VkCommandBufferAllocateInfo allocateInfo{
      .sType              = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO,
      .commandPool        = m_pool,
      .level              = VK_COMMAND_BUFFER_LEVEL_PRIMARY,
      .commandBufferCount = 1,
  };
  VK_CHECK(vkAllocateCommandBuffers(m_device, &allocateInfo, &commandBuffer));
  return commandBuffer;
}

void VulkanCommandPool::freeNativeCommandBuffer(VkCommandBuffer commandBuffer)
{
  if(commandBuffer == VK_NULL_HANDLE)
  {
    return;
  }
  ASSERT(m_device != VK_NULL_HANDLE, "VulkanCommandPool::freeNativeCommandBuffer requires VkDevice");
  ASSERT(m_pool != VK_NULL_HANDLE, "VulkanCommandPool::freeNativeCommandBuffer requires VkCommandPool");
  vkFreeCommandBuffers(m_device, m_pool, 1, &commandBuffer);
}

uint64_t VulkanCommandPool::getBackendHandle() const
{
  return toNativeU64(reinterpret_cast<uintptr_t>(m_pool));
}

}  // namespace vulkan
}  // namespace rhi
}  // namespace demo
