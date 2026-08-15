#pragma once

#include "../../common/HandlePool.h"
#include "../RHIArgumentTable.h"
#include "../RHIPipeline.h"
#include "../RHIResidency.h"
#include "../RHIResourceLifetime.h"

#include <cstdint>
#include <limits>
#include <string>
#include <utility>
#include <vector>

namespace MTL {
class ArgumentEncoder;
class Buffer;
class ComputePipelineState;
class DepthStencilState;
class Library;
class RenderPipelineState;
class ResidencySet;
class SamplerState;
class Texture;
}

namespace MTL4 {
class CounterHeap;
}

namespace demo::rhi::metal {

inline constexpr uint32_t kInvalidMetalBinding = std::numeric_limits<uint32_t>::max();

struct MetalBufferRecord
{
  MTL::Buffer* nativeBuffer{nullptr};
  BufferDesc desc{};
  std::string debugName;
  SubmissionTokenSet pendingUses{};
  bool owned{true};
};

struct MetalTextureRecord
{
  MTL::Texture* nativeTexture{nullptr};
  TextureDesc desc{};
  std::string debugName;
  SubmissionTokenSet pendingUses{};
  bool owned{true};
};

struct MetalTextureViewRecord
{
  MTL::Texture* nativeTextureView{nullptr};
  TextureHandle parentTexture{};
  TextureViewCreateDesc desc{};
  uint64_t resourceId{0};
  uint32_t poolIndex{kInvalidMetalBinding};
  std::string debugName;
  SubmissionTokenSet pendingUses{};
  bool ownsNativeView{false};
};

struct MetalSamplerRecord
{
  MTL::SamplerState* nativeSampler{nullptr};
  std::string debugName;
  SubmissionTokenSet pendingUses{};
};

struct MetalQueryPoolRecord
{
  MTL4::CounterHeap* nativeHeap{nullptr};
  uint32_t count{0};
  SubmissionTokenSet pendingUses{};
};

struct MetalShaderLibraryRecord
{
  MTL::Library* nativeLibrary{nullptr};
  MTL::Library* icbLibrary{nullptr};
  ShaderIRFormat format{ShaderIRFormat::unknown};
  std::string debugName;
  SubmissionTokenSet pendingUses{};
};

struct MetalArgumentLayoutRecord
{
  std::vector<ArgumentBinding> bindings;
  std::string debugName;
};

struct MetalArgumentValue
{
  uint32_t binding{0};
  uint32_t arrayElement{0};
  ArgumentType type{ArgumentType::uniformBuffer};
  BufferHandle buffer{};
  TextureViewHandle textureView{};
  SamplerHandle sampler{};
  uint64_t offset{0};
  uint64_t size{0};
  ArgumentAccessIntent accessIntent{ArgumentAccessIntent::sampledRead};
  bool written{false};
};

struct MetalArgumentTableRecord
{
  ArgumentLayoutHandle layout{};
  ArgumentTableLifetime lifetime{ArgumentTableLifetime::persistent};
  std::vector<MetalArgumentValue> values;
  std::string debugName;
  uint64_t version{1};
  SubmissionTokenSet pendingUses{};
};

struct MetalArgumentPlacement
{
  uint32_t tableSlot{0};
  ArgumentLayoutHandle layout{};
  uint32_t binding{0};
  ArgumentType type{ArgumentType::uniformBuffer};
  ShaderStage visibility{ShaderStage::none};
  uint32_t arrayCount{1};
  bool dynamicOffset{false};
  uint32_t bufferIndex{kInvalidMetalBinding};
  uint32_t textureIndex{kInvalidMetalBinding};
  uint32_t samplerIndex{kInvalidMetalBinding};
  uint32_t nestedArgumentIndex{kInvalidMetalBinding};
};

struct MetalRootPlacement
{
  uint32_t slot{0};
  RootBindingKind kind{RootBindingKind::constants};
  ShaderStage visibility{ShaderStage::none};
  uint32_t size{0};
  uint32_t alignment{0};
  DynamicBufferBindingDesc dynamicBuffer{};
  uint32_t bufferIndex{kInvalidMetalBinding};
};

struct MetalVertexPlacement
{
  uint32_t binding{0};
  uint32_t bufferIndex{kInvalidMetalBinding};
  uint32_t stride{0};
};

struct MetalPipelineBindingLayout
{
  std::vector<MetalArgumentPlacement> arguments;
  std::vector<MetalRootPlacement> roots;
  std::vector<MetalVertexPlacement> vertexBuffers;
  uint32_t bufferCount{0};
  uint32_t textureCount{0};
  uint32_t samplerCount{0};
  uint32_t nestedArgumentBufferIndex{kInvalidMetalBinding};
  ShaderStage stages{ShaderStage::none};

  [[nodiscard]] const MetalArgumentPlacement* findArgument(
    uint32_t tableSlot, uint32_t binding) const noexcept;
  [[nodiscard]] const MetalRootPlacement* findRoot(uint32_t slot) const noexcept;
  [[nodiscard]] const MetalVertexPlacement* findVertex(uint32_t binding) const noexcept;
};

enum class MetalPipelineKind : uint8_t
{
  graphics = 0,
  compute,
};

struct MetalPipelineRecord
{
  MetalPipelineKind kind{MetalPipelineKind::graphics};
  std::string debugName;
  MTL::RenderPipelineState* nativeRenderPipeline{nullptr};
  MTL::ComputePipelineState* nativeComputePipeline{nullptr};
  MTL::DepthStencilState* nativeDepthStencilState{nullptr};
  MTL::ArgumentEncoder* nestedArgumentEncoder{nullptr};
  MetalPipelineBindingLayout bindingLayout{};
  RasterState rasterState{};
  DepthState depthState{};
  uint32_t specializationVariant{0};
  bool supportsGpuGeneratedCommands{false};
  uint32_t computeThreadExecutionWidth{1};
  uint32_t computeMaxThreadsPerThreadgroup{1};
  Extent3D computeThreadsPerThreadgroup{1, 1, 1};
  SubmissionTokenSet pendingUses{};
};

struct MetalResidencySetRecord
{
  MTL::ResidencySet* nativeSet{nullptr};
  uint32_t maxResources{0};
  std::vector<ResidencyResource> resources;
  SubmissionTokenSet pendingUses{};
};

class MetalResourceTable
{
public:
  BufferHandle registerBuffer(MetalBufferRecord record);
  [[nodiscard]] MetalBufferRecord* tryGetBuffer(BufferHandle handle);
  [[nodiscard]] const MetalBufferRecord* tryGetBuffer(BufferHandle handle) const;
  MetalBufferRecord removeBuffer(BufferHandle handle);

  TextureHandle registerTexture(MetalTextureRecord record);
  [[nodiscard]] MetalTextureRecord* tryGetTexture(TextureHandle handle);
  [[nodiscard]] const MetalTextureRecord* tryGetTexture(TextureHandle handle) const;
  MetalTextureRecord removeTexture(TextureHandle handle);

  TextureViewHandle registerTextureView(MetalTextureViewRecord record);
  [[nodiscard]] MetalTextureViewRecord* tryGetTextureView(TextureViewHandle handle);
  [[nodiscard]] const MetalTextureViewRecord* tryGetTextureView(TextureViewHandle handle) const;
  MetalTextureViewRecord removeTextureView(TextureViewHandle handle);

  SamplerHandle registerSampler(MetalSamplerRecord record);
  [[nodiscard]] MetalSamplerRecord* tryGetSampler(SamplerHandle handle);
  [[nodiscard]] const MetalSamplerRecord* tryGetSampler(SamplerHandle handle) const;
  MetalSamplerRecord removeSampler(SamplerHandle handle);

  QueryPoolHandle registerQueryPool(MetalQueryPoolRecord record);
  [[nodiscard]] MetalQueryPoolRecord* tryGetQueryPool(QueryPoolHandle handle);
  [[nodiscard]] const MetalQueryPoolRecord* tryGetQueryPool(QueryPoolHandle handle) const;
  MetalQueryPoolRecord removeQueryPool(QueryPoolHandle handle);

  ShaderLibraryHandle registerShaderLibrary(MetalShaderLibraryRecord record);
  [[nodiscard]] MetalShaderLibraryRecord* tryGetShaderLibrary(ShaderLibraryHandle handle);
  [[nodiscard]] const MetalShaderLibraryRecord* tryGetShaderLibrary(ShaderLibraryHandle handle) const;
  MetalShaderLibraryRecord removeShaderLibrary(ShaderLibraryHandle handle);

  ArgumentLayoutHandle registerArgumentLayout(MetalArgumentLayoutRecord record);
  [[nodiscard]] MetalArgumentLayoutRecord* tryGetArgumentLayout(ArgumentLayoutHandle handle);
  [[nodiscard]] const MetalArgumentLayoutRecord* tryGetArgumentLayout(ArgumentLayoutHandle handle) const;
  MetalArgumentLayoutRecord removeArgumentLayout(ArgumentLayoutHandle handle);

  ArgumentTableHandle registerArgumentTable(MetalArgumentTableRecord record);
  [[nodiscard]] MetalArgumentTableRecord* tryGetArgumentTable(ArgumentTableHandle handle);
  [[nodiscard]] const MetalArgumentTableRecord* tryGetArgumentTable(ArgumentTableHandle handle) const;
  MetalArgumentTableRecord removeArgumentTable(ArgumentTableHandle handle);

  PipelineHandle registerPipeline(MetalPipelineRecord record);
  [[nodiscard]] MetalPipelineRecord* tryGetPipeline(PipelineHandle handle);
  [[nodiscard]] const MetalPipelineRecord* tryGetPipeline(PipelineHandle handle) const;
  MetalPipelineRecord removePipeline(PipelineHandle handle);

  ResidencySetHandle registerResidencySet(MetalResidencySetRecord record);
  [[nodiscard]] MetalResidencySetRecord* tryGetResidencySet(ResidencySetHandle handle);
  [[nodiscard]] const MetalResidencySetRecord* tryGetResidencySet(ResidencySetHandle handle) const;
  MetalResidencySetRecord removeResidencySet(ResidencySetHandle handle);

  [[nodiscard]] bool isAlive(ResidencyResource resource) const;

  template <typename Fn> void forEachBuffer(Fn&& fn) { m_buffers.forEachActive(std::forward<Fn>(fn)); }
  template <typename Fn> void forEachTexture(Fn&& fn) { m_textures.forEachActive(std::forward<Fn>(fn)); }
  template <typename Fn> void forEachTextureView(Fn&& fn) { m_textureViews.forEachActive(std::forward<Fn>(fn)); }
  template <typename Fn> void forEachSampler(Fn&& fn) { m_samplers.forEachActive(std::forward<Fn>(fn)); }
  template <typename Fn> void forEachQueryPool(Fn&& fn) { m_queryPools.forEachActive(std::forward<Fn>(fn)); }
  template <typename Fn> void forEachShaderLibrary(Fn&& fn) { m_shaderLibraries.forEachActive(std::forward<Fn>(fn)); }
  template <typename Fn> void forEachArgumentLayout(Fn&& fn) { m_argumentLayouts.forEachActive(std::forward<Fn>(fn)); }
  template <typename Fn> void forEachArgumentTable(Fn&& fn) { m_argumentTables.forEachActive(std::forward<Fn>(fn)); }
  template <typename Fn> void forEachPipeline(Fn&& fn) { m_pipelines.forEachActive(std::forward<Fn>(fn)); }
  template <typename Fn> void forEachResidencySet(Fn&& fn) { m_residencySets.forEachActive(std::forward<Fn>(fn)); }

private:
  demo::HandlePool<BufferHandle, MetalBufferRecord> m_buffers;
  demo::HandlePool<TextureHandle, MetalTextureRecord> m_textures;
  demo::HandlePool<TextureViewHandle, MetalTextureViewRecord> m_textureViews;
  demo::HandlePool<SamplerHandle, MetalSamplerRecord> m_samplers;
  demo::HandlePool<QueryPoolHandle, MetalQueryPoolRecord> m_queryPools;
  demo::HandlePool<ShaderLibraryHandle, MetalShaderLibraryRecord> m_shaderLibraries;
  demo::HandlePool<ArgumentLayoutHandle, MetalArgumentLayoutRecord> m_argumentLayouts;
  demo::HandlePool<ArgumentTableHandle, MetalArgumentTableRecord> m_argumentTables;
  demo::HandlePool<PipelineHandle, MetalPipelineRecord> m_pipelines;
  demo::HandlePool<ResidencySetHandle, MetalResidencySetRecord> m_residencySets;
};

}  // namespace demo::rhi::metal
