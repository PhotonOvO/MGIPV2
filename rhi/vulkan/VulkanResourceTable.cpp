#include "VulkanResourceTable.h"

#include "../RHIDescriptor.h"
#include "../../common/Common.h"

#include <cassert>

namespace demo::rhi::vulkan {

namespace {
[[nodiscard]] uint64_t packHandle(BindGroupHandle handle)
{
  return (static_cast<uint64_t>(handle.index) << 32) | static_cast<uint64_t>(handle.generation);
}
}  // namespace

PipelineHandle VulkanResourceTable::registerPipeline(uint32_t bindPoint, uint64_t nativePipeline,
                                                     uint32_t specializationVariant, uint64_t nativeLayout)
{
  ASSERT(nativePipeline != 0, "Pipeline registry entries require a valid native pipeline");
  return m_pipelines.emplace(PipelineRecord{
      .bindPoint             = bindPoint,
      .nativePipeline        = nativePipeline,
      .specializationVariant = specializationVariant,
      .nativeLayout          = nativeLayout,
  });
}

const PipelineRecord* VulkanResourceTable::tryGetPipeline(PipelineHandle handle) const
{
  return m_pipelines.tryGet(handle);
}

void VulkanResourceTable::destroyPipeline(PipelineHandle handle)
{
  m_pipelines.destroy(handle);
}

uint64_t VulkanResourceTable::resolvePipeline(PipelineHandle handle, uint32_t expectedBindPoint) const
{
  const PipelineRecord* record = m_pipelines.tryGet(handle);
  ASSERT(record != nullptr, "PipelineHandle must resolve to an active pipeline record");
  ASSERT(record->bindPoint == expectedBindPoint, "PipelineHandle bind-point mismatch");
  ASSERT(record->nativePipeline != 0, "Pipeline record must own a valid native pipeline");
  return record->nativePipeline;
}

uint64_t VulkanResourceTable::resolvePipelineLayout(PipelineHandle handle) const
{
  const PipelineRecord* record = m_pipelines.tryGet(handle);
  return record != nullptr ? record->nativeLayout : 0;
}

uint64_t VulkanResourceTable::resolveBindGroupDescriptorSet(BindGroupHandle handle) const
{
  const auto it = m_bindGroupTables.find(packHandle(handle));
  if(it == m_bindGroupTables.end() || it->second == nullptr)
  {
    return 0;
  }
  return it->second->getNativeHandle();
}

void VulkanResourceTable::registerBindGroup(BindGroupHandle handle, BindTable* table)
{
  m_bindGroupTables[packHandle(handle)] = table;
}

void VulkanResourceTable::unregisterBindGroup(BindGroupHandle handle)
{
  m_bindGroupTables.erase(packHandle(handle));
}

}  // namespace demo::rhi::vulkan
