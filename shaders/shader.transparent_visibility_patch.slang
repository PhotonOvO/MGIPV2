#include "shader_io.h"

[[vk::binding(0, 0)]]
StructuredBuffer<uint> sortedDrawIndices;

[[vk::binding(1, 0)]]
StructuredBuffer<GPUCullIndirectCommand> gpuCullingIndirectCommands;

[[vk::binding(2, 0)]]
RWStructuredBuffer<GPUCullIndirectCommand> forwardIndirectCommands;

[[vk::push_constant]]
TransparentVisibilityPatchPushConstants pushConstants;

[numthreads(64, 1, 1)]
[shader("compute")]
void transparentVisibilityPatchMain(uint3 dispatchThreadId : SV_DispatchThreadID)
{
    const uint index = dispatchThreadId.x;
    if(index >= pushConstants.elementCount)
    {
        return;
    }

    const uint drawIndex = sortedDrawIndices[index];
    GPUCullIndirectCommand command = forwardIndirectCommands[index];
    command.instanceCount = gpuCullingIndirectCommands[drawIndex].instanceCount;
    command.firstInstance = index;
    forwardIndirectCommands[index] = command;
}
