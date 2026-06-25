#pragma once

// Global Surface Atlas object model (FGI-020).
// Defines the CPU-side data for static opaque objects that may be captured
// into the Global Surface Atlas. No Flax actor/draw-pass types; no native
// backend handles. GPU resources are owned by the future GlobalSurfaceAtlasPass.
//
// Participation rules:
//  - Static opaque meshes only (alphaMode == OPAQUE).
//  - Excluded: transparent, skinned, particle, UI, terrain, foliage.
//  - Stable key: scene objectID (from GPUSceneRegistry) | (meshIndex << 32).

#include <cstdint>
#include <glm/glm.hpp>

namespace demo
{

// Face indices for six directional atlas tiles (axis-aligned captures).
enum class AtlasFace : uint8_t
{
  positiveX = 0,
  negativeX = 1,
  positiveY = 2,
  negativeY = 3,
  positiveZ = 4,
  negativeZ = 5,
  count = 6,
};

// Describes one allocated tile in the surface atlas.
struct GlobalSurfaceAtlasTile
{
  uint32_t atlasX{0};       // top-left pixel X in atlas
  uint32_t atlasY{0};       // top-left pixel Y in atlas
  uint32_t width{0};
  uint32_t height{0};
  AtlasFace face{AtlasFace::positiveX};
  uint32_t objectKey{0};    // owning object key
  bool dirty{true};          // needs redraw
};

// A single candidate object for the Surface Atlas.
// Keyed by (sceneObjectID | (meshIndex << 32)) — stable across frames
// unless the scene changes.
struct GlobalSurfaceAtlasObject
{
  // Stable identity
  uint64_t key{0};            // packed objectID + meshIndex
  uint32_t meshIndex{0};
  uint32_t materialIndex{0};

  // World-space geometry
  glm::vec3 boundsMin{0.0f};
  glm::vec3 boundsMax{0.0f};
  glm::vec3 boundsCenter{0.0f};
  float boundsRadius{0.0f};
  glm::mat4 transform{1.0f};  // local-to-world

  // Tile handles (indices into the tile array). UINT32_MAX = unallocated.
  uint32_t tileIndices[static_cast<uint8_t>(AtlasFace::count)]{};
  bool anyTileDirty{true};     // at least one tile needs redraw

  // Lifecycle flags
  bool visible{true};          // currently in scene
  bool participates{false};    // meets participation rules
  bool dirty{true};            // needs full tile redraw (transform/material change)
  uint32_t lastSeenFrame{0};   // frame index of last scene presence
};

// Key packing helper: combines GPUSceneRegistry objectID and mesh index.
inline constexpr uint64_t makeSurfaceAtlasObjectKey(uint32_t objectID, uint32_t meshIndex)
{
  return (static_cast<uint64_t>(meshIndex) << 32) | static_cast<uint64_t>(objectID);
}

} // namespace demo
