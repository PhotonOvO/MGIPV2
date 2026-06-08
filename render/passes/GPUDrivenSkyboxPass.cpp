#include "GPUDrivenSkyboxPass.h"

#include "../ArgumentTables.h"

#include "../GPUDrivenRenderer.h"
#include "../PassExecutor.h"
#include "../../shaders/shader_io.h"

#include <array>

namespace demo
{
	GPUDrivenSkyboxPass::GPUDrivenSkyboxPass(GPUDrivenRenderer* renderer)
		: m_renderer(renderer)
	{
	}

	PassNode::HandleSlice<PassResourceDependency> GPUDrivenSkyboxPass::getDependencies() const
	{
		static const std::array<PassResourceDependency, 2> dependencies = {
			PassResourceDependency::texture(kPassSceneColorHdrHandle, ResourceAccess::readWrite,
			                                rhi::ShaderStage::fragment,
			                                rhi::ResourceState::ColorAttachment),
			PassResourceDependency::texture(kPassSceneDepthHandle, ResourceAccess::read, rhi::ShaderStage::fragment,
			                                rhi::ResourceState::DepthStencilReadOnly),
		};
		return {dependencies.data(), static_cast<uint32_t>(dependencies.size())};
	}

	void GPUDrivenSkyboxPass::execute(const PassContext& context) const
	{
		if (m_renderer == nullptr || context.commandBuffer == nullptr || context.executor == nullptr || context.params
			== nullptr
			|| !context.params->debugOptions.enableIBL || context.params->gpuDrivenSceneView == nullptr
			|| !context.cameraAllocValid)
		{
			return;
		}

		const GPUDrivenRenderer::ScreenPassTargets targets = m_renderer->getScreenColorDepthTargets();
		const PipelineHandle skyboxPipeline = m_renderer->getGPUDrivenSkyboxPipelineHandle();
		if (!targets.valid || skyboxPipeline.isNull())
		{
			return;
		}

		context.commandBuffer->beginEvent("GPUDrivenSkybox");

		const rhi::RenderTargetDesc colorTarget{
			.texture = {},
			.view = targets.colorView,
			.state = rhi::ResourceState::ColorAttachment,
			.loadOp = rhi::LoadOp::load,
			.storeOp = rhi::StoreOp::store,
		};
		const rhi::DepthTargetDesc depthTarget{
			.texture = {},
			.view = targets.depthView,
			.state = rhi::ResourceState::DepthStencilReadOnly,
			.loadOp = rhi::LoadOp::load,
			.storeOp = rhi::StoreOp::store,
			.clearValue = {0.0f, 0},
		};

		rhi::RenderEncoder* enc = context.commandBuffer->beginRenderPass(rhi::RenderPassDesc{
			.renderArea = {{0, 0}, targets.extent},
			.colorTargets = &colorTarget,
			.colorTargetCount = 1,
			.depthTarget = &depthTarget,
		});
		enc->setViewport(rhi::Viewport{
			0.0f, 0.0f, static_cast<float>(targets.extent.width),
			static_cast<float>(targets.extent.height), 0.0f, 1.0f
		});
		enc->setScissor(rhi::Rect2D{{0, 0}, targets.extent});

		// Pipeline + both descriptor sets bind through RHI handles.
		enc->setPipeline(skyboxPipeline);
		const rhi::ArgumentTableHandle inputTable = m_renderer->getLightingInputArgumentTable(context.frameIndex);
		enc->setArgumentTable(rhi::ShaderStage::fragment, shaderio::LSetTextures, inputTable);

		// Point the lighting-scene set at this frame's transient camera allocation, then
		// bind it (with its 2 dynamic UBOs, flushed in binding order) through the RHI.
		const rhi::ArgumentTableHandle sceneTable = m_renderer->getLightingSceneArgumentTable(context.frameIndex);
		enc->setDynamicBuffer(rhi::ShaderStage::allGraphics, kSceneDynamicBufferTableSlot, {},
		                      context.cameraAlloc.offset, 0);
		enc->setDynamicBuffer(rhi::ShaderStage::allGraphics, kSceneDynamicBufferTableSlot, {}, 0, 0);
		enc->setArgumentTable(rhi::ShaderStage::allGraphics, kSceneDynamicBufferTableSlot, sceneTable);

		enc->draw(rhi::DrawDesc{.vertexCount = 3, .instanceCount = 1, .firstVertex = 0, .firstInstance = 0});

		context.commandBuffer->endEncoding();

		context.commandBuffer->endEvent();
	}
} // namespace demo
