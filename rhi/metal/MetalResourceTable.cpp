#include "MetalResourceTable.h"

namespace demo::rhi::metal {
namespace {

template <typename Value, typename Pool, typename Handle>
Value removeRecord(Pool& pool, Handle handle)
{
  Value result{};
  if(Value* record = pool.tryGet(handle))
  {
    result = std::move(*record);
    pool.destroy(handle);
  }
  return result;
}

}  // namespace

const MetalArgumentPlacement* MetalPipelineBindingLayout::findArgument(
  uint32_t tableSlot, uint32_t binding) const noexcept
{
  for(const MetalArgumentPlacement& placement : arguments)
  {
    if(placement.tableSlot == tableSlot && placement.binding == binding)
      return &placement;
  }
  return nullptr;
}

const MetalRootPlacement* MetalPipelineBindingLayout::findRoot(uint32_t slot) const noexcept
{
  for(const MetalRootPlacement& placement : roots)
  {
    if(placement.slot == slot)
      return &placement;
  }
  return nullptr;
}

const MetalVertexPlacement* MetalPipelineBindingLayout::findVertex(uint32_t binding) const noexcept
{
  for(const MetalVertexPlacement& placement : vertexBuffers)
  {
    if(placement.binding == binding)
      return &placement;
  }
  return nullptr;
}

BufferHandle MetalResourceTable::registerBuffer(MetalBufferRecord record)
{
  return m_buffers.emplace(std::move(record));
}

MetalBufferRecord* MetalResourceTable::tryGetBuffer(BufferHandle handle) { return m_buffers.tryGet(handle); }
const MetalBufferRecord* MetalResourceTable::tryGetBuffer(BufferHandle handle) const { return m_buffers.tryGet(handle); }
MetalBufferRecord MetalResourceTable::removeBuffer(BufferHandle handle)
{
  return removeRecord<MetalBufferRecord>(m_buffers, handle);
}

TextureHandle MetalResourceTable::registerTexture(MetalTextureRecord record)
{
  return m_textures.emplace(std::move(record));
}

MetalTextureRecord* MetalResourceTable::tryGetTexture(TextureHandle handle) { return m_textures.tryGet(handle); }
const MetalTextureRecord* MetalResourceTable::tryGetTexture(TextureHandle handle) const { return m_textures.tryGet(handle); }
MetalTextureRecord MetalResourceTable::removeTexture(TextureHandle handle)
{
  return removeRecord<MetalTextureRecord>(m_textures, handle);
}

TextureViewHandle MetalResourceTable::registerTextureView(MetalTextureViewRecord record)
{
  return m_textureViews.emplace(std::move(record));
}

MetalTextureViewRecord* MetalResourceTable::tryGetTextureView(TextureViewHandle handle) { return m_textureViews.tryGet(handle); }
const MetalTextureViewRecord* MetalResourceTable::tryGetTextureView(TextureViewHandle handle) const { return m_textureViews.tryGet(handle); }
MetalTextureViewRecord MetalResourceTable::removeTextureView(TextureViewHandle handle)
{
  return removeRecord<MetalTextureViewRecord>(m_textureViews, handle);
}

SamplerHandle MetalResourceTable::registerSampler(MetalSamplerRecord record)
{
  return m_samplers.emplace(std::move(record));
}

MetalSamplerRecord* MetalResourceTable::tryGetSampler(SamplerHandle handle) { return m_samplers.tryGet(handle); }
const MetalSamplerRecord* MetalResourceTable::tryGetSampler(SamplerHandle handle) const { return m_samplers.tryGet(handle); }
MetalSamplerRecord MetalResourceTable::removeSampler(SamplerHandle handle)
{
  return removeRecord<MetalSamplerRecord>(m_samplers, handle);
}

QueryPoolHandle MetalResourceTable::registerQueryPool(MetalQueryPoolRecord record)
{
  return m_queryPools.emplace(std::move(record));
}

MetalQueryPoolRecord* MetalResourceTable::tryGetQueryPool(QueryPoolHandle handle) { return m_queryPools.tryGet(handle); }
const MetalQueryPoolRecord* MetalResourceTable::tryGetQueryPool(QueryPoolHandle handle) const { return m_queryPools.tryGet(handle); }
MetalQueryPoolRecord MetalResourceTable::removeQueryPool(QueryPoolHandle handle)
{
  return removeRecord<MetalQueryPoolRecord>(m_queryPools, handle);
}

ShaderLibraryHandle MetalResourceTable::registerShaderLibrary(MetalShaderLibraryRecord record)
{
  return m_shaderLibraries.emplace(std::move(record));
}

MetalShaderLibraryRecord* MetalResourceTable::tryGetShaderLibrary(ShaderLibraryHandle handle) { return m_shaderLibraries.tryGet(handle); }
const MetalShaderLibraryRecord* MetalResourceTable::tryGetShaderLibrary(ShaderLibraryHandle handle) const { return m_shaderLibraries.tryGet(handle); }
MetalShaderLibraryRecord MetalResourceTable::removeShaderLibrary(ShaderLibraryHandle handle)
{
  return removeRecord<MetalShaderLibraryRecord>(m_shaderLibraries, handle);
}

ArgumentLayoutHandle MetalResourceTable::registerArgumentLayout(MetalArgumentLayoutRecord record)
{
  return m_argumentLayouts.emplace(std::move(record));
}

MetalArgumentLayoutRecord* MetalResourceTable::tryGetArgumentLayout(ArgumentLayoutHandle handle) { return m_argumentLayouts.tryGet(handle); }
const MetalArgumentLayoutRecord* MetalResourceTable::tryGetArgumentLayout(ArgumentLayoutHandle handle) const { return m_argumentLayouts.tryGet(handle); }
MetalArgumentLayoutRecord MetalResourceTable::removeArgumentLayout(ArgumentLayoutHandle handle)
{
  return removeRecord<MetalArgumentLayoutRecord>(m_argumentLayouts, handle);
}

ArgumentTableHandle MetalResourceTable::registerArgumentTable(MetalArgumentTableRecord record)
{
  return m_argumentTables.emplace(std::move(record));
}

MetalArgumentTableRecord* MetalResourceTable::tryGetArgumentTable(ArgumentTableHandle handle) { return m_argumentTables.tryGet(handle); }
const MetalArgumentTableRecord* MetalResourceTable::tryGetArgumentTable(ArgumentTableHandle handle) const { return m_argumentTables.tryGet(handle); }
MetalArgumentTableRecord MetalResourceTable::removeArgumentTable(ArgumentTableHandle handle)
{
  return removeRecord<MetalArgumentTableRecord>(m_argumentTables, handle);
}

PipelineHandle MetalResourceTable::registerPipeline(MetalPipelineRecord record)
{
  return m_pipelines.emplace(std::move(record));
}

MetalPipelineRecord* MetalResourceTable::tryGetPipeline(PipelineHandle handle) { return m_pipelines.tryGet(handle); }
const MetalPipelineRecord* MetalResourceTable::tryGetPipeline(PipelineHandle handle) const { return m_pipelines.tryGet(handle); }
MetalPipelineRecord MetalResourceTable::removePipeline(PipelineHandle handle)
{
  return removeRecord<MetalPipelineRecord>(m_pipelines, handle);
}

ResidencySetHandle MetalResourceTable::registerResidencySet(MetalResidencySetRecord record)
{
  return m_residencySets.emplace(std::move(record));
}

MetalResidencySetRecord* MetalResourceTable::tryGetResidencySet(ResidencySetHandle handle) { return m_residencySets.tryGet(handle); }
const MetalResidencySetRecord* MetalResourceTable::tryGetResidencySet(ResidencySetHandle handle) const { return m_residencySets.tryGet(handle); }
MetalResidencySetRecord MetalResourceTable::removeResidencySet(ResidencySetHandle handle)
{
  return removeRecord<MetalResidencySetRecord>(m_residencySets, handle);
}

bool MetalResourceTable::isAlive(ResidencyResource resource) const
{
  switch(resource.kind)
  {
  case ResidencyResourceKind::buffer:
    return m_buffers.isAlive(BufferHandle{resource.index, resource.generation});
  case ResidencyResourceKind::texture:
    return m_textures.isAlive(TextureHandle{resource.index, resource.generation});
  case ResidencyResourceKind::textureView:
    return m_textureViews.isAlive(TextureViewHandle{resource.index, resource.generation});
  case ResidencyResourceKind::sampler:
    return m_samplers.isAlive(SamplerHandle{resource.index, resource.generation});
  case ResidencyResourceKind::shaderLibrary:
    return m_shaderLibraries.isAlive(ShaderLibraryHandle{resource.index, resource.generation});
  case ResidencyResourceKind::pipeline:
    return m_pipelines.isAlive(PipelineHandle{resource.index, resource.generation});
  case ResidencyResourceKind::argumentTable:
    return m_argumentTables.isAlive(ArgumentTableHandle{resource.index, resource.generation});
  }
  return false;
}

}  // namespace demo::rhi::metal
