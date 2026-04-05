#include "MeshPool.h"
#include "../loader/GltfLoader.h"

#include <cstring>

namespace demo {

void MeshPool::init(VkDevice device, VmaAllocator allocator) {
    m_device = device;
    m_allocator = allocator;
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

    m_device = VK_NULL_HANDLE;
    m_allocator = nullptr;
}

MeshHandle MeshPool::uploadMesh(const GltfMeshData& meshData, VkCommandBuffer cmd) {
    // Validate input
    if (meshData.positions.empty() || meshData.indices.empty()) {
        return kNullMeshHandle;
    }

    MeshRecord record;
    record.vertexCount = static_cast<uint32_t>(meshData.positions.size() / 3);
    record.indexCount = static_cast<uint32_t>(meshData.indices.size());
    record.transform = meshData.transform;
    record.materialIndex = meshData.materialIndex;

    // Build interleaved vertex buffer: Position(12) + Normal(12) + TexCoord(8) + Tangent(16) = 48 bytes
    std::vector<uint8_t> vertexData(record.vertexCount * 48);

    for (uint32_t i = 0; i < record.vertexCount; ++i) {
        float* dst = reinterpret_cast<float*>(&vertexData[i * 48]);

        // Position
        dst[0] = meshData.positions[i * 3 + 0];
        dst[1] = meshData.positions[i * 3 + 1];
        dst[2] = meshData.positions[i * 3 + 2];

        // Normal
        if (!meshData.normals.empty()) {
            dst[3] = meshData.normals[i * 3 + 0];
            dst[4] = meshData.normals[i * 3 + 1];
            dst[5] = meshData.normals[i * 3 + 2];
        } else {
            dst[3] = 0.0f;
            dst[4] = 1.0f;
            dst[5] = 0.0f;
        }

        // TexCoord
        if (!meshData.texCoords.empty()) {
            dst[6] = meshData.texCoords[i * 2 + 0];
            dst[7] = meshData.texCoords[i * 2 + 1];
        } else {
            dst[6] = 0.0f;
            dst[7] = 0.0f;
        }

        // Tangent
        if (!meshData.tangents.empty()) {
            dst[8] = meshData.tangents[i * 4 + 0];
            dst[9] = meshData.tangents[i * 4 + 1];
            dst[10] = meshData.tangents[i * 4 + 2];
            dst[11] = meshData.tangents[i * 4 + 3];
        } else {
            // Default tangent (right-handed)
            dst[8] = 1.0f;
            dst[9] = 0.0f;
            dst[10] = 0.0f;
            dst[11] = 1.0f;
        }
    }

    // Create vertex buffer
    VkBufferCreateInfo vertexBufferInfo{
        .sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO,
        .size = vertexData.size(),
        .usage = VK_BUFFER_USAGE_VERTEX_BUFFER_BIT | VK_BUFFER_USAGE_TRANSFER_DST_BIT,
        .sharingMode = VK_SHARING_MODE_EXCLUSIVE,
    };

    VmaAllocationCreateInfo vertexAllocInfo{
        .usage = VMA_MEMORY_USAGE_GPU_ONLY,
    };

    VkBuffer vertexStagingBuffer = VK_NULL_HANDLE;
    VmaAllocation vertexStagingAllocation = nullptr;
    void* vertexMappedData = nullptr;

    // Create staging buffer
    VkBufferCreateInfo stagingInfo{
        .sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO,
        .size = vertexData.size(),
        .usage = VK_BUFFER_USAGE_TRANSFER_SRC_BIT,
        .sharingMode = VK_SHARING_MODE_EXCLUSIVE,
    };

    VmaAllocationCreateInfo stagingAllocInfo{
        .flags = VMA_ALLOCATION_CREATE_HOST_ACCESS_SEQUENTIAL_WRITE_BIT,
        .usage = VMA_MEMORY_USAGE_CPU_ONLY,  // Use system RAM for staging, not device-local heap
    };

    VK_CHECK(vmaCreateBuffer(m_allocator, &stagingInfo, &stagingAllocInfo,
                    &vertexStagingBuffer, &vertexStagingAllocation, nullptr));
    VK_CHECK(vmaMapMemory(m_allocator, vertexStagingAllocation, &vertexMappedData));
    std::memcpy(vertexMappedData, vertexData.data(), vertexData.size());
    vmaUnmapMemory(m_allocator, vertexStagingAllocation);

    // Create GPU vertex buffer
    VkBuffer vertexBuffer = VK_NULL_HANDLE;
    VK_CHECK(vmaCreateBuffer(m_allocator, &vertexBufferInfo, &vertexAllocInfo,
                    &vertexBuffer, &record.vertexAllocation, nullptr));
    record.setNativeVertexBuffer(vertexBuffer);

    // Copy to GPU
    VkBufferCopy vertexCopy{.size = vertexData.size()};
    vkCmdCopyBuffer(cmd, vertexStagingBuffer, vertexBuffer, 1, &vertexCopy);

    // Create index buffer
    VkBufferCreateInfo indexBufferInfo{
        .sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO,
        .size = record.indexCount * sizeof(uint32_t),
        .usage = VK_BUFFER_USAGE_INDEX_BUFFER_BIT | VK_BUFFER_USAGE_TRANSFER_DST_BIT,
        .sharingMode = VK_SHARING_MODE_EXCLUSIVE,
    };

    VkBuffer indexStagingBuffer = VK_NULL_HANDLE;
    VmaAllocation indexStagingAllocation = nullptr;
    void* indexMappedData = nullptr;

    stagingInfo.size = record.indexCount * sizeof(uint32_t);
    VK_CHECK(vmaCreateBuffer(m_allocator, &stagingInfo, &stagingAllocInfo,
                    &indexStagingBuffer, &indexStagingAllocation, nullptr));
    VK_CHECK(vmaMapMemory(m_allocator, indexStagingAllocation, &indexMappedData));
    std::memcpy(indexMappedData, meshData.indices.data(), record.indexCount * sizeof(uint32_t));
    vmaUnmapMemory(m_allocator, indexStagingAllocation);

    // Create GPU index buffer
    VkBuffer indexBuffer = VK_NULL_HANDLE;
    VK_CHECK(vmaCreateBuffer(m_allocator, &indexBufferInfo, &vertexAllocInfo,
                    &indexBuffer, &record.indexAllocation, nullptr));
    record.setNativeIndexBuffer(indexBuffer);

    // Copy to GPU
    VkBufferCopy indexCopy{.size = record.indexCount * sizeof(uint32_t)};
    vkCmdCopyBuffer(cmd, indexStagingBuffer, indexBuffer, 1, &indexCopy);

    // Store staging buffers for deferred deletion after GPU sync
    m_stagingBuffers.push_back({vertexStagingBuffer, vertexStagingAllocation});
    m_stagingBuffers.push_back({indexStagingBuffer, indexStagingAllocation});

    return m_pool.emplace(std::move(record));
}

void MeshPool::destroyMesh(MeshHandle handle) {
    MeshRecord* record = m_pool.tryGet(handle);
    if (record == nullptr) {
        return;
    }

    VkBuffer vertexBuffer = record->getNativeVertexBuffer();
    VkBuffer indexBuffer = record->getNativeIndexBuffer();

    if (vertexBuffer != VK_NULL_HANDLE) {
        vmaDestroyBuffer(m_allocator, vertexBuffer, record->vertexAllocation);
    }

    if (indexBuffer != VK_NULL_HANDLE) {
        vmaDestroyBuffer(m_allocator, indexBuffer, record->indexAllocation);
    }

    m_pool.destroy(handle);
}

const MeshRecord* MeshPool::tryGet(MeshHandle handle) const {
    return m_pool.tryGet(handle);
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