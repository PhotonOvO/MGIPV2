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
		static const std::array<PassResourceDependency, 7> dependencies = {
			PassResourceDependency::buffer(kPassPointLightBufferHandle, ResourceAccess::read,
			                               rhi::ShaderStage::compute),
			PassResourceDependency::buffer(kPassSpotLightBufferHandle, ResourceAccess::read, rhi::ShaderStage::compute),
			PassResourceDependency::buffer(kPassClusteredLightUniformHandle, ResourceAccess::read,
			                               rhi::ShaderStage::compute),
			PassResourceDependency::buffer(kPassClusterLightCountsHandle, ResourceAccess::write,
			                               rhi::ShaderStage::compute),
			PassResourceDependency::buffer(kPassClusterLightIndicesHandle, ResourceAccess::write,
			                               rhi::ShaderStage::compute),
			PassResourceDependency::buffer(kPassClusterLightStatsHandle, ResourceAccess::write,
			                               rhi::ShaderStage::compute),
			PassResourceDependency::texture(kPassSceneDepthHandle, ResourceAccess::read, rhi::ShaderStage::compute,
			                                rhi::ResourceState::ShaderRead),
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
			const rhi::BufferHandle statsBuffer = m_renderer->getClusterStatsBufferHandle(context.frameIndex);
			if (!statsBuffer.isNull())
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

			// Local output barrier: clustered culling writes light-list buffers consumed
			// through lighting tables that are not fully represented in pass dependencies.
			context.commandBuffer->barrier(rhi::StageFlags::compute, rhi::StageFlags::fragmentShader,
			                               rhi::HazardFlags::bufferWrites);
		}

		context.commandBuffer->endEvent();
	}
} // namespace demo
