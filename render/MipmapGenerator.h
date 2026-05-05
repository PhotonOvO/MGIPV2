#pragma once

#include "../common/Common.h"

namespace demo {

class MipmapGenerator
{
public:
  [[nodiscard]] static uint32_t calculateMipLevelCount(uint32_t width, uint32_t height);

  static void generateMipmaps(VkCommandBuffer cmd,
                              VkImage         image,
                              VkFormat        format,
                              uint32_t        width,
                              uint32_t        height,
                              uint32_t        mipLevels);
};

}  // namespace demo
