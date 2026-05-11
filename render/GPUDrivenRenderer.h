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
#include "passes/GPUDrivenPresentPass.h"
#include "passes/GPUDrivenVisibilitySortPass.h"
#include "Renderer.h"

#include <array>
#include <unordered_map>

namespace demo {

enum class GPUDrivenVisibilityOwnership : uint32_t
{
  cpuBootstrap = 0,
  gpuSortCpuFeedback = 1,
  gpuOwned = 2,
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
};

class GPUDrivenRenderer
{
public:
  void init(GLFWwindow* window, rhi::Surface& surface, bool vSync);
  void shutdown(rhi::Surface& surface);
  void setVSync(bool enabled) { m_renderer.setVSync(enabled); }
  [[nodiscard]] bool getVSync() const { return m_renderer.getVSync(); }
  void setFullscreen(bool enabled, void* platformHandle = nullptr) { m_renderer.setFullscreen(enabled, platformHandle); }
  [[nodiscard]] const char* getSwapchainPresentModeName() const { return m_renderer.getSwapchainPresentModeName(); }
  [[nodiscard]] uint32_t getSwapchainImageCount() const { return m_renderer.getSwapchainImageCount(); }
  void resize(rhi::Extent2D size);
  void render(const RenderParams& params);

  TextureHandle  getViewportTextureHandle() const { return m_renderer.getViewportTextureHandle(); }
  ImTextureID    getViewportTextureID(TextureHandle handle) const { return m_renderer.getViewportTextureID(handle); }
  MaterialHandle getMaterialHandle(uint32_t slot) const { return m_renderer.getMaterialHandle(slot); }
  GltfUploadResult uploadGltfModel(const GltfModel& model, VkCommandBuffer cmd);
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
  void             executeUploadCommand(std::function<void(VkCommandBuffer)> uploadFn) { m_renderer.executeUploadCommand(std::move(uploadFn)); }
  void             waitForIdle() { m_renderer.waitForIdle(); }

  [[nodiscard]] const shaderio::GPUCullStats& getLastGPUCullingStats() const { return m_renderer.getLastGPUCullingStats(); }
  [[nodiscard]] const std::vector<GPUCullOverlayObject>& getLastGPUCullingOverlayObjects() const
  {
    return m_renderer.getLastGPUCullingOverlayObjects();
  }
  [[nodiscard]] const GPUDrivenRuntimeStats& getRuntimeStats() const { return m_runtimeStats; }
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
  [[nodiscard]] bool                 tryGetMeshDrawIndex(MeshHandle handle, uint32_t& outDrawIndex) const;
  [[nodiscard]] bool                 tryGetMeshHandleForDrawIndex(uint32_t drawIndex, MeshHandle& outHandle) const;
  [[nodiscard]] std::span<const uint32_t> getOpaqueDrawIndices() const { return m_opaqueDrawIndices; }
  [[nodiscard]] std::span<const uint32_t> getAlphaTestDrawIndices() const { return m_alphaTestDrawIndices; }
  [[nodiscard]] std::span<const uint32_t> getTransparentDrawIndices() const { return m_transparentDrawIndices; }
  [[nodiscard]] std::span<const uint32_t> getVisibleOpaqueObjects() const { return m_lastVisibleOpaqueObjects; }
  [[nodiscard]] std::span<const uint32_t> getVisibleTransparentObjects() const { return m_lastVisibleTransparentObjects; }
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
  [[nodiscard]] uint32_t getPreviousGPUCullingObjectCount(uint32_t frameIndex) const
  {
    return m_renderer.getPreviousGPUCullingObjectCount(frameIndex, nullptr);
  }
  [[nodiscard]] uint64_t getGPUCullingIndirectBufferOpaque(uint32_t frameIndex) const
  {
    return m_renderer.getGPUCullingIndirectBufferOpaque(frameIndex);
  }
  [[nodiscard]] uint64_t getShadowCullingIndirectBufferOpaque(uint32_t frameIndex) const
  {
    return m_renderer.getShadowCullingIndirectBufferOpaque(frameIndex);
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
  [[nodiscard]] uint64_t getGPUDrivenBootstrapIndirectBuffer(uint32_t frameIndex) const
  {
    return m_renderer.getGPUDrivenBootstrapIndirectBuffer(frameIndex);
  }
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
  void uploadGPUDrivenBootstrapCommands(uint32_t frameIndex,
                                        std::span<const shaderio::GPUCullIndirectCommand> commands)
  {
    m_renderer.uploadGPUDrivenBootstrapCommands(frameIndex, commands);
  }
  void uploadForwardMDICommands(uint32_t frameIndex, std::span<const shaderio::GPUCullIndirectCommand> commands)
  {
    m_renderer.uploadForwardMDICommands(frameIndex, commands);
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
  bool prepareAndDispatchTransparentVisibilityPatch(rhi::CommandList&         cmd,
                                                    uint32_t                  frameIndex,
                                                    std::span<const uint32_t> sortedDrawIndices,
                                                    uint64_t                  forwardIndirectBufferHandle);

private:
  struct VisibilitySortFrameResources
  {
    utils::Buffer keyBuffer{};
    utils::Buffer valueBuffer{};
    VkDescriptorSet descriptorSet{VK_NULL_HANDLE};
    uint32_t capacity{0};
    uint32_t activeElementCount{0};
    uint32_t paddedElementCount{0};
  };

  struct TransparentVisibilityFrameResources
  {
    utils::Buffer drawIndexBuffer{};
    VkDescriptorSet descriptorSet{VK_NULL_HANDLE};
    uint32_t capacity{0};
    uint64_t boundSourceIndirectHandle{0};
    uint64_t boundForwardIndirectHandle{0};
  };

  static uint64_t packMeshHandleKey(MeshHandle handle);
  void            initVisibilitySortResources();
  void            shutdownVisibilitySortResources();
  void            ensureVisibilitySortCapacity(uint32_t frameIndex, uint32_t requiredCount);
  void            updateVisibilitySortDescriptorSet(uint32_t frameIndex);
  [[nodiscard]] bool consumeVisibilitySortOutputs(uint32_t frameIndex);
  void            prepareVisibilitySortInputs(uint32_t frameIndex);
  void            initTransparentVisibilityPatchResources();
  void            shutdownTransparentVisibilityPatchResources();
  void            ensureTransparentVisibilityPatchCapacity(uint32_t frameIndex, uint32_t requiredCount);
  void            updateTransparentVisibilityPatchDescriptorSet(uint32_t frameIndex,
                                                               uint64_t sourceIndirectBufferHandle,
                                                               uint64_t forwardIndirectBufferHandle);
  void            rebuildGPUDrivenScene(const GltfModel& model, const GltfUploadResult& uploadResult, VkCommandBuffer cmd);
  void            clearGPUDrivenScene();
  void            flushPendingSceneUploads();
  void            refreshSceneView();
  [[nodiscard]] uint32_t getSafePersistentObjectCount() const;

  Renderer                           m_renderer;
  PassExecutor                       m_passExecutor;
  GPUSceneRegistry                   m_sceneRegistry;
  HiZDepthPyramid                    m_hiZDepthPyramid;
  GPUMeshletBuffer                   m_meshletBuffer;
  std::vector<shaderio::Meshlet>     m_meshletDataCpu;
  std::vector<uint32_t>              m_meshletIndicesCpu;
  std::vector<uint32_t>              m_lastVisibleOpaqueObjects;
  std::vector<uint32_t>              m_lastVisibleTransparentObjects;
  std::vector<uint32_t>              m_visibilitySortInputObjects;
  std::vector<uint32_t>              m_visibilitySortInputKeys;
  std::unique_ptr<GPUDrivenDepthPrepass> m_depthPrepass;
  std::unique_ptr<GPUDrivenDepthPyramidPass> m_depthPyramidPass;
  std::unique_ptr<GPUDrivenCullingPass>      m_gpuCullingPass;
  std::unique_ptr<GPUDrivenVisibilitySortPass> m_visibilitySortPass;
  std::unique_ptr<GPUDrivenLightCullingPass> m_lightCullingPass;
  std::unique_ptr<GPUDrivenCSMShadowPass>    m_csmShadowPass;
  std::unique_ptr<GPUDrivenGBufferPass>  m_gbufferPass;
  std::unique_ptr<GPUDrivenLightPass>    m_lightPass;
  std::unique_ptr<GPUDrivenForwardPass>  m_forwardPass;
  std::unique_ptr<GPUDrivenDebugPass>    m_debugPass;
  std::unique_ptr<GPUDrivenPresentPass>  m_presentPass;
  std::unique_ptr<GPUDrivenImguiPass>    m_imguiPass;
  GPUDrivenSceneView                 m_sceneView{};
  GPUDrivenRuntimeStats              m_runtimeStats{};
  const GltfUploadResult*            m_activeUploadResult{nullptr};
  std::unordered_map<uint64_t, uint32_t> m_objectIdByMeshHandle;
  std::unordered_map<uint64_t, uint32_t> m_drawIndexByMeshHandle;
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
  bool                               m_enableExperimentalMeshletPath{false};
  bool                               m_sceneUploadPending{false};
};

}  // namespace demo
