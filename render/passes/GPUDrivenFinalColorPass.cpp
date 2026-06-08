#include "GPUDrivenFinalColorPass.h"

#include "../ArgumentTables.h"

#include "../GPUDrivenRenderer.h"
#include "../PassExecutor.h"
#include "../../shaders/shader_io.h"

#include <algorithm>
#include <array>
#include <cstring>

namespace demo
{
	GPUDrivenFinalColorPass::GPUDrivenFinalColorPass(GPUDrivenRenderer* renderer)
		: m_renderer(renderer)
	{
	}

	PassNode::HandleSlice<PassResourceDependency> GPUDrivenFinalColorPass::getDependencies() const
	{
		static const std::array<PassResourceDependency, 3> dependencies = {
			PassResourceDependency::texture(kPassSceneColorHdrHandle, ResourceAccess::read, rhi::ShaderStage::fragment),
			PassResourceDependency::texture(kPassBloomOutputHandle, ResourceAccess::read, rhi::ShaderStage::fragment),
			PassResourceDependency::texture(kPassOutputHandle, ResourceAccess::write, rhi::ShaderStage::fragment,
			                                rhi::ResourceState::ColorAttachment),
		};
		return {dependencies.data(), static_cast<uint32_t>(dependencies.size())};
	}

	void GPUDrivenFinalColorPass::execute(const PassContext& context) const
	{
		if (m_renderer == nullptr || context.commandBuffer == nullptr || context.executor == nullptr || context.params
			== nullptr)
		{
			return;
		}

		const rhi::TextureHandle outputImage = m_renderer->getOutputTextureImage();
		const rhi::TextureViewHandle outputView = m_renderer->getOutputTextureView();
		const rhi::Extent2D outputExtent = m_renderer->getSceneExtent();
		if (outputImage.isNull() || outputView.isNull() || outputExtent.width == 0u || outputExtent.height == 0u)
		{
			return;
		}

		context.commandBuffer->beginEvent("GPUDrivenFinalColor");
		rhi::RenderTargetDesc colorTarget{
			.texture = {},
			.view = outputView,
			.state = rhi::ResourceState::ColorAttachment,
			.loadOp = rhi::LoadOp::clear,
			.storeOp = rhi::StoreOp::store,
			.clearColor = {0.0f, 0.0f, 0.0f, 1.0f},
		};

		rhi::RenderEncoder* enc = context.commandBuffer->beginRenderPass(rhi::RenderPassDesc{
			.renderArea = {{0, 0}, outputExtent},
			.colorTargets = &colorTarget,
			.colorTargetCount = 1,
			.depthTarget = nullptr,
		});
		enc->setViewport(rhi::Viewport{
			0.0f, 0.0f, static_cast<float>(outputExtent.width), static_cast<float>(outputExtent.height), 0.0f, 1.0f
		});
		enc->setScissor(rhi::Rect2D{{0, 0}, outputExtent});

		const PipelineHandle pipelineHandle = m_renderer->getFinalColorPipelineHandle();
		if (pipelineHandle.isNull())
		{
			context.commandBuffer->endEncoding();
			context.commandBuffer->endEvent();
			return;
		}
		const rhi::ArgumentTableHandle inputTable = m_renderer->getLightingInputArgumentTable(context.frameIndex);
		if (!pipelineHandle.isNull() && !inputTable.isNull())
		{
			enc->setPipeline(pipelineHandle);
			enc->setArgumentTable(rhi::ShaderStage::fragment, shaderio::LSetTextures, inputTable);

			const float exposure = context.params->debugOptions.enablePostProcessing
				                       ? std::max(context.params->debugOptions.postExposure, 0.01f)
				                       : 1.0f;
			const shaderio::PostProcessUniforms postProcessUniforms{
				.params0 = glm::vec4(exposure,
				                     context.params->debugOptions.bloomIntensity,
				                     context.params->debugOptions.bloomThreshold,
				                     (context.params->debugOptions.enablePostProcessing
					                     && context.params->debugOptions.enableBloom)
					                     ? 1.0f
					                     : 0.0f),
				.params1 = glm::vec4(1.0f / static_cast<float>(std::max(1u, outputExtent.width)),
				                     1.0f / static_cast<float>(std::max(1u, outputExtent.height)),
				                     1.0f / static_cast<float>(std::max(1u, outputExtent.width)),
				                     1.0f / static_cast<float>(std::max(1u, outputExtent.height))),
				.params2 = glm::vec4((context.params->debugOptions.enablePostProcessing
					                     && context.params->debugOptions.enableAdaptiveExposure)
					                     ? 1.0f
					                     : 0.0f,
				                     context.params->debugOptions.exposureTargetLuminance,
				                     context.params->debugOptions.minAutoExposure,
				                     context.params->debugOptions.maxAutoExposure),
				.params3 = glm::vec4((context.params->debugOptions.enablePostProcessing
					                     && context.params->debugOptions.enableColorGrading)
					                     ? context.params->debugOptions.colorSaturation
					                     : 1.0f,
				                     (context.params->debugOptions.enablePostProcessing
					                     && context.params->debugOptions.enableColorGrading)
					                     ? context.params->debugOptions.colorContrast
					                     : 1.0f,
				                     (context.params->debugOptions.enablePostProcessing
					                     && context.params->debugOptions.enableColorGrading)
					                     ? context.params->debugOptions.colorGamma
					                     : 1.0f,
				                     (context.params->debugOptions.enablePostProcessing
					                     && context.params->debugOptions.enableColorGrading)
					                     ? context.params->debugOptions.vignetteIntensity
					                     : 0.0f),
				.params4 = glm::vec4((context.params->debugOptions.enablePostProcessing
					                     && context.params->debugOptions.enableLensEffects)
					                     ? 1.0f
					                     : 0.0f,
				                     context.params->debugOptions.lensDirtIntensity,
				                     (context.params->debugOptions.enablePostProcessing
					                     && context.params->debugOptions.enableColorGrading)
					                     ? std::clamp(context.params->debugOptions.colorLutStrength, 0.0f, 1.0f)
					                     : 0.0f,
				                     0.0f),
				.params5 = glm::vec4((context.params->debugOptions.enablePostProcessing
					                     && context.params->debugOptions.enableTAA
					                     && !m_renderer->getTAAResolvePipelineHandle().isNull())
					                     ? 1.0f
					                     : 0.0f,
				                     0.0f,
				                     context.params->debugOptions.taaBlendWeight,
				                     context.params->debugOptions.showVelocity ? 1.0f : 0.0f),
			};
			const TransientAllocator::Allocation postProcessAlloc =
				context.transientAllocator->allocate(sizeof(postProcessUniforms), 256);
			std::memcpy(postProcessAlloc.cpuPtr, &postProcessUniforms, sizeof(postProcessUniforms));
			context.transientAllocator->flushAllocation(postProcessAlloc, sizeof(postProcessUniforms));
			const rhi::ArgumentTableHandle sceneTable = m_renderer->getLightingSceneArgumentTable(context.frameIndex);
			if (!sceneTable.isNull())
			{
				enc->setDynamicBuffer(rhi::ShaderStage::allGraphics, kSceneDynamicBufferTableSlot, {},
				                      context.cameraAlloc.offset, 0);
				enc->setDynamicBuffer(rhi::ShaderStage::allGraphics, kSceneDynamicBufferTableSlot, {},
				                      postProcessAlloc.offset, 0);
				enc->setArgumentTable(rhi::ShaderStage::allGraphics, kSceneDynamicBufferTableSlot, sceneTable);
				enc->draw(rhi::DrawDesc{.vertexCount = 3, .instanceCount = 1, .firstVertex = 0, .firstInstance = 0});
			}
		}

		context.commandBuffer->endEncoding();
		context.commandBuffer->endEvent();
	}
} // namespace demo
