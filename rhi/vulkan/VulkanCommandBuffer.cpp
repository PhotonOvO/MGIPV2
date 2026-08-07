#include "VulkanCommandBuffer.h"

#include "internal/VulkanCommon.h"
#include "VulkanBarrierConversions.h"
#include "VulkanCommandAllocator.h"
#include "VulkanResourceTable.h"
#include "VulkanShaderConversions.h"
#include "../RHIDebugCounters.h"

#include <array>
#include <stdexcept>
#include <string>
#include <vulkan/vulkan.h>

namespace demo::rhi::vulkan
{
	namespace
	{
		[[nodiscard]] uint32_t indirectStride(uint32_t requestedStride, uint32_t drawCount, uint32_t commandSize)
		{
			return drawCount > 1 && requestedStride < commandSize ? commandSize : requestedStride;
		}

		[[nodiscard]] const PipelineRecord::RootBindingLowering& requireRootBinding(
			const VulkanResourceTable& table, PipelineHandle pipeline, uint32_t slot,
			RootBindingKind expectedKind)
		{
			const PipelineRecord* record = table.tryGetPipeline(pipeline);
			if (record == nullptr)
				throw std::runtime_error("Vulkan root binding requires an active pipeline");
			for (const PipelineRecord::RootBindingLowering& binding : record->rootBindings)
			{
				if (binding.slot == slot && binding.kind == static_cast<uint32_t>(expectedKind))
					return binding;
			}
			throw std::runtime_error("Vulkan root binding slot/kind mismatch");
		}

		inline constexpr uint32_t kMaxColorAttachmentsPerPass = 8;
		inline constexpr uint32_t kMaxBarrierBatchSize = 16;

		template <typename Handle, size_t Capacity>
		void trackHandle(
			Handle (&handles)[Capacity], uint32_t& count, Handle handle, const char* capacityError)
		{
			for (uint32_t index = 0; index < count; ++index)
			{
				if (handles[index] == handle)
					return;
			}
			if (count == Capacity)
				throw std::runtime_error(capacityError);
			handles[count++] = handle;
		}
	} // namespace

	// ---------------------------------------------------------------------------
	// VulkanRenderEncoder
	// ---------------------------------------------------------------------------
	void VulkanRenderEncoder::prepare(VkCommandBuffer cmd, VulkanResourceTable* table, VulkanCommandBuffer* owner)
	{
		m_cmd = cmd;
		m_table = table;
		m_owner = owner;
		m_pipeline = {};
		m_layout = VK_NULL_HANDLE;
		m_valid = true;
		for (uint32_t i = 0; i < kMaxArgumentSlots; ++i) m_pendingDynCount[i] = 0;
	}
	void VulkanRenderEncoder::invalidate() noexcept
	{
		m_valid = false;
	}
	void VulkanRenderEncoder::requireActive() const
	{
		if (!m_valid)
			throw std::runtime_error("Vulkan render encoder is outside its encoding scope");
	}
	void VulkanRenderEncoder::setPipeline(PipelineHandle pipeline)
	{
		requireActive();
		m_layout = m_table->resolvePipelineLayout(pipeline);
		const VkPipeline native = m_table->resolvePipeline(pipeline, VK_PIPELINE_BIND_POINT_GRAPHICS);
		if (m_layout == VK_NULL_HANDLE || native == VK_NULL_HANDLE)
			throw std::runtime_error("VulkanRenderEncoder::setPipeline received a stale pipeline");
		m_pipeline = pipeline;
		if (m_owner != nullptr) m_owner->trackPipeline(pipeline);
		vkCmdBindPipeline(m_cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, native);
	}
	void VulkanRenderEncoder::setArgumentTable(ShaderStage /*stages*/, uint32_t slot, ArgumentTableHandle table)
	{
		requireActive();
		if (m_layout == VK_NULL_HANDLE)
			throw std::runtime_error("Vulkan argument-table binding requires a pipeline");
		const VkDescriptorSet set = m_table->resolveArgumentTable(table);
		if (set == VK_NULL_HANDLE)
			throw std::runtime_error("Vulkan argument-table binding received a stale table");
		if (m_owner != nullptr) m_owner->trackArgumentTable(table);
		const uint32_t dynCount = slot < kMaxArgumentSlots ? m_pendingDynCount[slot] : 0;
		const uint32_t* dynOffsets = dynCount > 0 ? m_pendingDynOffsets[slot] : nullptr;
		vkCmdBindDescriptorSets(m_cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, m_layout, slot, 1, &set, dynCount, dynOffsets);
		if (slot < kMaxArgumentSlots) m_pendingDynCount[slot] = 0;
	}
	void VulkanRenderEncoder::setDynamicBuffer(ShaderStage, uint32_t slot, BufferHandle buffer, uint64_t offset, uint64_t)
	{
		requireActive();
		if (slot >= kMaxArgumentSlots || m_pendingDynCount[slot] >= kMaxDynOffsetPerSlot)
			throw std::runtime_error("Vulkan dynamic-offset capacity exceeded");
		if (offset > UINT32_MAX)
			throw std::runtime_error("Vulkan dynamic offset exceeds uint32_t");
		if (m_owner != nullptr && buffer.isValid()) m_owner->trackBuffer(buffer);
		m_pendingDynOffsets[slot][m_pendingDynCount[slot]++] = static_cast<uint32_t>(offset);
	}
	void VulkanRenderEncoder::setRootConstants(ShaderStage stage, uint32_t slot, std::span<const std::byte> data)
	{
		requireActive();
		const auto& binding = requireRootBinding(*m_table, m_pipeline, slot, RootBindingKind::constants);
		if (data.empty() || data.size() > binding.size)
			throw std::runtime_error("Vulkan root-constant write exceeds the pipeline schema");
		const VkShaderStageFlags stages = toVkShaderStageFlags(stage);
		if (stages == 0)
			throw std::runtime_error("Vulkan root-constant write requires shader visibility");
		vkCmdPushConstants(m_cmd, m_layout, stages, binding.offset, static_cast<uint32_t>(data.size()), data.data());
	}
	void VulkanRenderEncoder::setRootPointer(ShaderStage stage, uint32_t slot, GpuPtr ptr)
	{
		requireActive();
		const auto& binding = requireRootBinding(*m_table, m_pipeline, slot, RootBindingKind::gpuPointer);
		const VkShaderStageFlags stages = toVkShaderStageFlags(stage);
		if (stages == 0 || binding.size != sizeof(ptr.value))
			throw std::runtime_error("Vulkan root-pointer binding does not match the pipeline schema");
		vkCmdPushConstants(m_cmd, m_layout, stages, binding.offset, sizeof(ptr.value), &ptr.value);
	}
	void VulkanRenderEncoder::setViewport(const Viewport& viewport)
	{
		requireActive();
		const VkViewport vp{
			viewport.x, viewport.y, viewport.width, viewport.height, viewport.minDepth, viewport.maxDepth
		};
		vkCmdSetViewportWithCount(m_cmd, 1, &vp);
	}

	void VulkanRenderEncoder::setScissor(const Rect2D& scissor)
	{
		requireActive();
		const VkRect2D rect{{scissor.offset.x, scissor.offset.y}, {scissor.extent.width, scissor.extent.height}};
		vkCmdSetScissorWithCount(m_cmd, 1, &rect);
	}
  void VulkanRenderEncoder::bindVertexBuffers(
    uint32_t firstBinding, std::span<const VertexBufferBinding> bindings)
  {
		requireActive();
    std::array<VkBuffer, 16> vkBuffers{};
    std::array<VkDeviceSize, 16> vkOffsets{};
    const uint32_t clamped = static_cast<uint32_t>((std::min)(bindings.size(), vkBuffers.size()));
    for(uint32_t index = 0; index < clamped; ++index)
    {
      vkBuffers[index] = m_table->resolveBuffer(bindings[index].buffer);
      if (vkBuffers[index] == VK_NULL_HANDLE)
        throw std::runtime_error("Vulkan vertex binding received a stale buffer");
      if (m_owner != nullptr) m_owner->trackBuffer(bindings[index].buffer);
      vkOffsets[index] = bindings[index].offset;
    }
    vkCmdBindVertexBuffers(m_cmd, firstBinding, clamped, vkBuffers.data(), vkOffsets.data());
  }

  void VulkanRenderEncoder::bindIndexBuffer(BufferHandle buffer, uint64_t offset, IndexFormat format)
	{
		requireActive();
		const VkBuffer native = m_table->resolveBuffer(buffer);
		if (native == VK_NULL_HANDLE)
			throw std::runtime_error("Vulkan index binding received a stale buffer");
		if (m_owner != nullptr) m_owner->trackBuffer(buffer);
		vkCmdBindIndexBuffer(m_cmd, native, offset,
		                     format == IndexFormat::uint16 ? VK_INDEX_TYPE_UINT16 : VK_INDEX_TYPE_UINT32);
	}

	void VulkanRenderEncoder::readInputAttachment(uint32_t)
	{
		requireActive();
		// Fallback path: input attachments are read via a sampled image binding today.
		// Real local-read maps to VK_KHR_dynamic_rendering_local_read in a later milestone.
	}

	void VulkanRenderEncoder::draw(const DrawDesc& desc)
	{
		requireActive();
		vkCmdDraw(m_cmd, desc.vertexCount, desc.instanceCount, desc.firstVertex, desc.firstInstance);
	}

	void VulkanRenderEncoder::drawIndexed(const DrawIndexedDesc& desc)
	{
		requireActive();
		if (!desc.indexBuffer.isNull())
		{
			bindIndexBuffer(desc.indexBuffer, desc.indexBufferOffset, desc.indexFormat);
		}
		vkCmdDrawIndexed(m_cmd, desc.indexCount, desc.instanceCount, desc.firstIndex, desc.vertexOffset,
		                 desc.firstInstance);
	}

	void VulkanRenderEncoder::drawIndexedIndirect(const DrawIndirectDesc& desc)
	{
		requireActive();
		const uint32_t stride = indirectStride(desc.stride, desc.drawCount, sizeof(VkDrawIndexedIndirectCommand));
		if (m_owner != nullptr) m_owner->trackBuffer(desc.argsBuffer);
		vkCmdDrawIndexedIndirect(m_cmd, m_table->resolveBuffer(desc.argsBuffer), desc.offset, desc.drawCount,
		                         stride);
	}

	void VulkanRenderEncoder::drawIndexedIndirectCount(const DrawIndirectCountDesc& desc)
	{
		requireActive();
		const uint32_t stride = indirectStride(desc.stride, desc.maxDrawCount, sizeof(VkDrawIndexedIndirectCommand));
		if (m_owner != nullptr)
		{
			m_owner->trackBuffer(desc.argsBuffer);
			m_owner->trackBuffer(desc.countBuffer);
		}
		vkCmdDrawIndexedIndirectCount(m_cmd, m_table->resolveBuffer(desc.argsBuffer), desc.argsOffset,
		                              m_table->resolveBuffer(desc.countBuffer), desc.countBufferOffset,
		                              desc.maxDrawCount, stride);
	}

	void VulkanRenderEncoder::drawIndirect(const DrawIndirectDesc& desc)
	{
		requireActive();
		const uint32_t stride = indirectStride(desc.stride, desc.drawCount, sizeof(VkDrawIndirectCommand));
		if (m_owner != nullptr) m_owner->trackBuffer(desc.argsBuffer);
		vkCmdDrawIndirect(m_cmd, m_table->resolveBuffer(desc.argsBuffer), desc.offset, desc.drawCount,
		                  stride);
	}

	void VulkanRenderEncoder::drawMeshTasks(uint32_t groupCountX, uint32_t groupCountY, uint32_t groupCountZ)
	{
		requireActive();
		if (vkCmdDrawMeshTasksEXT != nullptr)
		{
			vkCmdDrawMeshTasksEXT(m_cmd, groupCountX, groupCountY, groupCountZ);
		}
	}

	void VulkanRenderEncoder::drawMeshTasksIndirect(const DrawIndirectDesc& desc)
	{
		requireActive();
		if (vkCmdDrawMeshTasksIndirectEXT != nullptr)
		{
			const uint32_t stride = indirectStride(desc.stride, desc.drawCount,
			                                       sizeof(VkDrawMeshTasksIndirectCommandEXT));
			if (m_owner != nullptr) m_owner->trackBuffer(desc.argsBuffer);
			vkCmdDrawMeshTasksIndirectEXT(m_cmd, m_table->resolveBuffer(desc.argsBuffer), desc.offset,
			                              desc.drawCount,
			                              stride);
		}
	}

	// ---------------------------------------------------------------------------
	// VulkanComputeEncoder
	// ---------------------------------------------------------------------------
	void VulkanComputeEncoder::prepare(VkCommandBuffer cmd, VulkanResourceTable* table, VulkanCommandBuffer* owner)
	{
		m_cmd = cmd;
		m_table = table;
		m_owner = owner;
		m_pipeline = {};
		m_layout = VK_NULL_HANDLE;
		m_valid = true;
	}
	void VulkanComputeEncoder::invalidate() noexcept
	{
		m_valid = false;
	}
	void VulkanComputeEncoder::requireActive() const
	{
		if (!m_valid)
			throw std::runtime_error("Vulkan compute encoder is outside its encoding scope");
	}
	void VulkanComputeEncoder::requireCompute(const char* command) const
	{
		if (m_owner == nullptr)
			throw std::runtime_error("Vulkan compute encoder has no owning command buffer");
		m_owner->requireQueueOperation(QueueOperation::compute, command);
	}
	void VulkanComputeEncoder::requireTransfer(const char* command) const
	{
		if (m_owner == nullptr)
			throw std::runtime_error("Vulkan compute encoder has no owning command buffer");
		m_owner->requireQueueOperation(QueueOperation::transfer, command);
	}
	void VulkanComputeEncoder::setPipeline(PipelineHandle pipeline)
	{
		requireActive();
  requireCompute("VulkanComputeEncoder::setPipeline");
		m_layout = m_table->resolvePipelineLayout(pipeline);
		const VkPipeline native = m_table->resolvePipeline(pipeline, VK_PIPELINE_BIND_POINT_COMPUTE);
		if (m_layout == VK_NULL_HANDLE || native == VK_NULL_HANDLE)
			throw std::runtime_error("VulkanComputeEncoder::setPipeline received a stale pipeline");
		m_pipeline = pipeline;
		if (m_owner != nullptr) m_owner->trackPipeline(pipeline);
		vkCmdBindPipeline(m_cmd, VK_PIPELINE_BIND_POINT_COMPUTE, native);
	}
	void VulkanComputeEncoder::setArgumentTable(uint32_t slot, ArgumentTableHandle table)
	{
		requireActive();
  requireCompute("VulkanComputeEncoder::setArgumentTable");
		if (m_layout == VK_NULL_HANDLE)
			throw std::runtime_error("Vulkan argument-table binding requires a pipeline");
		const VkDescriptorSet set = m_table->resolveArgumentTable(table);
		if (set == VK_NULL_HANDLE)
			throw std::runtime_error("Vulkan argument-table binding received a stale table");
		if (m_owner != nullptr) m_owner->trackArgumentTable(table);
		vkCmdBindDescriptorSets(m_cmd, VK_PIPELINE_BIND_POINT_COMPUTE, m_layout, slot, 1, &set, 0, nullptr);
	}
	void VulkanComputeEncoder::setRootConstants(uint32_t slot, std::span<const std::byte> data)
	{
		requireActive();
  requireCompute("VulkanComputeEncoder::setRootConstants");
		const auto& binding = requireRootBinding(*m_table, m_pipeline, slot, RootBindingKind::constants);
		if (data.empty() || data.size() > binding.size)
			throw std::runtime_error("Vulkan root-constant write exceeds the pipeline schema");
		vkCmdPushConstants(m_cmd, m_layout, VK_SHADER_STAGE_COMPUTE_BIT, binding.offset, static_cast<uint32_t>(data.size()), data.data());
	}
	void VulkanComputeEncoder::setRootPointer(uint32_t slot, GpuPtr ptr)
	{
		requireActive();
  requireCompute("VulkanComputeEncoder::setRootPointer");
		const auto& binding = requireRootBinding(*m_table, m_pipeline, slot, RootBindingKind::gpuPointer);
		if (binding.size != sizeof(ptr.value))
			throw std::runtime_error("Vulkan root-pointer binding does not match the pipeline schema");
		vkCmdPushConstants(m_cmd, m_layout, VK_SHADER_STAGE_COMPUTE_BIT, binding.offset, sizeof(ptr.value), &ptr.value);
	}
	void VulkanComputeEncoder::dispatch(const DispatchDesc& desc)
	{
		requireActive();
  requireCompute("VulkanComputeEncoder::dispatch");
		vkCmdDispatch(m_cmd, desc.groupCountX, desc.groupCountY, desc.groupCountZ);
	}

	void VulkanComputeEncoder::dispatchIndirect(const DispatchIndirectDesc& desc)
	{
		requireActive();
  requireCompute("VulkanComputeEncoder::dispatchIndirect");
		if (m_owner != nullptr) m_owner->trackBuffer(desc.argsBuffer);
		vkCmdDispatchIndirect(m_cmd, m_table->resolveBuffer(desc.argsBuffer), desc.offset);
	}

	// ---------------------------------------------------------------------------
	// VulkanComputeEncoder: copy / blit command subset (Metal 4-aligned)
	// ---------------------------------------------------------------------------
	void VulkanComputeEncoder::copyBuffer(BufferHandle src, uint64_t srcOffset, BufferHandle dst, uint64_t dstOffset,
	                                      uint64_t size)
	{
		requireActive();
  requireTransfer("VulkanComputeEncoder::copyBuffer");
		const VkBufferCopy region{srcOffset, dstOffset, size};
		if (m_owner != nullptr)
		{
			m_owner->trackBuffer(src);
			m_owner->trackBuffer(dst);
		}
		vkCmdCopyBuffer(m_cmd, m_table->resolveBuffer(src), m_table->resolveBuffer(dst), 1,
		                &region);
	}

	void VulkanComputeEncoder::copyBufferToTexture(const BufferTextureCopyDesc& desc)
	{
		requireActive();
  requireTransfer("VulkanComputeEncoder::copyBufferToTexture");
		const VkBufferImageCopy region{
			.bufferOffset = desc.bufferOffset,
			.imageSubresource = {
				.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT,
				.mipLevel = desc.mipLevel,
				.baseArrayLayer = desc.baseArrayLayer,
				.layerCount = desc.layerCount
			},
			.imageOffset = {
				.x = desc.textureOffset.x,
				.y = desc.textureOffset.y,
				.z = desc.textureOffset.z,
			},
			.imageExtent = {desc.width, desc.height, desc.depth},
		};
		if (m_owner != nullptr)
		{
			m_owner->trackBuffer(desc.buffer);
			m_owner->trackTexture(desc.texture);
		}
		vkCmdCopyBufferToImage(m_cmd, m_table->resolveBuffer(desc.buffer),
		                       m_table->resolveTexture(desc.texture),
		                       VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &region);
	}

	void VulkanComputeEncoder::copyTextureToBuffer(const BufferTextureCopyDesc& desc)
	{
		requireActive();
  requireTransfer("VulkanComputeEncoder::copyTextureToBuffer");
		const VkBufferImageCopy region{
			.bufferOffset = desc.bufferOffset,
			.imageSubresource = {
				.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT,
				.mipLevel = desc.mipLevel,
				.baseArrayLayer = desc.baseArrayLayer,
				.layerCount = desc.layerCount
			},
			.imageOffset = {
				.x = desc.textureOffset.x,
				.y = desc.textureOffset.y,
				.z = desc.textureOffset.z,
			},
			.imageExtent = {desc.width, desc.height, desc.depth},
		};
		if (m_owner != nullptr)
		{
			m_owner->trackTexture(desc.texture);
			m_owner->trackBuffer(desc.buffer);
		}
		vkCmdCopyImageToBuffer(m_cmd, m_table->resolveTexture(desc.texture),
		                       VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
		                       m_table->resolveBuffer(desc.buffer), 1, &region);
	}

	void VulkanComputeEncoder::blitTexture(const TextureBlitDesc& desc)
	{
		requireActive();
  requireTransfer("VulkanComputeEncoder::blitTexture");
		VkImageBlit region{
			.srcSubresource = {
				.aspectMask = toVkImageAspect(desc.aspect),
				.mipLevel = desc.srcMipLevel,
				.baseArrayLayer = desc.srcBaseArrayLayer,
				.layerCount = desc.layerCount
			},
			.dstSubresource = {
				.aspectMask = toVkImageAspect(desc.aspect),
				.mipLevel = desc.dstMipLevel,
				.baseArrayLayer = desc.dstBaseArrayLayer,
				.layerCount = desc.layerCount
			},
		};
		region.srcOffsets[0] = {desc.srcOffsets[0].x, desc.srcOffsets[0].y, desc.srcOffsets[0].z};
		region.srcOffsets[1] = {desc.srcOffsets[1].x, desc.srcOffsets[1].y, desc.srcOffsets[1].z};
		region.dstOffsets[0] = {desc.dstOffsets[0].x, desc.dstOffsets[0].y, desc.dstOffsets[0].z};
		region.dstOffsets[1] = {desc.dstOffsets[1].x, desc.dstOffsets[1].y, desc.dstOffsets[1].z};
		if (m_owner != nullptr)
		{
			m_owner->trackTexture(desc.srcTexture);
			m_owner->trackTexture(desc.dstTexture);
		}
		vkCmdBlitImage(m_cmd, m_table->resolveTexture(desc.srcTexture), VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
		               m_table->resolveTexture(desc.dstTexture), VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1,
		               &region,
		               VK_FILTER_LINEAR);
	}

	void VulkanComputeEncoder::fillBuffer(BufferHandle buffer, uint64_t offset, uint64_t size, uint32_t data)
	{
		requireActive();
  requireTransfer("VulkanComputeEncoder::fillBuffer");
		if (m_owner != nullptr) m_owner->trackBuffer(buffer);
		vkCmdFillBuffer(m_cmd, m_table->resolveBuffer(buffer), offset, size == 0 ? VK_WHOLE_SIZE : size,
		                data);
	}

	// ---------------------------------------------------------------------------
	// VulkanCommandBuffer
	// ---------------------------------------------------------------------------
	VulkanCommandBuffer::VulkanCommandBuffer(
		VulkanCommandAllocator& allocator, VkCommandBuffer cmd, VulkanResourceTable* table)
		: m_cmd(cmd), m_table(table), m_allocator(&allocator)
	{
		if (m_cmd == VK_NULL_HANDLE || m_table == nullptr)
			throw std::runtime_error("VulkanCommandBuffer requires native storage and a resource table");
	}

	VulkanCommandBuffer::~VulkanCommandBuffer()
	{
		if (m_allocator != nullptr)
			m_allocator->releaseCommandBuffer(*this, m_cmd);
		m_cmd = VK_NULL_HANDLE;
	}

	void VulkanCommandBuffer::begin(CommandAllocator& allocator)
	{
		auto* vkAllocator = dynamic_cast<VulkanCommandAllocator*>(&allocator);
		if (vkAllocator == nullptr || vkAllocator != m_allocator)
			throw std::runtime_error("VulkanCommandBuffer::begin rejected a foreign allocator");
		if (m_state != CommandBufferState::idle && m_state != CommandBufferState::reusable)
			throw std::runtime_error("VulkanCommandBuffer::begin requires idle or reusable state");
		const VkCommandBufferBeginInfo beginInfo{
			.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO,
			.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT,
		};
		if (vkBeginCommandBuffer(m_cmd, &beginInfo) != VK_SUCCESS)
			throw std::runtime_error("VulkanCommandBuffer::begin failed");
		m_active = EncoderKind::none;
		m_submission = {};
		m_argumentTableCount = 0;
		m_pipelineCount = 0;
		m_bufferCount = 0;
		m_textureCount = 0;
		m_textureViewCount = 0;
		m_queryPoolCount = 0;
		m_residencySetCount = 0;
		m_resourceStates.reset();
		m_state = CommandBufferState::recording;
		incrementHotPathCounter(BackendType::vulkan, HotPathCounter::commandBufferBegins);
	}

	void VulkanCommandBuffer::end()
	{
		if (m_state != CommandBufferState::recording || m_active != EncoderKind::none)
			throw std::runtime_error(
				"VulkanCommandBuffer::end requires recording state with no active encoder");
		if (vkEndCommandBuffer(m_cmd) != VK_SUCCESS)
			throw std::runtime_error("VulkanCommandBuffer::end failed");
		m_state = CommandBufferState::executable;
	}

	CommandBufferState VulkanCommandBuffer::state() const noexcept
	{
		return m_state;
	}

	QueueClass VulkanCommandBuffer::queueClass() const noexcept
	{
		return m_allocator != nullptr ? m_allocator->queueClass() : QueueClass::graphics;
	}
	void VulkanCommandBuffer::requireQueueOperation(
		QueueOperation operation, const char* command) const
	{
		if (!supportsQueueOperation(queueClass(), operation))
			throw std::runtime_error(std::string(command) + " is unsupported by this queue class");
	}
	void VulkanCommandBuffer::validateForSubmit() const
	{
		if (m_state != CommandBufferState::executable || m_allocator == nullptr)
			throw std::runtime_error("VulkanCommandBuffer submission state is invalid");
		for (uint32_t index = 0; index < m_argumentTableCount; ++index)
		{
			if (!m_table->validateArgumentTableForSubmit(m_argumentTables[index]))
				throw std::runtime_error(
					"VulkanCommandBuffer submission references a stale argument table or resource");
		}
		for (uint32_t index = 0; index < m_pipelineCount; ++index)
			if (m_table->tryGetPipeline(m_pipelines[index]) == nullptr)
				throw std::runtime_error("VulkanCommandBuffer submission references a stale pipeline");
		for (uint32_t index = 0; index < m_bufferCount; ++index)
		{
			const BufferHandle buffer = m_buffers[index];
			if (m_table->tryGetBufferHot(buffer) == nullptr)
				throw std::runtime_error(
					"VulkanCommandBuffer submission references stale buffer " +
					std::to_string(buffer.index) + ":" + std::to_string(buffer.generation));
		}
		for (uint32_t index = 0; index < m_textureCount; ++index)
			if (m_table->tryGetTextureHot(m_textures[index]) == nullptr)
				throw std::runtime_error("VulkanCommandBuffer submission references a stale texture");
		for (uint32_t index = 0; index < m_textureViewCount; ++index)
			if (m_table->tryGetTextureViewHot(m_textureViews[index]) == nullptr)
				throw std::runtime_error("VulkanCommandBuffer submission references a stale texture view");
		for (uint32_t index = 0; index < m_queryPoolCount; ++index)
			if (m_table->resolveQueryPool(m_queryPools[index]) == VK_NULL_HANDLE)
				throw std::runtime_error("VulkanCommandBuffer submission references a stale query pool");
		for (uint32_t index = 0; index < m_residencySetCount; ++index)
			if (!m_table->validateResidencySetForSubmit(m_residencySets[index]))
				throw std::runtime_error(
					"VulkanCommandBuffer submission references a stale residency set or resource");
	}

	void VulkanCommandBuffer::markSubmitted(SubmissionToken token)
	{
		if (m_state != CommandBufferState::executable || m_allocator == nullptr || !token.isValid())
			throw std::runtime_error("VulkanCommandBuffer submission state is invalid");
		for (uint32_t index = 0; index < m_argumentTableCount; ++index)
			m_table->markArgumentTableSubmitted(m_argumentTables[index], token);
		for (uint32_t index = 0; index < m_pipelineCount; ++index)
			m_table->markPipelineSubmitted(m_pipelines[index], token);
		for (uint32_t index = 0; index < m_bufferCount; ++index)
			m_table->markBufferSubmitted(m_buffers[index], token);
		for (uint32_t index = 0; index < m_textureCount; ++index)
			m_table->markTextureSubmitted(m_textures[index], token);
		for (uint32_t index = 0; index < m_textureViewCount; ++index)
			m_table->markTextureViewSubmitted(m_textureViews[index], token);
		for (uint32_t index = 0; index < m_queryPoolCount; ++index)
			m_table->markQueryPoolSubmitted(m_queryPools[index], token);
		for (uint32_t index = 0; index < m_residencySetCount; ++index)
			m_table->markResidencySetSubmitted(m_residencySets[index], token);
		m_submission = token;
		m_state = CommandBufferState::submitted;
		m_allocator->noteSubmitted(*this, token);
	}

	void VulkanCommandBuffer::markReusable()
	{
		if (m_state == CommandBufferState::recording)
			throw std::runtime_error("VulkanCommandBuffer cannot reset while recording");
		m_active = EncoderKind::none;
		m_submission = {};
		m_argumentTableCount = 0;
		m_pipelineCount = 0;
		m_bufferCount = 0;
		m_textureCount = 0;
		m_textureViewCount = 0;
		m_queryPoolCount = 0;
		m_residencySetCount = 0;
		m_state = CommandBufferState::reusable;
	}

	void VulkanCommandBuffer::trackArgumentTable(ArgumentTableHandle table)
	{
		trackHandle(
			m_argumentTables, m_argumentTableCount, table,
			"Vulkan command-buffer argument-table capacity exceeded");
	}

	void VulkanCommandBuffer::trackPipeline(PipelineHandle pipeline)
	{
		trackHandle(
			m_pipelines, m_pipelineCount, pipeline,
			"Vulkan command-buffer pipeline capacity exceeded");
	}

	void VulkanCommandBuffer::trackBuffer(BufferHandle buffer)
	{
		trackHandle(
			m_buffers, m_bufferCount, buffer,
			"Vulkan command-buffer buffer capacity exceeded");
	}

	void VulkanCommandBuffer::trackTexture(TextureHandle texture)
	{
		trackHandle(
			m_textures, m_textureCount, texture,
			"Vulkan command-buffer texture capacity exceeded");
	}

	void VulkanCommandBuffer::trackTextureView(TextureViewHandle view)
	{
		trackHandle(
			m_textureViews, m_textureViewCount, view,
			"Vulkan command-buffer texture-view capacity exceeded");
	}

	void VulkanCommandBuffer::trackQueryPool(QueryPoolHandle pool)
	{
		trackHandle(
			m_queryPools, m_queryPoolCount, pool,
			"Vulkan command-buffer query-pool capacity exceeded");
	}

	void VulkanCommandBuffer::trackResidencySet(ResidencySetHandle set)
	{
		trackHandle(
			m_residencySets, m_residencySetCount, set,
			"Vulkan command-buffer residency-set capacity exceeded");
	}

	void VulkanCommandBuffer::requireRecording(const char* operation) const
	{
		if (m_state != CommandBufferState::recording)
			throw std::runtime_error(std::string(operation) + " requires recording state");
	}

	void VulkanCommandBuffer::setTarget(VkCommandBuffer cmd, VulkanResourceTable* table)
	{
		if (cmd == VK_NULL_HANDLE || table == nullptr)
			throw std::runtime_error("VulkanCommandBuffer::setTarget requires native storage and a table");
		m_cmd = cmd;
		m_table = table;
		m_active = EncoderKind::none;
		m_argumentTableCount = 0;
		m_pipelineCount = 0;
		m_bufferCount = 0;
		m_textureCount = 0;
		m_textureViewCount = 0;
		m_queryPoolCount = 0;
		m_residencySetCount = 0;
		m_resourceStates.reset();
		m_state = CommandBufferState::recording;
	}
	RenderEncoder* VulkanCommandBuffer::beginRenderPass(const RenderPassDesc& desc)
	{
		requireRecording("VulkanCommandBuffer::beginRenderPass");
		requireQueueOperation(QueueOperation::render, "VulkanCommandBuffer::beginRenderPass");
		if (m_active != EncoderKind::none)
			throw std::runtime_error("VulkanCommandBuffer::beginRenderPass called while an encoder is active");
		const uint32_t colorTargetCount = static_cast<uint32_t>(desc.colorTargets.size());
    if(colorTargetCount > kMaxColorAttachmentsPerPass)
			throw std::runtime_error("Vulkan render pass has invalid color attachments");
		std::array<VkRenderingAttachmentInfo, kMaxColorAttachmentsPerPass> colorAttachments{};
		for (uint32_t i = 0; i < colorTargetCount; ++i)
		{
			const RenderTargetDesc& target = desc.colorTargets[i];
			const VkImageView view = m_table->resolveTextureView(target.view);
			if (view == VK_NULL_HANDLE)
				throw std::runtime_error("Vulkan render pass received a stale color target view");
			trackTextureView(target.view);
			colorAttachments[i] = VkRenderingAttachmentInfo{
				.sType = VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO,
				.imageView = view,
				.imageLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL,
				.loadOp = static_cast<VkAttachmentLoadOp>(target.loadOp),
				.storeOp = static_cast<VkAttachmentStoreOp>(target.storeOp),
			};
			colorAttachments[i].clearValue.color = {
				{target.clearColor.r, target.clearColor.g, target.clearColor.b, target.clearColor.a}
			};
		}

		VkRenderingAttachmentInfo depthAttachment{};
		if (desc.depthTarget != nullptr)
		{
			const VkImageView view = m_table->resolveTextureView(desc.depthTarget->view);
			if (view == VK_NULL_HANDLE)
				throw std::runtime_error("Vulkan render pass received a stale depth target view");
			trackTextureView(desc.depthTarget->view);
			depthAttachment = VkRenderingAttachmentInfo{
				.sType = VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO,
				.imageView = view,
				.imageLayout = toVkImageLayout(desc.depthTarget->state),
				.loadOp = static_cast<VkAttachmentLoadOp>(desc.depthTarget->loadOp),
				.storeOp = static_cast<VkAttachmentStoreOp>(desc.depthTarget->storeOp),
			};
			depthAttachment.clearValue.depthStencil = {
				desc.depthTarget->clearValue.depth, desc.depthTarget->clearValue.stencil
			};
		}

		for (const InputAttachmentDesc& input : desc.inputAttachments)
		{
			if (m_table->resolveTextureView(input.view) == VK_NULL_HANDLE)
				throw std::runtime_error("Vulkan render pass received a stale input attachment view");
			trackTextureView(input.view);
		}

		const VkRenderingInfo renderingInfo{
			.sType = VK_STRUCTURE_TYPE_RENDERING_INFO,
			.renderArea = {
				{desc.renderArea.offset.x, desc.renderArea.offset.y},
				{desc.renderArea.extent.width, desc.renderArea.extent.height}
			},
			.layerCount = 1,
			.colorAttachmentCount = colorTargetCount,
			.pColorAttachments = colorTargetCount > 0 ? colorAttachments.data() : nullptr,
			.pDepthAttachment = desc.depthTarget != nullptr ? &depthAttachment : nullptr,
		};
		vkCmdBeginRendering(m_cmd, &renderingInfo);
		incrementHotPathCounter(BackendType::vulkan, HotPathCounter::encoderBegins);
		m_renderEncoder.prepare(m_cmd, m_table, this);
		m_active = EncoderKind::render;
		return &m_renderEncoder;
	}
	ComputeEncoder* VulkanCommandBuffer::beginComputePass()
	{
		requireRecording("VulkanCommandBuffer::beginComputePass");
		if (m_active != EncoderKind::none)
			throw std::runtime_error("VulkanCommandBuffer::beginComputePass called while an encoder is active");
		incrementHotPathCounter(BackendType::vulkan, HotPathCounter::encoderBegins);
		m_computeEncoder.prepare(m_cmd, m_table, this);
		m_active = EncoderKind::compute;
		return &m_computeEncoder;
	}
	void VulkanCommandBuffer::endEncoding()
	{
		requireRecording("VulkanCommandBuffer::endEncoding");
		if (m_active == EncoderKind::none)
			throw std::runtime_error("VulkanCommandBuffer::endEncoding called without an active encoder");
		if (m_active == EncoderKind::render)
		{
			vkCmdEndRendering(m_cmd);
			m_renderEncoder.invalidate();
		}
		else
		{
			m_computeEncoder.invalidate();
		}
		m_active = EncoderKind::none;
	}

	RHIResult VulkanCommandBuffer::useResidencySet(ResidencySetHandle set)
	{
		if (m_state != CommandBufferState::recording)
			return RHIResult::fail(RHIErrorCode::invalidState, "ResidencySet use requires recording state");
		if (m_table == nullptr || !m_table->validateResidencySetForSubmit(set))
			return RHIResult::fail(
				RHIErrorCode::invalidHandle, "ResidencySet handle or resource is stale");
		trackResidencySet(set);
		return RHIResult::ok();
	}
	void VulkanCommandBuffer::barrier(StageFlags producer, StageFlags consumer, HazardFlags hazards)
	{
		requireRecording("VulkanCommandBuffer::barrier");
		if (!any(hazards)) return;
		const VkMemoryBarrier2 memoryBarrier = makeMemoryBarrier2(producer, consumer, hazards);
		const VkDependencyInfo dependencyInfo{
			.sType = VK_STRUCTURE_TYPE_DEPENDENCY_INFO,
			.memoryBarrierCount = 1,
			.pMemoryBarriers = &memoryBarrier,
		};
		vkCmdPipelineBarrier2(m_cmd, &dependencyInfo);
	}
  void VulkanCommandBuffer::resourceBarrier(std::span<const TextureBarrier> textures,
                                            std::span<const BufferBarrier> buffers,
                                            std::span<const AliasingBarrier> aliasing)
	{
		requireRecording("VulkanCommandBuffer::resourceBarrier");
    const uint32_t textureCount = static_cast<uint32_t>(textures.size());
    const uint32_t bufferCount = static_cast<uint32_t>(buffers.size());
#ifndef NDEBUG
		for (uint32_t index = 0; index < textureCount; ++index)
		{
			if (!m_resourceStates.transition(textures[index]).valid())
				throw std::runtime_error("VulkanCommandBuffer rejected an invalid texture transition");
		}
		for (uint32_t index = 0; index < bufferCount; ++index)
		{
			if (!m_resourceStates.transition(buffers[index]).valid())
				throw std::runtime_error("VulkanCommandBuffer rejected an invalid buffer transition");
		}
#endif
		uint32_t textureOffset = 0;
		uint32_t bufferOffset = 0;
		while (textureOffset < textureCount || bufferOffset < bufferCount)
		{
			std::array<VkImageMemoryBarrier2, kMaxBarrierBatchSize> imageBarriers{};
			std::array<VkBufferMemoryBarrier2, kMaxBarrierBatchSize> bufferBarriers{};

			const uint32_t textureRemaining = textureCount - textureOffset;
			const uint32_t bufferRemaining = bufferCount - bufferOffset;
			const uint32_t imageBatch = textureRemaining < kMaxBarrierBatchSize
				                            ? textureRemaining
				                            : kMaxBarrierBatchSize;
			const uint32_t bufferBatch = bufferRemaining < kMaxBarrierBatchSize
				                             ? bufferRemaining
				                             : kMaxBarrierBatchSize;

			for (uint32_t i = 0; i < imageBatch; ++i)
			{
				const TextureBarrier& b = textures[textureOffset + i];
				const VkImage nativeImage = m_table->resolveTexture(b.texture);
				trackTexture(b.texture);
				if (nativeImage == VK_NULL_HANDLE)
					throw std::runtime_error("Vulkan resource barrier received a stale texture");
				// Queue ownership is mappable in the RHI barrier shape, but v1 records the
				// renderer on a same-queue path. Keep ownership fields ignored until the
				// backend enables real multi-queue scheduling.
				imageBarriers[i] = VkImageMemoryBarrier2{
					.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER_2,
					.srcStageMask = VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT,
					.srcAccessMask = VK_ACCESS_2_MEMORY_WRITE_BIT,
					.dstStageMask = VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT,
					.dstAccessMask = VK_ACCESS_2_MEMORY_READ_BIT | VK_ACCESS_2_MEMORY_WRITE_BIT,
					.oldLayout = toVkImageLayout(b.before),
					.newLayout = toVkImageLayout(b.after),
					.image = nativeImage,
					.subresourceRange = {
						.aspectMask = toVkImageAspect(b.range.aspect),
						.baseMipLevel = b.range.baseMipLevel,
						.levelCount = b.range.levelCount,
						.baseArrayLayer = b.range.baseArrayLayer,
						.layerCount = b.range.layerCount
					},
				};
			}
			for (uint32_t i = 0; i < bufferBatch; ++i)
			{
				const BufferBarrier& b = buffers[bufferOffset + i];
				const VkBuffer nativeBuffer = m_table->resolveBuffer(b.buffer);
				trackBuffer(b.buffer);
				if (nativeBuffer == VK_NULL_HANDLE)
					throw std::runtime_error("Vulkan resource barrier received a stale buffer");
				// Same-queue v1 behavior: srcQueue/dstQueue are retained in the public RHI
				// barrier but do not lower to queue-family ownership transfers yet.
				bufferBarriers[i] = VkBufferMemoryBarrier2{
					.sType = VK_STRUCTURE_TYPE_BUFFER_MEMORY_BARRIER_2,
					.srcStageMask = VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT,
					.srcAccessMask = VK_ACCESS_2_MEMORY_WRITE_BIT,
					.dstStageMask = VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT,
					.dstAccessMask = VK_ACCESS_2_MEMORY_READ_BIT | VK_ACCESS_2_MEMORY_WRITE_BIT,
					.buffer = nativeBuffer,
					.offset = b.offset,
					.size = b.size == 0 ? VK_WHOLE_SIZE : b.size,
				};
			}

			const VkDependencyInfo dependencyInfo{
				.sType = VK_STRUCTURE_TYPE_DEPENDENCY_INFO,
				.bufferMemoryBarrierCount = bufferBatch,
				.pBufferMemoryBarriers = bufferBatch > 0 ? bufferBarriers.data() : nullptr,
				.imageMemoryBarrierCount = imageBatch,
				.pImageMemoryBarriers = imageBatch > 0 ? imageBarriers.data() : nullptr,
			};
			vkCmdPipelineBarrier2(m_cmd, &dependencyInfo);

			textureOffset += imageBatch;
			bufferOffset += bufferBatch;
		}

		if (!aliasing.empty())
		{
			for (const AliasingBarrier& source : aliasing)
			{
				if (!source.before.isValid() || !source.after.isValid() ||
				    source.after.kind == AliasingResourceKind::none)
					throw std::runtime_error("Vulkan aliasing barrier received an invalid resource");
				const auto resolve = [this](const AliasingResource& resource) {
					switch (resource.kind)
					{
					case AliasingResourceKind::none:
						return true;
					case AliasingResourceKind::buffer:
						return m_table->resolveBuffer(resource.buffer) != VK_NULL_HANDLE;
					case AliasingResourceKind::texture:
						return m_table->resolveTexture(resource.texture) != VK_NULL_HANDLE;
					}
					return false;
				};
				if (!resolve(source.before) || !resolve(source.after))
					throw std::runtime_error("Vulkan aliasing barrier received a stale resource");
				const auto track = [this](const AliasingResource& resource) {
					switch (resource.kind)
					{
					case AliasingResourceKind::none:
						break;
					case AliasingResourceKind::buffer:
						trackBuffer(resource.buffer);
						break;
					case AliasingResourceKind::texture:
						trackTexture(resource.texture);
						break;
					}
				};
				track(source.before);
				track(source.after);
			}

			const VkMemoryBarrier2 memoryBarrier{
				.sType = VK_STRUCTURE_TYPE_MEMORY_BARRIER_2,
				.srcStageMask = VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT,
				.srcAccessMask = VK_ACCESS_2_MEMORY_WRITE_BIT,
				.dstStageMask = VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT,
				.dstAccessMask = VK_ACCESS_2_MEMORY_READ_BIT | VK_ACCESS_2_MEMORY_WRITE_BIT,
			};
			const VkDependencyInfo dependencyInfo{
				.sType = VK_STRUCTURE_TYPE_DEPENDENCY_INFO,
				.memoryBarrierCount = 1,
				.pMemoryBarriers = &memoryBarrier,
			};
			vkCmdPipelineBarrier2(m_cmd, &dependencyInfo);
		}
	}

	void VulkanCommandBuffer::clearColorTexture(TextureHandle texture,
	                                            const TextureSubresourceRange& range,
	                                            const ClearColorValue& clearColor)
	{
		requireRecording("VulkanCommandBuffer::clearColorTexture");
		const VkImage nativeImage = m_table->resolveTexture(texture);
		if (nativeImage == VK_NULL_HANDLE)
			throw std::runtime_error("Vulkan clear received a stale texture");
		trackTexture(texture);
		const VkClearColorValue value{{clearColor.r, clearColor.g, clearColor.b, clearColor.a}};
		const VkImageSubresourceRange vkRange{
			.aspectMask = toVkImageAspect(range.aspect),
			.baseMipLevel = range.baseMipLevel,
			.levelCount = range.levelCount,
			.baseArrayLayer = range.baseArrayLayer,
			.layerCount = range.layerCount,
		};
		vkCmdClearColorImage(m_cmd, nativeImage, VK_IMAGE_LAYOUT_GENERAL, &value, 1,
		                     &vkRange);
	}

	void VulkanCommandBuffer::beginEvent(const char* name)
	{
		requireRecording("VulkanCommandBuffer::beginEvent");
		if (name == nullptr)
			throw std::invalid_argument("VulkanCommandBuffer::beginEvent requires a name");
		if (vkCmdBeginDebugUtilsLabelEXT != nullptr)
		{
			const VkDebugUtilsLabelEXT label{.sType = VK_STRUCTURE_TYPE_DEBUG_UTILS_LABEL_EXT, .pLabelName = name};
			vkCmdBeginDebugUtilsLabelEXT(m_cmd, &label);
		}
	}

	void VulkanCommandBuffer::endEvent()
	{
		requireRecording("VulkanCommandBuffer::endEvent");
		if (vkCmdEndDebugUtilsLabelEXT != nullptr)
		{
			vkCmdEndDebugUtilsLabelEXT(m_cmd);
		}
	}

	void VulkanCommandBuffer::resetQueryPool(QueryPoolHandle pool, uint32_t firstQuery, uint32_t queryCount)
	{
		requireRecording("VulkanCommandBuffer::resetQueryPool");
		if (m_active != EncoderKind::none)
			throw std::runtime_error("VulkanCommandBuffer::resetQueryPool requires no active encoder");
		const VkQueryPool nativePool = m_table != nullptr ? m_table->resolveQueryPool(pool) : VK_NULL_HANDLE;
		if (nativePool == VK_NULL_HANDLE || queryCount == 0)
		{
			return;
		}
		trackQueryPool(pool);
		vkCmdResetQueryPool(m_cmd, nativePool, firstQuery,
		                    queryCount);
	}

	void VulkanCommandBuffer::writeTimestamp(QueryPoolHandle pool, uint32_t queryIndex, bool afterAllCommands)
	{
		requireRecording("VulkanCommandBuffer::writeTimestamp");
		if (m_active != EncoderKind::none)
			throw std::runtime_error("VulkanCommandBuffer::writeTimestamp requires no active encoder");
		const VkQueryPool nativePool = m_table != nullptr ? m_table->resolveQueryPool(pool) : VK_NULL_HANDLE;
		if (nativePool == VK_NULL_HANDLE)
		{
			return;
		}
		trackQueryPool(pool);
		vkCmdWriteTimestamp2(m_cmd,
		                     afterAllCommands
			                     ? VK_PIPELINE_STAGE_2_BOTTOM_OF_PIPE_BIT
			                     : VK_PIPELINE_STAGE_2_TOP_OF_PIPE_BIT,
		                     nativePool, queryIndex);
	}
} // namespace demo::rhi::vulkan
