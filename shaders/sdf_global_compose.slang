// Global SDF compose (DDGI Wave D1-2).
// For every voxel of the global volume, walks the registered mesh SDF list and
// takes the min distance (cf. LuxGI GlobalDistanceField.cpp:82-191, simplified
// to a brute-force loop over <= LGlobalSDFMaxMeshSDFs assets for the first
// single-cascade version).
//
// Distance conventions (must stay in sync with tools/sdf_baker):
//   - Mesh SDF texels store (d + 1) / 2 where d = worldDistance / meshMaxDistance,
//     meshMaxDistance = max edge length of the padded mesh AABB (boundsMax.w).
//   - The global volume re-encodes with the same scheme against the global max
//     encode distance (volumeBoundsMax.w).
//   - Outside a mesh AABB the distance to the AABB is used as a conservative
//     lower bound of the true surface distance (safe for sphere tracing).
#include "shader_io.h"

[[vk::binding(0, 0)]]
RWTexture3D<float> dstVolume;

[[vk::binding(1, 0)]]
Sampler3D<float> meshSDFTextures[LGlobalSDFMaxMeshSDFs];

[[vk::binding(2, 0)]]
ConstantBuffer<GlobalSDFComposeUniforms> composeUniforms;

float distanceToAABB(float3 p, float3 boundsMin, float3 boundsMax)
{
    const float3 d = max(max(boundsMin - p, p - boundsMax), float3(0.0, 0.0, 0.0));
    return length(d);
}

[shader("compute")]
[numthreads(4, 4, 4)]
void kernelGlobalSDFCompose(uint3 dispatchThreadId : SV_DispatchThreadID)
{
    const uint resolution = composeUniforms.resolution;
    if (dispatchThreadId.x >= resolution || dispatchThreadId.y >= resolution
        || dispatchThreadId.z >= resolution)
    {
        return;
    }

    const float3 volumeMin = composeUniforms.volumeBoundsMin.xyz;
    const float voxelSize = composeUniforms.volumeBoundsMin.w;
    const float globalMaxDistance = max(composeUniforms.volumeBoundsMax.w, 1e-4);
    const float3 worldPos = volumeMin + (float3(dispatchThreadId) + float3(0.5, 0.5, 0.5)) * voxelSize;

    float minDistance = globalMaxDistance;
    const uint numMeshSDFs = min(composeUniforms.numMeshSDFs, uint(LGlobalSDFMaxMeshSDFs));
    for (uint i = 0u; i < numMeshSDFs; ++i)
    {
        const float3 boundsMin = composeUniforms.meshBoundsMin[i].xyz;
        const float3 boundsMax = composeUniforms.meshBoundsMax[i].xyz;
        const float meshMaxDistance = max(composeUniforms.meshBoundsMax[i].w, 1e-4);
        const float aabbDistance = distanceToAABB(worldPos, boundsMin, boundsMax);

        float meshDistance;
        if (aabbDistance > 0.0)
        {
            // Outside the baked volume: distance to the AABB is a conservative
            // lower bound of the distance to the surface inside it.
            meshDistance = aabbDistance;
        }
        else
        {
            const float3 extent = max(boundsMax - boundsMin, float3(1e-4, 1e-4, 1e-4));
            const float3 uvw = (worldPos - boundsMin) / extent;
            const float encoded = meshSDFTextures[i].SampleLevel(uvw, 0.0);
            meshDistance = (encoded * 2.0 - 1.0) * meshMaxDistance;
        }
        minDistance = min(minDistance, meshDistance);
    }

    const float normalized = clamp(minDistance / globalMaxDistance, -1.0, 1.0);
    dstVolume[dispatchThreadId] = (normalized + 1.0) * 0.5;
}
