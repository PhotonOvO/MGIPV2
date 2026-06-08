#include "GPUDrivenVelocityPass.h"

#include "../ArgumentTables.h"

#include "../GPUDrivenRenderer.h"
#include "../PassExecutor.h"
#include "../../shaders/shader_io.h"

#include <array>

namespace demo
{
	GPUDrivenVelocityPass::GPUDrivenVelocityPass(GPUDrivenRenderer* renderer)
		: m_renderer(renderer)
	{
	}

	PassNode::HandleSlice<PassResourceDependency> GPUDrivenVelocityPass::getDependencies() const
	{
		static const std::array<PassResourceDependency, 2> dependencies = {
			PassResourceDependency::texture(kPassSceneDepthHandle, ResourceAccess::read, rhi::ShaderStage::fragment,
			                                rhi::ResourceState::ShaderRead),
			PassResourceDependency::texture(kPassVelocityHandle, ResourceAccess::write, rhi::ShaderStage::fragment,
			                                rhi::ResourceState::ColorAttachment),
		};
		return {dependencies.data(), static_cast<uint32_t>(dependencies.size())};
	}

	void GPUDrivenVelocityPass::execute(const PassContext& context) const
	{
		if (m_renderer == nullptr || context.commandBuffer == nullptr || context.executor == nullptr || context.params
			== nullptr
			|| !context.cameraAllocValid)
		{
			return;
		}

		const GPUDrivenSceneView* sceneView = context.params->gpuDrivenSceneView;
		if (sceneView == nullptr || sceneView->velocityImage.isNull() || sceneView->velocityView.isNull())
		{
			return;
		}

		const rhi::Extent2D extent = m_renderer->getSceneExtent();
		if (extent.width == 0u || extent.height == 0u)
		{
			return;
		}

		context.commandBuffer->beginEvent("GPUDrivenVelocity");
		const rhi::RenderTargetDesc colorTarget{
			.texture = {},
			.view = sceneView->velocityView,
			.state = rhi::ResourceState::ColorAttachment,
			.loadOp = rhi::LoadOp::clear,
			.storeOp = rhi::StoreOp::store,
			.clearColor = {0.0f, 0.0f, 0.0f, 0.0f},
		};
		rhi::RenderEncoder* enc = context.commandBuffer->beginRenderPass(rhi::RenderPassDesc{
			.renderArea = {{0, 0}, extent},
			.colorTargets = &colorTarget,
			.colorTargetCount = 1,
			.depthTarget = nullptr,
		});
		enc->setViewport(rhi::Viewport{
			0.0f, 0.0f, static_cast<float>(extent.width), static_cast<float>(extent.height), 0.0f, 1.0f
		});
		enc->setScissor(rhi::Rect2D{{0, 0}, extent});

		const PipelineHandle pipelineHandle = m_renderer->getVelocityPipelineHandle();
		const rhi::ArgumentTableHandle inputTable = m_renderer->getLightingInputArgumentTable(context.frameIndex);
		const rhi::ArgumentTableHandle sceneTable = m_renderer->getLightingSceneArgumentTable(context.frameIndex);
		if (!pipelineHandle.isNull() && !inputTable.isNull() && !sceneTable.isNull())
		{
			enc->setPipeline(pipelineHandle);
			enc->setArgumentTable(rhi::ShaderStage::fragment, shaderio::LSetTextures, inputTable);
			// LSetScene carries 2 dynamic UBOs (camera + scene); offsets flush in this order.
			enc->setDynamicBuffer(rhi::ShaderStage::allGraphics, kSceneDynamicBufferTableSlot, {},
			                      context.cameraAlloc.offset, 0);
			enc->setDynamicBuffer(rhi::ShaderStage::allGraphics, kSceneDynamicBufferTableSlot, {}, 0, 0);
			enc->setArgumentTable(rhi::ShaderStage::allGraphics, kSceneDynamicBufferTableSlot, sceneTable);
			enc->draw(rhi::DrawDesc{.vertexCount = 3, .instanceCount = 1, .firstVertex = 0, .firstInstance = 0});
		}

		context.commandBuffer->endEncoding();
		context.commandBuffer->endEvent();
	}
} // namespace demo
