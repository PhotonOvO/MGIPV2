#pragma once

#include "../Pass.h"

namespace demo {

class GPUDrivenRenderer;

class GPUDrivenLightCullingPass : public ComputePassNode
{
public:
  explicit GPUDrivenLightCullingPass(GPUDrivenRenderer* renderer);

  [[nodiscard]] const char* getName() const override { return "GPUDrivenLightCulling"; }
  [[nodiscard]] HandleSlice<PassResourceDependency> getDependencies() const override;
  void execute(const PassContext& context) const override;

private:
  GPUDrivenRenderer* m_renderer{nullptr};
};

}  // namespace demo
