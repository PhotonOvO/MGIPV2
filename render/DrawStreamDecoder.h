#pragma once

#include "DrawStream.h"
#include "DrawPacket.h"
#include "../rhi/RHIBindlessTypes.h"

#include <cstdint>
#include <vector>

namespace demo {

class DrawStreamDecoder
{
public:
  struct State
  {
    PipelineHandle           pipeline{};
    demo::rhi::ResourceIndex materialIndex{kDrawStreamInvalidResourceIndex};
    MeshHandle               mesh{};
    demo::rhi::ResourceIndex dynamicBufferIndex{kDrawStreamInvalidResourceIndex};
    uint32_t                 dynamicOffset{kDrawStreamInvalidDynamicOffset};
  };

  struct DecodedDraw
  {
    State    state{};
    uint32_t vertexOffset{0};
    uint32_t vertexCount{0};
    uint32_t instanceCount{0};
    // Indexed draw parameters
    bool     isIndexed{false};
    uint32_t indexCount{0};
    uint32_t firstIndex{0};
    int32_t  vertexOffsetIndexed{0};
    uint32_t firstInstance{0};
  };

  [[nodiscard]] bool decode(const DrawStream& stream, std::vector<DecodedDraw>& outDraws) const;
  [[nodiscard]] bool decodeToDrawPackets(const DrawStream& stream, std::vector<DrawPacket>& outPackets) const;
};

}  // namespace demo
