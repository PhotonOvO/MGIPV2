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
};

}  // namespace demo
