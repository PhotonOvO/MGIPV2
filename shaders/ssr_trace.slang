#include "shader_io.h"

[[vk::binding(0, 0)]]
Texture2D<float4> sceneColorHistory;
[[vk::binding(1, 0)]]
Texture2D<float> depthTexture;
[[vk::binding(2, 0)]]
Texture2D<float4> normalMaterialTexture;
[[vk::binding(3, 0)]]
Texture2D<float4> baseColorRoughnessTexture;
[[vk::binding(4, 0)]]
RWTexture2D<float4> ssrRaw;
[[vk::binding(5, 0)]]
ConstantBuffer<CameraUniforms> camera;

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

float3 reconstructWorldPosition(float2 uv, float depth)
{
    float4 clipPos = float4(uv * 2.0 - 1.0, depth, 1.0);
    float4 worldPos = mul(camera.inverseViewProjection, clipPos);
    return worldPos.xyz / max(worldPos.w, 1.0e-5);
}

bool projectWorldPosition(float3 worldPos, out float2 uv, out float depth)
{
    float4 clipPos = mul(camera.viewProjection, float4(worldPos, 1.0));
    if(clipPos.w <= 1.0e-5)
    {
        uv = float2(0.0);
        depth = 0.0;
        return false;
    }

    float3 ndc = clipPos.xyz / clipPos.w;
    uv = ndc.xy * 0.5 + 0.5;
    depth = ndc.z;
    return uv.x > 0.0 && uv.y > 0.0 && uv.x < 1.0 && uv.y < 1.0 && depth > 0.0 && depth < 1.0;
}

float edgeFade(float2 uv)
{
    float2 edgeDistance = min(uv, 1.0 - uv);
    return saturate(min(edgeDistance.x, edgeDistance.y) * 12.0);
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
    const float2 uv = (float2(fullCoord) + 0.5) / float2(inputSize);
    const float depth = depthTexture.Load(int3(fullCoord, 0));
    if(depth <= 0.0001)
    {
        ssrRaw[dispatchThreadId.xy] = float4(0.0);
        return;
    }

    const float4 baseRoughness = baseColorRoughnessTexture.Load(int3(fullCoord, 0));
    const float roughness = saturate(baseRoughness.a);
    if(roughness > 0.72)
    {
        ssrRaw[dispatchThreadId.xy] = float4(0.0);
        return;
    }

    const float4 normalPacked = normalMaterialTexture.Load(int3(fullCoord, 0));
    const float metallic = saturate(normalPacked.b);
    const float3 n = decodeNormalOct(normalPacked.rg);
    const float3 worldPos = reconstructWorldPosition(uv, depth);
    const float3 viewDir = normalize(worldPos - camera.cameraPosition);
    const float3 rayDir = normalize(reflect(viewDir, n));
    if(dot(rayDir, n) <= 0.0)
    {
        ssrRaw[dispatchThreadId.xy] = float4(0.0);
        return;
    }

    const uint maxSteps = max(uint(ssrPush.params0.z), 1u);
    const float thickness = max(ssrPush.params0.w, 0.0001);
    const float minStep = max(ssrPush.params1.x, 0.01);
    const float maxDistance = max(ssrPush.params1.y, minStep);
    const float confidenceScale = ssrPush.params1.z;
    const float roughnessFade = saturate(1.0 - roughness / 0.72);
    const float materialFade = saturate(0.35 + metallic * 0.65);

    float3 hitColor = float3(0.0);
    float confidence = 0.0;

    [loop]
    for(uint stepIndex = 1u; stepIndex <= maxSteps; ++stepIndex)
    {
        const float t = float(stepIndex) / float(maxSteps);
        const float rayDistance = max(minStep, t * maxDistance);
        const float3 sampleWorld = worldPos + rayDir * rayDistance;
        float2 sampleUv;
        float rayDepth;
        if(!projectWorldPosition(sampleWorld, sampleUv, rayDepth))
        {
            break;
        }

        const uint2 sampleCoord = min(uint2(sampleUv * float2(inputSize)), inputSize - uint2(1u, 1u));
        const float sceneDepth = depthTexture.Load(int3(sampleCoord, 0));
        // reversed-Z: larger depth = closer. A hit means the ray point fell behind the
        // surface at this pixel, i.e. the ray is farther than the scene (rayDepth < sceneDepth).
        const float depthDelta = sceneDepth - rayDepth;
        if(sceneDepth > 0.0001 && depthDelta > 0.0 && depthDelta < thickness)
        {
            hitColor = sceneColorHistory.Load(int3(sampleCoord, 0)).rgb;
            confidence = saturate((1.0 - t) * edgeFade(sampleUv) * roughnessFade * materialFade * confidenceScale);
            break;
        }
    }

    ssrRaw[dispatchThreadId.xy] = float4(hitColor, confidence);
}
