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
#if defined(DEMO_METAL_ICB)
struct MetalIcbMaterialResources
{
    Texture2D<float4> textures[LMaterialTextureCapacity];
    SamplerState sampler;
};
[[vk::binding(LBindTextures, LSetTextures)]]
ParameterBlock<MetalIcbMaterialResources> materialResources;
float4 sampleBindlessTexture(uint index, float2 uv)
{
    return materialResources.textures[index].Sample(
        materialResources.sampler, uv);
}
#elif defined(DEMO_METAL)
[[vk::binding(LBindTextures, LSetTextures)]]
Texture2D<float4> inTextureResources[LMaterialTextureCapacity];
[[vk::binding(LBindSharedTextureSampler, LSetTextures)]]
SamplerState inTextureSampler;
float4 sampleBindlessTexture(uint index, float2 uv)
{
    return inTextureResources[index].Sample(inTextureSampler, uv);
}
#else
[[vk::binding(LBindTextures, LSetTextures)]]
Sampler2D inTexture[LMaterialTextureCapacity];
#define sampleBindlessTexture(index, uv) inTexture[index].Sample(uv)
#endif

// Set 1: Scene-level camera uniform buffer
[[vk::binding(LBindCamera, LSetScene)]]
ConstantBuffer<CameraUniforms> camera;

// Set 2: Per-draw model matrix (dynamic uniform buffer)
[[vk::binding(LBindDrawModel, LSetDraw)]]
ConstantBuffer<DrawUniforms> draw;

[[vk::binding(LBindDrawModelMdi, LSetDraw)]]
StructuredBuffer<DrawUniformsStorage> drawData;

#ifdef DEMO_D3D12
[[vk::push_constant]]
ConstantBuffer<MdiDrawIndexPushConstants> mdiDrawIndex;
#endif

//------------------------------------------------------------------------------
// Specialization Constants
//------------------------------------------------------------------------------

// Alpha test enabled (set at pipeline creation time)
#ifdef DEMO_D3D12
#ifndef DEMO_SPECIALIZATION_0
#define DEMO_SPECIALIZATION_0 0
#endif
static const bool alphaTestEnabled = (DEMO_SPECIALIZATION_0 != 0);
#else
[[vk::constant_id(0)]]
const bool alphaTestEnabled = false;
#endif

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

struct VertexOutputMdi
{
    float4 position : SV_Position;
    float2 uv       : TEXCOORD0;
    float3 worldNormal : TEXCOORD1;
    float3 worldTangent : TEXCOORD2;
    float tangentW : TEXCOORD3;
    nointerpolation uint drawIndex : TEXCOORD4;
};

[shader("vertex")]
#ifdef DEMO_D3D12
VertexOutputMdi vertexMdiMain(VertexInput input)
{
    const uint drawIndex = mdiDrawIndex.drawIndex;
#else
VertexOutputMdi vertexMdiMain(VertexInput input, uint drawIndex : DEMO_DRAW_INSTANCE_SEMANTIC)
{
#endif
    const DrawUniformsStorage mdiDraw = drawData[drawIndex];
    VertexOutputMdi output;

    float4 worldPos = mul(mdiDraw.modelMatrix, float4(input.position, 1.0));
    output.position = mul(camera.viewProjection, worldPos);
    output.uv = input.texCoord;
    output.worldNormal = normalize(mul(mdiDraw.modelMatrix, float4(input.normal, 0.0)).xyz);
    output.worldTangent = normalize(mul(mdiDraw.modelMatrix, float4(input.tangent.xyz, 0.0)).xyz);
    output.tangentW = input.tangent.w;
    output.drawIndex = drawIndex;
    return output;
}

//------------------------------------------------------------------------------
// Fragment Shader (MRT Output)
//------------------------------------------------------------------------------

float2 signNotZero(float2 v)
{
    return float2(v.x >= 0.0 ? 1.0 : -1.0, v.y >= 0.0 ? 1.0 : -1.0);
}

float2 encodeNormalOct(float3 n)
{
    n /= max(abs(n.x) + abs(n.y) + abs(n.z), 1e-6);
    if(n.z < 0.0)
    {
        n.xy = (1.0 - abs(n.yx)) * signNotZero(n.xy);
    }
    return n.xy * 0.5 + 0.5;
}

// Geometric Specular AA（Kaplanyan 2016 / Tokuyoshi-Kaplanyan 2019，
// 对齐 Filament surface_shading_lit.fs normalFiltering 及其默认参数）。
// 用世界法线的屏幕空间导数估计像素内法线方差并折算进 perceptual roughness：
// 法线变化越剧烈高光越宽越淡，抑制法线贴图细节在 TAA jitter 下的
// 单像素高光闪烁（specular aliasing / firefly）。
// GBuffer 存的是 perceptual roughness（光照端 NDF_GGX 用 a=r²），
// 故按 Filament 在 α=r² 域做加法后开两次方根返回 perceptual 域。
float filterRoughness(float perceptualRoughness, float3 worldNormal)
{
    const float kSpecularAAVariance  = 0.25;  // 上调：jitter 欠采样法线变化幅度更大，需更强折算
    const float kSpecularAAThreshold = 0.4;   // 上调：允许更大的 kernelRoughness 上限以覆盖 13° 摆动
#if defined(DEMO_D3D12) && !defined(DEMO_METAL)
    const float3 du = ddx_fine(worldNormal);
    const float3 dv = ddy_fine(worldNormal);
#else
    const float3 du = ddx(worldNormal);
    const float3 dv = ddy(worldNormal);
#endif
    const float variance = kSpecularAAVariance * (dot(du, du) + dot(dv, dv));
    const float roughness = perceptualRoughness * perceptualRoughness;
    const float kernelRoughness = min(2.0 * variance, kSpecularAAThreshold);
    const float squareRoughness = saturate(roughness * roughness + kernelRoughness);
    return sqrt(sqrt(squareRoughness));
}

struct GBufferOutput
{
    float4 gbuffer0 : SV_Target0;
    float4 gbuffer1 : SV_Target1;
    float4 gbuffer2 : SV_Target2;
};

[shader("fragment")]
GBufferOutput fragmentMain(
    VertexOutput input,
    bool isFrontFace : SV_IsFrontFace
) {
    // 1. BaseColor: sample texture if available, otherwise use factor
    float4 color = draw.baseColorFactor;
    if(draw.baseColorTextureIndex >= 0)
    {
        uint baseColorTextureIndex = DEMO_NON_UNIFORM_RESOURCE_INDEX(uint(draw.baseColorTextureIndex));
        float4 texColor = sampleBindlessTexture(baseColorTextureIndex, input.uv);
        color = float4(texColor.rgb * draw.baseColorFactor.rgb, texColor.a * draw.baseColorFactor.a);
    }
    // 2. Alpha test (specialization constant controlled)
    if (alphaTestEnabled)
    {
        if (color.a < draw.alphaCutoff)
        { 
            DEMO_DISCARD_FRAGMENT();
        }
    }

    // 3. Normal: apply normal mapping if texture available
    float3 worldNormal = normalize(input.worldNormal);

    if(draw.normalTextureIndex >= 0)
    {
        // Sample normal map (tangent space)
        uint normalTextureIndex = DEMO_NON_UNIFORM_RESOURCE_INDEX(uint(draw.normalTextureIndex));
        float3 tangentNormal = sampleBindlessTexture(normalTextureIndex, input.uv).rgb;
        // Convert from [0,1] to [-1,1]
        tangentNormal = tangentNormal * 2.0 - 1.0;
        tangentNormal.xy *= draw.normalScale;

        tangentNormal = normalize(tangentNormal);
        // Build TBN matrix
        float3 N = normalize(input.worldNormal);
        float3 T = normalize(input.worldTangent);
        // Recalculate bitangent using normal and tangent
        float3 B = normalize(cross(N, T)) * input.tangentW;

        // Transform to world space explicitly to avoid row/column-major ambiguity.
        worldNormal = normalize(tangentNormal.x * T + tangentNormal.y * B + tangentNormal.z * N);
        // worldNormal = tangentNormal;
    }
    
    // Flip normal for back faces (double-sided rendering support)
    if (!isFrontFace)
    {
        worldNormal = -worldNormal;
    }

    // 3. PBR values
    float metallic = draw.materialWorkflow == 1 ? 0.0 : draw.metallicFactor;
    float roughness = draw.materialWorkflow == 1 ? (1.0 - draw.roughnessFactor) : draw.roughnessFactor;
    float ao = 1.0;

    // Sample metallic-roughness texture (glTF: G=roughness, B=metallic)
    if(draw.metallicRoughnessTextureIndex >= 0)
    {
        uint metallicRoughnessTextureIndex = DEMO_NON_UNIFORM_RESOURCE_INDEX(uint(draw.metallicRoughnessTextureIndex));
        float4 mrSample = sampleBindlessTexture(metallicRoughnessTextureIndex, input.uv);
        if(draw.materialWorkflow == 1)
        {
            metallic = 0.0;
            roughness = 1.0 - mrSample.a * draw.roughnessFactor;
        }
        else
        {
            metallic = mrSample.b * draw.metallicFactor;
            roughness = mrSample.g * draw.roughnessFactor;
        }
    }

    // Sample occlusion texture
    if(draw.occlusionTextureIndex >= 0)
    {
        uint occlusionTextureIndex = DEMO_NON_UNIFORM_RESOURCE_INDEX(uint(draw.occlusionTextureIndex));
        float sampledAo = sampleBindlessTexture(occlusionTextureIndex, input.uv).r;
        ao = lerp(1.0, sampledAo, draw.occlusionStrength);
    }

    float3 emissive = draw.emissiveFactor.rgb;
    if(draw.emissiveTextureIndex >= 0)
    {
        uint emissiveTextureIndex = DEMO_NON_UNIFORM_RESOURCE_INDEX(uint(draw.emissiveTextureIndex));
        emissive *= sampleBindlessTexture(emissiveTextureIndex, input.uv).rgb;
    }

    // Pack outputs（roughness 经 Geometric Specular AA 调制）
    GBufferOutput output;
    output.gbuffer0 = float4(color.rgb, filterRoughness(roughness, worldNormal));
    output.gbuffer1 = float4(encodeNormalOct(worldNormal), metallic, ao);
    output.gbuffer2 = float4(emissive, 0.0);
    return output;
}

[shader("fragment")]
GBufferOutput fragmentMdiMain(
    VertexOutputMdi input,
    bool isFrontFace : SV_IsFrontFace
) {
    const DrawUniformsStorage draw = drawData[input.drawIndex];
    float4 color = draw.baseColorFactor;
    if(draw.baseColorTextureIndex >= 0)
    {
        uint baseColorTextureIndex = DEMO_NON_UNIFORM_RESOURCE_INDEX(uint(draw.baseColorTextureIndex));
        float4 texColor = sampleBindlessTexture(baseColorTextureIndex, input.uv);
        color = float4(texColor.rgb * draw.baseColorFactor.rgb, texColor.a * draw.baseColorFactor.a);
    }

    if (alphaTestEnabled)
    {
        if (color.a < draw.alphaCutoff)
        {
            DEMO_DISCARD_FRAGMENT();
        }
    }

    float3 worldNormal = normalize(input.worldNormal);
    if(draw.normalTextureIndex >= 0)
    {
        uint normalTextureIndex = DEMO_NON_UNIFORM_RESOURCE_INDEX(uint(draw.normalTextureIndex));
        float3 tangentNormal = sampleBindlessTexture(normalTextureIndex, input.uv).rgb;
        tangentNormal = tangentNormal * 2.0 - 1.0;
        tangentNormal.xy *= draw.normalScale;
        tangentNormal = normalize(tangentNormal);
        float3 N = normalize(input.worldNormal);
        float3 T = normalize(input.worldTangent);
        float3 B = normalize(cross(N, T)) * input.tangentW;
        worldNormal = normalize(tangentNormal.x * T + tangentNormal.y * B + tangentNormal.z * N);
    }
    if (!isFrontFace)
    {
        worldNormal = -worldNormal;
    }

    float metallic = draw.materialWorkflow == 1 ? 0.0 : draw.metallicFactor;
    float roughness = draw.materialWorkflow == 1 ? (1.0 - draw.roughnessFactor) : draw.roughnessFactor;
    float ao = 1.0;
    if(draw.metallicRoughnessTextureIndex >= 0)
    {
        uint metallicRoughnessTextureIndex = DEMO_NON_UNIFORM_RESOURCE_INDEX(uint(draw.metallicRoughnessTextureIndex));
        float4 mrSample = sampleBindlessTexture(metallicRoughnessTextureIndex, input.uv);
        if(draw.materialWorkflow == 1)
        {
            metallic = 0.0;
            roughness = 1.0 - mrSample.a * draw.roughnessFactor;
        }
        else
        {
            metallic = mrSample.b * draw.metallicFactor;
            roughness = mrSample.g * draw.roughnessFactor;
        }
    }
    if(draw.occlusionTextureIndex >= 0)
    {
        uint occlusionTextureIndex = DEMO_NON_UNIFORM_RESOURCE_INDEX(uint(draw.occlusionTextureIndex));
        float sampledAo = sampleBindlessTexture(occlusionTextureIndex, input.uv).r;
        ao = lerp(1.0, sampledAo, draw.occlusionStrength);
    }

    float3 emissive = draw.emissiveFactor.rgb;
    if(draw.emissiveTextureIndex >= 0)
    {
        uint emissiveTextureIndex = DEMO_NON_UNIFORM_RESOURCE_INDEX(uint(draw.emissiveTextureIndex));
        emissive *= sampleBindlessTexture(emissiveTextureIndex, input.uv).rgb;
    }

    GBufferOutput output;
    output.gbuffer0 = float4(color.rgb, filterRoughness(roughness, worldNormal));
    output.gbuffer1 = float4(encodeNormalOct(worldNormal), metallic, ao);
    output.gbuffer2 = float4(emissive, 0.0);
    return output;
}
