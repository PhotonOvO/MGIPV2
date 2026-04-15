// Single-dispatch min-depth pyramid generation.

#include "shader_io.h"

[[vk::binding(0, 0)]]
Texture2D<float> depthTexture;

[[vk::binding(1, 0)]]
RWTexture2D<float> depthPyramidTexture[LDepthPyramidMaxMips];

[[vk::binding(2, 0)]]
ConstantBuffer<DepthPyramidUniforms> depthPyramidUniforms;

STATIC_CONST uint kDepthPyramidThreadSize = 8;

uint ceilDiv(uint value, uint divisor)
{
    return (value + divisor - 1u) / divisor;
}

float loadSourceDepth(uint2 coord)
{
    const uint2 sourceSize = uint2(depthPyramidUniforms.sourceWidth, depthPyramidUniforms.sourceHeight);
    const uint2 clampedCoord = min(coord, sourceSize - uint2(1u, 1u));
    return depthTexture.Load(int3(clampedCoord, 0));
}

float reduceSourceRegion(uint2 mipCoord, uint mipLevel)
{
    const uint sourceFootprint = 1u << (mipLevel + 1u);
    const uint2 sourceStart = mipCoord * sourceFootprint;
    const uint2 sourceSize = uint2(depthPyramidUniforms.sourceWidth, depthPyramidUniforms.sourceHeight);
    const uint2 sourceEnd = min(sourceStart + uint2(sourceFootprint, sourceFootprint), sourceSize);

    float minDepth = 1.0;
    for(uint y = sourceStart.y; y < sourceEnd.y; ++y)
    {
        for(uint x = sourceStart.x; x < sourceEnd.x; ++x)
        {
            minDepth = min(minDepth, loadSourceDepth(uint2(x, y)));
        }
    }
    return minDepth;
}

[numthreads(kDepthPyramidThreadSize, kDepthPyramidThreadSize, 1)]
[shader("compute")]
void depthPyramid(uint3 dispatchThreadId : SV_DispatchThreadID)
{
    const uint2 pixel = dispatchThreadId.xy;
    const uint mipCount = min(depthPyramidUniforms.mipCount, uint(LDepthPyramidMaxMips));

    for(uint mipLevel = 0u; mipLevel < mipCount; ++mipLevel)
    {
        const uint2 mipSize =
            max(uint2(1u, 1u), uint2(depthPyramidUniforms.pyramidWidth, depthPyramidUniforms.pyramidHeight) >> mipLevel);
        if(pixel.x >= mipSize.x || pixel.y >= mipSize.y)
        {
            continue;
        }

        depthPyramidTexture[mipLevel][pixel] = reduceSourceRegion(pixel, mipLevel);
    }
}
