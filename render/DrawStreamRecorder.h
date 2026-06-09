#pragma once

#include "DrawStream.h"
#include "DrawStreamDecoder.h"
#include "../rhi/RHIEncoder.h"

namespace demo
{
	class DrawStreamRecorder
	{
	public:
		class Resolver
		{
		public:
			virtual ~Resolver() = default;

			virtual bool bindState(rhi::RenderEncoder& encoder, const DrawStreamDecoder::State& state) = 0;
			virtual bool draw(rhi::RenderEncoder& encoder, const DrawStreamDecoder::DecodedDraw& draw) = 0;
			virtual bool drawIndexed(rhi::RenderEncoder& encoder, const DrawStreamDecoder::DecodedDraw& draw) = 0;
		};

		struct IndexedIndirectCountRecordDesc
		{
			rhi::BufferHandle argsBuffer{};
			uint64_t argsOffset{0};
			rhi::BufferHandle countBuffer{};
			uint64_t countBufferOffset{0};
			uint32_t maxDrawCount{0};
			uint32_t stride{0};
		};

		struct IndexedIndirectRecordDesc
		{
			rhi::BufferHandle argsBuffer{};
			uint64_t offset{0};
			uint32_t drawCount{1};
			uint32_t stride{0};
		};

		[[nodiscard]] bool record(const DrawStream& stream, rhi::RenderEncoder& encoder, Resolver& resolver) const;

		static void recordIndexedIndirectCount(rhi::RenderEncoder& encoder, const IndexedIndirectCountRecordDesc& desc);
		static void recordIndexedIndirect(rhi::RenderEncoder& encoder, const IndexedIndirectRecordDesc& desc);
	};
} // namespace demo
