#pragma once

#include "Pass.h"
#include "../rhi/RHITypes.h"

#include <cstddef>
#include <vector>

namespace demo {

class PassExecutor
{
public:
  struct TextureBinding
  {
    TextureHandle      handle{};
    uint64_t           nativeImage{0};
    rhi::TextureAspect aspect{rhi::TextureAspect::color};
    rhi::ResourceState initialState{rhi::ResourceState::general};
    bool               isSwapchain{false};
  };

  struct BufferBinding
  {
    BufferHandle handle{};
    uint64_t     nativeBuffer{0};
  };

  void                 clear();
  void                 addPass(const PassNode& pass);
  void                 clearResourceBindings();
  void                 bindTexture(TextureBinding binding);
  void                 bindBuffer(BufferBinding binding);
  [[nodiscard]] size_t getPassCount() const;
  void                 execute(const PassContext& context) const;

private:
  [[nodiscard]] const TextureBinding* findTextureBinding(TextureHandle handle) const;
  [[nodiscard]] const BufferBinding*  findBufferBinding(BufferHandle handle) const;

  std::vector<const PassNode*> m_passes;
  std::vector<TextureBinding>  m_textureBindings;
  std::vector<BufferBinding>   m_bufferBindings;
};

}  // namespace demo
