#pragma once

#include "../Pass.h"
#include "../../common/Common.h"  // For VkPipelineLayout forward decl via vulkan.h

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
  bool prepareMultiDrawIndirect(const PassContext& context, uint32_t cascadeIndex) const;
  void drawMeshesLegacy(const PassContext& context, VkPipelineLayout pipelineLayout, uint32_t cascadeIndex) const;
  void drawMeshesMultiDrawIndirect(const PassContext& context, VkPipelineLayout pipelineLayout, uint32_t cascadeIndex) const;
};

}  // namespace demo
