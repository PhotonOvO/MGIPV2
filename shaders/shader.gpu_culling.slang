#include "shader_io.h"

[[vk::binding(0, 0)]]
StructuredBuffer<GPUCullObject> gpuCullObjects;

[[vk::binding(1, 0)]]
RWStructuredBuffer<GPUCullIndirectCommand> indirectCommands;

[[vk::binding(2, 0)]]
RWStructuredBuffer<GPUCullStats> cullStats;

[[vk::binding(3, 0)]]
RWStructuredBuffer<uint> cullResults;

[[vk::binding(4, 0)]]
Texture2D<float> depthPyramidTexture[LDepthPyramidMaxMips];

[[vk::binding(5, 0)]]
ConstantBuffer<GPUCullingUniforms> gpuCullingUniforms;

bool isInsideFrustum(float3 center, float radius)
{
    for(uint planeIndex = 0; planeIndex < uint(LGPUCullingFrustumPlaneCount); ++planeIndex)
    {
        float4 plane = gpuCullingUniforms.frustumPlanes[planeIndex];
        float distanceToPlane = dot(plane.xyz, center) + plane.w;
        if(distanceToPlane < -radius)
        {
            return false;
        }
    }
    return true;
}

uint selectMip(float2 pixelMin, float2 pixelMax, uint mipCount)
{
    float2 extentPixels = max(pixelMax - pixelMin, float2(1.0, 1.0));
    float footprint = max(extentPixels.x, extentPixels.y);
    uint mip = footprint > 8.0 ? uint(floor(log2(footprint))) - 3u : 0u;
    const uint maxAvailableMip = max(mipCount, 1u) - 1u;
    const uint maxAllowedMip = min(maxAvailableMip, 1u);
    return min(mip, maxAllowedMip);
}

float sampleHiZCenterClusterDepth(uint mipLevel, int2 centerCoord, int2 mipExtent)
{
    const int2 leftCoord = clamp(centerCoord + int2(-1, 0), int2(0, 0), mipExtent - int2(1, 1));
    const int2 rightCoord = clamp(centerCoord + int2(1, 0), int2(0, 0), mipExtent - int2(1, 1));
    const int2 upCoord = clamp(centerCoord + int2(0, -1), int2(0, 0), mipExtent - int2(1, 1));
    const int2 downCoord = clamp(centerCoord + int2(0, 1), int2(0, 0), mipExtent - int2(1, 1));

    float representativeDepth = depthPyramidTexture[mipLevel].Load(int3(centerCoord, 0));
    representativeDepth = max(representativeDepth, depthPyramidTexture[mipLevel].Load(int3(leftCoord, 0)));
    representativeDepth = max(representativeDepth, depthPyramidTexture[mipLevel].Load(int3(rightCoord, 0)));
    representativeDepth = max(representativeDepth, depthPyramidTexture[mipLevel].Load(int3(upCoord, 0)));
    representativeDepth = max(representativeDepth, depthPyramidTexture[mipLevel].Load(int3(downCoord, 0)));
    return representativeDepth;
}

bool isOccluded(float3 center, float radius)
{
    const float3 centerView = mul(gpuCullingUniforms.viewMatrix, float4(center, 1.0)).xyz;
    const float nearestPositiveZ = -centerView.z - radius;
    if(nearestPositiveZ <= 1e-4)
    {
        return false;
    }

    const float4 clipCenter = mul(gpuCullingUniforms.projectionMatrix, float4(centerView, 1.0));
    if(abs(clipCenter.w) <= 1e-5)
    {
        return false;
    }

    const float2 centerNdc = clipCenter.xy / clipCenter.w;
    const float2 projectionScale = float2(abs(gpuCullingUniforms.projectionMatrix[0][0]),
                                          abs(gpuCullingUniforms.projectionMatrix[1][1]));
    const float2 radiusNdc = projectionScale * (radius / nearestPositiveZ);

    float2 minNdc = centerNdc - radiusNdc;
    float2 maxNdc = centerNdc + radiusNdc;
    if(maxNdc.x < -1.0 || minNdc.x > 1.0 || maxNdc.y < -1.0 || minNdc.y > 1.0)
    {
        return false;
    }

    minNdc = clamp(minNdc, float2(-1.0, -1.0), float2(1.0, 1.0));
    maxNdc = clamp(maxNdc, float2(-1.0, -1.0), float2(1.0, 1.0));

    const float2 screenSize = gpuCullingUniforms.screenSizeAndPyramidSize.xy;
    const float2 pyramidSize = gpuCullingUniforms.screenSizeAndPyramidSize.zw;
    const uint mipCount = uint(gpuCullingUniforms.cullingInfo.y);

    const float2 minPixel = (minNdc * 0.5 + 0.5) * screenSize;
    const float2 maxPixel = (maxNdc * 0.5 + 0.5) * screenSize;
    const uint mipLevel = selectMip(minPixel, maxPixel, mipCount);
    const float sourceFootprint = exp2(float(mipLevel + 1u));

    const int2 mipExtent = max(int2(1, 1), int2(pyramidSize) >> int(mipLevel));
    const float2 centerPixel = 0.5 * (minPixel + maxPixel);
    const int2 centerCoord = clamp(int2(centerPixel / sourceFootprint), int2(0, 0), mipExtent - int2(1, 1));
    const float hizDepth = sampleHiZCenterClusterDepth(mipLevel, centerCoord, mipExtent);

    const float nearestViewZ = centerView.z + radius;
    if(nearestViewZ >= -1e-4)
    {
        return false;
    }

    const float4 nearestClip = mul(gpuCullingUniforms.projectionMatrix, float4(centerView.xy, nearestViewZ, 1.0));
    if(abs(nearestClip.w) <= 1e-5)
    {
        return false;
    }

    const float nearestDepth = saturate(nearestClip.z / nearestClip.w);
    const float depthEpsilon = gpuCullingUniforms.cullingInfo.w;
    return nearestDepth < hizDepth - depthEpsilon;
}

[numthreads(LGPUCullingThreadCount, 1, 1)]
[shader("compute")]
void gpuCullingMain(uint3 dispatchThreadId : SV_DispatchThreadID)
{
    const uint objectIndex = dispatchThreadId.x;
    const uint objectCount = uint(gpuCullingUniforms.cullingInfo.x);
    if(objectIndex >= objectCount)
    {
        return;
    }

    if(objectIndex == 0)
    {
        cullStats[0].totalCount = objectCount;
    }

    const GPUCullObject objectData = gpuCullObjects[objectIndex];
    const bool isTransparent = (objectData.flags & LGPUCullFlagTransparent) != 0u;
    if(isTransparent)
    {
        InterlockedAdd(cullStats[0].transparentCount, 1u);
    }
    else
    {
        InterlockedAdd(cullStats[0].opaqueCount, 1u);
    }

    GPUCullIndirectCommand command;
    command.indexCount = objectData.indexCount;
    command.instanceCount = 0u;
    command.firstIndex = objectData.firstIndex;
    command.vertexOffset = objectData.vertexOffset;
    command.firstInstance = 0u;

    const float3 center = objectData.sphereCenterRadius.xyz;
    const float radius = max(objectData.sphereCenterRadius.w, 0.0);

    const bool enableFrustumCulling = (objectData.flags & LGPUCullFlagFrustumCulling) != 0u;
    const bool enableOcclusionCulling = (objectData.flags & LGPUCullFlagOcclusionCulling) != 0u;

    if(enableFrustumCulling && !isInsideFrustum(center, radius))
    {
        InterlockedAdd(cullStats[0].frustumCulledCount, 1u);
        cullResults[objectIndex] = LGPUCullResultFrustumCulled;
        indirectCommands[objectIndex] = command;
        return;
    }

    const bool useOcclusion = gpuCullingUniforms.cullingInfo.z > 0.5;
    if(enableOcclusionCulling && useOcclusion && isOccluded(center, radius))
    {
        InterlockedAdd(cullStats[0].occlusionCulledCount, 1u);
        cullResults[objectIndex] = LGPUCullResultOcclusionCulled;
        indirectCommands[objectIndex] = command;
        return;
    }

    command.instanceCount = 1u;
    InterlockedAdd(cullStats[0].visibleCount, 1u);
    if(isTransparent)
    {
        InterlockedAdd(cullStats[0].transparentVisibleCount, 1u);
    }
    else
    {
        InterlockedAdd(cullStats[0].opaqueVisibleCount, 1u);
    }
    cullResults[objectIndex] = LGPUCullResultVisible;
    indirectCommands[objectIndex] = command;
}
