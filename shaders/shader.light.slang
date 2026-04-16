// LightPass shader: Full PBR lighting from GBuffer with cascaded shadow maps (CSM)

#include "shader_io.h"

[[vk::binding(0, 0)]]
Sampler2D inTexture[];

[[vk::binding(LBindShadowMap, LSetTextures)]]
Sampler2DArray shadowMap;

[[vk::binding(LBindCamera, LSetScene)]]
ConstantBuffer<CameraUniforms> camera;

[[vk::binding(LBindLighting, LSetScene)]]
ConstantBuffer<LightingUniforms> lighting;

STATIC_CONST int kGBufferBaseColorIndex = 0;
STATIC_CONST int kGBufferNormalIndex    = 1;
STATIC_CONST int kGBufferMaterialIndex  = 2;
STATIC_CONST int kGBufferDepthIndex     = 3;
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

float computeViewDepth(vec3 worldPos)
{
    float4 viewPos = mul(camera.view, float4(worldPos, 1.0));
    return abs(viewPos.z);
}

// Select cascade based on view-space depth
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

// CSM shadow sampling with adaptive bias
float sampleCSMShadow(vec3 worldPos, vec3 worldNormal, float viewDepth)
{
    int cascadeIdx = selectCascadeIndex(viewDepth);

    float4 shadowClip = mul(lighting.light.worldToShadow[cascadeIdx], float4(worldPos, 1.0));
    float3 shadowNdc = shadowClip.xyz / shadowClip.w;
    float2 shadowUv = shadowNdc.xy * 0.5 + 0.5;

    // Bounds check
    if(shadowUv.x < 0.0 || shadowUv.x > 1.0 || shadowUv.y < 0.0 || shadowUv.y > 1.0)
        return 1.0;
    if(shadowNdc.z <= 0.0 || shadowNdc.z >= 1.0)
        return 1.0;

    // Adaptive bias (cascade-scaled)
    float3 lightDir = normalize(lighting.light.lightDirectionAndShadowStrength.xyz);
    float cosTheta = saturate(abs(dot(worldNormal, lightDir)));
    float sinTheta = sqrt(saturate(1.0 - cosTheta * cosTheta));
    float slope = sinTheta / max(cosTheta, 0.01);

    float baseBias = lighting.light.shadowMetrics.y;
    float slopeBias = lighting.light.shadowMetrics.z;
    float cascadeScale = 0.5;  // Scale factor for cascade bias

    float cascadeBias = baseBias * (1.0 + float(cascadeIdx) * cascadeScale);
    float bias = cascadeBias + slopeBias * (1.0 + float(cascadeIdx) * cascadeScale) * slope;

    // 3x3 PCF
    float texelSize = lighting.light.shadowMetrics.x;
    float visibility = 0.0;

    [unroll]
    for(int y = -1; y <= 1; ++y)
    {
        [unroll]
        for(int x = -1; x <= 1; ++x)
        {
            float2 offset = float2(x, y) * texelSize;
            float3 sampleCoord = float3(shadowUv + offset, float(cascadeIdx));  // Layer = cascade index
            float shadowDepth = shadowMap.Sample(sampleCoord).r;
            visibility += (shadowNdc.z + bias >= shadowDepth) ? 1.0 : 0.0;
        }
    }
    return visibility / 9.0;
}

vec3 computePBRLighting(vec3 baseColor, vec3 normalEncoded, vec3 worldPos, float metallic, float roughness, float ao,
                        float shadowFade, float viewDepth)
{
    vec3 L = normalize(lighting.light.lightDirectionAndShadowStrength.xyz);
    vec3 V = normalize(camera.cameraPosition - worldPos);
    vec3 N = normalize(normalEncoded * 2.0 - 1.0);
    vec3 H = normalize(V + L);

    float NdotV = max(dot(N, V), 0.001);
    float NdotL = max(dot(N, L), 0.001);
    float NdotH = max(dot(N, H), 0.0);
    float VdotH = max(dot(V, H), 0.0);

    vec3 F0 = lerp(float3(0.04, 0.04, 0.04), baseColor, metallic);
    vec3 radiance = lighting.light.lightColorAndNormalBias.rgb;

    float D = NDF_GGX(NdotH, roughness);
    vec3 F = Fresnel_Schlick(VdotH, F0);
    float G = Geometry_Smith(NdotV, NdotL, roughness);

    vec3 numerator = D * G * F;
    float denominator = 4.0 * NdotV * NdotL + 0.001;
    vec3 specular = numerator / denominator;
    vec3 kS = F;
    vec3 kD = (1.0 - kS) * (1.0 - metallic);
    vec3 diffuse = kD * baseColor / PI;

    float shadowVisibility = sampleCSMShadow(worldPos, N, viewDepth);
    float shadowStrength = lighting.light.lightDirectionAndShadowStrength.w * shadowFade;
    float directVisibility = lerp(1.0, shadowVisibility, shadowStrength);
    vec3 Lo = (diffuse + specular) * radiance * NdotL * directVisibility;
    vec3 ambient = lighting.light.ambientColorAndTexelSize.rgb * baseColor * ao;

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
    float4 normalSample    = inTexture[kGBufferNormalIndex].Sample(input.uv);
    float  depthSample     = inTexture[kGBufferDepthIndex].Sample(input.uv).r;

    if(depthSample <= 0.0001)
    {
        return float4(0.0, 0.0, 0.0, 1.0);
    }

    vec3 worldPos = reconstructWorldPosition(input.uv, depthSample);
    vec3 worldNormal = normalize(normalSample.rgb * 2.0 - 1.0);
    float viewDepth = computeViewDepth(worldPos);
    float shadowVisibility = sampleCSMShadow(worldPos, worldNormal, viewDepth);
    return float4(shadowVisibility, shadowVisibility, shadowVisibility, 1.0);
}
