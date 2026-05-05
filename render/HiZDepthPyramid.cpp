#include "HiZDepthPyramid.h"

#include <algorithm>

namespace demo {

void HiZDepthPyramid::init(VkExtent2D size)
{
  m_size = size;
  uint32_t maxDimension = std::max(size.width, size.height);
  m_mipCount = 0;
  while(maxDimension > 0)
  {
    ++m_mipCount;
    maxDimension >>= 1u;
  }
}

void HiZDepthPyramid::generate(VkCommandBuffer, TextureHandle)
{
}

void HiZDepthPyramid::bindForCulling(VkDescriptorSet, uint32_t)
{
}

}  // namespace demo
