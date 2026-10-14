#pragma once

#include "GPUDrivenRenderer.h"
#if !defined(__ANDROID__)
#include "editor/ScenePicking.h"
#endif

#include <filesystem>
#include <utility>

namespace demo
{
	enum class RendererBackend
	{
		gpuDriven,
	};

	class RendererFacade
#if !defined(__ANDROID__)
		: private editor::IScenePickingTerminalSink
#endif
	{
	public:
		explicit RendererFacade(rhi::BackendType backend = rhi::defaultBackend())
			: m_gpuDrivenRenderer(backend)
#if !defined(__ANDROID__)
			, m_scenePicking(m_gpuDrivenRenderer, *this)
#endif
		{
		}
		explicit RendererFacade(std::unique_ptr<rhi::Device> sceneTransactionDevice)
			: m_gpuDrivenRenderer(std::move(sceneTransactionDevice))
#if !defined(__ANDROID__)
			, m_scenePicking(m_gpuDrivenRenderer, *this)
#endif
		{
		}

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
#if !defined(__ANDROID__)
		[[nodiscard]] std::optional<editor::EditorPickFence>
		editorPickFence() const noexcept;
		[[nodiscard]] std::optional<std::pair<editor::EditorPickFence, uint64_t>>
		editorPickPublication() const noexcept;
		[[nodiscard]] std::optional<editor::RenderOccurrenceRecord>
		editorResolveOccurrence(const editor::EditorPickFence& fence,
		                        uint64_t occurrenceGeneration,
		                        editor::RenderOccurrenceId occurrence) const noexcept;
		[[nodiscard]] std::optional<editor::RenderOccurrenceRecord>
		editorResolveOccurrence(const editor::EditorPickFence& fence,
		                        uint64_t occurrenceGeneration,
		                        editor::PrimitiveOccurrenceKey occurrence) const noexcept;
		[[nodiscard]] bool editorNodeHighlightEligible(
			const editor::EditorPickFence& fence,
			uint64_t occurrenceGeneration, editor::NodeId node) const noexcept;
		[[nodiscard]] editor::PickSubmitResult
		enqueueEditorPick(const editor::EditorPickRequest& request);
		void pollEditorPickResults(std::vector<editor::EditorPickResult>& out);
		void cancelEditorPicks() noexcept;
		void cancelEditorPicks(editor::EditorDocumentAuthority authority) noexcept;
		[[nodiscard]] bool editorPickingBusy() const noexcept;
			[[nodiscard]] EditorGpuInteractionDiagnostics
			editorInteractionDiagnostics() const noexcept;
			[[nodiscard]] bool editorOverlayAvailable() const noexcept;
			[[nodiscard]] EditorOverlayPassDiagnostics
			editorOverlayDiagnostics() const noexcept;
		[[nodiscard]] bool setEditorHighlight(
			const editor::SceneSelectionValue& selection,
			editor::SelectionRevision revision) noexcept;
		void clearEditorHighlight() noexcept;
		void invalidateEditorPickingPublication() noexcept;
		[[nodiscard]] size_t pendingEditorPickCount() const noexcept;
#endif

		[[nodiscard]] TextureHandle getViewportTextureHandle() const;
		[[nodiscard]] ImTextureID getViewportTextureID(TextureHandle handle) const;
		[[nodiscard]] MaterialHandle getMaterialHandle(uint32_t slot) const;
		GltfUploadResult uploadGltfModel(const GltfModel& model, rhi::CommandBuffer& cmd);
		SceneUploadResult commitSceneUploadPlan(const SceneAsset& asset, const SceneUploadPlan& plan,
		                                        rhi::CommandBuffer& cmd);
		void uploadGltfModelBatch(const GltfModel& model,
		                          std::span<const uint32_t> textureIndices,
		                          std::span<const uint32_t> materialIndices,
		                          std::span<const uint32_t> meshIndices,
		                          GltfUploadResult& ioResult,
		                          rhi::CommandBuffer& cmd);
		void initializeGltfUploadResult(const GltfModel& model, GltfUploadResult& outResult) const;
		void destroyGltfResources(const GltfUploadResult& result);
		void updateMeshTransform(MeshHandle handle, const glm::mat4& transform);
		void updateSceneInstanceTransform(uint32_t instanceIndex, const glm::mat4& transform);
		void syncActiveSceneRuntimeState(SceneUploadResult& ioResult) const;
#if defined(MGIF_ENABLE_SCENE_TRANSACTIONS)
		[[nodiscard]] RendererSceneTransactionInitializationResult
		initializeSceneTransactions(RendererSceneTransactionInitialState initialState) noexcept;
		void resetSceneTransactions() noexcept;
		[[nodiscard]] editor::SceneApplyResult
		applySceneChanges(const editor::ApplySceneChangeRequest& request) noexcept;
		[[nodiscard]] editor::SceneApplyResult
		applyFullSceneSnapshot(const ApplyFullSceneSnapshotRequest& request) noexcept;
		[[nodiscard]] RendererSceneStateSnapshot activeSceneState() const noexcept;
		[[nodiscard]] editor::SceneDeltaCounterSnapshot sceneDeltaCounters() const noexcept;
		[[nodiscard]] SceneTransactionConcreteStateSnapshot
		sceneTransactionConcreteState() const;
		void setSceneTransactionFailurePoint(
			SceneTransactionFailurePoint point,
			uint32_t submissionIndex = 0u) noexcept;
		[[nodiscard]] uint64_t collectSceneTransactionGarbage() noexcept;
		[[nodiscard]] uint64_t sceneTransactionVisibleGeneration() const noexcept;
		[[nodiscard]] editor::RendererInstanceId editorRendererIdentity() const noexcept;
		[[nodiscard]] editor::RendererDeviceId editorDeviceIdentity() const noexcept;
#endif
		void executeUploadCommand(std::function<void(rhi::CommandBuffer&)> uploadFn);
		void waitForIdle();
		[[nodiscard]] bool loadDDGIMeshSDF(const std::filesystem::path& path, std::string& outError);
		void setDDGIEnabled(bool enabled);
		[[nodiscard]] bool isDDGIEnabled() const;
		[[nodiscard]] bool hasDDGIMeshSDF() const;
		void resetDDGIHistory();
		[[nodiscard]] DDGIConfig getDDGIConfig() const;
		void setDDGIConfig(const DDGIConfig& config);
		[[nodiscard]] FlaxGIDebugStatus getFlaxGIDebugStatus() const;
		[[nodiscard]] FlaxGIDebugSnapshot getFlaxGIDebugSnapshot() const;
		[[nodiscard]] FlaxGIDebugViewSet getFlaxGIDebugViewSet() const;

		[[nodiscard]] const shaderio::GPUCullStats& getLastGPUCullingStats() const;
		[[nodiscard]] RuntimeProfileSnapshot getRuntimeProfileSnapshot() const;
		[[nodiscard]] rhi::RHIHotPathCounters getRhiHotPathCounters() const noexcept;
		void resetRhiHotPathCounters() noexcept;
		[[nodiscard]] shaderio::ShadowUniforms* getShadowUniformsData();
		[[nodiscard]] CSMShadowResources& getCSMShadowResources();
		[[nodiscard]] RendererBackend getBackend() const { return RendererBackend::gpuDriven; }
		[[nodiscard]] const char* getBackendName() const;
		[[nodiscard]] GPUDrivenRuntimeStats getGPUDrivenRuntimeStats() const;
		[[nodiscard]] bool isExperimentalMeshletPathEnabled() const;

	private:
		GPUDrivenRenderer& gpuDriven() { return m_gpuDrivenRenderer; }
		const GPUDrivenRenderer& gpuDriven() const { return m_gpuDrivenRenderer; }

#if !defined(__ANDROID__)
		void onTerminal(const editor::EditorPickResult& result) noexcept override;
#endif

		GPUDrivenRenderer m_gpuDrivenRenderer;
#if !defined(__ANDROID__)
		uint64_t m_shutdownPickTerminalCount{0u};
		editor::ScenePicking m_scenePicking;
#endif
	};
} // namespace demo
