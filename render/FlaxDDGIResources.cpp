#include "FlaxDDGIResources.h"
#include "../common/logger.h"

#include <algorithm>
#include <cstring>
#include <numeric>

namespace demo
{

void FlaxDDGIResources::init(rhi::Device& device, const DDGIConfig& config,
                              const std::vector<glm::uvec3>& probesPerCascade)
{
  const uint32_t cascadeCount = static_cast<uint32_t>(probesPerCascade.size());
  if (cascadeCount == 0 || cascadeCount > kMaxCascades) return;

  // Validate all cascades have non-zero probe counts
  for (uint32_t c = 0; c < cascadeCount; ++c) {
    if (probesPerCascade[c].x == 0 || probesPerCascade[c].y == 0 || probesPerCascade[c].z == 0)
      return;
  }

  deinit();

  m_device = &device;
  m_cascadeCount = cascadeCount;
  m_probesPerCascade = probesPerCascade;
  m_raysPerProbe = config.raysPerProbe;

  // Compute per-cascade totals and max
  m_maxProbesPerCascade = 0;
  for (uint32_t c = 0; c < cascadeCount; ++c) {
    const uint32_t n = probesPerCascade[c].x * probesPerCascade[c].y * probesPerCascade[c].z;
    m_maxProbesPerCascade = std::max(m_maxProbesPerCascade, n);
  }

  // Batch size: each cascade processes probes in batches. Max batch = 256 (GPU warp-friendly).
  static constexpr uint32_t kBatchSize = 256u;
  m_maxBatchesPerCascade = (m_maxProbesPerCascade + kBatchSize - 1u) / kBatchSize;

  // --- RGBA8_SNORM capability check ---
  m_usesSNORM = device.isFormatSupported(rhi::TextureFormat::rgba8Snorm,
                                          rhi::FormatFeatureFlag::storageImage);
  const rhi::TextureFormat probeDataFormat = m_usesSNORM
    ? rhi::TextureFormat::rgba8Snorm : rhi::TextureFormat::rgba16Sfloat;
  const uint32_t probeDataBytes = m_usesSNORM ? 4u : 8u;

  // --- ProbesTrace: 2D (raysPerProbe x maxProbesPerCascade) ---
  // Sized for the largest cascade; batch dispatch selects the active sub-rectangle.
  {
    rhi::TextureDesc desc{};
    desc.dimension = rhi::TextureDimension::e2D;
    desc.format = rhi::TextureFormat::rgba16Sfloat;
    desc.usage = rhi::TextureUsageFlags::storage | rhi::TextureUsageFlags::sampled
               | rhi::TextureUsageFlags::transferDst;
    desc.extent = {m_raysPerProbe, m_maxProbesPerCascade, 1};
    desc.mipLevels = 1;
    desc.arrayLayers = 1;
    desc.memoryUsage = rhi::MemoryUsage::gpuOnly;
    desc.debugName = "FlaxDDGI.ProbesTrace";
    m_probesTrace = device.createTexture(desc);
    m_traceExtent = {m_raysPerProbe, m_maxProbesPerCascade};
  }

  // --- ProbesData: per-probe offset+state+attention ---
  // Layout: width = max(probesPerCascade.x * probesPerCascade.y) across cascades
  //         height = sum(probesPerCascade[c].z) over all cascades
  // Each texel = one probe's float4 (offset.xyz, state+attention)
  {
    uint32_t maxProbesPerPlane = 0;
    uint32_t totalZ = 0;
    for (uint32_t c = 0; c < cascadeCount; ++c) {
      maxProbesPerPlane = std::max(maxProbesPerPlane,
                                    probesPerCascade[c].x * probesPerCascade[c].y);
      totalZ += probesPerCascade[c].z;
    }

    rhi::TextureDesc desc{};
    desc.dimension = rhi::TextureDimension::e2D;
    desc.format = probeDataFormat;
    desc.usage = rhi::TextureUsageFlags::storage | rhi::TextureUsageFlags::sampled
               | rhi::TextureUsageFlags::transferDst;
    desc.extent = {maxProbesPerPlane, totalZ, 1};
    desc.mipLevels = 1;
    desc.arrayLayers = 1;
    desc.memoryUsage = rhi::MemoryUsage::gpuOnly;
    desc.debugName = "FlaxDDGI.ProbesData";
    m_probesData = device.createTexture(desc);
    m_dataExtent = {maxProbesPerPlane, totalZ};
    if (!m_usesSNORM) {
      LOGW("FlaxDDGI: rgba8Snorm storage-image not supported, falling back to rgba16Sfloat "
           "(ProbesData will use 2x memory)");
    }
  }

  // --- ProbesIrradiance: octahedral irradiance atlas (double-buffered) ---
  // Tile size = kProbeResolutionIrradiance + border
  {
    const uint32_t tileSize = kProbeResolutionIrradiance + kProbeBorderTexels;
    uint32_t maxAtlasWidth = 0;
    uint32_t totalAtlasHeight = 0;
    for (uint32_t c = 0; c < cascadeCount; ++c) {
      const uint32_t w = probesPerCascade[c].x * probesPerCascade[c].y * tileSize;
      const uint32_t h = probesPerCascade[c].z * tileSize;
      maxAtlasWidth = std::max(maxAtlasWidth, w);
      totalAtlasHeight += h;
    }

    rhi::TextureDesc desc{};
    desc.dimension = rhi::TextureDimension::e2D;
    desc.format = rhi::TextureFormat::rgba16Sfloat;
    desc.usage = rhi::TextureUsageFlags::storage | rhi::TextureUsageFlags::sampled
               | rhi::TextureUsageFlags::transferDst;
    desc.extent = {maxAtlasWidth, totalAtlasHeight, 1};
    desc.mipLevels = 1;
    desc.arrayLayers = 1;
    desc.memoryUsage = rhi::MemoryUsage::gpuOnly;
    desc.debugName = "FlaxDDGI.ProbesIrradiance";
    m_probesIrradiance = device.createTexture(desc);

    desc.debugName = "FlaxDDGI.ProbesIrradianceHistory";
    m_probesIrradianceHistory = device.createTexture(desc);

    m_irradianceAtlasExtent = {maxAtlasWidth, totalAtlasHeight};
  }

  // --- ProbesDistance: octahedral distance atlas (double-buffered) ---
  {
    const uint32_t tileSize = kProbeResolutionDistance + kProbeBorderTexels;
    uint32_t maxAtlasWidth = 0;
    uint32_t totalAtlasHeight = 0;
    for (uint32_t c = 0; c < cascadeCount; ++c) {
      const uint32_t w = probesPerCascade[c].x * probesPerCascade[c].y * tileSize;
      const uint32_t h = probesPerCascade[c].z * tileSize;
      maxAtlasWidth = std::max(maxAtlasWidth, w);
      totalAtlasHeight += h;
    }

    rhi::TextureDesc desc{};
    desc.dimension = rhi::TextureDimension::e2D;
    desc.format = rhi::TextureFormat::rg16Sfloat;
    desc.usage = rhi::TextureUsageFlags::storage | rhi::TextureUsageFlags::sampled
               | rhi::TextureUsageFlags::transferDst;
    desc.extent = {maxAtlasWidth, totalAtlasHeight, 1};
    desc.mipLevels = 1;
    desc.arrayLayers = 1;
    desc.memoryUsage = rhi::MemoryUsage::gpuOnly;
    desc.debugName = "FlaxDDGI.ProbesDistance";
    m_probesDistance = device.createTexture(desc);

    desc.debugName = "FlaxDDGI.ProbesDistanceHistory";
    m_probesDistanceHistory = device.createTexture(desc);

    m_distanceAtlasExtent = {maxAtlasWidth, totalAtlasHeight};
  }

  // --- ActiveProbes: counters + disjoint priority/regular compact lists ---
  m_activeProbes.resize(cascadeCount);
  for (uint32_t c = 0; c < cascadeCount; ++c) {
    const uint32_t probeCount = probesPerCascade[c].x * probesPerCascade[c].y * probesPerCascade[c].z;
    rhi::BufferDesc desc{};
    // [0] active count, [1] priority count, [2] regular count,
    // [3] dynamically allocated update count; then one probe-index array for
    // each class, each with probeCount capacity.
    desc.size = static_cast<uint64_t>(4u + 2u * probeCount) * sizeof(uint32_t);
    desc.usage = rhi::BufferUsageFlags::storage | rhi::BufferUsageFlags::transferDst
               | rhi::BufferUsageFlags::transferSrc;
    desc.memoryUsage = rhi::MemoryUsage::gpuOnly;
    desc.allowIndirectArgument = true;
    desc.debugName = "FlaxDDGI.ActiveProbes";
    m_activeProbes[c] = device.createBuffer(desc);
  }

  // --- UpdateProbesInitArgs: [cascade][pass] indirect args + slack scratch ---
  // Each entry: (groupCountX, groupCountY, groupCountZ) = 3 × uint32_t
  // Total: cascadeCount × 3 passes × 3 uints × 4 bytes
  {
    const uint32_t passCount = static_cast<uint32_t>(IndirectPass::Count);
    rhi::BufferDesc desc{};
    desc.size = (static_cast<uint64_t>(cascadeCount) * passCount * 3u + 1u)
              * sizeof(uint32_t);
    desc.usage = rhi::BufferUsageFlags::storage | rhi::BufferUsageFlags::indirect
               | rhi::BufferUsageFlags::transferSrc | rhi::BufferUsageFlags::transferDst;
    desc.memoryUsage = rhi::MemoryUsage::gpuOnly;
    desc.allowIndirectArgument = true;
    desc.debugName = "FlaxDDGI.UpdateProbesInitArgs";
    m_updateProbesInitArgs = device.createBuffer(desc);
  }

  LOGI("FlaxDDGIResources R1: %u cascades, max %u probes/cascade, %u batches/cascade, "
       "irradiance %ux%u (double-buffered), distance %ux%u (double-buffered), "
       "trace %ux%u, data %ux%u (%s), memory ~%llu KiB",
       m_cascadeCount, m_maxProbesPerCascade, m_maxBatchesPerCascade,
       m_irradianceAtlasExtent.width, m_irradianceAtlasExtent.height,
       m_distanceAtlasExtent.width, m_distanceAtlasExtent.height,
       m_traceExtent.width, m_traceExtent.height,
       m_dataExtent.width, m_dataExtent.height,
       m_usesSNORM ? "rgba8Snorm" : "rgba16Sfloat",
       static_cast<unsigned long long>(getTotalMemoryBytes() / 1024));
}

void FlaxDDGIResources::deinit()
{
  if (!m_device) return;

  m_device->destroyTexture(m_probesTrace);
  m_device->destroyTexture(m_probesData);
  m_device->destroyTexture(m_probesIrradiance);
  m_device->destroyTexture(m_probesIrradianceHistory);
  m_device->destroyTexture(m_probesDistance);
  m_device->destroyTexture(m_probesDistanceHistory);
  for (auto& buf : m_activeProbes) {
    m_device->destroyBuffer(buf);
  }
  m_device->destroyBuffer(m_updateProbesInitArgs);

  m_probesTrace = {};
  m_probesData = {};
  m_probesIrradiance = {};
  m_probesIrradianceHistory = {};
  m_probesDistance = {};
  m_probesDistanceHistory = {};
  m_activeProbes.clear();
  m_updateProbesInitArgs = {};
  m_device = nullptr;
  m_cascadeCount = 0;
  m_maxBatchesPerCascade = 1;
  m_maxProbesPerCascade = 0;
  m_probesPerCascade.clear();
}

uint64_t FlaxDDGIResources::getTotalMemoryBytes() const
{
  uint64_t total = 0;
  // Trace: raysPerProbe * maxProbesPerCascade * 8 bytes (rgba16f)
  total += static_cast<uint64_t>(m_traceExtent.width) * m_traceExtent.height * 8;
  // Data: maxProbesPerPlane * totalZ * bytesPerTexel
  total += static_cast<uint64_t>(m_dataExtent.width) * m_dataExtent.height
         * (m_usesSNORM ? 4u : 8u);
  // Irradiance ×2 (double-buffered): atlas area * 8 bytes
  total += static_cast<uint64_t>(m_irradianceAtlasExtent.width)
         * m_irradianceAtlasExtent.height * 8 * 2;
  // Distance ×2 (double-buffered): atlas area * 4 bytes
  total += static_cast<uint64_t>(m_distanceAtlasExtent.width)
         * m_distanceAtlasExtent.height * 4 * 2;
  // ActiveProbes per cascade
  for (size_t c = 0; c < m_probesPerCascade.size(); ++c) {
    const uint32_t n = m_probesPerCascade[c].x * m_probesPerCascade[c].y * m_probesPerCascade[c].z;
    total += (static_cast<uint64_t>(4u) + 2u * n) * 4;
  }
  // InitArgs plus one shared slack counter.
  total += (static_cast<uint64_t>(m_cascadeCount)
         * static_cast<uint32_t>(IndirectPass::Count) * 3u + 1u) * 4u;
  return total;
}

bool FlaxDDGIResources::computeCascades(const DDGIConfig& config,
                                        const glm::vec3& cameraPosition,
                                        const glm::vec3& coverageCenter,
                                        const std::vector<glm::uvec3>& probesPerCascade,
                                        std::vector<DDGICascadeDesc>& outCascades)
{
  const uint32_t cascadeCount = static_cast<uint32_t>(probesPerCascade.size());
  const std::vector<DDGICascadeDesc> previous = outCascades;
  outCascades.resize(cascadeCount);

  bool historyInvalidated =
    !previous.empty() && previous.size() != cascadeCount;

  for (uint32_t c = 0; c < cascadeCount; ++c)
  {
    DDGICascadeDesc& cascade = outCascades[c];
    cascade.enabled = true;
    cascade.probeCountX = probesPerCascade[c].x;
    cascade.probeCountY = probesPerCascade[c].y;
    cascade.probeCountZ = probesPerCascade[c].z;

    const float spacingMultiplier = static_cast<float>(1u << c);
    cascade.probeSpacing = config.probeSpacing * spacingMultiplier;
    cascade.snappedOrigin =
      glm::round(coverageCenter / cascade.probeSpacing) * cascade.probeSpacing;
    cascade.blendOrigin = cameraPosition;

    const bool hasPrevious = c < previous.size() && previous[c].enabled;
    const float spacingTolerance =
      std::max(cascade.probeSpacing, 1.0f) * 1.0e-4f;
    const bool spacingChanged = hasPrevious
      && std::abs(previous[c].probeSpacing - cascade.probeSpacing)
        > spacingTolerance;
    const glm::vec3 deltaCells = hasPrevious
      ? (cascade.snappedOrigin - previous[c].snappedOrigin)
          / cascade.probeSpacing
      : glm::vec3(0.0f);
    const glm::vec3 roundedDelta = glm::round(deltaCells);
    const bool originGridAligned = glm::all(glm::lessThanEqual(
      glm::abs(deltaCells - roundedDelta), glm::vec3(1.0e-3f)));
    historyInvalidated |= spacingChanged || (hasPrevious && !originGridAligned);

    // The origin is the current logical grid center. Texture scrolling is a
    // separate cumulative ring offset; world positions must never include it.
    if (hasPrevious && !spacingChanged && originGridAligned)
    {
      cascade.scrollDelta = glm::ivec3(roundedDelta);
      const glm::ivec3 counts(
        static_cast<int>(cascade.probeCountX),
        static_cast<int>(cascade.probeCountY),
        static_cast<int>(cascade.probeCountZ));
      glm::ivec3 cumulative = previous[c].scrollOffset + cascade.scrollDelta;
      cumulative = (cumulative % counts + counts) % counts;
      cascade.scrollOffset = cumulative;
    }
    else
    {
      cascade.scrollOffset = glm::ivec3(0, 0, 0);
      cascade.scrollDelta = glm::ivec3(0, 0, 0);
    }
  }
  return historyInvalidated;
}

glm::uvec3 FlaxDDGIResources::computeProbesPerCascade(const DDGIConfig& config,
                                                       uint32_t cascadeIndex,
                                                       uint32_t totalCascades)
{
  // All cascades share the same probe grid resolution (matches Flax DDGI.hlsl).
  // Per-cascade differentiation is via probesOriginAndSpacing[c] — same grid,
  // different world-space coverage. The inner cascade's spacing determines
  // the grid resolution; outer cascades cover more area at coarser spacing.
  (void)cascadeIndex;
  (void)totalCascades;

  const float spacing = config.probeSpacing;
  const float extent = config.giDistance;

  uint32_t countPerAxis = static_cast<uint32_t>(std::ceil(extent / spacing)) * 2 + 1;
  countPerAxis = std::min(countPerAxis, 32u); // hard cap at 32^3 = 32768 probes
  countPerAxis = std::max(countPerAxis, 4u);

  return glm::uvec3(countPerAxis, countPerAxis / 2 + 1, countPerAxis);
}

} // namespace demo
