#pragma once

// DDGI probe volume resource management (Wave D2-1).
//
// Owns the probe grid GPU resources consumed by the later DDGI waves:
// double-buffered irradiance/depth octahedral atlases (current + history, for
// the D4-1 infinite-bounce ping-pong), the per-probe radiance scratch texture
// written by the SDF ray trace pass (D2-2), and the probe world-position
// storage buffer (BDA-addressable, consumed via GpuPtr).
//
// Resource-management only: no compute dispatch lives here. All resources go
// through rhi:: types exclusively (DDGI hard constraint 1 - no native graphics
// symbols in render/). Allocation is gated by the caller on
// DDGIConfig::enabled (default false), so the default frame allocates nothing.

#include "../rhi/RHIDevice.h"
#include "DDGIConfig.h"

#include <glm/glm.hpp>

namespace demo
{
	// Creation parameters (plan section 4, Wave D2-1 data-structure draft).
	// gridDims == 0 means "derive from the scene bounds":
	// ivec3(sceneLength / probeSpacing) + 2, clamped per axis by
	// DDGIProbeVolume::kMaxGridDims to bound GPU memory.
	struct DDGIProbeVolumeDesc
	{
		glm::uvec3 gridDims{0u, 0u, 0u};
		float probeSpacing{1.5f};
		uint32_t irradianceTexelSize{8u};
		uint32_t depthTexelSize{16u};
		uint32_t raysPerProbe{64u};
		glm::vec3 sceneBoundsMin{0.0f};
		glm::vec3 sceneBoundsMax{0.0f};
	};

	class DDGIProbeVolume
	{
	public:
		// Per-axis probe-count ceiling (memory guard; plan section 3.1 "上限钳制防爆显存").
		static constexpr glm::uvec3 kMaxGridDims{24u, 16u, 24u};
		// Octahedral border texels around each probe tile (1px on each side).
		static constexpr uint32_t kProbeBorderTexels = 2u;
		// Outer guard band around the whole atlas (LuxGI layout).
		static constexpr uint32_t kAtlasEdgeTexels = 2u;

		// Adaptive grid sizing: ivec3(sceneLength / probeSpacing) + 2, clamped
		// per axis to kMaxGridDims (each axis is at least 2 probes).
		[[nodiscard]] static glm::uvec3 computeGridDims(const glm::vec3& boundsMin,
		                                                const glm::vec3& boundsMax,
		                                                float probeSpacing);

		// Builds a volume desc from the runtime DDGI config + scene AABB.
		[[nodiscard]] static DDGIProbeVolumeDesc makeDesc(const DDGIConfig& config,
		                                                  const glm::vec3& sceneBoundsMin,
		                                                  const glm::vec3& sceneBoundsMax);

		// Creates all GPU resources and uploads the CPU-computed probe world
		// positions. Re-init is allowed (frees previous resources first).
		void init(rhi::Device& device, const DDGIProbeVolumeDesc& desc);
		// Safe to call when never initialized (no-op).
		void deinit();

		// Ping-pong (Wave D4-1, implementation approach (2): fixed handles +
		// parity selection). The atlas handles never swap; instead every DDGI
		// pass prebuilds two argument-table sets and selects one with the frame
		// parity derived from the MONOTONIC temporal frame counter
		// (temporalFrameCounter & 1 — never the frames-in-flight ring index,
		// DDGI hard constraint 4). This replaces the previously stubbed
		// swapAtlases(): swapping handles would invalidate every view/table
		// built over them and reintroduce the descriptor-lag class of bugs the
		// TAA fix (c04c878) addressed.
		//
		// Convention: parity 0 writes the A set (m_irradianceAtlas/m_depthAtlas)
		// and reads the B set (the *History members); parity 1 is the reverse.
		// "Write" is the atlas the probe update produces this frame (and the
		// lighting/debug passes consume this frame); "Read" is last frame's.
		[[nodiscard]] rhi::TextureHandle getIrradianceAtlasWrite(uint32_t frameParity) const
		{
			return (frameParity & 1u) == 0u ? m_irradianceAtlas : m_irradianceAtlasHistory;
		}
		[[nodiscard]] rhi::TextureHandle getIrradianceAtlasRead(uint32_t frameParity) const
		{
			return (frameParity & 1u) == 0u ? m_irradianceAtlasHistory : m_irradianceAtlas;
		}
		[[nodiscard]] rhi::TextureHandle getDepthAtlasWrite(uint32_t frameParity) const
		{
			return (frameParity & 1u) == 0u ? m_depthAtlas : m_depthAtlasHistory;
		}
		[[nodiscard]] rhi::TextureHandle getDepthAtlasRead(uint32_t frameParity) const
		{
			return (frameParity & 1u) == 0u ? m_depthAtlasHistory : m_depthAtlas;
		}

		[[nodiscard]] bool isInitialized() const { return m_device != nullptr; }
		[[nodiscard]] const DDGIProbeVolumeDesc& getDesc() const { return m_desc; }
		[[nodiscard]] glm::uvec3 getGridDims() const { return m_desc.gridDims; }
		[[nodiscard]] uint32_t getTotalProbes() const { return m_totalProbes; }
		[[nodiscard]] rhi::Extent2D getIrradianceAtlasExtent() const { return m_irradianceAtlasExtent; }
		[[nodiscard]] rhi::Extent2D getDepthAtlasExtent() const { return m_depthAtlasExtent; }
		[[nodiscard]] rhi::Extent2D getRadianceExtent() const { return m_radianceExtent; }

		[[nodiscard]] rhi::TextureHandle getIrradianceAtlas() const { return m_irradianceAtlas; }
		[[nodiscard]] rhi::TextureHandle getIrradianceAtlasHistory() const { return m_irradianceAtlasHistory; }
		[[nodiscard]] rhi::TextureHandle getDepthAtlas() const { return m_depthAtlas; }
		[[nodiscard]] rhi::TextureHandle getDepthAtlasHistory() const { return m_depthAtlasHistory; }
		[[nodiscard]] rhi::TextureHandle getRadianceBuffer() const { return m_radianceBuffer; }
		[[nodiscard]] rhi::BufferHandle getProbePositionBuffer() const { return m_probePositionBuffer; }
		[[nodiscard]] rhi::GpuPtr getProbePositionAddress() const { return m_probePositionAddress; }

	private:
		// LuxGI atlas layout: probes tile along X as (x + y * dims.x) columns
		// and along Y as z rows; each tile is texelSize + 2 border texels, plus
		// a 2-texel outer edge.
		[[nodiscard]] static rhi::Extent2D atlasExtent(const glm::uvec3& gridDims, uint32_t texelSize);

		// CPU-side probe placement: sceneBoundsMin + ivec3(x,y,z) * probeSpacing
		// stored as float4 (w = 1), written through the mapped storage buffer.
		void uploadProbePositions() const;

		rhi::Device* m_device{nullptr};
		DDGIProbeVolumeDesc m_desc{};
		uint32_t m_totalProbes{0};
		rhi::Extent2D m_irradianceAtlasExtent{};
		rhi::Extent2D m_depthAtlasExtent{};
		rhi::Extent2D m_radianceExtent{};

		// Double-buffered octahedral atlases (RGBA16F irradiance, RG16F depth).
		rhi::TextureHandle m_irradianceAtlas{};
		rhi::TextureHandle m_irradianceAtlasHistory{};
		rhi::TextureHandle m_depthAtlas{};
		rhi::TextureHandle m_depthAtlasHistory{};
		// Per-frame ray-hit radiance scratch (width = raysPerProbe, height = totalProbes).
		rhi::TextureHandle m_radianceBuffer{};
		// totalProbes x float4 world positions, BDA-addressable storage buffer.
		rhi::BufferHandle m_probePositionBuffer{};
		rhi::GpuPtr m_probePositionAddress{};
	};
} // namespace demo
