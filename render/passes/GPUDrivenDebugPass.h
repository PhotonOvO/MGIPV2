#pragma once

#include "../Pass.h"

namespace demo {

class GPUDrivenRenderer;

class GPUDrivenDebugPass : public RenderPassNode
{
public:
  explicit GPUDrivenDebugPass(GPUDrivenRenderer* renderer);

  [[nodiscard]] const char* getName() const override { return "GPUDrivenDebug"; }
  [[nodiscard]] HandleSlice<PassResourceDependency> getDependencies() const override;
  void execute(const PassContext& context) const override;

private:
  GPUDrivenRenderer* m_renderer{nullptr};
};

}  // namespace demo
