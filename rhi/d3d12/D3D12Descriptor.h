#pragma once

#include "../RHIDescriptor.h"

#include <unordered_map>
#include <vector>

namespace demo {
namespace rhi {
namespace d3d12 {

class D3D12BindTableLayout final : public BindTableLayout
{
public:
  D3D12BindTableLayout() = default;
  ~D3D12BindTableLayout() override;

  void init(void* nativeDevice, const std::vector<BindTableLayoutEntry>& entries) override;
  void deinit() override;

  uint64_t getNativeHandle() const override;

  bool resolveLogicalIndex(ResourceIndex logicalIndex, uint32_t& outIndex) const;

  const std::vector<BindTableLayoutEntry>& entries() const;

private:
  // NOTES: D3D12 Descriptor Heaps for Bindless
  // D3D12 uses descriptor heaps organized by type for bindless resource access:
  //
  // Heap Types:
  // - CBV_SRV_UAV heap: Constant buffers, shader resource views, unordered access views
  // - Sampler heap: Samplers
  //
  // Bind Table Layout → Root Signature Mapping:
  // - BindTableLayout entries map to root signature parameters
  // - Each entry maps to a descriptor table slot in the heap
  // - Logical indices map to heap offsets
  //
  // Initialization:
  // 1. Store bind table entries
  // 2. Map logical indices to heap offsets
  // 3. Calculate descriptor heap requirements
  // 4. Return heap handle (or nullptr if using global heap)
  //
  // NOTES: D3D12 typically uses global descriptor heaps for bindless
  // Individual bind tables don't need their own heaps
  // Instead, they calculate offsets into the global heap

  void* m_device{nullptr};  // ID3D12Device

  std::unordered_map<ResourceIndex, uint32_t> m_logicalToIndex;
  std::vector<BindTableLayoutEntry>           m_entries;

  uint32_t m_cbvSrvUavHeapOffset{0};
  uint32_t m_samplerHeapOffset{0};
};

class D3D12BindTable final : public BindTable
{
public:
  D3D12BindTable() = default;
  ~D3D12BindTable() override;

  void init(void* nativeDevice, const BindTableLayout& layout, uint32_t maxLogicalEntries) override;
  void deinit() override;

  void update(uint32_t writeCount, const BindTableWrite* writes) override;

  uint64_t getNativeHandle() const override;

private:
  // NOTES: D3D12 Bind Table Implementation
  // Bind tables are backed by offsets into global descriptor heaps:
  //
  // Update Flow:
  // 1. Get CBV_SRV_UAV heap and Sampler heap from device
  // 2. For each BindTableWrite:
  //    - Calculate descriptor handle using heap start + offset
  //    - Copy descriptors using CopyDescriptorsSimple:
  //      - device->CopyDescriptorsSimple(1, destHandle, srcHandle, heapType)
  // 3. Mark bind table as dirty (if using update batching)
  //
  // Descriptor Creation:
  // - CBV: CreateConstantBufferView(desc, destHandle)
  // - SRV (texture): CreateShaderResourceView(resource, desc, destHandle)
  // - UAV: CreateUnorderedAccessView(resource, desc, destHandle)
  // - Sampler: CreateSampler(desc, destHandle)
  //
  // Shader Access:
  // - Root signature has descriptor table parameter pointing to heap region
  // - Shader uses t#, u#, s#, b# registers for resource access
  // - Register number matches heap offset

  void* m_device{nullptr};  // ID3D12Device

  const D3D12BindTableLayout* m_layoutView{nullptr};
  uint32_t                    m_maxLogicalEntries{0};

  void* m_cbvSrvUavHeapStart{nullptr};  // D3D12_CPU_DESCRIPTOR_HANDLE
  void* m_samplerHeapStart{nullptr};    // D3D12_CPU_DESCRIPTOR_HANDLE

  bool m_needsUpdate{false};
};

}  // namespace d3d12
}  // namespace rhi
}  // namespace demo