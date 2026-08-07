#include "GlobalSurfaceAtlasPass.h"
#include "../../common/logger.h"

#include <cstring>

namespace demo
{

// Chunk count constant (must match GlobalSurfaceAtlas.hlsl / shader_io.h)
static constexpr uint32_t kChunksResolution = 40;

void GlobalSurfaceAtlasPass::init(rhi::Device& device, const DDGIConfig& config)
{
  if (!config.enableGlobalSurfaceAtlas || !config.enabled) return;

  deinit();
  m_device = &device;
  m_resolution = config.surfaceAtlasResolution;
  m_tileResolution = config.surfaceAtlasTileResolution;
  m_tilePadding = 1;

  const uint32_t res = m_resolution;

  // --- Atlas textures ---
  // Depth: D16 depth for tile rasterization
  {
    rhi::TextureDesc desc{};
    desc.dimension = rhi::TextureDimension::e2D;
    desc.format = rhi::TextureFormat::d16Unorm;
    desc.usage = rhi::TextureUsageFlags::depthAttachment | rhi::TextureUsageFlags::sampled;
    desc.extent = {res, res, 1};
    desc.mipLevels = 1;
    desc.memoryUsage = rhi::MemoryUsage::gpuOnly;
    desc.debugName = "GlobalSurfaceAtlas.Depth";
    m_atlasDepth = device.createTexture(desc);
  }

  // Emissive: HDR color (r11g11b10 float preferred, fallback rgba16f)
  {
    rhi::TextureDesc desc{};
    desc.dimension = rhi::TextureDimension::e2D;
    desc.format = rhi::TextureFormat::r11g11b10Ufloat;
    desc.usage = rhi::TextureUsageFlags::colorAttachment | rhi::TextureUsageFlags::sampled;
    desc.extent = {res, res, 1};
    desc.mipLevels = 1;
    desc.memoryUsage = rhi::MemoryUsage::gpuOnly;
    desc.debugName = "GlobalSurfaceAtlas.Emissive";
    m_atlasEmissive = device.createTexture(desc);
  }

  // GBuffer0: baseColor.rgb + AO (rgba8Unorm)
  {
    rhi::TextureDesc desc{};
    desc.dimension = rhi::TextureDimension::e2D;
    desc.format = rhi::TextureFormat::rgba8Unorm;
    desc.usage = rhi::TextureUsageFlags::colorAttachment | rhi::TextureUsageFlags::sampled;
    desc.extent = {res, res, 1};
    desc.mipLevels = 1;
    desc.memoryUsage = rhi::MemoryUsage::gpuOnly;
    desc.debugName = "GlobalSurfaceAtlas.GBuffer0";
    m_atlasGBuffer0 = device.createTexture(desc);
  }

  // GBuffer1: normal.xyz + shading model (rgba16Sfloat)
  {
    rhi::TextureDesc desc{};
    desc.dimension = rhi::TextureDimension::e2D;
    desc.format = rhi::TextureFormat::rgba16Sfloat;
    desc.usage = rhi::TextureUsageFlags::colorAttachment | rhi::TextureUsageFlags::sampled;
    desc.extent = {res, res, 1};
    desc.mipLevels = 1;
    desc.memoryUsage = rhi::MemoryUsage::gpuOnly;
    desc.debugName = "GlobalSurfaceAtlas.GBuffer1";
    m_atlasGBuffer1 = device.createTexture(desc);
  }

  // Lighting: HDR direct/indirect lighting (rgba16Sfloat)
  {
    rhi::TextureDesc desc{};
    desc.dimension = rhi::TextureDimension::e2D;
    desc.format = rhi::TextureFormat::rgba16Sfloat;
    desc.usage = rhi::TextureUsageFlags::colorAttachment | rhi::TextureUsageFlags::sampled |
                 rhi::TextureUsageFlags::storage;
    desc.extent = {res, res, 1};
    desc.mipLevels = 1;
    desc.memoryUsage = rhi::MemoryUsage::gpuOnly;
    desc.debugName = "GlobalSurfaceAtlas.Lighting";
    m_atlasLighting = device.createTexture(desc);
  }

  // --- Storage buffers ---
  // Chunks buffer: kChunksResolution^3 * 4 bytes (object counter per chunk)
  {
    rhi::BufferDesc desc{};
    desc.size = static_cast<uint64_t>(kChunksResolution) * kChunksResolution * kChunksResolution * sizeof(uint32_t);
    desc.usage = rhi::BufferUsageFlags::storage | rhi::BufferUsageFlags::transferDst;
    desc.memoryUsage = rhi::MemoryUsage::gpuOnly;
    desc.debugName = "GlobalSurfaceAtlas.Chunks";
    m_chunksBuffer = device.createBuffer(desc);
  }

  // Culled objects: conservative 4096 * 4 bytes
  {
    rhi::BufferDesc desc{};
    desc.size = 4096 * sizeof(uint32_t);
    desc.usage = rhi::BufferUsageFlags::storage | rhi::BufferUsageFlags::transferDst;
    desc.memoryUsage = rhi::MemoryUsage::gpuOnly;
    desc.debugName = "GlobalSurfaceAtlas.CulledObjects";
    m_culledObjectsBuffer = device.createBuffer(desc);
  }

  // Objects buffer: conservative 4096 * 36 * 16 bytes (~2.3 MiB)
  {
    rhi::BufferDesc desc{};
    desc.size = 4096ull * GlobalSurfaceAtlasDataManager::kMaxFloat4sPerObject * sizeof(float) * 4;
    desc.usage = rhi::BufferUsageFlags::storage | rhi::BufferUsageFlags::transferDst;
    desc.memoryUsage = rhi::MemoryUsage::gpuOnly;
    desc.debugName = "GlobalSurfaceAtlas.Objects";
    m_objectsBuffer = device.createBuffer(desc);
  }

  // Objects list: 4096 * 4 bytes
  {
    rhi::BufferDesc desc{};
    desc.size = 4096 * sizeof(uint32_t);
    desc.usage = rhi::BufferUsageFlags::storage | rhi::BufferUsageFlags::transferDst;
    desc.memoryUsage = rhi::MemoryUsage::gpuOnly;
    desc.debugName = "GlobalSurfaceAtlas.ObjectsList";
    m_objectsListBuffer = device.createBuffer(desc);
  }

  // Initialize the CPU data manager
  m_dataManager.init(m_resolution, m_tileResolution, m_tilePadding);

  LOGI("GlobalSurfaceAtlasPass: allocated %ux%u atlas (depth+emissive+GB0+GB1+lighting), ~%llu KiB",
       res, res, static_cast<unsigned long long>(getTotalMemoryBytes() / 1024));
}

void GlobalSurfaceAtlasPass::deinit()
{
  if (!m_device) return;

  m_device->destroyTexture(m_atlasDepth);
  m_device->destroyTexture(m_atlasEmissive);
  m_device->destroyTexture(m_atlasGBuffer0);
  m_device->destroyTexture(m_atlasGBuffer1);
  m_device->destroyTexture(m_atlasLighting);
  m_device->destroyBuffer(m_chunksBuffer);
  m_device->destroyBuffer(m_culledObjectsBuffer);
  m_device->destroyBuffer(m_objectsBuffer);
  m_device->destroyBuffer(m_objectsListBuffer);

  m_atlasDepth = {};
  m_atlasEmissive = {};
  m_atlasGBuffer0 = {};
  m_atlasGBuffer1 = {};
  m_atlasLighting = {};
  m_chunksBuffer = {};
  m_culledObjectsBuffer = {};
  m_objectsBuffer = {};
  m_objectsListBuffer = {};
  m_device = nullptr;
}

uint64_t GlobalSurfaceAtlasPass::getTotalMemoryBytes() const
{
  uint64_t total = 0;
  const uint64_t res = m_resolution;
  total += res * res * 2;                                           // depth D16
  total += res * res * 4;                                           // emissive r11g11b10f
  total += res * res * 4;                                           // GBuffer0 rgba8
  total += res * res * 8;                                           // GBuffer1 rgba16f
  total += res * res * 8;                                           // lighting rgba16f
  total += 40 * 40 * 40 * 4;                                        // chunks
  total += 4096 * 4;                                                // culledObjects
  total += 4096ull * 36 * 16;                                       // objects
  total += 4096 * 4;                                                // objectsList
  return total;
}

} // namespace demo
