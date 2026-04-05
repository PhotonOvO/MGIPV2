// LightPass shader: Fullscreen triangle sampling GBuffer
// Current: Pass-through BaseColor
// Future: Full PBR lighting calculation

#include "shader_io.h"

//------------------------------------------------------------------------------
// Resource Bindings - using same layout as GBuffer for compatibility
//------------------------------------------------------------------------------

// Set 0: Bindless textures (we'll use indices 0-3 for GBuffer)
[[vk::binding(LBindTextures, LSetTextures)]]
Sampler2D inTexture[];

// GBuffer texture indices (passed via push constants or hardcoded)
STATIC_CONST int kGBufferBaseColorIndex = 0;
STATIC_CONST int kGBufferNormalIndex    = 1;
STATIC_CONST int kGBufferMaterialIndex  = 2;
STATIC_CONST int kGBufferDepthIndex     = 3;

//------------------------------------------------------------------------------
// Vertex Shader
//------------------------------------------------------------------------------

struct VSOutput {
    float4 position : SV_Position;
    float2 uv       : TEXCOORD0;
};

// Fullscreen triangle vertex shader
[shader("vertex")]
VSOutput vertexMain(uint vertexIndex : SV_VertexID) {
    VSOutput output;

    float2 positions[3] = {
        float2(-1.0, -1.0),
        float2( 3.0, -1.0),
        float2(-1.0,  3.0)
    };

    output.position = float4(positions[vertexIndex], 0.0, 1.0);
    output.uv = positions[vertexIndex] * 0.5 + 0.5;

    // Flip Y for Vulkan
    output.uv.y = 1.0 - output.uv.y;

    return output;
}

//------------------------------------------------------------------------------
// Fragment Shader
//------------------------------------------------------------------------------

// Fragment shader: Pass-through BaseColor (current implementation)
[shader("fragment")]
float4 fragmentMain(VSOutput input) : SV_Target {
    // PASS-THROUGH: Output BaseColor directly
    float4 baseColor = inTexture[kGBufferBaseColorIndex].Sample(input.uv);
    return baseColor;

    // FUTURE: PBR lighting calculation
    // Normal is stored as [0,1] mapped from [-1,1]
    // float3 normal = inTexture[kGBufferNormalIndex].Sample(input.uv).rgb * 2.0 - 1.0;
    // float2 mr = inTexture[kGBufferMaterialIndex].Sample(input.uv).rg;
    // float ao = inTexture[kGBufferMaterialIndex].Sample(input.uv).b;
    // float3 result = computePBR(baseColor.rgb, normal, mr.x, mr.y, ao);
    // return float4(result, 1.0);
}