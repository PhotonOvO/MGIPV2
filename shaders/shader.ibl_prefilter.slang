#include "shader.ibl_common.slang"

[[vk::binding(0, 0)]]
Sampler2D environmentMap;

[[vk::binding(1, 0)]]
RWTexture2DArray<float4> prefilteredMap;

[[vk::push_constant]]
ConstantBuffer<IBLGeneratePushConstants> pushConstants;

[shader("compute")]
[numthreads(8, 8, 1)]
void prefilterGGXMain(uint3 dispatchThreadId : SV_DispatchThreadID)
{
    if(dispatchThreadId.x >= pushConstants.width || dispatchThreadId.y >= pushConstants.height || dispatchThreadId.z >= 6u)
    {
        return;
    }

    float3 r = cubeTexelDirection(dispatchThreadId.z, dispatchThreadId.xy, uint2(pushConstants.width, pushConstants.height));
    float3 n = r;
    float3 v = r;
    float roughness = clamp(pushConstants.roughness, 0.0, 1.0);
    uint sampleCount = max(pushConstants.sampleCount, 1u);
    float3 prefilteredColor = float3(0.0);
    float totalWeight = 0.0;

    for(uint i = 0u; i < sampleCount; ++i)
    {
        float2 xi = hammersley(i, sampleCount);
        float3 h = importanceSampleGGX(xi, max(roughness, 0.001), n);
        float3 l = normalize(2.0 * dot(v, h) * h - v);
        float nDotL = max(dot(n, l), 0.0);

        if(nDotL > 0.0)
        {
            float nDotH = max(dot(n, h), 0.0);
            float hDotV = max(dot(h, v), 0.0);
            float pdf = distributionGGX(nDotH, max(roughness, 0.001)) * nDotH / max(4.0 * hDotV, 1e-4);
            float sourceTexelSolidAngle = 4.0 * IBL_PI / max(float(pushConstants.sourceWidth * pushConstants.sourceHeight), 1.0);
            float sampleSolidAngle = 1.0 / max(float(sampleCount) * pdf, 1e-4);
            float sourceMip = roughness <= 0.001 ? 0.0 : 0.5 * log2(sampleSolidAngle / sourceTexelSolidAngle);
            sourceMip = clamp(sourceMip, 0.0, float(pushConstants.sourceMaxMip));
            float3 radiance = environmentMap.SampleLevel(directionToEquirectUv(l), sourceMip).rgb;
            prefilteredColor += radiance * nDotL;
            totalWeight += nDotL;
        }
    }

    prefilteredColor = totalWeight > 0.0 ? prefilteredColor / totalWeight : float3(0.0);
    prefilteredMap[uint3(dispatchThreadId.xy, dispatchThreadId.z)] = float4(prefilteredColor, 1.0);
}
