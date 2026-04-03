#pragma once

#include "../Pass.h"

namespace demo {
class Renderer;

class PresentPass : public RenderPassNode
{
public:
  explicit PresentPass(Renderer* renderer);
  [[nodiscard]] const char*                         getName() const override { return "PresentPass"; }
  [[nodiscard]] HandleSlice<PassResourceDependency> getDependencies() const override;
  void                                              execute(const PassContext& context) const override;

private:
  Renderer* m_renderer{nullptr};
};
}  // namespace demo
