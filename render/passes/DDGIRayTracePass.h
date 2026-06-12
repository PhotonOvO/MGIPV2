#pragma once

// DDGI SDF ray trace pass (GISDFRays, DDGI Wave D2-2).
//
// For every probe of the DDGI volume, traces raysPerProbe spherical-Fibonacci
// rays (rotated by a per-frame random rotation seeded from the monotonically
// increasing temporal frame counter) through the Global SDF volume via
// sphere-marching (coarse-mip-first, cf. shaders/sdf_common.slang), shades
// hits with the simplified model (SDF gradient normal + sun light + SDF
// shadow ray + constant albedo; constant sky on miss) and writes
// radiance + hit distance into the probe volume's radiance scratch texture.
//
// All resources go through rhi:: only — no native graphics types and no ray
// tracing extensions (DDGI hard constraints 1/3). The whole pass is gated on
// DDGIConfig::enabled (default false), so default rendering is unchanged.

#include "../Pass.h"
#include "../../rhi/RHIDevice.h"

#include <glm/glm.hpp>

#include <vector>

namespace demo
{
	class GPUDrivenRenderer;

	class DDGIRayTracePass final : public ComputePassNode
	{
	public:
		// Shader local_size_x: dispatchX = ceil(raysPerProbe / 16), Y = totalProbes.
		static constexpr uint32_t kGroupSizeX = 16u;
		// Sphere-march step budget (plan range 64-128; uniform-adjustable).
		static constexpr uint32_t kDefaultMaxSteps = 96u;
		static constexpr float kDefaultStepScale = 1.0f;
		// First-version constant hit albedo (plan-sanctioned fallback for the
		// per-object average albedo pipeline; replaced in a later wave).
		static constexpr float kDefaultAlbedo = 0.5f;

		explicit DDGIRayTracePass(GPUDrivenRenderer* renderer);

		// Requires GlobalSDFPass::initResources and DDGIProbeVolume::init to
		// have run; only called when DDGIConfig::enabled is true.
		void initResources(rhi::Device& device, uint32_t frameCount);
		void shutdownResources();

		[[nodiscard]] const char* getName() const override { return "DDGIRayTracePass"; }
		[[nodiscard]] HandleSlice<PassResourceDependency> getDependencies() const override;
		void execute(const PassContext& context) const override;

	private:
		void writeUniforms(uint32_t frameIndex, const PassContext& context) const;

		GPUDrivenRenderer* m_renderer{nullptr};
		rhi::Device* m_device{nullptr};
		uint32_t m_frameCount{0};

		// Views owned by this pass over resources owned elsewhere
		// (DDGIProbeVolume radiance texture, GlobalSDFPass volume mip chain).
		rhi::TextureViewHandle m_radianceView{};
		rhi::TextureViewHandle m_globalSDFView{};
		rhi::SamplerHandle m_globalSDFSampler{};

		rhi::ArgumentLayoutHandle m_layout{};
		rhi::PipelineHandle m_pipeline{};
		// One table + uniform buffer per frame in flight: the views are static
		// (written once at init), only the uniform contents change per dispatch.
		std::vector<rhi::ArgumentTableHandle> m_tables;
		std::vector<rhi::BufferHandle> m_uniformBuffers;

		// Lazily transitions the radiance texture Undefined -> General.
		mutable bool m_radianceLayoutInitialized{false};
	};
} // namespace demo
