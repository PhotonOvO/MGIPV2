#pragma once

#include "../Pass.h"

namespace demo {

class Renderer;

class DepthPrepass : public RenderPassNode
{
public:
  explicit DepthPrepass(Renderer* renderer);
  ~DepthPrepass() override = default;

  [[nodiscard]] const char*                         getName() const override { return "DepthPrepass"; }
  [[nodiscard]] HandleSlice<PassResourceDependency> getDependencies() const override;
  void                                              execute(const PassContext& context) const override;

private:
  Renderer* m_renderer{nullptr};
};

}  // namespace demo
