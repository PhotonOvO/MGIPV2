#include "D3D12CommandBuffer.h"

#include "../RHIDebugCounters.h"

#include "D3D12Device.h"
#include "D3D12CommandAllocator.h"

#define NOMINMAX
#include <Windows.h>
#include <d3d12.h>

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstring>
#include <limits>
#include <stdexcept>
#include <string>

namespace demo::rhi::d3d12 {
namespace {

constexpr uint64_t kInitialFillUploadCapacity = 64u * 1024u;

template <typename Handle, size_t Capacity>
void trackHandle(
  std::array<Handle, Capacity>& handles, uint32_t& count,
  Handle handle, const char* capacityError)
{
  for(uint32_t index = 0; index < count; ++index)
  {
    if(handles[index] == handle)
      return;
  }
  if(count == Capacity)
    throw std::runtime_error(capacityError);
  handles[count++] = handle;
}

ID3D12GraphicsCommandList* commandList(void* value)
{
  if(value == nullptr)
    throw std::runtime_error("D3D12 command encoder has no active command list");
  return static_cast<ID3D12GraphicsCommandList*>(value);
}

ID3D12Resource* requireResource(void* value, const char* operation)
{
  if(value == nullptr)
    throw std::runtime_error(operation);
  return static_cast<ID3D12Resource*>(value);
}

D3D12_RESOURCE_STATES toD3D12State(ResourceState state)
{
  switch(state)
  {
  case ResourceState::Undefined:
  case ResourceState::General:                 return D3D12_RESOURCE_STATE_COMMON;
  case ResourceState::ColorAttachment:         return D3D12_RESOURCE_STATE_RENDER_TARGET;
  case ResourceState::DepthStencilAttachment:  return D3D12_RESOURCE_STATE_DEPTH_WRITE;
  case ResourceState::DepthStencilReadOnly:    return D3D12_RESOURCE_STATE_DEPTH_READ;
  case ResourceState::ShaderRead:
    return D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE |
           D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE;
  case ResourceState::ShaderWrite:              return D3D12_RESOURCE_STATE_UNORDERED_ACCESS;
  case ResourceState::TransferSrc:              return D3D12_RESOURCE_STATE_COPY_SOURCE;
  case ResourceState::TransferDst:              return D3D12_RESOURCE_STATE_COPY_DEST;
  case ResourceState::IndirectArgument:         return D3D12_RESOURCE_STATE_INDIRECT_ARGUMENT;
  case ResourceState::Present:                  return D3D12_RESOURCE_STATE_PRESENT;
  }
  return D3D12_RESOURCE_STATE_COMMON;
}

UINT subresourceIndex(const D3D12_RESOURCE_DESC& desc, uint32_t mipLevel, uint32_t arrayLayer)
{
  return mipLevel + arrayLayer * static_cast<UINT>(desc.MipLevels);
}

void unsupported(const char* operation)
{
  throw std::runtime_error(operation);
}

}  // namespace

void D3D12RenderEncoder::prepare(
  void* nativeCommandList, D3D12Device* device, D3D12CommandBuffer* owner)
{
  m_commandList = nativeCommandList;
  m_device = device;
  m_owner = owner;
  m_pipeline = {};
  m_dynamicOffsetCounts.fill(0);
  m_valid = true;
}

void D3D12RenderEncoder::invalidate() noexcept
{
  m_valid = false;
}

void D3D12RenderEncoder::requireActive() const
{
  if(!m_valid)
    throw std::runtime_error("D3D12 render encoder is outside its encoding scope");
}

void D3D12RenderEncoder::setPipeline(PipelineHandle pipeline)
{
  requireActive();
  m_device->bindPipeline(m_commandList, pipeline, false);
  if(m_owner != nullptr) m_owner->trackPipeline(pipeline);
  m_pipeline = pipeline;
}

void D3D12RenderEncoder::setArgumentTable(ShaderStage, uint32_t slot, ArgumentTableHandle table)
{
  requireActive();
  const uint32_t count = slot < m_dynamicOffsetCounts.size()
    ? m_dynamicOffsetCounts[slot] : 0;
  const uint64_t* offsets = count != 0 ? m_dynamicOffsets[slot].data() : nullptr;
  if(m_owner != nullptr) m_owner->trackArgumentTable(table);
  m_device->bindArgumentTable(m_commandList, m_pipeline, false, slot, table, offsets, count);
  if(slot < m_dynamicOffsetCounts.size()) m_dynamicOffsetCounts[slot] = 0;
}

void D3D12RenderEncoder::setDynamicBuffer(
  ShaderStage, uint32_t slot, BufferHandle buffer, uint64_t offset, uint64_t)
{
  requireActive();
  if(slot >= m_dynamicOffsetCounts.size() ||
     m_dynamicOffsetCounts[slot] >= m_dynamicOffsets[slot].size())
    throw std::runtime_error("D3D12 dynamic-offset capacity exceeded");
  if(m_owner != nullptr && buffer.isValid()) m_owner->trackBuffer(buffer);
  m_dynamicOffsets[slot][m_dynamicOffsetCounts[slot]++] = offset;
}

void D3D12RenderEncoder::setRootConstants(ShaderStage, uint32_t slot, std::span<const std::byte> data)
{
  requireActive();
  m_device->bindRootConstants(m_commandList, m_pipeline, false, slot, data.data(), static_cast<uint32_t>(data.size()));
}

void D3D12RenderEncoder::setRootPointer(ShaderStage, uint32_t slot, GpuPtr ptr)
{
  requireActive();
  m_device->bindRootConstants(
    m_commandList, m_pipeline, false, slot, &ptr.value, sizeof(ptr.value));
}

void D3D12RenderEncoder::setViewport(const Viewport& viewport)
{
  requireActive();
  const D3D12_VIEWPORT native{
    viewport.x, viewport.y, viewport.width, viewport.height,
    viewport.minDepth, viewport.maxDepth,
  };
  commandList(m_commandList)->RSSetViewports(1, &native);
}

void D3D12RenderEncoder::setScissor(const Rect2D& scissor)
{
  requireActive();
  const D3D12_RECT native{
    static_cast<LONG>(scissor.offset.x),
    static_cast<LONG>(scissor.offset.y),
    static_cast<LONG>(scissor.offset.x + static_cast<int32_t>(scissor.extent.width)),
    static_cast<LONG>(scissor.offset.y + static_cast<int32_t>(scissor.extent.height)),
  };
  commandList(m_commandList)->RSSetScissorRects(1, &native);
}

void D3D12RenderEncoder::bindVertexBuffers(
  uint32_t firstBinding, std::span<const VertexBufferBinding> bindings)
{
  requireActive();
  constexpr uint32_t kMaxVertexBuffers = D3D12_IA_VERTEX_INPUT_RESOURCE_SLOT_COUNT;
  const uint32_t count = static_cast<uint32_t>(bindings.size());
  if(count > kMaxVertexBuffers)
    throw std::runtime_error("D3D12 vertex-buffer bind exceeds the native slot limit");
  std::array<D3D12_VERTEX_BUFFER_VIEW, kMaxVertexBuffers> views{};
  for(uint32_t index = 0; index < count; ++index)
  {
    auto* resource = requireResource(
      m_device->resolveBuffer(bindings[index].buffer),
      "D3D12 vertex-buffer bind received an invalid buffer");
    if(m_owner != nullptr) m_owner->trackBuffer(bindings[index].buffer);
    const uint64_t offset = bindings[index].offset;
    const uint64_t size = resource->GetDesc().Width;
    const uint32_t stride = m_device->vertexStride(m_pipeline, firstBinding + index);
    if(offset >= size || stride == 0)
      throw std::runtime_error("D3D12 vertex-buffer view is invalid");
    views[index] = {
      resource->GetGPUVirtualAddress() + offset,
      static_cast<UINT>(std::min<uint64_t>(size - offset, UINT_MAX)),
      stride};
  }
  commandList(m_commandList)->IASetVertexBuffers(firstBinding, count, views.data());
}

void D3D12RenderEncoder::bindIndexBuffer(BufferHandle buffer, uint64_t offset, IndexFormat format)
{
  requireActive();
  auto* resource = requireResource(m_device->resolveBuffer(buffer),
                                   "D3D12RenderEncoder::bindIndexBuffer received an invalid buffer");
  if(m_owner != nullptr) m_owner->trackBuffer(buffer);
  const uint64_t resourceSize = resource->GetDesc().Width;
  if(offset >= resourceSize)
    throw std::runtime_error("D3D12RenderEncoder::bindIndexBuffer offset exceeds the buffer");

  const D3D12_INDEX_BUFFER_VIEW view{
    .BufferLocation = resource->GetGPUVirtualAddress() + offset,
    .SizeInBytes = static_cast<UINT>(std::min<uint64_t>(resourceSize - offset, UINT_MAX)),
    .Format = format == IndexFormat::uint16 ? DXGI_FORMAT_R16_UINT : DXGI_FORMAT_R32_UINT,
  };
  commandList(m_commandList)->IASetIndexBuffer(&view);
}

void D3D12RenderEncoder::readInputAttachment(uint32_t)
{
  requireActive();
}

void D3D12RenderEncoder::draw(const DrawDesc& desc)
{
  requireActive();
  commandList(m_commandList)->DrawInstanced(desc.vertexCount, desc.instanceCount,
                                             desc.firstVertex, desc.firstInstance);
}

void D3D12RenderEncoder::drawIndexed(const DrawIndexedDesc& desc)
{
  requireActive();
  if(!desc.indexBuffer.isNull())
    bindIndexBuffer(desc.indexBuffer, desc.indexBufferOffset, desc.indexFormat);
  commandList(m_commandList)->DrawIndexedInstanced(desc.indexCount, desc.instanceCount,
                                                    desc.firstIndex, desc.vertexOffset,
                                                    desc.firstInstance);
}

void D3D12RenderEncoder::drawIndexedIndirect(const DrawIndirectDesc& desc)
{
  requireActive();
  const uint32_t stride =
    desc.stride != 0 ? desc.stride : sizeof(D3D12_DRAW_INDEXED_ARGUMENTS);
  if(stride != sizeof(D3D12_DRAW_INDEXED_ARGUMENTS) &&
     stride != sizeof(uint32_t) + sizeof(D3D12_DRAW_INDEXED_ARGUMENTS))
    throw std::runtime_error("D3D12 indexed-indirect stride is unsupported");
  if(desc.drawCount == 0)
    return;
  auto* arguments = requireResource(
    m_device->resolveBuffer(desc.argsBuffer),
    "D3D12RenderEncoder::drawIndexedIndirect received an invalid argument buffer");
  if(m_owner != nullptr) m_owner->trackBuffer(desc.argsBuffer);
  auto* signature = static_cast<ID3D12CommandSignature*>(
    m_device->drawIndirectSignature(true, m_pipeline, stride));
  if(signature == nullptr)
    throw std::runtime_error("D3D12 indexed-indirect command signature is unavailable");
  m_device->transitionBuffer(
    m_commandList, desc.argsBuffer, D3D12_RESOURCE_STATE_INDIRECT_ARGUMENT);
  commandList(m_commandList)->ExecuteIndirect(
    signature, desc.drawCount, arguments, desc.offset, nullptr, 0);
}

void D3D12RenderEncoder::drawIndexedIndirectCount(
  const DrawIndirectCountDesc& desc)
{
  requireActive();
  const uint32_t stride =
    desc.stride != 0 ? desc.stride : sizeof(D3D12_DRAW_INDEXED_ARGUMENTS);
  if(stride != sizeof(D3D12_DRAW_INDEXED_ARGUMENTS) &&
     stride != sizeof(uint32_t) + sizeof(D3D12_DRAW_INDEXED_ARGUMENTS))
    throw std::runtime_error("D3D12 indexed-indirect-count stride is unsupported");
  if(desc.maxDrawCount == 0)
    return;
  auto* arguments = requireResource(
    m_device->resolveBuffer(desc.argsBuffer),
    "D3D12RenderEncoder::drawIndexedIndirectCount received an invalid argument buffer");
  auto* count = requireResource(
    m_device->resolveBuffer(desc.countBuffer),
    "D3D12RenderEncoder::drawIndexedIndirectCount received an invalid count buffer");
  if(m_owner != nullptr) m_owner->trackBuffer(desc.countBuffer);
  if(m_owner != nullptr) m_owner->trackBuffer(desc.argsBuffer);
  auto* signature = static_cast<ID3D12CommandSignature*>(
    m_device->drawIndirectSignature(true, m_pipeline, stride));
  if(signature == nullptr)
    throw std::runtime_error("D3D12 indexed-indirect command signature is unavailable");
  m_device->transitionBuffer(
    m_commandList, desc.argsBuffer, D3D12_RESOURCE_STATE_INDIRECT_ARGUMENT);
  m_device->transitionBuffer(
    m_commandList, desc.countBuffer, D3D12_RESOURCE_STATE_INDIRECT_ARGUMENT);
  commandList(m_commandList)->ExecuteIndirect(
    signature, desc.maxDrawCount, arguments, desc.argsOffset,
    count, desc.countBufferOffset);
}

void D3D12RenderEncoder::drawIndirect(const DrawIndirectDesc& desc)
{
  requireActive();
  if(desc.stride != 0 && desc.stride != sizeof(D3D12_DRAW_ARGUMENTS))
    throw std::runtime_error("D3D12 indirect stride must match D3D12_DRAW_ARGUMENTS");
  if(desc.drawCount == 0)
    return;
  auto* arguments = requireResource(
    m_device->resolveBuffer(desc.argsBuffer),
    "D3D12RenderEncoder::drawIndirect received an invalid argument buffer");
  const uint32_t stride =
    desc.stride != 0 ? desc.stride : sizeof(D3D12_DRAW_ARGUMENTS);
  if(m_owner != nullptr) m_owner->trackBuffer(desc.argsBuffer);
  auto* signature = static_cast<ID3D12CommandSignature*>(
    m_device->drawIndirectSignature(false, m_pipeline, stride));
  if(signature == nullptr)
    throw std::runtime_error("D3D12 indirect command signature is unavailable");
  m_device->transitionBuffer(
    m_commandList, desc.argsBuffer, D3D12_RESOURCE_STATE_INDIRECT_ARGUMENT);
  commandList(m_commandList)->ExecuteIndirect(
    signature, desc.drawCount, arguments, desc.offset, nullptr, 0);
}

void D3D12RenderEncoder::drawMeshTasks(uint32_t, uint32_t, uint32_t)
{
  requireActive();
  unsupported("D3D12RenderEncoder::drawMeshTasks requires ID3D12GraphicsCommandList6");
}

void D3D12RenderEncoder::drawMeshTasksIndirect(const DrawIndirectDesc&)
{
  requireActive();
  unsupported("D3D12RenderEncoder::drawMeshTasksIndirect requires a mesh command signature");
}

void D3D12ComputeEncoder::prepare(void* nativeCommandList, D3D12Device* device,
                                  D3D12CommandBuffer* owner)
{
  m_commandList = nativeCommandList;
  m_device = device;
  m_owner = owner;
  m_pipeline = {};
  m_valid = true;
}

void D3D12ComputeEncoder::invalidate() noexcept
{
  m_valid = false;
}

void D3D12ComputeEncoder::requireActive() const
{
  if(!m_valid)
    throw std::runtime_error("D3D12 compute encoder is outside its encoding scope");
}

void D3D12ComputeEncoder::requireCompute(const char* command) const
{
  if(m_owner == nullptr)
    throw std::runtime_error("D3D12 compute encoder has no owning command buffer");
  m_owner->requireQueueOperation(QueueOperation::compute, command);
}
void D3D12ComputeEncoder::requireTransfer(const char* command) const
{
  if(m_owner == nullptr)
    throw std::runtime_error("D3D12 compute encoder has no owning command buffer");
  m_owner->requireQueueOperation(QueueOperation::transfer, command);
}
void D3D12ComputeEncoder::setPipeline(PipelineHandle pipeline)
{
  requireActive();
  requireCompute("D3D12ComputeEncoder::setPipeline");
  m_device->bindPipeline(m_commandList, pipeline, true);
  if(m_owner != nullptr) m_owner->trackPipeline(pipeline);
  m_pipeline = pipeline;
}

void D3D12ComputeEncoder::setArgumentTable(uint32_t slot, ArgumentTableHandle table)
{
  requireActive();
  requireCompute("D3D12ComputeEncoder::setArgumentTable");
  if(m_owner != nullptr) m_owner->trackArgumentTable(table);
  m_device->bindArgumentTable(
    m_commandList, m_pipeline, true, slot, table, nullptr, 0);
}

void D3D12ComputeEncoder::setRootConstants(uint32_t slot, std::span<const std::byte> data)
{
  requireActive();
  requireCompute("D3D12ComputeEncoder::setRootConstants");
  m_device->bindRootConstants(m_commandList, m_pipeline, true, slot, data.data(), static_cast<uint32_t>(data.size()));
}

void D3D12ComputeEncoder::setRootPointer(uint32_t slot, GpuPtr ptr)
{
  requireActive();
  requireCompute("D3D12ComputeEncoder::setRootPointer");
  m_device->bindRootConstants(
    m_commandList, m_pipeline, true, slot, &ptr.value, sizeof(ptr.value));
}

void D3D12ComputeEncoder::dispatch(const DispatchDesc& desc)
{
  requireActive();
  requireCompute("D3D12ComputeEncoder::dispatch");
  commandList(m_commandList)->Dispatch(desc.groupCountX, desc.groupCountY, desc.groupCountZ);
}

void D3D12ComputeEncoder::dispatchIndirect(const DispatchIndirectDesc& desc)
{
  requireActive();
  requireCompute("D3D12ComputeEncoder::dispatchIndirect");
  auto* arguments = requireResource(
    m_device->resolveBuffer(desc.argsBuffer),
    "D3D12ComputeEncoder::dispatchIndirect received an invalid argument buffer");
  if(m_owner != nullptr) m_owner->trackBuffer(desc.argsBuffer);
  auto* signature = static_cast<ID3D12CommandSignature*>(
    m_device->dispatchIndirectSignature());
  if(signature == nullptr)
    throw std::runtime_error("D3D12 dispatch-indirect command signature is unavailable");
  m_device->transitionBuffer(
    m_commandList, desc.argsBuffer, D3D12_RESOURCE_STATE_INDIRECT_ARGUMENT);
  commandList(m_commandList)->ExecuteIndirect(
    signature, 1, arguments, desc.offset, nullptr, 0);
}

void D3D12ComputeEncoder::copyBuffer(BufferHandle src, uint64_t srcOffset,
                                     BufferHandle dst, uint64_t dstOffset, uint64_t size)
{
  requireActive();
  requireTransfer("D3D12ComputeEncoder::copyBuffer");
  auto* source = requireResource(m_device->resolveBuffer(src),
                                 "D3D12ComputeEncoder::copyBuffer received an invalid source");
  auto* destination = requireResource(m_device->resolveBuffer(dst),
                                      "D3D12ComputeEncoder::copyBuffer received an invalid destination");
  if(m_owner != nullptr)
  {
    m_owner->trackBuffer(src);
    m_owner->trackBuffer(dst);
  }
  m_device->transitionBuffer(m_commandList, src, D3D12_RESOURCE_STATE_COPY_SOURCE);
  m_device->transitionBuffer(m_commandList, dst, D3D12_RESOURCE_STATE_COPY_DEST);
  commandList(m_commandList)->CopyBufferRegion(destination, dstOffset, source, srcOffset, size);
  if(m_owner != nullptr && m_owner->queueClass() == QueueClass::transfer)
    m_device->prepareBufferForQueueHandoff(m_commandList, dst);
}

void D3D12ComputeEncoder::copyBufferToTexture(const BufferTextureCopyDesc& desc)
{
  requireActive();
  requireTransfer("D3D12ComputeEncoder::copyBufferToTexture");
  auto* sourceData = static_cast<const std::byte*>(
    m_device->resolveBufferMappedData(desc.buffer));
  if(sourceData == nullptr)
    sourceData = static_cast<const std::byte*>(m_device->mapBuffer(desc.buffer));
  const uint64_t sourceBufferSize = m_device->resolveBufferSize(desc.buffer);
  auto* texture = requireResource(m_device->resolveTexture(desc.texture),
    "D3D12ComputeEncoder::copyBufferToTexture received an invalid texture");
  if(m_owner != nullptr)
  {
    m_owner->trackBuffer(desc.buffer);
    m_owner->trackTexture(desc.texture);
  }
  if(sourceData == nullptr)
    throw std::runtime_error(
      "D3D12ComputeEncoder::copyBufferToTexture requires a host-visible upload buffer");
  if(m_owner == nullptr)
    throw std::runtime_error(
      "D3D12ComputeEncoder::copyBufferToTexture has no command-buffer owner");

  auto* device = static_cast<ID3D12Device*>(m_device->getD3D12Device());
  const D3D12_RESOURCE_DESC textureDesc = texture->GetDesc();
  if(desc.aspect != TextureAspect::color)
    throw std::runtime_error(
      "D3D12ComputeEncoder::copyBufferToTexture only supports color subresources");
  if(desc.mipLevel >= textureDesc.MipLevels || desc.layerCount == 0 ||
     desc.width == 0 || desc.height == 0 || desc.depth == 0 ||
     desc.textureOffset.x < 0 || desc.textureOffset.y < 0 ||
     desc.textureOffset.z < 0)
    throw std::runtime_error(
      "D3D12ComputeEncoder::copyBufferToTexture received an invalid subresource range");

  const bool is3D = textureDesc.Dimension == D3D12_RESOURCE_DIMENSION_TEXTURE3D;
  if((is3D && (desc.baseArrayLayer != 0 || desc.layerCount != 1)) ||
     (!is3D && desc.depth != 1) ||
     (!is3D && (desc.baseArrayLayer >= textureDesc.DepthOrArraySize ||
                desc.layerCount > textureDesc.DepthOrArraySize - desc.baseArrayLayer)))
    throw std::runtime_error(
      "D3D12ComputeEncoder::copyBufferToTexture subresource layers exceed the texture");

  const uint64_t mipWidth = std::max<uint64_t>(1, textureDesc.Width >> desc.mipLevel);
  const uint32_t mipHeight = std::max<UINT>(1, textureDesc.Height >> desc.mipLevel);
  const uint32_t mipDepth = is3D
    ? std::max<UINT>(1, textureDesc.DepthOrArraySize >> desc.mipLevel)
    : 1;
  const uint64_t offsetX = static_cast<uint32_t>(desc.textureOffset.x);
  const uint64_t offsetY = static_cast<uint32_t>(desc.textureOffset.y);
  const uint64_t offsetZ = static_cast<uint32_t>(desc.textureOffset.z);
  if(offsetX > mipWidth || desc.width > mipWidth - offsetX ||
     offsetY > mipHeight || desc.height > mipHeight - offsetY ||
     offsetZ > mipDepth || desc.depth > mipDepth - offsetZ)
    throw std::runtime_error(
      "D3D12ComputeEncoder::copyBufferToTexture region exceeds the destination mip");

  const bool blockCompressed =
    textureDesc.Format == DXGI_FORMAT_BC6H_UF16 ||
    textureDesc.Format == DXGI_FORMAT_BC6H_SF16 ||
    textureDesc.Format == DXGI_FORMAT_BC7_UNORM ||
    textureDesc.Format == DXGI_FORMAT_BC7_UNORM_SRGB;
  if(blockCompressed &&
     (((offsetX | offsetY) & 3u) != 0 ||
      ((desc.width & 3u) != 0 && offsetX + desc.width != mipWidth) ||
      ((desc.height & 3u) != 0 && offsetY + desc.height != mipHeight)))
    throw std::runtime_error(
      "D3D12ComputeEncoder::copyBufferToTexture has a misaligned compressed region");

  // Ask D3D12 for the row/block layout of the requested region, not the full
  // destination mip. The public RHI source buffer is tightly packed.
  D3D12_RESOURCE_DESC regionDesc = textureDesc;
  regionDesc.Alignment = 0;
  regionDesc.Width = desc.width;
  regionDesc.Height = desc.height;
  regionDesc.DepthOrArraySize = static_cast<UINT16>(is3D ? desc.depth : 1);
  regionDesc.MipLevels = 1;
  regionDesc.Flags = D3D12_RESOURCE_FLAG_NONE;

  D3D12_PLACED_SUBRESOURCE_FOOTPRINT footprint{};
  UINT numRows = 0;
  UINT64 rowSize = 0;
  UINT64 totalBytes = 0;
  device->GetCopyableFootprints(
    &regionDesc, 0, 1, 0, &footprint, &numRows, &rowSize, &totalBytes);

  const UINT copyDepth = footprint.Footprint.Depth;
  const uint64_t compactLayerBytes = rowSize * numRows * copyDepth;
  if(desc.bufferOffset > sourceBufferSize ||
     compactLayerBytes >
       (sourceBufferSize - desc.bufferOffset) / desc.layerCount)
    throw std::runtime_error(
      "D3D12ComputeEncoder::copyBufferToTexture source range exceeds its upload buffer"
      " (offset=" + std::to_string(desc.bufferOffset) +
      ", compactBytes=" +
      std::to_string(compactLayerBytes * desc.layerCount) +
      ", bufferSize=" + std::to_string(sourceBufferSize) +
      ", mip=" + std::to_string(desc.mipLevel) +
      ", width=" + std::to_string(desc.width) +
      ", height=" + std::to_string(desc.height) + ")");

  const D3D12_HEAP_PROPERTIES heapProperties{
    .Type = D3D12_HEAP_TYPE_UPLOAD,
    .CPUPageProperty = D3D12_CPU_PAGE_PROPERTY_UNKNOWN,
    .MemoryPoolPreference = D3D12_MEMORY_POOL_UNKNOWN,
    .CreationNodeMask = 1,
    .VisibleNodeMask = 1,
  };
  const D3D12_RESOURCE_DESC uploadDesc{
    .Dimension = D3D12_RESOURCE_DIMENSION_BUFFER,
    .Alignment = 0,
    .Width = totalBytes,
    .Height = 1,
    .DepthOrArraySize = 1,
    .MipLevels = 1,
    .Format = DXGI_FORMAT_UNKNOWN,
    .SampleDesc = {.Count = 1, .Quality = 0},
    .Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR,
    .Flags = D3D12_RESOURCE_FLAG_NONE,
  };

  for(uint32_t layer = 0; layer < desc.layerCount; ++layer)
  {
    ID3D12Resource* upload = nullptr;
    incrementHotPathCounter(
      BackendType::d3d12, HotPathCounter::commandRecordingNativeObjectCreations);
    const HRESULT createResult = device->CreateCommittedResource(
      &heapProperties, D3D12_HEAP_FLAG_NONE, &uploadDesc,
      D3D12_RESOURCE_STATE_GENERIC_READ, nullptr, IID_PPV_ARGS(&upload));
    if(FAILED(createResult))
      throw std::runtime_error(
        "D3D12ComputeEncoder::copyBufferToTexture failed to create a repack buffer");

    try
    {
      void* mapped = nullptr;
      const D3D12_RANGE readRange{0, 0};
      if(FAILED(upload->Map(0, &readRange, &mapped)) || mapped == nullptr)
        throw std::runtime_error(
          "D3D12ComputeEncoder::copyBufferToTexture failed to map its repack buffer");

      const auto* source = sourceData + desc.bufferOffset +
                           compactLayerBytes * layer;
      auto* destination = static_cast<std::byte*>(mapped) + footprint.Offset;
      const uint64_t sourceSlicePitch = rowSize * numRows;
      const uint64_t destinationSlicePitch =
        static_cast<uint64_t>(footprint.Footprint.RowPitch) * numRows;
      for(UINT z = 0; z < copyDepth; ++z)
      {
        for(UINT row = 0; row < numRows; ++row)
        {
          std::memcpy(destination + z * destinationSlicePitch +
                        static_cast<uint64_t>(row) * footprint.Footprint.RowPitch,
                      source + z * sourceSlicePitch +
                        static_cast<uint64_t>(row) * rowSize,
                      static_cast<size_t>(rowSize));
        }
      }
      const D3D12_RANGE writtenRange{0, static_cast<SIZE_T>(totalBytes)};
      upload->Unmap(0, &writtenRange);
    }
    catch(...)
    {
      upload->Release();
      throw;
    }

    const D3D12_TEXTURE_COPY_LOCATION source{
      .pResource = upload,
      .Type = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT,
      .PlacedFootprint = footprint,
    };
    const D3D12_TEXTURE_COPY_LOCATION destination{
      .pResource = texture,
      .Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX,
      .SubresourceIndex = subresourceIndex(
        textureDesc, desc.mipLevel, desc.baseArrayLayer + layer),
    };
    commandList(m_commandList)->CopyTextureRegion(
      &destination, static_cast<UINT>(desc.textureOffset.x),
      static_cast<UINT>(desc.textureOffset.y),
      static_cast<UINT>(desc.textureOffset.z), &source, nullptr);
    m_owner->retainTransientResource(upload);
  }
}
void D3D12ComputeEncoder::copyTextureToBuffer(const BufferTextureCopyDesc& desc)
{
  requireActive();
  requireTransfer("D3D12ComputeEncoder::copyTextureToBuffer");
  auto* buffer = requireResource(m_device->resolveBuffer(desc.buffer),
                                 "D3D12ComputeEncoder::copyTextureToBuffer received an invalid buffer");
  auto* texture = requireResource(m_device->resolveTexture(desc.texture),
                                  "D3D12ComputeEncoder::copyTextureToBuffer received an invalid texture");
  if(m_owner != nullptr)
  {
    m_owner->trackTexture(desc.texture);
    m_owner->trackBuffer(desc.buffer);
  }
  const D3D12_RESOURCE_DESC textureDesc = texture->GetDesc();
  const UINT subresource = subresourceIndex(textureDesc, desc.mipLevel, desc.baseArrayLayer);

  D3D12_PLACED_SUBRESOURCE_FOOTPRINT footprint{};
  static_cast<ID3D12Device*>(m_device->getD3D12Device())->GetCopyableFootprints(
    &textureDesc, subresource, 1, desc.bufferOffset, &footprint, nullptr, nullptr, nullptr);

  const D3D12_TEXTURE_COPY_LOCATION source{
    .pResource = texture,
    .Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX,
    .SubresourceIndex = subresource,
  };
  const D3D12_TEXTURE_COPY_LOCATION destination{
    .pResource = buffer,
    .Type = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT,
    .PlacedFootprint = footprint,
  };
  const D3D12_BOX sourceBox{
    static_cast<UINT>(desc.textureOffset.x),
    static_cast<UINT>(desc.textureOffset.y),
    static_cast<UINT>(desc.textureOffset.z),
    static_cast<UINT>(desc.textureOffset.x) + (desc.width != 0 ? desc.width : footprint.Footprint.Width),
    static_cast<UINT>(desc.textureOffset.y) + (desc.height != 0 ? desc.height : footprint.Footprint.Height),
    static_cast<UINT>(desc.textureOffset.z) + (desc.depth != 0 ? desc.depth : footprint.Footprint.Depth),
  };
  commandList(m_commandList)->CopyTextureRegion(&destination, 0, 0, 0, &source, &sourceBox);
}

void D3D12ComputeEncoder::blitTexture(const TextureBlitDesc& desc)
{
  requireActive();
  requireTransfer("D3D12ComputeEncoder::blitTexture");
  if(desc.aspect != TextureAspect::color)
    throw std::runtime_error("D3D12ComputeEncoder::blitTexture only supports color textures");
  auto* source = requireResource(
    m_device->resolveTexture(desc.srcTexture),
    "D3D12ComputeEncoder::blitTexture received an invalid source texture");
  auto* destination = requireResource(
    m_device->resolveTexture(desc.dstTexture),
    "D3D12ComputeEncoder::blitTexture received an invalid destination texture");
  if(m_owner != nullptr)
  {
    m_owner->trackTexture(desc.srcTexture);
    m_owner->trackTexture(desc.dstTexture);
  }

  const D3D12_RESOURCE_DESC sourceDesc = source->GetDesc();
  const D3D12_RESOURCE_DESC destinationDesc = destination->GetDesc();
  if(sourceDesc.Format != destinationDesc.Format ||
     sourceDesc.SampleDesc.Count != destinationDesc.SampleDesc.Count)
    throw std::runtime_error(
      "D3D12ComputeEncoder::blitTexture requires matching native formats and sample counts");

  const Offset3D& sourceBegin = desc.srcOffsets[0];
  const Offset3D& sourceEnd = desc.srcOffsets[1];
  const Offset3D& destinationBegin = desc.dstOffsets[0];
  const Offset3D& destinationEnd = desc.dstOffsets[1];
  if(sourceBegin.x < 0 || sourceBegin.y < 0 || sourceBegin.z < 0 ||
     destinationBegin.x < 0 || destinationBegin.y < 0 || destinationBegin.z < 0 ||
     sourceEnd.x <= sourceBegin.x || sourceEnd.y <= sourceBegin.y ||
     sourceEnd.z <= sourceBegin.z || destinationEnd.x <= destinationBegin.x ||
     destinationEnd.y <= destinationBegin.y || destinationEnd.z <= destinationBegin.z)
    throw std::runtime_error("D3D12ComputeEncoder::blitTexture received invalid offsets");

  const uint32_t sourceWidth = static_cast<uint32_t>(sourceEnd.x - sourceBegin.x);
  const uint32_t sourceHeight = static_cast<uint32_t>(sourceEnd.y - sourceBegin.y);
  const uint32_t sourceDepth = static_cast<uint32_t>(sourceEnd.z - sourceBegin.z);
  if(sourceWidth != static_cast<uint32_t>(destinationEnd.x - destinationBegin.x) ||
     sourceHeight != static_cast<uint32_t>(destinationEnd.y - destinationBegin.y) ||
     sourceDepth != static_cast<uint32_t>(destinationEnd.z - destinationBegin.z))
    throw std::runtime_error(
      "D3D12ComputeEncoder::blitTexture requires equal source and destination extents");

  const D3D12_TEXTURE_COPY_LOCATION sourceLocation{
    .pResource = source,
    .Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX,
    .SubresourceIndex = subresourceIndex(
      sourceDesc, desc.srcMipLevel, desc.srcBaseArrayLayer),
  };
  const D3D12_TEXTURE_COPY_LOCATION destinationLocation{
    .pResource = destination,
    .Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX,
    .SubresourceIndex = subresourceIndex(
      destinationDesc, desc.dstMipLevel, desc.dstBaseArrayLayer),
  };
  const D3D12_BOX sourceBox{
    static_cast<UINT>(sourceBegin.x),
    static_cast<UINT>(sourceBegin.y),
    static_cast<UINT>(sourceBegin.z),
    static_cast<UINT>(sourceEnd.x),
    static_cast<UINT>(sourceEnd.y),
    static_cast<UINT>(sourceEnd.z),
  };
  commandList(m_commandList)->CopyTextureRegion(
    &destinationLocation,
    static_cast<UINT>(destinationBegin.x),
    static_cast<UINT>(destinationBegin.y),
    static_cast<UINT>(destinationBegin.z),
    &sourceLocation, &sourceBox);
}

void D3D12ComputeEncoder::fillBuffer(
  BufferHandle buffer, uint64_t offset, uint64_t size, uint32_t data)
{
  requireActive();
  requireTransfer("D3D12ComputeEncoder::fillBuffer");
  auto* destination = requireResource(
    m_device->resolveBuffer(buffer),
    "D3D12ComputeEncoder::fillBuffer received an invalid buffer");
  if(m_owner != nullptr) m_owner->trackBuffer(buffer);
  const uint64_t bufferSize = m_device->resolveBufferSize(buffer);
  if((offset & 3u) != 0 || offset > bufferSize)
    throw std::runtime_error("D3D12ComputeEncoder::fillBuffer requires a valid aligned offset");
  const uint64_t fillSize = size == 0 ? bufferSize - offset : size;
  if(fillSize == 0 || (fillSize & 3u) != 0 || fillSize > bufferSize - offset)
    throw std::runtime_error("D3D12ComputeEncoder::fillBuffer requires a valid aligned range");
  if(m_owner == nullptr)
    throw std::runtime_error("D3D12ComputeEncoder::fillBuffer has no command-buffer owner");

  uint64_t sourceOffset = 0;
  auto* upload = requireResource(
    m_owner->prepareFillUpload(fillSize, data, sourceOffset),
    "D3D12ComputeEncoder::fillBuffer failed to prepare upload storage");

  m_device->transitionBuffer(m_commandList, buffer, D3D12_RESOURCE_STATE_COPY_DEST);
  commandList(m_commandList)->CopyBufferRegion(
    destination, offset, upload, sourceOffset, fillSize);
  m_device->transitionBuffer(m_commandList, buffer, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
}
D3D12CommandBuffer::D3D12CommandBuffer(
  D3D12CommandAllocator& allocator, void* nativeCommandList, D3D12Device* device)
  : m_commandList(nativeCommandList), m_device(device), m_allocator(&allocator)
{
  if(m_commandList == nullptr || m_device == nullptr)
    throw std::runtime_error("D3D12CommandBuffer requires native storage and a device");
  m_transientResources.reserve(16);
  ensureFillUploadCapacity(kInitialFillUploadCapacity);
}

D3D12CommandBuffer::~D3D12CommandBuffer()
{
  releaseTransientResources();
  releaseFillUpload();
  if(m_allocator != nullptr)
    m_allocator->releaseCommandBuffer(*this, m_commandList);
  m_commandList = nullptr;
}

void D3D12CommandBuffer::retainTransientResource(void* resource)
{
  if(resource != nullptr)
  {
    if(m_transientResources.size() == m_transientResources.capacity())
      incrementHotPathCounter(BackendType::d3d12, HotPathCounter::commandRecordingHeapAllocations);
    m_transientResources.push_back(resource);
  }
}

void* D3D12CommandBuffer::prepareFillUpload(
  uint64_t size, uint32_t data, uint64_t& sourceOffset)
{
  if(size == 0 || (size & 3u) != 0)
    throw std::runtime_error(
      "D3D12CommandBuffer::prepareFillUpload requires a nonzero aligned size");

  const uint64_t alignedHead = (m_fillUploadHead + 3u) & ~uint64_t{3u};
  if(size > std::numeric_limits<uint64_t>::max() - alignedHead)
    throw std::runtime_error("D3D12 fill-upload capacity overflow");
  const uint64_t requiredCapacity = alignedHead + size;
  ensureFillUploadCapacity(requiredCapacity);

  auto* words = static_cast<uint32_t*>(m_fillUploadMapped) +
                alignedHead / sizeof(uint32_t);
  std::fill(words, words + size / sizeof(uint32_t), data);
  sourceOffset = alignedHead;
  m_fillUploadHead = requiredCapacity;
  return m_fillUpload;
}

void D3D12CommandBuffer::ensureFillUploadCapacity(uint64_t requiredCapacity)
{
  if(requiredCapacity <= m_fillUploadCapacity)
    return;
  if(m_device == nullptr)
    throw std::runtime_error("D3D12 fill-upload allocation requires a device");

  uint64_t capacity = std::max(kInitialFillUploadCapacity, m_fillUploadCapacity);
  while(capacity < requiredCapacity)
  {
    if(capacity > std::numeric_limits<uint64_t>::max() / 2u)
    {
      capacity = requiredCapacity;
      break;
    }
    capacity *= 2u;
  }

  const D3D12_HEAP_PROPERTIES heapProperties{
    .Type = D3D12_HEAP_TYPE_UPLOAD,
    .CPUPageProperty = D3D12_CPU_PAGE_PROPERTY_UNKNOWN,
    .MemoryPoolPreference = D3D12_MEMORY_POOL_UNKNOWN,
    .CreationNodeMask = 1,
    .VisibleNodeMask = 1,
  };
  const D3D12_RESOURCE_DESC uploadDesc{
    .Dimension = D3D12_RESOURCE_DIMENSION_BUFFER,
    .Alignment = 0,
    .Width = capacity,
    .Height = 1,
    .DepthOrArraySize = 1,
    .MipLevels = 1,
    .Format = DXGI_FORMAT_UNKNOWN,
    .SampleDesc = {.Count = 1, .Quality = 0},
    .Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR,
    .Flags = D3D12_RESOURCE_FLAG_NONE,
  };

  ID3D12Resource* upload = nullptr;
  const HRESULT createResult =
    static_cast<ID3D12Device*>(m_device->getD3D12Device())->CreateCommittedResource(
      &heapProperties, D3D12_HEAP_FLAG_NONE, &uploadDesc,
      D3D12_RESOURCE_STATE_GENERIC_READ, nullptr, IID_PPV_ARGS(&upload));
  if(FAILED(createResult))
    throw std::runtime_error("D3D12 fill-upload allocation failed");

  void* mapped = nullptr;
  const D3D12_RANGE readRange{0, 0};
  if(FAILED(upload->Map(0, &readRange, &mapped)) || mapped == nullptr)
  {
    upload->Release();
    throw std::runtime_error("D3D12 fill-upload mapping failed");
  }

  if(m_state == CommandBufferState::recording)
    incrementHotPathCounter(
      BackendType::d3d12, HotPathCounter::commandRecordingNativeObjectCreations);

  if(m_fillUpload != nullptr)
  {
    static_cast<ID3D12Resource*>(m_fillUpload)->Unmap(0, nullptr);
    if(m_fillUploadHead != 0)
      retainTransientResource(m_fillUpload);
    else
      static_cast<ID3D12Resource*>(m_fillUpload)->Release();
  }
  m_fillUpload = upload;
  m_fillUploadMapped = mapped;
  m_fillUploadCapacity = capacity;
}

void D3D12CommandBuffer::releaseFillUpload() noexcept
{
  if(m_fillUpload != nullptr)
  {
    static_cast<ID3D12Resource*>(m_fillUpload)->Unmap(0, nullptr);
    static_cast<ID3D12Resource*>(m_fillUpload)->Release();
  }
  m_fillUpload = nullptr;
  m_fillUploadMapped = nullptr;
  m_fillUploadCapacity = 0;
  m_fillUploadHead = 0;
}

void D3D12CommandBuffer::releaseTransientResources() noexcept
{
  for(void* resource : m_transientResources)
  {
    if(resource != nullptr)
      static_cast<ID3D12Resource*>(resource)->Release();
  }
  m_transientResources.clear();
}
void D3D12CommandBuffer::begin(CommandAllocator& allocator)
{
  auto* d3dAllocator = dynamic_cast<D3D12CommandAllocator*>(&allocator);
  if(d3dAllocator == nullptr || d3dAllocator != m_allocator)
    throw std::runtime_error("D3D12CommandBuffer::begin rejected a foreign allocator");
  if(m_state != CommandBufferState::idle && m_state != CommandBufferState::reusable)
    throw std::runtime_error("D3D12CommandBuffer::begin requires idle or reusable state");

  releaseTransientResources();
  m_fillUploadHead = 0;
  m_argumentTableCount = 0;
  m_pipelineCount = 0;
  m_bufferCount = 0;
  m_textureCount = 0;
  m_textureViewCount = 0;
  m_queryPoolCount = 0;
  m_resourceStates.reset();
  incrementHotPathCounter(BackendType::d3d12, HotPathCounter::commandBufferBegins);
  const HRESULT result = commandList(m_commandList)->Reset(
    static_cast<ID3D12CommandAllocator*>(d3dAllocator->nativeAllocator()), nullptr);
  if(FAILED(result))
    throw std::runtime_error("D3D12CommandBuffer::begin failed to reset the command list");
  m_active = EncoderKind::none;
  m_submission = {};
  m_state = CommandBufferState::recording;
}

void D3D12CommandBuffer::end()
{
  if(m_state != CommandBufferState::recording || m_active != EncoderKind::none)
    throw std::runtime_error(
      "D3D12CommandBuffer::end requires recording state with no active encoder");
  const HRESULT result = commandList(m_commandList)->Close();
  if(FAILED(result))
    throw std::runtime_error("D3D12CommandBuffer::end failed to close the command list");
  m_state = CommandBufferState::executable;
}

CommandBufferState D3D12CommandBuffer::state() const noexcept
{
  return m_state;
}

QueueClass D3D12CommandBuffer::queueClass() const noexcept
{
  return m_allocator != nullptr ? m_allocator->queueClass() : QueueClass::graphics;
}
void D3D12CommandBuffer::requireQueueOperation(
  QueueOperation operation, const char* command) const
{
  if(!supportsQueueOperation(queueClass(), operation))
    throw std::runtime_error(std::string(command) + " is unsupported by this queue class");
}
void D3D12CommandBuffer::validateForSubmit() const
{
  if(m_state != CommandBufferState::executable || m_allocator == nullptr)
    throw std::runtime_error("D3D12CommandBuffer submission state is invalid");
  for(uint32_t index = 0; index < m_argumentTableCount; ++index)
  {
    if(!m_device->validateArgumentTableForSubmit(m_argumentTables[index]))
      throw std::runtime_error(
        "D3D12CommandBuffer submission references a stale argument table or resource");
  }
  for(uint32_t index = 0; index < m_pipelineCount; ++index)
    if(!m_device->isPipelineValid(m_pipelines[index]))
      throw std::runtime_error("D3D12CommandBuffer submission references a stale pipeline");
  for(uint32_t index = 0; index < m_bufferCount; ++index)
    if(!m_device->isBufferValid(m_buffers[index]))
      throw std::runtime_error("D3D12CommandBuffer submission references a stale buffer");
  for(uint32_t index = 0; index < m_textureCount; ++index)
    if(!m_device->isTextureValid(m_textures[index]))
      throw std::runtime_error(
        "D3D12CommandBuffer submission references stale texture " +
        std::to_string(m_textures[index].index) + ":" +
        std::to_string(m_textures[index].generation));
  for(uint32_t index = 0; index < m_textureViewCount; ++index)
    if(!m_device->isTextureViewValid(m_textureViews[index]))
      throw std::runtime_error("D3D12CommandBuffer submission references a stale texture view");
  for(uint32_t index = 0; index < m_queryPoolCount; ++index)
    if(!m_device->isQueryPoolValid(m_queryPools[index]))
      throw std::runtime_error("D3D12CommandBuffer submission references a stale query pool");
}

void D3D12CommandBuffer::markSubmitted(SubmissionToken token)
{
  if(m_state != CommandBufferState::executable || m_allocator == nullptr)
    throw std::runtime_error("D3D12CommandBuffer submission state is invalid");
  for(uint32_t index = 0; index < m_argumentTableCount; ++index)
    m_device->markArgumentTableSubmitted(m_argumentTables[index], token);
  for(uint32_t index = 0; index < m_pipelineCount; ++index)
    m_device->markPipelineSubmitted(m_pipelines[index], token);
  for(uint32_t index = 0; index < m_bufferCount; ++index)
    m_device->markBufferSubmitted(m_buffers[index], token);
  for(uint32_t index = 0; index < m_textureCount; ++index)
    m_device->markTextureSubmitted(m_textures[index], token);
  for(uint32_t index = 0; index < m_textureViewCount; ++index)
    m_device->markTextureViewSubmitted(m_textureViews[index], token);
  for(uint32_t index = 0; index < m_queryPoolCount; ++index)
    m_device->markQueryPoolSubmitted(m_queryPools[index], token);
  m_submission = token;
  m_state = CommandBufferState::submitted;
  m_allocator->noteSubmitted(*this, token);
}

void D3D12CommandBuffer::markReusable()
{
  if(m_state == CommandBufferState::recording)
    throw std::runtime_error("D3D12CommandBuffer cannot reset while recording");
  releaseTransientResources();
  m_active = EncoderKind::none;
  m_submission = {};
  m_argumentTableCount = 0;
  m_pipelineCount = 0;
  m_bufferCount = 0;
  m_textureCount = 0;
  m_textureViewCount = 0;
  m_queryPoolCount = 0;
  m_state = CommandBufferState::reusable;
}

void D3D12CommandBuffer::trackArgumentTable(ArgumentTableHandle table)
{
  trackHandle(
    m_argumentTables, m_argumentTableCount, table,
    "D3D12 command-buffer argument-table capacity exceeded");
}

void D3D12CommandBuffer::trackPipeline(PipelineHandle pipeline)
{
  trackHandle(
    m_pipelines, m_pipelineCount, pipeline,
    "D3D12 command-buffer pipeline capacity exceeded");
}

void D3D12CommandBuffer::trackBuffer(BufferHandle buffer)
{
  trackHandle(
    m_buffers, m_bufferCount, buffer,
    "D3D12 command-buffer buffer capacity exceeded");
}

void D3D12CommandBuffer::trackTexture(TextureHandle texture)
{
  trackHandle(
    m_textures, m_textureCount, texture,
    "D3D12 command-buffer texture capacity exceeded");
}

void D3D12CommandBuffer::trackTextureView(TextureViewHandle view)
{
  trackHandle(
    m_textureViews, m_textureViewCount, view,
    "D3D12 command-buffer texture-view capacity exceeded");
}

void D3D12CommandBuffer::trackQueryPool(QueryPoolHandle pool)
{
  trackHandle(
    m_queryPools, m_queryPoolCount, pool,
    "D3D12 command-buffer query-pool capacity exceeded");
}

void D3D12CommandBuffer::requireRecording(const char* operation) const
{
  if(m_state != CommandBufferState::recording)
    throw std::runtime_error(std::string(operation) + " requires recording state");
}

void D3D12CommandBuffer::setTarget(void* nativeCommandList, D3D12Device* device)
{
  releaseTransientResources();
  if(nativeCommandList == nullptr || device == nullptr)
    throw std::runtime_error("D3D12CommandBuffer::setTarget requires a command list and device");
  m_commandList = nativeCommandList;
  m_device = device;
  m_active = EncoderKind::none;
  m_argumentTableCount = 0;
  m_pipelineCount = 0;
  m_bufferCount = 0;
  m_textureCount = 0;
  m_textureViewCount = 0;
  m_queryPoolCount = 0;
  m_state = CommandBufferState::recording;
}

RenderEncoder* D3D12CommandBuffer::beginRenderPass(const RenderPassDesc& desc)
{
  requireRecording("D3D12CommandBuffer::beginRenderPass");
  requireQueueOperation(QueueOperation::render, "D3D12CommandBuffer::beginRenderPass");
  if(m_active != EncoderKind::none)
    throw std::runtime_error("D3D12CommandBuffer::beginRenderPass called while an encoder is active");
  const uint32_t colorTargetCount = static_cast<uint32_t>(desc.colorTargets.size());
  if(colorTargetCount > D3D12_SIMULTANEOUS_RENDER_TARGET_COUNT)
    throw std::runtime_error("D3D12 render pass exceeds the color attachment limit");

  std::array<D3D12_CPU_DESCRIPTOR_HANDLE, D3D12_SIMULTANEOUS_RENDER_TARGET_COUNT> rtvs{};
  for(uint32_t index = 0; index < colorTargetCount; ++index)
  {
    rtvs[index].ptr = static_cast<SIZE_T>(
      m_device->resolveTextureViewAttachmentDescriptor(desc.colorTargets[index].view));
    if(rtvs[index].ptr == 0)
      throw std::runtime_error("D3D12 render pass received an invalid color target view");
    trackTextureView(desc.colorTargets[index].view);
    if(desc.colorTargets[index].texture.isValid())
      trackTexture(desc.colorTargets[index].texture);
  }

  D3D12_CPU_DESCRIPTOR_HANDLE dsv{};
  if(desc.depthTarget != nullptr)
  {
    dsv.ptr = static_cast<SIZE_T>(
      m_device->resolveTextureViewAttachmentDescriptor(desc.depthTarget->view));
    if(dsv.ptr == 0)
      throw std::runtime_error("D3D12 render pass received an invalid depth target view");
    trackTextureView(desc.depthTarget->view);
    if(desc.depthTarget->texture.isValid())
      trackTexture(desc.depthTarget->texture);
  }
  for(const InputAttachmentDesc& input : desc.inputAttachments)
  {
    if(!m_device->isTextureViewValid(input.view))
      throw std::runtime_error("D3D12 render pass received an invalid input attachment view");
    trackTextureView(input.view);
  }

  auto* list = commandList(m_commandList);
  list->OMSetRenderTargets(colorTargetCount,
                           colorTargetCount != 0 ? rtvs.data() : nullptr,
                           FALSE, desc.depthTarget != nullptr ? &dsv : nullptr);

  for(uint32_t index = 0; index < colorTargetCount; ++index)
  {
    const RenderTargetDesc& target = desc.colorTargets[index];
    if(target.loadOp == LoadOp::clear)
    {
      const float color[4]{
        target.clearColor.r, target.clearColor.g,
        target.clearColor.b, target.clearColor.a,
      };
      list->ClearRenderTargetView(rtvs[index], color, 0, nullptr);
    }
    else if(target.loadOp == LoadOp::dontCare)
    {
      if(auto* resource = static_cast<ID3D12Resource*>(m_device->resolveTexture(target.texture)))
        list->DiscardResource(resource, nullptr);
    }
  }

  if(desc.depthTarget != nullptr)
  {
    const DepthTargetDesc& target = *desc.depthTarget;
    if(target.loadOp == LoadOp::clear)
    {
      D3D12_CLEAR_FLAGS flags = D3D12_CLEAR_FLAG_DEPTH;
      if(m_device->textureViewHasStencil(target.view))
        flags |= D3D12_CLEAR_FLAG_STENCIL;
      list->ClearDepthStencilView(dsv, flags, target.clearValue.depth,
                                  static_cast<UINT8>(target.clearValue.stencil), 0, nullptr);
    }
    else if(target.loadOp == LoadOp::dontCare)
    {
      if(auto* resource = static_cast<ID3D12Resource*>(m_device->resolveTexture(target.texture)))
        list->DiscardResource(resource, nullptr);
    }
  }

  incrementHotPathCounter(BackendType::d3d12, HotPathCounter::encoderBegins);
  m_renderEncoder.prepare(m_commandList, m_device, this);
  m_active = EncoderKind::render;
  return &m_renderEncoder;
}

ComputeEncoder* D3D12CommandBuffer::beginComputePass()
{
  requireRecording("D3D12CommandBuffer::beginComputePass");
  if(m_active != EncoderKind::none)
    throw std::runtime_error("D3D12CommandBuffer::beginComputePass called while an encoder is active");
  incrementHotPathCounter(BackendType::d3d12, HotPathCounter::encoderBegins);
  m_computeEncoder.prepare(m_commandList, m_device, this);
  m_active = EncoderKind::compute;
  return &m_computeEncoder;
}

void D3D12CommandBuffer::endEncoding()
{
  if(m_active == EncoderKind::none)
    throw std::runtime_error("D3D12CommandBuffer::endEncoding called without an active encoder");
  if(m_active == EncoderKind::render)
    m_renderEncoder.invalidate();
  else
    m_computeEncoder.invalidate();
  m_active = EncoderKind::none;
}

RHIResult D3D12CommandBuffer::useResidencySet(ResidencySetHandle)
{
  if(m_state != CommandBufferState::recording)
    return RHIResult::fail(RHIErrorCode::invalidState, "ResidencySet use requires recording state");
  return RHIResult::fail(
    RHIErrorCode::unsupported, "D3D12 explicit residency is unavailable on this backend path");
}

void D3D12CommandBuffer::barrier(StageFlags, StageFlags, HazardFlags hazards)
{
  requireRecording("D3D12CommandBuffer::barrier");
  if(!any(hazards))
    return;
  if(queueClass() == QueueClass::transfer)
    return;
  const D3D12_RESOURCE_BARRIER barrier{
    .Type = D3D12_RESOURCE_BARRIER_TYPE_UAV,
    .Flags = D3D12_RESOURCE_BARRIER_FLAG_NONE,
    .UAV = {.pResource = nullptr},
  };
  commandList(m_commandList)->ResourceBarrier(1, &barrier);
}

void D3D12CommandBuffer::resourceBarrier(std::span<const TextureBarrier> textures,
                                         std::span<const BufferBarrier> buffers,
                                         std::span<const AliasingBarrier> aliasing)
{
  requireRecording("D3D12CommandBuffer::resourceBarrier");
  const uint32_t textureCount = static_cast<uint32_t>(textures.size());
  const uint32_t bufferCount = static_cast<uint32_t>(buffers.size());
#ifndef NDEBUG
  for(uint32_t index = 0; index < textureCount; ++index)
  {
    if(!m_resourceStates.transition(textures[index]).valid())
      throw std::runtime_error("D3D12CommandBuffer rejected an invalid texture transition");
  }
  for(uint32_t index = 0; index < bufferCount; ++index)
  {
    if(!m_resourceStates.transition(buffers[index]).valid())
      throw std::runtime_error("D3D12CommandBuffer rejected an invalid buffer transition");
  }
#endif

  for(uint32_t index = 0; index < textureCount; ++index)
  {
    const TextureBarrier& source = textures[index];
    trackTexture(source.texture);
    m_device->transitionTexture(
      m_commandList, source.texture, source.range,
      static_cast<uint32_t>(toD3D12State(source.after)));
  }
  for(uint32_t index = 0; index < bufferCount; ++index)
  {
    const BufferBarrier& source = buffers[index];
    trackBuffer(source.buffer);
    if(toD3D12State(source.before) == toD3D12State(source.after))
      continue;
    requireResource(m_device->resolveBuffer(source.buffer),
      "D3D12CommandBuffer::resourceBarrier received an invalid buffer");
    m_device->transitionBuffer(
      m_commandList, source.buffer, static_cast<uint32_t>(toD3D12State(source.after)));
  }

  const auto resolveAliasingResource =
    [this](const AliasingResource& resource) -> ID3D12Resource* {
      switch(resource.kind)
      {
      case AliasingResourceKind::none:
        return nullptr;
      case AliasingResourceKind::buffer:
        return static_cast<ID3D12Resource*>(m_device->resolveBuffer(resource.buffer));
      case AliasingResourceKind::texture:
        return static_cast<ID3D12Resource*>(m_device->resolveTexture(resource.texture));
      }
      return nullptr;
    };
  for(const AliasingBarrier& source : aliasing)
  {
    if(!source.before.isValid() || !source.after.isValid() ||
       source.after.kind == AliasingResourceKind::none)
      throw std::runtime_error("D3D12 aliasing barrier received an invalid resource");
    ID3D12Resource* before = resolveAliasingResource(source.before);
    ID3D12Resource* after = resolveAliasingResource(source.after);
    if((source.before.kind != AliasingResourceKind::none && before == nullptr) ||
       after == nullptr)
      throw std::runtime_error("D3D12 aliasing barrier received a stale resource");
    const auto trackAliased = [this](const AliasingResource& resource) {
      if(resource.kind == AliasingResourceKind::buffer)
        trackBuffer(resource.buffer);
      else if(resource.kind == AliasingResourceKind::texture)
        trackTexture(resource.texture);
    };
    trackAliased(source.before);
    trackAliased(source.after);
    const D3D12_RESOURCE_BARRIER barrier{
      .Type = D3D12_RESOURCE_BARRIER_TYPE_ALIASING,
      .Flags = D3D12_RESOURCE_BARRIER_FLAG_NONE,
      .Aliasing = {.pResourceBefore = before, .pResourceAfter = after},
    };
    commandList(m_commandList)->ResourceBarrier(1, &barrier);
  }
}

void D3D12CommandBuffer::clearColorTexture(TextureHandle texture,
                                            const TextureSubresourceRange& range,
                                            const ClearColorValue& clearColor)
{
  if(range.aspect != TextureAspect::color)
    throw std::runtime_error("D3D12CommandBuffer::clearColorTexture requires the color aspect");

  auto* resource = requireResource(m_device->resolveTexture(texture),
    "D3D12CommandBuffer::clearColorTexture received an invalid texture");
  trackTexture(texture);
  const uint64_t descriptor =
    m_device->resolveTextureAttachmentDescriptor(texture, range);
  if(descriptor == 0)
    throw std::runtime_error(
      "D3D12CommandBuffer::clearColorTexture requires a matching render-target view");

  const D3D12_RESOURCE_DESC desc = resource->GetDesc();
  const uint32_t levelCount = range.levelCount == 0 ? desc.MipLevels : range.levelCount;
  const uint32_t layerCount = range.layerCount == 0
    ? (desc.Dimension == D3D12_RESOURCE_DIMENSION_TEXTURE3D ? 1u : desc.DepthOrArraySize)
    : range.layerCount;
  auto* list = commandList(m_commandList);
  const auto transitionSubresources = [&](D3D12_RESOURCE_STATES before,
                                           D3D12_RESOURCE_STATES after) {
    for(uint32_t layer = 0; layer < layerCount; ++layer)
    {
      for(uint32_t mip = 0; mip < levelCount; ++mip)
      {
        const D3D12_RESOURCE_BARRIER barrier{
          .Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION,
          .Flags = D3D12_RESOURCE_BARRIER_FLAG_NONE,
          .Transition = {
            .pResource = resource,
            .Subresource = subresourceIndex(
              desc, range.baseMipLevel + mip, range.baseArrayLayer + layer),
            .StateBefore = before,
            .StateAfter = after,
          },
        };
        list->ResourceBarrier(1, &barrier);
      }
    }
  };

  transitionSubresources(D3D12_RESOURCE_STATE_COMMON, D3D12_RESOURCE_STATE_RENDER_TARGET);
  const D3D12_CPU_DESCRIPTOR_HANDLE rtv{static_cast<SIZE_T>(descriptor)};
  const float value[4]{clearColor.r, clearColor.g, clearColor.b, clearColor.a};
  list->ClearRenderTargetView(rtv, value, 0, nullptr);
  transitionSubresources(D3D12_RESOURCE_STATE_RENDER_TARGET, D3D12_RESOURCE_STATE_COMMON);}

void D3D12CommandBuffer::beginEvent(const char* name)
{
  requireRecording("beginEvent");
  if(name == nullptr)
    throw std::invalid_argument("D3D12CommandBuffer::beginEvent requires a name");

  // D3D12 reserves metadata value 1 for a null-terminated ANSI event name.
  // This legacy encoding is understood by PIX, DRED, and RenderDoc without
  // requiring the WinPixEventRuntime DLL in a renderer backend.
  constexpr UINT kPixEventAnsiVersion = 1;
  commandList(m_commandList)->BeginEvent(
    kPixEventAnsiVersion,
    name,
    static_cast<UINT>(std::strlen(name) + 1));
}

void D3D12CommandBuffer::endEvent()
{
  requireRecording("endEvent");
  commandList(m_commandList)->EndEvent();
}

void D3D12CommandBuffer::resetQueryPool(
  QueryPoolHandle pool, uint32_t firstQuery, uint32_t queryCount)
{
  requireRecording("resetQueryPool");
  if(m_active != EncoderKind::none)
    throw std::runtime_error("D3D12CommandBuffer::resetQueryPool requires no active encoder");
  m_device->resetQueryPool(pool, firstQuery, queryCount);
  trackQueryPool(pool);
}

void D3D12CommandBuffer::writeTimestamp(
  QueryPoolHandle pool, uint32_t queryIndex, bool)
{
  requireRecording("writeTimestamp");
  if(m_active != EncoderKind::none)
    throw std::runtime_error("D3D12CommandBuffer::writeTimestamp requires no active encoder");
  m_device->writeTimestamp(m_commandList, pool, queryIndex);
  trackQueryPool(pool);
}

}  // namespace demo::rhi::d3d12
