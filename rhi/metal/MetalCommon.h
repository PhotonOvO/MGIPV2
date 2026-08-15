#pragma once

#include "../RHIStageBarrier.h"
#include "../RHITypes.h"

#include <Foundation/Foundation.hpp>
#include <Metal/MTL4CommandEncoder.hpp>
#include <Metal/MTLDepthStencil.hpp>
#include <Metal/MTLPixelFormat.hpp>
#include <Metal/MTLRenderCommandEncoder.hpp>
#include <Metal/MTLRenderPass.hpp>
#include <Metal/MTLRenderPipeline.hpp>
#include <Metal/MTLResource.hpp>
#include <Metal/MTLSampler.hpp>
#include <Metal/MTLTexture.hpp>
#include <Metal/MTLVertexDescriptor.hpp>

#include <algorithm>
#include <cstdint>
#include <stdexcept>
#include <string>

namespace demo::rhi::metal {

inline NS::String* nsString(const char* value)
{
  return NS::String::string(value != nullptr ? value : "", NS::UTF8StringEncoding);
}

inline std::string errorMessage(const char* operation, NS::Error* error)
{
  std::string result = operation;
  result += " failed";
  if(error != nullptr && error->localizedDescription() != nullptr)
  {
    result += ": ";
    result += error->localizedDescription()->utf8String();
  }
  return result;
}

inline void require(bool condition, const char* message)
{
  if(!condition)
    throw std::runtime_error(message);
}

inline bool hasStage(ShaderStage mask, ShaderStage stage)
{
  return (static_cast<uint32_t>(mask) & static_cast<uint32_t>(stage)) != 0;
}

inline bool hasUsage(BufferUsageFlags mask, BufferUsageFlags flag)
{
  return static_cast<uint32_t>(mask & flag) != 0;
}

inline bool hasUsage(TextureUsageFlags mask, TextureUsageFlags flag)
{
  return static_cast<uint32_t>(mask & flag) != 0;
}

inline MTL::PixelFormat toMetalPixelFormat(TextureFormat format)
{
  switch(format)
  {
  case TextureFormat::rgba8Unorm:       return MTL::PixelFormatRGBA8Unorm;
  case TextureFormat::rgba8Srgb:        return MTL::PixelFormatRGBA8Unorm_sRGB;
  case TextureFormat::bgra8Unorm:       return MTL::PixelFormatBGRA8Unorm;
  case TextureFormat::rgba16Sfloat:     return MTL::PixelFormatRGBA16Float;
  case TextureFormat::d16Unorm:         return MTL::PixelFormatDepth16Unorm;
  case TextureFormat::d32Sfloat:        return MTL::PixelFormatDepth32Float;
  case TextureFormat::d24UnormS8:       return MTL::PixelFormatDepth24Unorm_Stencil8;
  case TextureFormat::d32SfloatS8:      return MTL::PixelFormatDepth32Float_Stencil8;
  case TextureFormat::rg16Sfloat:       return MTL::PixelFormatRG16Float;
  case TextureFormat::r32Sfloat:        return MTL::PixelFormatR32Float;
  case TextureFormat::r16Sfloat:        return MTL::PixelFormatR16Float;
  case TextureFormat::rgba8Snorm:       return MTL::PixelFormatRGBA8Snorm;
  case TextureFormat::r11g11b10Ufloat:  return MTL::PixelFormatRG11B10Float;
  case TextureFormat::bc6hUfloatBlock:  return MTL::PixelFormatBC6H_RGBUfloat;
  case TextureFormat::bc6hSfloatBlock:  return MTL::PixelFormatBC6H_RGBFloat;
  case TextureFormat::bc7UnormBlock:    return MTL::PixelFormatBC7_RGBAUnorm;
  case TextureFormat::bc7SrgbBlock:     return MTL::PixelFormatBC7_RGBAUnorm_sRGB;
  case TextureFormat::undefined:        return MTL::PixelFormatInvalid;
  }
  return MTL::PixelFormatInvalid;
}

inline TextureFormat fromMetalPixelFormat(MTL::PixelFormat format)
{
  switch(format)
  {
  case MTL::PixelFormatRGBA8Unorm:             return TextureFormat::rgba8Unorm;
  case MTL::PixelFormatRGBA8Unorm_sRGB:        return TextureFormat::rgba8Srgb;
  case MTL::PixelFormatBGRA8Unorm:             return TextureFormat::bgra8Unorm;
  case MTL::PixelFormatRGBA16Float:            return TextureFormat::rgba16Sfloat;
  case MTL::PixelFormatDepth16Unorm:           return TextureFormat::d16Unorm;
  case MTL::PixelFormatDepth32Float:           return TextureFormat::d32Sfloat;
  case MTL::PixelFormatDepth24Unorm_Stencil8:  return TextureFormat::d24UnormS8;
  case MTL::PixelFormatDepth32Float_Stencil8:  return TextureFormat::d32SfloatS8;
  case MTL::PixelFormatRG16Float:              return TextureFormat::rg16Sfloat;
  case MTL::PixelFormatR32Float:               return TextureFormat::r32Sfloat;
  case MTL::PixelFormatR16Float:               return TextureFormat::r16Sfloat;
  case MTL::PixelFormatRGBA8Snorm:             return TextureFormat::rgba8Snorm;
  case MTL::PixelFormatRG11B10Float:           return TextureFormat::r11g11b10Ufloat;
  case MTL::PixelFormatBC6H_RGBUfloat:         return TextureFormat::bc6hUfloatBlock;
  case MTL::PixelFormatBC6H_RGBFloat:          return TextureFormat::bc6hSfloatBlock;
  case MTL::PixelFormatBC7_RGBAUnorm:          return TextureFormat::bc7UnormBlock;
  case MTL::PixelFormatBC7_RGBAUnorm_sRGB:     return TextureFormat::bc7SrgbBlock;
  default:                                     return TextureFormat::undefined;
  }
}

inline MTL::StorageMode toMetalStorageMode(MemoryUsage usage)
{
  switch(usage)
  {
  case MemoryUsage::gpuOnly:             return MTL::StorageModePrivate;
  case MemoryUsage::cpuToGpu:            return MTL::StorageModeShared;
  case MemoryUsage::gpuToCpu:            return MTL::StorageModeShared;
  case MemoryUsage::transientAttachment: return MTL::StorageModeMemoryless;
  }
  return MTL::StorageModePrivate;
}

inline MTL::ResourceOptions toMetalBufferOptions(MemoryUsage usage)
{
  const NS::UInteger storage =
    usage == MemoryUsage::gpuOnly || usage == MemoryUsage::transientAttachment
      ? MTL::ResourceStorageModePrivate
      : MTL::ResourceStorageModeShared;
  return static_cast<MTL::ResourceOptions>(
    storage | MTL::ResourceHazardTrackingModeUntracked);
}

inline MTL::TextureUsage toMetalTextureUsage(
  TextureUsageFlags usage, bool supportsRenderPassClear = false)
{
  NS::UInteger result = MTL::TextureUsageUnknown;
  if(hasUsage(usage, TextureUsageFlags::sampled) ||
     hasUsage(usage, TextureUsageFlags::inputAttachment) ||
     hasUsage(usage, TextureUsageFlags::transferSrc))
    result |= MTL::TextureUsageShaderRead;
  if(hasUsage(usage, TextureUsageFlags::storage))
    result |= MTL::TextureUsageShaderRead | MTL::TextureUsageShaderWrite;
  if(hasUsage(usage, TextureUsageFlags::transferDst))
    result |= MTL::TextureUsageShaderWrite;
  if(hasUsage(usage, TextureUsageFlags::colorAttachment) ||
     hasUsage(usage, TextureUsageFlags::depthAttachment) ||
     (supportsRenderPassClear &&
      hasUsage(usage, TextureUsageFlags::transferDst)))
    result |= MTL::TextureUsageRenderTarget;
  return static_cast<MTL::TextureUsage>(result);
}

inline MTL::TextureType toMetalTextureType(TextureDimension dimension, SampleCount samples)
{
  if(samples != SampleCount::count1)
  {
    require(dimension == TextureDimension::e2D,
            "Metal multisample textures must be two-dimensional");
    return MTL::TextureType2DMultisample;
  }
  switch(dimension)
  {
  case TextureDimension::e2D:      return MTL::TextureType2D;
  case TextureDimension::e2DArray: return MTL::TextureType2DArray;
  case TextureDimension::eCube:    return MTL::TextureTypeCube;
  case TextureDimension::e3D:      return MTL::TextureType3D;
  }
  return MTL::TextureType2D;
}

inline MTL::TextureType toMetalTextureType(ImageViewType type)
{
  switch(type)
  {
  case ImageViewType::e2D:      return MTL::TextureType2D;
  case ImageViewType::e2DArray: return MTL::TextureType2DArray;
  case ImageViewType::eCube:    return MTL::TextureTypeCube;
  case ImageViewType::e3D:      return MTL::TextureType3D;
  }
  return MTL::TextureType2D;
}

inline MTL::TextureSwizzle toMetalSwizzle(ComponentSwizzle swizzle)
{
  switch(swizzle)
  {
  case ComponentSwizzle::zero: return MTL::TextureSwizzleZero;
  case ComponentSwizzle::one:  return MTL::TextureSwizzleOne;
  case ComponentSwizzle::g:    return MTL::TextureSwizzleGreen;
  case ComponentSwizzle::b:    return MTL::TextureSwizzleBlue;
  case ComponentSwizzle::a:    return MTL::TextureSwizzleAlpha;
  case ComponentSwizzle::identity:
  case ComponentSwizzle::r:    return MTL::TextureSwizzleRed;
  }
  return MTL::TextureSwizzleRed;
}

inline MTL::LoadAction toMetalLoadAction(LoadOp op)
{
  switch(op)
  {
  case LoadOp::load:     return MTL::LoadActionLoad;
  case LoadOp::clear:    return MTL::LoadActionClear;
  case LoadOp::dontCare: return MTL::LoadActionDontCare;
  }
  return MTL::LoadActionDontCare;
}

inline MTL::StoreAction toMetalStoreAction(StoreOp op)
{
  return op == StoreOp::store ? MTL::StoreActionStore : MTL::StoreActionDontCare;
}

inline MTL::PrimitiveType toMetalPrimitive(PrimitiveTopology topology)
{
  switch(topology)
  {
  case PrimitiveTopology::pointList:     return MTL::PrimitiveTypePoint;
  case PrimitiveTopology::lineList:      return MTL::PrimitiveTypeLine;
  case PrimitiveTopology::lineStrip:     return MTL::PrimitiveTypeLineStrip;
  case PrimitiveTopology::triangleList:  return MTL::PrimitiveTypeTriangle;
  case PrimitiveTopology::triangleStrip: return MTL::PrimitiveTypeTriangleStrip;
  }
  return MTL::PrimitiveTypeTriangle;
}

inline MTL::PrimitiveTopologyClass toMetalTopologyClass(PrimitiveTopology topology)
{
  if(topology == PrimitiveTopology::pointList)
    return MTL::PrimitiveTopologyClassPoint;
  if(topology == PrimitiveTopology::lineList || topology == PrimitiveTopology::lineStrip)
    return MTL::PrimitiveTopologyClassLine;
  return MTL::PrimitiveTopologyClassTriangle;
}

inline MTL::IndexType toMetalIndexType(IndexFormat format)
{
  return format == IndexFormat::uint16 ? MTL::IndexTypeUInt16 : MTL::IndexTypeUInt32;
}

inline uint32_t indexElementSize(IndexFormat format)
{
  return format == IndexFormat::uint16 ? 2u : 4u;
}

inline MTL::CompareFunction toMetalCompare(CompareOp op)
{
  return static_cast<MTL::CompareFunction>(static_cast<NS::UInteger>(op));
}

inline MTL::BlendFactor toMetalBlendFactor(BlendFactor factor)
{
  switch(factor)
  {
  case BlendFactor::zero:             return MTL::BlendFactorZero;
  case BlendFactor::one:              return MTL::BlendFactorOne;
  case BlendFactor::srcColor:         return MTL::BlendFactorSourceColor;
  case BlendFactor::oneMinusSrcColor: return MTL::BlendFactorOneMinusSourceColor;
  case BlendFactor::dstColor:         return MTL::BlendFactorDestinationColor;
  case BlendFactor::oneMinusDstColor: return MTL::BlendFactorOneMinusDestinationColor;
  case BlendFactor::srcAlpha:         return MTL::BlendFactorSourceAlpha;
  case BlendFactor::oneMinusSrcAlpha: return MTL::BlendFactorOneMinusSourceAlpha;
  case BlendFactor::dstAlpha:         return MTL::BlendFactorDestinationAlpha;
  case BlendFactor::oneMinusDstAlpha: return MTL::BlendFactorOneMinusDestinationAlpha;
  }
  return MTL::BlendFactorOne;
}

inline MTL::BlendOperation toMetalBlendOperation(BlendOp op)
{
  return static_cast<MTL::BlendOperation>(static_cast<NS::UInteger>(op));
}

inline MTL::VertexFormat toMetalVertexFormat(VertexFormat format)
{
  switch(format)
  {
  case VertexFormat::r32Sfloat:          return MTL::VertexFormatFloat;
  case VertexFormat::r32g32Sfloat:       return MTL::VertexFormatFloat2;
  case VertexFormat::r32g32b32Sfloat:    return MTL::VertexFormatFloat3;
  case VertexFormat::r32g32b32a32Sfloat: return MTL::VertexFormatFloat4;
  case VertexFormat::r8g8b8a8Unorm:      return MTL::VertexFormatUChar4Normalized;
  case VertexFormat::undefined:          return MTL::VertexFormatInvalid;
  }
  return MTL::VertexFormatInvalid;
}

inline MTL::RenderStages toMetalRenderStages(ShaderStage stages)
{
  NS::UInteger result = 0;
  if(hasStage(stages, ShaderStage::vertex))
    result |= MTL::RenderStageVertex;
  if(hasStage(stages, ShaderStage::fragment))
    result |= MTL::RenderStageFragment;
  return static_cast<MTL::RenderStages>(result);
}

inline MTL::Stages toMetalStages(StageFlags stages)
{
  // Match the portable/Vulkan contract: an unspecified scope is conservative,
  // not an empty dependency.
  if(stages == StageFlags::all || stages == StageFlags::none)
    return MTL::StageAll;
  NS::UInteger result = 0;
  if(any(stages & StageFlags::vertexShader) || any(stages & StageFlags::commandInput))
    result |= MTL::StageVertex;
  if(any(stages & StageFlags::fragmentShader) || any(stages & StageFlags::rasterColorOut) ||
     any(stages & StageFlags::rasterDepthOut))
    result |= MTL::StageFragment;
  if(any(stages & StageFlags::compute))
    result |= MTL::StageDispatch;
  if(any(stages & StageFlags::transfer))
    result |= MTL::StageBlit;
  return static_cast<MTL::Stages>(result);
}

inline MTL4::VisibilityOptions toMetalVisibility(HazardFlags hazards)
{
  (void)hazards;
  NS::UInteger result = MTL4::VisibilityOptionDevice;
  return static_cast<MTL4::VisibilityOptions>(result);
}

inline MTL::SamplerMinMagFilter toMetalFilter(Filter filter)
{
  return filter == Filter::linear ? MTL::SamplerMinMagFilterLinear
                                  : MTL::SamplerMinMagFilterNearest;
}

inline MTL::SamplerMipFilter toMetalMipFilter(MipmapMode filter)
{
  return filter == MipmapMode::linear ? MTL::SamplerMipFilterLinear
                                      : MTL::SamplerMipFilterNearest;
}

inline MTL::SamplerAddressMode toMetalAddressMode(AddressMode mode)
{
  switch(mode)
  {
  case AddressMode::repeat:         return MTL::SamplerAddressModeRepeat;
  case AddressMode::clampToEdge:    return MTL::SamplerAddressModeClampToEdge;
  case AddressMode::clampToBorder:  return MTL::SamplerAddressModeClampToBorderColor;
  case AddressMode::mirroredRepeat: return MTL::SamplerAddressModeMirrorRepeat;
  }
  return MTL::SamplerAddressModeClampToEdge;
}

inline uint32_t formatBytesPerBlock(TextureFormat format)
{
  switch(format)
  {
  case TextureFormat::rgba16Sfloat:    return 8;
  case TextureFormat::d32SfloatS8:     return 8;
  case TextureFormat::r16Sfloat:
  case TextureFormat::d16Unorm:        return 2;
  case TextureFormat::bc6hUfloatBlock:
  case TextureFormat::bc6hSfloatBlock:
  case TextureFormat::bc7UnormBlock:
  case TextureFormat::bc7SrgbBlock:    return 16;
  case TextureFormat::undefined:       return 0;
  default:                             return 4;
  }
}

inline bool isBlockCompressed(TextureFormat format)
{
  return format == TextureFormat::bc6hUfloatBlock ||
         format == TextureFormat::bc6hSfloatBlock ||
         format == TextureFormat::bc7UnormBlock ||
         format == TextureFormat::bc7SrgbBlock;
}

inline uint32_t copyBytesPerRow(TextureFormat format, uint32_t width)
{
  if(isBlockCompressed(format))
    return std::max(1u, (width + 3u) / 4u) * formatBytesPerBlock(format);
  return width * formatBytesPerBlock(format);
}

}  // namespace demo::rhi::metal
