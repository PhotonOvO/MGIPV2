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

		using IndexedIndirectRecordDesc = rhi::DrawIndirectDesc;

		[[nodiscard]] bool record(const DrawStream& stream, rhi::RenderEncoder& encoder, Resolver& resolver) const;

		static void recordIndexedIndirectCount(rhi::RenderEncoder& encoder,
		                                       const rhi::DrawIndirectCountDesc& desc);
		static void recordIndexedIndirect(rhi::RenderEncoder& encoder, const IndexedIndirectRecordDesc& desc);
	};
} // namespace demo
