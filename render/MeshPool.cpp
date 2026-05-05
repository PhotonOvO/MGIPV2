#include "MeshPool.h"
#include "BatchUploadContext.h"
#include "../loader/GltfLoader.h"

#include <array>
#include <cstring>
#include <limits>
#include <span>

namespace demo {

namespace {

constexpr uint32_t kInterleavedVertexStride = 48;
constexpr VkDeviceSize kInitialSharedVertexCapacity = 4ull * 1024ull * 1024ull;
constexpr VkDeviceSize kInitialSharedIndexCapacity = 2ull * 1024ull * 1024ull;

void updateWorldBounds(MeshRecord& record)
{
    const std::array<glm::vec3, 8> localCorners{{
        {record.localBoundsMin.x, record.localBoundsMin.y, record.localBoundsMin.z},
        {record.localBoundsMax.x, record.localBoundsMin.y, record.localBoundsMin.z},
        {record.localBoundsMin.x, record.localBoundsMax.y, record.localBoundsMin.z},
        {record.localBoundsMax.x, record.localBoundsMax.y, record.localBoundsMin.z},
        {record.localBoundsMin.x, record.localBoundsMin.y, record.localBoundsMax.z},
        {record.localBoundsMax.x, record.localBoundsMin.y, record.localBoundsMax.z},
        {record.localBoundsMin.x, record.localBoundsMax.y, record.localBoundsMax.z},
        {record.localBoundsMax.x, record.localBoundsMax.y, record.localBoundsMax.z},
    }};

    record.worldBoundsMin = glm::vec3(std::numeric_limits<float>::max());
    record.worldBoundsMax = glm::vec3(std::numeric_limits<float>::lowest());
    for(const glm::vec3& localCorner : localCorners)
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

    for (uint32_t i = 0; i < record.vertexCount; ++i) {
        float* dst = reinterpret_cast<float*>(vertexData.data() + static_cast<size_t>(i) * kInterleavedVertexStride);

        dst[0] = meshData.positions[i * 3 + 0];
        dst[1] = meshData.positions[i * 3 + 1];
        dst[2] = meshData.positions[i * 3 + 2];

        const glm::vec3 position(dst[0], dst[1], dst[2]);
        record.localBoundsMin = glm::min(record.localBoundsMin, position);
        record.localBoundsMax = glm::max(record.localBoundsMax, position);

        if (!meshData.normals.empty()) {
            dst[3] = meshData.normals[i * 3 + 0];
            dst[4] = meshData.normals[i * 3 + 1];
            dst[5] = meshData.normals[i * 3 + 2];
        } else {
            dst[3] = 0.0f;
            dst[4] = 1.0f;
            dst[5] = 0.0f;
        }

        if (!meshData.texCoords.empty()) {
            dst[6] = meshData.texCoords[i * 2 + 0];
            dst[7] = meshData.texCoords[i * 2 + 1];
        } else {
            dst[6] = 0.0f;
            dst[7] = 0.0f;
        }

        if (!meshData.tangents.empty()) {
            dst[8] = meshData.tangents[i * 4 + 0];
            dst[9] = meshData.tangents[i * 4 + 1];
            dst[10] = meshData.tangents[i * 4 + 2];
            dst[11] = meshData.tangents[i * 4 + 3];
        } else {
            dst[8] = 1.0f;
            dst[9] = 0.0f;
            dst[10] = 0.0f;
            dst[11] = 1.0f;
        }
    }
}

}  // namespace

void MeshPool::init(VkDevice device, VmaAllocator allocator, upload::StaticBufferUploadPolicy staticUploadPolicy) {
    m_device = device;
    m_allocator = allocator;
    m_staticUploadPolicy = staticUploadPolicy;
}

void MeshPool::deinit() {
    // Free any remaining staging buffers
    freeStagingBuffers();

    // Destroy all remaining meshes
    std::vector<MeshHandle> handles;
    m_pool.forEachActive([&handles](MeshHandle handle, const MeshRecord&) {
        handles.push_back(handle);
    });

    for (MeshHandle handle : handles) {
        destroyMesh(handle);
    }

    resetSharedBuffers();

    m_device = VK_NULL_HANDLE;
    m_allocator = nullptr;
}

void MeshPool::ensureSharedCapacity(SharedBufferArena& arena,
                                    VkDeviceSize requiredSize,
                                    VkBufferUsageFlags2KHR usage,
                                    VkCommandBuffer cmd)
{
    if(requiredSize <= arena.capacity)
    {
        return;
    }

    VkDeviceSize newCapacity = arena.capacity == 0 ? requiredSize : arena.capacity;
    while(newCapacity < requiredSize)
    {
        newCapacity = std::max(newCapacity * 2, requiredSize);
    }

    if(usage & VK_BUFFER_USAGE_2_VERTEX_BUFFER_BIT_KHR)
    {
        newCapacity = std::max(newCapacity, kInitialSharedVertexCapacity);
    }
    if(usage & VK_BUFFER_USAGE_2_INDEX_BUFFER_BIT_KHR)
    {
        newCapacity = std::max(newCapacity, kInitialSharedIndexCapacity);
    }

    utils::Buffer newBuffer = upload::createStaticBuffer(m_device, m_allocator, newCapacity, usage);
    const bool replacingVertexArena = (usage & VK_BUFFER_USAGE_2_VERTEX_BUFFER_BIT_KHR) != 0;
    const bool replacingIndexArena = (usage & VK_BUFFER_USAGE_2_INDEX_BUFFER_BIT_KHR) != 0;
    if(arena.buffer.buffer != VK_NULL_HANDLE && arena.bytesUsed > 0)
    {
        const VkBufferCopy copyRegion{.size = arena.bytesUsed};
        vkCmdCopyBuffer(cmd, arena.buffer.buffer, newBuffer.buffer, 1, &copyRegion);
        m_stagingBuffers.push_back(arena.buffer);
    }

    arena.buffer = newBuffer;
    arena.capacity = newCapacity;

    if(replacingVertexArena || replacingIndexArena)
    {
        m_pool.forEachActive([&](MeshHandle, MeshRecord& record) {
            if(replacingVertexArena)
            {
                record.setNativeVertexBuffer(newBuffer.buffer);
            }
            if(replacingIndexArena)
            {
                record.setNativeIndexBuffer(newBuffer.buffer);
            }
        });
    }
}

void MeshPool::reserve(VkDeviceSize additionalVertexBytes, VkDeviceSize additionalIndexBytes, VkCommandBuffer cmd)
{
    ensureSharedCapacity(m_sharedVertexBuffer,
                         m_sharedVertexBuffer.bytesUsed + additionalVertexBytes,
                         VK_BUFFER_USAGE_2_VERTEX_BUFFER_BIT_KHR,
                         cmd);
    ensureSharedCapacity(m_sharedIndexBuffer,
                         m_sharedIndexBuffer.bytesUsed + additionalIndexBytes,
                         VK_BUFFER_USAGE_2_INDEX_BUFFER_BIT_KHR,
                         cmd);
}

MeshHandle MeshPool::uploadMesh(const GltfMeshData& meshData, VkCommandBuffer cmd, BatchUploadContext* batchUpload) {
    // Validate input
    if (meshData.positions.empty() || meshData.indices.empty()) {
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
    record.setNativeVertexBuffer(m_sharedVertexBuffer.buffer.buffer);
    record.setNativeIndexBuffer(m_sharedIndexBuffer.buffer.buffer);

    if(batchUpload != nullptr)
    {
        const BatchUploadContext::Slice vertexSlice = batchUpload->allocate(vertexDataSize, alignof(float));
        buildInterleavedVertexData(meshData,
                                   record,
                                   std::span<uint8_t>(static_cast<uint8_t*>(vertexSlice.cpuPtr), vertexDataSize));
        batchUpload->recordBufferUpload(vertexSlice,
                                        m_sharedVertexBuffer.buffer.buffer,
                                        VkBufferCopy{.dstOffset = record.vertexBufferOffset, .size = vertexDataSize});

        const BatchUploadContext::Slice indexSlice = batchUpload->allocate(indexDataSize, alignof(uint32_t));
        std::memcpy(indexSlice.cpuPtr, meshData.indices.data(), indexDataSize);
        batchUpload->recordBufferUpload(indexSlice,
                                        m_sharedIndexBuffer.buffer.buffer,
                                        VkBufferCopy{.dstOffset = record.indexBufferOffset, .size = indexDataSize});
    }
    else
    {
        std::vector<uint8_t> vertexData(vertexDataSize);
        buildInterleavedVertexData(meshData, record, vertexData);

        const std::span<const std::byte> vertexBytes(reinterpret_cast<const std::byte*>(vertexData.data()), vertexData.size());
        const std::span<const std::byte> indexBytes(reinterpret_cast<const std::byte*>(meshData.indices.data()),
                                                    indexDataSize);

        utils::Buffer vertexStagingBuffer = upload::createUploadStagingBuffer(m_device, m_allocator, vertexBytes);
        utils::Buffer indexStagingBuffer = upload::createUploadStagingBuffer(m_device, m_allocator, indexBytes);

        const VkBufferCopy vertexCopy{.dstOffset = record.vertexBufferOffset, .size = vertexDataSize};
        const VkBufferCopy indexCopy{.dstOffset = record.indexBufferOffset, .size = indexDataSize};
        vkCmdCopyBuffer(cmd, vertexStagingBuffer.buffer, m_sharedVertexBuffer.buffer.buffer, 1, &vertexCopy);
        vkCmdCopyBuffer(cmd, indexStagingBuffer.buffer, m_sharedIndexBuffer.buffer.buffer, 1, &indexCopy);

        m_stagingBuffers.push_back(vertexStagingBuffer);
        m_stagingBuffers.push_back(indexStagingBuffer);
    }

    m_sharedVertexBuffer.bytesUsed += vertexDataSize;
    m_sharedIndexBuffer.bytesUsed += indexDataSize;

    updateWorldBounds(record);

    return m_pool.emplace(std::move(record));
}

void MeshPool::destroyMesh(MeshHandle handle) {
    MeshRecord* record = m_pool.tryGet(handle);
    if (record == nullptr) {
        return;
    }

    VkBuffer vertexBuffer = record->getNativeVertexBuffer();
    VkBuffer indexBuffer = record->getNativeIndexBuffer();

    if (record->vertexAllocation != nullptr && vertexBuffer != VK_NULL_HANDLE) {
        vmaDestroyBuffer(m_allocator, vertexBuffer, record->vertexAllocation);
    }

    if (record->indexAllocation != nullptr && indexBuffer != VK_NULL_HANDLE) {
        vmaDestroyBuffer(m_allocator, indexBuffer, record->indexAllocation);
    }

    m_pool.destroy(handle);
    if(m_pool.liveCount() == 0)
    {
        resetSharedBuffers();
    }
}

void MeshPool::updateTransform(MeshHandle handle, const glm::mat4& transform)
{
    MeshRecord* record = m_pool.tryGet(handle);
    if(record == nullptr)
    {
        return;
    }

    record->transform = transform;
    updateWorldBounds(*record);
}

void MeshPool::setMeshAlphaMode(MeshHandle handle, int32_t alphaMode, float alphaCutoff)
{
    MeshRecord* record = m_pool.tryGet(handle);
    if(record == nullptr)
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
                                   float metallicFactor,
                                   float roughnessFactor,
                                   float normalScale)
{
    MeshRecord* record = m_pool.tryGet(handle);
    if(record == nullptr)
    {
        return;
    }

    record->baseColorFactor = baseColorFactor;
    record->baseColorTextureIndex = baseColorTextureIndex;
    record->normalTextureIndex = normalTextureIndex;
    record->metallicRoughnessTextureIndex = metallicRoughnessTextureIndex;
    record->occlusionTextureIndex = occlusionTextureIndex;
    record->metallicFactor = metallicFactor;
    record->roughnessFactor = roughnessFactor;
    record->normalScale = normalScale;
}

const MeshRecord* MeshPool::tryGet(MeshHandle handle) const {
    return m_pool.tryGet(handle);
}

uint64_t MeshPool::getSharedVertexBufferHandle() const
{
    return static_cast<uint64_t>(reinterpret_cast<uintptr_t>(m_sharedVertexBuffer.buffer.buffer));
}

uint64_t MeshPool::getSharedIndexBufferHandle() const
{
    return static_cast<uint64_t>(reinterpret_cast<uintptr_t>(m_sharedIndexBuffer.buffer.buffer));
}

void MeshPool::resetSharedBuffers()
{
    if(m_sharedVertexBuffer.buffer.buffer != VK_NULL_HANDLE)
    {
        vmaDestroyBuffer(m_allocator, m_sharedVertexBuffer.buffer.buffer, m_sharedVertexBuffer.buffer.allocation);
    }
    if(m_sharedIndexBuffer.buffer.buffer != VK_NULL_HANDLE)
    {
        vmaDestroyBuffer(m_allocator, m_sharedIndexBuffer.buffer.buffer, m_sharedIndexBuffer.buffer.allocation);
    }
    m_sharedVertexBuffer = {};
    m_sharedIndexBuffer = {};
}

void MeshPool::freeStagingBuffers() {
    for (auto& buffer : m_stagingBuffers) {
        if (buffer.buffer != VK_NULL_HANDLE) {
            vmaDestroyBuffer(m_allocator, buffer.buffer, buffer.allocation);
        }
    }
    m_stagingBuffers.clear();
}

}  // namespace demo
