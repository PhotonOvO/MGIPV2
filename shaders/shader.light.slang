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
STATIC_CONST int kBloomEighthIndex                 = 13;
STATIC_CONST int kBloomSixteenthIndex              = 14;
STATIC_CONST int kBloomThirtySecondIndex           = 15;
STATIC_CONST int kBloomUpsampleSixteenthIndex      = 16;
STATIC_CONST int kBloomUpsampleEighthIndex         = 17;
STATIC_CONST int kBloomUpsampleQuarterIndex        = 18;
STATIC_CONST int kBloomOutputIndex                 = 19;
STATIC_CONST int kColorGradingLutIndex             = 20;
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
    const float maxShadowDistance = lighting.light.cascadeSplitDistances.w;
    const float fadeWidth = max(maxShadowDistance * 0.08, 2.0);
    const float distanceFade = saturate((maxShadowDistance - viewDepth) / fadeWidth);
    if(distanceFade <= 0.0)
        return 1.0;

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

    return lerp(1.0, visibility / 9.0, distanceFade);
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
    // 标准 windowed inverse-square 衰减（UE/Frostbite）：window = (1-(d/r)^4)^2。
    // 旧公式额外乘 range²（range=100 时放大 10000 倍），把掠射面 NdotL 逐帧翻转
    // 放大成满幅 HDR 闪光（残余高光闪烁根因）；强度应只由 intensity 控制。
    const float distanceRatio = distanceToLight / max(light.range, 0.0001);
    const float rangeAttenuation = pow(saturate(1.0 - pow(distanceRatio, 4.0)), 2.0);
    float spotAttenuation = 1.0;
    if(light.lightType == LLightTypeSpot)
    {
        const vec3 spotDirection = normalize(light.spotDirection);
        const float innerCos = cos(light.spotInnerAngle);
        const float outerCos = cos(light.spotOuterAngle);
        const float spotCos = dot(spotDirection, -L);
        spotAttenuation = smoothstep(outerCos, innerCos, spotCos);
    }

    const vec3 radiance = light.color * light.intensity * invDistanceSq * rangeAttenuation * spotAttenuation;
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

// evaluateEnvironmentIBL now accepts an SSR sample (float4: rgb=radiance, a=confidence).
// Where confidence > 0, the SSR radiance replaces the prefiltered IBL specular radiance
// while keeping the same BRDF weight -- preventing the double-count that occurred when SSR
// was added on top of the full IBL specular term.  Confidence == 0 falls back to IBL
// seamlessly, providing automatic hole-filling with energy-conserving blending.
vec3 evaluateEnvironmentIBL(vec3 N, vec3 V, vec3 baseColor, float metallic, float roughness, float ao, float4 ssr)
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
        // Blend SSR radiance into the specular term at the same BRDF weight, replacing the
        // prefiltered env sample where SSR confidence is high.
        const vec3 ssrRadiance = lerp(prefiltered, ssr.rgb, saturate(ssr.a));
        specular = ssrRadiance * (F0 * envBRDF.x + envBRDF.y);
    }
    else
#endif
    {
        const vec2 envBRDF = approximateEnvironmentBRDF(NdotV, roughness);
        const vec3 irradiance = sampleEquirectEnvironment(N, maxMip);
        const vec3 prefiltered = sampleEquirectEnvironment(R, roughness * maxMip);
        diffuse = kD * irradiance * baseColor / PI;
        // Same SSR blend for the equirect path.
        const vec3 ssrRadiance = lerp(prefiltered, ssr.rgb, saturate(ssr.a));
        specular = ssrRadiance * (F * envBRDF.x + envBRDF.y) * kEquirectSpecularFallbackWeight;
    }

    const uint debugMode = uint(lighting.light.iblDebugInfo.x);
    if(debugMode == 1u)
    {
        return diffuse * ao * intensity;
    }
    if(debugMode == 2u)
    {
        // Debug mode 2 visualises specular, which now includes the SSR blend result.
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

// Sharpness of the depth weight used by the SSR upsample, in inverse view-space
// metres: taps whose linear depth differs from the shaded pixel by ~0.25 m are
// down-weighted to ~e^-1, suppressing reflection bleed across silhouettes.
STATIC_CONST float kSSRUpsampleDepthSharpness = 4.0;

// Convert a reversed-Z NDC depth to positive view-space distance.  Identical
// derivation to ssr_trace.slang's extractNear/linearizeDepth pair: for the
// engine's reversed-Z projection, near = b / (a + 1) with a = projection[2][2]
// and b = projection[2][3] (GLM [col][row] mapped to Slang row-major), and
// linearZ = near / ndcDepth.
float linearizeViewZ(float ndcDepth)
{
    const float a = camera.projection[2][2];
    const float b = camera.projection[2][3];
    const float denom = a + 1.0;
    const float near = abs(denom) > 1.0e-5 ? abs(b / denom) : 0.1;
    return near / max(ndcDepth, 1.0e-7);
}

// Returns float4(ssrColor * F * fade, confidence).  confidence == 0 when SSR is disabled
// or no hit was found, allowing evaluateEnvironmentIBL to fall back cleanly to pure IBL.
float4 evaluateSSR(float2 uv, vec3 N, vec3 V, vec3 baseColor, float metallic, float roughness)
{
    if(lighting.light.phase7Info.y <= 0.5 || roughness > 0.72)
    {
        return float4(0.0, 0.0, 0.0, 0.0);
    }

    // Depth-aware 4-tap upsample of the half-res SSR texture.  The full-res depth at
    // the shaded pixel is the reference; each half-res tap is compared against the
    // full-res depth at its own 2x2 block centre (fullCoord = halfCoord * 2 + 1, the
    // same half->full mapping used by gtao and ssr_trace).
    uint ssrWidth, ssrHeight;
    inTexture[kSSRTextureIndex].GetDimensions(ssrWidth, ssrHeight);
    uint depthWidth, depthHeight;
    inTexture[kGBufferDepthIndex].GetDimensions(depthWidth, depthHeight);
    const int2 ssrMaxCoord   = int2(int(ssrWidth) - 1, int(ssrHeight) - 1);
    const int2 depthMaxCoord = int2(int(depthWidth) - 1, int(depthHeight) - 1);

    const float refNdc = inTexture[kGBufferDepthIndex].SampleLevel(uv, 0.0).r;
    const float refViewZ = linearizeViewZ(max(refNdc, 1.0e-7));

    // Bilinear footprint in half-res texel space.
    const float2 texelPos = uv * float2(ssrWidth, ssrHeight) - 0.5;
    const float2 baseTexel = floor(texelPos);
    const float2 lerpW = texelPos - baseTexel;
    const int2 baseCoord = int2(baseTexel);

    const int2 centerCoord = clamp(int2(texelPos + 0.5), int2(0, 0), ssrMaxCoord);
    const float4 centerTap = inTexture[kSSRTextureIndex].Load(int3(centerCoord, 0));

    float4 sum = float4(0.0);
    float weightSum = 0.0;
    [unroll]
    for(int i = 0; i < 4; ++i)
    {
        const int2 offset = int2(i & 1, i >> 1);
        const int2 tapCoord = clamp(baseCoord + offset, int2(0, 0), ssrMaxCoord);
        const float4 tap = inTexture[kSSRTextureIndex].Load(int3(tapCoord, 0));
        const int2 tapFullCoord = clamp(tapCoord * 2 + int2(1, 1), int2(0, 0), depthMaxCoord);
        const float tapNdc = inTexture[kGBufferDepthIndex].Load(int3(tapFullCoord, 0)).r;
        const float tapViewZ = linearizeViewZ(max(tapNdc, 1.0e-7));
        const float bilinearWeight = (offset.x == 1 ? lerpW.x : 1.0 - lerpW.x)
                                   * (offset.y == 1 ? lerpW.y : 1.0 - lerpW.y);
        const float depthWeight = exp(-abs(refViewZ - tapViewZ) * kSSRUpsampleDepthSharpness);
        const float weight = bilinearWeight * depthWeight;
        sum += tap * weight;
        weightSum += weight;
    }
    // If every tap was rejected by the depth weight, fall back to the centre tap so
    // thin features never produce a hard zero where the raw SSR still had a hit.
    const float4 ssrSample = weightSum > 1.0e-5 ? sum / weightSum : centerTap;

    const vec3 F0 = lerp(vec3(0.04, 0.04, 0.04), baseColor, metallic);
    const vec3 F = Fresnel_Schlick_Roughness(max(dot(N, V), 0.001), F0, roughness);
    const float roughnessFade = saturate(1.0 - roughness / 0.72);
    const float confidence = saturate(ssrSample.a) * roughnessFade;
    return float4(ssrSample.rgb * F, confidence);
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

    // Evaluate SSR first so its confidence can drive IBL specular blending, avoiding the
    // double-count that results from adding SSR on top of full IBL specular.
    float4 ssr = evaluateSSR(input.uv, N, V, baseColor, metallic, roughness);
    vec3 ambient = evaluateEnvironmentIBL(N, V, baseColor, metallic, roughness, ao, ssr);
    vec3 directional = evaluateDirectionalLight(N, V, worldPos, baseColor, metallic, roughness, viewDepth);
    vec3 pointLights = evaluatePointLights(input.uv, N, V, worldPos, baseColor, metallic, roughness);

    vec3 color = ambient + directional + pointLights + emissiveSample.rgb;
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

    // Evaluate SSR first so its confidence can drive IBL specular blending.
    float4 ssr = evaluateSSR(input.uv, N, V, baseColor, metallic, roughness);
    vec3 ambient = evaluateEnvironmentIBL(N, V, baseColor, metallic, roughness, ao, ssr);
    vec3 directional = evaluateDirectionalLight(N, V, worldPos, baseColor, metallic, roughness, viewDepth);
    vec3 pointLights = evaluatePointLights(input.uv, N, V, worldPos, baseColor, metallic, roughness);

    return float4(max(ambient + directional + pointLights + emissiveSample.rgb, 0.0), 1.0);
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

float3 samplePostTexture(int textureIndex, float2 uv)
{
    return max(inTexture[textureIndex].Sample(uv).rgb, 0.0);
}

float luminance(float3 color)
{
    return dot(color, float3(0.2126, 0.7152, 0.0722));
}

float3 karisAverage(float3 a, float3 b, float3 c, float3 d)
{
    float wa = 1.0 / (1.0 + luminance(a));
    float wb = 1.0 / (1.0 + luminance(b));
    float wc = 1.0 / (1.0 + luminance(c));
    float wd = 1.0 / (1.0 + luminance(d));
    return (a * wa + b * wb + c * wc + d * wd) / max(wa + wb + wc + wd, 1.0e-4);
}

float3 sampleBloomKaris2x2(int textureIndex, float2 uv, float2 texelSize)
{
    float2 o = texelSize * 0.5;
    float3 a = samplePostTexture(textureIndex, uv + float2(-o.x, -o.y));
    float3 b = samplePostTexture(textureIndex, uv + float2( o.x, -o.y));
    float3 c = samplePostTexture(textureIndex, uv + float2(-o.x,  o.y));
    float3 d = samplePostTexture(textureIndex, uv + float2( o.x,  o.y));
    return karisAverage(a, b, c, d);
}

float3 sampleBloomDownsample13Tap(int textureIndex, float2 uv, float2 texelSize)
{
    float x = texelSize.x;
    float y = texelSize.y;

    float3 a = samplePostTexture(textureIndex, uv + float2(-2.0 * x,  2.0 * y));
    float3 b = samplePostTexture(textureIndex, uv + float2( 0.0,      2.0 * y));
    float3 c = samplePostTexture(textureIndex, uv + float2( 2.0 * x,  2.0 * y));
    float3 d = samplePostTexture(textureIndex, uv + float2(-2.0 * x,  0.0));
    float3 e = samplePostTexture(textureIndex, uv);
    float3 f = samplePostTexture(textureIndex, uv + float2( 2.0 * x,  0.0));
    float3 g = samplePostTexture(textureIndex, uv + float2(-2.0 * x, -2.0 * y));
    float3 h = samplePostTexture(textureIndex, uv + float2( 0.0,     -2.0 * y));
    float3 i = samplePostTexture(textureIndex, uv + float2( 2.0 * x, -2.0 * y));
    float3 j = samplePostTexture(textureIndex, uv + float2(-x,  y));
    float3 k = samplePostTexture(textureIndex, uv + float2( x,  y));
    float3 l = samplePostTexture(textureIndex, uv + float2(-x, -y));
    float3 m = samplePostTexture(textureIndex, uv + float2( x, -y));

    return e * 0.125
         + (a + c + g + i) * 0.03125
         + (b + d + f + h) * 0.0625
         + (j + k + l + m) * 0.125;
}

float3 sampleBloomUpsampleTent(int textureIndex, float2 uv, float2 texelSize, float radius)
{
    float2 d = texelSize * radius;
    float3 color = samplePostTexture(textureIndex, uv + float2(-d.x, -d.y)) * 1.0;
    color += samplePostTexture(textureIndex, uv + float2( 0.0, -d.y)) * 2.0;
    color += samplePostTexture(textureIndex, uv + float2( d.x, -d.y)) * 1.0;
    color += samplePostTexture(textureIndex, uv + float2(-d.x,  0.0)) * 2.0;
    color += samplePostTexture(textureIndex, uv) * 4.0;
    color += samplePostTexture(textureIndex, uv + float2( d.x,  0.0)) * 2.0;
    color += samplePostTexture(textureIndex, uv + float2(-d.x,  d.y)) * 1.0;
    color += samplePostTexture(textureIndex, uv + float2( 0.0,  d.y)) * 2.0;
    color += samplePostTexture(textureIndex, uv + float2( d.x,  d.y)) * 1.0;
    return color * 0.0625;
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

float3 sampleColorGradingLut(float3 color)
{
    const float lutSize = 32.0;
    const float lutWidth = lutSize * lutSize;
    float3 coord = saturate(color) * (lutSize - 1.0);
    float slice0 = floor(coord.b);
    float slice1 = min(slice0 + 1.0, lutSize - 1.0);
    float blend = frac(coord.b);

    float2 uv0 = (float2(coord.r + slice0 * lutSize, coord.g) + 0.5) / float2(lutWidth, lutSize);
    float2 uv1 = (float2(coord.r + slice1 * lutSize, coord.g) + 0.5) / float2(lutWidth, lutSize);
    float3 grade0 = samplePostTexture(kColorGradingLutIndex, uv0);
    float3 grade1 = samplePostTexture(kColorGradingLutIndex, uv1);
    return lerp(grade0, grade1, blend);
}

float3 applyColorGrade(float3 color, float2 uv)
{
    float gray = luminance(color);
    color = lerp(float3(gray, gray, gray), color, postProcess.params3.x);
    color = (color - 0.5) * postProcess.params3.y + 0.5;
    color = pow(max(color, 0.0), float3(1.0 / max(postProcess.params3.z, 0.01)));
    color = lerp(color, sampleColorGradingLut(color), saturate(postProcess.params4.z));

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
    float3 hdrColor = sampleBloomKaris2x2(selectTemporalSceneColorIndex(), input.uv, postProcess.params1.xy);
    float threshold = max(postProcess.params0.z, 0.0);
    if(threshold > 0.001)
    {
        float brightness = max(hdrColor.r, max(hdrColor.g, hdrColor.b));
        float contribution = smoothstep(threshold * 0.5, threshold, brightness);
        hdrColor *= contribution;
    }
    return float4(hdrColor, 1.0);
}

[shader("fragment")]
float4 fragmentBloomDownsampleMain(VSOutput input) : SV_Target
{
    int sourceIndex = postProcess.params5.y > 0.5 ? int(postProcess.params5.y + 0.5) : kBloomHalfIndex;
    return float4(sampleBloomDownsample13Tap(sourceIndex, input.uv, postProcess.params1.xy), 1.0);
}

[shader("fragment")]
float4 fragmentBloomUpsampleMain(VSOutput input) : SV_Target
{
    int currentIndex = int(postProcess.params5.y + 0.5);
    int lowerIndex = int(postProcess.params5.z + 0.5);
    float radius = max(postProcess.params5.w, 0.25);
    float3 current = samplePostTexture(currentIndex, input.uv);
    float3 lower = sampleBloomUpsampleTent(lowerIndex, input.uv, postProcess.params1.xy, radius);
    return float4(current + lower, 1.0);
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
    float3 bloom = samplePostTexture(kBloomOutputIndex, input.uv);
    if(luminance(bloom) <= 1.0e-6)
    {
        bloom = samplePostTexture(kBloomHalfIndex, input.uv) * 0.65
              + samplePostTexture(kBloomQuarterIndex, input.uv) * 0.35;
    }
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

// 辅助：float3 三分量最大值
float taaMax3(float3 v) { return max(v.x, max(v.y, v.z)); }

// Filament tonemap：c/(1+max3(c))，压制 HDR 高光
float3 taaTonemapRgb(float3 c) { return c / (1.0 + taaMax3(c)); }

// Filament untonemap：逆变换，epsilon 防除零（epsilon = 1/65504）
float3 taaUntonemapRgb(float3 c)
{
    const float eps = 1.0 / 65504.0;
    return c / max(eps, 1.0 - taaMax3(c));
}

// Catmull-Rom 5-tap 历史采样（去掉 4 个角采样），移植自 Filament taa.mat:197-259
// texSize = 1.0 / texelSize（即像素分辨率），texelSize = params1.zw
float3 sampleCatmullRom5Tap(int texIdx, float2 uv, float2 texSize)
{
    float2 samplePos = uv * texSize;
    float2 texPos1   = floor(samplePos - 0.5) + 0.5;
    float2 f         = samplePos - texPos1;
    float2 f2        = f * f;
    float2 f3        = f2 * f;
    float2 w0  = f2 - 0.5 * (f3 + f);
    float2 w1  = 1.5 * f3 - 2.5 * f2 + 1.0;
    float2 w3  = 0.5 * (f3 - f2);
    float2 w2  = 1.0 - w0 - w1 - w3;
    float2 w12 = w1 + w2;
    float2 invTex   = 1.0 / texSize;
    float2 texPos0  = (texPos1 - 1.0) * invTex;
    float2 texPos3  = (texPos1 + 2.0) * invTex;
    float2 texPos12 = (texPos1 + w2 / w12) * invTex;
    float k0 = w12.x * w0.y;
    float k1 = w0.x  * w12.y;
    float k2 = w12.x * w12.y;
    float k3 = w3.x  * w12.y;
    float k4 = w12.x * w3.y;
    float3 s0 = inTexture[texIdx].SampleLevel(float2(texPos12.x, texPos0.y),  0).rgb;
    float3 s1 = inTexture[texIdx].SampleLevel(float2(texPos0.x,  texPos12.y), 0).rgb;
    float3 s2 = inTexture[texIdx].SampleLevel(float2(texPos12.x, texPos12.y), 0).rgb;
    float3 s3 = inTexture[texIdx].SampleLevel(float2(texPos3.x,  texPos12.y), 0).rgb;
    float3 s4 = inTexture[texIdx].SampleLevel(float2(texPos12.x, texPos3.y),  0).rgb;
    float3 result = k0*s0 + k1*s1 + k2*s2 + k3*s3 + k4*s4;
    float kSum = k0 + k1 + k2 + k3 + k4;
    return max(float3(0), result / kSum);
}

// Lanczos 重建核（Filament PostProcessManager filterWeights 同款，a = filterWidth）
float taaLanczos(float x, float a)
{
    if(x <= 1.0e-5)
    {
        return 1.0;
    }
    if(abs(x) <= a)
    {
        const float pix = 3.14159265 * x;
        return (a * sin(pix) * sin(pix / a)) / (pix * pix);
    }
    return 0.0;
}

// 盒裁剪：把 history 向盒中心按比例收缩（Playdead clip_aabb）。
// 旧实现沿 history→filtered 求线段入盒参数 t = saturate(max3(imin))：
// 当某色度通道 |filtered-history| 趋零（1/(eps+r) 爆炸，r 为小负数时
// eps+r 还会翻符号）而 history 该通道又在盒外时 t→1，整条历史被替换为
// 当前帧值——亮度通道被色度通道劫持拖满，单帧镜面尖峰因此穿透
// clamp 盒与防火花上限（其基准 clampedHistory 已被污染）。
// 向盒中心收缩有界且不越过盒面，且各通道按同一比例收缩保持色相方向。
float3 taaClipToBox(float3 boxMin, float3 boxMax, float3 filtered, float3 history)
{
    const float eps = 0.0001;
    float3 center  = 0.5 * (boxMax + boxMin);
    float3 extents = 0.5 * (boxMax - boxMin) + eps;
    float  ma = taaMax3(abs((history - center) / extents));
    return ma > 1.0 ? center + (history - center) / ma : history;
}

[shader("fragment")]
float4 fragmentTAAResolveMain(VSOutput input) : SV_Target
{
    float3 currentColor = inTexture[kSceneColorHdrIndex].Sample(input.uv).rgb;
    // TAA 关闭早退（行为不变）
    if(postProcess.params5.x <= 0.5 || postProcess.params5.y <= 0.5)
    {
        return float4(max(currentColor, 0.0), 1.0);
    }

    // 调试开关（params4 = filterInput / varianceBox / lottes / catmullRom）与当前帧 jitter（params2.xy，像素单位）
    const bool useFilterInput = postProcess.params4.x > 0.5;
    const bool useVarianceBox = postProcess.params4.y > 0.5;
    const bool useLottes      = postProcess.params4.z > 0.5;
    const bool useCatmullRom  = postProcess.params4.w > 0.5;
    const float2 jitterPx     = postProcess.params2.xy;

    // --- 1. velocity 重投影（天空像素 velocity=(0,0)，historyUv 自动回退到 input.uv）---
    float2 velocity  = inTexture[kVelocityIndex].Sample(input.uv).xy;
    float2 historyUv = input.uv - velocity;

    // --- 2. 历史采样（Catmull-Rom 5-tap 或双线性）---
    float2 texSize = 1.0 / postProcess.params1.zw;  // params1.zw = texelSize → 1/texelSize = 像素分辨率
    float3 historyRgb = useCatmullRom
        ? sampleCatmullRom5Tap(kSceneColorHistoryReadIndex, historyUv, texSize)
        : inTexture[kSceneColorHistoryReadIndex].SampleLevel(historyUv, 0).rgb;

    // --- 3. 3x3 邻域采样（保留 RGB，供输入滤波与 clamp 盒共用）---
    const float2 texel = postProcess.params1.zw;
    // 顺序：[-1,-1][0,-1][1,-1][-1,0][0,0][1,0][-1,1][0,1][1,1]
    float3 rgbS[9];
    rgbS[0] = inTexture[kSceneColorHdrIndex].SampleLevel(input.uv + float2(-texel.x, -texel.y), 0).rgb;
    rgbS[1] = inTexture[kSceneColorHdrIndex].SampleLevel(input.uv + float2( 0.0,     -texel.y), 0).rgb;
    rgbS[2] = inTexture[kSceneColorHdrIndex].SampleLevel(input.uv + float2( texel.x, -texel.y), 0).rgb;
    rgbS[3] = inTexture[kSceneColorHdrIndex].SampleLevel(input.uv + float2(-texel.x,  0.0    ), 0).rgb;
    rgbS[4] = currentColor;
    rgbS[5] = inTexture[kSceneColorHdrIndex].SampleLevel(input.uv + float2( texel.x,  0.0    ), 0).rgb;
    rgbS[6] = inTexture[kSceneColorHdrIndex].SampleLevel(input.uv + float2(-texel.x,  texel.y), 0).rgb;
    rgbS[7] = inTexture[kSceneColorHdrIndex].SampleLevel(input.uv + float2( 0.0,      texel.y), 0).rgb;
    rgbS[8] = inTexture[kSceneColorHdrIndex].SampleLevel(input.uv + float2( texel.x,  texel.y), 0).rgb;

    // --- 4. filterInput：以 jitter 为中心的 Lanczos 核重建像素中心的“未抖动”当前值 ---
    // 本引擎 jitter 经 projection[2][0/1] 加偏移后，渲染图 R(p) = 真实场景 S(p + jitterPx)，
    // 故邻域样本 i 的真实位置为 offset_i + jitterPx，核权重 = lanczos(|offset_i + jitterPx|)。
    // 该滤波消除当前帧的亚像素抖动，是 clamp 盒与混合的稳定前提（Filament filterInput=true 默认开启）。
    float3 filteredRgb = currentColor;
    if(useFilterInput)
    {
        const float2 kOffsets[9] = {
            float2(-1.0, -1.0), float2(0.0, -1.0), float2(1.0, -1.0),
            float2(-1.0,  0.0), float2(0.0,  0.0), float2(1.0,  0.0),
            float2(-1.0,  1.0), float2(0.0,  1.0), float2(1.0,  1.0)
        };
        float3 acc  = float3(0, 0, 0);
        float  wsum = 0.0;
        [unroll]
        for(int i = 0; i < 9; ++i)
        {
            // Karis 亮度加权（UE4 tonemapped filtering）：核权重除以 (1+luma)，
            // 单像素 HDR 高光尖峰（specular firefly）在进入 clamp 盒/混合前被压制，
            // 而面积型高光（多像素一致亮）权重同比缩放、归一化后不受影响。
            const float3 c = rgbS[i];
            const float luma = dot(c, float3(0.299, 0.587, 0.114));
            const float w = taaLanczos(length(kOffsets[i] + jitterPx), 1.0) / (1.0 + luma);
            acc  += c * w;
            wsum += w;
        }
        filteredRgb = wsum > 1.0e-5 ? max(acc / wsum, 0.0) : currentColor;
    }

    // --- 5. YCoCg 空间 clamp 盒：圆角 3x3 AABB（必开）∩ Salvi 方差盒（可关）---
    // 在 tonemapped 域构建 clamp 盒，单帧高光尖峰被压制后不再拉高上界
    float3 s[9];
    [unroll]
    for(int i = 0; i < 9; ++i)
    {
        s[i] = rgbToYCoCg(taaTonemapRgb(max(rgbS[i], 0.0)));
    }
    float3 crossMin = min(s[4], min(min(s[1], s[3]), min(s[5], s[7])));
    float3 crossMax = max(s[4], max(max(s[1], s[3]), max(s[5], s[7])));
    float3 box9Min  = min(crossMin, min(min(s[0], s[2]), min(s[6], s[8])));
    float3 box9Max  = max(crossMax, max(max(s[0], s[2]), max(s[6], s[8])));
    float3 boxMin   = (crossMin + box9Min) * 0.5;
    float3 boxMax   = (crossMax + box9Max) * 0.5;
    if(useVarianceBox)
    {
        float3 m0 = s[4];
        float3 m1 = m0 * m0;
        [unroll]
        for(int i = 1; i < 9; i += 2)  // s[1],s[3],s[5],s[7] — 4 样本，合计 5 个
        {
            m0 += s[i];
            m1 += s[i] * s[i];
        }
        float3 mu    = m0 * (1.0 / 5.0);
        float3 sigma = sqrt(max(float3(0, 0, 0), m1 * (1.0 / 5.0) - mu * mu));
        boxMin = max(boxMin, mu - sigma);
        boxMax = min(boxMax, mu + sigma);
    }

    // --- 6. 精确盒裁剪（沿 history→filtered 方向，替代逐通道 clamp）---
    // clamp 盒已在 tonemapped 域，filteredRgb 与 historyRgb 同样先 tonemap 再进入 taaClipToBox
    float3 tonemappedFilteredYCoCg = rgbToYCoCg(taaTonemapRgb(max(filteredRgb, 0.0)));
    float3 clippedHistoryYCoCg     = taaClipToBox(boxMin, boxMax, tonemappedFilteredYCoCg,
                                                  rgbToYCoCg(taaTonemapRgb(max(historyRgb, 0.0))));
    float3 clampedHistory          = taaUntonemapRgb(yCoCgToRgb(clippedHistoryYCoCg));

    // --- 7. 越界置零 ---
    const bool historyOutOfBounds = historyUv.x < 0.0 || historyUv.y < 0.0
                                 || historyUv.x > 1.0 || historyUv.y > 1.0;
    float blendWeight = historyOutOfBounds ? 0.0 : saturate(postProcess.params5.z);

    // --- 7.5 历史引导防火花：限制当前帧相对历史的单帧亮度增长率 ---
    // 亚像素细几何（布幔轮廓）随 jitter 相位翻转产生 1px 竖线状单帧镜面尖峰
    //（200~500× 背景亮度），该列 3x3 邻域自身含亮样本，clamp 盒无法拦截。
    // 以裁剪后历史亮度为基准设上限：尖峰被压制；真实变亮区域按每帧 ~4 倍
    // 几何收敛（全黑→满幅约 4 帧），静态场景下无副作用。
    if(!historyOutOfBounds)
    {
        const float kFireflyRatio  = 4.0;
        const float kFireflyOffset = 0.05;
        const float lumaFiltered = luminance(filteredRgb);
        const float lumaCap = luminance(clampedHistory) * kFireflyRatio + kFireflyOffset;
        if(lumaFiltered > lumaCap)
        {
            filteredRgb *= lumaCap / max(lumaFiltered, 1.0e-5);
        }
    }

    // --- 8. tonemap 包夹混合（当前帧用滤波后的值）---
    // clampedHistory 已经历 tonemapped 域裁剪后 taaUntonemapRgb 回线性，此处再次 tonemap 进入混合域——与 Filament 路径一致
    float3 tonemappedCurrent = taaTonemapRgb(max(filteredRgb, 0.0));
    float3 tonemappedHistory = taaTonemapRgb(max(clampedHistory, 0.0));

    // --- 9. Lottes 抗闪烁（可选，默认关闭对齐 Filament preventFlickering=false）---
    // Filament 中 alpha 是当前帧权重，亮度差越大 alpha 越小 → 越倒向稳定的历史。
    // 此处 blendWeight 是历史权重，等价变换：historyWeight = 1 - currentWeight * diff²。
    if(useLottes && !historyOutOfBounds)
    {
        float lumaC = tonemappedCurrent.x + tonemappedCurrent.y * 2.0 + tonemappedCurrent.z;
        float lumaH = tonemappedHistory.x + tonemappedHistory.y * 2.0 + tonemappedHistory.z;
        float lumaDiff = 1.0 - abs(lumaC - lumaH) / (0.001 + max(lumaC, lumaH));
        blendWeight = 1.0 - (1.0 - blendWeight) * lumaDiff * lumaDiff;
    }

    // --- 10. 混合 + untonemap ---
    float3 mixed    = lerp(tonemappedCurrent, tonemappedHistory, blendWeight);
    float3 resolved = taaUntonemapRgb(mixed);

    return float4(max(resolved, 0.0), 1.0);
}
