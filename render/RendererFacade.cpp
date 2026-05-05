#include "RendererFacade.h"

#include <cstdlib>
#include <string>

namespace demo {

namespace {

RendererBackend resolveBackend()
{
  const char* value = std::getenv("VKDEMO_RENDERER");
  if(value == nullptr)
  {
    return RendererBackend::gpuDriven;
  }

  const std::string selected = value;
  if(selected == "legacy" || selected == "cpu")
  {
    return RendererBackend::legacy;
  }
  return RendererBackend::gpuDriven;
}

}  // namespace

RendererFacade::RendererFacade()
    : m_backend(resolveBackend())
{
}

void RendererFacade::init(GLFWwindow* window, rhi::Surface& surface, bool vSync)
{
  if(m_backend == RendererBackend::gpuDriven)
  {
    gpuDriven().init(window, surface, vSync);
    return;
  }
  legacy().init(window, surface, vSync);
}

void RendererFacade::shutdown(rhi::Surface& surface)
{
  if(m_backend == RendererBackend::gpuDriven)
  {
    gpuDriven().shutdown(surface);
    return;
  }
  legacy().shutdown(surface);
}

void RendererFacade::setVSync(bool enabled)
{
  if(m_backend == RendererBackend::gpuDriven)
  {
    gpuDriven().setVSync(enabled);
    return;
  }
  legacy().setVSync(enabled);
}

bool RendererFacade::getVSync() const
{
  return m_backend == RendererBackend::gpuDriven ? gpuDriven().getVSync() : legacy().getVSync();
}

void RendererFacade::setFullscreen(bool enabled, void* platformHandle)
{
  if(m_backend == RendererBackend::gpuDriven)
  {
    gpuDriven().setFullscreen(enabled, platformHandle);
    return;
  }
  legacy().setFullscreen(enabled, platformHandle);
}

const char* RendererFacade::getSwapchainPresentModeName() const
{
  return m_backend == RendererBackend::gpuDriven ? gpuDriven().getSwapchainPresentModeName() : legacy().getSwapchainPresentModeName();
}

uint32_t RendererFacade::getSwapchainImageCount() const
{
  return m_backend == RendererBackend::gpuDriven ? gpuDriven().getSwapchainImageCount() : legacy().getSwapchainImageCount();
}

void RendererFacade::resize(rhi::Extent2D size)
{
  if(m_backend == RendererBackend::gpuDriven)
  {
    gpuDriven().resize(size);
    return;
  }
  legacy().resize(size);
}

void RendererFacade::render(const RenderParams& params)
{
  if(m_backend == RendererBackend::gpuDriven)
  {
    gpuDriven().render(params);
    return;
  }
  legacy().render(params);
}

TextureHandle RendererFacade::getViewportTextureHandle() const
{
  return m_backend == RendererBackend::gpuDriven ? gpuDriven().getViewportTextureHandle() : legacy().getViewportTextureHandle();
}

ImTextureID RendererFacade::getViewportTextureID(TextureHandle handle) const
{
  return m_backend == RendererBackend::gpuDriven ? gpuDriven().getViewportTextureID(handle) : legacy().getViewportTextureID(handle);
}

MaterialHandle RendererFacade::getMaterialHandle(uint32_t slot) const
{
  return m_backend == RendererBackend::gpuDriven ? gpuDriven().getMaterialHandle(slot) : legacy().getMaterialHandle(slot);
}

GltfUploadResult RendererFacade::uploadGltfModel(const GltfModel& model, VkCommandBuffer cmd)
{
  return m_backend == RendererBackend::gpuDriven ? gpuDriven().uploadGltfModel(model, cmd) : legacy().uploadGltfModel(model, cmd);
}

void RendererFacade::uploadGltfModelBatch(const GltfModel&          model,
                                          std::span<const uint32_t> textureIndices,
                                          std::span<const uint32_t> materialIndices,
                                          std::span<const uint32_t> meshIndices,
                                          GltfUploadResult&         ioResult,
                                          VkCommandBuffer           cmd)
{
  if(m_backend == RendererBackend::gpuDriven)
  {
    gpuDriven().uploadGltfModelBatch(model, textureIndices, materialIndices, meshIndices, ioResult, cmd);
    return;
  }
  legacy().uploadGltfModelBatch(model, textureIndices, materialIndices, meshIndices, ioResult, cmd);
}

void RendererFacade::initializeGltfUploadResult(const GltfModel& model, GltfUploadResult& outResult) const
{
  if(m_backend == RendererBackend::gpuDriven)
  {
    gpuDriven().initializeGltfUploadResult(model, outResult);
    return;
  }
  legacy().initializeGltfUploadResult(model, outResult);
}

void RendererFacade::destroyGltfResources(const GltfUploadResult& result)
{
  if(m_backend == RendererBackend::gpuDriven)
  {
    gpuDriven().destroyGltfResources(result);
    return;
  }
  legacy().destroyGltfResources(result);
}

void RendererFacade::updateMeshTransform(MeshHandle handle, const glm::mat4& transform)
{
  if(m_backend == RendererBackend::gpuDriven)
  {
    gpuDriven().updateMeshTransform(handle, transform);
    return;
  }
  legacy().updateMeshTransform(handle, transform);
}

void RendererFacade::executeUploadCommand(std::function<void(VkCommandBuffer)> uploadFn)
{
  if(m_backend == RendererBackend::gpuDriven)
  {
    gpuDriven().executeUploadCommand(std::move(uploadFn));
    return;
  }
  legacy().executeUploadCommand(std::move(uploadFn));
}

void RendererFacade::waitForIdle()
{
  if(m_backend == RendererBackend::gpuDriven)
  {
    gpuDriven().waitForIdle();
    return;
  }
  legacy().waitForIdle();
}

const shaderio::GPUCullStats& RendererFacade::getLastGPUCullingStats() const
{
  return m_backend == RendererBackend::gpuDriven ? gpuDriven().getLastGPUCullingStats() : legacy().getLastGPUCullingStats();
}

shaderio::ShadowUniforms* RendererFacade::getShadowUniformsData()
{
  return m_backend == RendererBackend::gpuDriven ? gpuDriven().getShadowUniformsData() : legacy().getShadowUniformsData();
}

CSMShadowResources& RendererFacade::getCSMShadowResources()
{
  return m_backend == RendererBackend::gpuDriven ? gpuDriven().getCSMShadowResources() : legacy().getCSMShadowResources();
}

const char* RendererFacade::getBackendName() const
{
  return m_backend == RendererBackend::gpuDriven ? "GPUDrivenRenderer" : "Renderer";
}

GPUDrivenRuntimeStats RendererFacade::getGPUDrivenRuntimeStats() const
{
  return m_backend == RendererBackend::gpuDriven ? gpuDriven().getRuntimeStats() : GPUDrivenRuntimeStats{};
}

}  // namespace demo
