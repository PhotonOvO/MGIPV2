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

bool accumulateProjectedPoint(float3 worldPos, inout float2 minNdc, inout float2 maxNdc, inout bool hasProjectedPoint)
{
    float4 clip = mul(gpuCullingUniforms.viewProjectionMatrix, float4(worldPos, 1.0));
    if(abs(clip.w) <= 1e-5)
    {
        return false;
    }

    float3 ndc = clip.xyz / clip.w;
    minNdc = hasProjectedPoint ? min(minNdc, ndc.xy) : ndc.xy;
    maxNdc = hasProjectedPoint ? max(maxNdc, ndc.xy) : ndc.xy;
    hasProjectedPoint = true;
    return true;
}

uint selectMip(float2 pixelMin, float2 pixelMax, uint mipCount)
{
    float2 extentPixels = max(pixelMax - pixelMin, float2(1.0, 1.0));
    float footprint = max(extentPixels.x, extentPixels.y);
    uint mip = footprint > 1.0 ? uint(floor(log2(footprint))) : 0u;
    return min(mip, max(mipCount, 1u) - 1u);
}

float sampleHiZMaxDepth(uint mipLevel, int2 minCoord, int2 maxCoord)
{
    float maxDepth = 0.0;
    maxDepth = max(maxDepth, depthPyramidTexture[mipLevel].Load(int3(minCoord, 0)));
    maxDepth = max(maxDepth, depthPyramidTexture[mipLevel].Load(int3(maxCoord.x, minCoord.y, 0)));
    maxDepth = max(maxDepth, depthPyramidTexture[mipLevel].Load(int3(minCoord.x, maxCoord.y, 0)));
    maxDepth = max(maxDepth, depthPyramidTexture[mipLevel].Load(int3(maxCoord, 0)));
    return maxDepth;
}

bool isOccluded(float3 center, float radius)
{
    float2 minNdc = float2(0.0, 0.0);
    float2 maxNdc = float2(0.0, 0.0);
    bool hasProjection = false;

    const float3 cameraRight = normalize(gpuCullingUniforms.cameraRight.xyz);
    const float3 cameraUp = normalize(gpuCullingUniforms.cameraUp.xyz);

    accumulateProjectedPoint(center, minNdc, maxNdc, hasProjection);
    accumulateProjectedPoint(center + cameraRight * radius, minNdc, maxNdc, hasProjection);
    accumulateProjectedPoint(center - cameraRight * radius, minNdc, maxNdc, hasProjection);
    accumulateProjectedPoint(center + cameraUp * radius, minNdc, maxNdc, hasProjection);
    accumulateProjectedPoint(center - cameraUp * radius, minNdc, maxNdc, hasProjection);

    if(!hasProjection || maxNdc.x < -1.0 || minNdc.x > 1.0 || maxNdc.y < -1.0 || minNdc.y > 1.0)
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
    const float mipScale = exp2(float(mipLevel));

    const int2 mipExtent = max(int2(1, 1), int2(pyramidSize) >> int(mipLevel));
    const int2 minCoord = clamp(int2(floor(minPixel / mipScale)), int2(0, 0), mipExtent - int2(1, 1));
    const int2 maxCoord = clamp(int2(ceil(maxPixel / mipScale)) - int2(1, 1), int2(0, 0), mipExtent - int2(1, 1));
    const float hizDepth = sampleHiZMaxDepth(mipLevel, minCoord, maxCoord);

    const float3 centerView = mul(gpuCullingUniforms.viewMatrix, float4(center, 1.0)).xyz;
    const float4 farthestClip = mul(gpuCullingUniforms.projectionMatrix, float4(centerView.xy, centerView.z - radius, 1.0));
    if(abs(farthestClip.w) <= 1e-5)
    {
        return false;
    }

    const float farthestDepth = saturate(farthestClip.z / farthestClip.w);
    const float depthEpsilon = gpuCullingUniforms.cullingInfo.w;
    return hizDepth > farthestDepth + depthEpsilon;
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
