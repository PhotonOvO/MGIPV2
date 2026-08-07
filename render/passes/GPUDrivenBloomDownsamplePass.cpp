#include "GPUDrivenBloomDownsamplePass.h"

#include "../ArgumentTables.h"

#include "../GPUDrivenRenderer.h"
#include "../PassExecutor.h"
#include "../../shaders/shader_io.h"

#include <algorithm>
#include <array>
#include <cstring>

namespace demo
{
	GPUDrivenBloomDownsamplePass::GPUDrivenBloomDownsamplePass(GPUDrivenRenderer* renderer)
		: m_renderer(renderer)
	{
	}

	PassNode::HandleSlice<PassResourceDependency> GPUDrivenBloomDownsamplePass::getDependencies() const
	{
		// PassExecutor cannot see the substeps inside this pass. Declare the entire
		// private bloom chain at its stable inter-pass sampled state; each destination
		// is transitioned to ColorAttachment only for its draw, then published back.
		static const std::array<PassResourceDependency, 9> dependencies = {
			PassResourceDependency::texture(kPassBloomHalfHandle, ResourceAccess::read,
			                                rhi::ShaderStage::fragment, rhi::ResourceState::ShaderRead),
			PassResourceDependency::texture(kPassBloomQuarterHandle, ResourceAccess::read,
			                                rhi::ShaderStage::fragment, rhi::ResourceState::ShaderRead),
			PassResourceDependency::texture(kPassBloomEighthHandle, ResourceAccess::read,
			                                rhi::ShaderStage::fragment, rhi::ResourceState::ShaderRead),
			PassResourceDependency::texture(kPassBloomSixteenthHandle, ResourceAccess::read,
			                                rhi::ShaderStage::fragment, rhi::ResourceState::ShaderRead),
			PassResourceDependency::texture(kPassBloomThirtySecondHandle, ResourceAccess::read,
			                                rhi::ShaderStage::fragment, rhi::ResourceState::ShaderRead),
			PassResourceDependency::texture(kPassBloomUpsampleSixteenthHandle, ResourceAccess::read,
			                                rhi::ShaderStage::fragment, rhi::ResourceState::ShaderRead),
			PassResourceDependency::texture(kPassBloomUpsampleEighthHandle, ResourceAccess::read,
			                                rhi::ShaderStage::fragment, rhi::ResourceState::ShaderRead),
			PassResourceDependency::texture(kPassBloomUpsampleQuarterHandle, ResourceAccess::read,
			                                rhi::ShaderStage::fragment, rhi::ResourceState::ShaderRead),
			PassResourceDependency::texture(kPassBloomOutputHandle, ResourceAccess::read,
			                                rhi::ShaderStage::fragment, rhi::ResourceState::ShaderRead),
		};
		return {dependencies.data(), static_cast<uint32_t>(dependencies.size())};
	}

	void GPUDrivenBloomDownsamplePass::execute(const PassContext& context) const
	{
		if (m_renderer == nullptr || context.commandBuffer == nullptr || context.executor == nullptr || context.params
			== nullptr || context.transientAllocator == nullptr || !context.cameraAllocValid
			|| !context.params->debugOptions.enablePostProcessing
			|| !context.params->debugOptions.enableBloom)
		{
			return;
		}

		struct BloomStep
		{
			rhi::TextureHandle image{};
			rhi::TextureViewHandle view{};
			rhi::Extent2D extent{};
			rhi::Extent2D sourceExtent{};
			TextureHandle handle{};
			PipelineHandle pipeline{};
			uint32_t sourceIndex{0u};
			uint32_t lowerIndex{0u};
			float radius{1.0f};
		};

		const PipelineHandle downsamplePipeline = m_renderer->getBloomDownsamplePipelineHandle();
		const PipelineHandle upsamplePipeline = m_renderer->getBloomUpsamplePipelineHandle();
		const std::array<BloomStep, 8> steps{
			BloomStep{
				m_renderer->getBloomQuarterImage(), m_renderer->getBloomQuarterView(),
				m_renderer->getBloomQuarterExtent(), m_renderer->getBloomHalfExtent(),
				kPassBloomQuarterHandle, downsamplePipeline, 5u, 0u, 1.0f
			},
			BloomStep{
				m_renderer->getBloomEighthImage(), m_renderer->getBloomEighthView(),
				m_renderer->getBloomEighthExtent(), m_renderer->getBloomQuarterExtent(),
				kPassBloomEighthHandle, downsamplePipeline, 6u, 0u, 1.0f
			},
			BloomStep{
				m_renderer->getBloomSixteenthImage(), m_renderer->getBloomSixteenthView(),
				m_renderer->getBloomSixteenthExtent(), m_renderer->getBloomEighthExtent(),
				kPassBloomSixteenthHandle, downsamplePipeline, 13u, 0u, 1.0f
			},
			BloomStep{
				m_renderer->getBloomThirtySecondImage(), m_renderer->getBloomThirtySecondView(),
				m_renderer->getBloomThirtySecondExtent(), m_renderer->getBloomSixteenthExtent(),
				kPassBloomThirtySecondHandle, downsamplePipeline, 14u, 0u, 1.0f
			},
			BloomStep{
				m_renderer->getBloomUpsampleSixteenthImage(), m_renderer->getBloomUpsampleSixteenthView(),
				m_renderer->getBloomUpsampleSixteenthExtent(), m_renderer->getBloomThirtySecondExtent(),
				kPassBloomUpsampleSixteenthHandle, upsamplePipeline, 14u, 15u, 1.0f
			},
			BloomStep{
				m_renderer->getBloomUpsampleEighthImage(), m_renderer->getBloomUpsampleEighthView(),
				m_renderer->getBloomUpsampleEighthExtent(), m_renderer->getBloomSixteenthExtent(),
				kPassBloomUpsampleEighthHandle, upsamplePipeline, 13u, 16u, 1.0f
			},
			BloomStep{
				m_renderer->getBloomUpsampleQuarterImage(), m_renderer->getBloomUpsampleQuarterView(),
				m_renderer->getBloomUpsampleQuarterExtent(), m_renderer->getBloomEighthExtent(),
				kPassBloomUpsampleQuarterHandle, upsamplePipeline, 6u, 17u, 1.0f
			},
			BloomStep{
				m_renderer->getBloomOutputImage(), m_renderer->getBloomOutputView(),
				m_renderer->getBloomOutputExtent(), m_renderer->getBloomQuarterExtent(),
				kPassBloomOutputHandle, upsamplePipeline, 5u, 18u, 1.0f
			},
		};

		const rhi::ArgumentTableHandle inputTable = m_renderer->getLightingInputArgumentTable(context.frameIndex);
		const rhi::ArgumentTableHandle sceneTable = m_renderer->getLightingSceneArgumentTable(context.frameIndex);
		const rhi::Extent2D bloomHalfExtent = m_renderer->getBloomHalfExtent();
		bool bloomWritesAllLevelsThisFrame = !downsamplePipeline.isNull() && !upsamplePipeline.isNull()
			&& !inputTable.isNull() && !sceneTable.isNull()
			&& !m_renderer->getBloomHalfImage().isNull() && !m_renderer->getBloomHalfView().isNull()
			&& bloomHalfExtent.width > 0u && bloomHalfExtent.height > 0u
			&& !context.executor->getTextureRHIHandle(kPassBloomHalfHandle).isNull();
		for (const BloomStep& step : steps)
		{
			bloomWritesAllLevelsThisFrame = bloomWritesAllLevelsThisFrame
				&& !step.image.isNull() && !step.view.isNull()
				&& step.extent.width > 0u && step.extent.height > 0u
				&& step.sourceExtent.width > 0u && step.sourceExtent.height > 0u
				&& !step.pipeline.isNull()
				&& !context.executor->getTextureRHIHandle(step.handle).isNull();
		}
		if (!bloomWritesAllLevelsThisFrame)
		{
			return;
		}

		context.commandBuffer->beginEvent("GPUDrivenBloomDownsample");
		const auto transitionStep = [&](const BloomStep& step, rhi::ResourceState before, rhi::ResourceState after)
		{
			const rhi::TextureHandle stepTexture = context.executor->getTextureRHIHandle(step.handle);
			const rhi::TextureBarrier barrier{
				.texture = stepTexture,
				.before = before,
				.after = after,
				.range = {
					.aspect = rhi::TextureAspect::color, .baseMipLevel = 0, .levelCount = ~0u, .baseArrayLayer = 0,
					.layerCount = ~0u
				},
			};
			// Each substep consumes sampled descriptors in ShaderRead, writes one
			// color target, then publishes that level for the next sampled read.
			context.commandBuffer->resourceBarrier(std::span{&barrier, 1}, {});
		};
		const auto renderStep = [&](const BloomStep& step)
		{
			transitionStep(step, rhi::ResourceState::ShaderRead, rhi::ResourceState::ColorAttachment);

			rhi::RenderTargetDesc colorTarget{
				.texture = {},
				.view = step.view,
				.state = rhi::ResourceState::ColorAttachment,
				.loadOp = rhi::LoadOp::clear,
				.storeOp = rhi::StoreOp::store,
				.clearColor = {0.0f, 0.0f, 0.0f, 1.0f},
			};
			const rhi::Extent2D extent{step.extent.width, step.extent.height};
			rhi::RenderEncoder* enc = context.commandBuffer->beginRenderPass(rhi::RenderPassDesc{
				.renderArea = {{0, 0}, extent},
				.colorTargets = std::span{&colorTarget, 1},
				.depthTarget = nullptr,
			});
			enc->setViewport(rhi::Viewport{
				0.0f, 0.0f, static_cast<float>(extent.width), static_cast<float>(extent.height), 0.0f, 1.0f
			});
			enc->setScissor(rhi::Rect2D{{0, 0}, extent});
			enc->setPipeline(step.pipeline);
			enc->setArgumentTable(rhi::ShaderStage::fragment, shaderio::LSetTextures, inputTable);

			const shaderio::PostProcessUniforms postProcessUniforms{
				.params0 = glm::vec4(context.params->debugOptions.postExposure,
				                     context.params->debugOptions.bloomIntensity,
				                     context.params->debugOptions.bloomThreshold,
				                     context.params->debugOptions.enableBloom ? 1.0f : 0.0f),
				.params1 = glm::vec4(1.0f / static_cast<float>(std::max(1u, step.sourceExtent.width)),
				                     1.0f / static_cast<float>(std::max(1u, step.sourceExtent.height)),
				                     1.0f / static_cast<float>(std::max(1u, step.extent.width)),
				                     1.0f / static_cast<float>(std::max(1u, step.extent.height))),
				.params2 = glm::vec4(0.0f),
				.params3 = glm::vec4(1.0f, 1.0f, 1.0f, 0.0f),
				.params4 = glm::vec4(0.0f),
				.params5 = glm::vec4((context.params->debugOptions.enablePostProcessing
					                     && context.params->debugOptions.enableTAA
					                     && !m_renderer->getTAAResolvePipelineHandle().isNull())
					                     ? 1.0f
					                     : 0.0f,
					                     static_cast<float>(step.sourceIndex),
					                     static_cast<float>(step.lowerIndex),
					                     step.radius),
			};
			const TransientAllocator::Allocation postProcessAlloc =
				context.transientAllocator->allocate(sizeof(postProcessUniforms), 256);
			std::memcpy(postProcessAlloc.cpuPtr, &postProcessUniforms, sizeof(postProcessUniforms));
			context.transientAllocator->flushAllocation(postProcessAlloc, sizeof(postProcessUniforms));
			enc->setDynamicBuffer(rhi::ShaderStage::allGraphics, kSceneDynamicBufferTableSlot, {},
			                      context.cameraAlloc.offset, 0);
			enc->setDynamicBuffer(rhi::ShaderStage::allGraphics, kSceneDynamicBufferTableSlot, {},
			                      postProcessAlloc.offset, 0);
			enc->setArgumentTable(rhi::ShaderStage::allGraphics, kSceneDynamicBufferTableSlot, sceneTable);
			enc->draw(rhi::DrawDesc{
				.vertexCount = 3, .instanceCount = 1, .firstVertex = 0, .firstInstance = 0
			});
			context.commandBuffer->endEncoding();

			transitionStep(step, rhi::ResourceState::ColorAttachment, rhi::ResourceState::ShaderRead);
		};

		for (const BloomStep& step : steps)
		{
			renderStep(step);
		}
		context.commandBuffer->endEvent();
	}
} // namespace demo
