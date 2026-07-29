#include "GPUSceneRegistry.h"

#include "../common/Common.h"
#include "../rhi/RHICommandBuffer.h"
#include "../rhi/RHIDevice.h"

#include <algorithm>
#include <cstddef>
#include <cstring>
#include <limits>
#include <span>

namespace demo
{
	namespace
	{
		constexpr uint64_t kBufferCopyAlignment = 4u;
		constexpr uint64_t kMinimumStagingCapacity = 4096u;

		static_assert(sizeof(shaderio::GPUSceneObject) % kBufferCopyAlignment == 0u);
		static_assert(sizeof(shaderio::GPUCullObject) % kBufferCopyAlignment == 0u);

		uint64_t alignStagingOffset(uint64_t offset)
		{
			ASSERT(offset <= std::numeric_limits<uint64_t>::max() - (kBufferCopyAlignment - 1u),
			       "GPUSceneRegistry staging offset overflow");
			return (offset + kBufferCopyAlignment - 1u) & ~(kBufferCopyAlignment - 1u);
		}

		glm::vec4 packRow(const glm::mat4& matrix, int row)
		{
			return glm::vec4(matrix[0][row], matrix[1][row], matrix[2][row], matrix[3][row]);
		}

		template <typename T>
		uint64_t requiredStagingBytesForRanges(
			uint64_t stagingOffset,
			std::span<const GPUSceneRegistry::DirtyRange> ranges)
		{
			for (const GPUSceneRegistry::DirtyRange& range : ranges)
			{
				const uint64_t byteCount = sizeof(T) * static_cast<uint64_t>(range.count);
				stagingOffset = alignStagingOffset(stagingOffset);
				ASSERT(stagingOffset <= std::numeric_limits<uint64_t>::max() - byteCount,
				       "GPUSceneRegistry staging size overflow");
				stagingOffset += byteCount;
			}
			return stagingOffset;
		}

		template <typename T>
		uint64_t copyRangesToGpu(rhi::ComputeEncoder& copy,
		                         void* stagingMapped,
		                         uint64_t stagingCapacityBytes,
		                         rhi::BufferHandle stagingBufferHandle,
		                         uint64_t stagingOffset,
		                         const std::vector<T>& source,
		                         rhi::BufferHandle destinationBuffer,
		                         std::span<const GPUSceneRegistry::DirtyRange> ranges)
		{
			for (const GPUSceneRegistry::DirtyRange& range : ranges)
			{
				const uint64_t byteCount = sizeof(T) * static_cast<uint64_t>(range.count);
				const uint64_t sourceOffset = alignStagingOffset(stagingOffset);
				ASSERT(sourceOffset <= stagingCapacityBytes
				           && byteCount <= stagingCapacityBytes - sourceOffset,
				       "GPUSceneRegistry staging slice exceeds the mapped upload buffer");
				std::memcpy(static_cast<std::byte*>(stagingMapped) + sourceOffset,
				            source.data() + range.startIndex,
				            static_cast<size_t>(byteCount));

				copy.copyBuffer(stagingBufferHandle,
				                sourceOffset,
				                destinationBuffer,
				                sizeof(T) * static_cast<uint64_t>(range.startIndex),
				                byteCount);
				stagingOffset = sourceOffset + byteCount;
			}
			return stagingOffset;
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
			if (!m_updateBufferRHI.isNull() && m_updateBufferMapped != nullptr)
			{
				m_rhiDevice->unmapBuffer(m_updateBufferRHI);
			}
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
		m_updateBufferCapacityBytes = 0;
		m_capacity = 0;
		m_gpuBuffersInitialized = false;
		m_rhiDevice = nullptr;
	}

	void GPUSceneRegistry::clear()
	{
		m_freeList.clear();
		if (m_slots.empty())
		{
			m_slots.resize(1);
		}
		m_slots[0] = {};
		for (uint32_t objectID = 1u; objectID < static_cast<uint32_t>(m_slots.size()); ++objectID)
		{
			ObjectSlot& slot = m_slots[objectID];
			slot.occupied = false;
			slot.generation = nextGeneration(slot.generation);
			slot.denseIndex = UINT32_MAX;
			slot.desc = {};
			slot.gpuObject = {};
			slot.cullObject = {};
		}
		for (uint32_t objectID = static_cast<uint32_t>(m_slots.size()); objectID-- > 1u;)
		{
			m_freeList.push_back(objectID);
		}
		m_denseSlotIds.clear();
		m_dirtyDenseIndices.clear();
		m_gpuObjects.clear();
		m_cullObjects.clear();
		m_dirty = true;
		m_requiresFullUpload = true;
	}

	GPUSceneObjectHandle GPUSceneRegistry::registerObject(const GPUSceneRegistrationDesc& desc)
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
			m_slots.back().generation = 1u;
		}

		ObjectSlot& slot = m_slots[objectID];
		if (slot.generation == 0u)
		{
			slot.generation = 1u;
		}
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
		return GPUSceneObjectHandle{
			.index = objectID,
			.generation = slot.generation,
		};
	}

	GPUSceneRemoveResult GPUSceneRegistry::removeObject(GPUSceneObjectHandle object)
	{
		if (!isLiveHandle(object))
		{
			return {};
		}

		const uint32_t objectID = object.index;
		ObjectSlot& slot = m_slots[objectID];
		const uint32_t denseIndex = slot.denseIndex;
		const uint32_t lastDenseIndex = static_cast<uint32_t>(m_gpuObjects.size() - 1u);
		GPUSceneRemoveResult result{
			.removed = true,
			.removedObject = object,
			.removedDenseIndex = denseIndex,
		};
		if (denseIndex != lastDenseIndex)
		{
			m_gpuObjects[denseIndex] = m_gpuObjects[lastDenseIndex];
			m_cullObjects[denseIndex] = m_cullObjects[lastDenseIndex];
			const uint32_t movedObjectID = m_denseSlotIds[lastDenseIndex];
			m_denseSlotIds[denseIndex] = movedObjectID;
			m_slots[movedObjectID].denseIndex = denseIndex;
			result.movedObject = GPUSceneObjectHandle{
				.index = movedObjectID,
				.generation = m_slots[movedObjectID].generation,
			};
			result.movedFromDenseIndex = lastDenseIndex;
			result.movedToDenseIndex = denseIndex;
			markDirtyDenseIndex(denseIndex);
		}

		m_gpuObjects.pop_back();
		m_cullObjects.pop_back();
		m_denseSlotIds.pop_back();
		m_dirtyDenseIndices.erase(
			std::remove_if(
				m_dirtyDenseIndices.begin(),
				m_dirtyDenseIndices.end(),
				[this](uint32_t dirtyDenseIndex)
				{
					return dirtyDenseIndex >= m_gpuObjects.size();
				}),
			m_dirtyDenseIndices.end());

		slot.occupied = false;
		slot.generation = nextGeneration(slot.generation);
		slot.denseIndex = UINT32_MAX;
		slot.desc = {};
		slot.gpuObject = {};
		slot.cullObject = {};
		m_freeList.push_back(objectID);
		m_dirty = !m_dirtyDenseIndices.empty() || m_requiresFullUpload;
		return result;
	}

	bool GPUSceneRegistry::updateTransform(GPUSceneObjectHandle object,
	                                       const glm::mat4& newTransform,
	                                       const glm::vec4& newBoundsSphere)
	{
		if (!isLiveHandle(object))
		{
			return false;
		}

		const uint32_t objectID = object.index;
		ObjectSlot& slot = m_slots[objectID];
		slot.desc.transform = newTransform;
		slot.desc.boundsSphere = newBoundsSphere;
		rebuildPackedObject(objectID);
		markDirtyDenseIndex(slot.denseIndex);
		m_dirty = true;
		return true;
	}

	bool GPUSceneRegistry::tryGetDenseIndex(GPUSceneObjectHandle object, uint32_t& outDenseIndex) const
	{
		if (!isLiveHandle(object))
		{
			return false;
		}
		outDenseIndex = m_slots[object.index].denseIndex;
		return true;
	}

	bool GPUSceneRegistry::isLiveHandle(GPUSceneObjectHandle object) const
	{
		return object.index != 0u
			&& object.index < m_slots.size()
			&& m_slots[object.index].occupied
			&& m_slots[object.index].generation == object.generation;
	}

	uint32_t GPUSceneRegistry::nextGeneration(uint32_t generation)
	{
		++generation;
		return generation == 0u ? 1u : generation;
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
		std::vector<DirtyRange> dirtyRanges;
		if (m_requiresFullUpload)
		{
			dirtyRanges.push_back(DirtyRange{
				.startIndex = 0,
				.count = objectCount,
			});
		}
		else if (!m_dirtyDenseIndices.empty())
		{
			dirtyRanges = buildDirtyRanges();
		}

		uint64_t requiredStagingBytes = 0;
		requiredStagingBytes =
			requiredStagingBytesForRanges<shaderio::GPUSceneObject>(requiredStagingBytes, dirtyRanges);
		requiredStagingBytes =
			requiredStagingBytesForRanges<shaderio::GPUCullObject>(requiredStagingBytes, dirtyRanges);
		ASSERT(requiredStagingBytes > 0u, "GPUSceneRegistry dirty upload must contain at least one staging slice");
		ensureStagingCapacity(requiredStagingBytes);

		// executeImmediateUpload and the normal frame submit both target the graphics
		// queue. A barrier at the head of this later submission therefore orders prior
		// compute/vertex shader readers before transfer overwrites of the shared buffers.
		// Newly allocated destination buffers have no prior readers and skip this WAR edge.
		if (m_gpuBuffersInitialized)
		{
			cmd.barrier(rhi::StageFlags::compute | rhi::StageFlags::vertexShader,
			            rhi::StageFlags::transfer,
			            rhi::HazardFlags::readBeforeWrite);
		}

		rhi::ComputeEncoder* copy = cmd.beginComputePass();
		uint64_t stagingOffset = 0;
		stagingOffset = copyRangesToGpu(
			*copy,
			m_updateBufferMapped,
			m_updateBufferCapacityBytes,
			m_updateBufferRHI,
			stagingOffset,
			m_gpuObjects,
			m_objectBufferRHI,
			dirtyRanges);
		stagingOffset = copyRangesToGpu(
			*copy,
			m_updateBufferMapped,
			m_updateBufferCapacityBytes,
			m_updateBufferRHI,
			stagingOffset,
			m_cullObjects,
			m_cullObjectBufferRHI,
			dirtyRanges);
		ASSERT(stagingOffset <= requiredStagingBytes,
		       "GPUSceneRegistry staging cursor exceeded its precomputed upload size");
		cmd.endEncoding();
		cmd.barrier(rhi::StageFlags::transfer,
		            rhi::StageFlags::compute | rhi::StageFlags::vertexShader,
		            rhi::HazardFlags::bufferWrites);

		m_dirty = false;
		m_requiresFullUpload = false;
		m_gpuBuffersInitialized = true;
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
		m_objectBufferRHI = {};
		m_cullObjectBufferRHI = {};

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
		m_capacity = newCapacity;
		m_requiresFullUpload = true;
		m_gpuBuffersInitialized = false;
	}

	void GPUSceneRegistry::ensureStagingCapacity(uint64_t requiredBytes)
	{
		if (requiredBytes <= m_updateBufferCapacityBytes
			&& !m_updateBufferRHI.isNull()
			&& m_updateBufferMapped != nullptr)
		{
			return;
		}

		ASSERT(m_rhiDevice != nullptr, "GPUSceneRegistry requires an RHI device for staging allocation");
		ASSERT(requiredBytes > 0u, "GPUSceneRegistry staging allocation must be non-empty");

		uint64_t newCapacityBytes =
			std::max(kMinimumStagingCapacity, m_updateBufferCapacityBytes);
		while (newCapacityBytes < requiredBytes)
		{
			if (newCapacityBytes > std::numeric_limits<uint64_t>::max() / 2u)
			{
				newCapacityBytes = requiredBytes;
				break;
			}
			newCapacityBytes *= 2u;
		}
		newCapacityBytes = alignStagingOffset(std::max(newCapacityBytes, requiredBytes));

		if (!m_updateBufferRHI.isNull() && m_updateBufferMapped != nullptr)
		{
			m_rhiDevice->unmapBuffer(m_updateBufferRHI);
		}
		if (!m_updateBufferRHI.isNull())
		{
			m_rhiDevice->destroyBuffer(m_updateBufferRHI);
		}
		m_updateBufferRHI = m_rhiDevice->createBuffer(rhi::BufferDesc{
			.size = newCapacityBytes,
			.usage = rhi::BufferUsageFlags::transferSrc,
			.memoryUsage = rhi::MemoryUsage::cpuToGpu,
			.debugName = "GPUSceneRegistry.staging",
		});
		m_updateBufferMapped = m_rhiDevice->mapBuffer(m_updateBufferRHI);
		ASSERT(m_updateBufferMapped != nullptr, "GPUSceneRegistry staging buffer mapping failed");
		m_updateBufferCapacityBytes = newCapacityBytes;
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
