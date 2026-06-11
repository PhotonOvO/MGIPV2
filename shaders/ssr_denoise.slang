#include "shader_io.h"

[[vk::binding(0, 0)]]
Texture2D<float4> ssrInput;
[[vk::binding(1, 0)]]
Texture2D<float> depthTexture;
[[vk::binding(2, 0)]]
RWTexture2D<float4> ssrOutput;

[[vk::push_constant]]
ConstantBuffer<GPUDrivenSSRPushConstants> ssrPush;

// Depth-difference sharpness for the bilateral weight.  Operates on raw reversed-Z
// NDC deltas (not linearised), matching the AO denoiser's depthScale of 64.
STATIC_CONST float kDepthSharpness = 64.0;
// Floor for the confidence weight so zero-confidence neighbours still bleed a small
// amount of colour into SSR holes instead of being rejected outright.
STATIC_CONST float kConfidenceFloor = 0.05;

[shader("compute")]
[numthreads(8, 8, 1)]
void kernelSSRDenoise(uint3 dispatchThreadId : SV_DispatchThreadID)
{
    const uint2 outputSize = uint2(ssrPush.params0.xy);
    if(dispatchThreadId.x >= outputSize.x || dispatchThreadId.y >= outputSize.y)
    {
        return;
    }

    const uint2 coord = dispatchThreadId.xy;
    // SSR runs at half resolution; map to the full-res depth texel the same way the
    // trace kernel does (centre of the 2x2 block, offset by (1,1)).
    const uint2 inputSize = max(outputSize * 2u, uint2(1u, 1u));
    const uint2 fullCoord = min(coord * 2u + uint2(1u, 1u), inputSize - uint2(1u, 1u));
    const float centerDepth = depthTexture.Load(int3(fullCoord, 0));
    const float4 centerTap = ssrInput.Load(int3(coord, 0));

    float4 sum = float4(0.0);
    float weightSum = 0.0;
    [unroll]
    for(int y = -1; y <= 1; ++y)
    {
        [unroll]
        for(int x = -1; x <= 1; ++x)
        {
            const uint2 sampleCoord = uint2(clamp(int2(coord) + int2(x, y), int2(0, 0), int2(outputSize) - int2(1, 1)));
            const uint2 sampleFullCoord = min(sampleCoord * 2u + uint2(1u, 1u), inputSize - uint2(1u, 1u));
            const float sampleDepth = depthTexture.Load(int3(sampleFullCoord, 0));
            const float4 tap = ssrInput.Load(int3(sampleCoord, 0));
            const float depthWeight = exp(-abs(sampleDepth - centerDepth) * kDepthSharpness);
            const float spatialWeight = (x == 0 && y == 0) ? 1.0 : 0.5;
            // Confidence weighting: neighbours holding a confident hit (large alpha)
            // dominate the blend; misses (alpha == 0) contribute only via the floor.
            const float confidenceWeight = kConfidenceFloor + saturate(tap.a);
            const float weight = depthWeight * spatialWeight * confidenceWeight;
            sum += tap * weight;
            weightSum += weight;
        }
    }

    ssrOutput[coord] = weightSum > 1.0e-4 ? sum / weightSum : centerTap;
}
