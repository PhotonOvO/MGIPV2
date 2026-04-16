#pragma once

#include "../Pass.h"

namespace demo {

class Renderer;

class GPUCullingPass : public ComputePassNode
{
public:
  explicit GPUCullingPass(Renderer* renderer);
  ~GPUCullingPass() override = default;

  [[nodiscard]] const char*                         getName() const override { return "GPUCullingPass"; }
  [[nodiscard]] HandleSlice<PassResourceDependency> getDependencies() const override;
  void                                              execute(const PassContext& context) const override;

private:
  Renderer* m_renderer{nullptr};
};

}  // namespace demo
