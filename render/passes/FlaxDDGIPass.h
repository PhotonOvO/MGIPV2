#pragma once

// Flax DDGI Pass (FGI-R2): single owner of Flax DDGI compute work.
//
// In Flax mode, this pass replaces Current DDGIRayTracePass + DDGIProbeUpdatePass.
// Current passes skip themselves when Flax mode is active (via isDDGIProbeDataPathEnabled
// returning false for their specific gating).
//
// Sub-passes:
//   Classify      — probe classification and relocation (FGI-050)
//   InitArgs      — indirect dispatch argument builder (FGI-051)
//   UpdateInactive — jump-flood inactive probe fallback (FGI-052)
//   TraceRays     — SDF ray trace into Flax ProbesTrace (FGI-053)
//   UpdateDistance — octahedral distance moment update (FGI-054)
//   UpdateIrradiance — octahedral irradiance convolution update (FGI-055)
//
// GPU markers follow FGI-081 naming: DDGI.Classify, DDGI.InitArgs, etc.

#include "../FlaxDDGIResources.h"
#include "../FlaxGIDebugModel.h"
#include "../Pass.h"

#include <vector>

namespace demo
{
class GPUDrivenRenderer;
class GlobalSurfaceAtlasPass;
struct DebugPassOptions;

class FlaxDDGIPass : public ComputePassNode
{
public:
  explicit FlaxDDGIPass(GPUDrivenRenderer* renderer);
  ~FlaxDDGIPass() override = default;

  [[nodiscard]] const char* getName() const override { return "FlaxDDGIPass"; }
  [[nodiscard]] HandleSlice<PassResourceDependency> getDependencies() const override;
  void execute(const PassContext& context) const override;

  // Call after FlaxDDGIResources are initialized (once, from GPUDrivenRenderer).
  void initResources(rhi::Device& device, uint32_t frameCount);

  // Call when FlaxDDGIResources are being deinitialized.
  void shutdownResources();

  // Invalidates probe state and both history atlases after a layout-scale change.
  void requestRuntimeReset() { m_runtimeResetRequested = true; }

  [[nodiscard]] bool isReady() const { return m_device != nullptr && m_flaxResources != nullptr && m_pipelinesCreated; }
  [[nodiscard]] rhi::TextureViewHandle getProbesDataView() const { return m_probesDataView; }
  [[nodiscard]] rhi::TextureViewHandle getProbesDistanceOutputView(uint32_t parity = 0u) const
  {
    return (parity & 1u) == 0u ? m_probesDistanceViewA : m_probesDistanceViewB;
  }
  [[nodiscard]] rhi::TextureViewHandle getProbesIrradianceOutputView(uint32_t parity = 0u) const
  {
    return (parity & 1u) == 0u ? m_probesIrradianceViewA : m_probesIrradianceViewB;
  }
  [[nodiscard]] FlaxGIOutputSelection getLightingOutputSelection(
    const DebugPassOptions& debugOptions) const;
  [[nodiscard]] FlaxGIOutputSelection getPublishedOutputSelection() const
  {
    return m_outputState.published();
  }
  [[nodiscard]] FlaxGIDebugSnapshot getDebugSnapshot() const { return m_debugSnapshot; }
  [[nodiscard]] FlaxGIDebugViewSet getDebugViewSet() const;

private:
  GPUDrivenRenderer* m_renderer{nullptr};
  FlaxDDGIResources* m_flaxResources{nullptr}; // non-owning pointer
  rhi::Device* m_device{nullptr};
  mutable bool m_pipelinesCreated{false};
  mutable bool m_textureLayoutsInitialized{false};

  // --- Argument layouts (one per shader) ---
  rhi::ArgumentLayoutHandle m_classifyLayout{};
  rhi::ArgumentLayoutHandle m_initArgsLayout{};
  rhi::ArgumentLayoutHandle m_updateInactiveLayout{};
  rhi::ArgumentLayoutHandle m_traceRaysLayout{};
  rhi::ArgumentLayoutHandle m_distanceLayout{};
  rhi::ArgumentLayoutHandle m_debugViewsLayout{};
  rhi::ArgumentLayoutHandle m_irradianceLayout{};

  // --- Argument tables ---
  std::vector<rhi::ArgumentTableHandle> m_classifyTables;
  std::vector<rhi::ArgumentTableHandle> m_initArgsTables;
  std::vector<rhi::ArgumentTableHandle> m_updateInactiveTables;
  std::vector<rhi::ArgumentTableHandle> m_traceRaysTables;
  std::vector<rhi::ArgumentTableHandle> m_distanceTables;
  std::vector<rhi::ArgumentTableHandle> m_debugViewsTables;
  std::vector<rhi::ArgumentTableHandle> m_irradianceTables;

  // --- Compute pipelines ---
  rhi::PipelineHandle m_classifyPipeline{};
  rhi::PipelineHandle m_initArgsPipeline{};
  rhi::PipelineHandle m_compactPipeline{};
  rhi::PipelineHandle m_updateInactivePipeline{};
  rhi::PipelineHandle m_traceRaysPipeline{};
  rhi::PipelineHandle m_distancePipeline{};
  rhi::PipelineHandle m_debugViewsPipeline{};
  rhi::PipelineHandle m_irradiancePipeline{};
	// --- Texture views (over FlaxDDGIResources textures) ---
	rhi::TextureViewHandle m_probesDataView{};
	rhi::TextureViewHandle m_probesTraceView{};
	rhi::TextureViewHandle m_probesIrradianceViewA{};
	rhi::TextureViewHandle m_probesIrradianceViewB{};
	rhi::TextureViewHandle m_probesDistanceViewA{};
	rhi::TextureViewHandle m_probesDistanceViewB{};
	rhi::TextureViewHandle m_classifySDFView{};
	rhi::TextureViewHandle m_traceSDFView{};
	rhi::TextureViewHandle m_atlasDepthView{};
	rhi::TextureViewHandle m_atlasLightingView{};
	rhi::TextureViewHandle m_traceAlbedoView{};
	rhi::TextureHandle m_debugAtlas{};
	rhi::TextureViewHandle m_debugAtlasView{};
	uint64_t m_debugTextureId{0};
	static constexpr uint32_t kDebugAtlasWidth = 1024;
	static constexpr uint32_t kDebugAtlasHeight = 512;
	static constexpr uint32_t kDebugReadbackUintCount = 11;
	mutable uint64_t m_consumedResetRequestId{0};
	mutable bool m_runtimeResetRequested{false};
	mutable uint64_t m_consumedRunToStageRequestId{0};
	rhi::SamplerHandle m_fallbackSampler{};

	// --- Per-frame resources ---
	static constexpr uint32_t kHistoryParityCount = 2u;
	uint32_t m_frameCount{0};
	std::vector<rhi::BufferHandle> m_ddgiUniformBuffers;
	std::vector<rhi::BufferHandle> m_debugReadbackBuffers;
	mutable std::vector<uint64_t> m_debugReadbackSourceFrames;
	mutable uint64_t m_debugAtlasSourceFrame{0};
	mutable FlaxGIDebugSnapshot m_debugSnapshot{};
	mutable FlaxGIOutputState m_outputState{};

	mutable std::vector<uint32_t> m_probeUpdateOffsets;
	mutable std::vector<uint32_t> m_priorityProbeUpdateOffsets;

	void writeFlaxDDGIDataToBuffer(uint32_t frameIndex) const;
	[[nodiscard]] bool plansFullOutputUpdate(const DebugPassOptions& debugOptions) const;
	[[nodiscard]] bool hasPendingReset(const DebugPassOptions& debugOptions) const;
	[[nodiscard]] uint32_t frameCascadeTableIndex(
		uint32_t frameIndex, uint32_t cascadeIndex) const
	{
		return frameIndex * m_flaxResources->getCascadeCount() + cascadeIndex;
	}
	[[nodiscard]] uint32_t historyTableIndex(
		uint32_t frameIndex, uint32_t cascadeIndex, uint32_t parity) const
	{
		return (frameCascadeTableIndex(frameIndex, cascadeIndex) * kHistoryParityCount)
			+ (parity & 1u);
	}
	void readDebugTelemetry(uint32_t frameIndex, uint32_t maximumProbeCount) const;
	void recordDebugReadback(rhi::CommandBuffer& cmd, uint32_t frameIndex, uint64_t sourceFrame) const;
	void executeDebugViews(rhi::CommandBuffer& cmd, const PassContext& context,
	                       rhi::ArgumentTableHandle table) const;
	void markDebugStage(FlaxGIDebugStage stage, FlaxGIDebugStageState state,
	                    uint64_t frame, std::string_view reason = {}) const;

};

} // namespace demo
