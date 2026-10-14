#include "GPUDrivenDepthPrepass.h"

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
	GPUDrivenDepthPrepass::GPUDrivenDepthPrepass(GPUDrivenRenderer* renderer)
		: m_renderer(renderer)
	{
	}

	PassNode::HandleSlice<PassResourceDependency> GPUDrivenDepthPrepass::getDependencies() const
	{
		static const std::array<PassResourceDependency, 2> dependencies = {
			PassResourceDependency::buffer(kPassVertexBufferHandle, ResourceAccess::read, rhi::ShaderStage::vertex),
			PassResourceDependency::texture(kPassSceneDepthHandle, ResourceAccess::write, rhi::ShaderStage::fragment,
			                                rhi::ResourceState::DepthStencilAttachment),
		};
		return {dependencies.data(), static_cast<uint32_t>(dependencies.size())};
	}

	void GPUDrivenDepthPrepass::execute(const PassContext& context) const
	{
		if (m_renderer == nullptr || context.params == nullptr || context.transientAllocator == nullptr
			|| context.commandBuffer == nullptr || context.executor == nullptr)
		{
			return;
		}

		context.commandBuffer->beginEvent("GPUDrivenDepthPrepass");

		const GPUDrivenSceneView* sceneView = context.params->gpuDrivenSceneView;
		if (sceneView == nullptr || sceneView->sceneDepthView.isNull())
		{
			context.commandBuffer->endEvent();
			return;
		}
		const rhi::Extent2D extent = sceneView->sceneDepthExtent;

		const rhi::DepthTargetDesc depthTarget{
			.texture = {},
			.view = sceneView->sceneDepthView,
			.state = rhi::ResourceState::DepthStencilAttachment,
			.loadOp = rhi::LoadOp::clear,
			.storeOp = rhi::StoreOp::store,
			.clearValue = {0.0f, 0},
		};

		const rhi::RenderPassDesc passDesc{
			.renderArea = {{0, 0}, extent},
			.colorTargets = {},
			.depthTarget = &depthTarget,
		};
		rhi::RenderEncoder* enc = context.commandBuffer->beginRenderPass(passDesc);
		enc->setViewport(rhi::Viewport{
			0.0f, 0.0f, static_cast<float>(extent.width), static_cast<float>(extent.height), 0.0f, 1.0f
		});
		enc->setScissor(rhi::Rect2D{{0, 0}, extent});

		if (context.drawStream == nullptr)
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

		const rhi::ArgumentTableHandle cameraTable = m_renderer->getCameraArgumentTable(context.frameIndex);
		const rhi::ArgumentTableHandle drawTable = m_renderer->getDrawArgumentTable(context.frameIndex);
		MeshPool& meshPool = m_renderer->getMeshPool();

		if (m_renderer->hasActiveSceneTransactionState())
		{
			// Transaction generations use private per-mesh buffers. Record a direct
			// depth draw for each occurrence instead of attempting to bind the legacy
			// shared-arena MDI stream.
			const auto drawRecords = m_renderer->getActiveSceneTransactionDrawRecords();
			const auto drawUniforms = m_renderer->getActivePersistentDrawUniforms();
			const rhi::BufferHandle transientBuffer = context.transientAllocator->getBufferHandle();
			const rhi::ArgumentTableHandle materialTable = m_renderer->getGraphicsMaterialArgumentTable();
			const auto recordDirectDepth = [&](std::span<const uint32_t> indices,
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
			recordDirectDepth(m_renderer->getOpaqueDrawIndices(),
			                  m_renderer->getDepthPrepassOpaquePipelineHandle());
			recordDirectDepth(m_renderer->getAlphaTestDrawIndices(),
			                  m_renderer->getDepthPrepassAlphaTestPipelineHandle());
			context.commandBuffer->endEncoding();
			context.commandBuffer->endEvent();
			return;
		}

		const GPUDrivenRenderer::PreviousRawCullingBootstrap previousRawBootstrap =
			m_renderer->getPreviousRawCullingBootstrap(context.frameIndex);
		uint32_t previousOpaqueCapacity = 0u;
		uint32_t previousAlphaCapacity = 0u;
		const bool previousSortedBootstrapValid =
			previousRawBootstrap.valid
			&& m_renderer->getPreviousSortedBootstrapState(context.frameIndex, previousOpaqueCapacity,
			                                               previousAlphaCapacity);
		const rhi::BufferHandle previousBootstrapIndirectBuffer = previousSortedBootstrapValid
			                                                             ? m_renderer->
			                                                             getPreviousGPUDrivenPersistentIndirectStreamBufferRHIHandle(
				                                                             context.frameIndex)
			                                                             : rhi::BufferHandle{};
		const rhi::BufferHandle indirectBuffer = !previousBootstrapIndirectBuffer.isNull()
			                                         ? previousBootstrapIndirectBuffer
			                                         : previousRawBootstrap.indirectBuffer;
		const rhi::BufferHandle countBuffer = previousRawBootstrap.countBuffer;
		const uint32_t previousIndirectObjectCount = previousRawBootstrap.objectCount;
		const uint32_t indirectCommandStride = m_renderer->getGPUCullingIndirectCommandStride();
		if (previousRawBootstrap.valid && !indirectBuffer.isNull() && !countBuffer.isNull()
			&& previousIndirectObjectCount > 0u && !drawTable.isNull())
		{
			const rhi::ArgumentTableHandle mdiDrawTable = m_renderer->getDepthMDIDrawArgumentTable(context.frameIndex);
			if (!mdiDrawTable.isNull())
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

				const MeshRecord* representativeMesh = pickRepresentativeMesh();
				if (representativeMesh == nullptr)
				{
					context.commandBuffer->endEncoding();
					context.commandBuffer->endEvent();
					return;
				}

				// All meshes share the MeshPool vertex/index arenas; bind their stable RHI
				// handles (meshlet path uses the GPUMeshletBuffer index handle).
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

				const uint64_t opaqueCommandOffset = !previousBootstrapIndirectBuffer.isNull()
					                                     ? 0u
					                                     : static_cast<uint64_t>(previousIndirectObjectCount) *
					                                     indirectCommandStride;
				const uint64_t alphaCommandOffset = !previousBootstrapIndirectBuffer.isNull()
					                                    ? static_cast<uint64_t>(previousOpaqueCapacity) *
					                                    indirectCommandStride
					                                    : opaqueCommandOffset * 2u;
				const uint64_t opaqueCountOffset = offsetof(shaderio::GPUCullDrawCounts, opaqueCount);
				const uint64_t alphaCountOffset = offsetof(shaderio::GPUCullDrawCounts, alphaTestCount);
				const uint32_t opaqueMaxDrawCount =
					!previousBootstrapIndirectBuffer.isNull() ? previousOpaqueCapacity : previousIndirectObjectCount;
				const uint32_t alphaMaxDrawCount =
					!previousBootstrapIndirectBuffer.isNull() ? previousAlphaCapacity : previousIndirectObjectCount;
				m_renderer->recordDepthPrepassVisibilitySource(true,
				                                               !previousBootstrapIndirectBuffer.isNull(),
				                                               previousIndirectObjectCount,
				                                               opaqueMaxDrawCount,
				                                               alphaMaxDrawCount);

				// Indirect args + count buffers as stable RHI handles (previous-frame ring),
				// selecting the bootstrap persistent stream or the culling output to match the
				// uint64 path above.
				const rhi::BufferHandle indirectBufferRHI = indirectBuffer;
				const rhi::BufferHandle countBufferRHI = countBuffer;

				enc->setPipeline(m_renderer->getDepthPrepassOpaqueMDIPipelineHandle());
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
				DrawStreamRecorder::recordIndexedIndirectCount(*enc, DrawStreamRecorder::IndexedIndirectCountRecordDesc{
					                                               .argsBuffer = indirectBufferRHI,
					                                               .argsOffset = opaqueCommandOffset,
					                                               .countBuffer = countBufferRHI,
					                                               .countBufferOffset = opaqueCountOffset,
					                                               .maxDrawCount = opaqueMaxDrawCount,
					                                               .stride = indirectCommandStride,
				                                               });

				enc->setPipeline(m_renderer->getDepthPrepassAlphaTestMDIPipelineHandle());
				DrawStreamRecorder::recordIndexedIndirectCount(*enc, DrawStreamRecorder::IndexedIndirectCountRecordDesc{
					                                               .argsBuffer = indirectBufferRHI,
					                                               .argsOffset = alphaCommandOffset,
					                                               .countBuffer = countBufferRHI,
					                                               .countBufferOffset = alphaCountOffset,
					                                               .maxDrawCount = alphaMaxDrawCount,
					                                               .stride = indirectCommandStride,
				                                               });
			}
		}

		context.commandBuffer->endEncoding();
		context.commandBuffer->endEvent();
	}
} // namespace demo
