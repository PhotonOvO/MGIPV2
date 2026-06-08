#include "GPUDrivenSkyPass.h"

#include "../ArgumentTables.h"
#include "../GPUDrivenRenderer.h"
#include "../PassExecutor.h"
#include "../../shaders/shader_io.h"

#include <array>

namespace demo
{
	GPUDrivenSkyPass::GPUDrivenSkyPass(GPUDrivenRenderer* renderer)
		: m_renderer(renderer)
	{
	}

	PassNode::HandleSlice<PassResourceDependency> GPUDrivenSkyPass::getDependencies() const
	{
		static const std::array<PassResourceDependency, 3> dependencies = {
			PassResourceDependency::texture(kPassOutputHandle, ResourceAccess::readWrite, rhi::ShaderStage::fragment),
			PassResourceDependency::texture(kPassSceneDepthHandle, ResourceAccess::read, rhi::ShaderStage::fragment,
			                                rhi::ResourceState::ShaderRead),
			PassResourceDependency::texture(kPassGBuffer0Handle, ResourceAccess::read, rhi::ShaderStage::fragment),
		};
		return {dependencies.data(), static_cast<uint32_t>(dependencies.size())};
	}

	void GPUDrivenSkyPass::execute(const PassContext& context) const
	{
		if (m_renderer == nullptr || context.commandBuffer == nullptr || context.executor == nullptr || context.params
			== nullptr)
		{
			return;
		}

		const GPUDrivenSceneView* sceneView = context.params->gpuDrivenSceneView;
		if (sceneView == nullptr || sceneView->outputImage.isNull() || sceneView->outputView.isNull()
			|| sceneView->sceneDepthExtent.width == 0 || sceneView->sceneDepthExtent.height == 0)
		{
			return;
		}

		const PipelineHandle skyPipeline = m_renderer->getSkyPipelineHandle();
		if (skyPipeline.isNull())
		{
			return;
		}

		context.commandBuffer->beginEvent("GPUDrivenSkyPass");

		rhi::RenderTargetDesc colorTarget{
			.texture = {},
			.view = sceneView->outputView,
			.state = rhi::ResourceState::ColorAttachment,
			.loadOp = rhi::LoadOp::load,
			.storeOp = rhi::StoreOp::store,
		};

		const rhi::Extent2D extent{sceneView->sceneDepthExtent.width, sceneView->sceneDepthExtent.height};
		rhi::RenderEncoder* enc = context.commandBuffer->beginRenderPass(rhi::RenderPassDesc{
			.renderArea = {{0, 0}, extent},
			.colorTargets = &colorTarget,
			.colorTargetCount = 1,
			.depthTarget = nullptr,
		});
		enc->setViewport(
			rhi::Viewport{0.0f, 0.0f, static_cast<float>(extent.width), static_cast<float>(extent.height), 0.0f, 1.0f});
		enc->setScissor(rhi::Rect2D{{0, 0}, extent});

		enc->setPipeline(skyPipeline);
		const rhi::ArgumentTableHandle inputTable = m_renderer->getLightingInputArgumentTable(context.frameIndex);
		enc->setArgumentTable(rhi::ShaderStage::fragment, shaderio::LSetTextures, inputTable);

		if (context.cameraAllocValid)
		{
			const rhi::ArgumentTableHandle cameraTable = m_renderer->getCameraArgumentTable(context.frameIndex);
			if (!cameraTable.isNull())
			{
				enc->setDynamicBuffer(rhi::ShaderStage::allGraphics, kSceneDynamicBufferTableSlot, {},
				                      context.cameraAlloc.offset, 0);
				enc->setArgumentTable(rhi::ShaderStage::allGraphics, kSceneDynamicBufferTableSlot, cameraTable);
			}
		}

		enc->draw(rhi::DrawDesc{.vertexCount = 3, .instanceCount = 1, .firstVertex = 0, .firstInstance = 0});
		context.commandBuffer->endEncoding();

		context.commandBuffer->endEvent();
	}
} // namespace demo
