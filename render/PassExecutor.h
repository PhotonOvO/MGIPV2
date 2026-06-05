#pragma once

#include "Pass.h"
#include "../rhi/RHITypes.h"

#include <cstddef>
#include <vector>

namespace demo::rhi::vulkan {
class VulkanResourceTable;
}

namespace demo {

// Forward declaration for Tracy GPU context
namespace profiling {
class TracyVulkanContext;
}

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
    // Backend registry handle mirroring nativeImage, so the Wave 7 resourceBarrier
    // path can resolveTexture() the pass attachments. Filled in bindTexture when a
    // resource table is set; null otherwise.
    rhi::TextureHandle rhiTexture{};
  };

  struct BufferBinding
  {
    BufferHandle handle{};
    uint64_t     nativeBuffer{0};
  };

  void                 clear();
  void                 addPass(const PassNode& pass);
  // Optional: when set, bindTexture mirrors each native image into the backend
  // registry so pass attachments are resolvable as TextureHandles.
  void                 setResourceTable(rhi::vulkan::VulkanResourceTable* table);
  void                 clearResourceBindings();
  void                 bindTexture(TextureBinding binding);
  void                 bindBuffer(BufferBinding binding);
  [[nodiscard]] size_t getPassCount() const;
  [[nodiscard]] const PassNode* getPass(size_t index) const;
  // Resolvable backend handle mirroring a bound pass attachment (null if unbound
  // or no resource table was set). Used by present to blit through the registry.
  [[nodiscard]] rhi::TextureHandle getTextureRHIHandle(TextureHandle handle) const;
  void                 execute(const PassContext& context, const ExecutionHooks* hooks = nullptr,
                               profiling::TracyVulkanContext* tracyVkCtx = nullptr) const;

private:
  [[nodiscard]] const TextureBinding* findTextureBinding(TextureHandle handle) const;
  [[nodiscard]] const BufferBinding*  findBufferBinding(BufferHandle handle) const;

  std::vector<const PassNode*>      m_passes;
  std::vector<TextureBinding>       m_textureBindings;
  std::vector<BufferBinding>        m_bufferBindings;
  rhi::vulkan::VulkanResourceTable* m_resourceTable{nullptr};
};

}  // namespace demo
