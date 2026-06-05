#include "VulkanCommandBuffer.h"

#include "../../common/Common.h"
#include "VulkanResourceTable.h"

#include <array>
#include <cassert>
#include <vector>
#include <vulkan/vulkan.h>

namespace demo::rhi::vulkan {
namespace {

[[nodiscard]] VkPipelineStageFlags2 toVkPipelineStage2(StageFlags stages)
{
  VkPipelineStageFlags2 out = VK_PIPELINE_STAGE_2_NONE;
  const auto has = [&](StageFlags bit) { return (static_cast<uint64_t>(stages) & static_cast<uint64_t>(bit)) != 0; };
  if(stages == StageFlags::all) return VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT;
  if(has(StageFlags::transfer))       out |= VK_PIPELINE_STAGE_2_ALL_TRANSFER_BIT;
  if(has(StageFlags::compute))        out |= VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT;
  if(has(StageFlags::vertexShader))   out |= VK_PIPELINE_STAGE_2_VERTEX_SHADER_BIT;
  if(has(StageFlags::fragmentShader)) out |= VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT;
  if(has(StageFlags::rasterColorOut)) out |= VK_PIPELINE_STAGE_2_COLOR_ATTACHMENT_OUTPUT_BIT;
  if(has(StageFlags::rasterDepthOut)) out |= VK_PIPELINE_STAGE_2_EARLY_FRAGMENT_TESTS_BIT | VK_PIPELINE_STAGE_2_LATE_FRAGMENT_TESTS_BIT;
  if(has(StageFlags::commandInput))   out |= VK_PIPELINE_STAGE_2_DRAW_INDIRECT_BIT;
  return out == VK_PIPELINE_STAGE_2_NONE ? VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT : out;
}

// Conservative access masks; Wave 7 refines per-hazard. Correctness-first.
[[nodiscard]] VkAccessFlags2 inferProducerAccess(HazardFlags hazards)
{
  VkAccessFlags2 out = VK_ACCESS_2_MEMORY_WRITE_BIT;
  if((static_cast<uint32_t>(hazards) & static_cast<uint32_t>(HazardFlags::depthStencil)) != 0)
    out |= VK_ACCESS_2_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT;
  return out;
}

[[nodiscard]] VkAccessFlags2 inferConsumerAccess(HazardFlags hazards)
{
  VkAccessFlags2 out = VK_ACCESS_2_MEMORY_READ_BIT;
  if((static_cast<uint32_t>(hazards) & static_cast<uint32_t>(HazardFlags::drawArguments)) != 0)
    out |= VK_ACCESS_2_INDIRECT_COMMAND_READ_BIT;
  return out;
}

[[nodiscard]] VkImageLayout toVkImageLayout(ResourceState state)
{
  switch(state)
  {
    case ResourceState::ColorAttachment:        return VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
    case ResourceState::DepthStencilAttachment: return VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL;
    case ResourceState::ShaderRead:             return VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
    case ResourceState::ShaderWrite:            return VK_IMAGE_LAYOUT_GENERAL;
    case ResourceState::TransferSrc:            return VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL;
    case ResourceState::TransferDst:            return VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
    case ResourceState::Present:                return VK_IMAGE_LAYOUT_PRESENT_SRC_KHR;
    case ResourceState::General:                return VK_IMAGE_LAYOUT_GENERAL;
    default:                                    return VK_IMAGE_LAYOUT_UNDEFINED;
  }
}

[[nodiscard]] VkShaderStageFlags toVkShaderStageFlags(ShaderStage stages)
{
  VkShaderStageFlags flags = 0;
  const auto         has   = [&](ShaderStage bit) { return (static_cast<uint32_t>(stages) & static_cast<uint32_t>(bit)) != 0; };
  if(has(ShaderStage::vertex))      flags |= VK_SHADER_STAGE_VERTEX_BIT;
  if(has(ShaderStage::fragment))    flags |= VK_SHADER_STAGE_FRAGMENT_BIT;
  if(has(ShaderStage::compute))     flags |= VK_SHADER_STAGE_COMPUTE_BIT;
  if(has(ShaderStage::geometry))    flags |= VK_SHADER_STAGE_GEOMETRY_BIT;
  if(has(ShaderStage::tessControl)) flags |= VK_SHADER_STAGE_TESSELLATION_CONTROL_BIT;
  if(has(ShaderStage::tessEval))    flags |= VK_SHADER_STAGE_TESSELLATION_EVALUATION_BIT;
  return flags == 0 ? VK_SHADER_STAGE_ALL : flags;
}

[[nodiscard]] VkBuffer asBuffer(uint64_t v) { return reinterpret_cast<VkBuffer>(static_cast<uintptr_t>(v)); }
[[nodiscard]] VkImage  asImage(uint64_t v) { return reinterpret_cast<VkImage>(static_cast<uintptr_t>(v)); }

}  // namespace

// ---------------------------------------------------------------------------
// VulkanRenderEncoder
// ---------------------------------------------------------------------------
void VulkanRenderEncoder::prepare(VkCommandBuffer cmd, VulkanResourceTable* table)
{
  m_cmd    = cmd;
  m_table  = table;
  m_layout = nullptr;
  for(uint32_t i = 0; i < kMaxArgumentSlots; ++i) m_pendingDynCount[i] = 0;
}

void VulkanRenderEncoder::setPipeline(PipelineHandle pipeline)
{
  m_layout = reinterpret_cast<VkPipelineLayout>(static_cast<uintptr_t>(m_table->resolvePipelineLayout(pipeline)));
  vkCmdBindPipeline(m_cmd, VK_PIPELINE_BIND_POINT_GRAPHICS,
                    reinterpret_cast<VkPipeline>(static_cast<uintptr_t>(m_table->resolvePipeline(pipeline, 0))));
}

void VulkanRenderEncoder::setArgumentTable(ShaderStage /*stages*/, uint32_t slot, ArgumentTableHandle table)
{
  // Vulkan binds descriptor sets pipeline-wide; `stages` is for Metal4 per-stage tables.
  VkDescriptorSet set      = reinterpret_cast<VkDescriptorSet>(static_cast<uintptr_t>(m_table->resolveArgumentTable(table)));
  const uint32_t  dynCount = slot < kMaxArgumentSlots ? m_pendingDynCount[slot] : 0;
  const uint32_t* dynOffsets = dynCount > 0 ? m_pendingDynOffsets[slot] : nullptr;
  vkCmdBindDescriptorSets(m_cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, m_layout, slot, 1, &set, dynCount, dynOffsets);
  if(slot < kMaxArgumentSlots) m_pendingDynCount[slot] = 0;
}

void VulkanRenderEncoder::setDynamicBuffer(ShaderStage, uint32_t slot, BufferHandle, uint64_t offset, uint64_t)
{
  // Dynamic offsets accumulate per slot in call order, matching the descriptor set's
  // binding order; flushed together at the next setArgumentTable(slot).
  if(slot < kMaxArgumentSlots && m_pendingDynCount[slot] < kMaxDynOffsetPerSlot)
  {
    m_pendingDynOffsets[slot][m_pendingDynCount[slot]++] = static_cast<uint32_t>(offset);
  }
}

void VulkanRenderEncoder::setRootConstants(ShaderStage stage, uint32_t slot, const void* data, uint32_t size)
{
  vkCmdPushConstants(m_cmd, m_layout, toVkShaderStageFlags(stage), slot, size, data);
}

void VulkanRenderEncoder::setRootPointer(ShaderStage stage, uint32_t slot, GpuPtr ptr)
{
  const uint64_t address = ptr.value;
  vkCmdPushConstants(m_cmd, m_layout, toVkShaderStageFlags(stage), slot, sizeof(address), &address);
}

void VulkanRenderEncoder::setViewport(const Viewport& viewport)
{
  const VkViewport vp{viewport.x, viewport.y, viewport.width, viewport.height, viewport.minDepth, viewport.maxDepth};
  vkCmdSetViewportWithCount(m_cmd, 1, &vp);
}

void VulkanRenderEncoder::setScissor(const Rect2D& scissor)
{
  const VkRect2D rect{{scissor.offset.x, scissor.offset.y}, {scissor.extent.width, scissor.extent.height}};
  vkCmdSetScissorWithCount(m_cmd, 1, &rect);
}

void VulkanRenderEncoder::bindVertexBuffers(uint32_t firstBinding, const BufferHandle* buffers, const uint64_t* offsets, uint32_t count)
{
  std::array<VkBuffer, 16>     vkBuffers{};
  std::array<VkDeviceSize, 16> vkOffsets{};
  const uint32_t               clamped = count < 16 ? count : 16;
  for(uint32_t i = 0; i < clamped; ++i)
  {
    vkBuffers[i] = asBuffer(m_table->resolveBuffer(buffers[i]));
    vkOffsets[i] = offsets != nullptr ? offsets[i] : 0;
  }
  vkCmdBindVertexBuffers(m_cmd, firstBinding, clamped, vkBuffers.data(), vkOffsets.data());
}

void VulkanRenderEncoder::bindIndexBuffer(BufferHandle buffer, uint64_t offset, IndexFormat format)
{
  vkCmdBindIndexBuffer(m_cmd, asBuffer(m_table->resolveBuffer(buffer)), offset,
                       format == IndexFormat::uint16 ? VK_INDEX_TYPE_UINT16 : VK_INDEX_TYPE_UINT32);
}

void VulkanRenderEncoder::readInputAttachment(uint32_t)
{
  // Fallback path: input attachments are read via a sampled image binding today.
  // Real local-read maps to VK_KHR_dynamic_rendering_local_read in a later milestone.
}

void VulkanRenderEncoder::draw(const DrawDesc& desc)
{
  vkCmdDraw(m_cmd, desc.vertexCount, desc.instanceCount, desc.firstVertex, desc.firstInstance);
}

void VulkanRenderEncoder::drawIndexed(const DrawIndexedDesc& desc)
{
  if(!desc.indexBuffer.isNull())
  {
    bindIndexBuffer(desc.indexBuffer, desc.indexBufferOffset, desc.indexFormat);
  }
  vkCmdDrawIndexed(m_cmd, desc.indexCount, desc.instanceCount, desc.firstIndex, desc.vertexOffset, desc.firstInstance);
}

void VulkanRenderEncoder::drawIndexedIndirect(const DrawIndirectDesc& desc)
{
  vkCmdDrawIndexedIndirect(m_cmd, asBuffer(m_table->resolveBuffer(desc.argsBuffer)), desc.offset, desc.drawCount,
                           desc.stride);
}

void VulkanRenderEncoder::drawIndexedIndirect(GpuPtr, uint32_t, uint32_t)
{
  assert(false && "GpuPtr indirect draw is not supported on Vulkan core; use the BufferHandle overload");
}

void VulkanRenderEncoder::drawIndexedIndirectCount(const DrawIndirectCountDesc& desc)
{
  vkCmdDrawIndexedIndirectCount(m_cmd, asBuffer(m_table->resolveBuffer(desc.argsBuffer)), desc.argsOffset,
                                asBuffer(m_table->resolveBuffer(desc.countBuffer)), desc.countBufferOffset,
                                desc.maxDrawCount, desc.stride);
}

void VulkanRenderEncoder::drawIndirect(const DrawIndirectDesc& desc)
{
  vkCmdDrawIndirect(m_cmd, asBuffer(m_table->resolveBuffer(desc.argsBuffer)), desc.offset, desc.drawCount, desc.stride);
}

void VulkanRenderEncoder::drawMeshTasks(uint32_t groupCountX, uint32_t groupCountY, uint32_t groupCountZ)
{
  if(vkCmdDrawMeshTasksEXT != nullptr)
  {
    vkCmdDrawMeshTasksEXT(m_cmd, groupCountX, groupCountY, groupCountZ);
  }
}

void VulkanRenderEncoder::drawMeshTasksIndirect(const DrawIndirectDesc& desc)
{
  if(vkCmdDrawMeshTasksIndirectEXT != nullptr)
  {
    vkCmdDrawMeshTasksIndirectEXT(m_cmd, asBuffer(m_table->resolveBuffer(desc.argsBuffer)), desc.offset, desc.drawCount,
                                  desc.stride);
  }
}

// ---------------------------------------------------------------------------
// VulkanComputeEncoder
// ---------------------------------------------------------------------------
void VulkanComputeEncoder::prepare(VkCommandBuffer cmd, VulkanResourceTable* table)
{
  m_cmd    = cmd;
  m_table  = table;
  m_layout = nullptr;
}

void VulkanComputeEncoder::setPipeline(PipelineHandle pipeline)
{
  m_layout = reinterpret_cast<VkPipelineLayout>(static_cast<uintptr_t>(m_table->resolvePipelineLayout(pipeline)));
  vkCmdBindPipeline(m_cmd, VK_PIPELINE_BIND_POINT_COMPUTE,
                    reinterpret_cast<VkPipeline>(static_cast<uintptr_t>(m_table->resolvePipeline(pipeline, 1))));
}

void VulkanComputeEncoder::setArgumentTable(uint32_t slot, ArgumentTableHandle table)
{
  VkDescriptorSet set = reinterpret_cast<VkDescriptorSet>(static_cast<uintptr_t>(m_table->resolveArgumentTable(table)));
  vkCmdBindDescriptorSets(m_cmd, VK_PIPELINE_BIND_POINT_COMPUTE, m_layout, slot, 1, &set, 0, nullptr);
}

void VulkanComputeEncoder::setRootConstants(uint32_t slot, const void* data, uint32_t size)
{
  vkCmdPushConstants(m_cmd, m_layout, VK_SHADER_STAGE_COMPUTE_BIT, slot, size, data);
}

void VulkanComputeEncoder::setRootPointer(uint32_t slot, GpuPtr ptr)
{
  const uint64_t address = ptr.value;
  vkCmdPushConstants(m_cmd, m_layout, VK_SHADER_STAGE_COMPUTE_BIT, slot, sizeof(address), &address);
}

void VulkanComputeEncoder::dispatch(const DispatchDesc& desc)
{
  vkCmdDispatch(m_cmd, desc.groupCountX, desc.groupCountY, desc.groupCountZ);
}

void VulkanComputeEncoder::dispatchIndirect(const DispatchIndirectDesc& desc)
{
  vkCmdDispatchIndirect(m_cmd, asBuffer(m_table->resolveBuffer(desc.argsBuffer)), desc.offset);
}

void VulkanComputeEncoder::dispatchIndirect(GpuPtr)
{
  assert(false && "GpuPtr indirect dispatch is not supported on Vulkan core; use the BufferHandle overload");
}

// ---------------------------------------------------------------------------
// VulkanComputeEncoder: copy / blit command subset (Metal 4-aligned)
// ---------------------------------------------------------------------------
void VulkanComputeEncoder::copyBuffer(BufferHandle src, uint64_t srcOffset, BufferHandle dst, uint64_t dstOffset, uint64_t size)
{
  const VkBufferCopy region{srcOffset, dstOffset, size};
  vkCmdCopyBuffer(m_cmd, asBuffer(m_table->resolveBuffer(src)), asBuffer(m_table->resolveBuffer(dst)), 1, &region);
}

void VulkanComputeEncoder::copyBufferToTexture(const BufferTextureCopyDesc& desc)
{
  const VkBufferImageCopy region{
      .bufferOffset      = desc.bufferOffset,
      .imageSubresource  = {.aspectMask     = VK_IMAGE_ASPECT_COLOR_BIT,
                            .mipLevel       = desc.mipLevel,
                            .baseArrayLayer = desc.baseArrayLayer,
                            .layerCount     = desc.layerCount},
      .imageExtent       = {desc.width, desc.height, desc.depth},
  };
  vkCmdCopyBufferToImage(m_cmd, asBuffer(m_table->resolveBuffer(desc.buffer)), asImage(m_table->resolveTexture(desc.texture)),
                         VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &region);
}

void VulkanComputeEncoder::copyTextureToBuffer(const BufferTextureCopyDesc& desc)
{
  const VkBufferImageCopy region{
      .bufferOffset      = desc.bufferOffset,
      .imageSubresource  = {.aspectMask     = VK_IMAGE_ASPECT_COLOR_BIT,
                            .mipLevel       = desc.mipLevel,
                            .baseArrayLayer = desc.baseArrayLayer,
                            .layerCount     = desc.layerCount},
      .imageExtent       = {desc.width, desc.height, desc.depth},
  };
  vkCmdCopyImageToBuffer(m_cmd, asImage(m_table->resolveTexture(desc.texture)), VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
                         asBuffer(m_table->resolveBuffer(desc.buffer)), 1, &region);
}

void VulkanComputeEncoder::blitTexture(const TextureBlitDesc& desc)
{
  const VkImageSubresourceLayers sub{.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT, .layerCount = 1};
  VkImageBlit                    region{.srcSubresource = sub, .dstSubresource = sub};
  region.srcOffsets[0] = {desc.srcOffsets[0].x, desc.srcOffsets[0].y, desc.srcOffsets[0].z};
  region.srcOffsets[1] = {desc.srcOffsets[1].x, desc.srcOffsets[1].y, desc.srcOffsets[1].z};
  region.dstOffsets[0] = {desc.dstOffsets[0].x, desc.dstOffsets[0].y, desc.dstOffsets[0].z};
  region.dstOffsets[1] = {desc.dstOffsets[1].x, desc.dstOffsets[1].y, desc.dstOffsets[1].z};
  vkCmdBlitImage(m_cmd, asImage(m_table->resolveTexture(desc.srcTexture)), VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
                 asImage(m_table->resolveTexture(desc.dstTexture)), VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &region,
                 VK_FILTER_LINEAR);
}

void VulkanComputeEncoder::fillBuffer(BufferHandle buffer, uint64_t offset, uint64_t size, uint32_t data)
{
  vkCmdFillBuffer(m_cmd, asBuffer(m_table->resolveBuffer(buffer)), offset, size == 0 ? VK_WHOLE_SIZE : size, data);
}

// ---------------------------------------------------------------------------
// VulkanCommandBuffer
// ---------------------------------------------------------------------------
void VulkanCommandBuffer::setTarget(VkCommandBuffer cmd, VulkanResourceTable* table)
{
  m_cmd    = cmd;
  m_table  = table;
  m_active = EncoderKind::none;
}

RenderEncoder* VulkanCommandBuffer::beginRenderPass(const RenderPassDesc& desc)
{
  std::vector<VkRenderingAttachmentInfo> colorAttachments(desc.colorTargetCount);
  for(uint32_t i = 0; i < desc.colorTargetCount; ++i)
  {
    const RenderTargetDesc& target = desc.colorTargets[i];
    colorAttachments[i]            = VkRenderingAttachmentInfo{
                   .sType       = VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO,
                   .imageView   = reinterpret_cast<VkImageView>(static_cast<uintptr_t>(m_table->resolveTextureView(target.view))),
                   .imageLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL,
                   .loadOp      = static_cast<VkAttachmentLoadOp>(target.loadOp),
                   .storeOp     = static_cast<VkAttachmentStoreOp>(target.storeOp),
    };
    colorAttachments[i].clearValue.color = {{target.clearColor.r, target.clearColor.g, target.clearColor.b, target.clearColor.a}};
  }

  VkRenderingAttachmentInfo depthAttachment{};
  if(desc.depthTarget != nullptr)
  {
    depthAttachment = VkRenderingAttachmentInfo{
        .sType       = VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO,
        .imageView   = reinterpret_cast<VkImageView>(static_cast<uintptr_t>(m_table->resolveTextureView(desc.depthTarget->view))),
        .imageLayout = VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL,
        .loadOp      = static_cast<VkAttachmentLoadOp>(desc.depthTarget->loadOp),
        .storeOp     = static_cast<VkAttachmentStoreOp>(desc.depthTarget->storeOp),
    };
    depthAttachment.clearValue.depthStencil = {desc.depthTarget->clearValue.depth, desc.depthTarget->clearValue.stencil};
  }

  const VkRenderingInfo renderingInfo{
      .sType                = VK_STRUCTURE_TYPE_RENDERING_INFO,
      .renderArea           = {{desc.renderArea.offset.x, desc.renderArea.offset.y},
                               {desc.renderArea.extent.width, desc.renderArea.extent.height}},
      .layerCount           = 1,
      .colorAttachmentCount = desc.colorTargetCount,
      .pColorAttachments    = colorAttachments.empty() ? nullptr : colorAttachments.data(),
      .pDepthAttachment     = desc.depthTarget != nullptr ? &depthAttachment : nullptr,
  };
  vkCmdBeginRendering(m_cmd, &renderingInfo);

  m_renderEncoder.prepare(m_cmd, m_table);
  m_active = EncoderKind::render;
  return &m_renderEncoder;
}

ComputeEncoder* VulkanCommandBuffer::beginComputePass()
{
  m_computeEncoder.prepare(m_cmd, m_table);
  m_active = EncoderKind::compute;
  return &m_computeEncoder;
}

void VulkanCommandBuffer::endEncoding()
{
  if(m_active == EncoderKind::render)
  {
    vkCmdEndRendering(m_cmd);
  }
  m_active = EncoderKind::none;
}

void VulkanCommandBuffer::barrier(StageFlags producer, StageFlags consumer, HazardFlags hazards)
{
  VkPipelineStageFlags2 dstStage = toVkPipelineStage2(consumer);
  // INDIRECT_COMMAND_READ access is only valid alongside the DRAW_INDIRECT stage, so
  // when the consumer reads indirect arguments, ensure that stage is present even if
  // the declarative consumer mask only named shader stages.
  if((static_cast<uint32_t>(hazards) & static_cast<uint32_t>(HazardFlags::drawArguments)) != 0)
  {
    dstStage |= VK_PIPELINE_STAGE_2_DRAW_INDIRECT_BIT;
  }
  const VkMemoryBarrier2 memoryBarrier{
      .sType         = VK_STRUCTURE_TYPE_MEMORY_BARRIER_2,
      .srcStageMask  = toVkPipelineStage2(producer),
      .srcAccessMask = inferProducerAccess(hazards),
      .dstStageMask  = dstStage,
      .dstAccessMask = inferConsumerAccess(hazards),
  };
  const VkDependencyInfo dependencyInfo{
      .sType              = VK_STRUCTURE_TYPE_DEPENDENCY_INFO,
      .memoryBarrierCount = 1,
      .pMemoryBarriers    = &memoryBarrier,
  };
  vkCmdPipelineBarrier2(m_cmd, &dependencyInfo);
}

void VulkanCommandBuffer::resourceBarrier(const TextureBarrier* textures, uint32_t textureCount,
                                          const BufferBarrier* buffers, uint32_t bufferCount)
{
  std::vector<VkImageMemoryBarrier2>  imageBarriers(textureCount);
  std::vector<VkBufferMemoryBarrier2> bufferBarriers(bufferCount);
  for(uint32_t i = 0; i < textureCount; ++i)
  {
    const TextureBarrier& b = textures[i];
    imageBarriers[i]        = VkImageMemoryBarrier2{
               .sType            = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER_2,
               .srcStageMask     = VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT,
               .srcAccessMask    = VK_ACCESS_2_MEMORY_WRITE_BIT,
               .dstStageMask     = VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT,
               .dstAccessMask    = VK_ACCESS_2_MEMORY_READ_BIT | VK_ACCESS_2_MEMORY_WRITE_BIT,
               .oldLayout        = toVkImageLayout(b.before),
               .newLayout        = toVkImageLayout(b.after),
               .image            = asImage(m_table->resolveTexture(b.texture)),
               .subresourceRange = {.aspectMask     = static_cast<VkImageAspectFlags>(b.range.aspect == TextureAspect::depth ? VK_IMAGE_ASPECT_DEPTH_BIT : VK_IMAGE_ASPECT_COLOR_BIT),
                                    .baseMipLevel   = b.range.baseMipLevel,
                                    .levelCount     = b.range.levelCount,
                                    .baseArrayLayer = b.range.baseArrayLayer,
                                    .layerCount     = b.range.layerCount},
    };
  }
  for(uint32_t i = 0; i < bufferCount; ++i)
  {
    const BufferBarrier& b = buffers[i];
    bufferBarriers[i]      = VkBufferMemoryBarrier2{
             .sType         = VK_STRUCTURE_TYPE_BUFFER_MEMORY_BARRIER_2,
             .srcStageMask  = VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT,
             .srcAccessMask = VK_ACCESS_2_MEMORY_WRITE_BIT,
             .dstStageMask  = VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT,
             .dstAccessMask = VK_ACCESS_2_MEMORY_READ_BIT | VK_ACCESS_2_MEMORY_WRITE_BIT,
             .buffer        = asBuffer(m_table->resolveBuffer(b.buffer)),
             .offset        = b.offset,
             .size          = b.size == 0 ? VK_WHOLE_SIZE : b.size,
    };
  }
  const VkDependencyInfo dependencyInfo{
      .sType                    = VK_STRUCTURE_TYPE_DEPENDENCY_INFO,
      .bufferMemoryBarrierCount = bufferCount,
      .pBufferMemoryBarriers    = bufferBarriers.empty() ? nullptr : bufferBarriers.data(),
      .imageMemoryBarrierCount  = textureCount,
      .pImageMemoryBarriers     = imageBarriers.empty() ? nullptr : imageBarriers.data(),
  };
  vkCmdPipelineBarrier2(m_cmd, &dependencyInfo);
}

void VulkanCommandBuffer::beginEvent(const char* name)
{
  if(vkCmdBeginDebugUtilsLabelEXT != nullptr)
  {
    const VkDebugUtilsLabelEXT label{.sType = VK_STRUCTURE_TYPE_DEBUG_UTILS_LABEL_EXT, .pLabelName = name};
    vkCmdBeginDebugUtilsLabelEXT(m_cmd, &label);
  }
}

void VulkanCommandBuffer::endEvent()
{
  if(vkCmdEndDebugUtilsLabelEXT != nullptr)
  {
    vkCmdEndDebugUtilsLabelEXT(m_cmd);
  }
}

}  // namespace demo::rhi::vulkan
