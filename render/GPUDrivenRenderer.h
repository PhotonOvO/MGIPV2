#pragma once

#include "GPUSceneRegistry.h"
#include "GPUDrivenLightResources.h"
#include "GPUMeshletBuffer.h"
#include "HiZDepthPyramid.h"
#include "IBLResources.h"
#include "MeshletConverter.h"
#include "passes/GPUDrivenCSMShadowPass.h"
#include "passes/GPUDrivenCullingPass.h"
#include "passes/GPUDrivenDebugPass.h"
#include "passes/GPUDrivenDepthPrepass.h"
#include "passes/GPUDrivenDepthPyramidPass.h"
#include "passes/GPUDrivenForwardPass.h"
#include "passes/GPUDrivenGBufferPass.h"
#include "passes/GPUDrivenImguiPass.h"
#include "passes/GPUDrivenLightCullingPass.h"
#include "passes/GPUDrivenClusteredLightCullingPass.h"
#include "passes/GPUDrivenAOPass.h"
#include "passes/GPUDrivenSSRPass.h"
#include "passes/GPUDrivenShadowAtlasPass.h"
#include "passes/GPUDrivenLightPass.h"
#include "passes/GPUDrivenSkyboxPass.h"
#include "passes/GPUDrivenBloomPrefilterPass.h"
#include "passes/GPUDrivenBloomDownsamplePass.h"
#include "passes/GPUDrivenFinalColorPass.h"
#include "passes/GPUDrivenVelocityPass.h"
#include "passes/GPUDrivenTAAResolvePass.h"
#include "passes/GPUDrivenPresentPass.h"
#include "passes/GPUDrivenVisibilitySortPass.h"
#include "RenderDevice.h"

#include <array>
#include <memory>
#include <string>
#include <unordered_map>
#include <vector>

namespace demo
{
	enum class GPUDrivenVisibilityOwnership : uint32_t
	{
		cpuBootstrap = 0,
		gpuSortCpuFeedback = 1,
		gpuOwned = 2,
	};

	enum class GPUDrivenOwnershipState : uint32_t
	{
		gpuOwned = 0,
		bridged = 1,
		legacy = 2,
		disabled = 3,
	};

	struct GPUDrivenPassDiagnostic
	{
		std::string name;
		GPUDrivenOwnershipState ownership{GPUDrivenOwnershipState::disabled};
		std::string note;
	};

	struct GPUDrivenResourceOwnershipSummary
	{
		GPUDrivenOwnershipState sceneAttachments{GPUDrivenOwnershipState::disabled};
		GPUDrivenOwnershipState depthPyramid{GPUDrivenOwnershipState::disabled};
		GPUDrivenOwnershipState visibility{GPUDrivenOwnershipState::disabled};
		GPUDrivenOwnershipState lightingResources{GPUDrivenOwnershipState::disabled};
		GPUDrivenOwnershipState shadowResources{GPUDrivenOwnershipState::disabled};
		GPUDrivenOwnershipState materialDescriptors{GPUDrivenOwnershipState::disabled};
	};

	struct GPUDrivenVisibilityDiagnostics
	{
		uint32_t safeObjectCount{0};
		uint32_t currentGPUCullingObjectCount{0};
		uint32_t previousGPUCullingObjectCount{0};
		uint32_t sortInputCount{0};
		uint32_t sortPaddedCount{0};
		uint32_t opaqueCapacity{0};
		uint32_t alphaCapacity{0};
		uint32_t transparentCapacity{0};
		uint32_t sameFrameOpaqueCapacity{0};
		uint32_t sameFrameAlphaCapacity{0};
		uint32_t sameFrameTransparentCapacity{0};
		uint32_t maxMobileTransparentDraws{0};
		bool depthUsesPreviousFrameIndirect{false};
		bool depthUsesSortedBootstrap{false};
		bool gbufferOpaqueAlphaPatchDispatched{false};
		bool transparentPatchDispatched{false};
		bool transparentOrderingCpuSeeded{false};
		bool materialSortKeysCpuSeeded{false};
		bool transparentCapacityOverflow{false};
	};

	struct GPUDrivenHiZDiagnostics
	{
		uint32_t sourceWidth{0};
		uint32_t sourceHeight{0};
		uint32_t pyramidWidth{0};
		uint32_t pyramidHeight{0};
		uint32_t mipCount{0};
		uint32_t fullMipCount{0};
		uint32_t policyDownsampleDivisor{0};
		uint32_t policyMaxMipCount{0};
		uint32_t policyMinMipSize{0};
		uint64_t estimatedMemoryBytes{0};
		uint64_t generation{0};
		bool valid{false};
		bool boundForGpuCulling{false};
		bool frustumCullingEnabled{false};
		bool occlusionCullingEnabled{false};
		bool meshletOcclusionEnabled{false};
		bool meshletConeCullingEnabled{false};
		float depthEpsilon{0.0f};
		float conservativeRadiusScale{0.0f};
		float conservativeRadiusBias{0.0f};
		float nearRejectEpsilon{0.0f};
		float largeObjectFootprintThreshold{0.0f};
		float fastCameraFallbackDistance{0.0f};
		float cameraDeltaDistance{0.0f};
		bool fastCameraFallbackTriggered{false};
	};

	struct GPUDrivenPostProcessDiagnostics
	{
		uint32_t displayWidth{0};
		uint32_t displayHeight{0};
		uint32_t internalWidth{0};
		uint32_t internalHeight{0};
		uint32_t outputWidth{0};
		uint32_t outputHeight{0};
		rhi::TextureFormat outputFormat{rhi::TextureFormat::undefined};
		rhi::TextureFormat sceneColorFormat{rhi::TextureFormat::undefined};
		rhi::TextureFormat recommendedHdrFormat{rhi::TextureFormat::rgba16Sfloat};
		std::string outputFormatName;
		std::string sceneColorFormatName;
		std::string recommendedHdrFormatName;
		uint64_t outputMemoryBytes{0};
		uint64_t sceneColorMemoryBytes{0};
		uint64_t recommendedHdrMemoryBytes{0};
		uint64_t bloomHalfQuarterMemoryBytes{0};
		float fixedExposure{1.0f};
		float adaptiveExposureTarget{0.18f};
		float minAutoExposure{0.25f};
		float maxAutoExposure{4.0f};
		float bloomIntensity{0.35f};
		float bloomThreshold{0.0f};
		float colorSaturation{1.0f};
		float colorContrast{1.0f};
		float colorGamma{1.0f};
		float colorLutStrength{0.0f};
		float vignetteIntensity{0.0f};
		float lensDirtIntensity{0.0f};
		float taaBlendWeight{0.90f};
		float taaJitterScale{1.0f};
		float renderScale{1.0f};
		uint32_t upscaleMode{0};
		bool hdrSceneColorActive{false};
		bool mobileHdrRecommended{true};
		bool toneMapInLightPass{true};
		bool finalColorPassActive{false};
		bool velocityBufferActive{false};
		bool taaPassActive{false};
		bool taaHistoryValid{false};
		bool temporalUpscalingActive{false};
		bool internalRenderScaleBlocked{false};
		bool exposurePassActive{false};
		bool adaptiveExposureActive{false};
		bool bloomPassActive{false};
		bool colorGradingLutActive{false};
		bool lensEffectsActive{false};
	};

	struct GPUDrivenIBLDiagnostics
	{
		uint32_t width{0};
		uint32_t height{0};
		uint32_t mipCount{0};
		rhi::TextureFormat format{rhi::TextureFormat::undefined};
		std::string formatName;
		uint64_t estimatedMemoryBytes{0};
		float intensity{0.0f};
		bool enabled{false};
		bool loaded{false};
		bool fallback{true};
		bool irradianceReady{false};
		bool prefilteredReady{false};
		bool brdfLutReady{false};
		bool splitSumReady{false};
		int debugMode{0};
		std::string path;
		std::string sourceMode;
		std::string status;
	};

	struct GPUDrivenClusteredLightingDiagnostics
	{
		uint32_t gridX{0};
		uint32_t gridY{0};
		uint32_t gridZ{0};
		uint32_t clusterCount{0};
		uint32_t maxLightsPerCluster{0};
		uint32_t activePointLights{0};
		uint32_t activeSpotLights{0};
		uint32_t maxPointLights{0};
		uint32_t maxSpotLights{0};
		uint32_t maxOccupancy{0};
		uint32_t overflowClusterCount{0};
		uint32_t appendedLightReferences{0};
		uint64_t clusterMemoryBytes{0};
		uint64_t lightMemoryBytes{0};
		bool enabled{false};
		bool resourcesOwned{false};
		bool descriptorsReady{false};
		bool fallbackActive{true};
	};

	struct GPUDrivenAOReflectionDiagnostics
	{
		uint32_t aoWidth{0};
		uint32_t aoHeight{0};
		uint32_t ssrWidth{0};
		uint32_t ssrHeight{0};
		uint64_t estimatedMemoryBytes{0};
		bool aoEnabled{false};
		bool aoReady{false};
		bool ssrEnabled{false};
		bool ssrReady{false};
	};

	struct GPUDrivenShadowAtlasDiagnostics
	{
		uint32_t atlasWidth{0};
		uint32_t atlasHeight{0};
		uint32_t tileSize{0};
		uint32_t tileCapacity{0};
		uint32_t allocatedTiles{0};
		uint64_t estimatedMemoryBytes{0};
		bool enabled{false};
		bool ready{false};
		bool fallbackToCSM{true};
		std::string status;
	};

	struct GPUDrivenRuntimeStats
	{
		uint32_t objectCount{0};
		uint32_t indirectDrawCount{0};
		uint32_t indirectCommandStride{0};
		uint32_t meshletCount{0};
		uint32_t meshletTriangleCount{0};
		uint32_t sceneUploadCount{0};
		uint32_t pendingSceneUpdates{0};
		GPUDrivenSceneAuthority authority{GPUDrivenSceneAuthority::none};
		GPUDrivenIndirectSourceKind indirectSource{GPUDrivenIndirectSourceKind::none};
		bool usesPersistentCullObjects{false};
		bool ownsFullRenderChain{false};
		bool ownsHiZVisibilityChain{false};
		uint64_t hiZGeneration{0};
		GPUDrivenVisibilityOwnership visibilityOwnership{GPUDrivenVisibilityOwnership::cpuBootstrap};
		shaderio::GPUBatchBuildStats batchStats{};
		GPUDrivenResourceOwnershipSummary resourceOwnership{};
		GPUDrivenVisibilityDiagnostics visibilityDiagnostics{};
		GPUDrivenHiZDiagnostics hiZDiagnostics{};
		GPUDrivenPostProcessDiagnostics postProcessDiagnostics{};
		GPUDrivenIBLDiagnostics iblDiagnostics{};
		GPUDrivenClusteredLightingDiagnostics clusteredLightingDiagnostics{};
		GPUDrivenAOReflectionDiagnostics aoReflectionDiagnostics{};
		GPUDrivenShadowAtlasDiagnostics shadowAtlasDiagnostics{};
		std::vector<GPUDrivenPassDiagnostic> passDiagnostics;
	};

	class GPUDrivenRenderer
	{
	public:
		[[nodiscard]] std::unique_ptr<rhi::Surface> createSurface() const { return m_renderer.createSurface(); }
		void init(void* window, rhi::Surface& surface, bool vSync);
		void shutdown(rhi::Surface& surface);
		void setVSync(bool enabled) { m_renderer.setVSync(enabled); }
		[[nodiscard]] bool getVSync() const { return m_renderer.getVSync(); }

		void setFullscreen(bool enabled, void* platformHandle = nullptr)
		{
			m_renderer.setFullscreen(enabled, platformHandle);
		}

		[[nodiscard]] const char* getSwapchainPresentModeName() const
		{
			return m_renderer.getSwapchainPresentModeName();
		}

		[[nodiscard]] uint32_t getSwapchainImageCount() const { return m_renderer.getSwapchainImageCount(); }
		void resize(rhi::Extent2D size);
		void beginUiFrame() { m_renderer.beginUiFrame(); }
		void render(const RenderParams& params);
		void setSceneRenderingSuspended(bool suspended) { m_suspendSceneRendering = suspended; }
		[[nodiscard]] bool isSceneRenderingSuspended() const { return m_suspendSceneRendering; }

		TextureHandle getViewportTextureHandle() const { return m_renderer.getViewportTextureHandle(); }
		ImTextureID getViewportTextureID(TextureHandle handle) const { return m_renderer.getViewportTextureID(handle); }
		MaterialHandle getMaterialHandle(uint32_t slot) const { return m_renderer.getMaterialHandle(slot); }
		GltfUploadResult uploadGltfModel(const GltfModel& model, rhi::CommandBuffer& cmd);
		SceneUploadResult commitSceneUploadPlan(const SceneAsset& asset, const SceneUploadPlan& plan,
		                                        rhi::CommandBuffer& cmd);
		void uploadGltfModelBatch(const GltfModel& model,
		                          std::span<const uint32_t> textureIndices,
		                          std::span<const uint32_t> materialIndices,
		                          std::span<const uint32_t> meshIndices,
		                          GltfUploadResult& ioResult,
		                          rhi::CommandBuffer& cmd);

		void initializeGltfUploadResult(const GltfModel& model, GltfUploadResult& outResult) const
		{
			m_renderer.initializeGltfUploadResult(model, outResult);
		}

		void destroyGltfResources(const GltfUploadResult& result);
		void updateMeshTransform(MeshHandle handle, const glm::mat4& transform);
		void updateSceneInstanceTransform(uint32_t instanceIndex, const glm::mat4& transform);

		void executeUploadCommand(std::function<void(rhi::CommandBuffer&)> uploadFn)
		{
			m_renderer.executeUploadCommand(std::move(uploadFn));
		}

		void waitForIdle() { m_renderer.waitForIdle(); }

		[[nodiscard]] const shaderio::GPUCullStats& getLastGPUCullingStats() const
		{
			return m_renderer.getLastGPUCullingStats();
		}

		[[nodiscard]] const shaderio::GPUCullDrawCounts& getLastGPUCullingDrawCounts() const
		{
			return m_renderer.getLastGPUCullingDrawCounts();
		}

		[[nodiscard]] const std::vector<GPUCullOverlayObject>& getLastGPUCullingOverlayObjects() const
		{
			return m_renderer.getLastGPUCullingOverlayObjects();
		}

		[[nodiscard]] const GPUDrivenRuntimeStats& getRuntimeStats() const { return m_runtimeStats; }
		[[nodiscard]] bool isTAAHistoryValid() const { return m_taaHistoryValid; }

		[[nodiscard]] RuntimeProfileSnapshot getRuntimeProfileSnapshot() const
		{
			return m_renderer.getRuntimeProfileSnapshot();
		}

		[[nodiscard]] const std::vector<shaderio::DebugLineVertex>& getDebugLineVertices() const
		{
			return m_renderer.getDebugLineVertices();
		}

		[[nodiscard]] rhi::BufferHandle getTransientBufferHandle(uint32_t frameIndex) const
		{
			return m_renderer.getTransientBufferHandle(frameIndex);
		}

		[[nodiscard]] shaderio::ShadowUniforms* getShadowUniformsData() { return m_renderer.getShadowUniformsData(); }
		[[nodiscard]] CSMShadowResources& getCSMShadowResources() { return m_renderer.getCSMShadowResources(); }

		[[nodiscard]] rhi::TextureViewHandle getCSMCascadeViewHandle(uint32_t cascadeIndex) const
		{
			return m_renderer.getCSMCascadeViewHandle(cascadeIndex);
		}

		[[nodiscard]] uint32_t getPersistentObjectCount() const { return m_sceneView.objectCount; }
		[[nodiscard]] bool isExperimentalMeshletPathEnabled() const { return m_enableExperimentalMeshletPath; }

		[[nodiscard]] bool isMeshletRenderingActive() const
		{
			return m_enableExperimentalMeshletPath && m_meshletBuffer.getMeshletCount() > 0u
				&& m_meshletBuffer.getMeshletIndexBufferHandle() != 0;
		}

		[[nodiscard]] uint64_t getMeshletIndexBufferHandle() const
		{
			return m_meshletBuffer.getMeshletIndexBufferHandle();
		}

		[[nodiscard]] rhi::BufferHandle getMeshletIndexBufferRHIHandle() const
		{
			return m_meshletBuffer.getMeshletIndexBufferRHIHandle();
		}

		// Stable RHI handles for the scene's shadow packed vertex/index buffers (rebound
		// when the active upload result changes). Consumed by CSM / shadow-atlas passes.
		[[nodiscard]] rhi::BufferHandle getShadowPackedVertexBufferRHIHandle() const
		{
			return m_shadowPackedVertexBufferRHI;
		}

		[[nodiscard]] rhi::BufferHandle getShadowPackedIndexBufferRHIHandle() const
		{
			return m_shadowPackedIndexBufferRHI;
		}

		[[nodiscard]] uintptr_t getMeshletDataBuffer() const
		{
			return m_meshletBuffer.getMeshletDataBuffer();
		}

		[[nodiscard]] bool tryGetMeshDrawIndex(MeshHandle handle, uint32_t& outDrawIndex) const;
		[[nodiscard]] bool tryGetMeshHandleForDrawIndex(uint32_t drawIndex, MeshHandle& outHandle) const;
		[[nodiscard]] std::span<const uint32_t> getOpaqueDrawIndices() const { return m_opaqueDrawIndices; }
		[[nodiscard]] std::span<const uint32_t> getAlphaTestDrawIndices() const { return m_alphaTestDrawIndices; }
		[[nodiscard]] std::span<const uint32_t> getTransparentDrawIndices() const { return m_transparentDrawIndices; }
		[[nodiscard]] MeshPool& getMeshPool() { return m_renderer.getMeshPool(); }
		[[nodiscard]] const MeshPool& getMeshPool() const { return m_renderer.getMeshPool(); }

		[[nodiscard]] PipelineHandle getDepthPrepassOpaquePipelineHandle() const
		{
			return m_renderer.getDepthPrepassOpaquePipelineHandle();
		}

		[[nodiscard]] PipelineHandle getDepthPrepassAlphaTestPipelineHandle() const
		{
			return m_renderer.getDepthPrepassAlphaTestPipelineHandle();
		}

		[[nodiscard]] PipelineHandle getDepthPrepassOpaqueMDIPipelineHandle() const
		{
			return m_renderer.getDepthPrepassOpaqueMDIPipelineHandle();
		}

		[[nodiscard]] PipelineHandle getDepthPrepassAlphaTestMDIPipelineHandle() const
		{
			return m_renderer.getDepthPrepassAlphaTestMDIPipelineHandle();
		}

		[[nodiscard]] PipelineHandle getGBufferOpaquePipelineHandle() const
		{
			return m_renderer.getGBufferOpaquePipelineHandle();
		}

		[[nodiscard]] PipelineHandle getGBufferAlphaTestPipelineHandle() const
		{
			return m_renderer.getGBufferAlphaTestPipelineHandle();
		}

		[[nodiscard]] PipelineHandle getGBufferOpaqueMDIPipelineHandle() const
		{
			return m_renderer.getGBufferOpaqueMDIPipelineHandle();
		}

		[[nodiscard]] PipelineHandle getGBufferAlphaTestMDIPipelineHandle() const
		{
			return m_renderer.getGBufferAlphaTestMDIPipelineHandle();
		}

		[[nodiscard]] PipelineHandle getLightPipelineHandle() const { return m_gpuDrivenLightHdrPipeline; }

		[[nodiscard]] PipelineHandle getGPUDrivenLightHdrPipelineHandle() const
		{
			return m_gpuDrivenLightHdrPipeline;
		}

		[[nodiscard]] PipelineHandle getGPUDrivenSkyboxPipelineHandle() const
		{
			return m_gpuDrivenSkyboxPipeline;
		}

		[[nodiscard]] PipelineHandle getBloomPrefilterPipelineHandle() const
		{
			return m_gpuDrivenBloomPrefilterPipeline;
		}

		[[nodiscard]] PipelineHandle getBloomDownsamplePipelineHandle() const
		{
			return m_gpuDrivenBloomDownsamplePipeline;
		}

		[[nodiscard]] PipelineHandle getBloomUpsamplePipelineHandle() const
		{
			return m_gpuDrivenBloomUpsamplePipeline;
		}

		[[nodiscard]] PipelineHandle getFinalColorPipelineHandle() const
		{
			return m_gpuDrivenFinalColorPipeline;
		}

		[[nodiscard]] PipelineHandle getVelocityPipelineHandle() const
		{
			return m_gpuDrivenVelocityPipeline;
		}

		[[nodiscard]] PipelineHandle getTAAResolvePipelineHandle() const
		{
			return m_gpuDrivenTAAResolvePipeline;
		}

		[[nodiscard]] PipelineHandle getForwardMDIPipelineHandle() const
		{
			return m_renderer.getForwardMDIPipelineHandle();
		}

		[[nodiscard]] PipelineHandle getCSMShadowPipelineHandle() const
		{
			return m_renderer.getCSMShadowPipelineHandle();
		}

		[[nodiscard]] PipelineHandle getShadowCullingPipelineHandle() const
		{
			return m_renderer.getShadowCullingPipelineHandle();
		}

		[[nodiscard]] PipelineHandle getDebugPipelineHandle() const { return m_renderer.getDebugPipelineHandle(); }

		[[nodiscard]] PipelineHandle getGPUCullingDebugPipelineHandle() const
		{
			return m_renderer.getGPUCullingDebugPipelineHandle();
		}

		[[nodiscard]] PipelineHandle getGPUCullingPipelineHandle() const
		{
			return m_renderer.getGPUCullingPipelineHandle();
		}

		[[nodiscard]] rhi::ArgumentTableHandle getGPUCullingArgumentTable(uint32_t frameIndex) const
		{
			return m_renderer.getGPUCullingArgumentTable(frameIndex);
		}

		[[nodiscard]] rhi::ArgumentTableHandle getShadowCullingArgumentTable(uint32_t frameIndex) const
		{
			return m_renderer.getShadowCullingArgumentTable(frameIndex);
		}

		[[nodiscard]] PipelineHandle getLightCullingPipelineHandle() const { return m_pointLightCoarseCullingPipeline; }

		[[nodiscard]] PipelineHandle getSpotLightCullingPipelineHandle() const
		{
			return m_spotLightCoarseCullingPipeline;
		}

		[[nodiscard]] rhi::ArgumentTableHandle getGraphicsMaterialArgumentTable() const
		{
			return m_renderer.getGraphicsMaterialArgumentTable();
		}

		[[nodiscard]] bool getIBLEnvironmentLoaded() const { return m_iblEnvironmentLoaded; }
		[[nodiscard]] bool getIBLUsingFallback() const { return m_iblUsingFallback; }
		[[nodiscard]] rhi::TextureFormat getIBLEnvironmentFormat() const;

		[[nodiscard]] rhi::Extent2D getIBLEnvironmentExtent() const
		{
			return rhi::Extent2D{m_iblEnvironmentExtent.width, m_iblEnvironmentExtent.height};
		}

		[[nodiscard]] uint32_t getIBLEnvironmentMipCount() const { return m_iblEnvironmentMipCount; }
		[[nodiscard]] uint64_t getIBLEnvironmentEstimatedBytes() const { return m_iblEnvironmentEstimatedBytes; }
		[[nodiscard]] const std::string& getIBLEnvironmentPath() const { return m_iblEnvironmentPath; }
		[[nodiscard]] const std::string& getIBLEnvironmentStatus() const { return m_iblEnvironmentStatus; }

		[[nodiscard]] rhi::ArgumentTableHandle getLightCoarseCullingArgumentTable(uint32_t frameIndex) const
		{
			return frameIndex < m_lightCoarseCullingArgumentTables.size()
				       ? m_lightCoarseCullingArgumentTables[frameIndex]
				       : rhi::ArgumentTableHandle{};
		}

		[[nodiscard]] rhi::ArgumentTableHandle getCurrentLightCullingArgumentTable() const;

		[[nodiscard]] PipelineHandle getClusteredLightCullingPipelineHandle() const
		{
			return m_clusteredLightCullingPipeline;
		}

		[[nodiscard]] uint64_t getClusterStatsBufferOpaque(uint32_t frameIndex) const
		{
			const rhi::BufferHandle handle = m_lightResources.getClusterStatsBuffer(frameIndex);
			return (static_cast<uint64_t>(handle.generation) << 32u) | handle.index;
		}

		// Stats buffer (binding 8) registered as owned=false handle alongside the coarse-culling table.
		[[nodiscard]] rhi::BufferHandle getClusterStatsBufferHandle(uint32_t frameIndex) const
		{
			const size_t idx = static_cast<size_t>(frameIndex) * 9u + 8u;
			return idx < m_lightCoarseCullingBufferHandles.size()
				       ? m_lightCoarseCullingBufferHandles[idx]
				       : rhi::BufferHandle{};
		}

		[[nodiscard]] rhi::Extent2D getPhase7HalfExtent() const
		{
			return rhi::Extent2D{m_phase7HalfExtent.width, m_phase7HalfExtent.height};
		}

		[[nodiscard]] uint64_t getSceneViewOutputImageOpaque() const
		{
			return (static_cast<uint64_t>(m_sceneView.outputImage.generation) << 32u) | m_sceneView.outputImage.index;
		}

		[[nodiscard]] rhi::Extent2D getSceneViewDepthExtent() const { return m_sceneView.sceneDepthExtent; }
		[[nodiscard]] uint64_t getAOTracePipelineOpaque() const { return 0; }
		[[nodiscard]] uint64_t getAODenoisePipelineOpaque() const { return 0; }
		[[nodiscard]] uint64_t getAORawImageOpaque() const { return reinterpret_cast<uint64_t>(m_aoRaw.image); }

		[[nodiscard]] uint64_t getAODenoisedImageOpaque() const
		{
			return reinterpret_cast<uint64_t>(m_aoDenoised.image);
		}

		[[nodiscard]] PipelineHandle getAOTracePipelineHandle() const { return m_gtaoPipelineHandle; }
		[[nodiscard]] PipelineHandle getAODenoisePipelineHandle() const { return m_aoDenoisePipelineHandle; }

		[[nodiscard]] rhi::ArgumentTableHandle getAOArgumentTable(uint32_t frameIndex) const
		{
			return frameIndex < m_aoArgumentTables.size() ? m_aoArgumentTables[frameIndex] : rhi::ArgumentTableHandle{};
		}

		[[nodiscard]] rhi::ArgumentTableHandle getAODenoiseArgumentTable(uint32_t frameIndex) const
		{
			return frameIndex < m_aoDenoiseArgumentTables.size()
				       ? m_aoDenoiseArgumentTables[frameIndex]
				       : rhi::ArgumentTableHandle{};
		}

		[[nodiscard]] uint64_t getSSRTracePipelineOpaque() const { return 0; }
		[[nodiscard]] uint64_t getSSRRawImageOpaque() const { return reinterpret_cast<uint64_t>(m_ssrRaw.image); }
		[[nodiscard]] PipelineHandle getSSRTracePipelineHandle() const { return m_ssrTracePipelineHandle; }
		// Builds the SSR compute set as a per-frame temporary argument table (gbuffer/depth/history
		// sampled images + ssrRaw storage image + the caller's camera UBO slice). Returns a
		// frame-lifetime handle; do not cache it across frames.
		[[nodiscard]] rhi::ArgumentTableHandle
		acquireSSRTempArgumentTable(uint64_t cameraBuffer, uint32_t cameraOffset);

		// Opaque snapshot of the per-frame bitonic visibility-sort resources, so the
		// visibility-sort pass can record without reaching into renderer internals.
		struct VisibilitySortDispatch
		{
			PipelineHandle pipelineHandle{};
			rhi::ArgumentTableHandle argumentTable{};
			rhi::BufferHandle uploadKeyBufferHandle{};
			rhi::BufferHandle uploadValueBufferHandle{};
			rhi::BufferHandle keyBufferHandle{};
			rhi::BufferHandle valueBufferHandle{};
			uint32_t paddedElementCount{0};
			bool valid{false};
		};

		[[nodiscard]] VisibilitySortDispatch getVisibilitySortDispatch(uint32_t frameIndex) const
		{
			VisibilitySortDispatch info{};
			if (m_visibilitySortPipelineHandle.isNull() || frameIndex >= m_visibilitySortFrames.size())
			{
				return info;
			}
			const VisibilitySortFrameResources& f = m_visibilitySortFrames[frameIndex];
			info.pipelineHandle = m_visibilitySortPipelineHandle;
			info.argumentTable = f.argumentTable;
			info.uploadKeyBufferHandle = f.uploadKeyBufferHandle;
			info.uploadValueBufferHandle = f.uploadValueBufferHandle;
			info.keyBufferHandle = f.keyBufferHandle;
			info.valueBufferHandle = f.valueBufferHandle;
			info.paddedElementCount = f.paddedElementCount;
			info.valid = !f.argumentTable.isNull() && f.paddedElementCount > 1u
				&& reinterpret_cast<uintptr_t>(f.uploadKeyBuffer.buffer) != 0
				&& reinterpret_cast<uintptr_t>(f.uploadValueBuffer.buffer) != 0;
			return info;
		}

		[[nodiscard]] rhi::ArgumentTableHandle getCSMShadowMDIDrawArgumentTable(
			uint32_t frameIndex, uint32_t cascadeIndex) const
		{
			return m_renderer.getCSMShadowMDIDrawArgumentTable(frameIndex, cascadeIndex);
		}

		[[nodiscard]] rhi::ArgumentTableHandle getCameraArgumentTable(uint32_t frameIndex) const
		{
			return m_renderer.getCameraArgumentTable(frameIndex);
		}

		[[nodiscard]] rhi::ArgumentTableHandle getDrawArgumentTable(uint32_t frameIndex) const
		{
			return m_renderer.getDrawArgumentTable(frameIndex);
		}

		[[nodiscard]] rhi::ArgumentTableHandle getMDIDrawArgumentTable(uint32_t frameIndex) const
		{
			return m_renderer.getMDIDrawArgumentTable(frameIndex);
		}

		[[nodiscard]] rhi::ArgumentTableHandle getGBufferMDIDrawArgumentTable(uint32_t frameIndex) const
		{
			return m_renderer.getGBufferMDIDrawArgumentTable(frameIndex);
		}

		[[nodiscard]] rhi::ArgumentTableHandle getDepthMDIDrawArgumentTable(uint32_t frameIndex) const
		{
			return m_renderer.getDepthMDIDrawArgumentTable(frameIndex);
		}

		[[nodiscard]] uint64_t getPreviousGPUCullingIndirectBufferOpaque(uint32_t frameIndex) const
		{
			return m_renderer.getPreviousGPUCullingIndirectBufferOpaque(frameIndex);
		}

		[[nodiscard]] uint64_t getPreviousGPUCullingDrawCountBufferOpaque(uint32_t frameIndex) const
		{
			return m_renderer.getPreviousGPUCullingDrawCountBufferOpaque(frameIndex);
		}

		[[nodiscard]] rhi::BufferHandle getPreviousGPUCullingIndirectBufferRHIHandle(uint32_t frameIndex) const
		{
			return m_renderer.getPreviousGPUCullingIndirectBufferRHIHandle(frameIndex);
		}

		[[nodiscard]] rhi::BufferHandle getPreviousGPUCullingDrawCountBufferRHIHandle(uint32_t frameIndex) const
		{
			return m_renderer.getPreviousGPUCullingDrawCountBufferRHIHandle(frameIndex);
		}

		[[nodiscard]] uint32_t getPreviousGPUCullingObjectCount(uint32_t frameIndex) const
		{
			return m_renderer.getPreviousGPUCullingObjectCount(frameIndex, nullptr);
		}

		[[nodiscard]] uint64_t getGPUCullingIndirectBufferOpaque(uint32_t frameIndex) const
		{
			return m_renderer.getGPUCullingIndirectBufferOpaque(frameIndex);
		}

		[[nodiscard]] uint64_t getGPUCullingDrawCountBufferOpaque(uint32_t frameIndex) const
		{
			return m_renderer.getGPUCullingDrawCountBufferOpaque(frameIndex);
		}

		[[nodiscard]] rhi::BufferHandle getGPUCullingIndirectBufferRHIHandle(uint32_t frameIndex) const
		{
			return m_renderer.getGPUCullingIndirectBufferRHIHandle(frameIndex);
		}

		[[nodiscard]] rhi::BufferHandle getGPUCullingDrawCountBufferRHIHandle(uint32_t frameIndex) const
		{
			return m_renderer.getGPUCullingDrawCountBufferRHIHandle(frameIndex);
		}

		[[nodiscard]] uint64_t getShadowCullingIndirectBufferOpaque(uint32_t frameIndex) const
		{
			return m_renderer.getShadowCullingIndirectBufferOpaque(frameIndex);
		}

		[[nodiscard]] rhi::BufferHandle getShadowCullingIndirectBufferRHIHandle(uint32_t frameIndex) const
		{
			return m_renderer.getShadowCullingIndirectBufferRHIHandle(frameIndex);
		}

		[[nodiscard]] uint32_t getShadowCullingMeshCapacity(uint32_t frameIndex) const
		{
			return m_renderer.getShadowCullingMeshCapacity(frameIndex);
		}

		[[nodiscard]] uint32_t getSafePersistentObjectCount() const;

		[[nodiscard]] uint64_t getShadowAtlasImageOpaque() const
		{
			return reinterpret_cast<uint64_t>(m_shadowAtlas.image);
		}

		[[nodiscard]] uint64_t getShadowAtlasViewOpaque() const
		{
			return reinterpret_cast<uint64_t>(m_shadowAtlas.view);
		}

		[[nodiscard]] rhi::TextureViewHandle getShadowAtlasViewHandle() const { return m_shadowAtlasViewHandle; }

		[[nodiscard]] rhi::Extent2D getShadowAtlasExtent() const
		{
			return rhi::Extent2D{m_shadowAtlasExtent.width, m_shadowAtlasExtent.height};
		}

		[[nodiscard]] uint32_t getShadowAtlasTileSize() const { return m_shadowAtlasTileSize; }
		void setShadowAtlasAllocatedTiles(uint32_t tiles) { m_shadowAtlasAllocatedTiles = tiles; }

		[[nodiscard]] uint64_t getGPUCullingObjectBufferAddress(uint32_t frameIndex) const
		{
			return m_renderer.getGPUCullingObjectBufferAddress(frameIndex);
		}

		[[nodiscard]] uint64_t getGPUCullingResultBufferAddress(uint32_t frameIndex) const
		{
			return m_renderer.getGPUCullingResultBufferAddress(frameIndex);
		}

		[[nodiscard]] uint32_t getGPUCullingObjectCount(uint32_t frameIndex) const
		{
			return m_renderer.getGPUCullingObjectCount(frameIndex);
		}

		[[nodiscard]] shaderio::ShadowCullPushConstants buildShadowCullPushConstants(uint32_t cascadeIndex,
			uint32_t objectCount) const
		{
			return m_renderer.buildShadowCullPushConstants(cascadeIndex, objectCount);
		}

		[[nodiscard]] uint32_t getGPUCullingIndirectCommandStride() const
		{
			return m_renderer.getGPUCullingIndirectCommandStride();
		}

		[[nodiscard]] uint64_t getGPUDrivenPersistentIndirectStreamBuffer(uint32_t frameIndex) const
		{
			return m_renderer.getGPUDrivenPersistentIndirectStreamBuffer(frameIndex);
		}

		[[nodiscard]] rhi::BufferHandle getGPUDrivenPersistentIndirectStreamBufferRHIHandle(uint32_t frameIndex) const
		{
			return m_renderer.getGPUDrivenPersistentIndirectStreamBufferRHIHandle(frameIndex);
		}

		[[nodiscard]] uint64_t getPreviousGPUDrivenPersistentIndirectStreamBuffer(uint32_t frameIndex) const
		{
			return m_renderer.getGPUDrivenPersistentIndirectStreamBuffer(getPreviousFrameIndex(frameIndex));
		}

		[[nodiscard]] rhi::BufferHandle getPreviousGPUDrivenPersistentIndirectStreamBufferRHIHandle(
			uint32_t frameIndex) const
		{
			return m_renderer.getGPUDrivenPersistentIndirectStreamBufferRHIHandle(getPreviousFrameIndex(frameIndex));
		}

		[[nodiscard]] bool getPreviousSortedBootstrapState(uint32_t frameIndex,
		                                                   uint32_t& outOpaqueCapacity,
		                                                   uint32_t& outAlphaCapacity) const;
		void invalidateSortedBootstrapStateForFrame(uint32_t frameIndex) { invalidateSortedBootstrapState(frameIndex); }

		void publishSortedBootstrapStateForFrame(uint32_t frameIndex, uint32_t opaqueCapacity, uint32_t alphaCapacity)
		{
			recordSortedBootstrapState(frameIndex, opaqueCapacity, alphaCapacity);
		}

		void recordDepthPrepassVisibilitySource(bool usedPreviousFrameIndirect,
		                                        bool usedSortedBootstrap,
		                                        uint32_t previousObjectCount,
		                                        uint32_t opaqueMaxDrawCount,
		                                        uint32_t alphaMaxDrawCount);
		void recordGBufferVisibilityPatch(bool patched, uint32_t opaqueCapacity, uint32_t alphaCapacity);
		void recordForwardVisibilityPatch(bool patched, uint32_t transparentCapacity, uint32_t totalPersistentCapacity);

		[[nodiscard]] uint64_t getForwardMDIIndirectBuffer(uint32_t frameIndex) const
		{
			return m_renderer.getForwardMDIIndirectBuffer(frameIndex);
		}

		[[nodiscard]] uint32_t getActivePointLightCount() const { return m_lightResources.getActivePointLightCount(); }
		[[nodiscard]] uint32_t getActiveSpotLightCount() const { return m_lightResources.getActiveSpotLightCount(); }
		[[nodiscard]] rhi::Extent2D getSceneExtent() const { return m_renderer.getSceneExtent(); }
		[[nodiscard]] rhi::TextureFormat getSceneDepthFormat() const { return m_renderer.getSceneDepthFormat(); }
		[[nodiscard]] rhi::TextureHandle getSceneDepthImage() const { return m_renderer.getSceneDepthImage(); }

		[[nodiscard]] rhi::TextureViewHandle getSceneDepthImageView() const
		{
			return m_renderer.getSceneDepthImageView();
		}

		[[nodiscard]] rhi::TextureHandle getSceneGBufferImage(uint32_t index) const
		{
			return m_renderer.getSceneGBufferImage(index);
		}

		[[nodiscard]] rhi::TextureViewHandle getSceneGBufferImageView(uint32_t index) const
		{
			return m_renderer.getSceneGBufferImageView(index);
		}

		[[nodiscard]] rhi::TextureHandle getOutputTextureImage() const { return m_renderer.getOutputTextureImage(); }
		[[nodiscard]] rhi::TextureViewHandle getOutputTextureView() const { return m_renderer.getOutputTextureView(); }
		[[nodiscard]] rhi::TextureFormat getOutputTextureFormat() const { return m_renderer.getOutputTextureFormat(); }

		[[nodiscard]] uint64_t getOutputTextureEstimatedBytes() const
		{
			return m_renderer.getOutputTextureEstimatedBytes();
		}

		[[nodiscard]] rhi::TextureHandle getSceneColorHdrImage() const { return m_renderer.getSceneColorHdrImage(); }
		[[nodiscard]] rhi::TextureViewHandle getSceneColorHdrView() const { return m_renderer.getSceneColorHdrView(); }
		[[nodiscard]] rhi::TextureFormat getSceneColorHdrFormat() const { return m_renderer.getSceneColorHdrFormat(); }

		[[nodiscard]] uint64_t getSceneColorHdrEstimatedBytes() const
		{
			return m_renderer.getSceneColorHdrEstimatedBytes();
		}

		[[nodiscard]] rhi::TextureHandle getBloomHalfImage() const { return m_renderer.getBloomHalfImage(); }
		[[nodiscard]] rhi::TextureViewHandle getBloomHalfView() const { return m_renderer.getBloomHalfView(); }
		[[nodiscard]] rhi::Extent2D getBloomHalfExtent() const { return m_renderer.getBloomHalfExtent(); }
		[[nodiscard]] rhi::TextureHandle getBloomQuarterImage() const { return m_renderer.getBloomQuarterImage(); }
		[[nodiscard]] rhi::TextureViewHandle getBloomQuarterView() const { return m_renderer.getBloomQuarterView(); }
		[[nodiscard]] rhi::Extent2D getBloomQuarterExtent() const { return m_renderer.getBloomQuarterExtent(); }
		[[nodiscard]] rhi::TextureHandle getBloomEighthImage() const { return m_renderer.getBloomEighthImage(); }
		[[nodiscard]] rhi::TextureViewHandle getBloomEighthView() const { return m_renderer.getBloomEighthView(); }
		[[nodiscard]] rhi::Extent2D getBloomEighthExtent() const { return m_renderer.getBloomEighthExtent(); }
		[[nodiscard]] rhi::TextureHandle getBloomSixteenthImage() const { return m_renderer.getBloomSixteenthImage(); }

		[[nodiscard]] rhi::TextureViewHandle getBloomSixteenthView() const
		{
			return m_renderer.getBloomSixteenthView();
		}

		[[nodiscard]] rhi::Extent2D getBloomSixteenthExtent() const { return m_renderer.getBloomSixteenthExtent(); }

		[[nodiscard]] rhi::TextureHandle getBloomThirtySecondImage() const
		{
			return m_renderer.getBloomThirtySecondImage();
		}

		[[nodiscard]] rhi::TextureViewHandle getBloomThirtySecondView() const
		{
			return m_renderer.getBloomThirtySecondView();
		}

		[[nodiscard]] rhi::Extent2D getBloomThirtySecondExtent() const
		{
			return m_renderer.getBloomThirtySecondExtent();
		}

		[[nodiscard]] rhi::TextureHandle getBloomUpsampleSixteenthImage() const
		{
			return m_renderer.getBloomUpsampleSixteenthImage();
		}

		[[nodiscard]] rhi::TextureViewHandle getBloomUpsampleSixteenthView() const
		{
			return m_renderer.getBloomUpsampleSixteenthView();
		}

		[[nodiscard]] rhi::Extent2D getBloomUpsampleSixteenthExtent() const
		{
			return m_renderer.getBloomUpsampleSixteenthExtent();
		}

		[[nodiscard]] rhi::TextureHandle getBloomUpsampleEighthImage() const
		{
			return m_renderer.getBloomUpsampleEighthImage();
		}

		[[nodiscard]] rhi::TextureViewHandle getBloomUpsampleEighthView() const
		{
			return m_renderer.getBloomUpsampleEighthView();
		}

		[[nodiscard]] rhi::Extent2D getBloomUpsampleEighthExtent() const
		{
			return m_renderer.getBloomUpsampleEighthExtent();
		}

		[[nodiscard]] rhi::TextureHandle getBloomUpsampleQuarterImage() const
		{
			return m_renderer.getBloomUpsampleQuarterImage();
		}

		[[nodiscard]] rhi::TextureViewHandle getBloomUpsampleQuarterView() const
		{
			return m_renderer.getBloomUpsampleQuarterView();
		}

		[[nodiscard]] rhi::Extent2D getBloomUpsampleQuarterExtent() const
		{
			return m_renderer.getBloomUpsampleQuarterExtent();
		}

		[[nodiscard]] rhi::TextureHandle getBloomOutputImage() const { return m_renderer.getBloomOutputImage(); }
		[[nodiscard]] rhi::TextureViewHandle getBloomOutputView() const { return m_renderer.getBloomOutputView(); }
		[[nodiscard]] rhi::Extent2D getBloomOutputExtent() const { return m_renderer.getBloomOutputExtent(); }

		[[nodiscard]] rhi::TextureHandle getColorGradingLutImage() const
		{
			return m_renderer.getColorGradingLutImage();
		}

		[[nodiscard]] rhi::TextureViewHandle getColorGradingLutView() const
		{
			return m_renderer.getColorGradingLutView();
		}

		[[nodiscard]] rhi::Extent2D getColorGradingLutExtent() const { return m_renderer.getColorGradingLutExtent(); }
		[[nodiscard]] uint64_t getBloomEstimatedBytes() const { return m_renderer.getBloomEstimatedBytes(); }
		[[nodiscard]] rhi::TextureHandle getVelocityImage() const { return m_renderer.getVelocityImage(); }
		[[nodiscard]] rhi::TextureViewHandle getVelocityView() const { return m_renderer.getVelocityView(); }
		[[nodiscard]] rhi::TextureFormat getVelocityFormat() const { return m_renderer.getVelocityFormat(); }
		[[nodiscard]] uint64_t getVelocityEstimatedBytes() const { return m_renderer.getVelocityEstimatedBytes(); }
		[[nodiscard]] glm::vec2 getCurrentTAAJitterUv() const { return m_currentTAAJitterUv; }
		[[nodiscard]] glm::vec2 getPreviousTAAJitterUv() const { return m_previousTAAJitterUv; }

		[[nodiscard]] rhi::TextureHandle getSceneColorHistoryImage(uint32_t index) const
		{
			return m_renderer.getSceneColorHistoryImage(index);
		}

		[[nodiscard]] rhi::TextureViewHandle getSceneColorHistoryView(uint32_t index) const
		{
			return m_renderer.getSceneColorHistoryView(index);
		}

		[[nodiscard]] uint64_t getSceneColorHistoryEstimatedBytes() const
		{
			return m_renderer.getSceneColorHistoryEstimatedBytes();
		}

		[[nodiscard]] rhi::Extent2D getSwapchainExtent() const { return m_renderer.getSwapchainExtent(); }

		[[nodiscard]] rhi::TextureHandle getCurrentSwapchainTextureRHIHandle() const
		{
			return m_renderer.getCurrentSwapchainTextureHandle();
		}

		[[nodiscard]] rhi::TextureHandle getPassOutputTextureRHIHandle() const
		{
			return m_passExecutor.getTextureRHIHandle(kPassOutputHandle);
		}

		[[nodiscard]] uint32_t getCurrentFrameIndexHint() const { return m_renderer.getCurrentFrameIndexHint(); }
		[[nodiscard]] uintptr_t getBackendDeviceToken() const;
		[[nodiscard]] rhi::Device& getRHIDevice() const { return m_renderer.getRHIDevice(); }
		[[nodiscard]] uintptr_t getAllocatorToken() const;
		// ArgumentTable wrapping the per-frame lighting-scene descriptor set (set LSetScene).
		[[nodiscard]] rhi::ArgumentTableHandle getLightingSceneArgumentTable(uint32_t frameIndex) const
		{
			return frameIndex < m_lightingSceneArgumentTables.size()
				       ? m_lightingSceneArgumentTables[frameIndex]
				       : rhi::ArgumentTableHandle{};
		}

		// ArgumentTable wrapping the GBuffer/lighting-input texture set (set LSetTextures).
		[[nodiscard]] rhi::ArgumentTableHandle getLightingInputArgumentTable(uint32_t frameIndex) const
		{
			return frameIndex < m_lightingInputArgumentTables.size()
				       ? m_lightingInputArgumentTables[frameIndex]
				       : rhi::ArgumentTableHandle{};
		}

		// rhi-typed render targets for the fullscreen screen-space passes (skybox, etc.),
		// so passes never name a Vulkan type. Native handles are read from m_sceneView here.
		struct ScreenPassTargets
		{
			uint64_t colorImage{0};
			rhi::TextureViewHandle colorView{};
			uint64_t depthImage{0};
			rhi::TextureViewHandle depthView{};
			rhi::TextureAspect depthAspect{rhi::TextureAspect::depth};
			rhi::Extent2D extent{};
			bool valid{false};
		};

		[[nodiscard]] ScreenPassTargets getScreenColorDepthTargets() const
		{
			ScreenPassTargets targets{};
			const bool ready = !m_sceneView.sceneColorHdrImage.isNull() && !m_sceneView.sceneColorHdrView.isNull()
				&& !m_sceneView.sceneDepthImage.isNull() && !m_sceneView.sceneDepthView.isNull()
				&& m_sceneView.sceneDepthExtent.width != 0u && m_sceneView.sceneDepthExtent.height != 0u;
			if (!ready)
			{
				return targets;
			}
			targets.colorImage = (static_cast<uint64_t>(m_sceneView.sceneColorHdrImage.generation) << 32u)
				| m_sceneView.sceneColorHdrImage.index;
			targets.colorView = m_sceneView.sceneColorHdrView;
			targets.depthImage = (static_cast<uint64_t>(m_sceneView.sceneDepthImage.generation) << 32u)
				| m_sceneView.sceneDepthImage.index;
			targets.depthView = m_sceneView.sceneDepthView;
			targets.depthAspect = depthAspectForFormat(m_sceneView.sceneDepthFormat);
			targets.extent = {m_sceneView.sceneDepthExtent.width, m_sceneView.sceneDepthExtent.height};
			targets.valid = true;
			return targets;
		}

		[[nodiscard]] static rhi::TextureAspect depthAspectForFormat(rhi::TextureFormat format)
		{
			switch (format)
			{
			case rhi::TextureFormat::d24UnormS8:
			case rhi::TextureFormat::d32SfloatS8:
				return rhi::TextureAspect::depthStencil;
			default:
				return rhi::TextureAspect::depth;
			}
		}

		void uploadSharedMDIDrawData(uint32_t frameIndex, std::span<const shaderio::DrawUniforms> drawData)
		{
			m_renderer.uploadMDIDrawData(frameIndex, drawData);
		}

		void uploadDepthMDIDrawData(uint32_t frameIndex, std::span<const shaderio::DrawUniforms> drawData)
		{
			m_renderer.uploadDepthMDIDrawData(frameIndex, drawData);
		}

		void uploadGBufferMDIDrawData(uint32_t frameIndex, std::span<const shaderio::DrawUniforms> drawData)
		{
			m_renderer.uploadGBufferMDIDrawData(frameIndex, drawData);
		}

		void ensureGPUDrivenPersistentIndirectStream(uint32_t frameIndex, uint32_t requiredDrawCount)
		{
			m_renderer.ensureGPUDrivenPersistentIndirectStream(frameIndex, requiredDrawCount);
		}

		void executeDepthPyramidPass(rhi::CommandBuffer& cmdBuffer, const RenderParams& params);
		void beginPresentPass(rhi::CommandBuffer& cmdBuffer) { m_renderer.beginPresentPass(cmdBuffer); }
		void endPresentPass(rhi::CommandBuffer& cmdBuffer) { m_renderer.endPresentPass(cmdBuffer); }

		void executeImGuiPass(rhi::CommandBuffer& cmdBuffer, const RenderParams& params)
		{
			m_renderer.executeImGuiPass(cmdBuffer, params);
		}

		void bindStaticPassResources()
		{
			m_passExecutor.setResourceTable(&m_renderer.getRHIDevice());
			m_renderer.bindStaticPassResources(m_passExecutor);
		}

		void submitPassGraph(const RenderParams& params) { m_renderer.renderWithPassExecutor(params, m_passExecutor); }
		bool prepareAndDispatchVisibilityPatch(rhi::CommandBuffer& cmdBuffer,
		                                       uint32_t frameIndex,
		                                       uint64_t targetIndirectBufferHandle,
		                                       uint32_t categoryValue,
		                                       uint32_t outputOffset);

	private:
		struct DirtyRange
		{
			uint32_t first{0};
			uint32_t count{0};
		};

		struct VisibilitySortFrameResources
		{
			utils::Buffer uploadKeyBuffer{};
			utils::Buffer uploadValueBuffer{};
			utils::Buffer keyBuffer{};
			utils::Buffer valueBuffer{};
			rhi::ArgumentTableHandle argumentTable{}; // owned RHI ArgumentTable
			rhi::BufferHandle keyBufferHandle{}; // owned=false mirror, rebound on realloc
			rhi::BufferHandle valueBufferHandle{};
			rhi::BufferHandle uploadKeyBufferHandle{}; // owned=false mirror of staging buffer
			rhi::BufferHandle uploadValueBufferHandle{};
			uint32_t capacity{0};
			uint32_t activeElementCount{0};
			uint32_t paddedElementCount{0};
		};

		struct TransparentVisibilityFrameResources
		{
			std::array<utils::Buffer, 2> prefixBuffers{};
			std::array<rhi::ArgumentTableHandle, 2> argumentTables{};
			std::array<rhi::BufferHandle, 2> prefixBufferHandles{};
			std::array<rhi::BufferHandle, 2> sourceIndirectBufferHandles{};
			std::array<rhi::BufferHandle, 2> targetIndirectBufferHandles{};
			std::array<uint64_t, 2> boundSortKeyHandles{0, 0};
			std::array<uint64_t, 2> boundSortValueHandles{0, 0};
			std::array<uint64_t, 2> boundSourceIndirectHandles{0, 0};
			std::array<uint64_t, 2> boundTargetIndirectHandles{0, 0};
			std::array<uint64_t, 2> boundPrefixAHandles{0, 0};
			std::array<uint64_t, 2> boundPrefixBHandles{0, 0};
			uint32_t prefixCapacity{0};
		};

		struct SortedBootstrapFrameState
		{
			uint32_t opaqueCapacity{0};
			uint32_t alphaCapacity{0};
			uint64_t sceneTopologyVersion{0};
			bool valid{false};
		};

		static uint64_t packMeshHandleKey(MeshHandle handle);
		void initVisibilitySortResources();
		void shutdownVisibilitySortResources();
		void ensureVisibilitySortCapacity(uint32_t frameIndex, uint32_t requiredCount);
		void updateVisibilitySortArgumentTable(uint32_t frameIndex);
		void prepareVisibilitySortInputs(uint32_t frameIndex);
		void initTransparentVisibilityPatchResources();
		void shutdownTransparentVisibilityPatchResources();
		void updateTransparentVisibilityPatchArgumentTable(uint32_t frameIndex,
		                                                   uint64_t sortKeyBufferHandle,
		                                                   uint64_t sortValueBufferHandle,
		                                                   uint64_t sourceIndirectBufferHandle,
		                                                   uint64_t targetIndirectBufferHandle);
		[[nodiscard]] uint32_t getPreviousFrameIndex(uint32_t frameIndex) const;
		void rebuildGPUDrivenScene(const GltfModel& model, const GltfUploadResult& uploadResult,
		                           rhi::CommandBuffer& cmd);
		void rebuildGPUDrivenScene(const SceneAsset& asset,
		                           const SceneUploadPlan& plan,
		                           const SceneUploadResult& uploadResult,
		                           rhi::CommandBuffer& cmd);
		void appendSceneObjectDraw(uint64_t meshKey, MeshHandle meshHandle, uint32_t drawIndex, SceneDrawBucket bucket);
		void clearGPUDrivenScene();
		void flushPendingSceneUploads();
		void invalidateSortedBootstrapStates();
		void invalidateSortedBootstrapState(uint32_t frameIndex);
		void recordSortedBootstrapState(uint32_t frameIndex, uint32_t opaqueCapacity, uint32_t alphaCapacity);
		void markPersistentDrawDirty(uint32_t drawIndex);
		[[nodiscard]] std::vector<DirtyRange> buildPersistentDrawDirtyRanges() const;
		void uploadPersistentDrawData();
		void refreshSceneView();
		void updateOwnershipDiagnostics(uint32_t frameIndex, bool sceneRenderingSuspended, uint32_t safeObjectCount);
		void initLightingResources();
		void shutdownLightingResources();
		void initIBLResources();
		void shutdownIBLResources();
		void updateGPUDrivenLights(const RenderParams& params, uint32_t frameIndex);
		void updateLightingArgumentTable(uint32_t frameIndex);
		void initLightingPipelines();
		void shutdownLightingPipelines();
		void initPhase7Resources();
		void shutdownPhase7Resources();
		void resizePhase7Resources();
		void bindPhase7PassResources();
		void initPhase7Pipelines();
		void shutdownPhase7Pipelines();
		void updatePhase7Descriptors(uint32_t frameIndex);

		RenderDevice m_renderer;
		PassExecutor m_passExecutor;
		GPUSceneRegistry m_sceneRegistry;
		HiZDepthPyramid m_hiZDepthPyramid;
		GPUDrivenLightResources m_lightResources;
		GPUMeshletBuffer m_meshletBuffer;
		rhi::BufferHandle m_shadowPackedVertexBufferRHI{};
		rhi::BufferHandle m_shadowPackedIndexBufferRHI{};
		std::vector<shaderio::Meshlet> m_meshletDataCpu;
		std::vector<uint32_t> m_meshletIndicesCpu;
		std::vector<shaderio::GPUCullObject> m_meshletCullObjectsCpu;
		std::vector<uint32_t> m_visibilitySortInputObjects;
		std::vector<uint32_t> m_visibilitySortInputKeys;
		std::vector<uint32_t> m_cachedStaticVisibilitySortObjects;
		std::vector<uint32_t> m_cachedStaticVisibilitySortKeys;
		uint64_t m_cachedStaticVisibilitySortTopologyVersion{0};
		std::vector<shaderio::DrawUniforms> m_persistentDrawData;
		std::vector<SceneUploadResult::SceneDrawRecord> m_sceneDrawRecords;
		std::vector<uint32_t> m_dirtyPersistentDrawIndices;
		std::unique_ptr<GPUDrivenDepthPrepass> m_depthPrepass;
		std::unique_ptr<GPUDrivenDepthPyramidPass> m_depthPyramidPass;
		std::unique_ptr<GPUDrivenCullingPass> m_gpuCullingPass;
		std::unique_ptr<GPUDrivenVisibilitySortPass> m_visibilitySortPass;
		std::unique_ptr<GPUDrivenLightCullingPass> m_lightCullingPass;
		std::unique_ptr<GPUDrivenClusteredLightCullingPass> m_clusteredLightCullingPass;
		std::unique_ptr<GPUDrivenCSMShadowPass> m_csmShadowPass;
		std::unique_ptr<GPUDrivenShadowAtlasPass> m_shadowAtlasPass;
		std::unique_ptr<GPUDrivenGBufferPass> m_gbufferPass;
		std::unique_ptr<GPUDrivenLightPass> m_lightPass;
		std::unique_ptr<GPUDrivenSkyboxPass> m_skyboxPass;
		std::unique_ptr<GPUDrivenAOPass> m_aoPass;
		std::unique_ptr<GPUDrivenSSRPass> m_ssrPass;
		std::unique_ptr<GPUDrivenVelocityPass> m_velocityPass;
		std::unique_ptr<GPUDrivenTAAResolvePass> m_taaResolvePass;
		std::unique_ptr<GPUDrivenBloomPrefilterPass> m_bloomPrefilterPass;
		std::unique_ptr<GPUDrivenBloomDownsamplePass> m_bloomDownsamplePass;
		std::unique_ptr<GPUDrivenFinalColorPass> m_finalColorPass;
		std::unique_ptr<GPUDrivenForwardPass> m_forwardPass;
		std::unique_ptr<GPUDrivenDebugPass> m_debugPass;
		std::unique_ptr<GPUDrivenPresentPass> m_presentPass;
		std::unique_ptr<GPUDrivenImguiPass> m_imguiPass;
		GPUDrivenSceneView m_sceneView{};
		GPUDrivenRuntimeStats m_runtimeStats{};
		SceneUploadResult m_activeUploadResultStorage{};
		const SceneUploadResult* m_activeUploadResult{nullptr};
		std::unordered_map<uint64_t, uint32_t> m_objectIdByMeshHandle;
		std::unordered_map<uint64_t, std::vector<uint32_t>> m_objectIdsByMeshHandle;
		std::unordered_map<uint64_t, uint32_t> m_drawIndexByMeshHandle;
		std::unordered_map<uint64_t, glm::mat4> m_previousTransformByMeshHandle;
		std::unordered_map<uint32_t, glm::mat4> m_previousTransformByDrawIndex;
		std::vector<uint32_t> m_objectIdByDrawRecord;
		std::vector<uint32_t> m_drawIndexByDrawRecord;
		std::vector<MeshHandle> m_meshHandleByDrawIndex;
		std::vector<uint32_t> m_opaqueDrawIndices;
		std::vector<uint32_t> m_alphaTestDrawIndices;
		std::vector<uint32_t> m_transparentDrawIndices;
		rhi::ArgumentLayoutHandle m_visibilitySortArgumentLayout{};
		PipelineHandle m_visibilitySortPipelineHandle{};
		std::vector<VisibilitySortFrameResources> m_visibilitySortFrames;
		PipelineHandle m_transparentVisibilityPatchPipelineHandle{};
		rhi::ArgumentLayoutHandle m_transparentVisibilityPatchArgumentLayout{};
		std::vector<TransparentVisibilityFrameResources> m_transparentVisibilityPatchFrames;
		// Wave 9: lighting-input (set LSetTextures) and lighting-scene (set LSetScene) are RHI
		// ArgumentLayouts + owned ArgumentTables. The coarse-culling set (point/spot + clustered)
		// is likewise RHI; its 9 light-resource buffers are mirrored as owned=false RHI handles.
		rhi::ArgumentLayoutHandle m_lightingArgumentLayout{};
		rhi::ArgumentLayoutHandle m_lightingSceneArgumentLayout{};
		rhi::ArgumentLayoutHandle m_lightCoarseCullingArgumentLayout{};
		std::vector<rhi::BufferHandle> m_lightCoarseCullingBufferHandles;
		// Stable light-resource buffer handles (owned=false) consumed by the lighting-input set.
		std::vector<rhi::BufferHandle> m_lightingInputBufferHandles;
		std::vector<rhi::ArgumentTableHandle> m_lightCoarseCullingArgumentTables;
		std::vector<rhi::ArgumentTableHandle> m_lightingSceneArgumentTables;
		std::vector<rhi::ArgumentTableHandle> m_lightingInputArgumentTables;
		uintptr_t m_linearClampSampler{0};
		rhi::SamplerHandle m_linearClampSamplerHandle{};
		rhi::SamplerHandle m_iblCubeSamplerHandle{};
		rhi::SamplerHandle m_iblLutSamplerHandle{};
		// Adopted (owned=false) RHI view handles for per-frame backend views fed into
		// the lighting-input set; re-registered when the underlying backend token changes.
		rhi::TextureViewHandle m_iblEnvViewHandle{};
		uintptr_t m_iblEnvViewToken{0};
		rhi::TextureViewHandle m_aoViewHandle{};
		uintptr_t m_aoViewToken{0};
		rhi::TextureViewHandle m_ssrViewHandle{};
		uintptr_t m_ssrViewToken{0};
		std::array<rhi::ArgumentLayoutHandle, 2> m_lightPipelineArgumentLayouts{};
		PipelineHandle m_pointLightCoarseCullingPipeline{};
		PipelineHandle m_spotLightCoarseCullingPipeline{};
		// Fullscreen graphics pipelines now live in the device pipeline registry; only
		// their handles are tracked here.
		PipelineHandle m_gpuDrivenLightHdrPipeline{};
		PipelineHandle m_gpuDrivenSkyboxPipeline{};
		PipelineHandle m_gpuDrivenTAAResolvePipeline{};
		PipelineHandle m_gpuDrivenBloomPrefilterPipeline{};
		PipelineHandle m_gpuDrivenBloomDownsamplePipeline{};
		PipelineHandle m_gpuDrivenBloomUpsamplePipeline{};
		PipelineHandle m_gpuDrivenFinalColorPipeline{};
		PipelineHandle m_gpuDrivenVelocityPipeline{};
		PipelineHandle m_clusteredLightCullingPipeline{};
		// Wave 9: AO/SSR sets are RHI ArgumentLayouts + owned/temporary ArgumentTables.
		rhi::ArgumentLayoutHandle m_aoArgumentLayout{};
		// Adopted (owned=false) view handles for the AO raw / denoised storage images, fed into
		// the AO ArgumentTables; re-registered when the backend view token changes.
		rhi::TextureViewHandle m_aoRawViewHandle{};
		uintptr_t m_aoRawViewToken{0};
		rhi::TextureViewHandle m_aoDenoisedViewHandle{};
		uintptr_t m_aoDenoisedViewToken{0};
		// Phase 6: RHI handles for the Phase-7 compute pipelines + adopted bind groups,
		// so the AO/SSR passes record through CommandBuffer verbs instead of raw vkCmd*. AO uses
		// persistent adopted sets; SSR builds a per-frame temporary bind group from
		// m_ssrLayoutHandle (see acquireSSRTempArgumentTable).
		PipelineHandle m_gtaoPipelineHandle{};
		PipelineHandle m_aoDenoisePipelineHandle{};
		PipelineHandle m_ssrTracePipelineHandle{};
		rhi::ArgumentLayoutHandle m_ssrLayoutHandle{};
		rhi::TextureViewHandle m_ssrRawViewHandle{}; // Wave 8: adopted storage-image view for SSR temp ArgumentWrite
		rhi::TextureViewHandle m_shadowAtlasViewHandle{}; // Wave 9: registry handle for the shadow-atlas depth view
		std::vector<rhi::ArgumentTableHandle> m_aoArgumentTables;
		std::vector<rhi::ArgumentTableHandle> m_aoDenoiseArgumentTables;
		utils::ImageResource m_aoRaw{};
		utils::ImageResource m_aoDenoised{};
		utils::ImageResource m_ssrRaw{};
		utils::ImageResource m_shadowAtlas{};
		rhi::Extent2D m_phase7HalfExtent{};
		rhi::Extent2D m_shadowAtlasExtent{2048u, 2048u};
		uint32_t m_shadowAtlasTileSize{512u};
		uint32_t m_shadowAtlasAllocatedTiles{0};
		std::vector<shaderio::LightData> m_gpuDrivenPointLights;
		std::vector<shaderio::LightData> m_gpuDrivenSpotLights;
		utils::ImageResource m_iblEnvironment{};
		IBLResources m_iblResources{};
		std::vector<utils::Buffer> m_gpuDrivenStagingBuffers;
		std::vector<rhi::BufferHandle> m_gpuDrivenRhiStagingBuffers;
		rhi::TextureFormat m_iblEnvironmentFormat{rhi::TextureFormat::undefined};
		rhi::Extent2D m_iblEnvironmentExtent{};
		uint32_t m_iblEnvironmentMipCount{0};
		uint64_t m_iblEnvironmentEstimatedBytes{0};
		bool m_iblEnvironmentLoaded{false};
		bool m_iblUsingFallback{true};
		std::string m_iblEnvironmentPath;
		std::string m_iblEnvironmentStatus{"Not initialized"};
		std::vector<SortedBootstrapFrameState> m_sortedBootstrapFrames;
		uint64_t m_sceneTopologyVersion{1};
		bool m_enableExperimentalMeshletPath{false};
		bool m_suspendSceneRendering{false};
		bool m_sceneUploadPending{false};
		bool m_persistentDrawDataDirty{false};
		bool m_previousTransformResetPending{false};
		bool m_hiZCameraHistoryValid{false};
		glm::vec3 m_lastHiZCameraPosition{0.0f};
		float m_lastHiZCameraDeltaDistance{0.0f};
		bool m_lastHiZFastCameraFallbackTriggered{false};
		shaderio::CameraUniforms m_temporalCameraUniforms{};
		shaderio::CameraUniforms m_previousCameraUniforms{};
		glm::mat4 m_previousJitteredViewProjection{1.0f};
		glm::vec2 m_currentTAAJitterUv{0.0f};
		glm::vec2 m_previousTAAJitterUv{0.0f};
		bool m_previousCameraValid{false};
		bool m_taaHistoryValid{false};
		uint64_t m_temporalFrameCounter{0};
	};
} // namespace demo
