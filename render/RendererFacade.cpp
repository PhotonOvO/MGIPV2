#include "RendererFacade.h"

#include <limits>

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
#if !defined(__ANDROID__)
		m_scenePicking.cancelAll();
		try
		{
			std::vector<editor::EditorPickResult> terminals;
			terminals.reserve(m_scenePicking.pendingCount());
			m_scenePicking.poll(terminals);
			for (const editor::EditorPickResult& terminal : terminals)
			{
				onTerminal(terminal);
			}
		}
		catch (...)
		{
			// ScenePicking's noexcept destructor accounts for any still-buffered
			// terminals through the allocation-free shutdown sink.
		}
		gpuDriven().clearEditorHighlight();
#endif
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
#if !defined(__ANDROID__)
		m_scenePicking.cancelAll();
		gpuDriven().invalidateEditorPickingPublication();
#endif
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
#if !defined(__ANDROID__)
		if (suspended)
		{
			m_scenePicking.cancelAll();
			gpuDriven().invalidateEditorPickingPublication();
		}
#endif
		gpuDriven().setSceneRenderingSuspended(suspended);
	}

	bool RendererFacade::isSceneRenderingSuspended() const
	{
		return gpuDriven().isSceneRenderingSuspended();
	}

#if !defined(__ANDROID__)
	std::optional<editor::EditorPickFence>
	RendererFacade::editorPickFence() const noexcept
	{
		const auto publication = gpuDriven().editorPickPublication();
		return publication ? std::optional<editor::EditorPickFence>{publication->first}
		                  : std::nullopt;
	}

	std::optional<std::pair<editor::EditorPickFence, uint64_t>>
	RendererFacade::editorPickPublication() const noexcept
	{
		return gpuDriven().editorPickPublication();
	}

	std::optional<editor::RenderOccurrenceRecord>
	RendererFacade::editorResolveOccurrence(
		const editor::EditorPickFence& fence, uint64_t occurrenceGeneration,
		editor::RenderOccurrenceId occurrence) const noexcept
	{
		return gpuDriven().editorResolveOccurrence(
			fence, occurrenceGeneration, occurrence);
	}

	std::optional<editor::RenderOccurrenceRecord>
	RendererFacade::editorResolveOccurrence(
		const editor::EditorPickFence& fence, uint64_t occurrenceGeneration,
		editor::PrimitiveOccurrenceKey occurrence) const noexcept
	{
		return gpuDriven().editorResolveOccurrence(
			fence, occurrenceGeneration, occurrence);
	}

	bool RendererFacade::editorNodeHighlightEligible(
		const editor::EditorPickFence& fence, uint64_t occurrenceGeneration,
		editor::NodeId node) const noexcept
	{
		return gpuDriven().editorNodeHighlightEligible(
			fence, occurrenceGeneration, node);
	}

	editor::PickSubmitResult RendererFacade::enqueueEditorPick(
		const editor::EditorPickRequest& request)
	{
		editor::SceneRenderOccurrenceSnapshot snapshot =
			gpuDriven().editorOccurrenceSnapshot(request);
		return snapshot
			? m_scenePicking.submit(request, std::move(snapshot))
			: editor::PickSubmitResult{
				editor::PickSubmitStatus::rejectedSnapshot, request.serial};
	}

	void RendererFacade::pollEditorPickResults(
		std::vector<editor::EditorPickResult>& out)
	{
		out.reserve(out.size() + m_scenePicking.pendingCount());
		m_scenePicking.poll(out);
	}

	void RendererFacade::cancelEditorPicks() noexcept
	{
		try
		{
			m_scenePicking.cancelAll();
		}
		catch (...)
		{
			std::terminate();
		}
	}

	void RendererFacade::cancelEditorPicks(
		editor::EditorDocumentAuthority authority) noexcept
	{
		try
		{
			m_scenePicking.cancelAuthority(authority);
		}
		catch (...)
		{
			std::terminate();
		}
	}

	bool RendererFacade::editorPickingBusy() const noexcept
	{
		return gpuDriven().editorPickingBusy();
	}

	EditorGpuInteractionDiagnostics
	RendererFacade::editorInteractionDiagnostics() const noexcept
	{
		return gpuDriven().editorInteractionDiagnostics();
	}

	bool RendererFacade::editorOverlayAvailable() const noexcept
	{
		return gpuDriven().editorOverlayAvailable();
	}

	EditorOverlayPassDiagnostics
	RendererFacade::editorOverlayDiagnostics() const noexcept
	{
		return gpuDriven().editorOverlayDiagnostics();
	}

	bool RendererFacade::setEditorHighlight(
		const editor::SceneSelectionValue& selection,
		editor::SelectionRevision revision) noexcept
	{
		return gpuDriven().setEditorHighlight(selection, revision);
	}

	void RendererFacade::clearEditorHighlight() noexcept
	{
		gpuDriven().clearEditorHighlight();
	}

	void RendererFacade::invalidateEditorPickingPublication() noexcept
	{
		gpuDriven().invalidateEditorPickingPublication();
	}

	size_t RendererFacade::pendingEditorPickCount() const noexcept
	{
		return m_scenePicking.pendingCount();
	}

	void RendererFacade::onTerminal(
		const editor::EditorPickResult& result) noexcept
	{
		(void)result;
		if (m_shutdownPickTerminalCount !=
			std::numeric_limits<uint64_t>::max())
		{
			++m_shutdownPickTerminalCount;
		}
	}
#endif

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

	void RendererFacade::syncActiveSceneRuntimeState(SceneUploadResult& ioResult) const
	{
		gpuDriven().syncActiveSceneRuntimeState(ioResult);
	}

#if defined(MGIF_ENABLE_SCENE_TRANSACTIONS)
	RendererSceneTransactionInitializationResult
	RendererFacade::initializeSceneTransactions(
		RendererSceneTransactionInitialState initialState) noexcept
	{
		return gpuDriven().initializeSceneTransactions(std::move(initialState));
	}

	void RendererFacade::resetSceneTransactions() noexcept
	{
#if !defined(__ANDROID__)
		cancelEditorPicks();
		clearEditorHighlight();
		invalidateEditorPickingPublication();
#endif
		gpuDriven().resetSceneTransactions();
	}

	editor::SceneApplyResult RendererFacade::applySceneChanges(
		const editor::ApplySceneChangeRequest& request) noexcept
	{
#if !defined(__ANDROID__)
		cancelEditorPicks(request.expectedTarget.authority);
		invalidateEditorPickingPublication();
#endif
		return gpuDriven().applySceneChanges(request);
	}

	editor::SceneApplyResult RendererFacade::applyFullSceneSnapshot(
		const ApplyFullSceneSnapshotRequest& request) noexcept
	{
#if !defined(__ANDROID__)
		cancelEditorPicks(request.transaction.expectedTarget.authority);
		invalidateEditorPickingPublication();
#endif
		return gpuDriven().applyFullSceneSnapshot(request);
	}

	RendererSceneStateSnapshot RendererFacade::activeSceneState() const noexcept
	{
		return gpuDriven().activeSceneState();
	}

	editor::SceneDeltaCounterSnapshot RendererFacade::sceneDeltaCounters() const noexcept
	{
		return gpuDriven().sceneDeltaCounters();
	}

	SceneTransactionConcreteStateSnapshot
	RendererFacade::sceneTransactionConcreteState() const
	{
		return gpuDriven().sceneTransactionConcreteState();
	}

	void RendererFacade::setSceneTransactionFailurePoint(
		SceneTransactionFailurePoint point,
		uint32_t submissionIndex) noexcept
	{
		gpuDriven().setSceneTransactionFailurePoint(point, submissionIndex);
	}

	uint64_t RendererFacade::collectSceneTransactionGarbage() noexcept
	{
		return gpuDriven().collectSceneTransactionGarbage();
	}

	uint64_t RendererFacade::sceneTransactionVisibleGeneration() const noexcept
	{
		return gpuDriven().sceneTransactionVisibleGeneration();
	}

	editor::RendererInstanceId RendererFacade::editorRendererIdentity() const noexcept
	{
		return gpuDriven().editorRendererIdentity();
	}

	editor::RendererDeviceId RendererFacade::editorDeviceIdentity() const noexcept
	{
		return gpuDriven().editorDeviceIdentity();
	}
#endif

	void RendererFacade::executeUploadCommand(std::function<void(rhi::CommandBuffer&)> uploadFn)
	{
		gpuDriven().executeUploadCommand(std::move(uploadFn));
	}

	void RendererFacade::waitForIdle()
	{
		gpuDriven().waitForIdle();
	}

	bool RendererFacade::loadDDGIMeshSDF(const std::filesystem::path& path, std::string& outError)
	{
		return gpuDriven().loadDDGIMeshSDF(path, outError);
	}

	void RendererFacade::setDDGIEnabled(bool enabled)
	{
		gpuDriven().setDDGIEnabled(enabled);
	}

	bool RendererFacade::isDDGIEnabled() const
	{
		return gpuDriven().isDDGIEnabled();
	}

	bool RendererFacade::hasDDGIMeshSDF() const
	{
		return gpuDriven().hasDDGIMeshSDF();
	}

	void RendererFacade::resetDDGIHistory()
	{
		gpuDriven().resetDDGIHistory();
	}

	DDGIConfig RendererFacade::getDDGIConfig() const
	{
		return gpuDriven().getEditableDDGIConfig();
	}

	void RendererFacade::setDDGIConfig(const DDGIConfig& config)
	{
		gpuDriven().setEditableDDGIConfig(config);
	}

	FlaxGIDebugStatus RendererFacade::getFlaxGIDebugStatus() const
	{
		return gpuDriven().getFlaxGIDebugStatus();
	}
	FlaxGIDebugSnapshot RendererFacade::getFlaxGIDebugSnapshot() const
	{
		return gpuDriven().getFlaxGIDebugSnapshot();
	}

	FlaxGIDebugViewSet RendererFacade::getFlaxGIDebugViewSet() const
	{
		return gpuDriven().getFlaxGIDebugViewSet();
	}

	const shaderio::GPUCullStats& RendererFacade::getLastGPUCullingStats() const
	{
		return gpuDriven().getLastGPUCullingStats();
	}

	RuntimeProfileSnapshot RendererFacade::getRuntimeProfileSnapshot() const
	{
		return gpuDriven().getRuntimeProfileSnapshot();
	}

	rhi::RHIHotPathCounters RendererFacade::getRhiHotPathCounters() const noexcept
	{
		return gpuDriven().getRhiHotPathCounters();
	}

	void RendererFacade::resetRhiHotPathCounters() noexcept
	{
		gpuDriven().resetRhiHotPathCounters();
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
