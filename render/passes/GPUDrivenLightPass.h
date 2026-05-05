#pragma once

#include "../Pass.h"

namespace demo {

class GPUDrivenRenderer;

class GPUDrivenLightPass : public RenderPassNode
{
public:
  explicit GPUDrivenLightPass(GPUDrivenRenderer* renderer);
  [[nodiscard]] const char* getName() const override { return "GPUDrivenLightPass"; }
  [[nodiscard]] HandleSlice<PassResourceDependency> getDependencies() const override;
  void execute(const PassContext& context) const override;

private:
  GPUDrivenRenderer* m_renderer{nullptr};
};

}  // namespace demo
