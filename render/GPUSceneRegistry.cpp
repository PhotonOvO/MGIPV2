#include "GPUSceneRegistry.h"

#include "../rhi/RHICommandBuffer.h"
#include "../rhi/RHIDevice.h"
#include "../rhi/vulkan/internal/VulkanCommon.h"

#include <algorithm>
#include <cstring>
#include <span>

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

		GPUSceneRegistry::BufferRecord createBuffer(VkDevice device,
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
			if ((allocInfo.flags & (VMA_ALLOCATION_CREATE_HOST_ACCESS_SEQUENTIAL_WRITE_BIT
				| VMA_ALLOCATION_CREATE_HOST_ACCESS_RANDOM_BIT)) != 0)
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
			return GPUSceneRegistry::BufferRecord{
				.buffer = reinterpret_cast<uintptr_t>(nativeBuffer),
				.allocation = reinterpret_cast<uintptr_t>(nativeAllocation),
				.address = static_cast<uintptr_t>(vkGetBufferDeviceAddress(device, &addressInfo)),
				.mapped = allocationInfo.pMappedData,
			};
		}

		glm::vec4 packRow(const glm::mat4& matrix, int row)
		{
			return glm::vec4(matrix[0][row], matrix[1][row], matrix[2][row], matrix[3][row]);
		}

		template <typename T>
		void copyRangesToGpu(rhi::ComputeEncoder& copy,
		                     GPUSceneRegistry::BufferRecord& stagingBuffer,
		                     rhi::BufferHandle stagingBufferHandle,
		                     const std::vector<T>& source,
		                     rhi::BufferHandle destinationBuffer,
		                     std::span<const GPUSceneRegistry::DirtyRange> ranges,
		                     VmaAllocator allocator)
		{
			for (const GPUSceneRegistry::DirtyRange& range : ranges)
			{
				const VkDeviceSize byteCount = sizeof(T) * static_cast<VkDeviceSize>(range.count);
				std::memcpy(stagingBuffer.mapped,
				            source.data() + range.startIndex,
				            static_cast<size_t>(byteCount));
				VK_CHECK(vmaFlushAllocation(allocator, toVmaAllocation(stagingBuffer.allocation), 0, byteCount));

				copy.copyBuffer(stagingBufferHandle,
				                0,
				                destinationBuffer,
				                sizeof(T) * static_cast<VkDeviceSize>(range.startIndex),
				                byteCount);
			}
		}
	} // namespace

	void GPUSceneRegistry::init(uintptr_t device, uintptr_t allocator, rhi::Device* rhiDevice)
	{
		m_device = device;
		m_allocator = allocator;
		m_rhiDevice = rhiDevice;
	}

	void GPUSceneRegistry::deinit()
	{
		clear();
		destroyBuffer(m_updateBuffer);
		destroyBuffer(m_objectBuffer);
		destroyBuffer(m_cullObjectBuffer);
		if (m_rhiDevice != nullptr)
		{
			if (!m_updateBufferRHI.isNull()) m_rhiDevice->destroyBuffer(m_updateBufferRHI);
			if (!m_objectBufferRHI.isNull()) m_rhiDevice->destroyBuffer(m_objectBufferRHI);
			if (!m_cullObjectBufferRHI.isNull()) m_rhiDevice->destroyBuffer(m_cullObjectBufferRHI);
		}
		m_updateBufferRHI = {};
		m_objectBufferRHI = {};
		m_cullObjectBufferRHI = {};
		m_capacity = 0;
		m_device = 0;
		m_allocator = 0;
		m_rhiDevice = nullptr;
	}

	void GPUSceneRegistry::clear()
	{
		m_slots.assign(1, ObjectSlot{});
		m_freeList.clear();
		m_denseSlotIds.clear();
		m_dirtyDenseIndices.clear();
		m_gpuObjects.clear();
		m_cullObjects.clear();
		m_dirty = true;
		m_requiresFullUpload = true;
	}

	uint32_t GPUSceneRegistry::registerObject(const GPUSceneRegistrationDesc& desc)
	{
		uint32_t objectID = 0;
		if (!m_freeList.empty())
		{
			objectID = m_freeList.back();
			m_freeList.pop_back();
		}
		else
		{
			objectID = static_cast<uint32_t>(m_slots.size());
			m_slots.push_back(ObjectSlot{});
		}

		ObjectSlot& slot = m_slots[objectID];
		slot.occupied = true;
		slot.denseIndex = static_cast<uint32_t>(m_gpuObjects.size());
		slot.desc = desc;
		slot.gpuObject = packSceneObject(desc);
		slot.cullObject = packCullObject(desc);

		m_denseSlotIds.push_back(objectID);
		m_gpuObjects.push_back(slot.gpuObject);
		m_cullObjects.push_back(slot.cullObject);
		markDirtyDenseIndex(slot.denseIndex);
		m_dirty = true;
		return objectID;
	}

	void GPUSceneRegistry::removeObject(uint32_t objectID)
	{
		if (objectID == 0 || objectID >= m_slots.size())
		{
			return;
		}

		ObjectSlot& slot = m_slots[objectID];
		if (!slot.occupied)
		{
			return;
		}

		const uint32_t denseIndex = slot.denseIndex;
		const uint32_t lastDenseIndex = static_cast<uint32_t>(m_gpuObjects.size() - 1u);
		if (denseIndex != lastDenseIndex)
		{
			m_gpuObjects[denseIndex] = m_gpuObjects[lastDenseIndex];
			m_cullObjects[denseIndex] = m_cullObjects[lastDenseIndex];
			const uint32_t movedObjectID = m_denseSlotIds[lastDenseIndex];
			m_denseSlotIds[denseIndex] = movedObjectID;
			m_slots[movedObjectID].denseIndex = denseIndex;
			markDirtyDenseIndex(denseIndex);
		}

		m_gpuObjects.pop_back();
		m_cullObjects.pop_back();
		m_denseSlotIds.pop_back();

		slot = {};
		m_freeList.push_back(objectID);
		m_dirty = !m_dirtyDenseIndices.empty() || m_requiresFullUpload;
	}

	void GPUSceneRegistry::updateTransform(uint32_t objectID, const glm::mat4& newTransform,
	                                       const glm::vec4& newBoundsSphere)
	{
		if (objectID == 0 || objectID >= m_slots.size())
		{
			return;
		}

		ObjectSlot& slot = m_slots[objectID];
		if (!slot.occupied)
		{
			return;
		}

		slot.desc.transform = newTransform;
		slot.desc.boundsSphere = newBoundsSphere;
		rebuildPackedObject(objectID);
		markDirtyDenseIndex(slot.denseIndex);
		m_dirty = true;
	}

	void GPUSceneRegistry::syncToGpu(rhi::CommandBuffer& cmd)
	{
		if (!m_dirty)
		{
			return;
		}

		const uint32_t objectCount = static_cast<uint32_t>(m_gpuObjects.size());
		if (objectCount == 0)
		{
			m_dirty = false;
			m_requiresFullUpload = false;
			m_dirtyDenseIndices.clear();
			return;
		}

		ensureCapacity(objectCount);
		bindBufferHandles();
		const VkDeviceSize sceneBytes = sizeof(shaderio::GPUSceneObject) * static_cast<VkDeviceSize>(objectCount);
		const VkDeviceSize cullBytes = sizeof(shaderio::GPUCullObject) * static_cast<VkDeviceSize>(objectCount);
		rhi::ComputeEncoder* copy = cmd.beginComputePass();
		if (m_requiresFullUpload)
		{
			std::memcpy(m_updateBuffer.mapped, m_gpuObjects.data(), static_cast<size_t>(sceneBytes));
			VK_CHECK(
				vmaFlushAllocation(toVmaAllocator(m_allocator), toVmaAllocation(m_updateBuffer.allocation), 0,
					sceneBytes));

			copy->copyBuffer(m_updateBufferRHI, 0, m_objectBufferRHI, 0, sceneBytes);

			std::memcpy(m_updateBuffer.mapped, m_cullObjects.data(), static_cast<size_t>(cullBytes));
			VK_CHECK(
				vmaFlushAllocation(toVmaAllocator(m_allocator), toVmaAllocation(m_updateBuffer.allocation), 0, cullBytes
				));

			copy->copyBuffer(m_updateBufferRHI, 0, m_cullObjectBufferRHI, 0, cullBytes);
		}
		else if (!m_dirtyDenseIndices.empty())
		{
			const std::vector<DirtyRange> dirtyRanges = buildDirtyRanges();
			copyRangesToGpu(*copy, m_updateBuffer, m_updateBufferRHI, m_gpuObjects, m_objectBufferRHI, dirtyRanges,
			                toVmaAllocator(m_allocator));
			copyRangesToGpu(*copy, m_updateBuffer, m_updateBufferRHI, m_cullObjects, m_cullObjectBufferRHI, dirtyRanges,
			                toVmaAllocator(m_allocator));
		}
		cmd.endEncoding();
		cmd.barrier(rhi::StageFlags::transfer,
		            rhi::StageFlags::compute | rhi::StageFlags::vertexShader,
		            rhi::HazardFlags::bufferWrites);

		m_dirty = false;
		m_requiresFullUpload = false;
		m_dirtyDenseIndices.clear();
	}

	void GPUSceneRegistry::ensureCapacity(uint32_t requiredCount)
	{
		if (requiredCount <= m_capacity)
		{
			return;
		}

		const uint32_t newCapacity = std::max(requiredCount, std::max(64u, m_capacity * 2u));
		if (m_rhiDevice != nullptr)
		{
			if (!m_objectBufferRHI.isNull()) m_rhiDevice->destroyBuffer(m_objectBufferRHI);
			if (!m_cullObjectBufferRHI.isNull()) m_rhiDevice->destroyBuffer(m_cullObjectBufferRHI);
			if (!m_updateBufferRHI.isNull()) m_rhiDevice->destroyBuffer(m_updateBufferRHI);
		}
		m_objectBufferRHI = {};
		m_cullObjectBufferRHI = {};
		m_updateBufferRHI = {};
		destroyBuffer(m_objectBuffer);
		destroyBuffer(m_cullObjectBuffer);
		destroyBuffer(m_updateBuffer);

		m_objectBuffer = createBuffer(toVkDevice(m_device),
		                              toVmaAllocator(m_allocator),
		                              sizeof(shaderio::GPUSceneObject) * static_cast<VkDeviceSize>(newCapacity),
		                              VK_BUFFER_USAGE_2_STORAGE_BUFFER_BIT_KHR | VK_BUFFER_USAGE_2_TRANSFER_DST_BIT_KHR,
		                              VMA_MEMORY_USAGE_AUTO_PREFER_DEVICE,
		                              0);
		m_cullObjectBuffer = createBuffer(toVkDevice(m_device),
		                                  toVmaAllocator(m_allocator),
		                                  sizeof(shaderio::GPUCullObject) * static_cast<VkDeviceSize>(newCapacity),
		                                  VK_BUFFER_USAGE_2_STORAGE_BUFFER_BIT_KHR |
		                                  VK_BUFFER_USAGE_2_TRANSFER_DST_BIT_KHR,
		                                  VMA_MEMORY_USAGE_AUTO_PREFER_DEVICE,
		                                  0);
		m_updateBuffer = createBuffer(toVkDevice(m_device),
		                              toVmaAllocator(m_allocator),
		                              std::max(sizeof(shaderio::GPUSceneObject), sizeof(shaderio::GPUCullObject))
		                              * static_cast<VkDeviceSize>(newCapacity),
		                              VK_BUFFER_USAGE_2_TRANSFER_SRC_BIT_KHR,
		                              VMA_MEMORY_USAGE_CPU_TO_GPU,
		                              VMA_ALLOCATION_CREATE_HOST_ACCESS_SEQUENTIAL_WRITE_BIT);
		m_capacity = newCapacity;
		m_requiresFullUpload = true;
	}

	void GPUSceneRegistry::bindBufferHandles()
	{
		ASSERT(m_rhiDevice != nullptr, "GPUSceneRegistry RHI device is required for command-buffer-only upload");
		const auto bind = [this](rhi::BufferHandle& handle, uintptr_t buffer)
		{
			const uint64_t native = static_cast<uint64_t>(buffer);
			if (handle.isNull())
			{
				handle = m_rhiDevice->registerExternalBuffer(native);
			}
			else
			{
				m_rhiDevice->updateBufferBinding(handle, native);
			}
		};
		bind(m_objectBufferRHI, m_objectBuffer.buffer);
		bind(m_cullObjectBufferRHI, m_cullObjectBuffer.buffer);
		bind(m_updateBufferRHI, m_updateBuffer.buffer);
	}

	void GPUSceneRegistry::markDirtyDenseIndex(uint32_t denseIndex)
	{
		if (denseIndex >= m_gpuObjects.size())
		{
			return;
		}

		if (std::find(m_dirtyDenseIndices.begin(), m_dirtyDenseIndices.end(), denseIndex) == m_dirtyDenseIndices.end())
		{
			m_dirtyDenseIndices.push_back(denseIndex);
		}
	}

	std::vector<GPUSceneRegistry::DirtyRange> GPUSceneRegistry::buildDirtyRanges() const
	{
		if (m_dirtyDenseIndices.empty())
		{
			return {};
		}

		std::vector<uint32_t> indices = m_dirtyDenseIndices;
		std::sort(indices.begin(), indices.end());

		std::vector<DirtyRange> ranges;
		ranges.reserve(indices.size());
		uint32_t rangeStart = indices.front();
		uint32_t previous = indices.front();
		for (size_t i = 1; i < indices.size(); ++i)
		{
			const uint32_t current = indices[i];
			if (current == previous + 1u)
			{
				previous = current;
				continue;
			}

			ranges.push_back(DirtyRange{
				.startIndex = rangeStart,
				.count = previous - rangeStart + 1u,
			});
			rangeStart = current;
			previous = current;
		}

		ranges.push_back(DirtyRange{
			.startIndex = rangeStart,
			.count = previous - rangeStart + 1u,
		});
		return ranges;
	}

	void GPUSceneRegistry::rebuildPackedObject(uint32_t objectID)
	{
		ObjectSlot& slot = m_slots[objectID];
		slot.gpuObject = packSceneObject(slot.desc);
		slot.cullObject = packCullObject(slot.desc);
		m_gpuObjects[slot.denseIndex] = slot.gpuObject;
		m_cullObjects[slot.denseIndex] = slot.cullObject;
	}

	void GPUSceneRegistry::destroyBuffer(BufferRecord& buffer)
	{
		if (buffer.buffer != 0)
		{
			vmaDestroyBuffer(toVmaAllocator(m_allocator), toVkBuffer(buffer.buffer),
			                 toVmaAllocation(buffer.allocation));
			buffer = {};
		}
	}

	shaderio::GPUSceneObject GPUSceneRegistry::packSceneObject(const GPUSceneRegistrationDesc& desc)
	{
		shaderio::GPUSceneObject object{};
		object.worldMatrixRows[0] = packRow(desc.transform, 0);
		object.worldMatrixRows[1] = packRow(desc.transform, 1);
		object.worldMatrixRows[2] = packRow(desc.transform, 2);
		object.boundsSphere = desc.boundsSphere;
		object.materialIndex = desc.materialIndex;
		object.meshIndex = desc.meshIndex;
		object.flags = desc.flags;
		return object;
	}

	shaderio::GPUCullObject GPUSceneRegistry::packCullObject(const GPUSceneRegistrationDesc& desc)
	{
		return shaderio::GPUCullObject{
			.sphereCenterRadius = desc.boundsSphere,
			.indexCount = desc.indexCount,
			.firstIndex = desc.firstIndex,
			.vertexOffset = desc.vertexOffset,
			.flags = desc.flags,
		};
	}
} // namespace demo
