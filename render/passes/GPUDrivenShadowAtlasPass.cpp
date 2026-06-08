#include "GPUDrivenShadowAtlasPass.h"

#include "../ArgumentTables.h"

#include "../GPUDrivenRenderer.h"
#include "../PassExecutor.h"
#include "../../shaders/shader_io.h"

#include <algorithm>
#include <array>
#include <cstring>

namespace demo
{
	GPUDrivenShadowAtlasPass::GPUDrivenShadowAtlasPass(GPUDrivenRenderer* renderer)
		: m_renderer(renderer)
	{
	}

	PassNode::HandleSlice<PassResourceDependency> GPUDrivenShadowAtlasPass::getDependencies() const
	{
		static const std::array<PassResourceDependency, 1> dependencies = {
			PassResourceDependency::texture(kPassGPUDrivenShadowAtlasHandle,
			                                ResourceAccess::write,
			                                rhi::ShaderStage::fragment,
			                                rhi::ResourceState::DepthStencilAttachment),
		};
		return {dependencies.data(), static_cast<uint32_t>(dependencies.size())};
	}

	void GPUDrivenShadowAtlasPass::execute(const PassContext& context) const
	{
		if (m_renderer == nullptr)
		{
			return;
		}

		m_renderer->setShadowAtlasAllocatedTiles(0u);
		const GPUDrivenSceneView* sceneView = context.params != nullptr ? context.params->gpuDrivenSceneView : nullptr;
		if (context.params == nullptr || context.transientAllocator == nullptr || context.commandBuffer == nullptr
			|| context.executor == nullptr
			|| !context.params->debugOptions.enableShadowAtlas || sceneView == nullptr || !sceneView->
			usePersistentCullingObjects
			|| sceneView->shadowPackedMeshes == nullptr || sceneView->shadowPackedMeshCount == 0
			|| sceneView->shadowPackedVertexBuffer == 0 || sceneView->shadowPackedIndexBuffer == 0
			|| m_renderer->getShadowAtlasImageOpaque() == 0 || m_renderer->getShadowAtlasViewOpaque() == 0)
		{
			return;
		}

		shaderio::ShadowUniforms* shadowData = m_renderer->getShadowUniformsData();
		if (shadowData == nullptr)
		{
			return;
		}

		const PipelineHandle shadowPipeline = m_renderer->getCSMShadowPipelineHandle();
		if (shadowPipeline.isNull())
		{
			return;
		}

		CSMShadowResources& csm = m_renderer->getCSMShadowResources();
		const rhi::Extent2D atlasFullExtent = m_renderer->getShadowAtlasExtent();
		const uint32_t tileSize = std::max(1u, m_renderer->getShadowAtlasTileSize());
		const uint32_t tilesX = atlasFullExtent.width / tileSize;
		const uint32_t tilesY = atlasFullExtent.height / tileSize;
		const uint32_t tileCapacity = tilesX * tilesY;
		const uint32_t cascadeCount = std::min(csm.getCascadeCount(), tileCapacity);
		if (cascadeCount == 0u)
		{
			return;
		}

		const uint32_t frameIndex = context.frameIndex;
		const rhi::ArgumentTableHandle cameraTable = m_renderer->getCameraArgumentTable(frameIndex);
		const rhi::ArgumentTableHandle drawTable = m_renderer->getCSMShadowMDIDrawArgumentTable(frameIndex, 0);
		if (cameraTable.isNull() || drawTable.isNull())
		{
			return;
		}

		const rhi::ArgumentTableHandle materialTable = m_renderer->getGraphicsMaterialArgumentTable();
		if (materialTable.isNull())
		{
			return;
		}

		context.commandBuffer->beginEvent("GPUDrivenShadowAtlas");
		const rhi::DepthTargetDesc depthTarget{
			.texture = rhi::TextureHandle{
				kPassGPUDrivenShadowAtlasHandle.index, kPassGPUDrivenShadowAtlasHandle.generation
			},
			.view = m_renderer->getShadowAtlasViewHandle(),
			.state = rhi::ResourceState::DepthStencilAttachment,
			.loadOp = rhi::LoadOp::clear,
			.storeOp = rhi::StoreOp::store,
			.clearValue = {0.0f, 0},
		};
		const rhi::Extent2D atlasExtent{atlasFullExtent.width, atlasFullExtent.height};
		rhi::RenderEncoder* enc = context.commandBuffer->beginRenderPass(rhi::RenderPassDesc{
			.renderArea = {{0, 0}, atlasExtent},
			.colorTargets = nullptr,
			.colorTargetCount = 0,
			.depthTarget = &depthTarget,
		});

		enc->setPipeline(shadowPipeline);
		enc->setArgumentTable(rhi::ShaderStage::fragment, shaderio::LSetTextures, materialTable);
		enc->setArgumentTable(rhi::ShaderStage::allGraphics, shaderio::LSetDraw, drawTable);

		const rhi::BufferHandle vertexBufferRHI = m_renderer->getShadowPackedVertexBufferRHIHandle();
		const rhi::BufferHandle indexBufferRHI = m_renderer->getShadowPackedIndexBufferRHIHandle();
		constexpr uint64_t vertexOffset = 0;
		enc->bindVertexBuffers(0, &vertexBufferRHI, &vertexOffset, 1);
		enc->bindIndexBuffer(indexBufferRHI, 0, rhi::IndexFormat::uint32);

		for (uint32_t cascadeIndex = 0; cascadeIndex < cascadeCount; ++cascadeIndex)
		{
			const uint32_t tileX = cascadeIndex % tilesX;
			const uint32_t tileY = cascadeIndex / tilesX;
			const int32_t viewportX = static_cast<int32_t>(tileX * tileSize);
			const int32_t viewportY = static_cast<int32_t>(tileY * tileSize);
			const rhi::Extent2D tileExtent{tileSize, tileSize};
			enc->setViewport(rhi::Viewport{
				static_cast<float>(viewportX),
				static_cast<float>(viewportY),
				static_cast<float>(tileSize),
				static_cast<float>(tileSize),
				0.0f,
				1.0f
			});
			enc->setScissor(rhi::Rect2D{{viewportX, viewportY}, tileExtent});

			const TransientAllocator::Allocation cameraAlloc =
				context.transientAllocator->allocate(sizeof(shaderio::CameraUniforms), 256);
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
			const float baseConstantBias = context.params->lightSettings.depthBias;
			const float baseSlopeBias = context.params->lightSettings.normalBias;
			const float biasScale = shadowData->cascadeBiasScale.z;
			const float cascadeBiasScale = 1.0f + static_cast<float>(cascadeIndex) * biasScale;
			const glm::vec3 lightTravelDir = glm::normalize(context.params->lightSettings.direction);
			const glm::vec3 dirToLight = -lightTravelDir;
			cascadeCamera.shadowConstantBias = baseConstantBias * cascadeBiasScale;
			cascadeCamera.shadowDirectionAndSlopeBias = glm::vec4(dirToLight, baseSlopeBias * cascadeBiasScale);
			std::memcpy(cameraAlloc.cpuPtr, &cascadeCamera, sizeof(cascadeCamera));
			context.transientAllocator->flushAllocation(cameraAlloc, sizeof(cascadeCamera));

			enc->setDynamicBuffer(rhi::ShaderStage::allGraphics, kSceneDynamicBufferTableSlot, {}, cameraAlloc.offset,
			                      0);
			enc->setDynamicBuffer(rhi::ShaderStage::allGraphics, kSceneDynamicBufferTableSlot, {}, 0, 0);
			enc->setArgumentTable(rhi::ShaderStage::allGraphics, kSceneDynamicBufferTableSlot, cameraTable);

			for (uint32_t meshIndex = 0; meshIndex < sceneView->shadowPackedMeshCount; ++meshIndex)
			{
				const ShadowPackedMesh& packedMesh = sceneView->shadowPackedMeshes[meshIndex];
				enc->drawIndexed(rhi::DrawIndexedDesc{
					.indexCount = packedMesh.indexCount,
					.instanceCount = 1,
					.firstIndex = packedMesh.firstIndex,
					.vertexOffset = packedMesh.vertexOffset,
					.firstInstance = meshIndex,
				});
			}
		}

		context.commandBuffer->endEncoding();
		m_renderer->setShadowAtlasAllocatedTiles(cascadeCount);
		context.commandBuffer->endEvent();
	}
} // namespace demo
