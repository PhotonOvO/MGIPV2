#pragma once

#include "../Pass.h"

namespace demo {

class GPUDrivenRenderer;

class GPUDrivenDepthPyramidPass : public ComputePassNode
{
public:
  explicit GPUDrivenDepthPyramidPass(GPUDrivenRenderer* renderer);

  [[nodiscard]] const char* getName() const override { return "GPUDrivenDepthPyramid"; }
  [[nodiscard]] HandleSlice<PassResourceDependency> getDependencies() const override;
  void execute(const PassContext& context) const override;

private:
  GPUDrivenRenderer* m_renderer{nullptr};
};

}  // namespace demo
