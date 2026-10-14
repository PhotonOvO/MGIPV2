// MGIF editor occurrence-outline composite. Tokens are snapshot-local and are
// compared only against the current revision-bound highlight token buffer.

struct EditorHighlightVertexOutput
{
    float4 position : SV_Position;
};

struct EditorHighlightPushConstants
{
    // Three explicit 16-byte lanes keep SPIR-V scalar layout and DXIL cbuffer
    // layout identical.
    uint4 viewport; // origin.xy, extent.zw
    uint4 counts;   // tokenCount.x, outlineRadius.y, reserved.zw
    float4 color;
};

[[vk::binding(0, 0)]]
Texture2D<uint> occurrenceTokens : register(t0);

[[vk::binding(1, 0)]]
ByteAddressBuffer highlightedTokenMask : register(t1);

[[vk::binding(2, 0)]]
Texture2D<float> editorPickingDepth : register(t2);

[[vk::binding(3, 0)]]
RWByteAddressBuffer editorPickingSampleOutput : register(u3);

[[vk::push_constant]]
ConstantBuffer<EditorHighlightPushConstants> editorHighlight;

[shader("compute")]
[numthreads(1, 1, 1)]
void editorPickSampleMain(uint3 dispatchThreadId : SV_DispatchThreadID)
{
    uint outputBytes = 0;
    editorPickingSampleOutput.GetDimensions(outputBytes);
    if (dispatchThreadId.x != 0u || dispatchThreadId.y != 0u ||
        dispatchThreadId.z != 0u || outputBytes < 8u)
    {
        return;
    }
    editorPickingSampleOutput.Store(0u, 0u);
    editorPickingSampleOutput.Store(4u, asuint(0.0f));

    uint tokenWidth = 0;
    uint tokenHeight = 0;
    uint depthWidth = 0;
    uint depthHeight = 0;
    occurrenceTokens.GetDimensions(tokenWidth, tokenHeight);
    editorPickingDepth.GetDimensions(depthWidth, depthHeight);
    const uint2 samplePixel = editorHighlight.viewport.xy;
    if (samplePixel.x >= tokenWidth || samplePixel.y >= tokenHeight ||
        samplePixel.x >= depthWidth || samplePixel.y >= depthHeight)
    {
        return;
    }
    editorPickingSampleOutput.Store(
        0u, occurrenceTokens.Load(int3(int2(samplePixel), 0)));
    editorPickingSampleOutput.Store(
        4u, asuint(editorPickingDepth.Load(int3(int2(samplePixel), 0))));
}

[shader("vertex")]
EditorHighlightVertexOutput editorHighlightVertexMain(uint vertexId : SV_VertexID)
{
    const float2 positions[3] = {
        float2(-1.0, -1.0), float2(-1.0, 3.0), float2(3.0, -1.0)
    };
    EditorHighlightVertexOutput output;
    output.position = float4(positions[vertexId], 0.0, 1.0);
    return output;
}

bool editorTokenSelected(uint token)
{
    if (token == 0)
    {
        return false;
    }
    uint availableBytes = 0;
    highlightedTokenMask.GetDimensions(availableBytes);
    const uint count = min(editorHighlight.counts.x, availableBytes / 4u);
    return token < count && highlightedTokenMask.Load(token * 4u) != 0u;
}

[shader("fragment")]
float4 editorHighlightFragmentMain(EditorHighlightVertexOutput input)
    : SV_Target0
{
    const int2 pixel = int2(input.position.xy);
    uint textureWidth = 0;
    uint textureHeight = 0;
    occurrenceTokens.GetDimensions(textureWidth, textureHeight);
    const uint2 textureExtent = uint2(textureWidth, textureHeight);
    const uint2 minimumUnsigned = min(editorHighlight.viewport.xy,
                                      textureExtent);
    const uint2 availableExtent = textureExtent - minimumUnsigned;
    const uint2 clampedExtent = min(editorHighlight.viewport.zw,
                                    availableExtent);
    const uint2 maximumUnsigned = minimumUnsigned + clampedExtent;
    const int2 minimum = int2(min(minimumUnsigned,
                                  uint2(0x7fffffffu, 0x7fffffffu)));
    const int2 maximum = int2(min(maximumUnsigned,
                                  uint2(0x7fffffffu, 0x7fffffffu)));
    if (any(pixel < minimum) || any(pixel >= maximum))
    {
        discard;
    }

    const bool centerSelected = editorTokenSelected(occurrenceTokens.Load(int3(pixel, 0)));
    bool neighborSelected = false;
    bool neighborUnselected = false;
    const int radius = int(clamp(editorHighlight.counts.y, 1u, 4u));
    [loop]
    for (int y = -radius; y <= radius; ++y)
    {
        [loop]
        for (int x = -radius; x <= radius; ++x)
        {
            if (x == 0 && y == 0)
            {
                continue;
            }
            const int2 samplePixel = pixel + int2(x, y);
            if (any(samplePixel < minimum) || any(samplePixel >= maximum))
            {
                neighborUnselected = true;
                continue;
            }
            const bool selected = editorTokenSelected(
                occurrenceTokens.Load(int3(samplePixel, 0)));
            neighborSelected = neighborSelected || selected;
            neighborUnselected = neighborUnselected || !selected;
        }
    }
    if ((!centerSelected && !neighborSelected) ||
        (centerSelected && !neighborUnselected))
    {
        discard;
    }
    return editorHighlight.color;
}
