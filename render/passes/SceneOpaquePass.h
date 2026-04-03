#pragma once

#include "../Pass.h"

namespace demo {
class Renderer;

class SceneOpaquePass : public RenderPassNode
{
public:
  explicit SceneOpaquePass(Renderer* renderer);
  [[nodiscard]] const char*                         getName() const override { return "SceneOpaquePass"; }
  [[nodiscard]] HandleSlice<PassResourceDependency> getDependencies() const override;
  void                                              execute(const PassContext& context) const override;

private:
  Renderer* m_renderer{nullptr};
};
}  // namespace demo
