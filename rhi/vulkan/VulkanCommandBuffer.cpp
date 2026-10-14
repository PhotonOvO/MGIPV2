#include "VulkanCommandBuffer.h"

#include "internal/VulkanCommon.h"
#include "VulkanBarrierConversions.h"
#include "VulkanCommandAllocator.h"
#include "VulkanQueue.h"
#include "VulkanResourceTable.h"
#include "VulkanShaderConversions.h"
#include "../RHIDebugCounters.h"

#include <algorithm>
#include <array>
#include <exception>
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

		[[nodiscard]] VkBuffer requireBufferForRecording(
			const VulkanResourceTable* table, BufferHandle handle, const char* operation)
		{
			const BufferHotRecord* record =
				table != nullptr && handle.isValid() ? table->tryGetBufferHot(handle) : nullptr;
			if(record == nullptr || record->nativeBuffer == VK_NULL_HANDLE)
				throw std::runtime_error(
					std::string(operation) + " received a stale/null buffer handle");
			return record->nativeBuffer;
		}

		[[nodiscard]] VkImage requireTextureForRecording(
			const VulkanResourceTable* table, TextureHandle handle, const char* operation)
		{
			const TextureHotRecord* record =
				table != nullptr && handle.isValid() ? table->tryGetTextureHot(handle) : nullptr;
			if(record == nullptr || record->nativeImage == VK_NULL_HANDLE)
				throw std::runtime_error(
					std::string(operation) + " received a stale/null texture handle");
			return record->nativeImage;
		}

		struct TextureViewForRecording
		{
			VkImageView native{VK_NULL_HANDLE};
			TextureHandle parent{};
		};

		[[nodiscard]] TextureViewForRecording requireTextureViewForRecording(
			const VulkanResourceTable* table, TextureViewHandle handle, const char* operation)
		{
			const TextureViewHotRecord* hot =
				table != nullptr && handle.isValid() ? table->tryGetTextureViewHot(handle) : nullptr;
			const TextureViewColdRecord* cold =
				hot != nullptr ? table->tryGetTextureViewCold(handle) : nullptr;
			if(hot == nullptr || cold == nullptr || hot->nativeView == VK_NULL_HANDLE)
				throw std::runtime_error(
					std::string(operation) + " received a stale/null texture-view handle");
			if(!cold->parentTexture.isNull())
				(void)requireTextureForRecording(table, cold->parentTexture, operation);
			return TextureViewForRecording{hot->nativeView, cold->parentTexture};
		}

		[[nodiscard]] VkSampler requireSamplerForRecording(
			const VulkanResourceTable* table, SamplerHandle handle, const char* operation)
		{
			const VkSampler native = table != nullptr && handle.isValid()
				                         ? table->resolveSampler(handle)
				                         : VK_NULL_HANDLE;
			if(native == VK_NULL_HANDLE)
				throw std::runtime_error(
					std::string(operation) + " received a stale/null sampler handle");
			return native;
		}

		struct PipelineForRecording
		{
			VkPipeline native{VK_NULL_HANDLE};
			VkPipelineLayout layout{VK_NULL_HANDLE};
		};

		[[nodiscard]] PipelineForRecording requirePipelineForRecording(
			const VulkanResourceTable* table, PipelineHandle handle,
			VkPipelineBindPoint expectedBindPoint, const char* operation)
		{
			const PipelineRecord* record =
				table != nullptr && handle.isValid() ? table->tryGetPipeline(handle) : nullptr;
			if(record == nullptr || record->bindPoint != expectedBindPoint ||
			   record->nativePipeline == VK_NULL_HANDLE || record->nativeLayout == VK_NULL_HANDLE)
				throw std::runtime_error(
					std::string(operation) + " received a stale/null pipeline handle");
			return PipelineForRecording{record->nativePipeline, record->nativeLayout};
		}

		[[nodiscard]] VkDescriptorSet requireArgumentTableForRecording(
			const VulkanResourceTable* table, ArgumentTableHandle handle, const char* operation)
		{
			const ArgumentTableRecord* record =
				table != nullptr && handle.isValid() ? table->tryGetArgumentTable(handle) : nullptr;
			if(record == nullptr || record->nativeSet == VK_NULL_HANDLE ||
			   !table->validateArgumentTableForSubmit(handle))
				throw std::runtime_error(
					std::string(operation) + " received a stale/null argument-table handle");
			return record->nativeSet;
		}

		[[nodiscard]] const QueryPoolRecord& requireQueryPoolForRecording(
			const VulkanResourceTable* table, QueryPoolHandle handle, const char* operation)
		{
			const QueryPoolRecord* record = table != nullptr && handle.isValid()
				                                ? table->tryGetQueryPool(handle)
				                                : nullptr;
			if(record == nullptr || record->nativePool == VK_NULL_HANDLE)
				throw std::runtime_error(
					std::string(operation) + " received a stale/null query-pool handle");
			return *record;
		}

		inline constexpr uint32_t kMaxColorAttachmentsPerPass = 8;
		inline constexpr uint32_t kMaxBarrierBatchSize = 16;
		inline constexpr uint32_t kInitialTrackedBufferCapacity = 512;

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
		const PipelineForRecording resolved = requirePipelineForRecording(
			m_table, pipeline, VK_PIPELINE_BIND_POINT_GRAPHICS,
			"VulkanRenderEncoder::setPipeline");
		if (m_owner != nullptr) m_owner->trackPipeline(pipeline);
		m_layout = resolved.layout;
		m_pipeline = pipeline;
		vkCmdBindPipeline(m_cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, resolved.native);
	}
	void VulkanRenderEncoder::setArgumentTable(ShaderStage /*stages*/, uint32_t slot, ArgumentTableHandle table)
	{
		requireActive();
		if (m_layout == VK_NULL_HANDLE)
			throw std::runtime_error("Vulkan argument-table binding requires a pipeline");
		const VkDescriptorSet set = requireArgumentTableForRecording(
				m_table, table, "Vulkan render argument-table binding");
		if (m_owner != nullptr) m_owner->trackArgumentTable(table);
		const uint32_t dynCount = slot < kMaxArgumentSlots ? m_pendingDynCount[slot] : 0;
		const uint32_t* dynOffsets = dynCount > 0 ? m_pendingDynOffsets[slot] : nullptr;
		vkCmdBindDescriptorSets(m_cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, m_layout, slot, 1, &set, dynCount, dynOffsets);
		if (slot < kMaxArgumentSlots) m_pendingDynCount[slot] = 0;
	}
	void VulkanRenderEncoder::setDynamicBuffer(ShaderStage, uint32_t slot, BufferHandle buffer, uint64_t offset, uint64_t)
	{
		requireActive();
		// A null handle is the Vulkan offset-only form used when the descriptor
		// table already owns the buffer binding. Every non-null shape is required.
		if(!buffer.isNull())
			(void)requireBufferForRecording(
				m_table, buffer, "Vulkan dynamic-buffer binding");
		if (slot >= kMaxArgumentSlots || m_pendingDynCount[slot] >= kMaxDynOffsetPerSlot)
			throw std::runtime_error("Vulkan dynamic-offset capacity exceeded");
		if (offset > UINT32_MAX)
			throw std::runtime_error("Vulkan dynamic offset exceeds uint32_t");
		if (m_owner != nullptr && !buffer.isNull()) m_owner->trackBuffer(buffer);
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
			vkBuffers[index] = requireBufferForRecording(
				m_table, bindings[index].buffer, "Vulkan vertex binding");
			vkOffsets[index] = bindings[index].offset;
		}
		if(m_owner != nullptr)
			for(uint32_t index = 0; index < clamped; ++index)
				m_owner->trackBuffer(bindings[index].buffer);
		vkCmdBindVertexBuffers(m_cmd, firstBinding, clamped, vkBuffers.data(), vkOffsets.data());
	}

	void VulkanRenderEncoder::bindIndexBuffer(BufferHandle buffer, uint64_t offset, IndexFormat format)
	{
		requireActive();
		const VkBuffer native = requireBufferForRecording(
			m_table, buffer, "Vulkan index binding");
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
		const VkBuffer argsBuffer = requireBufferForRecording(
			m_table, desc.argsBuffer, "Vulkan indexed-indirect draw");
		const uint32_t stride = indirectStride(desc.stride, desc.drawCount, sizeof(VkDrawIndexedIndirectCommand));
		if (m_owner != nullptr) m_owner->trackBuffer(desc.argsBuffer);
		vkCmdDrawIndexedIndirect(m_cmd, argsBuffer, desc.offset, desc.drawCount,
		                         stride);
	}

	void VulkanRenderEncoder::drawIndexedIndirectCount(const DrawIndirectCountDesc& desc)
	{
		requireActive();
		const VkBuffer argsBuffer = requireBufferForRecording(
			m_table, desc.argsBuffer, "Vulkan indexed-indirect-count draw arguments");
		const VkBuffer countBuffer = requireBufferForRecording(
			m_table, desc.countBuffer, "Vulkan indexed-indirect-count draw counter");
		const uint32_t stride = indirectStride(desc.stride, desc.maxDrawCount, sizeof(VkDrawIndexedIndirectCommand));
		if (m_owner != nullptr)
		{
			m_owner->trackBuffer(desc.argsBuffer);
			m_owner->trackBuffer(desc.countBuffer);
		}
		vkCmdDrawIndexedIndirectCount(m_cmd, argsBuffer, desc.argsOffset,
		                              countBuffer, desc.countBufferOffset,
		                              desc.maxDrawCount, stride);
	}

	void VulkanRenderEncoder::drawIndirect(const DrawIndirectDesc& desc)
	{
		requireActive();
		const VkBuffer argsBuffer = requireBufferForRecording(
			m_table, desc.argsBuffer, "Vulkan indirect draw");
		const uint32_t stride = indirectStride(desc.stride, desc.drawCount, sizeof(VkDrawIndirectCommand));
		if (m_owner != nullptr) m_owner->trackBuffer(desc.argsBuffer);
		vkCmdDrawIndirect(m_cmd, argsBuffer, desc.offset, desc.drawCount,
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
		const VkBuffer argsBuffer = requireBufferForRecording(
			m_table, desc.argsBuffer, "Vulkan mesh-indirect draw");
		if (vkCmdDrawMeshTasksIndirectEXT != nullptr)
		{
			const uint32_t stride = indirectStride(desc.stride, desc.drawCount,
				                                       sizeof(VkDrawMeshTasksIndirectCommandEXT));
			if (m_owner != nullptr) m_owner->trackBuffer(desc.argsBuffer);
			vkCmdDrawMeshTasksIndirectEXT(m_cmd, argsBuffer, desc.offset,
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
		const PipelineForRecording resolved = requirePipelineForRecording(
			m_table, pipeline, VK_PIPELINE_BIND_POINT_COMPUTE,
			"VulkanComputeEncoder::setPipeline");
		if (m_owner != nullptr) m_owner->trackPipeline(pipeline);
		m_layout = resolved.layout;
		m_pipeline = pipeline;
		vkCmdBindPipeline(m_cmd, VK_PIPELINE_BIND_POINT_COMPUTE, resolved.native);
	}
	void VulkanComputeEncoder::setArgumentTable(uint32_t slot, ArgumentTableHandle table)
	{
		requireActive();
  requireCompute("VulkanComputeEncoder::setArgumentTable");
		if (m_layout == VK_NULL_HANDLE)
			throw std::runtime_error("Vulkan argument-table binding requires a pipeline");
		const VkDescriptorSet set = requireArgumentTableForRecording(
			m_table, table, "Vulkan compute argument-table binding");
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
		const VkBuffer argsBuffer = requireBufferForRecording(
			m_table, desc.argsBuffer, "Vulkan indirect dispatch");
		if (m_owner != nullptr) m_owner->trackBuffer(desc.argsBuffer);
		vkCmdDispatchIndirect(m_cmd, argsBuffer, desc.offset);
	}

	// ---------------------------------------------------------------------------
	// VulkanComputeEncoder: copy / blit command subset (Metal 4-aligned)
	// ---------------------------------------------------------------------------
	void VulkanComputeEncoder::copyBuffer(BufferHandle src, uint64_t srcOffset, BufferHandle dst, uint64_t dstOffset,
	                                      uint64_t size)
	{
		requireActive();
  requireTransfer("VulkanComputeEncoder::copyBuffer");
		const VkBuffer sourceBuffer = requireBufferForRecording(
			m_table, src, "Vulkan buffer copy source");
		const VkBuffer destinationBuffer = requireBufferForRecording(
			m_table, dst, "Vulkan buffer copy destination");
		const VkBufferCopy region{srcOffset, dstOffset, size};
		if (m_owner != nullptr)
		{
			m_owner->trackBuffer(src);
			m_owner->trackBuffer(dst);
		}
		vkCmdCopyBuffer(m_cmd, sourceBuffer, destinationBuffer, 1,
		                &region);
	}

	void VulkanComputeEncoder::copyBufferToTexture(const BufferTextureCopyDesc& desc)
	{
		requireActive();
		requireTransfer("VulkanComputeEncoder::copyBufferToTexture");
		const VkBuffer sourceBuffer = requireBufferForRecording(
			m_table, desc.buffer, "Vulkan buffer-to-image copy source");
		const VkImage destinationImage = requireTextureForRecording(
			m_table, desc.texture, "Vulkan buffer-to-image copy destination");
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
		vkCmdCopyBufferToImage(m_cmd, sourceBuffer, destinationImage,
		                       VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &region);
	}

	void VulkanComputeEncoder::copyTextureToBuffer(const BufferTextureCopyDesc& desc)
	{
		requireActive();
		requireTransfer("VulkanComputeEncoder::copyTextureToBuffer");
		const VkBuffer destinationBuffer = requireBufferForRecording(
			m_table, desc.buffer, "Vulkan image-to-buffer copy destination");
		const VkImage sourceImage = requireTextureForRecording(
			m_table, desc.texture, "Vulkan image-to-buffer copy source");
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
		vkCmdCopyImageToBuffer(m_cmd, sourceImage, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
		                       destinationBuffer, 1, &region);
	}

	void VulkanComputeEncoder::blitTexture(const TextureBlitDesc& desc)
	{
		requireActive();
  requireTransfer("VulkanComputeEncoder::blitTexture");
		const VkImage sourceImage = requireTextureForRecording(
			m_table, desc.srcTexture, "Vulkan texture blit source");
		const VkImage destinationImage = requireTextureForRecording(
			m_table, desc.dstTexture, "Vulkan texture blit destination");
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
		vkCmdBlitImage(m_cmd, sourceImage, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
		               destinationImage, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1,
		               &region,
		               VK_FILTER_LINEAR);
	}

	void VulkanComputeEncoder::fillBuffer(BufferHandle buffer, uint64_t offset, uint64_t size, uint32_t data)
	{
		requireActive();
  requireTransfer("VulkanComputeEncoder::fillBuffer");
		const VkBuffer nativeBuffer = requireBufferForRecording(
			m_table, buffer, "Vulkan buffer fill");
		if (m_owner != nullptr) m_owner->trackBuffer(buffer);
		vkCmdFillBuffer(m_cmd, nativeBuffer, offset, size == 0 ? VK_WHOLE_SIZE : size,
		                data);
	}

	// ---------------------------------------------------------------------------
	// VulkanCommandBuffer
	// ---------------------------------------------------------------------------
	VulkanCommandBuffer::VulkanCommandBuffer()
	{
		m_buffers.reserve(kInitialTrackedBufferCapacity);
		m_textures.reserve(256);
		m_textureViews.reserve(128);
	}

	VulkanCommandBuffer::VulkanCommandBuffer(
		VulkanCommandAllocator& allocator, VkCommandBuffer cmd, VulkanResourceTable* table)
		: m_cmd(cmd), m_table(table), m_allocator(&allocator)
	{
		if (m_cmd == VK_NULL_HANDLE || m_table == nullptr)
			throw std::runtime_error("VulkanCommandBuffer requires native storage and a resource table");
		m_buffers.reserve(kInitialTrackedBufferCapacity);
		m_textures.reserve(256);
		m_textureViews.reserve(128);
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
		clearTrackedBuffers();
		m_textures.clear();
		m_textureViews.clear();
		m_queryPoolCount = 0;
		m_residencySetCount = 0;
		m_submissionPlan = {};
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
	void VulkanCommandBuffer::prepareForSubmit(SubmissionToken token)
	{
		if (m_state != CommandBufferState::executable || m_allocator == nullptr ||
		    m_table == nullptr || !token.isValid())
			throw std::runtime_error("VulkanCommandBuffer submission state is invalid");
		if(m_allocator->queue().identity() != token.queue)
			throw std::runtime_error(
				"VulkanCommandBuffer submission queue does not own its allocator");
		if(!m_allocator->owns(*this))
			throw std::runtime_error(
				"VulkanCommandBuffer submission allocator does not own the command buffer");

		m_table->prepareSubmission(
			VulkanSubmissionRoots{
				.argumentTables = std::span<const ArgumentTableHandle>(
					m_argumentTables, m_argumentTableCount),
				.pipelines = std::span<const PipelineHandle>(m_pipelines, m_pipelineCount),
				.buffers = std::span<const BufferHandle>(m_buffers),
				.textures = std::span<const TextureHandle>(m_textures),
				.textureViews = std::span<const TextureViewHandle>(m_textureViews),
				.queryPools = std::span<const QueryPoolHandle>(
					m_queryPools, m_queryPoolCount),
				.residencySets = std::span<const ResidencySetHandle>(
					m_residencySets, m_residencySetCount),
			},
			token, m_submissionPlan);
	}

	void VulkanCommandBuffer::markSubmitted(SubmissionToken token) noexcept
	{
		if (m_state != CommandBufferState::executable || m_allocator == nullptr ||
		    m_table == nullptr || !m_submissionPlan.preparedFor(token))
			std::terminate();
		m_table->commitSubmission(m_submissionPlan);
		m_submission = token;
		m_state = CommandBufferState::submitted;
		m_allocator->noteSubmitted(token);
	}

	void VulkanCommandBuffer::markReusable()
	{
		if (m_state == CommandBufferState::recording)
			throw std::runtime_error("VulkanCommandBuffer cannot reset while recording");
		m_active = EncoderKind::none;
		m_submission = {};
		m_argumentTableCount = 0;
		m_pipelineCount = 0;
		clearTrackedBuffers();
		m_textures.clear();
		m_textureViews.clear();
		m_queryPoolCount = 0;
		m_residencySetCount = 0;
		m_submissionPlan = {};
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
		if(m_trackedBufferGenerations.size() <= buffer.index)
			m_trackedBufferGenerations.resize(
				static_cast<size_t>(buffer.index) + 1u, 0u);
		if(m_trackedBufferGenerations[buffer.index] == buffer.generation)
			return;
		m_buffers.push_back(buffer);
		m_trackedBufferGenerations[buffer.index] = buffer.generation;
	}

	void VulkanCommandBuffer::clearTrackedBuffers() noexcept
	{
		for(const BufferHandle buffer : m_buffers)
		{
			if(buffer.index < m_trackedBufferGenerations.size() &&
			   m_trackedBufferGenerations[buffer.index] == buffer.generation)
				m_trackedBufferGenerations[buffer.index] = 0u;
		}
		m_buffers.clear();
	}

	void VulkanCommandBuffer::trackTexture(TextureHandle texture)
	{
		if (std::find(m_textures.begin(), m_textures.end(), texture) == m_textures.end())
			m_textures.push_back(texture);
	}

	void VulkanCommandBuffer::trackTextureView(TextureViewHandle view)
	{
		if (std::find(m_textureViews.begin(), m_textureViews.end(), view) == m_textureViews.end())
			m_textureViews.push_back(view);
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
		clearTrackedBuffers();
		m_textures.clear();
		m_textureViews.clear();
		m_queryPoolCount = 0;
		m_residencySetCount = 0;
		m_submissionPlan = {};
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
			const TextureViewForRecording view = requireTextureViewForRecording(
				m_table, target.view, "Vulkan render-pass color target");
			if(!target.texture.isNull())
			{
				(void)requireTextureForRecording(
					m_table, target.texture, "Vulkan render-pass color target");
				if(target.texture != view.parent)
					throw std::runtime_error(
						"Vulkan render-pass color target texture/view parent mismatch");
			}
			colorAttachments[i] = VkRenderingAttachmentInfo{
				.sType = VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO,
				.imageView = view.native,
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
			const TextureViewForRecording view = requireTextureViewForRecording(
				m_table, desc.depthTarget->view, "Vulkan render-pass depth target");
			if(!desc.depthTarget->texture.isNull())
			{
				(void)requireTextureForRecording(
					m_table, desc.depthTarget->texture, "Vulkan render-pass depth target");
				if(desc.depthTarget->texture != view.parent)
					throw std::runtime_error(
						"Vulkan render-pass depth target texture/view parent mismatch");
			}
			depthAttachment = VkRenderingAttachmentInfo{
				.sType = VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO,
				.imageView = view.native,
				.imageLayout = toVkImageLayout(desc.depthTarget->state),
				.loadOp = static_cast<VkAttachmentLoadOp>(desc.depthTarget->loadOp),
				.storeOp = static_cast<VkAttachmentStoreOp>(desc.depthTarget->storeOp),
			};
			depthAttachment.clearValue.depthStencil = {
				desc.depthTarget->clearValue.depth, desc.depthTarget->clearValue.stencil
			};
		}

		for (const InputAttachmentDesc& input : desc.inputAttachments)
			(void)requireTextureViewForRecording(
				m_table, input.view, "Vulkan render-pass input attachment");

		const size_t depthCount = desc.depthTarget != nullptr ? 1u : 0u;
		m_textureViews.reserve(
			m_textureViews.size() + colorTargetCount + depthCount +
			desc.inputAttachments.size());
		m_textures.reserve(m_textures.size() + colorTargetCount + depthCount);

		for(uint32_t i = 0; i < colorTargetCount; ++i)
		{
			trackTextureView(desc.colorTargets[i].view);
			if(!desc.colorTargets[i].texture.isNull())
				trackTexture(desc.colorTargets[i].texture);
		}
		if(desc.depthTarget != nullptr)
		{
			trackTextureView(desc.depthTarget->view);
			if(!desc.depthTarget->texture.isNull())
				trackTexture(desc.depthTarget->texture);
		}
		for(const InputAttachmentDesc& input : desc.inputAttachments)
			trackTextureView(input.view);

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
		for(const TextureBarrier& barrier : textures)
			(void)requireTextureForRecording(
				m_table, barrier.texture, "Vulkan resource barrier");
		for(const BufferBarrier& barrier : buffers)
			(void)requireBufferForRecording(
				m_table, barrier.buffer, "Vulkan resource barrier");
		const auto requireAliasingResource = [this](const AliasingResource& resource) {
			switch(resource.kind)
			{
			case AliasingResourceKind::none:
				return;
			case AliasingResourceKind::buffer:
				(void)requireBufferForRecording(
					m_table, resource.buffer, "Vulkan aliasing barrier");
				return;
			case AliasingResourceKind::texture:
				(void)requireTextureForRecording(
					m_table, resource.texture, "Vulkan aliasing barrier");
				return;
			}
			throw std::runtime_error("Vulkan aliasing barrier received an invalid resource kind");
		};
		for(const AliasingBarrier& source : aliasing)
		{
			if(!source.before.isValid() || !source.after.isValid() ||
			   source.after.kind == AliasingResourceKind::none)
				throw std::runtime_error("Vulkan aliasing barrier received an invalid resource");
			requireAliasingResource(source.before);
			requireAliasingResource(source.after);
		}

		std::vector<TextureHandle> texturesToTrack;
		std::vector<BufferHandle> buffersToTrack;
		const auto appendUnique = [](auto& handles, auto handle) {
			if(std::find(handles.begin(), handles.end(), handle) == handles.end())
				handles.push_back(handle);
		};
		for(const TextureBarrier& source : textures)
			appendUnique(texturesToTrack, source.texture);
		for(const BufferBarrier& source : buffers)
			appendUnique(buffersToTrack, source.buffer);
		for(const AliasingBarrier& source : aliasing)
		{
			const auto appendAliasing = [&](const AliasingResource& resource) {
				if(resource.kind == AliasingResourceKind::buffer)
					appendUnique(buffersToTrack, resource.buffer);
				else if(resource.kind == AliasingResourceKind::texture)
					appendUnique(texturesToTrack, resource.texture);
			};
			appendAliasing(source.before);
			appendAliasing(source.after);
		}

		std::vector<VkImageMemoryBarrier2> imageBarriers(textureCount);
		for(uint32_t index = 0; index < textureCount; ++index)
		{
			const TextureBarrier& barrier = textures[index];
			imageBarriers[index] = VkImageMemoryBarrier2{
				.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER_2,
				.srcStageMask = VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT,
				.srcAccessMask = VK_ACCESS_2_MEMORY_WRITE_BIT,
				.dstStageMask = VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT,
				.dstAccessMask = VK_ACCESS_2_MEMORY_READ_BIT | VK_ACCESS_2_MEMORY_WRITE_BIT,
				.oldLayout = toVkImageLayout(barrier.before),
				.newLayout = toVkImageLayout(barrier.after),
				.image = requireTextureForRecording(
					m_table, barrier.texture, "Vulkan resource barrier"),
				.subresourceRange = {
					.aspectMask = toVkImageAspect(barrier.range.aspect),
					.baseMipLevel = barrier.range.baseMipLevel,
					.levelCount = barrier.range.levelCount,
					.baseArrayLayer = barrier.range.baseArrayLayer,
					.layerCount = barrier.range.layerCount,
				},
			};
		}
		std::vector<VkBufferMemoryBarrier2> bufferBarriers(bufferCount);
		for(uint32_t index = 0; index < bufferCount; ++index)
		{
			const BufferBarrier& barrier = buffers[index];
			bufferBarriers[index] = VkBufferMemoryBarrier2{
				.sType = VK_STRUCTURE_TYPE_BUFFER_MEMORY_BARRIER_2,
				.srcStageMask = VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT,
				.srcAccessMask = VK_ACCESS_2_MEMORY_WRITE_BIT,
				.dstStageMask = VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT,
				.dstAccessMask = VK_ACCESS_2_MEMORY_READ_BIT | VK_ACCESS_2_MEMORY_WRITE_BIT,
				.buffer = requireBufferForRecording(
					m_table, barrier.buffer, "Vulkan resource barrier"),
				.offset = barrier.offset,
				.size = barrier.size == 0 ? VK_WHOLE_SIZE : barrier.size,
			};
		}

		const auto countNew = [](const auto& tracked, const auto& requested) {
			return static_cast<size_t>(std::count_if(
				requested.begin(), requested.end(), [&](auto handle) {
					return std::find(tracked.begin(), tracked.end(), handle) == tracked.end();
				}));
		};
		m_textures.reserve(m_textures.size() + countNew(m_textures, texturesToTrack));
		m_buffers.reserve(m_buffers.size() + countNew(m_buffers, buffersToTrack));
#ifndef NDEBUG
		DebugResourceStateTracker pendingStates = m_resourceStates;
		for (uint32_t index = 0; index < textureCount; ++index)
		{
			if (!pendingStates.transition(textures[index]).valid())
				throw std::runtime_error("VulkanCommandBuffer rejected an invalid texture transition");
		}
		for (uint32_t index = 0; index < bufferCount; ++index)
		{
			if (!pendingStates.transition(buffers[index]).valid())
				throw std::runtime_error("VulkanCommandBuffer rejected an invalid buffer transition");
		}
		m_resourceStates = pendingStates;
#endif
		for(TextureHandle texture : texturesToTrack)
			trackTexture(texture);
		for(BufferHandle buffer : buffersToTrack)
			trackBuffer(buffer);
		uint32_t textureOffset = 0;
		uint32_t bufferOffset = 0;
		while (textureOffset < textureCount || bufferOffset < bufferCount)
		{
			const uint32_t textureRemaining = textureCount - textureOffset;
			const uint32_t bufferRemaining = bufferCount - bufferOffset;
			const uint32_t imageBatch = textureRemaining < kMaxBarrierBatchSize
				                            ? textureRemaining
				                            : kMaxBarrierBatchSize;
			const uint32_t bufferBatch = bufferRemaining < kMaxBarrierBatchSize
				                             ? bufferRemaining
				                             : kMaxBarrierBatchSize;
			const VkDependencyInfo dependencyInfo{
				.sType = VK_STRUCTURE_TYPE_DEPENDENCY_INFO,
				.bufferMemoryBarrierCount = bufferBatch,
				.pBufferMemoryBarriers = bufferBatch > 0
					? bufferBarriers.data() + bufferOffset : nullptr,
				.imageMemoryBarrierCount = imageBatch,
				.pImageMemoryBarriers = imageBatch > 0
					? imageBarriers.data() + textureOffset : nullptr,
			};
			vkCmdPipelineBarrier2(m_cmd, &dependencyInfo);

			textureOffset += imageBatch;
			bufferOffset += bufferBatch;
		}

		if (!aliasing.empty())
		{
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
		const VkImage nativeImage = requireTextureForRecording(
			m_table, texture, "Vulkan color clear");
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
		const QueryPoolRecord& record = requireQueryPoolForRecording(
			m_table, pool, "VulkanCommandBuffer::resetQueryPool");
		if (queryCount == 0)
		{
			if(firstQuery > record.count)
				throw std::runtime_error(
					"VulkanCommandBuffer::resetQueryPool range exceeds the pool");
			return;
		}
		if(firstQuery >= record.count || queryCount > record.count - firstQuery)
			throw std::runtime_error(
				"VulkanCommandBuffer::resetQueryPool range exceeds the pool");
		trackQueryPool(pool);
		vkCmdResetQueryPool(m_cmd, record.nativePool, firstQuery,
		                    queryCount);
	}

	void VulkanCommandBuffer::writeTimestamp(QueryPoolHandle pool, uint32_t queryIndex, bool afterAllCommands)
	{
		requireRecording("VulkanCommandBuffer::writeTimestamp");
		if (m_active != EncoderKind::none)
			throw std::runtime_error("VulkanCommandBuffer::writeTimestamp requires no active encoder");
		const QueryPoolRecord& record = requireQueryPoolForRecording(
			m_table, pool, "VulkanCommandBuffer::writeTimestamp");
		if(queryIndex >= record.count)
			throw std::runtime_error(
				"VulkanCommandBuffer::writeTimestamp index exceeds the pool");
		trackQueryPool(pool);
		vkCmdWriteTimestamp2(m_cmd,
		                     afterAllCommands
			                     ? VK_PIPELINE_STAGE_2_BOTTOM_OF_PIPE_BIT
			                     : VK_PIPELINE_STAGE_2_TOP_OF_PIPE_BIT,
		                     record.nativePool, queryIndex);
	}
} // namespace demo::rhi::vulkan
