#pragma once

#include "../rhi/RHIHandles.h"

#include <cstdint>
#include <type_traits>

namespace demo {

// Use RHI handles directly (unified types)
using BufferHandle    = rhi::BufferHandle;
using TextureHandle   = rhi::TextureHandle;
using PipelineHandle  = rhi::PipelineHandle;
using SamplerHandle   = rhi::SamplerHandle;
using BindGroupHandle = rhi::BindGroupHandle;

// Keep application-specific handles that don't exist in RHI
// Using the original macro pattern
#define DEMO_DECLARE_TYPED_HANDLE(handleName)                                                                          \
  struct handleName                                                                                                    \
  {                                                                                                                    \
    uint32_t index;                                                                                                    \
    uint32_t generation;                                                                                               \
                                                                                                                       \
    constexpr bool        isNull() const noexcept { return index == 0U && generation == 0U; }                          \
    constexpr explicit    operator bool() const noexcept { return !isNull(); }                                         \
    friend constexpr bool operator==(handleName lhs, handleName rhs) noexcept = default;                               \
  };                                                                                                                   \
  static_assert(std::is_trivial_v<handleName>, #handleName " must stay trivial");                                      \
  static_assert(std::is_standard_layout_v<handleName>, #handleName " must stay standard layout");                      \
  inline constexpr handleName kNull##handleName {}

DEMO_DECLARE_TYPED_HANDLE(MeshHandle);
DEMO_DECLARE_TYPED_HANDLE(MaterialHandle);

#undef DEMO_DECLARE_TYPED_HANDLE

// Null handle constants for unified types
inline constexpr rhi::BufferHandle   kNullBufferHandle{};
inline constexpr rhi::TextureHandle  kNullTextureHandle{};
inline constexpr rhi::PipelineHandle kNullPipelineHandle{};
inline constexpr rhi::SamplerHandle  kNullSamplerHandle{};
inline constexpr rhi::BindGroupHandle kNullBindGroupHandle{};

}  // namespace demo