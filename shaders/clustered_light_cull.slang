#include "shader_io.h"

[[vk::binding(0, 0)]]
StructuredBuffer<LightData> pointLightBuffer;
[[vk::binding(1, 0)]]
StructuredBuffer<LightData> spotLightBuffer;
[[vk::binding(2, 0)]]
StructuredBuffer<uint16_t4> pointLightBounds;
[[vk::binding(3, 0)]]
StructuredBuffer<uint16_t4> spotLightBounds;
[[vk::binding(4, 0)]]
ConstantBuffer<LightCoarseCullingUniforms> lightCoarse;
[[vk::binding(5, 0)]]
ConstantBuffer<ClusteredLightUniforms> clusteredLight;
[[vk::binding(6, 0)]]
RWStructuredBuffer<uint> clusterLightCounts;
[[vk::binding(7, 0)]]
RWStructuredBuffer<uint> clusterLightIndices;
[[vk::binding(8, 0)]]
RWStructuredBuffer<uint> clusterLightStats;

STATIC_CONST uint kClusterCullingThreadCount = 64u;

float linearSliceDepth(uint z, uint clusterCountZ)
{
    const float nearPlane = max(clusteredLight.clipInfo.x, 0.001);
    const float farPlane = max(clusteredLight.clipInfo.y, nearPlane + 0.001);
    const float sliceT = float(z) / float(max(clusterCountZ, 1u));
    return lerp(nearPlane, farPlane, sliceT);
}

float3 reconstructViewRay(float2 uv)
{
    float2 ndc = demoFramebufferUvToNdc(uv);
    const float4 clip = float4(ndc, 1.0, 1.0);
    const float4 view = mul(clusteredLight.invProjectionMatrix, clip);
    const float3 viewPos = view.xyz / max(abs(view.w), 1.0e-5);
    return viewPos / max(abs(viewPos.z), 1.0e-5);
}

float3 reconstructViewPosition(float2 uv, float viewDepth)
{
    return reconstructViewRay(uv) * viewDepth;
}

void computeClusterViewAabb(uint3 clusterCoord, out float3 minView, out float3 maxView)
{
    const uint clusterCountX = max(uint(clusteredLight.screenSizeAndClusterInfo.z), 1u);
    const uint clusterCountY = max(uint(clusteredLight.screenSizeAndClusterInfo.w), 1u);
    const uint clusterCountZ = max(uint(clusteredLight.clusterZAndLightInfo.x), 1u);

    const float2 minUv = float2(float(clusterCoord.x), float(clusterCoord.y)) / float2(clusterCountX, clusterCountY);
    const float2 maxUv = float2(float(clusterCoord.x + 1u), float(clusterCoord.y + 1u)) / float2(clusterCountX, clusterCountY);
    const float nearDepth = linearSliceDepth(clusterCoord.z, clusterCountZ);
    const float farDepth = linearSliceDepth(clusterCoord.z + 1u, clusterCountZ);

    float3 corners[8] =
    {
        reconstructViewPosition(float2(minUv.x, minUv.y), nearDepth),
        reconstructViewPosition(float2(maxUv.x, minUv.y), nearDepth),
        reconstructViewPosition(float2(minUv.x, maxUv.y), nearDepth),
        reconstructViewPosition(float2(maxUv.x, maxUv.y), nearDepth),
        reconstructViewPosition(float2(minUv.x, minUv.y), farDepth),
        reconstructViewPosition(float2(maxUv.x, minUv.y), farDepth),
        reconstructViewPosition(float2(minUv.x, maxUv.y), farDepth),
        reconstructViewPosition(float2(maxUv.x, maxUv.y), farDepth)
    };

    minView = corners[0];
    maxView = corners[0];
    [unroll]
    for(uint i = 1u; i < 8u; ++i)
    {
        minView = min(minView, corners[i]);
        maxView = max(maxView, corners[i]);
    }
}

bool sphereOverlapsAabb(float3 center, float radius, float3 minView, float3 maxView)
{
    const float3 closest = clamp(center, minView, maxView);
    const float3 delta = center - closest;
    return dot(delta, delta) <= radius * radius;
}

bool localLightOverlapsCluster(LightData light, float3 minView, float3 maxView)
{
    const float4 centerView4 = mul(clusteredLight.viewMatrix, float4(light.positionOrDirection, 1.0));
    const float radius = max(light.range, 0.0);
    return sphereOverlapsAabb(centerView4.xyz, radius, minView, maxView);
}

bool clusterOverlapsCoarseBounds(uint3 clusterCoord, uint16_t4 bounds)
{
    const uint lightTileCountX = uint(bounds.y);
    const uint lightTileCountY = uint(bounds.w);
    // Empty coarse bounds are ambiguous for lights crossing the near plane.
    // Preserve correctness by falling through to the exact sphere/AABB test.
    if(lightTileCountX == 0u || lightTileCountY == 0u)
    {
        return true;
    }

    const uint tileCountX = max(uint(lightCoarse.screenTileInfo.z), 1u);
    const uint tileCountY = max(uint(lightCoarse.screenTileInfo.w), 1u);
    const uint clusterCountX = max(uint(clusteredLight.screenSizeAndClusterInfo.z), 1u);
    const uint clusterCountY = max(uint(clusteredLight.screenSizeAndClusterInfo.w), 1u);

    const uint clusterTileStartX = clusterCoord.x * tileCountX / clusterCountX;
    const uint clusterTileEndX =
        ((clusterCoord.x + 1u) * tileCountX + clusterCountX - 1u) / clusterCountX;
    const uint clusterTileStartY = clusterCoord.y * tileCountY / clusterCountY;
    const uint clusterTileEndY =
        ((clusterCoord.y + 1u) * tileCountY + clusterCountY - 1u) / clusterCountY;

    // The coarse projector samples a sphere at five cardinal points. Expand by
    // one full cluster so it remains a conservative prefilter near projection
    // extrema; the precise view-space test below decides final membership.
    const uint paddingX = (tileCountX + clusterCountX - 1u) / clusterCountX;
    const uint paddingY = (tileCountY + clusterCountY - 1u) / clusterCountY;
    const uint lightTileStartX = uint(bounds.x) > paddingX
        ? uint(bounds.x) - paddingX : 0u;
    const uint lightTileEndX = min(
        uint(bounds.x) + lightTileCountX + paddingX, tileCountX);
    const uint lightTileStartY = uint(bounds.z) > paddingY
        ? uint(bounds.z) - paddingY : 0u;
    const uint lightTileEndY = min(
        uint(bounds.z) + lightTileCountY + paddingY, tileCountY);

    return clusterTileStartX < lightTileEndX && clusterTileEndX > lightTileStartX
        && clusterTileStartY < lightTileEndY && clusterTileEndY > lightTileStartY;
}

uint appendClusterLight(uint clusterId, inout uint visibleCount, inout bool overflow, uint encodedLightIndex)
{
    if(visibleCount >= LMaxLightsPerCluster)
    {
        overflow = true;
        return 0u;
    }

    clusterLightIndices[clusterId * LMaxLightsPerCluster + visibleCount] = encodedLightIndex;
    ++visibleCount;
    return 1u;
}

[numthreads(kClusterCullingThreadCount, 1, 1)]
void kernelClusteredLightCulling(uint3 dispatchThreadId : SV_DispatchThreadID)
{
    const uint clusterCountX = max(uint(clusteredLight.screenSizeAndClusterInfo.z), 1u);
    const uint clusterCountY = max(uint(clusteredLight.screenSizeAndClusterInfo.w), 1u);
    const uint clusterCountZ = max(uint(clusteredLight.clusterZAndLightInfo.x), 1u);
    const uint clusterCount = clusterCountX * clusterCountY * clusterCountZ;
    const uint clusterId = dispatchThreadId.x;
    if(clusterId >= clusterCount)
    {
        return;
    }

    const uint pointCount = uint(clusteredLight.clusterZAndLightInfo.z);
    const uint spotCount = uint(clusteredLight.clusterZAndLightInfo.w);
    const bool collectStats = clusteredLight.debugInfo.w > 0.5;
    if(collectStats && clusterId == 0u)
    {
        clusterLightStats[3] = pointCount;
        clusterLightStats[4] = spotCount;
    }

    uint visibleCount = 0u;
    uint appendedCount = 0u;
    bool overflow = false;
    const uint z = clusterId / (clusterCountX * clusterCountY);
    const uint xy = clusterId - z * clusterCountX * clusterCountY;
    const uint y = xy / clusterCountX;
    const uint x = xy - y * clusterCountX;
    const uint3 clusterCoord = uint3(x, y, z);
    float3 minView;
    float3 maxView;
    computeClusterViewAabb(clusterCoord, minView, maxView);

    for(uint lightIndex = 0u; lightIndex < pointCount; ++lightIndex)
    {
        const LightData light = pointLightBuffer[lightIndex];
        if(!clusterOverlapsCoarseBounds(clusterCoord, pointLightBounds[lightIndex])
            || !localLightOverlapsCluster(light, minView, maxView))
        {
            continue;
        }

        appendedCount += appendClusterLight(
            clusterId, visibleCount, overflow,
            lightIndex & LClusterLightIndexMask);
        if(overflow)
        {
            break;
        }
    }

    if(!overflow)
    {
        for(uint lightIndex = 0u; lightIndex < spotCount; ++lightIndex)
        {
            const LightData light = spotLightBuffer[lightIndex];
            if(!clusterOverlapsCoarseBounds(clusterCoord, spotLightBounds[lightIndex])
                || !localLightOverlapsCluster(light, minView, maxView))
            {
                continue;
            }

            appendedCount += appendClusterLight(
                clusterId, visibleCount, overflow,
                LClusterLightTypeSpotBit | (lightIndex & LClusterLightIndexMask));
            if(overflow)
            {
                break;
            }
        }
    }

    uint storedCount = visibleCount;
    if(overflow)
    {
        storedCount |= LClusterOverflowBit;
    }
    clusterLightCounts[clusterId] = storedCount;

    if(collectStats)
    {
        uint ignoredMax;
        InterlockedMax(clusterLightStats[0], visibleCount, ignoredMax);
        if(overflow)
        {
            uint ignoredOverflow;
            InterlockedAdd(clusterLightStats[1], 1u, ignoredOverflow);
        }
        uint ignoredAdd;
        InterlockedAdd(clusterLightStats[2], appendedCount, ignoredAdd);
    }
}
