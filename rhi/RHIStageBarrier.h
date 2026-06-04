#pragma once

#include "RHIHandles.h"
#include "RHITypes.h"

#include <cstdint>

namespace demo::rhi {

// ---------------------------------------------------------------------------
// StageBarrier (main path): express stage dependency + hazard class, not
// per-resource old/new layout. In a bindless / GpuPtr world the CPU often does
// not know which concrete resources a shader touches, so synchronization is
// expressed as producer->consumer stage scopes plus a hazard category.
// ---------------------------------------------------------------------------
enum class StageFlags : uint64_t
{
  none           = 0,
  transfer       = 1ull << 0u,
  compute        = 1ull << 1u,
  vertexShader   = 1ull << 2u,
  fragmentShader = 1ull << 3u,
  rasterColorOut = 1ull << 4u,
  rasterDepthOut = 1ull << 5u,
  commandInput   = 1ull << 6u,  // indirect draw/dispatch arguments
  all            = ~0ull,
};

constexpr StageFlags operator|(StageFlags a, StageFlags b)
{
  return static_cast<StageFlags>(static_cast<uint64_t>(a) | static_cast<uint64_t>(b));
}
constexpr StageFlags operator&(StageFlags a, StageFlags b)
{
  return static_cast<StageFlags>(static_cast<uint64_t>(a) & static_cast<uint64_t>(b));
}
constexpr StageFlags& operator|=(StageFlags& a, StageFlags b)
{
  a = a | b;
  return a;
}
constexpr bool any(StageFlags f)
{
  return static_cast<uint64_t>(f) != 0;
}

enum class HazardFlags : uint32_t
{
  none          = 0,
  descriptors   = 1u << 0u,
  drawArguments = 1u << 1u,
  depthStencil  = 1u << 2u,
  textureWrites = 1u << 3u,
  bufferWrites  = 1u << 4u,
};

constexpr HazardFlags operator|(HazardFlags a, HazardFlags b)
{
  return static_cast<HazardFlags>(static_cast<uint32_t>(a) | static_cast<uint32_t>(b));
}
constexpr HazardFlags operator&(HazardFlags a, HazardFlags b)
{
  return static_cast<HazardFlags>(static_cast<uint32_t>(a) & static_cast<uint32_t>(b));
}
constexpr HazardFlags& operator|=(HazardFlags& a, HazardFlags b)
{
  a = a | b;
  return a;
}
constexpr bool any(HazardFlags f)
{
  return static_cast<uint32_t>(f) != 0;
}

// ---------------------------------------------------------------------------
// ResourceBarrier (special path): explicit per-resource layout / queue
// ownership / present / aliasing. This is a first-class verb, NOT a deprecated
// fallback. The StageBarrier covers the common producer->consumer cases; this
// covers cases that truly need an explicit resource state or queue transfer.
// ---------------------------------------------------------------------------
struct TextureSubresourceRange
{
  TextureAspect aspect{TextureAspect::color};
  uint32_t      baseMipLevel{0};
  uint32_t      levelCount{1};
  uint32_t      baseArrayLayer{0};
  uint32_t      layerCount{1};
};

struct TextureBarrier
{
  TextureHandle           texture{};
  ResourceState           before{ResourceState::Undefined};
  ResourceState           after{ResourceState::Undefined};
  TextureSubresourceRange range{};
  QueueType               srcQueue{QueueType::graphics};
  QueueType               dstQueue{QueueType::graphics};
};

struct BufferBarrier
{
  BufferHandle  buffer{};
  ResourceState before{ResourceState::Undefined};
  ResourceState after{ResourceState::Undefined};
  uint64_t      offset{0};
  uint64_t      size{0};  // 0 = whole buffer
  QueueType     srcQueue{QueueType::graphics};
  QueueType     dstQueue{QueueType::graphics};
};

}  // namespace demo::rhi
