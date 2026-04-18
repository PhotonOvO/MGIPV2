#include "MeshPool.h"
#include "../loader/GltfLoader.h"

#include <array>
#include <cstring>
#include <limits>

namespace demo {

namespace {

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
    record.localBoundsMin = glm::vec3(std::numeric_limits<float>::max());
    record.localBoundsMax = glm::vec3(std::numeric_limits<float>::lowest());

    // Build interleaved vertex buffer: Position(12) + Normal(12) + TexCoord(8) + Tangent(16) = 48 bytes
    std::vector<uint8_t> vertexData(record.vertexCount * 48);

    for (uint32_t i = 0; i < record.vertexCount; ++i) {
        float* dst = reinterpret_cast<float*>(&vertexData[i * 48]);

        // Position
        dst[0] = meshData.positions[i * 3 + 0];
        dst[1] = meshData.positions[i * 3 + 1];
        dst[2] = meshData.positions[i * 3 + 2];

        const glm::vec3 position(dst[0], dst[1], dst[2]);
        record.localBoundsMin = glm::min(record.localBoundsMin, position);
        record.localBoundsMax = glm::max(record.localBoundsMax, position);

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

    updateWorldBounds(record);

    const std::span<const std::byte> vertexBytes(reinterpret_cast<const std::byte*>(vertexData.data()), vertexData.size());
    const std::span<const std::byte> indexBytes(reinterpret_cast<const std::byte*>(meshData.indices.data()),
                                                record.indexCount * sizeof(uint32_t));

    const utils::Buffer vertexBuffer = upload::createStaticBufferWithUpload(
        m_device,
        m_allocator,
        cmd,
        vertexBytes,
        VK_BUFFER_USAGE_2_VERTEX_BUFFER_BIT_KHR,
        m_staticUploadPolicy,
        &m_stagingBuffers);
    record.vertexAllocation = vertexBuffer.allocation;
    record.setNativeVertexBuffer(vertexBuffer.buffer);

    const utils::Buffer indexBuffer = upload::createStaticBufferWithUpload(
        m_device,
        m_allocator,
        cmd,
        indexBytes,
        VK_BUFFER_USAGE_2_INDEX_BUFFER_BIT_KHR,
        m_staticUploadPolicy,
        &m_stagingBuffers);
    record.indexAllocation = indexBuffer.allocation;
    record.setNativeIndexBuffer(indexBuffer.buffer);

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
