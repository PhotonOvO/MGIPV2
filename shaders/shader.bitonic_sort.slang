#include "shader_io.h"

[[vk::binding(0, 0)]]
RWStructuredBuffer<uint> sortKeys;

[[vk::binding(1, 0)]]
RWStructuredBuffer<uint> sortValues;

[[vk::push_constant]]
BitonicSortPushConstants pushConstants;

[numthreads(64, 1, 1)]
[shader("compute")]
void bitonicSortMain(uint3 dispatchThreadId : SV_DispatchThreadID)
{
    const uint index = dispatchThreadId.x;
    if(index >= pushConstants.elementCount)
    {
        return;
    }

    const uint partnerIndex = index ^ pushConstants.levelMask;
    if(partnerIndex <= index || partnerIndex >= pushConstants.elementCount)
    {
        return;
    }

    const bool ascendingForBlock =
        ((index & pushConstants.level) == 0u) ^ (pushConstants.descending != 0u);

    const uint leftKey = sortKeys[index];
    const uint rightKey = sortKeys[partnerIndex];
    const bool keysOutOfOrder = ascendingForBlock ? (leftKey > rightKey) : (leftKey < rightKey);
    if(!keysOutOfOrder)
    {
        return;
    }

    const uint leftValue = sortValues[index];
    sortKeys[index] = rightKey;
    sortValues[index] = sortValues[partnerIndex];
    sortKeys[partnerIndex] = leftKey;
    sortValues[partnerIndex] = leftValue;
}
