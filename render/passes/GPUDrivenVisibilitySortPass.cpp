#include "GPUDrivenVisibilitySortPass.h"

#include "../ArgumentTables.h"
#include "../GPUDrivenRenderer.h"
#include "../../shaders/shader_io.h"

#include <array>

namespace demo
{
	void recordVisibilitySort(const PassContext& context, GPUDrivenRenderer& renderer)
	{
		if (context.commandBuffer == nullptr)
		{
			return;
		}

		const GPUDrivenRenderer::VisibilitySortDispatch sort = renderer.getVisibilitySortDispatch(context.frameIndex);
		if (!sort.valid)
		{
			return;
		}

		if (sort.uploadKeyBufferHandle.isNull() || sort.uploadValueBufferHandle.isNull()
			|| sort.keyBufferHandle.isNull() || sort.valueBufferHandle.isNull())
		{
			return;
		}

		// Upload only the CPU-owned inputs: keys = GPUSceneObject index, values = draw index.
		// The keygen dispatch below writes every padded slot (distance key or 0xffffffff).
		const uint64_t copySize = static_cast<uint64_t>(sort.keygenPushConstants.activeCount) * sizeof(uint32_t);
		rhi::ComputeEncoder* copyEnc = context.commandBuffer->beginComputePass();
		copyEnc->copyBuffer(sort.uploadKeyBufferHandle, 0, sort.keyBufferHandle, 0, copySize);
		copyEnc->copyBuffer(sort.uploadValueBufferHandle, 0, sort.valueBufferHandle, 0, copySize);
		context.commandBuffer->endEncoding();

		// Same-pass barrier: copyBuffer uploads the keygen inputs, which the keygen compute
		// dispatch reads and overwrites in place.
		context.commandBuffer->barrier(rhi::StageFlags::transfer, rhi::StageFlags::compute,
		                               rhi::HazardFlags::bufferWrites);

		// GPU transparent sort-key generation: key = 0x80000000 | (asuint(d2) >> 2 & 0x3fffffff),
		// d2 = |GPUSceneObject translation - camera|^2; padding slots get 0xffffffff.
		{
			rhi::ComputeEncoder* keygenEnc = context.commandBuffer->beginComputePass();
			keygenEnc->setPipeline(sort.keygenPipelineHandle);
			keygenEnc->setArgumentTable(0, sort.keygenArgumentTable);
			keygenEnc->setRootConstants(kPrimaryRootConstantsSlot,
			                            std::as_bytes(std::span{&sort.keygenPushConstants, 1}));
			keygenEnc->dispatch(rhi::DispatchDesc{(sort.paddedElementCount + 63u) / 64u, 1u, 1u});
			context.commandBuffer->endEncoding();
		}

		// Same-pass barrier: keygen compute writes keys/values; the bitonic steps (or, for one
		// element, the visibility patch) read them from compute.
		context.commandBuffer->barrier(rhi::StageFlags::compute, rhi::StageFlags::compute,
		                               rhi::HazardFlags::bufferWrites);

		if (sort.paddedElementCount <= 1u)
		{
			return;
		}

		for (uint32_t level = 2u; level <= sort.paddedElementCount; level <<= 1u)
		{
			for (uint32_t levelMask = level >> 1u; levelMask > 0u; levelMask >>= 1u)
			{
				const shaderio::BitonicSortPushConstants pushConstants{
					.elementCount = sort.paddedElementCount,
					.level = level,
					.levelMask = levelMask,
					.descending = 1u,
				};
				rhi::ComputeEncoder* enc = context.commandBuffer->beginComputePass();
				enc->setPipeline(sort.pipelineHandle);
				enc->setArgumentTable(0, sort.argumentTable);
				enc->setRootConstants(kPrimaryRootConstantsSlot, std::as_bytes(std::span{&pushConstants, 1}));
				enc->dispatch(rhi::DispatchDesc{(sort.paddedElementCount + 63u) / 64u, 1u, 1u});
				context.commandBuffer->endEncoding();
				// Same-pass barrier: each bitonic step reads the key/value data written
				// by the previous dispatch; PassExecutor cannot model sub-dispatch phases.
				context.commandBuffer->barrier(rhi::StageFlags::compute, rhi::StageFlags::compute,
				                               rhi::HazardFlags::bufferWrites);
			}
		}
	}

	GPUDrivenVisibilitySortPass::GPUDrivenVisibilitySortPass(GPUDrivenRenderer* renderer)
		: m_renderer(renderer)
	{
	}

	PassNode::HandleSlice<PassResourceDependency> GPUDrivenVisibilitySortPass::getDependencies() const
	{
		static const std::array<PassResourceDependency, 2> dependencies = {
			PassResourceDependency::buffer(kPassGPUDrivenSortKeyBufferHandle, ResourceAccess::readWrite,
			                               rhi::ShaderStage::compute),
			PassResourceDependency::buffer(kPassGPUDrivenSortValueBufferHandle, ResourceAccess::readWrite,
			                               rhi::ShaderStage::compute),
		};
		return {dependencies.data(), static_cast<uint32_t>(dependencies.size())};
	}

	void GPUDrivenVisibilitySortPass::execute(const PassContext& context) const
	{
		if (m_renderer == nullptr || context.commandBuffer == nullptr)
		{
			return;
		}

		context.commandBuffer->beginEvent("GPUDrivenVisibilitySort");
		recordVisibilitySort(context, *m_renderer);
		context.commandBuffer->endEvent();
	}
} // namespace demo
