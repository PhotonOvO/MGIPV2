#pragma once

#include "../Pass.h"

namespace demo
{
	class GPUDrivenRenderer;

	class GPUDrivenAOPass final : public ComputePassNode
	{
	public:
		explicit GPUDrivenAOPass(GPUDrivenRenderer* renderer);

		[[nodiscard]] const char* getName() const override { return "GPUDrivenAOPass"; }
		[[nodiscard]] HandleSlice<PassResourceDependency> getDependencies() const override;
		void execute(const PassContext& context) const override;

	private:
		GPUDrivenRenderer* m_renderer{nullptr};
	};
} // namespace demo
