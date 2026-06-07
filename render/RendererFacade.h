#pragma once

#include "GPUDrivenRenderer.h"

namespace demo {

enum class RendererBackend
{
  gpuDriven,
};

class RendererFacade
{
public:
  RendererFacade() = default;

  [[nodiscard]] std::unique_ptr<rhi::Surface> createSurface() const;
  void init(void* window, rhi::Surface& surface, bool vSync);
  void shutdown(rhi::Surface& surface);
  void setVSync(bool enabled);
  [[nodiscard]] bool getVSync() const;
  void setFullscreen(bool enabled, void* platformHandle = nullptr);
  [[nodiscard]] const char* getSwapchainPresentModeName() const;
  [[nodiscard]] uint32_t getSwapchainImageCount() const;
  void resize(rhi::Extent2D size);
  void beginUiFrame();
  void render(const RenderParams& params);
  void setSceneRenderingSuspended(bool suspended);
  [[nodiscard]] bool isSceneRenderingSuspended() const;

  [[nodiscard]] TextureHandle  getViewportTextureHandle() const;
  [[nodiscard]] ImTextureID    getViewportTextureID(TextureHandle handle) const;
  [[nodiscard]] MaterialHandle getMaterialHandle(uint32_t slot) const;
  GltfUploadResult uploadGltfModel(const GltfModel& model, rhi::CommandBuffer& cmd);
  SceneUploadResult commitSceneUploadPlan(const SceneAsset& asset, const SceneUploadPlan& plan, rhi::CommandBuffer& cmd);
  void             uploadGltfModelBatch(const GltfModel&          model,
                                        std::span<const uint32_t> textureIndices,
                                        std::span<const uint32_t> materialIndices,
                                        std::span<const uint32_t> meshIndices,
                                        GltfUploadResult&         ioResult,
                                        rhi::CommandBuffer&       cmd);
  void             initializeGltfUploadResult(const GltfModel& model, GltfUploadResult& outResult) const;
  void             destroyGltfResources(const GltfUploadResult& result);
  void             updateMeshTransform(MeshHandle handle, const glm::mat4& transform);
  void             updateSceneInstanceTransform(uint32_t instanceIndex, const glm::mat4& transform);
  void             executeUploadCommand(std::function<void(rhi::CommandBuffer&)> uploadFn);
  void             waitForIdle();

  [[nodiscard]] const shaderio::GPUCullStats& getLastGPUCullingStats() const;
  [[nodiscard]] RuntimeProfileSnapshot getRuntimeProfileSnapshot() const;
  [[nodiscard]] shaderio::ShadowUniforms* getShadowUniformsData();
  [[nodiscard]] CSMShadowResources& getCSMShadowResources();
  [[nodiscard]] RendererBackend getBackend() const { return RendererBackend::gpuDriven; }
  [[nodiscard]] const char* getBackendName() const;
  [[nodiscard]] GPUDrivenRuntimeStats getGPUDrivenRuntimeStats() const;
  [[nodiscard]] bool isExperimentalMeshletPathEnabled() const;

private:
  GPUDrivenRenderer& gpuDriven() { return m_gpuDrivenRenderer; }
  const GPUDrivenRenderer& gpuDriven() const { return m_gpuDrivenRenderer; }

  GPUDrivenRenderer m_gpuDrivenRenderer;
};

}  // namespace demo
