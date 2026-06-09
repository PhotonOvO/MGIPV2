#pragma once

#include "RHIArgumentTable.h"
#include "RHIBindlessTypes.h"
#include "RHICapabilities.h"
#include "RHIHandles.h"
#include "RHIPipeline.h"
#include "RHIQueue.h"
#include "RHITypes.h"

#include <cassert>
#include <cstdint>
#include <functional>
#include <string>
#include <vector>

#include "RHIBoundary.h"

namespace demo::rhi
{
	// Forward-declared for executeImmediateUpload's std::function signature (used by reference only).
	class CommandBuffer;

	// Vulkan-specific fields have been moved to VulkanDeviceCreateInfo (D-08).
	// D3D12/Metal paths use only these backend-neutral fields.
	struct DeviceCreateInfo
	{
		CapabilityRequirements capabilityRequirements{};
		bool enableValidationLayers{true};
	};

	struct PhysicalDeviceInfo
	{
		std::string deviceName;
		uint32_t apiVersion{0};
		uint32_t driverVersion{0};
		uint32_t vendorId{0};
		uint32_t deviceId{0};
		uint32_t deviceType{0};
	};

	struct DeviceFeatureInfo
	{
		bool timelineSemaphore{false};
		bool synchronization2{false};
		bool dynamicRendering{false};
		bool maintenance5{false};
		bool maintenance6{false};
	};

	struct MemoryTypeInfo
	{
		uint32_t propertyFlags{0};
		uint32_t heapIndex{0};
	};

	struct MemoryHeapInfo
	{
		uint64_t size{0};
		uint32_t flags{0};
	};

	struct MemoryProperties
	{
		std::vector<MemoryTypeInfo> memoryTypes;
		std::vector<MemoryHeapInfo> memoryHeaps;
	};

	enum class DescriptorHeapType : uint8_t
	{
		resource = 0,
		sampler,
	};

	struct DescriptorHeapDesc
	{
		DescriptorHeapType type{DescriptorHeapType::resource};
		uint32_t descriptorCapacity{0};
		bool shaderVisible{true};
	};

	struct DescriptorAllocation
	{
		DescriptorHeapHandle heap{};
		ResourceIndex firstIndex{kInvalidResourceIndex};
		uint32_t count{0};

		[[nodiscard]] bool isValid() const
		{
			return heap.isValid() && firstIndex != kInvalidResourceIndex && count > 0;
		}
	};

	class Device
	{
	public:
		virtual ~Device() = default;

		virtual void init(const DeviceCreateInfo& createInfo) = 0;
		virtual void deinit() = 0;

		virtual uint64_t getBackendInstanceHandle() const = 0;
		virtual uint64_t getBackendPhysicalDeviceHandle() const = 0;
		virtual uint64_t getBackendDeviceHandle() const = 0;

		virtual uint32_t getApiVersion() const = 0;
		virtual const char* getDeviceName() const = 0;
		virtual const PhysicalDeviceInfo& getPhysicalDeviceInfo() const = 0;
		virtual const DeviceFeatureInfo& getEnabledFeatureInfo() const = 0;
		virtual CapabilityReport queryCapabilities() const = 0;
		virtual bool supports(CapabilityTier tier) const = 0;
		virtual const MemoryProperties& getPhysicalMemoryProperties() const = 0;
		virtual void* getFeaturesChainHead() const = 0;

		virtual QueueInfo getGraphicsQueue() const = 0;
		virtual QueueInfo getComputeQueue() const = 0;
		virtual QueueInfo getTransferQueue() const = 0;

		virtual bool isInstanceExtensionSupported(const char* name) const = 0;
		virtual bool isDeviceExtensionSupported(const char* name) const = 0;

		virtual void waitIdle() = 0;

		// --- Immediate upload seam (UPL-02) ---
		// executeImmediateUpload submits a one-shot upload command to the graphics queue.
		// The backend (VulkanDevice) owns the upload cmd pool, fence lifecycle, and per-frame
		// pending queue. D3D12/Metal stubs abort until backend support is added.
		// Callers must not escape the VkCommandBuffer or fence; use rhi::CommandBuffer& only.
		virtual void executeImmediateUpload(std::function<void(rhi::CommandBuffer&)> uploadFn)
		{
			RHI_UNIMPLEMENTED("executeImmediateUpload");
		}

		// flushUploadRetirements polls (waitForCompletion=false) or blocks (true) on all
		// pending upload fences and recycles their cmd buffers and fences.
		// Staging buffer retirement (rhiStagingBuffers) stays in the render layer.
		virtual void flushUploadRetirements(bool waitForCompletion)
		{
			RHI_UNIMPLEMENTED("flushUploadRetirements");
		}

		// --- Texture views ---
		// createTextureView builds a backend view from the desc and registers an owned handle.
		// registerExternalTextureView adopts an externally-owned backend view (e.g. swapchain)
		// without taking ownership. destroyTextureView frees owned views. resolveTextureViewBackendHandle
		// returns the backing native handle (as uint64) for descriptor-write / ImGui seams.
		virtual TextureViewHandle createTextureView(const TextureViewCreateDesc& desc) = 0;
		virtual TextureViewHandle registerExternalTextureView(uint64_t externalView) = 0;
		virtual void destroyTextureView(TextureViewHandle handle) = 0;
		virtual uint64_t resolveTextureViewBackendHandle(TextureViewHandle handle) const = 0;

		// --- Textures (images) ---
		// createTexture creates an RHI-owned texture. registerExternalTexture adopts an
		// externally-owned backend texture (e.g. swapchain) without taking ownership.
		// destroyTexture frees owned images; destroyImage is kept as the legacy alias
		// while renderer call sites migrate.
		virtual TextureHandle createTexture(const TextureDesc&)
		{
			RHI_UNIMPLEMENTED("createTexture");
			return {};
		}

		virtual void destroyTexture(TextureHandle handle) { destroyImage(handle); }
		// registerExternalTexture adopts an externally-owned backend texture (e.g. swapchain)
		// without taking ownership. destroyImage frees owned images. resolveTextureBackendHandle
		// returns the backing backend object handle (as uint64) for command/seam paths.
		virtual TextureHandle registerExternalTexture(uint64_t externalImage) = 0;
		virtual void destroyImage(TextureHandle handle) = 0;
		virtual uint64_t resolveTextureBackendHandle(TextureHandle handle) const = 0;

		// ----- Modern GPU interface (Wave 0 contract) ----------------------------
		// Default bodies assert: backends opt in by overriding. Vulkan implements
		// these in Wave 1; D3D12/Metal stay asserting stubs until later milestones.
		// destroy* invalidates the logical handle immediately. For owned resources,
		// physical backend destruction is delayed until the backend retirement point;
		// adopted/external resources are only unregistered from the handle table.

		// --- Buffer (wraps the existing device-address path) ---
		virtual BufferHandle createBuffer(const BufferDesc&)
		{
			RHI_UNIMPLEMENTED("createBuffer");
			return {};
		}

		virtual void destroyBuffer(BufferHandle)
		{
			RHI_UNIMPLEMENTED("destroyBuffer");
		}

		// Adopt an externally-owned backend buffer (owned=false): the registry only mirrors it so
		// it can be addressed by handle. updateBufferBinding rebinds the handle to a reallocated
		// backend buffer. destroyBuffer on an owned=false handle only unregisters.
		virtual BufferHandle registerExternalBuffer(uint64_t /*externalBuffer*/)
		{
			RHI_UNIMPLEMENTED("registerExternalBuffer");
			return {};
		}

		virtual void updateBufferBinding(BufferHandle, uint64_t /*externalBuffer*/)
		{
			RHI_UNIMPLEMENTED("updateBufferBinding");
		}

		virtual GpuPtr getBufferGpuAddress(BufferHandle) const
		{
			RHI_UNIMPLEMENTED("getBufferGpuAddress");
			return {};
		}

		virtual void* mapBuffer(BufferHandle)
		{
			RHI_UNIMPLEMENTED("mapBuffer");
			return nullptr;
		}

		virtual void unmapBuffer(BufferHandle)
		{
			RHI_UNIMPLEMENTED("unmapBuffer");
		}

		// --- Sampler ---
		virtual SamplerHandle createSampler(const SamplerDesc&)
		{
			RHI_UNIMPLEMENTED("createSampler");
			return {};
		}

		virtual void destroySampler(SamplerHandle)
		{
			RHI_UNIMPLEMENTED("destroySampler");
		}

		virtual uint64_t resolveSamplerBackendHandle(SamplerHandle) const
		{
			RHI_UNIMPLEMENTED("resolveSamplerBackendHandle");
			return 0;
		}

		// --- Argument layout / table ---
		virtual ArgumentLayoutHandle createArgumentLayout(const ArgumentLayoutDesc&)
		{
			RHI_UNIMPLEMENTED("createArgumentLayout");
			return {};
		}

		virtual void destroyArgumentLayout(ArgumentLayoutHandle)
		{
			RHI_UNIMPLEMENTED("destroyArgumentLayout");
		}

		virtual ArgumentTableHandle createArgumentTable(ArgumentLayoutHandle)
		{
			RHI_UNIMPLEMENTED("createArgumentTable");
			return {};
		}

		virtual void destroyArgumentTable(ArgumentTableHandle)
		{
			RHI_UNIMPLEMENTED("destroyArgumentTable");
		}

		virtual void updateArgumentTable(ArgumentTableHandle, uint32_t /*writeCount*/, const ArgumentWrite*)
		{
			RHI_UNIMPLEMENTED("updateArgumentTable");
		}

		// --- Pipeline ---
		virtual PipelineHandle createGraphicsPipeline(const GraphicsPipelineDesc&)
		{
			RHI_UNIMPLEMENTED("createGraphicsPipeline");
			return {};
		}

		virtual PipelineHandle createComputePipeline(const ComputePipelineDesc&)
		{
			RHI_UNIMPLEMENTED("createComputePipeline");
			return {};
		}

		virtual void destroyPipeline(PipelineHandle)
		{
			RHI_UNIMPLEMENTED("destroyPipeline");
		}

		// --- Query pool ---
		virtual QueryPoolHandle createQueryPool(uint32_t /*queryCount*/)
		{
			RHI_UNIMPLEMENTED("createQueryPool");
			return {};
		}

		virtual void destroyQueryPool(QueryPoolHandle)
		{
			RHI_UNIMPLEMENTED("destroyQueryPool");
		}

		virtual uint64_t getQueryPoolResult(QueryPoolHandle, uint32_t /*queryIndex*/)
		{
			RHI_UNIMPLEMENTED("getQueryPoolResult");
			return 0;
		}

		// Non-blocking batch read. Writes queryCount (value, availability) pairs into outPairs
		// (size >= queryCount*2). availability==0 means the result is not yet ready. Returns
		// false if the whole batch could not be read.
		virtual bool getQueryPoolResultsWithAvailability(QueryPoolHandle, uint32_t /*firstQuery*/,
		                                                 uint32_t /*queryCount*/, uint64_t* /*outPairs*/)
		{
			RHI_UNIMPLEMENTED("getQueryPoolResultsWithAvailability");
			return false;
		}

		// --- Future RHI features (capability-gated) ---
		virtual DescriptorHeapHandle allocateDescriptorHeap(const DescriptorHeapDesc&)
		{
			RHI_UNIMPLEMENTED("allocateDescriptorHeap");
			return {};
		}

		virtual void freeDescriptorHeap(DescriptorHeapHandle)
		{
			RHI_UNIMPLEMENTED("freeDescriptorHeap");
		}

		virtual DescriptorAllocation allocateDescriptors(DescriptorHeapHandle, uint32_t /*count*/)
		{
			RHI_UNIMPLEMENTED("allocateDescriptors");
			return {};
		}

		virtual void freeDescriptors(const DescriptorAllocation&)
		{
			RHI_UNIMPLEMENTED("freeDescriptors");
		}

		virtual ResidencySetHandle createResidencySet()
		{
			RHI_UNIMPLEMENTED("createResidencySet");
			return {};
		}

		virtual void destroyResidencySet(ResidencySetHandle)
		{
			RHI_UNIMPLEMENTED("destroyResidencySet");
		}

		virtual ShaderLibraryHandle createShaderLibrary(const ShaderLibraryDesc&)
		{
			RHI_UNIMPLEMENTED("createShaderLibrary");
			return {};
		}

		virtual void destroyShaderLibrary(ShaderLibraryHandle)
		{
			RHI_UNIMPLEMENTED("destroyShaderLibrary");
		}

		virtual PipelineCompilerHandle createPipelineCompiler(const PipelineCompileOptions&)
		{
			RHI_UNIMPLEMENTED("createPipelineCompiler");
			return {};
		}

		virtual void destroyPipelineCompiler(PipelineCompilerHandle)
		{
			RHI_UNIMPLEMENTED("destroyPipelineCompiler");
		}
	};
} // namespace demo::rhi
