#include "shader_io.h"

[[vk::binding(0, 0)]]
Texture2D<float4> sceneColorHdr;
[[vk::binding(1, 0)]]
Texture2D<float> depthTexture;
[[vk::binding(2, 0)]]
Texture2D<float4> normalMaterialTexture;
[[vk::binding(3, 0)]]
RWTexture2D<float4> ssrRaw;

[[vk::push_constant]]
ConstantBuffer<GPUDrivenSSRPushConstants> ssrPush;

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

[shader("compute")]
[numthreads(8, 8, 1)]
void kernelSSRTrace(uint3 dispatchThreadId : SV_DispatchThreadID)
{
    const uint2 outputSize = uint2(ssrPush.params0.xy);
    if(dispatchThreadId.x >= outputSize.x || dispatchThreadId.y >= outputSize.y)
    {
        return;
    }

    const uint2 inputSize = max(outputSize * 2u, uint2(1u, 1u));
    const uint2 fullCoord = min(dispatchThreadId.xy * 2u + uint2(1u, 1u), inputSize - uint2(1u, 1u));
    const float depth = depthTexture.Load(int3(fullCoord, 0));
    if(depth <= 0.0001)
    {
        ssrRaw[dispatchThreadId.xy] = float4(0.0, 0.0, 0.0, 0.0);
        return;
    }

    const float4 normalPacked = normalMaterialTexture.Load(int3(fullCoord, 0));
    const float roughnessProxy = saturate(normalPacked.a);
    if(roughnessProxy > 0.72)
    {
        ssrRaw[dispatchThreadId.xy] = float4(0.0, 0.0, 0.0, 0.0);
        return;
    }

    const float3 n = decodeNormalOct(normalPacked.rg);
    const float2 uv = (float2(fullCoord) + 0.5) / float2(inputSize);
    float2 dir = normalize(n.xy + float2(0.001, 0.002));
    dir.y = -dir.y;

    const uint maxSteps = uint(ssrPush.params0.z);
    const float stridePixels = max(ssrPush.params1.x, 1.0);
    const float thickness = max(ssrPush.params0.w, 0.0001);
    float confidence = 0.0;
    float3 hitColor = float3(0.0, 0.0, 0.0);

    [loop]
    for(uint stepIndex = 1u; stepIndex <= maxSteps; ++stepIndex)
    {
        const float2 sampleUv = uv + dir * (float(stepIndex) * stridePixels) / float2(inputSize);
        if(sampleUv.x <= 0.0 || sampleUv.y <= 0.0 || sampleUv.x >= 1.0 || sampleUv.y >= 1.0)
        {
            break;
        }

        const uint2 sampleCoord = uint2(sampleUv * float2(inputSize));
        const float sampleDepth = depthTexture.Load(int3(sampleCoord, 0));
        const float depthDelta = depth - sampleDepth;
        if(sampleDepth > 0.0001 && depthDelta > 0.0 && depthDelta < thickness)
        {
            hitColor = sceneColorHdr.Load(int3(sampleCoord, 0)).rgb;
            confidence = saturate((1.0 - float(stepIndex) / max(float(maxSteps), 1.0)) * ssrPush.params1.z);
            break;
        }
    }

    ssrRaw[dispatchThreadId.xy] = float4(hitColor, confidence);
}
