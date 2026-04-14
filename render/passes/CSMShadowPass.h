#pragma once

#include "../Pass.h"

namespace demo {

class Renderer;

class CSMShadowPass : public RenderPassNode
{
public:
  explicit CSMShadowPass(Renderer* renderer);
  ~CSMShadowPass() override = default;

  [[nodiscard]] const char* getName() const override { return "CSMShadowPass"; }
  [[nodiscard]] HandleSlice<PassResourceDependency> getDependencies() const override;
  void execute(const PassContext& context) const override;

private:
  Renderer* m_renderer{nullptr};

  void renderCascadeLayer(const PassContext& context, uint32_t cascadeIndex) const;
  void drawMeshes(const PassContext& context, VkPipelineLayout pipelineLayout) const;
};

}  // namespace demo