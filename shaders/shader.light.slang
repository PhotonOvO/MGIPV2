// LightPass shader: Full PBR lighting from GBuffer
// Implements Cook-Torrance BRDF with directional light and IBL

#include "shader_io.h"
#include "shader_ibl.h"

//------------------------------------------------------------------------------
// Resource Bindings
//------------------------------------------------------------------------------

// Set 0: Combined descriptor set for LightPass
// Binding 0: Bindless textures (GBuffer + Depth)
[[vk::binding(0, 0)]]
Sampler2D inTexture[];

// Binding 1: Camera uniform buffer (for inverse projection)
[[vk::binding(1, 0)]]
ConstantBuffer<CameraUniforms> camera;

// Binding 2: Prefiltered environment cube map (IBL specular)
[[vk::binding(2, 0)]]
SamplerCube prefilteredMap;

// Binding 3: DFG LUT texture (IBL BRDF integration)
[[vk::binding(3, 0)]]
Sampler2D dfgLUT;

// Binding 4: Irradiance cube map (IBL diffuse)
[[vk::binding(4, 0)]]
SamplerCube irradianceMap;

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

// LightParams struct defined in shader_io.h

// Push constant binding (binding 0, range 0-48 bytes)
[[vk::push_constant]]
LightParams lightParams;

//------------------------------------------------------------------------------
// Constants
//------------------------------------------------------------------------------

STATIC_CONST float PI = 3.14159265359;

//------------------------------------------------------------------------------
// PBR Helper Functions
//------------------------------------------------------------------------------

// Normal distribution function (Trowbridge-Reitz GGX)
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

// Geometry function (Smith's method with GGX)
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

// Fresnel function (Schlick approximation)
vec3 Fresnel_Schlick(float cosTheta, vec3 F0)
{
    return F0 + (1.0 - F0) * pow(clamp(1.0 - cosTheta, 0.0, 1.0), 5.0);
}

//------------------------------------------------------------------------------
// World Position Reconstruction from Depth
//------------------------------------------------------------------------------

vec3 reconstructWorldPosition(float2 uv, float depth)
{
    // Convert UV to NDC coordinates
    float ndcX = uv.x * 2.0 - 1.0;
    float ndcY = uv.y * 2.0 - 1.0;

    // For perspective projection with GLM_FORCE_DEPTH_ZERO_TO_ONE:
    // ndcZ = P20 + P32/viewZ
    // viewZ = P32 / (ndcZ - P20)
    // Where P20 = projection[2][2], P32 = projection[3][2]
    float P20 = camera.projection[2][2];  // far/(far-near) for [0,1] depth
    float P32 = camera.projection[3][2];  // -near*far/(far-near)

    float viewZ = P32 / (depth - P20);

    // Reconstruct view position from NDC and viewZ
    // viewX = ndcX * viewZ / projection[0][0]
    // viewY = ndcY * viewZ / projection[1][1]
    float viewX = ndcX * viewZ / camera.projection[0][0];
    float viewY = ndcY * viewZ / camera.projection[1][1];

    // Transform to world position using inverse view matrix
    // view matrix transforms world -> view: viewPos = view * worldPos
    // inverse: worldPos = view^T * viewPos + cameraPosition
    // (view is rotation + translation: view = [R | -R*camPos])

    // Extract rotation from view matrix (column-major in GLM)
    // view[0] = right (column 0), view[1] = up (column 1), view[2] = forward (column 2)
    // These are the columns of R, so R^T has them as rows
    float3 right   = float3(camera.view[0][0], camera.view[1][0], camera.view[2][0]);
    float3 up      = float3(camera.view[0][1], camera.view[1][1], camera.view[2][1]);
    float3 forward = float3(camera.view[0][2], camera.view[1][2], camera.view[2][2]);

    // worldPos = R^T * viewPos + cameraPosition
    float3 worldPos = right * viewX + up * viewY + forward * viewZ + camera.cameraPosition;

    return worldPos;
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
    // Light direction (already normalized, pointing TO light)
    vec3 L = lightParams.lightDirection;

    // View direction (from world position to camera)
    vec3 V = normalize(camera.cameraPosition - worldPos);

    // Normal (decode from [0,1] to [-1,1])
    vec3 N = normalize(normal * 2.0 - 1.0);

    // Half vector
    vec3 H = normalize(V + L);

    // Calculate dot products
    float NdotV = max(dot(N, V), 0.001);
    float NdotL = max(dot(N, L), 0.001);
    float NdotH = max(dot(N, H), 0.0);
    float VdotH = max(dot(V, H), 0.0);

    // F0 (surface reflection at normal incidence)
    // For metals, F0 = baseColor (colored Fresnel)
    // For non-metals, F0 = 0.04 (approximate for most dielectrics)
    vec3 F0 = lerp(float3(0.04, 0.04, 0.04), baseColor, metallic);

    // Light radiance (assuming directional light with constant intensity)
    vec3 radiance = lightParams.lightColor;

    // Cook-Torrance BRDF
    float D = NDF_GGX(NdotH, roughness);
    vec3 F = Fresnel_Schlick(VdotH, F0);
    float G = Geometry_Smith(NdotV, NdotL, roughness);

    // Specular term
    vec3 numerator = D * G * F;
    float denominator = 4.0 * NdotV * NdotL + 0.001;
    vec3 specular = numerator / denominator;

    // Energy conservation: kS + kD = 1
    // kS = F (Fresnel), kD = (1 - F) * (1 - metallic)
    // For metals, diffuse contribution is minimal
    vec3 kS = F;
    vec3 kD = (1.0 - kS) * (1.0 - metallic);

    // Diffuse term (Lambertian)
    vec3 diffuse = kD * baseColor / PI;

    // Final lighting (direct)
    vec3 Lo = (diffuse + specular) * radiance * NdotL;

    // IBL contribution
    // Sample irradiance cube map for diffuse IBL
    vec3 irradiance = irradianceMap.Sample(N).rgb;

    // Compute IBL contribution using helper from shader_ibl.h
    // maxMipLevel = 5 (from IBLResources configuration)
    vec3 iblContribution = computeIBLContribution(
        N, V, baseColor, metallic, roughness,
        prefilteredMap, dfgLUT,
        5,  // maxMipLevel
        irradiance
    );

    // Ambient term (reduced when IBL is active)
    vec3 ambient = lightParams.ambientColor * baseColor * ao * 0.1;

    // Final color: direct lighting + IBL + ambient
    vec3 color = ambient + Lo + iblContribution;

    // HDR tonemapping (simple Reinhard)
    color = color / (color + float3(1.0, 1.0, 1.0));

    // Gamma correction
    color = pow(color, float3(1.0/2.2, 1.0/2.2, 1.0/2.2));

    return color;
}

//------------------------------------------------------------------------------
// Vertex Shader
//------------------------------------------------------------------------------

struct VSOutput {
    float4 position : SV_Position;
    float2 uv       : TEXCOORD0;
};

[shader("vertex")]
VSOutput vertexMain(uint vertexIndex : SV_VertexID) {
    VSOutput output;

    // Fullscreen triangle vertices
    float2 positions[3] = {
        float2(-1.0, -1.0),
        float2( 3.0, -1.0),
        float2(-1.0,  3.0)
    };

    output.position = float4(positions[vertexIndex], 0.0, 1.0);
    output.uv = positions[vertexIndex] * 0.5 + 0.5;
    // Note: UV flip removed - Y orientation handled by projection matrix

    return output;
}

//------------------------------------------------------------------------------
// Fragment Shader
//------------------------------------------------------------------------------

[shader("fragment")]
float4 fragmentMain(VSOutput input) : SV_Target {
    // Sample GBuffer textures
    float4 baseColorSample = inTexture[kGBufferBaseColorIndex].Sample(input.uv);
    float4 normalSample    = inTexture[kGBufferNormalIndex].Sample(input.uv);
    float4 materialSample  = inTexture[kGBufferMaterialIndex].Sample(input.uv);
    float  depthSample     = inTexture[kGBufferDepthIndex].Sample(input.uv).r;

    // Check if pixel was written (depth != 1.0 means something was rendered)
    // For standard depth (1.0 at far plane), depth 1.0 means nothing rendered
    if(depthSample >= 0.9999)
    {
        // Return ambient color for background
        return float4(lightParams.ambientColor, 1.0);
    }
     
    // Extract GBuffer data
    vec3 baseColor = baseColorSample.rgb;
    vec3 normal = normalSample.rgb;
    float metallic = materialSample.r;
    float roughness = materialSample.g;
    float ao       = materialSample.b;

    // Reconstruct world position from depth
    vec3 worldPos = reconstructWorldPosition(input.uv, depthSample);

    // Compute PBR lighting
    vec3 color = computePBRLighting(baseColor, normal, worldPos, metallic, roughness, ao);

    return float4(color, 1.0);
}