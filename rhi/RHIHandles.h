#pragma once

#include <cstdint>

namespace demo::rhi {

template <typename Tag>
struct Handle
{
  uint32_t index{0};
  uint32_t generation{0};

  [[nodiscard]] constexpr bool isValid() const noexcept { return index != 0 || generation != 0; }
  [[nodiscard]] constexpr bool isNull() const noexcept { return index == 0 && generation == 0; }
  constexpr explicit operator bool() const noexcept { return isValid(); }

  constexpr bool operator==(const Handle&) const = default;
};

struct BufferTag;
struct TextureTag;
struct PipelineTag;
struct SamplerTag;
struct TextureViewTag;
struct BufferViewTag;
struct ResourceViewTag;
struct BindLayoutTag;
struct BindGroupLayoutTag;
struct BindTableTag;
struct BindGroupTag;
struct SwapchainTag;
struct TimelineTag;
struct FenceTag;

using BufferHandle       = Handle<BufferTag>;
using TextureHandle      = Handle<TextureTag>;
using PipelineHandle     = Handle<PipelineTag>;
using SamplerHandle      = Handle<SamplerTag>;

// TextureViewHandle with native pointer support for wrapping native image views
struct TextureViewHandle : Handle<TextureViewTag>
{
  // Create from native pointer (encodes 64-bit pointer into index+generation)
  [[nodiscard]] static TextureViewHandle fromNativePtr(void* nativePtr) noexcept
  {
    TextureViewHandle h;
    const uint64_t value = reinterpret_cast<uint64_t>(nativePtr);
    h.index = static_cast<uint32_t>(value & 0xFFFFFFFFULL);
    h.generation = static_cast<uint32_t>((value >> 32) & 0xFFFFFFFFULL);
    return h;
  }

  // Get native pointer from handle
  [[nodiscard]] void* toNativePtr() const noexcept
  {
    const uint64_t value = (static_cast<uint64_t>(generation) << 32) | index;
    return reinterpret_cast<void*>(value);
  }

  // Convenience for typed access
  template <typename T>
  [[nodiscard]] static TextureViewHandle fromNative(T* ptr) noexcept
  {
    return fromNativePtr(static_cast<void*>(ptr));
  }

  template <typename T>
  [[nodiscard]] T* as() const noexcept
  {
    return static_cast<T*>(toNativePtr());
  }
};

using BufferViewHandle   = Handle<BufferViewTag>;
using ResourceViewHandle = Handle<ResourceViewTag>;
using BindLayoutHandle   = Handle<BindLayoutTag>;
using BindGroupLayoutHandle = Handle<BindGroupLayoutTag>;
using BindTableHandle    = Handle<BindTableTag>;
using BindGroupHandle    = Handle<BindGroupTag>;
using SwapchainHandle    = Handle<SwapchainTag>;
using TimelineHandle     = Handle<TimelineTag>;
using FenceHandle        = Handle<FenceTag>;

}  // namespace demo::rhi
