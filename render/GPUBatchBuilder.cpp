#include "GPUBatchBuilder.h"

#include <algorithm>

namespace demo {

void GPUBatchBuilder::init(uint32_t maxObjects)
{
  m_maxObjects = maxObjects;
  m_stats = {};
}

void GPUBatchBuilder::buildBatches(VkCommandBuffer, uint32_t visibleCount)
{
  m_stats.visibleCount = visibleCount;
  m_stats.batchCount = visibleCount;

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
