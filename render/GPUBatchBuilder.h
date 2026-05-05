#pragma once

#include "../common/Common.h"

namespace demo {

class GPUBatchBuilder
{
public:
  void init(uint32_t maxObjects);
  void buildBatches(VkCommandBuffer cmd, uint32_t visibleCount);
  [[nodiscard]] const shaderio::GPUBatchBuildStats& getStats() const { return m_stats; }

private:
  uint32_t                    m_maxObjects{0};
  shaderio::GPUBatchBuildStats m_stats{};
};

}  // namespace demo
