#include "VulkanResourceTable.h"

#include "internal/VulkanCommon.h"

#include <algorithm>
#include <cassert>
#include <new>
#include <stdexcept>
#include <utility>

namespace demo::rhi::vulkan {

PipelineHandle VulkanResourceTable::registerPipeline(VkPipelineBindPoint bindPoint, VkPipeline nativePipeline,
                                                     uint32_t specializationVariant, VkPipelineLayout nativeLayout,
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

TextureViewHandle VulkanResourceTable::registerTextureView(
  VkImageView nativeView, TextureViewColdRecord cold)
{
  ASSERT(nativeView != 0, "Texture view registry entries require a valid native image view");
  const TextureViewHandle handle =
    m_textureViews.emplace(TextureViewHotRecord{.nativeView = nativeView});
  if(m_textureViewCold.size() <= handle.index)
    m_textureViewCold.resize(static_cast<size_t>(handle.index) + 1u);
  m_textureViewCold[handle.index] = std::move(cold);
  return handle;
}

VkImageView VulkanResourceTable::resolveTextureView(TextureViewHandle handle) const
{
  const TextureViewHotRecord* record = m_textureViews.tryGet(handle);
  return record != nullptr ? record->nativeView : VK_NULL_HANDLE;
}

const TextureViewHotRecord* VulkanResourceTable::tryGetTextureViewHot(
  TextureViewHandle handle) const
{
  return m_textureViews.tryGet(handle);
}

const TextureViewColdRecord* VulkanResourceTable::tryGetTextureViewCold(
  TextureViewHandle handle) const
{
  if(m_textureViews.tryGet(handle) == nullptr || handle.index >= m_textureViewCold.size())
    return nullptr;
  return &m_textureViewCold[handle.index];
}

TextureViewRecord VulkanResourceTable::removeTextureView(TextureViewHandle handle)
{
  const TextureViewHotRecord* hot = m_textureViews.tryGet(handle);
  if(hot == nullptr || handle.index >= m_textureViewCold.size())
    return {};
  TextureViewRecord record{.hot = *hot, .cold = std::move(m_textureViewCold[handle.index])};
  m_textureViewCold[handle.index] = {};
  m_textureViews.destroy(handle);
  return record;
}

TextureHandle VulkanResourceTable::registerTexture(
  VkImage nativeImage, TextureColdRecord cold)
{
  ASSERT(nativeImage != 0, "Texture registry entries require a valid native image");
  const TextureHandle handle =
    m_textures.emplace(TextureHotRecord{.nativeImage = nativeImage});
  if(m_textureCold.size() <= handle.index)
    m_textureCold.resize(static_cast<size_t>(handle.index) + 1u);
  m_textureCold[handle.index] = std::move(cold);
  return handle;
}

VkImage VulkanResourceTable::resolveTexture(TextureHandle handle) const
{
  const TextureHotRecord* record = m_textures.tryGet(handle);
  return record != nullptr ? record->nativeImage : VK_NULL_HANDLE;
}

const TextureHotRecord* VulkanResourceTable::tryGetTextureHot(TextureHandle handle) const
{
  return m_textures.tryGet(handle);
}

const TextureColdRecord* VulkanResourceTable::tryGetTextureCold(
  TextureHandle handle) const
{
  if(m_textures.tryGet(handle) == nullptr || handle.index >= m_textureCold.size())
    return nullptr;
  return &m_textureCold[handle.index];
}

TextureRecord VulkanResourceTable::removeTexture(TextureHandle handle)
{
  const TextureHotRecord* hot = m_textures.tryGet(handle);
  if(hot == nullptr || handle.index >= m_textureCold.size())
    return {};
  TextureRecord record{.hot = *hot, .cold = std::move(m_textureCold[handle.index])};
  m_textureCold[handle.index] = {};
  m_textures.destroy(handle);
  return record;
}

VkPipeline VulkanResourceTable::resolvePipeline(PipelineHandle handle, VkPipelineBindPoint expectedBindPoint) const
{
  const PipelineRecord* record = m_pipelines.tryGet(handle);
  ASSERT(record != nullptr, "PipelineHandle must resolve to an active pipeline record");
  ASSERT(record->bindPoint == expectedBindPoint, "PipelineHandle bind-point mismatch");
  ASSERT(record->nativePipeline != 0, "Pipeline record must own a valid native pipeline");
  return record->nativePipeline;
}

VkPipelineLayout VulkanResourceTable::resolvePipelineLayout(PipelineHandle handle) const
{
  const PipelineRecord* record = m_pipelines.tryGet(handle);
  return record != nullptr ? record->nativeLayout : VK_NULL_HANDLE;
}

BufferHandle VulkanResourceTable::registerBuffer(
  BufferHotRecord hot, BufferColdRecord cold)
{
  ASSERT(hot.nativeBuffer != 0, "Buffer registry entries require a valid native buffer");
  const BufferHandle handle = m_buffers.emplace(std::move(hot));
  if(m_bufferCold.size() <= handle.index)
    m_bufferCold.resize(static_cast<size_t>(handle.index) + 1u);
  m_bufferCold[handle.index] = std::move(cold);
  return handle;
}

VkBuffer VulkanResourceTable::resolveBuffer(BufferHandle handle) const
{
  const BufferHotRecord* record = m_buffers.tryGet(handle);
  return record != nullptr ? record->nativeBuffer : VK_NULL_HANDLE;
}

void VulkanResourceTable::updateBuffer(
  BufferHandle handle, VkBuffer nativeBuffer, VkDeviceAddress gpuAddress)
{
  BufferHotRecord* record = m_buffers.tryGet(handle);
  if(record != nullptr)
  {
    record->nativeBuffer = nativeBuffer;
    record->gpuAddress = gpuAddress;
  }
}

const BufferHotRecord* VulkanResourceTable::tryGetBufferHot(
  BufferHandle handle) const
{
  return m_buffers.tryGet(handle);
}

const BufferColdRecord* VulkanResourceTable::tryGetBufferCold(
  BufferHandle handle) const
{
  if(m_buffers.tryGet(handle) == nullptr || handle.index >= m_bufferCold.size())
    return nullptr;
  return &m_bufferCold[handle.index];
}

BufferRecord VulkanResourceTable::removeBuffer(BufferHandle handle)
{
  const BufferHotRecord* hot = m_buffers.tryGet(handle);
  if(hot == nullptr || handle.index >= m_bufferCold.size())
    return {};
  BufferRecord record{.hot = *hot, .cold = std::move(m_bufferCold[handle.index])};
  m_bufferCold[handle.index] = {};
  m_buffers.destroy(handle);
  return record;
}

SamplerHandle VulkanResourceTable::registerSampler(VkSampler nativeSampler)
{
  ASSERT(nativeSampler != 0, "Sampler registry entries require a valid native sampler");
  return m_samplers.emplace(SamplerRecord{.nativeSampler = nativeSampler});
}

VkSampler VulkanResourceTable::resolveSampler(SamplerHandle handle) const
{
  const SamplerRecord* record = m_samplers.tryGet(handle);
  return record != nullptr ? record->nativeSampler : VK_NULL_HANDLE;
}

SamplerRecord VulkanResourceTable::removeSampler(SamplerHandle handle)
{
  const SamplerRecord* record = m_samplers.tryGet(handle);
  const SamplerRecord  copy   = record != nullptr ? *record : SamplerRecord{};
  m_samplers.destroy(handle);
  return copy;
}

QueryPoolHandle VulkanResourceTable::registerQueryPool(VkQueryPool nativePool, uint32_t count)
{
  ASSERT(nativePool != 0, "Query pool registry entries require a valid native pool");
  return m_queryPools.emplace(QueryPoolRecord{.nativePool = nativePool, .count = count});
}

VkQueryPool VulkanResourceTable::resolveQueryPool(QueryPoolHandle handle) const
{
  const QueryPoolRecord* record = m_queryPools.tryGet(handle);
  return record != nullptr ? record->nativePool : VK_NULL_HANDLE;
}

QueryPoolRecord VulkanResourceTable::removeQueryPool(QueryPoolHandle handle)
{
  const QueryPoolRecord* record = m_queryPools.tryGet(handle);
  const QueryPoolRecord  copy   = record != nullptr ? *record : QueryPoolRecord{};
  m_queryPools.destroy(handle);
  return copy;
}

ShaderLibraryHandle VulkanResourceTable::registerShaderLibrary(
  VkShaderModule nativeModule, ShaderIRFormat format)
{
  ASSERT(nativeModule != 0, "Shader library registry entries require a valid shader module");
  return m_shaderLibraries.emplace(ShaderLibraryRecord{
    .nativeModule = nativeModule,
    .format = format,
  });
}

const ShaderLibraryRecord* VulkanResourceTable::tryGetShaderLibrary(
  ShaderLibraryHandle handle) const
{
  return m_shaderLibraries.tryGet(handle);
}

ShaderLibraryRecord VulkanResourceTable::removeShaderLibrary(
  ShaderLibraryHandle handle)
{
  const ShaderLibraryRecord* record = m_shaderLibraries.tryGet(handle);
  const ShaderLibraryRecord copy = record != nullptr ? *record : ShaderLibraryRecord{};
  m_shaderLibraries.destroy(handle);
  return copy;
}

ArgumentLayoutHandle VulkanResourceTable::registerArgumentLayout(VkDescriptorSetLayout nativeLayout, std::vector<uint32_t> dynamicBindings)
{
  ASSERT(nativeLayout != 0, "Argument layout registry entries require a valid native layout");
  return m_argumentLayouts.emplace(ArgumentLayoutRecord{.nativeLayout = nativeLayout, .dynamicBindings = std::move(dynamicBindings)});
}

VkDescriptorSetLayout VulkanResourceTable::resolveArgumentLayout(ArgumentLayoutHandle handle) const
{
  const ArgumentLayoutRecord* record = m_argumentLayouts.tryGet(handle);
  return record != nullptr ? record->nativeLayout : VK_NULL_HANDLE;
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

ArgumentTableHandle VulkanResourceTable::registerArgumentTable(VkDescriptorSet nativeSet, ArgumentLayoutHandle layout, ArgumentTableLifetime lifetime, bool owned)
{
  ASSERT(nativeSet != 0, "Argument table registry entries require a valid native descriptor set");
  return m_argumentTables.emplace(ArgumentTableRecord{.nativeSet = nativeSet, .layout = layout, .lifetime = lifetime, .owned = owned});
}

const ArgumentTableRecord* VulkanResourceTable::tryGetArgumentTable(ArgumentTableHandle handle) const
{
  return m_argumentTables.tryGet(handle);
}

ArgumentTableRecord* VulkanResourceTable::tryGetArgumentTableMutable(
  ArgumentTableHandle handle)
{
  return m_argumentTables.tryGet(handle);
}

bool VulkanResourceTable::validateArgumentTableForSubmit(
  ArgumentTableHandle handle) const
{
  const ArgumentTableRecord* record = m_argumentTables.tryGet(handle);
  if(record == nullptr)
    return false;
  return std::all_of(
    record->referencedResources.begin(), record->referencedResources.end(),
    [this](const ArgumentResourceReference& reference) {
      return isAlive(reference.resource);
    });
}

void VulkanResourceTable::markArgumentTableSubmitted(
  ArgumentTableHandle handle, SubmissionToken token)
{
  ArgumentTableRecord* record = m_argumentTables.tryGet(handle);
  if(record == nullptr)
    throw std::runtime_error("Vulkan command buffer references a stale argument table");
  if(!record->pendingUses.record(token))
    throw std::runtime_error("Vulkan argument-table queue dependency capacity exceeded");
  for(const ArgumentResourceReference& reference : record->referencedResources)
  {
    const ResidencyResource resource = reference.resource;
    switch(resource.kind)
    {
    case ResidencyResourceKind::buffer:
      markBufferSubmitted(BufferHandle{resource.index, resource.generation}, token);
      break;
    case ResidencyResourceKind::texture:
      markTextureSubmitted(TextureHandle{resource.index, resource.generation}, token);
      break;
    case ResidencyResourceKind::textureView:
      markTextureViewSubmitted(
        TextureViewHandle{resource.index, resource.generation}, token);
      break;
    case ResidencyResourceKind::sampler:
      markSamplerSubmitted(SamplerHandle{resource.index, resource.generation}, token);
      break;
    case ResidencyResourceKind::pipeline:
      markPipelineSubmitted(PipelineHandle{resource.index, resource.generation}, token);
      break;
    case ResidencyResourceKind::argumentTable:
    case ResidencyResourceKind::shaderLibrary:
      break;
    }
  }
}

void VulkanResourceTable::recordArgumentTableResource(
  ArgumentTableHandle handle, uint32_t binding, uint32_t arrayElement,
  ResidencyResource resource)
{
  ArgumentTableRecord* record = m_argumentTables.tryGet(handle);
  if(record == nullptr)
    throw std::runtime_error("Vulkan argument table resource tracking received a stale table");
  if(!isAlive(resource))
    throw std::runtime_error("Vulkan argument table resource tracking received a stale resource");
  const auto existing = std::find_if(
    record->referencedResources.begin(), record->referencedResources.end(),
    [binding, arrayElement, resource](const ArgumentResourceReference& reference) {
      return reference.binding == binding &&
             reference.arrayElement == arrayElement &&
             reference.resource.kind == resource.kind;
    });
  if(existing != record->referencedResources.end())
    existing->resource = resource;
  else
    record->referencedResources.push_back(
      ArgumentResourceReference{binding, arrayElement, resource});
}

void VulkanResourceTable::markPipelineSubmitted(
  PipelineHandle handle, SubmissionToken token)
{
  PipelineRecord* record = m_pipelines.tryGet(handle);
  if(record == nullptr || !record->pendingUses.record(token))
    throw std::runtime_error("Vulkan pipeline submission tracking failed");
}

void VulkanResourceTable::markTextureViewSubmitted(
  TextureViewHandle handle, SubmissionToken token)
{
  TextureViewHotRecord* hot = m_textureViews.tryGet(handle);
  const TextureViewColdRecord* cold = tryGetTextureViewCold(handle);
  if(hot == nullptr || cold == nullptr || !hot->pendingUses.record(token))
    throw std::runtime_error("Vulkan texture-view submission tracking failed");
  if(cold->parentTexture.isValid())
    markTextureSubmitted(cold->parentTexture, token);
}

void VulkanResourceTable::markTextureSubmitted(
  TextureHandle handle, SubmissionToken token)
{
  TextureHotRecord* record = m_textures.tryGet(handle);
  if(record == nullptr || !record->pendingUses.record(token))
    throw std::runtime_error("Vulkan texture submission tracking failed");
}

void VulkanResourceTable::markBufferSubmitted(
  BufferHandle handle, SubmissionToken token)
{
  BufferHotRecord* record = m_buffers.tryGet(handle);
  if(record == nullptr || !record->pendingUses.record(token))
    throw std::runtime_error("Vulkan buffer submission tracking failed");
}

void VulkanResourceTable::markSamplerSubmitted(
  SamplerHandle handle, SubmissionToken token)
{
  SamplerRecord* record = m_samplers.tryGet(handle);
  if(record == nullptr || !record->pendingUses.record(token))
    throw std::runtime_error("Vulkan sampler submission tracking failed");
}

void VulkanResourceTable::markQueryPoolSubmitted(
  QueryPoolHandle handle, SubmissionToken token)
{
  QueryPoolRecord* record = m_queryPools.tryGet(handle);
  if(record == nullptr || !record->pendingUses.record(token))
    throw std::runtime_error("Vulkan query-pool submission tracking failed");
}

void VulkanResourceTable::markResidencySetSubmitted(
  ResidencySetHandle handle, SubmissionToken token)
{
  ResidencySetRecord* record = m_residencySets.tryGet(handle);
  if(record == nullptr || !record->pendingUses.record(token))
    throw std::runtime_error("Vulkan residency-set submission tracking failed");
  for(const ResidencyResource resource : record->resources)
  {
    switch(resource.kind)
    {
    case ResidencyResourceKind::buffer:
      markBufferSubmitted(BufferHandle{resource.index, resource.generation}, token);
      break;
    case ResidencyResourceKind::texture:
      markTextureSubmitted(TextureHandle{resource.index, resource.generation}, token);
      break;
    case ResidencyResourceKind::textureView:
      markTextureViewSubmitted(
        TextureViewHandle{resource.index, resource.generation}, token);
      break;
    case ResidencyResourceKind::sampler:
      markSamplerSubmitted(SamplerHandle{resource.index, resource.generation}, token);
      break;
    case ResidencyResourceKind::pipeline:
      markPipelineSubmitted(PipelineHandle{resource.index, resource.generation}, token);
      break;
    case ResidencyResourceKind::argumentTable:
      markArgumentTableSubmitted(
        ArgumentTableHandle{resource.index, resource.generation}, token);
      break;
    case ResidencyResourceKind::shaderLibrary:
      break;
    }
  }
}

bool VulkanResourceTable::isAlive(ResidencyResource resource) const
{
  if(!resource.isValid())
    return false;
  switch(resource.kind)
  {
  case ResidencyResourceKind::buffer:
    return m_buffers.isAlive(BufferHandle{resource.index, resource.generation});
  case ResidencyResourceKind::texture:
    return m_textures.isAlive(TextureHandle{resource.index, resource.generation});
  case ResidencyResourceKind::textureView:
    return m_textureViews.isAlive(
      TextureViewHandle{resource.index, resource.generation});
  case ResidencyResourceKind::sampler:
    return m_samplers.isAlive(SamplerHandle{resource.index, resource.generation});
  case ResidencyResourceKind::shaderLibrary:
    return m_shaderLibraries.isAlive(
      ShaderLibraryHandle{resource.index, resource.generation});
  case ResidencyResourceKind::pipeline:
    return m_pipelines.isAlive(PipelineHandle{resource.index, resource.generation});
  case ResidencyResourceKind::argumentTable:
    return m_argumentTables.isAlive(
      ArgumentTableHandle{resource.index, resource.generation});
  }
  return false;
}

VkDescriptorSet VulkanResourceTable::resolveArgumentTable(ArgumentTableHandle handle) const
{
  const ArgumentTableRecord* record = m_argumentTables.tryGet(handle);
  return record != nullptr ? record->nativeSet : VK_NULL_HANDLE;
}

ArgumentTableRecord VulkanResourceTable::removeArgumentTable(ArgumentTableHandle handle)
{
  const ArgumentTableRecord* record = m_argumentTables.tryGet(handle);
  const ArgumentTableRecord  copy   = record != nullptr ? *record : ArgumentTableRecord{};
  m_argumentTables.destroy(handle);
  return copy;
}

Result<ResidencySetHandle> VulkanResourceTable::registerResidencySet(
  const ResidencySetDesc& desc)
{
  if(desc.maxResources == 0)
    return Result<ResidencySetHandle>::fail(
      RHIErrorCode::invalidArgument, "ResidencySet maxResources must be non-zero");
  ResidencySetRecord record{.maxResources = desc.maxResources};
  try
  {
    record.resources.reserve(desc.maxResources);
    return Result<ResidencySetHandle>::ok(m_residencySets.emplace(std::move(record)));
  }
  catch(const std::bad_alloc&)
  {
    return Result<ResidencySetHandle>::fail(
      RHIErrorCode::outOfMemory, "ResidencySet allocation failed");
  }
}

RHIResult VulkanResourceTable::removeResidencySet(ResidencySetHandle handle)
{
  if(!m_residencySets.destroy(handle))
    return RHIResult::fail(RHIErrorCode::invalidHandle, "ResidencySet handle is stale");
  return RHIResult::ok();
}

RHIResult VulkanResourceTable::updateResidencySet(
  ResidencySetHandle handle, const ResidencyUpdateBatch& batch)
{
  ResidencySetRecord* set = m_residencySets.tryGet(handle);
  if(set == nullptr)
    return RHIResult::fail(RHIErrorCode::invalidHandle, "ResidencySet handle is stale");

  const auto resourceIsValid = [this](const ResidencyResource& resource) {
    return isAlive(resource);
  };

  for(const ResidencyResource& resource : batch.remove)
  {
    if(!resource.isValid())
      return RHIResult::fail(RHIErrorCode::invalidArgument, "ResidencySet removal is invalid");
    const auto it = std::find(set->resources.begin(), set->resources.end(), resource);
    if(it == set->resources.end())
      return RHIResult::fail(RHIErrorCode::invalidHandle, "ResidencySet removal was not present");
    set->resources.erase(it);
  }

  for(const ResidencyResource& resource : batch.add)
  {
    if(!resourceIsValid(resource))
      return RHIResult::fail(RHIErrorCode::invalidHandle, "ResidencySet resource handle is stale");
    if(std::find(set->resources.begin(), set->resources.end(), resource) != set->resources.end())
      continue;
    if(set->resources.size() == set->maxResources)
      return RHIResult::fail(RHIErrorCode::invalidState, "ResidencySet capacity exceeded");
    set->resources.push_back(resource);
  }
  return RHIResult::ok();
}

bool VulkanResourceTable::isValidResidencySet(ResidencySetHandle handle) const
{
  return m_residencySets.isAlive(handle);
}

bool VulkanResourceTable::validateResidencySetForSubmit(
  ResidencySetHandle handle) const
{
  const ResidencySetRecord* record = m_residencySets.tryGet(handle);
  if(record == nullptr)
    return false;
  return std::all_of(
    record->resources.begin(), record->resources.end(),
    [this](ResidencyResource resource) { return isAlive(resource); });
}
}  // namespace demo::rhi::vulkan
