#include "GPUMeshletBuffer.h"

#include "../rhi/RHIDevice.h"

#include <algorithm>
#include <cstddef>
#include <cstring>

namespace demo
{
	void GPUMeshletBuffer::init(rhi::Device* rhiDevice)
	{
		m_rhiDevice = rhiDevice;
	}

	void GPUMeshletBuffer::deinit()
	{
		releaseBuffers();
		m_meshletCount = 0;
		m_meshletIndexCount = 0;
		m_meshletCapacity = 0;
		m_meshletIndexCapacity = 0;
	}

	void GPUMeshletBuffer::clear()
	{
		releaseBuffers();
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
			const uint64_t meshletOffsetBytes =
				sizeof(shaderio::Meshlet) * static_cast<uint64_t>(meshletStart);
			const uint64_t meshletUploadBytes =
				sizeof(shaderio::Meshlet) * static_cast<uint64_t>(meshletUploadCount);
			std::memcpy(static_cast<std::byte*>(m_meshletDataMapped) + meshletOffsetBytes,
			            meshlets.data() + meshletStart,
			            static_cast<size_t>(meshletUploadBytes));
		}

		if (!meshletCullObjects.empty())
		{
			const uint32_t cullObjectUploadCount = std::min(newMeshletCount,
			                                                static_cast<uint32_t>(meshletCullObjects.size()));
			const uint64_t cullObjectUploadBytes =
				sizeof(shaderio::GPUCullObject) * static_cast<uint64_t>(cullObjectUploadCount);
			std::memcpy(m_meshletCullObjectMapped,
			            meshletCullObjects.data(),
			            static_cast<size_t>(cullObjectUploadBytes));
		}

		const uint32_t indexStart = rewriteAll ? 0u : std::min(previousIndexCount, newIndexCount);
		const uint32_t indexUploadCount = newIndexCount - indexStart;
		if (indexUploadCount > 0)
		{
			const uint64_t indexOffsetBytes = sizeof(uint32_t) * static_cast<uint64_t>(indexStart);
			const uint64_t indexUploadBytes = sizeof(uint32_t) * static_cast<uint64_t>(indexUploadCount);
			std::memcpy(static_cast<std::byte*>(m_meshletIndexMapped) + indexOffsetBytes,
			            meshletIndices.data() + indexStart,
			            static_cast<size_t>(indexUploadBytes));
		}
	}

	void GPUMeshletBuffer::ensureCapacities(uint32_t requiredMeshletCount, uint32_t requiredIndexCount)
	{
		if (requiredMeshletCount > m_meshletCapacity)
		{
			if (!m_meshletDataBuffer.isNull())
			{
				m_rhiDevice->destroyBuffer(m_meshletDataBuffer);
				m_meshletDataBuffer = {};
			}
			if (!m_meshletCullObjectBuffer.isNull())
			{
				m_rhiDevice->destroyBuffer(m_meshletCullObjectBuffer);
				m_meshletCullObjectBuffer = {};
			}
			m_meshletCapacity = std::max(requiredMeshletCount, std::max(64u, m_meshletCapacity * 2u));
			const uint64_t meshletBytes = sizeof(shaderio::Meshlet) * static_cast<uint64_t>(m_meshletCapacity);
			m_meshletDataBuffer = m_rhiDevice->createBuffer(rhi::BufferDesc{
				.size = meshletBytes,
				.usage = rhi::BufferUsageFlags::storage,
				.memoryUsage = rhi::MemoryUsage::cpuToGpu,
				.allowGpuAddress = true,
				.debugName = "GPUMeshletBuffer.meshletData",
			});
			m_meshletDataMapped = m_rhiDevice->mapBuffer(m_meshletDataBuffer);
			m_meshletDataAddress = m_rhiDevice->getBufferGpuAddress(m_meshletDataBuffer);
			const uint64_t cullObjectBytes =
				sizeof(shaderio::GPUCullObject) * static_cast<uint64_t>(m_meshletCapacity);
			m_meshletCullObjectBuffer = m_rhiDevice->createBuffer(rhi::BufferDesc{
				.size = cullObjectBytes,
				.usage = rhi::BufferUsageFlags::storage,
				.memoryUsage = rhi::MemoryUsage::cpuToGpu,
				.allowGpuAddress = true,
				.debugName = "GPUMeshletBuffer.cullObjects",
			});
			m_meshletCullObjectMapped = m_rhiDevice->mapBuffer(m_meshletCullObjectBuffer);
			m_meshletCullObjectAddress = m_rhiDevice->getBufferGpuAddress(m_meshletCullObjectBuffer);
		}

		if (requiredIndexCount > 0 && requiredIndexCount > m_meshletIndexCapacity)
		{
			if (!m_meshletIndexBuffer.isNull())
			{
				m_rhiDevice->destroyBuffer(m_meshletIndexBuffer);
				m_meshletIndexBuffer = {};
			}
			m_meshletIndexCapacity = std::max(requiredIndexCount, std::max(128u, m_meshletIndexCapacity * 2u));
			const uint64_t indexBytes = sizeof(uint32_t) * static_cast<uint64_t>(m_meshletIndexCapacity);
			m_meshletIndexBuffer = m_rhiDevice->createBuffer(rhi::BufferDesc{
				.size = indexBytes,
				.usage = rhi::BufferUsageFlags::storage | rhi::BufferUsageFlags::index,
				.memoryUsage = rhi::MemoryUsage::cpuToGpu,
				.allowGpuAddress = true,
				.debugName = "GPUMeshletBuffer.meshletIndices",
			});
			m_meshletIndexMapped = m_rhiDevice->mapBuffer(m_meshletIndexBuffer);
		}
	}

	void GPUMeshletBuffer::releaseBuffers()
	{
		if (m_rhiDevice != nullptr)
		{
			if (!m_meshletDataBuffer.isNull())
			{
				m_rhiDevice->destroyBuffer(m_meshletDataBuffer);
			}
			if (!m_meshletCullObjectBuffer.isNull())
			{
				m_rhiDevice->destroyBuffer(m_meshletCullObjectBuffer);
			}
			if (!m_meshletIndexBuffer.isNull())
			{
				m_rhiDevice->destroyBuffer(m_meshletIndexBuffer);
			}
		}
		m_meshletDataBuffer = {};
		m_meshletCullObjectBuffer = {};
		m_meshletIndexBuffer = {};
		m_meshletDataMapped = nullptr;
		m_meshletCullObjectMapped = nullptr;
		m_meshletIndexMapped = nullptr;
		m_meshletDataAddress = {};
		m_meshletCullObjectAddress = {};
	}
} // namespace demo
