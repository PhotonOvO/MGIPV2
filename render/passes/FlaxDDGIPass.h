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
#include "../Pass.h"

namespace demo
{
class GPUDrivenRenderer;
class GlobalSurfaceAtlasPass;

class FlaxDDGIPass : public ComputePassNode
{
public:
  explicit FlaxDDGIPass(GPUDrivenRenderer* renderer);
  ~FlaxDDGIPass() override = default;

  [[nodiscard]] const char* getName() const override { return "FlaxDDGIPass"; }
  [[nodiscard]] HandleSlice<PassResourceDependency> getDependencies() const override;
  void execute(const PassContext& context) const override;

  // Call after FlaxDDGIResources are initialized (once, from GPUDrivenRenderer).
  void initResources(rhi::Device& device);

  // Call when FlaxDDGIResources are being deinitialized.
  void shutdownResources();

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
  rhi::ArgumentLayoutHandle m_irradianceLayout{};

  // --- Argument tables ---
  rhi::ArgumentTableHandle m_classifyTable{};
  rhi::ArgumentTableHandle m_initArgsTable{};
  rhi::ArgumentTableHandle m_updateInactiveTable{};
  rhi::ArgumentTableHandle m_traceRaysTable{};
  rhi::ArgumentTableHandle m_distanceTable{};
  rhi::ArgumentTableHandle m_irradianceTable{};

  // --- Compute pipelines ---
  rhi::PipelineHandle m_classifyPipeline{};
  rhi::PipelineHandle m_initArgsPipeline{};
  rhi::PipelineHandle m_updateInactivePipeline{};
  rhi::PipelineHandle m_traceRaysPipeline{};
  rhi::PipelineHandle m_distancePipeline{};
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
	rhi::SamplerHandle m_fallbackSampler{};

	// --- Per-frame uniform buffers (FlaxDDGIData, ~288 bytes) ---
	static constexpr uint32_t kMaxFramesInFlight = 3;
	rhi::BufferHandle m_ddgiUniformBuffers[kMaxFramesInFlight]{};
	mutable uint32_t m_probeUpdateOffset{0u};

	void writeFlaxDDGIDataToBuffer(uint32_t frameIndex) const;
	void bindFlaxDDGIDataBuffer(uint32_t frameIndex) const;
	void bindFlaxHistoryParity(uint32_t parity) const;
};

} // namespace demo
