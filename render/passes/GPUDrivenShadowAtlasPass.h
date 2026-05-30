#pragma once

#include "../Pass.h"

namespace demo {

class GPUDrivenRenderer;

class GPUDrivenShadowAtlasPass final : public RenderPassNode
{
public:
  explicit GPUDrivenShadowAtlasPass(GPUDrivenRenderer* renderer);

  [[nodiscard]] const char* getName() const override { return "GPUDrivenShadowAtlas"; }
  [[nodiscard]] HandleSlice<PassResourceDependency> getDependencies() const override;
  void execute(const PassContext& context) const override;

private:
  GPUDrivenRenderer* m_renderer{nullptr};
};

}  // namespace demo
