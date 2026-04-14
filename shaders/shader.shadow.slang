// Shadow pass shader: renders a directional shadow map with alpha-cutout support

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

[shader("vertex")]
VertexOutput vertexMain(VertexInput input)
{
    VertexOutput output;
    float4 worldPos = mul(draw.modelMatrix, float4(input.position, 1.0));
    float3 worldNormal = normalize(mul((float3x3)draw.modelMatrix, input.normal));
    float3 dirToLight = normalize(camera.shadowDirectionAndSlopeBias.xyz);
    float constantBias = camera.shadowConstantBias;
    float slopeScaleBias = camera.shadowDirectionAndSlopeBias.w;
    float ndotl = saturate(abs(dot(worldNormal, dirToLight)));
    float slopeBias = slopeScaleBias * (1.0 - ndotl);
    worldPos.xyz += worldNormal * (constantBias + slopeBias);
    output.position = mul(camera.viewProjection, worldPos);
    output.uv = input.texCoord;
    return output;
}

[shader("fragment")]
void fragmentMain(VertexOutput input)
{
    if(draw.alphaMode != LAlphaMask || draw.baseColorTextureIndex < 0)
    {
        return;
    }

    float alpha = inTexture[draw.baseColorTextureIndex].Sample(input.uv).a * draw.baseColorFactor.a;
    if(alpha < draw.alphaCutoff)
    {
        discard;
    }
}
