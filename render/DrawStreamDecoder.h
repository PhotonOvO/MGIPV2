#pragma once

#include "DrawStream.h"
#include "DrawPacket.h"
#include "../rhi/RHIBindlessTypes.h"

#include <cstdint>
#include <utility>
#include <vector>

namespace demo
{
	class DrawStreamDecoder
	{
	public:
		struct State
		{
			PipelineHandle pipeline{};
			demo::rhi::ResourceIndex materialIndex{kDrawStreamInvalidResourceIndex};
			MeshHandle mesh{};
			demo::rhi::ResourceIndex dynamicBufferIndex{kDrawStreamInvalidResourceIndex};
			uint32_t dynamicOffset{kDrawStreamInvalidDynamicOffset};
		};

		struct DecodedDraw
		{
			State state{};
			uint32_t vertexOffset{0};
			uint32_t vertexCount{0};
			uint32_t instanceCount{0};
			// Indexed draw parameters
			bool isIndexed{false};
			uint32_t indexCount{0};
			uint32_t firstIndex{0};
			int32_t vertexOffsetIndexed{0};
			uint32_t firstInstance{0};
		};

		[[nodiscard]] bool decode(const DrawStream& stream, std::vector<DecodedDraw>& outDraws) const;
		[[nodiscard]] bool decodeToDrawPackets(const DrawStream& stream, std::vector<DrawPacket>& outPackets) const;

		template <typename Visitor>
		[[nodiscard]] bool forEachDecodedDraw(const DrawStream& stream, Visitor&& visitor) const
		{
			State currentState{};
			bool hasPipeline{false};
			bool hasMaterial{false};
			bool hasMesh{false};
			bool hasDynamicBuffer{false};
			bool hasDynamicOffset{false};
			uint32_t pendingDirtyMask{0};

			for (const StreamEntry& entry : stream)
			{
				switch (entry.type)
				{
				case StreamEntryType::setPipeline:
					currentState.pipeline = entry.payload.pipeline;
					hasPipeline = true;
					pendingDirtyMask |= kDrawStreamDirtyPipeline;
					break;
				case StreamEntryType::setMaterial:
					currentState.materialIndex = entry.payload.materialIndex;
					hasMaterial = true;
					pendingDirtyMask |= kDrawStreamDirtyMaterial;
					break;
				case StreamEntryType::setMesh:
					currentState.mesh = entry.payload.mesh;
					hasMesh = true;
					pendingDirtyMask |= kDrawStreamDirtyMesh;
					break;
				case StreamEntryType::setDynamicBuffer:
					currentState.dynamicBufferIndex = entry.payload.dynamicBufferIndex;
					hasDynamicBuffer = true;
					pendingDirtyMask |= kDrawStreamDirtyDynamicBuffer;
					break;
				case StreamEntryType::setDynamicOffset:
					currentState.dynamicOffset = entry.payload.dynamicOffset;
					hasDynamicOffset = true;
					pendingDirtyMask |= kDrawStreamDirtyDynamicOffset;
					break;
				case StreamEntryType::draw:
					{
						if (!(hasPipeline && hasMaterial && hasMesh && hasDynamicBuffer && hasDynamicOffset))
						{
							return false;
						}
						if (entry.payload.draw.dirtyMask != pendingDirtyMask)
						{
							return false;
						}

						DecodedDraw decodedDraw{};
						decodedDraw.state = currentState;
						decodedDraw.vertexOffset = entry.payload.draw.vertexOffset;
						decodedDraw.vertexCount = entry.payload.draw.vertexCount;
						decodedDraw.instanceCount = entry.payload.draw.instanceCount;
						decodedDraw.isIndexed = false;
						std::forward<Visitor>(visitor)(decodedDraw);
						pendingDirtyMask = 0;
					}
					break;
				case StreamEntryType::drawIndexed:
					{
						if (!(hasPipeline && hasMaterial && hasMesh && hasDynamicBuffer && hasDynamicOffset))
						{
							return false;
						}
						if (entry.payload.drawIndexed.dirtyMask != pendingDirtyMask)
						{
							return false;
						}

						DecodedDraw decodedDraw{};
						decodedDraw.state = currentState;
						decodedDraw.isIndexed = true;
						decodedDraw.indexCount = entry.payload.drawIndexed.indexCount;
						decodedDraw.instanceCount = entry.payload.drawIndexed.instanceCount;
						decodedDraw.firstIndex = entry.payload.drawIndexed.firstIndex;
						decodedDraw.vertexOffsetIndexed = entry.payload.drawIndexed.vertexOffset;
						decodedDraw.firstInstance = entry.payload.drawIndexed.firstInstance;
						std::forward<Visitor>(visitor)(decodedDraw);
						pendingDirtyMask = 0;
					}
					break;
				}
			}

			return true;
		}
	};
} // namespace demo
