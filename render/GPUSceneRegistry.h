#pragma once

#include "../common/Handles.h"
#include "../rhi/RHIHandles.h"
#include "../rhi/RHITypes.h"
#include "ShaderInterop.h"

#include <cstdint>
#include <vector>

#include <glm/glm.hpp>

namespace demo
{
	namespace rhi
	{
		class CommandBuffer;
		class Device;
	}

	struct GPUSceneRegistrationDesc
	{
		MeshHandle meshHandle{};
		uint32_t meshIndex{0};
		uint32_t materialIndex{UINT32_MAX};
		glm::mat4 transform{1.0f};
		glm::vec4 boundsSphere{0.0f};
		uint32_t flags{0};
		uint32_t indexCount{0};
		uint32_t firstIndex{0};
		int32_t vertexOffset{0};
	};

	class GPUSceneRegistry
	{
	public:
		struct DirtyRange
		{
			uint32_t startIndex{0};
			uint32_t count{0};
		};

		void init(rhi::Device* rhiDevice);
		void deinit();
		void clear();

		[[nodiscard]] uint32_t registerObject(const GPUSceneRegistrationDesc& desc);
		void removeObject(uint32_t objectID);
		void updateTransform(uint32_t objectID, const glm::mat4& newTransform, const glm::vec4& newBoundsSphere);

		void syncToGpu(rhi::CommandBuffer& cmd);

		[[nodiscard]] uint64_t getBufferAddress() const { return m_objectBufferAddress.value; }
		[[nodiscard]] rhi::BufferHandle getBufferHandle() const { return m_objectBufferRHI; }

		[[nodiscard]] uint64_t getCullBufferAddress() const { return m_cullObjectBufferAddress.value; }

		[[nodiscard]] rhi::BufferHandle getCullBufferHandle() const { return m_cullObjectBufferRHI; }
		[[nodiscard]] uint32_t getObjectCount() const { return static_cast<uint32_t>(m_gpuObjects.size()); }
		[[nodiscard]] bool isDirty() const { return m_dirty; }
		[[nodiscard]] const std::vector<shaderio::GPUCullObject>& getOverlayObjects() const { return m_cullObjects; }

	private:
		struct ObjectSlot
		{
			bool occupied{false};
			uint32_t denseIndex{0};
			GPUSceneRegistrationDesc desc{};
			shaderio::GPUSceneObject gpuObject{};
			shaderio::GPUCullObject cullObject{};
		};

		void ensureCapacity(uint32_t requiredCount);
		void markDirtyDenseIndex(uint32_t denseIndex);
		[[nodiscard]] std::vector<DirtyRange> buildDirtyRanges() const;
		void rebuildPackedObject(uint32_t objectID);
		static shaderio::GPUSceneObject packSceneObject(const GPUSceneRegistrationDesc& desc);
		static shaderio::GPUCullObject packCullObject(const GPUSceneRegistrationDesc& desc);

		rhi::Device* m_rhiDevice{nullptr};
		rhi::BufferHandle m_objectBufferRHI{};
		rhi::BufferHandle m_cullObjectBufferRHI{};
		rhi::BufferHandle m_updateBufferRHI{};
		rhi::GpuPtr m_objectBufferAddress{};
		rhi::GpuPtr m_cullObjectBufferAddress{};
		void* m_updateBufferMapped{nullptr};
		uint32_t m_capacity{0};
		bool m_dirty{false};
		bool m_requiresFullUpload{true};
		std::vector<ObjectSlot> m_slots{1};
		std::vector<uint32_t> m_freeList;
		std::vector<uint32_t> m_denseSlotIds;
		std::vector<uint32_t> m_dirtyDenseIndices;
		std::vector<shaderio::GPUSceneObject> m_gpuObjects;
		std::vector<shaderio::GPUCullObject> m_cullObjects;
	};
} // namespace demo
