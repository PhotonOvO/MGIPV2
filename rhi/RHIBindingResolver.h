#pragma once

#include "RHIHandles.h"
#include "RHITypes.h"

#include <cstdint>

namespace demo::rhi {

// Resolves opaque RHI handles to native backend objects for command recording.
//
// The abstract CommandList interface intentionally speaks only in handles, but a
// backend (e.g. Vulkan) still needs to map a PipelineHandle to a VkPipeline and a
// BindGroupHandle to a VkDescriptorSet at bind time. The owner of the resource
// registries (the renderer) implements this interface and injects it into the
// backend command list, so passes can bind purely through handles.
class BindingResolver
{
public:
  virtual ~BindingResolver() = default;

  // Native pipeline object (e.g. VkPipeline) for the given handle / bind point.
  [[nodiscard]] virtual uint64_t resolvePipeline(PipelineHandle handle, PipelineBindPoint bindPoint) const = 0;

  // Native pipeline layout (e.g. VkPipelineLayout) the pipeline was created with.
  // Used by the command list to bind descriptor sets compatibly.
  [[nodiscard]] virtual uint64_t resolvePipelineLayout(PipelineHandle handle) const = 0;

  // Native descriptor set (e.g. VkDescriptorSet) backing the bind group.
  [[nodiscard]] virtual uint64_t resolveBindGroupDescriptorSet(BindGroupHandle handle) const = 0;
};

}  // namespace demo::rhi
