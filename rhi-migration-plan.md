# RHI 重构实施计划（Vulkan-first）：现代 GPU 接口

> 设计愿景以 `future-rhi-design-review.md` 为准：
> **GpuPtr + ArgumentTable + StageBarrier + One-shot CommandBuffer + DrawStream**。
> 本文件是落地到代码的可执行任务计划。执行策略：**Vulkan-first**——只实现 Vulkan 后端，
> D3D12 / Metal 提供 stub；但所有 RHI 公共接口必须保持**三后端可映射、无 native 泄漏**。
> 每个 Wave 结束都能编译运行，可独立提交与回滚。

---

## 0. 起点认知与核心原则

### 0.1 现状盘点：本引擎已是 ~80% 现代 GPU-Driven

经代码核实，许多"现代特性"**已经存在并在用**。本计划的任务是**用干净的 RHI 接口包装它们 + 去除 native 泄漏**，
而不是从零引入。

| 现代要素 | 现状 | 证据 |
|---------|------|------|
| dynamic rendering（无 subpass） | 已用 | `rhi/vulkan/VulkanCommandList.cpp:307` `vkCmdBeginRendering` |
| buffer device address / GpuPtr | 已用 | `render/GPUMeshletBuffer.cpp:41`、`render/GPUSceneRegistry.cpp:43` + shader `buffer_reference` |
| GPUScene（buffer-first 数据模型） | 已用 | `render/GPUSceneRegistry`（打包 `GPUSceneObject/GPUCullObject`，per-draw 走 device address 索引） |
| 持久 bind group + dynamic offset | 已用 | per-frame 持久数组；`render/passes/GPUDrivenLightPass.cpp:140` `dynamicOffsets` |
| pass-level 自动 barrier | 已用 | `render/PassExecutor.cpp:239` 从声明式依赖生成（`render/Pass.h:50-80,137`） |
| timeline semaphore + per-frame pool | 已用 | `rhi/vulkan/VulkanFrameContext.h`（`FrameSlot` + `VulkanTimelineSemaphore`） |
| 延迟删除队列 | 已用 | `rhi/RHIResourceLifetime.h`（`InlineDeferredDestructionQueue` + `RetirementPolicy`） |
| `BufferHandle` 句柄类型 | 已存在 | `rhi/RHIHandles.h` |
| Indirect draw / indirect count | 已用 | `RHICommandList::drawIndexedIndirect/Count` |

**待重构（本计划的真正目标）**：
1. unified `CommandList` → `CommandBuffer + Encoder`（接近 Metal4 / 现代 pass 模型）。
2. `BindGroup`/`BindTable`（裸指针，偏 Vulkan DescriptorSet）→ `ArgumentLayout`/`ArgumentTable`（句柄）。
3. per-resource `transitionTexture`（携带 old/new layout）→ `barrier(Stage, Stage, Hazard)`（后端推断 layout）。
4. `GpuPtr` 从裸 `uint64_t address` → 类型化 RHI 一等公民（包装既有 device address）。
5. 去除 `getNativeCommandBuffer()` / `VkImage`/`VkDescriptorSet`/`VkPipelineLayout` 等 native escape hatch。
6. `common/Common.h` 与 `RenderDevice.h` 的 Vulkan 类型泄漏下沉。
7. DrawStream → Encoder 的 decode 路径（`PassContext::drawStream` 已存在，缺 decode）。

### 0.2 核心原则

1. **包装既有，不重造**：凡现状已有的现代行为（device address、GPUScene、dynamic rendering），
   重构只改"接口形状"，**保持运行时行为不变**。禁止借迁移之名重写已工作的数据路径。
2. **先建新、不删旧**：保留 `CommandList`/`BindGroup` 为 `[[deprecated]]` 兼容层，逐 Pass 迁移，最后统一删除。
3. **Vulkan-first，接口干净**：只实现 Vulkan；D3D12/Metal 用 `assert(false)` stub 保持可编译。
   但公共接口**不得**出现 Vulkan-only 字段（语义化 `TextureUsageFlags`/`BufferUsageFlags`，不 bit-transparent）。
4. **每 Wave 可编译运行**，单独 git commit，验证失败即回滚到上一 Wave。

### 0.3 Barrier 模型：PassExecutor 即 "RenderGraph-lite"

设计文档 §3.4 / Phase 6 要求 **pass-level 生成 barrier，禁止 per-draw resource state tracking**。
现状 `PassExecutor` 已满足：它读取每个 Pass 的 `getDependencies()`（`PassResourceDependency`：
`access` / `stageMask` / `requiredState` / handle），集中插入 barrier。

**决策（双 barrier 模型，对齐设计文档 §7）**：保留这套声明式依赖为 barrier 的唯一真相来源，
`PassExecutor` 把依赖翻译为两类正式 verb：
- **`barrier(Stage, Stage, Hazard)`（主路径）**：表达阶段依赖 + hazard 类型。大多数 producer→consumer 用它。
- **`resourceBarrier(TextureBarrier[], BufferBarrier[])`（特殊路径，正式保留）**：表达 image layout / queue ownership /
  present / aliasing。**这不是 deprecated fallback，而是设计文档 §7.2 的一等 verb**。

实现层面：`transitionTexture/transitionBuffer`（携带 per-resource old/new layout）**重塑**为 `resourceBarrier`，
而非废弃删除。常规同步优先用 `barrier`；只有真正需要显式 layout/queue 的边界（如 present、blit）才发 `resourceBarrier`。
Vulkan 后端维护 per-texture layout tracker，在 `beginRenderPass`/`blitTexture`/present 时据依赖的 `requiredState` 自动补 layout 转换。

> 不采用"在每个 Pass 内手写 barrier"。手写仅作为声明式模型无法表达的 **intrapass hazard** 的显式 override
> （例如同一 compute pass 内先写 indirect args、本 pass 后续指令立即消费）。

**翻译映射**（Wave 7 在 PassExecutor 内实现）：
- `ShaderStage::compute`→`StageFlags::compute`；`fragment`→`fragmentShader`；color/depth 写→`rasterColorOut`/`rasterDepthOut`；indirect 消费→`commandInput`。
- texture write→read=`textureWrites`；buffer write→read=`bufferWrites`；depth=`depthStencil`；indirect args=`drawArguments`；descriptor=`descriptors`。
- 需要显式 layout/queue 的边界 → `resourceBarrier`；其余 → `barrier`。
- 现有去重逻辑 `requiresBarrier(prev,next)`（read→read 不插）原样保留。

### 0.4 CommandBuffer 与 FrameContext 的衔接

提交与同步由 `FrameContext` 负责（per-frame `FrameSlot` + timeline semaphore），**不得绕过或重造**。

**决策**：
1. `CommandBuffer` 是当前帧 `FrameSlot` 内 `VkCommandBuffer` 的录制门面，由 `FrameContext` 拥有/复用（one-shot，每帧重置）。
2. `FrameContext` 新增 `CommandBuffer* getCommandBuffer()`（取当前帧门面）。**不**在 `Device` 上加 `createCommandBuffer/destroyCommandBuffer`（lifetime 自管理会与 per-frame pool 冲突）。
3. 提交路径不变：`endFrame()` 接受当前帧录制对象并 signal timeline。过渡期 `endFrame` 同时接受旧 `CommandList*` 与新 `CommandBuffer*`（内部同一 `VkCommandBuffer`）。

### 0.5 资源销毁走延迟删除队列

所有新增 `destroy*`（buffer/sampler/argumentTable/argumentLayout/queryPool）**一律走延迟删除**，禁止即时销毁在飞资源：

```cpp
void VulkanDevice::destroyBuffer(BufferHandle h) {
    const uint64_t retireAt = calculateRetirementTimelineValue(
        frameCtx.getCurrentFrameValue(),
        RetirementPolicy::frameCount(frameCtx.getFrameCount()));
    frameCtx.enqueueRetirement({ResourceKind::Buffer, h.index, h.generation}, retireAt);
}
```
物理释放发生在 `processRetirements()`（每帧 `beginFrame` 按当前 timeline 值排空）。
Tier 归属：几何/meshlet/indirect buffer→`Device`/`Swapchain`；per-frame uniform→`PerFrame`；ArgumentTable→`Device`（句柄稳定、内容每帧重写）；QueryPool→`Device`。

### 0.6 ArgumentTable 句柄化所有权

现状 `BindGroupDesc`（`render/BindGroups.h:34-41`）持**裸指针** `BindTableLayout*`/`BindTable*`；
后端 `VulkanResourceTable::m_bindGroupTables` 是 `std::unordered_map<uint64_t, BindTable*>`。

**决策**：`ArgumentLayout`/`ArgumentTable` 改为**句柄 + 后端 HandlePool**：
1. `BindGroupDesc` 的两个指针字段 → `ArgumentLayoutHandle`/`ArgumentTableHandle`（**字段替换，不能用 alias 掩盖**）。
2. 后端 `m_bindGroupTables` 从 `unordered_map` → `HandlePool` / 数组索引，解析 O(1)。
3. 寿命走 §0.5 延迟删除。
4. 兼容期可加 `using BindGroupHandle = ArgumentTableHandle;`（仅句柄层别名，不替代字段替换）。
5. 沿用现状的"持久 table + dynamic offset"模式（HypeHype §2.5），per-frame 数据用 dynamic buffer offset，不每帧重建 table。

### 0.7 接口形状的两项前瞻预留

即使本次不实现，接口形状现在就要预留，避免将来二次破坏 API：

- **多线程录制**（设计文档 Phase 7）：`CommandBuffer` 可由非主线程获取与录制；`Encoder` 不持有全局可变状态；
  `FrameContext` 接口允许 vend 多个 CommandBuffer / 未来支持 secondary command buffer。本次实现仍单线程，但 API 不假设单一全局 recorder。
- **移动端 tile-resident deferred**（input attachment / local read）：现状 GBuffer 与 Lighting 是分离 render pass，
  GBuffer 写出再采样（tiler 上一次全 GBuffer 显存往返）。`RenderPassDesc` 现在就要预留 **input attachment 与 local-read** 表达能力，
  Vulkan 后端在支持 `VK_KHR_dynamic_rendering_local_read` 时可映射、否则回退现有分离采样行为。实际 tile 驻留优化为后续里程碑。

### 0.8 范围边界

**本次做**：接口形状重构（Encoder/ArgumentTable/StageBarrier/ResourceBarrier/GpuPtr 类型化）、native 泄漏清理、DrawStream decode、QueryPool。
**本次不做（标记为后续里程碑，但接口预留不冲突）**：async compute / 多队列（compute/transfer queue 已枚举未用）、
多线程录制实现、`VK_EXT_descriptor_buffer` bindless 主路径、ResidencySet 实体（Vulkan 下 no-op）、tile-resident deferred 实现。

### 0.9 多后端公共不变量（验收门）

源自 `future-rhi-design-review.md` §1。每个 Wave 完成前按相关条目自查；标 ⛔ 的为强制门，违反不可合并。

1. ⛔ public RHI 头不含 `Vk*`/`ID3D12*`/`MTL*`/`Vma*` 等 native 类型。
2. ⛔ 业务层（`render/` 等）禁 include `rhi/vulkan/*`、`rhi/d3d12/*`、`rhi/metal/*`（Wave 9 加 grep 守卫）。
3. ⛔ 热路径（bind/draw/dispatch 录制）**禁 hashmap**（消灭 `resolveBindGroupDescriptorSet` 的 `unordered_map`，`VulkanResourceTable.cpp:107`）。
4. ⛔ 热路径禁堆分配。
5. ⛔ 热路径禁创建 backend object。
6. ⛔ 所有 handle resolve 为 O(1) 数组索引（`HandlePool`）。
7. `GpuPtr` 只表示 buffer GPU address（texture/sampler 用 descriptor index / view handle，不塞进 GpuPtr）。
8. descriptor / argument write 只接受 RHI handle，不接受 native descriptor。
9. pipeline layout / root signature 不暴露给业务层。
10. RenderGraph(=PassExecutor) 负责 pass-level barrier；RHI 不做 per-draw state tracking。
11. native escape hatch 仅 backend tests / debug 工具可用，renderer 不可用（含 TRACY 路径 `PassExecutor.cpp:236`，Wave 9 清理）。
12. renderer 主路径禁 `#ifdef VULKAN/D3D12/METAL`，backend 差异走 `DeviceCapabilities`。

> **Wave 1/Wave 2 额外验收**：grep 确认 `VulkanCommandList`/`VulkanResourceTable`/`render/` 的解析路径无 `unordered_map`，
> 所有 handle→native 仅经 `HandlePool` O(1)（落实第 3/6 条）。

### 0.10 与 future-rhi-design-review.md 目标态的对齐（本次范围外，接口不冲突）

设计文档的下列目标态**本次不实现**，但接口设计须保证未来可无破坏接入：

| 设计文档 | 目标态 | 本次处置 |
|---------|--------|---------|
| §3 Capability Tier 矩阵 | `DeviceCapabilities` 细分 tier + renderer 选路 | 现有 `CapabilityReport`/`supports()` 够用；tier 矩阵留后续 |
| §4.2 GpuResourceRef | texture/sampler/AS 独立 GPU 引用 | 本次 texture 走 `TextureViewHandle`；预留命名空间不与 GpuPtr 冲突 |
| §5 DescriptorHeap | `VK_EXT_descriptor_buffer` bindless 主路径 | 现状 descriptor set / ArgumentTable 承载 bindless；descriptor heap 留后续 |
| §6.1 RootBindingSchema | 跨后端 binding contract 对象 | 本次用 `setRoot*(ShaderStage,…)` 显式表达 visibility，等价且可平滑升级 |
| §9 PipelineCompiler/ShaderLibrary | `ShaderIR` 多后端编译 + pipeline cache | 现状 Slang→SPIR-V 直建 `VulkanPipelines`；编译抽象留后续 |
| §13 移动端 tier 回退 | Android A/B/C 选路 | 本次仅预留 input-attachment/local-read（§0.7）；tier 选路留后续 |

---

## 1. 23 个 Pass 分类与 Encoder 映射

| # | Pass | 类型 | 目标 Encoder | 复杂度 | 备注 |
|---|------|------|-------------|--------|------|
| 1 | `GPUDrivenCullingPass` | Compute | ComputeEncoder | 低 | 试点 |
| 2 | `GPUDrivenDepthPyramidPass` | Compute | ComputeEncoder | 低 | 试点 |
| 3 | `GPUDrivenLightCullingPass` | Compute | ComputeEncoder | 低 | |
| 4 | `GPUDrivenClusteredLightCullingPass` | Compute | ComputeEncoder | 低 | |
| 5 | `GPUDrivenVisibilitySortPass` | Compute | ComputeEncoder | 低 | |
| 6 | `GPUDrivenAOPass` | Compute | ComputeEncoder | 中 | texture R/W |
| 7 | `GPUDrivenSSRPass` | Compute | ComputeEncoder | 中 | texture R/W |
| 8 | `GPUDrivenBloomPrefilterPass` | Compute | ComputeEncoder | 低 | |
| 9 | `GPUDrivenBloomDownsamplePass` | Compute | ComputeEncoder | 低 | |
| 10 | `GPUDrivenTAAResolvePass` | Compute | ComputeEncoder | 中 | history texture |
| 11 | `GPUDrivenDepthPrepass` | Render | RenderEncoder | 中 | depth only |
| 12 | `GPUDrivenGBufferPass` | Render | RenderEncoder | 高 | MRT + MDI + alpha test，最后迁 |
| 13 | `GPUDrivenForwardPass` | Render | RenderEncoder | 中 | transparent，用 DrawStream |
| 14 | `GPUDrivenLightPass` | Render | RenderEncoder | 中 | 采样 GBuffer，input-attachment 候选 |
| 15 | `GPUDrivenSkyboxPass` | Render | RenderEncoder | 低 | |
| 16 | `GPUDrivenSkyPass` | Render | RenderEncoder | 低 | |
| 17 | `GPUDrivenCSMShadowPass` | Render | RenderEncoder | 中 | cascade array |
| 18 | `GPUDrivenShadowAtlasPass` | Render | RenderEncoder | 中 | atlas tiles |
| 19 | `GPUDrivenDebugPass` | Render | RenderEncoder | 低 | |
| 20 | `GPUDrivenImguiPass` | Render | RenderEncoder | 中 | ImGui draw data + 动态顶点缓冲 |
| 21 | `GPUDrivenVelocityPass` | Render | RenderEncoder | 低 | |
| 22 | `GPUDrivenFinalColorPass` | Render | RenderEncoder | 中 | tone mapping |
| 23 | `GPUDrivenPresentPass` | Copy/Blit | ComputeEncoder（copy 子集） | 低 | swapchain blit |

**迁移顺序**：纯 Compute（1-5）→ Copy（23）→ 简单 Render（15,16,19,21）→ 中等 Render（11,13,14,17,18,20,22）→ 复杂（12 GBuffer）+ 复杂 Compute（6,7,10）。

---

## 2. RHI Core 接口契约（Wave 0 落地）

### 2.1 `rhi/RHIStageBarrier.h`（新建）

```cpp
enum class StageFlags : uint64_t {
    none=0, transfer=1ull<<0, compute=1ull<<1, vertexShader=1ull<<2, fragmentShader=1ull<<3,
    rasterColorOut=1ull<<4, rasterDepthOut=1ull<<5, commandInput=1ull<<6, all=~0ull,
};
enum class HazardFlags : uint32_t {
    none=0, descriptors=1u<<0, drawArguments=1u<<1, depthStencil=1u<<2, textureWrites=1u<<3, bufferWrites=1u<<4,
};
// + operator| / operator&
```

### 2.2 `rhi/RHIHandles.h`（扩展）

新增：`ArgumentLayoutHandle`、`ArgumentTableHandle`、`QueryPoolHandle`、`ResidencySetHandle`（后两者可 capability-gated）。
（`BufferHandle` 已存在，复用。）`ShaderModule`/`PipelineLayout` 由后端内部管理，**不**暴露句柄给业务层。
`TextureViewHandle::fromNativePtr/toNativePtr` 标 `[[deprecated]]`，Wave 9 删除。

### 2.3 `rhi/RHITypes.h`（扩展）

```cpp
struct GpuPtr { uint64_t value{0}; bool isValid() const { return value!=0; } };  // 包装既有 device address
struct BufferDesc {
    uint64_t size{0}; BufferUsageFlags usage{}; MemoryUsage memoryUsage{MemoryUsage::gpuOnly};
    bool allowGpuAddress{false}, allowIndirectArgument{false}, allowArgumentTableBinding{false};
    const char* debugName{nullptr};
};
// SamplerDesc；语义化 TextureUsageFlags / BufferUsageFlags（不 bit-transparent 对应 Vulkan）
// TextureCreateDesc / TextureViewCreateDesc 移除 native 字段
```

### 2.4 `rhi/RHIArgumentTable.h`（新建）

`ArgumentType` / `ArgumentBinding`（含 `bindless`、`dynamicOffset`）/ `ArgumentLayoutDesc` / `ArgumentWrite`
（只接受 RHI handle：`BufferHandle`/`TextureViewHandle`/`SamplerHandle`，**不**接受 native descriptor）。

### 2.5 `rhi/RHIEncoder.h`（新建）—— 含 input-attachment / local-read 预留

```cpp
// 关键：RenderPassDesc 预留 input attachment 与 local read（§0.7）
struct RenderPassDesc {
    Rect2D                  renderArea{};
    const RenderTargetDesc* colorTargets{nullptr};
    uint32_t                colorTargetCount{0};
    const DepthTargetDesc*  depthTarget{nullptr};
    // --- 预留：tile-resident deferred ---
    const InputAttachmentDesc* inputAttachments{nullptr};  // 供 local read 的输入附件
    uint32_t                   inputAttachmentCount{0};
    bool                       enableLocalRead{false};     // 后端支持时映射 dynamic_rendering_local_read，否则回退
};

// 注意：setRoot*/setDynamicBuffer 带 ShaderStage（对齐设计文档 §6 与现有 pushConstants(ShaderStage,...)），
// slot visibility 由 ShaderStage 显式表达，避免将来引入 RootBindingSchema 时破坏 API。
class RenderEncoder {
public:
    virtual void setPipeline(PipelineHandle) = 0;
    virtual void setArgumentTable(uint32_t slot, ArgumentTableHandle) = 0;
    virtual void setDynamicBuffer(ShaderStage stage, uint32_t slot, BufferHandle, uint64_t offset, uint64_t size) = 0;
    virtual void setRootConstants(ShaderStage stage, uint32_t slot, const void* data, uint32_t size) = 0;  // 小数据 fast path
    virtual void setRootPointer(ShaderStage stage, uint32_t slot, GpuPtr ptr) = 0;                          // per-draw 主通道
    virtual void setViewport(const Viewport&) = 0;
    virtual void setScissor(const Rect2D&) = 0;
    virtual void bindVertexBuffers(uint32_t first, const BufferHandle*, const uint64_t* offsets, uint32_t count) = 0;
    virtual void bindIndexBuffer(BufferHandle, uint64_t offset, IndexFormat) = 0;
    virtual void readInputAttachment(uint32_t index) = 0;  // 预留：local read 采样
    virtual void drawIndexed(const DrawIndexedDesc&) = 0;
    virtual void drawIndexedIndirect(const DrawIndirectDesc&) = 0;            // BufferHandle + offset
    virtual void drawIndexedIndirect(GpuPtr args, uint32_t count, uint32_t stride) = 0;  // GpuPtr 重载（设计 §8）
    virtual void drawIndexedIndirectCount(const DrawIndirectCountDesc&) = 0;
    virtual void drawIndirect(const DrawIndirectDesc&) = 0;
    virtual void drawMeshTasks(uint32_t, uint32_t, uint32_t) = 0;        // capability-gated
    virtual void drawMeshTasksIndirect(const DrawIndirectDesc&) = 0;
};
// ComputeEncoder: setPipeline/setArgumentTable/setRootConstants(slot,…)/setRootPointer(slot,…)/dispatch
//                 （compute 阶段恒定，setRoot* 不带 ShaderStage）+ dispatchIndirect(BufferHandle/GpuPtr) 重载
//                 + copy/blit 命令子集（无独立 CopyEncoder，对齐 Metal 4）：
//                   copyBuffer/copyBufferToTexture/copyTextureToBuffer/blitTexture/fillBuffer
```

> `InputAttachmentDesc` 在 Wave 0 仅定义结构与接口；Vulkan 后端 Wave 1 可先实现为"分离采样回退"（行为同现状），
> `enableLocalRead=true` 且设备支持时才走 local read。Pass 暂不使用，留作 Lighting pass 后续优化入口。

### 2.6 `rhi/RHICommandBuffer.h`（新建）

双 barrier 模型（设计文档 §7）：`barrier` 是主路径（stage/hazard），`resourceBarrier` 是**正式的特殊路径**
（image layout / queue ownership / present / aliasing / validation），**不是 deprecated fallback**。

```cpp
// TextureBarrier{texture, before, after, range, srcQueue, dstQueue}
// BufferBarrier {buffer,  before, after, offset, size, srcQueue, dstQueue}
class CommandBuffer {
public:
    virtual RenderEncoder*  beginRenderPass(const RenderPassDesc&) = 0;
    virtual ComputeEncoder* beginComputePass() = 0;   // copy/blit 为其命令子集，无 beginCopyPass
    virtual void            endEncoding() = 0;
    virtual void            barrier(StageFlags producer, StageFlags consumer, HazardFlags) = 0;        // 主路径
    virtual void            resourceBarrier(const TextureBarrier* textures, uint32_t textureCount,
                                            const BufferBarrier* buffers, uint32_t bufferCount) = 0;   // 特殊路径（保留）
    virtual void            beginEvent(const char* name) = 0;
    virtual void            endEvent() = 0;
};
```

### 2.7 `rhi/RHIDevice.h`（扩展，不删旧）

```cpp
// Buffer（包装既有 device address 路径）
virtual BufferHandle createBuffer(const BufferDesc&) = 0;
virtual void         destroyBuffer(BufferHandle) = 0;                  // 走延迟删除
virtual GpuPtr       getBufferGpuAddress(BufferHandle) const = 0;      // 类型化既有 .address
virtual void*        mapBuffer(BufferHandle) = 0;
virtual void         unmapBuffer(BufferHandle) = 0;
// Sampler / ArgumentTable / QueryPool（均走延迟删除）
virtual SamplerHandle        createSampler(const SamplerDesc&) = 0;
virtual ArgumentLayoutHandle createArgumentLayout(const ArgumentLayoutDesc&) = 0;
virtual ArgumentTableHandle  createArgumentTable(ArgumentLayoutHandle) = 0;
virtual void                 updateArgumentTable(ArgumentTableHandle, uint32_t writeCount, const ArgumentWrite*) = 0;
virtual QueryPoolHandle      createQueryPool(uint32_t queryCount) = 0;
virtual uint64_t             getQueryPoolResult(QueryPoolHandle, uint32_t index) = 0;
// 注意：CommandBuffer 经 FrameContext::getCommandBuffer() 获取（§0.4），不在 Device 上 create/destroy
```

### 2.8 `rhi/RHIFrameContext.h`（扩展）

```cpp
virtual CommandBuffer* getCommandBuffer() = 0;   // 当前帧的 one-shot 录制门面
```

---

> **每个 Wave 的统一节奏**：`目标` → `前置依赖` → `改动文件与步骤`（逐文件可执行）→ `验证`（可观测判据）→ `提交`（git commit message）。
> 复选框为可勾选的执行项。签名以 `before → after` 表示。

## 3. Wave 0：接口契约定义（不破坏现有代码） — ✅ 已落地（待全量编译）

**目标**：定义 §2 全部接口，业务层与三后端继续编译。
**前置依赖**：无（起点）。

> **实现说明（对计划的小幅修订）**：新增的 Device/FrameContext 方法采用**基类默认 `assert(false)` 实现**，
> 而非"纯虚 + 每后端手写桩"。效果等价（未实现即运行期断言）但 Wave 0 **零改动三后端 cpp**，爆炸半径最小。
> Vulkan 在 Wave 1 override 真实实现；D3D12/Metal 继承断言默认。

**改动文件与步骤**：
- [x] 新建 `rhi/RHIStageBarrier.h`：`StageFlags`/`HazardFlags`（+ `operator|/&/|=`/`any`）+ `TextureBarrier`/`BufferBarrier`/`TextureSubresourceRange`
- [x] 新建 `rhi/RHIArgumentTable.h`：`ArgumentType`/`ArgumentBinding`/`ArgumentLayoutDesc`/`ArgumentWrite`（只含 RHI handle）
- [x] 新建 `rhi/RHIEncoder.h`：`RenderEncoder`/`ComputeEncoder`（copy/blit 为 ComputeEncoder 命令子集，无独立 CopyEncoder，对齐 Metal 4）（`setRoot*` 带 `ShaderStage`、`readInputAttachment`、GpuPtr indirect 重载）+ Draw/Dispatch/Copy desc 结构
- [x] 新建 `rhi/RHICommandBuffer.h`：`CommandBuffer`（`barrier` + `resourceBarrier` 双 verb）
- [x] 改 `rhi/RHIHandles.h`：新增 `ArgumentLayoutHandle`/`ArgumentTableHandle`/`QueryPoolHandle`/`ResidencySetHandle`
- [x] 改 `rhi/RHITypes.h`：新增 `GpuPtr`/`MemoryUsage`/语义化 `BufferUsageFlags`/`TextureUsageFlags`/`BufferDesc`/`SamplerDesc`（+Filter/MipmapMode/AddressMode）；**下沉** `RenderTargetDesc`/`DepthTargetDesc`/`RenderPassDesc`（+`InputAttachmentDesc` 与 local-read 三字段）
- [x] 改 `rhi/RHICommandList.h`：移除已下沉的 3 个 struct（经其 `#include RHITypes.h` 仍可见）
- [x] 改 `rhi/RHIDevice.h`：加 buffer/sampler/argument-table/query-pool 方法（基类默认 assert 实现）+ `#include RHIArgumentTable.h`/`<cassert>`
- [x] 改 `rhi/RHIFrameContext.h`：加 `CommandBuffer* getCommandBuffer()`（默认 assert）+ `#include RHICommandBuffer.h`
- [x] `CMakeLists.txt`：4 个新头加入 `demo_core` 源列表
- [x] 三后端：因采用基类默认实现，**无需改动**（自动继承断言桩）
- [ ] 接口干净性自查（非阻塞）：对照 §0.9 不变量 + 三后端映射表（待补 PR 描述）
- [~] **延后到 Wave 1/2**：`TextureCreateDesc`/`TextureViewCreateDesc` 移除 native 字段——现 VulkanDevice 仍读 `nativeFormat/nativeUsage`，移除会破坏后端，随 buffer/texture 包装一起做。

**验证**：
- [x] 新头 + 改动头经 clang 19 `-std=c++20 -fsyntax-only` 独立校验通过（无错误）。
- [x] **全量编译通过**：`out/build/x64-debug` 重新 configure 到 `G:/MGIF`（原缓存指向旧路径 `G:/VK_DEMO`，已清顶层 + `_deps` 残留缓存并复用已下载依赖重配）后，`cmake --build --target demo_core` 成功（`[179/179] Linking demo_core.lib`，无错误）。
- 现有功能行为未变（纯接口新增 + 默认 assert 实现，无调用方改动）。

**提交**：`feat(rhi): add Encoder/CommandBuffer/ArgumentTable/StageBarrier/GpuPtr interface contracts`

---

## 4. Wave 1：Vulkan 后端实现新接口 — ✅ 完成（全量编译通过）

**目标**：Vulkan 后端实现 §2 接口，可独立创建 buffer/获取 GpuPtr/录制空 encoder。
**前置依赖**：Wave 0。

> **进度**：增量 1（buffer/sampler/query-pool + 资源表池）与增量 2（CommandBuffer + 3 encoders + ArgumentTable + getCommandBuffer）均完成，全量编译通过（`[2/2] Linking demo_core.lib`）。
> **对计划的小幅修订（已落地）**：
> - `destroy*` 暂用即时销毁（与现有 `destroyImage` 一致）；延迟删除（§0.5）作为后续 wiring（需把 FrameContext 注入 Device）。
> - `buffer device address` feature 现状已启用，无需补。
> - 三个 encoder 与 CommandBuffer 合并到单个 `VulkanCommandBuffer.h/.cpp`（少建文件）；ArgumentTable 后端直接实现在 `VulkanDevice` + 资源表（descriptor pool 由 Device 懒创建），未单独建 `VulkanArgumentTable.*`，等价且更简洁。
> - `BufferRecord` 当前是单结构（hot 字段在前）；完整 Hot/Cold `ResourcePool` 模板拆分留作性能优化（O(1) resolve 不变量已满足）。
> - `setRoot*` 的 `slot` 在 Vulkan 解释为 push-constant 字节偏移（Vulkan 无 slot 概念）；具体偏移在 Wave 3 随试点 pass 的 shader 布局敲定。
> - `getCommandBuffer()` 返回的门面需 render 层经 `VulkanFrameContext::setResourceTable()` 注入资源表（Wave 3 接线）；`drawIndexedIndirect(GpuPtr)`/`dispatchIndirect(GpuPtr)` 在 Vulkan core 不支持，断言提示用 BufferHandle 重载。

**改动文件与步骤**：
- [x] buffer device address：现状已启用（无需补）
- [x] `VulkanDevice`：`createBuffer`/`destroyBuffer`/`getBufferGpuAddress`/`mapBuffer`/`unmapBuffer`/`createSampler`/`destroySampler`/`createQueryPool`/`destroyQueryPool`/`getQueryPoolResult`/`createArgumentLayout`/`destroyArgumentLayout`/`createArgumentTable`/`destroyArgumentTable`/`updateArgumentTable`（+ 懒创建 descriptor pool，deinit 释放）
- [x] `VulkanResourceTable`：`BufferRecord`/`SamplerRecord`/`QueryPoolRecord`/`ArgumentLayoutRecord`/`ArgumentTableRecord` 的 `HandlePool` O(1) 池 + register/resolve/remove
- [x] 新建 `rhi/vulkan/VulkanCommandBuffer.h/.cpp`：`VulkanCommandBuffer`（`beginRenderPass`→`vkCmdBeginRendering`/`endEncoding`/`barrier`(stage+hazard,`vkCmdPipelineBarrier2`)/`resourceBarrier`(image+buffer)/debug marker）+ `VulkanRenderEncoder`/`VulkanComputeEncoder`（copy/blit 并入 ComputeEncoder，无独立 CopyEncoder）（真实 `vkCmd*` 录制）
- [x] `VulkanFrameContext`：`getCommandBuffer()`（返回当前帧 `VkCommandBuffer` 的门面）+ `setResourceTable()`；持有复用的 `VulkanCommandBuffer` 门面
- [x] `CMakeLists.txt`：收录 `VulkanCommandBuffer.h/.cpp`
- [~] 延迟删除 wiring（`destroy*`→`enqueueRetirement`）：留待后续（现即时销毁，与现有约定一致）

**验证**：
- [x] **全量编译通过**：`cmake --build --target demo_core` → `[2/2] Linking demo_core.lib`（仅 C4819 编码告警；LSP 的 volk.h 报错为 LSP 配置噪声，非真实错误）。
- [ ] 运行期冒烟（createBuffer→address→destroy / 空 compute pass 录制）：待 Wave 3 接入资源表后随试点 pass 一起验证。

**提交**：`feat(rhi/vulkan): implement encoders, command buffer, argument table, buffer+gpuptr`

---

## 5. Wave 2：Common 层下沉 + 包装既有 Buffer 资源 — 🔁 已重排序（仅做安全增量）

**目标**：Vulkan 类型移出公共头；用 `BufferHandle`/`GpuPtr` 包装既有 device-address 资源，**运行时行为不变**。
**前置依赖**：Wave 1。

> **重排序决策（实测后调整）**：实测 `Common.h` 被 **36 文件** include、**24 文件**直接用 `utils::Buffer`、
> `VK_CHECK`×159/`ASSERT`×133 遍布全仓，且宏与 `utils::` helper 强耦合。"净化 Common.h + utils::Buffer→BufferHandle"
> 是 30+ 文件的强波及大爆炸，违背"每 Wave 10–20 文件、保持编译绿色"，且即时价值低（消费方仍 include Common.h）。
> 因此：
> - **本 Wave 仅做安全增量**：FrameGpuAllocator（已完成）。
> - **Common.h 净化 + `utils::Buffer→BufferHandle` 迁移降级**为"随消费方迁移逐步进行 / 靠近 Wave 9 的清理"，不作为阻塞性前置。
> - Wave 3 试点（culling/depth-pyramid 用现有 pipeline/bindgroup/dispatch）**不依赖** buffer 包装，可直接进行。

**改动文件与步骤**：
- [x] `render/TransientAllocator`（FrameGpuAllocator）：`Allocation`/`TypedAllocation` 增 `rhi::GpuPtr gpu`，`allocate()` 按 `m_buffer.address + offset` 填充（纯增量，编译通过）
- [~] 以下降级为后续/随迁移进行（非阻塞）：
  - 新建 `common/VulkanTypes.h`：迁入 `utils::Buffer/Image/ImageResource/QueueInfo/AccelerationStructure`
- [ ] 新建 `common/VulkanHelpers.h`：迁入 `cmdInitImageLayout`/`cmdTransitionSwapchainLayout`/`cmdBufferMemoryBarrier`/`findSupportedFormat`/`findDepthFormat`/`createShaderModule`/`beginSingleTimeCommands`/`endSingleTimeCommands`/`pNextChainPushFront`
- [ ] 新建 `common/VulkanContextConfig.h`：迁入 `ExtensionConfig`/`ContextCreateInfo`/`ValidationSettings`
- [ ] 净化 `common/Common.h`：移除上述 Vulkan 内容；保留 GLM include / `shaderio` / `packNormalRGB10A2`+`unpack` / `findFile` / `ASSERT` / `hashCombine`；`Vertex::getBindingDescription/getAttributeDescriptions` 改返回 `rhi::Vertex*Desc`（`volk.h`/`vk_mem_alloc.h` 下沉到对应新头或后端 cpp）
- [ ] `render/UploadUtils.h/.cpp`：
  - `utils::Buffer createStaticBufferWithUpload(VkDevice, VmaAllocator, VkCommandBuffer, ...)` → `BufferHandle createStaticBufferWithUpload(rhi::Device&, rhi::CommandBuffer&, std::span<const std::byte>, rhi::BufferUsageFlags)`，内部 `createBuffer` + `ComputeEncoder::copyBuffer`（copy 子集）
- [ ] `render/GPUMeshletBuffer.h/.cpp`：成员 `utils::Buffer`→`rhi::BufferHandle`；`getMeshletDataAddress()`(`uint64_t`)→`rhi::GpuPtr`（实现 = `device->getBufferGpuAddress(handle)`，**封装既有 `.address`，语义不变**，证据 `GPUMeshletBuffer.cpp:41`）
- [ ] `render/GPUSceneRegistry.h/.cpp`：`m_objectBuffer`/`m_cullObjectBuffer` 改 `BufferHandle`；`getBufferAddress()`→`GpuPtr`（证据 `GPUSceneRegistry.cpp:43`），调用方同步改类型

**验证**：
- [x] 安全增量（FrameGpuAllocator）编译通过：`[2/2] Linking demo_core.lib`。
- [ ] 降级项的验证（RenderDoc 逐 buffer 对比、画面一致）随其实际迁移时进行。

**提交**：`feat(render): add FrameGpuAllocator GpuPtr to TransientAllocator`（降级项后续单独提交）

---

## 6. Wave 3：试点 Pass 迁移（Compute）— ✅ 试点完成（编译通过）

**目标**：打通 `FrameContext→CommandBuffer→Encoder→Pass` 链路，验证模型可行。
**前置依赖**：Wave 1（接口/后端）。buffer 包装非必需（试点 pass 用现有 pipeline/bindgroup/dispatch）。

> **进度**：`GPUDrivenCullingPass` 已迁移到 `ComputeEncoder`，整条链路打通并全量编译通过（`[4/4] Linking demo_core.lib`）。
> `GPUDrivenDepthPyramidPass` 委托进 `RenderDevice::executeDepthPyramidPass`（大函数），随后单独迁移。
> **关键发现/桥接**：试点 pass 仍用既有 `BindGroupHandle`，而新 `ComputeEncoder::setArgumentTable` 经新池解析。
> 加了**过渡桥接**：`VulkanResourceTable::resolveArgumentTable` 在新池未命中时按 handle bits 回退到 `m_bindGroupTables`，
> pass 把 bind group bits 当 `ArgumentTableHandle` 传入。Wave 8 正式替换 ArgumentTable 时移除该桥接。

**改动文件与步骤**：
- [x] `render/Pass.h`：`PassContext` 增 `rhi::CommandBuffer* cmdBuffer`（保留 `cmd` 兼容）+ include `RHICommandBuffer.h`
- [x] `render/RenderDevice.cpp`：frameContext init 后 `setResourceTable(&resourceTable)`（资源表注入）；`PassContext` 构造追加 `frameContext->getCommandBuffer()`
- [x] `rhi/vulkan/VulkanResourceTable.cpp`：`resolveArgumentTable` 加 bind-group 过渡桥接
- [x] `render/passes/GPUDrivenCullingPass.cpp`：删 `rhi/vulkan/VulkanCommandList.h` include 与 native barrier；`execute` 改 `beginComputePass`→`setPipeline`→`setArgumentTable`(桥接)→`dispatch`→`endEncoding`→`barrier(compute→commandInput, drawArguments|bufferWrites)`
- [ ] `render/passes/GPUDrivenDepthPyramidPass.cpp` + `RenderDevice::executeDepthPyramidPass(CommandList&→CommandBuffer&)`：随后迁移
- [x] 依赖声明（`getDependencies`）不动——barrier 仍由 `PassExecutor` 自动产生（Wave 7 才换动词）

**验证**：
- [x] **全量编译通过**：`[4/4] Linking demo_core.lib`。
- [ ] 运行期（`getLastGPUCullingStats` 计数一致）：构建目录仅验证编译；运行验证待整机跑起来时进行。

**提交**：`refactor(render): migrate GPUDrivenCullingPass to ComputeEncoder (pilot) + frame/resource-table wiring + bindgroup bridge`

---

## 7. Wave 4：全部 Compute Pass 迁移 — ✅ 干净 compute 已迁（编译通过）

**目标**：把真正的 Compute Pass 转 `ComputeEncoder`。
**前置依赖**：Wave 3。

> **triage 修正**：计划 §1 误把 `GPUDrivenBloomPrefilterPass` / `GPUDrivenBloomDownsamplePass` / `GPUDrivenTAAResolvePass`
> 归为 Compute，实际它们是 **fullscreen render pass**（`beginRenderPass`+graphics+`draw(3)`）→ **移至 Wave 5（RenderEncoder）**。
> 真正的 compute pass 有 5 个：3 个"干净"（本回合迁），2 个含 native buffer copy/fill（依赖 buffer 包装，推迟）。

**改动文件与步骤**：
- [x] `GPUDrivenLightCullingPass`（2 dispatch + barrier）→ ComputeEncoder（+ bridge）
- [x] `GPUDrivenAOPass`（trace+denoise 两 dispatch + pushConstants + barrier）→ ComputeEncoder
- [x] `GPUDrivenSSRPass`（dispatch + pushConstants + barrier）→ ComputeEncoder
- 模式：移除 `rhi/vulkan/*` include；`bindPipeline(compute)`→`setPipeline`；`bindBindGroup`→`setArgumentTable`(桥接)；`pushConstants`→`setRootConstants(slot,data,size)`；`dispatch`→`enc->dispatch(DispatchDesc)`；`memoryBarrier`→`cmdBuffer->barrier(stage,stage,hazard)`
- [ ] **推迟（用 native uint64 buffer 的 copy/fill，待 buffer 包装/桥接）**：`GPUDrivenClusteredLightCullingPass`（`fillBuffer`）、`GPUDrivenVisibilitySortPass`（`copyBuffer`+bitonic）

**验证**：
- [x] **全量编译通过**：`[4/4] Linking demo_core.lib`。
- [ ] 运行期效果对比（AO/SSR/light culling）：待整机运行验证。

**提交**：`refactor(render): migrate clean compute passes (light-culling/AO/SSR) to ComputeEncoder`

---

## 8. Wave 5：Render Pass 迁移 — ✅ 完成（全部可迁 render pass 已转 RenderEncoder，Imgui/Debug 为例外）

**目标**：12 个 Render Pass 全部转 `RenderEncoder`。
**前置依赖**：Wave 4。

> **进度**：所有**纯全屏 pass** 已迁移到 `RenderEncoder`，全量编译通过（`[3/3] Linking demo_core.lib`）：
> - Tier 1：`GPUDrivenSkyboxPass` / `GPUDrivenSkyPass` / `GPUDrivenVelocityPass`
> - Tier 2 全屏：`GPUDrivenLightPass`（camera+scene 2 dyn offset）/ `GPUDrivenFinalColorPass`（camera+postProcess 2 dyn offset）
>
> **阻塞剩余 pass 的关键发现**：`GPUDrivenDepthPrepass` / `GPUDrivenForwardPass` / `GPUDrivenCSMShadowPass` / `GPUDrivenShadowAtlasPass` / `GPUDrivenGBufferPass` / `GPUDrivenDebugPass` / `GPUDrivenImguiPass` 都绑定**裸 `VkBuffer` 指针值**（`MeshRecord::vertexBufferHandle` = `reinterpret_cast<uint64_t>(VkBuffer)`、`getBufferOpaque()`、culling indirect buffer 的 uint64）。新 `RenderEncoder` 的 `bindVertexBuffers`/`bindIndexBuffer`/`drawIndexedIndirect*` 只接受 `BufferHandle` 并经 `m_table->resolveBuffer()`（buffer 池）解析；而 `BufferHandle{index,generation}` 装不下 64 位裸指针。
> **决策点（下一步）**：需先实现 **buffer 句柄化桥接**——要么把这些 native buffer 注册进 `VulkanResourceTable` buffer 池拿到 `BufferHandle`，要么给 encoder 加 native-buffer 桥接路径（类似 ArgumentTable 的 bind-group 桥接，但需容纳 64 位）。这是几何 MDI pass 迁移的前置，单独处理。
>
> **进度（旧）**：Tier 1 的 `GPUDrivenSkyboxPass` / `GPUDrivenSkyPass` / `GPUDrivenVelocityPass` 已迁移到 `RenderEncoder`，全量编译通过（`[4/4] Linking demo_core.lib`）。
> `GPUDrivenDebugPass` 的 `renderDebugOverlay` 当前被注释禁用（`execute` 不调用），且依赖 native 顶点缓冲（transient buffer 的 `getBufferOpaque()` uint64），随 buffer 包装/Tier 2 一并迁移。
> **本回合补齐的 backend 缺口（RenderEncoder）**：
> - 新增非索引 `draw(DrawDesc)`（`vkCmdDraw`）——fullscreen 三角形 `draw(3,1,0,0)` 所需，原接口缺失。
> - `setDynamicBuffer`/`setArgumentTable` 改为**每 slot 累积多个 dynamic offset**（`kMaxDynOffsetPerSlot=4`，按调用顺序＝binding 顺序 flush）——`LSetScene` 需 2 个 dynamic UBO offset。
> **迁移模式（behavior-preserving）**：手动 `transitionTexture`/`beginEvent`/`endEvent` 仍留在 `context.cmd`（与 `context.cmdBuffer` 同一 `VkCommandBuffer`），仅把 `beginRenderPass…draw…endRenderPass` 块换成 `context.cmdBuffer->beginRenderPass()`→encoder→`endEncoding`。layout transition 仍由 `PassExecutor` 自动生成（Wave 7 才换动词）。bind group 经 `ArgumentTableHandle{bg.index,bg.generation}` 桥接（Wave 8 正式替换）。
> **native 泄漏未清**：3 个 pass 仍 include `rhi/vulkan/VulkanCommandList.h`（用于 `VkExtent2D`/`VK_NULL_HANDLE`/`VkImage` 的早退检查与 `transitionTexture`），随 native accessor 替换（`getSceneExtent()→Extent2D` 等）/Wave 9 清理。

**改动文件与步骤**：
- [x] **Tier 1（简单 fullscreen）**：`GPUDrivenSkyboxPass` / `GPUDrivenSkyPass` / `GPUDrivenVelocityPass`（`GPUDrivenDebugPass` 推迟：dead code + native 顶点缓冲）
- [x] **Tier 2 全屏**：`GPUDrivenLightPass` / `GPUDrivenFinalColorPass`（纯全屏 `draw(3)`，无 native 缓冲绑定）
- [ ] **Tier 2 几何（buffer 句柄化进行中）**：`GPUDrivenDepthPrepass` / `GPUDrivenForwardPass` / `GPUDrivenCSMShadowPass`（cascade array）/ `GPUDrivenShadowAtlasPass`（atlas tiles）/ `GPUDrivenImguiPass`（ImGui draw data + 动态顶点缓冲）—— 均绑定裸 VkBuffer，阻塞于 buffer 句柄化
  - **方案 1（选项 B：稳定 handle + 每帧更新 record）**：给 `VulkanResourceTable` 加 `updateBuffer(handle, native, gpuAddress=0)`，per-frame/arena buffer 一次分配稳定 `BufferHandle`，realloc 时只重绑 native，无 handle churn。
  - [x] 基础设施：`VulkanResourceTable::updateBuffer`（rebind 稳定 handle）。
  - [x] **MeshPool 共享 vertex/index arena 句柄化**：`MeshPool` 持 `VulkanResourceTable*`（前向声明，native include 落 .cpp，不污染 render 头）+ 2 个稳定 `BufferHandle`；`ensureSharedCapacity` realloc 后 register/updateBuffer（owned=false，VMA 寿命仍归 MeshPool）；`deinit/resetSharedBuffers` removeBuffer；新 getter `getSharedVertexBufferRHIHandle()`/`getSharedIndexBufferRHIHandle()`。`MeshPool::init` 增 `resourceTable` 参数（`RenderDevice.cpp:894`）。编译通过（`[5/5] Linking demo_core.lib`）。
  - [x] **GPUMeshletBuffer meshlet index 句柄化**：持 `VulkanResourceTable*`（前向声明 + .cpp include）+ 稳定 handle；`ensureCapacities` realloc 后 register/updateBuffer（owned=false）；`deinit` removeBuffer；getter `getMeshletIndexBufferRHIHandle()`；`init` 增 resourceTable（`GPUDrivenRenderer.cpp:353` 传 `m_renderer.getResourceTable()`，RenderDevice 新增 `getResourceTable()`）。编译通过（`[3/3] Linking demo_core.lib`）。
  - [x] **FrameUserData per-frame buffer 句柄化**：`FrameUserData` 加 4 个稳定 `BufferHandle`（cullingIndirect/cullingDrawCount/shadowCullingIndirect/persistentStream）；RenderDevice 加统一 helper `rebindFrameBufferHandle(handle, buffer)`（首次 register、之后 updateBuffer、null 时 remove，owned=false）；在 3 个创建点调用（`ensureGPUCullingBuffers:2552`、`ensureShadowCullingBuffers:2715`、persistent `:2955`）；加 7 个 RHI handle getter（含 previous 变体 = `(frameIndex+count-1)%count`，frameCounter>1 守卫）。编译通过（`[2/2] Linking demo_core.lib`）。
  - **至此所有几何 pass 所需 buffer 均已句柄化**（MeshPool vertex/index、meshlet index、culling indirect/count/persistent/shadow）。
  - [x] **试点迁移 `GPUDrivenDepthPrepass`**（buffer 句柄化首个消费者）：GPUDrivenRenderer 委托 3 个 RHI getter（meshlet index / previous culling indirect+drawCount / previous persistent stream）；pass 改用 `rhi::BufferHandle` + `RenderEncoder::bindVertexBuffers/bindIndexBuffer/drawIndexedIndirectCount(DrawIndirectCountDesc)`；vertex/index 用 `MeshPool::getSharedVertex/IndexBufferRHIHandle()`；MDI args/count 用 previous-frame handle；bind group 经 `ArgumentTableHandle` 桥接；transition/beginEvent 仍留 `context.cmd`。保留 uint64 变量仅用于有效性判断与 offset 逻辑。编译通过（`[2/2] Linking demo_core.lib`）。**运行/RenderDoc 验证待整机运行**（深度缓冲一致性、MDI draw 序列）。
  - [x] **`GPUDrivenForwardPass`** 迁移：透明几何 MDI；`bindPipeline`/`bindBindGroup` 原在 `beginRenderPass` 前，**重排**到 encoder 之后；args=当前帧 persistent stream RHI handle，count=当前帧 culling drawCount RHI handle；`prepareAndDispatchVisibilityPatch` 仍走 `context.cmd`（render pass 外 compute）。编译通过。
  - [x] **`GPUDrivenGBufferPass`**（Tier3）迁移：MDI opaque+alphaTest 两 pipeline；args=sorted persistent / culling indirect RHI handle，count=culling drawCount RHI handle；保留 uint64 变量做有效性判断与 offset 选择。编译通过（`[2/2] Linking demo_core.lib`）。
  - GPUDrivenRenderer 补委托当前帧 RHI getter（culling indirect/drawCount/persistent stream）。
  - [x] **`GPUDrivenCSMShadowPass`** 迁移：句柄化 scene shadow packed vertex/index buffer（`GPUDrivenRenderer` 在 `updateSceneView` rebind 稳定 handle，owned=false）；per-cascade render 走 RenderEncoder，vertex/index 用 shadow packed RHI handle，`drawIndexedIndirect` 用 shadow culling indirect RHI handle；**per-cascade culling compute dispatch + transitionBuffer 保留 `context.cmd`**（render pass 外，留待后续 ComputeEncoder/Wave 7）。
  - [x] **`GPUDrivenShadowAtlasPass`** 迁移：单 render pass，per-tile viewport/scissor + per-mesh `drawIndexed(DrawIndexedDesc)`；vertex/index 用 shadow packed RHI handle。编译通过（`[4/4] Linking demo_core.lib`）。
  - [~] **`GPUDrivenImguiPass`（例外，保留 native）**：委托 `RenderDevice::executeImGuiPass`，内部走 ImGui Vulkan backend（`ImGui_ImplVulkan_RenderDrawData` 强制 native `VkCommandBuffer`，接口固定无法用 Encoder 抽象）+ present blit（属 Wave 6）。作为合理例外保留，Wave 9 评估。
  - [~] **`GPUDrivenDebugPass`（例外，dead code）**：`execute` 不调用 `renderDebugOverlay`（已注释禁用），且依赖 transient ring buffer 的 native 顶点缓冲。功能恢复时再迁移。
  - [ ] **全部 pass 迁移后需运行 + RenderDoc 验证**（深度/GBuffer/透明/阴影/CSM/atlas 画面一致、MDI draw 序列、validation layer 零报错）——本环境仅验证编译。
- [ ] **Tier 3（最复杂）**：`GPUDrivenGBufferPass` —— 先迁 non-MDI 路径，再迁 MDI（`drawIndexedIndirect`/`drawIndexedIndirectCount`），最后处理 alpha test 分支
- 每个文件：`beginRenderPass({colorTargets,…,depthTarget})`→`setPipeline`→`setArgumentTable(0/1/2,…)`→`setDynamicBuffer`/`setRootPointer`→`drawIndexed`/`drawIndexedIndirect`→`endEncoding`
- [ ] 替换 native accessor：`getSceneDepthImage()`/`getCurrentSwapchainImageView()`→`TextureViewHandle`；`getSceneExtent()`→`rhi::Extent2D`；`getSceneDepthFormat()`→`rhi::TextureFormat`
- [ ] `GPUDrivenLightPass`：在注释标记为 **input-attachment/local-read 优化候选**（本 Wave 仍用 `getLightingInputBindGroup` 分离采样，`GPUDrivenLightPass.cpp:111`）

**验证**：
- 编译通过；
- 完整管线 RenderDoc 对比：GBuffer 三张 + depth、阴影（CSM/atlas）、forward 透明、ImGui、最终色调映射逐 pass 一致。

**提交**：`refactor(render): migrate all render passes to RenderEncoder`

---

## 9. Wave 6：DrawStream decode + Present/Swapchain

**目标**：补齐 DrawStream→Encoder decode（设计文档 §9/Phase 7）；迁移 Present 与 Swapchain。
**前置依赖**：Wave 5。

**改动文件与步骤**：
- [ ] Renderer Framework 层新增 `decodeDrawStream(rhi::RenderEncoder&, const DrawStream&)`：
  - 维护 `DrawState`，用 dirty mask 压缩 pipeline / argument table / dynamic offset / index/vertex buffer 变化，仅变化字段调对应 setter（照设计文档 §9 伪码）
  - **留在 Renderer 层，不下沉 RHI Core**（设计文档决策 #10）
- [ ] 消费 `PassContext::drawStream`（`Pass.h:27`）的 Render Pass（GBuffer/Forward 等）改为调用 `decodeDrawStream`，替换逐 draw 手写
- [ ] `render/passes/GPUDrivenPresentPass.cpp`：`transitionTexture`+`blitImage` → `beginComputePass`→`blitTexture({srcTexture, dstTexture, …letterbox offsets})`（copy 子集）→`endEncoding`（layout 由后端处理，见 Wave 7）
- [ ] `rhi/RHISwapchain.h` + `rhi/vulkan/VulkanSwapchain.cpp`：移除 `getNativeSwapchain/getNativeImageView/getNativeImage`；`currentTexture()`→`TextureHandle`；新增 `currentTextureView()`→`TextureViewHandle`

**验证**：
- 编译通过；present 正确、无撕裂、letterbox 比例正确；
- DrawStream 路径下 GBuffer/Forward 的 draw call 序列 RenderDoc 与 Wave 5 手写路径一致。

**提交**：`feat(render): add DrawStream→Encoder decode; migrate present + swapchain`

---

## 10. Wave 7：StageBarrier + ResourceBarrier 重构（PassExecutor 改发射动词）

**目标**：双 barrier 模型（设计文档 §7）。常规同步走 `barrier(stage,hazard)`；显式 layout/queue/present 走 `resourceBarrier`；
layout 由后端 tracker 推断。**`transitionTexture/transitionBuffer` 重塑为 `resourceBarrier`，非废弃删除**。
**前置依赖**：Wave 6（所有 pass 已用 CommandBuffer）。

**改动文件与步骤**：
- [ ] `rhi/vulkan/VulkanCommandBuffer.cpp`：实现 `barrier(StageFlags, StageFlags, HazardFlags)`（主路径）
  - `ToVkPipelineStageFlags2(StageFlags)` + `InferAccessFlags(StageFlags, HazardFlags, isProducer)` → `VkMemoryBarrier2`
  - 装入 `VkDependencyInfo`（**字段为 `pMemoryBarriers`**，勿写成 `pMemoryMemoryBarriers`）→ `vkCmdPipelineBarrier2`
- [ ] `rhi/vulkan/VulkanCommandBuffer.cpp`：实现 `resourceBarrier(TextureBarrier[], BufferBarrier[])`（特殊路径）
  - 由现有 `transitionTexture/transitionBuffer` 逻辑重塑而来（`VkImageMemoryBarrier2`/`VkBufferMemoryBarrier2`），支持 `before/after` layout 与 `srcQueue/dstQueue` ownership
- [ ] `rhi/vulkan/VulkanCommandBuffer.cpp`：新增 **per-texture layout tracker**（per-command-buffer 数组/`unordered_map<texture,VkImageLayout>`，仅录制期内部用，非热路径解析）
  - 在 `beginRenderPass`（color/depth target）/`blitTexture`/present 时按目标 layout 自动补 `VkImageMemoryBarrier2`
  - layout 目标由依赖的 `requiredState`（`PassResourceDependency.requiredState`，`Pass.h:55`）映射
- [ ] `render/PassExecutor.cpp:239-336`：发射动词替换
  - 常规依赖 → `context.cmdBuffer->barrier(producerStage, consumerStage, hazards)`（§0.3 映射表）
  - 需显式 layout/queue/present 的边界 → `context.cmdBuffer->resourceBarrier(...)`
  - 保留 `requiresBarrier(prev,next)` 去重（read→read 不插）
- [ ] **双重 barrier 防护**：同一资源在单次 producer→consumer 边界仅一条 barrier；pass 不得对已被 PassExecutor 覆盖的资源再手写（仅 intrapass override）
- [ ] `rhi/RHICommandList.h`：`memoryBarrier`/`setResourceState` 加 `[[deprecated]]`（`transitionTexture/transitionBuffer` 的能力迁移到 `resourceBarrier`，旧入口 deprecated）

**验证**：
- 编译通过；Vulkan validation layer **零 layout/sync 报错**；
- RenderDoc：barrier 数量较迁移前不增多（无冗余）、关键边界（culling→indirect、GBuffer→lighting、blit→present）均有正确 barrier；
- 画面一致。

**提交**：`refactor(rhi,render): dual barrier model — StageBarrier main path + ResourceBarrier special path`

---

## 11. Wave 8：ArgumentTable 全面替换 BindGroup

**目标**：业务层与 RenderDevice 的 BindGroup/BindTable → ArgumentLayout/ArgumentTable 句柄。
**前置依赖**：Wave 7。

**改动文件与步骤**：
- [ ] `render/BindGroups.h:34-41`：`BindGroupDesc` 的 `rhi::BindTableLayout* layout` / `rhi::BindTable* table` → `rhi::ArgumentLayoutHandle layout` / `rhi::ArgumentTableHandle table`（**字段替换**）
- [ ] `render/RenderDevice.h/.cpp`：
  - 成员 `VkDescriptorSetLayout`→`ArgumentLayoutHandle`；`VkDescriptorSet`→`ArgumentTableHandle`
  - 删除 `getXXXPipelineLayout()`（返回 `PipelineLayoutHandle` 的方法，layout 由后端内部管理）
  - `getXXXDescriptorSet()`（返回 `uint64_t`）→ 返回 `ArgumentTableHandle`（如 `getLightingInputBindGroup`，`GPUDrivenRenderer.h:763`）
  - `createBindGroupLayout`/`createBindGroup`→`createArgumentLayout`/`createArgumentTable`
  - `SamplerCache`（内部 `VkSampler` map）→ `rhi::Device::createSampler`
- [ ] 绑定模式：沿用现状"per-frame 持久 table + dynamic offset"（不每帧重建 table），保留 per-frame 数组（`GPUDrivenRenderer.cpp:2312/2350`）；per-frame 内容更新仍走 `updateArgumentTable`（`GPUDrivenRenderer.cpp:3195-3267` 的等价路径）
- [ ] 过渡别名：`using BindGroupHandle = rhi::ArgumentTableHandle;`（仅句柄层，不替代上面字段替换）

**验证**：
- 编译通过；
- material 贴图绑定、per-frame camera/scene dynamic offset、bindless 全局组（`globalBindlessGroup`）渲染正确；
- 验证 O(1) resolve（无 per-frame hashmap 查找热点）。

**提交**：`refactor(render): replace BindGroup/BindTable with ArgumentLayout/ArgumentTable handles`

---

## 12. Wave 9：QueryPool + 清理兼容层

**目标**：QueryPool 句柄化；删除所有 deprecated 接口与 native escape hatch。
**前置依赖**：Wave 8。

**改动文件与步骤**：
- [ ] `render/RenderDevice.h`：`VkQueryPool queryPool`→`rhi::QueryPoolHandle`；移除 `VkPipelineLayout`/`VkDescriptorSet`/`VkFence`/`VkCommandBuffer`/`VkCommandPool` 成员（改由 `FrameContext`/`CommandBuffer` 管理）
- [ ] 删除 `rhi/RHICommandList.h`（基类）与 `rhi/vulkan/VulkanCommandList.h/.cpp` 的 escape hatch（`getNativeCommandBuffer()` 及 `cmd*` 自由函数）；若 `VulkanCommandList` 仍作内部实现则重命名 `VulkanCommandBuffer`
- [ ] 清理 TRACY native 泄漏（不变量 #11）：`PassExecutor.cpp:236` 的 `rhi::vulkan::getNativeCommandBuffer` 改走 RHI 提供的 profiling 接口（如 `CommandBuffer` 暴露后端无关的 tracy 上下文），消除 `render/` 对 `rhi/vulkan/*` 的依赖
- [ ] 删除 `rhi/RHIHandles.h` 的 `TextureViewHandle::fromNativePtr/toNativePtr`
- [ ] 加 CI 守卫（落实 §0.9 不变量）：
  - 禁止 `render/`、业务层 `#include "rhi/vulkan|d3d12|metal/*"`（#2）
  - 解析热路径（`VulkanCommandList`/`VulkanResourceTable`/`render/`）无 `unordered_map`（#3）
  - public RHI 头无 `Vk*`/`ID3D12*`/`MTL*`/`Vma*`（#1）

**验证**：
- 全量编译通过、**无 `[[deprecated]]` 警告**；
- `grep -r 'rhi/vulkan' render/` 为空；
- benchmark：迁移前后帧时间无回归（±2% 内），GPU timestamp（经新 QueryPool）正常输出。

**提交**：`refactor(rhi,render): handle-ize query pool; remove CommandList compat layer and native escape hatches`

---

## 13. 风险与回滚

| 风险 | 影响 | 缓解 |
|------|------|------|
| 借迁移之名重写已工作的 device-address/GPUScene 路径致回归 | 高 | §0.2 原则：包装不重造；Wave 2 RenderDoc 逐 buffer 对比 |
| StageBarrier 后端 layout 推断出错 | 高 | 用正式 `resourceBarrier` verb 做显式 layout 兜底（非 deprecated）；validation layer 全程开 |
| 双重 barrier（PassExecutor 自动 + Pass 手写） | 中 | §0.3 禁止；Wave 7 同提交内移除旧 transition |
| ArgumentTable 句柄化破坏 per-frame 绑定 | 高 | 沿用持久 table + dynamic offset；Wave 8 单独验证 |
| Encoder 引入额外开销 | 中 | 单 `VkCommandBuffer`，encoder 切换仅内联/虚调用，`beginRenderPass`→`vkCmdBeginRendering` |
| input-attachment 预留增加接口复杂度 | 低 | Wave 1 仅实现回退路径，Pass 暂不使用 |
| 每 Wave 改动过大 | 低 | 每 Wave 文件数控制在 10-20，单独 commit |

**回滚**：每 Wave 一个 commit；验证失败回退上一 Wave；兼容层保留至 Wave 9。

---

## 14. 关键决策记录

| # | 决策 | 结论 |
|---|------|------|
| 1 | 后端实现顺序 | **Vulkan-first**；D3D12/Metal stub。接口保持三后端可映射（Wave 0 非阻塞自查） |
| 2 | barrier 模型 | 双 barrier：`barrier`(stage/hazard) 主路径 + `resourceBarrier` 正式特殊路径；PassExecutor 集中生成（RenderGraph-lite），手写仅 intrapass override |
| 3 | CommandBuffer 获取 | `FrameContext::getCommandBuffer()`，不在 Device create/destroy |
| 4 | GpuPtr | 包装既有 device address，非新引入；只表示 buffer 地址（texture 走 view handle） |
| 5 | per-draw 主数据通道 | root pointer（GpuPtr）；RenderEncoder 的 `setRoot*`/`setDynamicBuffer` 带 `ShaderStage`，ComputeEncoder 不带（阶段恒定）；`setRootConstants` 仅小数据 fast path |
| 6 | ArgumentTable | 句柄化 + HandlePool；持久 table + dynamic offset，不每帧重建 |
| 7 | 销毁语义 | 全部走延迟删除队列 |
| 8 | input-attachment/local-read | 接口预留 + Vulkan 回退实现；tile 驻留优化为后续里程碑 |
| 9 | 多线程录制 | 接口形状预留；本次仍单线程 |
| 10 | 执行顺序（方案 A） | 纯 Vulkan-first；mock-first 仅作 Wave 0 非阻塞接口自查（对照三后端映射表） |
| 11 | 多后端不变量 | §0.9 的 13 条作为各 Wave 验收门（⛔ 强制门违反不可合并） |
| 12 | 范围外（接口预留不冲突） | async compute/多队列、descriptor_buffer/DescriptorHeap、GpuResourceRef、PipelineCompiler、Capability tier 矩阵、移动端 tier 选路、ResidencySet 实体、tile-resident deferred |

---

## 15. 执行检查清单（启动前）

- [ ] git 干净，创建分支 `feature/modern-rhi`
- [ ] Vulkan SDK ≥ 1.3（synchronization2 / dynamic rendering / buffer device address）
- [ ] RenderDoc 可用（barrier/layout/capture 对比）
- [ ] 确认 `VK_KHR_buffer_device_address` 当前启用状态（Wave 1 据此决定是否补启用）
