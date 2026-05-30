#include "shader_io.h"

[[vk::binding(0, 0)]]
Texture2D<float> aoInput;
[[vk::binding(1, 0)]]
Texture2D<float> depthTexture;
[[vk::binding(2, 0)]]
RWTexture2D<float> aoOutput;

[[vk::push_constant]]
ConstantBuffer<GPUDrivenAOPushConstants> aoPush;

[shader("compute")]
[numthreads(8, 8, 1)]
void kernelAODenoise(uint3 dispatchThreadId : SV_DispatchThreadID)
{
    const uint2 outputSize = uint2(aoPush.params0.xy);
    if(dispatchThreadId.x >= outputSize.x || dispatchThreadId.y >= outputSize.y)
    {
        return;
    }

    const uint2 coord = dispatchThreadId.xy;
    const uint2 inputSize = max(outputSize * 2u, uint2(1u, 1u));
    const uint2 fullCoord = min(coord * 2u + uint2(1u, 1u), inputSize - uint2(1u, 1u));
    const float centerDepth = depthTexture.Load(int3(fullCoord, 0));

    float sum = 0.0;
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
            const float depthWeight = exp(-abs(sampleDepth - centerDepth) * aoPush.params1.z);
            const float spatialWeight = (x == 0 && y == 0) ? 1.0 : 0.5;
            const float weight = depthWeight * spatialWeight;
            sum += aoInput.Load(int3(sampleCoord, 0)) * weight;
            weightSum += weight;
        }
    }

    aoOutput[coord] = sum / max(weightSum, 0.0001);
}
