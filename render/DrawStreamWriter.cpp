#include "DrawStreamWriter.h"

namespace demo {

void DrawStreamWriter::clear()
{
  m_state            = State{};
  m_lastEmittedState = State{};
  m_hasEmittedDraw   = false;
  m_entries          = DrawStream{};
}

void DrawStreamWriter::setPipeline(PipelineHandle pipeline)
{
  m_state.pipeline = pipeline;
}

void DrawStreamWriter::setMaterialIndex(demo::rhi::ResourceIndex materialIndex)
{
  m_state.materialIndex = materialIndex;
}

void DrawStreamWriter::setMesh(MeshHandle mesh)
{
  m_state.mesh = mesh;
}

void DrawStreamWriter::setDynamicBufferIndex(demo::rhi::ResourceIndex dynamicBufferIndex)
{
  m_state.dynamicBufferIndex = dynamicBufferIndex;
}

void DrawStreamWriter::setDynamicOffset(uint32_t dynamicOffset)
{
  m_state.dynamicOffset = dynamicOffset;
}

void DrawStreamWriter::draw(uint32_t vertexOffset, uint32_t vertexCount, uint32_t instanceCount)
{
  const uint32_t dirtyMask = emitCurrentState();

  StreamEntry drawEntry{};
  drawEntry.type                       = StreamEntryType::draw;
  drawEntry.payload.draw.dirtyMask     = dirtyMask;
  drawEntry.payload.draw.vertexOffset  = vertexOffset;
  drawEntry.payload.draw.vertexCount   = vertexCount;
  drawEntry.payload.draw.instanceCount = instanceCount;
  m_entries.push_back(drawEntry);
}

const DrawStreamWriter::State& DrawStreamWriter::state() const
{
  return m_state;
}

const DrawStream& DrawStreamWriter::entries() const
{
  return m_entries;
}

uint32_t DrawStreamWriter::emitCurrentState()
{
  StreamEntry entry{};
  uint32_t    dirtyMask{0};

  if(!m_hasEmittedDraw || !(m_state.pipeline == m_lastEmittedState.pipeline))
  {
    entry.type             = StreamEntryType::setPipeline;
    entry.payload.pipeline = m_state.pipeline;
    m_entries.push_back(entry);
    dirtyMask |= kDrawStreamDirtyPipeline;
  }

  if(!m_hasEmittedDraw || m_state.materialIndex != m_lastEmittedState.materialIndex)
  {
    entry.type                  = StreamEntryType::setMaterial;
    entry.payload.materialIndex = m_state.materialIndex;
    m_entries.push_back(entry);
    dirtyMask |= kDrawStreamDirtyMaterial;
  }

  if(!m_hasEmittedDraw || !(m_state.mesh == m_lastEmittedState.mesh))
  {
    entry.type         = StreamEntryType::setMesh;
    entry.payload.mesh = m_state.mesh;
    m_entries.push_back(entry);
    dirtyMask |= kDrawStreamDirtyMesh;
  }

  if(!m_hasEmittedDraw || m_state.dynamicBufferIndex != m_lastEmittedState.dynamicBufferIndex)
  {
    entry.type                       = StreamEntryType::setDynamicBuffer;
    entry.payload.dynamicBufferIndex = m_state.dynamicBufferIndex;
    m_entries.push_back(entry);
    dirtyMask |= kDrawStreamDirtyDynamicBuffer;
  }

  if(!m_hasEmittedDraw || m_state.dynamicOffset != m_lastEmittedState.dynamicOffset)
  {
    entry.type                  = StreamEntryType::setDynamicOffset;
    entry.payload.dynamicOffset = m_state.dynamicOffset;
    m_entries.push_back(entry);
    dirtyMask |= kDrawStreamDirtyDynamicOffset;
  }

  m_lastEmittedState = m_state;
  m_hasEmittedDraw   = true;
  return dirtyMask;
}

}  // namespace demo
