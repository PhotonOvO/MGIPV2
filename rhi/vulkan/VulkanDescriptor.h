#pragma once

#include "../RHIDescriptor.h"

#include <unordered_map>
#include <vector>

struct VkDevice_T;
struct VkDescriptorSetLayout_T;
struct VkDescriptorPool_T;
struct VkDescriptorSet_T;

using VkDevice              = VkDevice_T*;
using VkDescriptorSetLayout = VkDescriptorSetLayout_T*;
using VkDescriptorPool      = VkDescriptorPool_T*;
using VkDescriptorSet       = VkDescriptorSet_T*;

namespace demo {
namespace rhi {
namespace vulkan {

class VulkanBindTableLayout final : public BindTableLayout
{
public:
  VulkanBindTableLayout() = default;
  ~VulkanBindTableLayout() override;

  void init(void* nativeDevice, const std::vector<BindTableLayoutEntry>& entries) override;
  void deinit() override;

  uint64_t getNativeHandle() const override;

  bool                                     resolveLogicalIndex(ResourceIndex logicalIndex, uint32_t& outBinding) const;
  const std::vector<BindTableLayoutEntry>& entries() const;

  // Direct access to Vulkan descriptor set layout
  VkDescriptorSetLayout getVkDescriptorSetLayout() const { return m_layout; }

private:
  VkDevice                                    m_device{nullptr};
  VkDescriptorSetLayout                       m_layout{nullptr};
  std::unordered_map<ResourceIndex, uint32_t> m_logicalToBinding;
  std::vector<BindTableLayoutEntry>           m_entries;
};

class VulkanBindTable final : public BindTable
{
public:
  VulkanBindTable() = default;
  ~VulkanBindTable() override;

  void init(void* nativeDevice, const BindTableLayout& layout, uint32_t maxLogicalEntries) override;
  void deinit() override;

  void update(uint32_t writeCount, const BindTableWrite* writes) override;

  uint64_t getNativeHandle() const override;

  // Direct access to Vulkan descriptor set
  VkDescriptorSet getVkDescriptorSet() const { return m_set; }

private:
  VkDevice         m_device{nullptr};
  VkDescriptorPool m_pool{nullptr};
  VkDescriptorSet  m_set{nullptr};

  const VulkanBindTableLayout* m_layoutView{nullptr};
  uint32_t                     m_maxLogicalEntries{0};
};

}  // namespace vulkan
}  // namespace rhi
}  // namespace demo
