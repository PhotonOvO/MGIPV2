#pragma once

#include "../rhi/RHIBackend.h"
#include "../rhi/RHITypes.h"

#include <algorithm>
#include <cstdint>
#include <limits>
#include <optional>

namespace demo::ui
{
  struct TextureHandle
  {
    uint32_t index{0};
    uint32_t generation{0};

    [[nodiscard]] constexpr bool isNull() const noexcept { return index == 0u; }
  };

  struct Float2
  {
    float x{0.0f};
    float y{0.0f};
  };

  struct Float4
  {
    float x{0.0f};
    float y{0.0f};
    float z{0.0f};
    float w{0.0f};
  };

  struct Projection
  {
    float scaleX{0.0f};
    float scaleY{0.0f};
    float translateX{0.0f};
    float translateY{0.0f};
  };

  [[nodiscard]] constexpr uint64_t encodeTextureId(TextureHandle handle) noexcept
  {
    return (static_cast<uint64_t>(handle.generation) << 32u) | handle.index;
  }

  [[nodiscard]] constexpr TextureHandle decodeTextureId(uint64_t textureId) noexcept
  {
    return {
      static_cast<uint32_t>(textureId & 0xFFFFFFFFull),
      static_cast<uint32_t>(textureId >> 32u),
    };
  }

  [[nodiscard]] constexpr Projection makeProjection(
    Float2 displayPos,
    Float2 displaySize,
    rhi::BackendType backend) noexcept
  {
    const float scaleX = displaySize.x > 0.0f ? 2.0f / displaySize.x : 0.0f;
    const float scaleYMagnitude = displaySize.y > 0.0f ? 2.0f / displaySize.y : 0.0f;
    const bool isVulkan = backend == rhi::BackendType::vulkan;
    const float scaleY = isVulkan ? scaleYMagnitude : -scaleYMagnitude;
    return {
      scaleX,
      scaleY,
      -1.0f - displayPos.x * scaleX,
      isVulkan
        ? -1.0f - displayPos.y * scaleY
        : 1.0f - displayPos.y * scaleY,
    };
  }

  [[nodiscard]] constexpr Float2 makeFramebufferScale(
    Float2 displaySize,
    rhi::Extent2D targetExtent) noexcept
  {
    return {
      displaySize.x > 0.0f
        ? static_cast<float>(targetExtent.width) / displaySize.x
        : 0.0f,
      displaySize.y > 0.0f
        ? static_cast<float>(targetExtent.height) / displaySize.y
        : 0.0f,
    };
  }

  [[nodiscard]] inline std::optional<rhi::Rect2D> makeScissor(
    Float4 clipRect,
    Float2 displayPos,
    Float2 framebufferScale,
    uint32_t framebufferWidth,
    uint32_t framebufferHeight) noexcept
  {
    const float clipMinX = (clipRect.x - displayPos.x) * framebufferScale.x;
    const float clipMinY = (clipRect.y - displayPos.y) * framebufferScale.y;
    const float clipMaxX = (clipRect.z - displayPos.x) * framebufferScale.x;
    const float clipMaxY = (clipRect.w - displayPos.y) * framebufferScale.y;

    const float clampedMinX = std::clamp(clipMinX, 0.0f, static_cast<float>(framebufferWidth));
    const float clampedMinY = std::clamp(clipMinY, 0.0f, static_cast<float>(framebufferHeight));
    const float clampedMaxX = std::clamp(clipMaxX, 0.0f, static_cast<float>(framebufferWidth));
    const float clampedMaxY = std::clamp(clipMaxY, 0.0f, static_cast<float>(framebufferHeight));
    if (clampedMaxX <= clampedMinX || clampedMaxY <= clampedMinY)
    {
      return std::nullopt;
    }

    const int32_t minX = static_cast<int32_t>(clampedMinX);
    const int32_t minY = static_cast<int32_t>(clampedMinY);
    const uint32_t maxX = static_cast<uint32_t>(clampedMaxX);
    const uint32_t maxY = static_cast<uint32_t>(clampedMaxY);
    return rhi::Rect2D{
      .offset = {minX, minY},
      .extent = {
        maxX - static_cast<uint32_t>(minX),
        maxY - static_cast<uint32_t>(minY),
      },
    };
  }

  [[nodiscard]] constexpr uint64_t nextBufferCapacity(uint64_t current, uint64_t required) noexcept
  {
    if (required <= current)
    {
      return current;
    }

    constexpr uint64_t minimumCapacity = 256u;
    uint64_t capacity = std::max(current, minimumCapacity);
    while (capacity < required && capacity <= std::numeric_limits<uint64_t>::max() / 2u)
    {
      capacity *= 2u;
    }
    return std::max(capacity, required);
  }
} // namespace demo::ui
