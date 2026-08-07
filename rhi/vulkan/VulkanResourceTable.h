#pragma once

#include "../../common/Handles.h"
#include "../../common/HandlePool.h"
#include "../RHIArgumentTable.h"
#include "../RHIResourceLifetime.h"
#include "../RHIPipeline.h"
#include "../RHIResidency.h"

#include <cstdint>
#include <string>
#include <utility>
#include <vector>
#include <vulkan/vulkan.h>

using VmaAllocation = struct VmaAllocation_T*;

namespace demo::rhi::vulkan {

struct TextureViewHotRecord
{
  VkImageView nativeView{VK_NULL_HANDLE};
  SubmissionTokenSet pendingUses{};
};

struct TextureViewColdRecord
{
  TextureViewCreateDesc desc{};
  TextureHandle parentTexture{};
  std::string debugName;
  bool owned{true};
};

struct TextureViewRecord
{
  TextureViewHotRecord hot{};
  TextureViewColdRecord cold{};
};

struct TextureHotRecord
{
  VkImage nativeImage{VK_NULL_HANDLE};
  SubmissionTokenSet pendingUses{};
};

struct TextureColdRecord
{
  VmaAllocation nativeAllocation{nullptr};
  TextureDesc desc{};
  std::string debugName;
  bool owned{true};
};

struct TextureRecord
{
  TextureHotRecord hot{};
  TextureColdRecord cold{};
};

// Native pipeline objects backing an opaque PipelineHandle.
struct PipelineRecord
{
  VkPipelineBindPoint bindPoint{VK_PIPELINE_BIND_POINT_GRAPHICS};
  VkPipeline nativePipeline{VK_NULL_HANDLE};
  uint32_t specializationVariant{0};
  VkPipelineLayout nativeLayout{VK_NULL_HANDLE};
  struct RootBindingLowering
  {
    uint32_t slot{0};
    uint32_t offset{0};
    uint32_t size{0};
    uint32_t kind{0};
    uint32_t stages{0};
  };
  std::vector<RootBindingLowering> rootBindings;
  bool     ownsLayout{false};
  // When false the native pipeline is owned by another subsystem; the registry
  // resolves it for command recording but must not destroy it.
  bool     owned{true};
  SubmissionTokenSet pendingUses{};
};

struct BufferHotRecord
{
  VkBuffer nativeBuffer{VK_NULL_HANDLE};
  VkDeviceAddress gpuAddress{0};
  void* mapped{nullptr};
  SubmissionTokenSet pendingUses{};
};

struct BufferColdRecord
{
  VmaAllocation nativeAllocation{nullptr};
  BufferDesc desc{};
  std::string debugName;
  bool hostCoherent{false};
  bool owned{true};
};

struct BufferRecord
{
  BufferHotRecord hot{};
  BufferColdRecord cold{};
};

struct SamplerRecord
{
  VkSampler nativeSampler{VK_NULL_HANDLE};
  SubmissionTokenSet pendingUses{};
};

struct QueryPoolRecord
{
  VkQueryPool nativePool{VK_NULL_HANDLE};
  uint32_t count{0};
  SubmissionTokenSet pendingUses{};
};

struct ShaderLibraryRecord
{
  VkShaderModule nativeModule{VK_NULL_HANDLE};
  ShaderIRFormat format{ShaderIRFormat::unknown};
};

struct ArgumentLayoutRecord
{
  VkDescriptorSetLayout nativeLayout{VK_NULL_HANDLE};
  // Binding numbers declared with dynamicOffset in the layout. updateArgumentTable
  // must emit the *_DYNAMIC descriptor type for these so the write matches the layout.
  std::vector<uint32_t> dynamicBindings;
};

struct ResidencySetRecord
{
  uint32_t maxResources{0};
  std::vector<ResidencyResource> resources;
  SubmissionTokenSet pendingUses{};
};

struct ArgumentResourceReference
{
  uint32_t binding{0};
  uint32_t arrayElement{0};
  ResidencyResource resource{};
};

struct ArgumentTableRecord
{
  VkDescriptorSet       nativeSet{VK_NULL_HANDLE};
  ArgumentLayoutHandle layout{};      // source layout, used to resolve per-binding dynamic-ness on update
  ArgumentTableLifetime lifetime{ArgumentTableLifetime::persistent};
  SubmissionTokenSet pendingUses{};
  std::vector<ArgumentResourceReference> referencedResources;
  // When false the descriptor set is owned by another subsystem (adopted external set);
  // the registry resolves it but must not free it back to the device argument pool.
  bool                 owned{true};
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
  PipelineHandle registerPipeline(VkPipelineBindPoint bindPoint, VkPipeline nativePipeline, uint32_t specializationVariant,
                                  VkPipelineLayout nativeLayout, std::vector<PipelineRecord::RootBindingLowering> rootBindings = {},
                                  bool owned = true, bool ownsLayout = false);
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
  TextureViewHandle registerTextureView(
    VkImageView nativeView, TextureViewColdRecord cold);
  [[nodiscard]] VkImageView resolveTextureView(TextureViewHandle handle) const;
  [[nodiscard]] const TextureViewHotRecord* tryGetTextureViewHot(
    TextureViewHandle handle) const;
  [[nodiscard]] const TextureViewColdRecord* tryGetTextureViewCold(
    TextureViewHandle handle) const;
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
  TextureHandle registerTexture(VkImage nativeImage, TextureColdRecord cold);
  [[nodiscard]] VkImage resolveTexture(TextureHandle handle) const;
  [[nodiscard]] const TextureHotRecord* tryGetTextureHot(TextureHandle handle) const;
  [[nodiscard]] const TextureColdRecord* tryGetTextureCold(TextureHandle handle) const;
  TextureRecord                    removeTexture(TextureHandle handle);

  template <typename Fn>
  void forEachTexture(Fn&& fn)
  {
    m_textures.forEachActive(std::forward<Fn>(fn));
  }

  // --- Native-object resolution used during command recording ---
  [[nodiscard]] VkPipeline resolvePipeline(PipelineHandle handle, VkPipelineBindPoint expectedBindPoint) const;
  [[nodiscard]] VkPipelineLayout resolvePipelineLayout(PipelineHandle handle) const;

  // --- Buffers (this table owns the handle allocation; native lifetime is the
  // caller's, owned=true for device-created buffers). Pure mapping. ---
  BufferHandle registerBuffer(BufferHotRecord hot, BufferColdRecord cold);
  // Rebinds a stable handle to a new native buffer (Option B: arena/per-frame realloc).
  void updateBuffer(BufferHandle handle, VkBuffer nativeBuffer,
                    VkDeviceAddress gpuAddress = 0);
  [[nodiscard]] VkBuffer resolveBuffer(BufferHandle handle) const;
  [[nodiscard]] const BufferHotRecord* tryGetBufferHot(BufferHandle handle) const;
  [[nodiscard]] const BufferColdRecord* tryGetBufferCold(BufferHandle handle) const;
  BufferRecord removeBuffer(BufferHandle handle);
  template <typename Fn>
  void forEachBuffer(Fn&& fn)
  {
    m_buffers.forEachActive(std::forward<Fn>(fn));
  }

  // --- Samplers ---
  SamplerHandle                    registerSampler(VkSampler nativeSampler);
  [[nodiscard]] VkSampler          resolveSampler(SamplerHandle handle) const;
  SamplerRecord                    removeSampler(SamplerHandle handle);
  template <typename Fn>
  void forEachSampler(Fn&& fn)
  {
    m_samplers.forEachActive(std::forward<Fn>(fn));
  }

  // --- Query pools ---
  QueryPoolHandle                  registerQueryPool(VkQueryPool nativePool, uint32_t count);
  [[nodiscard]] VkQueryPool        resolveQueryPool(QueryPoolHandle handle) const;
  QueryPoolRecord                  removeQueryPool(QueryPoolHandle handle);
  template <typename Fn>
  void forEachQueryPool(Fn&& fn)
  {
    m_queryPools.forEachActive(std::forward<Fn>(fn));
  }

  // --- Shader libraries ---
  ShaderLibraryHandle registerShaderLibrary(VkShaderModule nativeModule, ShaderIRFormat format);
  [[nodiscard]] const ShaderLibraryRecord* tryGetShaderLibrary(ShaderLibraryHandle handle) const;
  ShaderLibraryRecord removeShaderLibrary(ShaderLibraryHandle handle);
  template <typename Fn>
  void forEachShaderLibrary(Fn&& fn)
  {
    m_shaderLibraries.forEachActive(std::forward<Fn>(fn));
  }

  // --- Argument layouts / tables ---
  ArgumentLayoutHandle                       registerArgumentLayout(VkDescriptorSetLayout nativeLayout, std::vector<uint32_t> dynamicBindings);
  [[nodiscard]] VkDescriptorSetLayout        resolveArgumentLayout(ArgumentLayoutHandle handle) const;
  [[nodiscard]] const ArgumentLayoutRecord*  tryGetArgumentLayout(ArgumentLayoutHandle handle) const;
  ArgumentLayoutRecord                       removeArgumentLayout(ArgumentLayoutHandle handle);
  template <typename Fn>
  void forEachArgumentLayout(Fn&& fn)
  {
    m_argumentLayouts.forEachActive(std::forward<Fn>(fn));
  }

  ArgumentTableHandle                        registerArgumentTable(VkDescriptorSet nativeSet, ArgumentLayoutHandle layout, ArgumentTableLifetime lifetime, bool owned = true);
  [[nodiscard]] VkDescriptorSet              resolveArgumentTable(ArgumentTableHandle handle) const;
  [[nodiscard]] const ArgumentTableRecord*   tryGetArgumentTable(ArgumentTableHandle handle) const;
  [[nodiscard]] ArgumentTableRecord*         tryGetArgumentTableMutable(ArgumentTableHandle handle);
  [[nodiscard]] bool validateArgumentTableForSubmit(ArgumentTableHandle handle) const;
  void markArgumentTableSubmitted(ArgumentTableHandle handle, SubmissionToken token);
  void recordArgumentTableResource(
    ArgumentTableHandle handle, uint32_t binding, uint32_t arrayElement,
    ResidencyResource resource);
  ArgumentTableRecord removeArgumentTable(ArgumentTableHandle handle);

  void markPipelineSubmitted(PipelineHandle handle, SubmissionToken token);
  void markTextureViewSubmitted(TextureViewHandle handle, SubmissionToken token);
  void markTextureSubmitted(TextureHandle handle, SubmissionToken token);
  void markBufferSubmitted(BufferHandle handle, SubmissionToken token);
  void markSamplerSubmitted(SamplerHandle handle, SubmissionToken token);
  void markQueryPoolSubmitted(QueryPoolHandle handle, SubmissionToken token);
  void markResidencySetSubmitted(ResidencySetHandle handle, SubmissionToken token);
  [[nodiscard]] bool isAlive(ResidencyResource resource) const;

  Result<ResidencySetHandle> registerResidencySet(const ResidencySetDesc& desc);
  RHIResult removeResidencySet(ResidencySetHandle handle);
  RHIResult updateResidencySet(ResidencySetHandle handle, const ResidencyUpdateBatch& batch);
  [[nodiscard]] bool isValidResidencySet(ResidencySetHandle handle) const;
  [[nodiscard]] bool validateResidencySetForSubmit(ResidencySetHandle handle) const;
  template <typename Fn>
  void forEachArgumentTable(Fn&& fn)
  {
    m_argumentTables.forEachActive(std::forward<Fn>(fn));
  }

private:
  HandlePool<PipelineHandle, PipelineRecord>          m_pipelines;
  HandlePool<TextureViewHandle, TextureViewHotRecord> m_textureViews;
  std::vector<TextureViewColdRecord>                   m_textureViewCold;
  HandlePool<TextureHandle, TextureHotRecord>          m_textures;
  std::vector<TextureColdRecord>                       m_textureCold;
  HandlePool<BufferHandle, BufferHotRecord>            m_buffers;
  std::vector<BufferColdRecord>                        m_bufferCold;
  HandlePool<SamplerHandle, SamplerRecord>            m_samplers;
  HandlePool<QueryPoolHandle, QueryPoolRecord>        m_queryPools;
  HandlePool<ShaderLibraryHandle, ShaderLibraryRecord>  m_shaderLibraries;
  HandlePool<ArgumentLayoutHandle, ArgumentLayoutRecord> m_argumentLayouts;
  HandlePool<ArgumentTableHandle, ArgumentTableRecord>   m_argumentTables;
  HandlePool<ResidencySetHandle, ResidencySetRecord>       m_residencySets;
};

}  // namespace demo::rhi::vulkan
