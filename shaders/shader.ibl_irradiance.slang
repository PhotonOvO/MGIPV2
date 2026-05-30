#include "shader.ibl_common.slang"

[[vk::binding(0, 0)]]
Sampler2D environmentMap;

[[vk::binding(1, 0)]]
RWTexture2DArray<float4> irradianceMap;

[[vk::push_constant]]
ConstantBuffer<IBLGeneratePushConstants> pushConstants;

[shader("compute")]
[numthreads(8, 8, 1)]
void irradianceConvolutionMain(uint3 dispatchThreadId : SV_DispatchThreadID)
{
    if(dispatchThreadId.x >= pushConstants.width || dispatchThreadId.y >= pushConstants.height || dispatchThreadId.z >= 6u)
    {
        return;
    }

    float3 n = cubeTexelDirection(dispatchThreadId.z, dispatchThreadId.xy, uint2(pushConstants.width, pushConstants.height));
    uint sampleCount = max(pushConstants.sampleCount, 1u);
    float3 irradiance = float3(0.0);

    for(uint i = 0u; i < sampleCount; ++i)
    {
        float2 xi = hammersley(i, sampleCount);
        float phi = 2.0 * IBL_PI * xi.x;
        float cosTheta = xi.y;
        float sinTheta = sqrt(max(1.0 - cosTheta * cosTheta, 0.0));
        float3 localDir = float3(cos(phi) * sinTheta, sin(phi) * sinTheta, cosTheta);
        float3 sampleDir = tangentToWorld(localDir, n);
        float3 radiance = environmentMap.SampleLevel(directionToEquirectUv(sampleDir), 0.0).rgb;
        irradiance += radiance * cosTheta;
    }

    irradiance *= (2.0 * IBL_PI) / float(sampleCount);
    irradianceMap[uint3(dispatchThreadId.xy, dispatchThreadId.z)] = float4(irradiance, 1.0);
}
