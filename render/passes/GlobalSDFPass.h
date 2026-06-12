#pragma once

// Global SDF composition pass (DDGI Wave D1-2).
//
// Owns the single-cascade global SDF volume (R16F 128^3 with a min-filtered mip
// chain on the same texture) and the three compute pipelines that rebuild it
// each frame: Clear (write max normalized distance) -> Compose (min over the
// registered mesh SDF list) -> Mipmap (per-level 2x2x2 min downsample).
//
// All resources go through rhi:: only — no native graphics types (DDGI hard
// constraint 1). The whole pass is gated on DDGIConfig::enabled (default
// false), so default rendering behavior is unchanged.

#include "../Pass.h"
#include "../../rhi/RHIDevice.h"

#include <glm/glm.hpp>

#include <vector>

namespace demo
{
	class GPUDrivenRenderer;

	// One mesh SDF instance to compose into the global volume
	// (GlobalSDFPassData entry): padded world-space AABB + the R16F Texture3D
	// produced by tools/sdf_baker and loaded through SDFLoader.
	struct GlobalSDFMeshEntry
	{
		glm::vec3 boundsMin{0.0f};
		glm::vec3 boundsMax{0.0f};
		rhi::TextureHandle sdfTexture{};
	};

	// Single-cascade global SDF volume. The mip chain lives on the same texture
	// (one e3D view per level) instead of a separate lower-resolution texture —
	// rhi::TextureDesc::mipLevels + per-mip createTextureView already support it.
	struct GlobalSDFVolume
	{
		rhi::TextureHandle sdfTexture{};
		std::vector<rhi::TextureViewHandle> mipViews;
		glm::vec3 worldBoundsMin{0.0f};
		glm::vec3 worldBoundsMax{0.0f};
		uint32_t resolution{0};
		float voxelSize{0.0f};
	};

	class GlobalSDFPass final : public ComputePassNode
	{
	public:
		// Plan section 3.1: first version is a single 128^3 cascade.
		static constexpr uint32_t kResolution = 128u;
		// Must match shaderio::LGlobalSDFMipCount (static_assert in the .cpp).
		static constexpr uint32_t kMipCount = 3u;
		// Default world bounds until a later wave wires scene-driven bounds in.
		static constexpr float kDefaultHalfExtent = 16.0f;

		explicit GlobalSDFPass(GPUDrivenRenderer* renderer);

		// Creates the volume, pipelines, layouts and per-frame compose resources.
		// frameCount = frames in flight (swapchain image count).
		void initResources(rhi::Device& device, uint32_t frameCount);
		void shutdownResources();

		// Replaces the composed mesh SDF list. An empty list is valid: the
		// compose dispatch is skipped and the volume stays at the cleared max
		// distance (this wave ships with no registered assets yet).
		void setMeshSDFList(const GlobalSDFMeshEntry* entries, uint32_t count);

		[[nodiscard]] const GlobalSDFVolume& getVolume() const { return m_volume; }
		[[nodiscard]] uint32_t getMeshSDFCount() const { return static_cast<uint32_t>(m_meshEntries.size()); }

		[[nodiscard]] const char* getName() const override { return "GlobalSDFPass"; }
		[[nodiscard]] HandleSlice<PassResourceDependency> getDependencies() const override;
		void execute(const PassContext& context) const override;

	private:
		void destroyMeshViews();
		void writeComposeUniforms(uint32_t frameIndex, uint32_t meshCount) const;
		void updateComposeArgumentTable(uint32_t frameIndex, uint32_t meshCount) const;

		GPUDrivenRenderer* m_renderer{nullptr};
		rhi::Device* m_device{nullptr};
		uint32_t m_frameCount{0};

		GlobalSDFVolume m_volume{};

		rhi::SamplerHandle m_meshSDFSampler{};
		rhi::ArgumentLayoutHandle m_clearLayout{};
		rhi::ArgumentLayoutHandle m_composeLayout{};
		rhi::ArgumentLayoutHandle m_mipmapLayout{};
		rhi::PipelineHandle m_clearPipeline{};
		rhi::PipelineHandle m_composePipeline{};
		rhi::PipelineHandle m_mipmapPipeline{};
		// Static: views never change after init.
		rhi::ArgumentTableHandle m_clearTable{};
		// One table per mip transition (mip N-1 -> mip N), static after init.
		std::vector<rhi::ArgumentTableHandle> m_mipmapTables;
		// Per frame in flight: mesh views + uniforms are rewritten on dispatch.
		std::vector<rhi::ArgumentTableHandle> m_composeTables;
		std::vector<rhi::BufferHandle> m_composeUniformBuffers;

		std::vector<GlobalSDFMeshEntry> m_meshEntries;
		// Views owned by this pass over the entry textures (created on set).
		std::vector<rhi::TextureViewHandle> m_meshViews;

		// Lazily transitions the volume Undefined -> General on first record.
		mutable bool m_layoutInitialized{false};
	};
} // namespace demo
