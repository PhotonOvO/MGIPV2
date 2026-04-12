// Debug Line Shader: Draw wireframe lines for visualization
// Minimal shader with position + color input

#include "shader_io.h"

//------------------------------------------------------------------------------
// Input Structures
//------------------------------------------------------------------------------

struct VertexInput
{
    [[vk::location(LVGltfPosition)]] float3 position : POSITION;
    [[vk::location(LVColor)]] float4 color : COLOR0;
};

struct VertexOutput
{
    float4 position : SV_Position;
    float4 color : TEXCOORD0;
};

//------------------------------------------------------------------------------
// Resource Bindings
//------------------------------------------------------------------------------

[[vk::binding(LBindCamera, LSetScene)]]
ConstantBuffer<CameraUniforms> camera;

//------------------------------------------------------------------------------
// Vertex Shader
//------------------------------------------------------------------------------

[shader("vertex")]
VertexOutput vertexMain(VertexInput input)
{
    VertexOutput output;

    // Transform to clip space directly (input is world position)
    output.position = mul(camera.viewProjection, float4(input.position, 1.0));
    output.color = input.color;

    return output;
}

//------------------------------------------------------------------------------
// Fragment Shader
//------------------------------------------------------------------------------

[shader("fragment")]
float4 fragmentMain(VertexOutput input) : SV_Target0
{
    return input.color;
}