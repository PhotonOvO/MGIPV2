// DDGI irradiance probe update (Wave D2-3).
//
// One 8x8 workgroup per probe: every thread owns one interior octahedral texel
// of the probe's irradiance tile and accumulates all raysPerProbe rays of that
// probe (radiance scratch row y = probe index) weighted by
// max(0, dot(texelDir, rayDir)). The blended result is gamma-encoded with
// pow(x, 1 / ddgiGamma) before storage and temporally mixed against the
// history atlas with mix(new, history, hysteresis) — skipped on the first
// frame, which writes directly.
//
// Translated from LuxGI ProbeUpdate.glsl (irradiance variant, hysteresis blend
// at ProbeUpdate.glsl:145). Ray directions are not stored by the ray trace
// pass; they are reconstructed from the shared spherical Fibonacci basis +
// the per-frame rotation columns (must match ddgi_gi_sdf_rays.slang exactly).

#include "shader_io.h"
#include "ddgi_common.slang"

// Current irradiance atlas (written this frame).
[[vk::binding(0, 0)]]
RWTexture2D<float4> irradianceOutput;

// History irradiance atlas (read-only; previous frame's blend target).
[[vk::binding(1, 0)]]
RWTexture2D<float4> irradianceHistory;

// Per-ray radiance scratch: x = ray index, y = probe index,
// rgb = radiance, a = hit distance (ddgi_gi_sdf_rays.slang).
[[vk::binding(2, 0)]]
RWTexture2D<float4> radianceInput;

[[vk::push_constant]]
ConstantBuffer<DDGIProbeUpdatePush> updatePush;

// Octahedral side length: must match DDGIProbeUpdatePass::kIrradianceTexelSize
// and the [numthreads] dimensions below.
static const int kProbeSideLength = 8;
static const uint kCacheSize = 64;
static const float kFltEps = 0.00000001;

groupshared float4 gRayDirectionDepth[kCacheSize];
groupshared float3 gRayHitRadiance[kCacheSize];

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
[numthreads(8, 8, 1)]
void kernelDDGIIrradianceProbeUpdate(uint3 groupId : SV_GroupID,
                                     uint3 localId : SV_GroupThreadID,
                                     uint localIndex : SV_GroupIndex)
{
    // One workgroup per probe tile; interior texel of this thread
    // (tile origin + 1px probe border + 1px atlas edge = +2, LuxGI
    // ProbeUpdate.glsl:107).
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
        irradianceOutput[uint2(currentCoord)] = irradianceHistory[uint2(currentCoord)];
        return;
    }

    const float3 texelDirection =
        octDecode(normalizedOctCoord(currentCoord, kProbeSideLength));

    float3 result = float3(0.0, 0.0, 0.0);
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
            const float4 radianceDepth = radianceInput[uint2(rayIndex, uint(probeIndex))];
            gRayDirectionDepth[localIndex] = float4(ddgiRayDirection(rayIndex), radianceDepth.a);
            gRayHitRadiance[localIndex] = radianceDepth.rgb;
        }
        GroupMemoryBarrierWithGroupSync();

        // Gather (LuxGI gatherRays, irradiance branch).
        for (uint r = 0u; r < numRays; ++r)
        {
            const float weight = max(0.0, dot(texelDirection, gRayDirectionDepth[r].xyz));
            if (weight >= kFltEps)
            {
                result += gRayHitRadiance[r] * weight;
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

    // Storage gamma encode (ddgiGamma = 5.0), then hysteresis blend in the
    // encoded domain — exactly LuxGI ProbeUpdate.glsl:141-145.
    result = pow(result, float3(1.0, 1.0, 1.0) / updatePush.ddgiGamma);
    if (updatePush.firstFrame == 0u)
    {
        const float3 prevResult = irradianceHistory[uint2(currentCoord)].rgb;
        result = lerp(result, prevResult, updatePush.hysteresis);
    }

    irradianceOutput[uint2(currentCoord)] = float4(result, 1.0);
}
