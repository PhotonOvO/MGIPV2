// LightPass shader: Full PBR lighting from GBuffer with a single directional shadow map

#include "shader_io.h"

[[vk::binding(0, 0)]]
Sampler2D inTexture[];

[[vk::binding(1, 0)]]
ConstantBuffer<CameraUniforms> camera;

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
    float P20 = camera.projection[2][2];
    float P32 = camera.projection[3][2];
    float viewZ = P32 / (depth - P20);
    float viewX = ndcX * viewZ / camera.projection[0][0];
    float viewY = ndcY * viewZ / camera.projection[1][1];

    float3 right   = float3(camera.view[0][0], camera.view[1][0], camera.view[2][0]);
    float3 up      = float3(camera.view[0][1], camera.view[1][1], camera.view[2][1]);
    float3 forward = float3(camera.view[0][2], camera.view[1][2], camera.view[2][2]);
    return right * viewX + up * viewY + forward * viewZ + camera.cameraPosition;
}

float sampleShadow(vec3 worldPos, vec3 worldNormal)
{
    float4 shadowClip = mul(lightParams.worldToShadow, float4(worldPos, 1.0));
    float3 shadowNdc = shadowClip.xyz / shadowClip.w;

    float2 shadowUv = float2(shadowNdc.x * 0.5 + 0.5, shadowNdc.y * -0.5 + 0.5);
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
    float bias = max(depthBias, (1.0 - saturate(dot(worldNormal, lightDir))) * normalBias);

    float visibility = 0.0;
    [unroll]
    for(int y = -1; y <= 1; ++y)
    {
        [unroll]
        for(int x = -1; x <= 1; ++x)
        {
            float2 offset = float2(x, y) * texelSize;
            float shadowDepth = inTexture[kShadowMapIndex].Sample(shadowUv + offset).r;
            visibility += (shadowNdc.z - bias) <= shadowDepth ? 1.0 : 0.0;
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
    float4 materialSample  = inTexture[kGBufferMaterialIndex].Sample(input.uv);
    float  depthSample     = inTexture[kGBufferDepthIndex].Sample(input.uv).r;

    if(depthSample >= 0.9999)
    {
        return float4(lightParams.ambientColorAndTexelSize.rgb, 1.0);
    }

    vec3 worldPos = reconstructWorldPosition(input.uv, depthSample);
    float distanceToCamera = length(worldPos - camera.cameraPosition);
    float shadowFade = distanceToCamera <= lightParams.shadowMetrics.x ? 1.0 : 0.0;
    vec3 color = computePBRLighting(baseColorSample.rgb, normalSample.rgb, worldPos,
                                    materialSample.r, materialSample.g, materialSample.b, shadowFade);
    return float4(color, 1.0);
}
