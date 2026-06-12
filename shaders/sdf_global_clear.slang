// Global SDF clear (DDGI Wave D1-2).
// Writes the maximum normalized distance (1.0) into every voxel of the global
// SDF volume mip 0. The min-filtered mip chain is rebuilt afterwards by
// sdf_global_mipmap.slang, so only mip 0 needs clearing.
#include "shader_io.h"

[[vk::binding(0, 0)]]
RWTexture3D<float> dstVolume;

[[vk::push_constant]]
ConstantBuffer<GlobalSDFDispatchPush> clearPush;

[shader("compute")]
[numthreads(4, 4, 4)]
void kernelGlobalSDFClear(uint3 dispatchThreadId : SV_DispatchThreadID)
{
    const uint resolution = uint(clearPush.params0.x);
    if (dispatchThreadId.x >= resolution || dispatchThreadId.y >= resolution
        || dispatchThreadId.z >= resolution)
    {
        return;
    }
    dstVolume[dispatchThreadId] = clearPush.params0.y;
}
