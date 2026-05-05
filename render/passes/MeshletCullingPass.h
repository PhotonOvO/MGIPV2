#pragma once

#include "../Pass.h"

namespace demo {

class GPUDrivenRenderer;

class MeshletCullingPass : public ComputePassNode
{
public:
  explicit MeshletCullingPass(GPUDrivenRenderer* renderer);
  [[nodiscard]] const char* getName() const override { return "MeshletCullingPass"; }
  [[nodiscard]] HandleSlice<PassResourceDependency> getDependencies() const override;
  void execute(const PassContext& context) const override;

private:
  GPUDrivenRenderer* m_renderer{nullptr};
};

}  // namespace demo
