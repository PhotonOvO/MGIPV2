#pragma once

#include <cstdint>
#include <limits>

namespace demo::rhi {

using ResourceIndex = uint32_t;

inline constexpr ResourceIndex kInvalidResourceIndex = std::numeric_limits<ResourceIndex>::max();

[[nodiscard]] inline constexpr bool isValidResourceIndex(ResourceIndex index)
{
  return index != kInvalidResourceIndex;
}

[[nodiscard]] inline constexpr bool isInvalidResourceIndex(ResourceIndex index)
{
  return !isValidResourceIndex(index);
}

enum class BindlessResourceType : uint8_t
{
  sampler = 0,
  sampledTexture,
  storageTexture,
  uniformBuffer,
  storageBuffer,
  uniformTexelBuffer,
  storageTexelBuffer,
};

enum class ResourceVisibility : uint32_t
{
  none        = 0,
  vertex      = 1u << 0u,
  fragment    = 1u << 1u,
  compute     = 1u << 2u,
  allGraphics = vertex | fragment,
  all         = allGraphics | compute,
};

constexpr ResourceVisibility operator|(ResourceVisibility lhs, ResourceVisibility rhs)
{
  return static_cast<ResourceVisibility>(static_cast<uint32_t>(lhs) | static_cast<uint32_t>(rhs));
}

constexpr bool any(ResourceVisibility visibility)
{
  return static_cast<uint32_t>(visibility) != 0;
}

enum class BindlessUpdateFlags : uint32_t
{
  none                = 0,
  immediateVisibility = 1u << 0u,
  deferredVisibility  = 1u << 1u,
  keepExisting        = 1u << 2u,
};

constexpr BindlessUpdateFlags operator|(BindlessUpdateFlags lhs, BindlessUpdateFlags rhs)
{
  return static_cast<BindlessUpdateFlags>(static_cast<uint32_t>(lhs) | static_cast<uint32_t>(rhs));
}

constexpr bool any(BindlessUpdateFlags flags)
{
  return static_cast<uint32_t>(flags) != 0;
}

}  // namespace demo::rhi
