// ddgi_flax_init_args.slang — DDGI indirect dispatch args builder (FGI-051)
// Reads active probe count and builds indirect dispatch arguments for
// the trace rays and probe update passes.

#include "shader_io.h"

[[vk::binding(0, 0)]] RWStructuredBuffer<uint> activeProbes       : register(u0); // counter at [0]
[[vk::binding(1, 0)]] RWStructuredBuffer<uint> updateProbesInitArgs : register(u1); // 3 uint dispatch args

struct FlaxDDGIInitArgsPush
{
    uint maxActiveProbes;
    uint maxUpdatedProbesPerFrame;
};

[[vk::push_constant]] ConstantBuffer<FlaxDDGIInitArgsPush> initArgs;

[numthreads(1, 1, 1)]
void CS_UpdateProbesInitArgs(uint3 dispatchThreadId : SV_DispatchThreadID)
{
    uint totalActiveCount = min(activeProbes[0], initArgs.maxActiveProbes);
    uint updateCount = totalActiveCount;
    if (initArgs.maxUpdatedProbesPerFrame != 0)
        updateCount = min(updateCount, initArgs.maxUpdatedProbesPerFrame);

    // Preserve totalActiveCount in activeProbes[0]. Trace/update shaders apply
    // the budget and rotating offset over the compacted list.

    // Trace: 256 probe threads per group.
    const uint kTraceBatchSize = 256;
    updateProbesInitArgs[0] = (updateCount + kTraceBatchSize - 1) / kTraceBatchSize;
    updateProbesInitArgs[1] = 1;
    updateProbesInitArgs[2] = 1;

    // Distance: one 16x16 tile per probe, covered by four 8x8 groups.
    updateProbesInitArgs[3] = updateCount * 4;
    updateProbesInitArgs[4] = 1;
    updateProbesInitArgs[5] = 1;

    // Irradiance: one 8x8 group covers one complete probe tile.
    updateProbesInitArgs[6] = updateCount;
    updateProbesInitArgs[7] = 1;
    updateProbesInitArgs[8] = 1;
}
