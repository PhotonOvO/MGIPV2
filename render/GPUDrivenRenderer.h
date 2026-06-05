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
#include "../rhi/RHIDescriptor.h"

#include <array>
#include <memory>
#include <string>
#include <unordered_map>
#include <vector>

namespace demo {

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
  std::string              name;
  GPUDrivenOwnershipState ownership{GPUDrivenOwnershipState::disabled};
  std::string              note;
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
  bool     depthUsesPreviousFrameIndirect{false};
  bool     depthUsesSortedBootstrap{false};
  bool     gbufferOpaqueAlphaPatchDispatched{false};
  bool     transparentPatchDispatched{false};
  bool     transparentOrderingCpuSeeded{false};
  bool     materialSortKeysCpuSeeded{false};
  bool     transparentCapacityOverflow{false};
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
  bool     valid{false};
  bool     boundForGpuCulling{false};
  bool     frustumCullingEnabled{false};
  bool     occlusionCullingEnabled{false};
  bool     meshletOcclusionEnabled{false};
  bool     meshletConeCullingEnabled{false};
  float    depthEpsilon{0.0f};
  float    conservativeRadiusScale{0.0f};
  float    conservativeRadiusBias{0.0f};
  float    nearRejectEpsilon{0.0f};
  float    largeObjectFootprintThreshold{0.0f};
  float    fastCameraFallbackDistance{0.0f};
  float    cameraDeltaDistance{0.0f};
  bool     fastCameraFallbackTriggered{false};
};

struct GPUDrivenPostProcessDiagnostics
{
  uint32_t displayWidth{0};
  uint32_t displayHeight{0};
  uint32_t internalWidth{0};
  uint32_t internalHeight{0};
  uint32_t outputWidth{0};
  uint32_t outputHeight{0};
  VkFormat outputFormat{VK_FORMAT_UNDEFINED};
  VkFormat sceneColorFormat{VK_FORMAT_UNDEFINED};
  VkFormat recommendedHdrFormat{VK_FORMAT_R16G16B16A16_SFLOAT};
  uint64_t outputMemoryBytes{0};
  uint64_t sceneColorMemoryBytes{0};
  uint64_t recommendedHdrMemoryBytes{0};
  uint64_t bloomHalfQuarterMemoryBytes{0};
  float    fixedExposure{1.0f};
  float    adaptiveExposureTarget{0.18f};
  float    minAutoExposure{0.25f};
  float    maxAutoExposure{4.0f};
  float    bloomIntensity{0.35f};
  float    bloomThreshold{0.0f};
  float    colorSaturation{1.0f};
  float    colorContrast{1.0f};
  float    colorGamma{1.0f};
  float    colorLutStrength{0.0f};
  float    vignetteIntensity{0.0f};
  float    lensDirtIntensity{0.0f};
  float    taaBlendWeight{0.90f};
  float    taaJitterScale{1.0f};
  float    renderScale{1.0f};
  uint32_t upscaleMode{0};
  bool     hdrSceneColorActive{false};
  bool     mobileHdrRecommended{true};
  bool     toneMapInLightPass{true};
  bool     finalColorPassActive{false};
  bool     velocityBufferActive{false};
  bool     taaPassActive{false};
  bool     taaHistoryValid{false};
  bool     temporalUpscalingActive{false};
  bool     internalRenderScaleBlocked{false};
  bool     exposurePassActive{false};
  bool     adaptiveExposureActive{false};
  bool     bloomPassActive{false};
  bool     colorGradingLutActive{false};
  bool     lensEffectsActive{false};
};

struct GPUDrivenIBLDiagnostics
{
  uint32_t width{0};
  uint32_t height{0};
  uint32_t mipCount{0};
  VkFormat format{VK_FORMAT_UNDEFINED};
  uint64_t estimatedMemoryBytes{0};
  float    intensity{0.0f};
  bool     enabled{false};
  bool     loaded{false};
  bool     fallback{true};
  bool     irradianceReady{false};
  bool     prefilteredReady{false};
  bool     brdfLutReady{false};
  bool     splitSumReady{false};
  int      debugMode{0};
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
  bool     enabled{false};
  bool     resourcesOwned{false};
  bool     descriptorsReady{false};
  bool     fallbackActive{true};
};

struct GPUDrivenAOReflectionDiagnostics
{
  uint32_t aoWidth{0};
  uint32_t aoHeight{0};
  uint32_t ssrWidth{0};
  uint32_t ssrHeight{0};
  uint64_t estimatedMemoryBytes{0};
  bool     aoEnabled{false};
  bool     aoReady{false};
  bool     ssrEnabled{false};
  bool     ssrReady{false};
};

struct GPUDrivenShadowAtlasDiagnostics
{
  uint32_t atlasWidth{0};
  uint32_t atlasHeight{0};
  uint32_t tileSize{0};
  uint32_t tileCapacity{0};
  uint32_t allocatedTiles{0};
  uint64_t estimatedMemoryBytes{0};
  bool     enabled{false};
  bool     ready{false};
  bool     fallbackToCSM{true};
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
  void init(void* window, rhi::Surface& surface, bool vSync);
  void shutdown(rhi::Surface& surface);
  void setVSync(bool enabled) { m_renderer.setVSync(enabled); }
  [[nodiscard]] bool getVSync() const { return m_renderer.getVSync(); }
  void setFullscreen(bool enabled, void* platformHandle = nullptr) { m_renderer.setFullscreen(enabled, platformHandle); }
  [[nodiscard]] const char* getSwapchainPresentModeName() const { return m_renderer.getSwapchainPresentModeName(); }
  [[nodiscard]] uint32_t getSwapchainImageCount() const { return m_renderer.getSwapchainImageCount(); }
  void resize(rhi::Extent2D size);
  void render(const RenderParams& params);
  void setSceneRenderingSuspended(bool suspended) { m_suspendSceneRendering = suspended; }
  [[nodiscard]] bool isSceneRenderingSuspended() const { return m_suspendSceneRendering; }

  TextureHandle  getViewportTextureHandle() const { return m_renderer.getViewportTextureHandle(); }
  ImTextureID    getViewportTextureID(TextureHandle handle) const { return m_renderer.getViewportTextureID(handle); }
  MaterialHandle getMaterialHandle(uint32_t slot) const { return m_renderer.getMaterialHandle(slot); }
  GltfUploadResult uploadGltfModel(const GltfModel& model, VkCommandBuffer cmd);
  SceneUploadResult commitSceneUploadPlan(const SceneAsset& asset, const SceneUploadPlan& plan, VkCommandBuffer cmd);
  void             uploadGltfModelBatch(const GltfModel&          model,
                                        std::span<const uint32_t> textureIndices,
                                        std::span<const uint32_t> materialIndices,
                                        std::span<const uint32_t> meshIndices,
                                        GltfUploadResult&         ioResult,
                                        VkCommandBuffer           cmd);
  void             initializeGltfUploadResult(const GltfModel& model, GltfUploadResult& outResult) const
  {
    m_renderer.initializeGltfUploadResult(model, outResult);
  }
  void             destroyGltfResources(const GltfUploadResult& result);
  void             updateMeshTransform(MeshHandle handle, const glm::mat4& transform);
  void             updateSceneInstanceTransform(uint32_t instanceIndex, const glm::mat4& transform);
  void             executeUploadCommand(std::function<void(VkCommandBuffer)> uploadFn) { m_renderer.executeUploadCommand(std::move(uploadFn)); }
  void             waitForIdle() { m_renderer.waitForIdle(); }

  [[nodiscard]] const shaderio::GPUCullStats& getLastGPUCullingStats() const { return m_renderer.getLastGPUCullingStats(); }
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
  [[nodiscard]] shaderio::ShadowUniforms* getShadowUniformsData() { return m_renderer.getShadowUniformsData(); }
  [[nodiscard]] CSMShadowResources& getCSMShadowResources() { return m_renderer.getCSMShadowResources(); }
  [[nodiscard]] rhi::TextureViewHandle getCSMCascadeViewHandle(uint32_t cascadeIndex) const
  {
    return m_renderer.getCSMCascadeViewHandle(cascadeIndex);
  }
  [[nodiscard]] uint32_t             getPersistentObjectCount() const { return m_sceneView.objectCount; }
  [[nodiscard]] bool                 isExperimentalMeshletPathEnabled() const { return m_enableExperimentalMeshletPath; }
  [[nodiscard]] bool                 isMeshletRenderingActive() const
  {
    return m_enableExperimentalMeshletPath && m_meshletBuffer.getMeshletCount() > 0u
           && m_meshletBuffer.getMeshletIndexBufferHandle() != 0;
  }
  [[nodiscard]] uint64_t             getMeshletIndexBufferHandle() const
  {
    return m_meshletBuffer.getMeshletIndexBufferHandle();
  }
  [[nodiscard]] rhi::BufferHandle    getMeshletIndexBufferRHIHandle() const
  {
    return m_meshletBuffer.getMeshletIndexBufferRHIHandle();
  }
  // Stable RHI handles for the scene's shadow packed vertex/index buffers (rebound
  // when the active upload result changes). Consumed by CSM / shadow-atlas passes.
  [[nodiscard]] rhi::BufferHandle    getShadowPackedVertexBufferRHIHandle() const { return m_shadowPackedVertexBufferRHI; }
  [[nodiscard]] rhi::BufferHandle    getShadowPackedIndexBufferRHIHandle() const { return m_shadowPackedIndexBufferRHI; }
  [[nodiscard]] VkBuffer             getMeshletDataBuffer() const
  {
    return m_meshletBuffer.getMeshletDataBuffer();
  }
  [[nodiscard]] bool                 tryGetMeshDrawIndex(MeshHandle handle, uint32_t& outDrawIndex) const;
  [[nodiscard]] bool                 tryGetMeshHandleForDrawIndex(uint32_t drawIndex, MeshHandle& outHandle) const;
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
  [[nodiscard]] PipelineHandle getForwardMDIPipelineHandle() const { return m_renderer.getForwardMDIPipelineHandle(); }
  [[nodiscard]] PipelineHandle getCSMShadowPipelineHandle() const { return m_renderer.getCSMShadowPipelineHandle(); }
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
  [[nodiscard]] BindGroupHandle getGPUCullingBindGroup(uint32_t frameIndex) const
  {
    return m_renderer.getGPUCullingBindGroup(frameIndex);
  }
  [[nodiscard]] BindGroupHandle getShadowCullingBindGroup(uint32_t frameIndex) const
  {
    return m_renderer.getShadowCullingBindGroup(frameIndex);
  }
  [[nodiscard]] PipelineHandle getLightCullingPipelineHandle() const { return m_pointLightCoarseCullingPipeline; }
  [[nodiscard]] PipelineHandle getSpotLightCullingPipelineHandle() const { return m_spotLightCoarseCullingPipeline; }
  [[nodiscard]] uint64_t getGraphicsMaterialDescriptorSet() const { return m_renderer.getGraphicsMaterialDescriptorSet(); }
  [[nodiscard]] BindGroupHandle getGraphicsMaterialBindGroup() const { return m_renderer.getGraphicsMaterialBindGroup(); }
  [[nodiscard]] uint64_t getLightPipelineLayout() const { return m_lightPipelineLayout ? m_lightPipelineLayout->getNativeHandle() : 0; }
  [[nodiscard]] uint64_t getLightingInputDescriptorSet() const;
  [[nodiscard]] uint64_t getLightingSceneDescriptorSet(uint32_t frameIndex) const;
  void updateLightingSceneDescriptorSet(uint32_t frameIndex, uint64_t transientBufferOpaque, uint32_t cameraOffset);
  [[nodiscard]] bool getIBLEnvironmentLoaded() const { return m_iblEnvironmentLoaded; }
  [[nodiscard]] bool getIBLUsingFallback() const { return m_iblUsingFallback; }
  [[nodiscard]] VkFormat getIBLEnvironmentFormat() const { return m_iblEnvironmentFormat; }
  [[nodiscard]] VkExtent2D getIBLEnvironmentExtent() const { return m_iblEnvironmentExtent; }
  [[nodiscard]] uint32_t getIBLEnvironmentMipCount() const { return m_iblEnvironmentMipCount; }
  [[nodiscard]] uint64_t getIBLEnvironmentEstimatedBytes() const { return m_iblEnvironmentEstimatedBytes; }
  [[nodiscard]] const std::string& getIBLEnvironmentPath() const { return m_iblEnvironmentPath; }
  [[nodiscard]] const std::string& getIBLEnvironmentStatus() const { return m_iblEnvironmentStatus; }
  [[nodiscard]] uint64_t getShadowCullingPipelineLayout() const { return m_renderer.getShadowCullingPipelineLayout(); }
  [[nodiscard]] BindGroupHandle getLightCoarseCullingBindGroup(uint32_t frameIndex) const
  {
    return frameIndex < m_lightCoarseCullingBindGroups.size() ? m_lightCoarseCullingBindGroups[frameIndex] : BindGroupHandle{};
  }
  [[nodiscard]] BindGroupHandle getCurrentLightCullingBindGroup() const;
  [[nodiscard]] PipelineHandle getClusteredLightCullingPipelineHandle() const { return m_clusteredLightCullingPipeline; }
  [[nodiscard]] uint64_t getClusterStatsBufferOpaque(uint32_t frameIndex) const
  {
    return reinterpret_cast<uint64_t>(m_lightResources.getClusterStatsBuffer(frameIndex));
  }
  // Stats buffer (binding 8) registered as owned=false handle alongside the coarse-culling table.
  [[nodiscard]] rhi::BufferHandle getClusterStatsBufferHandle(uint32_t frameIndex) const
  {
    const size_t idx = static_cast<size_t>(frameIndex) * 9u + 8u;
    return idx < m_lightCoarseCullingBufferHandles.size() ? m_lightCoarseCullingBufferHandles[idx] : rhi::BufferHandle{};
  }
  [[nodiscard]] VkExtent2D getPhase7HalfExtent() const { return m_phase7HalfExtent; }
  [[nodiscard]] uint64_t   getSceneViewOutputImageOpaque() const { return reinterpret_cast<uint64_t>(m_sceneView.outputImage); }
  [[nodiscard]] VkExtent2D getSceneViewDepthExtent() const { return m_sceneView.sceneDepthExtent; }
  [[nodiscard]] uint64_t getAOTracePipelineOpaque() const { return reinterpret_cast<uint64_t>(m_gtaoPipeline); }
  [[nodiscard]] uint64_t getAODenoisePipelineOpaque() const { return reinterpret_cast<uint64_t>(m_aoDenoisePipeline); }
  [[nodiscard]] uint64_t getAORawImageOpaque() const { return reinterpret_cast<uint64_t>(m_aoRaw.image); }
  [[nodiscard]] uint64_t getAODenoisedImageOpaque() const { return reinterpret_cast<uint64_t>(m_aoDenoised.image); }
  [[nodiscard]] PipelineHandle getAOTracePipelineHandle() const { return m_gtaoPipelineHandle; }
  [[nodiscard]] PipelineHandle getAODenoisePipelineHandle() const { return m_aoDenoisePipelineHandle; }
  [[nodiscard]] BindGroupHandle getAOBindGroup(uint32_t frameIndex) const
  {
    return frameIndex < m_aoBindGroups.size() ? m_aoBindGroups[frameIndex] : BindGroupHandle{};
  }
  [[nodiscard]] BindGroupHandle getAODenoiseBindGroup(uint32_t frameIndex) const
  {
    return frameIndex < m_aoDenoiseBindGroups.size() ? m_aoDenoiseBindGroups[frameIndex] : BindGroupHandle{};
  }
  [[nodiscard]] uint64_t getSSRTracePipelineOpaque() const { return reinterpret_cast<uint64_t>(m_ssrTracePipeline); }
  [[nodiscard]] uint64_t getSSRRawImageOpaque() const { return reinterpret_cast<uint64_t>(m_ssrRaw.image); }
  [[nodiscard]] PipelineHandle getSSRTracePipelineHandle() const { return m_ssrTracePipelineHandle; }
  // Builds the SSR compute set as a per-frame temporary bind group (gbuffer/depth/history
  // sampled images + ssrRaw storage image + the caller's camera UBO slice). Returns a
  // frame-lifetime handle; do not cache it across frames.
  [[nodiscard]] BindGroupHandle acquireSSRTempBindGroup(uint64_t cameraBuffer, uint32_t cameraOffset);

  // Opaque snapshot of the per-frame bitonic visibility-sort resources, so the
  // visibility-sort pass can record without reaching into renderer internals.
  struct VisibilitySortDispatch
  {
    PipelineHandle    pipelineHandle{};
    BindGroupHandle   bindGroup{};
    rhi::BufferHandle uploadKeyBufferHandle{};
    rhi::BufferHandle uploadValueBufferHandle{};
    rhi::BufferHandle keyBufferHandle{};
    rhi::BufferHandle valueBufferHandle{};
    uint32_t paddedElementCount{0};
    bool     valid{false};
  };
  [[nodiscard]] VisibilitySortDispatch getVisibilitySortDispatch(uint32_t frameIndex) const
  {
    VisibilitySortDispatch info{};
    if(m_visibilitySortPipeline == VK_NULL_HANDLE || !m_visibilitySortPipelineLayout
       || frameIndex >= m_visibilitySortFrames.size())
    {
      return info;
    }
    const VisibilitySortFrameResources& f = m_visibilitySortFrames[frameIndex];
    info.pipelineHandle          = m_visibilitySortPipelineHandle;
    info.bindGroup               = f.bindGroup;
    info.uploadKeyBufferHandle   = f.uploadKeyBufferHandle;
    info.uploadValueBufferHandle = f.uploadValueBufferHandle;
    info.keyBufferHandle         = f.keyBufferHandle;
    info.valueBufferHandle       = f.valueBufferHandle;
    info.paddedElementCount = f.paddedElementCount;
    info.valid = !f.bindGroup.isNull() && f.paddedElementCount > 1u
                 && f.uploadKeyBuffer.buffer != VK_NULL_HANDLE && f.uploadValueBuffer.buffer != VK_NULL_HANDLE;
    return info;
  }
  [[nodiscard]] uint64_t getShadowCullingDescriptorSetOpaque(uint32_t frameIndex) const
  {
    return m_renderer.getShadowCullingDescriptorSetOpaque(frameIndex);
  }
  [[nodiscard]] uint64_t getGPUCullingDescriptorSetOpaque(uint32_t frameIndex) const
  {
    return m_renderer.getGPUCullingDescriptorSetOpaque(frameIndex);
  }
  [[nodiscard]] BindGroupHandle getCSMShadowMDIDrawBindGroup(uint32_t frameIndex, uint32_t cascadeIndex) const
  {
    return m_renderer.getCSMShadowMDIDrawBindGroup(frameIndex, cascadeIndex);
  }
  [[nodiscard]] BindGroupHandle getCameraBindGroup(uint32_t frameIndex) const { return m_renderer.getCameraBindGroup(frameIndex); }
  [[nodiscard]] BindGroupHandle getDrawBindGroup(uint32_t frameIndex) const { return m_renderer.getDrawBindGroup(frameIndex); }
  [[nodiscard]] BindGroupHandle getMDIDrawBindGroup(uint32_t frameIndex) const { return m_renderer.getMDIDrawBindGroup(frameIndex); }
  [[nodiscard]] BindGroupHandle getGBufferMDIDrawBindGroup(uint32_t frameIndex) const
  {
    return m_renderer.getGBufferMDIDrawBindGroup(frameIndex);
  }
  [[nodiscard]] BindGroupHandle getDepthMDIDrawBindGroup(uint32_t frameIndex) const
  {
    return m_renderer.getDepthMDIDrawBindGroup(frameIndex);
  }
  [[nodiscard]] uint64_t getBindGroupDescriptorSet(BindGroupHandle handle, BindGroupSetSlot slot) const
  {
    return m_renderer.getBindGroupDescriptorSet(handle, slot);
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
  [[nodiscard]] uint64_t getShadowAtlasImageOpaque() const { return reinterpret_cast<uint64_t>(m_shadowAtlas.image); }
  [[nodiscard]] uint64_t getShadowAtlasViewOpaque() const { return reinterpret_cast<uint64_t>(m_shadowAtlas.view); }
  [[nodiscard]] rhi::TextureViewHandle getShadowAtlasViewHandle() const { return m_shadowAtlasViewHandle; }
  [[nodiscard]] VkExtent2D getShadowAtlasExtent() const { return m_shadowAtlasExtent; }
  [[nodiscard]] uint32_t   getShadowAtlasTileSize() const { return m_shadowAtlasTileSize; }
  void                     setShadowAtlasAllocatedTiles(uint32_t tiles) { m_shadowAtlasAllocatedTiles = tiles; }
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
  [[nodiscard]] rhi::BufferHandle getPreviousGPUDrivenPersistentIndirectStreamBufferRHIHandle(uint32_t frameIndex) const
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
  [[nodiscard]] VkExtent2D getSceneExtent() const { return m_renderer.getSceneExtent(); }
  [[nodiscard]] VkFormat getSceneDepthFormat() const { return m_renderer.getSceneDepthFormat(); }
  [[nodiscard]] VkImage getSceneDepthImage() const { return m_renderer.getSceneDepthImage(); }
  [[nodiscard]] rhi::TextureViewHandle getSceneDepthImageView() const { return m_renderer.getSceneDepthImageView(); }
  [[nodiscard]] VkImage getSceneGBufferImage(uint32_t index) const { return m_renderer.getSceneGBufferImage(index); }
  [[nodiscard]] rhi::TextureViewHandle getSceneGBufferImageView(uint32_t index) const
  {
    return m_renderer.getSceneGBufferImageView(index);
  }
  [[nodiscard]] VkImage getOutputTextureImage() const { return m_renderer.getOutputTextureImage(); }
  [[nodiscard]] rhi::TextureViewHandle getOutputTextureView() const { return m_renderer.getOutputTextureView(); }
  [[nodiscard]] VkFormat getOutputTextureFormat() const { return m_renderer.getOutputTextureFormat(); }
  [[nodiscard]] uint64_t getOutputTextureEstimatedBytes() const
  {
    return m_renderer.getOutputTextureEstimatedBytes();
  }
  [[nodiscard]] VkImage getSceneColorHdrImage() const { return m_renderer.getSceneColorHdrImage(); }
  [[nodiscard]] rhi::TextureViewHandle getSceneColorHdrView() const { return m_renderer.getSceneColorHdrView(); }
  [[nodiscard]] VkFormat getSceneColorHdrFormat() const { return m_renderer.getSceneColorHdrFormat(); }
  [[nodiscard]] uint64_t getSceneColorHdrEstimatedBytes() const
  {
    return m_renderer.getSceneColorHdrEstimatedBytes();
  }
  [[nodiscard]] VkImage getBloomHalfImage() const { return m_renderer.getBloomHalfImage(); }
  [[nodiscard]] rhi::TextureViewHandle getBloomHalfView() const { return m_renderer.getBloomHalfView(); }
  [[nodiscard]] VkExtent2D getBloomHalfExtent() const { return m_renderer.getBloomHalfExtent(); }
  [[nodiscard]] VkImage getBloomQuarterImage() const { return m_renderer.getBloomQuarterImage(); }
  [[nodiscard]] rhi::TextureViewHandle getBloomQuarterView() const { return m_renderer.getBloomQuarterView(); }
  [[nodiscard]] VkExtent2D getBloomQuarterExtent() const { return m_renderer.getBloomQuarterExtent(); }
  [[nodiscard]] VkImage getBloomEighthImage() const { return m_renderer.getBloomEighthImage(); }
  [[nodiscard]] rhi::TextureViewHandle getBloomEighthView() const { return m_renderer.getBloomEighthView(); }
  [[nodiscard]] VkExtent2D getBloomEighthExtent() const { return m_renderer.getBloomEighthExtent(); }
  [[nodiscard]] VkImage getBloomSixteenthImage() const { return m_renderer.getBloomSixteenthImage(); }
  [[nodiscard]] rhi::TextureViewHandle getBloomSixteenthView() const { return m_renderer.getBloomSixteenthView(); }
  [[nodiscard]] VkExtent2D getBloomSixteenthExtent() const { return m_renderer.getBloomSixteenthExtent(); }
  [[nodiscard]] VkImage getBloomThirtySecondImage() const { return m_renderer.getBloomThirtySecondImage(); }
  [[nodiscard]] rhi::TextureViewHandle getBloomThirtySecondView() const { return m_renderer.getBloomThirtySecondView(); }
  [[nodiscard]] VkExtent2D getBloomThirtySecondExtent() const { return m_renderer.getBloomThirtySecondExtent(); }
  [[nodiscard]] VkImage getBloomUpsampleSixteenthImage() const { return m_renderer.getBloomUpsampleSixteenthImage(); }
  [[nodiscard]] rhi::TextureViewHandle getBloomUpsampleSixteenthView() const { return m_renderer.getBloomUpsampleSixteenthView(); }
  [[nodiscard]] VkExtent2D getBloomUpsampleSixteenthExtent() const { return m_renderer.getBloomUpsampleSixteenthExtent(); }
  [[nodiscard]] VkImage getBloomUpsampleEighthImage() const { return m_renderer.getBloomUpsampleEighthImage(); }
  [[nodiscard]] rhi::TextureViewHandle getBloomUpsampleEighthView() const { return m_renderer.getBloomUpsampleEighthView(); }
  [[nodiscard]] VkExtent2D getBloomUpsampleEighthExtent() const { return m_renderer.getBloomUpsampleEighthExtent(); }
  [[nodiscard]] VkImage getBloomUpsampleQuarterImage() const { return m_renderer.getBloomUpsampleQuarterImage(); }
  [[nodiscard]] rhi::TextureViewHandle getBloomUpsampleQuarterView() const { return m_renderer.getBloomUpsampleQuarterView(); }
  [[nodiscard]] VkExtent2D getBloomUpsampleQuarterExtent() const { return m_renderer.getBloomUpsampleQuarterExtent(); }
  [[nodiscard]] VkImage getBloomOutputImage() const { return m_renderer.getBloomOutputImage(); }
  [[nodiscard]] rhi::TextureViewHandle getBloomOutputView() const { return m_renderer.getBloomOutputView(); }
  [[nodiscard]] VkExtent2D getBloomOutputExtent() const { return m_renderer.getBloomOutputExtent(); }
  [[nodiscard]] VkImage getColorGradingLutImage() const { return m_renderer.getColorGradingLutImage(); }
  [[nodiscard]] rhi::TextureViewHandle getColorGradingLutView() const { return m_renderer.getColorGradingLutView(); }
  [[nodiscard]] VkExtent2D getColorGradingLutExtent() const { return m_renderer.getColorGradingLutExtent(); }
  [[nodiscard]] uint64_t getBloomEstimatedBytes() const { return m_renderer.getBloomEstimatedBytes(); }
  [[nodiscard]] VkImage getVelocityImage() const { return m_renderer.getVelocityImage(); }
  [[nodiscard]] rhi::TextureViewHandle getVelocityView() const { return m_renderer.getVelocityView(); }
  [[nodiscard]] VkFormat getVelocityFormat() const { return m_renderer.getVelocityFormat(); }
  [[nodiscard]] uint64_t getVelocityEstimatedBytes() const { return m_renderer.getVelocityEstimatedBytes(); }
  [[nodiscard]] glm::vec2 getCurrentTAAJitterUv() const { return m_currentTAAJitterUv; }
  [[nodiscard]] glm::vec2 getPreviousTAAJitterUv() const { return m_previousTAAJitterUv; }
  [[nodiscard]] VkImage getSceneColorHistoryImage(uint32_t index) const
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
  [[nodiscard]] VkExtent2D getSwapchainExtent() const { return m_renderer.getSwapchainExtent(); }
  [[nodiscard]] VkImage getCurrentSwapchainImage() const { return m_renderer.getCurrentSwapchainImage(); }
  [[nodiscard]] rhi::TextureHandle getCurrentSwapchainTextureRHIHandle() const { return m_renderer.getCurrentSwapchainTextureHandle(); }
  [[nodiscard]] rhi::TextureHandle getPassOutputTextureRHIHandle() const { return m_passExecutor.getTextureRHIHandle(kPassOutputHandle); }
  [[nodiscard]] uint32_t getCurrentFrameIndexHint() const { return m_renderer.getCurrentFrameIndexHint(); }
  [[nodiscard]] VkDevice getNativeDeviceHandle() const { return m_renderer.getNativeDeviceHandle(); }
  [[nodiscard]] rhi::Device& getRHIDevice() const { return m_renderer.getRHIDevice(); }
  [[nodiscard]] VmaAllocator getAllocatorHandle() const { return m_renderer.getAllocatorHandle(); }
  [[nodiscard]] uint64_t getNativeComputePipeline(PipelineHandle pipelineHandle) const
  {
    return m_renderer.getPipelineOpaque(pipelineHandle, static_cast<uint32_t>(VK_PIPELINE_BIND_POINT_COMPUTE));
  }
  // BindGroup wrapping the per-frame lighting-scene descriptor set (set LSetScene),
  // so GPUDriven fullscreen passes can bind it through cmd->bindBindGroup.
  [[nodiscard]] BindGroupHandle getLightingSceneBindGroup(uint32_t frameIndex) const
  {
    return frameIndex < m_lightingSceneBindGroups.size() ? m_lightingSceneBindGroups[frameIndex] : BindGroupHandle{};
  }
  // BindGroup wrapping the GBuffer/lighting-input texture set (set LSetTextures).
  [[nodiscard]] BindGroupHandle getLightingInputBindGroup(uint32_t frameIndex) const
  {
    return frameIndex < m_lightingInputBindGroups.size() ? m_lightingInputBindGroups[frameIndex] : BindGroupHandle{};
  }

  // rhi-typed render targets for the fullscreen screen-space passes (skybox, etc.),
  // so passes never name a Vulkan type. Native handles are read from m_sceneView here.
  struct ScreenPassTargets
  {
    uint64_t              colorImage{0};
    rhi::TextureViewHandle colorView{};
    uint64_t              depthImage{0};
    rhi::TextureViewHandle depthView{};
    rhi::TextureAspect    depthAspect{rhi::TextureAspect::depth};
    rhi::Extent2D         extent{};
    bool                  valid{false};
  };
  [[nodiscard]] ScreenPassTargets getScreenColorDepthTargets() const
  {
    ScreenPassTargets targets{};
    const bool ready = m_sceneView.sceneColorHdrImage != VK_NULL_HANDLE && !m_sceneView.sceneColorHdrView.isNull()
                       && m_sceneView.sceneDepthImage != VK_NULL_HANDLE && !m_sceneView.sceneDepthView.isNull()
                       && m_sceneView.sceneDepthExtent.width != 0u && m_sceneView.sceneDepthExtent.height != 0u;
    if(!ready)
    {
      return targets;
    }
    targets.colorImage  = reinterpret_cast<uint64_t>(m_sceneView.sceneColorHdrImage);
    targets.colorView   = m_sceneView.sceneColorHdrView;
    targets.depthImage  = reinterpret_cast<uint64_t>(m_sceneView.sceneDepthImage);
    targets.depthView   = m_sceneView.sceneDepthView;
    targets.depthAspect = depthAspectForFormat(m_sceneView.sceneDepthFormat);
    targets.extent      = {m_sceneView.sceneDepthExtent.width, m_sceneView.sceneDepthExtent.height};
    targets.valid       = true;
    return targets;
  }

  [[nodiscard]] static rhi::TextureAspect depthAspectForFormat(VkFormat format)
  {
    switch(format)
    {
      case VK_FORMAT_D16_UNORM_S8_UINT:
      case VK_FORMAT_D24_UNORM_S8_UINT:
      case VK_FORMAT_D32_SFLOAT_S8_UINT:
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
  void beginPresentPass(rhi::CommandList& cmd) { m_renderer.beginPresentPass(cmd); }
  void endPresentPass(rhi::CommandList& cmd) { m_renderer.endPresentPass(cmd); }
  void executeImGuiPass(rhi::CommandList& cmd, const RenderParams& params) { m_renderer.executeImGuiPass(cmd, params); }
  void bindStaticPassResources()
  {
    m_passExecutor.setResourceTable(m_renderer.getResourceTable());
    m_renderer.bindStaticPassResources(m_passExecutor);
  }
  void submitPassGraph(const RenderParams& params) { m_renderer.renderWithPassExecutor(params, m_passExecutor); }
  bool prepareAndDispatchVisibilityPatch(rhi::CommandBuffer& cmdBuffer,
                                         uint32_t            frameIndex,
                                         uint64_t            targetIndirectBufferHandle,
                                         uint32_t            categoryValue,
                                         uint32_t            outputOffset);
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
    BindGroupHandle bindGroup{};         // owned RHI ArgumentTable
    rhi::BufferHandle keyBufferHandle{};   // owned=false mirror, rebound on realloc
    rhi::BufferHandle valueBufferHandle{};
    rhi::BufferHandle uploadKeyBufferHandle{};    // owned=false mirror of staging buffer
    rhi::BufferHandle uploadValueBufferHandle{};
    uint32_t capacity{0};
    uint32_t activeElementCount{0};
    uint32_t paddedElementCount{0};
  };

  struct TransparentVisibilityFrameResources
  {
    std::array<utils::Buffer, 2>   prefixBuffers{};
    std::array<VkDescriptorSet, 2> descriptorSets{VK_NULL_HANDLE, VK_NULL_HANDLE};
    std::array<BindGroupHandle, 2> argumentTables{};  // Wave 9: adopted ArgumentTable handles for descriptorSets[0/1]
    std::array<uint64_t, 2>        boundSortKeyHandles{0, 0};
    std::array<uint64_t, 2>        boundSortValueHandles{0, 0};
    std::array<uint64_t, 2>        boundSourceIndirectHandles{0, 0};
    std::array<uint64_t, 2>        boundTargetIndirectHandles{0, 0};
    std::array<uint64_t, 2>        boundPrefixAHandles{0, 0};
    std::array<uint64_t, 2>        boundPrefixBHandles{0, 0};
    uint32_t                       prefixCapacity{0};
  };

  struct SortedBootstrapFrameState
  {
    uint32_t opaqueCapacity{0};
    uint32_t alphaCapacity{0};
    uint64_t sceneTopologyVersion{0};
    bool     valid{false};
  };

  static uint64_t packMeshHandleKey(MeshHandle handle);
  void            initVisibilitySortResources();
  void            shutdownVisibilitySortResources();
  void            ensureVisibilitySortCapacity(uint32_t frameIndex, uint32_t requiredCount);
  void            updateVisibilitySortDescriptorSet(uint32_t frameIndex);
  void            prepareVisibilitySortInputs(uint32_t frameIndex);
  void            initTransparentVisibilityPatchResources();
  void            shutdownTransparentVisibilityPatchResources();
  void            updateTransparentVisibilityPatchDescriptorSet(uint32_t frameIndex,
                                                               uint64_t sortKeyBufferHandle,
                                                               uint64_t sortValueBufferHandle,
                                                               uint64_t sourceIndirectBufferHandle,
                                                               uint64_t targetIndirectBufferHandle);
  [[nodiscard]] uint32_t getPreviousFrameIndex(uint32_t frameIndex) const;
  void            rebuildGPUDrivenScene(const GltfModel& model, const GltfUploadResult& uploadResult, VkCommandBuffer cmd);
  void            rebuildGPUDrivenScene(const SceneAsset& asset,
                                        const SceneUploadPlan& plan,
                                        const SceneUploadResult& uploadResult,
                                        VkCommandBuffer cmd);
  void            appendSceneObjectDraw(uint64_t meshKey, MeshHandle meshHandle, uint32_t drawIndex, SceneDrawBucket bucket);
  void            clearGPUDrivenScene();
  void            flushPendingSceneUploads();
  void            invalidateSortedBootstrapStates();
  void            invalidateSortedBootstrapState(uint32_t frameIndex);
  void            recordSortedBootstrapState(uint32_t frameIndex, uint32_t opaqueCapacity, uint32_t alphaCapacity);
  void            markPersistentDrawDirty(uint32_t drawIndex);
  [[nodiscard]] std::vector<DirtyRange> buildPersistentDrawDirtyRanges() const;
  void            uploadPersistentDrawData();
  void            refreshSceneView();
  void            updateOwnershipDiagnostics(uint32_t frameIndex, bool sceneRenderingSuspended, uint32_t safeObjectCount);
  void            initLightingResources();
  void            shutdownLightingResources();
  void            initIBLResources();
  void            shutdownIBLResources();
  void            updateGPUDrivenLights(const RenderParams& params, uint32_t frameIndex);
  void            updateLightingDescriptorSet(uint32_t frameIndex);
  void            initLightingPipelines();
  void            shutdownLightingPipelines();
  void            initPhase7Resources();
  void            shutdownPhase7Resources();
  void            resizePhase7Resources();
  void            bindPhase7PassResources();
  void            initPhase7Pipelines();
  void            shutdownPhase7Pipelines();
  void            updatePhase7Descriptors(uint32_t frameIndex);

  RenderDevice                           m_renderer;
  PassExecutor                       m_passExecutor;
  GPUSceneRegistry                   m_sceneRegistry;
  HiZDepthPyramid                    m_hiZDepthPyramid;
  GPUDrivenLightResources            m_lightResources;
  GPUMeshletBuffer                   m_meshletBuffer;
  rhi::BufferHandle                  m_shadowPackedVertexBufferRHI{};
  rhi::BufferHandle                  m_shadowPackedIndexBufferRHI{};
  std::vector<shaderio::Meshlet>     m_meshletDataCpu;
  std::vector<uint32_t>              m_meshletIndicesCpu;
  std::vector<shaderio::GPUCullObject> m_meshletCullObjectsCpu;
  std::vector<uint32_t>              m_visibilitySortInputObjects;
  std::vector<uint32_t>              m_visibilitySortInputKeys;
  std::vector<uint32_t>              m_cachedStaticVisibilitySortObjects;
  std::vector<uint32_t>              m_cachedStaticVisibilitySortKeys;
  uint64_t                           m_cachedStaticVisibilitySortTopologyVersion{0};
  std::vector<shaderio::DrawUniforms> m_persistentDrawData;
  std::vector<SceneUploadResult::SceneDrawRecord> m_sceneDrawRecords;
  std::vector<uint32_t>              m_dirtyPersistentDrawIndices;
  std::unique_ptr<GPUDrivenDepthPrepass> m_depthPrepass;
  std::unique_ptr<GPUDrivenDepthPyramidPass> m_depthPyramidPass;
  std::unique_ptr<GPUDrivenCullingPass>      m_gpuCullingPass;
  std::unique_ptr<GPUDrivenVisibilitySortPass> m_visibilitySortPass;
  std::unique_ptr<GPUDrivenLightCullingPass> m_lightCullingPass;
  std::unique_ptr<GPUDrivenClusteredLightCullingPass> m_clusteredLightCullingPass;
  std::unique_ptr<GPUDrivenCSMShadowPass>    m_csmShadowPass;
  std::unique_ptr<GPUDrivenShadowAtlasPass>  m_shadowAtlasPass;
  std::unique_ptr<GPUDrivenGBufferPass>  m_gbufferPass;
  std::unique_ptr<GPUDrivenLightPass>    m_lightPass;
  std::unique_ptr<GPUDrivenSkyboxPass>    m_skyboxPass;
  std::unique_ptr<GPUDrivenAOPass>        m_aoPass;
  std::unique_ptr<GPUDrivenSSRPass>       m_ssrPass;
  std::unique_ptr<GPUDrivenVelocityPass>  m_velocityPass;
  std::unique_ptr<GPUDrivenTAAResolvePass> m_taaResolvePass;
  std::unique_ptr<GPUDrivenBloomPrefilterPass> m_bloomPrefilterPass;
  std::unique_ptr<GPUDrivenBloomDownsamplePass> m_bloomDownsamplePass;
  std::unique_ptr<GPUDrivenFinalColorPass> m_finalColorPass;
  std::unique_ptr<GPUDrivenForwardPass>  m_forwardPass;
  std::unique_ptr<GPUDrivenDebugPass>    m_debugPass;
  std::unique_ptr<GPUDrivenPresentPass>  m_presentPass;
  std::unique_ptr<GPUDrivenImguiPass>    m_imguiPass;
  GPUDrivenSceneView                 m_sceneView{};
  GPUDrivenRuntimeStats              m_runtimeStats{};
  SceneUploadResult                  m_activeUploadResultStorage{};
  const SceneUploadResult*           m_activeUploadResult{nullptr};
  std::unordered_map<uint64_t, uint32_t> m_objectIdByMeshHandle;
  std::unordered_map<uint64_t, std::vector<uint32_t>> m_objectIdsByMeshHandle;
  std::unordered_map<uint64_t, uint32_t> m_drawIndexByMeshHandle;
  std::unordered_map<uint64_t, glm::mat4> m_previousTransformByMeshHandle;
  std::unordered_map<uint32_t, glm::mat4> m_previousTransformByDrawIndex;
  std::vector<uint32_t>              m_objectIdByDrawRecord;
  std::vector<uint32_t>              m_drawIndexByDrawRecord;
  std::vector<MeshHandle>            m_meshHandleByDrawIndex;
  std::vector<uint32_t>              m_opaqueDrawIndices;
  std::vector<uint32_t>              m_alphaTestDrawIndices;
  std::vector<uint32_t>              m_transparentDrawIndices;
  rhi::ArgumentLayoutHandle          m_visibilitySortArgumentLayout{};
  std::unique_ptr<rhi::PipelineLayout> m_visibilitySortPipelineLayout;
  VkPipeline                         m_visibilitySortPipeline{VK_NULL_HANDLE};
  PipelineHandle                     m_visibilitySortPipelineHandle{};
  std::vector<VisibilitySortFrameResources> m_visibilitySortFrames;
  VkDescriptorPool                   m_transparentVisibilityPatchDescriptorPool{VK_NULL_HANDLE};
  VkDescriptorSetLayout              m_transparentVisibilityPatchSetLayout{VK_NULL_HANDLE};
  VkPipelineLayout                   m_transparentVisibilityPatchPipelineLayout{VK_NULL_HANDLE};
  VkPipeline                         m_transparentVisibilityPatchPipeline{VK_NULL_HANDLE};
  PipelineHandle                     m_transparentVisibilityPatchPipelineHandle{};  // Wave 9: adopted compute pipeline handle
  std::vector<TransparentVisibilityFrameResources> m_transparentVisibilityPatchFrames;
  // Wave 9: lighting-input (set LSetTextures) and lighting-scene (set LSetScene) are RHI
  // ArgumentLayouts + owned ArgumentTables. The coarse-culling set (point/spot + clustered)
  // is likewise RHI; its 9 light-resource buffers are mirrored as owned=false RHI handles.
  rhi::ArgumentLayoutHandle          m_lightingArgumentLayout{};
  rhi::ArgumentLayoutHandle          m_lightingSceneArgumentLayout{};
  rhi::ArgumentLayoutHandle          m_lightCoarseCullingArgumentLayout{};
  std::vector<rhi::BufferHandle>     m_lightCoarseCullingBufferHandles;
  // Stable light-resource buffer handles (owned=false) consumed by the lighting-input set.
  std::vector<rhi::BufferHandle>     m_lightingInputBufferHandles;
  std::vector<BindGroupHandle>       m_lightCoarseCullingBindGroups;
  std::vector<BindGroupHandle>       m_lightingSceneBindGroups;
  std::vector<BindGroupHandle>       m_lightingInputBindGroups;
  VkSampler                          m_linearClampSampler{VK_NULL_HANDLE};
  rhi::SamplerHandle                 m_linearClampSamplerHandle{};
  rhi::SamplerHandle                 m_iblCubeSamplerHandle{};
  rhi::SamplerHandle                 m_iblLutSamplerHandle{};
  // Adopted (owned=false) RHI view handles for the per-frame native VkImageViews fed into
  // the lighting-input set; re-registered when the underlying native view changes.
  rhi::TextureViewHandle             m_iblEnvViewHandle{};      VkImageView m_iblEnvViewNative{VK_NULL_HANDLE};
  rhi::TextureViewHandle             m_aoViewHandle{};          VkImageView m_aoViewNative{VK_NULL_HANDLE};
  rhi::TextureViewHandle             m_ssrViewHandle{};         VkImageView m_ssrViewNative{VK_NULL_HANDLE};
  rhi::TextureViewHandle             m_shadowMapViewHandle{};   VkImageView m_shadowMapViewNative{VK_NULL_HANDLE};
  std::unique_ptr<rhi::PipelineLayout> m_lightCoarseCullingPipelineLayout;
  std::unique_ptr<rhi::PipelineLayout> m_lightPipelineLayout;
  PipelineHandle                     m_pointLightCoarseCullingPipeline{};
  PipelineHandle                     m_spotLightCoarseCullingPipeline{};
  // Fullscreen graphics pipelines now live in the device pipeline registry; only
  // their handles are tracked here. The native VkPipeline is owned by the registry.
  PipelineHandle                     m_gpuDrivenLightHdrPipeline{};
  PipelineHandle                     m_gpuDrivenSkyboxPipeline{};
  PipelineHandle                     m_gpuDrivenTAAResolvePipeline{};
  PipelineHandle                     m_gpuDrivenBloomPrefilterPipeline{};
  PipelineHandle                     m_gpuDrivenBloomDownsamplePipeline{};
  PipelineHandle                     m_gpuDrivenBloomUpsamplePipeline{};
  PipelineHandle                     m_gpuDrivenFinalColorPipeline{};
  PipelineHandle                     m_gpuDrivenVelocityPipeline{};
  VkPipeline                         m_pointLightCoarseCullingVkPipeline{VK_NULL_HANDLE};
  VkPipeline                         m_spotLightCoarseCullingVkPipeline{VK_NULL_HANDLE};
  VkPipeline                         m_clusteredLightCullingVkPipeline{VK_NULL_HANDLE};
  PipelineHandle                     m_clusteredLightCullingPipeline{};
  // Wave 9: AO set is an RHI ArgumentLayout + owned ArgumentTables; AO/SSR pipeline
  // layouts are RHI VulkanPipelineLayouts (SSR set layout comes from m_ssrLayoutHandle).
  rhi::ArgumentLayoutHandle          m_aoArgumentLayout{};
  std::unique_ptr<rhi::PipelineLayout> m_aoPipelineLayout;
  std::unique_ptr<rhi::PipelineLayout> m_ssrPipelineLayout;
  VkPipeline                         m_gtaoPipeline{VK_NULL_HANDLE};
  VkPipeline                         m_aoDenoisePipeline{VK_NULL_HANDLE};
  VkPipeline                         m_ssrTracePipeline{VK_NULL_HANDLE};
  // Adopted (owned=false) view handles for the AO raw / denoised storage images, fed into
  // the AO ArgumentTables; re-registered when the native view changes.
  rhi::TextureViewHandle             m_aoRawViewHandle{};       VkImageView m_aoRawViewNative{VK_NULL_HANDLE};
  rhi::TextureViewHandle             m_aoDenoisedViewHandle{};  VkImageView m_aoDenoisedViewNative{VK_NULL_HANDLE};
  // Phase 6: RHI handles for the Phase-7 compute pipelines + adopted bind groups,
  // so the AO/SSR passes record through cmd-> verbs instead of raw vkCmd*. AO uses
  // persistent adopted sets; SSR builds a per-frame temporary bind group from
  // m_ssrLayoutHandle (see acquireSSRTempBindGroup).
  PipelineHandle                     m_gtaoPipelineHandle{};
  PipelineHandle                     m_aoDenoisePipelineHandle{};
  PipelineHandle                     m_ssrTracePipelineHandle{};
  rhi::ArgumentLayoutHandle          m_ssrLayoutHandle{};
  rhi::TextureViewHandle             m_ssrRawViewHandle{};  // Wave 8: adopted storage-image view for SSR temp ArgumentWrite
  rhi::TextureViewHandle             m_shadowAtlasViewHandle{};  // Wave 9: registry handle for the shadow-atlas depth view
  std::vector<BindGroupHandle>       m_aoBindGroups;
  std::vector<BindGroupHandle>       m_aoDenoiseBindGroups;
  utils::ImageResource               m_aoRaw{};
  utils::ImageResource               m_aoDenoised{};
  utils::ImageResource               m_ssrRaw{};
  utils::ImageResource               m_shadowAtlas{};
  VkExtent2D                         m_phase7HalfExtent{};
  VkExtent2D                         m_shadowAtlasExtent{2048u, 2048u};
  uint32_t                           m_shadowAtlasTileSize{512u};
  uint32_t                           m_shadowAtlasAllocatedTiles{0};
  std::vector<shaderio::LightData>   m_gpuDrivenPointLights;
  std::vector<shaderio::LightData>   m_gpuDrivenSpotLights;
  utils::ImageResource               m_iblEnvironment{};
  IBLResources                       m_iblResources{};
  std::vector<utils::Buffer>         m_gpuDrivenStagingBuffers;
  VkFormat                           m_iblEnvironmentFormat{VK_FORMAT_UNDEFINED};
  VkExtent2D                         m_iblEnvironmentExtent{};
  uint32_t                           m_iblEnvironmentMipCount{0};
  uint64_t                           m_iblEnvironmentEstimatedBytes{0};
  bool                               m_iblEnvironmentLoaded{false};
  bool                               m_iblUsingFallback{true};
  std::string                        m_iblEnvironmentPath;
  std::string                        m_iblEnvironmentStatus{"Not initialized"};
  std::vector<SortedBootstrapFrameState> m_sortedBootstrapFrames;
  uint64_t                           m_sceneTopologyVersion{1};
  bool                               m_enableExperimentalMeshletPath{false};
  bool                               m_suspendSceneRendering{false};
  bool                               m_sceneUploadPending{false};
  bool                               m_persistentDrawDataDirty{false};
  bool                               m_previousTransformResetPending{false};
  bool                               m_hiZCameraHistoryValid{false};
  glm::vec3                          m_lastHiZCameraPosition{0.0f};
  float                              m_lastHiZCameraDeltaDistance{0.0f};
  bool                               m_lastHiZFastCameraFallbackTriggered{false};
  shaderio::CameraUniforms           m_temporalCameraUniforms{};
  shaderio::CameraUniforms           m_previousCameraUniforms{};
  glm::mat4                          m_previousJitteredViewProjection{1.0f};
  glm::vec2                          m_currentTAAJitterUv{0.0f};
  glm::vec2                          m_previousTAAJitterUv{0.0f};
  bool                               m_previousCameraValid{false};
  bool                               m_taaHistoryValid{false};
  uint64_t                           m_temporalFrameCounter{0};
};

}  // namespace demo
