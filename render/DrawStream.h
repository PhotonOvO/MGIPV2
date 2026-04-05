#pragma once

#include "../common/Handles.h"
#include "../rhi/RHIBindlessTypes.h"

#include <cstdint>
#include <vector>

namespace demo {

enum class StreamEntryType : uint8_t
{
  setPipeline      = 0,
  setMaterial      = 1,
  setMesh          = 2,
  setDynamicBuffer = 3,
  setDynamicOffset = 4,
  draw             = 5,
  drawIndexed      = 6,
};

inline constexpr uint32_t                 kDrawStreamInvalidDynamicOffset = 0xFFFFFFFFu;
inline constexpr demo::rhi::ResourceIndex kDrawStreamInvalidResourceIndex = demo::rhi::kInvalidResourceIndex;
inline constexpr uint32_t                 kDrawStreamDirtyPipeline        = 1u << 0u;
inline constexpr uint32_t                 kDrawStreamDirtyMaterial        = 1u << 1u;
inline constexpr uint32_t                 kDrawStreamDirtyMesh            = 1u << 2u;
inline constexpr uint32_t                 kDrawStreamDirtyDynamicBuffer   = 1u << 3u;
inline constexpr uint32_t                 kDrawStreamDirtyDynamicOffset   = 1u << 4u;

struct StreamEntry
{
  StreamEntryType type{StreamEntryType::draw};
  union
  {
    PipelineHandle           pipeline;
    demo::rhi::ResourceIndex materialIndex;
    MeshHandle               mesh;
    demo::rhi::ResourceIndex dynamicBufferIndex;
    uint32_t                 dynamicOffset;
    struct
    {
      uint32_t dirtyMask;
      uint32_t vertexOffset;
      uint32_t vertexCount;
      uint32_t instanceCount;
    } draw;
    struct
    {
      uint32_t dirtyMask;
      uint32_t indexCount;
      uint32_t instanceCount;
      uint32_t firstIndex;
      int32_t  vertexOffset;
      uint32_t firstInstance;
    } drawIndexed;
  } payload{};
};

using DrawStream = std::vector<StreamEntry>;

}  // namespace demo
