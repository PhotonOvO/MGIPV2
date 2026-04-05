#include "VulkanDescriptor.h"

#include <cassert>
#include <cstddef>
#include <vector>

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

VkDescriptorType toVkDescriptorType(DescriptorKind type)
{
  switch(type)
  {
    case DescriptorKind::Sampler:
      return VK_DESCRIPTOR_TYPE_SAMPLER;
    case DescriptorKind::CombinedImageSampler:
      return VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
    case DescriptorKind::SampledImage:
      return VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE;
    case DescriptorKind::StorageImage:
      return VK_DESCRIPTOR_TYPE_STORAGE_IMAGE;
    case DescriptorKind::UniformTexelBuffer:
      return VK_DESCRIPTOR_TYPE_UNIFORM_TEXEL_BUFFER;
    case DescriptorKind::StorageTexelBuffer:
      return VK_DESCRIPTOR_TYPE_STORAGE_TEXEL_BUFFER;
    case DescriptorKind::UniformBuffer:
      return VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER;
    case DescriptorKind::StorageBuffer:
      return VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
    case DescriptorKind::UniformBufferDynamic:
      return VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER_DYNAMIC;
    case DescriptorKind::StorageBufferDynamic:
      return VK_DESCRIPTOR_TYPE_STORAGE_BUFFER_DYNAMIC;
    case DescriptorKind::InputAttachment:
      return VK_DESCRIPTOR_TYPE_INPUT_ATTACHMENT;
    default:
      ASSERT(false, "Unsupported DescriptorType");
      return VK_DESCRIPTOR_TYPE_MAX_ENUM;
  }
}

VkDescriptorType toVkDescriptorType(BindlessResourceType type)
{
  return toVkDescriptorType(toDescriptorType(type));
}

VkShaderStageFlags toVkShaderStageFlags(ResourceVisibility visibility)
{
  VkShaderStageFlags flags = 0;
  const uint32_t     bits  = static_cast<uint32_t>(visibility);
  if((bits & static_cast<uint32_t>(ResourceVisibility::vertex)) != 0)
  {
    flags |= VK_SHADER_STAGE_VERTEX_BIT;
  }
  if((bits & static_cast<uint32_t>(ResourceVisibility::fragment)) != 0)
  {
    flags |= VK_SHADER_STAGE_FRAGMENT_BIT;
  }
  if((bits & static_cast<uint32_t>(ResourceVisibility::compute)) != 0)
  {
    flags |= VK_SHADER_STAGE_COMPUTE_BIT;
  }
  return flags;
}

}  // namespace

VulkanBindTableLayout::~VulkanBindTableLayout()
{
  deinit();
}

void VulkanBindTableLayout::init(void* nativeDevice, const std::vector<BindTableLayoutEntry>& entries)
{
  ASSERT(nativeDevice != nullptr, "VulkanBindTableLayout::init requires VkDevice");
  ASSERT(m_device == VK_NULL_HANDLE, "VulkanBindTableLayout::init already initialized");
  ASSERT(m_layout == VK_NULL_HANDLE, "VulkanBindTableLayout::init found stale VkDescriptorSetLayout");

  m_device  = static_cast<VkDevice>(nativeDevice);
  m_entries = entries;
  m_logicalToBinding.clear();

  std::vector<VkDescriptorSetLayoutBinding> vkBindings;
  vkBindings.reserve(entries.size());

  for(const BindTableLayoutEntry& entry : entries)
  {
    ASSERT(isValidResourceIndex(entry.logicalIndex), "VulkanBindTableLayout::init requires valid logical index");
    ASSERT(m_logicalToBinding.find(entry.logicalIndex) == m_logicalToBinding.end(),
           "VulkanBindTableLayout::init logical index must be unique");

    // Use logicalIndex as the actual binding number (not sequential)
    const uint32_t binding = entry.logicalIndex;
    m_logicalToBinding.emplace(entry.logicalIndex, binding);

    vkBindings.push_back(VkDescriptorSetLayoutBinding{
        .binding            = binding,
        .descriptorType     = toVkDescriptorType(entry.resourceType),
        .descriptorCount    = entry.descriptorCount,
        .stageFlags         = toVkShaderStageFlags(entry.visibility),
        .pImmutableSamplers = nullptr,
    });
  }

  const VkDescriptorSetLayoutCreateInfo createInfo{
      .sType        = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO,
      .bindingCount = static_cast<uint32_t>(vkBindings.size()),
      .pBindings    = vkBindings.empty() ? nullptr : vkBindings.data(),
  };

  checkVk(vkCreateDescriptorSetLayout(m_device, &createInfo, nullptr, &m_layout),
          "VulkanBindTableLayout::init failed creating descriptor set layout");
  ASSERT(m_layout != VK_NULL_HANDLE, "VulkanBindTableLayout::init failed creating descriptor set layout");
}

void VulkanBindTableLayout::deinit()
{
  if(m_device == VK_NULL_HANDLE)
  {
    m_layout = VK_NULL_HANDLE;
    m_logicalToBinding.clear();
    m_entries.clear();
    return;
  }

  if(m_layout != VK_NULL_HANDLE)
  {
    vkDestroyDescriptorSetLayout(m_device, m_layout, nullptr);
    m_layout = VK_NULL_HANDLE;
  }

  m_logicalToBinding.clear();
  m_entries.clear();
  m_device = VK_NULL_HANDLE;
}

uint64_t VulkanBindTableLayout::getNativeHandle() const
{
  return toNativeU64(reinterpret_cast<uintptr_t>(m_layout));
}

bool VulkanBindTableLayout::resolveLogicalIndex(ResourceIndex logicalIndex, uint32_t& outBinding) const
{
  const auto it = m_logicalToBinding.find(logicalIndex);
  if(it == m_logicalToBinding.end())
  {
    return false;
  }

  outBinding = it->second;
  return true;
}

const std::vector<BindTableLayoutEntry>& VulkanBindTableLayout::entries() const
{
  return m_entries;
}

VulkanBindTable::~VulkanBindTable()
{
  deinit();
}

void VulkanBindTable::init(void* nativeDevice, const BindTableLayout& layout, uint32_t maxLogicalEntries)
{
  ASSERT(nativeDevice != nullptr, "VulkanBindTable::init requires VkDevice");
  ASSERT(m_device == VK_NULL_HANDLE, "VulkanBindTable::init already initialized");
  ASSERT(m_pool == VK_NULL_HANDLE, "VulkanBindTable::init found stale VkDescriptorPool");
  ASSERT(m_set == VK_NULL_HANDLE, "VulkanBindTable::init found stale VkDescriptorSet");

  m_device            = static_cast<VkDevice>(nativeDevice);
  m_layoutView        = dynamic_cast<const VulkanBindTableLayout*>(&layout);
  m_maxLogicalEntries = maxLogicalEntries;
  ASSERT(m_layoutView != nullptr, "VulkanBindTable::init requires VulkanBindTableLayout");

  std::unordered_map<VkDescriptorType, uint32_t> descriptorCounts;
  for(const BindTableLayoutEntry& entry : m_layoutView->entries())
  {
    descriptorCounts[toVkDescriptorType(entry.resourceType)] += entry.descriptorCount;
  }

  std::vector<VkDescriptorPoolSize> poolSizes;
  poolSizes.reserve(descriptorCounts.size());
  for(const auto& [descriptorType, count] : descriptorCounts)
  {
    poolSizes.push_back(VkDescriptorPoolSize{.type = descriptorType, .descriptorCount = count});
  }

  const VkDescriptorPoolCreateInfo poolCreateInfo{
      .sType         = VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO,
      .flags         = VK_DESCRIPTOR_POOL_CREATE_FREE_DESCRIPTOR_SET_BIT,
      .maxSets       = 1,
      .poolSizeCount = static_cast<uint32_t>(poolSizes.size()),
      .pPoolSizes    = poolSizes.empty() ? nullptr : poolSizes.data(),
  };

  checkVk(vkCreateDescriptorPool(m_device, &poolCreateInfo, nullptr, &m_pool), "VulkanBindTable::init failed creating descriptor pool");
  ASSERT(m_pool != VK_NULL_HANDLE, "VulkanBindTable::init failed creating descriptor pool");

  const VkDescriptorSetLayout vkLayout =
      reinterpret_cast<VkDescriptorSetLayout>(static_cast<uintptr_t>(layout.getNativeHandle()));
  ASSERT(vkLayout != VK_NULL_HANDLE, "VulkanBindTable::init requires valid bind table layout");

  const VkDescriptorSetAllocateInfo allocInfo{
      .sType              = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO,
      .descriptorPool     = m_pool,
      .descriptorSetCount = 1,
      .pSetLayouts        = &vkLayout,
  };
  checkVk(vkAllocateDescriptorSets(m_device, &allocInfo, &m_set), "VulkanBindTable::init failed allocating descriptor set");
}

void VulkanBindTable::deinit()
{
  if(m_device == VK_NULL_HANDLE)
  {
    m_pool              = VK_NULL_HANDLE;
    m_set               = VK_NULL_HANDLE;
    m_layoutView        = nullptr;
    m_maxLogicalEntries = 0;
    return;
  }

  // Destroying the pool automatically frees all descriptor sets allocated from it
  // So we don't need to call vkFreeDescriptorSets separately
  m_set = VK_NULL_HANDLE;

  if(m_pool != VK_NULL_HANDLE)
  {
    vkDestroyDescriptorPool(m_device, m_pool, nullptr);
    m_pool = VK_NULL_HANDLE;
  }

  m_layoutView        = nullptr;
  m_maxLogicalEntries = 0;
  m_device            = VK_NULL_HANDLE;
}

void VulkanBindTable::update(uint32_t writeCount, const BindTableWrite* writes)
{
  ASSERT(m_device != VK_NULL_HANDLE, "VulkanBindTable::update requires VkDevice");
  ASSERT(m_set != VK_NULL_HANDLE, "VulkanBindTable::update requires descriptor set");
  ASSERT(m_layoutView != nullptr, "VulkanBindTable::update requires layout mapping");
  ASSERT(writeCount == 0 || writes != nullptr, "VulkanBindTable::update requires writes");

  std::vector<VkWriteDescriptorSet>   vkWrites;
  std::vector<VkDescriptorImageInfo>  vkImageInfos;
  std::vector<VkDescriptorBufferInfo> vkBufferInfos;

  vkWrites.reserve(writeCount);
  vkImageInfos.reserve(writeCount);
  vkBufferInfos.reserve(writeCount);

  for(uint32_t i = 0; i < writeCount; ++i)
  {
    const BindTableWrite& write = writes[i];
    ASSERT(isValidResourceIndex(write.dstIndex), "VulkanBindTable::update invalid logical index");
    ASSERT(write.dstIndex < m_maxLogicalEntries, "VulkanBindTable::update logical index exceeds table capacity");
    ASSERT((write.updateFlags == BindlessUpdateFlags::none) || any(write.updateFlags), "VulkanBindTable::update invalid update flags");

    uint32_t   dstBinding = 0;
    const bool resolved   = m_layoutView->resolveLogicalIndex(write.dstIndex, dstBinding);
    ASSERT(resolved, "VulkanBindTable::update unresolved logical index");

    VkWriteDescriptorSet vkWrite{
        .sType           = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET,
        .dstSet          = m_set,
        .dstBinding      = dstBinding,
        .dstArrayElement = write.dstArrayElement,
        .descriptorCount = write.descriptorCount,
        .descriptorType  = toVkDescriptorType(write.resourceType),
    };

    if(write.pImageInfo != nullptr && write.descriptorCount > 0)
    {
      const size_t imageInfoOffset = vkImageInfos.size();
      for(uint32_t descriptorIndex = 0; descriptorIndex < write.descriptorCount; ++descriptorIndex)
      {
        const DescriptorImageInfo& imageInfo = write.pImageInfo[descriptorIndex];
        vkImageInfos.push_back(VkDescriptorImageInfo{
            .sampler     = reinterpret_cast<VkSampler>(static_cast<uintptr_t>(imageInfo.sampler)),
            .imageView   = reinterpret_cast<VkImageView>(static_cast<uintptr_t>(imageInfo.imageView)),
            .imageLayout = static_cast<VkImageLayout>(imageInfo.imageLayout),
        });
      }
      vkWrite.pImageInfo = vkImageInfos.data() + static_cast<std::ptrdiff_t>(imageInfoOffset);
    }

    if(write.pBufferInfo != nullptr && write.descriptorCount > 0)
    {
      const size_t bufferInfoOffset = vkBufferInfos.size();
      for(uint32_t descriptorIndex = 0; descriptorIndex < write.descriptorCount; ++descriptorIndex)
      {
        const DescriptorBufferInfo& bufferInfo = write.pBufferInfo[descriptorIndex];
        vkBufferInfos.push_back(VkDescriptorBufferInfo{
            .buffer = reinterpret_cast<VkBuffer>(static_cast<uintptr_t>(bufferInfo.buffer)),
            .offset = static_cast<VkDeviceSize>(bufferInfo.offset),
            .range  = static_cast<VkDeviceSize>(bufferInfo.range),
        });
      }
      vkWrite.pBufferInfo = vkBufferInfos.data() + static_cast<std::ptrdiff_t>(bufferInfoOffset);
    }

    vkWrites.push_back(vkWrite);
  }

  vkUpdateDescriptorSets(m_device, static_cast<uint32_t>(vkWrites.size()), vkWrites.data(), 0, nullptr);
}

uint64_t VulkanBindTable::getNativeHandle() const
{
  return toNativeU64(reinterpret_cast<uintptr_t>(m_set));
}

}  // namespace vulkan
}  // namespace rhi
}  // namespace demo
