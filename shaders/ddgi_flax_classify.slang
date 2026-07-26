// ddgi_flax_classify.slang — Flax-style DDGI probe classification (FGI-050)
// Uses flax_ddgi_common helpers for probe math, state encode/decode.
// SDF distance check, relocation, empty-area fallback probes, active flags.

#include "shader_io.h"
#include "flax_ddgi_common.slang"

[[vk::binding(0, 0)]] RWTexture2D<float4> probesData        : register(u0);
[[vk::binding(1, 0)]] RWStructuredBuffer<uint> activeProbes   : register(u1);
[[vk::binding(2, 0)]] Texture3D<float> globalSDFTex           : register(t0);
[[vk::binding(3, 0)]] SamplerState linearSampler                 : register(s0);
[[vk::binding(4, 0)]] StructuredBuffer<FlaxDDGIData> ddgiData   : register(t1);

// Helper: access the single-element structured buffer like a constant buffer
static FlaxDDGIData ddgi = ddgiData[0];

struct FlaxDDGICascadePush
{
    uint cascadeIndex;
};
[[vk::push_constant]] ConstantBuffer<FlaxDDGICascadePush> cascadePush;

[numthreads(32, 1, 1)]
void CS_Classify(uint3 dispatchThreadId : SV_DispatchThreadID)
{
    uint cascadeIndex = min(cascadePush.cascadeIndex, ddgi.cascadesCount - 1u);
    uint probeIndex = dispatchThreadId.x;
    uint probesPerPlane = ddgi.probesCounts.x * ddgi.probesCounts.z;
    uint totalProbes = probesPerPlane * ddgi.probesCounts.y;
    if (probeIndex >= totalProbes) return;

    // Write one deterministic flag per logical probe. The compaction pass
    // scans these flags in probe-index order, independent of wave scheduling.
    uint priorityBase = 4u;
    uint regularBase = priorityBase + totalProbes;
    activeProbes[priorityBase + probeIndex] = 0u;
    activeProbes[regularBase + probeIndex] = 0u;


    uint3 probeCoords = getProbeCoords(ddgi, probeIndex);
    uint scrolledIdx = getScrollingProbeIndex(ddgi, cascadeIndex, probeCoords);
    uint2 dataCoord = getProbeTexelCoords(ddgi, cascadeIndex, scrolledIdx);
    float probesSpacing = ddgi.probesOriginAndSpacing[cascadeIndex].w;
    float3 probeBasePos = getProbeWorldPosition(ddgi, cascadeIndex, probeCoords);

    // Check if higher-quality cascade covers this probe
    if (cascadeIndex > 0)
    {
        uint prevC = cascadeIndex - 1;
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

    // Mark only the logical slab exposed by this frame's origin movement.
    int3 scrollDelta = ddgi.probesScrollDeltas[cascadeIndex].xyz;
    bool wasScrolled = false;
    for (uint p = 0; p < 3; p++)
    {
        int delta = scrollDelta[p];
        int count = int(ddgi.probesCounts[p]);
        if (abs(delta) >= count) wasScrolled = true;
        else if (delta > 0 && int(probeCoords[p]) >= count - delta) wasScrolled = true;
        else if (delta < 0 && int(probeCoords[p]) < -delta) wasScrolled = true;
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

    // SDF curvature separates planar/empty regions from corners and thin
    // geometry without changing the fixed probe-grid/atlas topology.
    float sdfTexel = 1.0f / max(sdfRes, 1.0f);
    float sdfXP = (globalSDFTex.SampleLevel(linearSampler, sdfUvw + float3(sdfTexel,0,0), 0) * 2.0f - 1.0f) * maxEncodeDistance;
    float sdfXN = (globalSDFTex.SampleLevel(linearSampler, sdfUvw - float3(sdfTexel,0,0), 0) * 2.0f - 1.0f) * maxEncodeDistance;
    float sdfYP = (globalSDFTex.SampleLevel(linearSampler, sdfUvw + float3(0,sdfTexel,0), 0) * 2.0f - 1.0f) * maxEncodeDistance;
    float sdfYN = (globalSDFTex.SampleLevel(linearSampler, sdfUvw - float3(0,sdfTexel,0), 0) * 2.0f - 1.0f) * maxEncodeDistance;
    float sdfZP = (globalSDFTex.SampleLevel(linearSampler, sdfUvw + float3(0,0,sdfTexel), 0) * 2.0f - 1.0f) * maxEncodeDistance;
    float sdfZN = (globalSDFTex.SampleLevel(linearSampler, sdfUvw - float3(0,0,sdfTexel), 0) * 2.0f - 1.0f) * maxEncodeDistance;
    float curvatureMagnitude =
        abs(sdfXP + sdfXN - 2.0f * sdf)
      + abs(sdfYP + sdfYN - 2.0f * sdf)
      + abs(sdfZP + sdfZN - 2.0f * sdf);
    float curvatureThreshold = max(voxelSize * 0.5f, probesSpacing * 0.05f);
    float proximityComplexity = saturate(
        1.0f - sdfDist / max(probesSpacing * 1.25f, voxelSize * 2.0f));
    float curvatureComplexity = saturate(curvatureMagnitude / curvatureThreshold);
    bool highGeometryComplexity =
        max(proximityComplexity, curvatureComplexity) > 0.2f;
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
        if (highGeometryComplexity)
            attention = max(attention, kDDGIProbeHighPriorityAttention);

        // Relocate away from surfaces
        if (sdf <= voxelLimit)
        {
            float3 gradient = float3(sdfXP - sdfXN, sdfYP - sdfYN, sdfZP - sdfZN);
            float3 sdfNormal = length(gradient) > 1e-6f ? normalize(gradient) : float3(0,1,0);

            // Probe density is independent of SDF resolution. A coarse SDF
            // cannot provide a reliable full-voxel relocation for a finer
            // probe grid, so cap both clearance and the representable offset.
            float speed = (stateOld != kDDGIProbeStateActive || sdf <= 0.0f) ? 1.0f : 0.3f;
            float relocationClearance = min(voxelLimit, probesSpacing * 0.35f);
            float relocationDistance = max(relocationClearance - sdf, 0.0f);
            float relocateLimit = probesSpacing * 0.45f;
            float cappedRelocationDistance =
                min(relocationDistance * speed, relocateLimit);
            float3 offsetToSet =
                probeOffset + sdfNormal * cappedRelocationDistance;
            float offsetLength = length(offsetToSet);
            if (offsetLength > relocateLimit && offsetLength > 1e-6f)
                offsetToSet *= relocateLimit / offsetLength;

            relocationChanged = length(offsetToSet - probeOffset)
                > probesSpacing * 0.01f;
            probeOffset = offsetToSet;

            // Trilinear sampling of a coarse SDF can remain slightly negative
            // after a valid sub-cell relocation. Reject only probes that are
            // still confidently deep inside geometry.
            float3 relocatedPosition = probeBasePos + probeOffset;
            float3 relocatedUvw =
                clamp((relocatedPosition - boundsMin) / sdfExtent,
                      float3(0,0,0), float3(1,1,1));
            float relocatedEncodedSDF =
                globalSDFTex.SampleLevel(linearSampler, relocatedUvw, 0);
            float relocatedSDF =
                (relocatedEncodedSDF * 2.0f - 1.0f) * maxEncodeDistance;
            float deepInsideTolerance =
                max(probesSpacing * 0.5f, voxelSize * 0.1f);
            if (relocatedSDF < -deepInsideTolerance)
            {
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

    // Mark disjoint deterministic inputs for the compaction pass.
    if (state != kDDGIProbeStateInactive)
    {
        bool priorityProbe = state == kDDGIProbeStateActivated
            || attention >= kDDGIProbeHighPriorityAttention;
        if (priorityProbe)
            activeProbes[priorityBase + probeIndex] = 1u;
        else
            activeProbes[regularBase + probeIndex] = 1u;
    }
}
