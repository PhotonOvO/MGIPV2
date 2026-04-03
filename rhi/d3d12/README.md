# D3D12 Backend Implementation Guide

This directory contains the DirectX 12 backend scaffold for the Demo RHI (Render Hardware Interface). This is an **INTERFACE ONLY** implementation - all methods are stubs with detailed D3D12 mapping strategy notes.

## D3D12 Requirements

- **Minimum OS**: Windows 10 1809 (Build 17763) or later
- **DirectX 12 API**: DirectX 12 (D3D12) with Feature Level 12.0+ recommended
- **Windows SDK**: Windows 10 SDK 19041 or later
- **Feature Level**: D3D_FEATURE_LEVEL_12_0+ recommended (12_2 for full Tier 3)

## D3D12 Architecture Overview

D3D12 has significant architectural differences from Vulkan:

| Concept | Vulkan | D3D12 |
|---------|--------|-------|
| Device | Instance + PhysicalDevice + LogicalDevice | IDXGIAdapter4 + ID3D12Device |
| Queues | Separate queue families | Command queues (graphics/compute/transfer) |
| Synchronization | Timeline semaphores, fences | ID3D12Fence (GetCompletedValue/SetEventOnCompletion) |
| Command Recording | Command buffers (begin/end) | Command allocators + command lists |
| Bindless | Descriptor indexing | Descriptor heaps with root signatures |
| Pipeline State | PSO with pipeline layout | PSO with root signature |
| Presentation | Swapchain with semaphores | IDXGISwapChain3 with fences |

## Bindless Resource Mapping Strategy

### Descriptor Heaps vs Descriptor Indexing

D3D12 uses **descriptor heaps** organized by type for bindless resource access:

```cpp
// Vulkan approach (descriptor indexing)
vkCmdBindDescriptorSets(..., descriptorSet, ...);

// D3D12 approach (descriptor heaps)
cmdList->SetDescriptorHeaps(heapCount, heaps);
cmdList->SetGraphicsRootDescriptorTable(rootParamIndex, heapBaseHandle);
```

### Descriptor Heap Types

D3D12 provides separate heap types for different descriptor kinds:

| Heap Type | D3D12 Type | Usage |
|-----------|-----------|-------|
| CBV_SRV_UAV | D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV | Constant buffers, shader resource views, unordered access views |
| Sampler | D3D12_DESCRIPTOR_HEAP_TYPE_SAMPLER | Samplers |
| RTV | D3D12_DESCRIPTOR_HEAP_TYPE_RTV | Render target views (CPU-only) |
| DSV | D3D12_DESCRIPTOR_HEAP_TYPE_DSV | Depth stencil views (CPU-only) |

### Bind Table Mapping

| RHI Concept | D3D12 Implementation |
|-------------|---------------------|
| BindTableLayout | Root signature descriptor table parameters |
| BindTable | Offset into global descriptor heap |
| Logical Index | Offset within descriptor table |
| Update | Copy descriptors to heap using CopyDescriptorsSimple |

### Example D3D12 API Usage

```cpp
// Create global descriptor heaps
D3D12_DESCRIPTOR_HEAP_DESC cbvSrvUavHeapDesc = {};
cbvSrvUavHeapDesc.Type = D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV;
cbvSrvUavHeapDesc.NumDescriptors = 10000;  // Large for bindless
cbvSrvUavHeapDesc.Flags = D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE;
device->CreateDescriptorHeap(&cbvSrvUavHeapDesc, IID_PPV_ARGS(&cbvSrvUavHeap));

// Create descriptor for texture
D3D12_SHADER_RESOURCE_VIEW_DESC srvDesc = {};
srvDesc.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
srvDesc.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D;
srvDesc.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
device->CreateShaderResourceView(resource, &srvDesc, destHandle);

// Bind during rendering
cmdList->SetDescriptorHeaps(1, &cbvSrvUavHeap);
cmdList->SetGraphicsRootDescriptorTable(0, heapStartHandle);
```

## Command Recording Architecture

### Command Allocator and Command List

D3D12 uses a two-level system for command recording:

```
ID3D12CommandQueue
  └─ ID3D12CommandAllocator (manages command memory per frame)
      └─ ID3D12GraphicsCommandList (records commands)
```

### Key Differences from Vulkan

1. **Command allocators**: Each frame-in-flight needs its own allocator
2. **No explicit begin/end**: Command lists are reset with `Reset(allocator, pso)`
3. **No render passes**: Use `OMSetRenderTargets()` to set render targets
4. **Implicit synchronization**: No explicit barriers needed (but ResourceBarrier is available)

### Example Command Recording

```cpp
// Begin frame
commandAllocator->Reset();
commandList->Reset(commandAllocator, nullptr);

// Set render targets (no beginRenderPass)
D3D12_CPU_DESCRIPTOR_HANDLE rtvHandles[] = { rtvHandle };
commandList->OMSetRenderTargets(1, rtvHandles, FALSE, nullptr);

// Clear render target
float clearColor[] = { 0.0f, 0.0f, 0.0f, 1.0f };
commandList->ClearRenderTargetView(rtvHandle, clearColor, 0, nullptr);

// Record commands
commandList->SetGraphicsRootSignature(rootSignature);
commandList->SetPipelineState(pipelineState);
commandList->DrawInstanced(3, 1, 0, 0);

// End frame
commandList->Close();
commandQueue->ExecuteCommandLists(1, &commandList);
```

## Presentation Architecture

### IDXGISwapChain3 vs VkSwapchainKHR

D3D12 uses DXGI (DirectX Graphics Infrastructure) for presentation:

| Vulkan Concept | D3D12 Equivalent |
|----------------|-----------------|
| VkSwapchainKHR | IDXGISwapChain3 |
| vkAcquireNextImageKHR | swapchain->GetCurrentBackBufferIndex() |
| vkQueuePresentKHR | swapchain->Present(syncInterval, flags) |
| ImageAvailable semaphore | Not needed (implicit in flip model) |
| RenderFinished semaphore | Not needed (implicit in flip model) |

### Presentation Flow

```cpp
// Initialize swapchain
DXGI_SWAP_CHAIN_DESC1 swapDesc = {};
swapDesc.Width = width;
swapDesc.Height = height;
swapDesc.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
swapDesc.BufferCount = 3;
swapDesc.SwapEffect = DXGI_SWAP_EFFECT_FLIP_DISCARD;  // Flip model for low latency
swapDesc.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT;
factory->CreateSwapChainForHwnd(commandQueue, hwnd, &swapDesc, nullptr, nullptr, &swapchain);

// Acquire (frame start)
UINT backbufferIndex = swapchain->GetCurrentBackBufferIndex();

// Present (frame end)
swapchain->Present(vSync ? 1 : 0, 0);
```

## Synchronization Strategy

### ID3D12Fence for Timeline Semaphore

D3D12 doesn't have timeline semaphores, but `ID3D12Fence` provides similar behavior:

```cpp
// Frame tracking with monotonic counter
uint64_t frameCounter = 0;

// Submit work
commandQueue->ExecuteCommandLists(1, &commandList);
commandQueue->Signal(fence, ++frameCounter);

// Wait for frame
if (fence->GetCompletedValue() < waitValue) {
    HANDLE fenceEvent = CreateEvent(nullptr, FALSE, FALSE, nullptr);
    fence->SetEventOnCompletion(waitValue, fenceEvent);
    WaitForSingleObject(fenceEvent, INFINITE);
    CloseHandle(fenceEvent);
}
```

### Resource Barriers

D3D12 provides explicit resource state transitions:

```cpp
// Create barrier
D3D12_RESOURCE_BARRIER barrier = {};
barrier.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
barrier.Transition.pResource = resource;
barrier.Transition.StateBefore = D3D12_RESOURCE_STATE_RENDER_TARGET;
barrier.Transition.StateAfter = D3D12_RESOURCE_STATE_PRESENT;

// Insert barrier
commandList->ResourceBarrier(1, &barrier);
```

## Pipeline Creation

### Graphics Pipeline

```cpp
D3D12_GRAPHICS_PIPELINE_STATE_DESC desc = {};
desc.pRootSignature = rootSignature;
desc.VS = { vertexShaderBytecode, vertexShaderSize };
desc.PS = { fragmentShaderBytecode, fragmentShaderSize };
desc.BlendState = blendDesc;
desc.RasterizerState = rasterizerDesc;
desc.DepthStencilState = depthStencilDesc;
desc.InputLayout = inputLayout;
desc.PrimitiveTopologyType = D3D12_PRIMITIVE_TOPOLOGY_TYPE_TRIANGLE;
desc.NumRenderTargets = 1;
desc.RTVFormats[0] = DXGI_FORMAT_R8G8B8A8_UNORM;

device->CreateGraphicsPipelineState(&desc, IID_PPV_ARGS(&pso));
```

### Compute Pipeline

```cpp
D3D12_COMPUTE_PIPELINE_STATE_DESC desc = {};
desc.pRootSignature = rootSignature;
desc.CS = { computeShaderBytecode, computeShaderSize };

device->CreateComputePipelineState(&desc, IID_PPV_ARGS(&pso));
```

### Root Signature (Pipeline Layout)

D3D12 uses root signatures to define pipeline layout:

```cpp
// Root parameters
CD3DX12_ROOT_PARAMETER1 rootParameters[2];

// Root constants (push constants)
rootParameters[0].InitAsConstants(pushConstantSize, 0, 0, D3D12_SHADER_VISIBILITY_ALL);

// Descriptor table (bindless)
CD3DX12_DESCRIPTOR_RANGE1 ranges[1];
ranges[0].Init(D3D12_DESCRIPTOR_RANGE_TYPE_SRV, 100, 0, 0, D3D12_DESCRIPTOR_RANGE_FLAG_DESCRIPTORS_VOLATILE);
rootParameters[1].InitAsDescriptorTable(1, ranges);

// Create root signature
D3D12_VERSIONED_ROOT_SIGNATURE_DESC rootSignatureDesc = {};
rootSignatureDesc.Init_1_1(2, rootParameters, 0, nullptr);

ID3DBlob* signature = nullptr;
ID3DBlob* error = nullptr;
D3D12SerializeVersionedRootSignature(&rootSignatureDesc, &signature, &error);
device->CreateRootSignature(0, signature->GetBufferPointer(), signature->GetBufferSize(), IID_PPV_ARGS(&rootSignature));
```

## Capability Mapping

### D3D12 Feature Level to RHI CapabilityTier

| D3D12 Feature Level | RHI Capability Tier | Features |
|---------------------|---------------------|----------|
| D3D_FEATURE_LEVEL_12_2 | Tier 3 (Full) | Mesh shaders, work graphs, ray tracing tier 1.1 |
| D3D_FEATURE_LEVEL_12_1 | Tier 2 (Enhanced) | Shader Model 6.6, variable rate shading |
| D3D_FEATURE_LEVEL_12_0 | Tier 1 (Basic) | Core D3D12 features |

### Feature Detection

```cpp
// Check feature level
D3D12_FEATURE_DATA_FEATURE_LEVELS featureLevels{};
device->CheckFeatureSupport(D3D12_FEATURE_FEATURE_LEVELS, &featureLevels, sizeof(featureLevels));

// Check optional features
D3D12_FEATURE_DATA_D3D12_OPTIONS options{};
device->CheckFeatureSupport(D3D12_FEATURE_D3D12_OPTIONS, &options, sizeof(options));

bool supportsTessellation = options.TessellationShader != 0;
bool supportsGeometryShader = options.GeometryShader != 0;
```

## Memory Model

### D3D12 Resource Allocation

D3D12 doesn't have a unified allocator like VMA. Resources are allocated directly:

```cpp
// Committed resource (GPU-only)
D3D12_HEAP_PROPERTIES heapProps = {};
heapProps.Type = D3D12_HEAP_TYPE_DEFAULT;
heapProps.CreationNodeMask = 1;
heapProps.VisibleNodeMask = 1;

D3D12_RESOURCE_DESC resourceDesc = {};
resourceDesc.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
resourceDesc.Width = bufferSize;
resourceDesc.Height = 1;
resourceDesc.DepthOrArraySize = 1;
resourceDesc.MipLevels = 1;
resourceDesc.Format = DXGI_FORMAT_UNKNOWN;
resourceDesc.SampleDesc.Count = 1;
resourceDesc.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;

device->CreateCommittedResource(&heapProps, D3D12_HEAP_FLAG_NONE, &resourceDesc,
                                D3D12_RESOURCE_STATE_COPY_DEST, nullptr, IID_PPV_ARGS(&resource));
```

### Placed Resources (for custom allocators)

```cpp
// Create heap
D3D12_HEAP_DESC heapDesc = {};
heapDesc.SizeInBytes = heapSize;
heapDesc.Properties.Type = D3D12_HEAP_TYPE_DEFAULT;
heapDesc.Flags = D3D12_HEAP_FLAG_ALLOW_ONLY_NON_RT_DS_TEXTURES;
device->CreateHeap(&heapDesc, IID_PPV_ARGS(&heap));

// Place resource in heap
device->CreatePlacedResource(heap, heapOffset, &resourceDesc,
                               D3D12_RESOURCE_STATE_COPY_DEST, nullptr, IID_PPV_ARGS(&resource));
```

## Push Constants

D3D12 uses root constants (limited to 256 bytes):

```cpp
// Define in root signature
rootParameters[0].InitAsConstants(pushConstantSize / 4, 0, 0, D3D12_SHADER_VISIBILITY_ALL);

// Set during rendering
commandList->SetGraphicsRoot32BitConstants(0, pushConstantSize / 4, data, 0);
```

## Dynamic State

D3D12 supports some dynamic state differently:

| State | Vulkan | D3D12 |
|-------|--------|-------|
| Viewport | vkCmdSetViewport | RSSetViewports |
| Scissor | vkCmdSetScissor | RSSetScissorRects |
| Blend Constants | vkCmdSetBlendConstants | Not dynamic (in PSO) |
| Depth Bias | vkCmdSetDepthBias | Not dynamic (in PSO) |
| Stencil Reference | vkCmdSetStencilReference | OMSetStencilRef |

## Implementation Notes

### DXGI Factory and Adapter

D3D12 requires DXGI to enumerate adapters:

```cpp
// Create DXGI factory
CreateDXGIFactory2(DXGI_CREATE_FACTORY_DEBUG, IID_PPV_ARGS(&factory));

// Enumerate adapters
IDXGIAdapter4* adapter = nullptr;
for (UINT i = 0; SUCCEEDED(factory->EnumAdapters1(i, &adapter)); ++i) {
    DXGI_ADAPTER_DESC1 desc;
    adapter->GetDesc1(&desc);
    // Check if adapter supports D3D12
    if (SUCCEEDED(D3D12CreateDevice(adapter, D3D_FEATURE_LEVEL_12_0, __uuidof(ID3D12Device), nullptr))) {
        // Use this adapter
        break;
    }
}
```

### Debug Layer

Enable D3D12 debug layer during development:

```cpp
#ifdef _DEBUG
ID3D12Debug* debugController = nullptr;
if (SUCCEEDED(D3D12GetDebugInterface(IID_PPV_ARGS(&debugController)))) {
    debugController->EnableDebugLayer();
}
#endif
```

### Interop with C++

D3D12 uses COM for object lifetime management:

```cpp
// Reference counting
device->AddRef();
device->Release();

// Smart pointers (Microsoft::WRL::ComPtr)
ComPtr<ID3D12Device> device;
D3D12CreateDevice(adapter, featureLevel, IID_PPV_ARGS(&device));
```

## Resources

- [D3D12 Programming Guide](https://docs.microsoft.com/en-us/windows/win32/direct3d12/directx-12-programming-guide)
- [D3D12 Reference](https://docs.microsoft.com/en-us/windows/win32/api/d3d12/)
- [DXGI Reference](https://docs.microsoft.com/en-us/windows/win32/api/dxgi/)
- [DirectX-Graphics-Samples](https://github.com/microsoft/DirectX-Graphics-Samples)

## Implementation Status

- ✅ Interface scaffold created
- ⏳ Full implementation pending
- ⏳ Testing required on Windows hardware

## Known Limitations

1. **No timeline semaphores**: Use ID3D12Fence with GetCompletedValue
2. **No explicit render passes**: Use OMSetRenderTargets
3. **Push constants limited**: Root constants max 256 bytes
4. **Dynamic state limited**: Most state must be in PSO
5. **Memory management**: No built-in allocator (use D3D12MA or custom)

## Future Work

- [ ] Full D3D12 API implementation
- [ ] Shader compilation pipeline (HLSL to DXBC/DXIL)
- [ ] Descriptor heap optimization
- [ ] Multi-queue support
- [ ] Async compute scheduling
- [ ] D3D12MA integration for memory management