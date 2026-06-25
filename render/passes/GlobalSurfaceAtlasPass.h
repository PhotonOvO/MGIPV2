#pragma once

// Global Surface Atlas resource owner (FGI-030).
// Allocates atlas textures (depth, emissive, GBuffer0, GBuffer1, lighting) and
// storage buffers (chunks, culledObjects, objects, objectsList) through RHI.
// Gated on DDGIConfig::enableGlobalSurfaceAtlas && DDGIConfig::enabled.

#include "rhi/RHIDevice.h"
#include "rhi/RHIHandles.h"
#include "rhi/RHITypes.h"
#include "../DDGIConfig.h"
#include "../GlobalSurfaceAtlasData.h"

#include <glm/glm.hpp>
#include <cstdint>

namespace demo
{

class GlobalSurfaceAtlasPass
{
public:
  void init(rhi::Device& device, const DDGIConfig& config);
  void deinit();

  [[nodiscard]] bool isInitialized() const { return m_device != nullptr; }

  // --- Atlas texture accessors ---
  [[nodiscard]] rhi::TextureHandle getAtlasDepth() const { return m_atlasDepth; }
  [[nodiscard]] rhi::TextureHandle getAtlasEmissive() const { return m_atlasEmissive; }
  [[nodiscard]] rhi::TextureHandle getAtlasGBuffer0() const { return m_atlasGBuffer0; }
  [[nodiscard]] rhi::TextureHandle getAtlasGBuffer1() const { return m_atlasGBuffer1; }
  [[nodiscard]] rhi::TextureHandle getAtlasLighting() const { return m_atlasLighting; }

  // --- Buffer accessors ---
  [[nodiscard]] rhi::BufferHandle getChunksBuffer() const { return m_chunksBuffer; }
  [[nodiscard]] rhi::BufferHandle getCulledObjectsBuffer() const { return m_culledObjectsBuffer; }
  [[nodiscard]] rhi::BufferHandle getObjectsBuffer() const { return m_objectsBuffer; }
  [[nodiscard]] rhi::BufferHandle getObjectsListBuffer() const { return m_objectsListBuffer; }

  [[nodiscard]] uint32_t getResolution() const { return m_resolution; }
  [[nodiscard]] uint64_t getTotalMemoryBytes() const;

  // --- Data manager ---
  [[nodiscard]] GlobalSurfaceAtlasDataManager& getDataManager() { return m_dataManager; }

private:
  rhi::Device* m_device{nullptr};
  uint32_t m_resolution{2048};
  uint32_t m_tileResolution{128};
  uint32_t m_tilePadding{1};

  // Atlas textures
  rhi::TextureHandle m_atlasDepth{};
  rhi::TextureHandle m_atlasEmissive{};
  rhi::TextureHandle m_atlasGBuffer0{};
  rhi::TextureHandle m_atlasGBuffer1{};
  rhi::TextureHandle m_atlasLighting{};

  // Storage buffers
  rhi::BufferHandle m_chunksBuffer{};
  rhi::BufferHandle m_culledObjectsBuffer{};
  rhi::BufferHandle m_objectsBuffer{};
  rhi::BufferHandle m_objectsListBuffer{};

  // CPU-side data manager (FGI-022/023)
  GlobalSurfaceAtlasDataManager m_dataManager;
};

} // namespace demo
