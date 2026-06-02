#include "VulkanCommandList.h"

#include "VulkanResourceTable.h"

#include <stdexcept>

namespace demo::rhi::vulkan {

namespace {

VkPipelineStageFlags2 toVkStageMask(PipelineStage stage)
{
  uint32_t              mask = static_cast<uint32_t>(stage);
  VkPipelineStageFlags2 result{0};
  if((mask & static_cast<uint32_t>(PipelineStage::TopOfPipe)) != 0)
  {
    result |= VK_PIPELINE_STAGE_2_TOP_OF_PIPE_BIT;
  }
  if((mask & static_cast<uint32_t>(PipelineStage::VertexShader)) != 0)
  {
    result |= VK_PIPELINE_STAGE_2_VERTEX_SHADER_BIT;
  }
  if((mask & static_cast<uint32_t>(PipelineStage::FragmentShader)) != 0)
  {
    result |= VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT;
  }
  if((mask & static_cast<uint32_t>(PipelineStage::Compute)) != 0)
  {
    result |= VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT;
  }
  if((mask & static_cast<uint32_t>(PipelineStage::Transfer)) != 0)
  {
    result |= VK_PIPELINE_STAGE_2_TRANSFER_BIT;
  }
  if((mask & static_cast<uint32_t>(PipelineStage::BottomOfPipe)) != 0)
  {
    result |= VK_PIPELINE_STAGE_2_BOTTOM_OF_PIPE_BIT;
  }
  if((mask & static_cast<uint32_t>(PipelineStage::DrawIndirect)) != 0)
  {
    result |= VK_PIPELINE_STAGE_2_DRAW_INDIRECT_BIT;
  }
  if((mask & static_cast<uint32_t>(PipelineStage::Host)) != 0)
  {
    result |= VK_PIPELINE_STAGE_2_HOST_BIT;
  }
  return result == 0 ? VK_PIPELINE_STAGE_2_TOP_OF_PIPE_BIT : result;
}

VkAccessFlags2 toVkAccessMask(ResourceAccess access, PipelineStage stage)
{
  const uint32_t stageMask = static_cast<uint32_t>(stage);
  const bool     shaderStage =
      (stageMask & (static_cast<uint32_t>(PipelineStage::VertexShader) |
                    static_cast<uint32_t>(PipelineStage::FragmentShader) |
                    static_cast<uint32_t>(PipelineStage::Compute))) != 0;
  const bool transferStage = (stageMask & static_cast<uint32_t>(PipelineStage::Transfer)) != 0;
  const bool indirectStage = (stageMask & static_cast<uint32_t>(PipelineStage::DrawIndirect)) != 0;
  const bool hostStage     = (stageMask & static_cast<uint32_t>(PipelineStage::Host)) != 0;

  VkAccessFlags2 result = VK_ACCESS_2_NONE;
  switch(access)
  {
    case ResourceAccess::read:
      if(shaderStage)
      {
        result |= VK_ACCESS_2_SHADER_READ_BIT;
      }
      if(transferStage)
      {
        result |= VK_ACCESS_2_TRANSFER_READ_BIT;
      }
      if(indirectStage)
      {
        result |= VK_ACCESS_2_INDIRECT_COMMAND_READ_BIT;
      }
      if(hostStage)
      {
        result |= VK_ACCESS_2_HOST_READ_BIT;
      }
      return result;
    case ResourceAccess::write:
      if(shaderStage)
      {
        result |= VK_ACCESS_2_SHADER_WRITE_BIT;
      }
      if(transferStage)
      {
        result |= VK_ACCESS_2_TRANSFER_WRITE_BIT;
      }
      if(hostStage)
      {
        result |= VK_ACCESS_2_HOST_WRITE_BIT;
      }
      return result;
    case ResourceAccess::readWrite:
      if(shaderStage)
      {
        result |= VK_ACCESS_2_SHADER_READ_BIT | VK_ACCESS_2_SHADER_WRITE_BIT;
      }
      if(transferStage)
      {
        result |= VK_ACCESS_2_TRANSFER_READ_BIT | VK_ACCESS_2_TRANSFER_WRITE_BIT;
      }
      if(indirectStage)
      {
        result |= VK_ACCESS_2_INDIRECT_COMMAND_READ_BIT;
      }
      if(hostStage)
      {
        result |= VK_ACCESS_2_HOST_READ_BIT | VK_ACCESS_2_HOST_WRITE_BIT;
      }
      return result;
    default:
      return VK_ACCESS_2_NONE;
  }
}

VkImageAspectFlags toVkAspectMask(TextureAspect aspect)
{
  switch(aspect)
  {
    case TextureAspect::depth:
      return VK_IMAGE_ASPECT_DEPTH_BIT;
    case TextureAspect::depthStencil:
      return VK_IMAGE_ASPECT_DEPTH_BIT | VK_IMAGE_ASPECT_STENCIL_BIT;
    default:
      return VK_IMAGE_ASPECT_COLOR_BIT;
  }
}

VkImageLayout toVkImageLayout(ResourceState state)
{
  switch(state)
  {
    case ResourceState::Present:
      return VK_IMAGE_LAYOUT_PRESENT_SRC_KHR;
    case ResourceState::Undefined:
      return VK_IMAGE_LAYOUT_UNDEFINED;
    case ResourceState::ColorAttachment:
      return VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
    case ResourceState::DepthStencilAttachment:
      return VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL;
    case ResourceState::ShaderRead:
      return VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
    case ResourceState::TransferSrc:
      return VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL;
    case ResourceState::TransferDst:
      return VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
    default:
      return VK_IMAGE_LAYOUT_GENERAL;
  }
}

VkAccessFlags2 stateToDefaultAccess(ResourceState state)
{
  switch(state)
  {
    case ResourceState::ShaderRead:
      return VK_ACCESS_2_SHADER_READ_BIT;
    case ResourceState::TransferSrc:
      return VK_ACCESS_2_TRANSFER_READ_BIT;
    case ResourceState::Present:
      return VK_ACCESS_2_NONE;  // Present src stage is BottomOfPipe, no access needed
    case ResourceState::TransferDst:
      return VK_ACCESS_2_TRANSFER_WRITE_BIT;
    case ResourceState::ColorAttachment:
      return VK_ACCESS_2_COLOR_ATTACHMENT_READ_BIT | VK_ACCESS_2_COLOR_ATTACHMENT_WRITE_BIT;
    case ResourceState::DepthStencilAttachment:
      return VK_ACCESS_2_DEPTH_STENCIL_ATTACHMENT_READ_BIT | VK_ACCESS_2_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT;
    case ResourceState::Undefined:
      return VK_ACCESS_2_NONE;
    default:  // General, ShaderWrite, etc.
      return VK_ACCESS_2_SHADER_READ_BIT | VK_ACCESS_2_SHADER_WRITE_BIT;
  }
}

VkPipelineStageFlags2 stateToDefaultStage(ResourceState state)
{
  switch(state)
  {
    case ResourceState::ColorAttachment:
      return VK_PIPELINE_STAGE_2_COLOR_ATTACHMENT_OUTPUT_BIT;
    case ResourceState::DepthStencilAttachment:
      return VK_PIPELINE_STAGE_2_EARLY_FRAGMENT_TESTS_BIT | VK_PIPELINE_STAGE_2_LATE_FRAGMENT_TESTS_BIT;
    case ResourceState::ShaderRead:
      return VK_PIPELINE_STAGE_2_VERTEX_SHADER_BIT | VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT | VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT;
    case ResourceState::ShaderWrite:
      return VK_PIPELINE_STAGE_2_VERTEX_SHADER_BIT | VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT | VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT;
    case ResourceState::TransferSrc:
    case ResourceState::TransferDst:
      return VK_PIPELINE_STAGE_2_TRANSFER_BIT;
    case ResourceState::Present:
      return VK_PIPELINE_STAGE_2_BOTTOM_OF_PIPE_BIT;
    case ResourceState::Undefined:
      return VK_PIPELINE_STAGE_2_TOP_OF_PIPE_BIT;
    default:  // General
      return VK_PIPELINE_STAGE_2_VERTEX_SHADER_BIT | VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT | VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT;
  }
}

VkAttachmentLoadOp toVkLoadOp(LoadOp op)
{
  switch(op)
  {
    case LoadOp::clear:
      return VK_ATTACHMENT_LOAD_OP_CLEAR;
    case LoadOp::dontCare:
      return VK_ATTACHMENT_LOAD_OP_DONT_CARE;
    default:
      return VK_ATTACHMENT_LOAD_OP_LOAD;
  }
}

VkAttachmentStoreOp toVkStoreOp(StoreOp op)
{
  return op == StoreOp::dontCare ? VK_ATTACHMENT_STORE_OP_DONT_CARE : VK_ATTACHMENT_STORE_OP_STORE;
}

VkPipelineBindPoint toVkBindPoint(PipelineBindPoint bindPoint)
{
  return bindPoint == PipelineBindPoint::compute ? VK_PIPELINE_BIND_POINT_COMPUTE : VK_PIPELINE_BIND_POINT_GRAPHICS;
}

VkIndexType toVkIndexType(IndexFormat format)
{
  return format == IndexFormat::uint32 ? VK_INDEX_TYPE_UINT32 : VK_INDEX_TYPE_UINT16;
}

void ensureCommandBuffer(VkCommandBuffer commandBuffer)
{
  if(commandBuffer == VK_NULL_HANDLE)
  {
    throw std::runtime_error("VulkanCommandList requires a valid VkCommandBuffer");
  }
}

}  // namespace

void VulkanCommandList::begin()
{
  ensureCommandBuffer(m_commandBuffer);
}

void VulkanCommandList::end()
{
  ensureCommandBuffer(m_commandBuffer);
}

VkImageView VulkanCommandList::getVkImageViewFromHandle(TextureViewHandle view) const
{
  // TextureViewHandle::fromNativePtr() encodes the 64-bit pointer in index+generation
  // Use toNativePtr() to correctly reconstruct the full pointer value
  // VkImageView is a non-dispatchable handle, reinterpret from void*
  return reinterpret_cast<VkImageView>(view.toNativePtr());
}

void VulkanCommandList::beginRenderPass(const RenderPassDesc& desc)
{
  ensureCommandBuffer(m_commandBuffer);
  std::vector<VkRenderingAttachmentInfo> colorAttachments(desc.colorTargetCount);
  for(uint32_t i = 0; i < desc.colorTargetCount; ++i)
  {
    colorAttachments[i] = VkRenderingAttachmentInfo{
        .sType       = VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO,
        .imageView   = getVkImageViewFromHandle(desc.colorTargets[i].view),
        .imageLayout = toVkImageLayout(desc.colorTargets[i].state),
        .loadOp      = toVkLoadOp(desc.colorTargets[i].loadOp),
        .storeOp     = toVkStoreOp(desc.colorTargets[i].storeOp),
        .clearValue  = {{{desc.colorTargets[i].clearColor.r, desc.colorTargets[i].clearColor.g,
                          desc.colorTargets[i].clearColor.b, desc.colorTargets[i].clearColor.a}}},
    };
  }

  VkRenderingAttachmentInfo depthAttachment{};
  VkRenderingInfo           renderingInfo{
      .sType                = VK_STRUCTURE_TYPE_RENDERING_INFO,
      .renderArea           = {{desc.renderArea.offset.x, desc.renderArea.offset.y},
                               {desc.renderArea.extent.width, desc.renderArea.extent.height}},
      .layerCount           = 1,
      .colorAttachmentCount = desc.colorTargetCount,
      .pColorAttachments    = colorAttachments.data(),
  };

  if(desc.depthTarget != nullptr)
  {
    depthAttachment = VkRenderingAttachmentInfo{
        .sType       = VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO,
        .imageView   = getVkImageViewFromHandle(desc.depthTarget->view),
        .imageLayout = toVkImageLayout(desc.depthTarget->state),
        .loadOp      = toVkLoadOp(desc.depthTarget->loadOp),
        .storeOp     = toVkStoreOp(desc.depthTarget->storeOp),
        .clearValue  = {.depthStencil = {desc.depthTarget->clearValue.depth, desc.depthTarget->clearValue.stencil}},
    };
    renderingInfo.pDepthAttachment = &depthAttachment;
  }

  vkCmdBeginRendering(m_commandBuffer, &renderingInfo);
}

void VulkanCommandList::endRenderPass()
{
  ensureCommandBuffer(m_commandBuffer);
  vkCmdEndRendering(m_commandBuffer);
}

void VulkanCommandList::setViewport(const Viewport& viewport)
{
  ensureCommandBuffer(m_commandBuffer);
  const VkViewport vkViewport{viewport.x,      viewport.y,        viewport.width,
                              viewport.height, viewport.minDepth, viewport.maxDepth};
  vkCmdSetViewportWithCount(m_commandBuffer, 1, &vkViewport);
}

void VulkanCommandList::setScissor(const Rect2D& scissor)
{
  ensureCommandBuffer(m_commandBuffer);
  const VkRect2D vkScissor{{scissor.offset.x, scissor.offset.y}, {scissor.extent.width, scissor.extent.height}};
  vkCmdSetScissorWithCount(m_commandBuffer, 1, &vkScissor);
}

ResourceState VulkanCommandList::getTrackedState(ResourceHandle resource, ResourceState fallback) const
{
  for(const ResourceStateEntry& entry : m_resourceStates)
  {
    if(entry.handle == resource)
    {
      return entry.state;
    }
  }
  return fallback;
}

void VulkanCommandList::setResourceState(ResourceHandle resource, ResourceState state)
{
  for(ResourceStateEntry& entry : m_resourceStates)
  {
    if(entry.handle == resource)
    {
      entry.state = state;
      return;
    }
  }

  m_resourceStates.push_back(ResourceStateEntry{resource, state});
}

void VulkanCommandList::insertBarrier(BarrierType barrierType)
{
  ensureCommandBuffer(m_commandBuffer);

  VkMemoryBarrier2 memoryBarrier{
      .sType = VK_STRUCTURE_TYPE_MEMORY_BARRIER_2,
  };

  switch(barrierType)
  {
    case BarrierType::Execution:
      memoryBarrier.srcStageMask  = VK_PIPELINE_STAGE_2_ALL_GRAPHICS_BIT | VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT;
      memoryBarrier.srcAccessMask = VK_ACCESS_2_NONE;
      memoryBarrier.dstStageMask  = VK_PIPELINE_STAGE_2_ALL_GRAPHICS_BIT | VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT;
      memoryBarrier.dstAccessMask = VK_ACCESS_2_NONE;
      break;
    case BarrierType::LayoutTransition:
    case BarrierType::Memory:
    default:
      memoryBarrier.srcStageMask  = VK_PIPELINE_STAGE_2_ALL_GRAPHICS_BIT | VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT | VK_PIPELINE_STAGE_2_TRANSFER_BIT;
      memoryBarrier.srcAccessMask = VK_ACCESS_2_SHADER_READ_BIT | VK_ACCESS_2_SHADER_WRITE_BIT | VK_ACCESS_2_TRANSFER_READ_BIT | VK_ACCESS_2_TRANSFER_WRITE_BIT;
      memoryBarrier.dstStageMask  = VK_PIPELINE_STAGE_2_ALL_GRAPHICS_BIT | VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT | VK_PIPELINE_STAGE_2_TRANSFER_BIT;
      memoryBarrier.dstAccessMask = VK_ACCESS_2_SHADER_READ_BIT | VK_ACCESS_2_SHADER_WRITE_BIT | VK_ACCESS_2_TRANSFER_READ_BIT | VK_ACCESS_2_TRANSFER_WRITE_BIT;
      break;
  }

  const VkDependencyInfo dependencyInfo{
      .sType              = VK_STRUCTURE_TYPE_DEPENDENCY_INFO,
      .memoryBarrierCount = 1,
      .pMemoryBarriers    = &memoryBarrier,
  };
  vkCmdPipelineBarrier2(m_commandBuffer, &dependencyInfo);
}

void VulkanCommandList::memoryBarrier(PipelineStage  srcStage,
                                      ResourceAccess srcAccess,
                                      PipelineStage  dstStage,
                                      ResourceAccess dstAccess)
{
  ensureCommandBuffer(m_commandBuffer);
  const VkMemoryBarrier2 barrier{
      .sType         = VK_STRUCTURE_TYPE_MEMORY_BARRIER_2,
      .srcStageMask  = toVkStageMask(srcStage),
      .srcAccessMask = toVkAccessMask(srcAccess, srcStage),
      .dstStageMask  = toVkStageMask(dstStage),
      .dstAccessMask = toVkAccessMask(dstAccess, dstStage),
  };
  const VkDependencyInfo dependencyInfo{
      .sType              = VK_STRUCTURE_TYPE_DEPENDENCY_INFO,
      .memoryBarrierCount = 1,
      .pMemoryBarriers    = &barrier,
  };
  vkCmdPipelineBarrier2(m_commandBuffer, &dependencyInfo);
}

void VulkanCommandList::transitionBuffer(const BufferBarrierDesc& desc)
{
  ensureCommandBuffer(m_commandBuffer);
  const VkBufferMemoryBarrier2 barrier{
      .sType               = VK_STRUCTURE_TYPE_BUFFER_MEMORY_BARRIER_2,
      .srcStageMask        = toVkStageMask(desc.srcStage),
      .srcAccessMask       = toVkAccessMask(desc.srcAccess, desc.srcStage),
      .dstStageMask        = toVkStageMask(desc.dstStage),
      .dstAccessMask       = toVkAccessMask(desc.dstAccess, desc.dstStage),
      .srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
      .dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
      .buffer              = reinterpret_cast<VkBuffer>(desc.nativeBuffer),
      .offset              = 0,
      .size                = VK_WHOLE_SIZE,
  };
  const VkDependencyInfo dependencyInfo{
      .sType                    = VK_STRUCTURE_TYPE_DEPENDENCY_INFO,
      .bufferMemoryBarrierCount = 1,
      .pBufferMemoryBarriers    = &barrier,
  };
  vkCmdPipelineBarrier2(m_commandBuffer, &dependencyInfo);
}

void VulkanCommandList::transitionTexture(const TextureBarrierDesc& desc)
{
  ensureCommandBuffer(m_commandBuffer);

  const ResourceHandle trackedResource{ResourceKind::Texture, desc.texture.index, desc.texture.generation};
  // Check tracked state, but use desc.oldState as fallback
  const ResourceState trackedState = getTrackedState(trackedResource, desc.oldState);

  // Use the tracked state (or desc.oldState as fallback) directly
  const ResourceState resolvedOldState = trackedState;

  // Derive srcAccess from oldState for specific states, ensuring stage/access compatibility
  const VkAccessFlags2 srcAccess =
      resolvedOldState == ResourceState::Undefined ? VK_ACCESS_2_NONE : stateToDefaultAccess(resolvedOldState);

  // Derive dstAccess from newState for specific states, ensuring stage/access compatibility
  const VkAccessFlags2 dstAccess = stateToDefaultAccess(desc.newState);

  // Derive srcStage from oldState for specific states to ensure stage/access compatibility
  const VkPipelineStageFlags2 srcStage =
      (resolvedOldState == ResourceState::TransferSrc || resolvedOldState == ResourceState::TransferDst ||
       resolvedOldState == ResourceState::Present || resolvedOldState == ResourceState::ColorAttachment ||
       resolvedOldState == ResourceState::DepthStencilAttachment)
          ? stateToDefaultStage(resolvedOldState)
          : (desc.srcStage == PipelineStage::TopOfPipe ? stateToDefaultStage(resolvedOldState) : toVkStageMask(desc.srcStage));

  // Derive dstStage from newState for specific states to ensure stage/access compatibility
  const VkPipelineStageFlags2 dstStage =
      (desc.newState == ResourceState::TransferSrc || desc.newState == ResourceState::TransferDst ||
       desc.newState == ResourceState::Present || desc.newState == ResourceState::ColorAttachment ||
       desc.newState == ResourceState::DepthStencilAttachment)
          ? stateToDefaultStage(desc.newState)
          : (desc.dstStage == PipelineStage::BottomOfPipe ? stateToDefaultStage(desc.newState) : toVkStageMask(desc.dstStage));

  const VkImageMemoryBarrier2 barrier{
      .sType               = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER_2,
      .srcStageMask        = srcStage,
      .srcAccessMask       = srcAccess,
      .dstStageMask        = dstStage,
      .dstAccessMask       = dstAccess,
      .oldLayout           = toVkImageLayout(resolvedOldState),
      .newLayout           = toVkImageLayout(desc.newState),
      .srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
      .dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
      .image               = reinterpret_cast<VkImage>(desc.nativeImage),
      .subresourceRange    = {toVkAspectMask(desc.aspect), 0, VK_REMAINING_MIP_LEVELS, 0, VK_REMAINING_ARRAY_LAYERS},
  };
  const VkDependencyInfo dependencyInfo{
      .sType                   = VK_STRUCTURE_TYPE_DEPENDENCY_INFO,
      .imageMemoryBarrierCount = 1,
      .pImageMemoryBarriers    = &barrier,
  };
  vkCmdPipelineBarrier2(m_commandBuffer, &dependencyInfo);

  setResourceState(trackedResource, desc.newState);
}

void VulkanCommandList::bindPipeline(PipelineBindPoint bindPoint, PipelineHandle pipeline)
{
  ensureCommandBuffer(m_commandBuffer);
  if(m_resourceTable == nullptr || pipeline.isNull())
  {
    return;
  }

  const VkPipelineBindPoint vkBindPoint =
      bindPoint == PipelineBindPoint::compute ? VK_PIPELINE_BIND_POINT_COMPUTE : VK_PIPELINE_BIND_POINT_GRAPHICS;
  const VkPipeline nativePipeline =
      reinterpret_cast<VkPipeline>(m_resourceTable->resolvePipeline(pipeline, static_cast<uint32_t>(vkBindPoint)));
  if(nativePipeline == VK_NULL_HANDLE)
  {
    return;
  }
  vkCmdBindPipeline(m_commandBuffer, vkBindPoint, nativePipeline);

  // Remember the layout and bind point so subsequent bindBindGroup/pushConstants
  // calls can target the bound pipeline without the caller passing them explicitly.
  m_currentPipelineLayout = reinterpret_cast<VkPipelineLayout>(m_resourceTable->resolvePipelineLayout(pipeline));
  m_currentBindPoint      = vkBindPoint;
}

void VulkanCommandList::bindBindTable(PipelineBindPoint, uint32_t, BindTableHandle, const uint32_t*, uint32_t)
{
  ensureCommandBuffer(m_commandBuffer);
}

void VulkanCommandList::bindBindGroup(uint32_t        slot,
                                      BindGroupHandle bindGroup,
                                      const uint32_t* dynamicOffsets,
                                      uint32_t        dynamicOffsetCount)
{
  ensureCommandBuffer(m_commandBuffer);
  if(m_resourceTable == nullptr || bindGroup.isNull() || m_currentPipelineLayout == VK_NULL_HANDLE)
  {
    return;
  }

  const VkDescriptorSet descriptorSet =
      reinterpret_cast<VkDescriptorSet>(m_resourceTable->resolveBindGroupDescriptorSet(bindGroup));
  if(descriptorSet == VK_NULL_HANDLE)
  {
    return;
  }

  vkCmdBindDescriptorSets(m_commandBuffer,
                          m_currentBindPoint,
                          m_currentPipelineLayout,
                          slot,
                          1,
                          &descriptorSet,
                          dynamicOffsetCount,
                          dynamicOffsets);
}

void VulkanCommandList::bindVertexBuffers(uint32_t firstBinding, const uint64_t* bufferHandles,
                                          const uint64_t* offsets, uint32_t bufferCount)
{
  ensureCommandBuffer(m_commandBuffer);
  if(bufferCount == 0)
  {
    return;
  }

  if(bufferCount == 1)
  {
    const VkBuffer nativeBuffer = reinterpret_cast<VkBuffer>(static_cast<uintptr_t>(bufferHandles[0]));
    const VkDeviceSize nativeOffset = offsets != nullptr ? static_cast<VkDeviceSize>(offsets[0]) : 0;
    vkCmdBindVertexBuffers(m_commandBuffer, firstBinding, 1, &nativeBuffer, &nativeOffset);
    return;
  }

  // Convert opaque handles to VkBuffer (handles are just encoded pointers)
  std::vector<VkBuffer> nativeBuffers(bufferCount);
  for(uint32_t i = 0; i < bufferCount; ++i)
  {
    nativeBuffers[i] = reinterpret_cast<VkBuffer>(static_cast<uintptr_t>(bufferHandles[i]));
  }
  vkCmdBindVertexBuffers(m_commandBuffer, firstBinding, bufferCount, nativeBuffers.data(), offsets);
}

void VulkanCommandList::bindIndexBuffer(uint64_t bufferHandle, uint64_t offset, IndexFormat format)
{
  ensureCommandBuffer(m_commandBuffer);
  VkBuffer   nativeBuffer = reinterpret_cast<VkBuffer>(static_cast<uintptr_t>(bufferHandle));
  VkIndexType indexType   = toVkIndexType(format);
  vkCmdBindIndexBuffer(m_commandBuffer, nativeBuffer, offset, indexType);
}

VkShaderStageFlags toVkShaderStageFlags(ShaderStage stages)
{
  const uint32_t      mask = static_cast<uint32_t>(stages);
  VkShaderStageFlags  result{0};
  if((mask & static_cast<uint32_t>(ShaderStage::vertex)) != 0)
  {
    result |= VK_SHADER_STAGE_VERTEX_BIT;
  }
  if((mask & static_cast<uint32_t>(ShaderStage::fragment)) != 0)
  {
    result |= VK_SHADER_STAGE_FRAGMENT_BIT;
  }
  if((mask & static_cast<uint32_t>(ShaderStage::compute)) != 0)
  {
    result |= VK_SHADER_STAGE_COMPUTE_BIT;
  }
  if((mask & static_cast<uint32_t>(ShaderStage::geometry)) != 0)
  {
    result |= VK_SHADER_STAGE_GEOMETRY_BIT;
  }
  if((mask & static_cast<uint32_t>(ShaderStage::tessControl)) != 0)
  {
    result |= VK_SHADER_STAGE_TESSELLATION_CONTROL_BIT;
  }
  if((mask & static_cast<uint32_t>(ShaderStage::tessEval)) != 0)
  {
    result |= VK_SHADER_STAGE_TESSELLATION_EVALUATION_BIT;
  }
  return result;
}

void VulkanCommandList::pushConstants(ShaderStage stages, uint32_t offset, uint32_t size, const void* data)
{
  ensureCommandBuffer(m_commandBuffer);
  if(m_currentPipelineLayout == VK_NULL_HANDLE || size == 0 || data == nullptr)
  {
    return;
  }
  vkCmdPushConstants(m_commandBuffer, m_currentPipelineLayout, toVkShaderStageFlags(stages), offset, size, data);
}

void VulkanCommandList::draw(uint32_t vertexCount, uint32_t instanceCount, uint32_t firstVertex, uint32_t firstInstance)
{
  ensureCommandBuffer(m_commandBuffer);
  vkCmdDraw(m_commandBuffer, vertexCount, instanceCount, firstVertex, firstInstance);
}

void VulkanCommandList::drawIndexed(uint32_t indexCount, uint32_t instanceCount,
                                    uint32_t firstIndex, int32_t vertexOffset,
                                    uint32_t firstInstance)
{
  ensureCommandBuffer(m_commandBuffer);
  vkCmdDrawIndexed(m_commandBuffer, indexCount, instanceCount, firstIndex, vertexOffset, firstInstance);
}

void VulkanCommandList::drawIndexedIndirect(uint64_t bufferHandle, uint64_t offset, uint32_t drawCount, uint32_t stride)
{
  ensureCommandBuffer(m_commandBuffer);
  const VkBuffer nativeBuffer = reinterpret_cast<VkBuffer>(static_cast<uintptr_t>(bufferHandle));
  vkCmdDrawIndexedIndirect(m_commandBuffer, nativeBuffer, offset, drawCount, stride);
}

void VulkanCommandList::drawIndexedIndirectCount(uint64_t bufferHandle,
                                                 uint64_t offset,
                                                 uint64_t countBufferHandle,
                                                 uint64_t countBufferOffset,
                                                 uint32_t maxDrawCount,
                                                 uint32_t stride)
{
  ensureCommandBuffer(m_commandBuffer);
  const VkBuffer nativeBuffer = reinterpret_cast<VkBuffer>(static_cast<uintptr_t>(bufferHandle));
  const VkBuffer nativeCountBuffer = reinterpret_cast<VkBuffer>(static_cast<uintptr_t>(countBufferHandle));
  vkCmdDrawIndexedIndirectCount(m_commandBuffer,
                                nativeBuffer,
                                offset,
                                nativeCountBuffer,
                                countBufferOffset,
                                maxDrawCount,
                                stride);
}

void VulkanCommandList::dispatch(uint32_t groupCountX, uint32_t groupCountY, uint32_t groupCountZ)
{
  ensureCommandBuffer(m_commandBuffer);
  vkCmdDispatch(m_commandBuffer, groupCountX, groupCountY, groupCountZ);
}

void VulkanCommandList::copyBuffer(uint64_t srcBuffer, uint64_t dstBuffer, uint64_t srcOffset, uint64_t dstOffset, uint64_t size)
{
  ensureCommandBuffer(m_commandBuffer);
  const VkBufferCopy region{
      .srcOffset = static_cast<VkDeviceSize>(srcOffset),
      .dstOffset = static_cast<VkDeviceSize>(dstOffset),
      .size      = static_cast<VkDeviceSize>(size),
  };
  vkCmdCopyBuffer(m_commandBuffer,
                  reinterpret_cast<VkBuffer>(static_cast<uintptr_t>(srcBuffer)),
                  reinterpret_cast<VkBuffer>(static_cast<uintptr_t>(dstBuffer)),
                  1,
                  &region);
}

void VulkanCommandList::fillBuffer(uint64_t dstBuffer, uint64_t offset, uint64_t size, uint32_t data)
{
  ensureCommandBuffer(m_commandBuffer);
  vkCmdFillBuffer(m_commandBuffer,
                  reinterpret_cast<VkBuffer>(static_cast<uintptr_t>(dstBuffer)),
                  static_cast<VkDeviceSize>(offset),
                  static_cast<VkDeviceSize>(size),
                  data);
}

void VulkanCommandList::blitImage(const ImageBlitDesc& desc)
{
  ensureCommandBuffer(m_commandBuffer);
  const VkImageAspectFlags aspectMask = toVkAspectMask(desc.aspect);
  const VkImageBlit region{
      .srcSubresource = {.aspectMask = aspectMask, .mipLevel = 0, .baseArrayLayer = 0, .layerCount = 1},
      .srcOffsets     = {{desc.srcOffsets[0].x, desc.srcOffsets[0].y, desc.srcOffsets[0].z},
                         {desc.srcOffsets[1].x, desc.srcOffsets[1].y, desc.srcOffsets[1].z}},
      .dstSubresource = {.aspectMask = aspectMask, .mipLevel = 0, .baseArrayLayer = 0, .layerCount = 1},
      .dstOffsets     = {{desc.dstOffsets[0].x, desc.dstOffsets[0].y, desc.dstOffsets[0].z},
                         {desc.dstOffsets[1].x, desc.dstOffsets[1].y, desc.dstOffsets[1].z}},
  };
  vkCmdBlitImage(m_commandBuffer,
                 reinterpret_cast<VkImage>(static_cast<uintptr_t>(desc.srcImage)),
                 toVkImageLayout(desc.srcState),
                 reinterpret_cast<VkImage>(static_cast<uintptr_t>(desc.dstImage)),
                 toVkImageLayout(desc.dstState),
                 1,
                 &region,
                 VK_FILTER_LINEAR);
}

void VulkanCommandList::beginEvent(const char* name)
{
  ensureCommandBuffer(m_commandBuffer);
  const VkDebugUtilsLabelEXT labelInfo{
      .sType      = VK_STRUCTURE_TYPE_DEBUG_UTILS_LABEL_EXT,
      .pLabelName = name,
      .color      = {0.0F, 0.0F, 0.0F, 0.0F},
  };
  vkCmdBeginDebugUtilsLabelEXT(m_commandBuffer, &labelInfo);
}

void VulkanCommandList::endEvent()
{
  ensureCommandBuffer(m_commandBuffer);
  vkCmdEndDebugUtilsLabelEXT(m_commandBuffer);
}

VkCommandBuffer getNativeCommandBuffer(demo::rhi::CommandList& commandList)
{
  return static_cast<VulkanCommandList&>(commandList).nativeHandle();
}

VkCommandBuffer getNativeCommandBuffer(const demo::rhi::CommandList& commandList)
{
  return static_cast<const VulkanCommandList&>(commandList).nativeHandle();
}

void setResourceTable(demo::rhi::CommandList& commandList, VulkanResourceTable* table)
{
  static_cast<VulkanCommandList&>(commandList).setResourceTable(table);
}

void cmdPipelineBarrier(const demo::rhi::CommandList& commandList, const VkDependencyInfo& dependencyInfo)
{
  vkCmdPipelineBarrier2(getNativeCommandBuffer(commandList), &dependencyInfo);
}

void cmdBeginRendering(const demo::rhi::CommandList& commandList, const VkRenderingInfo& renderingInfo)
{
  vkCmdBeginRendering(getNativeCommandBuffer(commandList), &renderingInfo);
}

void cmdEndRendering(const demo::rhi::CommandList& commandList)
{
  vkCmdEndRendering(getNativeCommandBuffer(commandList));
}

void cmdSetViewport(const demo::rhi::CommandList& commandList, const VkViewport& viewport)
{
  vkCmdSetViewportWithCount(getNativeCommandBuffer(commandList), 1, &viewport);
}

void cmdSetScissor(const demo::rhi::CommandList& commandList, const VkRect2D& scissor)
{
  vkCmdSetScissorWithCount(getNativeCommandBuffer(commandList), 1, &scissor);
}

void cmdPushConstants(const demo::rhi::CommandList& commandList, const VkPushConstantsInfo& pushInfo)
{
  vkCmdPushConstants2(getNativeCommandBuffer(commandList), &pushInfo);
}

void cmdBindPipeline(const demo::rhi::CommandList& commandList, VkPipelineBindPoint bindPoint, VkPipeline pipeline)
{
  vkCmdBindPipeline(getNativeCommandBuffer(commandList), bindPoint, pipeline);
}

void cmdBindDescriptorSets(const demo::rhi::CommandList& commandList,
                           VkPipelineBindPoint           bindPoint,
                           VkPipelineLayout              layout,
                           uint32_t                      firstSet,
                           uint32_t                      descriptorSetCount,
                           const VkDescriptorSet*        descriptorSets,
                           uint32_t                      dynamicOffsetCount,
                           const uint32_t*               dynamicOffsets)
{
  vkCmdBindDescriptorSets(getNativeCommandBuffer(commandList), bindPoint, layout, firstSet, descriptorSetCount,
                          descriptorSets, dynamicOffsetCount, dynamicOffsets);
}

void cmdBindDescriptorSetOpaque(const demo::rhi::CommandList& commandList,
                                uint32_t                      bindPoint,
                                uint64_t                      layoutHandle,
                                uint32_t                      firstSet,
                                uint64_t                      descriptorSetHandle,
                                uint32_t                      dynamicOffsetCount,
                                const uint32_t*               dynamicOffsets)
{
  const auto nativeBindPoint = static_cast<VkPipelineBindPoint>(bindPoint);
  const auto nativeLayout    = reinterpret_cast<VkPipelineLayout>(static_cast<uintptr_t>(layoutHandle));
  const auto nativeSet       = reinterpret_cast<VkDescriptorSet>(static_cast<uintptr_t>(descriptorSetHandle));
  vkCmdBindDescriptorSets(getNativeCommandBuffer(commandList), nativeBindPoint, nativeLayout, firstSet, 1, &nativeSet,
                          dynamicOffsetCount, dynamicOffsets);
}

void cmdBindVertexBuffers(const demo::rhi::CommandList& commandList,
                          uint32_t                      firstBinding,
                          uint32_t                      bindingCount,
                          const VkBuffer*               buffers,
                          const VkDeviceSize*           offsets)
{
  vkCmdBindVertexBuffers(getNativeCommandBuffer(commandList), firstBinding, bindingCount, buffers, offsets);
}

void cmdDraw(const demo::rhi::CommandList& commandList, uint32_t vertexCount, uint32_t instanceCount, uint32_t firstVertex, uint32_t firstInstance)
{
  vkCmdDraw(getNativeCommandBuffer(commandList), vertexCount, instanceCount, firstVertex, firstInstance);
}

void cmdBindIndexBuffer(const demo::rhi::CommandList& commandList, VkBuffer buffer, VkDeviceSize offset, VkIndexType indexType)
{
  vkCmdBindIndexBuffer(getNativeCommandBuffer(commandList), buffer, offset, indexType);
}

void cmdDrawIndexed(const demo::rhi::CommandList& commandList, uint32_t indexCount, uint32_t instanceCount, uint32_t firstIndex, int32_t vertexOffset, uint32_t firstInstance)
{
  vkCmdDrawIndexed(getNativeCommandBuffer(commandList), indexCount, instanceCount, firstIndex, vertexOffset, firstInstance);
}

void cmdDrawIndexedIndirect(const demo::rhi::CommandList& commandList, VkBuffer buffer, VkDeviceSize offset, uint32_t drawCount, uint32_t stride)
{
  vkCmdDrawIndexedIndirect(getNativeCommandBuffer(commandList), buffer, offset, drawCount, stride);
}

void cmdDrawIndexedIndirectCount(const demo::rhi::CommandList& commandList,
                                 VkBuffer                       buffer,
                                 VkDeviceSize                   offset,
                                 VkBuffer                       countBuffer,
                                 VkDeviceSize                   countBufferOffset,
                                 uint32_t                       maxDrawCount,
                                 uint32_t                       stride)
{
  vkCmdDrawIndexedIndirectCount(getNativeCommandBuffer(commandList),
                                buffer,
                                offset,
                                countBuffer,
                                countBufferOffset,
                                maxDrawCount,
                                stride);
}

void cmdDispatch(const demo::rhi::CommandList& commandList, uint32_t groupCountX, uint32_t groupCountY, uint32_t groupCountZ)
{
  vkCmdDispatch(getNativeCommandBuffer(commandList), groupCountX, groupCountY, groupCountZ);
}

}  // namespace demo::rhi::vulkan
