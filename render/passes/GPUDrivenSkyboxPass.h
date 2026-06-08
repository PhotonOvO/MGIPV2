#pragma once

#include "../Pass.h"

namespace demo
{
	class GPUDrivenRenderer;

	class GPUDrivenSkyboxPass : public RenderPassNode
	{
	public:
		explicit GPUDrivenSkyboxPass(GPUDrivenRenderer* renderer);
		[[nodiscard]] const char* getName() const override { return "GPUDrivenSkybox"; }
		[[nodiscard]] HandleSlice<PassResourceDependency> getDependencies() const override;
		void execute(const PassContext& context) const override;

	private:
		GPUDrivenRenderer* m_renderer{nullptr};
	};
} // namespace demo
