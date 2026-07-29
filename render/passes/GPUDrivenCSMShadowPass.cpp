#include "GPUDrivenCSMShadowPass.h"

#include "../ArgumentTables.h"
#include "../GPUDrivenRenderer.h"
#include "../DrawStreamRecorder.h"
#include "../PassExecutor.h"
#include "../../shaders/shader_io.h"

#include <array>
#include <cstring>

namespace demo
{
	namespace
	{
		rhi::RenderEncoder* beginCascadeDepthPass(rhi::CommandBuffer& commandBuffer,
		                                          const GPUDrivenRenderer& renderer,
		                                          const rhi::Extent2D extent,
		                                          const uint32_t cascadeIndex)
		{
			const rhi::DepthTargetDesc depthTarget{
				.texture = rhi::TextureHandle{kPassCSMShadowHandle.index, kPassCSMShadowHandle.generation},
				.view = renderer.getCSMCascadeViewHandle(cascadeIndex),
				.state = rhi::ResourceState::DepthStencilAttachment,
				.loadOp = rhi::LoadOp::clear,
				.storeOp = rhi::StoreOp::store,
				.clearValue = {0.0f, 0},
			};

			const rhi::RenderPassDesc passDesc{
				.renderArea = {{0, 0}, extent},
				.colorTargets = nullptr,
				.colorTargetCount = 0,
				.depthTarget = &depthTarget,
			};
			return commandBuffer.beginRenderPass(passDesc);
		}

		void recordClearOnlyCascades(rhi::CommandBuffer& commandBuffer,
		                             const GPUDrivenRenderer& renderer,
		                             const rhi::Extent2D extent,
		                             const uint32_t cascadeCount)
		{
			for (uint32_t cascadeIndex = 0; cascadeIndex < cascadeCount; ++cascadeIndex)
			{
				beginCascadeDepthPass(commandBuffer, renderer, extent, cascadeIndex);
				commandBuffer.endEncoding();
			}
		}
	}

	GPUDrivenCSMShadowPass::GPUDrivenCSMShadowPass(GPUDrivenRenderer* renderer)
		: m_renderer(renderer)
	{
	}

	PassNode::HandleSlice<PassResourceDependency> GPUDrivenCSMShadowPass::getDependencies() const
	{
		static const std::array<PassResourceDependency, 1> dependencies = {
			PassResourceDependency::texture(kPassCSMShadowHandle, ResourceAccess::write,
			                                rhi::StageFlags::rasterDepthOut, rhi::HazardFlags::depthStencil,
			                                rhi::ResourceState::DepthStencilAttachment),
		};
		return {dependencies.data(), static_cast<uint32_t>(dependencies.size())};
	}

	void GPUDrivenCSMShadowPass::execute(const PassContext& context) const
	{
		if (m_renderer == nullptr || context.commandBuffer == nullptr)
		{
			return;
		}

		CSMShadowResources& csm = m_renderer->getCSMShadowResources();
		const uint32_t cascadeCount = csm.getCascadeCount();
		const rhi::Extent2D cascadeExtent = csm.getCascadeExtent();
		const uint32_t frameIndex = context.frameIndex;
		const GPUDrivenSceneView* sceneView =
			context.params != nullptr ? context.params->gpuDrivenSceneView : nullptr;
		const uint32_t shadowMeshCount = sceneView != nullptr ? sceneView->shadowPackedMeshCount : 0u;
		const bool hasPassInputs = context.params != nullptr && context.transientAllocator != nullptr
			&& context.executor != nullptr;
		const bool hasShadowCasters = sceneView != nullptr && sceneView->usePersistentCullingObjects
			&& sceneView->shadowPackedMeshes != nullptr && shadowMeshCount > 0u
			&& sceneView->shadowPackedVertexBuffer != 0 && sceneView->shadowPackedIndexBuffer != 0;
		shaderio::ShadowUniforms* shadowData = m_renderer->getShadowUniformsData();
		const PipelineHandle csmPipeline = m_renderer->getCSMShadowPipelineHandle();
		const PipelineHandle computePipeline = m_renderer->getShadowCullingPipelineHandle();
		const rhi::ArgumentTableHandle computeTable = m_renderer->getShadowCullingArgumentTable(frameIndex);
		const rhi::ArgumentTableHandle materialTable = m_renderer->getGraphicsMaterialArgumentTable();
		const rhi::ArgumentTableHandle cameraTable = m_renderer->getCameraArgumentTable(frameIndex);
		const uint64_t shadowIndirectBuffer = m_renderer->getShadowCullingIndirectBufferOpaque(frameIndex);
		const rhi::BufferHandle shadowIndirectBufferRHI =
			m_renderer->getShadowCullingIndirectBufferRHIHandle(frameIndex);
		const rhi::BufferHandle vertexBufferRHI = m_renderer->getShadowPackedVertexBufferRHIHandle();
		const rhi::BufferHandle indexBufferRHI = m_renderer->getShadowPackedIndexBufferRHIHandle();
		const uint32_t shadowIndirectCapacity = m_renderer->getShadowCullingMeshCapacity(frameIndex);
		std::array<rhi::ArgumentTableHandle, shaderio::LCascadeCount> drawTables{};
		bool hasDrawArgumentTables = cascadeCount > 0u;
		for (uint32_t cascadeIndex = 0; cascadeIndex < cascadeCount; ++cascadeIndex)
		{
			drawTables[cascadeIndex] = m_renderer->getCSMShadowMDIDrawArgumentTable(frameIndex, cascadeIndex);
			hasDrawArgumentTables = hasDrawArgumentTables && !drawTables[cascadeIndex].isNull();
		}

		const bool hasShadowIndirectBuffer = shadowIndirectBuffer != 0 && shadowIndirectBufferRHI.isValid();
		const bool hasGeometryBuffers = vertexBufferRHI.isValid() && indexBufferRHI.isValid();
		const bool canDrawShadows = hasPassInputs && hasShadowCasters && shadowData != nullptr
			&& !csmPipeline.isNull() && !computePipeline.isNull() && !computeTable.isNull()
			&& !materialTable.isNull() && !cameraTable.isNull() && hasDrawArgumentTables
			&& hasShadowIndirectBuffer && hasGeometryBuffers && shadowIndirectCapacity >= shadowMeshCount;

		context.commandBuffer->beginEvent("GPUDrivenCSMShadow");
		const bool hasAutomationDebugMarker =
			context.params != nullptr && !context.params->automationDebugMarker.empty();
		if (hasAutomationDebugMarker)
		{
			context.commandBuffer->beginEvent(context.params->automationDebugMarker.c_str());
		}
		if (!canDrawShadows)
		{
			if (hasPassInputs && hasShadowCasters && hasShadowIndirectBuffer && hasDrawArgumentTables
				&& shadowIndirectCapacity < shadowMeshCount)
			{
				LOGW("Clearing GPUDrivenCSMShadow: indirect capacity %u smaller than shadow mesh count %u",
				     shadowIndirectCapacity,
				     shadowMeshCount);
			}
			else if (hasPassInputs && hasShadowCasters && (computePipeline.isNull() || computeTable.isNull()))
			{
				LOGW("Clearing GPUDrivenCSMShadow: shadow indirect culling pipeline is unavailable");
			}
			recordClearOnlyCascades(*context.commandBuffer, *m_renderer, cascadeExtent, cascadeCount);
			if (hasAutomationDebugMarker)
			{
				context.commandBuffer->endEvent();
			}
			context.commandBuffer->endEvent();
			return;
		}

		const CSMShadowResources::FrameData& csmFrameData = csm.getFrameData();
		for (uint32_t cascadeIndex = 0; cascadeIndex < cascadeCount; ++cascadeIndex)
		{
			if (cascadeIndex > 0u)
			{
				// The previous cascade consumed this frame's indirect arguments at commandInput;
				// wait before current compute overwrites them. The Vulkan readBeforeWrite source
				// access mask also contains shader reads; VkMemoryBarrier2 VUIDs 03905/03906
				// require a shader source stage, so include the already-completed producer compute.
				// The preceding compute->commandInput barrier already orders it before the draw.
				context.commandBuffer->barrier(rhi::StageFlags::commandInput | rhi::StageFlags::compute,
				                               rhi::StageFlags::compute,
				                               rhi::HazardFlags::readBeforeWrite);
			}

			const shaderio::ShadowCullPushConstants pushConstants =
				m_renderer->buildShadowCullPushConstants(cascadeIndex, shadowMeshCount);
			rhi::ComputeEncoder* cenc = context.commandBuffer->beginComputePass();
			cenc->setPipeline(computePipeline);
			cenc->setArgumentTable(0, computeTable);
			cenc->setRootConstants(kPrimaryRootConstantsSlot, &pushConstants, sizeof(pushConstants));
			cenc->dispatch(rhi::DispatchDesc{
				(shadowMeshCount + shaderio::LGPUCullingThreadCount - 1u) / shaderio::LGPUCullingThreadCount,
				1u, 1u
			});
			context.commandBuffer->endEncoding();

			// Same-pass barrier: per-cascade culling writes indirect draw arguments
			// consumed by drawIndexedIndirect below inside the same cascade pass.
			context.commandBuffer->barrier(rhi::StageFlags::compute, rhi::StageFlags::commandInput,
			                               rhi::HazardFlags::drawArguments);

			rhi::RenderEncoder* enc =
				beginCascadeDepthPass(*context.commandBuffer, *m_renderer, cascadeExtent, cascadeIndex);
			enc->setViewport(
				rhi::Viewport{
					0.0f, 0.0f, static_cast<float>(cascadeExtent.width), static_cast<float>(cascadeExtent.height),
					0.0f, 1.0f
				});
			enc->setScissor(rhi::Rect2D{{0, 0}, cascadeExtent});

			enc->setPipeline(csmPipeline);
			enc->setArgumentTable(rhi::ShaderStage::fragment, shaderio::LSetTextures, materialTable);

			shaderio::CameraUniforms cascadeCamera{};
			cascadeCamera.viewProjection = shadowData->cascadeViewProjection[cascadeIndex];
			cascadeCamera.projection = cascadeCamera.viewProjection;
			cascadeCamera.view = glm::mat4(1.0f);
			cascadeCamera.inverseViewProjection = glm::inverse(cascadeCamera.viewProjection);
			cascadeCamera.prevView = cascadeCamera.view;
			cascadeCamera.prevProjection = cascadeCamera.projection;
			cascadeCamera.prevViewProjection = cascadeCamera.viewProjection;
			cascadeCamera.unjitteredViewProjection = cascadeCamera.viewProjection;
			cascadeCamera.unjitteredInverseViewProjection = cascadeCamera.inverseViewProjection;
			cascadeCamera.prevUnjitteredViewProjection = cascadeCamera.viewProjection;
			cascadeCamera.prevJitteredViewProjection = cascadeCamera.viewProjection;
			cascadeCamera.cameraPosition = glm::vec3(0.0f);
			const float casterNormalBias = csmFrameData.normalBiasWorld;
			const glm::vec3 dirToLight = -csmFrameData.lightDirection;
			cascadeCamera.shadowConstantBias = 0.0f; // Receiver depth bias is applied once in LightPass.
			cascadeCamera.shadowDirectionAndSlopeBias = glm::vec4(dirToLight, casterNormalBias);
			const TransientAllocator::Allocation cameraAlloc = context.transientAllocator->allocateAndWrite(
				cascadeCamera, 256);

			enc->setDynamicBuffer(rhi::ShaderStage::allGraphics, kSceneDynamicBufferTableSlot, {},
			                      cameraAlloc.offset, 0);
			enc->setDynamicBuffer(rhi::ShaderStage::allGraphics, kSceneDynamicBufferTableSlot, {}, 0, 0);
			enc->setArgumentTable(rhi::ShaderStage::allGraphics, kSceneDynamicBufferTableSlot, cameraTable);

			const rhi::ArgumentTableHandle drawTable = drawTables[cascadeIndex];
			enc->setArgumentTable(rhi::ShaderStage::allGraphics, shaderio::LSetDraw, drawTable);

			constexpr uint64_t vertexOffset = 0;
			enc->bindVertexBuffers(0, &vertexBufferRHI, &vertexOffset, 1);
			enc->bindIndexBuffer(indexBufferRHI, 0, rhi::IndexFormat::uint32);
			DrawStreamRecorder::recordIndexedIndirect(*enc, DrawStreamRecorder::IndexedIndirectRecordDesc{
				                                          .argsBuffer = shadowIndirectBufferRHI,
				                                          .offset = 0,
				                                          .drawCount = shadowMeshCount,
				                                          .stride = 0,
			                                          });

			context.commandBuffer->endEncoding();
		}

		if (hasAutomationDebugMarker)
		{
			context.commandBuffer->endEvent();
		}
		context.commandBuffer->endEvent();
	}
} // namespace demo
