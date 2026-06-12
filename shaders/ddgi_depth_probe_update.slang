// DDGI depth probe update (Wave D2-3).
//
// One 16x16 workgroup per probe: every thread owns one interior octahedral
// texel of the probe's depth tile and accumulates all raysPerProbe ray-hit
// distances weighted by pow(max(0, dot(texelDir, rayDir)), depthSharpness)
// (sharpness = 50). Stores r = mean depth, g = mean depth^2 for the Chebyshev
// visibility test in SampleProbe (D3-1), temporally blended against the
// history atlas with mix(new, history, hysteresis) after the first frame.
//
// Translated from LuxGI ProbeUpdate.glsl (DEPTHPROBE_UPDATE variant). The ray
// distance comes from the radiance scratch alpha channel; misses store the
// max trace distance (volume diagonal, never the LuxGI -1 sentinel — see
// ddgi_gi_sdf_rays.slang), so only the maxDistance clamp applies here.

#include "shader_io.h"
#include "ddgi_common.slang"

// Current depth atlas (written this frame, RG16F: mean depth / mean depth^2).
[[vk::binding(0, 0)]]
RWTexture2D<float2> depthOutput;

// History depth atlas (read-only; previous frame's blend target).
[[vk::binding(1, 0)]]
RWTexture2D<float2> depthHistory;

// Per-ray radiance scratch: x = ray index, y = probe index,
// rgb = radiance, a = hit distance (ddgi_gi_sdf_rays.slang).
[[vk::binding(2, 0)]]
RWTexture2D<float4> radianceInput;

[[vk::push_constant]]
ConstantBuffer<DDGIProbeUpdatePush> updatePush;

// Octahedral side length: must match DDGIProbeUpdatePass::kDepthTexelSize and
// the [numthreads] dimensions below.
static const int kProbeSideLength = 16;
static const uint kCacheSize = 64;
static const float kFltEps = 0.00000001;

groupshared float4 gRayDirectionDepth[kCacheSize];

// Reconstructs the exact ray direction traced by ddgi_gi_sdf_rays.slang for
// this frame (same Fibonacci basis, same rotation columns).
float3 ddgiRayDirection(uint rayIndex)
{
    const float3 fibonacci =
        sphericalFibonacci(float(rayIndex), float(updatePush.raysPerProbe));
    return normalize(updatePush.rotationCol0.xyz * fibonacci.x
                     + updatePush.rotationCol1.xyz * fibonacci.y
                     + updatePush.rotationCol2.xyz * fibonacci.z);
}

[shader("compute")]
[numthreads(16, 16, 1)]
void kernelDDGIDepthProbeUpdate(uint3 groupId : SV_GroupID,
                                uint3 localId : SV_GroupThreadID,
                                uint localIndex : SV_GroupIndex)
{
    // One workgroup per probe tile; interior texel of this thread
    // (tile origin + 1px probe border + 1px atlas edge = +2).
    const int probeIndex = int(groupId.x) + int(updatePush.probesPerRow) * int(groupId.y);
    const int2 currentCoord =
        int2(groupId.xy) * (kProbeSideLength + 2) + int2(localId.xy) + int2(2, 2);

    // Staggered update (Wave D4-2, option (1)): probes outside this frame's
    // subset pass-through copy their history texel into the current atlas so
    // both ping-pong atlases stay complete (the border update still runs for
    // every probe and reads this freshly copied interior). Skipped on the
    // first frame (history holds garbage — everything must compute), and
    // updateStride <= 1 degenerates to a full per-frame update. The branch is
    // workgroup-uniform (probeIndex depends only on groupId), so returning
    // before the group barriers below is safe.
    if (updatePush.firstFrame == 0u && updatePush.updateStride > 1u
        && (uint(probeIndex) % updatePush.updateStride) != updatePush.updateOffset)
    {
        depthOutput[uint2(currentCoord)] = depthHistory[uint2(currentCoord)];
        return;
    }

    const float3 texelDirection =
        octDecode(normalizedOctCoord(currentCoord, kProbeSideLength));

    float2 result = float2(0.0, 0.0);
    float totalWeight = 0.0;

    uint remainingRays = updatePush.raysPerProbe;
    uint offset = 0u;
    while (remainingRays > 0u)
    {
        const uint numRays = min(kCacheSize, remainingRays);

        // Cooperative cache fill (LuxGI loadCache): one ray per thread.
        if (localIndex < numRays)
        {
            const uint rayIndex = offset + localIndex;
            const float hitDistance = radianceInput[uint2(rayIndex, uint(probeIndex))].a;
            gRayDirectionDepth[localIndex] = float4(ddgiRayDirection(rayIndex), hitDistance);
        }
        GroupMemoryBarrierWithGroupSync();

        // Gather (LuxGI gatherRays, DEPTHPROBE_UPDATE branch).
        for (uint r = 0u; r < numRays; ++r)
        {
            // Misses carry the max trace distance, so the clamp below covers
            // them (no LuxGI -1 sentinel remap needed).
            const float rayProbeDistance =
                min(updatePush.maxDistance, gRayDirectionDepth[r].w - 0.01);
            const float weight =
                pow(max(0.0, dot(texelDirection, gRayDirectionDepth[r].xyz)),
                    updatePush.depthSharpness);
            if (weight >= kFltEps)
            {
                result += float2(rayProbeDistance * weight,
                                 rayProbeDistance * rayProbeDistance * weight);
                totalWeight += weight;
            }
        }
        GroupMemoryBarrierWithGroupSync();

        remainingRays -= numRays;
        offset += numRays;
    }

    if (totalWeight > kFltEps)
    {
        result *= 1.0 / (2.0 * totalWeight);
    }

    // Hysteresis blend (LuxGI ProbeUpdate.glsl:144-145); first frame writes
    // the fresh result directly.
    if (updatePush.firstFrame == 0u)
    {
        const float2 prevResult = depthHistory[uint2(currentCoord)].rg;
        result = lerp(result, prevResult, updatePush.hysteresis);
    }

    depthOutput[uint2(currentCoord)] = result;
}
