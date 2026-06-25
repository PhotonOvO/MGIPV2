#include "SurfaceAtlasRasterPass.h"
#include "../GlobalSurfaceAtlasObject.h"
#include "../GlobalSurfaceAtlasTilePacker.h"
#include "../MeshPool.h"
#include "../SceneResources.h"
#include "../../common/logger.h"

#include <algorithm>
#include <cstring>
#include <array>

namespace demo
{

static constexpr uint32_t kTilesPerObject = static_cast<uint32_t>(AtlasFace::count);

void SurfaceAtlasRasterPass::init(rhi::Device& device,
                                   GlobalSurfaceAtlasPass& atlasPass,
                                   MeshPool* meshPool,
                                   SceneResources* sceneResources)
{
  m_device = &device;
  m_atlasPass = &atlasPass;
  m_meshPool = meshPool;
  m_sceneResources = sceneResources;
}

void SurfaceAtlasRasterPass::deinit()
{
  m_device = nullptr;
  m_atlasPass = nullptr;
  m_meshPool = nullptr;
  m_sceneResources = nullptr;
}

// --- Clear paths (FGI-032) ---

void SurfaceAtlasRasterPass::clearFullAtlas(rhi::CommandBuffer& cmd,
                                              GlobalSurfaceAtlasPass& atlasPass)
{
  cmd.beginEvent("GlobalSurfaceAtlas.ClearFull");

  // Clear depth to 1.0
  rhi::TextureSubresourceRange depthRange{};
  depthRange.aspect = rhi::TextureAspect::depth;
  depthRange.levelCount = 1;
  depthRange.layerCount = 1;
  rhi::ClearDepthStencilValue depthClear{1.0f, 0};
  // Note: the RHI may not support clearColorTexture for depth directly.
  // Use a render pass with depth clear instead.
  // For now, we rely on the tile raster to clear on first use via LoadOp::clear.

  // Clear lighting to black using a fullscreen compute clear
  rhi::ClearColorValue black{0.0f, 0.0f, 0.0f, 0.0f};
  rhi::TextureSubresourceRange colorRange{};
  colorRange.aspect = rhi::TextureAspect::color;
  colorRange.levelCount = 1;
  colorRange.layerCount = 1;
  cmd.clearColorTexture(atlasPass.getAtlasLighting(), colorRange, black);

  cmd.endEvent();
  LOGI("GlobalSurfaceAtlas: full atlas cleared");
}

void SurfaceAtlasRasterPass::clearTile(rhi::CommandBuffer& cmd,
                                        GlobalSurfaceAtlasPass& atlasPass,
                                        const SurfaceAtlasTileAllocation& tile)
{
  // Per-tile clear using a scissored render pass.
  // The tile raster pass will use LoadOp::clear for dirty regions.
  // This function is a placeholder — actual clears happen via render pass LoadOp.
  (void)cmd;
  (void)atlasPass;
  (void)tile;
}

void SurfaceAtlasRasterPass::clearLightingTile(rhi::CommandBuffer& cmd,
                                                GlobalSurfaceAtlasPass& atlasPass,
                                                const SurfaceAtlasTileAllocation& tile)
{
  // Clear a single tile's lighting region using the clearColorTexture
  // (only clears the scissored region via RHI impl)
  (void)cmd;
  (void)atlasPass;
  (void)tile;
}

// --- Tile rasterization (FGI-031) ---

SurfaceAtlasRasterPass::TileViewMatrices
SurfaceAtlasRasterPass::buildTileView(const GlobalSurfaceAtlasObject& object,
                                       AtlasFace face)
{
  TileViewMatrices result;
  glm::vec3 center = object.boundsCenter;
  glm::vec3 extents = (object.boundsMax - object.boundsMin) * 0.5f;
  extents += 0.1f; // small projection plane offset (matches Flax)

  switch (face)
  {
  case AtlasFace::positiveX:
    result.viewPosition = center + glm::vec3(extents.x, 0, 0);
    result.viewForward = glm::vec3(-1, 0, 0);
    break;
  case AtlasFace::negativeX:
    result.viewPosition = center - glm::vec3(extents.x, 0, 0);
    result.viewForward = glm::vec3(1, 0, 0);
    break;
  case AtlasFace::positiveY:
    result.viewPosition = center + glm::vec3(0, extents.y, 0);
    result.viewForward = glm::vec3(0, -1, 0);
    break;
  case AtlasFace::negativeY:
    result.viewPosition = center - glm::vec3(0, extents.y, 0);
    result.viewForward = glm::vec3(0, 1, 0);
    break;
  case AtlasFace::positiveZ:
    result.viewPosition = center + glm::vec3(0, 0, extents.z);
    result.viewForward = glm::vec3(0, 0, -1);
    break;
  case AtlasFace::negativeZ:
    result.viewPosition = center - glm::vec3(0, 0, extents.z);
    result.viewForward = glm::vec3(0, 0, 1);
    break;
  default:
    result.viewPosition = center + glm::vec3(extents.x, 0, 0);
    result.viewForward = glm::vec3(-1, 0, 0);
    break;
  }

  glm::vec3 up = glm::vec3(0, 1, 0);
  if (face == AtlasFace::positiveY || face == AtlasFace::negativeY)
    up = glm::vec3(0, 0, -1);

  result.view = glm::lookAt(result.viewPosition, result.viewPosition + result.viewForward, up);
  // VKDemo convention: Vulkan depth 0-to-1, reverse-Z
  float nearPlane = 0.01f;
  float farPlane = extents.x * 2.0f + extents.y * 2.0f + extents.z * 2.0f;
  float halfSize = std::max({extents.x, extents.y, extents.z});
  result.projection = glm::ortho(-halfSize, halfSize, -halfSize, halfSize, nearPlane, farPlane);
  // Flip Y for Vulkan
  result.projection[1][1] *= -1.0f;

  return result;
}

void SurfaceAtlasRasterPass::renderDirtyTiles(rhi::CommandBuffer& cmd,
                                               GlobalSurfaceAtlasPass& atlasPass,
                                               const DDGIConfig& config)
{
  if (!atlasPass.isInitialized()) return;
  auto& dataMgr = atlasPass.getDataManager();
  if (dataMgr.getDirtyObjectCount() == 0) return;

  cmd.beginEvent("GlobalSurfaceAtlas.RasterizeTiles");

  const uint32_t res = atlasPass.getResolution();
  // (Tile packer allocations are internal to the data manager — we use the data manager's pack interface)

  // For each dirty object, render its 6 tiles
  for (uint32_t i = 0; i < dataMgr.getObjectCount(); ++i)
  {
    auto* obj = dataMgr.getObject(i);
    if (!obj || !obj->dirty) continue;

    for (uint8_t f = 0; f < kTilesPerObject; ++f)
    {
      uint32_t tileIdx = obj->tileIndices[f];
      if (tileIdx == UINT32_MAX) continue;

      // Get tile view matrices
      TileViewMatrices vm = buildTileView(*obj, static_cast<AtlasFace>(f));

      // Set up the render pass on the atlas tile
      const uint32_t tileRes = config.surfaceAtlasTileResolution;
      // Tile placement in atlas from the packer — for now use object's own bounds
      // In production, tile coordinates come from the packer via dataMgr

      std::array<rhi::RenderTargetDesc, 2> colorTargets;
      // GBuffer0 (baseColor+AO)
      colorTargets[0] = {
        .texture = atlasPass.getAtlasGBuffer0(),
        .view = {}, // view would be created with sub-region in production
        .state = rhi::ResourceState::ColorAttachment,
        .loadOp = rhi::LoadOp::clear,
        .storeOp = rhi::StoreOp::store,
        .clearColor = {0.0f, 0.0f, 0.0f, 0.0f},
      };
      // GBuffer1 (normal+roughness)
      colorTargets[1] = {
        .texture = atlasPass.getAtlasGBuffer1(),
        .view = {},
        .state = rhi::ResourceState::ColorAttachment,
        .loadOp = rhi::LoadOp::clear,
        .storeOp = rhi::StoreOp::store,
        .clearColor = {0.0f, 0.0f, 0.0f, 0.0f},
      };

      rhi::DepthTargetDesc depthTarget{
        .texture = atlasPass.getAtlasDepth(),
        .view = {},
        .state = rhi::ResourceState::depthAttachment,
        .loadOp = rhi::LoadOp::clear,
        .storeOp = rhi::StoreOp::store,
        .clearValue = {1.0f, 0},
      };

      rhi::RenderPassDesc renderPassDesc{};
      renderPassDesc.renderArea = {{0, 0}, {tileRes, tileRes}};
      renderPassDesc.colorTargets = colorTargets.data();
      renderPassDesc.colorTargetCount = 2;
      renderPassDesc.depthTarget = &depthTarget;

      rhi::RenderEncoder* enc = cmd.beginRenderPass(renderPassDesc);
      if (enc)
      {
        rhi::Viewport vp{0.0f, 0.0f, static_cast<float>(tileRes), static_cast<float>(tileRes), 0.0f, 1.0f};
        rhi::Rect2D scissor{{0, 0}, {tileRes, tileRes}};
        enc->setViewport(vp);
        enc->setScissor(scissor);

        // In production: bind pipeline, vertex buffers, set push constants
        // with vm.view, vm.projection, and draw the object's meshes.
        // For now, this is the architecture skeleton.

        cmd.endEncoding();
      }
    }
    obj->dirty = false;
    obj->anyTileDirty = false;
  }

  cmd.endEvent();
}

} // namespace demo
