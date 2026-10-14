#pragma once

#include "../CSMShadowCache.h"
#include "../Pass.h"

#include <array>

namespace demo
{
	class GPUDrivenRenderer;

	class GPUDrivenCSMShadowPass : public RenderPassNode
	{
	public:
		explicit GPUDrivenCSMShadowPass(GPUDrivenRenderer* renderer);

		[[nodiscard]] const char* getName() const override { return "GPUDrivenCSMShadow"; }
		[[nodiscard]] HandleSlice<PassResourceDependency> getDependencies() const override;
		void execute(const PassContext& context) const override;

	private:
		GPUDrivenRenderer* m_renderer{nullptr};
		mutable CSMShadowCache m_cache{};
		mutable CSMShadowCacheIdentity m_commandWarmupIdentity{};
		mutable std::array<uint8_t, 64> m_commandWarmupPassCounts{};
	};
} // namespace demo
