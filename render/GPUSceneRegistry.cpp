#include "GPUSceneRegistry.h"

#include "../common/Common.h"
#include "../rhi/RHICommandBuffer.h"
#include "../rhi/RHIDevice.h"

#include <algorithm>
#include <cstring>
#include <span>

namespace demo
{
	namespace
	{
		glm::vec4 packRow(const glm::mat4& matrix, int row)
		{
			return glm::vec4(matrix[0][row], matrix[1][row], matrix[2][row], matrix[3][row]);
		}

		template <typename T>
		void copyRangesToGpu(rhi::ComputeEncoder& copy,
		                     void* stagingMapped,
		                     rhi::BufferHandle stagingBufferHandle,
		                     const std::vector<T>& source,
		                     rhi::BufferHandle destinationBuffer,
		                     std::span<const GPUSceneRegistry::DirtyRange> ranges)
		{
			for (const GPUSceneRegistry::DirtyRange& range : ranges)
			{
				const uint64_t byteCount = sizeof(T) * static_cast<uint64_t>(range.count);
				std::memcpy(stagingMapped,
				            source.data() + range.startIndex,
				            static_cast<size_t>(byteCount));

				copy.copyBuffer(stagingBufferHandle,
				                0,
				                destinationBuffer,
				                sizeof(T) * static_cast<uint64_t>(range.startIndex),
				                byteCount);
			}
		}
	} // namespace

	void GPUSceneRegistry::init(rhi::Device* rhiDevice)
	{
		m_rhiDevice = rhiDevice;
	}

	void GPUSceneRegistry::deinit()
	{
		clear();
		if (m_rhiDevice != nullptr)
		{
			if (!m_updateBufferRHI.isNull()) m_rhiDevice->destroyBuffer(m_updateBufferRHI);
			if (!m_objectBufferRHI.isNull()) m_rhiDevice->destroyBuffer(m_objectBufferRHI);
			if (!m_cullObjectBufferRHI.isNull()) m_rhiDevice->destroyBuffer(m_cullObjectBufferRHI);
		}
		m_updateBufferRHI = {};
		m_objectBufferRHI = {};
		m_cullObjectBufferRHI = {};
		m_objectBufferAddress = {};
		m_cullObjectBufferAddress = {};
		m_updateBufferMapped = nullptr;
		m_capacity = 0;
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
		const uint64_t sceneBytes = sizeof(shaderio::GPUSceneObject) * static_cast<uint64_t>(objectCount);
		const uint64_t cullBytes = sizeof(shaderio::GPUCullObject) * static_cast<uint64_t>(objectCount);
		rhi::ComputeEncoder* copy = cmd.beginComputePass();
		if (m_requiresFullUpload)
		{
			std::memcpy(m_updateBufferMapped, m_gpuObjects.data(), static_cast<size_t>(sceneBytes));

			copy->copyBuffer(m_updateBufferRHI, 0, m_objectBufferRHI, 0, sceneBytes);

			std::memcpy(m_updateBufferMapped, m_cullObjects.data(), static_cast<size_t>(cullBytes));

			copy->copyBuffer(m_updateBufferRHI, 0, m_cullObjectBufferRHI, 0, cullBytes);
		}
		else if (!m_dirtyDenseIndices.empty())
		{
			const std::vector<DirtyRange> dirtyRanges = buildDirtyRanges();
			copyRangesToGpu(*copy, m_updateBufferMapped, m_updateBufferRHI, m_gpuObjects, m_objectBufferRHI,
			                dirtyRanges);
			copyRangesToGpu(*copy, m_updateBufferMapped, m_updateBufferRHI, m_cullObjects, m_cullObjectBufferRHI,
			                dirtyRanges);
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

		ASSERT(m_rhiDevice != nullptr, "GPUSceneRegistry requires an RHI device for buffer allocation");
		const uint32_t newCapacity = std::max(requiredCount, std::max(64u, m_capacity * 2u));
		if (!m_objectBufferRHI.isNull()) m_rhiDevice->destroyBuffer(m_objectBufferRHI);
		if (!m_cullObjectBufferRHI.isNull()) m_rhiDevice->destroyBuffer(m_cullObjectBufferRHI);
		if (!m_updateBufferRHI.isNull()) m_rhiDevice->destroyBuffer(m_updateBufferRHI);
		m_objectBufferRHI = {};
		m_cullObjectBufferRHI = {};
		m_updateBufferRHI = {};
		m_updateBufferMapped = nullptr;

		m_objectBufferRHI = m_rhiDevice->createBuffer(rhi::BufferDesc{
			.size = sizeof(shaderio::GPUSceneObject) * static_cast<uint64_t>(newCapacity),
			.usage = rhi::BufferUsageFlags::storage | rhi::BufferUsageFlags::transferDst,
			.memoryUsage = rhi::MemoryUsage::gpuOnly,
			.allowGpuAddress = true,
			.debugName = "GPUSceneRegistry.objects",
		});
		m_objectBufferAddress = m_rhiDevice->getBufferGpuAddress(m_objectBufferRHI);
		m_cullObjectBufferRHI = m_rhiDevice->createBuffer(rhi::BufferDesc{
			.size = sizeof(shaderio::GPUCullObject) * static_cast<uint64_t>(newCapacity),
			.usage = rhi::BufferUsageFlags::storage | rhi::BufferUsageFlags::transferDst,
			.memoryUsage = rhi::MemoryUsage::gpuOnly,
			.allowGpuAddress = true,
			.debugName = "GPUSceneRegistry.cullObjects",
		});
		m_cullObjectBufferAddress = m_rhiDevice->getBufferGpuAddress(m_cullObjectBufferRHI);
		m_updateBufferRHI = m_rhiDevice->createBuffer(rhi::BufferDesc{
			.size = std::max(sizeof(shaderio::GPUSceneObject), sizeof(shaderio::GPUCullObject))
			        * static_cast<uint64_t>(newCapacity),
			.usage = rhi::BufferUsageFlags::transferSrc,
			.memoryUsage = rhi::MemoryUsage::cpuToGpu,
			.debugName = "GPUSceneRegistry.staging",
		});
		m_updateBufferMapped = m_rhiDevice->mapBuffer(m_updateBufferRHI);
		m_capacity = newCapacity;
		m_requiresFullUpload = true;
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
