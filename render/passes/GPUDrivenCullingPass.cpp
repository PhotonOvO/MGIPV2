#include "GPUDrivenCullingPass.h"

#include "../GPUDrivenRenderer.h"
#include "../../shaders/shader_io.h"

#include <array>

namespace demo
{
	GPUDrivenCullingPass::GPUDrivenCullingPass(GPUDrivenRenderer* renderer)
		: m_renderer(renderer)
	{
	}

	PassNode::HandleSlice<PassResourceDependency> GPUDrivenCullingPass::getDependencies() const
	{
		static const std::array<PassResourceDependency, 6> dependencies = {
			// hiz-depth-pyramid is a storage image written by compute, so Vulkan requires it to
			// stay in GENERAL for both the write and the sampled read.
			PassResourceDependency::texture(kPassDepthPyramidHandle, ResourceAccess::read, rhi::ShaderStage::compute,
			                                rhi::ResourceState::General),
			PassResourceDependency::buffer(kPassGPUCullObjectBufferHandle, ResourceAccess::read,
			                               rhi::ShaderStage::compute),
			PassResourceDependency::buffer(kPassGPUCullIndirectBufferHandle, ResourceAccess::write,
			                               rhi::ShaderStage::compute),
			PassResourceDependency::buffer(kPassGPUCullStatsBufferHandle, ResourceAccess::write,
			                               rhi::ShaderStage::compute),
			PassResourceDependency::buffer(kPassGPUCullUniformBufferHandle, ResourceAccess::read,
			                               rhi::ShaderStage::compute),
			PassResourceDependency::buffer(kPassGPUCullResultBufferHandle, ResourceAccess::write,
			                               rhi::ShaderStage::compute),
		};
		return {dependencies.data(), static_cast<uint32_t>(dependencies.size())};
	}

	void GPUDrivenCullingPass::execute(const PassContext& context) const
	{
		if (m_renderer == nullptr || context.commandBuffer == nullptr || context.params == nullptr)
		{
			return;
		}

		rhi::CommandBuffer* cmdBuffer = context.commandBuffer;
		cmdBuffer->beginEvent("GPUDrivenCulling");

		const RenderParams& params = *context.params;
		const uint32_t safeObjectCount = m_renderer->getSafePersistentObjectCount();
		const bool useExternalPersistentObjects = params.gpuDrivenSceneView != nullptr
			&& params.gpuDrivenSceneView->usePersistentCullingObjects
			&& params.gpuDrivenSceneView->gpuCullObjectBuffer.isValid()
			&& safeObjectCount > 0u;

		if (params.cameraUniforms != nullptr && !m_renderer->getGPUCullingPipelineHandle().isNull())
		{
			const uint32_t currentFrameIndex = context.frameIndex;
			const uint32_t objectCount =
				useExternalPersistentObjects
					? safeObjectCount
					: (params.gltfModel != nullptr ? static_cast<uint32_t>(params.gltfModel->meshes.size()) : 0u);
			const rhi::ArgumentTableHandle argumentTable = m_renderer->getGPUCullingArgumentTable(currentFrameIndex);
			const rhi::BufferHandle indirectBuffer = m_renderer->getGPUCullingIndirectBufferRHIHandle(currentFrameIndex);
			const rhi::BufferHandle drawCountBuffer = m_renderer->getGPUCullingDrawCountBufferRHIHandle(currentFrameIndex);
			if (objectCount != 0u && !argumentTable.isNull() && !indirectBuffer.isNull() && !drawCountBuffer.isNull())
			{
				rhi::ComputeEncoder* enc = cmdBuffer->beginComputePass();
				enc->setPipeline(m_renderer->getGPUCullingPipelineHandle());
				enc->setArgumentTable(0, argumentTable);
				enc->dispatch(rhi::DispatchDesc{
					.groupCountX = (objectCount + shaderio::LGPUCullingThreadCount - 1u) /
					shaderio::LGPUCullingThreadCount,
					.groupCountY = 1u,
					.groupCountZ = 1u
				});
				cmdBuffer->endEncoding();

				// Culling writes category-compacted indirect args/counts. Opaque and alpha
				// consume them directly as command input; transparent draws may first read
				// them from the visibility-patch compute shader.
				cmdBuffer->barrier(rhi::StageFlags::compute,
				                   rhi::StageFlags::compute | rhi::StageFlags::commandInput,
				                   rhi::HazardFlags::drawArguments | rhi::HazardFlags::bufferWrites);

				// Snapshot the culling outputs for CPU readback. The compute writes above are made
				// visible to transfer reads, then the device-local buffers are copied to gpuToCpu staging.
				// The stats/draw-count copies are unconditional (small). The result copy is always
				// recorded because the overlay may be enabled on any frame.
				const rhi::BufferHandle statsBuffer = m_renderer->getGPUCullingStatsBufferRHIHandle(currentFrameIndex);
				const rhi::BufferHandle statsStaging = m_renderer->getGPUCullingStatsStagingHandle(currentFrameIndex);
				const rhi::BufferHandle resultBuffer = m_renderer->getGPUCullingResultBufferRHIHandle(currentFrameIndex);
				const rhi::BufferHandle resultStaging = m_renderer->getGPUCullingResultStagingHandle(currentFrameIndex);
				const rhi::BufferHandle drawCountStaging = m_renderer->getGPUCullingDrawCountStagingHandle(currentFrameIndex);
				if (!statsBuffer.isNull() && !statsStaging.isNull() && !resultBuffer.isNull()
					&& !resultStaging.isNull() && !drawCountStaging.isNull())
				{
					const uint32_t resultCount = m_renderer->getGPUCullingResultCount(currentFrameIndex);
					cmdBuffer->barrier(rhi::StageFlags::compute, rhi::StageFlags::transfer,
					                   rhi::HazardFlags::bufferWrites);
					const std::array<rhi::BufferBarrier, 3> toTransferSrc{
						rhi::BufferBarrier{.buffer = statsBuffer, .before = rhi::ResourceState::ShaderWrite,
						                   .after = rhi::ResourceState::TransferSrc},
						rhi::BufferBarrier{.buffer = drawCountBuffer, .before = rhi::ResourceState::ShaderWrite,
						                   .after = rhi::ResourceState::TransferSrc},
						rhi::BufferBarrier{.buffer = resultBuffer, .before = rhi::ResourceState::ShaderWrite,
						                   .after = rhi::ResourceState::TransferSrc},
					};
					cmdBuffer->resourceBarrier({}, toTransferSrc);
					rhi::ComputeEncoder* copy = cmdBuffer->beginComputePass();
					copy->copyBuffer(statsBuffer, 0u, statsStaging, 0u, sizeof(shaderio::GPUCullStats));
					copy->copyBuffer(drawCountBuffer, 0u, drawCountStaging, 0u, sizeof(shaderio::GPUCullDrawCounts));
					if (resultCount != 0u)
					{
						copy->copyBuffer(resultBuffer, 0u, resultStaging, 0u, sizeof(uint32_t) * resultCount);
					}
					cmdBuffer->endEncoding();
				}
			}
		}

		cmdBuffer->endEvent();
	}
} // namespace demo
