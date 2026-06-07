#include "VulkanResourceTable.h"

#include "internal/VulkanCommon.h"

#include <cassert>
#include <utility>

namespace demo::rhi::vulkan {

PipelineHandle VulkanResourceTable::registerPipeline(uint32_t bindPoint, uint64_t nativePipeline,
                                                     uint32_t specializationVariant, uint64_t nativeLayout,
                                                     std::vector<PipelineRecord::RootBindingLowering> rootBindings,
                                                     bool owned,
                                                     bool ownsLayout)
{
  ASSERT(nativePipeline != 0, "Pipeline registry entries require a valid native pipeline");
  return m_pipelines.emplace(PipelineRecord{
      .bindPoint             = bindPoint,
      .nativePipeline        = nativePipeline,
      .specializationVariant = specializationVariant,
      .nativeLayout          = nativeLayout,
      .rootBindings          = std::move(rootBindings),
      .ownsLayout            = ownsLayout,
      .owned                 = owned,
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

TextureViewHandle VulkanResourceTable::registerTextureView(uint64_t nativeView, bool owned)
{
  ASSERT(nativeView != 0, "Texture view registry entries require a valid native image view");
  return m_textureViews.emplace(TextureViewRecord{.nativeView = nativeView, .owned = owned});
}

uint64_t VulkanResourceTable::resolveTextureView(TextureViewHandle handle) const
{
  const TextureViewRecord* record = m_textureViews.tryGet(handle);
  return record != nullptr ? record->nativeView : 0;
}

const TextureViewRecord* VulkanResourceTable::tryGetTextureView(TextureViewHandle handle) const
{
  return m_textureViews.tryGet(handle);
}

TextureViewRecord VulkanResourceTable::removeTextureView(TextureViewHandle handle)
{
  const TextureViewRecord* record = m_textureViews.tryGet(handle);
  const TextureViewRecord  copy   = record != nullptr ? *record : TextureViewRecord{};
  m_textureViews.destroy(handle);
  return copy;
}

TextureHandle VulkanResourceTable::registerTexture(uint64_t nativeImage, uint64_t nativeAllocation, bool owned)
{
  ASSERT(nativeImage != 0, "Texture registry entries require a valid native image");
  return m_textures.emplace(TextureRecord{.nativeImage = nativeImage, .nativeAllocation = nativeAllocation, .owned = owned});
}

uint64_t VulkanResourceTable::resolveTexture(TextureHandle handle) const
{
  const TextureRecord* record = m_textures.tryGet(handle);
  return record != nullptr ? record->nativeImage : 0;
}

const TextureRecord* VulkanResourceTable::tryGetTexture(TextureHandle handle) const
{
  return m_textures.tryGet(handle);
}

TextureRecord VulkanResourceTable::removeTexture(TextureHandle handle)
{
  const TextureRecord* record = m_textures.tryGet(handle);
  const TextureRecord  copy   = record != nullptr ? *record : TextureRecord{};
  m_textures.destroy(handle);
  return copy;
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

BufferHandle VulkanResourceTable::registerBuffer(const BufferRecord& record)
{
  ASSERT(record.nativeBuffer != 0, "Buffer registry entries require a valid native buffer");
  return m_buffers.emplace(record);
}

uint64_t VulkanResourceTable::resolveBuffer(BufferHandle handle) const
{
  const BufferRecord* record = m_buffers.tryGet(handle);
  return record != nullptr ? record->nativeBuffer : 0;
}

void VulkanResourceTable::updateBuffer(BufferHandle handle, uint64_t nativeBuffer, uint64_t gpuAddress)
{
  // Option B: per-frame / arena buffers keep a stable handle across native
  // reallocation; only the record's native pointer (and optional address) updates.
  BufferRecord* record = m_buffers.tryGet(handle);
  if(record != nullptr)
  {
    record->nativeBuffer = nativeBuffer;
    record->gpuAddress   = gpuAddress;
  }
}

const BufferRecord* VulkanResourceTable::tryGetBuffer(BufferHandle handle) const
{
  return m_buffers.tryGet(handle);
}

BufferRecord VulkanResourceTable::removeBuffer(BufferHandle handle)
{
  const BufferRecord* record = m_buffers.tryGet(handle);
  const BufferRecord  copy   = record != nullptr ? *record : BufferRecord{};
  m_buffers.destroy(handle);
  return copy;
}

SamplerHandle VulkanResourceTable::registerSampler(uint64_t nativeSampler)
{
  ASSERT(nativeSampler != 0, "Sampler registry entries require a valid native sampler");
  return m_samplers.emplace(SamplerRecord{.nativeSampler = nativeSampler});
}

uint64_t VulkanResourceTable::resolveSampler(SamplerHandle handle) const
{
  const SamplerRecord* record = m_samplers.tryGet(handle);
  return record != nullptr ? record->nativeSampler : 0;
}

SamplerRecord VulkanResourceTable::removeSampler(SamplerHandle handle)
{
  const SamplerRecord* record = m_samplers.tryGet(handle);
  const SamplerRecord  copy   = record != nullptr ? *record : SamplerRecord{};
  m_samplers.destroy(handle);
  return copy;
}

QueryPoolHandle VulkanResourceTable::registerQueryPool(uint64_t nativePool, uint32_t count)
{
  ASSERT(nativePool != 0, "Query pool registry entries require a valid native pool");
  return m_queryPools.emplace(QueryPoolRecord{.nativePool = nativePool, .count = count});
}

uint64_t VulkanResourceTable::resolveQueryPool(QueryPoolHandle handle) const
{
  const QueryPoolRecord* record = m_queryPools.tryGet(handle);
  return record != nullptr ? record->nativePool : 0;
}

QueryPoolRecord VulkanResourceTable::removeQueryPool(QueryPoolHandle handle)
{
  const QueryPoolRecord* record = m_queryPools.tryGet(handle);
  const QueryPoolRecord  copy   = record != nullptr ? *record : QueryPoolRecord{};
  m_queryPools.destroy(handle);
  return copy;
}

ArgumentLayoutHandle VulkanResourceTable::registerArgumentLayout(uint64_t nativeLayout, std::vector<uint32_t> dynamicBindings)
{
  ASSERT(nativeLayout != 0, "Argument layout registry entries require a valid native layout");
  return m_argumentLayouts.emplace(ArgumentLayoutRecord{.nativeLayout = nativeLayout, .dynamicBindings = std::move(dynamicBindings)});
}

uint64_t VulkanResourceTable::resolveArgumentLayout(ArgumentLayoutHandle handle) const
{
  const ArgumentLayoutRecord* record = m_argumentLayouts.tryGet(handle);
  return record != nullptr ? record->nativeLayout : 0;
}

const ArgumentLayoutRecord* VulkanResourceTable::tryGetArgumentLayout(ArgumentLayoutHandle handle) const
{
  return m_argumentLayouts.tryGet(handle);
}

ArgumentLayoutRecord VulkanResourceTable::removeArgumentLayout(ArgumentLayoutHandle handle)
{
  const ArgumentLayoutRecord* record = m_argumentLayouts.tryGet(handle);
  const ArgumentLayoutRecord  copy   = record != nullptr ? *record : ArgumentLayoutRecord{};
  m_argumentLayouts.destroy(handle);
  return copy;
}

ArgumentTableHandle VulkanResourceTable::registerArgumentTable(uint64_t nativeSet, ArgumentLayoutHandle layout, bool owned)
{
  ASSERT(nativeSet != 0, "Argument table registry entries require a valid native descriptor set");
  return m_argumentTables.emplace(ArgumentTableRecord{.nativeSet = nativeSet, .layout = layout, .owned = owned});
}

const ArgumentTableRecord* VulkanResourceTable::tryGetArgumentTable(ArgumentTableHandle handle) const
{
  return m_argumentTables.tryGet(handle);
}

uint64_t VulkanResourceTable::resolveArgumentTable(ArgumentTableHandle handle) const
{
  const ArgumentTableRecord* record = m_argumentTables.tryGet(handle);
  return record != nullptr ? record->nativeSet : 0;
}

ArgumentTableRecord VulkanResourceTable::removeArgumentTable(ArgumentTableHandle handle)
{
  const ArgumentTableRecord* record = m_argumentTables.tryGet(handle);
  const ArgumentTableRecord  copy   = record != nullptr ? *record : ArgumentTableRecord{};
  m_argumentTables.destroy(handle);
  return copy;
}

}  // namespace demo::rhi::vulkan
