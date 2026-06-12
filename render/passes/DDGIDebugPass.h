#pragma once

// DDGI probe visualization debug pass (Wave D3-2).
//
// Instanced procedural UV-sphere draw (one instance per probe, no vertex or
// index buffer) onto the scene HDR color target with depth test on / depth
// write off, showing each probe's directional irradiance sampled from the
// octahedral irradiance atlas (LuxGI ProbeVisualization approach).
//
// Doubly gated so the default frame is untouched: DDGIConfig::enabled
// (master gate, default false) AND DebugPassOptions::ddgiDebugVisualize
// (ImGui "DDGI Probe Visualize" checkbox, default false).
//
// All resources go through rhi:: only — no native graphics types
// (DDGI hard constraint 1).

#include "../Pass.h"
#include "../../rhi/RHIDevice.h"

#include <vector>

namespace demo
{
	class GPUDrivenRenderer;

	class DDGIDebugPass final : public RenderPassNode
	{
	public:
		// Debug sphere world-space radius (plan: unit sphere, scale 0.1).
		static constexpr float kProbeRadius = 0.1f;

		explicit DDGIDebugPass(GPUDrivenRenderer* renderer);

		// Requires DDGIProbeVolume::init to have run; only called when
		// DDGIConfig::enabled is true.
		void initResources(rhi::Device& device, uint32_t frameCount);
		void shutdownResources();

		[[nodiscard]] const char* getName() const override { return "DDGIDebugPass"; }
		[[nodiscard]] HandleSlice<PassResourceDependency> getDependencies() const override;
		void execute(const PassContext& context) const override;

	private:
		void writeUniforms(uint32_t frameIndex, const PassContext& context) const;

		GPUDrivenRenderer* m_renderer{nullptr};
		rhi::Device* m_device{nullptr};
		uint32_t m_frameCount{0};

		// Sampled view over the probe volume's *current* irradiance atlas
		// (owned by DDGIProbeVolume; the view is owned here).
		rhi::TextureViewHandle m_irradianceView{};
		rhi::SamplerHandle m_sampler{};

		rhi::ArgumentLayoutHandle m_layout{};
		rhi::PipelineHandle m_pipeline{};
		// One table + uniform buffer per frame in flight (same pattern as
		// DDGIRayTracePass): views are static, uniform contents change per frame.
		std::vector<rhi::ArgumentTableHandle> m_tables;
		std::vector<rhi::BufferHandle> m_uniformBuffers;
	};
} // namespace demo
