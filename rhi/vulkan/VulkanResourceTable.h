#pragma once

#include "../../common/Handles.h"
#include "../../common/HandlePool.h"

#include <cstdint>
#include <unordered_map>
#include <utility>

namespace demo::rhi {
class BindTable;
}

namespace demo::rhi::vulkan {

// Native image view backing an opaque TextureViewHandle. `owned` views are created by
// the device (vkCreateImageView) and destroyed by it; adopted views (e.g. swapchain)
// are not destroyed through the registry.
struct TextureViewRecord
{
  uint64_t nativeView{0};
  bool     owned{true};
};

// Native image + VMA allocation backing an opaque TextureHandle. `owned` images are
// created by the device (vmaCreateImage) and destroyed by it; adopted images (e.g.
// swapchain) are not destroyed through the registry.
struct TextureRecord
{
  uint64_t nativeImage{0};
  uint64_t nativeAllocation{0};
  bool     owned{true};
};

// Native pipeline objects backing an opaque PipelineHandle.
struct PipelineRecord
{
  uint32_t bindPoint{0};
  uint64_t nativePipeline{0};
  uint32_t specializationVariant{0};
  uint64_t nativeLayout{0};
  // When false the native pipeline is owned by another subsystem; the registry
  // resolves it for command recording but must not destroy it.
  bool     owned{true};
};

// Native buffer backing an opaque BufferHandle. Hot fields (nativeBuffer,
// gpuAddress, mapped) are read on the recording path; the rest is cold.
struct BufferRecord
{
  uint64_t nativeBuffer{0};      // VkBuffer
  uint64_t nativeAllocation{0};  // VmaAllocation
  uint64_t gpuAddress{0};        // VkDeviceAddress (0 if not requested)
  void*    mapped{nullptr};      // persistent mapping for CPU-visible buffers
  bool     owned{true};
};

struct SamplerRecord
{
  uint64_t nativeSampler{0};  // VkSampler
};

struct QueryPoolRecord
{
  uint64_t nativePool{0};  // VkQueryPool
  uint32_t count{0};
};

struct ArgumentLayoutRecord
{
  uint64_t nativeLayout{0};  // VkDescriptorSetLayout
};

struct ArgumentTableRecord
{
  uint64_t nativeSet{0};  // VkDescriptorSet (allocated from the device argument pool)
};

// Backend-owned table mapping opaque RHI handles to native Vulkan objects.
//
// The command list resolves handles by a direct in-layer lookup here, instead of
// up-calling a render-layer resolver interface. The render layer owns an instance
// of this table, registers its pipelines/bind-groups into it, and hands the
// command list a pointer to it for recording.
class VulkanResourceTable
{
public:
  // --- Pipelines (this table owns the handle allocation) ---
  PipelineHandle registerPipeline(uint32_t bindPoint, uint64_t nativePipeline, uint32_t specializationVariant,
                                  uint64_t nativeLayout, bool owned = true);
  [[nodiscard]] const PipelineRecord* tryGetPipeline(PipelineHandle handle) const;
  void                                destroyPipeline(PipelineHandle handle);

  template <typename Fn>
  void forEachPipeline(Fn&& fn)
  {
    m_pipelines.forEachActive(std::forward<Fn>(fn));
  }

  // --- Texture views (this table owns the handle allocation; native lifetime is the
  // caller's: it passes owned=true for device-created views so destroyTextureView can
  // report them back for vkDestroyImageView). Pure mapping — no Vulkan calls here. ---
  TextureViewHandle                       registerTextureView(uint64_t nativeView, bool owned);
  [[nodiscard]] uint64_t                  resolveTextureView(TextureViewHandle handle) const;
  [[nodiscard]] const TextureViewRecord*  tryGetTextureView(TextureViewHandle handle) const;
  // Removes the entry and returns the record it held (so the caller can vkDestroy owned views).
  TextureViewRecord                       removeTextureView(TextureViewHandle handle);

  template <typename Fn>
  void forEachTextureView(Fn&& fn)
  {
    m_textureViews.forEachActive(std::forward<Fn>(fn));
  }

  // --- Textures (images). This table owns the handle allocation; native lifetime is the
  // caller's (owned=true for device-created images so removeTexture reports them back for
  // vmaDestroyImage). Pure mapping — no Vulkan/VMA calls here. ---
  TextureHandle                    registerTexture(uint64_t nativeImage, uint64_t nativeAllocation, bool owned);
  [[nodiscard]] uint64_t           resolveTexture(TextureHandle handle) const;
  [[nodiscard]] const TextureRecord* tryGetTexture(TextureHandle handle) const;
  TextureRecord                    removeTexture(TextureHandle handle);

  template <typename Fn>
  void forEachTexture(Fn&& fn)
  {
    m_textures.forEachActive(std::forward<Fn>(fn));
  }

  // --- Native-object resolution used during command recording ---
  [[nodiscard]] uint64_t resolvePipeline(PipelineHandle handle, uint32_t expectedBindPoint) const;
  [[nodiscard]] uint64_t resolvePipelineLayout(PipelineHandle handle) const;
  [[nodiscard]] uint64_t resolveBindGroupDescriptorSet(BindGroupHandle handle) const;

  // --- Bind-group descriptor-set mirror ---
  // The render layer still owns the bind-group pool; only the native descriptor
  // lookup is mirrored here so the command list can resolve it in-layer. The
  // BindTable pointer is read live at bind time (its descriptor set may change).
  void registerBindGroup(BindGroupHandle handle, BindTable* table);
  void unregisterBindGroup(BindGroupHandle handle);

  // --- Buffers (this table owns the handle allocation; native lifetime is the
  // caller's, owned=true for device-created buffers). Pure mapping. ---
  BufferHandle                     registerBuffer(const BufferRecord& record);
  // Rebinds a stable handle to a new native buffer (Option B: arena/per-frame realloc).
  void                             updateBuffer(BufferHandle handle, uint64_t nativeBuffer, uint64_t gpuAddress = 0);
  [[nodiscard]] uint64_t           resolveBuffer(BufferHandle handle) const;
  [[nodiscard]] const BufferRecord* tryGetBuffer(BufferHandle handle) const;
  BufferRecord                     removeBuffer(BufferHandle handle);

  // --- Samplers ---
  SamplerHandle                    registerSampler(uint64_t nativeSampler);
  [[nodiscard]] uint64_t           resolveSampler(SamplerHandle handle) const;
  SamplerRecord                    removeSampler(SamplerHandle handle);

  // --- Query pools ---
  QueryPoolHandle                  registerQueryPool(uint64_t nativePool, uint32_t count);
  [[nodiscard]] uint64_t           resolveQueryPool(QueryPoolHandle handle) const;
  QueryPoolRecord                  removeQueryPool(QueryPoolHandle handle);

  // --- Argument layouts / tables ---
  ArgumentLayoutHandle             registerArgumentLayout(uint64_t nativeLayout);
  [[nodiscard]] uint64_t           resolveArgumentLayout(ArgumentLayoutHandle handle) const;
  ArgumentLayoutRecord             removeArgumentLayout(ArgumentLayoutHandle handle);

  ArgumentTableHandle              registerArgumentTable(uint64_t nativeSet);
  [[nodiscard]] uint64_t           resolveArgumentTable(ArgumentTableHandle handle) const;
  ArgumentTableRecord              removeArgumentTable(ArgumentTableHandle handle);

private:
  HandlePool<PipelineHandle, PipelineRecord>          m_pipelines;
  HandlePool<TextureViewHandle, TextureViewRecord>    m_textureViews;
  HandlePool<TextureHandle, TextureRecord>            m_textures;
  HandlePool<BufferHandle, BufferRecord>              m_buffers;
  HandlePool<SamplerHandle, SamplerRecord>            m_samplers;
  HandlePool<QueryPoolHandle, QueryPoolRecord>        m_queryPools;
  HandlePool<ArgumentLayoutHandle, ArgumentLayoutRecord> m_argumentLayouts;
  HandlePool<ArgumentTableHandle, ArgumentTableRecord>   m_argumentTables;
  std::unordered_map<uint64_t, BindTable*>            m_bindGroupTables;
};

}  // namespace demo::rhi::vulkan
