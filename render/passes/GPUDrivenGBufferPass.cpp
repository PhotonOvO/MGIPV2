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
		// GPU culling already compacts opaque and alpha commands into the N and 2N
		// partitions. Material-order sorting has no correctness value for these passes;
		// only transparent draws retain the sorted visibility-patch path.
		const bool useCurrentRawStream = context.drawStream != nullptr && currentRawCullingProduced;
		m_renderer->recordGBufferRawVisibilitySource(useCurrentRawStream, currentIndirectObjectCount);

		bool drawReady = false;
		bool directDrawReady = m_renderer->hasActiveSceneTransactionState() && context.cameraAllocValid;
		rhi::BufferHandle vertexBuffer{};
		rhi::BufferHandle indexBuffer{};
		rhi::ArgumentTableHandle cameraTable{};
		rhi::ArgumentTableHandle mdiDrawTable{};
		rhi::DrawIndirectCountDesc opaqueDraw{};
		rhi::DrawIndirectCountDesc alphaDraw{};
		if (context.drawStream != nullptr && context.cameraAllocValid)
		{
			MeshPool& meshPool = m_renderer->getMeshPool();
			const rhi::BufferHandle indirectBuffer =
				useCurrentRawStream ? currentRawIndirectBuffer : rhi::BufferHandle{};
			const rhi::BufferHandle countBuffer =
				useCurrentRawStream ? currentRawCountBuffer : rhi::BufferHandle{};
			const uint32_t indirectCommandStride = m_renderer->getGPUCullingIndirectCommandStride();
			cameraTable = m_renderer->getCameraArgumentTable(context.frameIndex);
			const rhi::ArgumentTableHandle drawTable = m_renderer->getDrawArgumentTable(context.frameIndex);
			mdiDrawTable = m_renderer->getGBufferMDIDrawArgumentTable(context.frameIndex);
			if (!indirectBuffer.isNull() && !countBuffer.isNull() && !drawTable.isNull() && !mdiDrawTable.isNull())
			{
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

				vertexBuffer = meshPool.getSharedVertexBufferRHIHandle();
				indexBuffer = m_renderer->isMeshletRenderingActive()
					                  ? m_renderer->getMeshletIndexBufferRHIHandle()
					                  : meshPool.getSharedIndexBufferRHIHandle();
				if (!directDrawReady && pickRepresentativeMesh() != nullptr && !vertexBuffer.isNull() && !indexBuffer.isNull())
				{
					const uint64_t opaqueCommandOffset =
						static_cast<uint64_t>(currentIndirectObjectCount) * indirectCommandStride;
					const uint64_t alphaCommandOffset = opaqueCommandOffset * 2u;
					opaqueDraw = rhi::DrawIndirectCountDesc{
						.argsBuffer = indirectBuffer,
						.argsOffset = opaqueCommandOffset,
						.countBuffer = countBuffer,
						.countBufferOffset = offsetof(shaderio::GPUCullDrawCounts, opaqueCount),
						.maxDrawCount = currentIndirectObjectCount,
						.stride = indirectCommandStride,
					};
					alphaDraw = rhi::DrawIndirectCountDesc{
						.argsBuffer = indirectBuffer,
						.argsOffset = alphaCommandOffset,
						.countBuffer = countBuffer,
						.countBufferOffset = offsetof(shaderio::GPUCullDrawCounts, alphaTestCount),
						.maxDrawCount = currentIndirectObjectCount,
						.stride = indirectCommandStride,
					};
					context.commandBuffer->prepareIndexedIndirectCount(rhi::PrepareIndexedIndirectCountDesc{
						.draw = opaqueDraw,
						.indexBuffer = indexBuffer,
						.indexBufferOffset = 0,
						.indexFormat = rhi::IndexFormat::uint32,
						.topology = rhi::PrimitiveTopology::triangleList,
					});
					context.commandBuffer->prepareIndexedIndirectCount(rhi::PrepareIndexedIndirectCountDesc{
						.draw = alphaDraw,
						.indexBuffer = indexBuffer,
						.indexBufferOffset = 0,
						.indexFormat = rhi::IndexFormat::uint32,
						.topology = rhi::PrimitiveTopology::triangleList,
					});
					drawReady = true;
				}
			}
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
		if (directDrawReady)
		{
			const rhi::ArgumentTableHandle cameraTable = m_renderer->getCameraArgumentTable(context.frameIndex);
			const rhi::ArgumentTableHandle drawTable = m_renderer->getDrawArgumentTable(context.frameIndex);
			const TransientAllocator::Allocation& cameraAlloc = context.cameraAlloc;
				const auto drawRecords = m_renderer->getActiveSceneTransactionDrawRecords();
				const auto drawUniforms = m_renderer->getActivePersistentDrawUniforms();
				const rhi::BufferHandle transientBuffer = context.transientAllocator->getBufferHandle();
				const rhi::ArgumentTableHandle materialTable = m_renderer->getGraphicsMaterialArgumentTable();
				const auto recordDirectDraws = [&](std::span<const uint32_t> indices,
				                                  PipelineHandle pipeline)
				{
					if (pipeline.isNull() || materialTable.isNull() || cameraTable.isNull() ||
						drawTable.isNull() || transientBuffer.isNull())
					{
						return;
					}
					enc->setPipeline(pipeline);
					enc->setArgumentTable(rhi::ShaderStage::fragment, shaderio::LSetTextures,
					                      materialTable);
					enc->setDynamicBuffer(rhi::ShaderStage::allGraphics,
					                      kSceneDynamicBufferTableSlot, {},
					                      cameraAlloc.offset, 0u);
					enc->setDynamicBuffer(rhi::ShaderStage::allGraphics,
					                      kSceneDynamicBufferTableSlot, {}, 0u, 0u);
					enc->setArgumentTable(rhi::ShaderStage::allGraphics,
					                      kSceneDynamicBufferTableSlot, cameraTable);
					rhi::BufferHandle boundVertexBuffer{};
					std::uint64_t boundVertexOffset = 0u;
					rhi::BufferHandle boundIndexBuffer{};
					std::uint64_t boundIndexOffset = 0u;
					for (const uint32_t drawIndex : indices)
					{
						if (drawIndex >= drawRecords.size() || drawIndex >= drawUniforms.size())
						{
							continue;
						}
						const auto& draw = drawRecords[drawIndex];
						const MeshRecord* mesh = m_renderer->tryGetActiveSceneTransactionMesh(
							draw.meshHandle);
						if (mesh == nullptr || mesh->vertexBuffer.isNull() ||
							mesh->indexBuffer.isNull() || mesh->indexCount == 0u)
						{
							continue;
						}
						const TransientAllocator::Allocation drawAlloc =
							context.transientAllocator->allocateAndWrite(drawUniforms[drawIndex], 256u);
						if (drawAlloc.cpuPtr == nullptr || drawAlloc.handle.isNull())
						{
							continue;
						}
						enc->setDynamicBuffer(rhi::ShaderStage::allGraphics,
						                      shaderio::LSetDraw, transientBuffer,
						                      drawAlloc.offset, 0u);
						enc->setArgumentTable(rhi::ShaderStage::allGraphics,
						                      shaderio::LSetDraw, drawTable);
						if (mesh->vertexBuffer != boundVertexBuffer ||
							mesh->vertexBufferOffset != boundVertexOffset)
						{
							enc->bindVertexBuffer(0u, mesh->vertexBuffer,
							                      mesh->vertexBufferOffset);
							boundVertexBuffer = mesh->vertexBuffer;
							boundVertexOffset = mesh->vertexBufferOffset;
						}
						if (mesh->indexBuffer != boundIndexBuffer ||
							mesh->indexBufferOffset != boundIndexOffset)
						{
							enc->bindIndexBuffer(mesh->indexBuffer,
							                     mesh->indexBufferOffset,
							                     rhi::IndexFormat::uint32);
							boundIndexBuffer = mesh->indexBuffer;
							boundIndexOffset = mesh->indexBufferOffset;
						}
						enc->drawIndexed(rhi::DrawIndexedDesc{
							.indexCount = mesh->indexCount,
							.instanceCount = 1u,
							.firstIndex = mesh->firstIndex,
							.vertexOffset = mesh->vertexOffset,
							.firstInstance = 0u,
						});
					}
				};
				recordDirectDraws(m_renderer->getOpaqueDrawIndices(),
				                  m_renderer->getGBufferOpaquePipelineHandle());
				recordDirectDraws(m_renderer->getAlphaTestDrawIndices(),
				                  m_renderer->getGBufferAlphaTestPipelineHandle());
		}
		else if (drawReady)
		{
			enc->setPipeline(m_renderer->getGBufferOpaqueMDIPipelineHandle());
			enc->bindVertexBuffer(0, vertexBuffer, 0);
			enc->bindIndexBuffer(indexBuffer, 0, rhi::IndexFormat::uint32);
			enc->setArgumentTable(rhi::ShaderStage::fragment, shaderio::LSetTextures,
			                      m_renderer->getGraphicsMaterialArgumentTable());
			if (!cameraTable.isNull())
			{
				enc->setDynamicBuffer(rhi::ShaderStage::allGraphics, kSceneDynamicBufferTableSlot, {},
				                      context.cameraAlloc.offset, 0);
				enc->setDynamicBuffer(rhi::ShaderStage::allGraphics, kSceneDynamicBufferTableSlot, {}, 0, 0);
				enc->setArgumentTable(rhi::ShaderStage::allGraphics, kSceneDynamicBufferTableSlot, cameraTable);
			}
			enc->setArgumentTable(rhi::ShaderStage::allGraphics, shaderio::LSetDraw, mdiDrawTable);
			DrawStreamRecorder::recordIndexedIndirectCount(*enc, opaqueDraw);

			enc->setPipeline(m_renderer->getGBufferAlphaTestMDIPipelineHandle());
			DrawStreamRecorder::recordIndexedIndirectCount(*enc, alphaDraw);
		}

		context.commandBuffer->endEncoding();

		context.commandBuffer->endEvent();
	}
} // namespace demo
