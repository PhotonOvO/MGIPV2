// LightPass shader: Fullscreen triangle sampling GBuffer Color
// No vertex input - generates fullscreen triangle in vertex shader

#ifdef USE_SLANG
#define VK_PUSH_CONSTANT [[vk::push_constant]]
#define VK_BINDING(set, binding) [[vk::binding(binding, set)]]
#else
#define VK_PUSH_CONSTANT
#define VK_BINDING(set, binding)
#endif

// GBuffer Color texture (input)
VK_BINDING(0, 0) Texture2D gbufferColor;
VK_BINDING(1, 0) SamplerState sampler0;

struct VSOutput {
    float4 position : SV_Position;
    float2 uv : TEXCOORD0;
};

// Fullscreen triangle vertex shader
// Uses vertex ID to generate a large triangle covering the screen
[shader("vertex")]
VSOutput vertexMain(uint vertexIndex : SV_VertexID) {
    VSOutput output;

    // Large triangle in clip space
    // Covers screen when rasterized
    float2 positions[3] = {
        float2(-1.0, -1.0),  // Bottom-left
        float2( 3.0, -1.0),  // Bottom-right (off-screen)
        float2(-1.0,  3.0)   // Top-left (off-screen)
    };

    output.position = float4(positions[vertexIndex], 0.0, 1.0);

    // UV = position * 0.5 + 0.5
    // (-1,-1) -> (0,0), (1,1) -> (1,1)
    output.uv = positions[vertexIndex] * 0.5 + 0.5;

    // Flip Y for Vulkan
    output.uv.y = 1.0 - output.uv.y;

    return output;
}

// Fragment shader: Sample GBuffer Color and output
[shader("fragment")]
float4 fragmentMain(VSOutput input) : SV_Target {
    return gbufferColor.Sample(sampler0, input.uv);
}