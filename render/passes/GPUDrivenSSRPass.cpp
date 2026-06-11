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
		// All four inputs are bound as sampled-image descriptors (SHADER_READ_ONLY layout)
		// in acquireSSRTempArgumentTable; the dependency state must say ShaderRead explicitly,
		// otherwise PassExecutor defaults unspecified read states to General and the descriptor
		// layout no longer matches the actual image layout at dispatch time.
		static const std::array<PassResourceDependency, 4> dependencies = {
			PassResourceDependency::texture(kPassSceneColorHistoryReadHandle, ResourceAccess::read,
			                                rhi::ShaderStage::compute, rhi::ResourceState::ShaderRead),
			PassResourceDependency::texture(kPassGBuffer0Handle, ResourceAccess::read, rhi::ShaderStage::compute,
			                                rhi::ResourceState::ShaderRead),
			PassResourceDependency::texture(kPassSceneDepthHandle, ResourceAccess::read, rhi::ShaderStage::compute,
			                                rhi::ResourceState::ShaderRead),
			PassResourceDependency::texture(kPassGBuffer1Handle, ResourceAccess::read, rhi::ShaderStage::compute,
			                                rhi::ResourceState::ShaderRead),
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

		rhi::ComputeEncoder* enc = context.commandBuffer->beginComputePass();
		enc->setPipeline(ssrPipeline);
		enc->setArgumentTable(0, ssrTable);
		enc->setRootConstants(kPrimaryRootConstantsSlot, &push, sizeof(push));
		enc->dispatch(rhi::DispatchDesc{(halfExtent.width + 7u) / 8u, (halfExtent.height + 7u) / 8u, 1u});
		context.commandBuffer->endEncoding();

		// Local output barrier: SSR trace writes a renderer-private texture sampled by
		// later lighting; it has no PassResourceDependency handle in the graph yet.
		context.commandBuffer->barrier(rhi::StageFlags::compute, rhi::StageFlags::fragmentShader,
		                               rhi::HazardFlags::textureWrites);
	}
} // namespace demo
