#include "FlaxDDGIResources.h"
#include "../common/logger.h"

#include <algorithm>
#include <cstring>

namespace demo
{

void FlaxDDGIResources::init(rhi::Device& device, const DDGIConfig& config,
                              uint32_t cascadeCount, const glm::uvec3& probesPerCascade)
{
  if (cascadeCount == 0 || probesPerCascade.x == 0) return;

  // Deinit any previous resources
  deinit();

  m_device = &device;
  m_cascadeCount = std::min(cascadeCount, 4u);
  m_probesPerCascade = probesPerCascade;
  m_raysPerProbe = config.raysPerProbe;

  const uint32_t totalProbes = probesPerCascade.x * probesPerCascade.y * probesPerCascade.z;
  const uint32_t probesPerPlane = probesPerCascade.x * probesPerCascade.z;
  m_activeProbeBatchSize = std::min(totalProbes, 256u);

  // --- ProbesTrace: 2D (raysPerProbe x activeProbeBatchSize) ---
  {
    rhi::TextureDesc desc{};
    desc.dimension = rhi::TextureDimension::e2D;
    desc.format = rhi::TextureFormat::rgba16Sfloat;
    desc.usage = rhi::TextureUsageFlags::storage | rhi::TextureUsageFlags::sampled;
    desc.extent = {m_raysPerProbe, m_activeProbeBatchSize, 1};
    desc.mipLevels = 1;
    desc.arrayLayers = 1;
    desc.memoryUsage = rhi::MemoryUsage::gpuOnly;
    desc.debugName = "FlaxDDGI.ProbesTrace";
    m_probesTrace = device.createTexture(desc);
    m_traceExtent = {m_raysPerProbe, m_activeProbeBatchSize};
  }

  // --- ProbesData: per-probe offset+state+attention ---
  // Atlas layout: width = probesPerCascade.x * probesPerCascade.y (planes stacked X),
  //               height = probesPerCascade.z * cascadeCount
  // Each texel = one probe's float4 (offset.xyz, state+attention)
  {
    const uint32_t dataWidth = probesPerCascade.x * probesPerCascade.y;
    const uint32_t dataHeight = probesPerCascade.z * m_cascadeCount;

    rhi::TextureDesc desc{};
    desc.dimension = rhi::TextureDimension::e2D;
    // Prefer rgba8Snorm for compact storage; fall back to rgba16Sfloat
    desc.format = rhi::TextureFormat::rgba8Snorm;
    desc.usage = rhi::TextureUsageFlags::storage | rhi::TextureUsageFlags::sampled;
    desc.extent = {dataWidth, dataHeight, 1};
    desc.mipLevels = 1;
    desc.arrayLayers = 1;
    desc.memoryUsage = rhi::MemoryUsage::gpuOnly;
    desc.debugName = "FlaxDDGI.ProbesData";
    m_probesData = device.createTexture(desc);
  }

  // --- ProbesIrradiance: octahedral irradiance atlas ---
  // Tile size = kProbeResolutionIrradiance + border (1px each side = 2px total)
  {
    const uint32_t tileSize = kProbeResolutionIrradiance + kProbeBorderTexels;
    const uint32_t atlasWidth = probesPerCascade.x * probesPerCascade.y * tileSize;
    const uint32_t atlasHeight = probesPerCascade.z * m_cascadeCount * tileSize;

    rhi::TextureDesc desc{};
    desc.dimension = rhi::TextureDimension::e2D;
    desc.format = rhi::TextureFormat::rgba16Sfloat;
    desc.usage = rhi::TextureUsageFlags::storage | rhi::TextureUsageFlags::sampled;
    desc.extent = {atlasWidth, atlasHeight, 1};
    desc.mipLevels = 1;
    desc.arrayLayers = 1;
    desc.memoryUsage = rhi::MemoryUsage::gpuOnly;
    desc.debugName = "FlaxDDGI.ProbesIrradiance";
    m_probesIrradiance = device.createTexture(desc);
    m_irradianceAtlasExtent = {atlasWidth, atlasHeight};
  }

  // --- ProbesDistance: octahedral distance atlas ---
  {
    const uint32_t tileSize = kProbeResolutionDistance + kProbeBorderTexels;
    const uint32_t atlasWidth = probesPerCascade.x * probesPerCascade.y * tileSize;
    const uint32_t atlasHeight = probesPerCascade.z * m_cascadeCount * tileSize;

    rhi::TextureDesc desc{};
    desc.dimension = rhi::TextureDimension::e2D;
    desc.format = rhi::TextureFormat::rg16Sfloat;
    desc.usage = rhi::TextureUsageFlags::storage | rhi::TextureUsageFlags::sampled;
    desc.extent = {atlasWidth, atlasHeight, 1};
    desc.mipLevels = 1;
    desc.arrayLayers = 1;
    desc.memoryUsage = rhi::MemoryUsage::gpuOnly;
    desc.debugName = "FlaxDDGI.ProbesDistance";
    m_probesDistance = device.createTexture(desc);
    m_distanceAtlasExtent = {atlasWidth, atlasHeight};
  }

  // --- ActiveProbes: counter at element 0, indices follow ---
  {
    rhi::BufferDesc desc{};
    desc.size = (totalProbes + 1) * sizeof(uint32_t); // counter + indices
    desc.usage = rhi::BufferUsageFlags::storage | rhi::BufferUsageFlags::transferDst;
    desc.memoryUsage = rhi::MemoryUsage::gpuOnly;
    desc.allowIndirectArgument = true;
    desc.debugName = "FlaxDDGI.ActiveProbes";
    m_activeProbes = device.createBuffer(desc);
  }

  // --- UpdateProbesInitArgs: indirect dispatch arguments ---
  {
    rhi::BufferDesc desc{};
    desc.size = 4 * sizeof(uint32_t); // (groupCountX, groupCountY, groupCountZ) + padding
    desc.usage = rhi::BufferUsageFlags::storage | rhi::BufferUsageFlags::indirect;
    desc.memoryUsage = rhi::MemoryUsage::gpuOnly;
    desc.allowIndirectArgument = true;
    desc.debugName = "FlaxDDGI.UpdateProbesInitArgs";
    m_updateProbesInitArgs = device.createBuffer(desc);
  }

  LOGI("FlaxDDGIResources: allocated %u cascades, %u probes/cascade, irradiance %ux%u, distance %ux%u, trace %ux%u, memory ~%llu KiB",
       m_cascadeCount, totalProbes,
       m_irradianceAtlasExtent.width, m_irradianceAtlasExtent.height,
       m_distanceAtlasExtent.width, m_distanceAtlasExtent.height,
       m_traceExtent.width, m_traceExtent.height,
       static_cast<unsigned long long>(getTotalMemoryBytes() / 1024));
}

void FlaxDDGIResources::deinit()
{
  if (!m_device) return;

  m_device->destroyImage(m_probesTrace);
  m_device->destroyImage(m_probesData);
  m_device->destroyImage(m_probesIrradiance);
  m_device->destroyImage(m_probesDistance);
  m_device->destroyBuffer(m_activeProbes);
  m_device->destroyBuffer(m_updateProbesInitArgs);

  m_probesTrace = {};
  m_probesData = {};
  m_probesIrradiance = {};
  m_probesDistance = {};
  m_activeProbes = {};
  m_updateProbesInitArgs = {};
  m_device = nullptr;
  m_cascadeCount = 0;
}

uint64_t FlaxDDGIResources::getTotalMemoryBytes() const
{
  uint64_t total = 0;
  // Trace: raysPerProbe * batchSize * 8 bytes (rgba16f)
  total += static_cast<uint64_t>(m_traceExtent.width) * m_traceExtent.height * 8;
  // Data: probesPerCascade^3 * cascadeCount * 4 bytes (rgba8Snorm) or 8 (rgba16f)
  const uint32_t probeCount = m_probesPerCascade.x * m_probesPerCascade.y * m_probesPerCascade.z;
  total += static_cast<uint64_t>(probeCount) * m_cascadeCount * 8; // conservative: rgba16f fallback
  // Irradiance: atlas area * 8 bytes (rgba16f)
  total += static_cast<uint64_t>(m_irradianceAtlasExtent.width) * m_irradianceAtlasExtent.height * 8;
  // Distance: atlas area * 4 bytes (rg16f)
  total += static_cast<uint64_t>(m_distanceAtlasExtent.width) * m_distanceAtlasExtent.height * 4;
  // ActiveProbes: (probeCount * cascadeCount + 1) * 4
  total += (static_cast<uint64_t>(probeCount) * m_cascadeCount + 1) * 4;
  // InitArgs: 4 * 4 = 16
  total += 16;
  return total;
}

void FlaxDDGIResources::computeCascades(const DDGIConfig& config,
                                         const glm::vec3& cameraPosition,
                                         uint32_t cascadeCount,
                                         const glm::uvec3& probesPerCascade,
                                         std::vector<DDGICascadeDesc>& outCascades)
{
  outCascades.resize(cascadeCount);

  for (uint32_t c = 0; c < cascadeCount; ++c)
  {
    DDGICascadeDesc& cascade = outCascades[c];
    cascade.enabled = true;

    // Each cascade covers a progressively larger area
    float spacingMultiplier = 1.0f;
    if (c > 0) spacingMultiplier = 2.0f; // outer cascades double spacing
    if (c > 1) spacingMultiplier = 4.0f;
    if (c > 2) spacingMultiplier = 8.0f;

    cascade.probeSpacing = config.probeSpacing * spacingMultiplier;

    // Snap origin to probe spacing grid
    cascade.snappedOrigin = glm::floor(cameraPosition / cascade.probeSpacing) * cascade.probeSpacing;
    cascade.blendOrigin = cameraPosition;

    cascade.probeCountX = probesPerCascade.x;
    cascade.probeCountY = probesPerCascade.y;
    cascade.probeCountZ = probesPerCascade.z;
  }
}

glm::uvec3 FlaxDDGIResources::computeProbesPerCascade(const DDGIConfig& config,
                                                       uint32_t cascadeIndex,
                                                       uint32_t totalCascades)
{
  // Inner cascades get higher density
  float spacingMultiplier = 1.0f;
  if (cascadeIndex == 1) spacingMultiplier = 2.0f;
  if (cascadeIndex == 2) spacingMultiplier = 4.0f;
  if (cascadeIndex == 3) spacingMultiplier = 8.0f;

  float spacing = config.probeSpacing * spacingMultiplier;
  float extent = config.giDistance / spacingMultiplier; // outer cascades cover larger area

  uint32_t countPerAxis = static_cast<uint32_t>(std::ceil(extent / spacing)) * 2 + 1;
  countPerAxis = std::min(countPerAxis, 32u); // hard cap at 32^3 = 32768 probes
  countPerAxis = std::max(countPerAxis, 4u);

  return glm::uvec3(countPerAxis);
}

} // namespace demo
