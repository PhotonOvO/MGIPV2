// Deterministic active-probe compaction and global update-budget allocation.

#include "shader_io.h"

[[vk::binding(0, 0)]] RWStructuredBuffer<uint> activeProbes          : register(u0);
[[vk::binding(1, 0)]] RWStructuredBuffer<uint> updateProbesInitArgs : register(u1);

struct FlaxDDGIInitArgsPush
{
    uint maxActiveProbes;
    uint baseUpdateBudget;
    uint argsOffset;
    uint scratchOffset;
};

[[vk::push_constant]] ConstantBuffer<FlaxDDGIInitArgsPush> initArgs;

// Classification writes flags by logical probe index. A single thread compacts
// them in ascending order, making the list independent of GPU wave scheduling.
[numthreads(1, 1, 1)]
void CS_CompactActiveProbes(uint3 dispatchThreadId : SV_DispatchThreadID)
{
    uint priorityBase = 4u;
    uint regularBase = priorityBase + initArgs.maxActiveProbes;
    uint priorityCount = 0u;
    uint regularCount = 0u;

    for (uint probeIndex = 0u; probeIndex < initArgs.maxActiveProbes; ++probeIndex)
    {
        bool priorityProbe = activeProbes[priorityBase + probeIndex] != 0u;
        bool regularProbe = activeProbes[regularBase + probeIndex] != 0u;
        if (priorityProbe)
            activeProbes[priorityBase + priorityCount++] = probeIndex;
        if (regularProbe)
            activeProbes[regularBase + regularCount++] = probeIndex;
    }

    uint activeCount = priorityCount + regularCount;
    uint initialUpdateCount = min(activeCount, initArgs.baseUpdateBudget);
    activeProbes[0] = activeCount;
    activeProbes[1] = priorityCount;
    activeProbes[2] = regularCount;
    activeProbes[3] = initialUpdateCount;

    uint unusedBudget = initArgs.baseUpdateBudget - initialUpdateCount;
    uint ignored;
    InterlockedAdd(updateProbesInitArgs[initArgs.scratchOffset], unusedBudget, ignored);
}

// Cascades execute this kernel in order with a buffer barrier between dispatches.
// Each keeps its fair base share, then consumes slack left by sparse cascades.
[numthreads(1, 1, 1)]
void CS_UpdateProbesInitArgs(uint3 dispatchThreadId : SV_DispatchThreadID)
{
    uint activeCount = min(activeProbes[0], initArgs.maxActiveProbes);
    uint updateCount = min(activeProbes[3], activeCount);
    uint availableSlack = updateProbesInitArgs[initArgs.scratchOffset];
    uint extraCount = min(activeCount - updateCount, availableSlack);
    updateCount += extraCount;
    activeProbes[3] = updateCount;
    updateProbesInitArgs[initArgs.scratchOffset] = availableSlack - extraCount;

    uint argsOffset = initArgs.argsOffset;

    const uint kTraceBatchSize = 256;
    updateProbesInitArgs[argsOffset + 0u] =
        (updateCount + kTraceBatchSize - 1u) / kTraceBatchSize;
    updateProbesInitArgs[argsOffset + 1u] = 1u;
    updateProbesInitArgs[argsOffset + 2u] = 1u;

    updateProbesInitArgs[argsOffset + 3u] = updateCount * 4u;
    updateProbesInitArgs[argsOffset + 4u] = 1u;
    updateProbesInitArgs[argsOffset + 5u] = 1u;

    updateProbesInitArgs[argsOffset + 6u] = updateCount;
    updateProbesInitArgs[argsOffset + 7u] = 1u;
    updateProbesInitArgs[argsOffset + 8u] = 1u;
}
