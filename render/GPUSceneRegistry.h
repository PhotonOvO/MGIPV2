#pragma once

#include "../common/Handles.h"
#include "../rhi/RHIHandles.h"
#include "../rhi/RHITypes.h"
#include "ShaderInterop.h"

#include <cstdint>
#include <type_traits>
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

	struct GPUSceneObjectHandle
	{
		uint32_t index;
		uint32_t generation;

		[[nodiscard]] constexpr bool isNull() const noexcept
		{
			return index == 0u && generation == 0u;
		}
		[[nodiscard]] constexpr explicit operator bool() const noexcept { return !isNull(); }
		friend constexpr bool operator==(GPUSceneObjectHandle lhs, GPUSceneObjectHandle rhs) noexcept = default;
	};
	static_assert(std::is_trivial_v<GPUSceneObjectHandle>);
	static_assert(std::is_standard_layout_v<GPUSceneObjectHandle>);
	inline constexpr GPUSceneObjectHandle kNullGPUSceneObjectHandle{};

	struct GPUSceneRemoveResult
	{
		bool removed{false};
		GPUSceneObjectHandle removedObject{};
		uint32_t removedDenseIndex{UINT32_MAX};
		GPUSceneObjectHandle movedObject{};
		uint32_t movedFromDenseIndex{UINT32_MAX};
		uint32_t movedToDenseIndex{UINT32_MAX};

		[[nodiscard]] constexpr bool hasDenseRemap() const noexcept
		{
			return removed && !movedObject.isNull() && movedFromDenseIndex != movedToDenseIndex;
		}
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

		[[nodiscard]] GPUSceneObjectHandle registerObject(const GPUSceneRegistrationDesc& desc);
		[[nodiscard]] GPUSceneRemoveResult removeObject(GPUSceneObjectHandle object);
		bool updateTransform(GPUSceneObjectHandle object,
		                     const glm::mat4& newTransform,
		                     const glm::vec4& newBoundsSphere);
		[[nodiscard]] bool tryGetDenseIndex(GPUSceneObjectHandle object, uint32_t& outDenseIndex) const;

		void syncToGpu(rhi::CommandBuffer& cmd);

		[[nodiscard]] rhi::GpuPtr getBufferAddress() const { return m_objectBufferAddress; }
		[[nodiscard]] rhi::BufferHandle getBufferHandle() const { return m_objectBufferRHI; }

		[[nodiscard]] rhi::GpuPtr getCullBufferAddress() const { return m_cullObjectBufferAddress; }

		[[nodiscard]] rhi::BufferHandle getCullBufferHandle() const { return m_cullObjectBufferRHI; }
		[[nodiscard]] uint32_t getObjectCount() const { return static_cast<uint32_t>(m_gpuObjects.size()); }
		[[nodiscard]] bool isDirty() const { return m_dirty; }
		[[nodiscard]] const std::vector<shaderio::GPUCullObject>& getOverlayObjects() const { return m_cullObjects; }

	private:
		struct ObjectSlot
		{
			bool occupied{false};
			uint32_t generation{0};
			uint32_t denseIndex{UINT32_MAX};
			GPUSceneRegistrationDesc desc{};
			shaderio::GPUSceneObject gpuObject{};
			shaderio::GPUCullObject cullObject{};
		};

		[[nodiscard]] bool isLiveHandle(GPUSceneObjectHandle object) const;
		[[nodiscard]] static uint32_t nextGeneration(uint32_t generation);
		void ensureCapacity(uint32_t requiredCount);
		void ensureStagingCapacity(uint64_t requiredBytes);
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
		uint64_t m_updateBufferCapacityBytes{0};
		uint32_t m_capacity{0};
		bool m_dirty{false};
		bool m_requiresFullUpload{true};
		bool m_gpuBuffersInitialized{false};
		std::vector<ObjectSlot> m_slots{1};
		std::vector<uint32_t> m_freeList;
		std::vector<uint32_t> m_denseSlotIds;
		std::vector<uint32_t> m_dirtyDenseIndices;
		std::vector<shaderio::GPUSceneObject> m_gpuObjects;
		std::vector<shaderio::GPUCullObject> m_cullObjects;
	};
} // namespace demo
