// Forward Pass Shader: Render transparent objects with alpha blending
// Used for BLEND alpha mode materials

#include "shader_io.h"

//------------------------------------------------------------------------------
// Specialization constant (consistent with GBuffer, but not used)
//------------------------------------------------------------------------------

[[vk::constant_id(0)]]
const bool alphaTestEnabled = false;

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
};

struct VertexOutputMdi
{
    float4 position : SV_Position;
    float2 uv       : TEXCOORD0;
    float3 worldNormal : TEXCOORD1;
    nointerpolation uint drawIndex : TEXCOORD2;
};

//------------------------------------------------------------------------------
// Resource Bindings (same as GBuffer)
//------------------------------------------------------------------------------

[[vk::binding(LBindTextures, LSetTextures)]]
Sampler2D inTexture[];

[[vk::binding(LBindCamera, LSetScene)]]
ConstantBuffer<CameraUniforms> camera;

[[vk::binding(LBindDrawModel, LSetDraw)]]
ConstantBuffer<DrawUniforms> draw;

[[vk::binding(LBindDrawModelMdi, LSetDraw)]]
StructuredBuffer<DrawUniforms> drawData;

//------------------------------------------------------------------------------
// Vertex Shader
//------------------------------------------------------------------------------

[shader("vertex")]
VertexOutput vertexMain(VertexInput input)
{
    VertexOutput output;

    // Transform to clip space
    float4 worldPos = mul(draw.modelMatrix, float4(input.position, 1.0));
    output.position = mul(camera.viewProjection, worldPos);

    // Pass UV
    output.uv = input.texCoord;

    // Transform normal to world space
    output.worldNormal = normalize(mul(draw.modelMatrix, float4(input.normal, 0.0)).xyz);

    return output;
}

[shader("vertex")]
VertexOutputMdi vertexMdiMain(VertexInput input, uint drawIndex : SV_StartInstanceLocation)
{
    VertexOutputMdi output;
    const DrawUniforms mdiDraw = drawData[drawIndex];

    float4 worldPos = mul(mdiDraw.modelMatrix, float4(input.position, 1.0));
    output.position = mul(camera.viewProjection, worldPos);
    output.uv = input.texCoord;
    output.worldNormal = normalize(mul(mdiDraw.modelMatrix, float4(input.normal, 0.0)).xyz);
    output.drawIndex = drawIndex;

    return output;
}

//------------------------------------------------------------------------------
// Fragment Shader (Output with alpha for blending)
//------------------------------------------------------------------------------

[shader("fragment")]
float4 fragmentMain(VertexOutput input) : SV_Target
{
    // BaseColor sampling
    float4 color = draw.baseColorFactor;
    if(draw.baseColorTextureIndex >= 0)
    {
        float4 texColor = inTexture[draw.baseColorTextureIndex].Sample(input.uv);
        color = float4(texColor.rgb * draw.baseColorFactor.rgb, texColor.a * draw.baseColorFactor.a);
    }

    // Output with alpha (blending handled by pipeline state)
    return color;
}

[shader("fragment")]
float4 fragmentMdiMain(VertexOutputMdi input) : SV_Target
{
    const DrawUniforms mdiDraw = drawData[input.drawIndex];

    float4 color = mdiDraw.baseColorFactor;
    if(mdiDraw.baseColorTextureIndex >= 0)
    {
        float4 texColor = inTexture[mdiDraw.baseColorTextureIndex].Sample(input.uv);
        color = float4(texColor.rgb * mdiDraw.baseColorFactor.rgb, texColor.a * mdiDraw.baseColorFactor.a);
    }

    return color;
}
