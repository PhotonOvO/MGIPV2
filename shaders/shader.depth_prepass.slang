// Depth prepass shader: writes main camera depth with alpha-cutout support.

#include "shader_io.h"

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
};

#if defined(DEMO_METAL_ICB)
struct MetalIcbMaterialResources
{
    Texture2D<float4> textures[LMaterialTextureCapacity];
    SamplerState sampler;
};
[[vk::binding(LBindTextures, LSetTextures)]]
ParameterBlock<MetalIcbMaterialResources> materialResources;
DemoMetalSampler2D getBindlessTexture(uint index)
{
    return makeDemoMetalSampler2D(
        materialResources.textures[index], materialResources.sampler);
}
#elif defined(DEMO_METAL)
[[vk::binding(LBindTextures, LSetTextures)]]
Texture2D<float4> inTextureResources[LMaterialTextureCapacity];
[[vk::binding(LBindSharedTextureSampler, LSetTextures)]]
SamplerState inTextureSampler;
DemoMetalSampler2D getBindlessTexture(uint index)
{
    return makeDemoMetalSampler2D(inTextureResources[index], inTextureSampler);
}
#else
[[vk::binding(LBindTextures, LSetTextures)]]
Sampler2D inTexture[LMaterialTextureCapacity];
#define getBindlessTexture(index) inTexture[index]
#endif

[[vk::binding(LBindCamera, LSetScene)]]
ConstantBuffer<CameraUniforms> camera;

[[vk::binding(LBindDrawModel, LSetDraw)]]
ConstantBuffer<DrawUniforms> draw;

[[vk::binding(LBindDrawModelMdi, LSetDraw)]]
StructuredBuffer<DrawUniformsStorage> drawData;

#ifdef DEMO_D3D12
[[vk::push_constant]]
ConstantBuffer<MdiDrawIndexPushConstants> mdiDrawIndex;
#endif

#ifdef DEMO_D3D12
#ifndef DEMO_SPECIALIZATION_0
#define DEMO_SPECIALIZATION_0 0
#endif
static const bool alphaTestEnabled = (DEMO_SPECIALIZATION_0 != 0);
#else
[[vk::constant_id(0)]]
const bool alphaTestEnabled = false;
#endif

[shader("vertex")]
VertexOutput vertexMain(VertexInput input)
{
    VertexOutput output;
    float4 worldPos = mul(draw.modelMatrix, float4(input.position, 1.0));
    output.position = mul(camera.viewProjection, worldPos);
    output.uv = input.texCoord;
    return output;
}

struct VertexOutputMdi
{
    float4 position : SV_Position;
    float2 uv       : TEXCOORD0;
    nointerpolation uint drawIndex : TEXCOORD1;
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
    output.drawIndex = drawIndex;
    return output;
}

[shader("fragment")]
void fragmentMain(VertexOutput input)
{
    if(!alphaTestEnabled)
    {
        return;
    }

    float alpha = draw.baseColorFactor.a;
    if(draw.baseColorTextureIndex >= 0)
    {
        uint baseColorTextureIndex = DEMO_NON_UNIFORM_RESOURCE_INDEX(uint(draw.baseColorTextureIndex));
        alpha *= getBindlessTexture(baseColorTextureIndex).Sample(input.uv).a;
    }
    if(alpha < draw.alphaCutoff)
    {
        DEMO_DISCARD_FRAGMENT();
    }
}

[shader("fragment")]
void fragmentMdiMain(VertexOutputMdi input)
{
    const DrawUniformsStorage mdiDraw = drawData[input.drawIndex];
    if(!alphaTestEnabled)
    {
        return;
    }

    float alpha = mdiDraw.baseColorFactor.a;
    if(mdiDraw.baseColorTextureIndex >= 0)
    {
        uint baseColorTextureIndex = DEMO_NON_UNIFORM_RESOURCE_INDEX(uint(mdiDraw.baseColorTextureIndex));
        alpha *= getBindlessTexture(baseColorTextureIndex).Sample(input.uv).a;
    }
    if(alpha < mdiDraw.alphaCutoff)
    {
        DEMO_DISCARD_FRAGMENT();
    }
}
