// LightPass shader: deferred PBR lighting with coarse point-light culling and cascaded shadow maps.

#include "shader_io.h"

[[vk::binding(0, 0)]]
Sampler2D inTexture[];

[[vk::binding(LBindShadowMap, LSetTextures)]]
Sampler2DArray shadowMap;
#ifdef GPU_DRIVEN_SPLIT_SUM_IBL
[[vk::binding(LBindIBLIrradiance, LSetTextures)]]
SamplerCube iblIrradianceMap;
[[vk::binding(LBindIBLPrefiltered, LSetTextures)]]
SamplerCube iblPrefilteredMap;
[[vk::binding(LBindIBLBrdfLut, LSetTextures)]]
Sampler2D iblBrdfLut;
#endif

[[vk::binding(2, 0)]]
StructuredBuffer<LightData> pointLightBuffer;
[[vk::binding(8, 0)]]
StructuredBuffer<LightData> spotLightBuffer;

[[vk::binding(3, 0)]]
StructuredBuffer<uint16_t4> pointLightXYCoarseCullIndicesBuffer;

[[vk::binding(4, 0)]]
ConstantBuffer<LightCoarseCullingUniforms> lightCoarseCulling;

[[vk::binding(5, 0)]]
StructuredBuffer<uint> clusterLightCounts;

[[vk::binding(6, 0)]]
StructuredBuffer<uint> clusterLightIndices;

[[vk::binding(7, 0)]]
ConstantBuffer<ClusteredLightUniforms> clusteredLight;

[[vk::binding(LBindCamera, LSetScene)]]
ConstantBuffer<CameraUniforms> camera;

[[vk::binding(LBindLighting, LSetScene)]]
ConstantBuffer<LightingUniforms> lighting;
[[vk::binding(LBindPostProcess, LSetScene)]]
ConstantBuffer<PostProcessUniforms> postProcess;

STATIC_CONST int kGBufferBaseColorIndex = 0;
STATIC_CONST int kGBufferPackedNormalMaterialIndex = 1;
STATIC_CONST int kGBufferEmissiveIndex             = 2;
STATIC_CONST int kGBufferDepthIndex                = 3;
STATIC_CONST int kSceneColorHdrIndex               = 4;
STATIC_CONST int kBloomHalfIndex                   = 5;
STATIC_CONST int kBloomQuarterIndex                = 6;
STATIC_CONST int kVelocityIndex                    = 7;
STATIC_CONST int kSceneColorHistoryReadIndex       = 8;
STATIC_CONST int kSceneColorHistoryWriteIndex      = 9;
STATIC_CONST int kIBLEnvironmentIndex              = 10;
STATIC_CONST int kAOTextureIndex                   = 11;
STATIC_CONST int kSSRTextureIndex                  = 12;
STATIC_CONST float PI                   = 3.14159265359;
STATIC_CONST float TWO_PI               = 6.28318530718;
STATIC_CONST float kEquirectSpecularFallbackWeight = 1.0;

float2 signNotZero(float2 v)
{
    return float2(v.x >= 0.0 ? 1.0 : -1.0, v.y >= 0.0 ? 1.0 : -1.0);
}

float3 decodeNormalOct(float2 encoded)
{
    float2 f = encoded * 2.0 - 1.0;
    float3 n = float3(f.x, f.y, 1.0 - abs(f.x) - abs(f.y));
    if(n.z < 0.0)
    {
        n.xy = (1.0 - abs(n.yx)) * signNotZero(n.xy);
    }
    return normalize(n);
}

float saturateRoughness(float roughness)
{
    return clamp(roughness, 0.045, 1.0);
}

float NDF_GGX(float NdotH, float roughness)
{
    float a = roughness * roughness;
    float a2 = a * a;
    float NdotH2 = NdotH * NdotH;
    float denom = NdotH2 * (a2 - 1.0) + 1.0;
    return a2 / max(PI * denom * denom, 1e-5);
}

float Geometry_SchlickGGX(float NdotV, float roughness)
{
    float r = roughness + 1.0;
    float k = (r * r) / 8.0;
    return NdotV / lerp(NdotV, 1.0, k);
}

float Geometry_Smith(float NdotV, float NdotL, float roughness)
{
    return Geometry_SchlickGGX(NdotV, roughness) * Geometry_SchlickGGX(NdotL, roughness);
}

vec3 Fresnel_Schlick(float cosTheta, vec3 F0)
{
    return F0 + (1.0 - F0) * pow(clamp(1.0 - cosTheta, 0.0, 1.0), 5.0);
}

vec3 Fresnel_Schlick_Roughness(float cosTheta, vec3 F0, float roughness)
{
    vec3 grazing = max(vec3(1.0 - roughness, 1.0 - roughness, 1.0 - roughness), F0);
    return F0 + (grazing - F0) * pow(clamp(1.0 - cosTheta, 0.0, 1.0), 5.0);
}

vec2 approximateEnvironmentBRDF(float NdotV, float roughness)
{
    const vec4 c0 = vec4(-1.0, -0.0275, -0.572, 0.022);
    const vec4 c1 = vec4( 1.0,  0.0425,  1.040, -0.040);
    vec4 r = roughness * c0 + c1;
    float a004 = min(r.x * r.x, exp2(-9.28 * NdotV)) * r.x + r.y;
    return vec2(-1.04, 1.04) * a004 + r.zw;
}

float2 directionToEquirectUv(vec3 direction)
{
    vec3 d = normalize(direction);
    float u = atan2(d.z, d.x) / TWO_PI + 0.5;
    float v = asin(clamp(d.y, -1.0, 1.0)) / PI + 0.5;
    return float2(u, 1.0 - v);
}

vec3 sampleEquirectEnvironment(vec3 direction, float mipLevel)
{
    return max(inTexture[kIBLEnvironmentIndex].SampleLevel(directionToEquirectUv(direction), mipLevel).rgb, 0.0);
}

vec3 reconstructWorldPosition(float2 uv, float depth)
{
    float ndcX = uv.x * 2.0 - 1.0;
    float ndcY = uv.y * 2.0 - 1.0;
    float4 clipPos = float4(ndcX, ndcY, depth, 1.0);
    float4 worldPos = mul(camera.inverseViewProjection, clipPos);
    return worldPos.xyz / worldPos.w;
}

vec3 reconstructWorldRay(float2 uv)
{
    float2 ndc = uv * 2.0 - 1.0;
    float4 clipPos = float4(ndc, 1.0, 1.0);
    float4 worldPos = mul(camera.inverseViewProjection, clipPos);
    return normalize(worldPos.xyz / max(worldPos.w, 1.0e-5) - camera.cameraPosition);
}

vec3 evaluateEnvironmentBackground(float2 uv)
{
    const bool useIBL = lighting.light.iblParams.x > 0.5 && lighting.light.iblParams.w > 0.5;
    if(!useIBL)
    {
        return vec3(0.0, 0.0, 0.0);
    }
    return sampleEquirectEnvironment(reconstructWorldRay(uv), 0.0) * lighting.light.iblParams.y;
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

float computeViewDepth(vec3 worldPos)
{
    float4 viewPos = mul(camera.view, float4(worldPos, 1.0));
    return abs(viewPos.z);
}

int selectCascadeIndex(float viewDepth)
{
    int cascadeIndex = 0;
    float splits[4] = {
        lighting.light.cascadeSplitDistances.x,
        lighting.light.cascadeSplitDistances.y,
        lighting.light.cascadeSplitDistances.z,
        lighting.light.cascadeSplitDistances.w
    };

    [unroll]
    for(int i = 0; i < LCascadeCount - 1; ++i)
    {
        if(viewDepth > splits[i])
            cascadeIndex = i + 1;
    }
    return cascadeIndex;
}

float sampleCSMShadow(vec3 worldPos, vec3 worldNormal, float viewDepth)
{
    int cascadeIdx = selectCascadeIndex(viewDepth);

    float4 shadowClip = mul(lighting.light.worldToShadow[cascadeIdx], float4(worldPos, 1.0));
    float3 shadowNdc = shadowClip.xyz / shadowClip.w;
    float2 shadowUv = shadowNdc.xy * 0.5 + 0.5;

    if(shadowUv.x < 0.0 || shadowUv.x > 1.0 || shadowUv.y < 0.0 || shadowUv.y > 1.0)
        return 1.0;
    if(shadowNdc.z <= 0.0 || shadowNdc.z >= 1.0)
        return 1.0;

    float3 lightDir = normalize(lighting.light.lightDirectionAndShadowStrength.xyz);
    float cosTheta = saturate(abs(dot(worldNormal, lightDir)));
    float sinTheta = sqrt(saturate(1.0 - cosTheta * cosTheta));
    float slope = sinTheta / max(cosTheta, 0.01);

    float baseBias = lighting.light.shadowMetrics.y;
    float slopeBias = lighting.light.shadowMetrics.z;
    float cascadeScale = 0.5;

    float cascadeBias = baseBias * (1.0 + float(cascadeIdx) * cascadeScale);
    float bias = cascadeBias + slopeBias * (1.0 + float(cascadeIdx) * cascadeScale) * slope;

    float texelSize = lighting.light.shadowMetrics.x;
    float visibility = 0.0;

    [unroll]
    for(int y = -1; y <= 1; ++y)
    {
        [unroll]
        for(int x = -1; x <= 1; ++x)
        {
            float2 offset = float2(x, y) * texelSize;
            float3 sampleCoord = float3(shadowUv + offset, float(cascadeIdx));
            float shadowDepth = shadowMap.Sample(sampleCoord).r;
            visibility += (shadowNdc.z + bias >= shadowDepth) ? 1.0 : 0.0;
        }
    }

    return visibility / 9.0;
}

vec3 evaluateDirectBRDF(vec3 N, vec3 V, vec3 L, vec3 radiance, vec3 baseColor, float metallic, float roughness)
{
    vec3 H = normalize(V + L);

    float NdotV = max(dot(N, V), 0.001);
    float NdotL = max(dot(N, L), 0.0);
    float NdotH = max(dot(N, H), 0.0);
    float VdotH = max(dot(V, H), 0.0);

    if(NdotL <= 0.0)
    {
        return vec3(0.0);
    }

    vec3 F0 = lerp(vec3(0.04, 0.04, 0.04), baseColor, metallic);
    float D = NDF_GGX(NdotH, roughness);
    vec3 F = Fresnel_Schlick(VdotH, F0);
    float G = Geometry_Smith(NdotV, NdotL, roughness);

    vec3 numerator = D * G * F;
    float denominator = max(4.0 * NdotV * NdotL, 0.001);
    vec3 specular = numerator / denominator;

    vec3 kS = F;
    vec3 kD = (1.0 - kS) * (1.0 - metallic);
    vec3 diffuse = kD * baseColor / PI;

    return (diffuse + specular) * radiance * NdotL;
}

vec3 evaluateDirectionalLight(vec3 N, vec3 V, vec3 worldPos, vec3 baseColor, float metallic, float roughness, float viewDepth)
{
    vec3 L = normalize(lighting.light.lightDirectionAndShadowStrength.xyz);
    vec3 radiance = lighting.light.lightColorAndNormalBias.rgb;
    vec3 direct = evaluateDirectBRDF(N, V, L, radiance, baseColor, metallic, roughness);

    float shadowVisibility = sampleCSMShadow(worldPos, N, viewDepth);
    float shadowStrength = lighting.light.lightDirectionAndShadowStrength.w;
    float directVisibility = lerp(1.0, shadowVisibility, shadowStrength);
    return direct * directVisibility;
}

vec3 evaluateLocalLight(LightData light, vec3 N, vec3 V, vec3 worldPos, vec3 baseColor, float metallic, float roughness)
{
    const vec3 toLight = light.positionOrDirection - worldPos;
    const float distanceToLight = length(toLight);
    if(distanceToLight <= 0.0001 || distanceToLight >= light.range)
    {
        return vec3(0.0);
    }

    const vec3 L = toLight / distanceToLight;
    const float invDistanceSq = 1.0 / max(distanceToLight * distanceToLight, 0.01);
    const float rangeAttenuation = pow(saturate(1.0 - distanceToLight / max(light.range, 0.0001)), 2.0);
    float spotAttenuation = 1.0;
    if(light.lightType == LLightTypeSpot)
    {
        const vec3 spotDirection = normalize(light.spotDirection);
        const float innerCos = cos(light.spotInnerAngle);
        const float outerCos = cos(light.spotOuterAngle);
        const float spotCos = dot(spotDirection, -L);
        spotAttenuation = smoothstep(outerCos, innerCos, spotCos);
    }

    const vec3 radiance = light.color * light.intensity * invDistanceSq * light.range * light.range * rangeAttenuation * spotAttenuation;
    return evaluateDirectBRDF(N, V, L, radiance, baseColor, metallic, roughness);
}

vec3 evaluatePointLight(uint lightIndex, vec3 N, vec3 V, vec3 worldPos, vec3 baseColor, float metallic, float roughness)
{
    return evaluateLocalLight(pointLightBuffer[lightIndex], N, V, worldPos, baseColor, metallic, roughness);
}

vec3 evaluateSpotLight(uint lightIndex, vec3 N, vec3 V, vec3 worldPos, vec3 baseColor, float metallic, float roughness)
{
    return evaluateLocalLight(spotLightBuffer[lightIndex], N, V, worldPos, baseColor, metallic, roughness);
}

vec3 evaluatePointLightsCoarse(float2 uv, vec3 N, vec3 V, vec3 worldPos, vec3 baseColor, float metallic, float roughness)
{
    const uint2 tileCoord = computeLightTileCoord(uv);
    vec3 accumulated = vec3(0.0);
    const uint pointLightCount = uint(lightCoarseCulling.lightCountInfo.x);

    for(uint lightIndex = 0; lightIndex < pointLightCount; ++lightIndex)
    {
        const uint16_t4 bounds = pointLightXYCoarseCullIndicesBuffer[lightIndex];
        if(!tileContainsLight(bounds, tileCoord))
        {
            continue;
        }

        accumulated += evaluatePointLight(lightIndex, N, V, worldPos, baseColor, metallic, roughness);
    }

    return accumulated;
}

uint computeClusterId(float2 uv, float viewDepth)
{
    const uint clusterCountX = max(uint(clusteredLight.screenSizeAndClusterInfo.z), 1u);
    const uint clusterCountY = max(uint(clusteredLight.screenSizeAndClusterInfo.w), 1u);
    const uint clusterCountZ = max(uint(clusteredLight.clusterZAndLightInfo.x), 1u);
    const uint clusterX = min(uint(uv.x * float(clusterCountX)), clusterCountX - 1u);
    const uint clusterY = min(uint(uv.y * float(clusterCountY)), clusterCountY - 1u);
    const float nearZ = max(clusteredLight.clipInfo.x, 1.0e-4);
    const float farZ = max(clusteredLight.clipInfo.y, nearZ + 1.0e-3);
    const float normalizedDepth = saturate((viewDepth - nearZ) / max(farZ - nearZ, 1.0e-3));
    const uint clusterZ = min(uint(normalizedDepth * float(clusterCountZ)), clusterCountZ - 1u);
    return clusterX + clusterY * clusterCountX + clusterZ * clusterCountX * clusterCountY;
}

vec3 evaluatePointLightsClustered(float2 uv, float viewDepth, vec3 N, vec3 V, vec3 worldPos, vec3 baseColor, float metallic, float roughness)
{
    const uint clusterId = computeClusterId(uv, viewDepth);
    const uint packedCount = clusterLightCounts[clusterId];
    if((packedCount & LClusterOverflowBit) != 0u)
    {
        return evaluatePointLightsCoarse(uv, N, V, worldPos, baseColor, metallic, roughness);
    }

    const uint lightCount = min(packedCount & LClusterCountMask, LMaxLightsPerCluster);
    const uint baseIndex = clusterId * LMaxLightsPerCluster;
    vec3 accumulated = vec3(0.0);

    for(uint i = 0; i < lightCount; ++i)
    {
        const uint packedLightIndex = clusterLightIndices[baseIndex + i];
        const uint lightIndex = packedLightIndex & LClusterLightIndexMask;
        accumulated += (packedLightIndex & LClusterLightTypeSpotBit) != 0u
            ? evaluateSpotLight(lightIndex, N, V, worldPos, baseColor, metallic, roughness)
            : evaluatePointLight(lightIndex, N, V, worldPos, baseColor, metallic, roughness);
    }

    return accumulated;
}

vec3 evaluatePointLights(float2 uv, float viewDepth, vec3 N, vec3 V, vec3 worldPos, vec3 baseColor, float metallic, float roughness)
{
    if(lightCoarseCulling.debugInfo.w > 0.5)
    {
        return evaluatePointLightsClustered(uv, viewDepth, N, V, worldPos, baseColor, metallic, roughness);
    }

    return evaluatePointLightsCoarse(uv, N, V, worldPos, baseColor, metallic, roughness);
}

vec3 evaluatePointLights(float2 uv, vec3 N, vec3 V, vec3 worldPos, vec3 baseColor, float metallic, float roughness)
{
    return evaluatePointLights(uv, computeViewDepth(worldPos), N, V, worldPos, baseColor, metallic, roughness);
}

float3 computeClusterHeatmap(float2 uv, float viewDepth, float3 baseColor)
{
    const uint clusterId = computeClusterId(uv, viewDepth);
    const uint packedCount = clusterLightCounts[clusterId];
    const uint count = packedCount & LClusterCountMask;
    const bool overflow = (packedCount & LClusterOverflowBit) != 0u;
    const float t = saturate(float(count) / float(LMaxLightsPerCluster));
    const float3 heat = overflow && lightCoarseCulling.debugInfo.z > 0.5
        ? float3(1.0, 0.0, 0.8)
        : float3(t, 0.25, 1.0 - t);
    return lerp(baseColor * 0.25, heat, 0.75);
}

vec3 evaluateEnvironmentIBL(vec3 N, vec3 V, vec3 baseColor, float metallic, float roughness, float ao)
{
    const bool useIBL = lighting.light.iblParams.x > 0.5 && lighting.light.iblParams.w > 0.5;
    if(!useIBL)
    {
        return lighting.light.ambientColorAndTexelSize.rgb * baseColor * ao;
    }

    const float intensity = lighting.light.iblParams.y;
    const float maxMip = max(lighting.light.iblParams.z, 0.0);
    const vec3 F0 = lerp(vec3(0.04, 0.04, 0.04), baseColor, metallic);
    const float NdotV = max(dot(N, V), 0.001);
    const vec3 F = Fresnel_Schlick_Roughness(NdotV, F0, roughness);
    const vec3 kD = (1.0 - F) * (1.0 - metallic);
    const vec3 R = reflect(-V, N);
    vec3 diffuse;
    vec3 specular;
#ifdef GPU_DRIVEN_SPLIT_SUM_IBL
    const bool useSplitSumIBL = lighting.light.iblDebugInfo.y > 0.5;
    if(useSplitSumIBL)
    {
        const vec3 irradiance = max(iblIrradianceMap.Sample(N).rgb, 0.0);
        const vec3 prefiltered = max(iblPrefilteredMap.SampleLevel(R, roughness * maxMip).rgb, 0.0);
        const vec2 envBRDF = iblBrdfLut.Sample(float2(NdotV, roughness)).rg;
        diffuse = kD * irradiance * baseColor / PI;
        specular = prefiltered * (F0 * envBRDF.x + envBRDF.y);
    }
    else
#endif
    {
        const vec2 envBRDF = approximateEnvironmentBRDF(NdotV, roughness);
        const vec3 irradiance = sampleEquirectEnvironment(N, maxMip);
        const vec3 prefiltered = sampleEquirectEnvironment(R, roughness * maxMip);
        diffuse = kD * irradiance * baseColor / PI;
        specular = prefiltered * (F * envBRDF.x + envBRDF.y) * kEquirectSpecularFallbackWeight;
    }

    const uint debugMode = uint(lighting.light.iblDebugInfo.x);
    if(debugMode == 1u)
    {
        return diffuse * ao * intensity;
    }
    if(debugMode == 2u)
    {
        return specular * intensity;
    }
    if(debugMode == 3u)
    {
        return lighting.light.ambientColorAndTexelSize.rgb * baseColor * ao;
    }
    if(debugMode == 4u)
    {
        return sampleEquirectEnvironment(N, 0.0) * intensity;
    }

    return (diffuse * ao + specular) * intensity;
}

vec3 evaluateSSR(float2 uv, vec3 N, vec3 V, vec3 baseColor, float metallic, float roughness)
{
    if(lighting.light.phase7Info.y <= 0.5 || roughness > 0.72)
    {
        return vec3(0.0);
    }
    const vec4 ssr = inTexture[kSSRTextureIndex].SampleLevel(uv, 0.0);
    const vec3 F0 = lerp(vec3(0.04, 0.04, 0.04), baseColor, metallic);
    const vec3 F = Fresnel_Schlick_Roughness(max(dot(N, V), 0.001), F0, roughness);
    const float roughnessFade = saturate(1.0 - roughness / 0.72);
    return ssr.rgb * F * saturate(ssr.a) * roughnessFade;
}

vec3 toneMapACES(vec3 color)
{
    const float a = 2.51;
    const float b = 0.03;
    const float c = 2.43;
    const float d = 0.59;
    const float e = 0.14;
    color = saturate((color * (a * color + b)) / (color * (c * color + d) + e));
    return pow(color, vec3(1.0 / 2.2, 1.0 / 2.2, 1.0 / 2.2));
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
    float4 packedSample    = inTexture[kGBufferPackedNormalMaterialIndex].Sample(input.uv);
    float4 emissiveSample  = inTexture[kGBufferEmissiveIndex].Sample(input.uv);
    float  depthSample     = inTexture[kGBufferDepthIndex].Sample(input.uv).r;

    if(lightCoarseCulling.debugInfo.x > 0.5)
    {
        return float4(computeCoarseCullHeatmap(input.uv, baseColorSample.rgb, depthSample > 0.0001), 1.0);
    }

    if(depthSample <= 0.0001)
    {
        return float4(toneMapACES(evaluateEnvironmentBackground(input.uv)), 1.0);
    }

    vec3 worldPos = reconstructWorldPosition(input.uv, depthSample);
    vec3 N = decodeNormalOct(packedSample.rg);
    vec3 V = normalize(camera.cameraPosition - worldPos);
    vec3 baseColor = saturate(baseColorSample.rgb);
    float metallic = saturate(packedSample.b);
    float roughness = saturateRoughness(baseColorSample.a);
    float ao = saturate(packedSample.a);
    if(lighting.light.phase7Info.x > 0.5)
    {
        ao *= saturate(inTexture[kAOTextureIndex].SampleLevel(input.uv, 0.0).r);
    }
    float viewDepth = computeViewDepth(worldPos);

    if(lightCoarseCulling.debugInfo.y > 0.5)
    {
        return float4(computeClusterHeatmap(input.uv, viewDepth, baseColor), 1.0);
    }

    vec3 ambient = evaluateEnvironmentIBL(N, V, baseColor, metallic, roughness, ao);
    vec3 directional = evaluateDirectionalLight(N, V, worldPos, baseColor, metallic, roughness, viewDepth);
    vec3 pointLights = evaluatePointLights(input.uv, N, V, worldPos, baseColor, metallic, roughness);

    vec3 ssr = evaluateSSR(input.uv, N, V, baseColor, metallic, roughness);
    vec3 color = ambient + directional + pointLights + ssr + emissiveSample.rgb;
    return float4(toneMapACES(color), 1.0);
}

[shader("fragment")]
float4 fragmentHdrMain(VSOutput input) : SV_Target
{
    float4 baseColorSample = inTexture[kGBufferBaseColorIndex].Sample(input.uv);
    float4 packedSample    = inTexture[kGBufferPackedNormalMaterialIndex].Sample(input.uv);
    float4 emissiveSample  = inTexture[kGBufferEmissiveIndex].Sample(input.uv);
    float  depthSample     = inTexture[kGBufferDepthIndex].Sample(input.uv).r;

    if(lightCoarseCulling.debugInfo.x > 0.5)
    {
        return float4(computeCoarseCullHeatmap(input.uv, baseColorSample.rgb, depthSample > 0.0001), 1.0);
    }

    if(depthSample <= 0.0001)
    {
        return float4(evaluateEnvironmentBackground(input.uv), 1.0);
    }

    vec3 worldPos = reconstructWorldPosition(input.uv, depthSample);
    vec3 N = decodeNormalOct(packedSample.rg);
    vec3 V = normalize(camera.cameraPosition - worldPos);
    vec3 baseColor = saturate(baseColorSample.rgb);
    float metallic = saturate(packedSample.b);
    float roughness = saturateRoughness(baseColorSample.a);
    float ao = saturate(packedSample.a);
    if(lighting.light.phase7Info.x > 0.5)
    {
        ao *= saturate(inTexture[kAOTextureIndex].SampleLevel(input.uv, 0.0).r);
    }
    float viewDepth = computeViewDepth(worldPos);

    vec3 ambient = evaluateEnvironmentIBL(N, V, baseColor, metallic, roughness, ao);
    vec3 directional = evaluateDirectionalLight(N, V, worldPos, baseColor, metallic, roughness, viewDepth);
    vec3 pointLights = evaluatePointLights(input.uv, N, V, worldPos, baseColor, metallic, roughness);

    vec3 ssr = evaluateSSR(input.uv, N, V, baseColor, metallic, roughness);
    return float4(max(ambient + directional + pointLights + ssr + emissiveSample.rgb, 0.0), 1.0);
}

[shader("fragment")]
float4 fragmentSkyboxMain(VSOutput input) : SV_Target
{
    return float4(evaluateEnvironmentBackground(input.uv), 1.0);
}

int selectTemporalSceneColorIndex()
{
    return postProcess.params5.x > 0.5 ? kSceneColorHistoryWriteIndex : kSceneColorHdrIndex;
}

float3 sampleBloomSource(int textureIndex, float2 uv, float2 texelSize)
{
    float3 color = inTexture[textureIndex].Sample(uv).rgb * 0.40;
    color += inTexture[textureIndex].Sample(uv + float2(texelSize.x, 0.0)).rgb * 0.15;
    color += inTexture[textureIndex].Sample(uv - float2(texelSize.x, 0.0)).rgb * 0.15;
    color += inTexture[textureIndex].Sample(uv + float2(0.0, texelSize.y)).rgb * 0.15;
    color += inTexture[textureIndex].Sample(uv - float2(0.0, texelSize.y)).rgb * 0.15;
    return color;
}

float luminance(float3 color)
{
    return dot(color, float3(0.2126, 0.7152, 0.0722));
}

float computeAdaptiveExposure()
{
    float total = 0.0;
    [unroll]
    for(int y = 0; y < 4; ++y)
    {
        [unroll]
        for(int x = 0; x < 4; ++x)
        {
            float2 uv = float2((float(x) + 0.5) * 0.25, (float(y) + 0.5) * 0.25);
            total += luminance(inTexture[selectTemporalSceneColorIndex()].Sample(uv).rgb);
        }
    }
    float averageLum = max(total * (1.0 / 16.0), 1.0e-4);
    return clamp(postProcess.params2.y / averageLum, postProcess.params2.z, postProcess.params2.w);
}

float3 applyColorGrade(float3 color, float2 uv)
{
    float gray = luminance(color);
    color = lerp(float3(gray, gray, gray), color, postProcess.params3.x);
    color = (color - 0.5) * postProcess.params3.y + 0.5;
    color = pow(max(color, 0.0), float3(1.0 / max(postProcess.params3.z, 0.01)));

    float2 centeredUv = uv * 2.0 - 1.0;
    float vignette = saturate(1.0 - dot(centeredUv, centeredUv) * postProcess.params3.w);
    return color * vignette;
}

float proceduralLensDirt(float2 uv)
{
    float grain = frac(sin(dot(floor(uv * 32.0), float2(12.9898, 78.233))) * 43758.5453);
    float smudge = 0.5 + 0.5 * sin((uv.x + uv.y) * 32.0) * sin((uv.x - uv.y) * 17.0);
    float radial = saturate(1.0 - length(uv - 0.5) * 1.45);
    return saturate((grain * 0.35 + smudge * 0.65) * radial);
}

[shader("fragment")]
float4 fragmentBloomPrefilterMain(VSOutput input) : SV_Target
{
    float3 hdrColor = sampleBloomSource(selectTemporalSceneColorIndex(), input.uv, postProcess.params1.xy);
    float brightness = max(hdrColor.r, max(hdrColor.g, hdrColor.b));
    float contribution = saturate((brightness - postProcess.params0.z) / max(brightness, 1e-4));
    return float4(hdrColor * contribution, 1.0);
}

[shader("fragment")]
float4 fragmentBloomDownsampleMain(VSOutput input) : SV_Target
{
    return float4(sampleBloomSource(kBloomHalfIndex, input.uv, postProcess.params1.xy), 1.0);
}

[shader("fragment")]
float4 fragmentFinalColorMain(VSOutput input) : SV_Target
{
    if(postProcess.params5.w > 0.5)
    {
        float2 velocity = inTexture[kVelocityIndex].Sample(input.uv).rg;
        float magnitude = saturate(length(velocity) * 64.0);
        return float4(magnitude, magnitude, magnitude, 1.0);
    }

    float3 hdrColor = inTexture[selectTemporalSceneColorIndex()].Sample(input.uv).rgb;
    float3 bloom = inTexture[kBloomHalfIndex].Sample(input.uv).rgb * 0.65
                 + inTexture[kBloomQuarterIndex].Sample(input.uv).rgb * 0.35;
    float bloomEnabled = postProcess.params0.w;
    float exposure = lerp(postProcess.params0.x, computeAdaptiveExposure(), postProcess.params2.x);
    float lensEnabled = postProcess.params4.x;
    float lensDirt = proceduralLensDirt(input.uv) * postProcess.params4.y * lensEnabled;
    float3 lensBloom = bloom * lensDirt;
    float3 color = (hdrColor + (bloom + lensBloom) * postProcess.params0.y * bloomEnabled) * exposure;
    color = toneMapACES(max(color, 0.0));
    color = applyColorGrade(color, input.uv);
    return float4(saturate(color), 1.0);
}

[shader("fragment")]
float4 fragmentVelocityMain(VSOutput input) : SV_Target
{
    float depthSample = inTexture[kGBufferDepthIndex].Sample(input.uv).r;
    if(depthSample <= 0.0001)
    {
        return float4(0.0, 0.0, 0.0, 1.0);
    }

    float2 currentNdc = input.uv * 2.0 - 1.0;
    float4 currentClip = float4(currentNdc, depthSample, 1.0);
    float4 worldPos = mul(camera.inverseViewProjection, currentClip);
    worldPos.xyz /= max(worldPos.w, 1.0e-5);
    worldPos.w = 1.0;

    float4 currentUnjitteredClip = mul(camera.unjitteredViewProjection, worldPos);
    float2 currentUnjitteredUv = currentUnjitteredClip.xy / max(currentUnjitteredClip.w, 1.0e-5) * 0.5 + 0.5;
    float4 previousClip = mul(camera.prevUnjitteredViewProjection, worldPos);
    float2 previousUv = previousClip.xy / max(previousClip.w, 1.0e-5) * 0.5 + 0.5;
    float2 velocity = currentUnjitteredUv - previousUv;
    return float4(velocity, 0.0, 1.0);
}

float3 rgbToYCoCg(float3 c)
{
    return float3(
        dot(c, float3(0.25, 0.5, 0.25)),
        dot(c, float3(0.5, 0.0, -0.5)),
        dot(c, float3(-0.25, 0.5, -0.25)));
}

float3 yCoCgToRgb(float3 c)
{
    return float3(c.x + c.y - c.z, c.x + c.z, c.x - c.y - c.z);
}

[shader("fragment")]
float4 fragmentTAAResolveMain(VSOutput input) : SV_Target
{
    float3 currentColor = inTexture[kSceneColorHdrIndex].Sample(input.uv).rgb;
    if(postProcess.params5.x <= 0.5 || postProcess.params5.y <= 0.5)
    {
        return float4(max(currentColor, 0.0), 1.0);
    }

    float depthSample = inTexture[kGBufferDepthIndex].Sample(input.uv).r;
    float2 historyUv = input.uv;
    if(depthSample > 0.0001)
    {
        float4 currentClip = float4(input.uv * 2.0 - 1.0, depthSample, 1.0);
        float4 worldPos = mul(camera.inverseViewProjection, currentClip);
        worldPos.xyz /= max(worldPos.w, 1.0e-5);
        worldPos.w = 1.0;

        float4 previousClip = mul(camera.prevJitteredViewProjection, worldPos);
        historyUv = previousClip.xy / max(previousClip.w, 1.0e-5) * 0.5 + 0.5;
    }
    float3 historyColor = inTexture[kSceneColorHistoryReadIndex].Sample(historyUv).rgb;

    float3 minColor = rgbToYCoCg(currentColor);
    float3 maxColor = minColor;
    const float2 texel = postProcess.params1.zw;
    float2 offsets[4] = {
        float2(texel.x, 0.0),
        float2(-texel.x, 0.0),
        float2(0.0, texel.y),
        float2(0.0, -texel.y)
    };
    [unroll]
    for(int i = 0; i < 4; ++i)
    {
        float3 sampleColor = rgbToYCoCg(inTexture[kSceneColorHdrIndex].Sample(input.uv + offsets[i]).rgb);
        minColor = min(minColor, sampleColor);
        maxColor = max(maxColor, sampleColor);
    }

    float3 historyYCoCg = clamp(rgbToYCoCg(historyColor), minColor, maxColor);
    float3 clampedHistory = yCoCgToRgb(historyYCoCg);
    const bool historyOutOfBounds =
        historyUv.x < 0.0 || historyUv.y < 0.0 || historyUv.x > 1.0 || historyUv.y > 1.0;
    float blendWeight = historyOutOfBounds ? 0.0 : saturate(postProcess.params5.z);
    float3 resolved = lerp(currentColor, clampedHistory, blendWeight);
    return float4(max(resolved, 0.0), 1.0);
}
