#pragma once

#include "../Pass.h"

namespace demo {

class Renderer;

class DebugPass : public RenderPassNode
{
public:
  explicit DebugPass(Renderer* renderer);
  ~DebugPass() override = default;

  [[nodiscard]] const char* getName() const override { return "DebugPass"; }
  [[nodiscard]] HandleSlice<PassResourceDependency> getDependencies() const override;
  void execute(const PassContext& context) const override;

private:
  Renderer* m_renderer{nullptr};
};

}  // namespace demo
