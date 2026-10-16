// Scaled texture blit.
//
// D3D12 has no scaled texel copy: CopyTextureRegion requires the source and destination
// regions to be the same size, while Vulkan (vkCmdBlitImage) and Metal (blit encoder) both
// scale. The present pass aspect-fits the scene into the swapchain, so the D3D12 backend
// lowers a mismatched blit to one full-screen triangle through this shader.
//
// The source region arrives as a normalized UV rectangle. The destination region is set by
// the viewport at draw time, so the triangle covers exactly the letterboxed area while the
// rest of the target keeps whatever the earlier clear wrote. Registered by the shaders/
// glob in CMakeLists, and the D3D12 backend compiles it like any other pipeline shader.

struct BlitParams
{
    // xy = source UV origin, zw = source UV size.
    float4 sourceUvRect;
};

[[vk::push_constant]]
ConstantBuffer<BlitParams> blitParams;

[[vk::binding(0, 0)]]
Sampler2D sourceTexture;

struct BlitVertexOutput
{
    float4 position : SV_Position;
    float2 uv : TEXCOORD0;
};

[shader("vertex")]
BlitVertexOutput blitVertexMain(uint vertexId : SV_VertexID)
{
    BlitVertexOutput output;
    // Single oversized triangle covering the viewport: (0,0) (2,0) (0,2) in NDC terms.
    // Both targets place NDC y downwards and put the viewport origin at the top-left, so
    // the UV mapping needs no per-backend flip.
    const float2 corner = float2(float((vertexId << 1u) & 2u), float(vertexId & 2u));
    output.uv = blitParams.sourceUvRect.xy + corner * blitParams.sourceUvRect.zw;
    output.position = float4(corner * 2.0 - 1.0, 0.0, 1.0);
    return output;
}

[shader("fragment")]
float4 blitFragmentMain(BlitVertexOutput input) : SV_Target
{
    // Base level only: both sides are single-mip render targets, and explicit level 0
    // avoids implicit derivatives (and their quad requirement) in this trivial pass.
    return sourceTexture.SampleLevel(input.uv, 0.0);
}
