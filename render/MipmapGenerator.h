#pragma once

#include "../rhi/RHICommandBuffer.h"

namespace demo {

class MipmapGenerator
{
public:
  [[nodiscard]] static uint32_t calculateMipLevelCount(uint32_t width, uint32_t height);

  static void generateMipmaps(rhi::CommandBuffer& cmd,
                              rhi::TextureHandle image,
                              uint32_t        width,
                              uint32_t        height,
                              uint32_t        mipLevels);
};

}  // namespace demo
