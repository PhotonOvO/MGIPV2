# Modern GPU Interface Architecture

A modern, low-level Rendering Hardware Interface designed for **Vulkan 1.3+**, **DirectX 12**, and **Metal 4**.

This RHI is built around modern GPU execution models instead of mirroring a single native graphics API.  
Its core design focuses on explicit resource lifetime, GPU-driven rendering, reusable command recording, argument-table based resource binding, and pass-level synchronization.

---

## 1. Core Objects

The RHI is organized around a small set of explicit objects.

```cpp
Device
Queue
CommandAllocator
CommandBuffer
RenderEncoder
ComputeEncoder

Buffer
Texture
TextureView
TextureViewPool
Sampler

ArgumentTable
Pipeline
ResidencySet
QueryPool
Timeline
```

### Device

`Device` owns resource creation, backend capability queries, pipeline creation, memory allocation, and argument table management.

```cpp
class Device {
public:
    BufferHandle createBuffer(const BufferDesc&);
    TextureHandle createTexture(const TextureDesc&);
    TextureViewHandle createTextureView(TextureViewPoolHandle,
                                        TextureHandle,
                                        const TextureViewDesc&);
    SamplerHandle createSampler(const SamplerDesc&);

    ArgumentTableHandle createArgumentTable(const ArgumentTableDesc&);
    void updateArgumentTable(ArgumentTableHandle,
                             Span<const ArgumentWrite>);

    PipelineHandle createGraphicsPipeline(const GraphicsPipelineDesc&);
    PipelineHandle createComputePipeline(const ComputePipelineDesc&);
    PipelineHandle createMeshPipeline(const MeshPipelineDesc&);

    GpuPtr getBufferGpuAddress(BufferHandle);
    GpuResourceId getTextureViewGpuId(TextureViewHandle);

    DeviceCapabilities capabilities() const;
};
```

### Queue

`Queue` submits one or more command buffers.

```cpp
class Queue {
public:
    void useResidencySet(ResidencySetHandle);

    void submit(Span<const CommandBufferHandle>,
                const SubmitInfo& = {});
};
```

The queue model maps naturally to modern explicit APIs:

| RHI | Vulkan | DirectX 12 | Metal 4 |
|---|---|---|---|
| Queue | `VkQueue` | `ID3D12CommandQueue` | `MTL4CommandQueue` |
| Submit multiple command buffers | `vkQueueSubmit2` | `ExecuteCommandLists` | Group commit |

### CommandAllocator and CommandBuffer

Command recording is transient, but command buffer objects can be reused.

```cpp
class CommandAllocator {
public:
    void reset();
};

class CommandBuffer {
public:
    void begin(CommandAllocatorHandle);
    void end();

    void useResidencySet(ResidencySetHandle);

    RenderEncoder beginRenderPass(const RenderPassDesc&);
    ComputeEncoder beginComputePass(const ComputePassDesc& = {});

    void barrier(StageFlags producer,
                 StageFlags consumer,
                 HazardFlags hazards);

    void resourceBarrier(Span<const TextureBarrier>,
                         Span<const BufferBarrier>);
};
```

The intended model is:

```text
Reusable CommandBuffer object
+ Per-frame CommandAllocator
+ Transient command contents
```

This maps well to:

| RHI | Vulkan | DirectX 12 | Metal 4 |
|---|---|---|---|
| CommandAllocator | Command pool / frame pool | `ID3D12CommandAllocator` | `MTL4CommandAllocator` |
| CommandBuffer | `VkCommandBuffer` | `ID3D12GraphicsCommandList` | `MTL4CommandBuffer` |

---

## 2. Abstraction Model

The RHI is not a wrapper around Vulkan, DirectX 12, or Metal.

Instead, it exposes a small set of modern GPU primitives:

```text
Stable typed handles
GPU buffer addresses
Argument tables
Texture view pools
Reusable command buffers
Render / compute encoders
Explicit stage barriers
Backend capability tiers
```

### Handles

All public resources are referenced by typed handles.

```cpp
template <typename Tag>
struct Handle {
    uint32_t index;
    uint32_t generation;
};

using BufferHandle          = Handle<BufferTag>;
using TextureHandle         = Handle<TextureTag>;
using TextureViewHandle     = Handle<TextureViewTag>;
using TextureViewPoolHandle = Handle<TextureViewPoolTag>;
using SamplerHandle         = Handle<SamplerTag>;
using PipelineHandle        = Handle<PipelineTag>;
using ArgumentTableHandle   = Handle<ArgumentTableTag>;
using ResidencySetHandle    = Handle<ResidencySetTag>;
```

Handles are stable logical references.  
They are not native API objects.

A buffer handle may keep the same logical identity while its backend allocation is updated.  
This is useful for per-frame resources such as:

- Culling indirect command buffers.
- Draw count buffers.
- Shadow culling buffers.
- Meshlet index buffers.
- Transient GPU memory.

### Hot / Cold Resource Data

Resource pools should split hot and cold data.

```cpp
struct BufferHot {
    BackendBufferHandle native;
    uint64_t gpuAddress;
    uint32_t descriptorIndex;
    uint32_t flags;
};

struct BufferCold {
    BufferDesc desc;
    MemoryAllocation allocation;
    void* mappedPtr;
    DebugName debugName;
};
```

Hot data is used by command recording.  
Cold data is used by debugging, destruction, validation, and resource inspection.

This keeps the hot command path compact and cache-friendly.

### GPU Pointer

The RHI exposes GPU buffer addresses through `GpuPtr`.

```cpp
struct GpuPtr {
    uint64_t value;
};
```

`GpuPtr` represents GPU-addressable **buffer memory**.

It does not represent:

- Textures.
- Samplers.
- Acceleration structures.
- Arbitrary native API objects.

Texture and sampler references are represented through descriptor indices, argument table entries, or backend-specific GPU resource IDs.

---

## 3. Resource Binding

The RHI uses an **ArgumentTable-first** binding model.

Argument tables store resource bindings such as buffers, texture views, and samplers.  
Encoders bind argument tables instead of binding individual native resources directly.

### RenderEncoder Binding

```cpp
class RenderEncoder {
public:
    void setPipeline(PipelineHandle);

    void setArgumentTable(ShaderStage stages,
                          uint32_t slot,
                          ArgumentTableHandle table);

    void setRootConstants(ShaderStage stages,
                          uint32_t slot,
                          const void* data,
                          uint32_t size);

    void setRootPointer(ShaderStage stages,
                        uint32_t slot,
                        GpuPtr ptr);
};
```

`RenderEncoder` keeps `ShaderStage` because graphics and mesh pipelines may contain multiple stages.

Examples:

- Vertex
- Fragment / Pixel
- Mesh
- Task / Amplification

### ComputeEncoder Binding

```cpp
class ComputeEncoder {
public:
    void setPipeline(PipelineHandle);

    void setArgumentTable(uint32_t slot,
                          ArgumentTableHandle table);

    void setRootConstants(uint32_t slot,
                          const void* data,
                          uint32_t size);

    void setRootPointer(uint32_t slot,
                        GpuPtr ptr);
};
```

`ComputeEncoder` does not need a `ShaderStage` parameter because compute visibility is implicit.

### Argument Writes

```cpp
enum class ArgumentType {
    uniformBuffer,
    storageBuffer,
    sampledTexture,
    storageTexture,
    sampler,
    accelerationStructure,
    indirectCommandBuffer,
};

struct ArgumentWrite {
    uint32_t binding;
    uint32_t arrayElement;
    ArgumentType type;

    BufferHandle buffer;
    TextureViewHandle textureView;
    SamplerHandle sampler;

    uint64_t offset;
    uint64_t size;
};
```

Argument writes accept RHI handles, not native descriptors.

### Frequency-based Binding Layout

A typical renderer may organize argument tables by update frequency:

| Slot | Purpose |
|---|---|
| 0 | Pass globals |
| 1 | Material resources |
| 2 | Shader / pass specific resources |
| 3 | Dynamic draw data / root pointer |

This keeps low-frequency resource binding out of the draw loop.

### Backend Mapping

| RHI Concept | Vulkan 1.3+ | DirectX 12 | Metal 4 |
|---|---|---|---|
| ArgumentTable | Descriptor set / descriptor buffer range | Root descriptor table / descriptor heap range | `MTL4ArgumentTable` |
| Buffer GPU address | Buffer Device Address | GPU Virtual Address | `MTLBuffer.gpuAddress` |
| Texture view ID | Descriptor index / image view | SRV / UAV descriptor index | `MTLResourceID` |
| TextureViewPool | Descriptor buffer range | Descriptor heap range | `MTLTextureViewPool` |

---

## 4. Command Recording

The public RHI exposes two encoder types:

```text
RenderEncoder
ComputeEncoder
```

There is no public `CopyEncoder`.

Copy, transfer, fill, indirect preparation, and acceleration-structure build operations are modeled as compute/transfer workloads under `ComputeEncoder`.

```cpp
class ComputeEncoder {
public:
    void dispatch(const DispatchDesc&);
    void dispatchIndirect(const DispatchIndirectDesc&);

    void copyBuffer(const CopyBufferDesc&);
    void copyBufferToTexture(const CopyBufferToTextureDesc&);
    void copyTextureToBuffer(const CopyTextureToBufferDesc&);
    void copyTexture(const CopyTextureDesc&);
    void fillBuffer(const FillBufferDesc&);

    // Future optional path.
    void buildAccelerationStructure(const BuildAccelerationStructureDesc&);
};
```

The public API treats copy as a workload category, not as a separate encoder type.

Backend implementations are free to route these commands internally:

| RHI Operation | Vulkan | DirectX 12 | Metal 4 |
|---|---|---|---|
| Dispatch | Compute / graphics queue | Compute / direct command list | Compute encoder |
| Buffer copy | Transfer / graphics queue | Copy / direct queue | Compute encoder copy command |
| Buffer to texture | Transfer command | `CopyTextureRegion` | Compute encoder copy command |
| Texture to texture | Copy / blit command | `CopyTextureRegion` | Compute encoder copy command |
| Acceleration structure build | AS build command | Raytracing AS build | Compute encoder AS path |

A `ComputeEncoder` does not necessarily mean the backend must use a hardware compute queue.  
Queue selection is a backend policy.

---

## 5. Resource and Memory Model

The RHI distinguishes between stable logical resources and changing backend allocations.

### Long-lived Resources

Examples:

- Mesh vertex buffers.
- Mesh index buffers.
- Persistent material buffers.
- Static textures.
- Pipeline resources.

These are created once and kept alive for a long time.

```cpp
struct MeshRecord {
    BufferHandle vertexBuffer;
    BufferHandle indexBuffer;
    uint64_t vertexOffset;
    uint64_t indexOffset;
    uint32_t indexCount;
};
```

### Per-frame Resources

Examples:

- GPU culling indirect command buffers.
- Draw count buffers.
- Shadow culling buffers.
- Meshlet index buffers.
- Frame transient buffers.

Per-frame resources use stable handles per frame slot.  
When the backend buffer is rebuilt or rotated, the resource record is updated without changing the handle.

```cpp
struct BufferBackingUpdate {
    BackendBufferHandle native;
    MemoryAllocation allocation;
    uint64_t gpuAddress;
    uint64_t size;
    BufferUsageFlags usage;
    void* mappedPtr;
};

void updateBuffer(BufferHandle handle,
                  const BufferBackingUpdate& update);
```

Recommended rule:

```text
BufferHandle generation changes only when the logical buffer is destroyed.
Backing version changes when the native allocation changes.
```

This avoids handle churn in high-frequency per-frame paths.

### Transient Allocations

Transient allocations return a stable buffer handle plus an offset and GPU pointer.

```cpp
struct TransientGpuAllocation {
    BufferHandle buffer;
    uint64_t offset;
    uint64_t size;
    void* cpu;
    GpuPtr gpu;
};
```

A transient ring buffer should not create a new handle for every allocation.

Instead:

```text
Stable BufferHandle
+ Changing offset
+ GpuPtr for shader access
```

---

## 6. Synchronization

The RHI does not perform automatic per-draw resource state tracking.

Synchronization is explicit and pass-oriented.

### Stage Barrier

```cpp
cmd.barrier(StageFlags::compute,
            StageFlags::fragmentShader,
            HazardFlags::bufferWrites);
```

Stage barriers express producer / consumer relationships between pipeline stages.

They are the primary synchronization primitive for:

- GPU-driven workloads.
- Bindless resource access.
- Indirect argument generation.
- Compute-to-render dependencies.
- Pass-level hazards.

Example:

```cpp
// Compute culling writes indirect arguments.
// Graphics command input reads them later.
cmd.barrier(StageFlags::compute,
            StageFlags::commandInput,
            HazardFlags::drawArguments);
```

### Resource Barrier

Resource barriers are still available for special cases:

```cpp
cmd.resourceBarrier(textureBarriers, bufferBarriers);
```

They are used for:

- Texture layout transitions.
- Swapchain present transitions.
- Queue ownership transfers.
- Transient resource aliasing.
- Depth / stencil transitions.
- Debug validation.
- Backend-specific fallback paths.

Copy commands do not implicitly transition resources for future use.  
The caller or RenderGraph must emit the required barriers.

---

## 7. Multi-backend Mapping

The RHI is designed around a common modern GPU model.

| Feature | Vulkan 1.3+ | DirectX 12 | Metal 4 |
|---|---|---|---|
| Command buffers | `VkCommandBuffer` | Command list | `MTL4CommandBuffer` |
| Command memory | Command pool | Command allocator | `MTL4CommandAllocator` |
| Graphics work | Dynamic rendering / render pass | Direct command list | `MTL4RenderCommandEncoder` |
| Compute / copy work | Compute / transfer commands | Compute / direct / copy queues | `MTL4ComputeCommandEncoder` |
| Binding model | Descriptor set / descriptor buffer | Descriptor heap / root table | `MTL4ArgumentTable` |
| Texture views | Image views / descriptor indices | SRV / UAV descriptors | `MTLTextureViewPool` |
| Synchronization | Synchronization2 | Enhanced barriers | Next-generation barriers |
| Residency | Manual policy / sparse | Residency management | Residency sets |
| GPU buffer address | Buffer Device Address | GPU Virtual Address | `MTLBuffer.gpuAddress` |

The public API does not expose native handles such as:

- `VkBuffer`
- `VkImage`
- `VkDescriptorSet`
- `ID3D12Resource`
- `ID3D12DescriptorHeap`
- `MTLResource`
- `MTLTexture`

Backend-specific APIs stay inside backend implementation files.

---