#include "GPUDrivenGBufferPass.h"

#include "../ArgumentTables.h"

#include "../ClipSpaceConvention.h"
#include "../GPUDrivenRenderer.h"
#include "../DrawStreamRecorder.h"
#include "../MeshPool.h"
#include "../PassExecutor.h"
#include "../../shaders/shader_io.h"

#include <array>
#include <cstddef>
#include <cstring>

namespace demo
{
	GPUDrivenGBufferPass::GPUDrivenGBufferPass(GPUDrivenRenderer* renderer)
		: m_renderer(renderer)
	{
	}

	PassNode::HandleSlice<PassResourceDependency> GPUDrivenGBufferPass::getDependencies() const
	{
		static const std::array<PassResourceDependency, 5> dependencies = {
			PassResourceDependency::buffer(kPassVertexBufferHandle, ResourceAccess::read, rhi::ShaderStage::vertex),
			PassResourceDependency::texture(kPassGBuffer0Handle, ResourceAccess::write, rhi::ShaderStage::fragment,
			                                rhi::ResourceState::ColorAttachment),
			PassResourceDependency::texture(kPassGBuffer1Handle, ResourceAccess::write, rhi::ShaderStage::fragment,
			                                rhi::ResourceState::ColorAttachment),
			PassResourceDependency::texture(kPassGBuffer2Handle, ResourceAccess::write, rhi::ShaderStage::fragment,
			                                rhi::ResourceState::ColorAttachment),
			PassResourceDependency::texture(kPassSceneDepthHandle, ResourceAccess::write, rhi::ShaderStage::fragment,
			                                rhi::ResourceState::DepthStencilAttachment),
		};
		return {dependencies.data(), static_cast<uint32_t>(dependencies.size())};
	}

	void GPUDrivenGBufferPass::execute(const PassContext& context) const
	{
		if (m_renderer == nullptr || context.params == nullptr || context.transientAllocator == nullptr
			|| context.commandBuffer == nullptr || context.executor == nullptr)
		{
			return;
		}

		context.commandBuffer->beginEvent("GPUDrivenGBufferPass");

		const GPUDrivenSceneView* sceneView = context.params->gpuDrivenSceneView;
		if (sceneView == nullptr || sceneView->sceneDepthView.isNull())
		{
			context.commandBuffer->endEvent();
			return;
		}
		const rhi::Extent2D extent = {sceneView->sceneDepthExtent.width, sceneView->sceneDepthExtent.height};

		std::array<rhi::RenderTargetDesc, kPackedGBufferTargetCount> colorTargets{};
		for (uint32_t i = 0; i < kPackedGBufferTargetCount; ++i)
		{
			colorTargets[i] = {
				.texture = {},
				.view = sceneView->gbufferViews[i],
				.state = rhi::ResourceState::ColorAttachment,
				.loadOp = rhi::LoadOp::clear,
				.storeOp = rhi::StoreOp::store,
				.clearColor = {0.0f, 0.0f, 0.0f, 0.0f},
			};
		}

		// The previous-visible depth prepass is only a bootstrap for the Hi-Z/culling
		// passes that have already completed. Rebuild the final scene depth from the
		// authoritative current-visible stream so GBuffer and lighting consume the same
		// pose/visibility set. Clearing to 0 is the reverse-Z far value and prevents stale
		// bootstrap depth from rejecting newly exposed current-visible surfaces.
		const rhi::DepthTargetDesc depthTarget{
			.texture = {},
			.view = sceneView->sceneDepthView,
			.state = rhi::ResourceState::DepthStencilAttachment,
			.loadOp = rhi::LoadOp::clear,
			.storeOp = rhi::StoreOp::store,
			.clearValue = {0.0f, 0},
		};

		rhi::BufferHandle sortedIndirectBuffer{};
		rhi::BufferHandle sortedCountBuffer{};
		uint32_t sortedOpaqueCapacity = 0;
		uint32_t sortedAlphaCapacity = 0;
		const uint32_t currentIndirectObjectCount = m_renderer->getGPUCullingObjectCount(context.frameIndex);
		const rhi::BufferHandle currentRawIndirectBuffer =
			m_renderer->getGPUCullingIndirectBufferRHIHandle(context.frameIndex);
		const rhi::BufferHandle currentRawCountBuffer =
			m_renderer->getGPUCullingDrawCountBufferRHIHandle(context.frameIndex);
		// Keep this predicate aligned with GPUDrivenCullingPass::execute. Publishing
		// is CPU metadata for the next frame, so it is valid only when this command
		// buffer actually records the current topology's culling dispatch.
		const bool currentRawCullingProduced =
			context.params->cameraUniforms != nullptr
			&& !m_renderer->getGPUCullingPipelineHandle().isNull()
			&& !m_renderer->getGPUCullingArgumentTable(context.frameIndex).isNull()
			&& currentIndirectObjectCount > 0u
			&& !currentRawIndirectBuffer.isNull()
			&& !currentRawCountBuffer.isNull();
		if (currentRawCullingProduced)
		{
			m_renderer->publishRawCullingBootstrapStateForFrame(context.frameIndex, currentIndirectObjectCount);
		}

		if (context.drawStream != nullptr && currentRawCullingProduced)
		{
			sortedCountBuffer = currentRawCountBuffer;
			sortedOpaqueCapacity = static_cast<uint32_t>(m_renderer->getOpaqueDrawIndices().size());
			sortedAlphaCapacity = static_cast<uint32_t>(m_renderer->getAlphaTestDrawIndices().size());
			const uint32_t transparentCapacity = static_cast<uint32_t>(m_renderer->getTransparentDrawIndices().size());
			const uint32_t totalSortedCapacity = sortedOpaqueCapacity + sortedAlphaCapacity + transparentCapacity;
			if (!sortedCountBuffer.isNull() && totalSortedCapacity > 0u)
			{
				m_renderer->ensureGPUDrivenPersistentIndirectStream(context.frameIndex, totalSortedCapacity);
				const rhi::BufferHandle persistentIndirectBuffer = m_renderer->getGPUDrivenPersistentIndirectStreamBufferRHIHandle(
					context.frameIndex);
				if (!persistentIndirectBuffer.isNull())
				{
					const bool opaquePatched = sortedOpaqueCapacity == 0u
						|| m_renderer->prepareAndDispatchVisibilityPatch(*context.commandBuffer,
						                                                 context.frameIndex,
						                                                 persistentIndirectBuffer,
						                                                 0x00000000u,
						                                                 0u);
					const bool alphaPatched = sortedAlphaCapacity == 0u
						|| m_renderer->prepareAndDispatchVisibilityPatch(*context.commandBuffer,
						                                                 context.frameIndex,
						                                                 persistentIndirectBuffer,
						                                                 0x40000000u,
						                                                 sortedOpaqueCapacity);
					if (opaquePatched && alphaPatched)
					{
						sortedIndirectBuffer = persistentIndirectBuffer;
						m_renderer->publishSortedBootstrapStateForFrame(context.frameIndex, sortedOpaqueCapacity,
						                                                sortedAlphaCapacity);
					}
				}
			}
			m_renderer->recordGBufferVisibilityPatch(!sortedIndirectBuffer.isNull(),
			                                         sortedOpaqueCapacity,
			                                         sortedAlphaCapacity);
		}

		const rhi::RenderPassDesc passDesc{
			.renderArea = {{0, 0}, extent},
			.colorTargets = colorTargets,
			.depthTarget = &depthTarget,
		};
		rhi::RenderEncoder* enc = context.commandBuffer->beginRenderPass(passDesc);
		enc->setViewport(rhi::Viewport{
			0.0f, 0.0f, static_cast<float>(extent.width), static_cast<float>(extent.height), 0.0f, 1.0f
		});
		enc->setScissor(rhi::Rect2D{{0, 0}, extent});

		if (context.drawStream != nullptr)
		{
			MeshPool& meshPool = m_renderer->getMeshPool();
			const rhi::BufferHandle indirectBuffer = !sortedIndirectBuffer.isNull()
				                                         ? sortedIndirectBuffer
				                                         : (currentRawCullingProduced
					                                         ? currentRawIndirectBuffer
					                                         : rhi::BufferHandle{});
			const rhi::BufferHandle countBuffer = !sortedCountBuffer.isNull()
				                                      ? sortedCountBuffer
				                                      : (currentRawCullingProduced
					                                      ? currentRawCountBuffer
					                                      : rhi::BufferHandle{});
			const uint32_t indirectCommandStride = m_renderer->getGPUCullingIndirectCommandStride();

			if (!context.cameraAllocValid)
			{
				context.commandBuffer->endEncoding();
				context.commandBuffer->endEvent();
				return;
			}
			const TransientAllocator::Allocation& cameraAlloc = context.cameraAlloc;

			const rhi::ArgumentTableHandle cameraTable = m_renderer->getCameraArgumentTable(context.frameIndex);
			const rhi::ArgumentTableHandle drawTable = m_renderer->getDrawArgumentTable(context.frameIndex);

			if (!indirectBuffer.isNull() && !drawTable.isNull())
			{
				const bool useMdi = !indirectBuffer.isNull() && !m_renderer->getGBufferMDIDrawArgumentTable(
					context.frameIndex).isNull();
				if (useMdi)
				{
					const rhi::ArgumentTableHandle mdiDrawTable = m_renderer->getGBufferMDIDrawArgumentTable(
						context.frameIndex);

					const auto pickRepresentativeMesh = [&]() -> const MeshRecord*
					{
						for (uint32_t drawIndex : m_renderer->getOpaqueDrawIndices())
						{
							MeshHandle meshHandle = kNullMeshHandle;
							if (m_renderer->tryGetMeshHandleForDrawIndex(drawIndex, meshHandle))
							{
								if (const MeshRecord* mesh = meshPool.tryGet(meshHandle))
								{
									return mesh;
								}
							}
						}
						for (uint32_t drawIndex : m_renderer->getAlphaTestDrawIndices())
						{
							MeshHandle meshHandle = kNullMeshHandle;
							if (m_renderer->tryGetMeshHandleForDrawIndex(drawIndex, meshHandle))
							{
								if (const MeshRecord* mesh = meshPool.tryGet(meshHandle))
								{
									return mesh;
								}
							}
						}
						return nullptr;
					};

					const MeshRecord* representativeMesh = pickRepresentativeMesh();
					if (representativeMesh == nullptr)
					{
						context.commandBuffer->endEncoding();
						context.commandBuffer->endEvent();
						return;
					}

					const rhi::BufferHandle vertexBufferRHI = meshPool.getSharedVertexBufferRHIHandle();
					const rhi::BufferHandle indexBufferRHI = m_renderer->isMeshletRenderingActive()
						                                         ? m_renderer->getMeshletIndexBufferRHIHandle()
						                                         : meshPool.getSharedIndexBufferRHIHandle();
					if (vertexBufferRHI.isNull() || indexBufferRHI.isNull())
					{
						context.commandBuffer->endEncoding();
						context.commandBuffer->endEvent();
						return;
					}
					const uint64_t vertexOffset = 0;

					const uint64_t opaqueCommandOffset = !sortedIndirectBuffer.isNull()
						                                     ? 0u
						                                     : static_cast<uint64_t>(currentIndirectObjectCount) *
						                                     indirectCommandStride;
					const uint64_t alphaCommandOffset = !sortedIndirectBuffer.isNull()
						                                    ? static_cast<uint64_t>(sortedOpaqueCapacity) *
						                                    indirectCommandStride
						                                    : opaqueCommandOffset * 2u;
					const uint64_t opaqueCountOffset = offsetof(shaderio::GPUCullDrawCounts, opaqueCount);
					const uint64_t alphaCountOffset = offsetof(shaderio::GPUCullDrawCounts, alphaTestCount);
					const uint32_t opaqueMaxDrawCount = !sortedIndirectBuffer.isNull()
						                                    ? sortedOpaqueCapacity
						                                    : currentIndirectObjectCount;
					const uint32_t alphaMaxDrawCount = !sortedIndirectBuffer.isNull()
						                                   ? sortedAlphaCapacity
						                                   : currentIndirectObjectCount;

					// args = sorted persistent stream (when bootstrapped) else culling output; count is the culling draw-count buffer.
					const rhi::BufferHandle indirectBufferRHI = indirectBuffer;
					const rhi::BufferHandle countBufferRHI = countBuffer;

					enc->setPipeline(m_renderer->getGBufferOpaqueMDIPipelineHandle());
					enc->bindVertexBuffer(0, vertexBufferRHI, vertexOffset);
					enc->bindIndexBuffer(indexBufferRHI, 0, rhi::IndexFormat::uint32);
					const rhi::ArgumentTableHandle materialTable = m_renderer->getGraphicsMaterialArgumentTable();
					enc->setArgumentTable(rhi::ShaderStage::fragment, shaderio::LSetTextures, materialTable);
					if (!cameraTable.isNull())
					{
						enc->setDynamicBuffer(rhi::ShaderStage::allGraphics, kSceneDynamicBufferTableSlot, {},
						                      cameraAlloc.offset, 0);
						enc->setDynamicBuffer(rhi::ShaderStage::allGraphics, kSceneDynamicBufferTableSlot, {}, 0, 0);
						enc->setArgumentTable(rhi::ShaderStage::allGraphics, kSceneDynamicBufferTableSlot, cameraTable);
					}
					enc->setArgumentTable(rhi::ShaderStage::allGraphics, shaderio::LSetDraw, mdiDrawTable);
					DrawStreamRecorder::recordIndexedIndirectCount(
						*enc, DrawStreamRecorder::IndexedIndirectCountRecordDesc{
							.argsBuffer = indirectBufferRHI,
							.argsOffset = opaqueCommandOffset,
							.countBuffer = countBufferRHI,
							.countBufferOffset = opaqueCountOffset,
							.maxDrawCount = opaqueMaxDrawCount,
							.stride = indirectCommandStride,
						});

					enc->setPipeline(m_renderer->getGBufferAlphaTestMDIPipelineHandle());
					DrawStreamRecorder::recordIndexedIndirectCount(
						*enc, DrawStreamRecorder::IndexedIndirectCountRecordDesc{
							.argsBuffer = indirectBufferRHI,
							.argsOffset = alphaCommandOffset,
							.countBuffer = countBufferRHI,
							.countBufferOffset = alphaCountOffset,
							.maxDrawCount = alphaMaxDrawCount,
							.stride = indirectCommandStride,
						});
				}
			}
		}

		context.commandBuffer->endEncoding();

		context.commandBuffer->endEvent();
	}
} // namespace demo
