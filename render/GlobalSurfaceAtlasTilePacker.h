#pragma once

// Rectangle-packing atlas for Global Surface Atlas tiles (FGI-021).
// Deterministic CPU-side tile allocation: six directional tiles per object.
// Sorted by descending size before insertion to reduce fragmentation.
// Full rebuild/defrag policy for failed incremental allocation.

#include "GlobalSurfaceAtlasObject.h"

#include <algorithm>
#include <cstdint>
#include <vector>

namespace demo
{

struct SurfaceAtlasTileAllocation
{
  uint32_t atlasX{0};
  uint32_t atlasY{0};
  uint32_t width{0};
  uint32_t height{0};
  uint32_t objectKey{0};
  AtlasFace face{AtlasFace::positiveX};
  bool allocated{false};
};

// Shelf-based rectangle packer (simpler than full bin packing, acceptable for
// the bounded tile count of a surface atlas).
class SurfaceAtlasTilePacker
{
public:
  // Atlas dimensions MUST be set before any allocation.
  void init(uint32_t atlasWidth, uint32_t atlasHeight, uint32_t tilePadding)
  {
    m_atlasWidth = atlasWidth;
    m_atlasHeight = atlasHeight;
    m_tilePadding = tilePadding;
    m_currentY = tilePadding;
    m_nextX = tilePadding;
    m_allocations.clear();
    m_usedPixels = 0;
  }

  // Attempt to allocate a tile of the given size. Returns false if no space.
  // Padded size = (width + 2*padding) x (height + 2*padding).
  bool allocate(uint32_t width, uint32_t height, uint32_t objectKey,
                AtlasFace face, SurfaceAtlasTileAllocation& out)
  {
    const uint32_t paddedW = width + 2 * m_tilePadding;
    const uint32_t paddedH = height + 2 * m_tilePadding;

    // Try to fit on the current row
    if (m_nextX + paddedW <= m_atlasWidth && m_currentY + paddedH <= m_atlasHeight)
    {
      // Fits on current row
      out.atlasX = m_nextX;
      out.atlasY = m_currentY;
      out.width = paddedW;
      out.height = paddedH;
      out.objectKey = objectKey;
      out.face = face;
      out.allocated = true;

      m_nextX += paddedW;
      m_usedPixels += paddedW * paddedH;
      m_allocations.push_back(out);
      return true;
    }

    // Move to next row
    if (m_currentY + paddedH + m_tilePadding <= m_atlasHeight)
    {
      m_currentY += m_maxRowHeight + m_tilePadding;
      m_nextX = m_tilePadding;
      m_maxRowHeight = 0;

      if (m_nextX + paddedW <= m_atlasWidth)
      {
        out.atlasX = m_nextX;
        out.atlasY = m_currentY;
        out.width = paddedW;
        out.height = paddedH;
        out.objectKey = objectKey;
        out.face = face;
        out.allocated = true;

        m_nextX += paddedW;
        m_maxRowHeight = paddedH;
        m_usedPixels += paddedW * paddedH;
        m_allocations.push_back(out);
        return true;
      }
    }

    // Does not fit
    out.allocated = false;
    return false;
  }

  // Allocate six tiles for one object. Sorted by descending size.
  // Returns the number of successfully allocated tiles (should be 6).
  uint32_t allocateObjectTiles(const GlobalSurfaceAtlasObject& object,
                                 uint32_t tileResolution,
                                 std::vector<uint32_t>& outTileIndices)
  {
    outTileIndices.resize(6, UINT32_MAX);
    if (tileResolution == 0) return 0;

    uint32_t allocated = 0;
    for (uint8_t f = 0; f < static_cast<uint8_t>(AtlasFace::count); ++f)
    {
      SurfaceAtlasTileAllocation alloc;
      if (allocate(tileResolution, tileResolution, static_cast<uint32_t>(object.key),
                   static_cast<AtlasFace>(f), alloc))
      {
        outTileIndices[f] = static_cast<uint32_t>(m_allocations.size() - 1);
        ++allocated;
      }
    }
    return allocated;
  }

  // Free all tiles owned by an object.
  void freeObjectTiles(uint32_t objectKey)
  {
    for (auto& alloc : m_allocations)
    {
      if (alloc.objectKey == objectKey)
      {
        alloc.allocated = false;
      }
    }
    // NOTE: no compaction — fragmentation accumulates. Call rebuild() to defrag.
  }

  // Defrag / full rebuild: re-pack all live allocations.
  void rebuild(uint32_t atlasWidth, uint32_t atlasHeight)
  {
    // Save live allocations
    std::vector<SurfaceAtlasTileAllocation> live;
    live.reserve(m_allocations.size());
    for (const auto& a : m_allocations)
    {
      if (a.allocated)
        live.push_back(a);
    }

    // Sort by descending size for better packing
    std::sort(live.begin(), live.end(),
              [](const SurfaceAtlasTileAllocation& a, const SurfaceAtlasTileAllocation& b) {
                return (a.width * a.height) > (b.width * b.height);
              });

    // Re-initialize and re-pack
    init(atlasWidth, atlasHeight, m_tilePadding);

    for (const auto& a : live)
    {
      SurfaceAtlasTileAllocation out;
      allocate(a.width - 2 * m_tilePadding, a.height - 2 * m_tilePadding,
               a.objectKey, a.face, out);
    }
  }

  [[nodiscard]] uint32_t getAtlasWidth() const { return m_atlasWidth; }
  [[nodiscard]] uint32_t getAtlasHeight() const { return m_atlasHeight; }
  [[nodiscard]] uint64_t getUsedPixels() const { return m_usedPixels; }
  [[nodiscard]] uint64_t getTotalPixels() const { return static_cast<uint64_t>(m_atlasWidth) * m_atlasHeight; }
  [[nodiscard]] float getUsageFraction() const
  {
    const uint64_t total = getTotalPixels();
    return total > 0 ? static_cast<float>(m_usedPixels) / static_cast<float>(total) : 0.0f;
  }
  [[nodiscard]] size_t getAllocationCount() const { return m_allocations.size(); }
  [[nodiscard]] const std::vector<SurfaceAtlasTileAllocation>& getAllocations() const { return m_allocations; }

private:
  uint32_t m_atlasWidth{0};
  uint32_t m_atlasHeight{0};
  uint32_t m_tilePadding{1};
  uint32_t m_currentY{0};
  uint32_t m_nextX{0};
  uint32_t m_maxRowHeight{0};
  uint64_t m_usedPixels{0};
  std::vector<SurfaceAtlasTileAllocation> m_allocations;
};

} // namespace demo
