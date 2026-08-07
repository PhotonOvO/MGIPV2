#pragma once

#include "RHIArgumentTable.h"
#include "RHIBackend.h"
#include "RHIBindlessTypes.h"
#include "RHICapabilities.h"
#include "RHICommandAllocator.h"
#include "RHIDebugCounters.h"
#include "RHIHandles.h"
#include "RHIPipeline.h"
#include "RHIQueue.h"
#include "RHIResidency.h"
#include "RHISurface.h"
#include "RHISwapchain.h"
#include "RHITypes.h"

#include <cassert>
#include <cstdint>
#include <memory>
#include <string>
#include <vector>


namespace demo::rhi
{

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
		uint32_t nativeApiVersion{0};
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

	class Device
	{
	public:
		virtual ~Device() = default;

		virtual void init(const DeviceCreateInfo& createInfo) = 0;
		virtual void deinit() = 0;

		virtual BackendInfo getBackendInfo() const = 0;
		virtual const char* getDeviceName() const = 0;
		virtual const PhysicalDeviceInfo& getPhysicalDeviceInfo() const = 0;
		virtual const DeviceFeatureInfo& getEnabledFeatureInfo() const = 0;
		virtual CapabilityReport queryCapabilities() const = 0;
		virtual bool supports(CapabilityTier tier) const = 0;
		virtual const MemoryProperties& getPhysicalMemoryProperties() const = 0;


		virtual Queue* getQueue(QueueClass queueClass) = 0;
		virtual std::unique_ptr<CommandAllocator> createCommandAllocator(QueueClass queueClass) = 0;
		virtual void collectGarbage() {}
		[[nodiscard]] virtual RHIHotPathCounters getHotPathCounters() const noexcept
		{
			return snapshotHotPathCounters(getBackendInfo().type);
		}
		virtual void resetHotPathCounters() noexcept
		{
			demo::rhi::resetHotPathCounters(getBackendInfo().type);
		}

		// --- Backend object factories (init sink, RDEV-06) ---
		// initSurface initializes the WSI surface from the backend-internal
		// instance/physicalDevice; the render layer never touches the natives.
		virtual void initSurface(Surface& surface, const WindowHandle& window) = 0;

		// createSwapchain builds the backend swapchain for an initialized surface.
		// All native handles (device/queue/surface/cmd pool) stay backend-internal.
		virtual std::unique_ptr<Swapchain> createSwapchain(Surface& surface, bool vSync) = 0;

		// GPU timestamp tick period in nanoseconds (for GPU profiling). 0 when unknown.
		virtual float getTimestampPeriodNs() const { return 0.0f; }

		// D3D12/Metal: conservative default — no capability assumed until backend overrides.
		// Vulkan backend queries vkGetPhysicalDeviceFormatProperties with optimalTilingFeatures.
		virtual bool isFormatSupported(TextureFormat /*format*/, FormatFeatureFlag /*feature*/) const { return false; }

		virtual void waitIdle() = 0;

		// --- Texture views ---
		// createTextureView builds a backend view from the desc and registers an owned handle.
		virtual TextureViewHandle createTextureView(const TextureViewCreateDesc& desc) = 0;
		virtual void destroyTextureView(TextureViewHandle handle) = 0;

		// --- Textures (images) ---
		// createTexture creates an RHI-owned texture.
		// destroyTexture invalidates the logical handle and retires owned storage.
		virtual TextureHandle createTexture(const TextureDesc&) = 0;

		virtual void destroyTexture(TextureHandle handle) = 0;

		// ----- Modern GPU interface (Wave 0 contract) ----------------------------
		// destroy* invalidates the logical handle immediately. For owned resources,
		// physical backend destruction is delayed until the backend retirement point.

		// --- Buffers ---
		virtual BufferHandle createBuffer(const BufferDesc&) = 0;

		virtual void destroyBuffer(BufferHandle) = 0;

		virtual GpuPtr getBufferGpuAddress(BufferHandle) const = 0;

		virtual void* mapBuffer(BufferHandle) = 0;

		virtual void unmapBuffer(BufferHandle) = 0;

		virtual Result<MappedBufferRange> mapBufferRange(BufferHandle, const BufferMapDesc&)
		{
			return Result<MappedBufferRange>::fail(RHIErrorCode::unsupported, "Explicit buffer mapping is unsupported" );
		}
		virtual RHIResult flushMappedBufferRange(BufferHandle, uint64_t, uint64_t)
		{
			return RHIResult::fail(RHIErrorCode::unsupported, "Mapped-range flush is unsupported" );
		}
		virtual RHIResult invalidateMappedBufferRange(BufferHandle, uint64_t, uint64_t)
		{
			return RHIResult::fail(RHIErrorCode::unsupported, "Mapped-range invalidate is unsupported" );
		}

		// --- Sampler ---
		virtual SamplerHandle createSampler(const SamplerDesc&) = 0;

		virtual void destroySampler(SamplerHandle) = 0;

		// --- Argument layout / table ---
		virtual ArgumentLayoutHandle createArgumentLayout(const ArgumentLayoutDesc&) = 0;

		virtual void destroyArgumentLayout(ArgumentLayoutHandle) = 0;

		virtual ArgumentTableHandle createArgumentTable(const ArgumentTableCreateDesc&) = 0;

		virtual void destroyArgumentTable(ArgumentTableHandle) = 0;

		virtual void updateArgumentTable(ArgumentTableHandle, ArgumentWriteBatch writes) = 0;

		virtual ArgumentLayoutHandle getArgumentTableLayout(ArgumentTableHandle) const = 0;

		// --- Pipeline ---
		virtual PipelineHandle createGraphicsPipeline(const GraphicsPipelineDesc&) = 0;

		virtual PipelineHandle createComputePipeline(const ComputePipelineDesc&) = 0;

		virtual void destroyPipeline(PipelineHandle) = 0;

		// --- Query pool ---
		virtual QueryPoolHandle createQueryPool(uint32_t queryCount) = 0;

		virtual void destroyQueryPool(QueryPoolHandle) = 0;

		virtual uint64_t getQueryPoolResult(QueryPoolHandle, uint32_t queryIndex) = 0;

		// Non-blocking batch read. Writes queryCount (value, availability) pairs into outPairs
		// (size >= queryCount*2). availability==0 means the result is not yet ready. Returns
		// false if the whole batch could not be read.
		virtual bool getQueryPoolResultsWithAvailability(QueryPoolHandle, uint32_t firstQuery,
		                                                 std::span<uint64_t> outValueAvailabilityPairs) = 0;

		virtual ShaderLibraryHandle createShaderLibrary(const ShaderLibraryDesc&) = 0;

		virtual void destroyShaderLibrary(ShaderLibraryHandle) = 0;

		virtual Result<ResidencySetHandle> createResidencySet(const ResidencySetDesc&)
		{
			return Result<ResidencySetHandle>::fail(RHIErrorCode::unsupported, "Explicit residency is unsupported" );
		}
		virtual RHIResult destroyResidencySet(ResidencySetHandle)
		{
			return RHIResult::fail(RHIErrorCode::unsupported, "Explicit residency is unsupported" );
		}
		virtual RHIResult updateResidencySet(ResidencySetHandle, const ResidencyUpdateBatch&)
		{
			return RHIResult::fail(RHIErrorCode::unsupported, "Explicit residency is unsupported" );
		}

	};
} // namespace demo::rhi
