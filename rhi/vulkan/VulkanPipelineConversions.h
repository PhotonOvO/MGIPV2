#pragma once

#include "../RHIPipeline.h"
#include "VulkanFormatUtils.h"
#include "VulkanResourceConversions.h"

#include <vulkan/vulkan.h>

namespace demo::rhi::vulkan {

[[nodiscard]] inline VkFormat toVkFormat(TextureFormat format) noexcept
{
  return toNativeFormat(format);
}

[[nodiscard]] constexpr VkPrimitiveTopology toVkTopology(PrimitiveTopology topology) noexcept
{
  switch(topology)
  {
  case PrimitiveTopology::pointList: return VK_PRIMITIVE_TOPOLOGY_POINT_LIST;
  case PrimitiveTopology::lineList: return VK_PRIMITIVE_TOPOLOGY_LINE_LIST;
  case PrimitiveTopology::lineStrip: return VK_PRIMITIVE_TOPOLOGY_LINE_STRIP;
  case PrimitiveTopology::triangleList: return VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST;
  case PrimitiveTopology::triangleStrip: return VK_PRIMITIVE_TOPOLOGY_TRIANGLE_STRIP;
  }
  return VK_PRIMITIVE_TOPOLOGY_MAX_ENUM;
}

[[nodiscard]] constexpr VkPolygonMode toVkPolygonMode(PolygonMode mode) noexcept
{
  switch(mode)
  {
  case PolygonMode::fill: return VK_POLYGON_MODE_FILL;
  case PolygonMode::line: return VK_POLYGON_MODE_LINE;
  case PolygonMode::point: return VK_POLYGON_MODE_POINT;
  }
  return VK_POLYGON_MODE_MAX_ENUM;
}

[[nodiscard]] constexpr VkCullModeFlags toVkCullMode(CullMode mode) noexcept
{
  switch(mode)
  {
  case CullMode::none: return VK_CULL_MODE_NONE;
  case CullMode::front: return VK_CULL_MODE_FRONT_BIT;
  case CullMode::back: return VK_CULL_MODE_BACK_BIT;
  case CullMode::frontAndBack: return VK_CULL_MODE_FRONT_AND_BACK;
  }
  return VK_CULL_MODE_FLAG_BITS_MAX_ENUM;
}

[[nodiscard]] constexpr VkFrontFace toVkFrontFace(FrontFace face) noexcept
{
  switch(face)
  {
  case FrontFace::counterClockwise: return VK_FRONT_FACE_COUNTER_CLOCKWISE;
  case FrontFace::clockwise: return VK_FRONT_FACE_CLOCKWISE;
  }
  return VK_FRONT_FACE_MAX_ENUM;
}

[[nodiscard]] constexpr VkCompareOp toVkCompareOp(CompareOp op) noexcept
{
  switch(op)
  {
  case CompareOp::never: return VK_COMPARE_OP_NEVER;
  case CompareOp::less: return VK_COMPARE_OP_LESS;
  case CompareOp::equal: return VK_COMPARE_OP_EQUAL;
  case CompareOp::lessOrEqual: return VK_COMPARE_OP_LESS_OR_EQUAL;
  case CompareOp::greater: return VK_COMPARE_OP_GREATER;
  case CompareOp::notEqual: return VK_COMPARE_OP_NOT_EQUAL;
  case CompareOp::greaterOrEqual: return VK_COMPARE_OP_GREATER_OR_EQUAL;
  case CompareOp::always: return VK_COMPARE_OP_ALWAYS;
  }
  return VK_COMPARE_OP_MAX_ENUM;
}

[[nodiscard]] constexpr VkBlendFactor toVkBlendFactor(BlendFactor factor) noexcept
{
  switch(factor)
  {
  case BlendFactor::zero: return VK_BLEND_FACTOR_ZERO;
  case BlendFactor::one: return VK_BLEND_FACTOR_ONE;
  case BlendFactor::srcColor: return VK_BLEND_FACTOR_SRC_COLOR;
  case BlendFactor::oneMinusSrcColor: return VK_BLEND_FACTOR_ONE_MINUS_SRC_COLOR;
  case BlendFactor::dstColor: return VK_BLEND_FACTOR_DST_COLOR;
  case BlendFactor::oneMinusDstColor: return VK_BLEND_FACTOR_ONE_MINUS_DST_COLOR;
  case BlendFactor::srcAlpha: return VK_BLEND_FACTOR_SRC_ALPHA;
  case BlendFactor::oneMinusSrcAlpha: return VK_BLEND_FACTOR_ONE_MINUS_SRC_ALPHA;
  case BlendFactor::dstAlpha: return VK_BLEND_FACTOR_DST_ALPHA;
  case BlendFactor::oneMinusDstAlpha: return VK_BLEND_FACTOR_ONE_MINUS_DST_ALPHA;
  }
  return VK_BLEND_FACTOR_MAX_ENUM;
}

[[nodiscard]] constexpr VkBlendOp toVkBlendOp(BlendOp op) noexcept
{
  switch(op)
  {
  case BlendOp::add: return VK_BLEND_OP_ADD;
  case BlendOp::subtract: return VK_BLEND_OP_SUBTRACT;
  case BlendOp::reverseSubtract: return VK_BLEND_OP_REVERSE_SUBTRACT;
  case BlendOp::min: return VK_BLEND_OP_MIN;
  case BlendOp::max: return VK_BLEND_OP_MAX;
  }
  return VK_BLEND_OP_MAX_ENUM;
}

[[nodiscard]] constexpr VkColorComponentFlags toVkColorMask(ColorComponentFlags mask) noexcept
{
  const uint32_t bits = static_cast<uint32_t>(mask);
  VkColorComponentFlags nativeMask = 0;
  if((bits & static_cast<uint32_t>(ColorComponentFlags::r)) != 0) nativeMask |= VK_COLOR_COMPONENT_R_BIT;
  if((bits & static_cast<uint32_t>(ColorComponentFlags::g)) != 0) nativeMask |= VK_COLOR_COMPONENT_G_BIT;
  if((bits & static_cast<uint32_t>(ColorComponentFlags::b)) != 0) nativeMask |= VK_COLOR_COMPONENT_B_BIT;
  if((bits & static_cast<uint32_t>(ColorComponentFlags::a)) != 0) nativeMask |= VK_COLOR_COMPONENT_A_BIT;
  return nativeMask;
}

[[nodiscard]] constexpr VkVertexInputRate toVkVertexInputRate(VertexInputRate rate) noexcept
{
  switch(rate)
  {
  case VertexInputRate::perVertex: return VK_VERTEX_INPUT_RATE_VERTEX;
  case VertexInputRate::perInstance: return VK_VERTEX_INPUT_RATE_INSTANCE;
  }
  return VK_VERTEX_INPUT_RATE_MAX_ENUM;
}

[[nodiscard]] constexpr VkFormat toVkVertexFormat(VertexFormat format) noexcept
{
  switch(format)
  {
  case VertexFormat::undefined: return VK_FORMAT_UNDEFINED;
  case VertexFormat::r32Sfloat: return VK_FORMAT_R32_SFLOAT;
  case VertexFormat::r32g32Sfloat: return VK_FORMAT_R32G32_SFLOAT;
  case VertexFormat::r32g32b32Sfloat: return VK_FORMAT_R32G32B32_SFLOAT;
  case VertexFormat::r32g32b32a32Sfloat: return VK_FORMAT_R32G32B32A32_SFLOAT;
  case VertexFormat::r8g8b8a8Unorm: return VK_FORMAT_R8G8B8A8_UNORM;
  }
  return VK_FORMAT_UNDEFINED;
}

[[nodiscard]] constexpr VkDynamicState toVkDynamicState(DynamicState state) noexcept
{
  switch(state)
  {
  case DynamicState::viewport: return VK_DYNAMIC_STATE_VIEWPORT_WITH_COUNT;
  case DynamicState::scissor: return VK_DYNAMIC_STATE_SCISSOR_WITH_COUNT;
  case DynamicState::depthBias: return VK_DYNAMIC_STATE_DEPTH_BIAS;
  }
  return VK_DYNAMIC_STATE_MAX_ENUM;
}

}  // namespace demo::rhi::vulkan
