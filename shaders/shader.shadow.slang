// Shadow map generation shader.
// Renders a single depth map for a directional light and supports alpha-masked materials.

#include "shader_io.h"

[[vk::constant_id(0)]]
const bool alphaTestEnabled = false;

// Set 0: bindless material textures
[[vk::binding(LBindTextures, LSetTextures)]]
Sampler2D inTexture[];

// Set 1: shadow matrices and sampling parameters
[[vk::binding(0, LSetScene)]]
ConstantBuffer<ShadowUniforms> shadowUniforms;

// Set 2: per-draw model data
[[vk::binding(LBindDrawModel, LSetDraw)]]
ConstantBuffer<DrawUniforms> draw;

struct VertexInput
{
    [[vk::location(LVGltfPosition)]] float3 position : POSITION;
    [[vk::location(LVGltfTexCoord)]] float2 texCoord : TEXCOORD0;
};

struct VertexOutput
{
    float4 position : SV_Position;
    float2 uv       : TEXCOORD0;
};

[shader("vertex")]
VertexOutput vertexMain(VertexInput input)
{
    VertexOutput output;

    float4 worldPos = mul(draw.modelMatrix, float4(input.position, 1.0));
    output.position = mul(shadowUniforms.lightViewProjectionMatrix, worldPos);
    output.uv = input.texCoord;

    return output;
}

[shader("fragment")]
void fragmentMain(VertexOutput input)
{
    if(alphaTestEnabled)
    {
        float alpha = draw.baseColorFactor.a;
        if(draw.baseColorTextureIndex >= 0)
        {
            alpha *= inTexture[draw.baseColorTextureIndex].Sample(input.uv).a;
        }

        if(alpha < draw.alphaCutoff)
        {
            discard;
        }
    }
}
