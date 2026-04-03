#pragma once

#include "../RHICommandPool.h"
#include "VulkanCommandList.h"

#include <memory>
#include <vector>

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

  CommandList* allocateCommandList() override;
  void         freeCommandList(CommandList* cmdList) override;

  void allocateCommandLists(uint32_t count, CommandList** cmdLists) override;
  void freeCommandLists(uint32_t count, CommandList** cmdLists) override;

  uint64_t getNativeHandle() const override;

  [[nodiscard]] VkCommandPool nativePool() const { return m_pool; }

private:
  struct CommandListRecord
  {
    VkCommandBuffer                    commandBuffer{nullptr};
    std::unique_ptr<VulkanCommandList> commandList;
  };

  size_t findRecordIndex(const CommandList* cmdList) const;

  VkDevice                       m_device{nullptr};
  VkCommandPool                  m_pool{nullptr};
  QueueClass                     m_queueClass{QueueClass::graphics};
  uint32_t                       m_queueFamilyIndex{~0U};
  std::vector<CommandListRecord> m_records;
};

}  // namespace vulkan
}  // namespace rhi
}  // namespace demo
