#include "GPUDrivenRenderer.h"

namespace demo {

namespace {

uint32_t buildGPUDrivenFlags(const MeshRecord& mesh)
{
  uint32_t flags = shaderio::LGPUCullFlagFrustumCulling | shaderio::LGPUCullFlagOcclusionCulling;
  if(mesh.alphaMode == shaderio::LAlphaBlend)
  {
    flags = shaderio::LGPUCullFlagFrustumCulling | shaderio::LGPUCullFlagTransparent;
  }
  return flags;
}

}  // namespace

void GPUDrivenRenderer::init(GLFWwindow* window, rhi::Surface& surface, bool vSync)
{
  m_renderer.init(window, surface, vSync);
  m_sceneRegistry.init(m_renderer.getNativeDeviceHandle(), m_renderer.getAllocatorHandle());
  m_meshletBuffer.init(m_renderer.getNativeDeviceHandle(), m_renderer.getAllocatorHandle());
  m_hiZDepthPyramid.init(m_renderer.getSceneExtent());
}

void GPUDrivenRenderer::shutdown(rhi::Surface& surface)
{
  m_meshletBuffer.deinit();
  m_sceneRegistry.deinit();
  m_renderer.shutdown(surface);
}

void GPUDrivenRenderer::resize(rhi::Extent2D size)
{
  m_renderer.resize(size);
  m_hiZDepthPyramid.init(m_renderer.getSceneExtent());
}

void GPUDrivenRenderer::render(const RenderParams& params)
{
  m_hiZDepthPyramid.init(m_renderer.getSceneExtent());
  flushPendingSceneUploads();
  refreshSceneView();

  RenderParams gpuParams = params;
  gpuParams.gpuDrivenSceneView = m_sceneView.usePersistentCullingObjects ? &m_sceneView : nullptr;
  m_renderer.render(gpuParams);

  m_batchBuilder.buildBatches(VK_NULL_HANDLE, m_renderer.getLastGPUCullingStats().visibleCount);
  m_runtimeStats.batchStats = m_batchBuilder.getStats();
}

GltfUploadResult GPUDrivenRenderer::uploadGltfModel(const GltfModel& model, VkCommandBuffer cmd)
{
  GltfUploadResult result = m_renderer.uploadGltfModel(model, cmd);
  rebuildGPUDrivenScene(model, result, cmd);
  return result;
}

void GPUDrivenRenderer::uploadGltfModelBatch(const GltfModel&          model,
                                             std::span<const uint32_t> textureIndices,
                                             std::span<const uint32_t> materialIndices,
                                             std::span<const uint32_t> meshIndices,
                                             GltfUploadResult&         ioResult,
                                             VkCommandBuffer           cmd)
{
  m_renderer.uploadGltfModelBatch(model, textureIndices, materialIndices, meshIndices, ioResult, cmd);
  rebuildGPUDrivenScene(model, ioResult, cmd);
}

void GPUDrivenRenderer::destroyGltfResources(const GltfUploadResult& result)
{
  clearGPUDrivenScene();
  m_renderer.destroyGltfResources(result);
}

void GPUDrivenRenderer::updateMeshTransform(MeshHandle handle, const glm::mat4& transform)
{
  m_renderer.updateMeshTransform(handle, transform);

  const auto it = m_objectIdByMeshHandle.find(packMeshHandleKey(handle));
  if(it == m_objectIdByMeshHandle.end())
  {
    return;
  }

  const MeshRecord* meshRecord = m_renderer.getMeshPool().tryGet(handle);
  if(meshRecord == nullptr)
  {
    return;
  }

  m_sceneRegistry.updateTransform(it->second,
                                  transform,
                                  glm::vec4(meshRecord->worldBoundsCenter, meshRecord->worldBoundsRadius));
  m_sceneUploadPending = true;
  m_runtimeStats.pendingSceneUpdates = 1;
  refreshSceneView();
}

uint64_t GPUDrivenRenderer::packMeshHandleKey(MeshHandle handle)
{
  return (static_cast<uint64_t>(handle.generation) << 32u) | static_cast<uint64_t>(handle.index);
}

void GPUDrivenRenderer::rebuildGPUDrivenScene(const GltfModel& model, const GltfUploadResult& uploadResult, VkCommandBuffer cmd)
{
  clearGPUDrivenScene();
  m_hiZDepthPyramid.init(m_renderer.getSceneExtent());
  m_batchBuilder.init(static_cast<uint32_t>(uploadResult.meshes.size()));

  std::vector<shaderio::Meshlet> allMeshlets;
  std::vector<uint32_t>          allMeshletIndices;

  for(size_t meshIndex = 0; meshIndex < uploadResult.meshes.size() && meshIndex < model.meshes.size(); ++meshIndex)
  {
    const MeshHandle meshHandle = uploadResult.meshes[meshIndex];
    const MeshRecord* meshRecord = m_renderer.getMeshPool().tryGet(meshHandle);
    if(meshRecord == nullptr)
    {
      continue;
    }

    GPUSceneRegistrationDesc desc{};
    desc.meshHandle = meshHandle;
    desc.meshIndex = static_cast<uint32_t>(meshIndex);
    desc.materialIndex = meshRecord->materialIndex >= 0 ? static_cast<uint32_t>(meshRecord->materialIndex) : UINT32_MAX;
    desc.transform = meshRecord->transform;
    desc.boundsSphere = glm::vec4(meshRecord->worldBoundsCenter, meshRecord->worldBoundsRadius);
    desc.flags = buildGPUDrivenFlags(*meshRecord);
    desc.indexCount = meshRecord->indexCount;
    desc.firstIndex = meshRecord->firstIndex;
    desc.vertexOffset = meshRecord->vertexOffset;

    const uint32_t objectId = m_sceneRegistry.registerObject(desc);
    m_objectIdByMeshHandle[packMeshHandleKey(meshHandle)] = objectId;

    MeshletConversionResult meshlets = MeshletConverter::convert(model.meshes[meshIndex]);
    for(shaderio::Meshlet& meshlet : meshlets.meshlets)
    {
      meshlet.materialIndex = desc.materialIndex;
    }
    allMeshlets.insert(allMeshlets.end(), meshlets.meshlets.begin(), meshlets.meshlets.end());
    allMeshletIndices.insert(allMeshletIndices.end(), meshlets.packedIndices.begin(), meshlets.packedIndices.end());
    m_runtimeStats.meshletTriangleCount += meshlets.triangleCount;
  }

  m_sceneRegistry.syncToGpu(cmd);
  m_meshletBuffer.uploadMeshlets(cmd, allMeshlets, allMeshletIndices);
  m_sceneUploadPending = false;
  m_runtimeStats.sceneUploadCount += 1;
  m_runtimeStats.pendingSceneUpdates = 0;
  m_runtimeStats.objectCount = m_sceneRegistry.getObjectCount();
  m_runtimeStats.meshletCount = m_meshletBuffer.getMeshletCount();
  refreshSceneView();
}

void GPUDrivenRenderer::clearGPUDrivenScene()
{
  m_sceneRegistry.clear();
  m_meshletBuffer.clear();
  m_objectIdByMeshHandle.clear();
  m_sceneView = {};
  m_runtimeStats = {};
  m_sceneUploadPending = false;
}

void GPUDrivenRenderer::flushPendingSceneUploads()
{
  if(!m_sceneUploadPending || !m_sceneRegistry.isDirty())
  {
    return;
  }

  m_renderer.executeUploadCommand([this](VkCommandBuffer cmd) {
    m_sceneRegistry.syncToGpu(cmd);
  });
  m_sceneUploadPending = false;
  m_runtimeStats.sceneUploadCount += 1;
  m_runtimeStats.pendingSceneUpdates = 0;
  m_runtimeStats.objectCount = m_sceneRegistry.getObjectCount();
}

void GPUDrivenRenderer::refreshSceneView()
{
  m_sceneView.gpuSceneObjectBufferAddress = m_sceneRegistry.getBufferAddress();
  m_sceneView.gpuCullObjectBufferAddress = m_sceneRegistry.getCullBufferAddress();
  m_sceneView.gpuCullObjectBuffer = m_sceneRegistry.getCullBufferHandle();
  m_sceneView.objectCount = m_sceneRegistry.getObjectCount();
  m_sceneView.overlayObjects = m_sceneRegistry.getOverlayObjects().empty() ? nullptr : m_sceneRegistry.getOverlayObjects().data();
  m_sceneView.overlayObjectCount = static_cast<uint32_t>(m_sceneRegistry.getOverlayObjects().size());
  m_sceneView.usePersistentCullingObjects = m_sceneView.gpuCullObjectBuffer != VK_NULL_HANDLE && m_sceneView.objectCount > 0;
  m_runtimeStats.objectCount = m_sceneView.objectCount;
  m_runtimeStats.meshletCount = m_meshletBuffer.getMeshletCount();
}

}  // namespace demo
