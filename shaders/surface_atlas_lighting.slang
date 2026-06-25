// surface_atlas_lighting.slang — Global Surface Atlas lighting update (FGI-035)
// Fullscreen compute: reads atlas GBuffer, applies directional light, writes
// HDR output to atlas lighting texture. Ported from Flax behavior.

#include "shader_io.h"

[[vk::binding(0, 0)]] Texture2D<float>   gbufferDepth    : register(t0);
[[vk::binding(1, 0)]] Texture2D<float4>  gbuffer0        : register(t1); // baseColor.rgb + AO
[[vk::binding(2, 0)]] Texture2D<float4>  gbuffer1        : register(t2); // normal.xyz + roughness
[[vk::binding(3, 0)]] RWTexture2D<float4> atlasLighting  : register(u0);
[[vk::binding(4, 0)]] ConstantBuffer<LightParams> lightParams : register(b0);

[[vk::push_constant]] ConstantBuffer<SurfaceAtlasLightingPush> lightingPush;

[numthreads(8, 8, 1)]
void CS_Lighting(uint3 dispatchThreadId : SV_DispatchThreadID)
{
    if (lightingPush.atlasResolutionAndPadding.z < 0.5f)
        return;

    uint2 texel = dispatchThreadId.xy;
    if (texel.x >= uint(lightingPush.atlasResolutionAndPadding.x) ||
        texel.y >= uint(lightingPush.atlasResolutionAndPadding.y))
        return;

    float depth = gbufferDepth.Load(int3(texel, 0));
    if (depth >= 1.0f)
    {
        atlasLighting[texel] = float4(0,0,0,0);
        return;
    }

    float4 gb0 = gbuffer0.Load(int3(texel, 0));
    float4 gb1 = gbuffer1.Load(int3(texel, 0));

    float3 baseColor = gb0.rgb;
    float ao = gb0.a;
    float3 N = normalize(gb1.xyz * 2.0f - 1.0f);
    float roughness = gb1.w;

    float3 L = normalize(-lightParams.lightDirectionAndShadowStrength.xyz);
    float3 lightColor = lightParams.lightColorAndNormalBias.rgb;
    float shadowStrength = lightParams.lightDirectionAndShadowStrength.w;

    float NdotL = max(dot(N, L), 0.0f);
    float3 diffuse = baseColor * lightColor * NdotL * (1.0f - shadowStrength * 0.5f);
    diffuse *= ao;

    atlasLighting[texel] = float4(diffuse, 1.0f);
}
