#pragma once

#include "../common/Handles.h"
#include "../rhi/RHIBindlessTypes.h"

#include <cstdint>

namespace demo {

struct DrawPacket
{
  PipelineHandle           pipeline{};
  demo::rhi::ResourceIndex materialIndex{demo::rhi::kInvalidResourceIndex};
  MeshHandle               mesh{};
  demo::rhi::ResourceIndex dynamicBufferIndex{demo::rhi::kInvalidResourceIndex};
  uint32_t                 dynamicOffset{0};
  uint32_t                 vertexOffset{0};
  uint32_t                 vertexCount{0};
  uint32_t                 instanceCount{0};
  // Indexed draw support
  bool                     isIndexed{false};
  uint32_t                 indexCount{0};
  uint32_t                 firstIndex{0};
  int32_t                  vertexOffsetIndexed{0};
  uint32_t                 firstInstance{0};
};

}  // namespace demo
