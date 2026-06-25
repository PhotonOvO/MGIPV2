#pragma once

// Flax-style DDGI probe resources (FGI-042) and probe state encoding (FGI-043).
//
// Owns the multi-cascade DDGI GPU resources matching the Flax DDGI data model:
//   - ProbesTrace: 2D staging texture (rays x active-probe-batch), HDR radiance
//   - ProbesData: 2D texture with per-probe offset + state + attention (SNORM or float)
//   - ProbesIrradiance: 2D octahedral irradiance atlas (HDR), all cascades
//   - ProbesDistance: 2D octahedral distance/moment atlas, all cascades
//   - ActiveProbes: storage buffer with counter at element 0
//   - UpdateProbesInitArgs: indirect dispatch argument buffer
//
// All resources go through rhi:: handles. No native backend types in this header.
// Resource creation is gated on DDGIConfig::runtimeMode == DDGIRuntimeMode::flaxStyle.

#include "rhi/RHIDevice.h"
#include "rhi/RHIHandles.h"
#include "rhi/RHITypes.h"
#include "DDGIConfig.h"

#include <glm/glm.hpp>
#include <cstdint>
#include <cmath>

namespace demo
{

// Per-cascade descriptor computed on CPU, mirrored to GPU via FlaxDDGIData.
struct DDGICascadeDesc
{
  glm::vec3 snappedOrigin{0.0f};   // origin snapped to probe spacing grid
  glm::vec3 blendOrigin{0.0f};     // blend origin (unsnapped, for blending)
  glm::ivec3 scrollOffset{0,0,0};  // current scroll offset in probe units
  float probeSpacing{1.5f};        // world-space probe distance
  uint32_t probeCountX{0};
  uint32_t probeCountY{0};
  uint32_t probeCountZ{0};
  bool enabled{false};
};

// --- Probe state encoding constants (FGI-043, matches Flax DDGI.hlsl) ---
namespace probe_state
{
  static constexpr float kInactive  = 0.0f;
  static constexpr float kActivated = 1.0f;
  static constexpr float kActive    = 2.0f;

  static constexpr float kAttentionMin = 0.02f;
  static constexpr float kAttentionMax = 0.98f;

  // Encode: offset (world-space float3 relocation), state (0/1/2), attention (0-1)
  inline glm::vec4 encodeProbeData(const glm::vec3& offset, uint32_t state, float attention)
  {
    float a = glm::clamp(attention, 0.0f, 1.0f) * 2.0f - 1.0f; // [0,1] -> [-1,1]
    if (state == 0) a = -1.0f;
    else if (state == 1) a = 1.0f;
    return glm::vec4(offset, a);
  }

  inline uint32_t decodeProbeState(float w)
  {
    if (w <= -1.0f) return 0;  // inactive
    if (w >= 1.0f)  return 1;  // activated
    return 2;                   // active
  }

  inline float decodeProbeAttention(float w)
  {
    if (w <= -1.0f) return 0.0f;
    if (w >=  1.0f) return 1.0f;
    return w * 0.5f + 0.5f;
  }

  inline glm::vec3 decodeProbeOffset(const glm::vec4& probeData, float probeSpacing)
  {
    return glm::vec3(probeData) * probeSpacing;
  }

  // Fallback coords encoding (maps integer coords into [0,~1] float range)
  inline glm::vec3 encodeFallbackCoords(const glm::uvec3& coords)
  {
    return (glm::vec3(coords) + 1.0f) / 128.0f;
  }

  inline glm::uvec3 decodeFallbackCoords(const glm::vec3& data)
  {
    return glm::uvec3(data * 128.0f - 1.0f);
  }

  inline bool fallbackCoordsValid(const glm::vec3& data)
  {
    return glm::length(data) > 0.0f;
  }
}

// --- Flax-style DDGI resource owner ---
class FlaxDDGIResources
{
public:
  // Flax defaults (from DDGI.hlsl)
  static constexpr uint32_t kProbeResolutionIrradiance = 6u;  // texels per probe (excl. padding)
  static constexpr uint32_t kProbeResolutionDistance   = 14u;
  static constexpr uint32_t kProbeBorderTexels         = 2u;  // 1px each side

  [[nodiscard]] bool isInitialized() const { return m_device != nullptr; }

  // Allocate all Flax DDGI resources. Safe to re-call (deinits first).
  void init(rhi::Device& device, const DDGIConfig& config,
            uint32_t cascadeCount, const glm::uvec3& probesPerCascade);

  void deinit();

  // Compute cascade descriptors from config + camera
  static void computeCascades(const DDGIConfig& config,
                               const glm::vec3& cameraPosition,
                               uint32_t cascadeCount,
                               const glm::uvec3& probesPerCascade,
                               std::vector<DDGICascadeDesc>& outCascades);

  // Compute max probes per cascade from config (bounded by max texture size)
  [[nodiscard]] static glm::uvec3 computeProbesPerCascade(const DDGIConfig& config,
                                                           uint32_t cascadeIndex,
                                                           uint32_t totalCascades);

  // --- Accessors ---
  [[nodiscard]] rhi::TextureHandle getProbesTrace() const { return m_probesTrace; }
  [[nodiscard]] rhi::TextureHandle getProbesData() const { return m_probesData; }
  [[nodiscard]] rhi::TextureHandle getProbesIrradiance() const { return m_probesIrradiance; }
  [[nodiscard]] rhi::TextureHandle getProbesDistance() const { return m_probesDistance; }
  [[nodiscard]] rhi::BufferHandle getActiveProbes() const { return m_activeProbes; }
  [[nodiscard]] rhi::BufferHandle getUpdateProbesInitArgs() const { return m_updateProbesInitArgs; }

  [[nodiscard]] uint32_t getActiveProbeBatchSize() const { return m_activeProbeBatchSize; }
  [[nodiscard]] uint32_t getCascadeCount() const { return m_cascadeCount; }
  [[nodiscard]] const glm::uvec3& getProbesPerCascade() const { return m_probesPerCascade; }
  [[nodiscard]] rhi::Extent2D getIrradianceAtlasExtent() const { return m_irradianceAtlasExtent; }
  [[nodiscard]] rhi::Extent2D getDistanceAtlasExtent() const { return m_distanceAtlasExtent; }
  [[nodiscard]] rhi::Extent2D getTraceExtent() const { return m_traceExtent; }

  // Estimated memory in bytes
  [[nodiscard]] uint64_t getTotalMemoryBytes() const;

private:
  rhi::Device* m_device{nullptr};
  uint32_t m_cascadeCount{0};
  uint32_t m_activeProbeBatchSize{256};  // probes per batch
  uint32_t m_raysPerProbe{64};
  glm::uvec3 m_probesPerCascade{0u};

  rhi::Extent2D m_irradianceAtlasExtent{};
  rhi::Extent2D m_distanceAtlasExtent{};
  rhi::Extent2D m_traceExtent{};

  rhi::TextureHandle m_probesTrace{};
  rhi::TextureHandle m_probesData{};
  rhi::TextureHandle m_probesIrradiance{};
  rhi::TextureHandle m_probesDistance{};
  rhi::BufferHandle  m_activeProbes{};
  rhi::BufferHandle  m_updateProbesInitArgs{};
};

} // namespace demo
