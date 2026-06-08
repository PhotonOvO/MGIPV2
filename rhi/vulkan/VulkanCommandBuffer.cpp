#include "VulkanCommandBuffer.h"

#include "internal/VulkanCommon.h"
#include "VulkanResourceTable.h"

#include <array>
#include <cassert>
#include <vulkan/vulkan.h>

namespace demo::rhi::vulkan
{
	namespace
	{
		[[nodiscard]] VkPipelineStageFlags2 toVkPipelineStage2(StageFlags stages)
		{
			VkPipelineStageFlags2 out = VK_PIPELINE_STAGE_2_NONE;
			const auto has = [&](StageFlags bit)
			{
				return (static_cast<uint64_t>(stages) & static_cast<uint64_t>(bit)) != 0;
			};
			if (stages == StageFlags::all) return VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT;
			if (has(StageFlags::transfer)) out |= VK_PIPELINE_STAGE_2_ALL_TRANSFER_BIT;
			if (has(StageFlags::compute)) out |= VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT;
			if (has(StageFlags::vertexShader)) out |= VK_PIPELINE_STAGE_2_VERTEX_SHADER_BIT;
			if (has(StageFlags::fragmentShader)) out |= VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT;
			if (has(StageFlags::rasterColorOut)) out |= VK_PIPELINE_STAGE_2_COLOR_ATTACHMENT_OUTPUT_BIT;
			if (has(StageFlags::rasterDepthOut))
				out |= VK_PIPELINE_STAGE_2_EARLY_FRAGMENT_TESTS_BIT |
					VK_PIPELINE_STAGE_2_LATE_FRAGMENT_TESTS_BIT;
			if (has(StageFlags::commandInput)) out |= VK_PIPELINE_STAGE_2_DRAW_INDIRECT_BIT;
			return out == VK_PIPELINE_STAGE_2_NONE ? VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT : out;
		}

		// Conservative access masks; Wave 7 refines per-hazard. Correctness-first.
		[[nodiscard]] VkAccessFlags2 inferProducerAccess(HazardFlags hazards)
		{
			VkAccessFlags2 out = VK_ACCESS_2_MEMORY_WRITE_BIT;
			if ((static_cast<uint32_t>(hazards) & static_cast<uint32_t>(HazardFlags::depthStencil)) != 0)
				out |= VK_ACCESS_2_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT;
			return out;
		}

		[[nodiscard]] VkAccessFlags2 inferConsumerAccess(HazardFlags hazards)
		{
			VkAccessFlags2 out = VK_ACCESS_2_MEMORY_READ_BIT;
			if ((static_cast<uint32_t>(hazards) & static_cast<uint32_t>(HazardFlags::drawArguments)) != 0)
				out |= VK_ACCESS_2_INDIRECT_COMMAND_READ_BIT;
			return out;
		}

		[[nodiscard]] VkImageLayout toVkImageLayout(ResourceState state)
		{
			switch (state)
			{
			case ResourceState::ColorAttachment: return VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
			case ResourceState::DepthStencilAttachment: return VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL;
			case ResourceState::DepthStencilReadOnly: return VK_IMAGE_LAYOUT_DEPTH_STENCIL_READ_ONLY_OPTIMAL;
			case ResourceState::ShaderRead: return VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
			case ResourceState::ShaderWrite: return VK_IMAGE_LAYOUT_GENERAL;
			case ResourceState::TransferSrc: return VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL;
			case ResourceState::TransferDst: return VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
			case ResourceState::Present: return VK_IMAGE_LAYOUT_PRESENT_SRC_KHR;
			case ResourceState::General: return VK_IMAGE_LAYOUT_GENERAL;
			default: return VK_IMAGE_LAYOUT_UNDEFINED;
			}
		}

		[[nodiscard]] VkImageAspectFlags toVkAspect(TextureAspect aspect)
		{
			switch (aspect)
			{
			case TextureAspect::depth: return VK_IMAGE_ASPECT_DEPTH_BIT;
			case TextureAspect::depthStencil: return VK_IMAGE_ASPECT_DEPTH_BIT | VK_IMAGE_ASPECT_STENCIL_BIT;
			default: return VK_IMAGE_ASPECT_COLOR_BIT;
			}
		}

		[[nodiscard]] VkShaderStageFlags toVkShaderStageFlags(ShaderStage stages)
		{
			VkShaderStageFlags flags = 0;
			const auto has = [&](ShaderStage bit)
			{
				return (static_cast<uint32_t>(stages) & static_cast<uint32_t>(bit)) != 0;
			};
			if (has(ShaderStage::vertex)) flags |= VK_SHADER_STAGE_VERTEX_BIT;
			if (has(ShaderStage::fragment)) flags |= VK_SHADER_STAGE_FRAGMENT_BIT;
			if (has(ShaderStage::compute)) flags |= VK_SHADER_STAGE_COMPUTE_BIT;
			if (has(ShaderStage::geometry)) flags |= VK_SHADER_STAGE_GEOMETRY_BIT;
			if (has(ShaderStage::tessControl)) flags |= VK_SHADER_STAGE_TESSELLATION_CONTROL_BIT;
			if (has(ShaderStage::tessEval)) flags |= VK_SHADER_STAGE_TESSELLATION_EVALUATION_BIT;
			return flags == 0 ? VK_SHADER_STAGE_ALL : flags;
		}

		[[nodiscard]] VkBuffer asBuffer(uint64_t v) { return reinterpret_cast<VkBuffer>(static_cast<uintptr_t>(v)); }
		[[nodiscard]] VkImage asImage(uint64_t v) { return reinterpret_cast<VkImage>(static_cast<uintptr_t>(v)); }

		[[nodiscard]] uint32_t indirectStride(uint32_t requestedStride, uint32_t drawCount, uint32_t commandSize)
		{
			return drawCount > 1 && requestedStride < commandSize ? commandSize : requestedStride;
		}

		inline constexpr uint32_t kMaxColorAttachmentsPerPass = 8;
		inline constexpr uint32_t kMaxBarrierBatchSize = 16;
	} // namespace

	// ---------------------------------------------------------------------------
	// VulkanRenderEncoder
	// ---------------------------------------------------------------------------
	void VulkanRenderEncoder::prepare(VkCommandBuffer cmd, VulkanResourceTable* table)
	{
		m_cmd = cmd;
		m_table = table;
		m_layout = nullptr;
		for (uint32_t i = 0; i < kMaxArgumentSlots; ++i) m_pendingDynCount[i] = 0;
	}

	void VulkanRenderEncoder::setPipeline(PipelineHandle pipeline)
	{
		m_layout = reinterpret_cast<VkPipelineLayout>(static_cast<uintptr_t>(m_table->resolvePipelineLayout(pipeline)));
		vkCmdBindPipeline(m_cmd, VK_PIPELINE_BIND_POINT_GRAPHICS,
		                  reinterpret_cast<VkPipeline>(static_cast<uintptr_t>(m_table->resolvePipeline(pipeline, 0))));
	}

	void VulkanRenderEncoder::setArgumentTable(ShaderStage /*stages*/, uint32_t slot, ArgumentTableHandle table)
	{
		// Vulkan binds descriptor sets pipeline-wide; `stages` is for Metal4 per-stage tables.
		VkDescriptorSet set = reinterpret_cast<VkDescriptorSet>(static_cast<uintptr_t>(m_table->
			resolveArgumentTable(table)));
		const uint32_t dynCount = slot < kMaxArgumentSlots ? m_pendingDynCount[slot] : 0;
		const uint32_t* dynOffsets = dynCount > 0 ? m_pendingDynOffsets[slot] : nullptr;
		vkCmdBindDescriptorSets(m_cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, m_layout, slot, 1, &set, dynCount, dynOffsets);
		if (slot < kMaxArgumentSlots) m_pendingDynCount[slot] = 0;
	}

	void VulkanRenderEncoder::setDynamicBuffer(ShaderStage, uint32_t slot, BufferHandle, uint64_t offset, uint64_t)
	{
		// Dynamic offsets accumulate per slot in call order, matching the descriptor set's
		// binding order; flushed together at the next setArgumentTable(slot).
		if (slot < kMaxArgumentSlots && m_pendingDynCount[slot] < kMaxDynOffsetPerSlot)
		{
			m_pendingDynOffsets[slot][m_pendingDynCount[slot]++] = static_cast<uint32_t>(offset);
		}
	}

	void VulkanRenderEncoder::setRootConstants(ShaderStage stage, uint32_t slot, const void* data, uint32_t size)
	{
		vkCmdPushConstants(m_cmd, m_layout, toVkShaderStageFlags(stage), slot, size, data);
	}

	void VulkanRenderEncoder::setRootPointer(ShaderStage stage, uint32_t slot, GpuPtr ptr)
	{
		const uint64_t address = ptr.value;
		vkCmdPushConstants(m_cmd, m_layout, toVkShaderStageFlags(stage), slot, sizeof(address), &address);
	}

	void VulkanRenderEncoder::setViewport(const Viewport& viewport)
	{
		const VkViewport vp{
			viewport.x, viewport.y, viewport.width, viewport.height, viewport.minDepth, viewport.maxDepth
		};
		vkCmdSetViewportWithCount(m_cmd, 1, &vp);
	}

	void VulkanRenderEncoder::setScissor(const Rect2D& scissor)
	{
		const VkRect2D rect{{scissor.offset.x, scissor.offset.y}, {scissor.extent.width, scissor.extent.height}};
		vkCmdSetScissorWithCount(m_cmd, 1, &rect);
	}

	void VulkanRenderEncoder::bindVertexBuffers(uint32_t firstBinding, const BufferHandle* buffers,
	                                            const uint64_t* offsets, uint32_t count)
	{
		std::array<VkBuffer, 16> vkBuffers{};
		std::array<VkDeviceSize, 16> vkOffsets{};
		const uint32_t clamped = count < 16 ? count : 16;
		for (uint32_t i = 0; i < clamped; ++i)
		{
			vkBuffers[i] = asBuffer(m_table->resolveBuffer(buffers[i]));
			vkOffsets[i] = offsets != nullptr ? offsets[i] : 0;
		}
		vkCmdBindVertexBuffers(m_cmd, firstBinding, clamped, vkBuffers.data(), vkOffsets.data());
	}

	void VulkanRenderEncoder::bindIndexBuffer(BufferHandle buffer, uint64_t offset, IndexFormat format)
	{
		vkCmdBindIndexBuffer(m_cmd, asBuffer(m_table->resolveBuffer(buffer)), offset,
		                     format == IndexFormat::uint16 ? VK_INDEX_TYPE_UINT16 : VK_INDEX_TYPE_UINT32);
	}

	void VulkanRenderEncoder::readInputAttachment(uint32_t)
	{
		// Fallback path: input attachments are read via a sampled image binding today.
		// Real local-read maps to VK_KHR_dynamic_rendering_local_read in a later milestone.
	}

	void VulkanRenderEncoder::draw(const DrawDesc& desc)
	{
		vkCmdDraw(m_cmd, desc.vertexCount, desc.instanceCount, desc.firstVertex, desc.firstInstance);
	}

	void VulkanRenderEncoder::drawIndexed(const DrawIndexedDesc& desc)
	{
		if (!desc.indexBuffer.isNull())
		{
			bindIndexBuffer(desc.indexBuffer, desc.indexBufferOffset, desc.indexFormat);
		}
		vkCmdDrawIndexed(m_cmd, desc.indexCount, desc.instanceCount, desc.firstIndex, desc.vertexOffset,
		                 desc.firstInstance);
	}

	void VulkanRenderEncoder::drawIndexedIndirect(const DrawIndirectDesc& desc)
	{
		const uint32_t stride = indirectStride(desc.stride, desc.drawCount, sizeof(VkDrawIndexedIndirectCommand));
		vkCmdDrawIndexedIndirect(m_cmd, asBuffer(m_table->resolveBuffer(desc.argsBuffer)), desc.offset, desc.drawCount,
		                         stride);
	}

	void VulkanRenderEncoder::drawIndexedIndirectCount(const DrawIndirectCountDesc& desc)
	{
		const uint32_t stride = indirectStride(desc.stride, desc.maxDrawCount, sizeof(VkDrawIndexedIndirectCommand));
		vkCmdDrawIndexedIndirectCount(m_cmd, asBuffer(m_table->resolveBuffer(desc.argsBuffer)), desc.argsOffset,
		                              asBuffer(m_table->resolveBuffer(desc.countBuffer)), desc.countBufferOffset,
		                              desc.maxDrawCount, stride);
	}

	void VulkanRenderEncoder::drawIndirect(const DrawIndirectDesc& desc)
	{
		const uint32_t stride = indirectStride(desc.stride, desc.drawCount, sizeof(VkDrawIndirectCommand));
		vkCmdDrawIndirect(m_cmd, asBuffer(m_table->resolveBuffer(desc.argsBuffer)), desc.offset, desc.drawCount,
		                  stride);
	}

	void VulkanRenderEncoder::drawMeshTasks(uint32_t groupCountX, uint32_t groupCountY, uint32_t groupCountZ)
	{
		if (vkCmdDrawMeshTasksEXT != nullptr)
		{
			vkCmdDrawMeshTasksEXT(m_cmd, groupCountX, groupCountY, groupCountZ);
		}
	}

	void VulkanRenderEncoder::drawMeshTasksIndirect(const DrawIndirectDesc& desc)
	{
		if (vkCmdDrawMeshTasksIndirectEXT != nullptr)
		{
			const uint32_t stride = indirectStride(desc.stride, desc.drawCount,
			                                       sizeof(VkDrawMeshTasksIndirectCommandEXT));
			vkCmdDrawMeshTasksIndirectEXT(m_cmd, asBuffer(m_table->resolveBuffer(desc.argsBuffer)), desc.offset,
			                              desc.drawCount,
			                              stride);
		}
	}

	// ---------------------------------------------------------------------------
	// VulkanComputeEncoder
	// ---------------------------------------------------------------------------
	void VulkanComputeEncoder::prepare(VkCommandBuffer cmd, VulkanResourceTable* table)
	{
		m_cmd = cmd;
		m_table = table;
		m_layout = nullptr;
	}

	void VulkanComputeEncoder::setPipeline(PipelineHandle pipeline)
	{
		m_layout = reinterpret_cast<VkPipelineLayout>(static_cast<uintptr_t>(m_table->resolvePipelineLayout(pipeline)));
		vkCmdBindPipeline(m_cmd, VK_PIPELINE_BIND_POINT_COMPUTE,
		                  reinterpret_cast<VkPipeline>(static_cast<uintptr_t>(m_table->resolvePipeline(pipeline, 1))));
	}

	void VulkanComputeEncoder::setArgumentTable(uint32_t slot, ArgumentTableHandle table)
	{
		VkDescriptorSet set = reinterpret_cast<VkDescriptorSet>(static_cast<uintptr_t>(m_table->
			resolveArgumentTable(table)));
		vkCmdBindDescriptorSets(m_cmd, VK_PIPELINE_BIND_POINT_COMPUTE, m_layout, slot, 1, &set, 0, nullptr);
	}

	void VulkanComputeEncoder::setRootConstants(uint32_t slot, const void* data, uint32_t size)
	{
		vkCmdPushConstants(m_cmd, m_layout, VK_SHADER_STAGE_COMPUTE_BIT, slot, size, data);
	}

	void VulkanComputeEncoder::setRootPointer(uint32_t slot, GpuPtr ptr)
	{
		const uint64_t address = ptr.value;
		vkCmdPushConstants(m_cmd, m_layout, VK_SHADER_STAGE_COMPUTE_BIT, slot, sizeof(address), &address);
	}

	void VulkanComputeEncoder::dispatch(const DispatchDesc& desc)
	{
		vkCmdDispatch(m_cmd, desc.groupCountX, desc.groupCountY, desc.groupCountZ);
	}

	void VulkanComputeEncoder::dispatchIndirect(const DispatchIndirectDesc& desc)
	{
		vkCmdDispatchIndirect(m_cmd, asBuffer(m_table->resolveBuffer(desc.argsBuffer)), desc.offset);
	}

	// ---------------------------------------------------------------------------
	// VulkanComputeEncoder: copy / blit command subset (Metal 4-aligned)
	// ---------------------------------------------------------------------------
	void VulkanComputeEncoder::copyBuffer(BufferHandle src, uint64_t srcOffset, BufferHandle dst, uint64_t dstOffset,
	                                      uint64_t size)
	{
		const VkBufferCopy region{srcOffset, dstOffset, size};
		vkCmdCopyBuffer(m_cmd, asBuffer(m_table->resolveBuffer(src)), asBuffer(m_table->resolveBuffer(dst)), 1,
		                &region);
	}

	void VulkanComputeEncoder::copyBufferToTexture(const BufferTextureCopyDesc& desc)
	{
		const VkBufferImageCopy region{
			.bufferOffset = desc.bufferOffset,
			.imageSubresource = {
				.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT,
				.mipLevel = desc.mipLevel,
				.baseArrayLayer = desc.baseArrayLayer,
				.layerCount = desc.layerCount
			},
			.imageExtent = {desc.width, desc.height, desc.depth},
		};
		vkCmdCopyBufferToImage(m_cmd, asBuffer(m_table->resolveBuffer(desc.buffer)),
		                       asImage(m_table->resolveTexture(desc.texture)),
		                       VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &region);
	}

	void VulkanComputeEncoder::copyTextureToBuffer(const BufferTextureCopyDesc& desc)
	{
		const VkBufferImageCopy region{
			.bufferOffset = desc.bufferOffset,
			.imageSubresource = {
				.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT,
				.mipLevel = desc.mipLevel,
				.baseArrayLayer = desc.baseArrayLayer,
				.layerCount = desc.layerCount
			},
			.imageExtent = {desc.width, desc.height, desc.depth},
		};
		vkCmdCopyImageToBuffer(m_cmd, asImage(m_table->resolveTexture(desc.texture)),
		                       VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
		                       asBuffer(m_table->resolveBuffer(desc.buffer)), 1, &region);
	}

	void VulkanComputeEncoder::blitTexture(const TextureBlitDesc& desc)
	{
		VkImageBlit region{
			.srcSubresource = {
				.aspectMask = toVkAspect(desc.aspect),
				.mipLevel = desc.srcMipLevel,
				.baseArrayLayer = desc.srcBaseArrayLayer,
				.layerCount = desc.layerCount
			},
			.dstSubresource = {
				.aspectMask = toVkAspect(desc.aspect),
				.mipLevel = desc.dstMipLevel,
				.baseArrayLayer = desc.dstBaseArrayLayer,
				.layerCount = desc.layerCount
			},
		};
		region.srcOffsets[0] = {desc.srcOffsets[0].x, desc.srcOffsets[0].y, desc.srcOffsets[0].z};
		region.srcOffsets[1] = {desc.srcOffsets[1].x, desc.srcOffsets[1].y, desc.srcOffsets[1].z};
		region.dstOffsets[0] = {desc.dstOffsets[0].x, desc.dstOffsets[0].y, desc.dstOffsets[0].z};
		region.dstOffsets[1] = {desc.dstOffsets[1].x, desc.dstOffsets[1].y, desc.dstOffsets[1].z};
		vkCmdBlitImage(m_cmd, asImage(m_table->resolveTexture(desc.srcTexture)), VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
		               asImage(m_table->resolveTexture(desc.dstTexture)), VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1,
		               &region,
		               VK_FILTER_LINEAR);
	}

	void VulkanComputeEncoder::fillBuffer(BufferHandle buffer, uint64_t offset, uint64_t size, uint32_t data)
	{
		vkCmdFillBuffer(m_cmd, asBuffer(m_table->resolveBuffer(buffer)), offset, size == 0 ? VK_WHOLE_SIZE : size,
		                data);
	}

	// ---------------------------------------------------------------------------
	// VulkanCommandBuffer
	// ---------------------------------------------------------------------------
	void VulkanCommandBuffer::setTarget(VkCommandBuffer cmd, VulkanResourceTable* table)
	{
		m_cmd = cmd;
		m_table = table;
		m_active = EncoderKind::none;
	}

	RenderEncoder* VulkanCommandBuffer::beginRenderPass(const RenderPassDesc& desc)
	{
		assert(desc.colorTargetCount <= kMaxColorAttachmentsPerPass);
		std::array<VkRenderingAttachmentInfo, kMaxColorAttachmentsPerPass> colorAttachments{};
		const uint32_t colorTargetCount = desc.colorTargetCount <= kMaxColorAttachmentsPerPass
			                                  ? desc.colorTargetCount
			                                  : kMaxColorAttachmentsPerPass;
		for (uint32_t i = 0; i < colorTargetCount; ++i)
		{
			const RenderTargetDesc& target = desc.colorTargets[i];
			colorAttachments[i] = VkRenderingAttachmentInfo{
				.sType = VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO,
				.imageView = reinterpret_cast<VkImageView>(static_cast<uintptr_t>(m_table->resolveTextureView(
					target.view))),
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
			depthAttachment = VkRenderingAttachmentInfo{
				.sType = VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO,
				.imageView = reinterpret_cast<VkImageView>(static_cast<uintptr_t>(m_table->resolveTextureView(
					desc.depthTarget->view))),
				.imageLayout = toVkImageLayout(desc.depthTarget->state),
				.loadOp = static_cast<VkAttachmentLoadOp>(desc.depthTarget->loadOp),
				.storeOp = static_cast<VkAttachmentStoreOp>(desc.depthTarget->storeOp),
			};
			depthAttachment.clearValue.depthStencil = {
				desc.depthTarget->clearValue.depth, desc.depthTarget->clearValue.stencil
			};
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

		m_renderEncoder.prepare(m_cmd, m_table);
		m_active = EncoderKind::render;
		return &m_renderEncoder;
	}

	ComputeEncoder* VulkanCommandBuffer::beginComputePass()
	{
		m_computeEncoder.prepare(m_cmd, m_table);
		m_active = EncoderKind::compute;
		return &m_computeEncoder;
	}

	void VulkanCommandBuffer::endEncoding()
	{
		if (m_active == EncoderKind::render)
		{
			vkCmdEndRendering(m_cmd);
		}
		m_active = EncoderKind::none;
	}

	void VulkanCommandBuffer::barrier(StageFlags producer, StageFlags consumer, HazardFlags hazards)
	{
		VkPipelineStageFlags2 dstStage = toVkPipelineStage2(consumer);
		// INDIRECT_COMMAND_READ access is only valid alongside the DRAW_INDIRECT stage, so
		// when the consumer reads indirect arguments, ensure that stage is present even if
		// the declarative consumer mask only named shader stages.
		if ((static_cast<uint32_t>(hazards) & static_cast<uint32_t>(HazardFlags::drawArguments)) != 0)
		{
			dstStage |= VK_PIPELINE_STAGE_2_DRAW_INDIRECT_BIT;
		}
		const VkMemoryBarrier2 memoryBarrier{
			.sType = VK_STRUCTURE_TYPE_MEMORY_BARRIER_2,
			.srcStageMask = toVkPipelineStage2(producer),
			.srcAccessMask = inferProducerAccess(hazards),
			.dstStageMask = dstStage,
			.dstAccessMask = inferConsumerAccess(hazards),
		};
		const VkDependencyInfo dependencyInfo{
			.sType = VK_STRUCTURE_TYPE_DEPENDENCY_INFO,
			.memoryBarrierCount = 1,
			.pMemoryBarriers = &memoryBarrier,
		};
		vkCmdPipelineBarrier2(m_cmd, &dependencyInfo);
	}

	void VulkanCommandBuffer::resourceBarrier(const TextureBarrier* textures, uint32_t textureCount,
	                                          const BufferBarrier* buffers, uint32_t bufferCount)
	{
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
				const uint64_t nativeImage = m_table->resolveTexture(b.texture);
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
					.image = asImage(nativeImage),
					.subresourceRange = {
						.aspectMask = toVkAspect(b.range.aspect),
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
				// Same-queue v1 behavior: srcQueue/dstQueue are retained in the public RHI
				// barrier but do not lower to queue-family ownership transfers yet.
				bufferBarriers[i] = VkBufferMemoryBarrier2{
					.sType = VK_STRUCTURE_TYPE_BUFFER_MEMORY_BARRIER_2,
					.srcStageMask = VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT,
					.srcAccessMask = VK_ACCESS_2_MEMORY_WRITE_BIT,
					.dstStageMask = VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT,
					.dstAccessMask = VK_ACCESS_2_MEMORY_READ_BIT | VK_ACCESS_2_MEMORY_WRITE_BIT,
					.buffer = asBuffer(m_table->resolveBuffer(b.buffer)),
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
	}

	void VulkanCommandBuffer::clearColorTexture(TextureHandle texture,
	                                            const TextureSubresourceRange& range,
	                                            const ClearColorValue& clearColor)
	{
		const VkClearColorValue value{{clearColor.r, clearColor.g, clearColor.b, clearColor.a}};
		const VkImageSubresourceRange vkRange{
			.aspectMask = toVkAspect(range.aspect),
			.baseMipLevel = range.baseMipLevel,
			.levelCount = range.levelCount,
			.baseArrayLayer = range.baseArrayLayer,
			.layerCount = range.layerCount,
		};
		vkCmdClearColorImage(m_cmd, asImage(m_table->resolveTexture(texture)), VK_IMAGE_LAYOUT_GENERAL, &value, 1,
		                     &vkRange);
	}

	void VulkanCommandBuffer::beginEvent(const char* name)
	{
		if (vkCmdBeginDebugUtilsLabelEXT != nullptr)
		{
			const VkDebugUtilsLabelEXT label{.sType = VK_STRUCTURE_TYPE_DEBUG_UTILS_LABEL_EXT, .pLabelName = name};
			vkCmdBeginDebugUtilsLabelEXT(m_cmd, &label);
		}
	}

	void VulkanCommandBuffer::endEvent()
	{
		if (vkCmdEndDebugUtilsLabelEXT != nullptr)
		{
			vkCmdEndDebugUtilsLabelEXT(m_cmd);
		}
	}

	void VulkanCommandBuffer::resetQueryPool(QueryPoolHandle pool, uint32_t firstQuery, uint32_t queryCount)
	{
		const uint64_t nativePool = m_table != nullptr ? m_table->resolveQueryPool(pool) : 0;
		if (nativePool == 0 || queryCount == 0)
		{
			return;
		}
		vkCmdResetQueryPool(m_cmd, reinterpret_cast<VkQueryPool>(static_cast<uintptr_t>(nativePool)), firstQuery,
		                    queryCount);
	}

	void VulkanCommandBuffer::writeTimestamp(QueryPoolHandle pool, uint32_t queryIndex, bool afterAllCommands)
	{
		const uint64_t nativePool = m_table != nullptr ? m_table->resolveQueryPool(pool) : 0;
		if (nativePool == 0)
		{
			return;
		}
		vkCmdWriteTimestamp2(m_cmd,
		                     afterAllCommands
			                     ? VK_PIPELINE_STAGE_2_BOTTOM_OF_PIPE_BIT
			                     : VK_PIPELINE_STAGE_2_TOP_OF_PIPE_BIT,
		                     reinterpret_cast<VkQueryPool>(static_cast<uintptr_t>(nativePool)), queryIndex);
	}
} // namespace demo::rhi::vulkan
