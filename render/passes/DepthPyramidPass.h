#pragma once

#include "../Pass.h"

namespace demo {

class Renderer;

class DepthPyramidPass : public ComputePassNode
{
public:
  explicit DepthPyramidPass(Renderer* renderer);
  ~DepthPyramidPass() override = default;

  [[nodiscard]] const char*                         getName() const override { return "DepthPyramidPass"; }
  [[nodiscard]] HandleSlice<PassResourceDependency> getDependencies() const override;
  void                                              execute(const PassContext& context) const override;

private:
  Renderer* m_renderer{nullptr};
};

}  // namespace demo
