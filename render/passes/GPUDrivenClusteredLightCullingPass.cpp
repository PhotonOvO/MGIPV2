#include "GPUDrivenClusteredLightCullingPass.h"

#include "../GPUDrivenRenderer.h"
#include "../../shaders/shader_io.h"

#include <array>

namespace demo
{
	GPUDrivenClusteredLightCullingPass::GPUDrivenClusteredLightCullingPass(GPUDrivenRenderer* renderer)
		: m_renderer(renderer)
	{
	}

	PassNode::HandleSlice<PassResourceDependency> GPUDrivenClusteredLightCullingPass::getDependencies() const
	{
		static const std::array<PassResourceDependency, 9> dependencies = {
			PassResourceDependency::buffer(kPassPointLightBufferHandle, ResourceAccess::read,
			                               rhi::ShaderStage::compute),
			PassResourceDependency::buffer(kPassSpotLightBufferHandle, ResourceAccess::read, rhi::ShaderStage::compute),
			PassResourceDependency::buffer(kPassPointLightCoarseBoundsHandle, ResourceAccess::read,
			                               rhi::ShaderStage::compute),
			PassResourceDependency::buffer(kPassSpotLightCoarseBoundsHandle, ResourceAccess::read,
			                               rhi::ShaderStage::compute),
			PassResourceDependency::buffer(kPassLightCoarseCullingUniformHandle, ResourceAccess::read,
			                               rhi::ShaderStage::compute),
			PassResourceDependency::buffer(kPassClusteredLightUniformHandle, ResourceAccess::read,
			                               rhi::ShaderStage::compute),
			PassResourceDependency::buffer(kPassClusterLightCountsHandle, ResourceAccess::write,
			                               rhi::ShaderStage::compute),
			PassResourceDependency::buffer(kPassClusterLightIndicesHandle, ResourceAccess::write,
			                               rhi::ShaderStage::compute),
			PassResourceDependency::buffer(kPassClusterLightStatsHandle, ResourceAccess::write,
			                               rhi::ShaderStage::compute),
		};
		return {dependencies.data(), static_cast<uint32_t>(dependencies.size())};
	}

	void GPUDrivenClusteredLightCullingPass::execute(const PassContext& context) const
	{
		if (m_renderer == nullptr || context.commandBuffer == nullptr || context.params == nullptr)
		{
			return;
		}

		context.commandBuffer->beginEvent("GPUDrivenClusteredLightCulling");

		const PipelineHandle pipeline = m_renderer->getClusteredLightCullingPipelineHandle();
		const rhi::ArgumentTableHandle argumentTable = m_renderer->getLightCoarseCullingArgumentTable(
			context.frameIndex);
		if (context.params->debugOptions.enableClusteredLighting && !pipeline.isNull() && !argumentTable.isNull())
		{
			const bool collectStats =
				(m_renderer->getTemporalFrameCounter() % shaderio::LClusterStatsPeriod) == 0u;
			const rhi::BufferHandle statsBuffer = m_renderer->getClusterStatsBufferHandle(context.frameIndex);
			if (collectStats && !statsBuffer.isNull())
			{
				rhi::ComputeEncoder* clear = context.commandBuffer->beginComputePass();
				clear->fillBuffer(statsBuffer, 0, sizeof(GPUDrivenLightResources::ClusterStats), 0u);
				context.commandBuffer->endEncoding();
				// Same-pass barrier: fillBuffer resets cluster stats, then the culling
				// dispatch accumulates into the same buffer below.
				context.commandBuffer->barrier(rhi::StageFlags::transfer, rhi::StageFlags::compute,
				                               rhi::HazardFlags::bufferWrites);
			}

			rhi::ComputeEncoder* enc = context.commandBuffer->beginComputePass();
			enc->setPipeline(pipeline);
			enc->setArgumentTable(0, argumentTable);
			enc->dispatch(rhi::DispatchDesc{(shaderio::LClusterCount + 63u) / 64u, 1u, 1u});
			context.commandBuffer->endEncoding();

			// Local output barrier: transfer-cleared stats and compute-written light-list/stats
			// buffers feed fragment lighting, while stats also feed the transfer readback below.
			context.commandBuffer->barrier(rhi::StageFlags::transfer | rhi::StageFlags::compute,
			                               rhi::StageFlags::fragmentShader | rhi::StageFlags::transfer,
			                               rhi::HazardFlags::bufferWrites);

			const rhi::BufferHandle statsReadback =
				m_renderer->getClusterStatsReadbackBufferHandle(context.frameIndex);
			if (collectStats && !statsBuffer.isNull() && !statsReadback.isNull())
			{
				rhi::ComputeEncoder* copy = context.commandBuffer->beginComputePass();
				copy->copyBuffer(statsBuffer, 0u, statsReadback, 0u,
				                 sizeof(GPUDrivenLightResources::ClusterStats));
				context.commandBuffer->endEncoding();
			}
		}

		context.commandBuffer->endEvent();
	}
} // namespace demo
