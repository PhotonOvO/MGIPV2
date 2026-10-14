#include "GPUDrivenForwardPass.h"

#include "../ArgumentTables.h"

#include "../GPUDrivenRenderer.h"
#include "../DrawStreamRecorder.h"
#include "../RenderDevice.h"
#include "../MeshPool.h"
#include "../PassExecutor.h"
#include "../ClipSpaceConvention.h"
#include "../../shaders/shader_io.h"

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstring>

namespace demo
{
	GPUDrivenForwardPass::GPUDrivenForwardPass(GPUDrivenRenderer* renderer)
		: m_renderer(renderer)
	{
	}

	PassNode::HandleSlice<PassResourceDependency> GPUDrivenForwardPass::getDependencies() const
	{
		static const std::array<PassResourceDependency, 2> dependencies = {
			PassResourceDependency::texture(kPassSceneColorHdrHandle, ResourceAccess::readWrite,
			                                rhi::StageFlags::rasterColorOut, rhi::HazardFlags::textureWrites,
			                                rhi::ResourceState::ColorAttachment),
			PassResourceDependency::texture(kPassSceneDepthHandle, ResourceAccess::read, rhi::ShaderStage::fragment,
			                                rhi::ResourceState::DepthStencilReadOnly),
		};
		return {dependencies.data(), static_cast<uint32_t>(dependencies.size())};
	}

	void GPUDrivenForwardPass::execute(const PassContext& context) const
	{
		if (m_renderer == nullptr || context.params == nullptr || context.transientAllocator == nullptr
			|| context.commandBuffer == nullptr || context.executor == nullptr)
		{
			return;
		}

		context.commandBuffer->beginEvent("GPUDrivenForwardPass");

		const GPUDrivenSceneView* sceneView = context.params->gpuDrivenSceneView;
		if (sceneView == nullptr || sceneView->sceneDepthView.isNull() || sceneView->sceneColorHdrView.isNull())
		{
			context.commandBuffer->endEvent();
			return;
		}

		const rhi::TextureViewHandle outputImageView = sceneView->sceneColorHdrView;
		const rhi::Extent2D renderExtent = sceneView->sceneDepthExtent;
		if (outputImageView.isNull() || renderExtent.width == 0 || renderExtent.height == 0)
		{
			context.commandBuffer->endEvent();
			return;
		}

		MeshPool& meshPool = m_renderer->getMeshPool();
		const uint32_t objectCount = m_renderer->getGPUCullingObjectCount(context.frameIndex);
		const rhi::BufferHandle indirectBuffer = m_renderer->getGPUCullingIndirectBufferRHIHandle(context.frameIndex);
		const rhi::BufferHandle countBuffer = m_renderer->getGPUCullingDrawCountBufferRHIHandle(context.frameIndex);
		if (objectCount == 0 || indirectBuffer.isNull() || countBuffer.isNull())
		{
			context.commandBuffer->endEvent();
			return;
		}

		rhi::RenderTargetDesc colorTarget{
			.texture = {},
			.view = outputImageView,
			.state = rhi::ResourceState::ColorAttachment,
			.loadOp = rhi::LoadOp::load,
			.storeOp = rhi::StoreOp::store,
		};
		const rhi::DepthTargetDesc depthTarget{
			.texture = {},
			.view = sceneView->sceneDepthView,
			.state = rhi::ResourceState::DepthStencilReadOnly,
			.loadOp = rhi::LoadOp::load,
			.storeOp = rhi::StoreOp::store,
			.clearValue = {0.0f, 0},
		};

		const PipelineHandle forwardPipeline = m_renderer->getForwardMDIPipelineHandle();
		if (forwardPipeline.isNull())
		{
			context.commandBuffer->endEvent();
			return;
		}

		if (!context.cameraAllocValid)
		{
			context.commandBuffer->endEvent();
			return;
		}

		const TransientAllocator::Allocation& cameraAlloc = context.cameraAlloc;
		const rhi::ArgumentTableHandle cameraTable = m_renderer->getCameraArgumentTable(context.frameIndex);

		const rhi::ArgumentTableHandle drawTable = m_renderer->getMDIDrawArgumentTable(context.frameIndex);
		if (drawTable.isNull())
		{
			context.commandBuffer->endEvent();
			return;
		}

		const uint32_t transparentCapacity = static_cast<uint32_t>(m_renderer->getTransparentDrawIndices().size());
		if (transparentCapacity == 0u)
		{
			context.commandBuffer->endEvent();
			return;
		}

		const uint32_t opaqueCapacity = static_cast<uint32_t>(m_renderer->getOpaqueDrawIndices().size());
		const uint32_t alphaCapacity = static_cast<uint32_t>(m_renderer->getAlphaTestDrawIndices().size());
		const uint32_t totalPersistentCapacity = opaqueCapacity + alphaCapacity + transparentCapacity;
		m_renderer->ensureGPUDrivenPersistentIndirectStream(context.frameIndex, totalPersistentCapacity);
		const rhi::BufferHandle forwardIndirectBuffer = m_renderer->getGPUDrivenPersistentIndirectStreamBufferRHIHandle(
			context.frameIndex);
		const bool transparentPatched =
			!forwardIndirectBuffer.isNull()
			&& m_renderer->prepareAndDispatchVisibilityPatch(*context.commandBuffer,
			                                                 context.frameIndex,
				                                                 forwardIndirectBuffer,
				                                                 0x80000000u,
				                                                 opaqueCapacity + alphaCapacity,
				                                                 transparentCapacity);
		m_renderer->recordForwardVisibilityPatch(transparentPatched, transparentCapacity, totalPersistentCapacity);
		if (!transparentPatched)
		{
			context.commandBuffer->endEvent();
			return;
		}

		const auto transparentDrawIndices = m_renderer->getTransparentDrawIndices();
		const MeshRecord* representativeMesh = nullptr;
		for (uint32_t drawIndex : transparentDrawIndices)
		{
			MeshHandle meshHandle = kNullMeshHandle;
			if (m_renderer->tryGetMeshHandleForDrawIndex(drawIndex, meshHandle))
			{
				representativeMesh = meshPool.tryGet(meshHandle);
				if (representativeMesh != nullptr)
				{
					break;
				}
			}
		}

		const rhi::BufferHandle vertexBuffer = meshPool.getSharedVertexBufferRHIHandle();
		const rhi::BufferHandle indexBuffer = m_renderer->isMeshletRenderingActive()
			                                      ? m_renderer->getMeshletIndexBufferRHIHandle()
			                                      : meshPool.getSharedIndexBufferRHIHandle();
		const uint32_t indirectCommandStride = m_renderer->getGPUCullingIndirectCommandStride();
		const uint64_t transparentCommandOffset =
			static_cast<uint64_t>(opaqueCapacity + alphaCapacity) * indirectCommandStride;
		const rhi::DrawIndirectCountDesc transparentDraw{
			.argsBuffer = forwardIndirectBuffer,
			.argsOffset = transparentCommandOffset,
			.countBuffer = countBuffer,
			.countBufferOffset = offsetof(shaderio::GPUCullDrawCounts, transparentCount),
			.maxDrawCount = transparentCapacity,
			.stride = indirectCommandStride,
		};
		const bool drawReady = representativeMesh != nullptr && !vertexBuffer.isNull() && !indexBuffer.isNull();
		if (drawReady)
		{
			context.commandBuffer->prepareIndexedIndirectCount(rhi::PrepareIndexedIndirectCountDesc{
				.draw = transparentDraw,
				.indexBuffer = indexBuffer,
				.indexBufferOffset = 0,
				.indexFormat = rhi::IndexFormat::uint32,
				.topology = rhi::PrimitiveTopology::triangleList,
			});
		}

		const rhi::RenderPassDesc passDesc{
			.renderArea = {{0, 0}, renderExtent},
			.colorTargets = std::span{&colorTarget, 1},
			.depthTarget = &depthTarget,
		};
		rhi::RenderEncoder* enc = context.commandBuffer->beginRenderPass(passDesc);
		enc->setViewport(rhi::Viewport{
			0.0f, 0.0f, static_cast<float>(renderExtent.width), static_cast<float>(renderExtent.height), 0.0f, 1.0f
		});
		enc->setScissor(rhi::Rect2D{{0, 0}, renderExtent});

		// Pipeline + descriptor binds reordered after beginRenderPass (RenderEncoder owns them).
		enc->setPipeline(forwardPipeline);
		const rhi::ArgumentTableHandle materialTable = m_renderer->getGraphicsMaterialArgumentTable();
		enc->setArgumentTable(rhi::ShaderStage::fragment, shaderio::LSetTextures, materialTable);
		if (!cameraTable.isNull())
		{
			enc->setDynamicBuffer(rhi::ShaderStage::allGraphics, kSceneDynamicBufferTableSlot, {}, cameraAlloc.offset,
			                      0);
			enc->setDynamicBuffer(rhi::ShaderStage::allGraphics, kSceneDynamicBufferTableSlot, {}, 0, 0);
			enc->setArgumentTable(rhi::ShaderStage::allGraphics, kSceneDynamicBufferTableSlot, cameraTable);
		}
		enc->setArgumentTable(rhi::ShaderStage::allGraphics, shaderio::LSetDraw, drawTable);

		if (drawReady)
		{
			enc->bindVertexBuffer(0, vertexBuffer, 0);
			enc->bindIndexBuffer(indexBuffer, 0, rhi::IndexFormat::uint32);

			// Transparent pass uses the current-frame persistent indirect stream + culling counts.
			DrawStreamRecorder::recordIndexedIndirectCount(*enc, transparentDraw);
		}

		context.commandBuffer->endEncoding();
		context.commandBuffer->endEvent();
	}
} // namespace demo
