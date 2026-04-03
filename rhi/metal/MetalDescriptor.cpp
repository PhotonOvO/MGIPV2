#include "MetalDescriptor.h"

#include <cassert>

namespace demo {
namespace rhi {
namespace metal {

MetalBindTableLayout::~MetalBindTableLayout()
{
  deinit();
}

void MetalBindTableLayout::init(void* nativeDevice, const std::vector<BindTableLayoutEntry>& entries)
{
  // TODO: Metal implementation
  // NOTES:
  // 1. Store device (id<MTLDevice>)
  // 2. For each entry, create MTLArgumentDescriptor:
  //    - texture2d → MTLDataTypeTexture with arrayLength
  //    - buffer → MTLDataTypePointer or MTLDataTypeStruct
  //    - sampler → MTLDataTypeSampler
  // 3. Create MTLArgumentEncoder with argument descriptors
  // 4. Calculate encodedLength for argument buffer size
  // 5. Build m_logicalToIndex mapping for fast lookup
  //
  // Example Metal API pattern:
  // NSMutableArray* argDescs = [NSMutableArray array];
  // for (const auto& entry : entries) {
  //   MTLArgumentDescriptor* desc = [MTLArgumentDescriptor argumentDescriptor];
  //   desc.dataType = mapToMetalDataType(entry.resourceType);
  //   desc.arrayLength = entry.descriptorCount;
  //   desc.access = mapToMetalAccess(entry.visibility);
  //   [argDescs addObject:desc];
  // }
  // m_encoder = [(id<MTLDevice>)nativeDevice newArgumentEncoderWithArguments:argDescs];
  // m_argumentBufferSize = m_encoder.encodedLength;
  (void)nativeDevice;
  (void)entries;
  assert(false && "Metal implementation not yet available");
}

void MetalBindTableLayout::deinit()
{
  // TODO: Metal implementation
  // NOTES:
  // 1. Release encoder (ARC handles automatically)
  // 2. Clear entries and mapping
  m_encoder = nullptr;
  m_logicalToIndex.clear();
  m_entries.clear();
}

uint64_t MetalBindTableLayout::getNativeHandle() const
{
  // TODO: Metal implementation
  // NOTES:
  // Return opaque handle to MTLArgumentEncoder
  // Use __bridge_retained for ARC interop
  return 0;
}

bool MetalBindTableLayout::resolveLogicalIndex(ResourceIndex logicalIndex, uint32_t& outIndex) const
{
  // TODO: Metal implementation
  // NOTES:
  // Look up logical index in m_logicalToIndex
  // Return true if found, false otherwise
  (void)logicalIndex;
  (void)outIndex;
  return false;
}

const std::vector<BindTableLayoutEntry>& MetalBindTableLayout::entries() const
{
  // TODO: Metal implementation
  // NOTES:
  // Return m_entries
  return m_entries;
}

MetalBindTable::~MetalBindTable()
{
  deinit();
}

void MetalBindTable::init(void* nativeDevice, const BindTableLayout& layout, uint32_t maxLogicalEntries)
{
  // TODO: Metal implementation
  // NOTES:
  // 1. Store device and layout view
  // 2. Get encoder from layout (MetalBindTableLayout)
  // 3. Create MTLBuffer for argument buffer:
  //    - Size: encoder.encodedLength
  //    - Options: MTLResourceStorageModeShared (CPU-accessible for updates)
  //    - Or MTLResourceStorageModePrivate (GPU-only, requires staging)
  // 4. Initialize argument buffer with encoder
  //
  // Example Metal API pattern:
  // const MetalBindTableLayout& metalLayout = static_cast<const MetalBindTableLayout&>(layout);
  // MTLArgumentEncoder* encoder = metalLayout.encoder();
  // id<MTLBuffer> argBuffer = [(id<MTLDevice>)nativeDevice newBufferWithLength:encoder.encodedLength options:0];
  // m_bufferSize = encoder.encodedLength;
  (void)nativeDevice;
  (void)layout;
  (void)maxLogicalEntries;
  assert(false && "Metal implementation not yet available");
}

void MetalBindTable::deinit()
{
  // TODO: Metal implementation
  // NOTES:
  // 1. Release argument buffer (ARC handles automatically)
  // 2. Clear layout reference
  m_buffer     = nullptr;
  m_layoutView = nullptr;
}

void MetalBindTable::update(uint32_t writeCount, const BindTableWrite* writes)
{
  // TODO: Metal implementation
  // NOTES:
  // 1. Get encoder from layout
  // 2. Set argument buffer: [encoder setArgumentBuffer:m_buffer offset:0]
  // 3. For each write:
  //    - Calculate index from logicalIndex using layout mapping
  //    - Call appropriate encoder method:
  //      - Texture: [encoder setTexture:texture atIndex:index]
  //      - Buffer: [encoder setBuffer:buffer offset:bufferOffset atIndex:index]
  //      - Sampler: [encoder setSampler:sampler atIndex:index]
  // 4. Mark buffer as updated (if using GPU-only storage)
  //
  // Example Metal API pattern:
  // [m_encoder setArgumentBuffer:m_buffer offset:0];
  // for (uint32_t i = 0; i < writeCount; ++i) {
  //   uint32_t index = m_layoutView->resolveLogicalIndex(writes[i].dstIndex);
  //   if (writes[i].resourceType == BindlessResourceType::sampledTexture) {
  //     for (uint32_t j = 0; j < writes[i].descriptorCount; ++j) {
  //       id<MTLTexture> tex = (id<MTLTexture>)writes[i].resources[j].texture;
  //       [m_encoder setTexture:tex atIndex:index + j];
  //     }
  //   }
  // }
  (void)writeCount;
  (void)writes;
  assert(false && "Metal implementation not yet available");
}

uint64_t MetalBindTable::getNativeHandle() const
{
  // TODO: Metal implementation
  // NOTES:
  // Return opaque handle to MTLBuffer (argument buffer)
  // Use __bridge_retained for ARC interop
  return 0;
}

}  // namespace metal
}  // namespace rhi
}  // namespace demo