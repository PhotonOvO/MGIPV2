#pragma once

#include "RHIHandles.h"
#include "RHIResult.h"

#include <cstdint>
#include <span>

namespace demo::rhi {

enum class ResidencyResourceKind : uint8_t
{
  buffer = 0,
  texture,
  textureView,
  sampler,
  shaderLibrary,
  pipeline,
  argumentTable,
};

struct ResidencyResource
{
  ResidencyResourceKind kind{ResidencyResourceKind::buffer};
  uint32_t index{0};
  uint32_t generation{0};

  [[nodiscard]] constexpr bool isValid() const noexcept
  {
    return index != 0 && generation != 0;
  }

  constexpr bool operator==(const ResidencyResource&) const = default;
};

template <typename Tag>
[[nodiscard]] constexpr ResidencyResource residencyResource(
  ResidencyResourceKind kind, Handle<Tag> handle) noexcept
{
  return ResidencyResource{kind, handle.index, handle.generation};
}

[[nodiscard]] constexpr ResidencyResource residencyResource(BufferHandle handle) noexcept
{
  return residencyResource(ResidencyResourceKind::buffer, handle);
}

[[nodiscard]] constexpr ResidencyResource residencyResource(TextureHandle handle) noexcept
{
  return residencyResource(ResidencyResourceKind::texture, handle);
}

[[nodiscard]] constexpr ResidencyResource residencyResource(TextureViewHandle handle) noexcept
{
  return ResidencyResource{ResidencyResourceKind::textureView, handle.index, handle.generation};
}

struct ResidencySetDesc
{
  uint32_t maxResources{0};
  const char* debugName{nullptr};
};

struct ResidencyUpdateBatch
{
  std::span<const ResidencyResource> add{};
  std::span<const ResidencyResource> remove{};
};

}  // namespace demo::rhi
