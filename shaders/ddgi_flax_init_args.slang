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
    uint activeCount = min(activeProbes[0], initArgs.maxActiveProbes);
    if (initArgs.maxUpdatedProbesPerFrame != 0)
        activeCount = min(activeCount, initArgs.maxUpdatedProbesPerFrame);

    // Publish the clamped count so the final partially occupied thread group
    // cannot process entries beyond the configured budget.
    activeProbes[0] = activeCount;

    // Trace: 256 probe threads per group.
    const uint kTraceBatchSize = 256;
    updateProbesInitArgs[0] = (activeCount + kTraceBatchSize - 1) / kTraceBatchSize;
    updateProbesInitArgs[1] = 1;
    updateProbesInitArgs[2] = 1;

    // Distance: one 16x16 tile per probe, covered by four 8x8 groups.
    updateProbesInitArgs[3] = activeCount * 4;
    updateProbesInitArgs[4] = 1;
    updateProbesInitArgs[5] = 1;

    // Irradiance: one 8x8 group covers one complete probe tile.
    updateProbesInitArgs[6] = activeCount;
    updateProbesInitArgs[7] = 1;
    updateProbesInitArgs[8] = 1;
}
