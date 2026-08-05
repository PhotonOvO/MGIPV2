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

static const uint kCompactGroupSize = 256u;
groupshared uint priorityPrefix[kCompactGroupSize];
groupshared uint regularPrefix[kCompactGroupSize];

// Classification writes flags by logical probe index. Each lane owns one
// contiguous range, the workgroup scans the per-lane counts, and every lane
// writes its range in ascending order. Separate flag/list ranges avoid in-place
// overwrite races while preserving deterministic compact-list order.
[numthreads(kCompactGroupSize, 1, 1)]
void CS_CompactActiveProbes(uint3 groupThreadId : SV_GroupThreadID)
{
    uint laneIndex = groupThreadId.x;
    uint priorityFlagsBase = 4u;
    uint regularFlagsBase = priorityFlagsBase + initArgs.maxActiveProbes;
    uint priorityListBase = regularFlagsBase + initArgs.maxActiveProbes;
    uint regularListBase = priorityListBase + initArgs.maxActiveProbes;

    uint probesPerThread =
        (initArgs.maxActiveProbes + kCompactGroupSize - 1u) / kCompactGroupSize;
    uint probeBegin = min(laneIndex * probesPerThread, initArgs.maxActiveProbes);
    uint probeEnd = min(probeBegin + probesPerThread, initArgs.maxActiveProbes);

    uint priorityCount = 0u;
    uint regularCount = 0u;
    for (uint probeIndex = probeBegin; probeIndex < probeEnd; ++probeIndex)
    {
        priorityCount += activeProbes[priorityFlagsBase + probeIndex] != 0u ? 1u : 0u;
        regularCount += activeProbes[regularFlagsBase + probeIndex] != 0u ? 1u : 0u;
    }

    priorityPrefix[laneIndex] = priorityCount;
    regularPrefix[laneIndex] = regularCount;
    GroupMemoryBarrierWithGroupSync();

    // Inclusive Hillis-Steele scan over lane-local counts. Both barriers are
    // required: all lanes first read the previous step, then publish the next.
    for (uint offset = 1u; offset < kCompactGroupSize; offset <<= 1u)
    {
        uint priorityAdd = laneIndex >= offset ? priorityPrefix[laneIndex - offset] : 0u;
        uint regularAdd = laneIndex >= offset ? regularPrefix[laneIndex - offset] : 0u;
        GroupMemoryBarrierWithGroupSync();
        priorityPrefix[laneIndex] += priorityAdd;
        regularPrefix[laneIndex] += regularAdd;
        GroupMemoryBarrierWithGroupSync();
    }

    uint priorityWrite = priorityPrefix[laneIndex] - priorityCount;
    uint regularWrite = regularPrefix[laneIndex] - regularCount;

    for (uint probeIndex = probeBegin; probeIndex < probeEnd; ++probeIndex)
    {
        bool priorityProbe = activeProbes[priorityFlagsBase + probeIndex] != 0u;
        bool regularProbe = activeProbes[regularFlagsBase + probeIndex] != 0u;
        if (priorityProbe)
            activeProbes[priorityListBase + priorityWrite++] = probeIndex;
        if (regularProbe)
            activeProbes[regularListBase + regularWrite++] = probeIndex;
    }

    if (laneIndex == 0u)
    {
        uint totalPriorityCount = priorityPrefix[kCompactGroupSize - 1u];
        uint totalRegularCount = regularPrefix[kCompactGroupSize - 1u];
        uint activeCount = totalPriorityCount + totalRegularCount;
        uint initialUpdateCount = min(activeCount, initArgs.baseUpdateBudget);
        activeProbes[0] = activeCount;
        activeProbes[1] = totalPriorityCount;
        activeProbes[2] = totalRegularCount;
        activeProbes[3] = initialUpdateCount;

        uint unusedBudget = initArgs.baseUpdateBudget - initialUpdateCount;
        uint ignored;
        InterlockedAdd(updateProbesInitArgs[initArgs.scratchOffset], unusedBudget, ignored);
    }
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

    // Trace maps one workgroup to one selected probe. Ray lanes are expanded
    // inside that workgroup, while the downstream atlas update contracts keep
    // their existing dispatch dimensions.
    updateProbesInitArgs[argsOffset + 0u] = updateCount;
    updateProbesInitArgs[argsOffset + 1u] = 1u;
    updateProbesInitArgs[argsOffset + 2u] = 1u;

    updateProbesInitArgs[argsOffset + 3u] = updateCount * 4u;
    updateProbesInitArgs[argsOffset + 4u] = 1u;
    updateProbesInitArgs[argsOffset + 5u] = 1u;

    updateProbesInitArgs[argsOffset + 6u] = updateCount;
    updateProbesInitArgs[argsOffset + 7u] = 1u;
    updateProbesInitArgs[argsOffset + 8u] = 1u;
}
