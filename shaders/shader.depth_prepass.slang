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

[[vk::binding(LBindTextures, LSetTextures)]]
Sampler2D inTexture[];

[[vk::binding(LBindCamera, LSetScene)]]
ConstantBuffer<CameraUniforms> camera;

[[vk::binding(LBindDrawModel, LSetDraw)]]
ConstantBuffer<DrawUniforms> draw;

[[vk::binding(LBindDrawModelMdi, LSetDraw)]]
StructuredBuffer<DrawUniforms> drawData;

[[vk::constant_id(0)]]
const bool alphaTestEnabled = false;

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
VertexOutputMdi vertexMdiMain(VertexInput input, uint drawIndex : SV_StartInstanceLocation)
{
    const DrawUniforms mdiDraw = drawData[drawIndex];
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
        uint baseColorTextureIndex = NonUniformResourceIndex(uint(draw.baseColorTextureIndex));
        alpha *= inTexture[baseColorTextureIndex].Sample(input.uv).a;
    }
    if(alpha < draw.alphaCutoff)
    {
        discard;
    }
}

[shader("fragment")]
void fragmentMdiMain(VertexOutputMdi input)
{
    const DrawUniforms mdiDraw = drawData[input.drawIndex];
    if(!alphaTestEnabled)
    {
        return;
    }

    float alpha = mdiDraw.baseColorFactor.a;
    if(mdiDraw.baseColorTextureIndex >= 0)
    {
        uint baseColorTextureIndex = NonUniformResourceIndex(uint(mdiDraw.baseColorTextureIndex));
        alpha *= inTexture[baseColorTextureIndex].Sample(input.uv).a;
    }
    if(alpha < mdiDraw.alphaCutoff)
    {
        discard;
    }
}
