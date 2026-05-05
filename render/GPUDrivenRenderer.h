#pragma once

#include "GPUBatchBuilder.h"
#include "GPUSceneRegistry.h"
#include "GPUMeshletBuffer.h"
#include "HiZDepthPyramid.h"
#include "MeshletConverter.h"
#include "Renderer.h"

#include <unordered_map>

namespace demo {

struct GPUDrivenRuntimeStats
{
  uint32_t objectCount{0};
  uint32_t meshletCount{0};
  uint32_t meshletTriangleCount{0};
  uint32_t sceneUploadCount{0};
  uint32_t pendingSceneUpdates{0};
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
  [[nodiscard]] const GPUDrivenRuntimeStats& getRuntimeStats() const { return m_runtimeStats; }
  [[nodiscard]] shaderio::ShadowUniforms* getShadowUniformsData() { return m_renderer.getShadowUniformsData(); }
  [[nodiscard]] CSMShadowResources& getCSMShadowResources() { return m_renderer.getCSMShadowResources(); }

private:
  static uint64_t packMeshHandleKey(MeshHandle handle);
  void            rebuildGPUDrivenScene(const GltfModel& model, const GltfUploadResult& uploadResult, VkCommandBuffer cmd);
  void            clearGPUDrivenScene();
  void            flushPendingSceneUploads();
  void            refreshSceneView();

  Renderer                           m_renderer;
  GPUSceneRegistry                   m_sceneRegistry;
  HiZDepthPyramid                    m_hiZDepthPyramid;
  GPUBatchBuilder                    m_batchBuilder;
  GPUMeshletBuffer                   m_meshletBuffer;
  GPUDrivenSceneView                 m_sceneView{};
  GPUDrivenRuntimeStats              m_runtimeStats{};
  std::unordered_map<uint64_t, uint32_t> m_objectIdByMeshHandle;
  bool                               m_sceneUploadPending{false};
};

}  // namespace demo
