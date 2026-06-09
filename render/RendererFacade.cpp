#include "RendererFacade.h"

namespace demo
{
	std::unique_ptr<rhi::Surface> RendererFacade::createSurface() const
	{
		return gpuDriven().createSurface();
	}

	void RendererFacade::init(void* window, rhi::Surface& surface, bool vSync)
	{
		gpuDriven().init(window, surface, vSync);
	}

	void RendererFacade::shutdown(rhi::Surface& surface)
	{
		gpuDriven().shutdown(surface);
	}

	void RendererFacade::setVSync(bool enabled)
	{
		gpuDriven().setVSync(enabled);
	}

	bool RendererFacade::getVSync() const
	{
		return gpuDriven().getVSync();
	}

	void RendererFacade::setFullscreen(bool enabled, void* platformHandle)
	{
		gpuDriven().setFullscreen(enabled, platformHandle);
	}

	const char* RendererFacade::getSwapchainPresentModeName() const
	{
		return gpuDriven().getSwapchainPresentModeName();
	}

	uint32_t RendererFacade::getSwapchainImageCount() const
	{
		return gpuDriven().getSwapchainImageCount();
	}

	void RendererFacade::resize(rhi::Extent2D size)
	{
		gpuDriven().resize(size);
	}

	void RendererFacade::beginUiFrame()
	{
		gpuDriven().beginUiFrame();
	}

	void RendererFacade::render(const RenderParams& params)
	{
		gpuDriven().render(params);
	}

	void RendererFacade::setSceneRenderingSuspended(bool suspended)
	{
		gpuDriven().setSceneRenderingSuspended(suspended);
	}

	bool RendererFacade::isSceneRenderingSuspended() const
	{
		return gpuDriven().isSceneRenderingSuspended();
	}

	TextureHandle RendererFacade::getViewportTextureHandle() const
	{
		return gpuDriven().getViewportTextureHandle();
	}

	ImTextureID RendererFacade::getViewportTextureID(TextureHandle handle) const
	{
		return gpuDriven().getViewportTextureID(handle);
	}

	MaterialHandle RendererFacade::getMaterialHandle(uint32_t slot) const
	{
		return gpuDriven().getMaterialHandle(slot);
	}

	GltfUploadResult RendererFacade::uploadGltfModel(const GltfModel& model, rhi::CommandBuffer& cmd)
	{
		return gpuDriven().uploadGltfModel(model, cmd);
	}

	SceneUploadResult RendererFacade::commitSceneUploadPlan(const SceneAsset& asset,
	                                                        const SceneUploadPlan& plan,
	                                                        rhi::CommandBuffer& cmd)
	{
		return gpuDriven().commitSceneUploadPlan(asset, plan, cmd);
	}

	void RendererFacade::uploadGltfModelBatch(const GltfModel& model,
	                                          std::span<const uint32_t> textureIndices,
	                                          std::span<const uint32_t> materialIndices,
	                                          std::span<const uint32_t> meshIndices,
	                                          GltfUploadResult& ioResult,
	                                          rhi::CommandBuffer& cmd)
	{
		gpuDriven().uploadGltfModelBatch(model, textureIndices, materialIndices, meshIndices, ioResult, cmd);
	}

	void RendererFacade::initializeGltfUploadResult(const GltfModel& model, GltfUploadResult& outResult) const
	{
		gpuDriven().initializeGltfUploadResult(model, outResult);
	}

	void RendererFacade::destroyGltfResources(const GltfUploadResult& result)
	{
		gpuDriven().destroyGltfResources(result);
	}

	void RendererFacade::updateMeshTransform(MeshHandle handle, const glm::mat4& transform)
	{
		gpuDriven().updateMeshTransform(handle, transform);
	}

	void RendererFacade::updateSceneInstanceTransform(uint32_t instanceIndex, const glm::mat4& transform)
	{
		gpuDriven().updateSceneInstanceTransform(instanceIndex, transform);
	}

	void RendererFacade::executeUploadCommand(std::function<void(rhi::CommandBuffer&)> uploadFn)
	{
		gpuDriven().executeUploadCommand(std::move(uploadFn));
	}

	void RendererFacade::waitForIdle()
	{
		gpuDriven().waitForIdle();
	}

	const shaderio::GPUCullStats& RendererFacade::getLastGPUCullingStats() const
	{
		return gpuDriven().getLastGPUCullingStats();
	}

	RuntimeProfileSnapshot RendererFacade::getRuntimeProfileSnapshot() const
	{
		return gpuDriven().getRuntimeProfileSnapshot();
	}

	shaderio::ShadowUniforms* RendererFacade::getShadowUniformsData()
	{
		return gpuDriven().getShadowUniformsData();
	}

	CSMShadowResources& RendererFacade::getCSMShadowResources()
	{
		return gpuDriven().getCSMShadowResources();
	}

	const char* RendererFacade::getBackendName() const
	{
		return "GPUDrivenRenderer";
	}

	GPUDrivenRuntimeStats RendererFacade::getGPUDrivenRuntimeStats() const
	{
		return gpuDriven().getRuntimeStats();
	}

	bool RendererFacade::isExperimentalMeshletPathEnabled() const
	{
		return gpuDriven().isExperimentalMeshletPathEnabled();
	}
} // namespace demo
