# Metal Backend Implementation Guide

This directory contains the Metal 3+ backend scaffold for the Demo RHI (Render Hardware Interface). This is an **INTERFACE ONLY** implementation - all methods are stubs with detailed Metal mapping strategy notes.

## Metal 3+ Requirements

- **Minimum OS**: macOS 14+ or iOS 17+
- **Metal API**: Metal 3+ (argument buffers, dynamic libraries, indirect drawing)
- **Feature Set**: Apple GPU Family 3+ for full Tier 3 support

## Metal Architecture Overview

Metal has significant architectural differences from Vulkan:

| Concept | Vulkan | Metal |
|---------|--------|-------|
| Device | Instance + PhysicalDevice + LogicalDevice | id<MTLDevice> |
| Queues | Separate graphics/compute/transfer queue families | Single command queue with encoders |
| Synchronization | Timeline semaphores, fences | Completion handlers, MTLSharedEvent |
| Command Recording | Command buffers (begin/end) | Command buffers with encoders |
| Bindless | Descriptor indexing | Argument buffers |
| Pipeline State | PSO with pipeline layout | PSO with implicit layout |
| Presentation | Swapchain with semaphores | CAMetalLayer with drawables |

## Bindless Resource Mapping Strategy

### Argument Buffers vs Descriptor Indexing

Metal uses **argument buffers** for bindless resource access, not descriptor indexing:

```cpp
// Vulkan approach (descriptor indexing)
vkCmdBindDescriptorSets(..., descriptorSet, ...);

// Metal approach (argument buffers)
[encoder setBuffer:argumentBuffer offset:0 atIndex:0];
// Shader: [[buffer(0)]] ArgumentBuffer argBuffer;
```

### Argument Buffer Structure

```metal
// Metal shader declaration
struct ArgumentBuffer {
  texture2d<float> textures[MAX_TEXTURES];
  device Buffer     buffers[MAX_BUFFERS];
  sampler           samplers[MAX_SAMPLERS];
};

[[buffer(N)]] ArgumentBuffer argBuffer;
```

### Bind Table Mapping

| RHI Concept | Metal Implementation |
|-------------|---------------------|
| BindTableLayout | MTLArgumentEncoder |
| BindTable | MTLBuffer (containing resource pointers) |
| Logical Index | Array index in argument buffer |
| Update | MTLArgumentEncoder setTexture/setBuffer/setSampler |

### Example Metal API Usage

```objc
// Create argument encoder
MTLArgumentDescriptor* texArgDesc = [MTLArgumentDescriptor argumentDescriptor];
texArgDesc.dataType = MTLDataTypeTexture;
texArgDesc.arrayLength = MAX_TEXTURES;
MTLArgumentEncoder* encoder = [device newArgumentEncoderWithArguments:@[texArgDesc]];

// Create argument buffer
id<MTLBuffer> argBuffer = [device newBufferWithLength:encoder.encodedLength options:0];

// Update resources
[encoder setArgumentBuffer:argBuffer offset:0];
[encoder setTexture:texture0 atIndex:0];
[encoder setTexture:texture1 atIndex:1];

// Bind during rendering
[renderEncoder setBuffer:argBuffer offset:0 atIndex:0];
```

## Command Recording Architecture

### Encoder Hierarchy

Metal uses a hierarchical encoder system:

```
MTLCommandQueue
  └─ MTLCommandBuffer
      ├─ MTLRenderCommandEncoder (rendering commands)
      ├─ MTLComputeCommandEncoder (compute commands)
      └─ MTLBlitCommandEncoder (copy operations)
```

### Key Differences from Vulkan

1. **No explicit begin/end**: Command buffers are created from queues
2. **Encoder boundaries**: Different work types require different encoders
3. **Implicit synchronization**: Metal tracks resource dependencies automatically
4. **No barriers**: Resource state transitions are implicit

### Example Command Recording

```objc
// Create command buffer
id<MTLCommandBuffer> cmdBuffer = [queue commandBuffer];

// Create render encoder
MTLRenderPassDescriptor* renderDesc = [MTLRenderPassDescriptor renderPassDescriptor];
renderDesc.colorAttachments[0].texture = colorTexture;
id<MTLRenderCommandEncoder> renderEncoder = [cmdBuffer renderCommandEncoderWithDescriptor:renderDesc];

// Record commands
[renderEncoder setRenderPipelineState:pipeline];
[renderEncoder setViewport:viewport];
[renderEncoder drawPrimitives:MTLPrimitiveTypeTriangle vertexStart:0 vertexCount:3];

// End encoding
[renderEncoder endEncoding];

// Submit with completion handler
[cmdBuffer addCompletedHandler:^(id<MTLCommandBuffer> buffer) {
    // Frame complete
}];
[cmdBuffer commit];
```

## Presentation Architecture

### CAMetalLayer vs Swapchain

Metal uses `CAMetalLayer` for presentation, not a swapchain object:

| Vulkan Concept | Metal Equivalent |
|----------------|-----------------|
| VkSwapchainKHR | CAMetalLayer |
| vkAcquireNextImageKHR | [layer nextDrawable] |
| vkQueuePresentKHR | [cmdBuffer presentDrawable:] |
| ImageAvailable semaphore | Not needed (implicit) |
| RenderFinished semaphore | Not needed (implicit) |

### Presentation Flow

```objc
// Initialize CAMetalLayer
CAMetalLayer* layer = (CAMetalLayer*)view.layer;
layer.device = device;
layer.pixelFormat = MTLPixelFormatBGRA8Unorm;
layer.framebufferOnly = YES;

// Acquire (frame start)
id<CAMetalDrawable> drawable = [layer nextDrawable];
MTLTexture* texture = drawable.texture;

// Render to texture
// ... render commands ...

// Present (frame end)
[cmdBuffer presentDrawable:drawable];
[cmdBuffer commit];
```

## Synchronization Strategy

### Timeline Semaphore Emulation

Metal doesn't have timeline semaphores. Use completion handlers:

```objc
// Frame tracking with monotonic counter
uint64_t frameCounter = 0;

[cmdBuffer addCompletedHandler:^(id<MTLCommandBuffer> buffer) {
    // Update completion tracking
    frameComplete[frameCounter] = true;
}];

[cmdBuffer commit];
frameCounter++;
```

### MTLSharedEvent (Alternative)

For cross-queue synchronization, use `MTLSharedEvent`:

```objc
id<MTLSharedEvent> sharedEvent = [device newSharedEvent];

// Signal
[commandBuffer encodeSignalEvent:sharedEvent value:signalValue];

// Wait
[commandBuffer encodeWaitForEvent:sharedEvent value:waitValue];
```

## Pipeline Creation

### Graphics Pipeline

```objc
MTLRenderPipelineDescriptor* desc = [MTLRenderPipelineDescriptor renderPipelineDescriptor];
desc.vertexFunction = vertexFunction;
desc.fragmentFunction = fragmentFunction;
desc.colorAttachments[0].pixelFormat = MTLPixelFormatBGRA8Unorm;

// Vertex descriptor (input layout)
MTLVertexDescriptor* vertexDesc = [MTLVertexDescriptor vertexDescriptor];
vertexDesc.attributes[0].format = MTLVertexFormatFloat3;
desc.vertexDescriptor = vertexDesc;

// Create PSO
id<MTLRenderPipelineState> pso = [device newRenderPipelineStateWithDescriptor:desc error:&error];
```

### Compute Pipeline

```objc
MTLComputePipelineDescriptor* desc = [MTLComputePipelineDescriptor computePipelineDescriptor];
desc.computeFunction = computeFunction;

id<MTLComputePipelineState> pso = [device newComputePipelineStateWithFunction:computeFunction error:&error];
```

### Pipeline Layout

Metal doesn't have explicit pipeline layout objects. Layout is defined by shader function signatures:

```metal
// Shader declares argument buffer binding
[[buffer(0)]] ArgumentBuffer argBuffer;
[[buffer(1)]] constant UniformData& uniforms;
[[texture(0)]] texture2d<float> colorTexture;
```

## Capability Mapping

### Metal Feature Sets to RHI Tiers

| Metal Feature Set | RHI Capability Tier | Features |
|------------------|---------------------|----------|
| Apple GPU Family 3+ | Tier 3 (Full) | Full Metal 3+, argument buffers, mesh shaders (emulated) |
| Apple GPU Family 2+ | Tier 2 (Enhanced) | Argument buffers, indirect drawing |
| Other Apple GPUs | Tier 1 (Basic) | Core Metal features |

### Feature Detection

```objc
// Check GPU family
if ([device supportsFamily:MTLGPUFamilyApple3]) {
    // Tier 3 capabilities
}

// Check specific features
BOOL supportsArgumentBuffers = [device supportsArgumentBuffers];
BOOL supportsDynamicLibraries = [device supportsDynamicLibraries];
BOOL supportsIndirectDrawing = [device supportsFeatureSet:MTLFeatureSet_iOS_GPUFamily5];
```

## Memory Model

### Unified Memory Architecture

Metal uses a unified memory model (CPU and GPU share same address space):

- **No separate heaps**: All memory is accessible from both CPU and GPU
- **Storage modes**:
  - `MTLStorageModeShared`: CPU and GPU accessible (default)
  - `MTLStorageModePrivate`: GPU-only (requires staging)
  - `MTLStorageModeManaged`: CPU writes, GPU reads (macOS only)

### Buffer Allocation

```objc
// Shared mode (CPU/GPU accessible)
id<MTLBuffer> buffer = [device newBufferWithLength:byteLength options:0];

// Private mode (GPU-only, requires staging)
id<MTLBuffer> privateBuffer = [device newBufferWithLength:byteLength options:MTLResourceStorageModePrivate];

// Managed mode (macOS only)
id<MTLBuffer> managedBuffer = [device newBufferWithLength:byteLength options:MTLResourceStorageModeManaged];
```

## Push Constants

Metal doesn't have push constants. Use alternatives:

1. **setVertexBytes/setFragmentBytes** (max 4KB):
```objc
[renderEncoder setVertexBytes:constants length:size atIndex:0];
```

2. **Inline argument buffer data**:
```objc
[encoder setBuffer:inlineBuffer offset:0 atIndex:0];
```

3. **Uniform buffer**:
```objc
[renderEncoder setVertexBuffer:uniformBuffer offset:0 atIndex:0];
```

## Dynamic State

Metal supports dynamic state differently:

| State | Vulkan | Metal |
|-------|--------|-------|
| Viewport | vkCmdSetViewport | setViewport: |
| Scissor | vkCmdSetScissor | setScissorRect: |
| Blend Constants | vkCmdSetBlendConstants | Not dynamic (in PSO) |
| Depth Bias | vkCmdSetDepthBias | Not dynamic (in PSO) |
| Stencil Reference | vkCmdSetStencilReference | setStencilReferenceValue: |

## Implementation Notes

### ARC (Automatic Reference Counting)

Metal uses Objective-C ARC for memory management:

```objc
// Objects are automatically retained/released
id<MTLDevice> device = MTLCreateSystemDefaultDevice();
// ARC handles release when device goes out of scope
```

### Interop with C++

```cpp
// Cast to/from Objective-C types
id<MTLDevice> device = (__bridge id<MTLDevice>)nativeDeviceHandle;
void* handle = (__bridge_retained void*)device;
```

### Thread Safety

- **MTLCommandQueue**: Thread-safe, can be used from any thread
- **MTLCommandBuffer**: Not thread-safe, use from one thread
- **MTLResources**: Thread-safe for read, write requires synchronization

## Resources

- [Metal Programming Guide](https://developer.apple.com/metal/Metal-Programming-Guide/)
- [Metal Shading Language Specification](https://developer.apple.com/metal/Metal-Shading-Language-Specification.pdf)
- [Metal Feature Set Tables](https://developer.apple.com/metal/Metal-Feature-Set-Tables.pdf)

## Implementation Status

- ✅ Interface scaffold created
- ⏳ Full implementation pending
- ⏳ Testing required on Apple hardware

## Known Limitations

1. **No timeline semaphores**: Use completion handlers or MTLSharedEvent
2. **No mesh shaders**: Emulate with compute + render pass
3. **No ray tracing**: Use Metal Ray Tracing (separate API)
4. **Push constants limited**: Use setVertexBytes/setFragmentBytes (4KB max)
5. **Dynamic state limited**: Most state must be in PSO

## Future Work

- [ ] Full Metal API implementation
- [ ] Shader compilation pipeline (Metal Shading Language)
- [ ] Argument buffer optimization
- [ ] Multi-queue support (if applicable)
- [ ] Async compute scheduling
- [ ] MTLSharedEvent integration for cross-queue sync