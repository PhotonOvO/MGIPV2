#include "GPUBatchBuilder.h"

#include <algorithm>

namespace demo {

void GPUBatchBuilder::init(uint32_t maxObjects)
{
  m_maxObjects = maxObjects;
  m_stats = {};
  m_visibleObjects.clear();
  m_sortKeys.clear();
  m_batchRanges.clear();
}

void GPUBatchBuilder::buildBatches(VkCommandBuffer, uint32_t visibleCount)
{
  m_stats.visibleCount = visibleCount;
  m_stats.batchCount = visibleCount;
  m_visibleObjects.clear();
  m_sortKeys.clear();
  m_batchRanges.clear();
  recomputeSortPassCount(visibleCount);
}

void GPUBatchBuilder::buildBatches(std::span<const uint32_t> opaqueVisibleObjects,
                                   std::span<const uint32_t> transparentVisibleObjects)
{
  m_visibleObjects.clear();
  m_sortKeys.clear();
  m_batchRanges.clear();
  m_visibleObjects.reserve(opaqueVisibleObjects.size() + transparentVisibleObjects.size());
  m_sortKeys.reserve(opaqueVisibleObjects.size() + transparentVisibleObjects.size());

  if(!opaqueVisibleObjects.empty())
  {
    const uint32_t firstVisibleIndex = static_cast<uint32_t>(m_visibleObjects.size());
    m_visibleObjects.insert(m_visibleObjects.end(), opaqueVisibleObjects.begin(), opaqueVisibleObjects.end());
    for(uint32_t objectIndex : opaqueVisibleObjects)
    {
      m_sortKeys.push_back(objectIndex & 0x7fffffffu);
    }
    m_batchRanges.push_back(BatchRange{
        .firstVisibleIndex = firstVisibleIndex,
        .visibleCount = static_cast<uint32_t>(opaqueVisibleObjects.size()),
        .transparent = false,
    });
  }

  if(!transparentVisibleObjects.empty())
  {
    const uint32_t firstVisibleIndex = static_cast<uint32_t>(m_visibleObjects.size());
    m_visibleObjects.insert(m_visibleObjects.end(), transparentVisibleObjects.begin(), transparentVisibleObjects.end());
    for(uint32_t objectIndex : transparentVisibleObjects)
    {
      m_sortKeys.push_back(objectIndex | 0x80000000u);
    }
    m_batchRanges.push_back(BatchRange{
        .firstVisibleIndex = firstVisibleIndex,
        .visibleCount = static_cast<uint32_t>(transparentVisibleObjects.size()),
        .transparent = true,
    });
  }

  m_stats.visibleCount = static_cast<uint32_t>(m_visibleObjects.size());
  m_stats.batchCount = static_cast<uint32_t>(m_batchRanges.size());
  recomputeSortPassCount(m_stats.visibleCount);
}

void GPUBatchBuilder::adoptSortedVisibleObjects(std::span<const uint32_t> sortedVisibleObjects,
                                                std::span<const uint32_t> sortedKeys)
{
  m_visibleObjects.assign(sortedVisibleObjects.begin(), sortedVisibleObjects.end());
  m_sortKeys.assign(sortedKeys.begin(), sortedKeys.end());
  m_batchRanges.clear();

  if(m_visibleObjects.empty() || m_visibleObjects.size() != m_sortKeys.size())
  {
    m_visibleObjects.clear();
    m_sortKeys.clear();
    m_stats.visibleCount = 0;
    m_stats.batchCount = 0;
    recomputeSortPassCount(0);
    return;
  }

  uint32_t batchBegin = 0;
  bool currentTransparent = (m_sortKeys.front() & 0x80000000u) != 0u;
  for(uint32_t i = 1; i < static_cast<uint32_t>(m_sortKeys.size()); ++i)
  {
    const bool transparent = (m_sortKeys[i] & 0x80000000u) != 0u;
    if(transparent == currentTransparent)
    {
      continue;
    }

    m_batchRanges.push_back(BatchRange{
        .firstVisibleIndex = batchBegin,
        .visibleCount = i - batchBegin,
        .transparent = currentTransparent,
    });
    batchBegin = i;
    currentTransparent = transparent;
  }

  m_batchRanges.push_back(BatchRange{
      .firstVisibleIndex = batchBegin,
      .visibleCount = static_cast<uint32_t>(m_visibleObjects.size()) - batchBegin,
      .transparent = currentTransparent,
  });

  m_stats.visibleCount = static_cast<uint32_t>(m_visibleObjects.size());
  m_stats.batchCount = static_cast<uint32_t>(m_batchRanges.size());
  recomputeSortPassCount(m_stats.visibleCount);
}

void GPUBatchBuilder::recomputeSortPassCount(uint32_t visibleCount)
{
  uint32_t passes = 0;
  for(uint32_t width = 2; width <= std::max(visibleCount, 1u); width <<= 1u)
  {
    for(uint32_t stride = width >> 1u; stride > 0; stride >>= 1u)
    {
      ++passes;
    }
  }
  m_stats.sortPassCount = passes;
}

}  // namespace demo
