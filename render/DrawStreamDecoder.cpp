#include "DrawStreamDecoder.h"

namespace demo {

bool DrawStreamDecoder::decode(const DrawStream& stream, std::vector<DecodedDraw>& outDraws) const  // Debug/compat only.
{
  // Debug/compat path only. Hot recording should use forEachDecodedDraw visitor decode.
  outDraws.clear();
  if(!forEachDecodedDraw(stream, [&outDraws](const DecodedDraw& decodedDraw)
  {
    outDraws.push_back(decodedDraw);
  }))
  {
    outDraws.clear();
    return false;
  }

  return true;
}

bool DrawStreamDecoder::decodeToDrawPackets(const DrawStream& stream, std::vector<DrawPacket>& outPackets) const  // Debug/compat only.
{
  // Debug/compat path only. Main draw recording must not materialize DrawPacket arrays.
  outPackets.clear();
  if(!forEachDecodedDraw(stream, [&outPackets](const DecodedDraw& decodedDraw)
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
    // Indexed draw support
    packet.isIndexed          = decodedDraw.isIndexed;
    packet.indexCount         = decodedDraw.indexCount;
    packet.firstIndex         = decodedDraw.firstIndex;
    packet.vertexOffsetIndexed = decodedDraw.vertexOffsetIndexed;
    packet.firstInstance      = decodedDraw.firstInstance;
    outPackets.push_back(packet);
  }))
  {
    outPackets.clear();
    return false;
  }

  return true;
}

}  // namespace demo
