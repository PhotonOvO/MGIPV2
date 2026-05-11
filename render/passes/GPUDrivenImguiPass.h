#pragma once

#include "../Pass.h"

namespace demo {

class GPUDrivenRenderer;

class GPUDrivenImguiPass : public RenderPassNode
{
public:
  explicit GPUDrivenImguiPass(GPUDrivenRenderer* renderer);

  [[nodiscard]] const char* getName() const override { return "GPUDrivenImgui"; }
  [[nodiscard]] HandleSlice<PassResourceDependency> getDependencies() const override;
  void execute(const PassContext& context) const override;

private:
  GPUDrivenRenderer* m_renderer{nullptr};
};

}  // namespace demo
