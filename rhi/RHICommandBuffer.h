#pragma once

#include "RHIEncoder.h"
#include "RHIHandles.h"
#include "RHIResidency.h"
#include "RHIStageBarrier.h"
#include "RHITypes.h"

#include <span>

namespace demo::rhi
{
	class CommandAllocator;

	enum class CommandBufferState : uint8_t
	{
		idle = 0,
		recording,
		executable,
		submitted,
		reusable,
	};
	[[nodiscard]] constexpr bool canTransitionCommandBufferState(
		CommandBufferState from, CommandBufferState to) noexcept
	{
		switch (from)
		{
		case CommandBufferState::idle:
		case CommandBufferState::reusable:
			return to == CommandBufferState::recording;
		case CommandBufferState::recording:
			return to == CommandBufferState::executable;
		case CommandBufferState::executable:
			return to == CommandBufferState::submitted;
		case CommandBufferState::submitted:
			return to == CommandBufferState::reusable;
		}
		return false;
	}

	class CommandBuffer
	{
	public:
		virtual ~CommandBuffer() = default;

		virtual void begin(CommandAllocator& allocator) = 0;
		virtual void end() = 0;
		[[nodiscard]] virtual CommandBufferState state() const noexcept = 0;

		virtual RenderEncoder* beginRenderPass(const RenderPassDesc& desc) = 0;
		virtual ComputeEncoder* beginComputePass() = 0;
		virtual void endEncoding() = 0;
		virtual RHIResult useResidencySet(ResidencySetHandle)
		{
			return RHIResult::fail(RHIErrorCode::unsupported, "Explicit residency is unsupported" );
		}

		virtual void barrier(StageFlags producer, StageFlags consumer, HazardFlags hazards) = 0;
		virtual void resourceBarrier(
			std::span<const TextureBarrier> textures,
			std::span<const BufferBarrier> buffers,
			std::span<const AliasingBarrier> aliasing = {}) = 0;
		virtual void clearColorTexture(TextureHandle texture,
		                               const TextureSubresourceRange& range,
		                               const ClearColorValue& clearColor) = 0;

		virtual void beginEvent(const char* name) = 0;
		virtual void endEvent() = 0;

		virtual void resetQueryPool(QueryPoolHandle pool, uint32_t firstQuery, uint32_t queryCount) = 0;
		virtual void writeTimestamp(QueryPoolHandle pool, uint32_t queryIndex, bool afterAllCommands) = 0;
	};
} // namespace demo::rhi