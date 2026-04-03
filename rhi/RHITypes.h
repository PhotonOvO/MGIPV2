#pragma once

#include <cstdint>

namespace demo::rhi {

enum class QueueType : uint8_t
{
  graphics = 0,
};

enum class ShaderStage : uint32_t
{
  none        = 0,
  vertex      = 1u << 0u,
  fragment    = 1u << 1u,
  compute     = 1u << 2u,
  geometry    = 1u << 3u,
  tessControl = 1u << 4u,
  tessEval    = 1u << 5u,
  allGraphics = vertex | fragment,
  all         = allGraphics | compute | geometry | tessControl | tessEval,
};

constexpr ShaderStage operator|(ShaderStage lhs, ShaderStage rhs)
{
  return static_cast<ShaderStage>(static_cast<uint32_t>(lhs) | static_cast<uint32_t>(rhs));
}

constexpr ShaderStage operator|=(ShaderStage& lhs, ShaderStage rhs)
{
  lhs = lhs | rhs;
  return lhs;
}

constexpr bool any(ShaderStage stages)
{
  return static_cast<uint32_t>(stages) != 0;
}

enum class ResourceAccess : uint8_t
{
  read = 0,
  write,
  readWrite,
};

enum class ResourceState : uint8_t
{
  Undefined = 0,
  General,
  ColorAttachment,
  DepthStencilAttachment,
  ShaderRead,
  ShaderWrite,
  TransferSrc,
  TransferDst,
  Present,

  undefined       = Undefined,
  general         = General,
  colorAttachment = ColorAttachment,
  depthAttachment = DepthStencilAttachment,
  shaderRead      = ShaderRead,
  shaderWrite     = ShaderWrite,
  transferSrc     = TransferSrc,
  transferDst     = TransferDst,
  present         = Present,
};

enum class BarrierType : uint8_t
{
  Memory = 0,
  Execution,
  LayoutTransition,

  memory           = Memory,
  execution        = Execution,
  layoutTransition = LayoutTransition,
};

enum class PipelineStage : uint32_t
{
  None           = 0,
  TopOfPipe      = 1u << 0u,
  VertexShader   = 1u << 1u,
  FragmentShader = 1u << 2u,
  Compute        = 1u << 3u,
  Transfer       = 1u << 4u,
  BottomOfPipe   = 1u << 5u,
  All            = TopOfPipe | VertexShader | FragmentShader | Compute | Transfer | BottomOfPipe,

  none           = None,
  topOfPipe      = TopOfPipe,
  vertexShader   = VertexShader,
  fragmentShader = FragmentShader,
  compute        = Compute,
  transfer       = Transfer,
  bottomOfPipe   = BottomOfPipe,
  all            = All,
};

constexpr PipelineStage operator|(PipelineStage lhs, PipelineStage rhs)
{
  return static_cast<PipelineStage>(static_cast<uint32_t>(lhs) | static_cast<uint32_t>(rhs));
}

constexpr PipelineStage operator|=(PipelineStage& lhs, PipelineStage rhs)
{
  lhs = lhs | rhs;
  return lhs;
}

constexpr bool any(PipelineStage stages)
{
  return static_cast<uint32_t>(stages) != 0;
}

enum class TextureAspect : uint8_t
{
  color = 0,
  depth,
};

enum class PipelineBindPoint : uint8_t
{
  graphics = 0,
  compute,
};

enum class TextureFormat : uint8_t
{
  undefined   = 0,
  rgba8Unorm  = 1,
  bgra8Unorm  = 2,
  d16Unorm    = 3,
  d32Sfloat   = 4,
  d24UnormS8  = 5,
  d32SfloatS8 = 6,
};

enum class DynamicState : uint8_t
{
  viewport = 0,
  scissor,
};

enum class PrimitiveTopology : uint8_t
{
  pointList    = 0,
  lineList     = 1,
  lineStrip    = 2,
  triangleList = 3,
  triangleStrip,
};

enum class PolygonMode : uint8_t
{
  fill  = 0,
  line  = 1,
  point = 2,
};

enum class CullMode : uint8_t
{
  none         = 0,
  front        = 1,
  back         = 2,
  frontAndBack = 3,
};

enum class FrontFace : uint8_t
{
  counterClockwise = 0,
  clockwise        = 1,
};

enum class CompareOp : uint8_t
{
  never          = 0,
  less           = 1,
  equal          = 2,
  lessOrEqual    = 3,
  greater        = 4,
  notEqual       = 5,
  greaterOrEqual = 6,
  always         = 7,
};

enum class BlendFactor : uint8_t
{
  zero             = 0,
  one              = 1,
  srcColor         = 2,
  oneMinusSrcColor = 3,
  dstColor         = 4,
  oneMinusDstColor = 5,
  srcAlpha         = 6,
  oneMinusSrcAlpha = 7,
  dstAlpha         = 8,
  oneMinusDstAlpha = 9,
};

enum class BlendOp : uint8_t
{
  add             = 0,
  subtract        = 1,
  reverseSubtract = 2,
  min             = 3,
  max             = 4,
};

enum class ColorComponentFlags : uint8_t
{
  none = 0,
  r    = 1u << 0u,
  g    = 1u << 1u,
  b    = 1u << 2u,
  a    = 1u << 3u,
  all  = r | g | b | a,
};

constexpr ColorComponentFlags operator|(ColorComponentFlags lhs, ColorComponentFlags rhs)
{
  return static_cast<ColorComponentFlags>(static_cast<uint8_t>(lhs) | static_cast<uint8_t>(rhs));
}

enum class SampleCount : uint8_t
{
  count1 = 1,
  count2 = 2,
  count4 = 4,
  count8 = 8,
};

enum class VertexInputRate : uint8_t
{
  perVertex   = 0,
  perInstance = 1,
};

enum class VertexFormat : uint8_t
{
  undefined          = 0,
  r32Sfloat          = 1,
  r32g32Sfloat       = 2,
  r32g32b32Sfloat    = 3,
  r32g32b32a32Sfloat = 4,
};

struct VertexBindingDesc
{
  uint32_t        binding{0};
  uint32_t        stride{0};
  VertexInputRate inputRate{VertexInputRate::perVertex};
};

struct VertexAttributeDesc
{
  uint32_t     location{0};
  uint32_t     binding{0};
  VertexFormat format{VertexFormat::undefined};
  uint32_t     offset{0};
};

struct VertexInputLayoutDesc
{
  const VertexBindingDesc*   bindings{nullptr};
  uint32_t                   bindingCount{0};
  const VertexAttributeDesc* attributes{nullptr};
  uint32_t                   attributeCount{0};
};

struct RasterState
{
  PrimitiveTopology topology{PrimitiveTopology::triangleList};
  bool              primitiveRestartEnable{false};
  PolygonMode       polygonMode{PolygonMode::fill};
  CullMode          cullMode{CullMode::none};
  FrontFace         frontFace{FrontFace::counterClockwise};
  float             lineWidth{1.0f};
  SampleCount       sampleCount{SampleCount::count1};
};

struct DepthState
{
  bool      depthTestEnable{false};
  bool      depthWriteEnable{false};
  CompareOp depthCompareOp{CompareOp::lessOrEqual};
};

struct BlendAttachmentState
{
  bool                blendEnable{false};
  BlendFactor         srcColorBlendFactor{BlendFactor::one};
  BlendFactor         dstColorBlendFactor{BlendFactor::zero};
  BlendOp             colorBlendOp{BlendOp::add};
  BlendFactor         srcAlphaBlendFactor{BlendFactor::one};
  BlendFactor         dstAlphaBlendFactor{BlendFactor::zero};
  BlendOp             alphaBlendOp{BlendOp::add};
  ColorComponentFlags colorWriteMask{ColorComponentFlags::all};
};

enum class LoadOp : uint8_t
{
  load = 0,
  clear,
  dontCare,
};

enum class StoreOp : uint8_t
{
  store = 0,
  dontCare,
};

struct Extent2D
{
  uint32_t width{0};
  uint32_t height{0};
};

struct Offset2D
{
  int32_t x{0};
  int32_t y{0};
};

struct Rect2D
{
  Offset2D offset{};
  Extent2D extent{};
};

struct Viewport
{
  float x{0.0f};
  float y{0.0f};
  float width{0.0f};
  float height{0.0f};
  float minDepth{0.0f};
  float maxDepth{1.0f};
};

struct ClearColorValue
{
  float r{0.0f};
  float g{0.0f};
  float b{0.0f};
  float a{1.0f};
};

struct ClearDepthStencilValue
{
  float    depth{1.0f};
  uint32_t stencil{0};
};

struct TimelinePoint
{
  uint64_t value{0};
};

}  // namespace demo::rhi
