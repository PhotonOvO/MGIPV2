#pragma once

#include "GPUSceneRegistry.h"
#include "GPUMeshletBuffer.h"
#include "HiZDepthPyramid.h"
#include "MeshletConverter.h"
#include "passes/DebugPass.h"
#include "passes/ForwardPass.h"
#include "passes/GPUDrivenCSMShadowPass.h"
#include "passes/GPUDrivenCullingPass.h"
#include "passes/GPUDrivenDebugPass.h"
#include "passes/GPUDrivenDepthPrepass.h"
#include "passes/GPUDrivenDepthPyramidPass.h"
#include "passes/GPUDrivenForwardPass.h"
#include "passes/GPUDrivenGBufferPass.h"
#include "passes/GPUDrivenImguiPass.h"
#include "passes/GPUDrivenLightCullingPass.h"
#include "passes/GPUDrivenLightPass.h"
#include "passes/GPUDrivenBloomPrefilterPass.h"
#include "passes/GPUDrivenBloomDownsamplePass.h"
#include "passes/GPUDrivenFinalColorPass.h"
#include "passes/GPUDrivenVelocityPass.h"
#include "passes/GPUDrivenTAAResolvePass.h"
#include "passes/GPUDrivenPresentPass.h"
#include "passes/GPUDrivenVisibilitySortPass.h"
#include "Renderer.h"

#include <array>
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
  float    bloomThreshold{1.0f};
  float    colorSaturation{1.0f};
  float    colorContrast{1.0f};
  float    colorGamma{1.0f};
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
  [[nodiscard]] PipelineHandle getLightPipelineHandle() const { return m_renderer.getLightPipelineHandle(); }
  [[nodiscard]] PipelineHandle getGPUDrivenLightHdrPipelineHandle() const
  {
    return m_renderer.getGPUDrivenLightHdrPipelineHandle();
  }
  [[nodiscard]] PipelineHandle getBloomPrefilterPipelineHandle() const
  {
    return m_renderer.getBloomPrefilterPipelineHandle();
  }
  [[nodiscard]] PipelineHandle getBloomDownsamplePipelineHandle() const
  {
    return m_renderer.getBloomDownsamplePipelineHandle();
  }
  [[nodiscard]] PipelineHandle getFinalColorPipelineHandle() const
  {
    return m_renderer.getFinalColorPipelineHandle();
  }
  [[nodiscard]] PipelineHandle getVelocityPipelineHandle() const
  {
    return m_renderer.getVelocityPipelineHandle();
  }
  [[nodiscard]] PipelineHandle getTAAResolvePipelineHandle() const
  {
    return m_renderer.getTAAResolvePipelineHandle();
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
  [[nodiscard]] PipelineHandle getLightCullingPipelineHandle() const
  {
    return m_renderer.getLightCullingPipelineHandle();
  }
  [[nodiscard]] PipelineHandle getSpotLightCullingPipelineHandle() const
  {
    return m_renderer.getSpotLightCoarseCullingPipelineHandle();
  }
  [[nodiscard]] uint64_t getGraphicsScenePipelineLayout() const { return m_renderer.getGraphicsScenePipelineLayout(); }
  [[nodiscard]] uint64_t getGraphicsMDIPipelineLayout() const { return m_renderer.getGraphicsMDIPipelineLayout(); }
  [[nodiscard]] uint64_t getGraphicsMaterialDescriptorSet() const { return m_renderer.getGraphicsMaterialDescriptorSet(); }
  [[nodiscard]] uint64_t getLightPipelineLayout() const { return m_renderer.getLightPipelineLayout(); }
  [[nodiscard]] uint64_t getPostProcessPipelineLayout() const { return m_renderer.getPostProcessPipelineLayout(); }
  [[nodiscard]] uint64_t getLightingInputDescriptorSet() const { return m_renderer.getLightingInputDescriptorSet(); }
  [[nodiscard]] uint64_t getCSMShadowPipelineLayout() const { return m_renderer.getCSMShadowPipelineLayout(); }
  [[nodiscard]] uint64_t getDebugPipelineLayout() const { return m_renderer.getDebugPipelineLayout(); }
  [[nodiscard]] uint64_t getShadowCullingPipelineLayout() const { return m_renderer.getShadowCullingPipelineLayout(); }
  [[nodiscard]] uint64_t getGPUCullingPipelineLayout() const { return m_renderer.getGPUCullingPipelineLayout(); }
  [[nodiscard]] uint64_t getLightCullingPipelineLayout() const { return m_renderer.getLightCullingPipelineLayout(); }
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
  [[nodiscard]] uint64_t getShadowCullingIndirectBufferOpaque(uint32_t frameIndex) const
  {
    return m_renderer.getShadowCullingIndirectBufferOpaque(frameIndex);
  }
  [[nodiscard]] uint32_t getShadowCullingMeshCapacity(uint32_t frameIndex) const
  {
    return m_renderer.getShadowCullingMeshCapacity(frameIndex);
  }
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
  [[nodiscard]] uint64_t getPreviousGPUDrivenPersistentIndirectStreamBuffer(uint32_t frameIndex) const
  {
    return m_renderer.getGPUDrivenPersistentIndirectStreamBuffer(getPreviousFrameIndex(frameIndex));
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
  [[nodiscard]] uint64_t getCurrentLightCullingDescriptorSet() const
  {
    return m_renderer.getLightCullingDescriptorSet();
  }
  void updateLightCoarseCullingResources(uint32_t frameIndex, const shaderio::LightCoarseCullingUniforms& uniforms)
  {
    m_renderer.updateLightCoarseCullingResources(frameIndex, uniforms);
  }
  [[nodiscard]] uint32_t getActivePointLightCount() const { return m_renderer.getActivePointLightCount(); }
  [[nodiscard]] uint32_t getActiveSpotLightCount() const { return m_renderer.getActiveSpotLightCount(); }
  [[nodiscard]] VkExtent2D getSceneExtent() const { return m_renderer.getSceneExtent(); }
  [[nodiscard]] VkFormat getSceneDepthFormat() const { return m_renderer.getSceneDepthFormat(); }
  [[nodiscard]] VkImage getSceneDepthImage() const { return m_renderer.getSceneDepthImage(); }
  [[nodiscard]] VkImageView getSceneDepthImageView() const { return m_renderer.getSceneDepthImageView(); }
  [[nodiscard]] VkImage getSceneGBufferImage(uint32_t index) const { return m_renderer.getSceneGBufferImage(index); }
  [[nodiscard]] VkImageView getSceneGBufferImageView(uint32_t index) const
  {
    return m_renderer.getSceneGBufferImageView(index);
  }
  [[nodiscard]] VkImage getOutputTextureImage() const { return m_renderer.getOutputTextureImage(); }
  [[nodiscard]] VkImageView getOutputTextureView() const { return m_renderer.getOutputTextureView(); }
  [[nodiscard]] VkFormat getOutputTextureFormat() const { return m_renderer.getOutputTextureFormat(); }
  [[nodiscard]] uint64_t getOutputTextureEstimatedBytes() const
  {
    return m_renderer.getOutputTextureEstimatedBytes();
  }
  [[nodiscard]] VkImage getSceneColorHdrImage() const { return m_renderer.getSceneColorHdrImage(); }
  [[nodiscard]] VkImageView getSceneColorHdrView() const { return m_renderer.getSceneColorHdrView(); }
  [[nodiscard]] VkFormat getSceneColorHdrFormat() const { return m_renderer.getSceneColorHdrFormat(); }
  [[nodiscard]] uint64_t getSceneColorHdrEstimatedBytes() const
  {
    return m_renderer.getSceneColorHdrEstimatedBytes();
  }
  [[nodiscard]] VkImage getBloomHalfImage() const { return m_renderer.getBloomHalfImage(); }
  [[nodiscard]] VkImageView getBloomHalfView() const { return m_renderer.getBloomHalfView(); }
  [[nodiscard]] VkExtent2D getBloomHalfExtent() const { return m_renderer.getBloomHalfExtent(); }
  [[nodiscard]] VkImage getBloomQuarterImage() const { return m_renderer.getBloomQuarterImage(); }
  [[nodiscard]] VkImageView getBloomQuarterView() const { return m_renderer.getBloomQuarterView(); }
  [[nodiscard]] VkExtent2D getBloomQuarterExtent() const { return m_renderer.getBloomQuarterExtent(); }
  [[nodiscard]] uint64_t getBloomEstimatedBytes() const { return m_renderer.getBloomEstimatedBytes(); }
  [[nodiscard]] VkImage getVelocityImage() const { return m_renderer.getVelocityImage(); }
  [[nodiscard]] VkImageView getVelocityView() const { return m_renderer.getVelocityView(); }
  [[nodiscard]] VkFormat getVelocityFormat() const { return m_renderer.getVelocityFormat(); }
  [[nodiscard]] uint64_t getVelocityEstimatedBytes() const { return m_renderer.getVelocityEstimatedBytes(); }
  [[nodiscard]] VkImage getSceneColorHistoryImage(uint32_t index) const
  {
    return m_renderer.getSceneColorHistoryImage(index);
  }
  [[nodiscard]] VkImageView getSceneColorHistoryView(uint32_t index) const
  {
    return m_renderer.getSceneColorHistoryView(index);
  }
  [[nodiscard]] uint64_t getSceneColorHistoryEstimatedBytes() const
  {
    return m_renderer.getSceneColorHistoryEstimatedBytes();
  }
  [[nodiscard]] VkExtent2D getSwapchainExtent() const { return m_renderer.getSwapchainExtent(); }
  [[nodiscard]] VkImage getCurrentSwapchainImage() const { return m_renderer.getCurrentSwapchainImage(); }
  [[nodiscard]] uint32_t getCurrentFrameIndexHint() const { return m_renderer.getCurrentFrameIndexHint(); }
  [[nodiscard]] VkDevice getNativeDeviceHandle() const { return m_renderer.getNativeDeviceHandle(); }
  [[nodiscard]] VmaAllocator getAllocatorHandle() const { return m_renderer.getAllocatorHandle(); }
  [[nodiscard]] uint64_t getNativeComputePipeline(PipelineHandle pipelineHandle) const
  {
    return m_renderer.getPipelineOpaque(pipelineHandle, static_cast<uint32_t>(VK_PIPELINE_BIND_POINT_COMPUTE));
  }
  [[nodiscard]] uint64_t getNativeGraphicsPipeline(PipelineHandle pipelineHandle) const
  {
    return m_renderer.getPipelineOpaque(pipelineHandle, static_cast<uint32_t>(VK_PIPELINE_BIND_POINT_GRAPHICS));
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
  void executeDepthPyramidPass(rhi::CommandList& cmd, const RenderParams& params);
  void executeGPUCullingPass(rhi::CommandList& cmd, const RenderParams& params);
  void executeLightCullingPass(rhi::CommandList& cmd, const RenderParams& params);
  void executeCSMShadowPass(const PassContext& context);
  void executeDebugPass(const PassContext& context);
  void executePresentPass(const PassContext& context);
  void executeImguiPass(const PassContext& context);
  void beginPresentPass(rhi::CommandList& cmd) { m_renderer.beginPresentPass(cmd); }
  void endPresentPass(rhi::CommandList& cmd) { m_renderer.endPresentPass(cmd); }
  void executeImGuiPass(rhi::CommandList& cmd, const RenderParams& params) { m_renderer.executeImGuiPass(cmd, params); }
  void bindStaticPassResources() { m_renderer.bindStaticPassResources(m_passExecutor); }
  void submitPassGraph(const RenderParams& params) { m_renderer.renderWithPassExecutor(params, m_passExecutor); }
  void executeVisibilitySortPass(const PassContext& context) const;
  bool prepareAndDispatchVisibilityPatch(rhi::CommandList& cmd,
                                         uint32_t          frameIndex,
                                         uint64_t          targetIndirectBufferHandle,
                                         uint32_t          categoryValue,
                                         uint32_t          outputOffset);
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
    VkDescriptorSet descriptorSet{VK_NULL_HANDLE};
    uint32_t capacity{0};
    uint32_t activeElementCount{0};
    uint32_t paddedElementCount{0};
  };

  struct TransparentVisibilityFrameResources
  {
    std::array<utils::Buffer, 2>   prefixBuffers{};
    std::array<VkDescriptorSet, 2> descriptorSets{VK_NULL_HANDLE, VK_NULL_HANDLE};
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
  [[nodiscard]] uint32_t getSafePersistentObjectCount() const;

  Renderer                           m_renderer;
  PassExecutor                       m_passExecutor;
  GPUSceneRegistry                   m_sceneRegistry;
  HiZDepthPyramid                    m_hiZDepthPyramid;
  GPUMeshletBuffer                   m_meshletBuffer;
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
  std::unique_ptr<GPUDrivenCSMShadowPass>    m_csmShadowPass;
  std::unique_ptr<GPUDrivenGBufferPass>  m_gbufferPass;
  std::unique_ptr<GPUDrivenLightPass>    m_lightPass;
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
  VkDescriptorPool                   m_visibilitySortDescriptorPool{VK_NULL_HANDLE};
  VkDescriptorSetLayout              m_visibilitySortSetLayout{VK_NULL_HANDLE};
  VkPipelineLayout                   m_visibilitySortPipelineLayout{VK_NULL_HANDLE};
  VkPipeline                         m_visibilitySortPipeline{VK_NULL_HANDLE};
  std::vector<VisibilitySortFrameResources> m_visibilitySortFrames;
  VkDescriptorPool                   m_transparentVisibilityPatchDescriptorPool{VK_NULL_HANDLE};
  VkDescriptorSetLayout              m_transparentVisibilityPatchSetLayout{VK_NULL_HANDLE};
  VkPipelineLayout                   m_transparentVisibilityPatchPipelineLayout{VK_NULL_HANDLE};
  VkPipeline                         m_transparentVisibilityPatchPipeline{VK_NULL_HANDLE};
  std::vector<TransparentVisibilityFrameResources> m_transparentVisibilityPatchFrames;
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
  bool                               m_previousCameraValid{false};
  bool                               m_taaHistoryValid{false};
  uint64_t                           m_temporalFrameCounter{0};
};

}  // namespace demo
