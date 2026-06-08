#include "GPUDrivenDebugPass.h"

#include "../ArgumentTables.h"
#include "../GPUDrivenRenderer.h"
#include "../../shaders/shader_io.h"

#include <array>
#include <cstring>
#include <vector>

namespace demo
{
	namespace
	{
		constexpr uint32_t kDebugCullSegmentCount = 24u;
	} // namespace

	GPUDrivenDebugPass::GPUDrivenDebugPass(GPUDrivenRenderer* renderer)
		: m_renderer(renderer)
	{
	}

	PassNode::HandleSlice<PassResourceDependency> GPUDrivenDebugPass::getDependencies() const
	{
		static const std::array<PassResourceDependency, 3> dependencies = {
			PassResourceDependency::buffer(kPassGPUCullObjectBufferHandle, ResourceAccess::read,
			                               rhi::ShaderStage::vertex),
			PassResourceDependency::buffer(kPassGPUCullResultBufferHandle, ResourceAccess::read,
			                               rhi::ShaderStage::vertex),
			PassResourceDependency::texture(kPassOutputHandle, ResourceAccess::write, rhi::ShaderStage::fragment,
			                                rhi::ResourceState::ColorAttachment),
		};
		return {dependencies.data(), static_cast<uint32_t>(dependencies.size())};
	}

	void GPUDrivenDebugPass::execute(const PassContext& context) const
	{
		if (m_renderer != nullptr && context.commandBuffer != nullptr && context.params != nullptr && context.
			transientAllocator != nullptr)
		{
			// renderDebugOverlay(context);
		}
	}

	void GPUDrivenDebugPass::renderDebugOverlay(const PassContext& context) const
	{
		if (m_renderer == nullptr)
		{
			return;
		}

		const uint32_t safeObjectCount = m_renderer->getSafePersistentObjectCount();
		if (context.params == nullptr || context.transientAllocator == nullptr || !context.params->debugOptions.enabled)
		{
			return;
		}

		const GPUDrivenSceneView* sceneView = context.params->gpuDrivenSceneView;
		if (sceneView == nullptr)
		{
			return;
		}

		const std::vector<shaderio::DebugLineVertex>& debugVertices = m_renderer->getDebugLineVertices();
		// Debug pass only; not on draw-heavy recording path.
		const bool hasLineDebug = !debugVertices.empty();
		const bool hasGPUCullingDebug =
			context.params->debugOptions.showGPUCullingOverlay && safeObjectCount > 0u
			&& !m_renderer->getGPUCullingDebugPipelineHandle().isNull()
			&& m_renderer->getGPUCullingObjectBufferAddress(context.frameIndex) != 0
			&& m_renderer->getGPUCullingResultBufferAddress(context.frameIndex) != 0;
		if (!hasLineDebug && !hasGPUCullingDebug)
		{
			return;
		}

		if (context.commandBuffer == nullptr)
		{
			return;
		}

		context.commandBuffer->beginEvent("GPUDrivenDebug");

		const rhi::Extent2D extent{sceneView->sceneDepthExtent.width, sceneView->sceneDepthExtent.height};
		rhi::RenderTargetDesc colorTarget{
			.texture = {},
			.view = sceneView->outputView,
			.state = rhi::ResourceState::ColorAttachment,
			.loadOp = rhi::LoadOp::load,
			.storeOp = rhi::StoreOp::store,
			.clearColor = {0.0f, 0.0f, 0.0f, 1.0f},
		};

		rhi::RenderEncoder* enc = context.commandBuffer->beginRenderPass(rhi::RenderPassDesc{
			.renderArea = {{0, 0}, extent},
			.colorTargets = &colorTarget,
			.colorTargetCount = 1,
			.depthTarget = nullptr,
		});
		enc->setViewport(
			rhi::Viewport{0.0f, 0.0f, static_cast<float>(extent.width), static_cast<float>(extent.height), 0.0f, 1.0f});
		enc->setScissor(rhi::Rect2D{{0, 0}, extent});

		const PipelineHandle debugPipeline = m_renderer->getDebugPipelineHandle();
		const PipelineHandle gpuCullingDebugPipeline = m_renderer->getGPUCullingDebugPipelineHandle();
		if ((hasLineDebug && debugPipeline.isNull()) || (hasGPUCullingDebug && gpuCullingDebugPipeline.isNull()))
		{
			context.commandBuffer->endEncoding();
			context.commandBuffer->endEvent();
			return;
		}

		if (!context.cameraAllocValid)
		{
			context.commandBuffer->endEncoding();
			context.commandBuffer->endEvent();
			return;
		}

		const TransientAllocator::Allocation& cameraAlloc = context.cameraAlloc;
		const rhi::ArgumentTableHandle cameraArgumentTable = m_renderer->getCameraArgumentTable(context.frameIndex);
		const rhi::BufferHandle transientBuffer = m_renderer->getTransientBufferHandle(context.frameIndex);

		if (hasLineDebug)
		{
			enc->setPipeline(debugPipeline);
			if (!cameraArgumentTable.isNull())
			{
				enc->setDynamicBuffer(rhi::ShaderStage::allGraphics, kSceneDynamicBufferTableSlot, transientBuffer,
				                      cameraAlloc.offset, 0);
				enc->setDynamicBuffer(rhi::ShaderStage::allGraphics, kSceneDynamicBufferTableSlot, transientBuffer, 0,
				                      0);
				enc->setArgumentTable(rhi::ShaderStage::allGraphics, kSceneDynamicBufferTableSlot, cameraArgumentTable);
			}

			const uint32_t vertexDataSize = static_cast<uint32_t>(debugVertices.size() * sizeof(
				shaderio::DebugLineVertex));
			const TransientAllocator::Allocation vertexAlloc =
				context.transientAllocator->allocate(vertexDataSize, alignof(shaderio::DebugLineVertex));
			std::memcpy(vertexAlloc.cpuPtr, debugVertices.data(), vertexDataSize);
			context.transientAllocator->flushAllocation(vertexAlloc, vertexDataSize);

			const uint64_t vertexOffset = vertexAlloc.offset;
			enc->bindVertexBuffers(0, &transientBuffer, &vertexOffset, 1);
			enc->draw(rhi::DrawDesc{
				.vertexCount = static_cast<uint32_t>(debugVertices.size()), .instanceCount = 1, .firstVertex = 0,
				.firstInstance = 0
			});
		}

		if (hasGPUCullingDebug)
		{
			enc->setPipeline(gpuCullingDebugPipeline);
			if (!cameraArgumentTable.isNull())
			{
				enc->setDynamicBuffer(rhi::ShaderStage::allGraphics, kSceneDynamicBufferTableSlot, transientBuffer,
				                      cameraAlloc.offset, 0);
				enc->setDynamicBuffer(rhi::ShaderStage::allGraphics, kSceneDynamicBufferTableSlot, transientBuffer, 0,
				                      0);
				enc->setArgumentTable(rhi::ShaderStage::allGraphics, kSceneDynamicBufferTableSlot, cameraArgumentTable);
			}

			const shaderio::PushConstantGPUCullDebug pushValues{
				.objectBufferAddress = m_renderer->getGPUCullingObjectBufferAddress(context.frameIndex),
				.resultBufferAddress = m_renderer->getGPUCullingResultBufferAddress(context.frameIndex),
				.objectCount = safeObjectCount,
				.segmentCount = kDebugCullSegmentCount,
				._padding0 = 0u,
				._padding1 = 0u,
			};
			enc->setRootConstants(rhi::ShaderStage::vertex, kPrimaryRootConstantsSlot, &pushValues, sizeof(pushValues));
			enc->draw(rhi::DrawDesc{
				.vertexCount = pushValues.segmentCount * 2u * 3u, .instanceCount = pushValues.objectCount,
				.firstVertex = 0, .firstInstance = 0
			});
		}

		context.commandBuffer->endEncoding();
		context.commandBuffer->endEvent();
	}
} // namespace demo
