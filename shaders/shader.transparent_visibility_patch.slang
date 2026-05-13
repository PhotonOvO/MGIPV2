#include "shader_io.h"

[[vk::binding(0, 0)]]
StructuredBuffer<uint> sortedKeys;

[[vk::binding(1, 0)]]
StructuredBuffer<uint> sortedDrawIndices;

[[vk::binding(2, 0)]]
StructuredBuffer<GPUCullIndirectCommand> gpuCullingIndirectCommands;

[[vk::binding(3, 0)]]
RWStructuredBuffer<GPUCullIndirectCommand> forwardIndirectCommands;

[[vk::push_constant]]
TransparentVisibilityPatchPushConstants pushConstants;

[numthreads(64, 1, 1)]
[shader("compute")]
void transparentVisibilityPatchMain(uint3 dispatchThreadId : SV_DispatchThreadID)
{
    if(dispatchThreadId.x != 0)
    {
        return;
    }

    uint outputIndex = 0u;
    for(uint index = 0u; index < pushConstants.elementCount; ++index)
    {
        const uint sortKey = sortedKeys[index];
        if((sortKey & pushConstants.categoryMask) != pushConstants.categoryValue)
        {
            continue;
        }

        const uint drawIndex = sortedDrawIndices[index];
        GPUCullIndirectCommand command = gpuCullingIndirectCommands[drawIndex];
        if(command.instanceCount == 0u)
        {
            continue;
        }

        command.firstInstance = drawIndex;
        forwardIndirectCommands[pushConstants.outputOffset + outputIndex] = command;
        outputIndex += 1u;
    }
}
