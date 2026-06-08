#pragma once

#include "../Pass.h"

namespace demo
{
	class GPUDrivenRenderer;

	class GPUDrivenClusteredLightCullingPass : public ComputePassNode
	{
	public:
		explicit GPUDrivenClusteredLightCullingPass(GPUDrivenRenderer* renderer);
		[[nodiscard]] const char* getName() const override { return "GPUDrivenClusteredLightCulling"; }
		[[nodiscard]] HandleSlice<PassResourceDependency> getDependencies() const override;
		void execute(const PassContext& context) const override;

	private:
		GPUDrivenRenderer* m_renderer{nullptr};
	};
} // namespace demo
