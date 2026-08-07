#pragma once

#include "../RHIStageBarrier.h"
#include "VulkanResourceConversions.h"

#include <vulkan/vulkan.h>

namespace demo::rhi::vulkan {

[[nodiscard]] constexpr bool hasStage(StageFlags stages, StageFlags bit) noexcept
{
  return (static_cast<uint64_t>(stages) & static_cast<uint64_t>(bit)) != 0;
}

[[nodiscard]] constexpr bool hasHazard(HazardFlags hazards, HazardFlags bit) noexcept
{
  return (static_cast<uint32_t>(hazards) & static_cast<uint32_t>(bit)) != 0;
}

[[nodiscard]] constexpr VkPipelineStageFlags2 toVkPipelineStage2(StageFlags stages) noexcept
{
  if(stages == StageFlags::all || stages == StageFlags::none)
    return VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT;
  VkPipelineStageFlags2 out = VK_PIPELINE_STAGE_2_NONE;
  if(hasStage(stages, StageFlags::transfer)) out |= VK_PIPELINE_STAGE_2_ALL_TRANSFER_BIT;
  if(hasStage(stages, StageFlags::compute)) out |= VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT;
  if(hasStage(stages, StageFlags::vertexShader)) out |= VK_PIPELINE_STAGE_2_VERTEX_SHADER_BIT;
  if(hasStage(stages, StageFlags::fragmentShader)) out |= VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT;
  if(hasStage(stages, StageFlags::rasterColorOut)) out |= VK_PIPELINE_STAGE_2_COLOR_ATTACHMENT_OUTPUT_BIT;
  if(hasStage(stages, StageFlags::rasterDepthOut))
    out |= VK_PIPELINE_STAGE_2_EARLY_FRAGMENT_TESTS_BIT |
           VK_PIPELINE_STAGE_2_LATE_FRAGMENT_TESTS_BIT;
  if(hasStage(stages, StageFlags::commandInput)) out |= VK_PIPELINE_STAGE_2_DRAW_INDIRECT_BIT;
  constexpr uint64_t knownMask =
    static_cast<uint64_t>(StageFlags::transfer) |
    static_cast<uint64_t>(StageFlags::compute) |
    static_cast<uint64_t>(StageFlags::vertexShader) |
    static_cast<uint64_t>(StageFlags::fragmentShader) |
    static_cast<uint64_t>(StageFlags::rasterColorOut) |
    static_cast<uint64_t>(StageFlags::rasterDepthOut) |
    static_cast<uint64_t>(StageFlags::commandInput);
  if((static_cast<uint64_t>(stages) & ~knownMask) != 0)
    return VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT;
  return out;
}

[[nodiscard]] constexpr VkAccessFlags2 inferProducerAccess(
  HazardFlags hazards, StageFlags producerStages) noexcept
{
  if(hasHazard(hazards, HazardFlags::readBeforeWrite))
    return VK_ACCESS_2_MEMORY_READ_BIT |
           VK_ACCESS_2_SHADER_SAMPLED_READ_BIT |
           VK_ACCESS_2_SHADER_STORAGE_READ_BIT;
  VkAccessFlags2 out = 0;
  if(hasStage(producerStages, StageFlags::transfer)) out |= VK_ACCESS_2_TRANSFER_WRITE_BIT;
  if(hasStage(producerStages, StageFlags::compute) ||
     hasStage(producerStages, StageFlags::vertexShader) ||
     hasStage(producerStages, StageFlags::fragmentShader))
    out |= VK_ACCESS_2_SHADER_STORAGE_WRITE_BIT;
  if(hasStage(producerStages, StageFlags::rasterColorOut)) out |= VK_ACCESS_2_COLOR_ATTACHMENT_WRITE_BIT;
  if(hasStage(producerStages, StageFlags::rasterDepthOut) ||
     hasHazard(hazards, HazardFlags::depthStencil))
    out |= VK_ACCESS_2_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT;
  return out == 0 ? VK_ACCESS_2_MEMORY_WRITE_BIT : out;
}

[[nodiscard]] constexpr VkAccessFlags2 inferConsumerAccess(
  HazardFlags hazards, StageFlags consumerStages) noexcept
{
  if(hasHazard(hazards, HazardFlags::readBeforeWrite))
  {
    VkAccessFlags2 out = VK_ACCESS_2_MEMORY_WRITE_BIT;
    if(hasStage(consumerStages, StageFlags::transfer)) out |= VK_ACCESS_2_TRANSFER_WRITE_BIT;
    if(hasStage(consumerStages, StageFlags::compute) ||
       hasStage(consumerStages, StageFlags::vertexShader) ||
       hasStage(consumerStages, StageFlags::fragmentShader))
      out |= VK_ACCESS_2_SHADER_STORAGE_WRITE_BIT;
    if(hasStage(consumerStages, StageFlags::rasterColorOut)) out |= VK_ACCESS_2_COLOR_ATTACHMENT_WRITE_BIT;
    if(hasStage(consumerStages, StageFlags::rasterDepthOut)) out |= VK_ACCESS_2_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT;
    return out;
  }
  VkAccessFlags2 out = hasHazard(hazards, HazardFlags::storageBufferReadWrite)
    ? VK_ACCESS_2_SHADER_STORAGE_READ_BIT | VK_ACCESS_2_SHADER_STORAGE_WRITE_BIT
    : VK_ACCESS_2_MEMORY_READ_BIT;
  if(hasHazard(hazards, HazardFlags::drawArguments)) out |= VK_ACCESS_2_INDIRECT_COMMAND_READ_BIT;
  return out;
}

[[nodiscard]] constexpr VkMemoryBarrier2 makeMemoryBarrier2(
  StageFlags producer, StageFlags consumer, HazardFlags hazards) noexcept
{
  VkPipelineStageFlags2 destinationStage = toVkPipelineStage2(consumer);
  if(hasHazard(hazards, HazardFlags::drawArguments))
    destinationStage |= VK_PIPELINE_STAGE_2_DRAW_INDIRECT_BIT;
  return VkMemoryBarrier2{
    .sType = VK_STRUCTURE_TYPE_MEMORY_BARRIER_2,
    .srcStageMask = toVkPipelineStage2(producer),
    .srcAccessMask = inferProducerAccess(hazards, producer),
    .dstStageMask = destinationStage,
    .dstAccessMask = inferConsumerAccess(hazards, consumer),
  };
}

[[nodiscard]] constexpr VkImageLayout toVkImageLayout(ResourceState state) noexcept
{
  switch(state)
  {
  case ResourceState::Undefined: return VK_IMAGE_LAYOUT_UNDEFINED;
  case ResourceState::General: return VK_IMAGE_LAYOUT_GENERAL;
  case ResourceState::ColorAttachment: return VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
  case ResourceState::DepthStencilAttachment: return VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL;
  case ResourceState::DepthStencilReadOnly: return VK_IMAGE_LAYOUT_DEPTH_STENCIL_READ_ONLY_OPTIMAL;
  case ResourceState::ShaderRead: return VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
  case ResourceState::ShaderWrite: return VK_IMAGE_LAYOUT_GENERAL;
  case ResourceState::TransferSrc: return VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL;
  case ResourceState::TransferDst: return VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
  case ResourceState::Present: return VK_IMAGE_LAYOUT_PRESENT_SRC_KHR;
  case ResourceState::IndirectArgument: return VK_IMAGE_LAYOUT_UNDEFINED;
  }
  return VK_IMAGE_LAYOUT_UNDEFINED;
}

}  // namespace demo::rhi::vulkan
