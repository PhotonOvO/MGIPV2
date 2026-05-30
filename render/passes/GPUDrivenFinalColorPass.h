#pragma once

#include "../Pass.h"

namespace demo {

class GPUDrivenRenderer;

class GPUDrivenFinalColorPass : public RenderPassNode
{
public:
  explicit GPUDrivenFinalColorPass(GPUDrivenRenderer* renderer);
  [[nodiscard]] const char* getName() const override { return "GPUDrivenFinalColor"; }
  [[nodiscard]] HandleSlice<PassResourceDependency> getDependencies() const override;
  void execute(const PassContext& context) const override;

private:
  GPUDrivenRenderer* m_renderer{nullptr};
};

}  // namespace demo
