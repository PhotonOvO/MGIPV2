#pragma once

// DDGI probe irradiance/depth update + border update pass (Wave D2-3).
//
// Four compute pipelines consuming the per-ray radiance scratch written by
// DDGIRayTracePass (D2-2):
//   1. IrradianceProbeUpdate — 8x8 octahedral texels per probe, weighted ray
//      accumulation + hysteresis blend + pow(x, 1/ddgiGamma) storage encode;
//   2. DepthProbeUpdate — 16x16 texels, pow(dot, depthSharpness) weights,
//      stores mean depth / mean depth^2 for the Chebyshev test (D3-1);
//   3/4. Irradiance/Depth BorderUpdate — octahedral wrap copy of interior
//      edge texels into each tile's 1px border ring.
//
// Wiring this wave: reads the HISTORY atlases, writes the CURRENT atlases
// (DDGIProbeVolume double buffer); the end-of-frame swap is enabled in D4-1.
// The per-frame ray rotation is recomputed via
// DDGIRayTracePass::makeRayRotation from the monotonic temporal frame counter
// (DDGI hard constraint 4 — never the frames-in-flight ring index), so the
// update kernels reconstruct the exact directions that were traced.
//
// All resources go through rhi:: only — no native graphics types (DDGI hard
// constraint 1). The whole pass is gated on DDGIConfig::enabled (default
// false), so default rendering is unchanged.

#include "../Pass.h"
#include "../../rhi/RHIDevice.h"

#include <glm/glm.hpp>

namespace demo
{
	class GPUDrivenRenderer;

	class DDGIProbeUpdatePass final : public ComputePassNode
	{
	public:
		// Octahedral texel sizes baked into the shaders' [numthreads] (8x8 and
		// 16x16 workgroups = one probe tile each); initResources refuses a
		// config that disagrees.
		static constexpr uint32_t kIrradianceTexelSize = 8u;
		static constexpr uint32_t kDepthTexelSize = 16u;

		explicit DDGIProbeUpdatePass(GPUDrivenRenderer* renderer);

		// Requires DDGIProbeVolume::init to have run; only called when
		// DDGIConfig::enabled is true. All argument tables are static (push
		// constants carry the per-frame values), so no frame-count plumbing.
		void initResources(rhi::Device& device);
		void shutdownResources();

		[[nodiscard]] const char* getName() const override { return "DDGIProbeUpdatePass"; }
		[[nodiscard]] HandleSlice<PassResourceDependency> getDependencies() const override;
		void execute(const PassContext& context) const override;

	private:
		GPUDrivenRenderer* m_renderer{nullptr};
		rhi::Device* m_device{nullptr};

		// Views owned by this pass over DDGIProbeVolume textures. NOTE: bound
		// statically to the D2-1 current/history handles — when D4-1 enables
		// swapAtlases, the tables must be rebuilt (or rebound per parity).
		rhi::TextureViewHandle m_irradianceCurrentView{};
		rhi::TextureViewHandle m_irradianceHistoryView{};
		rhi::TextureViewHandle m_depthCurrentView{};
		rhi::TextureViewHandle m_depthHistoryView{};
		rhi::TextureViewHandle m_radianceView{};

		// Shared by both update pipelines: 0 = out atlas, 1 = history atlas,
		// 2 = radiance scratch (all storage textures).
		rhi::ArgumentLayoutHandle m_updateLayout{};
		// Border pipelines: 0 = atlas (storage, read + write).
		rhi::ArgumentLayoutHandle m_borderLayout{};

		rhi::PipelineHandle m_irradianceUpdatePipeline{};
		rhi::PipelineHandle m_depthUpdatePipeline{};
		rhi::PipelineHandle m_irradianceBorderPipeline{};
		rhi::PipelineHandle m_depthBorderPipeline{};

		// Static tables: views never change after init (see note above).
		rhi::ArgumentTableHandle m_irradianceUpdateTable{};
		rhi::ArgumentTableHandle m_depthUpdateTable{};
		rhi::ArgumentTableHandle m_irradianceBorderTable{};
		rhi::ArgumentTableHandle m_depthBorderTable{};

		// Lazily transitions the four atlas textures Undefined -> General on
		// the first recorded execute; doubling as the "atlases hold no valid
		// history yet" latch for the first-frame direct write.
		mutable bool m_atlasLayoutInitialized{false};
	};
} // namespace demo
