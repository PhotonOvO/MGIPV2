#include "shader.ibl_common.slang"

[[vk::binding(0, 0)]]
RWTexture2D<float2> dfgLUT;

[[vk::push_constant]]
ConstantBuffer<IBLGeneratePushConstants> pushConstants;

float2 integrateBRDF(float nDotV, float roughness)
{
    float3 v = float3(sqrt(max(1.0 - nDotV * nDotV, 0.0)), 0.0, nDotV);
    float3 n = float3(0.0, 0.0, 1.0);
    float a = 0.0;
    float b = 0.0;
    uint sampleCount = max(pushConstants.sampleCount, 1u);

    for(uint i = 0u; i < sampleCount; ++i)
    {
        float2 xi = hammersley(i, sampleCount);
        float3 h = importanceSampleGGX(xi, max(roughness, 0.001), n);
        float3 l = normalize(2.0 * dot(v, h) * h - v);
        float nDotL = max(l.z, 0.0);
        float nDotH = max(h.z, 0.0);
        float vDotH = max(dot(v, h), 0.0);

        if(nDotL > 0.0)
        {
            float g = geometrySmithIBL(n, v, l, roughness);
            float gVis = (g * vDotH) / max(nDotH * nDotV, 1e-4);
            float fc = pow(1.0 - vDotH, 5.0);
            a += (1.0 - fc) * gVis;
            b += fc * gVis;
        }
    }

    return float2(a, b) / float(sampleCount);
}

[shader("compute")]
[numthreads(8, 8, 1)]
void dfgLUTMain(uint3 dispatchThreadId : SV_DispatchThreadID)
{
    if(dispatchThreadId.x >= pushConstants.width || dispatchThreadId.y >= pushConstants.height)
    {
        return;
    }

    float2 uv = (float2(dispatchThreadId.xy) + 0.5) / float2(pushConstants.width, pushConstants.height);
    float nDotV = clamp(uv.x, 0.001, 1.0);
    float roughness = clamp(uv.y, 0.001, 1.0);
    dfgLUT[dispatchThreadId.xy] = integrateBRDF(nDotV, roughness);
}
