#pragma once

#include "../Pass.h"

namespace demo {

class GPUDrivenRenderer;

class GPUDrivenSkyPass : public RenderPassNode
{
public:
  explicit GPUDrivenSkyPass(GPUDrivenRenderer* renderer);
  ~GPUDrivenSkyPass() override = default;

  [[nodiscard]] const char* getName() const override { return "GPUDrivenSkyPass"; }
  [[nodiscard]] HandleSlice<PassResourceDependency> getDependencies() const override;
  void execute(const PassContext& context) const override;

private:
  GPUDrivenRenderer* m_renderer{nullptr};
};

}  // namespace demo
