#pragma once

#include "../common/Common.h"
#include "../common/Handles.h"
#include "../common/HandlePool.h"
#include "../common/TracyProfiling.h"
#include "BindGroups.h"
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
#include "../rhi/vulkan/VulkanResourceTable.h"
#include "../rhi/RHICommandList.h"
#include "../rhi/RHIFrameContext.h"
#include "../rhi/RHIDevice.h"
#include "../rhi/RHIBindlessTypes.h"
#include "../rhi/RHIPipeline.h"
#include "../rhi/RHISwapchain.h"
#include "../rhi/RHISurface.h"
#include <functional>
#include <optional>
#include <span>
#include <string>
#include <unordered_map>

namespace demo {
namespace rhi {
namespace vulkan {
class VulkanBindTableLayout;  // Forward declaration for shared layout ownership
}
}

namespace rhi {
struct BindTableWrite;
}

class RenderDevice
{
public:
  static constexpr uint32_t kDemoMaterialSlotCount = 2;

  // Register a bind group that adopts caller-owned BindTable/BindTableLayout
  // objects (e.g. wrapping an externally-managed descriptor set). The caller must
  // keep those objects alive for the lifetime of the returned handle.
  BindGroupHandle registerExternalBindGroup(BindGroupDesc desc) { return createBindGroup(std::move(desc)); }

  RenderDevice() = default;

  void init(void* window, rhi::Surface& surface, bool vSync);
  void shutdown(rhi::Surface& surface);
  void setVSync(bool enabled);
  [[nodiscard]] bool        getVSync() const { return m_swapchainDependent.vSync; }
  void setFullscreen(bool enabled, void* platformHandle = nullptr);
  [[nodiscard]] const char* getSwapchainPresentModeName() const;
  void resize(rhi::Extent2D size);
  void renderWithPassExecutor(const RenderParams& params, PassExecutor& passExecutor);

  // Pass execution helpers (wrappers for per-pass commands)
  void executeImGuiPass(rhi::CommandList& cmd, const RenderParams& params);
  void beginPresentPass(rhi::CommandList& cmd);
  void endPresentPass(rhi::CommandList& cmd);
  [[nodiscard]] rhi::ResourceIndex getSceneBindlessResourceIndex() const { return kSceneBindlessInfoIndex; }

  TextureHandle  getViewportTextureHandle() const;
  ImTextureID    getViewportTextureID(TextureHandle handle) const;
  MaterialHandle getMaterialHandle(uint32_t slot) const;

  // glTF model support
  GltfUploadResult uploadGltfModel(const GltfModel& model, VkCommandBuffer cmd);
  SceneUploadResult commitSceneUploadPlan(const SceneAsset& asset, const SceneUploadPlan& plan, VkCommandBuffer cmd);
  void             uploadGltfModelBatch(const GltfModel&          model,
                                        std::span<const uint32_t> textureIndices,
                                        std::span<const uint32_t> materialIndices,
                                        std::span<const uint32_t> meshIndices,
                                        GltfUploadResult&         ioResult,
                                        VkCommandBuffer           cmd);
  void             initializeGltfUploadResult(const GltfModel& model, GltfUploadResult& outResult) const;
  void             destroyGltfResources(const GltfUploadResult& result);
  void             updateMeshTransform(MeshHandle handle, const glm::mat4& transform);

  // Execute upload commands with internal command buffer management
  void executeUploadCommand(std::function<void(VkCommandBuffer)> uploadFn);

  MeshPool& getMeshPool() { return m_meshPool; }
  const MeshPool& getMeshPool() const { return m_meshPool; }
  rhi::vulkan::VulkanResourceTable* getResourceTable() { return &m_device.resourceTable; }
  SceneResources& getSceneResources() { return m_swapchainDependent.sceneResources; }
  void            bindStaticPassResources(PassExecutor& passExecutor) const;
  void      waitForIdle();
  [[nodiscard]] VkDevice getNativeDeviceHandle() const;
  [[nodiscard]] VmaAllocator getAllocatorHandle() const { return m_device.allocator; }
  [[nodiscard]] VkExtent2D getSceneExtent() const { return m_swapchainDependent.sceneResources.getSize(); }

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
  void executeLightCoarseCullingPass(rhi::CommandList& cmd, const RenderParams& params);
  void executeDepthPyramidPass(rhi::CommandList& cmd, const RenderParams& params);
  void executeGPUCullingPass(rhi::CommandList& cmd, const RenderParams& params);
  PipelineHandle getGPUCullingPipelineHandle() const;
  uint64_t       getGPUCullingPipelineLayout() const;
  PipelineHandle getCSMShadowPipelineHandle() const;  // CSM shadow depth pipeline
  uint64_t       getCSMShadowPipelineLayout() const;
  PipelineHandle getShadowCullingPipelineHandle() const;
  uint64_t       getShadowCullingPipelineLayout() const;
  uint64_t       getShadowCullingDescriptorSetOpaque(uint32_t frameIndex) const;
  uint64_t       getGPUCullingDescriptorSetOpaque(uint32_t frameIndex) const;
  [[nodiscard]] BindGroupHandle getGPUCullingBindGroup(uint32_t frameIndex) const
  {
    return frameIndex < m_device.gpuCullingBindGroups.size() ? m_device.gpuCullingBindGroups[frameIndex] : BindGroupHandle{};
  }
  [[nodiscard]] BindGroupHandle getShadowCullingBindGroup(uint32_t frameIndex) const
  {
    return frameIndex < m_device.shadowCullingBindGroups.size() ? m_device.shadowCullingBindGroups[frameIndex] : BindGroupHandle{};
  }
  uint64_t       getShadowCullingIndirectBufferOpaque(uint32_t frameIndex) const;
  [[nodiscard]] uint32_t getShadowCullingMeshCapacity(uint32_t frameIndex) const;
  PipelineHandle getLightCullingPipelineHandle() const;
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
  [[nodiscard]] rhi::BufferHandle getPreviousGPUDrivenPersistentIndirectStreamBufferRHIHandle(uint32_t currentFrameIndex) const;
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
  // registry in init). Replaces the raw per-layer VkImageView CSMShadowResources used to own.
  [[nodiscard]] rhi::TextureViewHandle getCSMCascadeViewHandle(uint32_t cascadeIndex) const
  {
    return cascadeIndex < m_csmCascadeViewHandles.size() ? m_csmCascadeViewHandles[cascadeIndex] : rhi::TextureViewHandle{};
  }
  const shaderio::CameraUniforms& getShadowCameraUniforms() const { return m_frameLightingState.shadowCamera; }
  const shaderio::LightParams& getLightPassParams() const { return m_frameLightingState.lightParams; }
  [[nodiscard]] shaderio::ShadowCullPushConstants buildShadowCullPushConstants(uint32_t cascadeIndex, uint32_t objectCount) const;
  const std::vector<shaderio::DebugLineVertex>& getDebugLineVertices() const { return m_debugDrawList.vertices; }
  uint64_t       getLightPipelineLayout() const;
  uint64_t       getPostProcessPipelineLayout() const;
  uint64_t       getLightCullingPipelineLayout() const;
  uint64_t       getDebugPipelineLayout() const;
  uint64_t       getGraphicsPipelineLayout() const;  // Graphics pipeline layout for descriptor binding
  uint64_t       getGBufferPipelineLayout() const;   // GBuffer-specific pipeline layout
  uint64_t       getMDIPipelineLayout() const;
  uint64_t       getGraphicsScenePipelineLayout() const;
  uint64_t       getGraphicsMDIPipelineLayout() const;
  uint64_t       getGBufferColorDescriptorSet() const;  // Material bindless texture array
  uint64_t       getGBufferTextureDescriptorSet() const; // GBuffer textures for LightPass
  uint64_t       getGBufferTextureSetLayout() const;      // Layout of the GBuffer texture set
  uint64_t       getGBufferTextureDescriptorSetAt(uint32_t frameIndex) const; // Per-frame GBuffer texture set
  uint32_t       getFrameResourceCount() const;           // Number of per-frame resource slots
  uint64_t       getGraphicsMaterialDescriptorSet() const;
  [[nodiscard]] BindGroupHandle getGraphicsMaterialBindGroup() const;
  uint64_t       getLightingInputDescriptorSet() const;
  uint64_t       getLightCullingDescriptorSet() const;
  [[nodiscard]] bool getIBLEnvironmentLoaded() const;
  [[nodiscard]] bool getIBLUsingFallback() const;
  [[nodiscard]] VkFormat getIBLEnvironmentFormat() const;
  [[nodiscard]] VkExtent2D getIBLEnvironmentExtent() const;
  [[nodiscard]] uint32_t getIBLEnvironmentMipCount() const;
  [[nodiscard]] uint64_t getIBLEnvironmentEstimatedBytes() const;
  [[nodiscard]] const std::string& getIBLEnvironmentPath() const;
  [[nodiscard]] const std::string& getIBLEnvironmentStatus() const;
  void updateLightCoarseCullingResources(uint32_t frameIndex, const shaderio::LightCoarseCullingUniforms& uniforms);
  uint64_t       getPipelineOpaque(PipelineHandle handle, uint32_t expectedBindPoint) const;
  // Registers an externally-created graphics VkPipeline (and the layout it was
  // built with) into the device pipeline registry, returning a real handle so it
  // resolves through the same path as device-owned pipelines. Ownership of the
  // VkPipeline transfers to the registry (destroyed by destroyPipelines()).
  PipelineHandle registerExternalGraphicsPipeline(VkPipeline pipeline, VkPipelineLayout layout,
                                                  uint32_t specializationVariant = 0);
  // Compute twin of registerExternalGraphicsPipeline. Registers an externally-created
  // compute VkPipeline together with its layout so it resolves through the registry
  // and cmd->bindBindGroup can recover the layout. Ownership stays with the caller
  // (these pipelines are destroyed by their owning subsystem, not destroyPipelines()).
  PipelineHandle registerExternalComputePipeline(VkPipeline pipeline, VkPipelineLayout layout,
                                                 uint32_t specializationVariant = 0);
  // Drops a registry record without destroying the native pipeline. Use before a
  // caller-owned (externally-registered) pipeline is recreated/destroyed so the
  // table never resolves a stale handle.
  void unregisterExternalPipeline(PipelineHandle handle);
  [[nodiscard]] uint64_t getGPUCullingObjectBufferAddress(uint32_t frameIndex) const;
  [[nodiscard]] uint64_t getGPUCullingResultBufferAddress(uint32_t frameIndex) const;

  // Get descriptor set from bind group (for descriptor set binding)
  uint64_t getBindGroupDescriptorSet(BindGroupHandle handle, BindGroupSetSlot slot) const {
      return getBindGroupDescriptorSetOpaque(handle, slot);
  }

  // Per-frame bind group accessors for dynamic uniform buffers
  BindGroupHandle getCameraBindGroup(uint32_t frameIndex) const;
  BindGroupHandle getDrawBindGroup(uint32_t frameIndex) const;
  BindGroupHandle getMDIDrawBindGroup(uint32_t frameIndex) const;
  BindGroupHandle getGBufferMDIDrawBindGroup(uint32_t frameIndex) const;
  BindGroupHandle getDepthMDIDrawBindGroup(uint32_t frameIndex) const;
  BindGroupHandle getCSMShadowMDIDrawBindGroup(uint32_t frameIndex, uint32_t cascadeIndex) const;
  [[nodiscard]] uint64_t getForwardMDIIndirectBuffer(uint32_t frameIndex) const;
  void ensureGPUDrivenPersistentIndirectStream(uint32_t frameIndex, uint32_t requiredDrawCount);
  void uploadMDIDrawData(uint32_t frameIndex, std::span<const shaderio::DrawUniforms> drawData);
  void uploadMDIDrawDataRange(uint32_t frameIndex, uint32_t firstDrawIndex, std::span<const shaderio::DrawUniforms> drawData);
  void uploadGBufferMDIDrawData(uint32_t frameIndex, std::span<const shaderio::DrawUniforms> drawData);
  void uploadGBufferMDIDrawDataRange(uint32_t frameIndex, uint32_t firstDrawIndex, std::span<const shaderio::DrawUniforms> drawData);
  void uploadDepthMDIDrawData(uint32_t frameIndex, std::span<const shaderio::DrawUniforms> drawData);
  void uploadDepthMDIDrawDataRange(uint32_t frameIndex, uint32_t firstDrawIndex, std::span<const shaderio::DrawUniforms> drawData);

  // BindGroup creation (new RHI interface)
  rhi::BindGroupLayoutHandle createBindGroupLayout(const rhi::BindGroupLayoutDesc& desc);
  rhi::BindGroupHandle createBindGroup(const rhi::BindGroupDesc& desc);
  void destroyBindGroupLayout(rhi::BindGroupLayoutHandle handle);
  void destroyBindGroup(rhi::BindGroupHandle handle);
  // Native VkDescriptorSetLayout (as uint64) backing a BindGroupLayoutHandle, e.g. to
  // build a VkPipelineLayout that is compatible with temporary bind groups of this layout.
  [[nodiscard]] uint64_t getBindGroupLayoutHandleNative(rhi::BindGroupLayoutHandle handle) const;

  // --- Texture views as RHI handles (the only thing business/pass code should hold) ---
  // createTextureView does vkCreateImageView from the desc and registers an owned view;
  // registerExternalTextureView adopts a caller-owned native view (e.g. per-frame swapchain
  // view) without taking ownership; destroyTextureView frees owned views. resolveTextureViewNative
  // returns the native VkImageView (as uint64) for backend resource-binding paths.
  rhi::TextureViewHandle           createTextureView(const rhi::TextureViewCreateDesc& desc);
  rhi::TextureViewHandle           registerExternalTextureView(uint64_t nativeView);
  void                             destroyTextureView(rhi::TextureViewHandle handle);
  [[nodiscard]] uint64_t           resolveTextureViewNative(rhi::TextureViewHandle handle) const;

  // Frame-lifetime bind group (HypeHype createTemporaryBindGroup). Allocates a fresh
  // descriptor set from the given layout, writes it, and returns a handle valid only
  // for the current frame — it is destroyed automatically when this frame index is
  // recorded again (after its fence). Callers must NOT cache the handle across frames.
  BindGroupHandle createTemporaryBindGroup(rhi::BindGroupLayoutHandle layout,
                                           const rhi::BindTableWrite* writes, uint32_t writeCount,
                                           uint32_t maxLogicalEntries, BindGroupSetSlot slot,
                                           rhi::ResourceIndex primaryLogicalIndex, const char* debugName);

  // Get material baseColorFactor and texture info for glTF rendering
  glm::vec4 getMaterialBaseColorFactor(MaterialHandle handle) const;
  int32_t getMaterialBaseColorTextureIndex(MaterialHandle materialHandle, const SceneUploadResult* gltfModel) const;

  // Material texture indices struct for GBuffer rendering
  struct MaterialTextureIndices {
    int32_t baseColor = -1;
    int32_t normal = -1;
    int32_t metallicRoughness = -1;
    int32_t occlusion = -1;
    float metallicFactor = 1.0f;
    float roughnessFactor = 1.0f;
    float normalScale = 1.0f;
    int32_t alphaMode = 0;    // 0=OPAQUE, 1=MASK, 2=BLEND
    float alphaCutoff = 0.5f;
  };
  MaterialTextureIndices getMaterialTextureIndices(MaterialHandle materialHandle, const SceneUploadResult* gltfModel) const;

  // RHI accessors (replacing native accessors)
  rhi::TextureViewHandle getCurrentSwapchainView() const;
  rhi::TextureViewHandle getGBufferView(uint32_t index) const;
  rhi::TextureViewHandle getDepthView() const;
  rhi::BindGroupHandle getGlobalBindlessGroup() const;

  void updateBindlessTexture(uint32_t index, TextureHandle textureHandle);
  void invalidateBindlessTexture(uint32_t index);
  // Get the base index for glTF textures in the bindless array
  static constexpr uint32_t getGltfTextureBaseIndex() { return kDemoMaterialSlotCount; }

  VkExtent2D getSwapchainExtent() const { return m_swapchainDependent.windowSize; }
  uint32_t   getSwapchainImageCount() const { return m_swapchainDependent.swapchain->getRequestedImageCount(); }
  VkImageView getCurrentSwapchainImageView() const;
  VkImage getCurrentSwapchainImage() const;
  rhi::TextureHandle getCurrentSwapchainTextureHandle() const;
  // Per-image-index registry handles mirroring the swapchain backbuffers into the
  // device resource table (lazily (re)registered by getCurrentSwapchainTextureHandle).
  mutable std::vector<rhi::TextureHandle> m_swapchainTextureHandles;
  mutable std::vector<uint64_t>           m_swapchainTextureNatives;
  VkFormat getSceneDepthFormat() const { return m_swapchainDependent.sceneResources.getDepthFormat(); }
  VkImage getSceneDepthImage() const { return m_swapchainDependent.sceneResources.getDepthImage(); }
  rhi::TextureViewHandle getSceneDepthImageView() const { return m_swapchainDependent.sceneResources.getDepthImageView(); }
  VkImage getSceneGBufferImage(uint32_t index) const { return m_swapchainDependent.sceneResources.getColorImage(index); }
  rhi::TextureViewHandle getSceneGBufferImageView(uint32_t index) const { return m_swapchainDependent.sceneResources.getGBufferImageView(index); }
  VkImage getOutputTextureImage() const { return m_swapchainDependent.sceneResources.getOutputTextureImage(); }
  rhi::TextureViewHandle getOutputTextureView() const;
  VkFormat getOutputTextureFormat() const { return m_swapchainDependent.sceneResources.getOutputTextureFormat(); }
  uint64_t getOutputTextureEstimatedBytes() const
  {
    return m_swapchainDependent.sceneResources.getOutputTextureEstimatedBytes();
  }
  VkImage getSceneColorHdrImage() const { return m_swapchainDependent.sceneResources.getSceneColorHdrImage(); }
  rhi::TextureViewHandle getSceneColorHdrView() const { return m_swapchainDependent.sceneResources.getSceneColorHdrView(); }
  VkFormat getSceneColorHdrFormat() const { return m_swapchainDependent.sceneResources.getSceneColorHdrFormat(); }
  uint64_t getSceneColorHdrEstimatedBytes() const
  {
    return m_swapchainDependent.sceneResources.getSceneColorHdrEstimatedBytes();
  }
  VkImage getBloomHalfImage() const { return m_swapchainDependent.sceneResources.getBloomHalfImage(); }
  rhi::TextureViewHandle getBloomHalfView() const { return m_swapchainDependent.sceneResources.getBloomHalfView(); }
  VkExtent2D getBloomHalfExtent() const { return m_swapchainDependent.sceneResources.getBloomHalfExtent(); }
  VkImage getBloomQuarterImage() const { return m_swapchainDependent.sceneResources.getBloomQuarterImage(); }
  rhi::TextureViewHandle getBloomQuarterView() const { return m_swapchainDependent.sceneResources.getBloomQuarterView(); }
  VkExtent2D getBloomQuarterExtent() const { return m_swapchainDependent.sceneResources.getBloomQuarterExtent(); }
  VkImage getBloomEighthImage() const { return m_swapchainDependent.sceneResources.getBloomEighthImage(); }
  rhi::TextureViewHandle getBloomEighthView() const { return m_swapchainDependent.sceneResources.getBloomEighthView(); }
  VkExtent2D getBloomEighthExtent() const { return m_swapchainDependent.sceneResources.getBloomEighthExtent(); }
  VkImage getBloomSixteenthImage() const { return m_swapchainDependent.sceneResources.getBloomSixteenthImage(); }
  rhi::TextureViewHandle getBloomSixteenthView() const { return m_swapchainDependent.sceneResources.getBloomSixteenthView(); }
  VkExtent2D getBloomSixteenthExtent() const { return m_swapchainDependent.sceneResources.getBloomSixteenthExtent(); }
  VkImage getBloomThirtySecondImage() const { return m_swapchainDependent.sceneResources.getBloomThirtySecondImage(); }
  rhi::TextureViewHandle getBloomThirtySecondView() const { return m_swapchainDependent.sceneResources.getBloomThirtySecondView(); }
  VkExtent2D getBloomThirtySecondExtent() const { return m_swapchainDependent.sceneResources.getBloomThirtySecondExtent(); }
  VkImage getBloomUpsampleSixteenthImage() const { return m_swapchainDependent.sceneResources.getBloomUpsampleSixteenthImage(); }
  rhi::TextureViewHandle getBloomUpsampleSixteenthView() const { return m_swapchainDependent.sceneResources.getBloomUpsampleSixteenthView(); }
  VkExtent2D getBloomUpsampleSixteenthExtent() const { return m_swapchainDependent.sceneResources.getBloomUpsampleSixteenthExtent(); }
  VkImage getBloomUpsampleEighthImage() const { return m_swapchainDependent.sceneResources.getBloomUpsampleEighthImage(); }
  rhi::TextureViewHandle getBloomUpsampleEighthView() const { return m_swapchainDependent.sceneResources.getBloomUpsampleEighthView(); }
  VkExtent2D getBloomUpsampleEighthExtent() const { return m_swapchainDependent.sceneResources.getBloomUpsampleEighthExtent(); }
  VkImage getBloomUpsampleQuarterImage() const { return m_swapchainDependent.sceneResources.getBloomUpsampleQuarterImage(); }
  rhi::TextureViewHandle getBloomUpsampleQuarterView() const { return m_swapchainDependent.sceneResources.getBloomUpsampleQuarterView(); }
  VkExtent2D getBloomUpsampleQuarterExtent() const { return m_swapchainDependent.sceneResources.getBloomUpsampleQuarterExtent(); }
  VkImage getBloomOutputImage() const { return m_swapchainDependent.sceneResources.getBloomOutputImage(); }
  rhi::TextureViewHandle getBloomOutputView() const { return m_swapchainDependent.sceneResources.getBloomOutputView(); }
  VkExtent2D getBloomOutputExtent() const { return m_swapchainDependent.sceneResources.getBloomOutputExtent(); }
  VkImage getColorGradingLutImage() const { return m_swapchainDependent.sceneResources.getColorGradingLutImage(); }
  rhi::TextureViewHandle getColorGradingLutView() const { return m_swapchainDependent.sceneResources.getColorGradingLutView(); }
  VkExtent2D getColorGradingLutExtent() const { return m_swapchainDependent.sceneResources.getColorGradingLutExtent(); }
  uint64_t getBloomEstimatedBytes() const { return m_swapchainDependent.sceneResources.getBloomEstimatedBytes(); }
  VkImage getVelocityImage() const { return m_swapchainDependent.sceneResources.getVelocityImage(); }
  rhi::TextureViewHandle getVelocityView() const { return m_swapchainDependent.sceneResources.getVelocityView(); }
  VkFormat getVelocityFormat() const { return m_swapchainDependent.sceneResources.getVelocityFormat(); }
  uint64_t getVelocityEstimatedBytes() const { return m_swapchainDependent.sceneResources.getVelocityEstimatedBytes(); }
  VkImage getSceneColorHistoryImage(uint32_t index) const
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
  VkImageView getShadowMapView() const;
  VkImage getShadowMapImage() const;
  shaderio::ShadowUniforms* getShadowUniformsData();
  uint64_t    getDeviceOpaque() const { return m_device.device ? m_device.device->getNativeDevice() : 0; }
  [[nodiscard]] rhi::Device& getRHIDevice() const { return *m_device.device; }
  uint64_t    getPhysicalDeviceOpaque() const { return m_device.device ? m_device.device->getNativePhysicalDevice() : 0; }

private:
  class SamplerCache
  {
  public:
    SamplerCache() = default;
    ~SamplerCache() { assert(m_device == VK_NULL_HANDLE && "Missing deinit()"); }

    void init(VkDevice device) { m_device = device; }
    void deinit()
    {
      for(const auto& entry : m_samplerMap)
      {
        vkDestroySampler(m_device, entry.second, nullptr);
      }
      m_samplerMap.clear();
      m_device = VK_NULL_HANDLE;
    }
    VkSampler acquireSampler(const VkSamplerCreateInfo& createInfo)
    {
      if(auto it = m_samplerMap.find(createInfo); it != m_samplerMap.end())
      {
        return it->second;
      }

      VkSampler sampler{VK_NULL_HANDLE};
      VK_CHECK(vkCreateSampler(m_device, &createInfo, nullptr, &sampler));
      m_samplerMap[createInfo] = sampler;
      return sampler;
    }

  private:
    struct SamplerCreateInfoHash
    {
      std::size_t operator()(const VkSamplerCreateInfo& info) const
      {
        std::size_t seed{0};
        seed = utils::hashCombine(seed, info.magFilter);
        seed = utils::hashCombine(seed, info.minFilter);
        seed = utils::hashCombine(seed, info.mipmapMode);
        seed = utils::hashCombine(seed, info.addressModeU);
        seed = utils::hashCombine(seed, info.addressModeV);
        seed = utils::hashCombine(seed, info.addressModeW);
        seed = utils::hashCombine(seed, info.mipLodBias);
        seed = utils::hashCombine(seed, info.anisotropyEnable);
        seed = utils::hashCombine(seed, info.maxAnisotropy);
        seed = utils::hashCombine(seed, info.compareEnable);
        seed = utils::hashCombine(seed, info.compareOp);
        seed = utils::hashCombine(seed, info.minLod);
        seed = utils::hashCombine(seed, info.maxLod);
        seed = utils::hashCombine(seed, info.borderColor);
        seed = utils::hashCombine(seed, info.unnormalizedCoordinates);
        return seed;
      }
    };

    struct SamplerCreateInfoEqual
    {
      bool operator()(const VkSamplerCreateInfo& lhs, const VkSamplerCreateInfo& rhs) const
      {
        return std::memcmp(&lhs, &rhs, sizeof(VkSamplerCreateInfo)) == 0;
      }
    };

    VkDevice m_device{VK_NULL_HANDLE};
    std::unordered_map<VkSamplerCreateInfo, VkSampler, SamplerCreateInfoHash, SamplerCreateInfoEqual> m_samplerMap;
  };

  // Created during RenderDevice::init() after feature negotiation.
  // Destroyed during RenderDevice::shutdown() after vkDeviceWaitIdle.
  // Rebuild trigger: none while device is alive; recreated only on full renderer/device re-init.
  struct DeviceLifetimeResources
  {

    std::unique_ptr<rhi::Device> device;
    VmaAllocator                 allocator{nullptr};
    std::vector<utils::Buffer>   stagingBuffers;
    upload::StaticBufferUploadPolicy staticBufferUploadPolicy{};
    SamplerCache                 samplerPool;

    utils::Buffer                              vertexBuffer;
    utils::Buffer                              pointsBuffer;
    VkCommandPool                              transientCmdPool{};
    VkCommandPool                              uploadCmdPool{};
    VkCommandPool                              computeCmdPool{};
    VkDescriptorPool                           descriptorPool{};
    VkDescriptorPool                           uiDescriptorPool{};
    VkDescriptorSetLayout                      gbufferTextureSetLayout{nullptr};
    std::vector<VkDescriptorSet>               gbufferTextureSets;
    utils::ImageResource                       iblEnvironment{};
    VkFormat                                   iblEnvironmentFormat{VK_FORMAT_UNDEFINED};
    VkExtent2D                                 iblEnvironmentExtent{};
    uint32_t                                   iblEnvironmentMipCount{0};
    uint64_t                                   iblEnvironmentEstimatedBytes{0};
    bool                                       iblEnvironmentLoaded{false};
    bool                                       iblUsingFallback{true};
    std::string                                iblEnvironmentPath;
    std::string                                iblEnvironmentStatus{"Not initialized"};
    VkPipelineLayout                           lightPipelineLayout{nullptr};
    VkPipelineLayout                           postProcessPipelineLayout{nullptr};
    VkDescriptorSetLayout                      depthPyramidSetLayout{nullptr};
    VkDescriptorSet                            depthPyramidDescriptorSet{nullptr};
    VkPipelineLayout                           depthPyramidPipelineLayout{nullptr};
    VkDescriptorSetLayout                      gpuCullingSetLayout{nullptr};
    std::vector<VkDescriptorSet>               gpuCullingDescriptorSets;
    std::vector<BindGroupHandle>               gpuCullingBindGroups;
    VkPipelineLayout                           gpuCullingPipelineLayout{nullptr};
    VkDescriptorSetLayout                      shadowCullingSetLayout{nullptr};
    std::vector<VkDescriptorSet>               shadowCullingDescriptorSets;
    std::vector<BindGroupHandle>               shadowCullingBindGroups;
    VkPipelineLayout                           shadowCullingPipelineLayout{nullptr};
    VkDescriptorSetLayout                      lightCoarseCullingSetLayout{nullptr};
    std::vector<VkDescriptorSet>               lightCoarseCullingDescriptorSets;
    VkPipelineLayout                           lightCoarseCullingPipelineLayout{nullptr};
    std::unique_ptr<rhi::PipelineLayout>       graphicPipelineLayout;
    std::unique_ptr<rhi::PipelineLayout>       computePipelineLayout;
    std::unique_ptr<rhi::PipelineLayout>       debugPipelineLayout;
    std::unique_ptr<rhi::PipelineLayout>       gbufferPipelineLayout;  // Separate layout for GBuffer pass
    std::unique_ptr<rhi::PipelineLayout>       mdiPipelineLayout;
    std::unique_ptr<rhi::PipelineLayout>       csmShadowMdiPipelineLayout;
    rhi::vulkan::VulkanResourceTable           resourceTable;

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
    SceneResources                  sceneResources;
    VkExtent2D                      windowSize{800, 600};
    VkExtent2D                      viewportSize{800, 600};
    VkFormat                        swapchainImageFormat{VK_FORMAT_B8G8R8A8_UNORM};
    uint32_t                        currentImageIndex{0};
    bool                            hasAcquiredImage{false};
    std::vector<rhi::ResourceState> imageStates;  // Track per-image layout state
    bool                            vSync{true};
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
      BindGroupHandle    sceneBindGroup{kNullBindGroupHandle};
      BindGroupHandle    cameraBindGroup{kNullBindGroupHandle};
      BindGroupHandle    drawBindGroup{kNullBindGroupHandle};
      BindGroupHandle    mdiDrawBindGroup{kNullBindGroupHandle};
      BindGroupHandle    gbufferMdiDrawBindGroup{kNullBindGroupHandle};
      BindGroupHandle    depthMdiDrawBindGroup{kNullBindGroupHandle};
      std::array<BindGroupHandle, shaderio::LCascadeCount> csmShadowMdiDrawBindGroups{
          kNullBindGroupHandle, kNullBindGroupHandle, kNullBindGroupHandle, kNullBindGroupHandle};
      utils::Buffer      lightingBuffer{};
      utils::Buffer      lightCullingBuffer{};
      utils::Buffer      gpuCullingObjectBuffer{};
      utils::Buffer      gpuCullingIndirectBuffer{};
      utils::Buffer      gpuCullingDrawCountBuffer{};
      utils::Buffer      gpuCullingStatsBuffer{};
      utils::Buffer      gpuCullingUniformBuffer{};
      utils::Buffer      gpuCullingResultBuffer{};
      VkBuffer           externalGPUCullingObjectBuffer{VK_NULL_HANDLE};
      VkBuffer           externalGPUCullingMeshletBuffer{VK_NULL_HANDLE};
      VkBuffer           externalGPUCullingSceneObjectBuffer{VK_NULL_HANDLE};
      uint64_t           externalGPUCullingObjectBufferAddress{0};
      bool               useExternalGPUCullingObjectBuffer{false};
      bool               useExternalGPUCullingMeshletData{false};
      const SceneUploadResult* gpuCullingSourceModel{nullptr};
      uint32_t           gpuCullingObjectCount{0};
      uint32_t           gpuCullingMeshCapacity{0};
      std::vector<uint32_t> gpuCullingResults;
      std::vector<shaderio::GPUCullObject> gpuCullingScratchObjects;
      utils::Buffer      shadowCullingObjectBuffer{};
      utils::Buffer      shadowCullingIndirectBuffer{};
      utils::Buffer      shadowCullingDrawDataBuffer{};
      utils::Buffer      mdiDrawDataBuffer{};
      utils::Buffer      gbufferMdiDrawDataBuffer{};
      utils::Buffer      depthMdiDrawDataBuffer{};
      utils::Buffer      gpuDrivenPersistentIndirectStreamBuffer{};
      // Stable RHI handles mirroring the per-frame native buffers above (Option B:
      // allocated once, rebound to the native buffer on each realloc). Consumed by
      // RenderEncoder-based passes via getXxxBufferRHIHandle().
      rhi::BufferHandle  gpuCullingIndirectBufferRHI{};
      rhi::BufferHandle  gpuCullingDrawCountBufferRHI{};
      rhi::BufferHandle  shadowCullingIndirectBufferRHI{};
      rhi::BufferHandle  gpuDrivenPersistentIndirectStreamBufferRHI{};
      uint32_t           shadowCullingMeshCapacity{0};
      uint32_t           mdiDrawCapacity{0};
      uint32_t           gbufferMdiDrawCapacity{0};
      uint32_t           depthMdiDrawCapacity{0};
      uint32_t           gpuDrivenPersistentIndirectStreamCapacity{0};
      std::vector<shaderio::ShadowCullObject> shadowCullingScratchObjects;
      std::vector<shaderio::DrawUniforms>     shadowCullingScratchDrawData;
      std::vector<VkCommandBuffer> pendingUploadCmds;
      std::vector<VkFence> pendingUploadFences;
      // Bind groups created via createTemporaryBindGroup during this frame index's
      // recording. Recycled (destroyed) the next time this frame index comes around,
      // after its fence has been waited on, so the descriptor sets are safely idle.
      std::vector<BindGroupHandle> transientBindGroups;
    };

    std::vector<FrameUserData> frameUserData;
    uint64_t                   frameCounter{1};
  };

  // Pass-scoped scratch and pass-owned descriptors will move here in later tasks.
  // Lifetime trigger: reset each recorded pass; no persistent renderer-owned pass data yet.
  struct PerPassResources
  {
    DrawStream drawStream;
  };

  bool                                 m_presentPassActive{false};

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
  PipelineHandle m_depthPyramidPipeline{};
  PipelineHandle m_gpuCullingPipeline{};
  PipelineHandle m_shadowCullingPipeline{};
  PipelineHandle m_gbufferOpaquePipeline{};      // GBuffer Opaque variant
  PipelineHandle m_gbufferAlphaTestPipeline{};   // GBuffer AlphaTest variant
  PipelineHandle m_gbufferOpaqueMDIPipeline{};
  PipelineHandle m_gbufferAlphaTestMDIPipeline{};
  PipelineHandle m_shadowPipeline{};             // Directional shadow depth pass
  PipelineHandle m_csmShadowPipeline{};          // CSM cascade MDI depth pass
  CSMShadowResources m_csmShadowResources{};   // CSM cascade texture and uniform buffer
  std::array<rhi::TextureViewHandle, shaderio::LCascadeCount> m_csmCascadeViewHandles{};  // per-cascade render-target views
  rhi::TextureViewHandle m_csmCascadeArrayViewHandle{};  // full-array sampling view
  PipelineHandle m_forwardPipeline{};            // Forward pass for transparent
  PipelineHandle m_forwardMDIPipeline{};         // Forward MDI pass for transparent
  PipelineHandle m_debugPipeline{};              // Debug line overlay pass
  PipelineHandle m_gpuCullingDebugPipeline{};    // Current-frame GPU culling visualization
  PipelineHandle m_pointLightCoarseCullingPipeline{};
  PipelineHandle m_spotLightCoarseCullingPipeline{};

  // GBuffer uniform buffer bind groups (per-frame)
  // BindGroupHandle getCameraBindGroup(uint32_t frameIndex) const;  // Moved to public
  // BindGroupHandle getDrawBindGroup(uint32_t frameIndex) const;    // Moved to public

  // Draw-call-scoped transient CPU/GPU data staging bucket.
  // Lifetime trigger: rebuilt per draw packet emission/consumption; currently no persistent owner fields.
  struct PerDrawData
  {
  };

  struct Aabb
  {
    glm::vec3 min{0.0f};
    glm::vec3 max{0.0f};
    bool      valid{false};
  };

  struct FrameLightingState
  {
    shaderio::CameraUniforms shadowCamera{};
    shaderio::LightParams    lightParams{};
    Aabb                     sceneBounds{};
    std::array<glm::vec3, 8> viewFrustumCorners{};
    std::array<glm::vec3, 8> shadowFrustumCorners{};
    glm::vec3                lightAnchor{0.0f};
    float                    shadowDistance{0.0f};
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
    float     radiusT{1.0f};
    float     intensityT{1.0f};
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
      outputTexture,  // OutputTexture for PBR lighting result
    };

    struct TextureHotData
    {
      TextureRuntimeKind runtimeKind{TextureRuntimeKind::materialSampled};
      uint32_t           viewportAttachmentIndex{0};
      VkImageView        sampledImageView{VK_NULL_HANDLE};
      VkImageLayout      sampledImageLayout{VK_IMAGE_LAYOUT_UNDEFINED};
    };

    struct TextureColdData
    {
      utils::ImageResource ownedImage{};
      VkExtent2D           sourceExtent{};
      uint32_t             mipLevels{1};
    };

    struct TextureRecord
    {
      TextureHotData  hot{};
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
      TextureHandle      sampledTexture{};

      // PBR Factors (fallback when texture missing)
      glm::vec4 baseColorFactor{1.0f};
      float     metallicFactor{1.0f};
      float     roughnessFactor{1.0f};
      float     normalScale{1.0f};
      float     occlusionStrength{1.0f};
      glm::vec3 emissiveFactor{0.0f};

      // Alpha properties
      int32_t alphaMode = 0;        // 0=OPAQUE, 1=MASK, 2=BLEND
      float alphaCutoff = 0.5f;     // for MASK mode

      // Bindless descriptor slot
      rhi::ResourceIndex descriptorIndex{0};
      const char*        debugName{"material"};
    };

    HandlePool<TextureHandle, TextureRecord>       texturePool;
    HandlePool<MaterialHandle, MaterialRecord>     materialPool;
    HandlePool<BindGroupHandle, BindGroupResource> bindGroupPool;
    // New RHI interface handle pools
    HandlePool<rhi::BindGroupLayoutHandle, std::unique_ptr<rhi::BindTableLayout>> bindGroupLayoutPool;
    HandlePool<rhi::BindGroupHandle, std::unique_ptr<rhi::BindGroup>> bindGroupRhiPool;
    MaterialHandle                                 sampleMaterials[kDemoMaterialSlotCount]{};
    TextureHandle                                  viewportTextureHandle{};
    BindGroupHandle                                materialBindGroup{};
    std::vector<BindGroupHandle>                   materialBindGroups;
    std::vector<rhi::DescriptorImageInfo>          materialDescriptorImageInfos;
    std::vector<uint8_t>                           materialDescriptorValid;
    std::vector<uint64_t>                          materialBindGroupGenerations;
    uint64_t                                       materialDescriptorGeneration{0};
    uint32_t                                       maxTextures{10000};
  };

  void              createTransientCommandPool();
  void              createFrameSubmission(uint32_t numFrames);
  void              rebuildSwapchainDependentResources(std::optional<VkExtent2D> requestedViewportSize = std::nullopt);
  bool              prepareFrameResources();
  bool              acquireSwapchainImageForPresent();
  rhi::CommandList& beginCommandRecording();
  void              drawFrame(rhi::CommandList& cmd, const RenderParams& params, PassExecutor& passExecutor);
  void              endFrame(rhi::CommandList& cmd);
  void              beginDynamicRenderingToSwapchain(const rhi::CommandList& cmd) const;
  void              endDynamicRenderingToSwapchain(const rhi::CommandList& cmd);
  void              updateLightingUniformBuffer(uint32_t frameIndex, const shaderio::LightingUniforms& lightingUniforms);
  void              updateLightCullingUniformBuffer(uint32_t frameIndex, const shaderio::LightCullingUniforms& cullingUniforms);
  void                 prebuildRequiredPipelineVariants();
  void                 createPrebuiltGraphicsPipelineVariants();
  void                 createPrebuiltComputePipelineVariant();
  void                 createLightCoarseCullingResources();
  void                 createLightCoarseCullingPipelines();
  void                 createDepthPyramidResources();
  void                 updateDepthPyramidDescriptorSet();
  void                 createDepthPyramidPipeline();
  void                 createGPUCullingResources();
  void                 updateGPUCullingDescriptorSet(uint32_t frameIndex);
  void                 createGPUCullingPipeline();
  void                 waitForAllFrameSlots();
  void                 ensureGPUCullingBuffers(PerFrameResources::FrameUserData& frameUserData, uint32_t requiredMeshCount);
  // Registers (first call) or rebinds (subsequent) a stable RHI BufferHandle to a
  // per-frame native buffer; clears the handle when the buffer is null.
  void                 rebindFrameBufferHandle(rhi::BufferHandle& handle, const utils::Buffer& buffer);
  void                 updateGPUCullingBuffers(uint32_t frameIndex, const RenderParams& params);
  void                 createShadowCullingResources();
  void                 updateShadowCullingDescriptorSet(uint32_t frameIndex);
  void                 updateShadowCullingDrawDataDescriptorSet(uint32_t frameIndex);
  void                 updateMdiDrawDataDescriptorSet(uint32_t frameIndex);
  void                 updateGBufferMdiDrawDataDescriptorSet(uint32_t frameIndex);
  void                 updateDepthMdiDrawDataDescriptorSet(uint32_t frameIndex);
  void                 createShadowCullingPipeline();
  void                 ensureShadowCullingBuffers(PerFrameResources::FrameUserData& frameUserData, uint32_t requiredMeshCount);
  void                 ensureMdiDrawDataBuffer(PerFrameResources::FrameUserData& frameUserData, uint32_t requiredDrawCount);
  void                 ensureGBufferMdiDrawDataBuffer(PerFrameResources::FrameUserData& frameUserData, uint32_t requiredDrawCount);
  void                 ensureDepthMdiDrawDataBuffer(PerFrameResources::FrameUserData& frameUserData, uint32_t requiredDrawCount);
  void                 ensureGPUDrivenPersistentIndirectStreamBuffer(PerFrameResources::FrameUserData& frameUserData,
                                                                     uint32_t requiredDrawCount);
  void                 updateShadowCullingBuffers(uint32_t frameIndex, const RenderParams& params);
  void                 cacheGPUCullingStats(uint32_t frameIndex, bool readOverlayObjects);
  void                 drawGPUInfoOverlay(const RenderParams& params) const;
  void                 drawGPUCullingOverlay(const RenderParams& params) const;
  void                 createPassGpuProfileResources(const PassExecutor& passExecutor);
  void                 destroyPassGpuProfileResources();
  void                 resolvePassGpuProfileResults(uint32_t frameIndex);
  void                 resetPassGpuProfileQueries(const rhi::CommandList& cmd, uint32_t frameIndex);
  void                 writePassGpuProfileTimestamp(const PassContext& context, uint32_t passIndex, bool isBegin) const;
  void                 drawPassGpuProfileOverlay(const RenderParams& params) const;
  void                 initImGui(void* window);
  void                 createDescriptorPool();
  void                 createMaterialBindGroup();     // Create material bind group early for pipeline layout
  void                 createGraphicDescriptorSet();
  void                 updateGraphicsDescriptorSet();
  void                 syncMaterialBindGroup(uint32_t frameIndex);
  BindGroupHandle      getCurrentMaterialBindGroupHandle() const;
  void                 flushPendingUploadCommands(bool waitForCompletion);
  void                 createIBLResources(VkCommandBuffer cmd);
  void                 destroyIBLResources();
  void                 updateGBufferTextureDescriptorSet();
  void                 destroyBindGroups();
  utils::ImageResource loadAndCreateImage(rhi::CommandList& cmd, const std::string& filename);
  rhi::ResourceIndex   getBindGroupPrimaryLogicalIndex(BindGroupHandle handle, BindGroupSetSlot expectedSlot) const;
  const MaterialResources::MaterialRecord*  tryGetMaterial(MaterialHandle handle) const;
  const MaterialResources::TextureHotData*  tryGetTextureHot(TextureHandle handle) const;
  const MaterialResources::TextureColdData* tryGetTextureCold(TextureHandle handle) const;
  BindGroupHandle                           createBindGroup(BindGroupDesc desc);
  void           updateBindGroup(BindGroupHandle handle, const rhi::BindTableWrite* writes, uint32_t writeCount) const;
  // destroyBindGroup is provided by public RHI interface (line 145)
  PipelineHandle registerPipeline(uint32_t bindPoint, uint64_t nativePipeline, uint32_t specializationVariant,
                                  uint64_t nativeLayout = 0, bool owned = true);
  void           destroyPipelines();
  const rhi::vulkan::PipelineRecord* tryGetPipelineRecord(PipelineHandle handle) const;
  const BindGroupResource* tryGetBindGroup(BindGroupHandle handle) const;
  uint64_t                 getBindGroupLayoutOpaque(BindGroupHandle handle, BindGroupSetSlot expectedSlot) const;
  uint64_t                 getBindGroupDescriptorSetOpaque(BindGroupHandle handle, BindGroupSetSlot expectedSlot) const;
  static std::optional<uint32_t> mapSetSlotToLegacyShaderSet(BindGroupSetSlot slot);
  [[nodiscard]] Aabb computeSceneBounds(const SceneUploadResult* gltfModel, const GPUDrivenSceneView* gpuDrivenSceneView) const;
  void                     rebuildShadowPackedBuffers(const GltfModel& model, GltfUploadResult& result, VkCommandBuffer cmd);
  void                     rebuildShadowPackedBuffers(const SceneAsset& asset, SceneUploadResult& result, VkCommandBuffer cmd);
  [[nodiscard]] FrameLightingState buildFrameLightingState(const RenderParams& params) const;
  void              ensureTestPointLights(const RenderParams& params);
  [[nodiscard]] shaderio::LightCullingUniforms buildLightCullingUniforms(const RenderParams& params) const;
  [[nodiscard]] shaderio::GPUCullingUniforms buildGPUCullingUniforms(const RenderParams& params, uint32_t objectCount) const;
  [[nodiscard]] std::array<glm::vec3, 8> computePerspectiveFrustumCorners(const shaderio::CameraUniforms& cameraUniforms,
                                                                          float nearDistance,
                                                                          float farDistance) const;
  [[nodiscard]] std::array<glm::vec3, 8> computeOrthoFrustumCorners(const glm::mat4& inverseViewProjection) const;
  void              buildDebugDrawList(const RenderParams& params);

  struct PassGpuProfileFrame
  {
    VkQueryPool         queryPool{VK_NULL_HANDLE};
    std::vector<double> cpuPassDurationsMs;
    std::vector<double> passDurationsMs;
    bool                valid{false};
    bool                hasRecordedQueries{false};
  };

  struct PassGpuProfileState
  {
    float                         timestampPeriodNs{0.0f};
    uint32_t                      queryCount{0};
    std::vector<std::string>      passNames;
    std::vector<double>           latestCpuPassDurationsMs;
    std::vector<double>           latestPassDurationsMs;
    bool                          latestValid{false};
    std::vector<PassGpuProfileFrame> frames;
    std::vector<uint64_t>         currentCpuPassStartNs;
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

  DeviceLifetimeResources     m_device;
  SwapchainDependentResources m_swapchainDependent;
  PerFrameResources           m_perFrame;
  PerPassResources            m_perPass;
  PerDrawData                 m_perDraw;
  MaterialResources           m_materials;
  LightResources              m_lightResources;
  utils::Buffer               m_depthPyramidUniformBuffer{};
  std::vector<shaderio::LightData> m_testPointLights;
  std::vector<TestPointLightMotion> m_testPointLightMotions;
  Aabb                     m_testPointLightSceneBounds{};
  std::vector<shaderio::LightData> m_testSpotLights;
  FrameLightingState          m_frameLightingState;
  DebugDrawList               m_debugDrawList;
  shaderio::GPUCullStats      m_lastGPUCullingStats{};
  shaderio::GPUCullDrawCounts m_lastGPUCullingDrawCounts{};
  const shaderio::GPUCullObject* m_externalGPUCullingOverlayObjects{nullptr};
  uint32_t                    m_externalGPUCullingOverlayObjectCount{0};
  std::vector<GPUCullOverlayObject> m_lastGPUCullingOverlayObjects;
  PassGpuProfileState         m_passGpuProfile;
  PassProfilingHooks          m_passProfilingHooks{this};

#ifdef TRACY_ENABLE
  std::unique_ptr<profiling::TracyVulkanContext> m_tracyVkCtx;
  VkCommandPool   m_tracyCmdPool{VK_NULL_HANDLE};
  VkCommandBuffer m_tracyCmdBuf{VK_NULL_HANDLE};
#endif
};

}  // namespace demo
