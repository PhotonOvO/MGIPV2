// Tiled Light Culling Compute Shader
// Culls lights against screen-space tiles using depth bounds

#include "shader_io.h"

//------------------------------------------------------------------------------
// Resource Bindings
//------------------------------------------------------------------------------

// Light data buffer (input)
[[vk::binding(0, 0)]]
StructuredBuffer<LightData> lightBuffer;

// Light uniforms (input)
[[vk::binding(1, 0)]]
ConstantBuffer<LightListUniforms> lightUniforms;

// Depth buffer (input for depth bounds calculation)
[[vk::binding(2, 0)]]
Texture2D<float> depthTexture;

// Tile light index list (output)
[[vk::binding(3, 0)]]
RWStructuredBuffer<uint> tileLightIndexBuffer;

// Scene-level culling parameters
[[vk::binding(LBindLightCulling, LSetScene)]]
ConstantBuffer<LightCullingUniforms> lightCulling;

//------------------------------------------------------------------------------
// Constants
//------------------------------------------------------------------------------

STATIC_CONST uint TILE_SIZE_X = 16;
STATIC_CONST uint TILE_SIZE_Y = 16;

//------------------------------------------------------------------------------
// Helper Functions
//------------------------------------------------------------------------------

// Convert screen position to view space depth bounds
float2 getTileDepthBounds(uint2 tileCoord)
{
    uint2 minPixel = tileCoord * uint2(TILE_SIZE_X, TILE_SIZE_Y);
    uint2 maxPixel = minPixel + uint2(TILE_SIZE_X, TILE_SIZE_Y) - 1;

    float minDepth = 1.0;
    float maxDepth = 0.0;

    // Sample depth buffer to find min/max depth in tile
    // Use Load for compute shader (no sampler needed)
    for (uint y = minPixel.y; y <= maxPixel.y && y < uint(lightCulling.screenSizeAndClipPlanes.y); y += 4)
    {
        for (uint x = minPixel.x; x <= maxPixel.x && x < uint(lightCulling.screenSizeAndClipPlanes.x); x += 4)
        {
            float depth = depthTexture.Load(int3(x, y, 0));
            if (depth < 0.9999)  // Not background
            {
                minDepth = min(minDepth, depth);
                maxDepth = max(maxDepth, depth);
            }
        }
    }

    // If no valid depth, use full range
    if (minDepth > maxDepth)
    {
        minDepth = 0.0;
        maxDepth = 1.0;
    }

    return float2(minDepth, maxDepth);
}

// Convert NDC depth to view-space Z
float ndcDepthToViewZ(float ndcDepth)
{
    // For perspective projection with GLM_FORCE_DEPTH_ZERO_TO_ONE:
    // ndcZ = P20 + P32/viewZ
    // viewZ = P32 / (ndcZ - P20)
    float P20 = lightCulling.projectionMatrix[2][2];
    float P32 = lightCulling.projectionMatrix[3][2];
    return P32 / (ndcDepth - P20);
}

// Check if point light intersects tile frustum
bool lightIntersectsTile(const LightData light, uint2 tileCoord, float2 depthBounds, float4 frustumPlanes[4])
{
    if (light.lightType == LLightTypeDirectional)
        return true;  // Directional lights always visible

    // Point light sphere-frustum intersection test
    // Transform light position to view space
    float4 viewPos = mul(lightCulling.viewMatrix, float4(light.positionOrDirection, 1.0));

    // Convert depth bounds to view space
    float viewMinZ = ndcDepthToViewZ(depthBounds.y);  // Near = max NDC depth
    float viewMaxZ = ndcDepthToViewZ(depthBounds.x);  // Far = min NDC depth

    // Check depth bounds (view space Z is negative in front of camera)
    if (viewPos.z - light.range > viewMinZ || viewPos.z + light.range < viewMaxZ)
        return false;

    // Check frustum planes (simplified sphere-plane test)
    for (uint i = 0; i < 4; ++i)
    {
        float dist = dot(frustumPlanes[i], float4(viewPos.xyz, 1.0));
        if (dist < -light.range)
            return false;  // Sphere entirely outside plane
    }

    return true;
}

//------------------------------------------------------------------------------
// Compute Shader
//------------------------------------------------------------------------------

[numthreads(TILE_SIZE_X, TILE_SIZE_Y, 1)]
[shader("compute")]
void computeMain(uint3 dispatchThreadId : SV_DispatchThreadID)
{
    const uint2 tileCoord = dispatchThreadId.xy;

    // Bounds check: ensure tile is within valid range
    const uint screenWidth = uint(lightCulling.screenSizeAndClipPlanes.x);
    const uint screenHeight = uint(lightCulling.screenSizeAndClipPlanes.y);
    const uint numTilesX = (screenWidth + TILE_SIZE_X - 1) / TILE_SIZE_X;
    const uint numTilesY = (screenHeight + TILE_SIZE_Y - 1) / TILE_SIZE_Y;

    if(tileCoord.x >= numTilesX || tileCoord.y >= numTilesY)
        return;  // Out of bounds tile - no work

    const uint tileIndex = tileCoord.y * numTilesX + tileCoord.x;

    // Get depth bounds for this tile
    float2 depthBounds = getTileDepthBounds(tileCoord);

    // Build tile frustum planes from projection matrix
    // Extract frustum planes from inverse projection matrix
    float4 frustumPlanes[4];

    // Left plane
    frustumPlanes[0] = float4(
        lightCulling.invProjectionMatrix[0][3] + lightCulling.invProjectionMatrix[0][0],
        lightCulling.invProjectionMatrix[1][3] + lightCulling.invProjectionMatrix[1][0],
        lightCulling.invProjectionMatrix[2][3] + lightCulling.invProjectionMatrix[2][0],
        lightCulling.invProjectionMatrix[3][3] + lightCulling.invProjectionMatrix[3][0]
    );
    // Right plane
    frustumPlanes[1] = float4(
        lightCulling.invProjectionMatrix[0][3] - lightCulling.invProjectionMatrix[0][0],
        lightCulling.invProjectionMatrix[1][3] - lightCulling.invProjectionMatrix[1][0],
        lightCulling.invProjectionMatrix[2][3] - lightCulling.invProjectionMatrix[2][0],
        lightCulling.invProjectionMatrix[3][3] - lightCulling.invProjectionMatrix[3][0]
    );
    // Top plane
    frustumPlanes[2] = float4(
        lightCulling.invProjectionMatrix[0][3] - lightCulling.invProjectionMatrix[0][1],
        lightCulling.invProjectionMatrix[1][3] - lightCulling.invProjectionMatrix[1][1],
        lightCulling.invProjectionMatrix[2][3] - lightCulling.invProjectionMatrix[2][1],
        lightCulling.invProjectionMatrix[3][3] - lightCulling.invProjectionMatrix[3][1]
    );
    // Bottom plane
    frustumPlanes[3] = float4(
        lightCulling.invProjectionMatrix[0][3] + lightCulling.invProjectionMatrix[0][1],
        lightCulling.invProjectionMatrix[1][3] + lightCulling.invProjectionMatrix[1][1],
        lightCulling.invProjectionMatrix[2][3] + lightCulling.invProjectionMatrix[2][1],
        lightCulling.invProjectionMatrix[3][3] + lightCulling.invProjectionMatrix[3][1]
    );

    // Normalize planes
    for (uint i = 0; i < 4; ++i)
    {
        float len = length(frustumPlanes[i].xyz);
        frustumPlanes[i] = frustumPlanes[i] / len;
    }

    // Cull lights against tile
    uint lightCount = 0;
    uint baseIndex = tileIndex * LMaxLightsPerTile;

    for (uint i = 0; i < lightUniforms.numLights && lightCount < LMaxLightsPerTile; ++i)
    {
        if (lightIntersectsTile(lightBuffer[i], tileCoord, depthBounds, frustumPlanes))
        {
            tileLightIndexBuffer[baseIndex + lightCount] = i;
            lightCount++;
        }
    }

    // Mark end of list with sentinel (-1)
    if (lightCount < LMaxLightsPerTile)
    {
        tileLightIndexBuffer[baseIndex + lightCount] = 0xFFFFFFFF;
    }
}
