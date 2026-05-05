#include "shader_io.h"

[[vk::binding(0, 0)]]
RWStructuredBuffer<uint> debugValues;

[numthreads(64, 1, 1)]
[shader("compute")]
void gpuSceneValidateMain(uint3 dispatchThreadId : SV_DispatchThreadID)
{
    if(dispatchThreadId.x == 0)
    {
        debugValues[0] = 0;
    }
}
