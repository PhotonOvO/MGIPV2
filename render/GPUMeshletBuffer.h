#pragma once

#include "../common/Handles.h"
#include "../rhi/RHIHandles.h"
#include "../rhi/RHITypes.h"
#include "ShaderInterop.h"

#include <cstdint>
#include <vector>

namespace demo::rhi
{
	class Device;
}

namespace demo
{
	class GPUMeshletBuffer
	{
	public:
		void init(rhi::Device* rhiDevice);
		void deinit();
		void clear();

		void uploadMeshlets(const std::vector<shaderio::Meshlet>& meshlets,
		                    const std::vector<uint32_t>& meshletIndices,
		                    const std::vector<shaderio::GPUCullObject>& meshletCullObjects);

		[[nodiscard]] uint64_t getMeshletDataAddress() const { return m_meshletDataAddress.value; }

		[[nodiscard]] rhi::BufferHandle getMeshletDataBuffer() const { return m_meshletDataBuffer; }

		[[nodiscard]] uint64_t getMeshletCullObjectAddress() const { return m_meshletCullObjectAddress.value; }

		[[nodiscard]] rhi::BufferHandle getMeshletCullObjectBuffer() const { return m_meshletCullObjectBuffer; }

		// Owned RHI handle for the meshlet index buffer; recreated on realloc, so
		// consumers (RenderEncoder-based passes) must fetch it each frame.
		[[nodiscard]] rhi::BufferHandle getMeshletIndexBufferRHIHandle() const { return m_meshletIndexBuffer; }
		[[nodiscard]] uint32_t getMeshletCount() const { return m_meshletCount; }
		[[nodiscard]] uint32_t getMeshletIndexCount() const { return m_meshletIndexCount; }

	private:
		void ensureCapacities(uint32_t requiredMeshletCount, uint32_t requiredIndexCount);
		void releaseBuffers();

		rhi::Device* m_rhiDevice{nullptr};
		rhi::BufferHandle m_meshletDataBuffer{};
		rhi::BufferHandle m_meshletCullObjectBuffer{};
		rhi::BufferHandle m_meshletIndexBuffer{};
		void* m_meshletDataMapped{nullptr};
		void* m_meshletCullObjectMapped{nullptr};
		void* m_meshletIndexMapped{nullptr};
		rhi::GpuPtr m_meshletDataAddress{};
		rhi::GpuPtr m_meshletCullObjectAddress{};
		uint32_t m_meshletCount{0};
		uint32_t m_meshletIndexCount{0};
		uint32_t m_meshletCapacity{0};
		uint32_t m_meshletIndexCapacity{0};
	};
} // namespace demo
