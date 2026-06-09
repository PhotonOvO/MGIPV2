#include "GPUMeshletBuffer.h"

#include "../rhi/RHIDevice.h"
#include "../rhi/vulkan/internal/VulkanCommon.h"

#include <algorithm>
#include <cstddef>
#include <cstring>

namespace demo
{
	namespace
	{
		[[nodiscard]] VkDevice toVkDevice(uintptr_t handle)
		{
			return reinterpret_cast<VkDevice>(handle);
		}

		[[nodiscard]] VmaAllocator toVmaAllocator(uintptr_t handle)
		{
			return reinterpret_cast<VmaAllocator>(handle);
		}

		[[nodiscard]] VkBuffer toVkBuffer(uintptr_t handle)
		{
			return reinterpret_cast<VkBuffer>(handle);
		}

		[[nodiscard]] VmaAllocation toVmaAllocation(uintptr_t handle)
		{
			return reinterpret_cast<VmaAllocation>(handle);
		}

		GPUMeshletBuffer::BufferRecord createBuffer(VkDevice device,
		                                            VmaAllocator allocator,
		                                            VkDeviceSize size,
		                                            VkBufferUsageFlags2KHR usage,
		                                            VmaMemoryUsage memoryUsage,
		                                            VmaAllocationCreateFlags flags)
		{
			const VkBufferUsageFlags2CreateInfoKHR usageInfo{
				.sType = VK_STRUCTURE_TYPE_BUFFER_USAGE_FLAGS_2_CREATE_INFO_KHR,
				.usage = usage | VK_BUFFER_USAGE_2_SHADER_DEVICE_ADDRESS_BIT_KHR,
			};
			const VkBufferCreateInfo bufferInfo{
				.sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO,
				.pNext = &usageInfo,
				.size = size,
				.usage = 0,
				.sharingMode = VK_SHARING_MODE_EXCLUSIVE,
			};
			VmaAllocationCreateInfo allocInfo{.flags = flags, .usage = memoryUsage};
			if ((allocInfo.flags & VMA_ALLOCATION_CREATE_HOST_ACCESS_SEQUENTIAL_WRITE_BIT) != 0)
			{
				allocInfo.flags |= VMA_ALLOCATION_CREATE_MAPPED_BIT;
			}

			VkBuffer nativeBuffer{VK_NULL_HANDLE};
			VmaAllocation nativeAllocation{nullptr};
			VmaAllocationInfo allocationInfo{};
			VK_CHECK(
				vmaCreateBuffer(allocator, &bufferInfo, &allocInfo, &nativeBuffer, &nativeAllocation, &allocationInfo));

			const VkBufferDeviceAddressInfo addressInfo{
				.sType = VK_STRUCTURE_TYPE_BUFFER_DEVICE_ADDRESS_INFO, .buffer = nativeBuffer
			};
			return GPUMeshletBuffer::BufferRecord{
				.buffer = reinterpret_cast<uintptr_t>(nativeBuffer),
				.allocation = reinterpret_cast<uintptr_t>(nativeAllocation),
				.address = static_cast<uintptr_t>(vkGetBufferDeviceAddress(device, &addressInfo)),
				.mapped = allocationInfo.pMappedData,
			};
		}
	} // namespace

	void GPUMeshletBuffer::init(uintptr_t device, uintptr_t allocator, rhi::Device* device_)
	{
		m_device = device;
		m_allocator = allocator;
		m_rhiDevice = device_;
	}

	void GPUMeshletBuffer::deinit()
	{
		destroyBuffer(m_meshletDataBuffer);
		destroyBuffer(m_meshletCullObjectBuffer);
		destroyBuffer(m_meshletIndexBuffer);
		if (m_rhiDevice != nullptr && !m_meshletIndexBufferRHI.isNull())
		{
			m_rhiDevice->destroyBuffer(m_meshletIndexBufferRHI);
		}
		m_meshletIndexBufferRHI = {};
		m_meshletCount = 0;
		m_meshletIndexCount = 0;
		m_meshletCapacity = 0;
		m_meshletIndexCapacity = 0;
		m_device = 0;
		m_allocator = 0;
	}

	void GPUMeshletBuffer::clear()
	{
		destroyBuffer(m_meshletDataBuffer);
		destroyBuffer(m_meshletCullObjectBuffer);
		destroyBuffer(m_meshletIndexBuffer);
		m_meshletCount = 0;
		m_meshletIndexCount = 0;
		m_meshletCapacity = 0;
		m_meshletIndexCapacity = 0;
	}

	void GPUMeshletBuffer::uploadMeshlets(const std::vector<shaderio::Meshlet>& meshlets,
	                                      const std::vector<uint32_t>& meshletIndices,
	                                      const std::vector<shaderio::GPUCullObject>& meshletCullObjects)
	{
		const uint32_t newMeshletCount = static_cast<uint32_t>(meshlets.size());
		const uint32_t newIndexCount = static_cast<uint32_t>(meshletIndices.size());
		const uint32_t previousMeshletCount = m_meshletCount;
		const uint32_t previousIndexCount = m_meshletIndexCount;
		const bool capacityGrowth =
			newMeshletCount > m_meshletCapacity || (newIndexCount > 0 && newIndexCount > m_meshletIndexCapacity);
		const bool rewriteAll = capacityGrowth || newMeshletCount < m_meshletCount || newIndexCount <
			m_meshletIndexCount;
		if (rewriteAll)
		{
			clear();
		}

		m_meshletCount = newMeshletCount;
		m_meshletIndexCount = newIndexCount;
		if (meshlets.empty())
		{
			return;
		}

		ensureCapacities(newMeshletCount, newIndexCount);

		const uint32_t meshletStart = rewriteAll ? 0u : std::min(previousMeshletCount, newMeshletCount);
		const uint32_t meshletUploadCount = newMeshletCount - meshletStart;
		if (meshletUploadCount > 0)
		{
			const VkDeviceSize meshletOffsetBytes =
				sizeof(shaderio::Meshlet) * static_cast<VkDeviceSize>(meshletStart);
			const VkDeviceSize meshletUploadBytes =
				sizeof(shaderio::Meshlet) * static_cast<VkDeviceSize>(meshletUploadCount);
			std::memcpy(static_cast<std::byte*>(m_meshletDataBuffer.mapped) + meshletOffsetBytes,
			            meshlets.data() + meshletStart,
			            static_cast<size_t>(meshletUploadBytes));
			VK_CHECK(
				vmaFlushAllocation(toVmaAllocator(m_allocator), toVmaAllocation(m_meshletDataBuffer.allocation),
					meshletOffsetBytes, meshletUploadBytes));
		}

		if (!meshletCullObjects.empty())
		{
			const uint32_t cullObjectUploadCount = std::min(newMeshletCount,
			                                                static_cast<uint32_t>(meshletCullObjects.size()));
			const VkDeviceSize cullObjectOffsetBytes = 0;
			const VkDeviceSize cullObjectUploadBytes =
				sizeof(shaderio::GPUCullObject) * static_cast<VkDeviceSize>(cullObjectUploadCount);
			std::memcpy(static_cast<std::byte*>(m_meshletCullObjectBuffer.mapped),
			            meshletCullObjects.data(),
			            static_cast<size_t>(cullObjectUploadBytes));
			VK_CHECK(vmaFlushAllocation(toVmaAllocator(m_allocator),
				toVmaAllocation(m_meshletCullObjectBuffer.allocation),
				cullObjectOffsetBytes,
				cullObjectUploadBytes));
		}

		const uint32_t indexStart = rewriteAll ? 0u : std::min(previousIndexCount, newIndexCount);
		const uint32_t indexUploadCount = newIndexCount - indexStart;
		if (indexUploadCount > 0)
		{
			const VkDeviceSize indexOffsetBytes = sizeof(uint32_t) * static_cast<VkDeviceSize>(indexStart);
			const VkDeviceSize indexUploadBytes = sizeof(uint32_t) * static_cast<VkDeviceSize>(indexUploadCount);
			std::memcpy(static_cast<std::byte*>(m_meshletIndexBuffer.mapped) + indexOffsetBytes,
			            meshletIndices.data() + indexStart,
			            static_cast<size_t>(indexUploadBytes));
			VK_CHECK(
				vmaFlushAllocation(toVmaAllocator(m_allocator), toVmaAllocation(m_meshletIndexBuffer.allocation),
					indexOffsetBytes, indexUploadBytes));
		}
	}

	void GPUMeshletBuffer::ensureCapacities(uint32_t requiredMeshletCount, uint32_t requiredIndexCount)
	{
		if (requiredMeshletCount > m_meshletCapacity)
		{
			destroyBuffer(m_meshletDataBuffer);
			destroyBuffer(m_meshletCullObjectBuffer);
			m_meshletCapacity = std::max(requiredMeshletCount, std::max(64u, m_meshletCapacity * 2u));
			const VkDeviceSize meshletBytes = sizeof(shaderio::Meshlet) * static_cast<VkDeviceSize>(m_meshletCapacity);
			m_meshletDataBuffer = createBuffer(toVkDevice(m_device),
			                                   toVmaAllocator(m_allocator),
			                                   meshletBytes,
			                                   VK_BUFFER_USAGE_2_STORAGE_BUFFER_BIT_KHR,
			                                   VMA_MEMORY_USAGE_CPU_TO_GPU,
			                                   VMA_ALLOCATION_CREATE_HOST_ACCESS_SEQUENTIAL_WRITE_BIT);
			const VkDeviceSize cullObjectBytes =
				sizeof(shaderio::GPUCullObject) * static_cast<VkDeviceSize>(m_meshletCapacity);
			m_meshletCullObjectBuffer = createBuffer(toVkDevice(m_device),
			                                         toVmaAllocator(m_allocator),
			                                         cullObjectBytes,
			                                         VK_BUFFER_USAGE_2_STORAGE_BUFFER_BIT_KHR,
			                                         VMA_MEMORY_USAGE_CPU_TO_GPU,
			                                         VMA_ALLOCATION_CREATE_HOST_ACCESS_SEQUENTIAL_WRITE_BIT);
		}

		if (requiredIndexCount > 0 && requiredIndexCount > m_meshletIndexCapacity)
		{
			destroyBuffer(m_meshletIndexBuffer);
			m_meshletIndexCapacity = std::max(requiredIndexCount, std::max(128u, m_meshletIndexCapacity * 2u));
			const VkDeviceSize indexBytes = sizeof(uint32_t) * static_cast<VkDeviceSize>(m_meshletIndexCapacity);
			m_meshletIndexBuffer = createBuffer(toVkDevice(m_device),
			                                    toVmaAllocator(m_allocator),
			                                    indexBytes,
			                                    VK_BUFFER_USAGE_2_STORAGE_BUFFER_BIT_KHR |
			                                    VK_BUFFER_USAGE_2_INDEX_BUFFER_BIT_KHR,
			                                    VMA_MEMORY_USAGE_CPU_TO_GPU,
			                                    VMA_ALLOCATION_CREATE_HOST_ACCESS_SEQUENTIAL_WRITE_BIT);

			// Rebind the stable RHI handle to the reallocated index buffer (owned=false:
			// GPUMeshletBuffer owns the VMA lifetime).
			if (m_rhiDevice != nullptr)
			{
				const uint64_t native = static_cast<uint64_t>(m_meshletIndexBuffer.buffer);
				if (m_meshletIndexBufferRHI.isNull())
				{
					m_meshletIndexBufferRHI = m_rhiDevice->registerExternalBuffer(native);
				}
				else
				{
					m_rhiDevice->updateBufferBinding(m_meshletIndexBufferRHI, native);
				}
			}
		}
	}

	void GPUMeshletBuffer::destroyBuffer(BufferRecord& buffer)
	{
		if (buffer.buffer != 0)
		{
			vmaDestroyBuffer(toVmaAllocator(m_allocator), toVkBuffer(buffer.buffer),
			                 toVmaAllocation(buffer.allocation));
			buffer = {};
		}
	}
} // namespace demo
