#pragma once

// Global Surface Atlas CPU buffer packing and dirty tracking (FGI-022, FGI-023).
// Packs object+tile data matching GlobalSurfaceAtlas.hlsl semantics into
// float4 storage buffers. Tracks dirty state for incremental atlas updates.
//
// Buffer layout (per object):
//   6 float4s  — object header (bounds, rotation, position, extents, tile offsets, visibility)
//   N*5 float4s — per-tile data (N = active tile count, up to 6)
//   Total per object = 6 + 6*5 = 36 float4s = 576 bytes

#include "GlobalSurfaceAtlasObject.h"
#include "GlobalSurfaceAtlasTilePacker.h"

#include <glm/glm.hpp>
#include <glm/gtc/matrix_transform.hpp>
#include <glm/gtc/type_ptr.hpp>

#include <cstdint>
#include <cstring>
#include <vector>

namespace demo
{

// --- Shader-compatible packed types (must match .hlsl / .slang) ---
// Each is stored as consecutive float4s in a storage buffer.

struct PackedAtlasObject
{
  glm::vec4 boundsPositionRadius;   // xyz = bounds center, w = bounds radius
  glm::vec4 packedTileOffsets;      // xyz = packed tile offsets (uint16 pairs), w = dataSize
  glm::vec4 worldToLocalRow0AndPosX;// xyz = rotation row 0, w = worldPosition.x
  glm::vec4 worldToLocalRow1AndPosY;// xyz = rotation row 1, w = worldPosition.y
  glm::vec4 worldToLocalRow2AndPosZ;// xyz = rotation row 2, w = worldPosition.z
  glm::vec4 worldExtentsAndVis;     // xyz = world extents, w = useVisibility (>0.5)
};
static_assert(sizeof(PackedAtlasObject) == 96, "PackedAtlasObject must be 6 float4s");

struct PackedAtlasTile
{
  glm::vec4 atlasRectUV;            // xy = atlas top-left UV, zw = atlas size UV
  glm::vec4 worldToLocalRow0;       // xyz = rotation row 0, w = translation.x
  glm::vec4 worldToLocalRow1;       // xyz = rotation row 1, w = translation.y
  glm::vec4 worldToLocalRow2;       // xyz = rotation row 2, w = translation.z
  glm::vec4 viewBoundsSize;         // xyz = view bounds size, w unused
};
static_assert(sizeof(PackedAtlasTile) == 80, "PackedAtlasTile must be 5 float4s");

// Helper: pack two uint16_t into a uint32_t
inline uint32_t packU16Pair(uint16_t a, uint16_t b)
{
  return (static_cast<uint32_t>(a) & 0xFFFFu) | (static_cast<uint32_t>(b) << 16u);
}

// --- Dirty tracking state ---
enum class AtlasDirtyReason : uint8_t
{
  none = 0,
  transformChanged,
  meshChanged,
  materialChanged,
  atlasLayoutChanged,
  forceRebuild,
};

// --- Surface Atlas data manager ---
class GlobalSurfaceAtlasDataManager
{
public:
  static constexpr uint32_t kFloat4sPerObject = 6;
  static constexpr uint32_t kFloat4sPerTile = 5;
  static constexpr uint32_t kMaxTilesPerObject = 6;
  static constexpr uint32_t kMaxFloat4sPerObject = kFloat4sPerObject + kMaxTilesPerObject * kFloat4sPerTile; // 36

  void init(uint32_t atlasResolution, uint32_t tileResolution, uint32_t tilePadding)
  {
    m_atlasResolution = atlasResolution;
    m_tileResolution = tileResolution;
    m_tilePadding = tilePadding;
    m_packer.init(atlasResolution, atlasResolution, tilePadding);
    m_objects.clear();
    m_packedObjects.clear();
    m_packedTiles.clear();
    m_dirtyObjects.clear();
    m_removedObjects.clear();
    m_frameIndex = 0;
  }

  // --- Object registration ---
  uint32_t registerObject(const GlobalSurfaceAtlasObject& obj)
  {
    uint32_t idx = static_cast<uint32_t>(m_objects.size());
    m_objects.push_back(obj);

    // Allocate tiles
    std::vector<uint32_t> tileIndices;
    uint32_t allocated = m_packer.allocateObjectTiles(obj, m_tileResolution, tileIndices);
    for (uint8_t f = 0; f < static_cast<uint8_t>(AtlasFace::count); ++f)
    {
      m_objects.back().tileIndices[f] = tileIndices[f];
    }
    m_objects.back().anyTileDirty = (allocated < 6);
    m_objects.back().dirty = true;
    m_objects.back().lastSeenFrame = m_frameIndex;

    m_dirtyObjects.push_back(idx);
    return idx;
  }

  // --- Dirty tracking ---
  void markDirty(uint32_t objectIdx, AtlasDirtyReason reason)
  {
    if (objectIdx >= m_objects.size()) return;
    auto& obj = m_objects[objectIdx];
    obj.dirty = true;
    obj.anyTileDirty = true;
    m_dirtyObjects.push_back(objectIdx);

    if (reason == AtlasDirtyReason::atlasLayoutChanged ||
        reason == AtlasDirtyReason::forceRebuild)
    {
      m_needsFullRebuild = true;
    }
  }

  void markTransformChanged(uint32_t objectIdx)
  {
    markDirty(objectIdx, AtlasDirtyReason::transformChanged);
  }

  // Advance frame, remove unseen objects
  void advanceFrame()
  {
    ++m_frameIndex;
    m_dirtyObjects.clear();
    m_removedObjects.clear();

    for (uint32_t i = 0; i < static_cast<uint32_t>(m_objects.size()); ++i)
    {
      auto& obj = m_objects[i];
      if (!obj.visible || (m_frameIndex - obj.lastSeenFrame) > kMaxUnseenFrames)
      {
        m_removedObjects.push_back(i);
      }
    }
  }

  void markAllSeen()
  {
    for (auto& obj : m_objects)
      obj.lastSeenFrame = m_frameIndex;
  }

  void removeObject(uint32_t objectIdx)
  {
    if (objectIdx >= m_objects.size()) return;
    auto& obj = m_objects[objectIdx];
    obj.visible = false;
    m_packer.freeObjectTiles(static_cast<uint32_t>(obj.key));
    m_removedObjects.push_back(objectIdx);
  }

  void forceRebuild()
  {
    m_needsFullRebuild = true;
    for (uint32_t i = 0; i < static_cast<uint32_t>(m_objects.size()); ++i)
      markDirty(i, AtlasDirtyReason::forceRebuild);
  }

  // --- Buffer packing ---
  // Pack all objects and tiles into float4 vectors ready for GPU upload.
  // Returns true if any data changed since last pack.
  bool packToBuffer()
  {
    if (m_dirtyObjects.empty() && !m_needsFullRebuild && m_removedObjects.empty())
      return false;

    if (m_needsFullRebuild)
    {
      m_packer.rebuild(m_atlasResolution, m_atlasResolution);
      // Re-assign tile indices after rebuild
      // (simplified: full rebuild re-allocates all)
      m_needsFullRebuild = false;
    }

    m_packedObjects.resize(m_objects.size());

    for (uint32_t idx : m_dirtyObjects)
    {
      if (idx >= m_objects.size()) continue;
      packObject(idx);
    }

    return true;
  }

  // --- Accessors for GPU upload ---
  [[nodiscard]] const std::vector<glm::vec4>& getObjectBuffer() const { return m_packedObjectFloats; }
  [[nodiscard]] const std::vector<uint32_t>& getObjectList() const { return m_objectList; }
  [[nodiscard]] size_t getObjectBufferSizeBytes() const { return m_packedObjectFloats.size() * sizeof(glm::vec4); }
  [[nodiscard]] uint32_t getObjectCount() const { return static_cast<uint32_t>(m_objects.size()); }
  [[nodiscard]] uint32_t getDirtyObjectCount() const { return static_cast<uint32_t>(m_dirtyObjects.size()); }
  [[nodiscard]] uint32_t getTileCount() const { return static_cast<uint32_t>(m_packer.getAllocationCount()); }

  [[nodiscard]] const std::vector<GlobalSurfaceAtlasObject>& getObjects() const { return m_objects; }
  [[nodiscard]] GlobalSurfaceAtlasObject* getObject(uint32_t idx)
  {
    return idx < m_objects.size() ? &m_objects[idx] : nullptr;
  }

  [[nodiscard]] uint64_t getUsedPixels() const { return m_packer.getUsedPixels(); }
  [[nodiscard]] float getUsageFraction() const { return m_packer.getUsageFraction(); }
  [[nodiscard]] uint32_t getFrameIndex() const { return m_frameIndex; }

private:
  void packObject(uint32_t idx)
  {
    const auto& obj = m_objects[idx];
    auto& packed = m_packedObjects[idx];

    // Object header (6 float4s)
    packed.boundsPositionRadius = glm::vec4(obj.boundsCenter, obj.boundsRadius);

    // Build rotation matrix (world-to-local = inverse of transform rotation)
    glm::mat3 worldToLocalRot = glm::mat3(glm::inverse(obj.transform));

    // Pack tile offsets into vector1
    uint32_t t0 = obj.tileIndices[0] != UINT32_MAX ? obj.tileIndices[0] : 0;
    uint32_t t1 = obj.tileIndices[1] != UINT32_MAX ? obj.tileIndices[1] : 0;
    uint32_t t2 = obj.tileIndices[2] != UINT32_MAX ? obj.tileIndices[2] : 0;
    uint32_t t3 = obj.tileIndices[3] != UINT32_MAX ? obj.tileIndices[3] : 0;
    uint32_t t4 = obj.tileIndices[4] != UINT32_MAX ? obj.tileIndices[4] : 0;
    uint32_t t5 = obj.tileIndices[5] != UINT32_MAX ? obj.tileIndices[5] : 0;

    packed.packedTileOffsets = glm::vec4(
      glm::uintBitsToFloat(packU16Pair(static_cast<uint16_t>(t0), static_cast<uint16_t>(t1))),
      glm::uintBitsToFloat(packU16Pair(static_cast<uint16_t>(t2), static_cast<uint16_t>(t3))),
      glm::uintBitsToFloat(packU16Pair(static_cast<uint16_t>(t4), static_cast<uint16_t>(t5))),
      glm::uintBitsToFloat(kFloat4sPerObject + kMaxTilesPerObject * kFloat4sPerTile) // dataSize
    );

    glm::vec3 worldPos(obj.transform[3]);
    packed.worldToLocalRow0AndPosX = glm::vec4(worldToLocalRot[0], worldPos.x);
    packed.worldToLocalRow1AndPosY = glm::vec4(worldToLocalRot[1], worldPos.y);
    packed.worldToLocalRow2AndPosZ = glm::vec4(worldToLocalRot[2], worldPos.z);

    glm::vec3 extents = (obj.boundsMax - obj.boundsMin) * 0.5f;
    packed.worldExtentsAndVis = glm::vec4(extents, obj.participates ? 1.0f : 0.0f);

    // Pack tiles
    packTilesForObject(idx, obj);

    // Build final float4 buffer
    rebuildFloatBuffer();
  }

  void packTilesForObject(uint32_t objIdx, const GlobalSurfaceAtlasObject& obj)
  {
    // Each tile is 5 float4s. Tile data starts after the object header.
    // Tile offsets in the buffer = objectBase + kFloat4sPerObject + faceIndex * kFloat4sPerTile

    for (uint8_t f = 0; f < static_cast<uint8_t>(AtlasFace::count); ++f)
    {
      if (obj.tileIndices[f] == UINT32_MAX) continue;

      const auto& alloc = m_packer.getAllocations()[obj.tileIndices[f]];
      if (!alloc.allocated) continue;

      // Build world-to-tile-local matrix
      glm::mat4 tileWorldToLocal = buildTileWorldToLocal(static_cast<AtlasFace>(f), obj);

      // Atlas UV rect
      float invRes = 1.0f / static_cast<float>(m_atlasResolution);
      glm::vec4 atlasRectUV(
        static_cast<float>(alloc.atlasX) * invRes,
        static_cast<float>(alloc.atlasY) * invRes,
        static_cast<float>(alloc.width) * invRes,
        static_cast<float>(alloc.height) * invRes
      );

      // View bounds size for this tile
      glm::vec3 viewExtents = (obj.boundsMax - obj.boundsMin) * 0.5f;

      // Store in packed tile array
      PackedAtlasTile packedTile;
      packedTile.atlasRectUV = atlasRectUV;
      packedTile.worldToLocalRow0 = glm::vec4(glm::vec3(tileWorldToLocal[0]), tileWorldToLocal[3].x);
      packedTile.worldToLocalRow1 = glm::vec4(glm::vec3(tileWorldToLocal[1]), tileWorldToLocal[3].y);
      packedTile.worldToLocalRow2 = glm::vec4(glm::vec3(tileWorldToLocal[2]), tileWorldToLocal[3].z);
      packedTile.viewBoundsSize = glm::vec4(viewExtents, 0.0f);

      m_packedTiles[objIdx * kMaxTilesPerObject + f] = packedTile;
    }
  }

  static glm::mat4 buildTileWorldToLocal(AtlasFace face, const GlobalSurfaceAtlasObject& obj)
  {
    // Six axis-aligned orthographic capture directions.
    // Tile view: looking from the object center toward the given face.
    glm::vec3 forward, up, right;
    switch (face)
    {
    case AtlasFace::positiveX: forward = glm::vec3( 1, 0, 0); up = glm::vec3(0, 1, 0); break;
    case AtlasFace::negativeX: forward = glm::vec3(-1, 0, 0); up = glm::vec3(0, 1, 0); break;
    case AtlasFace::positiveY: forward = glm::vec3( 0, 1, 0); up = glm::vec3(0, 0,-1); break;
    case AtlasFace::negativeY: forward = glm::vec3( 0,-1, 0); up = glm::vec3(0, 0, 1); break;
    case AtlasFace::positiveZ: forward = glm::vec3( 0, 0, 1); up = glm::vec3(0, 1, 0); break;
    case AtlasFace::negativeZ: forward = glm::vec3( 0, 0,-1); up = glm::vec3(0, 1, 0); break;
    default: forward = glm::vec3(1,0,0); up = glm::vec3(0,1,0); break;
    }
    right = glm::cross(up, forward);

    glm::vec3 extents = (obj.boundsMax - obj.boundsMin) * 0.5f;
    glm::vec3 center = obj.boundsCenter;

    // View matrix: look-at from object center toward face
    glm::mat4 view = glm::lookAt(center, center + forward, up);
    // World-to-local = inverse of the view (which is the object's face-local space)
    return glm::inverse(view);
  }

  void rebuildFloatBuffer()
  {
    m_packedObjectFloats.clear();
    m_objectList.clear();

    for (uint32_t i = 0; i < static_cast<uint32_t>(m_objects.size()); ++i)
    {
      if (!m_objects[i].visible) continue;

      m_objectList.push_back(i);
      uint32_t baseOffset = static_cast<uint32_t>(m_packedObjectFloats.size());

      const auto& packed = m_packedObjects[i];
      const float* data = reinterpret_cast<const float*>(&packed);
      for (uint32_t j = 0; j < kFloat4sPerObject * 4; ++j)
        m_packedObjectFloats.push_back(glm::vec4()); // placeholder

      // Actually write the object data
      uint32_t writeBase = baseOffset;
      const glm::vec4* vecs = reinterpret_cast<const glm::vec4*>(&packed);
      m_packedObjectFloats[writeBase + 0] = vecs[0];
      m_packedObjectFloats[writeBase + 1] = vecs[1];
      m_packedObjectFloats[writeBase + 2] = vecs[2];
      m_packedObjectFloats[writeBase + 3] = vecs[3];
      m_packedObjectFloats[writeBase + 4] = vecs[4];
      m_packedObjectFloats[writeBase + 5] = vecs[5];

      // Append tile data from packed tiles
      for (uint32_t t = 0; t < kMaxTilesPerObject; ++t)
      {
        const auto& tile = m_packedTiles[i * kMaxTilesPerObject + t];
        const glm::vec4* tileVecs = reinterpret_cast<const glm::vec4*>(&tile);
        m_packedObjectFloats.push_back(tileVecs[0]);
        m_packedObjectFloats.push_back(tileVecs[1]);
        m_packedObjectFloats.push_back(tileVecs[2]);
        m_packedObjectFloats.push_back(tileVecs[3]);
        m_packedObjectFloats.push_back(tileVecs[4]);
      }
    }
  }

  uint32_t m_atlasResolution{512};
  uint32_t m_tileResolution{64};
  uint32_t m_tilePadding{1};
  uint32_t m_frameIndex{0};
  bool m_needsFullRebuild{false};

  static constexpr uint32_t kMaxUnseenFrames = 60; // ~1 second at 60fps

  SurfaceAtlasTilePacker m_packer;
  std::vector<GlobalSurfaceAtlasObject> m_objects;
  std::vector<PackedAtlasObject> m_packedObjects;
  std::vector<PackedAtlasTile> m_packedTiles;
  std::vector<uint32_t> m_dirtyObjects;
  std::vector<uint32_t> m_removedObjects;

  // Final GPU-ready buffers
  std::vector<glm::vec4> m_packedObjectFloats;   // float4 storage buffer
  std::vector<uint32_t> m_objectList;            // object index list for culling input
};

} // namespace demo
