#pragma once

#include "../Pass.h"

namespace demo {

class GPUDrivenRenderer;

class GPUDrivenForwardPass : public RenderPassNode
{
public:
  explicit GPUDrivenForwardPass(GPUDrivenRenderer* renderer);
  [[nodiscard]] const char* getName() const override { return "GPUDrivenForwardPass"; }
  [[nodiscard]] HandleSlice<PassResourceDependency> getDependencies() const override;
  void execute(const PassContext& context) const override;

private:
  GPUDrivenRenderer* m_renderer{nullptr};
};

}  // namespace demo
