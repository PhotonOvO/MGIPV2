#pragma once

#include "../RHIDescriptor.h"

#include <unordered_map>
#include <vector>

namespace demo {
namespace rhi {
namespace metal {

class MetalBindTableLayout final : public BindTableLayout
{
public:
  MetalBindTableLayout() = default;
  ~MetalBindTableLayout() override;

  void init(void* nativeDevice, const std::vector<BindTableLayoutEntry>& entries) override;
  void deinit() override;

  uint64_t getNativeHandle() const override;

  bool resolveLogicalIndex(ResourceIndex logicalIndex, uint32_t& outIndex) const;

  const std::vector<BindTableLayoutEntry>& entries() const;

private:
  // NOTES: Metal Argument Buffer Architecture
  // Metal uses argument buffers for bindless resource access
  //
  // Concept Mapping:
  // - BindTableLayout → MTLArgumentEncoder (describes argument buffer structure)
  // - BindTable → MTLBuffer (containing resource pointers)
  // - Logical Index → Array index in argument buffer
  //
  // Argument Buffer Structure:
  // struct ArgumentBuffer {
  //   texture2d<float> textures[MAX_TEXTURES];    // Array of texture bindings
  //   device Buffer buffers[MAX_BUFFERS];         // Array of buffer bindings
  //   sampler samplers[MAX_SAMPLERS];             // Array of sampler bindings
  // };
  //
  // Initialization:
  // 1. Create MTLArgumentEncoder from device using encoded array arguments
  // 2. Encoder describes the structure and alignment of the argument buffer
  // 3. Store logical index → offset mapping for binding resources
  //
  // Example Metal API pattern:
  // MTLArgumentDescriptor* texArgDesc = [MTLArgumentDescriptor argumentDescriptor];
  // texArgDesc.dataType = MTLDataTypeTexture;
  // texArgDesc.arrayLength = maxTextures;
  // MTLArgumentEncoder* encoder = [device newArgumentEncoderWithArguments:@[texArgDesc]];
  //
  // NOTES: Metal argument buffers support:
  // - Textures (sampled, storage)
  // - Buffers (uniform, storage, vertex, etc.)
  // - Samplers
  // - Inline data (constant values)

  void* m_device{nullptr};   // id<MTLDevice>
  void* m_encoder{nullptr};  // id<MTLArgumentEncoder>

  std::unordered_map<ResourceIndex, uint32_t> m_logicalToIndex;
  std::vector<BindTableLayoutEntry>           m_entries;

  uint32_t m_argumentBufferSize{0};  // Total size needed for argument buffer
};

class MetalBindTable final : public BindTable
{
public:
  MetalBindTable() = default;
  ~MetalBindTable() override;

  void init(void* nativeDevice, const BindTableLayout& layout, uint32_t maxLogicalEntries) override;
  void deinit() override;

  void update(uint32_t writeCount, const BindTableWrite* writes) override;

  uint64_t getNativeHandle() const override;

private:
  // NOTES: Metal Bind Table Implementation
  // Bind tables are backed by MTLBuffer containing argument buffer data
  //
  // Update Flow:
  // 1. Get MTLArgumentEncoder from layout
  // 2. For each BindTableWrite:
  //    - Calculate offset using encoder.encodedBufferAlignment
  //    - Use encoder to set resource in argument buffer:
  //      [encoder setTexture:texture atIndex:index]
  //      [encoder setBuffer:buffer offset:0 atIndex:index]
  // 3. Mark argument buffer as dirty (if using staging)
  //
  // Shader Access:
  // - Argument buffer is bound as a regular buffer to shader
  // - Shader declares argument buffer as [[buffer(N)]] or uses Metal 3+ bindless
  // - Resources accessed via array indexing: textures[index]
  //
  // Example Metal API pattern:
  // id<MTLBuffer> argBuffer = [device newBufferWithLength:encoder.encodedLength options:0];
  // [encoder setArgumentBuffer:argBuffer offset:0];
  // for (size_t i = 0; i < writeCount; ++i) {
  //   if (writes[i].resourceType == BindlessResourceType::sampledTexture) {
  //     [encoder setTexture:textures[i] atIndex:writes[i].dstIndex];
  //   }
  // }

  void* m_device{nullptr};  // id<MTLDevice>
  void* m_buffer{nullptr};  // id<MTLBuffer> (argument buffer)

  const MetalBindTableLayout* m_layoutView{nullptr};
  uint32_t                    m_maxLogicalEntries{0};
  uint32_t                    m_bufferSize{0};

  bool m_needsUpdate{false};
};

}  // namespace metal
}  // namespace rhi
}  // namespace demo