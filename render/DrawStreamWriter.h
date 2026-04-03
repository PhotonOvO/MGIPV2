#pragma once

#include "DrawStream.h"
#include "../rhi/RHIBindlessTypes.h"

namespace demo {

class DrawStreamWriter
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

  DrawStreamWriter() = default;

  void clear();

  void setPipeline(PipelineHandle pipeline);
  void setMaterialIndex(demo::rhi::ResourceIndex materialIndex);
  void setMesh(MeshHandle mesh);
  void setDynamicBufferIndex(demo::rhi::ResourceIndex dynamicBufferIndex);
  void setDynamicOffset(uint32_t dynamicOffset);
  void draw(uint32_t vertexOffset, uint32_t vertexCount, uint32_t instanceCount);

  [[nodiscard]] const State&      state() const;
  [[nodiscard]] const DrawStream& entries() const;

private:
  [[nodiscard]] uint32_t emitCurrentState();

  State      m_state{};
  State      m_lastEmittedState{};
  bool       m_hasEmittedDraw{false};
  DrawStream m_entries;
};

}  // namespace demo
