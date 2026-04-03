#include "MeshPool.h"
#include "../loader/GltfLoader.h"

#include <cstring>

namespace demo {

void MeshPool::init(VkDevice device, VmaAllocator allocator) {
    m_device = device;
    m_allocator = allocator;
}

void MeshPool::deinit() {
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
    MeshRecord record;
    record.vertexCount = static_cast<uint32_t>(meshData.positions.size() / 3);
    record.indexCount = static_cast<uint32_t>(meshData.indices.size());
    record.transform = meshData.transform;

    // Build interleaved vertex buffer: Position(12) + Normal(12) + TexCoord(8) = 32 bytes
    std::vector<uint8_t> vertexData(record.vertexCount * 32);

    for (uint32_t i = 0; i < record.vertexCount; ++i) {
        float* dst = reinterpret_cast<float*>(&vertexData[i * 32]);

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
        .usage = VMA_MEMORY_USAGE_CPU_TO_GPU,
    };

    vmaCreateBuffer(m_allocator, &stagingInfo, &stagingAllocInfo,
                    &vertexStagingBuffer, &vertexStagingAllocation, nullptr);
    vmaMapMemory(m_allocator, vertexStagingAllocation, &vertexMappedData);
    std::memcpy(vertexMappedData, vertexData.data(), vertexData.size());
    vmaUnmapMemory(m_allocator, vertexStagingAllocation);

    // Create GPU vertex buffer
    vmaCreateBuffer(m_allocator, &vertexBufferInfo, &vertexAllocInfo,
                    &record.vertexBuffer, &record.vertexAllocation, nullptr);

    // Copy to GPU
    VkBufferCopy vertexCopy{.size = vertexData.size()};
    vkCmdCopyBuffer(cmd, vertexStagingBuffer, record.vertexBuffer, 1, &vertexCopy);

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
    vmaCreateBuffer(m_allocator, &stagingInfo, &stagingAllocInfo,
                    &indexStagingBuffer, &indexStagingAllocation, nullptr);
    vmaMapMemory(m_allocator, indexStagingAllocation, &indexMappedData);
    std::memcpy(indexMappedData, meshData.indices.data(), record.indexCount * sizeof(uint32_t));
    vmaUnmapMemory(m_allocator, indexStagingAllocation);

    // Create GPU index buffer
    vmaCreateBuffer(m_allocator, &indexBufferInfo, &vertexAllocInfo,
                    &record.indexBuffer, &record.indexAllocation, nullptr);

    // Copy to GPU
    VkBufferCopy indexCopy{.size = record.indexCount * sizeof(uint32_t)};
    vkCmdCopyBuffer(cmd, indexStagingBuffer, record.indexBuffer, 1, &indexCopy);

    // Cleanup staging buffers (will be freed after command buffer completes)
    vmaDestroyBuffer(m_allocator, vertexStagingBuffer, vertexStagingAllocation);
    vmaDestroyBuffer(m_allocator, indexStagingBuffer, indexStagingAllocation);

    return m_pool.emplace(std::move(record));
}

void MeshPool::destroyMesh(MeshHandle handle) {
    MeshRecord* record = m_pool.tryGet(handle);
    if (record == nullptr) {
        return;
    }

    if (record->vertexBuffer != VK_NULL_HANDLE) {
        vmaDestroyBuffer(m_allocator, record->vertexBuffer, record->vertexAllocation);
    }

    if (record->indexBuffer != VK_NULL_HANDLE) {
        vmaDestroyBuffer(m_allocator, record->indexBuffer, record->indexAllocation);
    }

    m_pool.destroy(handle);
}

const MeshRecord* MeshPool::tryGet(MeshHandle handle) const {
    return m_pool.tryGet(handle);
}

}  // namespace demo