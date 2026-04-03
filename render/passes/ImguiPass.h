#pragma once

#include "../Pass.h"

namespace demo {
class Renderer;

class ImguiPass : public RenderPassNode
{
public:
  explicit ImguiPass(Renderer* renderer);
  [[nodiscard]] const char*                         getName() const override { return "ImguiPass"; }
  [[nodiscard]] HandleSlice<PassResourceDependency> getDependencies() const override;
  void                                              execute(const PassContext& context) const override;

private:
  Renderer* m_renderer{nullptr};
};
}  // namespace demo
