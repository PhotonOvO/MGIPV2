#pragma once

#include "../rhi/vulkan/internal/VulkanCommon.h"
#include "../common/Handles.h"
#include "../common/HandlePool.h"
#include "ArgumentTables.h"
#include "DrawStream.h"
#include "DrawStreamDecoder.h"
#include <memory>
#include "PassExecutor.h"
#include "MeshPool.h"
#include "LightResources.h"
#include "../loader/GltfLoader.h"
#include "../scene/SceneAsset.h"
#include "../scene/SceneUploadPlan.h"
#include "SceneResources.h"
#include "CSMShadowResources.h"
#include "TransientAllocator.h"
#include "UploadUtils.h"
#include "RenderTypes.h"
#include "../rhi/RHIFrameContext.h"
#include "../rhi/RHIDevice.h"
#include "../rhi/RHIBindlessTypes.h"
#include "../rhi/RHIPipeline.h"
#include "../rhi/RHISwapchain.h"
#include "../rhi/RHISurface.h"
#include <functional>
#include <cstdint>
#include <optional>
#include <span>
#include <string>
#include <unordered_map>

#ifndef DEMO_RHI_VK
#define DEMO_RHI_VK(name) V##k##name
#endif
#ifndef DEMO_RHI_ALLOCATOR
#define DEMO_RHI_ALLOCATOR Vma##Allocator
#endif
#ifndef DEMO_RHI_VULKAN_NS
#define DEMO_RHI_VULKAN_NS vulkan
#endif
#ifndef DEMO_RHI_VULKAN_TYPE
#define DEMO_RHI_VULKAN_TYPE(name) Vulkan##name
#endif
#ifndef DEMO_RENDER_JOIN3
#define DEMO_RENDER_JOIN3(a, b, c) a##b##c
#endif
#ifndef DEMO_RENDER_JOIN4
#define DEMO_RENDER_JOIN4(a, b, c, d) a##b##c##d
#endif
#ifndef DEMO_RENDER_RTV_BACKEND
#define DEMO_RENDER_RTV_BACKEND resolveTextureViewBackendHandle
#endif

#include "../rhi/vulkan/VulkanResourceTable.h"

namespace demo
{
	namespace rhi
	{
		namespace
		DEMO_RHI_VULKAN_NS
		{
		}
	}

	class RenderDevice
	{
	public:
		static constexpr uint32_t kDemoMaterialSlotCount = 2;

		RenderDevice() = default;

		void init(void* window, rhi::Surface& surface, bool vSync);
		[[nodiscard]] std::unique_ptr<rhi::Surface> createSurface() const;
		void shutdown(rhi::Surface& surface);
		void setVSync(bool enabled);
		[[nodiscard]] bool getVSync() const { return m_swapchainDependent.vSync; }
		void setFullscreen(bool enabled, void* platformHandle = nullptr);
		[[nodiscard]] const char* getSwapchainPresentModeName() const;
		void resize(rhi::Extent2D size);
		void beginUiFrame();
		void renderWithPassExecutor(const RenderParams& params, PassExecutor& passExecutor);

		// Pass execution helpers (wrappers for per-pass commands)
		void executeImGuiPass(rhi::CommandBuffer& cmdBuffer, const RenderParams& params);
		void beginPresentPass(rhi::CommandBuffer& cmdBuffer);
		void endPresentPass(rhi::CommandBuffer& cmdBuffer);
		[[nodiscard]] rhi::ResourceIndex getSceneBindlessResourceIndex() const { return kSceneBindlessInfoIndex; }

		TextureHandle getViewportTextureHandle() const;
		ImTextureID getViewportTextureID(TextureHandle handle) const;
		MaterialHandle getMaterialHandle(uint32_t slot) const;

		// glTF model support
		GltfUploadResult uploadGltfModel(const GltfModel& model, rhi::CommandBuffer& cmd);
		SceneUploadResult commitSceneUploadPlan(const SceneAsset& asset, const SceneUploadPlan& plan,
		                                        rhi::CommandBuffer& cmd);
		void uploadGltfModelBatch(const GltfModel& model,
		                          std::span<const uint32_t> textureIndices,
		                          std::span<const uint32_t> materialIndices,
		                          std::span<const uint32_t> meshIndices,
		                          GltfUploadResult& ioResult,
		                          rhi::CommandBuffer& cmd);
		void initializeGltfUploadResult(const GltfModel& model, GltfUploadResult& outResult) const;
		void destroyGltfResources(const GltfUploadResult& result);
		void updateMeshTransform(MeshHandle handle, const glm::mat4& transform);

		// Execute upload commands with internal command buffer management
		void executeUploadCommand(std::function<void(rhi::CommandBuffer&)> uploadFn);

		MeshPool& getMeshPool() { return m_meshPool; }
		const MeshPool& getMeshPool() const { return m_meshPool; }

		rhi::DEMO_RHI_VULKAN_NS::DEMO_RHI_VULKAN_TYPE(ResourceTable)* getResourceTable()
		{
			return &m_device.resourceTable;
		}

		SceneResources& getSceneResources() { return m_swapchainDependent.sceneResources; }
		void bindStaticPassResources(PassExecutor& passExecutor) const;
		void waitForIdle();
		[[nodiscard]] uintptr_t getBackendDeviceToken() const;
		[[nodiscard]] uintptr_t getAllocatorToken() const { return reinterpret_cast<uintptr_t>(m_device.allocator); }
		[[nodiscard]] rhi::Extent2D getSceneExtent() const { return m_swapchainDependent.sceneResources.getSize(); }

		// LightPass support
		PipelineHandle getLightPipelineHandle() const;
		PipelineHandle getGPUDrivenLightHdrPipelineHandle() const;
		PipelineHandle getGPUDrivenSkyboxPipelineHandle() const;
		PipelineHandle getBloomPrefilterPipelineHandle() const;
		PipelineHandle getBloomDownsamplePipelineHandle() const;
		PipelineHandle getFinalColorPipelineHandle() const;
		PipelineHandle getVelocityPipelineHandle() const;
		PipelineHandle getTAAResolvePipelineHandle() const;
		PipelineHandle getDepthPrepassOpaquePipelineHandle() const;
		PipelineHandle getDepthPrepassAlphaTestPipelineHandle() const;
		PipelineHandle getDepthPrepassOpaqueMDIPipelineHandle() const;
		PipelineHandle getDepthPrepassAlphaTestMDIPipelineHandle() const;
		PipelineHandle getGBufferOpaquePipelineHandle() const;
		PipelineHandle getGBufferAlphaTestPipelineHandle() const;
		PipelineHandle getGBufferOpaqueMDIPipelineHandle() const;
		PipelineHandle getGBufferAlphaTestMDIPipelineHandle() const;
		PipelineHandle getForwardPipelineHandle() const;
		PipelineHandle getForwardMDIPipelineHandle() const;
		PipelineHandle getShadowPipelineHandle() const;
		PipelineHandle getDebugPipelineHandle() const;
		PipelineHandle getGPUCullingDebugPipelineHandle() const;
		PipelineHandle getGPUCullingPipelineHandle() const;
		PipelineHandle getCSMShadowPipelineHandle() const; // CSM shadow depth pipeline
		PipelineHandle getShadowCullingPipelineHandle() const;
		void updateGPUCullingDepthPyramidArgumentTable(uint32_t frameIndex,
		                                               const rhi::TextureViewHandle* mipViews,
		                                               uint32_t mipCount);

		[[nodiscard]] rhi::ArgumentTableHandle getGPUCullingArgumentTable(uint32_t frameIndex) const
		{
			return frameIndex < m_device.gpuCullingArgumentTables.size()
				       ? m_device.gpuCullingArgumentTables[frameIndex]
				       : rhi::ArgumentTableHandle{};
		}

		[[nodiscard]] rhi::ArgumentTableHandle getShadowCullingArgumentTable(uint32_t frameIndex) const
		{
			return frameIndex < m_device.shadowCullingArgumentTables.size()
				       ? m_device.shadowCullingArgumentTables[frameIndex]
				       : rhi::ArgumentTableHandle{};
		}

		uint64_t getShadowCullingIndirectBufferOpaque(uint32_t frameIndex) const;
		[[nodiscard]] uint32_t getShadowCullingMeshCapacity(uint32_t frameIndex) const;
		[[nodiscard]] uint64_t getGPUCullingIndirectBufferOpaque(uint32_t frameIndex) const;
		[[nodiscard]] uint64_t getGPUCullingDrawCountBufferOpaque(uint32_t frameIndex) const;
		[[nodiscard]] uint32_t getGPUCullingObjectCount(uint32_t frameIndex) const;
		[[nodiscard]] uint64_t getPreviousGPUCullingIndirectBufferOpaque(uint32_t currentFrameIndex) const;
		[[nodiscard]] uint64_t getPreviousGPUCullingDrawCountBufferOpaque(uint32_t currentFrameIndex) const;
		[[nodiscard]] uint64_t getGPUDrivenPersistentIndirectStreamBuffer(uint32_t frameIndex) const;
		// RHI handle variants of the per-frame culling buffers (Option B). Consumed by
		// RenderEncoder-based geometry passes; previous-frame variants index the same
		// stable handle ring at (frameIndex + frameCount - 1) % frameCount.
		[[nodiscard]] rhi::BufferHandle getGPUCullingIndirectBufferRHIHandle(uint32_t frameIndex) const;
		[[nodiscard]] rhi::BufferHandle getGPUCullingDrawCountBufferRHIHandle(uint32_t frameIndex) const;
		[[nodiscard]] rhi::BufferHandle getPreviousGPUCullingIndirectBufferRHIHandle(uint32_t currentFrameIndex) const;
		[[nodiscard]] rhi::BufferHandle getPreviousGPUCullingDrawCountBufferRHIHandle(uint32_t currentFrameIndex) const;
		[[nodiscard]] rhi::BufferHandle getGPUDrivenPersistentIndirectStreamBufferRHIHandle(uint32_t frameIndex) const;
		[[nodiscard]] rhi::BufferHandle getPreviousGPUDrivenPersistentIndirectStreamBufferRHIHandle(
			uint32_t currentFrameIndex) const;
		[[nodiscard]] rhi::BufferHandle getShadowCullingIndirectBufferRHIHandle(uint32_t frameIndex) const;
		[[nodiscard]] uint32_t getPreviousGPUCullingObjectCount(uint32_t currentFrameIndex,
		                                                        const SceneUploadResult* gltfModel) const;

		[[nodiscard]] uint32_t getGPUCullingIndirectCommandStride() const
		{
			return static_cast<uint32_t>(sizeof(shaderio::GPUCullIndirectCommand));
		}

		[[nodiscard]] uint32_t getCurrentFrameIndexHint() const;
		[[nodiscard]] const shaderio::GPUCullStats& getLastGPUCullingStats() const { return m_lastGPUCullingStats; }

		[[nodiscard]] const shaderio::GPUCullDrawCounts& getLastGPUCullingDrawCounts() const
		{
			return m_lastGPUCullingDrawCounts;
		}

		[[nodiscard]] const std::vector<GPUCullOverlayObject>& getLastGPUCullingOverlayObjects() const
		{
			return m_lastGPUCullingOverlayObjects;
		}

		[[nodiscard]] RuntimeProfileSnapshot getRuntimeProfileSnapshot() const;
		CSMShadowResources& getCSMShadowResources() { return m_csmShadowResources; }
		// Per-cascade depth render-target view as an RHI handle (created via the texture-view
		// registry in init). Replaces the previous per-layer image-view resources.
		[[nodiscard]] rhi::TextureViewHandle getCSMCascadeViewHandle(uint32_t cascadeIndex) const
		{
			return cascadeIndex < m_csmCascadeViewHandles.size()
				       ? m_csmCascadeViewHandles[cascadeIndex]
				       : rhi::TextureViewHandle{};
		}

		const shaderio::CameraUniforms& getShadowCameraUniforms() const { return m_frameLightingState.shadowCamera; }
		const shaderio::LightParams& getLightPassParams() const { return m_frameLightingState.lightParams; }
		[[nodiscard]] shaderio::ShadowCullPushConstants buildShadowCullPushConstants(
			uint32_t cascadeIndex, uint32_t objectCount) const;
		const std::vector<shaderio::DebugLineVertex>& getDebugLineVertices() const { return m_debugDrawList.vertices; }
		uint32_t getFrameResourceCount() const; // Number of per-frame resource slots
		[[nodiscard]] rhi::ArgumentTableHandle getGraphicsMaterialArgumentTable() const;
		[[nodiscard]] bool getIBLEnvironmentLoaded() const;
		[[nodiscard]] bool getIBLUsingFallback() const;
		[[nodiscard]] rhi::TextureFormat getIBLEnvironmentFormat() const;
		[[nodiscard]] rhi::Extent2D getIBLEnvironmentExtent() const;
		[[nodiscard]] uint32_t getIBLEnvironmentMipCount() const;
		[[nodiscard]] uint64_t getIBLEnvironmentEstimatedBytes() const;
		[[nodiscard]] const std::string& getIBLEnvironmentPath() const;
		[[nodiscard]] const std::string& getIBLEnvironmentStatus() const;
		void updateLightCoarseCullingResources(uint32_t frameIndex,
		                                       const shaderio::LightCoarseCullingUniforms& uniforms);
		[[nodiscard]] uint64_t getGPUCullingObjectBufferAddress(uint32_t frameIndex) const;
		[[nodiscard]] uint64_t getGPUCullingResultBufferAddress(uint32_t frameIndex) const;

		// Per-frame argument table accessors for dynamic uniform buffers.
		rhi::ArgumentTableHandle getCameraArgumentTable(uint32_t frameIndex) const;
		rhi::ArgumentTableHandle getDrawArgumentTable(uint32_t frameIndex) const;
		rhi::ArgumentTableHandle getMDIDrawArgumentTable(uint32_t frameIndex) const;
		rhi::ArgumentTableHandle getGBufferMDIDrawArgumentTable(uint32_t frameIndex) const;
		rhi::ArgumentTableHandle getDepthMDIDrawArgumentTable(uint32_t frameIndex) const;
		rhi::ArgumentTableHandle getCSMShadowMDIDrawArgumentTable(uint32_t frameIndex, uint32_t cascadeIndex) const;
		// Legacy compatibility accessors.
		[[nodiscard]] uint64_t getForwardMDIIndirectBuffer(uint32_t frameIndex) const;
		void ensureGPUDrivenPersistentIndirectStream(uint32_t frameIndex, uint32_t requiredDrawCount);
		void uploadMDIDrawData(uint32_t frameIndex, std::span<const shaderio::DrawUniforms> drawData);
		void uploadMDIDrawDataRange(uint32_t frameIndex, uint32_t firstDrawIndex,
		                            std::span<const shaderio::DrawUniforms> drawData);
		void uploadGBufferMDIDrawData(uint32_t frameIndex, std::span<const shaderio::DrawUniforms> drawData);
		void uploadGBufferMDIDrawDataRange(uint32_t frameIndex, uint32_t firstDrawIndex,
		                                   std::span<const shaderio::DrawUniforms> drawData);
		void uploadDepthMDIDrawData(uint32_t frameIndex, std::span<const shaderio::DrawUniforms> drawData);
		void uploadDepthMDIDrawDataRange(uint32_t frameIndex, uint32_t firstDrawIndex,
		                                 std::span<const shaderio::DrawUniforms> drawData);

		// Argument table creation.
		rhi::ArgumentLayoutHandle createArgumentLayout(const ArgumentLayoutDesc& desc);
		void destroyArgumentTable(rhi::ArgumentTableHandle handle);
		rhi::ArgumentTableHandle createPersistentArgumentTable(rhi::ArgumentLayoutHandle layout, const char* debugName);
		void updateArgumentTable(rhi::ArgumentTableHandle handle,
		                         const rhi::ArgumentWrite* writes,
		                         uint32_t writeCount) const;


		// --- Texture views as RHI handles (the only thing business/pass code should hold) ---
		// Texture views are exposed to renderer/pass code only as RHI handles.
		rhi::TextureViewHandle createTextureView(const rhi::TextureViewCreateDesc& desc);
		rhi::TextureViewHandle registerExternalTextureView(uint64_t externalView);
		void destroyTextureView(rhi::TextureViewHandle handle);

		// Frame-lifetime argument table. Allocates a fresh
		// descriptor set from the given layout, writes it, and returns a handle valid only
		// for the current frame Ã¢â‚¬â€ it is destroyed automatically when this frame index is
		// recorded again (after its fence). Callers must NOT cache the handle across frames.
		rhi::ArgumentTableHandle createTemporaryArgumentTable(rhi::ArgumentLayoutHandle layout,
		                                                      const rhi::ArgumentWrite* writes,
		                                                      uint32_t writeCount,
		                                                      ArgumentSlot slot,
		                                                      const char* debugName);

		// Get material baseColorFactor and texture info for glTF rendering
		glm::vec4 getMaterialBaseColorFactor(MaterialHandle handle) const;
		int32_t getMaterialBaseColorTextureIndex(MaterialHandle materialHandle,
		                                         const SceneUploadResult* gltfModel) const;

		// Material texture indices struct for GBuffer rendering
		struct MaterialTextureIndices
		{
			int32_t baseColor = -1;
			int32_t normal = -1;
			int32_t metallicRoughness = -1;
			int32_t occlusion = -1;
			float metallicFactor = 1.0f;
			float roughnessFactor = 1.0f;
			float normalScale = 1.0f;
			int32_t alphaMode = 0; // 0=OPAQUE, 1=MASK, 2=BLEND
			float alphaCutoff = 0.5f;
		};

		MaterialTextureIndices getMaterialTextureIndices(MaterialHandle materialHandle,
		                                                 const SceneUploadResult* gltfModel) const;

		// RHI accessors (replacing native accessors)
		rhi::TextureViewHandle getCurrentSwapchainView() const;
		rhi::TextureViewHandle getGBufferView(uint32_t index) const;
		rhi::TextureViewHandle getDepthView() const;
		rhi::ArgumentTableHandle getGlobalBindlessGroup() const;
		// Wave 8: current frame's transient allocator buffer as an RHI handle (for ArgumentWrites).
		[[nodiscard]] rhi::BufferHandle getCurrentTransientBufferHandle() const;
		[[nodiscard]] rhi::BufferHandle getTransientBufferHandle(uint32_t frameIndex) const;

		void updateBindlessTexture(uint32_t index, TextureHandle textureHandle);
		void invalidateBindlessTexture(uint32_t index);
		// Get the base index for glTF textures in the bindless array
		static constexpr uint32_t getGltfTextureBaseIndex() { return kDemoMaterialSlotCount; }

		rhi::Extent2D getSwapchainExtent() const
		{
			return {m_swapchainDependent.windowSize.width, m_swapchainDependent.windowSize.height};
		}

		uint32_t getSwapchainImageCount() const { return m_swapchainDependent.swapchain->getRequestedImageCount(); }
		rhi::TextureHandle getCurrentSwapchainTextureHandle() const;
		// Per-image-index registry handles mirroring the swapchain backbuffers into the
		// device resource table (lazily (re)registered by getCurrentSwapchainTextureHandle).
		mutable std::vector<rhi::TextureHandle> m_swapchainTextureHandles;
		mutable std::vector<uint64_t> m_swapchainTextureNatives;
		rhi::TextureFormat getSceneDepthFormat() const { return m_swapchainDependent.sceneResources.getDepthFormat(); }
		rhi::TextureHandle getSceneDepthImage() const { return m_swapchainDependent.sceneResources.getDepthImage(); }

		rhi::TextureViewHandle getSceneDepthImageView() const
		{
			return m_swapchainDependent.sceneResources.getDepthImageView();
		}

		rhi::TextureHandle getSceneGBufferImage(uint32_t index) const
		{
			return m_swapchainDependent.sceneResources.getColorImage(index);
		}

		rhi::TextureViewHandle getSceneGBufferImageView(uint32_t index) const
		{
			return m_swapchainDependent.sceneResources.getGBufferImageView(index);
		}

		rhi::TextureHandle getOutputTextureImage() const
		{
			return m_swapchainDependent.sceneResources.getOutputTextureImage();
		}

		rhi::TextureViewHandle getOutputTextureView() const;

		rhi::TextureFormat getOutputTextureFormat() const
		{
			return m_swapchainDependent.sceneResources.getOutputTextureFormat();
		}

		uint64_t getOutputTextureEstimatedBytes() const
		{
			return m_swapchainDependent.sceneResources.getOutputTextureEstimatedBytes();
		}

		rhi::TextureHandle getSceneColorHdrImage() const
		{
			return m_swapchainDependent.sceneResources.getSceneColorHdrImage();
		}

		rhi::TextureViewHandle getSceneColorHdrView() const
		{
			return m_swapchainDependent.sceneResources.getSceneColorHdrView();
		}

		rhi::TextureFormat getSceneColorHdrFormat() const
		{
			return m_swapchainDependent.sceneResources.getSceneColorHdrFormat();
		}

		uint64_t getSceneColorHdrEstimatedBytes() const
		{
			return m_swapchainDependent.sceneResources.getSceneColorHdrEstimatedBytes();
		}

		rhi::TextureHandle getBloomHalfImage() const { return m_swapchainDependent.sceneResources.getBloomHalfImage(); }

		rhi::TextureViewHandle getBloomHalfView() const
		{
			return m_swapchainDependent.sceneResources.getBloomHalfView();
		}

		rhi::Extent2D getBloomHalfExtent() const { return m_swapchainDependent.sceneResources.getBloomHalfExtent(); }

		rhi::TextureHandle getBloomQuarterImage() const
		{
			return m_swapchainDependent.sceneResources.getBloomQuarterImage();
		}

		rhi::TextureViewHandle getBloomQuarterView() const
		{
			return m_swapchainDependent.sceneResources.getBloomQuarterView();
		}

		rhi::Extent2D getBloomQuarterExtent() const
		{
			return m_swapchainDependent.sceneResources.getBloomQuarterExtent();
		}

		rhi::TextureHandle getBloomEighthImage() const
		{
			return m_swapchainDependent.sceneResources.getBloomEighthImage();
		}

		rhi::TextureViewHandle getBloomEighthView() const
		{
			return m_swapchainDependent.sceneResources.getBloomEighthView();
		}

		rhi::Extent2D getBloomEighthExtent() const
		{
			return m_swapchainDependent.sceneResources.getBloomEighthExtent();
		}

		rhi::TextureHandle getBloomSixteenthImage() const
		{
			return m_swapchainDependent.sceneResources.getBloomSixteenthImage();
		}

		rhi::TextureViewHandle getBloomSixteenthView() const
		{
			return m_swapchainDependent.sceneResources.getBloomSixteenthView();
		}

		rhi::Extent2D getBloomSixteenthExtent() const
		{
			return m_swapchainDependent.sceneResources.getBloomSixteenthExtent();
		}

		rhi::TextureHandle getBloomThirtySecondImage() const
		{
			return m_swapchainDependent.sceneResources.getBloomThirtySecondImage();
		}

		rhi::TextureViewHandle getBloomThirtySecondView() const
		{
			return m_swapchainDependent.sceneResources.getBloomThirtySecondView();
		}

		rhi::Extent2D getBloomThirtySecondExtent() const
		{
			return m_swapchainDependent.sceneResources.getBloomThirtySecondExtent();
		}

		rhi::TextureHandle getBloomUpsampleSixteenthImage() const
		{
			return m_swapchainDependent.sceneResources.getBloomUpsampleSixteenthImage();
		}

		rhi::TextureViewHandle getBloomUpsampleSixteenthView() const
		{
			return m_swapchainDependent.sceneResources.getBloomUpsampleSixteenthView();
		}

		rhi::Extent2D getBloomUpsampleSixteenthExtent() const
		{
			return m_swapchainDependent.sceneResources.getBloomUpsampleSixteenthExtent();
		}

		rhi::TextureHandle getBloomUpsampleEighthImage() const
		{
			return m_swapchainDependent.sceneResources.getBloomUpsampleEighthImage();
		}

		rhi::TextureViewHandle getBloomUpsampleEighthView() const
		{
			return m_swapchainDependent.sceneResources.getBloomUpsampleEighthView();
		}

		rhi::Extent2D getBloomUpsampleEighthExtent() const
		{
			return m_swapchainDependent.sceneResources.getBloomUpsampleEighthExtent();
		}

		rhi::TextureHandle getBloomUpsampleQuarterImage() const
		{
			return m_swapchainDependent.sceneResources.getBloomUpsampleQuarterImage();
		}

		rhi::TextureViewHandle getBloomUpsampleQuarterView() const
		{
			return m_swapchainDependent.sceneResources.getBloomUpsampleQuarterView();
		}

		rhi::Extent2D getBloomUpsampleQuarterExtent() const
		{
			return m_swapchainDependent.sceneResources.getBloomUpsampleQuarterExtent();
		}

		rhi::TextureHandle getBloomOutputImage() const
		{
			return m_swapchainDependent.sceneResources.getBloomOutputImage();
		}

		rhi::TextureViewHandle getBloomOutputView() const
		{
			return m_swapchainDependent.sceneResources.getBloomOutputView();
		}

		rhi::Extent2D getBloomOutputExtent() const
		{
			return m_swapchainDependent.sceneResources.getBloomOutputExtent();
		}

		rhi::TextureHandle getColorGradingLutImage() const
		{
			return m_swapchainDependent.sceneResources.getColorGradingLutImage();
		}

		rhi::TextureViewHandle getColorGradingLutView() const
		{
			return m_swapchainDependent.sceneResources.getColorGradingLutView();
		}

		rhi::Extent2D getColorGradingLutExtent() const
		{
			return m_swapchainDependent.sceneResources.getColorGradingLutExtent();
		}

		uint64_t getBloomEstimatedBytes() const { return m_swapchainDependent.sceneResources.getBloomEstimatedBytes(); }
		rhi::TextureHandle getVelocityImage() const { return m_swapchainDependent.sceneResources.getVelocityImage(); }
		rhi::TextureViewHandle getVelocityView() const { return m_swapchainDependent.sceneResources.getVelocityView(); }
		rhi::TextureFormat getVelocityFormat() const { return m_swapchainDependent.sceneResources.getVelocityFormat(); }

		uint64_t getVelocityEstimatedBytes() const
		{
			return m_swapchainDependent.sceneResources.getVelocityEstimatedBytes();
		}

		rhi::TextureHandle getSceneColorHistoryImage(uint32_t index) const
		{
			return m_swapchainDependent.sceneResources.getSceneColorHistoryImage(index);
		}

		rhi::TextureViewHandle getSceneColorHistoryView(uint32_t index) const
		{
			return m_swapchainDependent.sceneResources.getSceneColorHistoryView(index);
		}

		uint64_t getSceneColorHistoryEstimatedBytes() const
		{
			return m_swapchainDependent.sceneResources.getSceneColorHistoryEstimatedBytes();
		}

		rhi::TextureViewHandle getShadowMapView() const;
		shaderio::ShadowUniforms* getShadowUniformsData();
		[[nodiscard]] rhi::Device& getRHIDevice() const { return *m_device.device; }

	private:
		// Created during RenderDevice::init() after feature negotiation.
		// Destroyed during RenderDevice::shutdown() after the backend device is idle.
		// Rebuild trigger: none while device is alive; recreated only on full renderer/device re-init.
		struct DeviceLifetimeResources
		{
			std::unique_ptr<rhi::Device> device;
			DEMO_RHI_ALLOCATOR allocator{nullptr};
			std::vector<rhi::BufferHandle> rhiStagingBuffers;
			upload::StaticBufferUploadPolicy staticBufferUploadPolicy{};
			// Shared samplers created through the RHI (rhi::Device::createSampler), held as handles.
			// sceneLinear feeds SceneResources; gbufferLinear feeds the GBuffer descriptor set.
			rhi::SamplerHandle sceneLinearSamplerHandle{};
			rhi::SamplerHandle gbufferLinearSamplerHandle{};

			utils::Buffer vertexBuffer;
			utils::Buffer pointsBuffer;
			// TODO(Phase4): sink to VulkanDevice -- blocked on VulkanSwapchain::init void* coupling (L1030)
			DEMO_RHI_VK(CommandPool) transientCmdPool{};
			DEMO_RHI_VK(DescriptorPool) descriptorPool{};
			DEMO_RHI_VK(DescriptorPool) uiDescriptorPool{};
			utils::ImageResource iblEnvironment{};
			rhi::TextureFormat iblEnvironmentFormat{rhi::TextureFormat::undefined};
			rhi::Extent2D iblEnvironmentExtent{};
			uint32_t iblEnvironmentMipCount{0};
			uint64_t iblEnvironmentEstimatedBytes{0};
			bool iblEnvironmentLoaded{false};
			bool iblUsingFallback{true};
			std::string iblEnvironmentPath;
			std::string iblEnvironmentStatus{"Not initialized"};
			std::vector<rhi::ArgumentTableHandle> gpuCullingArgumentTables;
			std::vector<rhi::ArgumentTableHandle> shadowCullingArgumentTables;
			std::array<rhi::ArgumentLayoutHandle, 2> rasterArgumentLayouts{};
			std::array<rhi::ArgumentLayoutHandle, 3> gbufferArgumentLayouts{};
			std::array<rhi::ArgumentLayoutHandle, 3> mdiArgumentLayouts{};
			std::array<rhi::ArgumentLayoutHandle, 3> csmShadowMdiArgumentLayouts{};
			std::array<rhi::ArgumentLayoutHandle, 2> debugArgumentLayouts{};
			std::array<rhi::ArgumentLayoutHandle, 2> fullscreenArgumentLayouts{};
			rhi::DEMO_RHI_VULKAN_NS::DEMO_RHI_VULKAN_TYPE(ResourceTable) resourceTable;

			struct PrebuiltPipelineVariants
			{
				PipelineHandle graphicsTextured{};
				PipelineHandle graphicsNonTextured{};
				PipelineHandle compute{};
			} prebuiltPipelines;
		};

		// Created during init(), reinitialized whenever swapchain indicates rebuild, and destroyed before device teardown.
		// Rebuild trigger: WSI/surface resize, suboptimal/out-of-date acquire or explicit requestSwapchainRebuild().
		struct SwapchainDependentResources
		{
			std::unique_ptr<rhi::Swapchain> swapchain;
			SceneResources sceneResources;
			rhi::Extent2D windowSize{800, 600};
			rhi::Extent2D viewportSize{800, 600};
			rhi::TextureFormat swapchainImageFormat{rhi::TextureFormat::bgra8Unorm};
			uint32_t currentImageIndex{0};
			bool hasAcquiredImage{false};
			std::vector<rhi::ResourceState> imageStates; // Track per-image layout state
			bool vSync{true};
		};

		// Created after swapchain frame-count is known.
		// Reset/reuse trigger: every frame-ring iteration waits timeline then resets that slot's command pool.
		// Recreated only if frame-count policy changes (future task) or full renderer re-init.
		struct PerFrameResources
		{
			std::unique_ptr<rhi::FrameContext> frameContext;

			struct FrameUserData
			{
				TransientAllocator transientAllocator{};
				rhi::ArgumentTableHandle sceneArgumentTable{};
				rhi::ArgumentTableHandle cameraArgumentTable{};
				rhi::ArgumentTableHandle drawArgumentTable{};
				rhi::ArgumentTableHandle mdiDrawArgumentTable{};
				rhi::ArgumentTableHandle gbufferMdiDrawArgumentTable{};
				rhi::ArgumentTableHandle depthMdiDrawArgumentTable{};
				std::array<rhi::ArgumentTableHandle, shaderio::LCascadeCount> csmShadowMdiDrawArgumentTables{};
				utils::Buffer lightingBuffer{};
				utils::Buffer lightCullingBuffer{};
				utils::Buffer gpuCullingObjectBuffer{};
				utils::Buffer gpuCullingIndirectBuffer{};
				utils::Buffer gpuCullingDrawCountBuffer{};
				utils::Buffer gpuCullingStatsBuffer{};
				utils::Buffer gpuCullingUniformBuffer{};
				utils::Buffer gpuCullingResultBuffer{};
				DEMO_RHI_VK(Buffer) externalGPUCullingObjectBuffer{VK_NULL_HANDLE};
				DEMO_RHI_VK(Buffer) externalGPUCullingMeshletBuffer{VK_NULL_HANDLE};
				DEMO_RHI_VK(Buffer) externalGPUCullingSceneObjectBuffer{VK_NULL_HANDLE};
				uint64_t externalGPUCullingObjectBufferAddress{0};
				bool useExternalGPUCullingObjectBuffer{false};
				bool useExternalGPUCullingMeshletData{false};
				const SceneUploadResult* gpuCullingSourceModel{nullptr};
				uint32_t gpuCullingObjectCount{0};
				uint32_t gpuCullingMeshCapacity{0};
				std::vector<uint32_t> gpuCullingResults;
				std::vector<shaderio::GPUCullObject> gpuCullingScratchObjects;
				utils::Buffer shadowCullingObjectBuffer{};
				utils::Buffer shadowCullingIndirectBuffer{};
				utils::Buffer shadowCullingDrawDataBuffer{};
				utils::Buffer mdiDrawDataBuffer{};
				utils::Buffer gbufferMdiDrawDataBuffer{};
				utils::Buffer depthMdiDrawDataBuffer{};
				utils::Buffer gpuDrivenPersistentIndirectStreamBuffer{};
				// Stable RHI handles mirroring the per-frame native buffers above (Option B:
				// allocated once, rebound to the native buffer on each realloc). Consumed by
				// RenderEncoder-based passes via getXxxBufferRHIHandle().
				rhi::BufferHandle gpuCullingIndirectBufferRHI{};
				rhi::BufferHandle gpuCullingDrawCountBufferRHI{};
				// Wave 9 (gpuCulling step 2): stable RHI handles mirroring the remaining native
				// culling buffers (binding 0/2/3/6 owned here, 7/8 the external variants). Rebound
				// on realloc / per-frame external swap; consumed by step 3's updateArgumentTable.
				rhi::BufferHandle gpuCullingObjectBufferRHI{};
				rhi::BufferHandle gpuCullingStatsBufferRHI{};
				rhi::BufferHandle gpuCullingResultBufferRHI{};
				rhi::BufferHandle gpuCullingUniformBufferRHI{};
				rhi::BufferHandle externalGPUCullingObjectBufferRHI{};
				rhi::BufferHandle externalGPUCullingMeshletBufferRHI{};
				rhi::BufferHandle externalGPUCullingSceneObjectBufferRHI{};
				rhi::BufferHandle shadowCullingObjectBufferRHI{};
				rhi::BufferHandle shadowCullingIndirectBufferRHI{};
				rhi::BufferHandle gpuDrivenPersistentIndirectStreamBufferRHI{};
				// Wave 8: stable RHI handles mirroring the per-frame UBO/SSBO buffers consumed by
				// camera/draw/scene/mdi bind groups (Option B rebind on realloc). The transient
				// allocator buffer is registered once and remains stable after init.
				rhi::BufferHandle transientBufferRHI{};
				rhi::BufferHandle lightingBufferRHI{};
				rhi::BufferHandle lightCullingBufferRHI{};
				rhi::BufferHandle mdiDrawDataBufferRHI{};
				rhi::BufferHandle gbufferMdiDrawDataBufferRHI{};
				rhi::BufferHandle depthMdiDrawDataBufferRHI{};
				rhi::BufferHandle shadowCullingDrawDataBufferRHI{};
				uint32_t shadowCullingMeshCapacity{0};
				uint32_t mdiDrawCapacity{0};
				uint32_t gbufferMdiDrawCapacity{0};
				uint32_t depthMdiDrawCapacity{0};
				uint32_t gpuDrivenPersistentIndirectStreamCapacity{0};
				std::vector<shaderio::ShadowCullObject> shadowCullingScratchObjects;
				std::vector<shaderio::DrawUniforms> shadowCullingScratchDrawData;
				// Argument tables created via createTemporaryArgumentTable during this frame index's
				// recording. Recycled (destroyed) the next time this frame index comes around,
				// after its fence has been waited on, so the descriptor sets are safely idle.
				std::vector<rhi::ArgumentTableHandle> transientArgumentTables;
			};

			std::vector<FrameUserData> frameUserData;
			uint64_t frameCounter{1};
		};

		// Pass-scoped scratch and pass-owned descriptors will move here in later tasks.
		// Lifetime trigger: reset each recorded pass; no persistent renderer-owned pass data yet.
		struct PerPassResources
		{
			DrawStream drawStream;
		};

		bool m_presentPassActive{false};

		// glTF support
		MeshPool m_meshPool;

		// Light pipeline
		PipelineHandle m_lightPipeline{};
		PipelineHandle m_gpuDrivenLightHdrPipeline{};
		PipelineHandle m_gpuDrivenSkyboxPipeline{};
		PipelineHandle m_bloomPrefilterPipeline{};
		PipelineHandle m_bloomDownsamplePipeline{};
		PipelineHandle m_finalColorPipeline{};
		PipelineHandle m_velocityPipeline{};
		PipelineHandle m_taaResolvePipeline{};
		PipelineHandle m_depthPrepassOpaquePipeline{};
		PipelineHandle m_depthPrepassAlphaTestPipeline{};
		PipelineHandle m_depthPrepassOpaqueMDIPipeline{};
		PipelineHandle m_depthPrepassAlphaTestMDIPipeline{};
		PipelineHandle m_gpuCullingPipeline{};
		PipelineHandle m_shadowCullingPipeline{};
		PipelineHandle m_gbufferOpaquePipeline{}; // GBuffer Opaque variant
		PipelineHandle m_gbufferAlphaTestPipeline{}; // GBuffer AlphaTest variant
		PipelineHandle m_gbufferOpaqueMDIPipeline{};
		PipelineHandle m_gbufferAlphaTestMDIPipeline{};
		PipelineHandle m_shadowPipeline{}; // Directional shadow depth pass
		PipelineHandle m_csmShadowPipeline{}; // CSM cascade MDI depth pass
		CSMShadowResources m_csmShadowResources{}; // CSM cascade texture and uniform buffer
		std::array<rhi::TextureViewHandle, shaderio::LCascadeCount> m_csmCascadeViewHandles{};
		// per-cascade render-target views
		rhi::TextureViewHandle m_csmCascadeArrayViewHandle{}; // full-array sampling view
		PipelineHandle m_forwardPipeline{}; // Forward pass for transparent
		PipelineHandle m_forwardMDIPipeline{}; // Forward MDI pass for transparent
		PipelineHandle m_debugPipeline{}; // Debug line overlay pass
		PipelineHandle m_gpuCullingDebugPipeline{}; // Current-frame GPU culling visualization

		// GBuffer uniform buffer bind groups (per-frame)

		// Draw-call-scoped transient CPU/GPU data staging bucket.
		// Lifetime trigger: rebuilt per draw packet emission/consumption; currently no persistent owner fields.
		struct PerDrawData
		{
		};

		struct Aabb
		{
			glm::vec3 min{0.0f};
			glm::vec3 max{0.0f};
			bool valid{false};
		};

		struct FrameLightingState
		{
			shaderio::CameraUniforms shadowCamera{};
			shaderio::LightParams lightParams{};
			Aabb sceneBounds{};
			std::array<glm::vec3, 8> viewFrustumCorners{};
			std::array<glm::vec3, 8> shadowFrustumCorners{};
			glm::vec3 lightAnchor{0.0f};
			float shadowDistance{0.0f};
		};

		struct DebugDrawList
		{
			std::vector<shaderio::DebugLineVertex> vertices;

			void clear() { vertices.clear(); }
			void addLine(const glm::vec3& a, const glm::vec3& b, const glm::vec4& color);
			void addAabb(const Aabb& bounds, const glm::vec4& color);
			void addFrustum(const std::array<glm::vec3, 8>& corners, const glm::vec4& color);
			void addSphere(const glm::vec3& center, float radius, const glm::vec4& color, uint32_t segments);
			void addArrow(const glm::vec3& origin, const glm::vec3& direction, float length, const glm::vec4& color);
		};

		struct TestPointLightMotion
		{
			glm::vec3 baseT{0.5f};
			glm::vec3 phase{0.0f};
			glm::vec3 speed{1.0f};
			glm::vec3 amplitude{0.0f};
			float radiusT{1.0f};
			float intensityT{1.0f};
		};

		// Material/texture domain resources.
		// Created during init(), updated when bound material texture set changes, destroyed during shutdown.
		// Rebuild trigger: descriptor/layout recreation policy or material-set growth changes.
		struct MaterialResources
		{
			enum class TextureRuntimeKind
			{
				materialSampled,
				viewportAttachment,
				outputTexture, // OutputTexture for PBR lighting result
			};

			struct TextureHotData
			{
				TextureRuntimeKind runtimeKind{TextureRuntimeKind::materialSampled};
				uint32_t viewportAttachmentIndex{0};
				DEMO_RHI_VK(ImageView) sampledImageView{VK_NULL_HANDLE};
				DEMO_RHI_VK(ImageLayout) sampledImageLayout{VK_IMAGE_LAYOUT_UNDEFINED};
				// Wave 8: adopted RHI view handle mirroring sampled image view, for ArgumentWrite-based
				// material (combinedImageSampler) bindless updates. Released via removeTextureView.
				rhi::TextureViewHandle sampledViewHandle{};
			};

			struct TextureColdData
			{
				// Phase 3: loadAndCreateImage 产物改用 RHI handle (D-03)
				// registerExternalTexture/registerExternalTextureView owned=false:
				//   destroyTexture + destroyTextureView 只解注册，不释放 VMA / vkDestroyImageView。
				//   VMA 释放须通过 destroyImageResource(allocator, utils::Image) 独立调用。
				// Destroy 顺序（必须）：先 destroyTextureView，再 destroyTexture，最后 VMA 释放。
				rhi::TextureHandle     ownedTexture{};
				rhi::TextureViewHandle ownedTextureView{};
				// Native image kept for VMA release in destroy path (registerExternal owned=false).
				utils::Image           ownedNativeImage{};
				DEMO_RHI_VK(Extent2D) sourceExtent{};
				uint32_t mipLevels{1};
			};

			struct TextureRecord
			{
				TextureHotData hot{};
				TextureColdData cold{};
			};

			struct MaterialRecord
			{
				// PBR Texture handles (each independent for sharing)
				TextureHandle baseColorTexture{kNullTextureHandle};
				TextureHandle metallicRoughnessTexture{kNullTextureHandle};
				TextureHandle normalTexture{kNullTextureHandle};
				TextureHandle occlusionTexture{kNullTextureHandle};
				TextureHandle emissiveTexture{kNullTextureHandle};

				// Legacy compatibility
				TextureHandle sampledTexture{};

				// PBR Factors (fallback when texture missing)
				glm::vec4 baseColorFactor{1.0f};
				float metallicFactor{1.0f};
				float roughnessFactor{1.0f};
				float normalScale{1.0f};
				float occlusionStrength{1.0f};
				glm::vec3 emissiveFactor{0.0f};

				// Alpha properties
				int32_t alphaMode = 0; // 0=OPAQUE, 1=MASK, 2=BLEND
				float alphaCutoff = 0.5f; // for MASK mode

				// Bindless descriptor slot
				rhi::ResourceIndex descriptorIndex{0};
				const char* debugName{"material"};
			};

			HandlePool<TextureHandle, TextureRecord> texturePool;
			HandlePool<MaterialHandle, MaterialRecord> materialPool;
			// Wave 8: a bind group is an ArgumentTable. Track all device-created tables and the
			// layouts they were built from so destroyArgumentTablesAndLayouts() can release them. Adopted
			// external tables (owned=false) are tracked here too and unregistered on teardown
			// (their native descriptor set is freed by whoever owns the external pool).
			std::vector<rhi::ArgumentTableHandle> ownedArgumentTables;
			std::vector<rhi::ArgumentLayoutHandle> ownedArgumentLayouts;
			MaterialHandle sampleMaterials[kDemoMaterialSlotCount]{};
			TextureHandle viewportTextureHandle{};
			rhi::ArgumentTableHandle materialArgumentTable{};
			rhi::SamplerHandle materialSamplerHandle{}; // Wave 8: shared sampler for combinedImageSampler ArgumentWrite
			std::vector<rhi::ArgumentTableHandle> materialArgumentTables;
			// Wave 8: per-slot adopted view handles for the bindless material array. The shared
			// sampler is materialSamplerHandle; writes go through combinedImageSampler ArgumentWrites.
			std::vector<rhi::TextureViewHandle> materialDescriptorViews;
			std::vector<uint8_t> materialDescriptorValid;
			std::vector<uint64_t> materialArgumentTableGenerations;
			uint64_t materialDescriptorGeneration{0};
			uint32_t maxTextures{10000};
		};

		void createTransientCommandPool();
		void createFrameSubmission(uint32_t numFrames);
		void rebuildSwapchainDependentResources(std::optional<rhi::Extent2D> requestedViewportSize = std::nullopt);
		bool prepareFrameResources();
		bool acquireSwapchainImageForPresent();
		rhi::CommandBuffer& beginCommandRecording();
		void drawFrame(rhi::CommandBuffer& cmdBuffer, const RenderParams& params, PassExecutor& passExecutor);
		void endFrame(rhi::CommandBuffer& cmdBuffer);
		void updateLightingUniformBuffer(uint32_t frameIndex, const shaderio::LightingUniforms& lightingUniforms);
		void updateLightCullingUniformBuffer(uint32_t frameIndex,
		                                     const shaderio::LightCullingUniforms& cullingUniforms);
		void prebuildRequiredPipelineVariants();
		void createPrebuiltGraphicsPipelineVariants();
		void createPrebuiltComputePipelineVariant();
		void createLightResources();
		void createGPUCullingResources();
		void updateGPUCullingArgumentTable(uint32_t frameIndex);
		void createGPUCullingPipeline();
		void waitForAllFrameSlots();
		void ensureGPUCullingBuffers(PerFrameResources::FrameUserData& frameUserData, uint32_t requiredMeshCount);
		// Registers (first call) or rebinds (subsequent) a stable RHI BufferHandle to a
		// per-frame native buffer; clears the handle when the buffer is null.
		void rebindFrameBufferHandle(rhi::BufferHandle& handle, const utils::Buffer& buffer);
		void rebindFrameBufferHandle(rhi::BufferHandle& handle, DEMO_RHI_VK(Buffer) buffer);
		void updateGPUCullingBuffers(uint32_t frameIndex, const RenderParams& params);
		void createShadowCullingResources();
		void updateShadowCullingArgumentTable(uint32_t frameIndex);
		void updateShadowCullingDrawDataArgumentTable(uint32_t frameIndex);
		void updateMdiDrawDataArgumentTable(uint32_t frameIndex);
		void updateGBufferMdiDrawDataArgumentTable(uint32_t frameIndex);
		void updateDepthMdiDrawDataArgumentTable(uint32_t frameIndex);
		void createShadowCullingPipeline();
		void ensureShadowCullingBuffers(PerFrameResources::FrameUserData& frameUserData, uint32_t requiredMeshCount);
		void ensureMdiDrawDataBuffer(PerFrameResources::FrameUserData& frameUserData, uint32_t requiredDrawCount);
		void ensureGBufferMdiDrawDataBuffer(PerFrameResources::FrameUserData& frameUserData,
		                                    uint32_t requiredDrawCount);
		void ensureDepthMdiDrawDataBuffer(PerFrameResources::FrameUserData& frameUserData, uint32_t requiredDrawCount);
		void ensureGPUDrivenPersistentIndirectStreamBuffer(PerFrameResources::FrameUserData& frameUserData,
		                                                   uint32_t requiredDrawCount);
		void updateShadowCullingBuffers(uint32_t frameIndex, const RenderParams& params);
		void cacheGPUCullingStats(uint32_t frameIndex, bool readOverlayObjects);
		void drawGPUInfoOverlay(const RenderParams& params) const;
		void drawGPUCullingOverlay(const RenderParams& params) const;
		void createPassGpuProfileResources(const PassExecutor& passExecutor);
		void destroyPassGpuProfileResources();
		void resolvePassGpuProfileResults(uint32_t frameIndex);
		void resetPassGpuProfileQueries(rhi::CommandBuffer& cmdBuffer, uint32_t frameIndex);
		void writePassGpuProfileTimestamp(const PassContext& context, uint32_t passIndex, bool isBegin) const;
		void drawPassGpuProfileOverlay(const RenderParams& params) const;
		void initImGui(void* window);
		void createDescriptorPool();
		void createMaterialArgumentTable(); // Create material argument table early for pipeline layout
		void createGraphicsArgumentTables();
		void updateGraphicsArgumentTables();
		void syncMaterialArgumentTable(uint32_t frameIndex);
		rhi::ArgumentTableHandle getCurrentMaterialArgumentTable() const;
		void flushPendingUploadCommands(bool waitForCompletion);
		void createIBLResources(rhi::CommandBuffer& cmd);
		void destroyIBLResources();
		void destroyArgumentTablesAndLayouts();
		// Phase 3 (D-03): loadAndCreateImage returns RHI handles instead of utils::ImageResource.
		// texture + view are registered as owned=false (external); VMA release requires
		// destroyImageResource helper to be called separately in destroy path.
		struct LoadedImageHandles
		{
			rhi::TextureHandle     texture{};
			rhi::TextureViewHandle view{};
			utils::Image           nativeImage{}; // kept for VMA release (owned=false semantics)
			DEMO_RHI_VK(Extent2D) extent{};       // image dimensions for sourceExtent
		};
		LoadedImageHandles loadAndCreateImage(rhi::CommandBuffer& cmd, const std::string& filename);
		const MaterialResources::MaterialRecord* tryGetMaterial(MaterialHandle handle) const;
		const MaterialResources::TextureHotData* tryGetTextureHot(TextureHandle handle) const;
		const MaterialResources::TextureColdData* tryGetTextureCold(TextureHandle handle) const;
		// Build an ArgumentTable from a layout, track it for teardown, and return the handle.
		rhi::ArgumentTableHandle createArgumentTable(ArgumentTableDesc desc);
		rhi::ArgumentLayoutHandle createArgumentLayoutFromBindings(std::span<const rhi::ArgumentBinding> bindings,
		                                                           const char* debugName);
		// destroyArgumentTable is provided by public RHI interface
		void destroyPipelines();
		static std::optional<uint32_t> mapSetSlotToLegacyShaderSet(ArgumentSlot slot);
		[[nodiscard]] Aabb computeSceneBounds(const SceneUploadResult* gltfModel,
		                                      const GPUDrivenSceneView* gpuDrivenSceneView) const;
		[[nodiscard]] UploadBufferRecord createShadowPackedUploadBuffer(rhi::CommandBuffer& cmd,
		                                                                std::span<const std::byte> data,
		                                                                rhi::BufferUsageFlags usage,
		                                                                const char* debugName);
		void rebuildShadowPackedBuffers(const GltfModel& model, GltfUploadResult& result, rhi::CommandBuffer& cmd);
		void rebuildShadowPackedBuffers(const SceneAsset& asset, SceneUploadResult& result, rhi::CommandBuffer& cmd);
		[[nodiscard]] FrameLightingState buildFrameLightingState(const RenderParams& params) const;
		void ensureTestPointLights(const RenderParams& params);
		[[nodiscard]] shaderio::LightCullingUniforms buildLightCullingUniforms(const RenderParams& params) const;
		[[nodiscard]] shaderio::GPUCullingUniforms buildGPUCullingUniforms(
			const RenderParams& params, uint32_t objectCount) const;
		[[nodiscard]] std::array<glm::vec3, 8> computePerspectiveFrustumCorners(
			const shaderio::CameraUniforms& cameraUniforms,
			float nearDistance,
			float farDistance) const;
		[[nodiscard]] std::array<glm::vec3, 8> computeOrthoFrustumCorners(const glm::mat4& inverseViewProjection) const;
		void buildDebugDrawList(const RenderParams& params);
		[[nodiscard]] uint64_t DEMO_RENDER_RTV_BACKEND(rhi::TextureViewHandle handle) const;

		struct PassGpuProfileFrame
		{
			rhi::QueryPoolHandle queryPool{};
			std::vector<double> cpuPassDurationsMs;
			std::vector<double> passDurationsMs;
			bool valid{false};
			bool hasRecordedQueries{false};
		};

		struct PassGpuProfileState
		{
			float timestampPeriodNs{0.0f};
			uint32_t queryCount{0};
			std::vector<std::string> passNames;
			std::vector<double> latestCpuPassDurationsMs;
			std::vector<double> latestPassDurationsMs;
			bool latestValid{false};
			std::vector<PassGpuProfileFrame> frames;
			std::vector<uint64_t> currentCpuPassStartNs;
		};

		struct PassProfilingHooks final : PassExecutor::ExecutionHooks
		{
			explicit PassProfilingHooks(RenderDevice* owner)
				: renderer(owner)
			{
			}

			void beforePass(const PassContext& context, const PassNode& pass, uint32_t passIndex) const override;
			void afterPass(const PassContext& context, const PassNode& pass, uint32_t passIndex) const override;

			RenderDevice* renderer{nullptr};
		};

		DeviceLifetimeResources m_device;
		SwapchainDependentResources m_swapchainDependent;
		PerFrameResources m_perFrame;
		PerPassResources m_perPass;
		PerDrawData m_perDraw;
		MaterialResources m_materials;
		LightResources m_lightResources;
		std::vector<shaderio::LightData> m_testPointLights;
		std::vector<TestPointLightMotion> m_testPointLightMotions;
		Aabb m_testPointLightSceneBounds{};
		std::vector<shaderio::LightData> m_testSpotLights;
		FrameLightingState m_frameLightingState;
		DebugDrawList m_debugDrawList;
		shaderio::GPUCullStats m_lastGPUCullingStats{};
		shaderio::GPUCullDrawCounts m_lastGPUCullingDrawCounts{};
		const shaderio::GPUCullObject* m_externalGPUCullingOverlayObjects{nullptr};
		uint32_t m_externalGPUCullingOverlayObjectCount{0};
		std::vector<GPUCullOverlayObject> m_lastGPUCullingOverlayObjects;
		PassGpuProfileState m_passGpuProfile;
		PassProfilingHooks m_passProfilingHooks{this};
	};
} // namespace demo
