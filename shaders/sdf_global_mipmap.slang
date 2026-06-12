// Global SDF mipmap (DDGI Wave D1-2).
// Downsamples one mip level of the global SDF volume with a 2x2x2 *min* kernel
// (not average) so coarser mips stay conservative for sphere tracing
// (cf. LuxGI GlobalSDFMipmap.comp). Dispatched once per destination mip with a
// compute->compute barrier between levels.
#include "shader_io.h"

// Source mip view (single level). The volume lives in General layout because it
// is also written through storage bindings; bound as a sampled texture with
// readWrite access intent and read via Load (no sampler needed).
[[vk::binding(0, 0)]]
Texture3D<float> srcMip;

[[vk::binding(1, 0)]]
RWTexture3D<float> dstMip;

[[vk::push_constant]]
ConstantBuffer<GlobalSDFDispatchPush> mipmapPush;

[shader("compute")]
[numthreads(4, 4, 4)]
void kernelGlobalSDFMipmap(uint3 dispatchThreadId : SV_DispatchThreadID)
{
    const uint dstResolution = uint(mipmapPush.params0.x);
    if (dispatchThreadId.x >= dstResolution || dispatchThreadId.y >= dstResolution
        || dispatchThreadId.z >= dstResolution)
    {
        return;
    }

    const uint srcResolution = dstResolution * 2u;
    const uint3 srcBase = dispatchThreadId * 2u;

    float minValue = 1.0;
    [unroll]
    for (uint i = 0u; i < 8u; ++i)
    {
        const uint3 offset = uint3(i & 1u, (i >> 1u) & 1u, (i >> 2u) & 1u);
        const uint3 coord = min(srcBase + offset, uint3(srcResolution - 1u, srcResolution - 1u, srcResolution - 1u));
        minValue = min(minValue, srcMip.Load(int4(coord, 0)));
    }

    dstMip[dispatchThreadId] = minValue;
}
