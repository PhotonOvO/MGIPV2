#include "DrawStreamDecoder.h"

namespace demo {

bool DrawStreamDecoder::decode(const DrawStream& stream, std::vector<DecodedDraw>& outDraws) const
{
  outDraws.clear();

  State    currentState{};
  bool     hasPipeline{false};
  bool     hasMaterial{false};
  bool     hasMesh{false};
  bool     hasDynamicBuffer{false};
  bool     hasDynamicOffset{false};
  uint32_t pendingDirtyMask{0};

  for(const StreamEntry& entry : stream)
  {
    switch(entry.type)
    {
      case StreamEntryType::setPipeline:
        currentState.pipeline = entry.payload.pipeline;
        hasPipeline           = true;
        pendingDirtyMask |= kDrawStreamDirtyPipeline;
        break;
      case StreamEntryType::setMaterial:
        currentState.materialIndex = entry.payload.materialIndex;
        hasMaterial                = true;
        pendingDirtyMask |= kDrawStreamDirtyMaterial;
        break;
      case StreamEntryType::setMesh:
        currentState.mesh = entry.payload.mesh;
        hasMesh           = true;
        pendingDirtyMask |= kDrawStreamDirtyMesh;
        break;
      case StreamEntryType::setDynamicBuffer:
        currentState.dynamicBufferIndex = entry.payload.dynamicBufferIndex;
        hasDynamicBuffer                = true;
        pendingDirtyMask |= kDrawStreamDirtyDynamicBuffer;
        break;
      case StreamEntryType::setDynamicOffset:
        currentState.dynamicOffset = entry.payload.dynamicOffset;
        hasDynamicOffset           = true;
        pendingDirtyMask |= kDrawStreamDirtyDynamicOffset;
        break;
      case StreamEntryType::draw: {
        if(!(hasPipeline && hasMaterial && hasMesh && hasDynamicBuffer && hasDynamicOffset))
        {
          outDraws.clear();
          return false;
        }
        if(entry.payload.draw.dirtyMask != pendingDirtyMask)
        {
          outDraws.clear();
          return false;
        }

        DecodedDraw decodedDraw{};
        decodedDraw.state         = currentState;
        decodedDraw.vertexOffset  = entry.payload.draw.vertexOffset;
        decodedDraw.vertexCount   = entry.payload.draw.vertexCount;
        decodedDraw.instanceCount = entry.payload.draw.instanceCount;
        outDraws.push_back(decodedDraw);
        pendingDirtyMask = 0;
      }
      break;
    }
  }

  return true;
}

bool DrawStreamDecoder::decodeToDrawPackets(const DrawStream& stream, std::vector<DrawPacket>& outPackets) const
{
  std::vector<DecodedDraw> decodedDraws;
  if(!decode(stream, decodedDraws))
  {
    outPackets.clear();
    return false;
  }

  outPackets.clear();
  outPackets.reserve(decodedDraws.size());
  for(const DecodedDraw& decodedDraw : decodedDraws)
  {
    DrawPacket packet{};
    packet.pipeline           = decodedDraw.state.pipeline;
    packet.materialIndex      = decodedDraw.state.materialIndex;
    packet.mesh               = decodedDraw.state.mesh;
    packet.dynamicBufferIndex = decodedDraw.state.dynamicBufferIndex;
    packet.dynamicOffset      = decodedDraw.state.dynamicOffset;
    packet.vertexOffset       = decodedDraw.vertexOffset;
    packet.vertexCount        = decodedDraw.vertexCount;
    packet.instanceCount      = decodedDraw.instanceCount;
    outPackets.push_back(packet);
  }

  return true;
}

}  // namespace demo
