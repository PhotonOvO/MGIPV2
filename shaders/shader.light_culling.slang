// Light coarse culling: projects light bounds to screen-space tile ranges.

#include "shader_io.h"

[[vk::binding(0, 0)]]
StructuredBuffer<LightData> pointLightBuffer;

[[vk::binding(1, 0)]]
StructuredBuffer<LightData> spotLightBuffer;

[[vk::binding(2, 0)]]
RWStructuredBuffer<uint16_t4> pointLightXYCoarseCullIndicesBuffer;

[[vk::binding(3, 0)]]
RWStructuredBuffer<uint16_t4> spotLightXYCoarseCullIndicesBuffer;

[[vk::binding(4, 0)]]
ConstantBuffer<LightCoarseCullingUniforms> lightCoarseCulling;

STATIC_CONST uint kCoarseCullingThreadCount = 64;

uint16_t4 emptyTileBounds()
{
    return uint16_t4(0, 0, 0, 0);
}

bool accumulateProjectedPoint(float3 worldPos, inout float2 minNdc, inout float2 maxNdc, inout bool hasProjectedPoint)
{
    float4 clip = mul(lightCoarseCulling.viewProjection, float4(worldPos, 1.0));
    if(clip.w <= 0.0001)
    {
        return false;
    }

    float3 ndc = clip.xyz / clip.w;
    if(ndc.z < 0.0 || ndc.z > 1.0)
    {
        return false;
    }

    minNdc = hasProjectedPoint ? min(minNdc, ndc.xy) : ndc.xy;
    maxNdc = hasProjectedPoint ? max(maxNdc, ndc.xy) : ndc.xy;
    hasProjectedPoint = true;
    return true;
}

uint16_t4 projectSphereToTileBounds(float3 center, float radius)
{
    const uint screenWidth = uint(lightCoarseCulling.screenTileInfo.x);
    const uint screenHeight = uint(lightCoarseCulling.screenTileInfo.y);
    const uint tileCountX = uint(lightCoarseCulling.screenTileInfo.z);
    const uint tileCountY = uint(lightCoarseCulling.screenTileInfo.w);
    const float3 right = normalize(lightCoarseCulling.cameraRight.xyz);
    const float3 up = normalize(lightCoarseCulling.cameraUp.xyz);

    float2 minNdc = float2(0.0, 0.0);
    float2 maxNdc = float2(0.0, 0.0);
    bool hasProjectedPoint = false;

    accumulateProjectedPoint(center, minNdc, maxNdc, hasProjectedPoint);
    accumulateProjectedPoint(center + right * radius, minNdc, maxNdc, hasProjectedPoint);
    accumulateProjectedPoint(center - right * radius, minNdc, maxNdc, hasProjectedPoint);
    accumulateProjectedPoint(center + up * radius, minNdc, maxNdc, hasProjectedPoint);
    accumulateProjectedPoint(center - up * radius, minNdc, maxNdc, hasProjectedPoint);

    if(!hasProjectedPoint || maxNdc.x < -1.0 || minNdc.x > 1.0 || maxNdc.y < -1.0 || minNdc.y > 1.0)
    {
        return emptyTileBounds();
    }

    minNdc = clamp(minNdc, float2(-1.0, -1.0), float2(1.0, 1.0));
    maxNdc = clamp(maxNdc, float2(-1.0, -1.0), float2(1.0, 1.0));

    const float minPixelX = (minNdc.x * 0.5 + 0.5) * float(screenWidth);
    const float maxPixelX = (maxNdc.x * 0.5 + 0.5) * float(screenWidth);
    const float minPixelY = (minNdc.y * 0.5 + 0.5) * float(screenHeight);
    const float maxPixelY = (maxNdc.y * 0.5 + 0.5) * float(screenHeight);

    const uint tileStartX = min(uint(floor(minPixelX)) / LTileSizeX, tileCountX);
    const uint tileEndX = min((uint(ceil(maxPixelX)) + LTileSizeX - 1) / LTileSizeX, tileCountX);
    const uint tileStartY = min(uint(floor(minPixelY)) / LTileSizeY, tileCountY);
    const uint tileEndY = min((uint(ceil(maxPixelY)) + LTileSizeY - 1) / LTileSizeY, tileCountY);

    if(tileEndX <= tileStartX || tileEndY <= tileStartY)
    {
        return emptyTileBounds();
    }

    return uint16_t4(
        uint16_t(tileStartX),
        uint16_t(tileEndX - tileStartX),
        uint16_t(tileStartY),
        uint16_t(tileEndY - tileStartY));
}

[numthreads(kCoarseCullingThreadCount, 1, 1)]
[shader("compute")]
void kernelPointLightCoarseCulling(uint3 dispatchThreadId : SV_DispatchThreadID)
{
    const uint lightIndex = dispatchThreadId.x;
    if(lightIndex >= uint(lightCoarseCulling.lightCountInfo.x))
    {
        return;
    }

    const LightData light = pointLightBuffer[lightIndex];
    pointLightXYCoarseCullIndicesBuffer[lightIndex] =
        projectSphereToTileBounds(light.positionOrDirection, max(light.range, 0.0));
}

[numthreads(kCoarseCullingThreadCount, 1, 1)]
[shader("compute")]
void kernelSpotLightCoarseCulling(uint3 dispatchThreadId : SV_DispatchThreadID)
{
    const uint lightIndex = dispatchThreadId.x;
    if(lightIndex >= uint(lightCoarseCulling.lightCountInfo.y))
    {
        return;
    }

    const LightData light = spotLightBuffer[lightIndex];
    spotLightXYCoarseCullIndicesBuffer[lightIndex] =
        projectSphereToTileBounds(light.positionOrDirection, max(light.range, 0.0));
}
