// SDF sphere-march common module (DDGI Wave D2-2).
//
// Single-cascade adaptation of LuxGI tracyGlobalSDF (SDFCommon.glsl:98-194):
//   - coarse-mip-first distance query (min-filtered mip chain on the same
//     texture, cf. sdf_global_mipmap.slang) refined with a mip-0 sample only
//     when the conservative coarse distance falls below a threshold;
//   - adaptive hit threshold minSurfaceThickness = voxelHalf *
//     clamp(stepTime / voxelSize, 0, 1) (LuxGI SDFCommon.glsl:166);
//   - step = max(distance * stepScale, voxelSize) (LuxGI SDFCommon.glsl:189);
//   - 6-tap central-difference gradient normal (LuxGI SDFCommon.glsl:174-183).
//
// Distance conventions (must stay in sync with sdf_global_compose.slang):
// texels store (d + 1) / 2 where d = worldDistance / maxEncodeDistance and
// maxEncodeDistance = max edge length of the global volume AABB.
//
// Include-only module: excluded from the compile_slang list in CMakeLists.txt
// (same pattern as shader.ibl_common.slang).

#ifndef SDF_COMMON_SLANG
#define SDF_COMMON_SLANG

#include "shader_io.h"

struct GlobalSDFTraceConfig
{
    float3 boundsMin;        // world-space volume min
    float3 boundsMax;        // world-space volume max
    float voxelSize;         // world units per mip-0 voxel
    float maxEncodeDistance; // world distance mapping to encoded 1.0
    float resolution;        // mip-0 voxel resolution (cubic)
    float stepScale;         // sphere-march step multiplier
    uint maxSteps;           // step budget (64-128 per plan section 3.1)
};

struct GlobalSDFTraceHit
{
    float hitTime; // world-space distance along the ray; < 0 = miss
    uint steps;    // steps consumed (stats/debug)
};

bool isGlobalSDFHit(GlobalSDFTraceHit hit)
{
    return hit.hitTime >= 0.0;
}

// Ray/AABB slab intersection: x = enter t, y = exit t (unclamped; miss when x >= y).
float2 rayAABBIntersection(float3 origin, float3 direction, float3 boxMin, float3 boxMax)
{
    const float3 invDirection = float3(1.0, 1.0, 1.0) / direction;
    const float3 t0 = (boxMin - origin) * invDirection;
    const float3 t1 = (boxMax - origin) * invDirection;
    const float3 tMin = min(t0, t1);
    const float3 tMax = max(t0, t1);
    return float2(max(max(tMin.x, tMin.y), tMin.z), min(min(tMax.x, tMax.y), tMax.z));
}

// Two-level distance query (LuxGI mip-coarse -> fine pattern, single cascade):
// sample the coarsest min mip first; only when its conservative distance drops
// below the refine threshold pay for a mip-0 sample.
float sampleGlobalSDFDistance(Sampler3D<float> sdfTexture, GlobalSDFTraceConfig config, float3 uvw)
{
    const float coarseMip = float(LGlobalSDFMipCount - 1);
    float encoded = sdfTexture.SampleLevel(uvw, coarseMip);
    float dist = (encoded * 2.0 - 1.0) * config.maxEncodeDistance;
    // Two coarse texels (one coarse voxel = voxelSize << (LGlobalSDFMipCount - 1)).
    const float refineThreshold = config.voxelSize * float(2 << (LGlobalSDFMipCount - 1));
    if (dist < refineThreshold)
    {
        encoded = sdfTexture.SampleLevel(uvw, 0.0);
        dist = (encoded * 2.0 - 1.0) * config.maxEncodeDistance;
    }
    return dist;
}

// Sphere-march through the single-cascade global SDF. Returns hitTime < 0 on miss.
GlobalSDFTraceHit traceGlobalSDF(Sampler3D<float> sdfTexture, GlobalSDFTraceConfig config,
                                 float3 origin, float3 direction, float maxDistance)
{
    GlobalSDFTraceHit hit;
    hit.hitTime = -1.0;
    hit.steps = 0u;

    const float2 intersections = rayAABBIntersection(origin, direction, config.boundsMin, config.boundsMax);
    const float tEnd = min(intersections.y, maxDistance);
    float stepTime = max(intersections.x, 0.0);
    if (stepTime >= tEnd)
    {
        return hit;
    }

    const float3 extent = max(config.boundsMax - config.boundsMin, float3(1e-4, 1e-4, 1e-4));
    const float voxelHalf = config.voxelSize * 0.5;

    for (uint step = 0u; step < config.maxSteps && stepTime < tEnd; ++step)
    {
        hit.steps = step + 1u;
        const float3 stepPosition = origin + direction * stepTime;
        const float3 uvw =
            clamp((stepPosition - config.boundsMin) / extent, float3(0.0, 0.0, 0.0), float3(1.0, 1.0, 1.0));
        const float stepDistance = sampleGlobalSDFDistance(sdfTexture, config, uvw);

        // Adaptive hit threshold (LuxGI SDFCommon.glsl:166): tight right at the
        // ray origin, half a voxel once the ray has travelled one voxel.
        const float minSurfaceThickness = voxelHalf * clamp(stepTime / config.voxelSize, 0.0, 1.0);
        if (stepDistance < minSurfaceThickness)
        {
            hit.hitTime = max(stepTime + stepDistance - minSurfaceThickness, 0.0);
            break;
        }

        // March at least one voxel per step (LuxGI SDFCommon.glsl:189).
        stepTime += max(stepDistance * config.stepScale, config.voxelSize);
    }
    return hit;
}

// SDF gradient normal: 6-tap central difference at mip 0
// (LuxGI SDFCommon.glsl:174-183).
float3 computeGlobalSDFGradientNormal(Sampler3D<float> sdfTexture, GlobalSDFTraceConfig config,
                                      float3 worldPos)
{
    const float3 extent = max(config.boundsMax - config.boundsMin, float3(1e-4, 1e-4, 1e-4));
    const float3 uvw =
        clamp((worldPos - config.boundsMin) / extent, float3(0.0, 0.0, 0.0), float3(1.0, 1.0, 1.0));
    const float texelOffset = 1.0 / config.resolution;
    const float xp = sdfTexture.SampleLevel(float3(uvw.x + texelOffset, uvw.y, uvw.z), 0.0);
    const float xn = sdfTexture.SampleLevel(float3(uvw.x - texelOffset, uvw.y, uvw.z), 0.0);
    const float yp = sdfTexture.SampleLevel(float3(uvw.x, uvw.y + texelOffset, uvw.z), 0.0);
    const float yn = sdfTexture.SampleLevel(float3(uvw.x, uvw.y - texelOffset, uvw.z), 0.0);
    const float zp = sdfTexture.SampleLevel(float3(uvw.x, uvw.y, uvw.z + texelOffset), 0.0);
    const float zn = sdfTexture.SampleLevel(float3(uvw.x, uvw.y, uvw.z - texelOffset), 0.0);
    const float3 gradient = float3(xp - xn, yp - yn, zp - zn);
    const float gradientLength = length(gradient);
    return gradientLength > 1e-6 ? gradient / gradientLength : float3(0.0, 1.0, 0.0);
}

#endif // SDF_COMMON_SLANG
