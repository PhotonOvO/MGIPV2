#include "VulkanCommandPool.h"

#include "../../common/Common.h"
#include "VulkanCommandList.h"

#include <cstddef>

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
    m_records.clear();
    m_pool             = VK_NULL_HANDLE;
    m_queueClass       = QueueClass::graphics;
    m_queueFamilyIndex = ~0U;
    return;
  }

  if(!m_records.empty() && m_pool != VK_NULL_HANDLE)
  {
    std::vector<VkCommandBuffer> commandBuffers;
    commandBuffers.reserve(m_records.size());
    for(const CommandListRecord& record : m_records)
    {
      if(record.commandBuffer != VK_NULL_HANDLE)
      {
        commandBuffers.push_back(record.commandBuffer);
      }
    }

    if(!commandBuffers.empty())
    {
      vkFreeCommandBuffers(m_device, m_pool, static_cast<uint32_t>(commandBuffers.size()), commandBuffers.data());
    }
  }

  m_records.clear();

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

CommandList* VulkanCommandPool::allocateCommandList()
{
  ASSERT(m_device != VK_NULL_HANDLE, "VulkanCommandPool::allocateCommandList requires VkDevice");
  ASSERT(m_pool != VK_NULL_HANDLE, "VulkanCommandPool::allocateCommandList requires VkCommandPool");

  VkCommandBuffer                   commandBuffer = VK_NULL_HANDLE;
  const VkCommandBufferAllocateInfo allocateInfo{
      .sType              = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO,
      .commandPool        = m_pool,
      .level              = VK_COMMAND_BUFFER_LEVEL_PRIMARY,
      .commandBufferCount = 1,
  };
  VK_CHECK(vkAllocateCommandBuffers(m_device, &allocateInfo, &commandBuffer));

  CommandListRecord record;
  record.commandBuffer = commandBuffer;
  record.commandList   = std::make_unique<VulkanCommandList>();
  record.commandList->setCommandBuffer(commandBuffer);
  VulkanCommandList* rawCommandList = record.commandList.get();
  m_records.push_back(std::move(record));
  return rawCommandList;
}

void VulkanCommandPool::freeCommandList(CommandList* cmdList)
{
  if(cmdList == nullptr)
  {
    return;
  }

  ASSERT(m_device != VK_NULL_HANDLE, "VulkanCommandPool::freeCommandList requires VkDevice");
  ASSERT(m_pool != VK_NULL_HANDLE, "VulkanCommandPool::freeCommandList requires VkCommandPool");

  const size_t recordIndex = findRecordIndex(cmdList);
  ASSERT(recordIndex < m_records.size(), "VulkanCommandPool::freeCommandList received unmanaged command list");

  const VkCommandBuffer commandBuffer = m_records[recordIndex].commandBuffer;
  if(commandBuffer != VK_NULL_HANDLE)
  {
    vkFreeCommandBuffers(m_device, m_pool, 1, &commandBuffer);
  }
  m_records.erase(m_records.begin() + static_cast<std::ptrdiff_t>(recordIndex));
}

void VulkanCommandPool::allocateCommandLists(uint32_t count, CommandList** cmdLists)
{
  ASSERT(count == 0 || cmdLists != nullptr, "VulkanCommandPool::allocateCommandLists requires output array");
  for(uint32_t i = 0; i < count; ++i)
  {
    cmdLists[i] = allocateCommandList();
  }
}

void VulkanCommandPool::freeCommandLists(uint32_t count, CommandList** cmdLists)
{
  ASSERT(count == 0 || cmdLists != nullptr, "VulkanCommandPool::freeCommandLists requires input array");
  for(uint32_t i = 0; i < count; ++i)
  {
    freeCommandList(cmdLists[i]);
    cmdLists[i] = nullptr;
  }
}

uint64_t VulkanCommandPool::getNativeHandle() const
{
  return toNativeU64(reinterpret_cast<uintptr_t>(m_pool));
}

size_t VulkanCommandPool::findRecordIndex(const CommandList* cmdList) const
{
  for(size_t i = 0; i < m_records.size(); ++i)
  {
    if(m_records[i].commandList.get() == cmdList)
    {
      return i;
    }
  }
  return m_records.size();
}

}  // namespace vulkan
}  // namespace rhi
}  // namespace demo
