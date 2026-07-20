// ddgi_flax_classify.slang — Flax-style DDGI probe classification (FGI-050)
// Uses flax_ddgi_common helpers for probe math, state encode/decode.
// SDF distance check, relocation, empty-area fallback probes, active list append.

#include "shader_io.h"
#include "flax_ddgi_common.slang"

[[vk::binding(0, 0)]] RWTexture2D<float4> probesData        : register(u0);
[[vk::binding(1, 0)]] RWStructuredBuffer<uint> activeProbes   : register(u1);
[[vk::binding(2, 0)]] Texture3D<float> globalSDFTex           : register(t0);
[[vk::binding(3, 0)]] SamplerState linearSampler                 : register(s0);
[[vk::binding(4, 0)]] StructuredBuffer<FlaxDDGIData> ddgiData   : register(t1);

// Helper: access the single-element structured buffer like a constant buffer
static FlaxDDGIData ddgi = ddgiData[0];

// Cascade index is set via specialization constant or push constant
#ifndef DDGI_CLASSIFY_CASCADE
static const uint kCascadeIndex = 0;
#else
static const uint kCascadeIndex = DDGI_CLASSIFY_CASCADE;
#endif

[numthreads(32, 1, 1)]
void CS_Classify(uint3 dispatchThreadId : SV_DispatchThreadID)
{
    uint probeIndex = dispatchThreadId.x;
    uint probesPerPlane = ddgi.probesCounts.x * ddgi.probesCounts.z;
    uint totalProbes = probesPerPlane * ddgi.probesCounts.y;
    if (probeIndex >= totalProbes) return;

    uint3 probeCoords = getProbeCoords(ddgi, probeIndex);
    uint scrolledIdx = getScrollingProbeIndex(ddgi, kCascadeIndex, probeCoords);
    uint2 dataCoord = getProbeTexelCoords(ddgi, kCascadeIndex, scrolledIdx);
    float probesSpacing = ddgi.probesOriginAndSpacing[kCascadeIndex].w;
    float3 probeBasePos = getProbeWorldPosition(ddgi, kCascadeIndex, probeCoords);

    // Check if higher-quality cascade covers this probe
    if (kCascadeIndex > 0)
    {
        uint prevC = kCascadeIndex - 1;
        float prevSpacing = ddgi.probesOriginAndSpacing[prevC].w;
        float3 prevOrigin = ddgi.probesOriginAndSpacing[prevC].xyz;
        float3 prevExtent = float3(ddgi.probesCounts - 1) * (prevSpacing * 0.5f);
        prevExtent -= prevSpacing * kDDGICascadeBlendSize * 2.0f;
        float3 prevCascadeWeight = prevExtent - abs(probeBasePos - prevOrigin);
        if (min(min(prevCascadeWeight.x, prevCascadeWeight.y), prevCascadeWeight.z) > 0.1f)
        {
            probesData[dataCoord] = encodeProbeData(float3(0,0,0), kDDGIProbeStateInactive, 0.0f);
            return;
        }
    }

    // Read scroll offsets to determine if probe was scrolled
    int3 scrollClears = ddgi.probesScrollOffsets[kCascadeIndex].xyz;
    bool wasScrolled = false;
    for (uint p = 0; p < 3; p++)
    {
        int newCoord = int(probeCoords[p]) + scrollClears[p];
        if (newCoord < 0 || newCoord >= int(ddgi.probesCounts[p])) wasScrolled = true;
        newCoord = int(probeCoords[p]) - scrollClears[p];
        if (newCoord < 0 || newCoord >= int(ddgi.probesCounts[p])) wasScrolled = true;
    }

    // Load existing probe data
    float4 probeData = probesData[dataCoord];
    float attention = decodeProbeAttention(probeData);
    uint state = decodeProbeState(probeData);
    uint stateOld = state;
    bool newlyActivated = wasScrolled || stateOld == kDDGIProbeStateInactive;
    float3 probeOffset = probeData.xyz * probesSpacing;
    bool relocationChanged = false;

    if (wasScrolled || state == kDDGIProbeStateInactive)
    {
        probeOffset = float3(0,0,0);
        attention = 1.0f; // wake-up
    }

    float3 probePosition = probeBasePos + probeOffset;

    // Sample SDF distance using real bounds from FlaxDDGIData
    float3 boundsMin = ddgi.sdfBoundsMinAndVoxel.xyz;
    float3 boundsMax = ddgi.sdfBoundsMaxAndRes.xyz;
    float voxelSize = ddgi.sdfBoundsMinAndVoxel.w;
    float sdfRes = ddgi.sdfBoundsMaxAndRes.w;
    float3 sdfExtent = max(boundsMax - boundsMin, float3(1e-4f, 1e-4f, 1e-4f));
    bool outsideSDFBounds = any(probePosition < boundsMin) || any(probePosition > boundsMax);
    if (outsideSDFBounds)
    {
        probesData[dataCoord] = encodeProbeData(float3(0,0,0), kDDGIProbeStateInactive, 0.0f);
        return;
    }
    float3 sdfUvw = (probePosition - boundsMin) / sdfExtent;
    float maxEncodeDistance = max(max(sdfExtent.x, sdfExtent.y), sdfExtent.z);
    float encodedSDF = globalSDFTex.SampleLevel(linearSampler, sdfUvw, 0);
    float sdf = (encodedSDF * 2.0f - 1.0f) * maxEncodeDistance;
    float sdfDist = abs(sdf);
    float voxelLimit = voxelSize * 0.8f;
    float distanceLimit = probesSpacing * 2.3f;

	// Empty area fallback: only for probes within cascade range of the SDF volume.
	// Tight threshold avoids marking far probes active that can never trace into SDF.
	float emptyAreaThreshold = probesSpacing * ddgi.probesCounts.x * 0.08f;
	if (sdf > emptyAreaThreshold && sdfDist < distanceLimit * 4.0f)
    {
        probeOffset = float3(0,0,0);
        state = newlyActivated ? kDDGIProbeStateActivated : kDDGIProbeStateActive;
        attention = kDDGIProbeAttentionMin;
    }
    else if (sdfDist > distanceLimit + length(probeOffset))
    {
        // Too far from geometry
        probeOffset = float3(0,0,0);
        state = kDDGIProbeStateInactive;
        attention = 0.0f;
    }
    else
    {
        // Active probe
        state = newlyActivated ? kDDGIProbeStateActivated : kDDGIProbeStateActive;
        float distToProbe = length(probePosition - ddgi.viewPos);
        float attentionRange = max(probesSpacing * float(ddgi.probesCounts.x), probesSpacing);
        float viewAttention = lerp(0.35f, 1.0f,
                                   1.0f - saturate(distToProbe / attentionRange));
        float geometryAttention = lerp(1.0f, 0.5f,
                                       saturate(sdfDist / max(voxelLimit, 0.0001f)));
        attention = clamp(viewAttention * geometryAttention,
                          kDDGIProbeAttentionMin, kDDGIProbeAttentionMax);

        // Relocate away from surfaces
        if (sdf <= voxelLimit)
        {
            float eps = 1.0f / max(sdfRes, 1.0f);
            float xp = globalSDFTex.SampleLevel(linearSampler, sdfUvw + float3(eps,0,0), 0);
            float xn = globalSDFTex.SampleLevel(linearSampler, sdfUvw - float3(eps,0,0), 0);
            float yp = globalSDFTex.SampleLevel(linearSampler, sdfUvw + float3(0,eps,0), 0);
            float yn = globalSDFTex.SampleLevel(linearSampler, sdfUvw - float3(0,eps,0), 0);
            float zp = globalSDFTex.SampleLevel(linearSampler, sdfUvw + float3(0,0,eps), 0);
            float zn = globalSDFTex.SampleLevel(linearSampler, sdfUvw - float3(0,0,eps), 0);
            float3 gradient = float3(xp - xn, yp - yn, zp - zn);
            float3 sdfNormal = length(gradient) > 1e-6f ? normalize(gradient) : float3(0,1,0);

            // A negative SDF means the probe is inside geometry. Move toward
            // increasing SDF by the full clearance instead of reversing the
            // gradient when sdf is less than -voxelLimit.
            float speed = (stateOld != kDDGIProbeStateActive || sdf <= 0.0f) ? 1.0f : 0.3f;
            float relocationDistance = max(voxelLimit - sdf, 0.0f);
            float3 offsetToSet = probeOffset + sdfNormal * (relocationDistance * speed);
            float relocateLimit = probesSpacing * 0.5f;
            if (length(offsetToSet) < relocateLimit)
            {
                relocationChanged = length(offsetToSet - probeOffset) > voxelSize * 0.01f;
                probeOffset = offsetToSet;

                // The finite-difference gradient is approximate. Do not trace
                // or sample a candidate that still resolves inside geometry.
                float3 relocatedPosition = probeBasePos + probeOffset;
                float3 relocatedUvw =
                    clamp((relocatedPosition - boundsMin) / sdfExtent,
                          float3(0,0,0), float3(1,1,1));
                float relocatedEncodedSDF =
                    globalSDFTex.SampleLevel(linearSampler, relocatedUvw, 0);
                float relocatedSDF =
                    (relocatedEncodedSDF * 2.0f - 1.0f) * maxEncodeDistance;
                if (relocatedSDF <= 0.0f)
                {
                    state = kDDGIProbeStateInactive;
                    attention = 0.0f;
                }
            }
            else
            {
                probeOffset = float3(0,0,0);
                state = kDDGIProbeStateInactive;
                attention = 0.0f;
            }
        }

        // Relocated probes must discard stale irradiance/distance history.
        if (state != kDDGIProbeStateInactive && relocationChanged)
            state = kDDGIProbeStateActivated;
    }

    // Write probe data
    probesData[dataCoord] = encodeProbeData(probeOffset / probesSpacing, state, attention);

    // Compact every active probe first. The per-frame budget is applied later
    // over this compacted list, otherwise sparse active probes are almost always
    // missed by a small window over the complete logical volume.
    if (state != kDDGIProbeStateInactive)
    {
        uint slot;
        InterlockedAdd(activeProbes[0], 1, slot);
        uint maxSlots = totalProbes; // buffer capacity = totalProbes + 1 (counter)
        if (slot < maxSlots)
            activeProbes[1 + slot] = probeIndex;
    }
}
