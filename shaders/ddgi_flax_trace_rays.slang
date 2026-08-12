// ddgi_flax_trace_rays.slang — Flax-style DDGI probe ray tracing (FGI-053)
// Traces spherical Fibonacci rays through Global SDF, samples Surface Atlas
// lighting on hit, writes radiance to probe trace buffer. Ported from Flax.

#include "shader_io.h"
#include "flax_ddgi_common.slang"
#include "sdf_common.slang"
#include "surface_atlas_common.slang"

[[vk::binding(0, 0)]] Texture2D<float4> probesData        DEMO_REGISTER(t0);
[[vk::binding(1, 0)]] RWStructuredBuffer<uint> activeProbes DEMO_REGISTER(u0);
[[vk::binding(2, 0)]] RWTexture2D<float4> probesTrace      DEMO_REGISTER(u1); // rays x selected update slot

// Keep these declarations in logical binding order. The Metal target has no
// register annotations, so Slang assigns direct Metal resource indices from
// declaration order even when an unused argument is removed from the entry point.
[[vk::binding(3, 0)]] Sampler3D<float> globalSDFTex            DEMO_REGISTER(t1);

// Surface Atlas resources (for hit shading)
[[vk::binding(4, 0)]] SamplerState linearSampler              DEMO_REGISTER(s0);
[[vk::binding(5, 0)]] Texture2D<float> atlasDepth             DEMO_REGISTER(t2);
[[vk::binding(6, 0)]] Texture2D<float4> atlasLighting         DEMO_REGISTER(t3);
[[vk::binding(7, 0)]] RWStructuredBuffer<uint> chunks         DEMO_REGISTER(u2);
[[vk::binding(8, 0)]] RWStructuredBuffer<uint> culledObjs     DEMO_REGISTER(u3);
[[vk::binding(9, 0)]] RWStructuredBuffer<float4> objects      DEMO_REGISTER(u4);
[[vk::binding(10, 0)]] StructuredBuffer<FlaxDDGIData> ddgiData DEMO_REGISTER(t4);

// Global SDF albedo, environment, and probe history
[[vk::binding(11, 0)]] Texture3D<float4> globalAlbedoTex        DEMO_REGISTER(t5);
[[vk::binding(12, 0)]] Texture2D<float4> environmentTex         DEMO_REGISTER(t6);
#ifdef DEMO_METAL
[[vk::binding(13, 0)]] Texture2D<float4> probesDataHistoryTexture;
[[vk::binding(13, 0)]] SamplerState probesDataHistorySampler;
[[vk::binding(14, 0)]] Texture2D<float4> probesDistanceHistoryTexture;
[[vk::binding(14, 0)]] SamplerState probesDistanceHistorySampler;
[[vk::binding(15, 0)]] Texture2D<float4> probesIrradianceHistoryTexture;
[[vk::binding(15, 0)]] SamplerState probesIrradianceHistorySampler;
DemoMetalSampler2D getProbesDataHistory()
{
    return makeDemoMetalSampler2D(probesDataHistoryTexture, probesDataHistorySampler);
}
DemoMetalSampler2D getProbesDistanceHistory()
{
    return makeDemoMetalSampler2D(
        probesDistanceHistoryTexture, probesDistanceHistorySampler);
}
DemoMetalSampler2D getProbesIrradianceHistory()
{
    return makeDemoMetalSampler2D(
        probesIrradianceHistoryTexture, probesIrradianceHistorySampler);
}
#else
[[vk::binding(13, 0)]] Sampler2D<float4> probesDataHistory       DEMO_REGISTER(t7);
[[vk::binding(14, 0)]] Sampler2D<float4> probesDistanceHistory   DEMO_REGISTER(t8);
[[vk::binding(15, 0)]] Sampler2D<float4> probesIrradianceHistory DEMO_REGISTER(t9);
#define getProbesDataHistory() probesDataHistory
#define getProbesDistanceHistory() probesDistanceHistory
#define getProbesIrradianceHistory() probesIrradianceHistory
#endif

static FlaxDDGIData ddgi = ddgiData[0];
struct FlaxDDGICascadePush
{
    uint cascadeIndex;
};
[[vk::push_constant]] ConstantBuffer<FlaxDDGICascadePush> cascadePush;

static const float kFlaxDDGITwoPi = 6.28318530718f;
static const float kFlaxDDGIMultiBounceFeedback = 0.5f;




// One workgroup owns one selected probe. Keeping the group to two warps limits
// duplicated probe setup while providing enough independent probe groups to
// occupy the GPU; lanes cover all configured rays with a fixed stride.
static const uint kRayGroupSize = 64;
// Surface Atlas raster/lighting is not complete yet. Keep the bindings stable,
// but make the textured GlobalSDF hit path authoritative until the atlas
// contains real draws and lighting data.
static const bool kUseSurfaceAtlas = false;

float3 loadGlobalAlbedoNearest(float3 uvw)
{
    uint width, height, depth;
    globalAlbedoTex.GetDimensions(width, height, depth);
    const int3 dimensions = max(int3(width, height, depth), int3(1, 1, 1));
    const float3 clampedUvw = clamp(uvw, float3(0, 0, 0), float3(1, 1, 1));
    // Keep the conversion signed until after floor. In particular uvw == 1
    // first maps to dimensions and is then clamped to dimensions - 1.
    int3 texel = int3(floor(clampedUvw * float3(dimensions)));
    texel = clamp(texel, int3(0, 0, 0), dimensions - int3(1, 1, 1));
    return max(globalAlbedoTex.Load(int4(texel, 0)).rgb, float3(0, 0, 0));
}

float2 flaxDirectionToEquirectUV(float3 direction)
{
    float3 d = normalize(direction);
    float u = atan2(d.z, d.x) / kFlaxDDGITwoPi + 0.5f;
    float v = asin(clamp(d.y, -1.0f, 1.0f)) / kFlaxDDGIPi + 0.5f;
    return float2(u, 1.0f - v);
}

bool flaxSDFVisible(GlobalSDFTraceConfig sdfConfig, float3 position, float3 normal,
                    float3 lightDirection, float maximumDistance, float bias)
{
    float safeBias = max(bias, sdfConfig.voxelSize * 0.5f);
    float3 shadowOrigin = position + normal * safeBias + lightDirection * safeBias;
    GlobalSDFTraceHit shadowHit = traceGlobalSDF(
        globalSDFTex, sdfConfig, shadowOrigin, lightDirection, maximumDistance);
    return !isGlobalSDFHit(shadowHit) || shadowHit.hitTime >= maximumDistance - safeBias;
}

float3 evaluateFlaxRadianceSources(float3 hitPosition, float3 hitNormal,
                                   GlobalSDFTraceConfig sdfConfig, float shadowBias)
{
    float3 incomingRadiance = float3(0, 0, 0);
    uint sourceCount = min(ddgi.radianceSourceParams.x,
                           uint(LFlaxDDGIMaxRadianceSources));
    for (uint sourceIndex = 0; sourceIndex < sourceCount; ++sourceIndex)
    {
        FlaxDDGIRadianceSource source = ddgi.radianceSources[sourceIndex];
        uint sourceType = uint(source.colorAndType.w + 0.5f);

        if (sourceType == LFlaxRadianceSourceEmissive)
        {
            float3 centerDirection = source.positionAndRange.xyz - hitPosition;
            float centerDistance = length(centerDirection);
            centerDirection /= max(centerDistance, 0.001f);
            if (centerDistance >= source.positionAndRange.w
                || !flaxSDFVisible(sdfConfig, hitPosition, hitNormal, centerDirection,
                                   max(centerDistance - shadowBias, 0.0f), shadowBias))
                continue;

            for (uint sampleIndex = 0u; sampleIndex < kFlaxEmissiveSampleCount; ++sampleIndex)
            {
                float3 lightDirection;
                float distanceToLight;
                float3 sampleRadiance = flaxEmissiveSampleRadiance(
                    source, hitPosition, sampleIndex, lightDirection, distanceToLight);
                incomingRadiance += sampleRadiance * saturate(dot(hitNormal, lightDirection));
            }
            continue;
        }

        float3 toLight = source.positionAndRange.xyz - hitPosition;
        float distanceToLight = length(toLight);
        float sourceRange = max(source.positionAndRange.w, 0.001f);
        if (distanceToLight >= sourceRange)
            continue;

        float3 lightDirection = toLight / max(distanceToLight, 0.001f);
        float attenuation = flaxWindowedInverseSquare(distanceToLight, sourceRange);

        if (sourceType == LFlaxRadianceSourceSpot)
        {
            float coneCos = dot(-lightDirection, normalize(source.directionAndCone.xyz));
            float outerCos = source.directionAndCone.w;
            attenuation *= smoothstep(outerCos, min(outerCos + 0.08f, 1.0f), coneCos);
        }

        float nDotL = saturate(dot(hitNormal, lightDirection));
        if (nDotL <= 0.0f || !flaxSDFVisible(
                sdfConfig, hitPosition, hitNormal, lightDirection,
                max(distanceToLight - shadowBias, 0.0f), shadowBias))
            continue;
        incomingRadiance += source.colorAndType.rgb * (attenuation * nDotL);
    }
    return incomingRadiance;
}

[numthreads(kRayGroupSize, 1, 1)]
void CS_TraceRays(uint3 groupId : SV_GroupID,
                  uint3 groupThreadId : SV_GroupThreadID)
{
    uint updateSlot = groupId.x;
    uint cascadeIndex = min(cascadePush.cascadeIndex, ddgi.probesCounts.w - 1u);
    uint probeIndex;
    uint requiredState;
    if (!getProbeUpdateSelection(
            ddgi, activeProbes, cascadeIndex, updateSlot, probeIndex, requiredState)) return;
    uint3 probeCoords = getProbeCoords(ddgi, probeIndex);
    uint scrolledIdx = getScrollingProbeIndex(ddgi, cascadeIndex, probeCoords);
    uint2 dataCoord = getProbeTexelCoords(ddgi, cascadeIndex, scrolledIdx);
    float3 probeWorldPos = getProbeWorldPosition(ddgi, cascadeIndex, probeCoords);

    // Read probe data for relocation offset
    float4 pd = probesData.Load(int3(dataCoord, 0));
    if (!probeMatchesUpdateState(pd, requiredState))
        return;
    float3 probePos = probeWorldPos + pd.xyz * ddgi.probesOriginAndSpacing[cascadeIndex].w;

	// GlobalSDF config — matches GlobalSDFPass defaults (128^3, [-16,16])
	// Using hardcoded values to match GlobalSDFPass::kDefaultHalfExtent = 16.0f
	GlobalSDFTraceConfig sdfConfig;
	sdfConfig.boundsMin = ddgi.sdfBoundsMinAndVoxel.xyz;
	sdfConfig.boundsMax = ddgi.sdfBoundsMaxAndRes.xyz;
	sdfConfig.voxelSize = ddgi.sdfBoundsMinAndVoxel.w;
	float3 sdfExtent = max(sdfConfig.boundsMax - sdfConfig.boundsMin,
	                       float3(1e-4f, 1e-4f, 1e-4f));
	sdfConfig.maxEncodeDistance = max(max(sdfExtent.x, sdfExtent.y), sdfExtent.z);
	sdfConfig.resolution = ddgi.sdfBoundsMaxAndRes.w;
	sdfConfig.stepScale = 1.0f;
	sdfConfig.maxSteps = 128;

    GlobalSurfaceAtlasData atlasData;
    atlasData.viewPos = ddgi.viewPos.xyz;
    atlasData.resolution = 2048.0f;
    atlasData.chunkSize = atlasData.resolution / 40.0f;
    atlasData.objectsCount = 0;

    const float distanceLimit = flaxProbeDistanceLimit(ddgi, cascadeIndex);
    const float rayStartOffset = flaxProbeRayStartOffset(ddgi, cascadeIndex);
    const float selfHitThreshold = flaxProbeSelfHitThreshold(ddgi);
    const float3 probeUvw = clamp(
        (probePos - sdfConfig.boundsMin) / (sdfConfig.boundsMax - sdfConfig.boundsMin),
        float3(0,0,0), float3(1,1,1));
    const float probeClearance =
        sampleGlobalSDFFineDistance(globalSDFTex, sdfConfig, probeUvw);

    // Ray tracing loop. Each lane owns a disjoint ray sequence, so the trace
    // atlas writes require no group-shared reduction or synchronization.
    uint raysPerProbe = uint(ddgi.viewPos.w);
    for (uint r = groupThreadId.x; r < raysPerProbe; r += kRayGroupSize)
    {
        float3 rayDir = flaxSphericalFibonacci(float(r), float(raysPerProbe));
        float3 origin = probePos + rayDir * rayStartOffset;

        GlobalSDFTraceHit hit = traceGlobalSDF(globalSDFTex, sdfConfig,
                                                origin, rayDir, ddgi.rayMaxDistance);

        // A miss represents known free space up to the finite moment horizon.
        // It also contributes sky radiance to the irradiance convolution.
        float storedDistance = distanceLimit;
        float3 radiance = ddgi.environmentParams.x > 0.5f
            ? max(environmentTex.SampleLevel(
                linearSampler, flaxDirectionToEquirectUV(rayDir), 0.0f).rgb, float3(0,0,0))
                * ddgi.environmentParams.y : ddgi.fallbackIrradiance.rgb / kFlaxDDGIPi;

        if (isGlobalSDFHit(hit))
        {
            float3 hitPos = origin + rayDir * hit.hitTime;
            float3 uvw = clamp(
                (hitPos - sdfConfig.boundsMin) / (sdfConfig.boundsMax - sdfConfig.boundsMin),
                float3(0,0,0), float3(1,1,1));
            float3 hitNormal =
                computeGlobalSDFGradientNormal(globalSDFTex, sdfConfig, hitPos);
            bool nearHit = hit.hitTime <= selfHitThreshold;
            bool frontFaceHit = dot(hitNormal, rayDir) < -0.05f;
            // Relocation should keep probes outside geometry. If a coarse SDF
            // still produces an immediate back-facing/tangent hit, treat that
            // sample as numerical self-intersection, not as a zero-distance
            // occluder. A near front-face hit remains a real nearby wall.
            bool probeInside = probeClearance < -0.5f * selfHitThreshold;
            bool unreliableNearHit = nearHit && !frontFaceHit;
            bool reliableHit = !probeInside && !unreliableNearHit;

            if (reliableHit)
            {
                // hitTime is measured from the biased origin. Add that offset
                // back so moment distances remain measured from the probe.
                storedDistance = min(rayStartOffset + hit.hitTime,
                                     ddgi.rayMaxDistance);

                bool emissiveHit = false;
                uint sourceCount = min(ddgi.radianceSourceParams.x,
                                       uint(LFlaxDDGIMaxRadianceSources));
                float hitTolerance = max(sdfConfig.voxelSize * 1.5f, 0.02f);
                for (uint sourceIndex = 0u; sourceIndex < sourceCount; ++sourceIndex)
                {
                    FlaxDDGIRadianceSource source = ddgi.radianceSources[sourceIndex];
                    if (flaxPointOnEmissiveSource(source, hitPos, hitTolerance))
                    {
                        radiance = source.colorAndType.rgb;
                        emissiveHit = true;
                        break;
                    }
                }

                float4 atlasSample = float4(0, 0, 0, 0);
                if (kUseSurfaceAtlas)
                {
                    atlasSample = sampleGlobalSurfaceAtlas(
                        atlasData, chunks, culledObjs, objects,
                        atlasDepth, atlasLighting, linearSampler,
                        hitPos, hitNormal, 20.0f);
                }

                if (emissiveHit)
                {
                    // Direct emissive radiance is already in outgoing-radiance units.
                }
                else if (atlasSample.a > 0.01f)
                {
                    radiance = atlasSample.rgb;
                }
                else
                {
                    // Textured SDF fallback with explicit SDF visibility.
                    float3 albedoSample = loadGlobalAlbedoNearest(uvw);
                    float3 sunDir = normalize(ddgi.sceneLightDirection.xyz);
                    float3 sunColor = ddgi.sceneLightColor.rgb;
                    float NdotL = saturate(dot(hitNormal, sunDir));
                    float shadowBias = max(selfHitThreshold * 1.5f, rayStartOffset);
                    float shadowMaxDistance = length(sdfConfig.boundsMax - sdfConfig.boundsMin);
                    float sunVisibility =
                        NdotL > 0.0f && flaxSDFVisible(
                            sdfConfig, hitPos, hitNormal, sunDir, shadowMaxDistance, shadowBias)
                        ? 1.0f : 0.0f;
                    float3 directRadiance = sunColor * (NdotL * sunVisibility);
                    directRadiance +=
                        evaluateFlaxRadianceSources(hitPos, hitNormal, sdfConfig, shadowBias);
                    float3 bouncedIrradiance = sampleFlaxDDGIIrradiance(
                        ddgi, getProbesDataHistory(), getProbesDistanceHistory(),
                        getProbesIrradianceHistory(), hitPos, hitNormal, -rayDir,
                        max(selfHitThreshold, rayStartOffset), 0.0f,
                        max(ddgi.probesOriginAndSpacing[0].w * 2.0f, 0.001f));
                    directRadiance += bouncedIrradiance * kFlaxDDGIMultiBounceFeedback;
                    radiance = albedoSample * directRadiance
                             * (1.0f / kFlaxDDGIPi);
                }
            }
	    }

        // Write to trace buffer (column = ray, row = selected update slot).
        probesTrace[uint2(r, updateSlot)] =
            float4(radiance, storedDistance);
    }
}
