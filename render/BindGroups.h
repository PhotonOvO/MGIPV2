#pragma once

#include "../common/Handles.h"
#include "../rhi/RHIBindlessTypes.h"
#include "../rhi/RHIDescriptor.h"

#include <cstdint>

namespace demo {

enum class BindGroupSetSlot : uint32_t
{
  passGlobals    = 0,
  material       = 1,
  drawDynamic    = 2,
  shaderSpecific = 3,
};

inline constexpr uint32_t           kBindGroupSetCount             = 4;
inline constexpr rhi::ResourceIndex kMaterialBindlessTexturesIndex = 0;
inline constexpr rhi::ResourceIndex kSceneBindlessInfoIndex        = 0;

static_assert(static_cast<uint32_t>(BindGroupSetSlot::passGlobals) == 0, "BindGroupSetSlot::passGlobals must stay set 0");
static_assert(static_cast<uint32_t>(BindGroupSetSlot::material) == 1, "BindGroupSetSlot::material must stay set 1");
static_assert(static_cast<uint32_t>(BindGroupSetSlot::drawDynamic) == 2, "BindGroupSetSlot::drawDynamic must stay set 2");
static_assert(static_cast<uint32_t>(BindGroupSetSlot::shaderSpecific) == 3, "BindGroupSetSlot::shaderSpecific must stay set 3");

[[nodiscard]] inline constexpr bool isStableBindGroupSetSlot(BindGroupSetSlot slot)
{
  const uint32_t index = static_cast<uint32_t>(slot);
  return index < kBindGroupSetCount;
}

struct BindGroupDesc
{
  BindGroupSetSlot      slot{BindGroupSetSlot::shaderSpecific};
  rhi::BindTableLayout* layout{nullptr};
  rhi::BindTable*       table{nullptr};
  rhi::ResourceIndex    primaryLogicalIndex{rhi::kInvalidResourceIndex};
  const char*           debugName{"bind-group"};
};

struct BindGroupResource
{
  BindGroupDesc desc{};
};

}  // namespace demo
