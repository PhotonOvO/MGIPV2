#include "VulkanCommandAllocator.h"

#include "VulkanCommandBuffer.h"
#include "VulkanQueue.h"
#include "volk.h"

#include <algorithm>
#include <stdexcept>

namespace demo::rhi::vulkan {
namespace {

void require(bool condition, const char* message)
{
  if(!condition)
    throw std::runtime_error(message);
}

void checkVkResult(VkResult result, const char* message)
{
  if(result != VK_SUCCESS)
    throw std::runtime_error(message);
}

}  // namespace

VulkanCommandAllocator::VulkanCommandAllocator(
  VulkanQueue& queue, VkDevice device, VulkanResourceTable* resourceTable)
  : m_queue(queue)
  , m_device(device)
  , m_resourceTable(resourceTable)
{
  require(m_device != VK_NULL_HANDLE,
          "VulkanCommandAllocator requires an initialized device");
  const VkCommandPoolCreateInfo createInfo{
    .sType = VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO,
    .flags = VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT,
    .queueFamilyIndex = queue.familyIndex(),
  };
  checkVkResult(vkCreateCommandPool(m_device, &createInfo, nullptr, &m_pool),
                "VulkanCommandAllocator failed to create VkCommandPool");
}

VulkanCommandAllocator::~VulkanCommandAllocator()
{
  if(!m_commandBuffers.empty())
  {
    // Command buffers retain the allocator's pool. The owner must destroy them first.
    std::terminate();
  }
  if(m_pool != VK_NULL_HANDLE)
    vkDestroyCommandPool(m_device, m_pool, nullptr);
}

QueueClass VulkanCommandAllocator::queueClass() const noexcept
{
  return m_queue.info().queueClass;
}

void VulkanCommandAllocator::reset(SubmissionToken completedToken)
{
  if(m_lastSubmission.isValid())
  {
    require(completedToken.isValid(),
            "VulkanCommandAllocator::reset requires its completed submission token");
    require(completedToken.queue == m_lastSubmission.queue,
            "VulkanCommandAllocator::reset rejected a wrong-queue token");
    require(completedToken.value >= m_lastSubmission.value,
            "VulkanCommandAllocator::reset rejected a stale token");
    require(m_queue.isComplete(m_lastSubmission),
            "VulkanCommandAllocator::reset called before GPU completion");
  }
  else
  {
    require(!completedToken.isValid() ||
              completedToken.queue == m_queue.identity(),
            "VulkanCommandAllocator::reset rejected a wrong-queue token");
  }

  checkVkResult(vkResetCommandPool(m_device, m_pool, 0),
                "VulkanCommandAllocator failed to reset VkCommandPool");
  for(VulkanCommandBuffer* commandBuffer : m_commandBuffers)
    commandBuffer->markReusable();
  m_lastSubmission = {};
}

std::unique_ptr<CommandBuffer> VulkanCommandAllocator::createCommandBuffer()
{
  VkCommandBuffer native = VK_NULL_HANDLE;
  const VkCommandBufferAllocateInfo allocateInfo{
    .sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO,
    .commandPool = m_pool,
    .level = VK_COMMAND_BUFFER_LEVEL_PRIMARY,
    .commandBufferCount = 1,
  };
  checkVkResult(vkAllocateCommandBuffers(m_device, &allocateInfo, &native),
                "VulkanCommandAllocator failed to allocate VkCommandBuffer");
  auto commandBuffer =
    std::make_unique<VulkanCommandBuffer>(*this, native, m_resourceTable);
  m_commandBuffers.push_back(commandBuffer.get());
  return commandBuffer;
}

void VulkanCommandAllocator::noteSubmitted(
  VulkanCommandBuffer& commandBuffer, SubmissionToken token)
{
  require(std::find(m_commandBuffers.begin(), m_commandBuffers.end(),
                    &commandBuffer) != m_commandBuffers.end(),
          "VulkanCommandAllocator received an unknown command buffer");
  require(token.queue == m_queue.identity(),
          "VulkanCommandAllocator received a wrong-queue submission token");
  if(!m_lastSubmission.isValid() || token.value > m_lastSubmission.value)
    m_lastSubmission = token;
}

void VulkanCommandAllocator::releaseCommandBuffer(
  VulkanCommandBuffer& commandBuffer, VkCommandBuffer nativeCommandBuffer) noexcept
{
  const auto found =
    std::find(m_commandBuffers.begin(), m_commandBuffers.end(), &commandBuffer);
  if(found != m_commandBuffers.end())
    m_commandBuffers.erase(found);
  if(m_device != VK_NULL_HANDLE && m_pool != VK_NULL_HANDLE &&
     nativeCommandBuffer != VK_NULL_HANDLE)
  {
    vkFreeCommandBuffers(m_device, m_pool, 1, &nativeCommandBuffer);
  }
}

}  // namespace demo::rhi::vulkan