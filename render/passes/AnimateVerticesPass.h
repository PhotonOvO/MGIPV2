#pragma once

#include "../Pass.h"

// Forward declare Renderer within the demo namespace
namespace demo {
class Renderer;
}

namespace demo {

class AnimateVerticesPass : public ComputePassNode
{
public:
  explicit AnimateVerticesPass(Renderer* renderer);

  // PassNode interface
  [[nodiscard]] const char*                         getName() const override { return "AnimateVerticesPass"; }
  [[nodiscard]] HandleSlice<PassResourceDependency> getDependencies() const override;
  void                                              execute(const PassContext& context) const override;

private:
  Renderer* m_renderer{nullptr};
};

}  // namespace demo
