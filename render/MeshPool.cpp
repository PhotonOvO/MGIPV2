#include "MeshPool.h"
#include "BatchUploadContext.h"
#include "../loader/GltfLoader.h"
#include "../scene/SceneAsset.h"
#include "../rhi/RHIDevice.h"
#include "../rhi/RHIEncoder.h"
#include "../rhi/vulkan/internal/VulkanCommon.h"

#include <array>
#include <cstring>
#include <limits>
#include <span>

namespace demo {

namespace {

constexpr uint32_t kInterleavedVertexStride = 48;
constexpr uint64_t kInitialSharedVertexCapacity = 4ull * 1024ull * 1024ull;
constexpr uint64_t kInitialSharedIndexCapacity = 2ull * 1024ull * 1024ull;

upload::NativeUploadContext makeNativeUploadContext(uintptr_t device, uintptr_t allocator)
{
    return upload::NativeUploadContext{
        .device    = device,
        .allocator = allocator,
    };
}

VkBuffer toVkBuffer(const upload::NativeUploadBuffer& buffer)
{
    return reinterpret_cast<VkBuffer>(buffer.buffer);
}

VmaAllocation toVmaAllocation(const upload::NativeUploadBuffer& buffer)
{
    return reinterpret_cast<VmaAllocation>(buffer.allocation);
}

VkBufferUsageFlags2KHR toVkBufferUsage(rhi::BufferUsageFlags usage)
{
    VkBufferUsageFlags2KHR result = 0;
    if(static_cast<uint32_t>(usage & rhi::BufferUsageFlags::vertex) != 0) result |= VK_BUFFER_USAGE_2_VERTEX_BUFFER_BIT_KHR;
    if(static_cast<uint32_t>(usage & rhi::BufferUsageFlags::index) != 0) result |= VK_BUFFER_USAGE_2_INDEX_BUFFER_BIT_KHR;
    if(static_cast<uint32_t>(usage & rhi::BufferUsageFlags::transferSrc) != 0) result |= VK_BUFFER_USAGE_2_TRANSFER_SRC_BIT_KHR;
    if(static_cast<uint32_t>(usage & rhi::BufferUsageFlags::transferDst) != 0) result |= VK_BUFFER_USAGE_2_TRANSFER_DST_BIT_KHR;
    if(static_cast<uint32_t>(usage & rhi::BufferUsageFlags::storage) != 0) result |= VK_BUFFER_USAGE_2_STORAGE_BUFFER_BIT_KHR;
    if(static_cast<uint32_t>(usage & rhi::BufferUsageFlags::uniform) != 0) result |= VK_BUFFER_USAGE_2_UNIFORM_BUFFER_BIT_KHR;
    if(static_cast<uint32_t>(usage & rhi::BufferUsageFlags::indirect) != 0) result |= VK_BUFFER_USAGE_2_INDIRECT_BUFFER_BIT_KHR;
    if(static_cast<uint32_t>(usage & rhi::BufferUsageFlags::shaderDeviceAddress) != 0) result |= VK_BUFFER_USAGE_2_SHADER_DEVICE_ADDRESS_BIT_KHR;
    return result;
}

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

void updateLocalBoundsFromInterleavedVertices(MeshRecord& record, std::span<const uint8_t> vertexData)
{
    ASSERT(vertexData.size() == static_cast<size_t>(record.vertexCount) * kInterleavedVertexStride,
           "Interleaved vertex buffer size mismatch");

    for (uint32_t i = 0; i < record.vertexCount; ++i) {
        const float* src = reinterpret_cast<const float*>(vertexData.data() + static_cast<size_t>(i) * kInterleavedVertexStride);
        const glm::vec3 position(src[0], src[1], src[2]);
        record.localBoundsMin = glm::min(record.localBoundsMin, position);
        record.localBoundsMax = glm::max(record.localBoundsMax, position);
    }
}

}  // namespace

void MeshPool::init(uintptr_t backendDeviceToken, uintptr_t backendAllocatorToken, rhi::Device* rhiDevice,
                    upload::StaticBufferUploadPolicy staticUploadPolicy) {
    m_backendDeviceToken = backendDeviceToken;
    m_backendAllocatorToken = backendAllocatorToken;
    m_rhiDevice = rhiDevice;
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

    m_backendDeviceToken = 0;
    m_backendAllocatorToken = 0;
}

void MeshPool::ensureSharedCapacity(SharedBufferArena& arena,
                                    uint64_t requiredSize,
                                    rhi::BufferUsageFlags usage,
                                    rhi::CommandBuffer& cmd)
{
    if(requiredSize <= arena.capacity)
    {
        return;
    }

    uint64_t newCapacity = arena.capacity == 0 ? requiredSize : arena.capacity;
    while(newCapacity < requiredSize)
    {
        newCapacity = std::max(newCapacity * 2, requiredSize);
    }

    if(static_cast<uint32_t>(usage & rhi::BufferUsageFlags::vertex) != 0)
    {
        newCapacity = std::max(newCapacity, kInitialSharedVertexCapacity);
    }
    if(static_cast<uint32_t>(usage & rhi::BufferUsageFlags::index) != 0)
    {
        newCapacity = std::max(newCapacity, kInitialSharedIndexCapacity);
    }

    upload::NativeUploadBuffer newBuffer =
        upload::createStaticBuffer(makeNativeUploadContext(m_backendDeviceToken, m_backendAllocatorToken), newCapacity, toVkBufferUsage(usage));
    const bool replacingVertexArena = static_cast<uint32_t>(usage & rhi::BufferUsageFlags::vertex) != 0;
    const bool replacingIndexArena = static_cast<uint32_t>(usage & rhi::BufferUsageFlags::index) != 0;
    if(!arena.buffer.isNull() && arena.bytesUsed > 0)
    {
        ASSERT(m_rhiDevice != nullptr, "MeshPool RHI device is required for arena copy");
        rhi::BufferHandle srcHandle = replacingVertexArena ? m_sharedVertexBufferRHI : m_sharedIndexBufferRHI;
        rhi::BufferHandle dstHandle =
            m_rhiDevice->registerExternalBuffer(newBuffer.buffer);
        rhi::ComputeEncoder* copy = cmd.beginComputePass();
        copy->copyBuffer(srcHandle, 0, dstHandle, 0, arena.bytesUsed);
        cmd.endEncoding();
        m_rhiDevice->destroyBuffer(dstHandle);
        m_stagingBuffers.push_back(arena.buffer);
    }

    arena.buffer = newBuffer;
    arena.capacity = newCapacity;

    if(replacingVertexArena || replacingIndexArena)
    {
        m_pool.forEachActive([&](MeshHandle, MeshRecord& record) {
            if(replacingVertexArena)
            {
                record.vertexBuffer = m_sharedVertexBufferRHI;
            }
            if(replacingIndexArena)
            {
                record.indexBuffer = m_sharedIndexBufferRHI;
            }
        });

        // Keep the stable RHI handle bound to the rebuilt arena (Option B: handle is
        // allocated once, its native buffer is rebound on each realloc). owned=false:
        // MeshPool owns the VMA lifetime, the registry only mirrors the native buffer.
        if(m_rhiDevice != nullptr)
        {
            const uint64_t native = newBuffer.buffer;
            const auto     bind   = [&](rhi::BufferHandle& rhiHandle) {
                if(rhiHandle.isNull())
                {
                    rhiHandle = m_rhiDevice->registerExternalBuffer(native);
                }
                else
                {
                    m_rhiDevice->updateBufferBinding(rhiHandle, native);
                }
            };
            if(replacingVertexArena) bind(m_sharedVertexBufferRHI);
            if(replacingIndexArena)  bind(m_sharedIndexBufferRHI);
        }
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

MeshHandle MeshPool::uploadMesh(const GltfMeshData& meshData, rhi::CommandBuffer& cmd, BatchUploadContext* batchUpload) {
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
    record.vertexBuffer = m_sharedVertexBufferRHI;
    record.indexBuffer = m_sharedIndexBufferRHI;

    if(batchUpload != nullptr)
    {
        const BatchUploadContext::Slice vertexSlice = batchUpload->allocate(vertexDataSize, alignof(float));
        buildInterleavedVertexData(meshData,
                                   record,
                                   std::span<uint8_t>(static_cast<uint8_t*>(vertexSlice.cpuPtr), vertexDataSize));
        batchUpload->recordBufferUpload(vertexSlice,
                                        m_sharedVertexBufferRHI,
                                        record.vertexBufferOffset,
                                        vertexDataSize);

        const BatchUploadContext::Slice indexSlice = batchUpload->allocate(indexDataSize, alignof(uint32_t));
        std::memcpy(indexSlice.cpuPtr, meshData.indices.data(), indexDataSize);
        batchUpload->recordBufferUpload(indexSlice,
                                        m_sharedIndexBufferRHI,
                                        record.indexBufferOffset,
                                        indexDataSize);
    }
    else
    {
        std::vector<uint8_t> vertexData(vertexDataSize);
        buildInterleavedVertexData(meshData, record, vertexData);

        const std::span<const std::byte> vertexBytes(reinterpret_cast<const std::byte*>(vertexData.data()), vertexData.size());
        const std::span<const std::byte> indexBytes(reinterpret_cast<const std::byte*>(meshData.indices.data()),
                                                    indexDataSize);

        ASSERT(m_rhiDevice != nullptr, "MeshPool RHI device is required for non-batched upload");
        rhi::BufferHandle vertexStagingBuffer = upload::createUploadStagingBuffer(*m_rhiDevice, vertexBytes);
        rhi::BufferHandle indexStagingBuffer = upload::createUploadStagingBuffer(*m_rhiDevice, indexBytes);

        rhi::ComputeEncoder* copy = cmd.beginComputePass();
        copy->copyBuffer(vertexStagingBuffer, 0, m_sharedVertexBufferRHI, record.vertexBufferOffset, vertexDataSize);
        copy->copyBuffer(indexStagingBuffer, 0, m_sharedIndexBufferRHI, record.indexBufferOffset, indexDataSize);
        cmd.endEncoding();

        m_rhiStagingBuffers.push_back(vertexStagingBuffer);
        m_rhiStagingBuffers.push_back(indexStagingBuffer);
    }

    m_sharedVertexBuffer.bytesUsed += vertexDataSize;
    m_sharedIndexBuffer.bytesUsed += indexDataSize;

    updateWorldBounds(record);

    return m_pool.emplace(std::move(record));
}

MeshHandle MeshPool::uploadMesh(const SceneMeshData& meshData, rhi::CommandBuffer& cmd, BatchUploadContext* batchUpload)
{
    if(meshData.interleavedVertexData.empty() || meshData.indices.empty() || meshData.vertexCount == 0) {
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
    record.vertexBuffer = m_sharedVertexBufferRHI;
    record.indexBuffer = m_sharedIndexBufferRHI;
    updateLocalBoundsFromInterleavedVertices(record, meshData.interleavedVertexData);

    if(batchUpload != nullptr)
    {
        const BatchUploadContext::Slice vertexSlice = batchUpload->allocate(vertexDataSize, alignof(float));
        std::memcpy(vertexSlice.cpuPtr, meshData.interleavedVertexData.data(), vertexDataSize);
        batchUpload->recordBufferUpload(vertexSlice,
                                        m_sharedVertexBufferRHI,
                                        record.vertexBufferOffset,
                                        vertexDataSize);

        const BatchUploadContext::Slice indexSlice = batchUpload->allocate(indexDataSize, alignof(uint32_t));
        std::memcpy(indexSlice.cpuPtr, meshData.indices.data(), indexDataSize);
        batchUpload->recordBufferUpload(indexSlice,
                                        m_sharedIndexBufferRHI,
                                        record.indexBufferOffset,
                                        indexDataSize);
    }
    else
    {
        const std::span<const std::byte> vertexBytes(reinterpret_cast<const std::byte*>(meshData.interleavedVertexData.data()), vertexDataSize);
        const std::span<const std::byte> indexBytes(reinterpret_cast<const std::byte*>(meshData.indices.data()), indexDataSize);

        ASSERT(m_rhiDevice != nullptr, "MeshPool RHI device is required for non-batched upload");
        rhi::BufferHandle vertexStagingBuffer = upload::createUploadStagingBuffer(*m_rhiDevice, vertexBytes);
        rhi::BufferHandle indexStagingBuffer = upload::createUploadStagingBuffer(*m_rhiDevice, indexBytes);

        rhi::ComputeEncoder* copy = cmd.beginComputePass();
        copy->copyBuffer(vertexStagingBuffer, 0, m_sharedVertexBufferRHI, record.vertexBufferOffset, vertexDataSize);
        copy->copyBuffer(indexStagingBuffer, 0, m_sharedIndexBufferRHI, record.indexBufferOffset, indexDataSize);
        cmd.endEncoding();

        m_rhiStagingBuffers.push_back(vertexStagingBuffer);
        m_rhiStagingBuffers.push_back(indexStagingBuffer);
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
                                   int32_t emissiveTextureIndex,
                                   float metallicFactor,
                                   float roughnessFactor,
                                   float normalScale,
                                   float occlusionStrength,
                                   const glm::vec4& emissiveFactor,
                                   int32_t materialWorkflow)
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
    record->emissiveTextureIndex = emissiveTextureIndex;
    record->metallicFactor = metallicFactor;
    record->roughnessFactor = roughnessFactor;
    record->normalScale = normalScale;
    record->occlusionStrength = occlusionStrength;
    record->emissiveFactor = emissiveFactor;
    record->materialWorkflow = materialWorkflow;
}

const MeshRecord* MeshPool::tryGet(MeshHandle handle) const {
    return m_pool.tryGet(handle);
}

size_t MeshPool::getDeferredStagingBufferCount() const
{
    return m_stagingBuffers.size();
}

uint64_t MeshPool::getDeferredStagingBufferBytes() const
{
    uint64_t totalBytes = 0;
    const VmaAllocator allocator = reinterpret_cast<VmaAllocator>(m_backendAllocatorToken);
    for(const upload::NativeUploadBuffer& buffer : m_stagingBuffers)
    {
        if(buffer.allocation == 0)
        {
            continue;
        }

        VmaAllocationInfo allocationInfo{};
        vmaGetAllocationInfo(allocator, toVmaAllocation(buffer), &allocationInfo);
        totalBytes += allocationInfo.size;
    }

    return totalBytes;
}

void MeshPool::resetSharedBuffers()
{
    const VmaAllocator allocator = reinterpret_cast<VmaAllocator>(m_backendAllocatorToken);
    if(!m_sharedVertexBuffer.buffer.isNull())
    {
        vmaDestroyBuffer(allocator, toVkBuffer(m_sharedVertexBuffer.buffer), toVmaAllocation(m_sharedVertexBuffer.buffer));
    }
    if(!m_sharedIndexBuffer.buffer.isNull())
    {
        vmaDestroyBuffer(allocator, toVkBuffer(m_sharedIndexBuffer.buffer), toVmaAllocation(m_sharedIndexBuffer.buffer));
    }
    m_sharedVertexBuffer = {};
    m_sharedIndexBuffer = {};

    if(m_rhiDevice != nullptr)
    {
        if(!m_sharedVertexBufferRHI.isNull()) m_rhiDevice->destroyBuffer(m_sharedVertexBufferRHI);
        if(!m_sharedIndexBufferRHI.isNull())  m_rhiDevice->destroyBuffer(m_sharedIndexBufferRHI);
    }
    m_sharedVertexBufferRHI = {};
    m_sharedIndexBufferRHI = {};
}

void MeshPool::deferStagingBuffer(rhi::BufferHandle buffer)
{
    if(!buffer.isNull())
    {
        m_rhiStagingBuffers.push_back(buffer);
    }
}

void MeshPool::freeStagingBuffers() {
    for (auto& buffer : m_stagingBuffers) {
        if (!buffer.isNull()) {
            vmaDestroyBuffer(reinterpret_cast<VmaAllocator>(m_backendAllocatorToken), toVkBuffer(buffer), toVmaAllocation(buffer));
        }
    }
    m_stagingBuffers.clear();
    if(m_rhiDevice != nullptr)
    {
        for(rhi::BufferHandle buffer : m_rhiStagingBuffers)
        {
            if(!buffer.isNull())
            {
                m_rhiDevice->destroyBuffer(buffer);
            }
        }
    }
    m_rhiStagingBuffers.clear();
}

}  // namespace demo
