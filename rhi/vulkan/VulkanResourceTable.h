#pragma once

#include "../../common/Handles.h"
#include "../../common/HandlePool.h"

#include <cstdint>
#include <unordered_map>
#include <utility>

namespace demo::rhi {
class BindTable;
}

namespace demo::rhi::vulkan {

// Native pipeline objects backing an opaque PipelineHandle.
struct PipelineRecord
{
  uint32_t bindPoint{0};
  uint64_t nativePipeline{0};
  uint32_t specializationVariant{0};
  uint64_t nativeLayout{0};
};

// Backend-owned table mapping opaque RHI handles to native Vulkan objects.
//
// The command list resolves handles by a direct in-layer lookup here, instead of
// up-calling a render-layer resolver interface. The render layer owns an instance
// of this table, registers its pipelines/bind-groups into it, and hands the
// command list a pointer to it for recording.
class VulkanResourceTable
{
public:
  // --- Pipelines (this table owns the handle allocation) ---
  PipelineHandle registerPipeline(uint32_t bindPoint, uint64_t nativePipeline, uint32_t specializationVariant,
                                  uint64_t nativeLayout);
  [[nodiscard]] const PipelineRecord* tryGetPipeline(PipelineHandle handle) const;
  void                                destroyPipeline(PipelineHandle handle);

  template <typename Fn>
  void forEachPipeline(Fn&& fn)
  {
    m_pipelines.forEachActive(std::forward<Fn>(fn));
  }

  // --- Native-object resolution used during command recording ---
  [[nodiscard]] uint64_t resolvePipeline(PipelineHandle handle, uint32_t expectedBindPoint) const;
  [[nodiscard]] uint64_t resolvePipelineLayout(PipelineHandle handle) const;
  [[nodiscard]] uint64_t resolveBindGroupDescriptorSet(BindGroupHandle handle) const;

  // --- Bind-group descriptor-set mirror ---
  // The render layer still owns the bind-group pool; only the native descriptor
  // lookup is mirrored here so the command list can resolve it in-layer. The
  // BindTable pointer is read live at bind time (its descriptor set may change).
  void registerBindGroup(BindGroupHandle handle, BindTable* table);
  void unregisterBindGroup(BindGroupHandle handle);

private:
  HandlePool<PipelineHandle, PipelineRecord> m_pipelines;
  std::unordered_map<uint64_t, BindTable*>   m_bindGroupTables;
};

}  // namespace demo::rhi::vulkan
