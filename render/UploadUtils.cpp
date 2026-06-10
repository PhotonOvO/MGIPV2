#include "UploadUtils.h"

#include "../common/Common.h"
#include "../rhi/vulkan/internal/VulkanCommon.h"

#include <algorithm>
#include <cstring>

namespace demo::upload
{
	namespace
	{
		constexpr uint64_t kKiB = 1024ull;
		constexpr uint64_t kMiB = 1024ull * kKiB;

		void writeMappedBytes(rhi::Device& device, rhi::BufferHandle buffer, std::span<const std::byte> data)
		{
			if (data.empty())
			{
				return;
			}
			void* mappedData = device.mapBuffer(buffer);
			ASSERT(mappedData != nullptr, "Upload buffer must be CPU mapped");
			std::memcpy(mappedData, data.data(), data.size_bytes());
		}

		[[nodiscard]] bool supportsDirectUploadMemory(const rhi::MemoryProperties& memoryProperties,
		                                              uint64_t& heapSizeOut)
		{
			(void)memoryProperties;
			heapSizeOut = 0;
			return false;
		}

		[[nodiscard]] VkDevice toVkDevice(NativeUploadContext context)
		{
			return reinterpret_cast<VkDevice>(context.device);
		}

		[[nodiscard]] VmaAllocator toVmaAllocator(NativeUploadContext context)
		{
			return reinterpret_cast<VmaAllocator>(context.allocator);
		}

		[[nodiscard]] NativeUploadBuffer toNativeUploadBuffer(const utils::Buffer& buffer)
		{
			return NativeUploadBuffer{
				.buffer = reinterpret_cast<uintptr_t>(buffer.buffer),
				.allocation = reinterpret_cast<uintptr_t>(buffer.allocation),
			};
		}

		[[nodiscard]] utils::Buffer toUtilsBuffer(NativeUploadBuffer buffer)
		{
			return utils::Buffer{
				.buffer = reinterpret_cast<VkBuffer>(buffer.buffer),
				.allocation = reinterpret_cast<VmaAllocation>(buffer.allocation),
			};
		}
	} // namespace

	StaticBufferUploadPolicy buildStaticBufferUploadPolicy(const rhi::MemoryProperties& memoryProperties)
	{
		uint64_t directUploadHeapSize = 0;
		if (!supportsDirectUploadMemory(memoryProperties, directUploadHeapSize))
		{
			return {};
		}

		// Keep direct writes conservative: they are useful on ReBAR heaps for small/medium
		// static buffers, but large uploads still favor staging to avoid saturating PCIe.
		const uint64_t threshold = std::clamp(directUploadHeapSize / 64ull, 8ull * kMiB, 64ull * kMiB);
		return StaticBufferUploadPolicy{
			.allowDirectHostVisibleDeviceLocalUpload = true,
			.directUploadThreshold = threshold,
		};
	}

	rhi::BufferHandle createUploadStagingBuffer(rhi::Device& device, std::span<const std::byte> data)
	{
		rhi::BufferHandle stagingBuffer = createMappedUploadStagingBuffer(device, data.size_bytes());
		writeMappedBytes(device, stagingBuffer, data);
		return stagingBuffer;
	}

	rhi::BufferHandle createMappedUploadStagingBuffer(rhi::Device& device, uint64_t size)
	{
		return device.createBuffer(rhi::BufferDesc{
			.size = size,
			.usage = rhi::BufferUsageFlags::transferSrc,
			.memoryUsage = rhi::MemoryUsage::cpuToGpu,
			.debugName = "UploadStagingBuffer",
		});
	}

	rhi::BufferHandle createStaticBuffer(rhi::Device& device, uint64_t size, rhi::BufferUsageFlags usage)
	{
		return device.createBuffer(rhi::BufferDesc{
			.size = size,
			.usage = usage | rhi::BufferUsageFlags::transferDst,
			.memoryUsage = rhi::MemoryUsage::gpuOnly,
			.allowGpuAddress = static_cast<uint32_t>(usage & rhi::BufferUsageFlags::shaderDeviceAddress) != 0,
			.allowIndirectArgument = static_cast<uint32_t>(usage & rhi::BufferUsageFlags::indirect) != 0,
			.debugName = "StaticBuffer",
		});
	}

	rhi::BufferHandle createStaticBufferWithUpload(rhi::Device& device,
	                                               rhi::CommandBuffer& cmd,
	                                               std::span<const std::byte> data,
	                                               rhi::BufferUsageFlags usage,
	                                               const StaticBufferUploadPolicy& policy,
	                                               std::vector<rhi::BufferHandle>* deferredStagingBuffers)
	{
		if (policy.allowDirectHostVisibleDeviceLocalUpload && policy.directUploadThreshold > 0
			&& data.size_bytes() <= policy.directUploadThreshold)
		{
			rhi::BufferHandle directBuffer = device.createBuffer(rhi::BufferDesc{
				.size = data.size_bytes(),
				.usage = usage,
				.memoryUsage = rhi::MemoryUsage::cpuToGpu,
				.allowGpuAddress = static_cast<uint32_t>(usage & rhi::BufferUsageFlags::shaderDeviceAddress) != 0,
				.allowIndirectArgument = static_cast<uint32_t>(usage & rhi::BufferUsageFlags::indirect) != 0,
				.debugName = "DirectUploadStaticBuffer",
			});
			writeMappedBytes(device, directBuffer, data);
			return directBuffer;
		}

		rhi::BufferHandle stagingBuffer = createUploadStagingBuffer(device, data);

		rhi::BufferHandle gpuBuffer = createStaticBuffer(device, data.size_bytes(), usage);

		rhi::ComputeEncoder* copy = cmd.beginComputePass();
		copy->copyBuffer(stagingBuffer, 0, gpuBuffer, 0, data.size_bytes());
		cmd.endEncoding();

		if (deferredStagingBuffers != nullptr)
		{
			deferredStagingBuffers->push_back(stagingBuffer);
		}
		else
		{
			device.destroyBuffer(stagingBuffer);
		}

		return gpuBuffer;
	}

	NativeUploadBuffer createUploadStagingBuffer(NativeUploadContext context, std::span<const std::byte> data)
	{
		const VmaAllocator allocator = toVmaAllocator(context);
		utils::Buffer stagingBuffer = toUtilsBuffer(createMappedUploadStagingBuffer(context, data.size_bytes()));
		VmaAllocationInfo allocationInfo{};
		vmaGetAllocationInfo(allocator, stagingBuffer.allocation, &allocationInfo);
		if (!data.empty())
		{
			std::memcpy(allocationInfo.pMappedData, data.data(), data.size_bytes());
		}
		return toNativeUploadBuffer(stagingBuffer);
	}

	NativeUploadBuffer createMappedUploadStagingBuffer(NativeUploadContext context, uint64_t size)
	{
		const VkDevice device = toVkDevice(context);
		const VmaAllocator allocator = toVmaAllocator(context);
		utils::Buffer stagingBuffer{};
		const VkBufferCreateInfo bufferInfo{
			.sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO,
			.size = static_cast<uint64_t>(size),
			.usage = VK_BUFFER_USAGE_2_TRANSFER_SRC_BIT_KHR,
		};
		const VmaAllocationCreateInfo allocInfo{
			.flags = VMA_ALLOCATION_CREATE_HOST_ACCESS_SEQUENTIAL_WRITE_BIT | VMA_ALLOCATION_CREATE_MAPPED_BIT,
			.usage = VMA_MEMORY_USAGE_CPU_ONLY,
		};
		VmaAllocationInfo allocationInfo{};
		VK_CHECK(
			vmaCreateBuffer(allocator, &bufferInfo, &allocInfo, &stagingBuffer.buffer, &stagingBuffer.allocation, &
				allocationInfo));
		(void)device;
		(void)allocationInfo;
		return toNativeUploadBuffer(stagingBuffer);
	}

	NativeUploadBuffer createStaticBuffer(NativeUploadContext context, uint64_t size, uint64_t usage)
	{
		const VkDevice device = toVkDevice(context);
		const VmaAllocator allocator = toVmaAllocator(context);
		(void)device;
		utils::Buffer buffer{};
		const VkBufferCreateInfo bufferInfo{
			.sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO,
			.size = static_cast<uint64_t>(size),
			.usage = static_cast<VkBufferUsageFlags>(static_cast<VkBufferUsageFlags2KHR>(usage) |
				VK_BUFFER_USAGE_2_TRANSFER_DST_BIT_KHR),
		};
		const VmaAllocationCreateInfo allocInfo{.usage = VMA_MEMORY_USAGE_GPU_ONLY};
		VK_CHECK(vmaCreateBuffer(allocator, &bufferInfo, &allocInfo, &buffer.buffer, &buffer.allocation, nullptr));
		return toNativeUploadBuffer(buffer);
	}
} // namespace demo::upload
