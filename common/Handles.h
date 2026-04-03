#pragma once

#include <cstdint>
#include <type_traits>

namespace demo {

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

DEMO_DECLARE_TYPED_HANDLE(BufferHandle);
DEMO_DECLARE_TYPED_HANDLE(TextureHandle);
DEMO_DECLARE_TYPED_HANDLE(SamplerHandle);
DEMO_DECLARE_TYPED_HANDLE(ShaderHandle);
DEMO_DECLARE_TYPED_HANDLE(PipelineHandle);
DEMO_DECLARE_TYPED_HANDLE(BindGroupHandle);
DEMO_DECLARE_TYPED_HANDLE(MeshHandle);
DEMO_DECLARE_TYPED_HANDLE(MaterialHandle);

#undef DEMO_DECLARE_TYPED_HANDLE

}  // namespace demo
