#pragma once

#include "../RHICommandPool.h"

struct VkDevice_T;
struct VkCommandPool_T;
struct VkCommandBuffer_T;

using VkDevice        = VkDevice_T*;
using VkCommandPool   = VkCommandPool_T*;
using VkCommandBuffer = VkCommandBuffer_T*;

namespace demo {
namespace rhi {
namespace vulkan {


class VulkanCommandPool final : public CommandPool
{
public:
  VulkanCommandPool() = default;
  ~VulkanCommandPool() override;

  void init(void* nativeDevice, QueueClass queueClass, uint32_t queueFamilyIndex) override;
  void deinit() override;
  void reset() override;

  VkCommandBuffer allocateNativeCommandBuffer();
  void            freeNativeCommandBuffer(VkCommandBuffer commandBuffer);

  uint64_t getBackendHandle() const override;

  [[nodiscard]] VkCommandPool nativePool() const { return m_pool; }

private:
  VkDevice      m_device{nullptr};
  VkCommandPool m_pool{nullptr};
  QueueClass    m_queueClass{QueueClass::graphics};
  uint32_t      m_queueFamilyIndex{~0U};
};

}  // namespace vulkan
}  // namespace rhi
}  // namespace demo
