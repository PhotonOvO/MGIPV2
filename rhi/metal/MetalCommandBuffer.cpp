#include "MetalCommandBuffer.h"

#include "MetalCommandAllocator.h"
#include "MetalCommon.h"
#include "MetalQueue.h"

#include "../RHIDebugCounters.h"

#include <Metal/MTL4ArgumentTable.hpp>
#include <Metal/MTL4CommandBuffer.hpp>
#include <Metal/MTL4ComputeCommandEncoder.hpp>
#include <Metal/MTL4Counters.hpp>
#include <Metal/MTL4RenderCommandEncoder.hpp>
#include <Metal/MTL4RenderPass.hpp>
#include <Metal/MTLArgumentEncoder.hpp>
#include <Metal/MTLBuffer.hpp>
#include <Metal/MTLComputePipeline.hpp>
#include <Metal/MTLDevice.hpp>
#include <Metal/MTLIndirectCommandBuffer.hpp>
#include <Metal/MTLRenderPipeline.hpp>
#include <Metal/MTLResidencySet.hpp>
#include <Metal/MTLSampler.hpp>
#include <Metal/MTLTexture.hpp>

#include <algorithm>
#include <array>
#include <cstring>
#include <iterator>
#include <limits>
#include <stdexcept>
#include <string>

namespace demo::rhi::metal {
namespace {

template <typename T>
void trackUnique(std::vector<T>& values, T value)
{
  if(!value.isValid())
    throw std::invalid_argument("Metal command recording received an invalid handle");
  if(std::find(values.begin(), values.end(), value) == values.end())
    values.push_back(value);
}

template <typename T>
NS::Object* asObject(T* object)
{
  return reinterpret_cast<NS::Object*>(object);
}

template <typename T, typename Pred>
T* findValue(std::vector<T>& values, Pred&& predicate)
{
  const auto found = std::find_if(values.begin(), values.end(),
                                  std::forward<Pred>(predicate));
  return found == values.end() ? nullptr : &*found;
}

const MetalBoundTable* findBoundTable(
  const std::vector<MetalBoundTable>& tables, uint32_t slot)
{
  const auto found = std::find_if(
    tables.begin(), tables.end(),
    [slot](const MetalBoundTable& value) { return value.slot == slot; });
  return found == tables.end() ? nullptr : &*found;
}

const MetalBoundTable* findBoundTable(
  const MetalEncoderBindingState& state, uint32_t slot)
{
  return findBoundTable(state.tables, slot);
}

const MetalArgumentValue* findArgumentValue(
  const MetalArgumentTableRecord& table, uint32_t binding, uint32_t arrayElement)
{
  const auto found = std::find_if(
    table.values.begin(), table.values.end(),
    [binding, arrayElement](const MetalArgumentValue& value) {
      return value.binding == binding && value.arrayElement == arrayElement;
    });
  return found == table.values.end() ? nullptr : &*found;
}

const ArgumentBinding* findLayoutBinding(
  const MetalArgumentLayoutRecord& layout, uint32_t binding)
{
  const auto found = std::find_if(
    layout.bindings.begin(), layout.bindings.end(),
    [binding](const ArgumentBinding& value) { return value.binding == binding; });
  return found == layout.bindings.end() ? nullptr : &*found;
}

bool layoutContainsPlacement(
  const MetalArgumentLayoutRecord& layout,
  const MetalArgumentPlacement& placement)
{
  const ArgumentBinding* binding = findLayoutBinding(layout, placement.binding);
  return binding != nullptr &&
         binding->type == placement.type &&
         binding->arrayCount >= placement.arrayCount &&
         binding->dynamicOffset == placement.dynamicOffset;
}

uint64_t checkedEnd(uint64_t offset, uint64_t size, uint64_t capacity,
                    const char* operation)
{
  if(offset > capacity || size > capacity - offset)
    throw std::out_of_range(std::string(operation) + " exceeds the resource bounds");
  return offset + size;
}

MTL::Origin metalOrigin(const Offset3D& offset, const char* operation)
{
  if(offset.x < 0 || offset.y < 0 || offset.z < 0)
    throw std::invalid_argument(std::string(operation) + " requires non-negative offsets");
  return MTL::Origin::Make(static_cast<NS::UInteger>(offset.x),
                           static_cast<NS::UInteger>(offset.y),
                           static_cast<NS::UInteger>(offset.z));
}

MTL::Size metalSize(uint32_t width, uint32_t height, uint32_t depth)
{
  if(width == 0 || height == 0 || depth == 0)
    throw std::invalid_argument("Metal copy/dispatch dimensions must be non-zero");
  return MTL::Size::Make(width, height, depth);
}

struct alignas(16) MetalBlitParameters
{
  int32_t sourceOffset0[2];
  int32_t sourceOffset1[2];
  int32_t destinationOffset0[2];
  int32_t destinationOffset1[2];
  uint32_t sourceMipLevel;
  uint32_t destinationMipLevel;
};

struct alignas(16) MetalIndexedIndirectPreparationParameters
{
  uint32_t maxDrawCount;
  uint32_t argumentStride;
  uint32_t primitiveType;
  uint32_t reserved;
};

constexpr uint32_t kIndexedIndirectCommandSize = 5u * sizeof(uint32_t);
constexpr uint32_t kIndexedIndirectThreadgroupSize = 64u;

uint32_t toMetalShaderPrimitive(PrimitiveTopology topology)
{
  switch(topology)
  {
  case PrimitiveTopology::pointList:     return 0u;
  case PrimitiveTopology::lineList:      return 1u;
  case PrimitiveTopology::lineStrip:     return 2u;
  case PrimitiveTopology::triangleList:  return 3u;
  case PrimitiveTopology::triangleStrip: return 4u;
  }
  return 3u;
}

uint32_t mipDimension(uint32_t dimension, uint32_t mipLevel)
{
  return mipLevel >= 32 ? 1u : std::max(1u, dimension >> mipLevel);
}

TextureSubresourceRange resolveTextureRange(
  const TextureSubresourceRange& range, const TextureDesc& desc,
  const char* operation)
{
  if(range.baseMipLevel >= desc.mipLevels ||
     range.baseArrayLayer >= desc.arrayLayers)
  {
    throw std::out_of_range(std::string(operation) +
                            " base subresource is outside the texture");
  }
  TextureSubresourceRange result = range;
  if(result.levelCount == ~0u)
    result.levelCount = desc.mipLevels - result.baseMipLevel;
  if(result.layerCount == ~0u)
    result.layerCount = desc.arrayLayers - result.baseArrayLayer;
  if(result.levelCount == 0 || result.layerCount == 0 ||
     result.levelCount > desc.mipLevels - result.baseMipLevel ||
     result.layerCount > desc.arrayLayers - result.baseArrayLayer)
  {
    throw std::out_of_range(std::string(operation) +
                            " subresource range exceeds the texture");
  }
  return result;
}

uint32_t validateBlitAxis(
  int32_t first, int32_t second, uint32_t limit, const char* axis)
{
  const int64_t first64 = first;
  const int64_t second64 = second;
  const int64_t lower = std::min(first64, second64);
  const int64_t upper = std::max(first64, second64);
  if(lower < 0 || upper > static_cast<int64_t>(limit))
    throw std::out_of_range(std::string("Metal texture blit ") + axis +
                            " range exceeds its mip extent");
  if(lower == upper)
    throw std::invalid_argument(std::string("Metal texture blit ") + axis +
                                " extent must not be zero");
  return static_cast<uint32_t>(upper - lower);
}

bool isDepthFormat(TextureFormat format)
{
  return format == TextureFormat::d16Unorm ||
         format == TextureFormat::d32Sfloat ||
         format == TextureFormat::d24UnormS8 ||
         format == TextureFormat::d32SfloatS8;
}

MTL::CullMode metalCullMode(CullMode mode)
{
  switch(mode)
  {
  case CullMode::none:  return MTL::CullModeNone;
  case CullMode::front: return MTL::CullModeFront;
  case CullMode::back:  return MTL::CullModeBack;
  case CullMode::frontAndBack:
    throw std::runtime_error("Metal does not support culling both triangle faces");
  }
  return MTL::CullModeNone;
}

StageFlags stageForState(ResourceState state)
{
  switch(state)
  {
  case ResourceState::ColorAttachment:        return StageFlags::rasterColorOut;
  case ResourceState::DepthStencilAttachment:
  case ResourceState::DepthStencilReadOnly:   return StageFlags::rasterDepthOut;
  case ResourceState::ShaderRead:
  case ResourceState::ShaderWrite:             return StageFlags::compute |
                                                       StageFlags::vertexShader |
                                                       StageFlags::fragmentShader;
  case ResourceState::TransferSrc:
  case ResourceState::TransferDst:             return StageFlags::transfer |
                                                       StageFlags::compute;
  case ResourceState::IndirectArgument:        return StageFlags::commandInput;
  case ResourceState::Undefined:
  case ResourceState::General:
  case ResourceState::Present:                 return StageFlags::all;
  }
  return StageFlags::all;
}

HazardFlags hazardForState(ResourceState state)
{
  switch(state)
  {
  case ResourceState::ColorAttachment:
  case ResourceState::DepthStencilAttachment:
  case ResourceState::ShaderWrite:
  case ResourceState::TransferDst:
  case ResourceState::General:
    return HazardFlags::bufferWrites | HazardFlags::textureWrites;
  default:
    return HazardFlags::readBeforeWrite;
  }
}

bool hasStencil(TextureFormat format)
{
  return format == TextureFormat::d24UnormS8 ||
         format == TextureFormat::d32SfloatS8;
}

uint32_t defaultIndirectStride(uint32_t requested, uint32_t nativeSize)
{
  const uint32_t result = requested == 0 ? nativeSize : requested;
  if(result < nativeSize || (result & 3u) != 0)
    throw std::invalid_argument("Metal indirect-command stride is invalid");
  return result;
}

}  // namespace

void MetalEncoderBindingState::reset()
{
  pipeline = {};
  tables.clear();
  pendingDynamicBuffers.clear();
  dynamicBuffers.clear();
  roots.clear();
  vertexBuffers.clear();
  snapshot = nullptr;
  dirty = true;
}

void MetalRenderEncoder::prepare(
  MTL4::RenderCommandEncoder* encoder, MetalCommandBuffer& owner)
{
  require(encoder != nullptr, "Metal render pass failed to create an MTL4 encoder");
  m_encoder = encoder;
  m_owner = &owner;
  m_bindings.reset();
  m_indexBuffer = {};
  m_indexOffset = 0;
  m_indexFormat = IndexFormat::uint32;
  m_valid = true;
}

void MetalRenderEncoder::invalidate() noexcept
{
  m_encoder = nullptr;
  m_owner = nullptr;
  m_valid = false;
}

void MetalRenderEncoder::requireActive() const
{
  if(!m_valid || m_encoder == nullptr || m_owner == nullptr)
    throw std::runtime_error("Metal render encoder is outside its encoding scope");
}

const MetalPipelineRecord& MetalRenderEncoder::requirePipeline() const
{
  requireActive();
  const MetalPipelineRecord* pipeline =
    m_owner->resources().tryGetPipeline(m_bindings.pipeline);
  if(pipeline == nullptr || pipeline->kind != MetalPipelineKind::graphics ||
     pipeline->nativeRenderPipeline == nullptr)
  {
    throw std::runtime_error("Metal render encoder requires a live graphics pipeline");
  }
  return *pipeline;
}

void MetalRenderEncoder::setPipeline(PipelineHandle pipelineHandle)
{
  requireActive();
  const MetalPipelineRecord* pipeline =
    m_owner->resources().tryGetPipeline(pipelineHandle);
  if(pipeline == nullptr || pipeline->kind != MetalPipelineKind::graphics ||
     pipeline->nativeRenderPipeline == nullptr)
  {
    throw std::invalid_argument("MetalRenderEncoder::setPipeline received a stale or non-graphics pipeline");
  }

  m_bindings.pipeline = pipelineHandle;
  m_bindings.dirty = true;
  m_bindings.snapshot = nullptr;
  m_owner->trackPipeline(pipelineHandle);
  m_owner->retainNative(asObject(pipeline->nativeRenderPipeline));
  if(pipeline->nativeDepthStencilState != nullptr)
    m_owner->retainNative(asObject(pipeline->nativeDepthStencilState));

  m_encoder->setRenderPipelineState(pipeline->nativeRenderPipeline);
  m_encoder->setDepthStencilState(pipeline->nativeDepthStencilState);
  m_encoder->setCullMode(metalCullMode(pipeline->rasterState.cullMode));
  m_encoder->setFrontFacingWinding(
    pipeline->rasterState.frontFace == FrontFace::clockwise
      ? MTL::WindingClockwise
      : MTL::WindingCounterClockwise);
  if(pipeline->rasterState.polygonMode == PolygonMode::point)
    throw std::runtime_error("Metal does not support point polygon fill mode");
  m_encoder->setTriangleFillMode(
    pipeline->rasterState.polygonMode == PolygonMode::line
      ? MTL::TriangleFillModeLines
      : MTL::TriangleFillModeFill);
  if(pipeline->rasterState.depthBiasEnable)
  {
    m_encoder->setDepthBias(pipeline->rasterState.depthBiasConstantFactor,
                            pipeline->rasterState.depthBiasSlopeFactor,
                            pipeline->rasterState.depthBiasClamp);
  }
  m_owner->recordCommand([pipelineHandle](MetalCommandBuffer& owner) {
    owner.m_renderEncoder.setPipeline(pipelineHandle);
  });
}

void MetalRenderEncoder::setArgumentTable(
  ShaderStage stages, uint32_t slot, ArgumentTableHandle table)
{
  requireActive();
  if(!any(stages) || (static_cast<uint32_t>(stages) &
                      ~static_cast<uint32_t>(ShaderStage::allGraphics)) != 0)
  {
    throw std::invalid_argument("Metal render argument table requires graphics-stage visibility");
  }
  if(m_owner->resources().tryGetArgumentTable(table) == nullptr)
    throw std::invalid_argument("Metal render argument-table binding is stale");
  MetalBoundTable* bound = findValue(
    m_bindings.tables, [slot](const MetalBoundTable& value) { return value.slot == slot; });
  if(bound == nullptr)
    m_bindings.tables.push_back(MetalBoundTable{slot, table, stages});
  else
    *bound = MetalBoundTable{slot, table, stages};
  std::erase_if(m_bindings.dynamicBuffers,
                [slot](const MetalDynamicBufferValue& value) {
                  return value.tableSlot == slot;
                });
  for(const MetalDynamicBufferValue& value : m_bindings.pendingDynamicBuffers)
  {
    if(value.tableSlot == slot)
      m_bindings.dynamicBuffers.push_back(value);
  }
  std::erase_if(m_bindings.pendingDynamicBuffers,
                [slot](const MetalDynamicBufferValue& value) {
                  return value.tableSlot == slot;
                });
  m_bindings.dirty = true;
  m_owner->trackArgumentTable(table);
  m_owner->recordCommand([stages, slot, table](MetalCommandBuffer& owner) {
    owner.m_renderEncoder.setArgumentTable(stages, slot, table);
  });
}

void MetalRenderEncoder::setDynamicBuffer(
  ShaderStage stages, uint32_t slot, BufferHandle buffer, uint64_t offset, uint64_t size)
{
  requireActive();
  if(!any(stages))
    throw std::invalid_argument("Metal dynamic buffer requires shader visibility");
  if(buffer.isValid())
  {
    const MetalBufferRecord* record = m_owner->resources().tryGetBuffer(buffer);
    if(record == nullptr)
      throw std::invalid_argument("Metal dynamic buffer is stale");
    const uint64_t resolvedSize = size == 0 ? record->desc.size - offset : size;
    checkedEnd(offset, resolvedSize, record->desc.size, "Metal dynamic buffer");
    m_owner->trackBuffer(buffer);
  }
  m_bindings.pendingDynamicBuffers.push_back(
    MetalDynamicBufferValue{slot, buffer, offset, size});
  m_owner->recordCommand(
    [stages, slot, buffer, offset, size](MetalCommandBuffer& owner) {
      owner.m_renderEncoder.setDynamicBuffer(stages, slot, buffer, offset, size);
    });
}

void MetalRenderEncoder::setRootConstants(
  ShaderStage stages, uint32_t slot, std::span<const std::byte> data)
{
  requireActive();
  if(data.empty() || !any(stages))
    throw std::invalid_argument("Metal root constants require data and visibility");
  MetalRootValue* root = findValue(
    m_bindings.roots, [slot](const MetalRootValue& value) { return value.slot == slot; });
  if(root == nullptr)
  {
    m_bindings.roots.push_back(MetalRootValue{});
    root = &m_bindings.roots.back();
  }
  root->slot = slot;
  root->kind = RootBindingKind::constants;
  root->stages = stages;
  root->constants.assign(data.begin(), data.end());
  root->pointer = {};
  m_bindings.dirty = true;
  std::vector<std::byte> replayData(data.begin(), data.end());
  m_owner->recordCommand(
    [stages, slot, replayData = std::move(replayData)](MetalCommandBuffer& owner) {
      owner.m_renderEncoder.setRootConstants(stages, slot, replayData);
    });
}

void MetalRenderEncoder::setRootPointer(
  ShaderStage stages, uint32_t slot, GpuPtr ptr)
{
  requireActive();
  if(!ptr.isValid() || !any(stages))
    throw std::invalid_argument("Metal root pointer requires an address and visibility");
  MetalRootValue* root = findValue(
    m_bindings.roots, [slot](const MetalRootValue& value) { return value.slot == slot; });
  if(root == nullptr)
  {
    m_bindings.roots.push_back(MetalRootValue{});
    root = &m_bindings.roots.back();
  }
  root->slot = slot;
  root->kind = RootBindingKind::gpuPointer;
  root->stages = stages;
  root->constants.clear();
  root->pointer = ptr;
  m_bindings.dirty = true;
  m_owner->recordCommand([stages, slot, ptr](MetalCommandBuffer& owner) {
    owner.m_renderEncoder.setRootPointer(stages, slot, ptr);
  });
}

void MetalRenderEncoder::setViewport(const Viewport& viewport)
{
  requireActive();
  if(viewport.width < 0.0f || viewport.height < 0.0f)
    throw std::invalid_argument("Metal viewport dimensions must be non-negative");
  m_encoder->setViewport(MTL::Viewport{
    viewport.x, viewport.y, viewport.width, viewport.height,
    viewport.minDepth, viewport.maxDepth});
  m_owner->recordCommand([viewport](MetalCommandBuffer& owner) {
    owner.m_renderEncoder.setViewport(viewport);
  });
}

void MetalRenderEncoder::setScissor(const Rect2D& scissor)
{
  requireActive();
  if(scissor.offset.x < 0 || scissor.offset.y < 0)
    throw std::invalid_argument("Metal scissor offsets must be non-negative");
  m_encoder->setScissorRect(MTL::ScissorRect{
    static_cast<NS::UInteger>(scissor.offset.x),
    static_cast<NS::UInteger>(scissor.offset.y),
    scissor.extent.width, scissor.extent.height});
  m_owner->recordCommand([scissor](MetalCommandBuffer& owner) {
    owner.m_renderEncoder.setScissor(scissor);
  });
}

void MetalRenderEncoder::bindVertexBuffers(
  uint32_t firstBinding, std::span<const VertexBufferBinding> bindings)
{
  requireActive();
  for(uint32_t index = 0; index < bindings.size(); ++index)
  {
    const uint32_t binding = firstBinding + index;
    const VertexBufferBinding& value = bindings[index];
    const MetalBufferRecord* buffer = m_owner->resources().tryGetBuffer(value.buffer);
    if(buffer == nullptr)
      throw std::invalid_argument("Metal vertex-buffer binding is stale");
    checkedEnd(value.offset, 0, buffer->desc.size, "Metal vertex-buffer binding");
    MetalVertexBufferValue* bound = findValue(
      m_bindings.vertexBuffers,
      [binding](const MetalVertexBufferValue& existing) {
        return existing.binding == binding;
      });
    if(bound == nullptr)
      m_bindings.vertexBuffers.push_back({binding, value.buffer, value.offset});
    else
      *bound = {binding, value.buffer, value.offset};
    m_owner->trackBuffer(value.buffer);
  }
  m_bindings.dirty = true;
  std::vector<VertexBufferBinding> replayBindings(bindings.begin(), bindings.end());
  m_owner->recordCommand(
    [firstBinding, replayBindings = std::move(replayBindings)](
      MetalCommandBuffer& owner) {
      owner.m_renderEncoder.bindVertexBuffers(firstBinding, replayBindings);
    });
}

void MetalRenderEncoder::bindIndexBuffer(
  BufferHandle buffer, uint64_t offset, IndexFormat format)
{
  requireActive();
  const MetalBufferRecord* record = m_owner->resources().tryGetBuffer(buffer);
  if(record == nullptr)
    throw std::invalid_argument("Metal index-buffer binding is stale");
  checkedEnd(offset, 0, record->desc.size, "Metal index-buffer binding");
  m_indexBuffer = buffer;
  m_indexOffset = offset;
  m_indexFormat = format;
  m_owner->trackBuffer(buffer);
  m_owner->recordCommand([buffer, offset, format](MetalCommandBuffer& owner) {
    owner.m_renderEncoder.bindIndexBuffer(buffer, offset, format);
  });
}

void MetalRenderEncoder::readInputAttachment(uint32_t index)
{
  requireActive();
  // Metal shaders consume the fallback sampled view through the composed table.
  m_owner->recordCommand([index](MetalCommandBuffer& owner) {
    owner.m_renderEncoder.readInputAttachment(index);
  });
}

void MetalRenderEncoder::bindSnapshot()
{
  const MetalPipelineRecord& pipeline = requirePipeline();
  MTL4::ArgumentTable* snapshot = m_owner->materializeArgumentTable(
    m_bindings, MetalPipelineKind::graphics);
  if(snapshot != nullptr)
    m_encoder->setArgumentTable(snapshot, toMetalRenderStages(pipeline.bindingLayout.stages));
}

void MetalRenderEncoder::draw(const DrawDesc& desc)
{
  bindSnapshot();
  const MetalPipelineRecord& pipeline = requirePipeline();
  m_encoder->drawPrimitives(toMetalPrimitive(pipeline.rasterState.topology),
                            desc.firstVertex, desc.vertexCount,
                            desc.instanceCount, desc.firstInstance);
  m_owner->recordCommand([desc](MetalCommandBuffer& owner) {
    owner.m_renderEncoder.draw(desc);
  });
}

void MetalRenderEncoder::drawIndexed(const DrawIndexedDesc& desc)
{
  requireActive();
  if(desc.indexBuffer.isValid())
    bindIndexBuffer(desc.indexBuffer, desc.indexBufferOffset, desc.indexFormat);
  if(!m_indexBuffer.isValid())
    throw std::runtime_error("Metal indexed draw requires an index buffer");
  bindSnapshot();
  const MetalPipelineRecord& pipeline = requirePipeline();
  const MetalBufferRecord* index = m_owner->resources().tryGetBuffer(m_indexBuffer);
  const uint64_t firstByte = m_indexOffset +
    static_cast<uint64_t>(desc.firstIndex) * indexElementSize(m_indexFormat);
  checkedEnd(firstByte, static_cast<uint64_t>(desc.indexCount) * indexElementSize(m_indexFormat),
             index->desc.size, "Metal indexed draw");
  m_owner->retainNative(asObject(index->nativeBuffer));
  m_encoder->drawIndexedPrimitives(
    toMetalPrimitive(pipeline.rasterState.topology), desc.indexCount,
    toMetalIndexType(m_indexFormat), index->nativeBuffer->gpuAddress() + firstByte,
    index->desc.size - firstByte, desc.instanceCount, desc.vertexOffset,
    desc.firstInstance);
  m_owner->recordCommand([desc](MetalCommandBuffer& owner) {
    owner.m_renderEncoder.drawIndexed(desc);
  });
}

void MetalRenderEncoder::drawIndexedIndirect(const DrawIndirectDesc& desc)
{
  requireActive();
  if(desc.drawCount == 0)
    return;
  if(!m_indexBuffer.isValid())
    throw std::runtime_error("Metal indexed indirect draw requires an index buffer");
  bindSnapshot();
  const MetalPipelineRecord& pipeline = requirePipeline();
  const MetalBufferRecord* index = m_owner->resources().tryGetBuffer(m_indexBuffer);
  const MetalBufferRecord* args = m_owner->resources().tryGetBuffer(desc.argsBuffer);
  if(index == nullptr || args == nullptr)
    throw std::invalid_argument("Metal indexed indirect draw references a stale buffer");
  constexpr uint32_t commandSize = 20;
  const uint32_t stride = defaultIndirectStride(desc.stride, commandSize);
  checkedEnd(desc.offset, static_cast<uint64_t>(stride) * desc.drawCount,
             args->desc.size, "Metal indexed indirect draw");
  m_owner->trackBuffer(desc.argsBuffer);
  m_owner->retainNative(asObject(index->nativeBuffer));
  m_owner->retainNative(asObject(args->nativeBuffer));

  MetalPreparedIndexedIndirect* prepared =
    m_owner->consumePreparedIndexedIndirect(
      desc, m_indexBuffer, m_indexOffset, m_indexFormat,
      pipeline.rasterState.topology);
  if(prepared != nullptr && pipeline.supportsGpuGeneratedCommands)
  {
    m_encoder->executeCommandsInBuffer(
      prepared->commandBuffer,
      prepared->executionRangeBuffer->gpuAddress() +
        prepared->executionRangeOffset);
  }
  else
  {
    if(prepared != nullptr && prepared->clampedArgumentsBuffer == nullptr)
    {
      throw std::runtime_error(
        "Metal indexed-indirect fallback preparation is incomplete");
    }
    for(uint32_t drawIndex = 0; drawIndex < desc.drawCount; ++drawIndex)
    {
      const uint64_t indirectAddress = prepared != nullptr
        ? prepared->clampedArgumentsBuffer->gpuAddress() +
            prepared->clampedArgumentsOffset +
            static_cast<uint64_t>(drawIndex) * prepared->argumentStride
        : args->nativeBuffer->gpuAddress() + desc.offset +
            static_cast<uint64_t>(drawIndex) * stride;
      m_encoder->drawIndexedPrimitives(
        toMetalPrimitive(pipeline.rasterState.topology),
        toMetalIndexType(m_indexFormat),
        index->nativeBuffer->gpuAddress() + m_indexOffset,
        index->desc.size - m_indexOffset, indirectAddress);
    }
  }
  m_owner->recordCommand([desc](MetalCommandBuffer& owner) {
    owner.m_renderEncoder.drawIndexedIndirect(desc);
  });
}

void MetalRenderEncoder::drawIndexedIndirectCount(const DrawIndirectCountDesc& desc)
{
  requireActive();
  if(desc.maxDrawCount == 0)
    return;
  if(!m_indexBuffer.isValid())
    throw std::runtime_error(
      "Metal indexed-indirect-count draw requires an index buffer");

  bindSnapshot();
  const MetalPipelineRecord& pipeline = requirePipeline();

  MetalPreparedIndexedIndirect& prepared =
    m_owner->consumePreparedIndexedIndirectCount(
      desc, m_indexBuffer, m_indexOffset, m_indexFormat,
      pipeline.rasterState.topology);
  if(pipeline.supportsGpuGeneratedCommands)
  {
    m_encoder->executeCommandsInBuffer(
      prepared.commandBuffer,
      prepared.executionRangeBuffer->gpuAddress() + prepared.executionRangeOffset);
  }
  else
  {
    const MetalBufferRecord* index =
      m_owner->resources().tryGetBuffer(m_indexBuffer);
    if(index == nullptr || prepared.clampedArgumentsBuffer == nullptr)
    {
      throw std::runtime_error(
        "Metal indexed-indirect-count fallback preparation is incomplete");
    }
    for(uint32_t drawIndex = 0; drawIndex < desc.maxDrawCount; ++drawIndex)
    {
      m_encoder->drawIndexedPrimitives(
        toMetalPrimitive(pipeline.rasterState.topology),
        toMetalIndexType(m_indexFormat),
        index->nativeBuffer->gpuAddress() + m_indexOffset,
        index->desc.size - m_indexOffset,
        prepared.clampedArgumentsBuffer->gpuAddress() +
          prepared.clampedArgumentsOffset +
          static_cast<uint64_t>(drawIndex) * prepared.argumentStride);
    }
  }
  m_owner->recordCommand([desc](MetalCommandBuffer& owner) {
    owner.m_renderEncoder.drawIndexedIndirectCount(desc);
  });
}

void MetalRenderEncoder::drawIndirect(const DrawIndirectDesc& desc)
{
  requireActive();
  bindSnapshot();
  const MetalPipelineRecord& pipeline = requirePipeline();
  const MetalBufferRecord* args = m_owner->resources().tryGetBuffer(desc.argsBuffer);
  if(args == nullptr)
    throw std::invalid_argument("Metal indirect draw references a stale buffer");
  constexpr uint32_t commandSize = 16;
  const uint32_t stride = defaultIndirectStride(desc.stride, commandSize);
  checkedEnd(desc.offset, static_cast<uint64_t>(stride) * desc.drawCount,
             args->desc.size, "Metal indirect draw");
  m_owner->trackBuffer(desc.argsBuffer);
  m_owner->retainNative(asObject(args->nativeBuffer));
  for(uint32_t drawIndex = 0; drawIndex < desc.drawCount; ++drawIndex)
  {
    m_encoder->drawPrimitives(
      toMetalPrimitive(pipeline.rasterState.topology),
      args->nativeBuffer->gpuAddress() + desc.offset +
        static_cast<uint64_t>(drawIndex) * stride);
  }
  m_owner->recordCommand([desc](MetalCommandBuffer& owner) {
    owner.m_renderEncoder.drawIndirect(desc);
  });
}

void MetalRenderEncoder::drawMeshTasks(
  uint32_t groupCountX, uint32_t groupCountY, uint32_t groupCountZ)
{
  bindSnapshot();
  m_encoder->drawMeshThreadgroups(
    MTL::Size::Make(groupCountX, groupCountY, groupCountZ),
    MTL::Size::Make(1, 1, 1), MTL::Size::Make(1, 1, 1));
  m_owner->recordCommand(
    [groupCountX, groupCountY, groupCountZ](MetalCommandBuffer& owner) {
      owner.m_renderEncoder.drawMeshTasks(groupCountX, groupCountY, groupCountZ);
    });
}

void MetalRenderEncoder::drawMeshTasksIndirect(const DrawIndirectDesc& desc)
{
  requireActive();
  bindSnapshot();
  const MetalBufferRecord* args = m_owner->resources().tryGetBuffer(desc.argsBuffer);
  if(args == nullptr || desc.drawCount != 1)
    throw std::invalid_argument("Metal mesh indirect draw requires one live command");
  checkedEnd(desc.offset, 12, args->desc.size, "Metal mesh indirect draw");
  m_owner->trackBuffer(desc.argsBuffer);
  m_owner->retainNative(asObject(args->nativeBuffer));
  m_encoder->drawMeshThreadgroups(args->nativeBuffer->gpuAddress() + desc.offset,
                                  MTL::Size::Make(1, 1, 1),
                                  MTL::Size::Make(1, 1, 1));
  m_owner->recordCommand([desc](MetalCommandBuffer& owner) {
    owner.m_renderEncoder.drawMeshTasksIndirect(desc);
  });
}

void MetalComputeEncoder::prepare(
  MTL4::ComputeCommandEncoder* encoder, MetalCommandBuffer& owner)
{
  require(encoder != nullptr, "Metal compute pass failed to create an MTL4 encoder");
  m_encoder = encoder;
  m_owner = &owner;
  m_bindings.reset();
  m_valid = true;
}

void MetalComputeEncoder::invalidate() noexcept
{
  m_encoder = nullptr;
  m_owner = nullptr;
  m_valid = false;
}

void MetalComputeEncoder::requireActive() const
{
  if(!m_valid || m_encoder == nullptr || m_owner == nullptr)
    throw std::runtime_error("Metal compute encoder is outside its encoding scope");
}

void MetalComputeEncoder::requireCompute(const char* command) const
{
  requireActive();
  m_owner->requireQueueOperation(QueueOperation::compute, command);
}

void MetalComputeEncoder::requireTransfer(const char* command) const
{
  requireActive();
  m_owner->requireQueueOperation(QueueOperation::transfer, command);
}

const MetalPipelineRecord& MetalComputeEncoder::requirePipeline() const
{
  requireActive();
  const MetalPipelineRecord* pipeline =
    m_owner->resources().tryGetPipeline(m_bindings.pipeline);
  if(pipeline == nullptr || pipeline->kind != MetalPipelineKind::compute ||
     pipeline->nativeComputePipeline == nullptr)
  {
    throw std::runtime_error("Metal compute encoder requires a live compute pipeline");
  }
  return *pipeline;
}

void MetalComputeEncoder::setPipeline(PipelineHandle pipelineHandle)
{
  requireCompute("MetalComputeEncoder::setPipeline");
  const MetalPipelineRecord* pipeline =
    m_owner->resources().tryGetPipeline(pipelineHandle);
  if(pipeline == nullptr || pipeline->kind != MetalPipelineKind::compute ||
     pipeline->nativeComputePipeline == nullptr)
  {
    throw std::invalid_argument("MetalComputeEncoder::setPipeline received a stale or non-compute pipeline");
  }
  m_bindings.pipeline = pipelineHandle;
  m_bindings.dirty = true;
  m_bindings.snapshot = nullptr;
  m_owner->trackPipeline(pipelineHandle);
  m_owner->retainNative(asObject(pipeline->nativeComputePipeline));
  m_encoder->setComputePipelineState(pipeline->nativeComputePipeline);
  m_owner->recordCommand([pipelineHandle](MetalCommandBuffer& owner) {
    owner.m_computeEncoder.setPipeline(pipelineHandle);
  });
}

void MetalComputeEncoder::setArgumentTable(
  uint32_t slot, ArgumentTableHandle table)
{
  requireCompute("MetalComputeEncoder::setArgumentTable");
  if(m_owner->resources().tryGetArgumentTable(table) == nullptr)
    throw std::invalid_argument("Metal compute argument-table binding is stale");
  MetalBoundTable* bound = findValue(
    m_bindings.tables, [slot](const MetalBoundTable& value) { return value.slot == slot; });
  if(bound == nullptr)
    m_bindings.tables.push_back(MetalBoundTable{slot, table, ShaderStage::compute});
  else
    *bound = MetalBoundTable{slot, table, ShaderStage::compute};
  std::erase_if(m_bindings.dynamicBuffers,
                [slot](const MetalDynamicBufferValue& value) {
                  return value.tableSlot == slot;
                });
  for(const MetalDynamicBufferValue& value : m_bindings.pendingDynamicBuffers)
  {
    if(value.tableSlot == slot)
      m_bindings.dynamicBuffers.push_back(value);
  }
  std::erase_if(m_bindings.pendingDynamicBuffers,
                [slot](const MetalDynamicBufferValue& value) {
                  return value.tableSlot == slot;
                });
  m_bindings.dirty = true;
  m_owner->trackArgumentTable(table);
  m_owner->recordCommand([slot, table](MetalCommandBuffer& owner) {
    owner.m_computeEncoder.setArgumentTable(slot, table);
  });
}

void MetalComputeEncoder::setRootConstants(
  uint32_t slot, std::span<const std::byte> data)
{
  requireCompute("MetalComputeEncoder::setRootConstants");
  if(data.empty())
    throw std::invalid_argument("Metal compute root constants must not be empty");
  MetalRootValue* root = findValue(
    m_bindings.roots, [slot](const MetalRootValue& value) { return value.slot == slot; });
  if(root == nullptr)
  {
    m_bindings.roots.push_back(MetalRootValue{});
    root = &m_bindings.roots.back();
  }
  root->slot = slot;
  root->kind = RootBindingKind::constants;
  root->stages = ShaderStage::compute;
  root->constants.assign(data.begin(), data.end());
  root->pointer = {};
  m_bindings.dirty = true;
  std::vector<std::byte> replayData(data.begin(), data.end());
  m_owner->recordCommand(
    [slot, replayData = std::move(replayData)](MetalCommandBuffer& owner) {
      owner.m_computeEncoder.setRootConstants(slot, replayData);
    });
}

void MetalComputeEncoder::setRootPointer(uint32_t slot, GpuPtr ptr)
{
  requireCompute("MetalComputeEncoder::setRootPointer");
  if(!ptr.isValid())
    throw std::invalid_argument("Metal compute root pointer is invalid");
  MetalRootValue* root = findValue(
    m_bindings.roots, [slot](const MetalRootValue& value) { return value.slot == slot; });
  if(root == nullptr)
  {
    m_bindings.roots.push_back(MetalRootValue{});
    root = &m_bindings.roots.back();
  }
  root->slot = slot;
  root->kind = RootBindingKind::gpuPointer;
  root->stages = ShaderStage::compute;
  root->constants.clear();
  root->pointer = ptr;
  m_bindings.dirty = true;
  m_owner->recordCommand([slot, ptr](MetalCommandBuffer& owner) {
    owner.m_computeEncoder.setRootPointer(slot, ptr);
  });
}

void MetalComputeEncoder::bindSnapshot()
{
  requirePipeline();
  MTL4::ArgumentTable* snapshot = m_owner->materializeArgumentTable(
    m_bindings, MetalPipelineKind::compute);
  if(snapshot != nullptr)
    m_encoder->setArgumentTable(snapshot);
}

void MetalComputeEncoder::dispatch(const DispatchDesc& desc)
{
  requireCompute("MetalComputeEncoder::dispatch");
  bindSnapshot();
  const MetalPipelineRecord& pipeline = requirePipeline();
  m_encoder->dispatchThreadgroups(
    metalSize(desc.groupCountX, desc.groupCountY, desc.groupCountZ),
    metalSize(pipeline.computeThreadsPerThreadgroup.width,
              pipeline.computeThreadsPerThreadgroup.height,
              pipeline.computeThreadsPerThreadgroup.depth));
  m_owner->recordCommand([desc](MetalCommandBuffer& owner) {
    owner.m_computeEncoder.dispatch(desc);
  });
}

void MetalComputeEncoder::dispatchIndirect(const DispatchIndirectDesc& desc)
{
  requireCompute("MetalComputeEncoder::dispatchIndirect");
  bindSnapshot();
  const MetalPipelineRecord& pipeline = requirePipeline();
  const MetalBufferRecord* args = m_owner->resources().tryGetBuffer(desc.argsBuffer);
  if(args == nullptr)
    throw std::invalid_argument("Metal indirect dispatch buffer is stale");
  checkedEnd(desc.offset, 12, args->desc.size, "Metal indirect dispatch");
  m_owner->trackBuffer(desc.argsBuffer);
  m_owner->retainNative(asObject(args->nativeBuffer));
  m_encoder->dispatchThreadgroups(
    args->nativeBuffer->gpuAddress() + desc.offset,
    metalSize(pipeline.computeThreadsPerThreadgroup.width,
              pipeline.computeThreadsPerThreadgroup.height,
              pipeline.computeThreadsPerThreadgroup.depth));
  m_owner->recordCommand([desc](MetalCommandBuffer& owner) {
    owner.m_computeEncoder.dispatchIndirect(desc);
  });
}

void MetalComputeEncoder::copyBuffer(
  BufferHandle srcHandle, uint64_t srcOffset, BufferHandle dstHandle,
  uint64_t dstOffset, uint64_t size)
{
  requireTransfer("MetalComputeEncoder::copyBuffer");
  const MetalBufferRecord* src = m_owner->resources().tryGetBuffer(srcHandle);
  const MetalBufferRecord* dst = m_owner->resources().tryGetBuffer(dstHandle);
  if(src == nullptr || dst == nullptr || size == 0)
    throw std::invalid_argument("Metal buffer copy requires live buffers and non-zero size");
  checkedEnd(srcOffset, size, src->desc.size, "Metal source buffer copy");
  checkedEnd(dstOffset, size, dst->desc.size, "Metal destination buffer copy");
  m_owner->trackBuffer(srcHandle);
  m_owner->trackBuffer(dstHandle);
  m_owner->retainNative(asObject(src->nativeBuffer));
  m_owner->retainNative(asObject(dst->nativeBuffer));
  m_encoder->copyFromBuffer(src->nativeBuffer, srcOffset, dst->nativeBuffer,
                            dstOffset, size);
  m_owner->recordCommand(
    [srcHandle, srcOffset, dstHandle, dstOffset, size](MetalCommandBuffer& owner) {
      owner.m_computeEncoder.copyBuffer(srcHandle, srcOffset, dstHandle,
                                        dstOffset, size);
    });
}

void MetalComputeEncoder::copyBufferToTexture(const BufferTextureCopyDesc& desc)
{
  requireTransfer("MetalComputeEncoder::copyBufferToTexture");
  if(desc.aspect != TextureAspect::color)
    throw std::runtime_error("Metal buffer-to-texture depth/stencil copies are not supported");
  const MetalBufferRecord* buffer = m_owner->resources().tryGetBuffer(desc.buffer);
  const MetalTextureRecord* texture = m_owner->resources().tryGetTexture(desc.texture);
  if(buffer == nullptr || texture == nullptr)
    throw std::invalid_argument("Metal buffer-to-texture copy references a stale resource");
  if(desc.mipLevel >= texture->desc.mipLevels ||
     desc.baseArrayLayer + desc.layerCount > texture->desc.arrayLayers)
    throw std::out_of_range("Metal buffer-to-texture subresource is out of range");
  const uint32_t bytesPerRow = copyBytesPerRow(texture->desc.format, desc.width);
  const uint32_t blockRows = isBlockCompressed(texture->desc.format)
    ? std::max(1u, (desc.height + 3u) / 4u)
    : desc.height;
  const uint64_t bytesPerImage = static_cast<uint64_t>(bytesPerRow) * blockRows;
  const uint64_t bytesPerLayer = bytesPerImage * desc.depth;
  checkedEnd(desc.bufferOffset, bytesPerLayer * desc.layerCount,
             buffer->desc.size, "Metal buffer-to-texture copy");
  const MTL::Size copySize = metalSize(desc.width, desc.height, desc.depth);
  const MTL::Origin origin = metalOrigin(desc.textureOffset,
                                         "Metal buffer-to-texture copy");
  for(uint32_t layer = 0; layer < desc.layerCount; ++layer)
  {
    m_encoder->copyFromBuffer(
      buffer->nativeBuffer, desc.bufferOffset + bytesPerLayer * layer,
      bytesPerRow, bytesPerImage, copySize, texture->nativeTexture,
      desc.baseArrayLayer + layer, desc.mipLevel, origin);
  }
  m_owner->trackBuffer(desc.buffer);
  m_owner->trackTexture(desc.texture);
  m_owner->retainNative(asObject(buffer->nativeBuffer));
  m_owner->retainNative(asObject(texture->nativeTexture));
  m_owner->recordCommand([desc](MetalCommandBuffer& owner) {
    owner.m_computeEncoder.copyBufferToTexture(desc);
  });
}

void MetalComputeEncoder::copyTextureToBuffer(const BufferTextureCopyDesc& desc)
{
  requireTransfer("MetalComputeEncoder::copyTextureToBuffer");
  if(desc.aspect != TextureAspect::color)
    throw std::runtime_error("Metal texture-to-buffer depth/stencil copies are not supported");
  const MetalBufferRecord* buffer = m_owner->resources().tryGetBuffer(desc.buffer);
  const MetalTextureRecord* texture = m_owner->resources().tryGetTexture(desc.texture);
  if(buffer == nullptr || texture == nullptr)
    throw std::invalid_argument("Metal texture-to-buffer copy references a stale resource");
  if(desc.mipLevel >= texture->desc.mipLevels ||
     desc.baseArrayLayer + desc.layerCount > texture->desc.arrayLayers)
    throw std::out_of_range("Metal texture-to-buffer subresource is out of range");
  const uint32_t bytesPerRow = copyBytesPerRow(texture->desc.format, desc.width);
  const uint32_t blockRows = isBlockCompressed(texture->desc.format)
    ? std::max(1u, (desc.height + 3u) / 4u)
    : desc.height;
  const uint64_t bytesPerImage = static_cast<uint64_t>(bytesPerRow) * blockRows;
  const uint64_t bytesPerLayer = bytesPerImage * desc.depth;
  checkedEnd(desc.bufferOffset, bytesPerLayer * desc.layerCount,
             buffer->desc.size, "Metal texture-to-buffer copy");
  const MTL::Size copySize = metalSize(desc.width, desc.height, desc.depth);
  const MTL::Origin origin = metalOrigin(desc.textureOffset,
                                         "Metal texture-to-buffer copy");
  for(uint32_t layer = 0; layer < desc.layerCount; ++layer)
  {
    m_encoder->copyFromTexture(
      texture->nativeTexture, desc.baseArrayLayer + layer, desc.mipLevel,
      origin, copySize, buffer->nativeBuffer,
      desc.bufferOffset + bytesPerLayer * layer, bytesPerRow, bytesPerImage);
  }
  m_owner->trackTexture(desc.texture);
  m_owner->trackBuffer(desc.buffer);
  m_owner->retainNative(asObject(texture->nativeTexture));
  m_owner->retainNative(asObject(buffer->nativeBuffer));
  m_owner->recordCommand([desc](MetalCommandBuffer& owner) {
    owner.m_computeEncoder.copyTextureToBuffer(desc);
  });
}

void MetalComputeEncoder::blitTexture(const TextureBlitDesc& desc)
{
  requireTransfer("MetalComputeEncoder::blitTexture");
  if(desc.aspect != TextureAspect::color)
    throw std::runtime_error("Metal texture blit supports color resources only");
  const MetalTextureRecord* src = m_owner->resources().tryGetTexture(desc.srcTexture);
  const MetalTextureRecord* dst = m_owner->resources().tryGetTexture(desc.dstTexture);
  if(src == nullptr || dst == nullptr)
    throw std::invalid_argument("Metal texture blit references a stale texture");
  if(src->desc.dimension != TextureDimension::e2D ||
     dst->desc.dimension != TextureDimension::e2D)
    throw std::runtime_error("Metal compute blit currently requires 2D textures");
  if(src->desc.sampleCount != SampleCount::count1 ||
     dst->desc.sampleCount != SampleCount::count1)
    throw std::runtime_error("Metal compute blit does not support multisample textures");
  if(desc.srcMipLevel >= src->desc.mipLevels ||
     desc.dstMipLevel >= dst->desc.mipLevels)
    throw std::out_of_range("Metal texture blit mip level is out of range");
  if(desc.layerCount != 1 || desc.srcBaseArrayLayer != 0 ||
     desc.dstBaseArrayLayer != 0)
    throw std::runtime_error("Metal 2D texture blit requires exactly one base layer");
  if(!hasUsage(src->desc.usage, TextureUsageFlags::transferSrc) ||
     !hasUsage(dst->desc.usage, TextureUsageFlags::transferDst))
    throw std::runtime_error("Metal texture blit requires transferSrc and transferDst usage");
  if(isDepthFormat(src->desc.format) || isDepthFormat(dst->desc.format))
    throw std::runtime_error("Metal texture blit supports color formats only");

  const uint32_t srcWidth = validateBlitAxis(
    desc.srcOffsets[0].x, desc.srcOffsets[1].x,
    mipDimension(src->desc.extent.width, desc.srcMipLevel), "source x");
  const uint32_t srcHeight = validateBlitAxis(
    desc.srcOffsets[0].y, desc.srcOffsets[1].y,
    mipDimension(src->desc.extent.height, desc.srcMipLevel), "source y");
  const uint32_t dstWidth = validateBlitAxis(
    desc.dstOffsets[0].x, desc.dstOffsets[1].x,
    mipDimension(dst->desc.extent.width, desc.dstMipLevel), "destination x");
  const uint32_t dstHeight = validateBlitAxis(
    desc.dstOffsets[0].y, desc.dstOffsets[1].y,
    mipDimension(dst->desc.extent.height, desc.dstMipLevel), "destination y");
  const uint32_t srcDepth = validateBlitAxis(
    desc.srcOffsets[0].z, desc.srcOffsets[1].z, 1, "source z");
  const uint32_t dstDepth = validateBlitAxis(
    desc.dstOffsets[0].z, desc.dstOffsets[1].z, 1, "destination z");

  const bool forwardCopy =
    desc.srcOffsets[1].x > desc.srcOffsets[0].x &&
    desc.srcOffsets[1].y > desc.srcOffsets[0].y &&
    desc.srcOffsets[1].z > desc.srcOffsets[0].z &&
    desc.dstOffsets[1].x > desc.dstOffsets[0].x &&
    desc.dstOffsets[1].y > desc.dstOffsets[0].y &&
    desc.dstOffsets[1].z > desc.dstOffsets[0].z;
  const bool equalExtent = srcWidth == dstWidth && srcHeight == dstHeight &&
                           srcDepth == dstDepth;
  const bool sameSubresource = desc.srcTexture == desc.dstTexture &&
    desc.srcMipLevel == desc.dstMipLevel;
  if(sameSubresource)
  {
    const bool identicalRegion =
      desc.srcOffsets[0].x == desc.dstOffsets[0].x &&
      desc.srcOffsets[0].y == desc.dstOffsets[0].y &&
      desc.srcOffsets[0].z == desc.dstOffsets[0].z &&
      desc.srcOffsets[1].x == desc.dstOffsets[1].x &&
      desc.srcOffsets[1].y == desc.dstOffsets[1].y &&
      desc.srcOffsets[1].z == desc.dstOffsets[1].z;
    if(identicalRegion)
      return;
    throw std::runtime_error("Metal texture blit does not allow overlapping source and destination subresources");
  }

  if(forwardCopy && equalExtent && src->desc.format == dst->desc.format)
  {
    m_encoder->copyFromTexture(
      src->nativeTexture, 0, desc.srcMipLevel,
      metalOrigin(desc.srcOffsets[0], "Metal source blit"),
      metalSize(srcWidth, srcHeight, srcDepth), dst->nativeTexture, 0,
      desc.dstMipLevel, metalOrigin(desc.dstOffsets[0], "Metal destination blit"));
  }
  else
  {
    m_owner->requireQueueOperation(
      QueueOperation::compute, "MetalComputeEncoder::blitTexture scaling");
    if(isBlockCompressed(src->desc.format) || isBlockCompressed(dst->desc.format))
      throw std::runtime_error("Metal compute blit cannot scale or flip block-compressed textures");
    MTL::ComputePipelineState* blitPipeline = m_owner->allocator().blitPipeline();
    require(blitPipeline != nullptr, "Metal compute blit pipeline is unavailable");
    m_owner->trackNativeAllocation(blitPipeline);
    const MetalBlitParameters parameters{
      .sourceOffset0 = {desc.srcOffsets[0].x, desc.srcOffsets[0].y},
      .sourceOffset1 = {desc.srcOffsets[1].x, desc.srcOffsets[1].y},
      .destinationOffset0 = {desc.dstOffsets[0].x, desc.dstOffsets[0].y},
      .destinationOffset1 = {desc.dstOffsets[1].x, desc.dstOffsets[1].y},
      .sourceMipLevel = desc.srcMipLevel,
      .destinationMipLevel = desc.dstMipLevel,
    };
    const MetalRootDataAllocation parameterData =
      m_owner->allocator().allocateRootData(
        std::as_bytes(std::span{&parameters, 1}), alignof(MetalBlitParameters));
    MTL4::ArgumentTable* table = m_owner->allocator().allocateArgumentTable(
      1, 2, 0, "Metal4 compute blit arguments");
    table->setAddress(parameterData.gpuAddress, 0);
    table->setTexture(src->nativeTexture->gpuResourceID(), 0);
    table->setTexture(dst->nativeTexture->gpuResourceID(), 1);

    m_encoder->setComputePipelineState(blitPipeline);
    m_encoder->setArgumentTable(table);
    m_encoder->dispatchThreads(MTL::Size::Make(dstWidth, dstHeight, 1),
                               MTL::Size::Make(8, 8, 1));

    if(m_bindings.pipeline.isValid())
    {
      const MetalPipelineRecord& previousPipeline = requirePipeline();
      m_encoder->setComputePipelineState(previousPipeline.nativeComputePipeline);
      if(m_bindings.snapshot != nullptr)
        m_encoder->setArgumentTable(m_bindings.snapshot);
    }
    m_owner->retainNative(asObject(blitPipeline));
  }
  m_owner->trackTexture(desc.srcTexture);
  m_owner->trackTexture(desc.dstTexture);
  m_owner->retainNative(asObject(src->nativeTexture));
  m_owner->retainNative(asObject(dst->nativeTexture));
  m_owner->recordCommand([desc](MetalCommandBuffer& owner) {
    owner.m_computeEncoder.blitTexture(desc);
  });
}

void MetalComputeEncoder::fillBuffer(
  BufferHandle handle, uint64_t offset, uint64_t size, uint32_t data)
{
  requireTransfer("MetalComputeEncoder::fillBuffer");
  const MetalBufferRecord* buffer = m_owner->resources().tryGetBuffer(handle);
  if(buffer == nullptr)
    throw std::invalid_argument("Metal fill buffer is stale");
  if(size == 0)
    size = buffer->desc.size - offset;
  checkedEnd(offset, size, buffer->desc.size, "Metal fill buffer");
  if((offset & 3u) != 0 || (size & 3u) != 0)
    throw std::invalid_argument("Metal 32-bit fill requires four-byte aligned offset and size");

  const std::array<uint8_t, 4> bytes{
    static_cast<uint8_t>(data), static_cast<uint8_t>(data >> 8u),
    static_cast<uint8_t>(data >> 16u), static_cast<uint8_t>(data >> 24u)};
  if(bytes[0] == bytes[1] && bytes[0] == bytes[2] && bytes[0] == bytes[3])
  {
    m_encoder->fillBuffer(buffer->nativeBuffer, NS::Range::Make(offset, size), bytes[0]);
  }
  else
  {
    constexpr uint64_t chunkCapacity = 64u * 1024u;
    const uint64_t patternSize = std::min(size, chunkCapacity);
    std::vector<std::byte> pattern(static_cast<size_t>(patternSize));
    for(uint64_t byteOffset = 0; byteOffset < patternSize; byteOffset += sizeof(data))
      std::memcpy(pattern.data() + byteOffset, &data, sizeof(data));
    const MetalRootDataAllocation source =
      m_owner->allocator().allocateRootData(pattern, alignof(uint32_t));
    for(uint64_t filled = 0; filled < size; filled += patternSize)
    {
      const uint64_t copySize = std::min(patternSize, size - filled);
      m_encoder->copyFromBuffer(source.buffer, source.offset,
                                buffer->nativeBuffer, offset + filled, copySize);
    }
  }
  m_owner->trackBuffer(handle);
  m_owner->retainNative(asObject(buffer->nativeBuffer));
  m_owner->recordCommand([handle, offset, size, data](MetalCommandBuffer& owner) {
    owner.m_computeEncoder.fillBuffer(handle, offset, size, data);
  });
}

MetalCommandBuffer::MetalCommandBuffer(
  MetalCommandAllocator& allocator, MTL4::CommandBuffer* commandBuffer,
  MetalResourceTable* resourceTable)
  : m_commandBuffer(commandBuffer), m_resourceTable(resourceTable),
    m_allocator(&allocator)
{
  require(m_commandBuffer != nullptr && m_resourceTable != nullptr,
          "MetalCommandBuffer requires native storage and a resource table");
  auto* descriptor = MTL::ResidencySetDescriptor::alloc()->init();
  descriptor->setInitialCapacity(256);
  descriptor->setLabel(nsString("Metal4 command buffer automatic residency"));
  NS::Error* error = nullptr;
  m_automaticResidencySet = allocator.device()->newResidencySet(descriptor, &error);
  descriptor->release();
  if(m_automaticResidencySet == nullptr)
  {
    m_commandBuffer->release();
    m_commandBuffer = nullptr;
    throw std::runtime_error(errorMessage("Metal automatic residency set", error));
  }
  m_automaticResidencySet->commit();
}

MetalCommandBuffer::~MetalCommandBuffer()
{
  if(m_state == CommandBufferState::submitted && m_submission.isValid())
  {
    try
    {
      m_allocator->queue().wait(m_submission);
    }
    catch(...)
    {
    }
  }
  releaseRetainedResources();
  if(m_allocator != nullptr)
    m_allocator->releaseCommandBuffer(*this);
  if(m_commandBuffer != nullptr)
    m_commandBuffer->release();
  if(m_automaticResidencySet != nullptr)
  {
    m_automaticResidencySet->endResidency();
    m_automaticResidencySet->release();
  }
  m_commandBuffer = nullptr;
  m_automaticResidencySet = nullptr;
}

void MetalCommandBuffer::begin(CommandAllocator& allocatorValue)
{
  auto* metalAllocator = dynamic_cast<MetalCommandAllocator*>(&allocatorValue);
  if(metalAllocator == nullptr || metalAllocator != m_allocator)
    throw std::invalid_argument("MetalCommandBuffer::begin rejected a foreign allocator");
  if(m_state != CommandBufferState::idle && m_state != CommandBufferState::reusable)
    throw std::runtime_error("MetalCommandBuffer::begin requires idle or reusable state");
  m_allocator->acquireRecording(*this);
  try
  {
    if(!m_replaying)
      m_replayCommands.clear();
    releaseRetainedResources();
    clearTracking();
    m_commandBuffer->beginCommandBuffer(m_allocator->nativeAllocator());
    m_commandBuffer->useResidencySet(m_automaticResidencySet);
    m_active = EncoderKind::none;
    m_submission = {};
    m_pendingProducer = StageFlags::none;
    m_pendingConsumer = StageFlags::none;
    m_pendingHazards = HazardFlags::none;
    m_pendingResourceAlias = false;
    m_debugGroupDepth = 0;
    m_resourceStates.reset();
    m_state = CommandBufferState::recording;
    if(!m_replaying)
      incrementHotPathCounter(BackendType::metal, HotPathCounter::commandBufferBegins);
  }
  catch(...)
  {
    m_allocator->releaseRecording(*this);
    throw;
  }
}

void MetalCommandBuffer::end()
{
  requireRecording("MetalCommandBuffer::end");
  if(m_active != EncoderKind::none)
    throw std::runtime_error("MetalCommandBuffer::end requires no active encoder");
  if(m_debugGroupDepth != 0)
    throw std::runtime_error("MetalCommandBuffer::end found an unbalanced debug group");
  try
  {
    m_commandBuffer->endCommandBuffer();
    m_state = CommandBufferState::executable;
    m_allocator->releaseRecording(*this);
  }
  catch(...)
  {
    m_allocator->releaseRecording(*this);
    m_state = CommandBufferState::reusable;
    throw;
  }
}

CommandBufferState MetalCommandBuffer::state() const noexcept
{
  return m_state;
}

QueueClass MetalCommandBuffer::queueClass() const noexcept
{
  return m_allocator != nullptr ? m_allocator->queueClass() : QueueClass::graphics;
}

void MetalCommandBuffer::requireQueueOperation(
  QueueOperation operation, const char* command) const
{
  if(!supportsQueueOperation(queueClass(), operation))
    throw std::runtime_error(std::string(command) + " is unsupported by this queue class");
}

MetalResourceTable& MetalCommandBuffer::resources() const
{
  if(m_resourceTable == nullptr)
    throw std::runtime_error("Metal command buffer has no resource table");
  return *m_resourceTable;
}

MetalCommandAllocator& MetalCommandBuffer::allocator() const
{
  if(m_allocator == nullptr)
    throw std::runtime_error("Metal command buffer has no allocator");
  return *m_allocator;
}

RenderEncoder* MetalCommandBuffer::beginRenderPass(const RenderPassDesc& desc)
{
  requireRecording("MetalCommandBuffer::beginRenderPass");
  requireQueueOperation(QueueOperation::render, "MetalCommandBuffer::beginRenderPass");
  if(m_active != EncoderKind::none)
    throw std::runtime_error("MetalCommandBuffer already has an active encoder");
  if(desc.colorTargets.empty() && desc.depthTarget == nullptr)
    throw std::invalid_argument("Metal render pass requires an attachment");
  if(desc.renderArea.offset.x != 0 || desc.renderArea.offset.y != 0)
  {
    throw std::invalid_argument(
      "Metal render pass does not support a non-zero render-area offset");
  }
  if(desc.renderArea.extent.width == 0 || desc.renderArea.extent.height == 0)
    throw std::invalid_argument("Metal render pass requires a non-empty render area");

  auto* nativeDesc = MTL4::RenderPassDescriptor::alloc()->init();
  nativeDesc->setRenderTargetWidth(desc.renderArea.extent.width);
  nativeDesc->setRenderTargetHeight(desc.renderArea.extent.height);
  for(uint32_t index = 0; index < desc.colorTargets.size(); ++index)
  {
    const RenderTargetDesc& target = desc.colorTargets[index];
    MTL::Texture* nativeTexture = nullptr;
    const MetalTextureRecord* textureRecord = nullptr;
    if(target.view.isValid())
    {
      const MetalTextureViewRecord* view = resources().tryGetTextureView(target.view);
      if(view == nullptr)
      {
        nativeDesc->release();
        throw std::invalid_argument("Metal render pass color view is stale");
      }
      nativeTexture = view->nativeTextureView;
      textureRecord = resources().tryGetTexture(view->parentTexture);
      if(nativeTexture == nullptr)
      {
        nativeTexture = textureRecord != nullptr
          ? textureRecord->nativeTexture
          : nullptr;
      }
      trackTextureView(target.view);
      if(view->parentTexture.isValid())
        trackTexture(view->parentTexture);
    }
    else
    {
      textureRecord = resources().tryGetTexture(target.texture);
      nativeTexture = textureRecord != nullptr
        ? textureRecord->nativeTexture
        : nullptr;
      trackTexture(target.texture);
    }
    if(nativeTexture == nullptr)
    {
      nativeDesc->release();
      throw std::invalid_argument("Metal render pass color attachment is stale");
    }
    if(textureRecord == nullptr ||
       !hasUsage(textureRecord->desc.usage, TextureUsageFlags::colorAttachment))
    {
      const std::string name = textureRecord != nullptr &&
                               !textureRecord->debugName.empty()
        ? textureRecord->debugName
        : "<unnamed>";
      nativeDesc->release();
      throw std::invalid_argument(
        "Metal render pass color attachment '" + name +
        "' was not created with TextureUsageFlags::colorAttachment");
    }
    if(textureRecord->desc.memoryUsage == MemoryUsage::transientAttachment &&
       (target.loadOp == LoadOp::load || target.storeOp == StoreOp::store))
    {
      nativeDesc->release();
      throw std::invalid_argument(
        "Metal memoryless color attachments require clear/dontCare load and dontCare store");
    }
    retainNative(asObject(nativeTexture));
    MTL::RenderPassColorAttachmentDescriptor* attachment =
      nativeDesc->colorAttachments()->object(index);
    attachment->setTexture(nativeTexture);
    attachment->setLoadAction(toMetalLoadAction(target.loadOp));
    attachment->setStoreAction(toMetalStoreAction(target.storeOp));
    attachment->setClearColor(MTL::ClearColor::Make(
      target.clearColor.r, target.clearColor.g, target.clearColor.b,
      target.clearColor.a));
  }

  if(desc.depthTarget != nullptr)
  {
    const DepthTargetDesc& target = *desc.depthTarget;
    MTL::Texture* nativeTexture = nullptr;
    const MetalTextureRecord* textureRecord = nullptr;
    TextureFormat format = TextureFormat::undefined;
    if(target.view.isValid())
    {
      const MetalTextureViewRecord* view = resources().tryGetTextureView(target.view);
      if(view != nullptr)
      {
        nativeTexture = view->nativeTextureView;
        format = view->desc.format;
        textureRecord = resources().tryGetTexture(view->parentTexture);
        if(nativeTexture == nullptr)
        {
          nativeTexture = textureRecord != nullptr
            ? textureRecord->nativeTexture
            : nullptr;
          if(format == TextureFormat::undefined && textureRecord != nullptr)
            format = textureRecord->desc.format;
        }
        trackTextureView(target.view);
        if(view->parentTexture.isValid())
          trackTexture(view->parentTexture);
      }
    }
    else
    {
      textureRecord = resources().tryGetTexture(target.texture);
      if(textureRecord != nullptr)
      {
        nativeTexture = textureRecord->nativeTexture;
        format = textureRecord->desc.format;
      }
      trackTexture(target.texture);
    }
    if(nativeTexture == nullptr)
    {
      nativeDesc->release();
      throw std::invalid_argument("Metal render pass depth attachment is stale");
    }
    if(textureRecord == nullptr ||
       !hasUsage(textureRecord->desc.usage, TextureUsageFlags::depthAttachment))
    {
      const std::string name = textureRecord != nullptr &&
                               !textureRecord->debugName.empty()
        ? textureRecord->debugName
        : "<unnamed>";
      nativeDesc->release();
      throw std::invalid_argument(
        "Metal render pass depth attachment '" + name +
        "' was not created with TextureUsageFlags::depthAttachment");
    }
    if(textureRecord->desc.memoryUsage == MemoryUsage::transientAttachment &&
       (target.loadOp == LoadOp::load || target.storeOp == StoreOp::store))
    {
      nativeDesc->release();
      throw std::invalid_argument(
        "Metal memoryless depth attachments require clear/dontCare load and dontCare store");
    }
    retainNative(asObject(nativeTexture));
    MTL::RenderPassDepthAttachmentDescriptor* depth = nativeDesc->depthAttachment();
    depth->setTexture(nativeTexture);
    depth->setLoadAction(toMetalLoadAction(target.loadOp));
    depth->setStoreAction(toMetalStoreAction(target.storeOp));
    depth->setClearDepth(target.clearValue.depth);
    if(hasStencil(format))
    {
      MTL::RenderPassStencilAttachmentDescriptor* stencil = nativeDesc->stencilAttachment();
      stencil->setTexture(nativeTexture);
      stencil->setLoadAction(toMetalLoadAction(target.loadOp));
      stencil->setStoreAction(toMetalStoreAction(target.storeOp));
      stencil->setClearStencil(target.clearValue.stencil);
    }
  }

  m_nativeRenderEncoder = m_commandBuffer->renderCommandEncoder(nativeDesc);
  nativeDesc->release();
  if(m_nativeRenderEncoder == nullptr)
    throw std::runtime_error("Metal failed to create MTL4::RenderCommandEncoder");
  applyPendingBarrier(*m_nativeRenderEncoder);
  m_renderEncoder.prepare(m_nativeRenderEncoder, *this);
  m_active = EncoderKind::render;
  const Rect2D replayArea = desc.renderArea;
  std::vector<RenderTargetDesc> replayColors(desc.colorTargets.begin(),
                                              desc.colorTargets.end());
  const bool replayHasDepth = desc.depthTarget != nullptr;
  const DepthTargetDesc replayDepth = replayHasDepth
    ? *desc.depthTarget
    : DepthTargetDesc{};
  std::vector<InputAttachmentDesc> replayInputs(desc.inputAttachments.begin(),
                                                 desc.inputAttachments.end());
  const bool replayLocalRead = desc.enableLocalRead;
  recordCommand(
    [replayArea, replayColors = std::move(replayColors), replayHasDepth,
     replayDepth, replayInputs = std::move(replayInputs), replayLocalRead](
      MetalCommandBuffer& owner) {
      owner.beginRenderPass(RenderPassDesc{
        .renderArea = replayArea,
        .colorTargets = std::span<const RenderTargetDesc>{replayColors},
        .depthTarget = replayHasDepth ? &replayDepth : nullptr,
        .inputAttachments = std::span<const InputAttachmentDesc>{replayInputs},
        .enableLocalRead = replayLocalRead,
      });
    });
  if(!m_replaying)
    incrementHotPathCounter(BackendType::metal, HotPathCounter::encoderBegins);
  return &m_renderEncoder;
}

ComputeEncoder* MetalCommandBuffer::beginComputePass()
{
  requireRecording("MetalCommandBuffer::beginComputePass");
  if(m_active != EncoderKind::none)
    throw std::runtime_error("MetalCommandBuffer already has an active encoder");
  m_nativeComputeEncoder = m_commandBuffer->computeCommandEncoder();
  if(m_nativeComputeEncoder == nullptr)
    throw std::runtime_error("Metal failed to create MTL4::ComputeCommandEncoder");
  applyPendingBarrier(*m_nativeComputeEncoder);
  m_computeEncoder.prepare(m_nativeComputeEncoder, *this);
  m_active = EncoderKind::compute;
  recordCommand([](MetalCommandBuffer& owner) { owner.beginComputePass(); });
  if(!m_replaying)
    incrementHotPathCounter(BackendType::metal, HotPathCounter::encoderBegins);
  return &m_computeEncoder;
}

void MetalCommandBuffer::endEncoding()
{
  requireRecording("MetalCommandBuffer::endEncoding");
  MTL4::CommandEncoder* encoder = activeEncoder();
  if(encoder == nullptr)
    throw std::runtime_error("MetalCommandBuffer::endEncoding has no active encoder");
  encoder->endEncoding();
  if(m_active == EncoderKind::render)
    m_renderEncoder.invalidate();
  else
    m_computeEncoder.invalidate();
  m_nativeRenderEncoder = nullptr;
  m_nativeComputeEncoder = nullptr;
  m_active = EncoderKind::none;
  recordCommand([](MetalCommandBuffer& owner) { owner.endEncoding(); });
}

void MetalCommandBuffer::prepareIndexedIndirect(
  const PrepareIndexedIndirectDesc& desc)
{
  const PrepareIndexedIndirectCountDesc normalized{
    .draw = {
      .argsBuffer = desc.draw.argsBuffer,
      .argsOffset = desc.draw.offset,
      .countBuffer = {},
      .countBufferOffset = 0,
      .maxDrawCount = desc.draw.drawCount,
      .stride = desc.draw.stride,
    },
    .indexBuffer = desc.indexBuffer,
    .indexBufferOffset = desc.indexBufferOffset,
    .indexFormat = desc.indexFormat,
    .topology = desc.topology,
  };
  prepareIndexedIndirectImpl(normalized);
  if(desc.draw.drawCount == 0)
    return;
  recordCommand([desc](MetalCommandBuffer& owner) {
    owner.prepareIndexedIndirect(desc);
  });
}

void MetalCommandBuffer::prepareIndexedIndirectCount(
  const PrepareIndexedIndirectCountDesc& desc)
{
  if(desc.draw.maxDrawCount != 0 && !desc.draw.countBuffer.isValid())
  {
    throw std::invalid_argument(
      "Metal indexed-indirect-count preparation requires a count buffer");
  }
  prepareIndexedIndirectImpl(desc);
  if(desc.draw.maxDrawCount == 0)
    return;
  recordCommand([desc](MetalCommandBuffer& owner) {
    owner.prepareIndexedIndirectCount(desc);
  });
}

void MetalCommandBuffer::prepareIndexedIndirectImpl(
  const PrepareIndexedIndirectCountDesc& desc)
{
  requireRecording("MetalCommandBuffer::prepareIndexedIndirect");
  requireQueueOperation(
    QueueOperation::render,
    "MetalCommandBuffer::prepareIndexedIndirect");
  if(m_active != EncoderKind::none)
  {
    throw std::runtime_error(
      "Metal indexed-indirect preparation requires no active encoder");
  }
  if(desc.draw.maxDrawCount == 0)
    return;

  const bool usesCountBuffer = desc.draw.countBuffer.isValid();
  const MetalBufferRecord* arguments =
    resources().tryGetBuffer(desc.draw.argsBuffer);
  const MetalBufferRecord* count = usesCountBuffer
    ? resources().tryGetBuffer(desc.draw.countBuffer)
    : nullptr;
  const MetalBufferRecord* index = resources().tryGetBuffer(desc.indexBuffer);
  if(arguments == nullptr || arguments->nativeBuffer == nullptr)
  {
    throw std::invalid_argument(
      "Metal indexed-indirect preparation has a stale argument buffer");
  }
  if(usesCountBuffer && (count == nullptr || count->nativeBuffer == nullptr))
  {
    throw std::invalid_argument(
      "Metal indexed-indirect-count preparation has a stale count buffer");
  }
  if(index == nullptr || index->nativeBuffer == nullptr)
  {
    throw std::invalid_argument(
      "Metal indexed-indirect preparation has a stale index buffer");
  }

  const uint32_t stride = defaultIndirectStride(
    desc.draw.stride, kIndexedIndirectCommandSize);
  if(stride < kIndexedIndirectCommandSize || (stride % sizeof(uint32_t)) != 0)
  {
    throw std::invalid_argument(
      "Metal indexed-indirect stride must be a 4-byte-aligned value of at least 20 bytes");
  }
  if((desc.draw.argsOffset % alignof(uint32_t)) != 0 ||
     (usesCountBuffer &&
      (desc.draw.countBufferOffset % alignof(uint32_t)) != 0))
  {
    throw std::invalid_argument(
      "Metal indexed-indirect argument and count offsets require 4-byte alignment");
  }
  const uint32_t indexSize = indexElementSize(desc.indexFormat);
  if((desc.indexBufferOffset % indexSize) != 0)
  {
    throw std::invalid_argument(
      "Metal indexed-indirect index offset is misaligned");
  }
  const uint64_t argumentBytes =
    static_cast<uint64_t>(desc.draw.maxDrawCount - 1u) * stride +
    kIndexedIndirectCommandSize;
  checkedEnd(desc.draw.argsOffset, argumentBytes, arguments->desc.size,
             "Metal indexed-indirect arguments");
  if(usesCountBuffer)
  {
    checkedEnd(desc.draw.countBufferOffset, sizeof(uint32_t), count->desc.size,
               "Metal indexed-indirect count");
  }
  checkedEnd(desc.indexBufferOffset, 0, index->desc.size,
             "Metal indexed-indirect index buffer");
  const uint64_t clampedArgumentBytes =
    static_cast<uint64_t>(desc.draw.maxDrawCount) * stride;
  if(clampedArgumentBytes > std::numeric_limits<size_t>::max())
  {
    throw std::invalid_argument(
      "Metal indexed-indirect fallback allocation is too large");
  }

  auto descriptor = NS::TransferPtr(
    MTL::IndirectCommandBufferDescriptor::alloc()->init());
  descriptor->setCommandTypes(MTL::IndirectCommandTypeDrawIndexed);
  descriptor->setInheritPipelineState(true);
  descriptor->setInheritBuffers(true);
  descriptor->setInheritCullMode(true);
  descriptor->setInheritDepthBias(true);
  descriptor->setInheritDepthClipMode(true);
  descriptor->setInheritDepthStencilState(true);
  descriptor->setInheritFrontFacingWinding(true);
  descriptor->setInheritTriangleFillMode(true);

  auto commandBuffer = NS::TransferPtr(allocator().device()->newIndirectCommandBuffer(
    descriptor.get(), desc.draw.maxDrawCount,
    static_cast<MTL::ResourceOptions>(
      MTL::ResourceStorageModePrivate |
      MTL::ResourceHazardTrackingModeUntracked)));
  if(!commandBuffer)
    throw std::runtime_error("Metal failed to create an indexed indirect command buffer");
  commandBuffer->setLabel(nsString("Metal4 Slang indexed-indirect command buffer"));

  MTL::ArgumentEncoder* argumentEncoder =
    allocator().indexedIndirectArgumentEncoder();
  require(argumentEncoder != nullptr,
          "Metal indexed-indirect argument encoder is unavailable");
  const NS::UInteger encodedLength = argumentEncoder->encodedLength();
  const NS::UInteger encodedAlignment = argumentEncoder->alignment();
  if(encodedLength == 0 || encodedAlignment > std::numeric_limits<uint32_t>::max())
  {
    throw std::runtime_error(
      "Metal indexed-indirect argument-buffer layout is invalid");
  }
  std::vector<std::byte> argumentBytesStorage(encodedLength);
  const MetalRootDataAllocation commandArguments = allocator().allocateRootData(
    argumentBytesStorage, static_cast<uint32_t>(encodedAlignment));
  argumentEncoder->setArgumentBuffer(
    commandArguments.buffer, commandArguments.offset);
  argumentEncoder->setIndirectCommandBuffer(commandBuffer.get(), 0);

  const MTL::IndirectCommandBufferExecutionRange initialExecutionRange{0u, 0u};
  const MetalRootDataAllocation executionRange = allocator().allocateRootData(
    std::as_bytes(std::span{&initialExecutionRange, 1}), alignof(uint32_t));
  const MetalIndexedIndirectPreparationParameters parameters{
    .maxDrawCount = desc.draw.maxDrawCount,
    .argumentStride = stride,
    .primitiveType = toMetalShaderPrimitive(desc.topology),
    .reserved = 0u,
  };
  const MetalRootDataAllocation preparationConstants = allocator().allocateRootData(
    std::as_bytes(std::span{&parameters, 1}), alignof(decltype(parameters)));
  const std::vector<std::byte> clampedArgumentStorage(
    static_cast<size_t>(clampedArgumentBytes));
  const MetalRootDataAllocation clampedArguments = allocator().allocateRootData(
    clampedArgumentStorage, alignof(uint32_t));
  uint64_t drawCountAddress = 0;
  if(usesCountBuffer)
  {
    drawCountAddress = count->nativeBuffer->gpuAddress() +
                       desc.draw.countBufferOffset;
  }
  else
  {
    const uint32_t fixedDrawCount = desc.draw.maxDrawCount;
    drawCountAddress = allocator().allocateRootData(
      std::as_bytes(std::span{&fixedDrawCount, 1}),
      alignof(decltype(fixedDrawCount))).gpuAddress;
  }

  MTL4::ArgumentTable* table = allocator().allocateArgumentTable(
    7, 0, 0, "Metal4 Slang indexed-indirect builder arguments");
  table->setAddress(commandArguments.gpuAddress, 0);
  table->setAddress(arguments->nativeBuffer->gpuAddress() + desc.draw.argsOffset, 1);
  table->setAddress(drawCountAddress, 2);
  table->setAddress(executionRange.gpuAddress, 3);
  table->setAddress(index->nativeBuffer->gpuAddress() + desc.indexBufferOffset, 4);
  table->setAddress(preparationConstants.gpuAddress, 5);
  table->setAddress(clampedArguments.gpuAddress, 6);

  MTL::ComputePipelineState* pipeline =
    allocator().indexedIndirectPipeline(desc.indexFormat);
  require(pipeline != nullptr,
          "Metal indexed-indirect builder pipeline is unavailable");

  // A caller's indirect-input barrier targets the eventual draw. The Slang
  // builder becomes the first consumer, so include its dispatch stage too.
  if(any(m_pendingHazards))
    m_pendingConsumer |= StageFlags::compute;
  MTL4::ComputeCommandEncoder* encoder = m_commandBuffer->computeCommandEncoder();
  if(encoder == nullptr)
    throw std::runtime_error("Metal failed to create the indexed-indirect builder encoder");
  applyPendingBarrier(*encoder);
  encoder->resetCommandsInBuffer(
    commandBuffer.get(), NS::Range::Make(0, desc.draw.maxDrawCount));
  encoder->barrierAfterEncoderStages(
    MTL::StageBlit, MTL::StageDispatch, MTL4::VisibilityOptionDevice);
  encoder->setComputePipelineState(pipeline);
  encoder->setArgumentTable(table);
  encoder->dispatchThreads(
    MTL::Size::Make(desc.draw.maxDrawCount, 1, 1),
    MTL::Size::Make(kIndexedIndirectThreadgroupSize, 1, 1));
  encoder->barrierAfterStages(
    MTL::StageDispatch, MTL::StageVertex, MTL4::VisibilityOptionDevice);
  encoder->endEncoding();

  trackBuffer(desc.draw.argsBuffer);
  if(usesCountBuffer)
    trackBuffer(desc.draw.countBuffer);
  trackBuffer(desc.indexBuffer);
  retainNative(asObject(arguments->nativeBuffer));
  if(usesCountBuffer)
    retainNative(asObject(count->nativeBuffer));
  retainNative(asObject(index->nativeBuffer));
  retainNative(asObject(commandBuffer.get()));
  trackNativeAllocation(commandBuffer.get());
  trackNativeAllocation(pipeline);
  m_preparedIndexedIndirect.push_back(MetalPreparedIndexedIndirect{
    .draw = desc.draw,
    .indexBuffer = desc.indexBuffer,
    .indexBufferOffset = desc.indexBufferOffset,
    .indexFormat = desc.indexFormat,
    .topology = desc.topology,
    .commandBuffer = commandBuffer.get(),
    .executionRangeBuffer = executionRange.buffer,
    .executionRangeOffset = executionRange.offset,
    .clampedArgumentsBuffer = clampedArguments.buffer,
    .clampedArgumentsOffset = clampedArguments.offset,
    .argumentStride = stride,
  });
}

RHIResult MetalCommandBuffer::useResidencySet(ResidencySetHandle set)
{
  if(m_state != CommandBufferState::recording)
    return RHIResult::fail(RHIErrorCode::invalidState,
                           "Metal residency-set use requires recording state");
  MetalResidencySetRecord* record = resources().tryGetResidencySet(set);
  if(record == nullptr || record->nativeSet == nullptr)
    return RHIResult::fail(RHIErrorCode::invalidHandle,
                           "Metal residency-set handle is stale");
  for(const ResidencyResource resource : record->resources)
  {
    if(!resources().isAlive(resource))
      return RHIResult::fail(RHIErrorCode::invalidHandle,
                             "Metal residency set contains a stale resource");
  }
  m_commandBuffer->useResidencySet(record->nativeSet);
  trackResidencySet(set);
  retainNative(asObject(record->nativeSet));
  recordCommand([set](MetalCommandBuffer& owner) {
    const RHIResult result = owner.useResidencySet(set);
    if(!result)
      throw std::runtime_error(result.error.message != nullptr
                                 ? result.error.message
                                 : "Metal residency-set replay failed");
  });
  return RHIResult::ok();
}

MTL4::CommandEncoder* MetalCommandBuffer::activeEncoder() const noexcept
{
  if(m_active == EncoderKind::render)
    return reinterpret_cast<MTL4::CommandEncoder*>(m_nativeRenderEncoder);
  if(m_active == EncoderKind::compute)
    return reinterpret_cast<MTL4::CommandEncoder*>(m_nativeComputeEncoder);
  return nullptr;
}

void MetalCommandBuffer::barrier(
  StageFlags producer, StageFlags consumer, HazardFlags hazards)
{
  requireRecording("MetalCommandBuffer::barrier");
  if(!any(hazards))
    return;
  encodeBarrier(producer, consumer, hazards);
  recordCommand([producer, consumer, hazards](MetalCommandBuffer& owner) {
    owner.barrier(producer, consumer, hazards);
  });
}

void MetalCommandBuffer::encodeBarrier(
  StageFlags producer, StageFlags consumer, HazardFlags hazards)
{
  if(MTL4::CommandEncoder* encoder = activeEncoder())
  {
    encoder->barrierAfterEncoderStages(toMetalStages(producer),
                                       toMetalStages(consumer),
                                       toMetalVisibility(hazards));
    return;
  }
  m_pendingProducer |= producer;
  m_pendingConsumer |= consumer;
  m_pendingHazards |= hazards;
}

void MetalCommandBuffer::applyPendingBarrier(MTL4::CommandEncoder& encoder)
{
  if(!any(m_pendingHazards))
    return;
  MTL4::VisibilityOptions visibility = toMetalVisibility(m_pendingHazards);
  if(m_pendingResourceAlias)
  {
    visibility = static_cast<MTL4::VisibilityOptions>(
      visibility | MTL4::VisibilityOptionResourceAlias);
  }
  encoder.barrierAfterQueueStages(toMetalStages(m_pendingProducer),
                                  toMetalStages(m_pendingConsumer),
                                  visibility);
  m_pendingProducer = StageFlags::none;
  m_pendingConsumer = StageFlags::none;
  m_pendingHazards = HazardFlags::none;
  m_pendingResourceAlias = false;
}

void MetalCommandBuffer::resourceBarrier(
  std::span<const TextureBarrier> textures,
  std::span<const BufferBarrier> buffers,
  std::span<const AliasingBarrier> aliasing)
{
  requireRecording("MetalCommandBuffer::resourceBarrier");
  StageFlags producer = StageFlags::none;
  StageFlags consumer = StageFlags::none;
  HazardFlags hazards = HazardFlags::none;

  for(const TextureBarrier& value : textures)
  {
    const MetalTextureRecord* texture = resources().tryGetTexture(value.texture);
    if(texture == nullptr)
      throw std::invalid_argument("Metal resource barrier references a stale texture");
    TextureBarrier resolved = value;
    resolved.range = resolveTextureRange(
      value.range, texture->desc, "Metal texture barrier");
#ifndef NDEBUG
    if(!m_resourceStates.transition(resolved).valid())
      throw std::runtime_error("Metal command buffer rejected an invalid texture transition");
#endif
    trackTexture(value.texture);
    retainNative(asObject(texture->nativeTexture));
    producer |= stageForState(value.before);
    consumer |= stageForState(value.after);
    hazards |= hazardForState(value.before);
  }

  for(const BufferBarrier& value : buffers)
  {
    const MetalBufferRecord* buffer = resources().tryGetBuffer(value.buffer);
    if(buffer == nullptr)
      throw std::invalid_argument("Metal resource barrier references a stale buffer");
    const uint64_t size = value.size == 0 ? buffer->desc.size - value.offset : value.size;
    checkedEnd(value.offset, size, buffer->desc.size, "Metal buffer barrier");
#ifndef NDEBUG
    if(!m_resourceStates.transition(value).valid())
      throw std::runtime_error("Metal command buffer rejected an invalid buffer transition");
#endif
    trackBuffer(value.buffer);
    retainNative(asObject(buffer->nativeBuffer));
    producer |= stageForState(value.before);
    consumer |= stageForState(value.after);
    hazards |= hazardForState(value.before);
  }

  for(const AliasingBarrier& value : aliasing)
  {
    if(!value.before.isValid() || !value.after.isValid())
      throw std::invalid_argument("Metal aliasing barrier has an invalid resource shape");
    const auto trackAliased = [this](const AliasingResource& resource) {
      if(resource.kind == AliasingResourceKind::buffer)
      {
        if(resources().tryGetBuffer(resource.buffer) == nullptr)
          throw std::invalid_argument("Metal aliasing barrier buffer is stale");
        trackBuffer(resource.buffer);
      }
      else if(resource.kind == AliasingResourceKind::texture)
      {
        if(resources().tryGetTexture(resource.texture) == nullptr)
          throw std::invalid_argument("Metal aliasing barrier texture is stale");
        trackTexture(resource.texture);
      }
    };
    trackAliased(value.before);
    trackAliased(value.after);
    producer |= StageFlags::all;
    consumer |= StageFlags::all;
    hazards |= HazardFlags::readBeforeWrite;
  }

  if(!aliasing.empty())
  {
    if(MTL4::CommandEncoder* encoder = activeEncoder())
    {
      encoder->barrierAfterEncoderStages(
        toMetalStages(producer), toMetalStages(consumer),
        static_cast<MTL4::VisibilityOptions>(
          toMetalVisibility(hazards) | MTL4::VisibilityOptionResourceAlias));
    }
    else
    {
      m_pendingProducer |= producer;
      m_pendingConsumer |= consumer;
      m_pendingHazards |= hazards | HazardFlags::readBeforeWrite;
      m_pendingResourceAlias = true;
    }
  }
  else if(!textures.empty() || !buffers.empty())
  {
    encodeBarrier(producer, consumer, hazards);
  }

  if(!textures.empty() || !buffers.empty() || !aliasing.empty())
  {
    std::vector<TextureBarrier> replayTextures(textures.begin(), textures.end());
    std::vector<BufferBarrier> replayBuffers(buffers.begin(), buffers.end());
    std::vector<AliasingBarrier> replayAliasing(aliasing.begin(), aliasing.end());
    recordCommand(
      [replayTextures = std::move(replayTextures),
       replayBuffers = std::move(replayBuffers),
       replayAliasing = std::move(replayAliasing)](MetalCommandBuffer& owner) {
        owner.resourceBarrier(replayTextures, replayBuffers, replayAliasing);
      });
  }
}

void MetalCommandBuffer::clearColorTexture(
  TextureHandle handle, const TextureSubresourceRange& range,
  const ClearColorValue& clearColor)
{
  requireRecording("MetalCommandBuffer::clearColorTexture");
  requireQueueOperation(QueueOperation::render,
                        "MetalCommandBuffer::clearColorTexture");
  if(m_active != EncoderKind::none)
    throw std::runtime_error("Metal texture clear requires no active encoder");
  if(range.aspect != TextureAspect::color)
    throw std::invalid_argument("Metal clearColorTexture requires the color aspect");
  const MetalTextureRecord* texture = resources().tryGetTexture(handle);
  if(texture == nullptr || texture->nativeTexture == nullptr)
    throw std::invalid_argument("Metal texture clear references a stale texture");
  if(texture->desc.memoryUsage == MemoryUsage::transientAttachment ||
     !hasUsage(texture->desc.usage, TextureUsageFlags::transferDst))
  {
    throw std::invalid_argument(
      "Metal texture clear requires a non-memoryless transfer destination");
  }
  const TextureSubresourceRange resolvedRange = resolveTextureRange(
    range, texture->desc, "Metal clear texture");
  trackTexture(handle);
  retainNative(asObject(texture->nativeTexture));

  for(uint32_t mipOffset = 0; mipOffset < resolvedRange.levelCount; ++mipOffset)
  {
    const uint32_t mip = resolvedRange.baseMipLevel + mipOffset;
    auto* descriptor = MTL4::RenderPassDescriptor::alloc()->init();
    descriptor->setRenderTargetWidth(std::max(1u, texture->desc.extent.width >> mip));
    descriptor->setRenderTargetHeight(std::max(1u, texture->desc.extent.height >> mip));
    if(resolvedRange.layerCount > 1)
      descriptor->setRenderTargetArrayLength(resolvedRange.layerCount);
    MTL::RenderPassColorAttachmentDescriptor* attachment =
      descriptor->colorAttachments()->object(0);
    attachment->setTexture(texture->nativeTexture);
    attachment->setLevel(mip);
    attachment->setSlice(resolvedRange.baseArrayLayer);
    attachment->setLoadAction(MTL::LoadActionClear);
    attachment->setStoreAction(MTL::StoreActionStore);
    attachment->setClearColor(MTL::ClearColor::Make(
      clearColor.r, clearColor.g, clearColor.b, clearColor.a));
    MTL4::RenderCommandEncoder* encoder =
      m_commandBuffer->renderCommandEncoder(descriptor);
    descriptor->release();
    if(encoder == nullptr)
      throw std::runtime_error("Metal failed to create a clear render encoder");
    applyPendingBarrier(*encoder);
    encoder->endEncoding();
  }
  recordCommand([handle, resolvedRange, clearColor](MetalCommandBuffer& owner) {
    owner.clearColorTexture(handle, resolvedRange, clearColor);
  });
}

void MetalCommandBuffer::beginEvent(const char* name)
{
  requireRecording("MetalCommandBuffer::beginEvent");
  if(name == nullptr || *name == '\0')
    throw std::invalid_argument("MetalCommandBuffer::beginEvent requires a name");
  m_commandBuffer->pushDebugGroup(nsString(name));
  ++m_debugGroupDepth;
  const std::string replayName{name};
  recordCommand([replayName](MetalCommandBuffer& owner) {
    owner.beginEvent(replayName.c_str());
  });
}

void MetalCommandBuffer::endEvent()
{
  requireRecording("MetalCommandBuffer::endEvent");
  if(m_debugGroupDepth == 0)
    throw std::runtime_error("MetalCommandBuffer::endEvent has no matching beginEvent");
  m_commandBuffer->popDebugGroup();
  --m_debugGroupDepth;
  recordCommand([](MetalCommandBuffer& owner) { owner.endEvent(); });
}

void MetalCommandBuffer::resetQueryPool(
  QueryPoolHandle pool, uint32_t firstQuery, uint32_t queryCount)
{
  requireRecording("MetalCommandBuffer::resetQueryPool");
  if(m_active != EncoderKind::none)
    throw std::runtime_error("Metal query reset requires no active encoder");
  MetalQueryPoolRecord* record = resources().tryGetQueryPool(pool);
  if(record == nullptr || record->nativeHeap == nullptr)
    throw std::invalid_argument("Metal query pool is stale");
  if(queryCount == 0 || firstQuery >= record->count ||
     queryCount > record->count - firstQuery)
    throw std::out_of_range("Metal query reset range is invalid");
  for(uint32_t index = 0; index < record->pendingUses.count; ++index)
  {
    if(!m_allocator->queue().isComplete(record->pendingUses.tokens[index]))
      throw std::runtime_error("Metal query reset requires prior GPU uses to complete");
  }
  record->nativeHeap->invalidateCounterRange(NS::Range::Make(firstQuery, queryCount));
  record->pendingUses = {};
  trackQueryPool(pool);
  retainNative(asObject(record->nativeHeap));
  recordCommand([pool, firstQuery, queryCount](MetalCommandBuffer& owner) {
    owner.resetQueryPool(pool, firstQuery, queryCount);
  });
}

void MetalCommandBuffer::writeTimestamp(
  QueryPoolHandle pool, uint32_t queryIndex, bool afterAllCommands)
{
  requireRecording("MetalCommandBuffer::writeTimestamp");
  if(m_active != EncoderKind::none)
    throw std::runtime_error("Metal timestamp write requires no active encoder");
  MetalQueryPoolRecord* record = resources().tryGetQueryPool(pool);
  if(record == nullptr || record->nativeHeap == nullptr)
    throw std::invalid_argument("Metal timestamp query pool is stale");
  if(queryIndex >= record->count)
    throw std::out_of_range("Metal timestamp query index is invalid");
  m_commandBuffer->writeTimestampIntoHeap(record->nativeHeap, queryIndex);
  trackQueryPool(pool);
  retainNative(asObject(record->nativeHeap));
  recordCommand([pool, queryIndex, afterAllCommands](MetalCommandBuffer& owner) {
    owner.writeTimestamp(pool, queryIndex, afterAllCommands);
  });
}

void MetalCommandBuffer::requireRecording(const char* operation) const
{
  if(m_state != CommandBufferState::recording)
    throw std::runtime_error(std::string(operation) + " requires recording state");
}

void MetalCommandBuffer::recordCommand(ReplayCommand command)
{
  if(!m_replaying)
    m_replayCommands.push_back(std::move(command));
}

MTL4::ArgumentTable* MetalCommandBuffer::materializeArgumentTable(
  MetalEncoderBindingState& state, MetalPipelineKind expectedKind)
{
  const MetalPipelineRecord* pipeline = resources().tryGetPipeline(state.pipeline);
  if(pipeline == nullptr || pipeline->kind != expectedKind)
    throw std::runtime_error("Metal argument-table materialization requires a matching pipeline");
  if(!state.dirty)
    return state.snapshot;
  if(pipeline->bindingLayout.bufferCount == 0 &&
     pipeline->bindingLayout.textureCount == 0 &&
     pipeline->bindingLayout.samplerCount == 0)
  {
    state.snapshot = nullptr;
    state.dirty = false;
    return nullptr;
  }

  MTL4::ArgumentTable* snapshot = allocator().allocateArgumentTable(
    pipeline->bindingLayout.bufferCount,
    pipeline->bindingLayout.textureCount,
    pipeline->bindingLayout.samplerCount,
    "Metal4 composed argument table");

  MTL::ArgumentEncoder* nestedEncoder = pipeline->nestedArgumentEncoder;
  if(pipeline->bindingLayout.nestedArgumentBufferIndex != kInvalidMetalBinding)
  {
    if(nestedEncoder == nullptr)
      throw std::runtime_error("Metal pipeline has no nested argument encoder");
    const NS::UInteger encodedLength = nestedEncoder->encodedLength();
    const NS::UInteger encodedAlignment = nestedEncoder->alignment();
    if(encodedLength == 0 ||
       encodedAlignment > std::numeric_limits<uint32_t>::max())
    {
      throw std::runtime_error("Metal nested argument-buffer layout is invalid");
    }
    const MetalRootDataAllocation nestedArguments =
      allocator().allocateArgumentData(
        encodedLength, static_cast<uint32_t>(encodedAlignment));
    nestedEncoder->setArgumentBuffer(
      nestedArguments.buffer, nestedArguments.offset);
    snapshot->setAddress(
      nestedArguments.gpuAddress,
      pipeline->bindingLayout.nestedArgumentBufferIndex);
  }

  std::vector<std::pair<uint32_t, uint32_t>> dynamicOrdinals;
  const auto takeDynamic = [&state, &dynamicOrdinals](uint32_t tableSlot)
    -> const MetalDynamicBufferValue* {
    uint32_t ordinal = 0;
    auto ordinalIt = std::find_if(
      dynamicOrdinals.begin(), dynamicOrdinals.end(),
      [tableSlot](const auto& value) { return value.first == tableSlot; });
    if(ordinalIt == dynamicOrdinals.end())
    {
      dynamicOrdinals.emplace_back(tableSlot, 1);
    }
    else
    {
      ordinal = ordinalIt->second;
      ++ordinalIt->second;
    }
    uint32_t current = 0;
    for(const MetalDynamicBufferValue& value : state.dynamicBuffers)
    {
      if(value.tableSlot == tableSlot && current++ == ordinal)
        return &value;
    }
    return nullptr;
  };

  for(const MetalArgumentPlacement& placement : pipeline->bindingLayout.arguments)
  {
    const MetalBoundTable* bound = findBoundTable(state, placement.tableSlot);
    if(bound == nullptr)
      continue;
    const MetalArgumentTableRecord* table =
      resources().tryGetArgumentTable(bound->table);
    const MetalArgumentLayoutRecord* actualLayout = table != nullptr
      ? resources().tryGetArgumentLayout(table->layout)
      : nullptr;
    if(table == nullptr || actualLayout == nullptr ||
       !layoutContainsPlacement(*actualLayout, placement))
    {
      const MetalArgumentLayoutRecord* expectedLayout =
        resources().tryGetArgumentLayout(placement.layout);
      const std::string pipelineName = pipeline->debugName.empty()
        ? "<unnamed>" : pipeline->debugName;
      const std::string expectedName = expectedLayout != nullptr &&
                                       !expectedLayout->debugName.empty()
        ? expectedLayout->debugName : "<unnamed>";
      const std::string actualName = actualLayout != nullptr &&
                                     !actualLayout->debugName.empty()
        ? actualLayout->debugName : "<stale>";
      const std::string tableName = table != nullptr && !table->debugName.empty()
        ? table->debugName : "<unnamed>";
      throw std::runtime_error(
        "Metal pipeline '" + pipelineName + "' slot " +
        std::to_string(placement.tableSlot) + " expects argument layout '" +
        expectedName + "', but table '" + tableName + "' uses '" +
        actualName + "'");
    }
    const MetalDynamicBufferValue* dynamic =
      placement.dynamicOffset ? takeDynamic(placement.tableSlot) : nullptr;

    for(uint32_t element = 0; element < placement.arrayCount; ++element)
    {
      const MetalArgumentValue* value =
        findArgumentValue(*table, placement.binding, element);
      if(value == nullptr || !value->written)
        continue;
      if(value->type != placement.type)
        throw std::runtime_error("Metal argument table contains a mismatched binding type");

      if(placement.bufferIndex != kInvalidMetalBinding)
      {
        BufferHandle bufferHandle = value->buffer;
        uint64_t dynamicOffset = 0;
        if(dynamic != nullptr)
        {
          if(dynamic->buffer.isValid())
            bufferHandle = dynamic->buffer;
          dynamicOffset = dynamic->offset;
        }
        const MetalBufferRecord* buffer = resources().tryGetBuffer(bufferHandle);
        if(buffer == nullptr || buffer->nativeBuffer == nullptr)
          throw std::runtime_error("Metal argument table references a stale buffer");
        const uint64_t bindingOffset = value->offset + dynamicOffset;
        const uint64_t bindingSize = dynamic != nullptr && dynamic->size != 0
          ? dynamic->size
          : (value->size == 0 ? buffer->desc.size - bindingOffset : value->size);
        checkedEnd(bindingOffset, bindingSize, buffer->desc.size,
                   "Metal argument buffer binding");
        snapshot->setAddress(buffer->nativeBuffer->gpuAddress() + bindingOffset,
                             placement.bufferIndex + element);
        retainNative(asObject(buffer->nativeBuffer));
      }

      const bool nestedTexture =
        placement.nestedArgumentIndex != kInvalidMetalBinding &&
        (placement.type == ArgumentType::sampledTexture ||
         placement.type == ArgumentType::storageTexture);
      if(placement.textureIndex != kInvalidMetalBinding || nestedTexture)
      {
        const MetalTextureViewRecord* view =
          resources().tryGetTextureView(value->textureView);
        if(view == nullptr)
          throw std::runtime_error("Metal argument table references a stale texture view");
        MTL::ResourceID resourceId{view->resourceId};
        if(resourceId._impl == 0 && view->nativeTextureView != nullptr)
          resourceId = view->nativeTextureView->gpuResourceID();
        if(resourceId._impl == 0)
          throw std::runtime_error("Metal texture view has no GPU resource ID");
        if(nestedTexture)
        {
          nestedEncoder->setTexture(
            view->nativeTextureView,
            placement.nestedArgumentIndex + element);
        }
        else
        {
          snapshot->setTexture(resourceId, placement.textureIndex + element);
        }
        if(view->parentTexture.isValid())
        {
          const MetalTextureRecord* parent =
            resources().tryGetTexture(view->parentTexture);
          if(parent == nullptr || parent->nativeTexture == nullptr)
            throw std::runtime_error("Metal texture view references a stale parent texture");
          retainNative(asObject(parent->nativeTexture));
        }
        if(view->nativeTextureView != nullptr)
          retainNative(asObject(view->nativeTextureView));
      }

      const bool nestedSampler =
        placement.nestedArgumentIndex != kInvalidMetalBinding &&
        placement.type == ArgumentType::sampler;
      if(placement.samplerIndex != kInvalidMetalBinding || nestedSampler)
      {
        const MetalSamplerRecord* sampler = resources().tryGetSampler(value->sampler);
        if(sampler == nullptr || sampler->nativeSampler == nullptr)
          throw std::runtime_error("Metal argument table references a stale sampler");
        if(nestedSampler)
        {
          nestedEncoder->setSamplerState(
            sampler->nativeSampler,
            placement.nestedArgumentIndex + element);
        }
        else
        {
          snapshot->setSamplerState(sampler->nativeSampler->gpuResourceID(),
                                    placement.samplerIndex + element);
        }
        retainNative(asObject(sampler->nativeSampler));
      }
    }
  }

  for(const MetalRootPlacement& placement : pipeline->bindingLayout.roots)
  {
    if(placement.bufferIndex == kInvalidMetalBinding)
      continue;
    const MetalRootValue* value = nullptr;
    const auto found = std::find_if(
      state.roots.begin(), state.roots.end(),
      [&placement](const MetalRootValue& root) { return root.slot == placement.slot; });
    if(found != state.roots.end())
      value = &*found;

    if(placement.kind == RootBindingKind::constants)
    {
      if(value == nullptr)
        continue;
      if(value->kind != RootBindingKind::constants ||
         value->constants.empty() || value->constants.size() > placement.size)
      {
        throw std::runtime_error("Metal root constants do not match the pipeline schema");
      }
      std::vector<std::byte> padded(placement.size);
      std::copy(value->constants.begin(), value->constants.end(), padded.begin());
      const MetalRootDataAllocation allocation = allocator().allocateRootData(
        padded, std::max(placement.alignment, 16u));
      snapshot->setAddress(allocation.gpuAddress, placement.bufferIndex);
    }
    else if(placement.kind == RootBindingKind::gpuPointer)
    {
      if(value == nullptr)
        continue;
      if(value->kind != RootBindingKind::gpuPointer || !value->pointer.isValid())
        throw std::runtime_error("Metal root pointer does not match the pipeline schema");
      snapshot->setAddress(value->pointer.value, placement.bufferIndex);
    }
    else
    {
      const MetalBoundTable* bound =
        findBoundTable(state, placement.dynamicBuffer.tableSlot);
      if(bound == nullptr)
        continue;
      const MetalArgumentTableRecord* table =
        resources().tryGetArgumentTable(bound->table);
      const MetalArgumentValue* argument = table != nullptr
        ? findArgumentValue(*table, placement.dynamicBuffer.binding, 0)
        : nullptr;
      if(argument == nullptr || !argument->written)
        continue;
      const MetalBufferRecord* buffer = resources().tryGetBuffer(argument->buffer);
      if(buffer == nullptr || buffer->nativeBuffer == nullptr)
        throw std::runtime_error("Metal dynamic root binding references a stale buffer");
      checkedEnd(argument->offset, argument->size, buffer->desc.size,
                 "Metal dynamic root binding");
      snapshot->setAddress(buffer->nativeBuffer->gpuAddress() + argument->offset,
                           placement.bufferIndex);
      retainNative(asObject(buffer->nativeBuffer));
    }
  }

  for(const MetalVertexPlacement& placement : pipeline->bindingLayout.vertexBuffers)
  {
    const auto found = std::find_if(
      state.vertexBuffers.begin(), state.vertexBuffers.end(),
      [&placement](const MetalVertexBufferValue& value) {
        return value.binding == placement.binding;
      });
    if(found == state.vertexBuffers.end())
      continue;
    const MetalBufferRecord* buffer = resources().tryGetBuffer(found->buffer);
    if(buffer == nullptr)
      throw std::runtime_error("Metal vertex table references a stale buffer");
    checkedEnd(found->offset, 0, buffer->desc.size, "Metal vertex table binding");
    snapshot->setAddress(buffer->nativeBuffer->gpuAddress() + found->offset,
                         placement.bufferIndex);
    trackBuffer(found->buffer);
    retainNative(asObject(buffer->nativeBuffer));
  }

  MetalArgumentTableSnapshot trackedSnapshot;
  for(const MetalBoundTable& bound : state.tables)
  {
    const auto duplicate = std::find_if(
      trackedSnapshot.sources.begin(), trackedSnapshot.sources.end(),
      [&bound](const MetalArgumentTableSnapshotSource& source) {
        return source.table == bound.table;
      });
    if(duplicate != trackedSnapshot.sources.end())
      continue;
    const MetalArgumentTableRecord* table =
      resources().tryGetArgumentTable(bound.table);
    if(table == nullptr)
      throw std::runtime_error("Metal argument-table snapshot source is stale");
    trackedSnapshot.sources.push_back({bound.table, table->version});
  }
  m_argumentTableSnapshots.push_back(std::move(trackedSnapshot));

  state.snapshot = snapshot;
  state.dirty = false;
  return snapshot;
}

bool MetalCommandBuffer::argumentTableSnapshotsChanged() const
{
  for(const MetalArgumentTableSnapshot& snapshot : m_argumentTableSnapshots)
  {
    for(const MetalArgumentTableSnapshotSource& source : snapshot.sources)
    {
      const MetalArgumentTableRecord* table =
        m_resourceTable->tryGetArgumentTable(source.table);
      if(table == nullptr)
        throw std::runtime_error("Metal submission references a stale argument table");
      if(table->version != source.version)
        return true;
    }
  }
  return false;
}

void MetalCommandBuffer::reencodeForSubmit()
{
  require(!m_replaying && m_state == CommandBufferState::executable,
          "Metal command buffer cannot replay in its current state");
  MTL4::CommandBuffer* replacement = m_allocator->device()->newCommandBuffer();
  require(replacement != nullptr,
          "Metal failed to allocate a command buffer for argument-table replay");

  // MTL4 encoders consume argument-table contents while draw/dispatch commands
  // are encoded. Rebuild the native command buffer so executable, unsubmitted
  // RHI command buffers observe the table's final version.
  MTL4::CommandBuffer* previous = m_commandBuffer;
  m_commandBuffer = replacement;
  previous->release();
  m_state = CommandBufferState::reusable;
  m_replaying = true;
  try
  {
    begin(*m_allocator);
    for(const ReplayCommand& command : m_replayCommands)
      command(*this);
    end();
    m_replaying = false;
  }
  catch(...)
  {
    m_replaying = false;
    if(m_state == CommandBufferState::recording)
    {
      m_renderEncoder.invalidate();
      m_computeEncoder.invalidate();
      m_nativeRenderEncoder = nullptr;
      m_nativeComputeEncoder = nullptr;
      m_active = EncoderKind::none;
      m_allocator->releaseRecording(*this);
      MTL4::CommandBuffer* failed = m_commandBuffer;
      m_commandBuffer = m_allocator->device()->newCommandBuffer();
      failed->release();
      m_state = CommandBufferState::reusable;
    }
    throw;
  }

  if(argumentTableSnapshotsChanged())
    throw std::runtime_error("Metal argument table changed concurrently during submission");
}

void MetalCommandBuffer::rebuildAutomaticResidencySet()
{
  require(m_automaticResidencySet != nullptr,
          "Metal command buffer has no automatic residency set");
  std::vector<const MTL::Allocation*> next;
  m_allocator->appendScratchAllocations(next);

  for(BufferHandle handle : m_buffers)
  {
    const MetalBufferRecord* buffer = m_resourceTable->tryGetBuffer(handle);
    if(buffer != nullptr && buffer->nativeBuffer != nullptr)
      next.push_back(buffer->nativeBuffer);
  }
  for(TextureHandle handle : m_textures)
  {
    const MetalTextureRecord* texture = m_resourceTable->tryGetTexture(handle);
    if(texture != nullptr && texture->nativeTexture != nullptr &&
       texture->desc.memoryUsage != MemoryUsage::transientAttachment)
    {
      next.push_back(texture->nativeTexture);
    }
  }
  for(TextureViewHandle handle : m_textureViews)
  {
    const MetalTextureViewRecord* view = m_resourceTable->tryGetTextureView(handle);
    const MetalTextureRecord* parent = view != nullptr
      ? m_resourceTable->tryGetTexture(view->parentTexture)
      : nullptr;
    if(view != nullptr && view->nativeTextureView != nullptr &&
       (parent == nullptr ||
        parent->desc.memoryUsage != MemoryUsage::transientAttachment))
    {
      next.push_back(view->nativeTextureView);
    }
  }
  for(PipelineHandle handle : m_pipelines)
  {
    const MetalPipelineRecord* pipeline = m_resourceTable->tryGetPipeline(handle);
    if(pipeline == nullptr)
      continue;
    if(pipeline->kind == MetalPipelineKind::graphics &&
       pipeline->nativeRenderPipeline != nullptr)
    {
      next.push_back(pipeline->nativeRenderPipeline);
    }
    else if(pipeline->kind == MetalPipelineKind::compute &&
            pipeline->nativeComputePipeline != nullptr)
    {
      next.push_back(pipeline->nativeComputePipeline);
    }
  }
  next.insert(next.end(), m_nativeAllocations.begin(), m_nativeAllocations.end());

  const std::less<const MTL::Allocation*> less;
  std::sort(next.begin(), next.end(), less);
  next.erase(std::unique(next.begin(), next.end()), next.end());

  std::vector<const MTL::Allocation*> removals;
  std::vector<const MTL::Allocation*> additions;
  std::set_difference(
    m_automaticResidencyAllocations.begin(), m_automaticResidencyAllocations.end(),
    next.begin(), next.end(), std::back_inserter(removals), less);
  std::set_difference(
    next.begin(), next.end(), m_automaticResidencyAllocations.begin(),
    m_automaticResidencyAllocations.end(), std::back_inserter(additions), less);
  if(removals.empty() && additions.empty())
    return;
  if(!removals.empty())
    m_automaticResidencySet->removeAllocations(removals.data(), removals.size());
  if(!additions.empty())
    m_automaticResidencySet->addAllocations(additions.data(), additions.size());
  m_automaticResidencySet->commit();
  m_automaticResidencyAllocations = std::move(next);
}

void MetalCommandBuffer::retainNative(NS::Object* object)
{
  if(object == nullptr ||
     std::find(m_retainedResources.begin(), m_retainedResources.end(), object) !=
       m_retainedResources.end())
  {
    return;
  }
  object->retain();
  m_retainedResources.push_back(object);
}

void MetalCommandBuffer::releaseRetainedResources() noexcept
{
  for(NS::Object* object : m_retainedResources)
    object->release();
  m_retainedResources.clear();
}

void MetalCommandBuffer::trackArgumentTable(ArgumentTableHandle value)
{
  trackUnique(m_argumentTables, value);
}

void MetalCommandBuffer::trackPipeline(PipelineHandle value)
{
  trackUnique(m_pipelines, value);
}

void MetalCommandBuffer::trackBuffer(BufferHandle value)
{
  trackUnique(m_buffers, value);
}

void MetalCommandBuffer::trackTexture(TextureHandle value)
{
  trackUnique(m_textures, value);
}

void MetalCommandBuffer::trackTextureView(TextureViewHandle value)
{
  trackUnique(m_textureViews, value);
}

void MetalCommandBuffer::trackSampler(SamplerHandle value)
{
  trackUnique(m_samplers, value);
}

void MetalCommandBuffer::trackQueryPool(QueryPoolHandle value)
{
  trackUnique(m_queryPools, value);
}

void MetalCommandBuffer::trackResidencySet(ResidencySetHandle value)
{
  trackUnique(m_residencySets, value);
}

void MetalCommandBuffer::trackNativeAllocation(
  const MTL::Allocation* allocation)
{
  if(allocation != nullptr &&
     std::find(m_nativeAllocations.begin(), m_nativeAllocations.end(), allocation) ==
       m_nativeAllocations.end())
  {
    m_nativeAllocations.push_back(allocation);
  }
}

void MetalCommandBuffer::clearTracking()
{
  m_argumentTables.clear();
  m_pipelines.clear();
  m_buffers.clear();
  m_textures.clear();
  m_textureViews.clear();
  m_samplers.clear();
  m_queryPools.clear();
  m_residencySets.clear();
  m_nativeAllocations.clear();
  m_argumentTableSnapshots.clear();
  m_preparedIndexedIndirect.clear();
}

MetalPreparedIndexedIndirect*
MetalCommandBuffer::consumePreparedIndexedIndirect(
  const DrawIndirectDesc& draw, BufferHandle indexBuffer,
  uint64_t indexBufferOffset, IndexFormat indexFormat,
  PrimitiveTopology topology)
{
  MetalPreparedIndexedIndirect* prepared = findValue(
    m_preparedIndexedIndirect,
    [&](const MetalPreparedIndexedIndirect& candidate) {
      return !candidate.consumed &&
             candidate.draw.argsBuffer == draw.argsBuffer &&
             candidate.draw.argsOffset == draw.offset &&
             !candidate.draw.countBuffer.isValid() &&
             candidate.draw.maxDrawCount == draw.drawCount &&
             candidate.draw.stride == draw.stride &&
             candidate.indexBuffer == indexBuffer &&
             candidate.indexBufferOffset == indexBufferOffset &&
             candidate.indexFormat == indexFormat &&
             candidate.topology == topology;
    });
  if(prepared == nullptr)
    return nullptr;
  if(prepared->commandBuffer == nullptr ||
     prepared->executionRangeBuffer == nullptr ||
     prepared->clampedArgumentsBuffer == nullptr ||
     prepared->argumentStride == 0)
  {
    throw std::runtime_error(
      "Metal indexed-indirect preparation is incomplete");
  }
  prepared->consumed = true;
  return prepared;
}

MetalPreparedIndexedIndirect&
MetalCommandBuffer::consumePreparedIndexedIndirectCount(
  const DrawIndirectCountDesc& draw, BufferHandle indexBuffer,
  uint64_t indexBufferOffset, IndexFormat indexFormat,
  PrimitiveTopology topology)
{
  const auto matches = [&](const MetalPreparedIndexedIndirect& prepared) {
    return !prepared.consumed &&
           prepared.draw.argsBuffer == draw.argsBuffer &&
           prepared.draw.argsOffset == draw.argsOffset &&
           prepared.draw.countBuffer == draw.countBuffer &&
           prepared.draw.countBufferOffset == draw.countBufferOffset &&
           prepared.draw.maxDrawCount == draw.maxDrawCount &&
           prepared.draw.stride == draw.stride &&
           prepared.indexBuffer == indexBuffer &&
           prepared.indexBufferOffset == indexBufferOffset &&
           prepared.indexFormat == indexFormat &&
           prepared.topology == topology;
  };
  const auto found = std::find_if(
    m_preparedIndexedIndirect.begin(), m_preparedIndexedIndirect.end(), matches);
  if(found == m_preparedIndexedIndirect.end())
  {
    throw std::runtime_error(
      "Metal indexed-indirect-count draw has no matching pre-render ICB preparation");
  }
  if(found->commandBuffer == nullptr || found->executionRangeBuffer == nullptr ||
     found->clampedArgumentsBuffer == nullptr || found->argumentStride == 0)
  {
    throw std::runtime_error(
      "Metal indexed-indirect-count preparation is incomplete");
  }
  found->consumed = true;
  return *found;
}

void MetalCommandBuffer::validateForSubmit(const MetalQueue& queue)
{
  if(m_state != CommandBufferState::executable || m_allocator == nullptr ||
     queue.identity() != m_allocator->queue().identity())
  {
    throw std::runtime_error("Metal command-buffer submission state or queue is invalid");
  }
  if(argumentTableSnapshotsChanged())
    reencodeForSubmit();
  for(ArgumentTableHandle handle : m_argumentTables)
  {
    const MetalArgumentTableRecord* table = m_resourceTable->tryGetArgumentTable(handle);
    if(table == nullptr)
      throw std::runtime_error("Metal submission references a stale argument table");
    for(const MetalArgumentValue& value : table->values)
    {
      if(!value.written)
        continue;
      if(value.buffer.isValid())
      {
        if(m_resourceTable->tryGetBuffer(value.buffer) == nullptr)
          throw std::runtime_error("Metal argument table references a stale buffer");
        trackBuffer(value.buffer);
      }
      if(value.textureView.isValid())
      {
        const MetalTextureViewRecord* view =
          m_resourceTable->tryGetTextureView(value.textureView);
        if(view == nullptr)
          throw std::runtime_error("Metal argument table references a stale texture view");
        trackTextureView(value.textureView);
        if(view->parentTexture.isValid())
        {
          if(m_resourceTable->tryGetTexture(view->parentTexture) == nullptr)
            throw std::runtime_error("Metal argument table texture view has a stale parent");
          trackTexture(view->parentTexture);
        }
      }
      if(value.sampler.isValid())
      {
        if(m_resourceTable->tryGetSampler(value.sampler) == nullptr)
          throw std::runtime_error("Metal argument table references a stale sampler");
        trackSampler(value.sampler);
      }
    }
  }
  for(PipelineHandle handle : m_pipelines)
    if(m_resourceTable->tryGetPipeline(handle) == nullptr)
      throw std::runtime_error("Metal submission references a stale pipeline");
  for(BufferHandle handle : m_buffers)
    if(m_resourceTable->tryGetBuffer(handle) == nullptr)
      throw std::runtime_error("Metal submission references a stale buffer");
  for(TextureHandle handle : m_textures)
    if(m_resourceTable->tryGetTexture(handle) == nullptr)
      throw std::runtime_error("Metal submission references a stale texture");
  for(TextureViewHandle handle : m_textureViews)
    if(m_resourceTable->tryGetTextureView(handle) == nullptr)
      throw std::runtime_error("Metal submission references a stale texture view");
  for(SamplerHandle handle : m_samplers)
    if(m_resourceTable->tryGetSampler(handle) == nullptr)
      throw std::runtime_error("Metal submission references a stale sampler");
  for(QueryPoolHandle handle : m_queryPools)
    if(m_resourceTable->tryGetQueryPool(handle) == nullptr)
      throw std::runtime_error("Metal submission references a stale query pool");
  for(ResidencySetHandle handle : m_residencySets)
  {
    const MetalResidencySetRecord* set = m_resourceTable->tryGetResidencySet(handle);
    if(set == nullptr)
      throw std::runtime_error("Metal submission references a stale residency set");
    for(ResidencyResource resource : set->resources)
      if(!m_resourceTable->isAlive(resource))
        throw std::runtime_error("Metal residency set references a stale resource");
  }
  rebuildAutomaticResidencySet();
}

void MetalCommandBuffer::markSubmitted(SubmissionToken token)
{
  if(m_state != CommandBufferState::executable || !token.isValid() ||
     token.queue != m_allocator->queue().identity())
  {
    throw std::runtime_error("Metal command-buffer submission token is invalid");
  }
  const auto recordUse = [token](SubmissionTokenSet& uses) {
    if(!uses.record(token))
      throw std::runtime_error("Metal resource exceeded the queue-dependency capacity");
  };
  for(ArgumentTableHandle handle : m_argumentTables)
    recordUse(m_resourceTable->tryGetArgumentTable(handle)->pendingUses);
  for(PipelineHandle handle : m_pipelines)
    recordUse(m_resourceTable->tryGetPipeline(handle)->pendingUses);
  for(BufferHandle handle : m_buffers)
    recordUse(m_resourceTable->tryGetBuffer(handle)->pendingUses);
  for(TextureHandle handle : m_textures)
    recordUse(m_resourceTable->tryGetTexture(handle)->pendingUses);
  for(TextureViewHandle handle : m_textureViews)
    recordUse(m_resourceTable->tryGetTextureView(handle)->pendingUses);
  for(SamplerHandle handle : m_samplers)
    recordUse(m_resourceTable->tryGetSampler(handle)->pendingUses);
  for(QueryPoolHandle handle : m_queryPools)
    recordUse(m_resourceTable->tryGetQueryPool(handle)->pendingUses);
  const auto recordResidencyResourceUse =
    [this, &recordUse](ResidencyResource resource) {
      switch(resource.kind)
      {
      case ResidencyResourceKind::buffer:
        recordUse(m_resourceTable->tryGetBuffer(
          BufferHandle{resource.index, resource.generation})->pendingUses);
        break;
      case ResidencyResourceKind::texture:
        recordUse(m_resourceTable->tryGetTexture(
          TextureHandle{resource.index, resource.generation})->pendingUses);
        break;
      case ResidencyResourceKind::textureView:
      {
        MetalTextureViewRecord* view = m_resourceTable->tryGetTextureView(
          TextureViewHandle{resource.index, resource.generation});
        recordUse(view->pendingUses);
        if(view->parentTexture.isValid())
          recordUse(m_resourceTable->tryGetTexture(view->parentTexture)->pendingUses);
        break;
      }
      case ResidencyResourceKind::pipeline:
        recordUse(m_resourceTable->tryGetPipeline(
          PipelineHandle{resource.index, resource.generation})->pendingUses);
        break;
      case ResidencyResourceKind::sampler:
      case ResidencyResourceKind::shaderLibrary:
      case ResidencyResourceKind::argumentTable:
        throw std::runtime_error(
          "Metal residency set contains a non-allocation resource");
      }
    };
  for(ResidencySetHandle handle : m_residencySets)
  {
    MetalResidencySetRecord* set = m_resourceTable->tryGetResidencySet(handle);
    for(ResidencyResource resource : set->resources)
      recordResidencyResourceUse(resource);
    recordUse(set->pendingUses);
  }
  m_submission = token;
  m_state = CommandBufferState::submitted;
  m_allocator->noteSubmitted(*this, token);
}

void MetalCommandBuffer::markReusable()
{
  if(m_state == CommandBufferState::recording)
    throw std::runtime_error("Metal command buffer cannot reset while recording");
  if(m_state == CommandBufferState::submitted &&
     !m_allocator->queue().isComplete(m_submission))
  {
    throw std::runtime_error("Metal command buffer cannot become reusable before completion");
  }
  releaseRetainedResources();
  clearTracking();
  m_replayCommands.clear();
  m_active = EncoderKind::none;
  m_nativeRenderEncoder = nullptr;
  m_nativeComputeEncoder = nullptr;
  m_submission = {};
  m_state = CommandBufferState::reusable;
}

}  // namespace demo::rhi::metal
