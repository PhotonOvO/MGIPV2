#include "shader_io.h"

[[vk::binding(0, 0)]]
StructuredBuffer<uint> sortedKeys;

[[vk::binding(1, 0)]]
StructuredBuffer<uint> sortedDrawIndices;

[[vk::binding(2, 0)]]
StructuredBuffer<GPUCullIndirectCommand> gpuCullingIndirectCommands;

[[vk::binding(3, 0)]]
RWStructuredBuffer<GPUCullIndirectCommand> forwardIndirectCommands;

[[vk::binding(4, 0)]]
RWStructuredBuffer<uint> visibilityPrefixA;

[[vk::binding(5, 0)]]
RWStructuredBuffer<uint> visibilityPrefixB;

[[vk::push_constant]]
TransparentVisibilityPatchPushConstants pushConstants;

bool isVisibleCategory(uint index)
{
    const uint sortKey = sortedKeys[index];
    if((sortKey & pushConstants.categoryMask) != pushConstants.categoryValue)
    {
        return false;
    }

    const uint drawIndex = sortedDrawIndices[index];
    if(drawIndex == 0xffffffffu)
    {
        return false;
    }

    const GPUCullIndirectCommand command = gpuCullingIndirectCommands[drawIndex];
    return command.instanceCount != 0u;
}

uint readPrefix(uint bufferIndex, uint index)
{
    if(bufferIndex == 0u)
    {
        return visibilityPrefixA[index];
    }
    return visibilityPrefixB[index];
}

void writePrefix(uint bufferIndex, uint index, uint value)
{
    if(bufferIndex == 0u)
    {
        visibilityPrefixA[index] = value;
    }
    else
    {
        visibilityPrefixB[index] = value;
    }
}

[numthreads(64, 1, 1)]
[shader("compute")]
void transparentVisibilityPatchMain(uint3 dispatchThreadId : SV_DispatchThreadID)
{
    const uint index = dispatchThreadId.x;
    if(index >= pushConstants.elementCount)
    {
        return;
    }

    if(pushConstants.mode == 0u)
    {
        visibilityPrefixA[index] = isVisibleCategory(index) ? 1u : 0u;
        return;
    }

    if(pushConstants.mode == 1u)
    {
        const uint srcBufferIndex = pushConstants.scanBufferIndex;
        const uint dstBufferIndex = 1u - srcBufferIndex;
        uint value = readPrefix(srcBufferIndex, index);
        if(index >= pushConstants.scanOffset)
        {
            value += readPrefix(srcBufferIndex, index - pushConstants.scanOffset);
        }
        writePrefix(dstBufferIndex, index, value);
        return;
    }

    if(!isVisibleCategory(index))
    {
        return;
    }

    const uint outputIndex = readPrefix(pushConstants.scanBufferIndex, index);
    if(outputIndex == 0u)
    {
        return;
    }

    const uint drawIndex = sortedDrawIndices[index];
    GPUCullIndirectCommand command = gpuCullingIndirectCommands[drawIndex];
    command.firstInstance = drawIndex;
    forwardIndirectCommands[pushConstants.outputOffset + outputIndex - 1u] = command;
}
