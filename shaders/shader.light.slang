// LightPass shader: Full PBR lighting from GBuffer with a single directional shadow map

#include "shader_io.h"

[[vk::binding(0, 0)]]
Sampler2D inTexture[];

[[vk::binding(1, 0)]]
ConstantBuffer<CameraUniforms> camera;

[[vk::binding(2, 0)]]
StructuredBuffer<LightData> pointLightBuffer;

[[vk::binding(3, 0)]]
StructuredBuffer<uint16_t4> pointLightXYCoarseCullIndicesBuffer;

[[vk::binding(4, 0)]]
ConstantBuffer<LightCoarseCullingUniforms> lightCoarseCulling;

[[vk::push_constant]]
LightParams lightParams;

STATIC_CONST int kGBufferBaseColorIndex = 0;
STATIC_CONST int kGBufferNormalIndex    = 1;
STATIC_CONST int kGBufferMaterialIndex  = 2;
STATIC_CONST int kGBufferDepthIndex     = 3;
STATIC_CONST int kShadowMapIndex        = 4;
STATIC_CONST float PI                   = 3.14159265359;

float NDF_GGX(float NdotH, float roughness)
{
    float a = roughness * roughness;
    float a2 = a * a;
    float NdotH2 = NdotH * NdotH;
    float num = a2;
    float denom = (NdotH2 * (a2 - 1.0) + 1.0);
    denom = PI * denom * denom;
    return num / denom;
}

float Geometry_SchlickGGX(float NdotV, float roughness)
{
    float r = roughness + 1.0;
    float k = (r * r) / 8.0;
    return NdotV / (NdotV * (1.0 - k) + k);
}

float Geometry_Smith(float NdotV, float NdotL, float roughness)
{
    return Geometry_SchlickGGX(NdotV, roughness) * Geometry_SchlickGGX(NdotL, roughness);
}

vec3 Fresnel_Schlick(float cosTheta, vec3 F0)
{
    return F0 + (1.0 - F0) * pow(clamp(1.0 - cosTheta, 0.0, 1.0), 5.0);
}

vec3 reconstructWorldPosition(float2 uv, float depth)
{
    float ndcX = uv.x * 2.0 - 1.0;
    float ndcY = uv.y * 2.0 - 1.0;
    float4 clipPos = float4(ndcX, ndcY, depth, 1.0);
    float4 worldPos = mul(camera.inverseViewProjection, clipPos);
    return worldPos.xyz / worldPos.w;
}

bool tileContainsLight(uint16_t4 bounds, uint2 tileCoord)
{
    const uint tileStartX = uint(bounds.x);
    const uint tileCountX = uint(bounds.y);
    const uint tileStartY = uint(bounds.z);
    const uint tileCountY = uint(bounds.w);
    if(tileCountX == 0 || tileCountY == 0)
    {
        return false;
    }

    return tileCoord.x >= tileStartX && tileCoord.x < tileStartX + tileCountX
        && tileCoord.y >= tileStartY && tileCoord.y < tileStartY + tileCountY;
}

uint2 computeLightTileCoord(float2 uv)
{
    const uint screenWidth = uint(lightCoarseCulling.screenTileInfo.x);
    const uint screenHeight = uint(lightCoarseCulling.screenTileInfo.y);
    const uint tileCountX = max(uint(lightCoarseCulling.screenTileInfo.z), 1u);
    const uint tileCountY = max(uint(lightCoarseCulling.screenTileInfo.w), 1u);
    const uint pixelX = min(uint(uv.x * float(screenWidth)), max(screenWidth, 1u) - 1u);
    const uint pixelY = min(uint(uv.y * float(screenHeight)), max(screenHeight, 1u) - 1u);
    return uint2(
        min(pixelX / LTileSizeX, tileCountX - 1u),
        min(pixelY / LTileSizeY, tileCountY - 1u));
}

uint countCoarseCulledPointLights(uint2 tileCoord)
{
    uint hitCount = 0u;
    const uint pointLightCount = uint(lightCoarseCulling.lightCountInfo.x);
    for(uint lightIndex = 0; lightIndex < pointLightCount; ++lightIndex)
    {
        if(tileContainsLight(pointLightXYCoarseCullIndicesBuffer[lightIndex], tileCoord))
        {
            hitCount += 1u;
        }
    }
    return hitCount;
}

vec3 computeCoarseCullHeatmap(float2 uv, vec3 baseColor, bool hasGeometry)
{
    const uint screenWidth = uint(lightCoarseCulling.screenTileInfo.x);
    const uint screenHeight = uint(lightCoarseCulling.screenTileInfo.y);
    const uint pixelX = min(uint(uv.x * float(screenWidth)), max(screenWidth, 1u) - 1u);
    const uint pixelY = min(uint(uv.y * float(screenHeight)), max(screenHeight, 1u) - 1u);
    const uint hitCount = countCoarseCulledPointLights(computeLightTileCoord(uv));
    const float heat = saturate(float(hitCount) / 16.0);

    vec3 heatColor = vec3(0.015, 0.020, 0.030);
    heatColor = lerp(heatColor, vec3(0.020, 0.220, 0.900), smoothstep(0.00, 0.25, heat));
    heatColor = lerp(heatColor, vec3(0.000, 0.850, 0.900), smoothstep(0.20, 0.45, heat));
    heatColor = lerp(heatColor, vec3(0.950, 0.900, 0.050), smoothstep(0.40, 0.70, heat));
    heatColor = lerp(heatColor, vec3(1.000, 0.120, 0.020), smoothstep(0.65, 1.00, heat));

    if(hitCount == 0u)
    {
        heatColor = hasGeometry ? baseColor * 0.08 : heatColor;
    }

    const bool gridLine = (pixelX % LTileSizeX) == 0u || (pixelY % LTileSizeY) == 0u;
    if(gridLine)
    {
        heatColor = max(heatColor, vec3(0.78, 0.84, 0.92));
    }

    return hasGeometry ? lerp(baseColor * 0.18, heatColor, 0.88) : heatColor;
}

vec3 computeCoarseCulledPointLighting(float2 uv, vec3 baseColor, vec3 worldPos, vec3 worldNormal)
{
    const uint2 tileCoord = computeLightTileCoord(uv);

    vec3 lighting = baseColor * 0.035;
    const uint pointLightCount = uint(lightCoarseCulling.lightCountInfo.x);
    for(uint lightIndex = 0; lightIndex < pointLightCount; ++lightIndex)
    {
        const uint16_t4 bounds = pointLightXYCoarseCullIndicesBuffer[lightIndex];
        if(!tileContainsLight(bounds, tileCoord))
        {
            continue;
        }

        const LightData light = pointLightBuffer[lightIndex];
        const vec3 toLight = light.positionOrDirection - worldPos;
        const float distanceToLight = length(toLight);
        if(distanceToLight <= 0.0001 || distanceToLight >= light.range)
        {
            continue;
        }

        const vec3 L = toLight / distanceToLight;
        const float attenuation = pow(saturate(1.0 - distanceToLight / max(light.range, 0.0001)), 2.0);
        const float diffuse = saturate(dot(worldNormal, L));
        lighting += baseColor * light.color * light.intensity * diffuse * attenuation;
    }

    return lighting;
}

float sampleShadow(vec3 worldPos, vec3 worldNormal)
{
    float4 shadowClip = mul(lightParams.worldToShadow, float4(worldPos, 1.0));
    float3 shadowNdc = shadowClip.xyz / shadowClip.w;

    float2 shadowUv = float2(shadowNdc.x * 0.5 + 0.5, shadowNdc.y * 0.5 + 0.5);
    if(shadowUv.x < 0.0 || shadowUv.x > 1.0 || shadowUv.y < 0.0 || shadowUv.y > 1.0)
    {
        return 1.0;
    }

    if(shadowNdc.z <= 0.0 || shadowNdc.z >= 1.0)
    {
        return 1.0;
    }

    float3 lightDir = normalize(lightParams.lightDirectionAndShadowStrength.xyz);
    float normalBias = lightParams.lightColorAndNormalBias.w;
    float depthBias = lightParams.shadowMetrics.y;
    float texelSize = lightParams.ambientColorAndTexelSize.w;
    float cosTheta = saturate(dot(worldNormal, lightDir));
    float sinTheta = sqrt(saturate(1.0 - cosTheta * cosTheta));
    float slope = sinTheta / max(cosTheta, 0.01);
    float bias = depthBias + normalBias * slope;

    float visibility = 0.0;
    [unroll]
    for(int y = -1; y <= 1; ++y)
    {
        [unroll]
        for(int x = -1; x <= 1; ++x)
        {
            float2 offset = float2(x, y) * texelSize;
            float shadowDepth = inTexture[kShadowMapIndex].Sample(shadowUv + offset).r;
            float receiverDepth = shadowNdc.z + bias;
            visibility += receiverDepth >= shadowDepth ? 1.0 : 0.0;
        }
    }

    return visibility / 9.0;
}

vec3 computePBRLighting(vec3 baseColor, vec3 normalEncoded, vec3 worldPos, float metallic, float roughness, float ao,
                        float shadowFade)
{
    vec3 L = normalize(lightParams.lightDirectionAndShadowStrength.xyz);
    vec3 V = normalize(camera.cameraPosition - worldPos);
    vec3 N = normalize(normalEncoded * 2.0 - 1.0);
    vec3 H = normalize(V + L);

    float NdotV = max(dot(N, V), 0.001);
    float NdotL = max(dot(N, L), 0.001);
    float NdotH = max(dot(N, H), 0.0);
    float VdotH = max(dot(V, H), 0.0);

    vec3 F0 = lerp(float3(0.04, 0.04, 0.04), baseColor, metallic);
    vec3 radiance = lightParams.lightColorAndNormalBias.rgb;

    float D = NDF_GGX(NdotH, roughness);
    vec3 F = Fresnel_Schlick(VdotH, F0);
    float G = Geometry_Smith(NdotV, NdotL, roughness);

    vec3 numerator = D * G * F;
    float denominator = 4.0 * NdotV * NdotL + 0.001;
    vec3 specular = numerator / denominator;
    vec3 kS = F;
    vec3 kD = (1.0 - kS) * (1.0 - metallic);
    vec3 diffuse = kD * baseColor / PI;

    float shadowVisibility = sampleShadow(worldPos, N);
    float shadowStrength = lightParams.lightDirectionAndShadowStrength.w * shadowFade;
    float directVisibility = lerp(1.0, shadowVisibility, shadowStrength);
    vec3 Lo = (diffuse + specular) * radiance * NdotL * directVisibility;
    vec3 ambient = lightParams.ambientColorAndTexelSize.rgb * baseColor * ao;

    vec3 color = ambient + Lo;
    color = color / (color + float3(1.0, 1.0, 1.0));
    color = pow(color, float3(1.0 / 2.2, 1.0 / 2.2, 1.0 / 2.2));
    return color;
}

struct VSOutput
{
    float4 position : SV_Position;
    float2 uv       : TEXCOORD0;
};

[shader("vertex")]
VSOutput vertexMain(uint vertexIndex : SV_VertexID)
{
    VSOutput output;
    float2 positions[3] = {
        float2(-1.0, -1.0),
        float2( 3.0, -1.0),
        float2(-1.0,  3.0)
    };
    output.position = float4(positions[vertexIndex], 0.0, 1.0);
    output.uv = positions[vertexIndex] * 0.5 + 0.5;
    return output;
}

[shader("fragment")]
float4 fragmentMain(VSOutput input) : SV_Target
{
    float4 baseColorSample = inTexture[kGBufferBaseColorIndex].Sample(input.uv);
    float4 normalSample    = inTexture[kGBufferNormalIndex].Sample(input.uv);
    float  depthSample     = inTexture[kGBufferDepthIndex].Sample(input.uv).r;

    if(lightCoarseCulling.debugInfo.x > 0.5)
    {
        return float4(computeCoarseCullHeatmap(input.uv, baseColorSample.rgb, depthSample > 0.0001), 1.0);
    }

    if(depthSample <= 0.0001)
    {
        return float4(0.0, 0.0, 0.0, 1.0);
    }

    vec3 worldPos = reconstructWorldPosition(input.uv, depthSample);
    vec3 worldNormal = normalize(normalSample.rgb * 2.0 - 1.0);
    vec3 color = computeCoarseCulledPointLighting(input.uv, baseColorSample.rgb, worldPos, worldNormal);
    color = color / (color + float3(1.0, 1.0, 1.0));
    color = pow(color, float3(1.0 / 2.2, 1.0 / 2.2, 1.0 / 2.2));
    return float4(color, 1.0);
}
