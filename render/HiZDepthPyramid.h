#pragma once

#include "../common/Common.h"
#include "../common/Handles.h"

namespace demo {

class HiZDepthPyramid
{
public:
  void init(VkExtent2D size);
  void generate(VkCommandBuffer cmd, TextureHandle sourceDepth);
  void bindForCulling(VkDescriptorSet set, uint32_t binding);
  [[nodiscard]] uint32_t getMipCount() const { return m_mipCount; }

private:
  VkExtent2D m_size{};
  uint32_t   m_mipCount{0};
};

}  // namespace demo
