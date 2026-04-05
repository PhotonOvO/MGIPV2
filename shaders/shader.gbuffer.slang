// GBuffer Pass Shader: Output PBR material properties to MRT
// Uses modern descriptor bindings for camera and model matrices
// Vertex format: Position(12) + Normal(12) + TexCoord(8) + Tangent(16) = 48 bytes

#include "shader_io.h"

//------------------------------------------------------------------------------
// Input Structures
//------------------------------------------------------------------------------

struct VertexInput
{
    [[vk::location(LVGltfPosition)]] float3 position : POSITION;
    [[vk::location(LVGltfNormal)]]   float3 normal   : NORMAL;
    [[vk::location(LVGltfTexCoord)]] float2 texCoord : TEXCOORD0;
    [[vk::location(LVGltfTangent)]]  float4 tangent  : TANGENT;
};

struct VertexOutput
{
    float4 position : SV_Position;
    float2 uv       : TEXCOORD0;
    float3 worldNormal : TEXCOORD1;
    float3 worldTangent : TEXCOORD2;
    float tangentW : TEXCOORD3;
};

//------------------------------------------------------------------------------
// Resource Bindings
//------------------------------------------------------------------------------

// Set 0: Bindless textures (for material sampling)
[[vk::binding(LBindTextures, LSetTextures)]]
Sampler2D inTexture[];

// Set 1: Scene-level camera uniform buffer
[[vk::binding(LBindCamera, LSetScene)]]
ConstantBuffer<CameraUniforms> camera;

// Set 2: Per-draw model matrix (dynamic uniform buffer)
[[vk::binding(LBindDrawModel, LSetDraw)]]
ConstantBuffer<DrawUniforms> draw;

//------------------------------------------------------------------------------
// Specialization Constants
//------------------------------------------------------------------------------

// Alpha test enabled (set at pipeline creation time)
[[vk::constant_id(0)]]
const bool alphaTestEnabled = false;

//------------------------------------------------------------------------------
// Vertex Shader
//------------------------------------------------------------------------------

[shader("vertex")]
VertexOutput vertexMain(VertexInput input)
{
    VertexOutput output;

    // Transform to clip space using uniform buffers
    // GLM matrices are column-major, HLSL/Slang also uses column-major by default
    // Use mul(matrix, vector) for correct M×v transformation (column vector)
    float4 worldPos = mul(draw.modelMatrix, float4(input.position, 1.0));
    output.position = mul(camera.viewProjection, worldPos);

    // Pass UV
    output.uv = input.texCoord;

    // Transform normal and tangent to world space
    // For normals/tangents (w=0), we use the same transform without translation
    // Use mul(matrix, vector) for correct M×v transformation
    output.worldNormal = normalize(mul(draw.modelMatrix, float4(input.normal, 0.0)).xyz);
    output.worldTangent = normalize(mul(draw.modelMatrix, float4(input.tangent.xyz, 0.0)).xyz);
    output.tangentW = input.tangent.w;

    return output;
}

//------------------------------------------------------------------------------
// Fragment Shader (MRT Output)
//------------------------------------------------------------------------------

[shader("fragment")]
void fragmentMain(
    VertexOutput input,
    out float4 outGBuffer0 : SV_Target0,
    out float4 outGBuffer1 : SV_Target1,
    out float4 outGBuffer2 : SV_Target2
) {
    // 1. BaseColor: sample texture if available, otherwise use factor
    float4 color = draw.baseColorFactor;
    if(draw.baseColorTextureIndex >= 0)
    {
        float4 texColor = inTexture[draw.baseColorTextureIndex].Sample(input.uv);
        color = float4(texColor.rgb * draw.baseColorFactor.rgb, texColor.a * draw.baseColorFactor.a);
    }

    // 2. Alpha test (specialization constant controlled)
    if (alphaTestEnabled)
    {
        if (color.a < draw.alphaCutoff)
        {
            discard;
        }
    }

    // 3. Normal: apply normal mapping if texture available
    float3 worldNormal = normalize(input.worldNormal);

    if(draw.normalTextureIndex >= 0)
    {
        // Sample normal map (tangent space)
        float3 tangentNormal = inTexture[draw.normalTextureIndex].Sample(input.uv).rgb;
        // Convert from [0,1] to [-1,1]
        tangentNormal = tangentNormal * 2.0 - 1.0;
        // Apply normal scale
        tangentNormal.xy *= draw.normalScale;

        // Build TBN matrix
        float3 N = normalize(input.worldNormal);
        float3 T = normalize(input.worldTangent);
        // Recalculate bitangent using normal and tangent
        float3 B = cross(N, T) * input.tangentW;
        float3x3 TBN = float3x3(T, B, N);

        // Transform to world space
        worldNormal = normalize(mul(TBN, tangentNormal));
    }

    // 3. PBR values
    float metallic = draw.metallicFactor;
    float roughness = draw.roughnessFactor;
    float ao = 1.0;

    // Sample metallic-roughness texture (glTF: G=metallic, B=roughness)
    if(draw.metallicRoughnessTextureIndex >= 0)
    {
        float4 mrSample = inTexture[draw.metallicRoughnessTextureIndex].Sample(input.uv);
        metallic = mrSample.g * draw.metallicFactor;
        roughness = mrSample.b * draw.roughnessFactor;
    }

    // Sample occlusion texture
    if(draw.occlusionTextureIndex >= 0)
    {
        ao = inTexture[draw.occlusionTextureIndex].Sample(input.uv).r;
    }

    // Pack outputs
    outGBuffer0 = float4(color.rgb, 0.0);
    outGBuffer1 = float4(worldNormal * 0.5 + 0.5, 0.0);
    outGBuffer2 = float4(metallic, roughness, ao, 0.0);
}