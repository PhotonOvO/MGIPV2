#pragma once

#include "../Pass.h"

namespace demo {

class GPUDrivenRenderer;

class GPUDrivenBloomPrefilterPass : public RenderPassNode
{
public:
  explicit GPUDrivenBloomPrefilterPass(GPUDrivenRenderer* renderer);
  [[nodiscard]] const char* getName() const override { return "GPUDrivenBloomPrefilter"; }
  [[nodiscard]] HandleSlice<PassResourceDependency> getDependencies() const override;
  void execute(const PassContext& context) const override;

private:
  GPUDrivenRenderer* m_renderer{nullptr};
};

}  // namespace demo
