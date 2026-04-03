#include "VulkanSynchronization.h"

#include <cassert>

#include "volk.h"

namespace demo {
namespace rhi {
namespace vulkan {
namespace {

#ifndef ASSERT
#define ASSERT(condition, message) assert((condition) && (message))
#endif

void checkVk(VkResult result, const char* message)
{
  ASSERT(result == VK_SUCCESS, message);
}

uint64_t toNativeU64(uintptr_t value)
{
  return static_cast<uint64_t>(value);
}

}  // namespace

VulkanFence::~VulkanFence()
{
  deinit();
}

void VulkanFence::init(void* nativeDevice, bool signaled)
{
  ASSERT(nativeDevice != nullptr, "VulkanFence::init requires VkDevice");
  ASSERT(m_device == VK_NULL_HANDLE, "VulkanFence::init already initialized");
  ASSERT(m_fence == VK_NULL_HANDLE, "VulkanFence::init found stale VkFence");

  m_device = static_cast<VkDevice>(nativeDevice);

  const VkFenceCreateInfo createInfo{
      .sType = VK_STRUCTURE_TYPE_FENCE_CREATE_INFO,
      .flags = signaled ? VK_FENCE_CREATE_SIGNALED_BIT : VkFenceCreateFlags(0),
  };
  checkVk(vkCreateFence(m_device, &createInfo, nullptr, &m_fence), "VulkanFence::init failed creating fence");
  ASSERT(m_fence != VK_NULL_HANDLE, "VulkanFence::init failed creating fence");
}

void VulkanFence::deinit()
{
  if(m_device == VK_NULL_HANDLE)
  {
    m_fence = VK_NULL_HANDLE;
    return;
  }

  if(m_fence != VK_NULL_HANDLE)
  {
    vkDestroyFence(m_device, m_fence, nullptr);
    m_fence = VK_NULL_HANDLE;
  }

  m_device = VK_NULL_HANDLE;
}

void VulkanFence::wait(uint64_t timeout)
{
  ASSERT(m_device != VK_NULL_HANDLE, "VulkanFence::wait requires VkDevice");
  ASSERT(m_fence != VK_NULL_HANDLE, "VulkanFence::wait requires VkFence");
  checkVk(vkWaitForFences(m_device, 1, &m_fence, VK_TRUE, timeout), "VulkanFence::wait failed");
}

void VulkanFence::reset()
{
  ASSERT(m_device != VK_NULL_HANDLE, "VulkanFence::reset requires VkDevice");
  ASSERT(m_fence != VK_NULL_HANDLE, "VulkanFence::reset requires VkFence");
  checkVk(vkResetFences(m_device, 1, &m_fence), "VulkanFence::reset failed");
}

bool VulkanFence::isSignaled() const
{
  ASSERT(m_device != VK_NULL_HANDLE, "VulkanFence::isSignaled requires VkDevice");
  ASSERT(m_fence != VK_NULL_HANDLE, "VulkanFence::isSignaled requires VkFence");

  const VkResult result = vkGetFenceStatus(m_device, m_fence);
  ASSERT(result == VK_SUCCESS || result == VK_NOT_READY, "VulkanFence::isSignaled failed querying status");
  return result == VK_SUCCESS;
}

uint64_t VulkanFence::getNativeHandle() const
{
  return toNativeU64(reinterpret_cast<uintptr_t>(m_fence));
}

VulkanTimelineSemaphore::~VulkanTimelineSemaphore()
{
  deinit();
}

void VulkanTimelineSemaphore::init(void* nativeDevice, uint64_t initialValue)
{
  ASSERT(nativeDevice != nullptr, "VulkanTimelineSemaphore::init requires VkDevice");
  ASSERT(m_device == VK_NULL_HANDLE, "VulkanTimelineSemaphore::init already initialized");
  ASSERT(m_semaphore == VK_NULL_HANDLE, "VulkanTimelineSemaphore::init found stale VkSemaphore");

  m_device = static_cast<VkDevice>(nativeDevice);

  const VkSemaphoreTypeCreateInfo timelineInfo{
      .sType         = VK_STRUCTURE_TYPE_SEMAPHORE_TYPE_CREATE_INFO,
      .semaphoreType = VK_SEMAPHORE_TYPE_TIMELINE,
      .initialValue  = initialValue,
  };

  const VkSemaphoreCreateInfo createInfo{
      .sType = VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO,
      .pNext = &timelineInfo,
  };

  checkVk(vkCreateSemaphore(m_device, &createInfo, nullptr, &m_semaphore),
          "VulkanTimelineSemaphore::init failed creating timeline semaphore");
  ASSERT(m_semaphore != VK_NULL_HANDLE, "VulkanTimelineSemaphore::init failed creating timeline semaphore");
}

void VulkanTimelineSemaphore::deinit()
{
  if(m_device == VK_NULL_HANDLE)
  {
    m_semaphore = VK_NULL_HANDLE;
    return;
  }

  if(m_semaphore != VK_NULL_HANDLE)
  {
    vkDestroySemaphore(m_device, m_semaphore, nullptr);
    m_semaphore = VK_NULL_HANDLE;
  }

  m_device = VK_NULL_HANDLE;
}

void VulkanTimelineSemaphore::signal(uint64_t value)
{
  ASSERT(m_device != VK_NULL_HANDLE, "VulkanTimelineSemaphore::signal requires VkDevice");
  ASSERT(m_semaphore != VK_NULL_HANDLE, "VulkanTimelineSemaphore::signal requires VkSemaphore");

  const VkSemaphoreSignalInfo signalInfo{
      .sType     = VK_STRUCTURE_TYPE_SEMAPHORE_SIGNAL_INFO,
      .semaphore = m_semaphore,
      .value     = value,
  };
  checkVk(vkSignalSemaphore(m_device, &signalInfo), "VulkanTimelineSemaphore::signal failed");
}

void VulkanTimelineSemaphore::wait(uint64_t value, uint64_t timeout)
{
  ASSERT(m_device != VK_NULL_HANDLE, "VulkanTimelineSemaphore::wait requires VkDevice");
  ASSERT(m_semaphore != VK_NULL_HANDLE, "VulkanTimelineSemaphore::wait requires VkSemaphore");

  const VkSemaphore         semaphore = m_semaphore;
  const VkSemaphoreWaitInfo waitInfo{
      .sType          = VK_STRUCTURE_TYPE_SEMAPHORE_WAIT_INFO,
      .semaphoreCount = 1,
      .pSemaphores    = &semaphore,
      .pValues        = &value,
  };
  checkVk(vkWaitSemaphores(m_device, &waitInfo, timeout), "VulkanTimelineSemaphore::wait failed");
}

uint64_t VulkanTimelineSemaphore::getCurrentValue() const
{
  ASSERT(m_device != VK_NULL_HANDLE, "VulkanTimelineSemaphore::getCurrentValue requires VkDevice");
  ASSERT(m_semaphore != VK_NULL_HANDLE, "VulkanTimelineSemaphore::getCurrentValue requires VkSemaphore");

  uint64_t currentValue = 0;
  checkVk(vkGetSemaphoreCounterValue(m_device, m_semaphore, &currentValue), "VulkanTimelineSemaphore::getCurrentValue failed");
  return currentValue;
}

uint64_t VulkanTimelineSemaphore::getNativeHandle() const
{
  return toNativeU64(reinterpret_cast<uintptr_t>(m_semaphore));
}

}  // namespace vulkan
}  // namespace rhi
}  // namespace demo
