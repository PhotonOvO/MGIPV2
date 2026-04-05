#include "VulkanCommandList.h"

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
  return result == 0 ? VK_PIPELINE_STAGE_2_TOP_OF_PIPE_BIT : result;
}

VkAccessFlags2 toVkAccessMask(ResourceAccess access)
{
  switch(access)
  {
    case ResourceAccess::read:
      return VK_ACCESS_2_MEMORY_READ_BIT;
    case ResourceAccess::write:
      return VK_ACCESS_2_MEMORY_WRITE_BIT;
    case ResourceAccess::readWrite:
      return VK_ACCESS_2_MEMORY_READ_BIT | VK_ACCESS_2_MEMORY_WRITE_BIT;
    default:
      return VK_ACCESS_2_NONE;
  }
}

VkImageAspectFlags toVkAspectMask(TextureAspect aspect)
{
  return aspect == TextureAspect::depth ? VK_IMAGE_ASPECT_DEPTH_BIT : VK_IMAGE_ASPECT_COLOR_BIT;
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
    case ResourceState::TransferSrc:
    case ResourceState::Present:
      return VK_ACCESS_2_MEMORY_READ_BIT;
    case ResourceState::Undefined:
      return VK_ACCESS_2_NONE;
    default:
      return VK_ACCESS_2_MEMORY_READ_BIT | VK_ACCESS_2_MEMORY_WRITE_BIT;
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
    case ResourceState::ShaderWrite:
      return VK_PIPELINE_STAGE_2_VERTEX_SHADER_BIT | VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT | VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT;
    case ResourceState::TransferSrc:
    case ResourceState::TransferDst:
      return VK_PIPELINE_STAGE_2_TRANSFER_BIT;
    case ResourceState::Present:
      return VK_PIPELINE_STAGE_2_BOTTOM_OF_PIPE_BIT;
    default:
      return VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT;
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

void VulkanCommandList::beginRenderPass(const RenderPassDesc& desc)
{
  ensureCommandBuffer(m_commandBuffer);
  std::vector<VkRenderingAttachmentInfo> colorAttachments(desc.colorTargetCount);
  for(uint32_t i = 0; i < desc.colorTargetCount; ++i)
  {
    colorAttachments[i] = VkRenderingAttachmentInfo{
        .sType       = VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO,
        .imageView   = VK_NULL_HANDLE,
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
        .imageView   = VK_NULL_HANDLE,
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
      memoryBarrier.srcStageMask  = VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT;
      memoryBarrier.srcAccessMask = VK_ACCESS_2_NONE;
      memoryBarrier.dstStageMask  = VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT;
      memoryBarrier.dstAccessMask = VK_ACCESS_2_NONE;
      break;
    case BarrierType::LayoutTransition:
    case BarrierType::Memory:
    default:
      memoryBarrier.srcStageMask  = VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT;
      memoryBarrier.srcAccessMask = VK_ACCESS_2_MEMORY_READ_BIT | VK_ACCESS_2_MEMORY_WRITE_BIT;
      memoryBarrier.dstStageMask  = VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT;
      memoryBarrier.dstAccessMask = VK_ACCESS_2_MEMORY_READ_BIT | VK_ACCESS_2_MEMORY_WRITE_BIT;
      break;
  }

  const VkDependencyInfo dependencyInfo{
      .sType              = VK_STRUCTURE_TYPE_DEPENDENCY_INFO,
      .memoryBarrierCount = 1,
      .pMemoryBarriers    = &memoryBarrier,
  };
  vkCmdPipelineBarrier2(m_commandBuffer, &dependencyInfo);
}

void VulkanCommandList::transitionBuffer(const BufferBarrierDesc& desc)
{
  ensureCommandBuffer(m_commandBuffer);
  const VkBufferMemoryBarrier2 barrier{
      .sType               = VK_STRUCTURE_TYPE_BUFFER_MEMORY_BARRIER_2,
      .srcStageMask        = toVkStageMask(desc.srcStage),
      .srcAccessMask       = toVkAccessMask(desc.srcAccess),
      .dstStageMask        = toVkStageMask(desc.dstStage),
      .dstAccessMask       = toVkAccessMask(desc.dstAccess),
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
  const ResourceState  resolvedOldState =
      desc.oldState == ResourceState::Undefined ? getTrackedState(trackedResource, desc.oldState) : desc.oldState;

  const VkAccessFlags2 srcAccess =
      resolvedOldState == ResourceState::Undefined ?
          VK_ACCESS_2_NONE :
          (desc.srcAccess == ResourceAccess::read ? stateToDefaultAccess(resolvedOldState) : toVkAccessMask(desc.srcAccess));
  const VkAccessFlags2 dstAccess =
      desc.dstAccess == ResourceAccess::read ? stateToDefaultAccess(desc.newState) : toVkAccessMask(desc.dstAccess);
  const VkPipelineStageFlags2 srcStage =
      desc.srcStage == PipelineStage::TopOfPipe ? stateToDefaultStage(resolvedOldState) : toVkStageMask(desc.srcStage);
  const VkPipelineStageFlags2 dstStage =
      desc.dstStage == PipelineStage::BottomOfPipe ? stateToDefaultStage(desc.newState) : toVkStageMask(desc.dstStage);

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

void VulkanCommandList::bindPipeline(PipelineBindPoint, PipelineHandle)
{
  ensureCommandBuffer(m_commandBuffer);
}

void VulkanCommandList::bindBindTable(PipelineBindPoint, uint32_t, BindTableHandle, const uint32_t*, uint32_t)
{
  ensureCommandBuffer(m_commandBuffer);
}

void VulkanCommandList::bindVertexBuffers(uint32_t, const BufferHandle*, const uint64_t*, uint32_t)
{
  ensureCommandBuffer(m_commandBuffer);
}

void VulkanCommandList::pushConstants(ShaderStage, uint32_t, uint32_t, const void*)
{
  ensureCommandBuffer(m_commandBuffer);
}

void VulkanCommandList::draw(uint32_t vertexCount, uint32_t instanceCount, uint32_t firstVertex, uint32_t firstInstance)
{
  ensureCommandBuffer(m_commandBuffer);
  vkCmdDraw(m_commandBuffer, vertexCount, instanceCount, firstVertex, firstInstance);
}

void VulkanCommandList::dispatch(uint32_t groupCountX, uint32_t groupCountY, uint32_t groupCountZ)
{
  ensureCommandBuffer(m_commandBuffer);
  vkCmdDispatch(m_commandBuffer, groupCountX, groupCountY, groupCountZ);
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

void cmdDispatch(const demo::rhi::CommandList& commandList, uint32_t groupCountX, uint32_t groupCountY, uint32_t groupCountZ)
{
  vkCmdDispatch(getNativeCommandBuffer(commandList), groupCountX, groupCountY, groupCountZ);
}

}  // namespace demo::rhi::vulkan
