#pragma once

#include "../common/Common.h"
#include "../common/Handles.h"
#include "../common/HandlePool.h"
#include "UploadUtils.h"

#include <cstdint>
#include <cassert>

namespace demo {

struct GltfMeshData;  // Forward declaration

struct MeshRecord {
    uint64_t vertexBufferHandle = 0;  // Opaque buffer handle (encoded VkBuffer)
    uint64_t indexBufferHandle = 0;   // Opaque buffer handle (encoded VkBuffer)
    VmaAllocation vertexAllocation = nullptr;
    VmaAllocation indexAllocation = nullptr;
    uint32_t vertexCount = 0;
    uint32_t indexCount = 0;
    uint32_t vertexStride = 48;  // Position(12) + Normal(12) + TexCoord(8) + Tangent(16)
    glm::mat4 transform = glm::mat4(1.0f);
    int32_t materialIndex = -1;  // -1 = default material
    // Pre-computed alpha mode from material - avoids per-frame per-pass lookup
    // 0=OPAQUE, 1=MASK, 2=BLEND (matches shaderio::LAlphaOpaque/Mask/Blend)
    int32_t alphaMode = 0;
    float alphaCutoff = 0.5f;

    // Pre-computed material texture indices (bindless)
    int32_t baseColorTextureIndex = -1;
    int32_t normalTextureIndex = -1;
    int32_t metallicRoughnessTextureIndex = -1;
    int32_t occlusionTextureIndex = -1;

    // Pre-computed material factors
    glm::vec4 baseColorFactor = glm::vec4(1.0f);
    float metallicFactor = 1.0f;
    float roughnessFactor = 1.0f;
    float normalScale = 1.0f;

    glm::vec3 localBoundsMin = glm::vec3(0.0f);
    glm::vec3 localBoundsMax = glm::vec3(0.0f);
    glm::vec3 worldBoundsMin = glm::vec3(0.0f);
    glm::vec3 worldBoundsMax = glm::vec3(0.0f);

    // Helper to get native VkBuffer from opaque handle
    VkBuffer getNativeVertexBuffer() const {
        return reinterpret_cast<VkBuffer>(static_cast<uintptr_t>(vertexBufferHandle));
    }
    VkBuffer getNativeIndexBuffer() const {
        return reinterpret_cast<VkBuffer>(static_cast<uintptr_t>(indexBufferHandle));
    }

    // Helper to set from native VkBuffer
    void setNativeVertexBuffer(VkBuffer buffer) {
        vertexBufferHandle = static_cast<uint64_t>(reinterpret_cast<uintptr_t>(buffer));
    }
    void setNativeIndexBuffer(VkBuffer buffer) {
        indexBufferHandle = static_cast<uint64_t>(reinterpret_cast<uintptr_t>(buffer));
    }
};

class MeshPool {
public:
    MeshPool() = default;
    ~MeshPool() { assert(m_device == VK_NULL_HANDLE && "Missing deinit()"); }

    void init(VkDevice device, VmaAllocator allocator, upload::StaticBufferUploadPolicy staticUploadPolicy = {});
    void deinit();

    MeshHandle uploadMesh(const GltfMeshData& meshData, VkCommandBuffer cmd);
    void destroyMesh(MeshHandle handle);
    void updateTransform(MeshHandle handle, const glm::mat4& transform);
    void setMeshAlphaMode(MeshHandle handle, int32_t alphaMode, float alphaCutoff);
    void setMeshMaterialData(MeshHandle handle,
                            const glm::vec4& baseColorFactor,
                            int32_t baseColorTextureIndex,
                            int32_t normalTextureIndex,
                            int32_t metallicRoughnessTextureIndex,
                            int32_t occlusionTextureIndex,
                            float metallicFactor,
                            float roughnessFactor,
                            float normalScale);

    [[nodiscard]] const MeshRecord* tryGet(MeshHandle handle) const;

    // Free staging buffers after GPU sync (call after command buffer completes)
    void freeStagingBuffers();

    template<typename Fn>
    void forEachActive(Fn&& fn) {
        m_pool.forEachActive(std::forward<Fn>(fn));
    }

private:
    VkDevice m_device = VK_NULL_HANDLE;
    VmaAllocator m_allocator = nullptr;
    upload::StaticBufferUploadPolicy m_staticUploadPolicy{};
    HandlePool<MeshHandle, MeshRecord> m_pool;
    std::vector<utils::Buffer> m_stagingBuffers;  // Deferred deletion after GPU sync
};

}  // namespace demo
