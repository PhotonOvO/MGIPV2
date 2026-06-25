// ddgi_flax_init_args.slang — DDGI indirect dispatch args builder (FGI-051)
// Reads active probe count and builds indirect dispatch arguments for
// the trace rays and probe update passes.

#include "shader_io.h"

[[vk::binding(0, 0)]] RWStructuredBuffer<uint> activeProbes       : register(u0); // counter at [0]
[[vk::binding(1, 0)]] RWStructuredBuffer<uint> updateProbesInitArgs : register(u1); // 3 uint dispatch args

[numthreads(1, 1, 1)]
void CS_UpdateProbesInitArgs(uint3 dispatchThreadId : SV_DispatchThreadID)
{
    uint activeCount = activeProbes[0];

    // Each trace/update dispatch handles up to 256 probes per batch
    const uint kBatchSize = 256;
    uint groupCountX = (activeCount + kBatchSize - 1) / kBatchSize;

    updateProbesInitArgs[0] = groupCountX;
    updateProbesInitArgs[1] = 1;
    updateProbesInitArgs[2] = 1;
}
