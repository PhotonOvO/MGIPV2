#pragma once

#include "../common/Common.h"

#include <span>
#include <vector>

namespace demo {

class GPUBatchBuilder
{
public:
  struct BatchRange
  {
    uint32_t firstVisibleIndex{0};
    uint32_t visibleCount{0};
    bool     transparent{false};
  };

  void init(uint32_t maxObjects);
  void buildBatches(VkCommandBuffer cmd, uint32_t visibleCount);
  void buildBatches(std::span<const uint32_t> opaqueVisibleObjects, std::span<const uint32_t> transparentVisibleObjects);
  void adoptSortedVisibleObjects(std::span<const uint32_t> sortedVisibleObjects,
                                 std::span<const uint32_t> sortedKeys);
  [[nodiscard]] const shaderio::GPUBatchBuildStats& getStats() const { return m_stats; }
  [[nodiscard]] std::span<const uint32_t> getVisibleObjects() const { return m_visibleObjects; }
  [[nodiscard]] std::span<const uint32_t> getSortKeys() const { return m_sortKeys; }
  [[nodiscard]] std::span<const BatchRange> getBatchRanges() const { return m_batchRanges; }

private:
  void recomputeSortPassCount(uint32_t visibleCount);

  uint32_t                     m_maxObjects{0};
  shaderio::GPUBatchBuildStats m_stats{};
  std::vector<uint32_t>        m_visibleObjects;
  std::vector<uint32_t>        m_sortKeys;
  std::vector<BatchRange>      m_batchRanges;
};

}  // namespace demo
