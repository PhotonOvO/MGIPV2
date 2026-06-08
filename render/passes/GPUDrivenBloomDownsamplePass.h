#pragma once

#include "../Pass.h"

namespace demo
{
	class GPUDrivenRenderer;

	class GPUDrivenBloomDownsamplePass : public RenderPassNode
	{
	public:
		explicit GPUDrivenBloomDownsamplePass(GPUDrivenRenderer* renderer);
		[[nodiscard]] const char* getName() const override { return "GPUDrivenBloomDownsample"; }
		[[nodiscard]] HandleSlice<PassResourceDependency> getDependencies() const override;
		void execute(const PassContext& context) const override;

	private:
		GPUDrivenRenderer* m_renderer{nullptr};
	};
} // namespace demo
