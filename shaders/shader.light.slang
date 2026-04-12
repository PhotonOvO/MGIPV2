// LightPass shader: Full PBR lighting from GBuffer
// Implements Cook-Torrance BRDF with a single directional shadow map.

#include "shader_io.h"
#include "shader_ibl.h"

//------------------------------------------------------------------------------
// Resource Bindings
//------------------------------------------------------------------------------

[[vk::binding(0, 0)]]
Sampler2D inTexture[];

[[vk::binding(1, 0)]]
ConstantBuffer<CameraUniforms> camera;

[[vk::binding(2, 0)]]
SamplerCube prefilteredMap;

[[vk::binding(3, 0)]]
Sampler2D dfgLUT;

[[vk::binding(4, 0)]]
SamplerCube irradianceMap;

[[vk::binding(5, 0)]]
Texture2D<float> shadowMap;

[[vk::binding(6, 0)]]
SamplerComparisonState shadowSampler;

[[vk::binding(7, 0)]]
ConstantBuffer<ShadowUniforms> shadowUniforms;

//------------------------------------------------------------------------------
// GBuffer Texture Indices
//------------------------------------------------------------------------------

STATIC_CONST int kGBufferBaseColorIndex = 0;
STATIC_CONST int kGBufferNormalIndex    = 1;
STATIC_CONST int kGBufferMaterialIndex  = 2;
STATIC_CONST int kGBufferDepthIndex     = 3;

//------------------------------------------------------------------------------
// Push Constants for Light Parameters
//------------------------------------------------------------------------------

[[vk::push_constant]]
LightParams lightParams;

STATIC_CONST float PI = 3.14159265359;

//------------------------------------------------------------------------------
// PBR Helper Functions
//------------------------------------------------------------------------------

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

    float num = NdotV;
    float denom = NdotV * (1.0 - k) + k;

    return num / denom;
}

float Geometry_Smith(float NdotV, float NdotL, float roughness)
{
    float ggx1 = Geometry_SchlickGGX(NdotV, roughness);
    float ggx2 = Geometry_SchlickGGX(NdotL, roughness);

    return ggx1 * ggx2;
}

vec3 Fresnel_Schlick(float cosTheta, vec3 F0)
{
    return F0 + (1.0 - F0) * pow(clamp(1.0 - cosTheta, 0.0, 1.0), 5.0);
}

//------------------------------------------------------------------------------
// Shadow Mapping
//------------------------------------------------------------------------------

float sampleShadowPCF(vec3 shadowCoord)
{
    int radius = int(shadowUniforms.shadowBiasAndKernel.w + 0.5);
    float visibility = 0.0;
    int taps = 0;

    for(int y = -radius; y <= radius; ++y)
    {
        for(int x = -radius; x <= radius; ++x)
        {
            float2 offset = float2(x, y) * shadowUniforms.shadowMapMetrics.x;
            visibility += shadowMap.SampleCmpLevelZero(shadowSampler, shadowCoord.xy + offset, shadowCoord.z);
            taps++;
        }
    }

    return visibility / max(1, taps);
}

float calculateShadow(vec3 worldPos, vec3 normal)
{
    float shadowIntensity = shadowUniforms.lightDirectionAndIntensity.w;
    if(shadowIntensity <= 0.0)
    {
        return 1.0;
    }

    float cameraDistance = length(camera.cameraPosition - worldPos);
    if(cameraDistance > shadowUniforms.shadowMapMetrics.y)
    {
        return 1.0;
    }

    vec3 biasedWorldPos = worldPos + normal * shadowUniforms.shadowMapMetrics.z;
    vec4 shadowTexPos = mul(shadowUniforms.worldToShadowTextureMatrix, vec4(biasedWorldPos, 1.0));
    vec3 shadowCoord = shadowTexPos.xyz / max(shadowTexPos.w, 1e-5);

    if(shadowCoord.x < 0.0 || shadowCoord.x > 1.0 ||
       shadowCoord.y < 0.0 || shadowCoord.y > 1.0 ||
       shadowCoord.z < 0.0 || shadowCoord.z > 1.0)
    {
        return 1.0;
    }

    float visibility = sampleShadowPCF(shadowCoord);
    return lerp(1.0, visibility, shadowIntensity);
}

//------------------------------------------------------------------------------
// World Position Reconstruction from Depth
//------------------------------------------------------------------------------

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

//------------------------------------------------------------------------------
// Main PBR Lighting Function
//------------------------------------------------------------------------------

vec3 computePBRLighting(
    vec3 baseColor,
    vec3 normal,
    vec3 worldPos,
    float metallic,
    float roughness,
    float ao
)
{
    vec3 L = lightParams.lightDirection;
    vec3 V = normalize(camera.cameraPosition - worldPos);
    vec3 N = normalize(normal * 2.0 - 1.0);
    vec3 H = normalize(V + L);

    float NdotV = max(dot(N, V), 0.001);
    float NdotL = max(dot(N, L), 0.001);
    float NdotH = max(dot(N, H), 0.0);
    float VdotH = max(dot(V, H), 0.0);

    vec3 F0 = lerp(float3(0.04, 0.04, 0.04), baseColor, metallic);
    vec3 radiance = lightParams.lightColor;

    float D = NDF_GGX(NdotH, roughness);
    vec3 F = Fresnel_Schlick(VdotH, F0);
    float G = Geometry_Smith(NdotV, NdotL, roughness);

    vec3 numerator = D * G * F;
    float denominator = 4.0 * NdotV * NdotL + 0.001;
    vec3 specular = numerator / denominator;

    vec3 kS = F;
    vec3 kD = (1.0 - kS) * (1.0 - metallic);
    vec3 diffuse = kD * baseColor / PI;

    float shadowFactor = calculateShadow(worldPos, N);
    vec3 Lo = (diffuse + specular) * radiance * NdotL * shadowFactor;

    vec3 irradiance = irradianceMap.Sample(N).rgb;
    vec3 iblContribution = computeIBLContribution(
        N, V, baseColor, metallic, roughness,
        prefilteredMap, dfgLUT,
        5,
        irradiance
    );

    vec3 ambient = lightParams.ambientColor * baseColor * ao * 0.1;
    vec3 color = ambient + Lo + iblContribution;

    color = color / (color + float3(1.0, 1.0, 1.0));
    color = pow(color, float3(1.0 / 2.2, 1.0 / 2.2, 1.0 / 2.2));

    return color;
}

//------------------------------------------------------------------------------
// Vertex Shader
//------------------------------------------------------------------------------

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

//------------------------------------------------------------------------------
// Fragment Shader
//------------------------------------------------------------------------------

[shader("fragment")]
float4 fragmentMain(VSOutput input) : SV_Target
{
    float4 baseColorSample = inTexture[kGBufferBaseColorIndex].Sample(input.uv);
    float4 normalSample    = inTexture[kGBufferNormalIndex].Sample(input.uv);
    float4 materialSample  = inTexture[kGBufferMaterialIndex].Sample(input.uv);
    float  depthSample     = inTexture[kGBufferDepthIndex].Sample(input.uv).r;

    if(depthSample >= 0.9999)
    {
        return float4(lightParams.ambientColor, 1.0);
    }

    vec3 baseColor = baseColorSample.rgb;
    vec3 normal = normalSample.rgb;
    float metallic = materialSample.r;
    float roughness = materialSample.g;
    float ao = materialSample.b;

    vec3 worldPos = reconstructWorldPosition(input.uv, depthSample);
    vec3 N = normalize(normal * 2.0 - 1.0);

    float shadowFactor = calculateShadow(worldPos, N);

    if(lightParams.debugMode == 1)
    {
        return float4(shadowFactor, shadowFactor, shadowFactor, 1.0);
    }
    else if(lightParams.debugMode == 2)
    {
        vec4 shadowTexPos = mul(
            shadowUniforms.worldToShadowTextureMatrix,
            vec4(worldPos + N * shadowUniforms.shadowMapMetrics.z, 1.0));
        vec3 shadowCoord = shadowTexPos.xyz / max(shadowTexPos.w, 1e-5);
        return float4(saturate(shadowCoord.xy), 0.0, 1.0);
    }
    else if(lightParams.debugMode == 3)
    {
        vec4 viewPos = mul(camera.view, vec4(worldPos, 1.0));
        float normalizedDepth = (-viewPos.z) / max(1.0, shadowUniforms.shadowMapMetrics.y);
        return float4(normalizedDepth, normalizedDepth, normalizedDepth, 1.0);
    }
    else if(lightParams.debugMode == 4)
    {
        vec4 lightClipPos = mul(shadowUniforms.lightViewProjectionMatrix, vec4(worldPos, 1.0));
        vec2 lightXY = lightClipPos.xy / max(lightClipPos.w, 1e-5);
        return float4(lightXY * 0.5 + 0.5, 0.0, 1.0);
    }
    else if(lightParams.debugMode == 5)
    {
        vec4 shadowTexPos = mul(shadowUniforms.worldToShadowTextureMatrix, vec4(worldPos, 1.0));
        float shadowDepth = shadowTexPos.z / max(shadowTexPos.w, 1e-5);
        return float4(shadowDepth, shadowDepth, shadowDepth, 1.0);
    }

    vec3 color = computePBRLighting(baseColor, normal, worldPos, metallic, roughness, ao);
    return float4(color, 1.0);
}
