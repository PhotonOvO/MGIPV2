#pragma once

// Global Surface Atlas tile rasterization pass (FGI-031) and clear paths (FGI-032).
// Renders dirty static object tiles into the atlas depth+GBuffer textures using
// six orthographic views per object. Reuses existing GBuffer rendering where possible.

#include "../DDGIConfig.h"
#include "../GlobalSurfaceAtlasData.h"
#include "GlobalSurfaceAtlasPass.h"

#include "../../rhi/RHICommandBuffer.h"
#include "../../rhi/RHIEncoder.h"
#include "../../rhi/RHIDevice.h"
#include "../../rhi/RHITypes.h"

#include <glm/glm.hpp>
#include <glm/gtc/matrix_transform.hpp>
#include <cstdint>
#include <vector>

namespace demo
{

class MeshPool;
class SceneResources;

class SurfaceAtlasRasterPass
{
public:
  void init(rhi::Device& device, GlobalSurfaceAtlasPass& atlasPass,
            MeshPool* meshPool, SceneResources* sceneResources);
  void deinit();

  // --- FGI-032: Clear paths ---
  // Full atlas clear (depth = 1.0, color = black, lighting = black)
  void clearFullAtlas(rhi::CommandBuffer& cmd, GlobalSurfaceAtlasPass& atlasPass);

  // Per-tile clear before rasterizing a dirty tile
  void clearTile(rhi::CommandBuffer& cmd, GlobalSurfaceAtlasPass& atlasPass,
                 const SurfaceAtlasTileAllocation& tile);

  // Lighting-only clear (for lighting refresh)
  void clearLightingTile(rhi::CommandBuffer& cmd, GlobalSurfaceAtlasPass& atlasPass,
                          const SurfaceAtlasTileAllocation& tile);

  // --- FGI-031: Tile rasterization ---
  // Renders all dirty tiles for all dirty objects in the data manager.
  // Reuses existing GBuffer rendering infrastructure.
  void renderDirtyTiles(rhi::CommandBuffer& cmd, GlobalSurfaceAtlasPass& atlasPass,
                         const DDGIConfig& config);

private:
  // Builds view/projection matrices for a tile capture direction
  struct TileViewMatrices
  {
    glm::mat4 view;
    glm::mat4 projection;
    glm::vec3 viewPosition;
    glm::vec3 viewForward;
  };

  static TileViewMatrices buildTileView(const GlobalSurfaceAtlasObject& object,
                                          AtlasFace face);

  rhi::Device* m_device{nullptr};
  GlobalSurfaceAtlasPass* m_atlasPass{nullptr};
  MeshPool* m_meshPool{nullptr};
  SceneResources* m_sceneResources{nullptr};
};

} // namespace demo
