// DDGI probe SDF ray trace (GISDFRays, Wave D2-2).
//
// For every probe of the DDGI volume, traces raysPerProbe spherical-Fibonacci
// rays (rotated by a per-frame random rotation matrix) through the global SDF
// via sphere-marching (sdf_common.slang), shades hits with the plan-sanctioned
// simplified model (SDF gradient normal + sun light + SDF shadow ray +
// constant albedo) and a constant sky color on miss, then writes
// rgb = radiance / a = hit distance into the probe radiance scratch texture
// (x = ray index, y = probe index).
//
// Cf. LuxGI GISDFRays.comp:63-128 — the GlobalSurfaceAtlas hit shading is
// replaced by the simplified direct-sun model until Phase D5; the skybox/IBL
// miss sample is a constant sky color for this first version (plan deviation
// note in ddgi-implementation-plan.md Wave D2-2).
//
// Software SDF path only: no ray tracing extensions (DDGI hard constraint 3).

#include "shader_io.h"
#include "sdf_common.slang"
#include "ddgi_common.slang"

[[vk::binding(0, 0)]]
RWTexture2D<float4> radianceOutput;

[[vk::binding(1, 0)]]
Sampler3D<float> globalSDFTexture;

[[vk::binding(2, 0)]]
ConstantBuffer<DDGIRayTraceUniforms> rayTraceUniforms;

// sphericalFibonacci moved to ddgi_common.slang (Wave D2-3): the probe update
// kernels reconstruct the exact same per-ray directions from the shared
// implementation + the same per-frame rotation columns.
STATIC_CONST float kDDGIRaysPi = 3.14159265359;

GlobalSDFTraceConfig makeTraceConfig()
{
    GlobalSDFTraceConfig config;
    config.boundsMin = rayTraceUniforms.volumeBoundsMin.xyz;
    config.boundsMax = rayTraceUniforms.volumeBoundsMax.xyz;
    config.voxelSize = rayTraceUniforms.volumeBoundsMin.w;
    config.maxEncodeDistance = rayTraceUniforms.volumeBoundsMax.w;
    config.resolution = float(rayTraceUniforms.resolution);
    config.stepScale = rayTraceUniforms.sunColor.w;
    config.maxSteps = rayTraceUniforms.maxSteps;
    return config;
}

[shader("compute")]
[numthreads(16, 1, 1)]
void kernelDDGIRayTrace(uint3 dispatchThreadId : SV_DispatchThreadID)
{
    const uint rayId = dispatchThreadId.x;
    const uint probeId = dispatchThreadId.y;
    if (rayId >= rayTraceUniforms.raysPerProbe || probeId >= rayTraceUniforms.totalProbes)
    {
        return;
    }

    // Probe origin through the buffer device address of the position buffer
    // (same BDA pattern as shader.rast.slang sceneInfo.dataBufferAddress).
    float4* probePositions = (float4*)rayTraceUniforms.probePositionAddress;
    const float3 rayOrigin = probePositions[probeId].xyz;

    // Per-frame random rotation applied to the Fibonacci basis direction.
    // Columns are combined explicitly so no matrix-layout convention applies.
    const float3 fibonacci =
        sphericalFibonacci(float(rayId), float(rayTraceUniforms.raysPerProbe));
    const float3 rayDirection = normalize(
        rayTraceUniforms.rotationCol0.xyz * fibonacci.x
        + rayTraceUniforms.rotationCol1.xyz * fibonacci.y
        + rayTraceUniforms.rotationCol2.xyz * fibonacci.z);

    const GlobalSDFTraceConfig config = makeTraceConfig();
    const float maxTraceDistance = rayTraceUniforms.sunDirection.w;

    const GlobalSDFTraceHit hit =
        traceGlobalSDF(globalSDFTexture, config, rayOrigin, rayDirection, maxTraceDistance);

    float3 radiance = float3(0.0, 0.0, 0.0);
    float hitDistance = maxTraceDistance;
    if (isGlobalSDFHit(hit))
    {
        if (hit.hitTime <= config.voxelSize)
        {
            // Probe sits inside / flush with geometry: no reliable shading
            // (LuxGI GISDFRays.comp:93-96 backface guard).
            radiance = float3(0.0, 0.0, 0.0);
            hitDistance = maxTraceDistance;
        }
        else
        {
            const float3 hitPosition = rayOrigin + rayDirection * hit.hitTime;
            const float3 hitNormal =
                computeGlobalSDFGradientNormal(globalSDFTexture, config, hitPosition);
            // sunDirection.xyz travels light -> scene, so the shading direction
            // toward the sun is its negation.
            const float3 toSun = -rayTraceUniforms.sunDirection.xyz;
            const float nDotL = max(dot(hitNormal, toSun), 0.0);

            // Sun shadow ray: march toward the sun from a normal-offset origin;
            // any hit means occluded (binary visibility).
            float sunVisibility = 0.0;
            if (nDotL > 0.0)
            {
                const float3 shadowOrigin = hitPosition + hitNormal * (config.voxelSize * 2.0);
                const GlobalSDFTraceHit shadowHit =
                    traceGlobalSDF(globalSDFTexture, config, shadowOrigin, toSun, maxTraceDistance);
                sunVisibility = isGlobalSDFHit(shadowHit) ? 0.0 : 1.0;
            }

            // Simplified hit shading: constant-albedo Lambert direct sun term.
            // Per-object average albedo + surface cache are later milestones.
            const float albedo = rayTraceUniforms.skyColorAlbedo.w;
            radiance =
                (albedo / kDDGIRaysPi) * rayTraceUniforms.sunColor.xyz * nDotL * sunVisibility;
            hitDistance = hit.hitTime;
        }
    }
    else
    {
        // Miss: constant sky radiance (IBL cubemap wiring deferred, see plan note).
        radiance = rayTraceUniforms.skyColorAlbedo.xyz;
        hitDistance = maxTraceDistance;
    }

    radianceOutput[uint2(rayId, probeId)] = float4(radiance, hitDistance);
}
