# RHI 迁移实施计划：Vulkan → 现代 GPU 接口（Encoder + StageBarrier + GpuPtr）

> 基于 `future-rhi-design-review.md` 制定的可执行文件级计划。
> 策略：先验证 Vulkan，保留兼容层，逐 Wave 迁移，每 Wave 可编译运行。

---

## 0. 前置确认

### 0.1 StageBarrier 手写：完全可行

**结论**：不实现 RenderGraph，在 Pass 中手写 `barrier(producer, consumer, hazard)` 是当前最务实的方案。

**理由**：
- 当前 `PassExecutor` 已控制 pass 执行顺序（`std::vector<const PassNode*> m_passes`）
- 每个 Pass 的 `execute()` 方法天然是 barrier 插入点
- 与 HypeHype / No Graphics API 的实践一致
- 后端将 `barrier(StageFlags, StageFlags, HazardFlags)` 转换为 `vkCmdPipelineBarrier2`

**手写示例**：

```cpp
// GPUDrivenCullingPass.cpp（Compute Pass）
void GPUDrivenCullingPass::execute(const PassContext& context) const
{
    // 前一个 DepthPyramidPass（compute）写了 depth pyramid
    // 当前 pass（compute）读取 depth pyramid 并写 indirect buffer
    context.cmdBuffer->barrier(
        rhi::StageFlags::compute,           // producer: depth pyramid compute write
        rhi::StageFlags::compute,           // consumer: culling compute read
        rhi::HazardFlags::textureWrites     // hazard: UAV/storage texture write -> read
    );

    auto* enc = context.cmdBuffer->beginComputePass();
    enc->setPipeline(m_renderer->getGPUCullingPipelineHandle());
    enc->setArgumentTable(0, m_renderer->getGPUCullingBindGroup(context.frameIndex));
    enc->dispatch((objectCount + 255) / 256, 1, 1);
    context.cmdBuffer->endEncoding();

    // Culling pass 产生 indirect draw args，后续 graphics pass 消费
    context.cmdBuffer->barrier(
        rhi::StageFlags::compute,
        rhi::StageFlags::commandInput,      // consumer: indirect draw args
        rhi::HazardFlags::drawArguments | rhi::HazardFlags::bufferWrites
    );
}
```

```cpp
// GPUDrivenLightPass.cpp（Render Pass）
void GPUDrivenLightPass::execute(const PassContext& context) const
{
    // GBuffer passes 写了 color/depth attachments
    // Light pass 读取 GBuffer textures
    context.cmdBuffer->barrier(
        rhi::StageFlags::rasterColorOut | rhi::StageFlags::rasterDepthOut,
        rhi::StageFlags::fragmentShader,
        rhi::HazardFlags::textureWrites | rhi::HazardFlags::depthStencil
    );

    auto* enc = context.cmdBuffer->beginRenderPass({
        .colorTargets = &colorTarget,
        .colorTargetCount = 1,
        .depthTarget = nullptr,  // Light pass 通常只写 color，depth 只读
    });
    enc->setPipeline(m_renderer->getLightPipelineHandle());
    enc->setArgumentTable(0, m_renderer->getLightingInputBindGroup(context.frameIndex));
    enc->drawIndexed({...});
    context.cmdBuffer->endEncoding();
}
```

**注意事项**：
- 当前 `PassContext` 中的 `cmd` 是 `rhi::CommandList*`（unified）
- 改为 Encoder 模型后，`PassContext` 将持有 `rhi::CommandBuffer*`
- `beginEvent`/`endEvent` 保留在 `CommandBuffer` 级别（跨 Encoder）
- `barrier()` 在 `CommandBuffer` 级别调用（Vulkan 下 barrier 可以在 command buffer 的任何位置，但在 render pass 内需要 synchronization2 的 subpass dependency 或 pipeline barrier）

---

## 1. 当前 23 个 Pass 的详尽分类与 Encoder 替换映射

| # | Pass 文件 | 当前类型 | 使用的主要 RHI API | 目标 Encoder | 复杂度 | 依赖 |
|---|----------|---------|-------------------|-------------|--------|------|
| 1 | `GPUDrivenCullingPass.cpp` | Compute | `bindPipeline`, `bindBindGroup`, `dispatch` | `ComputeEncoder` | 低 | 无 |
| 2 | `GPUDrivenDepthPyramidPass.cpp` | Compute | `executeDepthPyramidPass`（内部 dispatch） | `ComputeEncoder` | 低 | 无 |
| 3 | `GPUDrivenLightCullingPass.cpp` | Compute | `dispatch` | `ComputeEncoder` | 低 | 无 |
| 4 | `GPUDrivenClusteredLightCullingPass.cpp` | Compute | `dispatch` | `ComputeEncoder` | 低 | 无 |
| 5 | `GPUDrivenVisibilitySortPass.cpp` | Compute | `dispatch` | `ComputeEncoder` | 低 | 无 |
| 6 | `GPUDrivenAOPass.cpp` | Compute | `dispatch` | `ComputeEncoder` | 中 | 需要 Texture/Buffer R/W |
| 7 | `GPUDrivenSSRPass.cpp` | Compute | `dispatch` | `ComputeEncoder` | 中 | 需要 Texture/Buffer R/W |
| 8 | `GPUDrivenBloomPrefilterPass.cpp` | Compute | `dispatch` | `ComputeEncoder` | 低 | 无 |
| 9 | `GPUDrivenBloomDownsamplePass.cpp` | Compute | `dispatch` | `ComputeEncoder` | 低 | 无 |
| 10 | `GPUDrivenTAAResolvePass.cpp` | Compute | `dispatch` | `ComputeEncoder` | 中 | 需要 history texture |
| 11 | `GPUDrivenDepthPrepass.cpp` | Render | `beginRenderPass`, `bindPipeline`, `drawIndexed` | `RenderEncoder` | 中 | 需要 depth attachment |
| 12 | `GPUDrivenGBufferPass.cpp` | Render | `beginRenderPass`, `bindPipeline`, `drawIndexed` | `RenderEncoder` | 高 | MRT，MDI 路径 |
| 13 | `GPUDrivenForwardPass.cpp` | Render | `beginRenderPass`, `bindPipeline`, `drawIndexed` | `RenderEncoder` | 中 | transparent draws |
| 14 | `GPUDrivenLightPass.cpp` | Render | `beginRenderPass`, `bindPipeline`, `drawIndexed` | `RenderEncoder` | 中 | fullscreen quad |
| 15 | `GPUDrivenSkyboxPass.cpp` | Render | `beginRenderPass`, `bindPipeline`, `drawIndexed` | `RenderEncoder` | 低 | fullscreen/cube draw |
| 16 | `GPUDrivenSkyPass.cpp` | Render | `beginRenderPass`, `bindPipeline`, `drawIndexed` | `RenderEncoder` | 低 | fullscreen quad |
| 17 | `GPUDrivenCSMShadowPass.cpp` | Render | `beginRenderPass`, `bindPipeline`, `drawIndexed` | `RenderEncoder` | 中 | cascade array |
| 18 | `GPUDrivenShadowAtlasPass.cpp` | Render | `beginRenderPass`, `bindPipeline`, `drawIndexed` | `RenderEncoder` | 中 | atlas tiles |
| 19 | `GPUDrivenDebugPass.cpp` | Render | `beginRenderPass`, `bindPipeline`, `drawIndexed` | `RenderEncoder` | 低 | line overlay |
| 20 | `GPUDrivenImguiPass.cpp` | Render | `beginRenderPass`, `bindPipeline`, `drawIndexed` | `RenderEncoder` | 中 | ImGui draw data |
| 21 | `GPUDrivenVelocityPass.cpp` | Render | `beginRenderPass`, `bindPipeline`, `drawIndexed` | `RenderEncoder` | 低 | motion vectors |
| 22 | `GPUDrivenFinalColorPass.cpp` | Render | `beginRenderPass`, `bindPipeline`, `drawIndexed` | `RenderEncoder` | 中 | tone mapping |
| 23 | `GPUDrivenPresentPass.cpp` | Copy/Blit | `transitionTexture`, `blitImage` | `CopyEncoder` + `CommandBuffer::barrier` | 低 | swapchain blit |

**迁移顺序建议**：
1. 先从 **纯 Compute Pass**（#1-5）开始，因为它们不涉 `beginRenderPass`，替换最简单
2. 然后是 **纯 Copy Pass**（#23）
3. 然后是 **简单 Render Pass**（#15,16,19,21）
4. 然后是 **中等 Render Pass**（#11,13,14,17,18,22）
5. 最后是 **复杂 Render Pass**（#12 GBuffer）和 **Compute Pass with complex R/W**（#6,7,10）

---

## 2. 总体迁移策略：渐进式接口替换

**核心原则**：
1. **不删除旧代码，先建新的**：保留现有 `rhi::CommandList` / `BindGroup` / `BindTable` 作为兼容层
2. **新接口在现有文件上扩展**：新增头文件或扩展现有头文件
3. **Vulkan 后端先实现**：确保每 Wave 可编译运行
4. **Pass 逐个迁移**：选择一个试点 Pass 完全迁移后，再推广到其他 Pass
5. **兼容层最后删除**：所有业务层迁移完成后，一次性删除旧接口

**兼容层策略**：
```cpp
// rhi/RHICommandList.h 中保留旧接口为 deprecated
class CommandList {
public:
    [[deprecated("Use CommandBuffer + Encoder instead")]]
    virtual void drawIndexed(...) = 0;
    // ...
};

// 新增 rhi/RHICommandBuffer.h
class CommandBuffer {
public:
    virtual RenderEncoder* beginRenderPass(const RenderPassDesc&) = 0;
    virtual ComputeEncoder* beginComputePass() = 0;
    virtual CopyEncoder* beginCopyPass() = 0;
    virtual void barrier(StageFlags producer, StageFlags consumer, HazardFlags hazards) = 0;
    virtual void endEncoding() = 0;
    virtual void beginEvent(const char* name) = 0;
    virtual void endEvent() = 0;
};
```

---

## 3. Wave 0：新 RHI Core 公共接口定义（不破坏现有代码）

**目标**：定义新的 RHI 接口，让 Vulkan 后端可以开始实现，同时现有业务层代码继续编译。

**验证标准**：项目编译通过，现有功能不受影响。

### 3.1 新建/修改的文件清单

#### File: `rhi/RHIStageBarrier.h` （新建）

```cpp
#pragma once
#include <cstdint>

namespace demo::rhi {

enum class StageFlags : uint64_t {
    none           = 0,
    transfer       = 1ull << 0,
    compute        = 1ull << 1,
    vertexShader   = 1ull << 2,
    fragmentShader = 1ull << 3,
    rasterColorOut = 1ull << 4,
    rasterDepthOut = 1ull << 5,
    commandInput   = 1ull << 6,  // Indirect draw/dispatch args
    all            = ~0ull,
};

constexpr StageFlags operator|(StageFlags a, StageFlags b) {
    return static_cast<StageFlags>(static_cast<uint64_t>(a) | static_cast<uint64_t>(b));
}
constexpr StageFlags operator&(StageFlags a, StageFlags b) {
    return static_cast<StageFlags>(static_cast<uint64_t>(a) & static_cast<uint64_t>(b));
}

enum class HazardFlags : uint32_t {
    none          = 0,
    descriptors   = 1u << 0,
    drawArguments = 1u << 1,
    depthStencil  = 1u << 2,
    textureWrites = 1u << 3,
    bufferWrites  = 1u << 4,
};

constexpr HazardFlags operator|(HazardFlags a, HazardFlags b) {
    return static_cast<HazardFlags>(static_cast<uint32_t>(a) | static_cast<uint32_t>(b));
}

} // namespace demo::rhi
```

#### File: `rhi/RHIHandles.h` （扩展）

在现有 `Handle<Tag>` 基础上新增：

```cpp
struct BufferTag;
struct ShaderModuleTag;
struct PipelineLayoutTag;
struct QueryPoolTag;
struct ArgumentLayoutTag;
struct ArgumentTableTag;
struct ResidencySetTag;

using BufferHandle         = Handle<BufferTag>;
using ShaderModuleHandle   = Handle<ShaderModuleTag>;
using PipelineLayoutHandle = Handle<PipelineLayoutTag>;
using QueryPoolHandle      = Handle<QueryPoolTag>;
using ArgumentLayoutHandle = Handle<ArgumentLayoutTag>;
using ArgumentTableHandle  = Handle<ArgumentTableTag>;
using ResidencySetHandle   = Handle<ResidencySetTag>;
```

**注意**：保留现有 `TextureViewHandle` 的 `fromNativePtr`/`toNativePtr`，但标记 `[[deprecated]]`。

#### File: `rhi/RHITypes.h` （扩展）

新增/替换结构：

```cpp
// 替换 TextureCreateDesc（移除 native 字段）
struct TextureCreateDesc {
    TextureFormat    format{TextureFormat::undefined};
    TextureUsageFlags usage{TextureUsageFlags::none};  // 新增 flags 类型
    uint32_t         width{0}, height{0}, depth{1};
    uint32_t         mipLevels{1}, arrayLayers{1};
    SampleCount      sampleCount{SampleCount::count1};
    TextureDimension dimension{TextureDimension::e2D};
    bool             cubeCompatible{false};
    const char*      debugName{nullptr};
};

// 替换 TextureViewCreateDesc
struct TextureViewCreateDesc {
    TextureHandle    image{};
    TextureFormat    format{TextureFormat::undefined};
    ImageViewType    viewType{ImageViewType::e2D};
    TextureAspect    aspect{TextureAspect::color};
    uint32_t         baseMipLevel{0}, levelCount{1};
    uint32_t         baseArrayLayer{0}, layerCount{1};
    ComponentMapping components{};
    const char*      debugName{nullptr};
};

// 新增 BufferDesc
struct BufferDesc {
    uint64_t         size{0};
    BufferUsageFlags usage{BufferUsageFlags::none};
    MemoryUsage      memoryUsage{MemoryUsage::gpuOnly};
    bool             allowGpuAddress{false};
    bool             allowIndirectArgument{false};
    const char*      debugName{nullptr};
};

// 新增 GpuPtr
struct GpuPtr {
    uint64_t value{0};
    [[nodiscard]] bool isValid() const { return value != 0; }
};

// 新增 SamplerDesc（基础版）
enum class Filter : uint8_t { nearest = 0, linear };
enum class MipmapMode : uint8_t { nearest = 0, linear };
enum class AddressMode : uint8_t { repeat = 0, clampToEdge, clampToBorder, mirroredRepeat };

struct SamplerDesc {
    Filter      magFilter{Filter::linear};
    Filter      minFilter{Filter::linear};
    MipmapMode  mipmapMode{MipmapMode::linear};
    AddressMode addressModeU{AddressMode::repeat};
    AddressMode addressModeV{AddressMode::repeat};
    AddressMode addressModeW{AddressMode::repeat};
    float       mipLodBias{0.0f};
    bool        anisotropyEnable{false};
    float       maxAnisotropy{1.0f};
    // ... 其他字段
};
```

**注意**：`TextureUsageFlags` 和 `BufferUsageFlags` 需要新建为 bit-flag 类型，但不 bit-transparent 对应 Vulkan（按 `future-rhi-design-review.md` 的建议，用语义 flags）。

#### File: `rhi/RHIArgumentTable.h` （新建）

```cpp
#pragma once
#include "RHIHandles.h"
#include "RHITypes.h"
#include "RHIStageBarrier.h"

namespace demo::rhi {

enum class ArgumentType : uint8_t {
    uniformBuffer,
    storageBuffer,
    sampledTexture,
    storageTexture,
    sampler,
    accelerationStructure,
};

struct ArgumentBinding {
    uint32_t     binding{0};
    ArgumentType type{ArgumentType::uniformBuffer};
    ShaderStage  visibility{ShaderStage::none};
    uint32_t     arrayCount{1};
    bool         bindless{false};
};

struct ArgumentLayoutDesc {
    const ArgumentBinding* bindings{nullptr};
    uint32_t               bindingCount{0};
};

struct ArgumentWrite {
    uint32_t          binding{0};
    uint32_t          arrayElement{0};
    ArgumentType      type{ArgumentType::uniformBuffer};
    BufferHandle      buffer{};
    TextureViewHandle textureView{};
    SamplerHandle     sampler{};
    uint64_t          offset{0};
    uint64_t          size{0};  // 0 = entire buffer
};

class ArgumentLayout {
public:
    virtual ~ArgumentLayout() = default;
    virtual void init(void* nativeDevice, const ArgumentLayoutDesc& desc) = 0;
    virtual void deinit() = 0;
};

class ArgumentTable {
public:
    virtual ~ArgumentTable() = default;
    virtual void init(void* nativeDevice, ArgumentLayoutHandle layout, uint32_t maxEntries) = 0;
    virtual void deinit() = 0;
    virtual void update(uint32_t writeCount, const ArgumentWrite* writes) = 0;
};

} // namespace demo::rhi
```

**注意**：这里保留了 `void* nativeDevice` 作为过渡，后续 Wave 中会移除。

#### File: `rhi/RHIEncoder.h` （新建）

```cpp
#pragma once
#include "RHIHandles.h"
#include "RHITypes.h"
#include "RHIStageBarrier.h"

namespace demo::rhi {

struct DrawIndexedDesc {
    BufferHandle indexBuffer{};
    uint64_t     indexBufferOffset{0};
    IndexFormat  indexFormat{IndexFormat::uint32};
    uint32_t     indexCount{0};
    uint32_t     instanceCount{1};
    uint32_t     firstIndex{0};
    int32_t      vertexOffset{0};
    uint32_t     firstInstance{0};
};

struct DrawIndirectDesc {
    BufferHandle argsBuffer{};
    uint64_t     offset{0};
    uint32_t     drawCount{1};
    uint32_t     stride{0};  // 0 = backend default
};

struct DrawIndirectCountDesc {
    BufferHandle argsBuffer{};
    uint64_t     argsOffset{0};
    BufferHandle countBuffer{};
    uint64_t     countBufferOffset{0};
    uint32_t     maxDrawCount{0};
    uint32_t     stride{0};
};

struct DispatchDesc {
    uint32_t groupCountX{1};
    uint32_t groupCountY{1};
    uint32_t groupCountZ{1};
};

struct DispatchIndirectDesc {
    BufferHandle argsBuffer{};
    uint64_t     offset{0};
};

class RenderEncoder {
public:
    virtual ~RenderEncoder() = default;

    virtual void setPipeline(PipelineHandle pipeline) = 0;
    virtual void setArgumentTable(uint32_t slot, ArgumentTableHandle table) = 0;
    virtual void setDynamicBuffer(uint32_t slot, BufferHandle buffer, uint64_t offset, uint64_t size) = 0;
    virtual void setRootConstants(uint32_t slot, const void* data, uint32_t size) = 0;
    virtual void setRootPointer(uint32_t slot, GpuPtr ptr) = 0;

    virtual void setViewport(const Viewport& viewport) = 0;
    virtual void setScissor(const Rect2D& scissor) = 0;

    virtual void bindVertexBuffers(uint32_t firstBinding, const BufferHandle* buffers,
                                   const uint64_t* offsets, uint32_t count) = 0;
    virtual void bindIndexBuffer(BufferHandle buffer, uint64_t offset, IndexFormat format) = 0;

    virtual void drawIndexed(const DrawIndexedDesc& desc) = 0;
    virtual void drawIndexedIndirect(const DrawIndirectDesc& desc) = 0;
    virtual void drawIndexedIndirectCount(const DrawIndirectCountDesc& desc) = 0;
    virtual void drawIndirect(const DrawIndirectDesc& desc) = 0;  // non-indexed

    // Mesh shader (optional, capability-gated)
    virtual void drawMeshTasks(uint32_t groupCountX, uint32_t groupCountY, uint32_t groupCountZ) = 0;
    virtual void drawMeshTasksIndirect(const DrawIndirectDesc& desc) = 0;
};

class ComputeEncoder {
public:
    virtual ~ComputeEncoder() = default;

    virtual void setPipeline(PipelineHandle pipeline) = 0;
    virtual void setArgumentTable(uint32_t slot, ArgumentTableHandle table) = 0;
    virtual void setRootConstants(uint32_t slot, const void* data, uint32_t size) = 0;
    virtual void setRootPointer(uint32_t slot, GpuPtr ptr) = 0;

    virtual void dispatch(const DispatchDesc& desc) = 0;
    virtual void dispatchIndirect(const DispatchIndirectDesc& desc) = 0;
};

class CopyEncoder {
public:
    virtual ~CopyEncoder() = default;

    virtual void copyBuffer(BufferHandle src, BufferHandle dst,
                            uint64_t srcOffset, uint64_t dstOffset, uint64_t size) = 0;
    virtual void copyBufferToTexture(const BufferTextureCopyDesc& desc) = 0;
    virtual void copyTextureToBuffer(const TextureBufferCopyDesc& desc) = 0;
    virtual void blitTexture(const TextureBlitDesc& desc) = 0;
    virtual void fillBuffer(BufferHandle buffer, uint64_t offset, uint64_t size, uint32_t data) = 0;
};

} // namespace demo::rhi
```

#### File: `rhi/RHICommandBuffer.h` （新建）

```cpp
#pragma once
#include "RHIEncoder.h"
#include "RHIStageBarrier.h"

namespace demo::rhi {

class CommandBuffer {
public:
    virtual ~CommandBuffer() = default;

    virtual void begin() = 0;
    virtual void end() = 0;

    // Encoder factory
    virtual RenderEncoder* beginRenderPass(const RenderPassDesc& desc) = 0;
    virtual ComputeEncoder* beginComputePass() = 0;
    virtual CopyEncoder* beginCopyPass() = 0;
    virtual void endEncoding() = 0;  // End current encoder

    // Cross-encoder / pre-encoder barrier
    virtual void barrier(StageFlags producer, StageFlags consumer, HazardFlags hazards) = 0;

    // Debug markers
    virtual void beginEvent(const char* name) = 0;
    virtual void endEvent() = 0;
};

} // namespace demo::rhi
```

#### File: `rhi/RHIDevice.h` （扩展）

在现有 `Device` 类上新增方法（不删除旧方法）：

```cpp
class Device {
public:
    // ... existing methods ...

    // --- Buffer ---
    virtual BufferHandle createBuffer(const BufferDesc& desc) = 0;
    virtual void destroyBuffer(BufferHandle handle) = 0;
    virtual GpuPtr getBufferGpuAddress(BufferHandle handle) const = 0;
    virtual void* mapBuffer(BufferHandle handle) = 0;  // For CPU-visible buffers
    virtual void unmapBuffer(BufferHandle handle) = 0;

    // --- Sampler ---
    virtual SamplerHandle createSampler(const SamplerDesc& desc) = 0;
    virtual void destroySampler(SamplerHandle handle) = 0;

    // --- Argument Table ---
    virtual ArgumentLayoutHandle createArgumentLayout(const ArgumentLayoutDesc& desc) = 0;
    virtual void destroyArgumentLayout(ArgumentLayoutHandle handle) = 0;
    virtual ArgumentTableHandle createArgumentTable(ArgumentLayoutHandle layout, uint32_t maxEntries) = 0;
    virtual void destroyArgumentTable(ArgumentTableHandle handle) = 0;
    virtual void updateArgumentTable(ArgumentTableHandle table,
                                     uint32_t writeCount, const ArgumentWrite* writes) = 0;

    // --- CommandBuffer ---
    virtual CommandBuffer* createCommandBuffer() = 0;
    virtual void destroyCommandBuffer(CommandBuffer* cmdBuffer) = 0;

    // --- QueryPool ---
    virtual QueryPoolHandle createQueryPool(uint32_t queryCount) = 0;
    virtual void destroyQueryPool(QueryPoolHandle handle) = 0;
    virtual uint64_t getQueryPoolResult(QueryPoolHandle handle, uint32_t queryIndex) = 0;

    // --- ResidencySet (optional, capability-gated) ---
    virtual ResidencySetHandle createResidencySet(const ResidencySetDesc& desc) = 0;
    virtual void destroyResidencySet(ResidencySetHandle handle) = 0;
    virtual void addToResidencySet(ResidencySetHandle set, ResourceHandle resource) = 0;
};
```

**注意**：现有 `createImage` / `createTextureView` 等方法保留，但新增语义化版本。

### 3.2 编译验证

- 新建的头文件编译通过
- 现有 `rhi/vulkan/*.cpp` 因新增纯虚方法而**编译失败**（这是预期行为，提醒需要实现）
- 业务层代码不受影响（因为只新增了文件，没改现有接口）

**解决方案**：为新增方法提供默认空实现或 `assert(false)` stub，让 Vulkan 后端在 Wave 1 中逐步填充。

---

## 4. Wave 1：Vulkan 后端新接口实现 + Buffer/Texture 管理重构

**目标**：让 Vulkan 后端实现 Wave 0 的新接口，同时引入 `ResourcePool<Hot, Cold>` 和 `GpuPtr` 支持。

**验证标准**：Vulkan 后端编译通过，至少一个简单测试（如创建 buffer 并获取 address）通过。

### 4.1 文件清单与修改内容

#### File: `rhi/vulkan/VulkanDevice.h/cpp`

**修改**：
1. 继承 `rhi::Device` 并声明新增纯虚方法
2. 实现 `createBuffer` / `destroyBuffer` / `getBufferGpuAddress`
3. 实现 `createSampler` / `destroySampler`
4. 实现 `createCommandBuffer` / `destroyCommandBuffer`
5. 实现 `createArgumentLayout` / `createArgumentTable` / `updateArgumentTable`
6. 实现 `createQueryPool` / `destroyQueryPool`

**Buffer 实现细节**：
```cpp
// VulkanDevice.cpp
BufferHandle VulkanDevice::createBuffer(const BufferDesc& desc) {
    VkBufferCreateInfo bufferInfo{...};
    bufferInfo.size = desc.size;
    bufferInfo.usage = ToVkBufferUsageFlags(desc.usage);
    if (desc.allowGpuAddress) bufferInfo.usage |= VK_BUFFER_USAGE_SHADER_DEVICE_ADDRESS_BIT;
    if (desc.allowIndirectArgument) bufferInfo.usage |= VK_BUFFER_USAGE_INDIRECT_BUFFER_BIT;

    VmaAllocationCreateInfo allocInfo{...};
    // 根据 desc.memoryUsage 选择 VMA_MEMORY_USAGE_GPU_ONLY / CPU_TO_GPU 等

    VkBuffer buffer;
    VmaAllocation allocation;
    vmaCreateBuffer(m_allocator, &bufferInfo, &allocInfo, &buffer, &allocation, nullptr);

    GpuPtr gpuAddress = {0};
    if (desc.allowGpuAddress) {
        VkBufferDeviceAddressInfo addrInfo{.sType = VK_STRUCTURE_TYPE_BUFFER_DEVICE_ADDRESS_INFO, .buffer = buffer};
        gpuAddress.value = vkGetBufferDeviceAddress(m_device, &addrInfo);
    }

    return m_buffers.create(buffer, allocation, gpuAddress, desc);
}
```

#### File: `rhi/vulkan/VulkanCommandBuffer.h/cpp` （新建）

实现 `rhi::CommandBuffer` 接口：
- `beginRenderPass` → 创建 `VulkanRenderEncoder`
- `beginComputePass` → 创建 `VulkanComputeEncoder`
- `beginCopyPass` → 创建 `VulkanCopyEncoder`
- `barrier` → `vkCmdPipelineBarrier2`（通过 `VkDependencyInfo` + `VkMemoryBarrier2` / `VkBufferMemoryBarrier2` / `VkImageMemoryBarrier2`）

**关键设计**：内部维护当前活跃的 `VkCommandBuffer` 和当前 encoder 类型。`endEncoding()` 根据当前 encoder 类型调用 `vkCmdEndRendering` 或什么都不做（compute/copy encoder 在 Vulkan 下没有显式 end，只有 command buffer end）。

#### File: `rhi/vulkan/VulkanRenderEncoder.h/cpp` （新建）

实现 `rhi::RenderEncoder`：
- `setPipeline` → `vkCmdBindPipeline`
- `setArgumentTable` → `vkCmdBindDescriptorSets`（如果 ArgumentTable 映射到 DescriptorSet）
- `setDynamicBuffer` → `vkCmdBindDescriptorSets` with dynamic offset
- `setRootConstants` → `vkCmdPushConstants`
- `setRootPointer` → **在 Vulkan 下需要特殊处理**。Root Pointer（GpuPtr）在 Vulkan 下通常是 `uint64_t` push constant 或 buffer device address in shader。可以映射为 `vkCmdPushConstants` 推送一个 `uint64_t` 地址。
- `drawIndexed` → `vkCmdDrawIndexed`
- `drawIndexedIndirect` → `vkCmdDrawIndexedIndirect`
- `drawIndexedIndirectCount` → `vkCmdDrawIndexedIndirectCount`
- `drawIndirect` → `vkCmdDrawIndirect`（新增）
- `drawMeshTasks` → `vkCmdDrawMeshTasksEXT`（如果支持）

#### File: `rhi/vulkan/VulkanComputeEncoder.h/cpp` （新建）

实现 `rhi::ComputeEncoder`：
- `dispatch` → `vkCmdDispatch`
- `dispatchIndirect` → `vkCmdDispatchIndirect`（新增）

#### File: `rhi/vulkan/VulkanCopyEncoder.h/cpp` （新建）

实现 `rhi::CopyEncoder`：
- `copyBuffer` → `vkCmdCopyBuffer`
- `copyBufferToTexture` → `vkCmdCopyBufferToImage`
- `blitTexture` → `vkCmdBlitImage`
- `fillBuffer` → `vkCmdFillBuffer`

#### File: `rhi/vulkan/VulkanArgumentTable.h/cpp` （新建）

将现有 `VulkanDescriptor.cpp/h` 的概念迁移为 `VulkanArgumentTable`：
- `VulkanArgumentLayout` 封装 `VkDescriptorSetLayout`
- `VulkanArgumentTable` 封装 `VkDescriptorSet`
- `updateArgumentTable` 映射为 `vkUpdateDescriptorSets`
- 内部从 `ArgumentWrite`（RHI Handle）解析为 `VkWriteDescriptorSet`

**注意**：这里需要一个 handle → native 的解析路径（`TextureHandle` → `VkImageView`，`BufferHandle` → `VkBuffer`）。这要求 `VulkanDevice` 或 `VulkanResourceTable` 提供 O(1) 的 handle resolve。

#### File: `rhi/vulkan/VulkanResourceTable.h/cpp` （修改）

**关键修改**：
1. 新增 `HandlePool<BufferHandle, BufferRecord>` 用于 buffer 管理
2. 新增 `HandlePool<TextureHandle, TextureRecord>` 已有的扩展
3. **移除 `m_bindGroupTables` 的 `std::unordered_map`**，改为 `HandlePool<BindGroupHandle, BindTable*>` 或等待 Wave 3 被 ArgumentTable 替代
4. 所有 `resolve*` 方法保持 O(1) 数组索引

```cpp
// VulkanResourceTable.h
class VulkanResourceTable {
    // ... existing pools ...
    HandlePool<BufferHandle, BufferRecord> m_buffers;  // NEW
    // BindGroup resolve: 改为数组索引
    std::vector<BindTable*> m_bindGroupTables;  // index-based, size = max handle index + 1
};
```

### 4.2 编译验证

- `rhi/vulkan/*.cpp` 全部编译通过
- `rhi/d3d12/*.cpp` 和 `rhi/metal/*.cpp` 因新增纯虚方法而编译失败 → **为它们提供 stub 实现**（`assert(false)`），保持项目可编译
- 运行简单验证：创建 buffer → 获取 GpuPtr → 销毁 buffer

---

## 5. Wave 2：Common 层拆分 + utils::Buffer 替换

**目标**：将 `common/Common.h` 中的 Vulkan 类型下沉，用 `BufferHandle` 替换 `utils::Buffer`。

**验证标准**：`render/` 下的文件不再直接 include Vulkan 类型，`BufferHandle` 可在 upload 路径中使用。

### 5.1 文件清单

#### File: `common/VulkanTypes.h` （新建）

从 `common/Common.h` 中提取：
```cpp
#pragma once
#include <vulkan/vulkan.h>
#include "vk_mem_alloc.h"

namespace utils {
struct Buffer { VkBuffer buffer; VmaAllocation allocation; VkDeviceAddress address; void* mapped; };
struct Image { VkImage image; VmaAllocation allocation; };
struct ImageResource : Image { VkImageView view; VkExtent2D extent; VkImageLayout layout; };
struct QueueInfo { uint32_t familyIndex; uint32_t queueIndex; VkQueue queue; };
struct AccelerationStructure { VkAccelerationStructureKHR accel; VmaAllocation allocation; VkDeviceAddress deviceAddress; VkDeviceSize size; Buffer buffer; };
} // namespace utils
```

#### File: `common/VulkanHelpers.h` （新建）

从 `common/Common.h` 中提取所有 Vulkan helper 函数：
- `cmdInitImageLayout`
- `cmdTransitionSwapchainLayout`
- `cmdBufferMemoryBarrier`
- `findSupportedFormat`
- `findDepthFormat`
- `createShaderModule`
- `beginSingleTimeCommands`
- `endSingleTimeCommands`
- `pNextChainPushFront`

#### File: `common/VulkanContextConfig.h` （新建）

从 `common/Common.h` 中提取：
- `ExtensionConfig`
- `ContextCreateInfo`
- `ValidationSettings`

#### File: `common/Common.h` （净化）

**删除所有 Vulkan-specific 内容**，只保留：
- `volk.h` → 移动到 `gfx/Context.h` 或 `rhi/vulkan/VulkanDevice.cpp`
- `vk_mem_alloc.h` → 移动到 `common/VulkanTypes.h`
- `Vertex::getBindingDescription()` → 改为返回 `rhi::VertexBindingDesc`
- `Vertex::getAttributeDescriptions()` → 改为返回 `rhi::VertexAttributeDesc`
- `packNormalRGB10A2` / `unpackNormalRGB10A2`
- `findFile`
- `GLM` includes
- `shaderio` namespace
- `ASSERT` macro
- `hashCombine`

#### File: `render/UploadUtils.h/cpp` （重写）

**当前**：
```cpp
utils::Buffer createStaticBufferWithUpload(VkDevice device, VmaAllocator allocator, VkCommandBuffer cmd, ...);
```

**目标**：
```cpp
BufferHandle createStaticBufferWithUpload(rhi::Device& device, rhi::CommandBuffer& cmd,
                                          std::span<const std::byte> data,
                                          rhi::BufferUsageFlags usage);
```

**实现方式**：使用 `device.createBuffer()` + `cmd->copyBuffer()`（通过 `CopyEncoder`）。

#### File: `render/GPUMeshletBuffer.h/cpp` （重写）

**当前**：
```cpp
class GPUMeshletBuffer {
    VkDevice m_device;
    VmaAllocator m_allocator;
    utils::Buffer m_meshletDataBuffer;
    VkBuffer getMeshletDataBuffer() const;
    uint64_t getMeshletDataAddress() const;  // VkDeviceAddress
};
```

**目标**：
```cpp
class GPUMeshletBuffer {
    rhi::Device* m_device{nullptr};
    rhi::BufferHandle m_meshletDataBuffer;
    rhi::BufferHandle m_meshletCullObjectBuffer;
    rhi::BufferHandle m_meshletIndexBuffer;
    rhi::BufferHandle getMeshletDataBuffer() const { return m_meshletDataBuffer; }
    rhi::GpuPtr getMeshletDataAddress() const { return m_device->getBufferGpuAddress(m_meshletDataBuffer); }
};
```

### 5.2 编译验证

- `render/UploadUtils.cpp` 编译通过
- `render/GPUMeshletBuffer.cpp` 编译通过
- 运行测试：上传 meshlet 数据 → 获取 GpuPtr → 验证

---

## 6. Wave 3：试点 Pass 迁移（Compute → Encoder 模型）

**目标**：选择 1-2 个纯 Compute Pass 完全迁移到新接口，验证 Encoder 模型在业务层的可行性。

**验证标准**：`GPUDrivenCullingPass` 和 `GPUDrivenDepthPyramidPass` 使用新接口后，渲染结果正确。

### 6.1 修改的文件

#### File: `render/Pass.h` （修改）

**当前**：
```cpp
struct PassContext {
    rhi::CommandList* cmd{nullptr};
    // ...
};
```

**目标（兼容过渡）**：
```cpp
struct PassContext {
    rhi::CommandList* cmd{nullptr};         // OLD，保留兼容
    rhi::CommandBuffer* cmdBuffer{nullptr}; // NEW
    // ...
};
```

`PassExecutor::execute()` 需要同时设置 `cmd` 和 `cmdBuffer`。

#### File: `render/PassExecutor.cpp` （修改）

在 `execute()` 中，创建 `CommandBuffer` 并赋值给 `PassContext::cmdBuffer`。

#### File: `render/passes/GPUDrivenCullingPass.h/cpp` （重写）

**当前 `execute()`**：
```cpp
void GPUDrivenCullingPass::execute(const PassContext& context) const {
    context.cmd->beginEvent("GPUDrivenCulling");
    // ... 获取 pipeline layout, bind group, buffer handles ...
    context.cmd->bindPipeline(rhi::PipelineBindPoint::compute, pipeline);
    context.cmd->bindBindGroup(...);
    context.cmd->dispatch((objectCount + 255) / 256, 1, 1);
    context.cmd->endEvent();
}
```

**目标 `execute()`**：
```cpp
void GPUDrivenCullingPass::execute(const PassContext& context) const {
    auto* cmdBuffer = context.cmdBuffer;
    cmdBuffer->beginEvent("GPUDrivenCulling");

    // Barrier: ensure depth pyramid is ready
    cmdBuffer->barrier(
        rhi::StageFlags::compute,   // DepthPyramidPass producer
        rhi::StageFlags::compute,   // This pass consumer
        rhi::HazardFlags::textureWrites
    );

    auto* enc = cmdBuffer->beginComputePass();
    enc->setPipeline(m_renderer->getGPUCullingPipelineHandle());
    enc->setArgumentTable(0, m_renderer->getGPUCullingBindGroup(context.frameIndex));
    // Root constants for push constants
    enc->setRootConstants(0, &pushConsts, sizeof(pushConsts));
    enc->dispatch((objectCount + 255) / 256, 1, 1);
    cmdBuffer->endEncoding();

    // Barrier: indirect args produced, consumed by subsequent graphics passes
    cmdBuffer->barrier(
        rhi::StageFlags::compute,
        rhi::StageFlags::commandInput,
        rhi::HazardFlags::drawArguments | rhi::HazardFlags::bufferWrites
    );

    cmdBuffer->endEvent();
}
```

**注意**：`GPUDrivenCullingPass` 当前 `#include "rhi/vulkan/VulkanCommandList.h"` 和 `m_renderer->getGPUCullingPipelineLayout()` / `m_renderer->getGPUCullingIndirectBufferOpaque()` / `getNativeComputePipeline()` 等 Native escape hatch。这些必须全部移除。

#### File: `render/passes/GPUDrivenDepthPyramidPass.h/cpp` （重写）

类似 `GPUDrivenCullingPass`，但更简单（它委托给 `m_renderer->executeDepthPyramidPass(*context.cmd, *context.params)`）。

需要修改 `RenderDevice::executeDepthPyramidPass` 的签名：
```cpp
// 从
void executeDepthPyramidPass(rhi::CommandList& cmd, const RenderParams& params);
// 改为
void executeDepthPyramidPass(rhi::CommandBuffer& cmdBuffer, const RenderParams& params);
```

### 6.2 编译验证

- `GPUDrivenCullingPass` 和 `GPUDrivenDepthPyramidPass` 编译通过
- 运行程序，验证 GPU Culling 结果正确（检查 `getLastGPUCullingStats`）
- 如果失败，回退到旧 `CommandList` 路径进行对比调试

---

## 7. Wave 4：所有 Compute Passes 迁移

**目标**：将所有纯 Compute Pass 迁移到 `ComputeEncoder`。

**Pass 列表**：
1. `GPUDrivenLightCullingPass`
2. `GPUDrivenClusteredLightCullingPass`
3. `GPUDrivenVisibilitySortPass`
4. `GPUDrivenAOPass`
5. `GPUDrivenSSRPass`
6. `GPUDrivenBloomPrefilterPass`
7. `GPUDrivenBloomDownsamplePass`
8. `GPUDrivenTAAResolvePass`

### 7.1 每个 Pass 的通用替换步骤

对于每个 Compute Pass：

1. **移除 `#include "rhi/vulkan/VulkanCommandList.h"`**
2. **修改 `execute()` 签名内部实现**：
   - `context.cmd->beginEvent(...)` → `context.cmdBuffer->beginEvent(...)`
   - `context.cmd->bindPipeline(rhi::PipelineBindPoint::compute, ...)` → `enc->setPipeline(...)`
   - `context.cmd->bindBindGroup(...)` → `enc->setArgumentTable(...)`
   - `context.cmd->pushConstants(...)` → `enc->setRootConstants(...)` 或 `enc->setRootPointer(...)`
   - `context.cmd->dispatch(...)` → `enc->dispatch(...)`
   - `context.cmd->endEvent()` → `context.cmdBuffer->endEvent()`
3. **在 `execute()` 开头和结尾添加 `barrier()`**：
   - 开头：等待前一个 producer（通常是 graphics pass 的 `rasterDepthOut` 或前一个 compute pass 的 `compute`）
   - 结尾：通知后续 consumer（通常是 graphics pass 的 `commandInput` 或 `fragmentShader`）
4. **移除所有 `VkPipelineLayout` / `VkDescriptorSet` / `uint64_t` Native accessor 的调用**：
   - `m_renderer->getXXXPipelineLayout()` → 不再需要（`setRootConstants` 不需要 layout）
   - `m_renderer->getXXXDescriptorSetOpaque()` → 改用 `ArgumentTableHandle`
   - `reinterpret_cast<VkPipeline>(...)` → 直接使用 `PipelineHandle`

### 7.2 编译验证

- 所有 Compute Pass 编译通过
- 运行程序，验证 Compute-based effects（AO, SSR, Bloom, TAA）渲染正确

---

## 8. Wave 5：Render Passes 迁移

**目标**：将所有 Render Pass 迁移到 `RenderEncoder`。

**Pass 列表**（按复杂度从低到高）：

#### Tier 1（简单，fullscreen quad / simple draw）
- `GPUDrivenSkyboxPass`
- `GPUDrivenSkyPass`
- `GPUDrivenDebugPass`
- `GPUDrivenVelocityPass`

#### Tier 2（中等，standard geometry + MRT/depth）
- `GPUDrivenDepthPrepass`
- `GPUDrivenForwardPass`
- `GPUDrivenLightPass`
- `GPUDrivenCSMShadowPass`
- `GPUDrivenShadowAtlasPass`
- `GPUDrivenFinalColorPass`

#### Tier 3（复杂，MDI / multi-layer）
- `GPUDrivenGBufferPass`（最复杂，MRT + MDI + alpha test）

### 8.1 每个 Render Pass 的通用替换步骤

对于每个 Render Pass：

1. **移除 `#include "rhi/vulkan/VulkanCommandList.h"`**
2. **修改 `execute()`**：
   ```cpp
   void XXXPass::execute(const PassContext& context) const {
       auto* cmdBuffer = context.cmdBuffer;
       cmdBuffer->beginEvent("XXXPass");

       // Barrier: wait for previous passes
       cmdBuffer->barrier(producerStage, consumerStage, hazardFlags);

       // Begin render pass
       auto* enc = cmdBuffer->beginRenderPass({
           .colorTargets = colorTargets,
           .colorTargetCount = N,
           .depthTarget = &depthTarget,
       });

       enc->setPipeline(pipelineHandle);
       enc->setArgumentTable(0, globalTable);
       enc->setArgumentTable(1, materialTable);
       enc->setDynamicBuffer(2, drawDataBuffer, dynamicOffset, drawDataSize);

       // For MDI passes
       enc->drawIndexedIndirect(indirectBuffer, offset, drawCount, stride);

       // Or for direct draw
       enc->drawIndexed({.indexBuffer = ib, .indexCount = count, ...});

       cmdBuffer->endEncoding();  // ends render pass
       cmdBuffer->endEvent();
   }
   ```
3. **替换 `VkImage` / `VkImageView` accessor**：
   - `m_renderer->getSceneDepthImage()` → 使用 `rhi::TextureHandle` 或 `rhi::TextureViewHandle`
   - `m_renderer->getCurrentSwapchainImageView()` → 使用 `swapchain->currentTextureView()`
4. **替换 `VkExtent2D`**：
   - `m_renderer->getSceneExtent()` → `rhi::Extent2D`
5. **替换 `VkFormat`**：
   - `m_renderer->getSceneDepthFormat()` → `rhi::TextureFormat`

### 8.2 GBuffer Pass 的特殊处理

`GPUDrivenGBufferPass` 是当前最复杂的 pass：
- 使用 MRT（3 个 color target + 1 depth target）
- 有 MDI（Multi-Draw Indirect）路径
- 有 Alpha Test 分支
- 当前直接操作 `VkImageView` 和 `VkPipelineLayout`

**迁移策略**：
1. 先确保 `RenderEncoder::drawIndexedIndirect` 和 `drawIndexedIndirectCount` 可用
2. 将 MRT setup 从 Native `VkImageView` 改为 `rhi::TextureViewHandle` 数组
3. 将 MDI buffer 从 `uint64_t` 改为 `BufferHandle`
4. 分阶段：先迁移 non-MDI 路径，再迁移 MDI 路径

### 8.3 编译验证

- 所有 Render Pass 编译通过
- 运行完整渲染管线，对比帧捕获（RenderDoc）验证输出一致

---

## 9. Wave 6：Present Pass + Swapchain 迁移

**目标**：迁移 `GPUDrivenPresentPass` 和 Swapchain 相关代码。

### 9.1 文件清单

#### File: `render/passes/GPUDrivenPresentPass.cpp`

**当前**：使用 `VkImage` + `blitImage` + `transitionTexture` + `VkExtent2D`

**目标**：
```cpp
void GPUDrivenPresentPass::execute(const PassContext& context) const {
    auto* cmdBuffer = context.cmdBuffer;
    cmdBuffer->beginEvent("GPUDrivenPresent");

    // Barrier: light pass / final color wrote to output texture
    cmdBuffer->barrier(
        rhi::StageFlags::rasterColorOut,
        rhi::StageFlags::transfer,
        rhi::HazardFlags::textureWrites
    );

    auto* enc = cmdBuffer->beginCopyPass();
    enc->blitTexture({
        .srcTexture = m_renderer->getOutputTextureHandle(),
        .dstTexture = m_renderer->getCurrentSwapchainTextureHandle(),
        .srcState = rhi::ResourceState::TransferSrc,
        .dstState = rhi::ResourceState::TransferDst,
        // ... offsets based on aspect ratio letterboxing
    });
    cmdBuffer->endEncoding();

    // Barrier: blit wrote to swapchain, prepare for present
    cmdBuffer->barrier(
        rhi::StageFlags::transfer,
        rhi::StageFlags::rasterColorOut,  // Present engine consumer
        rhi::HazardFlags::textureWrites
    );

    cmdBuffer->endEvent();
}
```

**注意**：如果 Present 不是 blit 而是 fullscreen quad（如需要做 tone mapping in present pass），则它应该是 `RenderEncoder` 而不是 `CopyEncoder`。当前代码使用 `blitImage`，所以是 `CopyEncoder`。

#### File: `rhi/RHISwapchain.h` / `rhi/vulkan/VulkanSwapchain.cpp`

**修改**：
- 移除 `getNativeSwapchain()` / `getNativeImageView()` / `getNativeImage()`
- `currentTexture()` 返回 `TextureHandle`
- 新增 `currentTextureView()` 返回 `TextureViewHandle`

### 9.2 编译验证

- Swapchain acquire + present 工作正常
- 屏幕输出正确，无撕裂或布局错误

---

## 10. Wave 7：ArgumentTable 全面替换 BindGroup/BindTable

**目标**：将业务层和 RenderDevice 中的 `BindGroup` / `BindTable` 概念替换为 `ArgumentLayout` / `ArgumentTable`。

### 10.1 文件清单

#### File: `render/BindGroups.h` （重写或新建 `render/ArgumentTables.h`）

将 `BindGroupDesc` / `BindGroupResource` 迁移为使用 `ArgumentTableHandle` / `ArgumentLayoutHandle`。

#### File: `render/RenderDevice.h/cpp` （大规模修改）

**当前内部**：大量 `VkDescriptorSetLayout` / `VkDescriptorSet` / `VkPipelineLayout`

**目标**：
- `DeviceLifetimeResources` 中的 `VkDescriptorSetLayout` → `ArgumentLayoutHandle`
- `VkDescriptorSet` → `ArgumentTableHandle`
- `VkPipelineLayout` → 完全由 Vulkan 后端内部管理，不暴露给业务层

**具体修改**：
1. `getLightPipelineLayout()` / `getGraphicsPipelineLayout()` 等返回 `PipelineLayoutHandle` 的方法 → **删除**
2. `getGBufferColorDescriptorSet()` / `getLightingInputDescriptorSet()` 等返回 `uint64_t`（VkDescriptorSet）的方法 → 改为返回 `ArgumentTableHandle`
3. `registerExternalGraphicsPipeline(VkPipeline, VkPipelineLayout)` → 改为 `registerExternalGraphicsPipeline(PipelineHandle)` 或完全移除（如果 pipeline 已由 RHI 创建）
4. `createBindGroupLayout` / `createBindGroup` → 改为 `createArgumentLayout` / `createArgumentTable`
5. `SamplerCache`（内部 `VkSampler` map）→ 使用 `rhi::Device::createSampler`

### 10.2 编译验证

- 所有 descriptor / bind group 创建路径编译通过
- Material binding、per-frame dynamic buffer binding 工作正常

---

## 11. Wave 8：StageBarrier 全面替换 ResourceBarrier

**目标**：移除 `transitionTexture` / `transitionBuffer` / `memoryBarrier` 的显式调用，改用 `barrier(producer, consumer, hazard)`。

### 11.1 文件清单

#### File: `rhi/RHICommandList.h` （标记 deprecated）

将 `transitionTexture` / `transitionBuffer` / `memoryBarrier` / `setResourceState` 标记为 `[[deprecated]]`。

#### File: `rhi/vulkan/VulkanCommandList.cpp`

实现 `barrier(StageFlags, StageFlags, HazardFlags)`：

```cpp
void VulkanCommandBuffer::barrier(StageFlags producer, StageFlags consumer, HazardFlags hazards) {
    VkMemoryBarrier2 memBarrier{.sType = VK_STRUCTURE_TYPE_MEMORY_BARRIER_2};
    memBarrier.srcStageMask = ToVkPipelineStageFlags2(producer);
    memBarrier.dstStageMask = ToVkPipelineStageFlags2(consumer);
    memBarrier.srcAccessMask = InferAccessFlags(producer, hazards, true);
    memBarrier.dstAccessMask = InferAccessFlags(consumer, hazards, false);

    VkDependencyInfo depInfo{.sType = VK_STRUCTURE_TYPE_DEPENDENCY_INFO};
    depInfo.memoryBarrierCount = 1;
    depInfo.pMemoryMemoryBarriers = &memBarrier;
    vkCmdPipelineBarrier2(m_commandBuffer, &depInfo);
}
```

**注意**：对于 image layout transition（如 swapchain image `TransferDst` → `Present`），`barrier()` 可能需要额外的信息。可以：
- 方案 A：`barrier()` 只处理 execution/memory barrier，image layout 由后端自动推断（基于 resource tracking）
- 方案 B：保留 `transitionTexture` 作为低频专用接口，仅用于 layout transition

考虑到 Metal 不需要显式 layout transition，**方案 A 更好**：Vulkan 后端内部维护一个轻量的 resource state cache（per-command-buffer），在 `beginRenderPass` / `blitTexture` / `present` 时自动插入必要的 layout transition。

#### File: `render/passes/*.cpp`

将所有 `context.cmd->transitionTexture(...)` 替换为 `context.cmdBuffer->barrier(...)`。

**特别注意 `GPUDrivenPresentPass`**：
- 当前调用 `context.cmd->transitionTexture(...)` 两次（output texture → transfer src, swapchain → transfer dst）
- 改为 `barrier(StageFlags::rasterColorOut, StageFlags::transfer, HazardFlags::textureWrites)`
- Vulkan 后端在 `blitTexture` 内部自动处理 `VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL` / `TRANSFER_DST_OPTIMAL`

### 11.2 编译验证

- 所有 pass 编译通过
- 运行 RenderDoc 捕获，验证 barrier 数量和位置合理（没有多余的 barrier，也没有 missing barrier）

---

## 12. Wave 9：QueryPool / GPU Profiling + 清理旧接口

**目标**：引入 `QueryPoolHandle`，替换 `VkQueryPool`；删除所有旧 RHI 接口和兼容层。

### 12.1 文件清单

#### File: `render/RenderDevice.h` （移除旧成员）

删除：
- `VkQueryPool queryPool` → 改为 `rhi::QueryPoolHandle`
- `VkPipelineLayout lightPipelineLayout` 等 → 完全移除（由后端内部管理）
- `VkDescriptorSet gbufferTextureSets` 等 → 改为 `rhi::ArgumentTableHandle`
- `VkFence` / `VkCommandBuffer` / `VkCommandPool` → 完全由 `rhi::FrameContext` / `rhi::CommandBuffer` 管理

#### File: `rhi/RHICommandList.h` （删除）

当所有业务层迁移完成后，删除 `rhi::CommandList` 基类。

#### File: `rhi/vulkan/VulkanCommandList.h/cpp` （删除或重构）

- 删除 `getNativeCommandBuffer()` 和所有 `cmd*` 自由函数
- 如果 `VulkanCommandList` 仍作为 `CommandBuffer` 的内部实现保留，则将其重命名为 `VulkanCommandBuffer`

#### File: `rhi/RHIHandles.h` （清理）

- 移除 `TextureViewHandle::fromNativePtr()` / `toNativePtr()`

### 12.2 编译验证

- 项目编译通过，无任何 `[[deprecated]]` 警告
- 运行完整 benchmark，验证性能无回归

---

## 13. 风险与回滚策略

| 风险 | 影响 | 缓解措施 |
|------|------|---------|
| Encoder 模型在 Vulkan 下引入额外开销 | 中 | `VulkanCommandBuffer` 内部维护单个 `VkCommandBuffer`，encoder 切换无 native 开销（只是 C++ 虚函数或内联调用）。`beginRenderPass` 映射为 `vkCmdBeginRendering`，`endEncoding` 映射为 `vkCmdEndRendering` |
| `ArgumentTable` 替代 `BindTable` 导致 descriptor update 性能下降 | 中 | 确保 `updateArgumentTable` 内部批量调用 `vkUpdateDescriptorSets`，而非逐个 update。保持 descriptor set 的持久化（immutable persistent table） |
| `StageBarrier` 自动推断 image layout 出错 | 高 | 保留 debug-only 的 `transitionTexture` 路径作为 fallback。在开发阶段开启 Vulkan validation layer，捕获 layout mismatch |
| `BufferHandle` 替换 `utils::Buffer` 导致 upload 路径中断 | 高 | Wave 2 中先实现 `createBuffer` + `getBufferGpuAddress`，确保 `UploadUtils` 和 `GPUMeshletBuffer` 完全迁移后再继续 |
| MDI / Indirect Draw 路径在新接口下行为异常 | 高 | GBuffer Pass 最后迁移，迁移后使用 RenderDoc 对比 capture，确保 indirect command buffer 内容一致 |
| 编译时间剧增（大量文件同时修改） | 低 | 每 Wave 控制修改文件数量在 10-20 个以内，保持增量编译友好 |

**回滚策略**：
- 每 Wave 完成后提交一个 Git commit
- 如果某 Wave 验证失败，回退到上一个 Wave 的 commit
- 保留旧接口的兼容层直到 Wave 9，随时可以切回旧路径

---

## 14. 执行检查清单（Checklist）

### Wave 0 启动前
- [ ] 确认 Git 仓库干净，创建分支 `feature/modern-rhi`
- [ ] 确认 Vulkan SDK 版本 ≥ 1.3（支持 synchronization2, dynamic rendering, buffer device address）
- [ ] 确认 RenderDoc 可用（用于 barrier/layout 验证）

### Wave 0
- [ ] `rhi/RHIStageBarrier.h` 创建
- [ ] `rhi/RHIEncoder.h` 创建
- [ ] `rhi/RHICommandBuffer.h` 创建
- [ ] `rhi/RHIArgumentTable.h` 创建
- [ ] `rhi/RHIHandles.h` 扩展新 Handle 类型
- [ ] `rhi/RHITypes.h` 扩展 BufferDesc/TextureDesc/SamplerDesc/GpuPtr
- [ ] `rhi/RHIDevice.h` 扩展新纯虚方法
- [ ] **编译验证**：项目编译通过（为新增纯虚方法提供 `assert(false)` stub）

### Wave 1
- [ ] `rhi/vulkan/VulkanDevice.h/cpp` 实现 `createBuffer` / `getBufferGpuAddress`
- [ ] `rhi/vulkan/VulkanCommandBuffer.h/cpp` 新建
- [ ] `rhi/vulkan/VulkanRenderEncoder.h/cpp` 新建
- [ ] `rhi/vulkan/VulkanComputeEncoder.h/cpp` 新建
- [ ] `rhi/vulkan/VulkanCopyEncoder.h/cpp` 新建
- [ ] `rhi/vulkan/VulkanArgumentTable.h/cpp` 新建
- [ ] `rhi/vulkan/VulkanResourceTable.h/cpp` 修复 `m_bindGroupTables` 为 O(1) 数组
- [ ] **编译验证**：Vulkan 后端编译通过
- [ ] **功能验证**：创建 buffer → map → write → unmap → get GpuPtr → destroy

### Wave 2
- [ ] `common/VulkanTypes.h` 新建
- [ ] `common/VulkanHelpers.h` 新建
- [ ] `common/Common.h` 净化（移除 Vulkan types）
- [ ] `render/UploadUtils.h/cpp` 重写为 BufferHandle
- [ ] `render/GPUMeshletBuffer.h/cpp` 重写为 BufferHandle + GpuPtr
- [ ] **编译验证**：`render/` 编译通过
- [ ] **功能验证**：Meshlet upload → GPU address → shader access 正确

### Wave 3
- [ ] `render/Pass.h` 扩展 `PassContext` 增加 `cmdBuffer`
- [ ] `render/PassExecutor.cpp` 设置 `cmdBuffer`
- [ ] `render/passes/GPUDrivenCullingPass.cpp` 迁移为 ComputeEncoder
- [ ] `render/passes/GPUDrivenDepthPyramidPass.cpp` 迁移为 ComputeEncoder
- [ ] **编译验证**：项目编译通过
- [ ] **功能验证**：GPU Culling 统计正确

### Wave 4
- [ ] 迁移所有 Compute Pass（8 个）
- [ ] **编译验证**：项目编译通过
- [ ] **功能验证**：AO/SSR/Bloom/TAA 渲染正确

### Wave 5
- [ ] Tier 1 Render Pass 迁移（4 个简单 pass）
- [ ] Tier 2 Render Pass 迁移（6 个中等 pass）
- [ ] Tier 3 Render Pass 迁移（GBuffer Pass）
- [ ] **编译验证**：项目编译通过
- [ ] **功能验证**：完整渲染管线输出正确（RenderDoc 对比）

### Wave 6
- [ ] `GPUDrivenPresentPass.cpp` 迁移为 CopyEncoder
- [ ] `rhi/RHISwapchain.h` 移除 Native accessor
- [ ] **编译验证**：项目编译通过
- [ ] **功能验证**：Swapchain present 正确，无撕裂

### Wave 7
- [ ] `render/BindGroups.h` 迁移为 ArgumentTable
- [ ] `render/RenderDevice.h/cpp` 移除 VkDescriptorSet/VkPipelineLayout 暴露
- [ ] **编译验证**：项目编译通过
- [ ] **功能验证**：Material binding、per-frame dynamic buffer 正确

### Wave 8
- [ ] 所有 Pass 移除 `transitionTexture` / `transitionBuffer`
- [ ] 改用 `barrier(producer, consumer, hazard)`
- [ ] **编译验证**：项目编译通过
- [ ] **功能验证**：RenderDoc 验证 barrier 合理

### Wave 9
- [ ] `render/RenderDevice.h` 移除所有 `Vk*` 成员
- [ ] `rhi/RHICommandList.h` 删除
- [ ] `rhi/vulkan/VulkanCommandList.h` 删除 escape hatches
- [ ] `rhi/RHIHandles.h` 移除 `fromNativePtr` / `toNativePtr`
- [ ] **编译验证**：项目编译通过，无 deprecated 警告
- [ ] **性能验证**：Benchmark 对比迁移前后帧时间

---

## 15. 需要用户确认的关键决策

1. **Wave 0 的 `CommandBuffer` 是否保留旧 `CommandList` 兼容？**
   - 建议：保留。`PassContext` 同时有 `cmd`（旧）和 `cmdBuffer`（新），未迁移的 pass 仍用 `cmd`。

2. **`ArgumentTable` 是否保留 `BindGroup` 作为别名？**
   - 建议：保留 `using BindGroupHandle = ArgumentTableHandle;` 作为过渡，减少业务层改名成本。

3. **`StageBarrier` 的 image layout 自动推断是否足够？**
   - 风险：PresentPass 的 swapchain layout transition（`TRANSFER_DST` → `PRESENT_SRC_KHR`）需要精确控制。
   - 建议：Vulkan 后端内部维护 per-resource layout cache，在 `present()` / `blitTexture` 时自动推断。如果验证发现推断错误，再增加显式 layout hint。

4. **是否允许 Wave 1-9 期间 D3D12/Metal stub 编译失败？**
   - 建议：允许。为所有新增纯虚方法提供 `assert(false)` stub，保持 D3D12/Metal 后端可编译但运行时崩溃。等 Vulkan 验证完成后再填充 D3D12/Metal。

5. **GBuffer Pass 的 MDI 路径是否可以最后迁移？**
   - 建议：可以。Tier 1/2 Render Pass 先迁移，验证 `RenderEncoder` 的 `drawIndexed` / `drawIndexedIndirect` 正确后，再处理 GBuffer 的复杂 MDI + Alpha Test 分支。
