#pragma once

#include "Pass.h"
#include "../rhi/RHITypes.h"

#include <cstddef>
#include <vector>

namespace demo {

class PassExecutor
{
public:
  struct ExecutionHooks
  {
    virtual ~ExecutionHooks() = default;
    virtual void beforePass(const PassContext& context, const PassNode& pass, uint32_t passIndex) const
    {
      (void)context;
      (void)pass;
      (void)passIndex;
    }
    virtual void afterPass(const PassContext& context, const PassNode& pass, uint32_t passIndex) const
    {
      (void)context;
      (void)pass;
      (void)passIndex;
    }
  };

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
  [[nodiscard]] const PassNode* getPass(size_t index) const;
  void                 execute(const PassContext& context, const ExecutionHooks* hooks = nullptr) const;

private:
  [[nodiscard]] const TextureBinding* findTextureBinding(TextureHandle handle) const;
  [[nodiscard]] const BufferBinding*  findBufferBinding(BufferHandle handle) const;

  std::vector<const PassNode*> m_passes;
  std::vector<TextureBinding>  m_textureBindings;
  std::vector<BufferBinding>   m_bufferBindings;
};

}  // namespace demo
