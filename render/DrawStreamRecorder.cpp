#include "DrawStreamRecorder.h"

namespace demo {

bool DrawStreamRecorder::record(const DrawStream& stream, rhi::RenderEncoder& encoder, Resolver& resolver) const
{
  DrawStreamDecoder decoder;
  bool resolverValid = true;
  const bool streamValid = decoder.forEachDecodedDraw(stream, [&encoder, &resolver, &resolverValid](const DrawStreamDecoder::DecodedDraw& draw) {
    if(!resolverValid)
    {
      return;
    }
    if(!resolver.bindState(encoder, draw.state))
    {
      resolverValid = false;
      return;
    }
    if(draw.isIndexed)
    {
      resolverValid = resolver.drawIndexed(encoder, draw);
    }
    else
    {
      resolverValid = resolver.draw(encoder, draw);
    }
  });
  return streamValid && resolverValid;
}

void DrawStreamRecorder::recordIndexedIndirectCount(rhi::RenderEncoder& encoder, const IndexedIndirectCountRecordDesc& desc)
{
  encoder.drawIndexedIndirectCount(rhi::DrawIndirectCountDesc{
      .argsBuffer        = desc.argsBuffer,
      .argsOffset        = desc.argsOffset,
      .countBuffer       = desc.countBuffer,
      .countBufferOffset = desc.countBufferOffset,
      .maxDrawCount      = desc.maxDrawCount,
      .stride            = desc.stride,
  });
}

void DrawStreamRecorder::recordIndexedIndirect(rhi::RenderEncoder& encoder, const IndexedIndirectRecordDesc& desc)
{
  encoder.drawIndexedIndirect(rhi::DrawIndirectDesc{
      .argsBuffer = desc.argsBuffer,
      .offset     = desc.offset,
      .drawCount  = desc.drawCount,
      .stride     = desc.stride,
  });
}

}  // namespace demo
