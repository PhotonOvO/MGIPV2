#include "GPUDrivenTAAResolvePass.h"

#include "../ArgumentTables.h"

#include "../GPUDrivenRenderer.h"
#include "../PassExecutor.h"
#include "../../shaders/shader_io.h"

#include <algorithm>
#include <array>
#include <cstring>

namespace demo
{
	GPUDrivenTAAResolvePass::GPUDrivenTAAResolvePass(GPUDrivenRenderer* renderer)
		: m_renderer(renderer)
	{
	}

	PassNode::HandleSlice<PassResourceDependency> GPUDrivenTAAResolvePass::getDependencies() const
	{
		static const std::array<PassResourceDependency, 4> dependencies = {
			PassResourceDependency::texture(kPassSceneColorHdrHandle, ResourceAccess::read, rhi::ShaderStage::fragment,
			                                rhi::ResourceState::ShaderRead),
			PassResourceDependency::texture(kPassVelocityHandle, ResourceAccess::read, rhi::ShaderStage::fragment,
			                                rhi::ResourceState::ShaderRead),
			PassResourceDependency::texture(kPassSceneColorHistoryReadHandle, ResourceAccess::read,
			                                rhi::ShaderStage::fragment, rhi::ResourceState::ShaderRead),
			PassResourceDependency::texture(kPassSceneColorHistoryWriteHandle, ResourceAccess::write,
			                                rhi::ShaderStage::fragment,
			                                rhi::ResourceState::ColorAttachment),
		};
		return {dependencies.data(), static_cast<uint32_t>(dependencies.size())};
	}

	void GPUDrivenTAAResolvePass::execute(const PassContext& context) const
	{
		if (m_renderer == nullptr || context.commandBuffer == nullptr || context.executor == nullptr || context.params
			== nullptr || context.transientAllocator == nullptr || !context.cameraAllocValid)
		{
			return;
		}

		const DebugPassOptions& options = context.params->debugOptions;
		const GPUDrivenSceneView* sceneView = context.params->gpuDrivenSceneView;
		const rhi::Extent2D extent = m_renderer->getSceneExtent();
		const PipelineHandle pipelineHandle = m_renderer->getTAAResolveExecutionPipelineHandle();
		const rhi::ArgumentTableHandle inputTable = m_renderer->getLightingInputArgumentTable(context.frameIndex);
		const rhi::ArgumentTableHandle sceneTable = m_renderer->getLightingSceneArgumentTable(context.frameIndex);
		const bool taaWritesHistoryThisFrame = options.enablePostProcessing && options.enableTAA
			&& sceneView != nullptr
			&& !sceneView->sceneColorHdrImage.isNull() && !sceneView->sceneColorHdrView.isNull()
			&& !sceneView->velocityImage.isNull() && !sceneView->velocityView.isNull()
			&& !sceneView->sceneColorHistoryReadImage.isNull() && !sceneView->sceneColorHistoryReadView.isNull()
			&& !sceneView->sceneColorHistoryWriteImage.isNull() && !sceneView->sceneColorHistoryWriteView.isNull()
			&& extent.width > 0u && extent.height > 0u
			&& !pipelineHandle.isNull() && !inputTable.isNull() && !sceneTable.isNull()
			&& !context.executor->getTextureRHIHandle(kPassSceneColorHdrHandle).isNull()
			&& !context.executor->getTextureRHIHandle(kPassVelocityHandle).isNull()
			&& !context.executor->getTextureRHIHandle(kPassSceneColorHistoryReadHandle).isNull()
			&& !context.executor->getTextureRHIHandle(kPassSceneColorHistoryWriteHandle).isNull();
		if (!taaWritesHistoryThisFrame)
		{
			return;
		}

		context.commandBuffer->beginEvent("GPUDrivenTAAResolve");
		const rhi::RenderTargetDesc colorTarget{
			.texture = {},
			.view = sceneView->sceneColorHistoryWriteView,
			.state = rhi::ResourceState::ColorAttachment,
			.loadOp = rhi::LoadOp::clear,
			.storeOp = rhi::StoreOp::store,
			.clearColor = {0.0f, 0.0f, 0.0f, 1.0f},
		};
		rhi::RenderEncoder* enc = context.commandBuffer->beginRenderPass(rhi::RenderPassDesc{
			.renderArea = {{0, 0}, extent},
			.colorTargets = std::span{&colorTarget, 1},
			.depthTarget = nullptr,
		});
		enc->setViewport(rhi::Viewport{
			0.0f, 0.0f, static_cast<float>(extent.width), static_cast<float>(extent.height), 0.0f, 1.0f
		});
		enc->setScissor(rhi::Rect2D{{0, 0}, extent});
		enc->setPipeline(pipelineHandle);
		enc->setArgumentTable(rhi::ShaderStage::fragment, shaderio::LSetTextures, inputTable);

		const TransientAllocator::Allocation& cameraAlloc = context.cameraAlloc;
		const shaderio::PostProcessUniforms postProcessUniforms{
			.params0 = glm::vec4(options.postExposure,
			                     options.bloomIntensity,
			                     options.bloomThreshold,
			                     options.enableBloom ? 1.0f : 0.0f),
			.params1 = glm::vec4(1.0f / static_cast<float>(std::max(1u, extent.width)),
			                     1.0f / static_cast<float>(std::max(1u, extent.height)),
			                     1.0f / static_cast<float>(std::max(1u, extent.width)),
			                     1.0f / static_cast<float>(std::max(1u, extent.height))),
			// params2.xy = 当前帧 jitter（像素单位），filterInput 的 Lanczos 核中心
			.params2 = glm::vec4(m_renderer->getCurrentTAAJitterUv().x * static_cast<float>(extent.width),
			                     m_renderer->getCurrentTAAJitterUv().y * static_cast<float>(extent.height),
			                     0.0f, 0.0f),
			.params3 = glm::vec4(1.0f, 1.0f, 1.0f, 0.0f),
			// params4 = TAA 特性开关：filterInput / varianceBox / lottes / catmullRom
			.params4 = glm::vec4(options.taaFilterInput ? 1.0f : 0.0f,
			                     options.taaVarianceClip ? 1.0f : 0.0f,
			                     options.taaPreventFlicker ? 1.0f : 0.0f,
			                     options.taaCatmullRom ? 1.0f : 0.0f),
			.params5 = glm::vec4(1.0f,
			                     m_renderer->isTAAHistoryValid() ? 1.0f : 0.0f,
			                     std::clamp(options.taaBlendWeight, 0.0f, 0.98f),
			                     0.0f),
		};
		const TransientAllocator::Allocation postProcessAlloc =
			context.transientAllocator->allocate(sizeof(postProcessUniforms), 256);
		std::memcpy(postProcessAlloc.cpuPtr, &postProcessUniforms, sizeof(postProcessUniforms));
		context.transientAllocator->flushAllocation(postProcessAlloc, sizeof(postProcessUniforms));
		enc->setDynamicBuffer(rhi::ShaderStage::allGraphics, kSceneDynamicBufferTableSlot, {},
		                      cameraAlloc.offset, 0);
		enc->setDynamicBuffer(rhi::ShaderStage::allGraphics, kSceneDynamicBufferTableSlot, {},
		                      postProcessAlloc.offset, 0);
		enc->setArgumentTable(rhi::ShaderStage::allGraphics, kSceneDynamicBufferTableSlot, sceneTable);
		enc->draw(rhi::DrawDesc{.vertexCount = 3, .instanceCount = 1, .firstVertex = 0, .firstInstance = 0});
		m_renderer->markTAAResolveHistoryWrittenThisFrame();

		context.commandBuffer->endEncoding();
		context.commandBuffer->endEvent();
	}
} // namespace demo
