// Shadow Pass Shader: Render depth-only for shadow mapping
// Uses same vertex format as GBuffer pass

#include "shader_io.h"

//------------------------------------------------------------------------------
// Resource Bindings
//------------------------------------------------------------------------------

// Set 1: Camera uniform (for cascade selection - optional in shadow pass)
[[vk::binding(LBindCamera, LSetScene)]]
ConstantBuffer<CameraUniforms> camera;

// Set 2: Per-draw model matrix
[[vk::binding(LBindDrawModel, LSetDraw)]]
ConstantBuffer<DrawUniforms> draw;

//------------------------------------------------------------------------------
// Push Constants Structure
//------------------------------------------------------------------------------

struct ShadowPushConstants
{
    mat4 lightViewProjection;
    float depthBias;
    float normalBias;
    uint cascadeIndex;
    float _padding;
};

// Push constant binding
[[vk::push_constant]]
ShadowPushConstants shadowPush;

//------------------------------------------------------------------------------
// Vertex Shader
//------------------------------------------------------------------------------

struct VertexInput
{
    [[vk::location(LVGltfPosition)]] float3 position : POSITION;
    [[vk::location(LVGltfNormal)]]   float3 normal   : NORMAL;
};

struct VertexOutput
{
    float4 position : SV_Position;
    float3 worldPos : TEXCOORD0;
    float3 worldNormal : TEXCOORD1;
};

[shader("vertex")]
VertexOutput vertexMain(VertexInput input)
{
    VertexOutput output;

    // Transform to world space
    float4 worldPos = mul(draw.modelMatrix, float4(input.position, 1.0));

    // Transform to light's clip space
    output.position = mul(shadowPush.lightViewProjection, worldPos);

    // Pass world position and normal for potential depth bias calculations
    output.worldPos = worldPos.xyz;
    output.worldNormal = normalize(mul(draw.modelMatrix, float4(input.normal, 0.0)).xyz);

    return output;
}

//------------------------------------------------------------------------------
// Fragment Shader (Depth-only, no color output)
//------------------------------------------------------------------------------

[shader("fragment")]
void fragmentMain(VertexOutput input)
{
    // No color output - only depth is written to the depth attachment
    // Depth bias is applied via pipeline dynamic state (depthBiasConstantFactor,
    // depthBiasClamp, depthBiasSlopeFactor) for more precise control

    // Alternative: Manual depth bias could be applied here if needed:
    // gl_FragDepth = input.position.z + shadowPush.depthBias;
    // However, pipeline depth bias is preferred for hardware optimization
}