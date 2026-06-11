#include "GPUDrivenAOPass.h"

#include "../ArgumentTables.h"
#include "../GPUDrivenRenderer.h"
#include "../../shaders/shader_io.h"

#include <algorithm>
#include <array>

namespace demo
{
	GPUDrivenAOPass::GPUDrivenAOPass(GPUDrivenRenderer* renderer)
		: m_renderer(renderer)
	{
	}

	PassNode::HandleSlice<PassResourceDependency> GPUDrivenAOPass::getDependencies() const
	{
		static const std::array<PassResourceDependency, 2> dependencies = {
			PassResourceDependency::texture(kPassSceneDepthHandle, ResourceAccess::read, rhi::ShaderStage::compute,
			                                rhi::ResourceState::ShaderRead),
			PassResourceDependency::texture(kPassGBuffer1Handle, ResourceAccess::read, rhi::ShaderStage::compute),
		};
		return {dependencies.data(), static_cast<uint32_t>(dependencies.size())};
	}

	void GPUDrivenAOPass::execute(const PassContext& context) const
	{
		if (m_renderer == nullptr || context.commandBuffer == nullptr || context.params == nullptr)
		{
			return;
		}
		// [1] enableAO=false is a legitimate off-switch; silent return.
		if (!context.params->debugOptions.enableAO)
			return;

		// [2] Pipeline opaque check (one-shot warning).
		if (m_renderer->getAOTracePipelineOpaque() == 0 || m_renderer->getAODenoisePipelineOpaque() == 0)
		{
			static bool s_loggedPipeline = false;
			if (!s_loggedPipeline)
			{
				LOGW("GPUDrivenAOPass skipped: AO pipeline handle not ready (trace=%llu denoise=%llu)",
				     static_cast<unsigned long long>(m_renderer->getAOTracePipelineOpaque()),
				     static_cast<unsigned long long>(m_renderer->getAODenoisePipelineOpaque()));
				s_loggedPipeline = true;
			}
			return;
		}

		// [3] Argument table check (one-shot warning).
		const uint32_t frameIndex = context.frameIndex;
		const rhi::ArgumentTableHandle aoTable = m_renderer->getAOArgumentTable(frameIndex);
		const rhi::ArgumentTableHandle aoDenoiseTable = m_renderer->getAODenoiseArgumentTable(frameIndex);
		if (aoTable.isNull() || aoDenoiseTable.isNull())
		{
			static bool s_loggedArgTable = false;
			if (!s_loggedArgTable)
			{
				LOGW("GPUDrivenAOPass skipped: argument table null (frameIndex=%u trace=%d denoise=%d)",
				     frameIndex, aoTable.isNull() ? 0 : 1, aoDenoiseTable.isNull() ? 0 : 1);
				s_loggedArgTable = true;
			}
			return;
		}

		// [4] Pipeline handle check (one-shot warning).
		const PipelineHandle aoTracePipeline = m_renderer->getAOTracePipelineHandle();
		const PipelineHandle aoDenoisePipeline = m_renderer->getAODenoisePipelineHandle();
		if (aoTracePipeline.isNull() || aoDenoisePipeline.isNull())
		{
			static bool s_loggedHandle = false;
			if (!s_loggedHandle)
			{
				LOGW("GPUDrivenAOPass skipped: pipeline handle null (trace=%d denoise=%d)",
				     aoTracePipeline.isNull() ? 0 : 1, aoDenoisePipeline.isNull() ? 0 : 1);
				s_loggedHandle = true;
			}
			return;
		}

		// [5] Image opaque check (one-shot warning).
		if (m_renderer->getAORawImageOpaque() == 0 || m_renderer->getAODenoisedImageOpaque() == 0)
		{
			static bool s_loggedImage = false;
			if (!s_loggedImage)
			{
				LOGW("GPUDrivenAOPass skipped: AO image null (raw=%llu denoised=%llu)",
				     static_cast<unsigned long long>(m_renderer->getAORawImageOpaque()),
				     static_cast<unsigned long long>(m_renderer->getAODenoisedImageOpaque()));
				s_loggedImage = true;
			}
			return;
		}

		const rhi::Extent2D halfExtent = m_renderer->getPhase7HalfExtent();
		const rhi::Extent2D sceneExtent = m_renderer->getSceneExtent();
		const shaderio::GPUDrivenAOPushConstants push{
			.params0 = glm::vec4(halfExtent.width, halfExtent.height, context.params->debugOptions.aoRadius,
			                     context.params->debugOptions.aoIntensity),
			.params1 = glm::vec4(1.0f / static_cast<float>(std::max(1u, sceneExtent.width)),
			                     1.0f / static_cast<float>(std::max(1u, sceneExtent.height)),
			                     64.0f,
			                     0.35f),
		};

		const uint32_t groupsX = (halfExtent.width + 7u) / 8u;
		const uint32_t groupsY = (halfExtent.height + 7u) / 8u;

		rhi::ComputeEncoder* trace = context.commandBuffer->beginComputePass();
		trace->setPipeline(aoTracePipeline);
		trace->setArgumentTable(0, aoTable);
		trace->setRootConstants(kPrimaryRootConstantsSlot, &push, sizeof(push));
		trace->dispatch(rhi::DispatchDesc{groupsX, groupsY, 1u});
		context.commandBuffer->endEncoding();

		// Same-pass barrier: AO trace dispatch writes the raw AO texture consumed by
		// the denoise dispatch below; no pass boundary exists for PassExecutor to model.
		context.commandBuffer->barrier(rhi::StageFlags::compute,
		                               rhi::StageFlags::compute | rhi::StageFlags::fragmentShader,
		                               rhi::HazardFlags::textureWrites);

		rhi::ComputeEncoder* denoise = context.commandBuffer->beginComputePass();
		denoise->setPipeline(aoDenoisePipeline);
		denoise->setArgumentTable(0, aoDenoiseTable);
		denoise->setRootConstants(kPrimaryRootConstantsSlot, &push, sizeof(push));
		denoise->dispatch(rhi::DispatchDesc{groupsX, groupsY, 1u});
		context.commandBuffer->endEncoding();
		// Local output barrier: AO denoise writes a renderer-private texture sampled by
		// later lighting; it has no PassResourceDependency handle in the graph yet.
		context.commandBuffer->barrier(rhi::StageFlags::compute,
		                               rhi::StageFlags::compute | rhi::StageFlags::fragmentShader,
		                               rhi::HazardFlags::textureWrites);
	}
} // namespace demo
