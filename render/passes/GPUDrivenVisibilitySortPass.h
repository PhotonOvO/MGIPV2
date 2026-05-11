#pragma once

#include "../Pass.h"

namespace demo {

class GPUDrivenRenderer;

class GPUDrivenVisibilitySortPass : public ComputePassNode
{
public:
  explicit GPUDrivenVisibilitySortPass(GPUDrivenRenderer* renderer);
  [[nodiscard]] const char* getName() const override { return "GPUDrivenVisibilitySortPass"; }
  [[nodiscard]] HandleSlice<PassResourceDependency> getDependencies() const override;
  void execute(const PassContext& context) const override;

private:
  GPUDrivenRenderer* m_renderer{nullptr};
};

}  // namespace demo
