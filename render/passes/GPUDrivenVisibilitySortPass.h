#pragma once

#include "../Pass.h"

namespace demo
{
	class GPUDrivenRenderer;

	// Records the bitonic visibility-sort compute dispatches for the current frame.
	// Shared by GPUDrivenVisibilitySortPass (and the legacy MeshletCullingPass) so the
	// recording logic lives in the pass layer rather than on the renderer.
	void recordVisibilitySort(const PassContext& context, GPUDrivenRenderer& renderer);

	class GPUDrivenVisibilitySortPass : public ComputePassNode
	{
	public:
		explicit GPUDrivenVisibilitySortPass(GPUDrivenRenderer* renderer);
		[[nodiscard]] const char* getName() const override { return "GPUDrivenVisibilitySortPass"; }
		[[nodiscard]] HandleSlice<PassResourceDependency> getDependencies() const override;
		void execute(const PassContext& context) const override;

	private:
		GPUDrivenRenderer* m_renderer{nullptr};
	};
} // namespace demo
