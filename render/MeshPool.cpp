#include "MeshPool.h"
#include "BatchUploadContext.h"
#include "../loader/GltfLoader.h"
#include "../scene/SceneAsset.h"
#include "../rhi/RHIDevice.h"
#include "../rhi/RHIEncoder.h"

#include <array>
#include <cstring>
#include <limits>
#include <span>

namespace demo
{
	namespace
	{
		constexpr uint32_t kInterleavedVertexStride = 48;
		constexpr uint64_t kInitialSharedVertexCapacity = 4ull * 1024ull * 1024ull;
		constexpr uint64_t kInitialSharedIndexCapacity = 2ull * 1024ull * 1024ull;

		void updateWorldBounds(MeshRecord& record)
		{
			const std::array<glm::vec3, 8> localCorners{
				{
					{record.localBoundsMin.x, record.localBoundsMin.y, record.localBoundsMin.z},
					{record.localBoundsMax.x, record.localBoundsMin.y, record.localBoundsMin.z},
					{record.localBoundsMin.x, record.localBoundsMax.y, record.localBoundsMin.z},
					{record.localBoundsMax.x, record.localBoundsMax.y, record.localBoundsMin.z},
					{record.localBoundsMin.x, record.localBoundsMin.y, record.localBoundsMax.z},
					{record.localBoundsMax.x, record.localBoundsMin.y, record.localBoundsMax.z},
					{record.localBoundsMin.x, record.localBoundsMax.y, record.localBoundsMax.z},
					{record.localBoundsMax.x, record.localBoundsMax.y, record.localBoundsMax.z},
				}
			};

			record.worldBoundsMin = glm::vec3(std::numeric_limits<float>::max());
			record.worldBoundsMax = glm::vec3(std::numeric_limits<float>::lowest());
			for (const glm::vec3& localCorner : localCorners)
			{
				const glm::vec3 worldCorner = glm::vec3(record.transform * glm::vec4(localCorner, 1.0f));
				record.worldBoundsMin = glm::min(record.worldBoundsMin, worldCorner);
				record.worldBoundsMax = glm::max(record.worldBoundsMax, worldCorner);
			}

			record.worldBoundsCenter = 0.5f * (record.worldBoundsMin + record.worldBoundsMax);
			record.worldBoundsRadius = glm::length(record.worldBoundsMax - record.worldBoundsCenter);
		}

		void buildInterleavedVertexData(const GltfMeshData& meshData, MeshRecord& record, std::span<uint8_t> vertexData)
		{
			ASSERT(vertexData.size() == static_cast<size_t>(record.vertexCount) * kInterleavedVertexStride,
			       "Interleaved vertex buffer size mismatch");

			for (uint32_t i = 0; i < record.vertexCount; ++i)
			{
				float* dst = reinterpret_cast<float*>(vertexData.data() + static_cast<size_t>(i) *
					kInterleavedVertexStride);

				dst[0] = meshData.positions[i * 3 + 0];
				dst[1] = meshData.positions[i * 3 + 1];
				dst[2] = meshData.positions[i * 3 + 2];

				const glm::vec3 position(dst[0], dst[1], dst[2]);
				record.localBoundsMin = glm::min(record.localBoundsMin, position);
				record.localBoundsMax = glm::max(record.localBoundsMax, position);

				if (!meshData.normals.empty())
				{
					dst[3] = meshData.normals[i * 3 + 0];
					dst[4] = meshData.normals[i * 3 + 1];
					dst[5] = meshData.normals[i * 3 + 2];
				}
				else
				{
					dst[3] = 0.0f;
					dst[4] = 1.0f;
					dst[5] = 0.0f;
				}

				if (!meshData.texCoords.empty())
				{
					dst[6] = meshData.texCoords[i * 2 + 0];
					dst[7] = meshData.texCoords[i * 2 + 1];
				}
				else
				{
					dst[6] = 0.0f;
					dst[7] = 0.0f;
				}

				if (!meshData.tangents.empty())
				{
					dst[8] = meshData.tangents[i * 4 + 0];
					dst[9] = meshData.tangents[i * 4 + 1];
					dst[10] = meshData.tangents[i * 4 + 2];
					dst[11] = meshData.tangents[i * 4 + 3];
				}
				else
				{
					dst[8] = 1.0f;
					dst[9] = 0.0f;
					dst[10] = 0.0f;
					dst[11] = 1.0f;
				}
			}
		}

		void updateLocalBoundsFromInterleavedVertices(MeshRecord& record, std::span<const uint8_t> vertexData)
		{
			ASSERT(vertexData.size() == static_cast<size_t>(record.vertexCount) * kInterleavedVertexStride,
			       "Interleaved vertex buffer size mismatch");

			for (uint32_t i = 0; i < record.vertexCount; ++i)
			{
				const float* src = reinterpret_cast<const float*>(vertexData.data() + static_cast<size_t>(i) *
					kInterleavedVertexStride);
				const glm::vec3 position(src[0], src[1], src[2]);
				record.localBoundsMin = glm::min(record.localBoundsMin, position);
				record.localBoundsMax = glm::max(record.localBoundsMax, position);
			}
		}
	} // namespace

	void MeshPool::init(rhi::Device* rhiDevice, upload::StaticBufferUploadPolicy staticUploadPolicy)
	{
		m_rhiDevice = rhiDevice;
		m_staticUploadPolicy = staticUploadPolicy;
	}

	void MeshPool::deinit()
	{
		// Free any remaining staging buffers
		freeStagingBuffers();

		// Destroy all remaining meshes
		std::vector<MeshHandle> handles;
		m_pool.forEachActive([&handles](MeshHandle handle, const MeshRecord&)
		{
			handles.push_back(handle);
		});

		for (MeshHandle handle : handles)
		{
			destroyMesh(handle);
		}

		resetSharedBuffers();

		m_rhiDevice = nullptr;
	}

	void MeshPool::ensureSharedCapacity(SharedBufferArena& arena,
	                                    uint64_t requiredSize,
	                                    rhi::BufferUsageFlags usage,
	                                    rhi::CommandBuffer& cmd)
	{
		if (requiredSize <= arena.capacity)
		{
			return;
		}

		uint64_t newCapacity = arena.capacity == 0 ? requiredSize : arena.capacity;
		while (newCapacity < requiredSize)
		{
			newCapacity = std::max(newCapacity * 2, requiredSize);
		}

		if (static_cast<uint32_t>(usage & rhi::BufferUsageFlags::vertex) != 0)
		{
			newCapacity = std::max(newCapacity, kInitialSharedVertexCapacity);
		}
		if (static_cast<uint32_t>(usage & rhi::BufferUsageFlags::index) != 0)
		{
			newCapacity = std::max(newCapacity, kInitialSharedIndexCapacity);
		}

		ASSERT(m_rhiDevice != nullptr, "MeshPool RHI device is required for arena allocation");
		const rhi::BufferHandle newHandle = m_rhiDevice->createBuffer(rhi::BufferDesc{
			.size = newCapacity,
			.usage = usage | rhi::BufferUsageFlags::transferDst,
			.memoryUsage = rhi::MemoryUsage::gpuOnly,
			.debugName = "MeshPool.sharedArena",
		});
		const bool replacingVertexArena = static_cast<uint32_t>(usage & rhi::BufferUsageFlags::vertex) != 0;
		const bool replacingIndexArena = static_cast<uint32_t>(usage & rhi::BufferUsageFlags::index) != 0;
		if (!arena.buffer.isNull() && arena.bytesUsed > 0)
		{
			rhi::ComputeEncoder* copy = cmd.beginComputePass();
			copy->copyBuffer(arena.buffer, 0, newHandle, 0, arena.bytesUsed);
			cmd.endEncoding();
		}
		if (!arena.buffer.isNull())
		{
			// Defer destruction until after GPU sync: the copy above (and any in-flight
			// frame) may still read the old arena buffer.
			m_rhiStagingBuffers.push_back(arena.buffer);
		}

		arena.buffer = newHandle;
		arena.capacity = newCapacity;

		if (replacingVertexArena || replacingIndexArena)
		{
			m_pool.forEachActive([&](MeshHandle, MeshRecord& record)
			{
				if (replacingVertexArena)
				{
					record.vertexBuffer = newHandle;
				}
				if (replacingIndexArena)
				{
					record.indexBuffer = newHandle;
				}
			});
		}
	}

	void MeshPool::reserve(uint64_t additionalVertexBytes, uint64_t additionalIndexBytes, rhi::CommandBuffer& cmd)
	{
		ensureSharedCapacity(m_sharedVertexBuffer,
		                     m_sharedVertexBuffer.bytesUsed + additionalVertexBytes,
		                     rhi::BufferUsageFlags::vertex | rhi::BufferUsageFlags::transferSrc,
		                     cmd);
		ensureSharedCapacity(m_sharedIndexBuffer,
		                     m_sharedIndexBuffer.bytesUsed + additionalIndexBytes,
		                     rhi::BufferUsageFlags::index | rhi::BufferUsageFlags::transferSrc,
		                     cmd);
	}

	MeshHandle MeshPool::uploadMesh(const GltfMeshData& meshData, rhi::CommandBuffer& cmd,
	                                BatchUploadContext* batchUpload)
	{
		// Validate input
		if (meshData.positions.empty() || meshData.indices.empty())
		{
			return kNullMeshHandle;
		}

		MeshRecord record;
		record.vertexCount = static_cast<uint32_t>(meshData.positions.size() / 3);
		record.indexCount = static_cast<uint32_t>(meshData.indices.size());
		record.firstIndex = static_cast<uint32_t>(m_sharedIndexBuffer.bytesUsed / sizeof(uint32_t));
		record.vertexOffset = static_cast<int32_t>(m_sharedVertexBuffer.bytesUsed / kInterleavedVertexStride);
		record.vertexStride = kInterleavedVertexStride;
		record.transform = meshData.transform;
		record.materialIndex = meshData.materialIndex;
		record.localBoundsMin = glm::vec3(std::numeric_limits<float>::max());
		record.localBoundsMax = glm::vec3(std::numeric_limits<float>::lowest());

		const size_t vertexDataSize = static_cast<size_t>(record.vertexCount) * kInterleavedVertexStride;
		const size_t indexDataSize = static_cast<size_t>(record.indexCount) * sizeof(uint32_t);

		reserve(vertexDataSize, indexDataSize, cmd);

		record.vertexBufferOffset = m_sharedVertexBuffer.bytesUsed;
		record.indexBufferOffset = m_sharedIndexBuffer.bytesUsed;
		record.vertexBuffer = m_sharedVertexBuffer.buffer;
		record.indexBuffer = m_sharedIndexBuffer.buffer;

		if (batchUpload != nullptr)
		{
			const BatchUploadContext::Slice vertexSlice = batchUpload->allocate(vertexDataSize, alignof(float));
			buildInterleavedVertexData(meshData,
			                           record,
			                           std::span<uint8_t>(static_cast<uint8_t*>(vertexSlice.cpuPtr), vertexDataSize));
			batchUpload->recordBufferUpload(vertexSlice,
			                                m_sharedVertexBuffer.buffer,
			                                record.vertexBufferOffset,
			                                vertexDataSize);

			const BatchUploadContext::Slice indexSlice = batchUpload->allocate(indexDataSize, alignof(uint32_t));
			std::memcpy(indexSlice.cpuPtr, meshData.indices.data(), indexDataSize);
			batchUpload->recordBufferUpload(indexSlice,
			                                m_sharedIndexBuffer.buffer,
			                                record.indexBufferOffset,
			                                indexDataSize);
		}
		else
		{
			std::vector<uint8_t> vertexData(vertexDataSize);
			buildInterleavedVertexData(meshData, record, vertexData);

			const std::span<const std::byte> vertexBytes(reinterpret_cast<const std::byte*>(vertexData.data()),
			                                             vertexData.size());
			const std::span<const std::byte> indexBytes(reinterpret_cast<const std::byte*>(meshData.indices.data()),
			                                            indexDataSize);

			ASSERT(m_rhiDevice != nullptr, "MeshPool RHI device is required for non-batched upload");
			rhi::BufferHandle vertexStagingBuffer = upload::createUploadStagingBuffer(*m_rhiDevice, vertexBytes);
			rhi::BufferHandle indexStagingBuffer = upload::createUploadStagingBuffer(*m_rhiDevice, indexBytes);

			rhi::ComputeEncoder* copy = cmd.beginComputePass();
			copy->copyBuffer(vertexStagingBuffer, 0, m_sharedVertexBuffer.buffer, record.vertexBufferOffset,
			                 vertexDataSize);
			copy->copyBuffer(indexStagingBuffer, 0, m_sharedIndexBuffer.buffer, record.indexBufferOffset, indexDataSize);
			cmd.endEncoding();

			m_rhiStagingBuffers.push_back(vertexStagingBuffer);
			m_rhiStagingBuffers.push_back(indexStagingBuffer);
		}

		m_sharedVertexBuffer.bytesUsed += vertexDataSize;
		m_sharedIndexBuffer.bytesUsed += indexDataSize;

		updateWorldBounds(record);

		return m_pool.emplace(std::move(record));
	}

	MeshHandle MeshPool::uploadMesh(const SceneMeshData& meshData, rhi::CommandBuffer& cmd,
	                                BatchUploadContext* batchUpload)
	{
		if (meshData.interleavedVertexData.empty() || meshData.indices.empty() || meshData.vertexCount == 0)
		{
			return kNullMeshHandle;
		}

		MeshRecord record;
		record.vertexCount = meshData.vertexCount;
		record.indexCount = static_cast<uint32_t>(meshData.indices.size());
		record.firstIndex = static_cast<uint32_t>(m_sharedIndexBuffer.bytesUsed / sizeof(uint32_t));
		record.vertexOffset = static_cast<int32_t>(m_sharedVertexBuffer.bytesUsed / kInterleavedVertexStride);
		record.vertexStride = kInterleavedVertexStride;
		record.transform = meshData.transform;
		record.materialIndex = meshData.materialIndex;
		record.localBoundsMin = glm::vec3(std::numeric_limits<float>::max());
		record.localBoundsMax = glm::vec3(std::numeric_limits<float>::lowest());

		const size_t vertexDataSize = meshData.interleavedVertexData.size();
		const size_t indexDataSize = meshData.indices.size_bytes();

		reserve(vertexDataSize, indexDataSize, cmd);

		record.vertexBufferOffset = m_sharedVertexBuffer.bytesUsed;
		record.indexBufferOffset = m_sharedIndexBuffer.bytesUsed;
		record.vertexBuffer = m_sharedVertexBuffer.buffer;
		record.indexBuffer = m_sharedIndexBuffer.buffer;
		updateLocalBoundsFromInterleavedVertices(record, meshData.interleavedVertexData);

		if (batchUpload != nullptr)
		{
			const BatchUploadContext::Slice vertexSlice = batchUpload->allocate(vertexDataSize, alignof(float));
			std::memcpy(vertexSlice.cpuPtr, meshData.interleavedVertexData.data(), vertexDataSize);
			batchUpload->recordBufferUpload(vertexSlice,
			                                m_sharedVertexBuffer.buffer,
			                                record.vertexBufferOffset,
			                                vertexDataSize);

			const BatchUploadContext::Slice indexSlice = batchUpload->allocate(indexDataSize, alignof(uint32_t));
			std::memcpy(indexSlice.cpuPtr, meshData.indices.data(), indexDataSize);
			batchUpload->recordBufferUpload(indexSlice,
			                                m_sharedIndexBuffer.buffer,
			                                record.indexBufferOffset,
			                                indexDataSize);
		}
		else
		{
			const std::span<const std::byte> vertexBytes(
				reinterpret_cast<const std::byte*>(meshData.interleavedVertexData.data()), vertexDataSize);
			const std::span<const std::byte> indexBytes(reinterpret_cast<const std::byte*>(meshData.indices.data()),
			                                            indexDataSize);

			ASSERT(m_rhiDevice != nullptr, "MeshPool RHI device is required for non-batched upload");
			rhi::BufferHandle vertexStagingBuffer = upload::createUploadStagingBuffer(*m_rhiDevice, vertexBytes);
			rhi::BufferHandle indexStagingBuffer = upload::createUploadStagingBuffer(*m_rhiDevice, indexBytes);

			rhi::ComputeEncoder* copy = cmd.beginComputePass();
			copy->copyBuffer(vertexStagingBuffer, 0, m_sharedVertexBuffer.buffer, record.vertexBufferOffset,
			                 vertexDataSize);
			copy->copyBuffer(indexStagingBuffer, 0, m_sharedIndexBuffer.buffer, record.indexBufferOffset, indexDataSize);
			cmd.endEncoding();

			m_rhiStagingBuffers.push_back(vertexStagingBuffer);
			m_rhiStagingBuffers.push_back(indexStagingBuffer);
		}

		m_sharedVertexBuffer.bytesUsed += vertexDataSize;
		m_sharedIndexBuffer.bytesUsed += indexDataSize;

		updateWorldBounds(record);

		return m_pool.emplace(std::move(record));
	}

	void MeshPool::destroyMesh(MeshHandle handle)
	{
		MeshRecord* record = m_pool.tryGet(handle);
		if (record == nullptr)
		{
			return;
		}

		m_pool.destroy(handle);
		if (m_pool.liveCount() == 0)
		{
			resetSharedBuffers();
		}
	}

	void MeshPool::updateTransform(MeshHandle handle, const glm::mat4& transform)
	{
		MeshRecord* record = m_pool.tryGet(handle);
		if (record == nullptr)
		{
			return;
		}

		record->transform = transform;
		updateWorldBounds(*record);
	}

	void MeshPool::setMeshAlphaMode(MeshHandle handle, int32_t alphaMode, float alphaCutoff)
	{
		MeshRecord* record = m_pool.tryGet(handle);
		if (record == nullptr)
		{
			return;
		}

		record->alphaMode = alphaMode;
		record->alphaCutoff = alphaCutoff;
	}

	void MeshPool::setMeshMaterialData(MeshHandle handle,
	                                   const glm::vec4& baseColorFactor,
	                                   int32_t baseColorTextureIndex,
	                                   int32_t normalTextureIndex,
	                                   int32_t metallicRoughnessTextureIndex,
	                                   int32_t occlusionTextureIndex,
	                                   int32_t emissiveTextureIndex,
	                                   float metallicFactor,
	                                   float roughnessFactor,
	                                   float normalScale,
	                                   float occlusionStrength,
	                                   const glm::vec4& emissiveFactor,
	                                   int32_t materialWorkflow)
	{
		MeshRecord* record = m_pool.tryGet(handle);
		if (record == nullptr)
		{
			return;
		}

		record->baseColorFactor = baseColorFactor;
		record->baseColorTextureIndex = baseColorTextureIndex;
		record->normalTextureIndex = normalTextureIndex;
		record->metallicRoughnessTextureIndex = metallicRoughnessTextureIndex;
		record->occlusionTextureIndex = occlusionTextureIndex;
		record->emissiveTextureIndex = emissiveTextureIndex;
		record->metallicFactor = metallicFactor;
		record->roughnessFactor = roughnessFactor;
		record->normalScale = normalScale;
		record->occlusionStrength = occlusionStrength;
		record->emissiveFactor = emissiveFactor;
		record->materialWorkflow = materialWorkflow;
	}

	const MeshRecord* MeshPool::tryGet(MeshHandle handle) const
	{
		return m_pool.tryGet(handle);
	}

	size_t MeshPool::getDeferredStagingBufferCount() const
	{
		return m_rhiStagingBuffers.size();
	}

	uint64_t MeshPool::getDeferredStagingBufferBytes() const
	{
		// The RHI does not expose allocation sizes; this debug-only statistic is now an
		// approximation based on the deferred buffer count.
		return static_cast<uint64_t>(m_rhiStagingBuffers.size()) * kInitialSharedVertexCapacity;
	}

	void MeshPool::resetSharedBuffers()
	{
		if (m_rhiDevice != nullptr)
		{
			if (!m_sharedVertexBuffer.buffer.isNull()) m_rhiDevice->destroyBuffer(m_sharedVertexBuffer.buffer);
			if (!m_sharedIndexBuffer.buffer.isNull()) m_rhiDevice->destroyBuffer(m_sharedIndexBuffer.buffer);
		}
		m_sharedVertexBuffer = {};
		m_sharedIndexBuffer = {};
	}

	void MeshPool::deferStagingBuffer(rhi::BufferHandle buffer)
	{
		if (!buffer.isNull())
		{
			m_rhiStagingBuffers.push_back(buffer);
		}
	}

	void MeshPool::freeStagingBuffers()
	{
		if (m_rhiDevice != nullptr)
		{
			for (rhi::BufferHandle buffer : m_rhiStagingBuffers)
			{
				if (!buffer.isNull())
				{
					m_rhiDevice->destroyBuffer(buffer);
				}
			}
		}
		m_rhiStagingBuffers.clear();
	}
} // namespace demo
