#pragma once

#include "../common/Common.h"
#include "../common/Handles.h"
#include "../common/HandlePool.h"

#include <cstdint>
#include <cassert>

namespace demo {

struct GltfMeshData;  // Forward declaration

struct MeshRecord {
    VkBuffer vertexBuffer = VK_NULL_HANDLE;
    VkBuffer indexBuffer = VK_NULL_HANDLE;
    VmaAllocation vertexAllocation = nullptr;
    VmaAllocation indexAllocation = nullptr;
    uint32_t vertexCount = 0;
    uint32_t indexCount = 0;
    uint32_t vertexStride = 32;  // Position(12) + Normal(12) + TexCoord(8)
    glm::mat4 transform = glm::mat4(1.0f);
};

class MeshPool {
public:
    MeshPool() = default;
    ~MeshPool() { assert(m_device == VK_NULL_HANDLE && "Missing deinit()"); }

    void init(VkDevice device, VmaAllocator allocator);
    void deinit();

    MeshHandle uploadMesh(const GltfMeshData& meshData, VkCommandBuffer cmd);
    void destroyMesh(MeshHandle handle);

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
    HandlePool<MeshHandle, MeshRecord> m_pool;
    std::vector<utils::Buffer> m_stagingBuffers;  // Deferred deletion after GPU sync
};

}  // namespace demo