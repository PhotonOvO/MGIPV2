#include "shader_io.h"

[[vk::binding(0, 0)]]
Texture2D<float> depthTexture;
[[vk::binding(1, 0)]]
Texture2D<float4> normalMaterialTexture;
[[vk::binding(2, 0)]]
RWTexture2D<float> aoRaw;

[[vk::push_constant]]
ConstantBuffer<GPUDrivenAOPushConstants> aoPush;

float3 decodeNormalOct(float2 encoded)
{
    float2 f = encoded * 2.0 - 1.0;
    float3 n = float3(f.x, f.y, 1.0 - abs(f.x) - abs(f.y));
    if(n.z < 0.0)
    {
        n.xy = (1.0 - abs(n.yx)) * float2(n.x >= 0.0 ? 1.0 : -1.0, n.y >= 0.0 ? 1.0 : -1.0);
    }
    return normalize(n);
}

float loadDepth(uint2 coord, uint2 size)
{
    return depthTexture.Load(int3(min(coord, size - uint2(1u, 1u)), 0));
}

[shader("compute")]
[numthreads(8, 8, 1)]
void kernelGTAO(uint3 dispatchThreadId : SV_DispatchThreadID)
{
    const uint2 outputSize = uint2(aoPush.params0.xy);
    if(dispatchThreadId.x >= outputSize.x || dispatchThreadId.y >= outputSize.y)
    {
        return;
    }

    const uint2 inputSize = max(outputSize * 2u, uint2(1u, 1u));
    const uint2 fullCoord = min(dispatchThreadId.xy * 2u + uint2(1u, 1u), inputSize - uint2(1u, 1u));
    const float centerDepth = loadDepth(fullCoord, inputSize);
    if(centerDepth <= 0.0001)
    {
        aoRaw[dispatchThreadId.xy] = 1.0;
        return;
    }

    const float3 normal = decodeNormalOct(normalMaterialTexture.Load(int3(fullCoord, 0)).rg);
    const float radius = max(aoPush.params0.z, 1.0);
    const float intensity = max(aoPush.params0.w, 0.0);
    const int2 dirs[4] = { int2(1, 0), int2(0, 1), int2(1, 1), int2(-1, 1) };
    float occlusion = 0.0;
    float weightSum = 0.0;

    [unroll]
    for(uint d = 0u; d < 4u; ++d)
    {
        [unroll]
        for(uint s = 1u; s <= 5u; ++s)
        {
            const int2 offset = dirs[d] * int(round(float(s) * radius / 5.0));
            const uint2 sampleCoord = uint2(clamp(int2(fullCoord) + offset, int2(0, 0), int2(inputSize) - int2(1, 1)));
            const float sampleDepth = loadDepth(sampleCoord, inputSize);
            const float depthDelta = sampleDepth - centerDepth; // reversed-Z: larger depth = closer; positive delta means sample is closer than center
            const float rangeWeight = saturate(1.0 - abs(depthDelta) * aoPush.params1.z);
            const float normalWeight = saturate(abs(normal.z) * aoPush.params1.w + (1.0 - aoPush.params1.w));
            const float hit = depthDelta > 0.0008 ? 1.0 : 0.0;
            const float weight = rangeWeight * normalWeight / float(s);
            occlusion += hit * weight;
            weightSum += weight;
        }
    }

    const float ao = saturate(1.0 - intensity * occlusion / max(weightSum, 0.0001));
    aoRaw[dispatchThreadId.xy] = ao;
}
