// MGIF editor occurrence-token pass. The uint output is a snapshot-local
// framebuffer token, never a document identifier or renderer draw index.

#include "shader_io.h"

struct EditorPickingVertexInput
{
    [[vk::location(LVGltfPosition)]] float3 position : POSITION;
    [[vk::location(LVGltfTexCoord)]] float2 texCoord : TEXCOORD0;
};

struct EditorPickingVertexOutput
{
    float4 position : SV_Position;
    float2 uv : TEXCOORD0;
    nointerpolation uint drawIndex : TEXCOORD1;
    nointerpolation uint framebufferToken : TEXCOORD2;
};

struct EditorPickingPushConstants
{
    uint drawIndex;
    uint framebufferToken;
};

[[vk::binding(LBindTextures, LSetTextures)]]
Sampler2D inTexture[];

[[vk::binding(LBindCamera, LSetScene)]]
ConstantBuffer<CameraUniforms> camera;

[[vk::binding(LBindDrawModelMdi, LSetDraw)]]
StructuredBuffer<DrawUniformsStorage> drawData;

[[vk::push_constant]]
ConstantBuffer<EditorPickingPushConstants> editorPicking;

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
EditorPickingVertexOutput editorPickingVertexMain(EditorPickingVertexInput input)
{
    const DrawUniformsStorage draw = drawData[editorPicking.drawIndex];
    EditorPickingVertexOutput output;
    const float4 worldPosition = mul(draw.modelMatrix, float4(input.position, 1.0));
    output.position = mul(camera.viewProjection, worldPosition);
    output.uv = input.texCoord;
    output.drawIndex = editorPicking.drawIndex;
    output.framebufferToken = editorPicking.framebufferToken;
    return output;
}

[shader("fragment")]
uint editorPickingFragmentMain(EditorPickingVertexOutput input) : SV_Target0
{
    const DrawUniformsStorage draw = drawData[input.drawIndex];
    if (alphaTestEnabled)
    {
        float alpha = draw.baseColorFactor.a;
        if (draw.baseColorTextureIndex >= 0)
        {
            const uint textureIndex =
                NonUniformResourceIndex(uint(draw.baseColorTextureIndex));
            alpha *= inTexture[textureIndex].Sample(input.uv).a;
        }
        if (alpha < draw.alphaCutoff)
        {
            discard;
        }
    }
    return input.framebufferToken;
}
