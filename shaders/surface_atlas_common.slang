// surface_atlas_common.slang — Global Surface Atlas sampling helpers (FGI-034)
// Include-only module: consumed via #include by atlas culling, lighting,
// DDGI probe trace, and debug shaders. Ported from Flax GlobalSurfaceAtlas.hlsl
// behavior. Uses VKDemo Slang conventions and shader_io.h types.
//
// Buffer layouts must match GlobalSurfaceAtlasData.h C++ packing:
//   Object: 6 float4s (bounds+radius, tileOffsets+dataSize, 3x worldToLocal
//           rows with worldPos packed in .w, worldExtents+visibility)
//   Tile:   5 float4s (atlasRectUV, 4x worldToLocal matrix rows with
//           translation in .w elements)

#ifndef SURFACE_ATLAS_COMMON_SLANG
#define SURFACE_ATLAS_COMMON_SLANG

#include "shader_io.h"

// Must match C++ GlobalSurfaceAtlasData.h
static const uint kSurfaceAtlasObjectFloat4s = 6;
static const uint kSurfaceAtlasTileFloat4s   = 5;
static const uint kSurfaceAtlasMaxTiles      = 6;

// --- Object unpacking (must match C++ PackedAtlasObject) ---

struct SurfaceAtlasObject
{
    float3 boundsPosition;
    float  boundsRadius;
    float3x3 worldToLocalRotation;
    float3 worldPosition;
    float3 worldExtents;
    bool   useVisibility;
    uint   tileOffsets[6];
    uint   dataSize;
};

float4 loadObjectBounds(RWStructuredBuffer<float4> objects, uint objectAddress)
{
    return objects[objectAddress + 0];
}

SurfaceAtlasObject loadAtlasObject(RWStructuredBuffer<float4> objects, uint objectAddress)
{
    float4 v0 = objects[objectAddress + 0];
    float4 v1 = objects[objectAddress + 1];
    float4 v2 = objects[objectAddress + 2];
    float4 v3 = objects[objectAddress + 3];
    float4 v4 = objects[objectAddress + 4];
    float4 v5 = objects[objectAddress + 5];

    SurfaceAtlasObject obj;
    obj.boundsPosition = v0.xyz;
    obj.boundsRadius   = v0.w;
    obj.worldToLocalRotation = float3x3(v2.xyz, v3.xyz, v4.xyz);
    obj.worldPosition  = float3(v2.w, v3.w, v4.w);
    obj.worldExtents   = v5.xyz;
    obj.useVisibility  = v5.w > 0.5f;

    uint v1x = asuint(v1.x);
    uint v1y = asuint(v1.y);
    uint v1z = asuint(v1.z);
    obj.dataSize       = asuint(v1.w);
    obj.tileOffsets[0] = v1x & 0xffff;
    obj.tileOffsets[1] = v1x >> 16;
    obj.tileOffsets[2] = v1y & 0xffff;
    obj.tileOffsets[3] = v1y >> 16;
    obj.tileOffsets[4] = v1z & 0xffff;
    obj.tileOffsets[5] = v1z >> 16;
    return obj;
}

// --- Tile unpacking (must match C++ PackedAtlasTile) ---

struct SurfaceAtlasTile
{
    float4 atlasRectUV;
    float4x4 worldToLocal;
    float3 viewBoundsSize;
};

SurfaceAtlasTile loadAtlasTile(RWStructuredBuffer<float4> objects, uint tileAddress)
{
    float4 v0 = objects[tileAddress + 0];
    float4 v1 = objects[tileAddress + 1];
    float4 v2 = objects[tileAddress + 2];
    float4 v3 = objects[tileAddress + 3];
    float4 v4 = objects[tileAddress + 4];

    SurfaceAtlasTile tile;
    tile.atlasRectUV = v0;
    tile.worldToLocal[0] = float4(v1.xyz, 0.0f);
    tile.worldToLocal[1] = float4(v2.xyz, 0.0f);
    tile.worldToLocal[2] = float4(v3.xyz, 0.0f);
    tile.worldToLocal[3] = float4(v1.w, v2.w, v3.w, 1.0f);
    tile.viewBoundsSize = v4.xyz;
    return tile;
}

// --- Sampling helpers ---

float3 sampleAtlasTexGather(Texture2D<float4> atlas, SamplerState linearSampler,
                            float2 atlasUV, float4 bilinearWeights)
{
    float4 sx = atlas.GatherRed(linearSampler, atlasUV);
    float4 sy = atlas.GatherGreen(linearSampler, atlasUV);
    float4 sz = atlas.GatherBlue(linearSampler, atlasUV);
    return float3(dot(sx, bilinearWeights), dot(sy, bilinearWeights), dot(sz, bilinearWeights));
}

// Sample a single atlas tile at a world-space position, returning lighting + weight.
float4 sampleAtlasTile(
    GlobalSurfaceAtlasData data,
    SurfaceAtlasObject object,
    SurfaceAtlasTile tile,
    Texture2D<float> depthTex,
    Texture2D<float4> atlasTex,
    SamplerState linearSampler,
    float3 worldPosition,
    float3 worldNormal,
    float surfaceThreshold)
{
    // Tile normal weight (dot product of sampling direction vs tile normal)
    float3 tileNormal = normalize(mul(worldNormal, (float3x3)tile.worldToLocal));
    float normalWeight = saturate(dot(float3(0, 0, -1), tileNormal));
    normalWeight = (normalWeight - 0.05f) / (1.0f - 0.05f);
    if (normalWeight <= 0.0f && object.useVisibility)
        return float4(0,0,0,0);

    // Transform world position to tile-local
    float3 tilePos = mul(float4(worldPosition, 1), tile.worldToLocal).xyz;
    float tileDepth = tilePos.z / tile.viewBoundsSize.z;
    float2 tileUV = saturate((tilePos.xy / tile.viewBoundsSize.xy) + 0.5f);
    tileUV.y = 1.0f - tileUV.y;
    tileUV = min(tileUV, 0.999999f);
    float2 atlasUV = tileUV * tile.atlasRectUV.zw + tile.atlasRectUV.xy;

    // Bilinear weights
    float2 bilinearUV = frac(atlasUV * data.resolution + 0.5f);
    float4 bilinearWeights;
    bilinearWeights.x = (1.0f - bilinearUV.x) * (bilinearUV.y);
    bilinearWeights.y = (bilinearUV.x) * (bilinearUV.y);
    bilinearWeights.z = (bilinearUV.x) * (1.0f - bilinearUV.y);
    bilinearWeights.w = (1.0f - bilinearUV.x) * (1.0f - bilinearUV.y);

    // Depth visibility
    float4 tileZ = depthTex.Gather(linearSampler, atlasUV);
    float depthThreshold = 2.0f * surfaceThreshold / tile.viewBoundsSize.z;
    float4 depthVis = float4(1,1,1,1);
    for (uint i = 0; i < 4; i++)
    {
        depthVis[i] = 1.0f - saturate((abs(tileDepth - tileZ[i]) - depthThreshold) / (0.5f * depthThreshold));
        if (tileZ[i] >= 1.0f)
            depthVis[i] = 0.0f;
    }
    float sampleWeight = dot(depthVis, bilinearWeights);
    if (object.useVisibility)
        sampleWeight *= normalWeight;
    if (sampleWeight <= 0.0f)
        return float4(0,0,0,0);

    bilinearWeights *= depthVis;

    float3 sampleColor = sampleAtlasTexGather(atlasTex, linearSampler, atlasUV, bilinearWeights);
    return float4(sampleColor * sampleWeight, sampleWeight);
}

// Main entry point: sample the Global Surface Atlas at a world position.
// Returns lighting color (rgb) and accumulated weight (a).
float4 sampleGlobalSurfaceAtlas(
    GlobalSurfaceAtlasData data,
    RWStructuredBuffer<uint> chunks,
    RWStructuredBuffer<uint> culledObjects,
    RWStructuredBuffer<float4> objects,
    Texture2D<float> depthTex,
    Texture2D<float4> atlasTex,
    SamplerState linearSampler,
    float3 worldPosition,
    float3 worldNormal,
    float surfaceThreshold)
{
    float4 result = float4(0,0,0,0);

    // The current atlas raster path is not yet scheduled by the pass graph.
    // Avoid touching uninitialized chunk/object storage until it publishes
    // an actual object count.
    if (data.objectsCount == 0)
        return result;

    // Find chunk
    int3 chunkCoord = int3(clamp(
        floor((worldPosition - data.viewPos) / data.chunkSize + float3(20.0f, 20.0f, 30.0f)),
        int3(0,0,0),
        int3(39, 39, 39)));

    uint chunkAddress = uint(chunkCoord.z * (40 * 40) + chunkCoord.y * 40 + chunkCoord.x);
    uint objectsStart = chunks[chunkAddress];
    if (objectsStart == 0)
        return result;

    // Read objects in this chunk
    uint objectsCount = culledObjects[objectsStart];
    if (objectsCount > data.objectsCount)
        return result;
    objectsStart++;

    for (uint oi = 0; oi < objectsCount; oi++)
    {
        uint objectAddress = culledObjects[objectsStart + oi];

        // Sphere cull
        float4 objBounds = loadObjectBounds(objects, objectAddress);
        if (distance(objBounds.xyz, worldPosition) > objBounds.w)
            continue;

        // Box cull
        SurfaceAtlasObject obj = loadAtlasObject(objects, objectAddress);
        float3 localPos = mul(worldPosition - obj.worldPosition, obj.worldToLocalRotation);
        float3 localExt = obj.worldExtents + surfaceThreshold;
        if (any(abs(localPos) > localExt))
            continue;

        // Sample up to 3 tiles based on normal direction
        float3 localNormal = mul(worldNormal, obj.worldToLocalRotation);
        float3 localNormalSq = localNormal * localNormal;

        uint to = obj.tileOffsets[localNormal.x > 0.0f ? 0 : 1];
        if (localNormalSq.x > 0.0025f && to != 0)
        {
            SurfaceAtlasTile tile = loadAtlasTile(objects, objectAddress + to);
            result += sampleAtlasTile(data, obj, tile, depthTex, atlasTex, linearSampler,
                                      worldPosition, worldNormal, surfaceThreshold);
        }
        to = obj.tileOffsets[localNormal.y > 0.0f ? 2 : 3];
        if (localNormalSq.y > 0.0025f && to != 0)
        {
            SurfaceAtlasTile tile = loadAtlasTile(objects, objectAddress + to);
            result += sampleAtlasTile(data, obj, tile, depthTex, atlasTex, linearSampler,
                                      worldPosition, worldNormal, surfaceThreshold);
        }
        to = obj.tileOffsets[localNormal.z > 0.0f ? 4 : 5];
        if (localNormalSq.z > 0.0025f && to != 0)
        {
            SurfaceAtlasTile tile = loadAtlasTile(objects, objectAddress + to);
            result += sampleAtlasTile(data, obj, tile, depthTex, atlasTex, linearSampler,
                                      worldPosition, worldNormal, surfaceThreshold);
        }
    }

    // Normalize
    result.rgb /= max(result.a, 0.0001f);
    return result;
}

#endif // SURFACE_ATLAS_COMMON_SLANG
