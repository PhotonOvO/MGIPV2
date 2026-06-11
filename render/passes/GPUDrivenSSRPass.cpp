#include "GPUDrivenSSRPass.h"

#include "../ArgumentTables.h"
#include "../GPUDrivenRenderer.h"
#include "../../shaders/shader_io.h"

#include <algorithm>
#include <array>

namespace demo
{
	GPUDrivenSSRPass::GPUDrivenSSRPass(GPUDrivenRenderer* renderer)
		: m_renderer(renderer)
	{
	}

	PassNode::HandleSlice<PassResourceDependency> GPUDrivenSSRPass::getDependencies() const
	{
		// The four sampled inputs are bound as sampled-image descriptors (SHADER_READ_ONLY layout)
		// in acquireSSRTempArgumentTable; the dependency state must say ShaderRead explicitly,
		// otherwise PassExecutor defaults unspecified read states to General and the descriptor
		// layout no longer matches the actual image layout at dispatch time.  The depth pyramid
		// (Hi-Z march input) is storage-written and stays in GENERAL, so its state is left
		// unspecified on purpose -- the default General matches its readWrite descriptor intent.
		static const std::array<PassResourceDependency, 5> dependencies = {
			PassResourceDependency::texture(kPassSceneColorHistoryReadHandle, ResourceAccess::read,
			                                rhi::ShaderStage::compute, rhi::ResourceState::ShaderRead),
			PassResourceDependency::texture(kPassGBuffer0Handle, ResourceAccess::read, rhi::ShaderStage::compute,
			                                rhi::ResourceState::ShaderRead),
			PassResourceDependency::texture(kPassSceneDepthHandle, ResourceAccess::read, rhi::ShaderStage::compute,
			                                rhi::ResourceState::ShaderRead),
			PassResourceDependency::texture(kPassGBuffer1Handle, ResourceAccess::read, rhi::ShaderStage::compute,
			                                rhi::ResourceState::ShaderRead),
			PassResourceDependency::texture(kPassDepthPyramidHandle, ResourceAccess::read, rhi::ShaderStage::compute),
		};
		return {dependencies.data(), static_cast<uint32_t>(dependencies.size())};
	}

	void GPUDrivenSSRPass::execute(const PassContext& context) const
	{
		if (m_renderer == nullptr || context.commandBuffer == nullptr || context.params == nullptr || context.
			transientAllocator == nullptr
			|| !context.cameraAllocValid)
		{
			return;
		}

		const rhi::BufferHandle cameraBufferHandle = context.transientAllocator->getBufferHandle();
		const uint64_t cameraBuffer = (static_cast<uint64_t>(cameraBufferHandle.generation) << 32u) | cameraBufferHandle
			.index;
		const uint32_t cameraOffset = context.cameraAlloc.offset;
		if (!context.params->debugOptions.enableSSR || m_renderer->getSSRTracePipelineOpaque() == 0
			|| m_renderer->getSSRRawImageOpaque() == 0 || cameraBuffer == 0)
		{
			return;
		}

		const PipelineHandle ssrPipeline = m_renderer->getSSRTracePipelineHandle();
		// Build this frame's SSR descriptor set (gbuffer/depth/history + ssrRaw + camera) as a
		// temporary bind group; it is recycled automatically at frame end.
		const rhi::ArgumentTableHandle ssrTable = m_renderer->acquireSSRTempArgumentTable(cameraBuffer, cameraOffset);
		if (ssrTable.isNull() || ssrPipeline.isNull())
		{
			return;
		}

		const rhi::Extent2D halfExtent = m_renderer->getPhase7HalfExtent();
		const shaderio::GPUDrivenSSRPushConstants push{
			.params0 = glm::vec4(halfExtent.width,
			                     halfExtent.height,
			                     static_cast<float>(std::max(1, context.params->debugOptions.ssrMaxSteps)),
			                     context.params->debugOptions.ssrThickness),
			.params1 = glm::vec4(0.05f, 80.0f, 1.0f, static_cast<float>(context.frameIndex % 64u)),
		};

		const uint32_t groupsX = (halfExtent.width + 7u) / 8u;
		const uint32_t groupsY = (halfExtent.height + 7u) / 8u;

		rhi::ComputeEncoder* enc = context.commandBuffer->beginComputePass();
		enc->setPipeline(ssrPipeline);
		enc->setArgumentTable(0, ssrTable);
		enc->setRootConstants(kPrimaryRootConstantsSlot, &push, sizeof(push));
		enc->dispatch(rhi::DispatchDesc{groupsX, groupsY, 1u});
		context.commandBuffer->endEncoding();

		// Same-pass barrier: the SSR trace dispatch writes the raw SSR texture consumed by
		// the denoise dispatch below (and by lighting when denoise is unavailable); no pass
		// boundary exists for PassExecutor to model.
		context.commandBuffer->barrier(rhi::StageFlags::compute,
		                               rhi::StageFlags::compute | rhi::StageFlags::fragmentShader,
		                               rhi::HazardFlags::textureWrites);

		const PipelineHandle ssrDenoisePipeline = m_renderer->getSSRDenoisePipelineHandle();
		const rhi::ArgumentTableHandle ssrDenoiseTable = m_renderer->getSSRDenoiseArgumentTable(context.frameIndex);
		if (ssrDenoisePipeline.isNull() || ssrDenoiseTable.isNull()
			|| m_renderer->getSSRDenoisedImageOpaque() == 0)
		{
			return;
		}

		rhi::ComputeEncoder* denoise = context.commandBuffer->beginComputePass();
		denoise->setPipeline(ssrDenoisePipeline);
		denoise->setArgumentTable(0, ssrDenoiseTable);
		denoise->setRootConstants(kPrimaryRootConstantsSlot, &push, sizeof(push));
		denoise->dispatch(rhi::DispatchDesc{groupsX, groupsY, 1u});
		context.commandBuffer->endEncoding();
		// Local output barrier: SSR denoise writes a renderer-private texture sampled by
		// later lighting; it has no PassResourceDependency handle in the graph yet.
		context.commandBuffer->barrier(rhi::StageFlags::compute,
		                               rhi::StageFlags::compute | rhi::StageFlags::fragmentShader,
		                               rhi::HazardFlags::textureWrites);
	}
} // namespace demo
