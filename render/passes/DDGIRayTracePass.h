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
// Infinite bounce (Wave D4-1): hit shading additionally samples LAST frame's
// probe atlases (SampleProbe weight chain) for the indirect term. The atlas
// ping-pong uses fixed handles + parity selection (DDGIProbeVolume contract):
// two argument tables per frame in flight are prebuilt, and execute selects
// by temporalFrameCounter & 1 (monotonic counter, DDGI hard constraint 4 —
// never the frames-in-flight ring index).
//
// All resources go through rhi:: only — no native graphics types and no ray
// tracing extensions (DDGI hard constraints 1/3). The whole pass is gated on
// DDGIConfig::enabled (default false), so default rendering is unchanged.

#include "../Pass.h"
#include "../../rhi/RHIDevice.h"

#include <glm/glm.hpp>

#include <array>
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

		// Per-frame uniform random ray rotation seeded by the monotonically
		// increasing temporal frame counter (DDGI hard constraint 4). Shared
		// with DDGIProbeUpdatePass (D2-3): the update kernels reconstruct the
		// traced ray directions, so both passes MUST use the exact same
		// rotation for a given frame.
		[[nodiscard]] static glm::mat3 makeRayRotation(uint64_t temporalFrameCounter);

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
		// D4-1: sampled views over BOTH probe atlas sets, indexed by the frame
		// parity whose READ pair they are (cf. DDGIProbeVolume parity contract).
		std::array<rhi::TextureViewHandle, 2> m_historyIrradianceViews{};
		std::array<rhi::TextureViewHandle, 2> m_historyDepthViews{};
		rhi::SamplerHandle m_atlasSampler{};

		rhi::ArgumentLayoutHandle m_layout{};
		rhi::PipelineHandle m_pipeline{};
		// Two tables per frame in flight (index = frameIndex * 2 + parity):
		// views are static (written once at init), only the per-frame uniform
		// buffer contents change. Both parity tables of a frame share that
		// frame's uniform buffer; they differ only in the history atlas pair.
		std::vector<rhi::ArgumentTableHandle> m_tables;
		std::vector<rhi::BufferHandle> m_uniformBuffers;

		// Lazily transitions the radiance texture AND the four probe atlases
		// Undefined -> General on the first recorded execute (this pass runs
		// first in the DDGI chain, so the history sample descriptors need the
		// General layout before the probe update pass ever records). Doubles
		// as the "no valid history yet" first-frame latch for the shader-side
		// constant-sky indirect fallback.
		mutable bool m_radianceLayoutInitialized{false};
	};
} // namespace demo
