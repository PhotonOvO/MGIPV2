#pragma once

#include "GPUDrivenRenderer.h"

namespace demo {

enum class RendererBackend
{
  legacy,
  gpuDriven,
};

class RendererFacade
{
public:
  RendererFacade();

  void init(GLFWwindow* window, rhi::Surface& surface, bool vSync);
  void shutdown(rhi::Surface& surface);
  void setVSync(bool enabled);
  [[nodiscard]] bool getVSync() const;
  void setFullscreen(bool enabled, void* platformHandle = nullptr);
  [[nodiscard]] const char* getSwapchainPresentModeName() const;
  [[nodiscard]] uint32_t getSwapchainImageCount() const;
  void resize(rhi::Extent2D size);
  void render(const RenderParams& params);

  [[nodiscard]] TextureHandle  getViewportTextureHandle() const;
  [[nodiscard]] ImTextureID    getViewportTextureID(TextureHandle handle) const;
  [[nodiscard]] MaterialHandle getMaterialHandle(uint32_t slot) const;
  GltfUploadResult uploadGltfModel(const GltfModel& model, VkCommandBuffer cmd);
  void             uploadGltfModelBatch(const GltfModel&          model,
                                        std::span<const uint32_t> textureIndices,
                                        std::span<const uint32_t> materialIndices,
                                        std::span<const uint32_t> meshIndices,
                                        GltfUploadResult&         ioResult,
                                        VkCommandBuffer           cmd);
  void             initializeGltfUploadResult(const GltfModel& model, GltfUploadResult& outResult) const;
  void             destroyGltfResources(const GltfUploadResult& result);
  void             updateMeshTransform(MeshHandle handle, const glm::mat4& transform);
  void             executeUploadCommand(std::function<void(VkCommandBuffer)> uploadFn);
  void             waitForIdle();

  [[nodiscard]] const shaderio::GPUCullStats& getLastGPUCullingStats() const;
  [[nodiscard]] shaderio::ShadowUniforms* getShadowUniformsData();
  [[nodiscard]] CSMShadowResources& getCSMShadowResources();
  [[nodiscard]] RendererBackend getBackend() const { return m_backend; }
  [[nodiscard]] const char* getBackendName() const;
  [[nodiscard]] GPUDrivenRuntimeStats getGPUDrivenRuntimeStats() const;

private:
  Renderer&          legacy() { return m_legacyRenderer; }
  const Renderer&    legacy() const { return m_legacyRenderer; }
  GPUDrivenRenderer& gpuDriven() { return m_gpuDrivenRenderer; }
  const GPUDrivenRenderer& gpuDriven() const { return m_gpuDrivenRenderer; }

  RendererBackend   m_backend{RendererBackend::gpuDriven};
  Renderer          m_legacyRenderer;
  GPUDrivenRenderer m_gpuDrivenRenderer;
};

}  // namespace demo
