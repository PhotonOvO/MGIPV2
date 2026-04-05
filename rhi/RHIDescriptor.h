#pragma once

#include "RHIBindlessTypes.h"
#include "RHIHandles.h"

#include <cstdint>
#include <vector>

namespace demo::rhi {

enum class DescriptorKind : uint8_t
{
  Sampler = 0,
  CombinedImageSampler,
  SampledImage,
  StorageImage,
  UniformTexelBuffer,
  StorageTexelBuffer,
  UniformBuffer,
  StorageBuffer,
  UniformBufferDynamic,
  StorageBufferDynamic,
  InputAttachment,
};

[[nodiscard]] inline constexpr DescriptorKind toDescriptorType(BindlessResourceType type)
{
  switch(type)
  {
    case BindlessResourceType::sampler:
      return DescriptorKind::Sampler;
    case BindlessResourceType::sampledTexture:
      return DescriptorKind::CombinedImageSampler;
    case BindlessResourceType::storageTexture:
      return DescriptorKind::StorageImage;
    case BindlessResourceType::uniformBuffer:
      return DescriptorKind::UniformBuffer;
    case BindlessResourceType::storageBuffer:
      return DescriptorKind::StorageBuffer;
    case BindlessResourceType::uniformTexelBuffer:
      return DescriptorKind::UniformTexelBuffer;
    case BindlessResourceType::storageTexelBuffer:
      return DescriptorKind::StorageTexelBuffer;
    case BindlessResourceType::uniformBufferDynamic:
      return DescriptorKind::UniformBufferDynamic;
    case BindlessResourceType::storageBufferDynamic:
      return DescriptorKind::StorageBufferDynamic;
    default:
      return DescriptorKind::CombinedImageSampler;
  }
}

enum class InvalidIndexBehavior : uint8_t
{
  assertOnWrite = 0,
  undefinedShaderRead,
};

enum class UpdateVisibilityRule : uint8_t
{
  immediate = 0,
  deferredUntilSubmit,
};

enum class IndexStabilityRule : uint8_t
{
  stableUntilExplicitRebindOrDestroy = 0,
};

struct BindTableContract
{
  static constexpr ResourceIndex        invalidIndex       = kInvalidResourceIndex;
  static constexpr InvalidIndexBehavior invalidIndexRule   = InvalidIndexBehavior::assertOnWrite;
  static constexpr UpdateVisibilityRule visibilityRule     = UpdateVisibilityRule::immediate;
  static constexpr IndexStabilityRule   indexStabilityRule = IndexStabilityRule::stableUntilExplicitRebindOrDestroy;
};

struct BindTableLayoutEntry
{
  ResourceIndex        logicalIndex{kInvalidResourceIndex};
  BindlessResourceType resourceType{BindlessResourceType::sampledTexture};
  uint32_t             descriptorCount{1};
  ResourceVisibility   visibility{ResourceVisibility::all};
};

class BindTableLayout
{
public:
  virtual ~BindTableLayout() = default;

  virtual void init(void* nativeDevice, const std::vector<BindTableLayoutEntry>& entries) = 0;
  virtual void deinit()                                                                   = 0;

  virtual uint64_t getNativeHandle() const = 0;
};

struct BindlessResourceRef
{
  BufferHandle       buffer{};
  TextureHandle      texture{};
  ResourceViewHandle view{};
  SamplerHandle      sampler{};
};

struct DescriptorImageInfo
{
  uint64_t sampler{0};
  uint64_t imageView{0};
  uint32_t imageLayout{0};
};

struct DescriptorBufferInfo
{
  uint64_t buffer{0};
  uint64_t offset{0};
  uint64_t range{0};
};

struct BindTableWrite
{
  ResourceIndex               dstIndex{kInvalidResourceIndex};
  uint32_t                    dstArrayElement{0};
  BindlessResourceType        resourceType{BindlessResourceType::sampledTexture};
  uint32_t                    descriptorCount{0};
  const BindlessResourceRef*  resources{nullptr};
  const DescriptorImageInfo*  pImageInfo{nullptr};
  const DescriptorBufferInfo* pBufferInfo{nullptr};
  ResourceVisibility          visibility{ResourceVisibility::all};
  BindlessUpdateFlags         updateFlags{BindlessUpdateFlags::immediateVisibility};
};

class BindTable
{
public:
  virtual ~BindTable() = default;

  virtual void init(void* nativeDevice, const BindTableLayout& layout, uint32_t maxLogicalEntries) = 0;
  virtual void deinit()                                                                            = 0;

  virtual void update(uint32_t writeCount, const BindTableWrite* writes) = 0;

  virtual uint64_t getNativeHandle() const = 0;
};

}  // namespace demo::rhi
