#pragma once

#include "../RHICommandBuffer.h"
#include "../RHIDebugValidation.h"
#include "../RHIEncoder.h"
#include "../RHIQueue.h"

struct VkCommandBuffer_T;
struct VkPipelineLayout_T;
using VkCommandBuffer = VkCommandBuffer_T*;
using VkPipelineLayout = VkPipelineLayout_T*;

namespace demo::rhi::vulkan
{
	class VulkanCommandAllocator;
	class VulkanResourceTable;
	class VulkanCommandBuffer;

	inline constexpr uint32_t kMaxArgumentSlots = 8;
	inline constexpr uint32_t kMaxDynOffsetPerSlot = 4;

	class VulkanRenderEncoder final : public RenderEncoder
	{
	public:
		void prepare(VkCommandBuffer cmd, VulkanResourceTable* table, VulkanCommandBuffer* owner);
		void invalidate() noexcept;

		void setPipeline(PipelineHandle pipeline) override;
		void setArgumentTable(ShaderStage stages, uint32_t slot, ArgumentTableHandle table) override;
		void setDynamicBuffer(ShaderStage stages, uint32_t slot, BufferHandle buffer, uint64_t offset,
		                      uint64_t size) override;
		void setRootConstants(ShaderStage stages, uint32_t slot, std::span<const std::byte> data) override;
		void setRootPointer(ShaderStage stages, uint32_t slot, GpuPtr ptr) override;
		void setViewport(const Viewport& viewport) override;
		void setScissor(const Rect2D& scissor) override;
        void bindVertexBuffers(uint32_t firstBinding,
                               std::span<const VertexBufferBinding> bindings) override;
		void bindIndexBuffer(BufferHandle buffer, uint64_t offset, IndexFormat format) override;
		void readInputAttachment(uint32_t index) override;
		void draw(const DrawDesc& desc) override;
		void drawIndexed(const DrawIndexedDesc& desc) override;
		void drawIndexedIndirect(const DrawIndirectDesc& desc) override;
		void drawIndexedIndirectCount(const DrawIndirectCountDesc& desc) override;
		void drawIndirect(const DrawIndirectDesc& desc) override;
		void drawMeshTasks(uint32_t groupCountX, uint32_t groupCountY, uint32_t groupCountZ) override;
		void drawMeshTasksIndirect(const DrawIndirectDesc& desc) override;

	private:
		void requireActive() const;
		VkCommandBuffer m_cmd{nullptr};
		VulkanResourceTable* m_table{nullptr};
		VulkanCommandBuffer* m_owner{nullptr};
		PipelineHandle m_pipeline{};
		VkPipelineLayout m_layout{nullptr};
		uint32_t m_pendingDynOffsets[kMaxArgumentSlots][kMaxDynOffsetPerSlot]{};
		uint32_t m_pendingDynCount[kMaxArgumentSlots]{};
		bool m_valid{false};
	};

	class VulkanComputeEncoder final : public ComputeEncoder
	{
	public:
		void prepare(VkCommandBuffer cmd, VulkanResourceTable* table, VulkanCommandBuffer* owner);
		void invalidate() noexcept;

		void setPipeline(PipelineHandle pipeline) override;
		void setArgumentTable(uint32_t slot, ArgumentTableHandle table) override;
		void setRootConstants(uint32_t slot, std::span<const std::byte> data) override;
		void setRootPointer(uint32_t slot, GpuPtr ptr) override;
		void dispatch(const DispatchDesc& desc) override;
		void dispatchIndirect(const DispatchIndirectDesc& desc) override;
		void copyBuffer(BufferHandle src, uint64_t srcOffset, BufferHandle dst, uint64_t dstOffset,
		                uint64_t size) override;
		void copyBufferToTexture(const BufferTextureCopyDesc& desc) override;
		void copyTextureToBuffer(const BufferTextureCopyDesc& desc) override;
		void blitTexture(const TextureBlitDesc& desc) override;
		void fillBuffer(BufferHandle buffer, uint64_t offset, uint64_t size, uint32_t data) override;

	private:
		void requireActive() const;
		void requireCompute(const char* command) const;
		void requireTransfer(const char* command) const;
		VkCommandBuffer m_cmd{nullptr};
		VulkanResourceTable* m_table{nullptr};
		VulkanCommandBuffer* m_owner{nullptr};
		PipelineHandle m_pipeline{};
		VkPipelineLayout m_layout{nullptr};
		bool m_valid{false};
	};

	class VulkanCommandBuffer final : public CommandBuffer
	{
	public:
		VulkanCommandBuffer() = default;
		VulkanCommandBuffer(VulkanCommandAllocator& allocator, VkCommandBuffer cmd,
		                    VulkanResourceTable* table);
		~VulkanCommandBuffer() override;

		void begin(CommandAllocator& allocator) override;
		void end() override;
		[[nodiscard]] CommandBufferState state() const noexcept override;

		void setTarget(VkCommandBuffer cmd, VulkanResourceTable* table);

		RenderEncoder* beginRenderPass(const RenderPassDesc& desc) override;
		ComputeEncoder* beginComputePass() override;
		void endEncoding() override;
		RHIResult useResidencySet(ResidencySetHandle set) override;

		void barrier(StageFlags producer, StageFlags consumer, HazardFlags hazards) override;
		void resourceBarrier(std::span<const TextureBarrier> textures,
		                     std::span<const BufferBarrier> buffers,
		                     std::span<const AliasingBarrier> aliasing = {}) override;
		void clearColorTexture(TextureHandle texture,
		                       const TextureSubresourceRange& range,
		                       const ClearColorValue& clearColor) override;

		void beginEvent(const char* name) override;
		void endEvent() override;

		void resetQueryPool(QueryPoolHandle pool, uint32_t firstQuery, uint32_t queryCount) override;
		void writeTimestamp(QueryPoolHandle pool, uint32_t queryIndex, bool afterAllCommands) override;

		[[nodiscard]] VkCommandBuffer nativeHandle() const { return m_cmd; }
		[[nodiscard]] QueueClass queueClass() const noexcept;
		void validateForSubmit() const;
		void markSubmitted(SubmissionToken token);
		void requireQueueOperation(QueueOperation operation, const char* command) const;
		void markReusable();
		void trackArgumentTable(ArgumentTableHandle table);
		void trackPipeline(PipelineHandle pipeline);
		void trackBuffer(BufferHandle buffer);
		void trackTexture(TextureHandle texture);
		void trackTextureView(TextureViewHandle view);
		void trackQueryPool(QueryPoolHandle pool);
		void trackResidencySet(ResidencySetHandle set);

	private:
		enum class EncoderKind : uint8_t
		{
			none,
			render,
			compute,
		};

		void requireRecording(const char* operation) const;

		VkCommandBuffer m_cmd{nullptr};
		VulkanResourceTable* m_table{nullptr};
		VulkanCommandAllocator* m_allocator{nullptr};
		VulkanRenderEncoder m_renderEncoder;
		VulkanComputeEncoder m_computeEncoder;
		EncoderKind m_active{EncoderKind::none};
		CommandBufferState m_state{CommandBufferState::idle};
		SubmissionToken m_submission{};
		ArgumentTableHandle m_argumentTables[64]{};
		uint32_t m_argumentTableCount{0};
		PipelineHandle m_pipelines[64]{};
		uint32_t m_pipelineCount{0};
		BufferHandle m_buffers[512]{};
		uint32_t m_bufferCount{0};
		TextureHandle m_textures[256]{};
		uint32_t m_textureCount{0};
		TextureViewHandle m_textureViews[128]{};
		uint32_t m_textureViewCount{0};
		QueryPoolHandle m_queryPools[16]{};
		uint32_t m_queryPoolCount{0};
		ResidencySetHandle m_residencySets[16]{};
		uint32_t m_residencySetCount{0};
		DebugResourceStateTracker m_resourceStates;
	};
} // namespace demo::rhi::vulkan