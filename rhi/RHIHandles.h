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
struct ArgumentLayoutTag;
struct ArgumentTableTag;
struct QueryPoolTag;
struct ResidencySetTag;

using BufferHandle       = Handle<BufferTag>;
using TextureHandle      = Handle<TextureTag>;
using PipelineHandle     = Handle<PipelineTag>;
using SamplerHandle      = Handle<SamplerTag>;

// Wave 9: texture views are always real registry handles (created via
// Device::createTextureView / registerExternalTextureView). The legacy native
// pointer-encoding helpers were removed once the last call site migrated.
struct TextureViewHandle : Handle<TextureViewTag>
{
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
using ArgumentLayoutHandle = Handle<ArgumentLayoutTag>;
using ArgumentTableHandle  = Handle<ArgumentTableTag>;
using QueryPoolHandle      = Handle<QueryPoolTag>;
using ResidencySetHandle   = Handle<ResidencySetTag>;

}  // namespace demo::rhi
