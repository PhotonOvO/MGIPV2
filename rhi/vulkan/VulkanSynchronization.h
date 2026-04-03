#pragma once

#include "../RHISynchronization.h"

struct VkDevice_T;
struct VkFence_T;
struct VkSemaphore_T;

using VkDevice    = VkDevice_T*;
using VkFence     = VkFence_T*;
using VkSemaphore = VkSemaphore_T*;

namespace demo {
namespace rhi {
namespace vulkan {

class VulkanFence final : public Fence
{
public:
  VulkanFence() = default;
  ~VulkanFence() override;

  void init(void* nativeDevice, bool signaled = false) override;
  void deinit() override;

  void wait(uint64_t timeout = UINT64_MAX) override;
  void reset() override;

  bool isSignaled() const override;

  uint64_t getNativeHandle() const override;

  VkFence nativeFence() const { return m_fence; }

private:
  VkDevice m_device{nullptr};
  VkFence  m_fence{nullptr};
};

class VulkanTimelineSemaphore final : public TimelineSemaphore
{
public:
  VulkanTimelineSemaphore() = default;
  ~VulkanTimelineSemaphore() override;

  void init(void* nativeDevice, uint64_t initialValue = 0) override;
  void deinit() override;

  void signal(uint64_t value) override;
  void wait(uint64_t value, uint64_t timeout = UINT64_MAX) override;

  uint64_t getCurrentValue() const override;

  uint64_t getNativeHandle() const override;

  VkSemaphore nativeSemaphore() const { return m_semaphore; }

private:
  VkDevice    m_device{nullptr};
  VkSemaphore m_semaphore{nullptr};
};

}  // namespace vulkan
}  // namespace rhi
}  // namespace demo
