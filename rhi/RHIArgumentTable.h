#pragma once

#include "RHIHandles.h"
#include "RHITypes.h"

#include <cstdint>

namespace demo::rhi {

// Unifies Vulkan descriptor set / DX12 descriptor heap range / Metal 4
// MTL4ArgumentTable. Writes only accept RHI handles, never native descriptors.
enum class ArgumentType : uint8_t
{
  uniformBuffer = 0,
  storageBuffer,
  sampledTexture,
  storageTexture,
  sampler,
  accelerationStructure,
  indirectCommandBuffer,
};

struct ArgumentBinding
{
  uint32_t     binding{0};
  ArgumentType type{ArgumentType::uniformBuffer};
  ShaderStage  visibility{ShaderStage::none};
  uint32_t     arrayCount{1};
  bool         bindless{false};
  bool         dynamicOffset{false};
};

struct ArgumentLayoutDesc
{
  const ArgumentBinding* bindings{nullptr};
  uint32_t               bindingCount{0};
  const char*            debugName{nullptr};
};

struct ArgumentWrite
{
  uint32_t          binding{0};
  uint32_t          arrayElement{0};
  ArgumentType      type{ArgumentType::uniformBuffer};
  BufferHandle      buffer{};
  TextureViewHandle textureView{};
  SamplerHandle     sampler{};
  uint64_t          offset{0};
  uint64_t          size{0};  // 0 = entire buffer
};

}  // namespace demo::rhi
