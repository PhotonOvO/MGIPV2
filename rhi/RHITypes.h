#pragma once

#include "RHIHandles.h"

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
  DrawIndirect   = 1u << 6u,
  Host           = 1u << 7u,
  All            = TopOfPipe | VertexShader | FragmentShader | Compute | Transfer | BottomOfPipe | DrawIndirect | Host,

  none           = None,
  topOfPipe      = TopOfPipe,
  vertexShader   = VertexShader,
  fragmentShader = FragmentShader,
  compute        = Compute,
  transfer       = Transfer,
  bottomOfPipe   = BottomOfPipe,
  drawIndirect   = DrawIndirect,
  host           = Host,
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
  depthStencil,
};

enum class ImageViewType : uint8_t
{
  e2D = 0,
  e2DArray,
  eCube,
  e3D,
};

enum class ComponentSwizzle : uint8_t
{
  identity = 0,
  zero,
  one,
  r,
  g,
  b,
  a,
};

struct ComponentMapping
{
  ComponentSwizzle r{ComponentSwizzle::identity};
  ComponentSwizzle g{ComponentSwizzle::identity};
  ComponentSwizzle b{ComponentSwizzle::identity};
  ComponentSwizzle a{ComponentSwizzle::identity};
};

// Describes a texture view to create through the RHI. The handle returned by
// RenderDevice::createTextureView is the only thing business/pass code should hold.
// NOTE (transitional): nativeImage/nativeFormat are native Vulkan values because images
// are not yet RHI handles and the RHI format enum does not cover every format — this is
// the deliberate "creation seam"; everything downstream of creation is handle-only.
struct TextureViewCreateDesc
{
  // Prefer `image` (an RHI handle). `nativeImage` is the legacy seam for call sites that
  // still hold a raw VkImage; createTextureView resolves `image` first when it is set.
  TextureHandle    image{};
  uint64_t         nativeImage{0};
  uint64_t         nativeFormat{0};
  ImageViewType    viewType{ImageViewType::e2D};
  TextureAspect    aspect{TextureAspect::color};
  uint32_t         baseMipLevel{0};
  uint32_t         levelCount{1};
  uint32_t         baseArrayLayer{0};
  uint32_t         layerCount{1};
  ComponentMapping components{};
  const char*      debugName{nullptr};
};

enum class TextureDimension : uint8_t
{
  e2D = 0,
  e2DArray,
  eCube,
  e3D,
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
  rgba16Sfloat = 3,
  d16Unorm    = 4,
  d32Sfloat   = 5,
  d24UnormS8  = 6,
  d32SfloatS8 = 7,
};

enum class DynamicState : uint8_t
{
  viewport = 0,
  scissor,
  depthBias,
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

enum class IndexFormat : uint8_t
{
  uint16 = 0,
  uint32 = 1,
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
  bool              depthBiasEnable{false};
  float             depthBiasConstantFactor{0.0f};
  float             depthBiasClamp{0.0f};
  float             depthBiasSlopeFactor{0.0f};
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

struct Offset3D
{
  int32_t x{0};
  int32_t y{0};
  int32_t z{0};
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

// ---------------------------------------------------------------------------
// Modern GPU interface additions (Wave 0). Semantic usage flags (NOT
// bit-transparent to any backend), GpuPtr (typed buffer device address),
// buffer/sampler descs, and the render-pass descs relocated from
// RHICommandList.h so the new Encoder/CommandBuffer headers do not depend on
// the (to-be-removed) CommandList header.
// ---------------------------------------------------------------------------

// Typed wrapper over a buffer GPU address. Only represents addressable buffer
// memory; textures/samplers use view handles / descriptor indices, never GpuPtr.
struct GpuPtr
{
  uint64_t value{0};

  [[nodiscard]] constexpr bool isValid() const noexcept { return value != 0; }
};

enum class MemoryUsage : uint8_t
{
  gpuOnly = 0,
  cpuToGpu,
  gpuToCpu,
  transientAttachment,
};

enum class BufferUsageFlags : uint32_t
{
  none                = 0,
  vertex              = 1u << 0u,
  index               = 1u << 1u,
  uniform             = 1u << 2u,
  storage             = 1u << 3u,
  indirect            = 1u << 4u,
  transferSrc         = 1u << 5u,
  transferDst         = 1u << 6u,
  shaderDeviceAddress = 1u << 7u,
};

constexpr BufferUsageFlags operator|(BufferUsageFlags a, BufferUsageFlags b)
{
  return static_cast<BufferUsageFlags>(static_cast<uint32_t>(a) | static_cast<uint32_t>(b));
}
constexpr BufferUsageFlags operator&(BufferUsageFlags a, BufferUsageFlags b)
{
  return static_cast<BufferUsageFlags>(static_cast<uint32_t>(a) & static_cast<uint32_t>(b));
}

enum class TextureUsageFlags : uint32_t
{
  none            = 0,
  sampled         = 1u << 0u,
  storage         = 1u << 1u,
  colorAttachment = 1u << 2u,
  depthAttachment = 1u << 3u,
  transferSrc     = 1u << 4u,
  transferDst     = 1u << 5u,
  inputAttachment = 1u << 6u,
};

constexpr TextureUsageFlags operator|(TextureUsageFlags a, TextureUsageFlags b)
{
  return static_cast<TextureUsageFlags>(static_cast<uint32_t>(a) | static_cast<uint32_t>(b));
}
constexpr TextureUsageFlags operator&(TextureUsageFlags a, TextureUsageFlags b)
{
  return static_cast<TextureUsageFlags>(static_cast<uint32_t>(a) & static_cast<uint32_t>(b));
}

struct BufferDesc
{
  uint64_t         size{0};
  BufferUsageFlags usage{BufferUsageFlags::none};
  MemoryUsage      memoryUsage{MemoryUsage::gpuOnly};
  bool             allowGpuAddress{false};
  bool             allowIndirectArgument{false};
  bool             allowArgumentTableBinding{false};
  const char*      debugName{nullptr};
};

enum class Filter : uint8_t
{
  nearest = 0,
  linear,
};

enum class MipmapMode : uint8_t
{
  nearest = 0,
  linear,
};

enum class AddressMode : uint8_t
{
  repeat = 0,
  clampToEdge,
  clampToBorder,
  mirroredRepeat,
};

struct SamplerDesc
{
  Filter      magFilter{Filter::linear};
  Filter      minFilter{Filter::linear};
  MipmapMode  mipmapMode{MipmapMode::linear};
  AddressMode addressModeU{AddressMode::repeat};
  AddressMode addressModeV{AddressMode::repeat};
  AddressMode addressModeW{AddressMode::repeat};
  float       mipLodBias{0.0f};
  bool        anisotropyEnable{false};
  float       maxAnisotropy{1.0f};
  bool        compareEnable{false};
  CompareOp   compareOp{CompareOp::never};
  float       minLod{0.0f};
  float       maxLod{1000.0f};
  const char* debugName{nullptr};
};

// --- Render-pass descs (relocated from RHICommandList.h) ---

struct RenderTargetDesc
{
  TextureHandle     texture{};
  TextureViewHandle view{};  // Texture view for rendering
  ResourceState     state{ResourceState::general};
  LoadOp            loadOp{LoadOp::load};
  StoreOp           storeOp{StoreOp::store};
  ClearColorValue   clearColor{};
};

struct DepthTargetDesc
{
  TextureHandle          texture{};
  TextureViewHandle      view{};  // Texture view for rendering
  ResourceState          state{ResourceState::general};
  LoadOp                 loadOp{LoadOp::load};
  StoreOp                storeOp{StoreOp::store};
  ClearDepthStencilValue clearValue{};
};

// Reserved for tile-resident deferred (input attachment / local read). The
// Vulkan backend maps this to VK_KHR_dynamic_rendering_local_read when
// available and falls back to separate-pass sampling otherwise.
struct InputAttachmentDesc
{
  TextureViewHandle view{};
  ResourceState     state{ResourceState::ShaderRead};
};

struct RenderPassDesc
{
  Rect2D                  renderArea{};
  const RenderTargetDesc* colorTargets{nullptr};
  uint32_t                colorTargetCount{0};
  const DepthTargetDesc*  depthTarget{nullptr};
  // --- reserved: tile-resident deferred (see InputAttachmentDesc) ---
  const InputAttachmentDesc* inputAttachments{nullptr};
  uint32_t                   inputAttachmentCount{0};
  bool                       enableLocalRead{false};
};

}  // namespace demo::rhi
