#pragma once

#include "../common/Common.h"
#include "../common/Handles.h"
#include "../common/HandlePool.h"
#include "../rhi/RHICommandBuffer.h"
#include "UploadUtils.h"

#include <cstdint>
#include <cassert>

namespace demo::rhi
{
	class Device;
}

namespace demo
{
	class BatchUploadContext;
	struct GltfMeshData; // Forward declaration
	struct SceneMeshData;

	struct MeshRecord
	{
		rhi::BufferHandle vertexBuffer{};
		rhi::BufferHandle indexBuffer{};
		uint32_t vertexCount = 0;
		uint32_t indexCount = 0;
		uint32_t firstIndex = 0;
		int32_t vertexOffset = 0;
		uint32_t vertexStride = 48; // Position(12) + Normal(12) + TexCoord(8) + Tangent(16)
		uint64_t vertexBufferOffset = 0;
		uint64_t indexBufferOffset = 0;
		glm::mat4 transform = glm::mat4(1.0f);
		int32_t materialIndex = -1; // -1 = default material
		// Pre-computed alpha mode from material - avoids per-frame per-pass lookup
		// 0=OPAQUE, 1=MASK, 2=BLEND (matches shaderio::LAlphaOpaque/Mask/Blend)
		int32_t alphaMode = 0;
		float alphaCutoff = 0.5f;

		// Pre-computed material texture indices (bindless)
		int32_t baseColorTextureIndex = -1;
		int32_t normalTextureIndex = -1;
		int32_t metallicRoughnessTextureIndex = -1;
		int32_t occlusionTextureIndex = -1;
		int32_t emissiveTextureIndex = -1;

		// Pre-computed material factors
		glm::vec4 baseColorFactor = glm::vec4(1.0f);
		float metallicFactor = 1.0f;
		float roughnessFactor = 1.0f;
		float normalScale = 1.0f;
		float occlusionStrength = 1.0f;
		glm::vec4 emissiveFactor = glm::vec4(0.0f);
		int32_t materialWorkflow = 0;

		glm::vec3 localBoundsMin = glm::vec3(0.0f);
		glm::vec3 localBoundsMax = glm::vec3(0.0f);
		glm::vec3 worldBoundsMin = glm::vec3(0.0f);
		glm::vec3 worldBoundsMax = glm::vec3(0.0f);
		glm::vec3 worldBoundsCenter = glm::vec3(0.0f);
		float worldBoundsRadius = 0.0f;
	};

	class MeshPool
	{
	public:
		MeshPool() = default;
		~MeshPool() { assert(m_backendDeviceToken == 0 && "Missing deinit()"); }

		void init(uintptr_t backendDeviceToken, uintptr_t backendAllocatorToken, rhi::Device* rhiDevice,
		          upload::StaticBufferUploadPolicy staticUploadPolicy = {});
		void deinit();

		MeshHandle uploadMesh(const GltfMeshData& meshData, rhi::CommandBuffer& cmd,
		                      BatchUploadContext* batchUpload = nullptr);
		MeshHandle uploadMesh(const SceneMeshData& meshData, rhi::CommandBuffer& cmd,
		                      BatchUploadContext* batchUpload = nullptr);
		void destroyMesh(MeshHandle handle);
		void updateTransform(MeshHandle handle, const glm::mat4& transform);
		void setMeshAlphaMode(MeshHandle handle, int32_t alphaMode, float alphaCutoff);
		void setMeshMaterialData(MeshHandle handle,
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
		                         int32_t materialWorkflow);

		[[nodiscard]] const MeshRecord* tryGet(MeshHandle handle) const;
		void reserve(uint64_t additionalVertexBytes, uint64_t additionalIndexBytes, rhi::CommandBuffer& cmd);
		// Stable RHI handles for the shared arenas (rebinds across arena realloc via
		// VulkanResourceTable::updateBuffer). Consumed by RenderEncoder-based passes.
		[[nodiscard]] rhi::BufferHandle getSharedVertexBufferRHIHandle() const { return m_sharedVertexBufferRHI; }
		[[nodiscard]] rhi::BufferHandle getSharedIndexBufferRHIHandle() const { return m_sharedIndexBufferRHI; }
		[[nodiscard]] size_t getDeferredStagingBufferCount() const;
		[[nodiscard]] uint64_t getDeferredStagingBufferBytes() const;

		// Free staging buffers after GPU sync (call after command buffer completes)
		void deferStagingBuffer(rhi::BufferHandle buffer);
		void freeStagingBuffers();

		template <typename Fn>
		void forEachActive(Fn&& fn)
		{
			m_pool.forEachActive(std::forward<Fn>(fn));
		}

	private:
		struct SharedBufferArena
		{
			upload::NativeUploadBuffer buffer{};
			uint64_t capacity{0};
			uint64_t bytesUsed{0};
		};

		void ensureSharedCapacity(SharedBufferArena& arena,
		                          uint64_t requiredSize,
		                          rhi::BufferUsageFlags usage,
		                          rhi::CommandBuffer& cmd);
		void resetSharedBuffers();

		uintptr_t m_backendDeviceToken = 0;
		uintptr_t m_backendAllocatorToken = 0;
		rhi::Device* m_rhiDevice = nullptr;
		rhi::BufferHandle m_sharedVertexBufferRHI{};
		rhi::BufferHandle m_sharedIndexBufferRHI{};
		upload::StaticBufferUploadPolicy m_staticUploadPolicy{};
		HandlePool<MeshHandle, MeshRecord> m_pool;
		SharedBufferArena m_sharedVertexBuffer{};
		SharedBufferArena m_sharedIndexBuffer{};
		std::vector<upload::NativeUploadBuffer> m_stagingBuffers; // Deferred deletion after GPU sync
		std::vector<rhi::BufferHandle> m_rhiStagingBuffers;
	};
} // namespace demo
