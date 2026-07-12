// ddgi_flax_trace_rays.slang — Flax-style DDGI probe ray tracing (FGI-053)
// Traces spherical Fibonacci rays through Global SDF, samples Surface Atlas
// lighting on hit, writes radiance to probe trace buffer. Ported from Flax.

#include "shader_io.h"
#include "flax_ddgi_common.slang"
#include "sdf_common.slang"
#include "surface_atlas_common.slang"

[[vk::binding(0, 0)]] Texture2D<float4> probesData        : register(t0);
[[vk::binding(1, 0)]] RWStructuredBuffer<uint> activeProbes : register(u0);
[[vk::binding(2, 0)]] RWTexture2D<float4> probesTrace      : register(u1); // rays x batch

// Global SDF + albedo
[[vk::binding(3, 0)]] Sampler3D<float> globalSDFTex            : register(t1);
[[vk::binding(11, 0)]] Texture3D<float4> globalAlbedoTex        : register(t5);

// Surface Atlas resources (for hit shading)
[[vk::binding(4, 0)]] SamplerState linearSampler              : register(s0);
[[vk::binding(5, 0)]] Texture2D<float> atlasDepth             : register(t2);
[[vk::binding(6, 0)]] Texture2D<float4> atlasLighting         : register(t3);
[[vk::binding(7, 0)]] RWStructuredBuffer<uint> chunks         : register(u2);
[[vk::binding(8, 0)]] RWStructuredBuffer<uint> culledObjs     : register(u3);
[[vk::binding(9, 0)]] RWStructuredBuffer<float4> objects      : register(u4);
[[vk::binding(10, 0)]] StructuredBuffer<FlaxDDGIData> ddgiData : register(t4);
static FlaxDDGIData ddgi = ddgiData[0];

static const uint kBatchSize = 256;
// Surface Atlas raster/lighting is not complete yet. Keep the bindings stable,
// but make the textured GlobalSDF hit path authoritative until the atlas
// contains real draws and lighting data.
static const bool kUseSurfaceAtlas = false;

float3 evaluateFlaxRadianceSources(float3 hitPosition, float3 hitNormal)
{
    float3 incomingRadiance = float3(0, 0, 0);
    uint sourceCount = min(ddgi.radianceSourceParams.x,
                           uint(LFlaxDDGIMaxRadianceSources));
    for (uint sourceIndex = 0; sourceIndex < sourceCount; ++sourceIndex)
    {
        FlaxDDGIRadianceSource source = ddgi.radianceSources[sourceIndex];
        float3 toLight = source.positionAndRange.xyz - hitPosition;
        float distanceToLight = length(toLight);
        float sourceRange = max(source.positionAndRange.w, 0.001f);
        if (distanceToLight >= sourceRange)
            continue;

        float3 lightDirection = toLight / max(distanceToLight, 0.001f);
        float attenuation = saturate(1.0f - distanceToLight / sourceRange);
        attenuation *= attenuation;

        uint sourceType = uint(source.colorAndType.w + 0.5f);
        if (sourceType == LFlaxRadianceSourceSpot)
        {
            float coneCos = dot(-lightDirection, normalize(source.directionAndCone.xyz));
            float outerCos = source.directionAndCone.w;
            attenuation *= smoothstep(outerCos, min(outerCos + 0.08f, 1.0f), coneCos);
        }

        float nDotL = saturate(dot(hitNormal, lightDirection));
        incomingRadiance += source.colorAndType.rgb * (attenuation * nDotL);
    }
    return incomingRadiance;
}

[numthreads(kBatchSize, 1, 1)]
void CS_TraceRays(uint3 dispatchThreadId : SV_DispatchThreadID)
{
    uint batchIndex = dispatchThreadId.x;
    uint activeCount = activeProbes[0];
    if (batchIndex >= activeCount) return;

    uint probeIndex = activeProbes[1 + batchIndex];
    uint3 probeCoords = getProbeCoords(ddgi, probeIndex);
    uint cascadeIndex = 0; // TODO: parameterize from dispatch
    uint scrolledIdx = getScrollingProbeIndex(ddgi, cascadeIndex, probeCoords);
    uint2 dataCoord = getProbeTexelCoords(ddgi, cascadeIndex, scrolledIdx);
    float3 probeWorldPos = getProbeWorldPosition(ddgi, cascadeIndex, probeCoords);

    // Read probe data for relocation offset
    float4 pd = probesData.Load(int3(dataCoord, 0));
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
	sdfConfig.maxSteps = 64;

    GlobalSurfaceAtlasData atlasData;
    atlasData.viewPos = ddgi.viewPos;
    atlasData.resolution = 2048.0f;
    atlasData.chunkSize = atlasData.resolution / 40.0f;
    atlasData.objectsCount = 0;

    // Ray tracing loop
    uint raysPerProbe = ddgi.raysCount;
    for (uint r = 0; r < raysPerProbe; r++)
    {
        float3 rayDir = flaxSphericalFibonacci(float(r), float(raysPerProbe));
        float3 origin = probePos + rayDir * 0.1f; // offset from probe center

        GlobalSDFTraceHit hit = traceGlobalSDF(globalSDFTex, sdfConfig,
                                                origin, rayDir, ddgi.rayMaxDistance);

        float3 radiance = float3(0,0,0);

        if (isGlobalSDFHit(hit))
        {
            float3 hitPos = origin + rayDir * hit.hitTime;
            // Compute SDF normal at hit point
            float3 uvw = clamp(
                (hitPos - sdfConfig.boundsMin) / (sdfConfig.boundsMax - sdfConfig.boundsMin),
                float3(0,0,0), float3(1,1,1));
            float3 grad = float3(0,0,1);
            float eps = 1.0f / max(sdfConfig.resolution, 1.0f);
            float xp = globalSDFTex.SampleLevel(uvw + float3(eps,0,0), 0);
            float xn = globalSDFTex.SampleLevel(uvw - float3(eps,0,0), 0);
            float yp = globalSDFTex.SampleLevel(uvw + float3(0,eps,0), 0);
            float yn = globalSDFTex.SampleLevel(uvw - float3(0,eps,0), 0);
            float zp = globalSDFTex.SampleLevel(uvw + float3(0,0,eps), 0);
            float zn = globalSDFTex.SampleLevel(uvw - float3(0,0,eps), 0);
            float3 gradient = float3(xp - xn, yp - yn, zp - zn);
            float3 hitNormal = length(gradient) > 1e-6f ? normalize(gradient) : float3(0,1,0);

	        // Try Surface Atlas sampling for colored hit
	        float4 atlasSample = float4(0, 0, 0, 0);
	        if (kUseSurfaceAtlas)
	        {
	            atlasSample = sampleGlobalSurfaceAtlas(
	                atlasData, chunks, culledObjs, objects,
	                atlasDepth, atlasLighting, linearSampler,
	                hitPos, hitNormal, 20.0f);
	        }

	        if (atlasSample.a > 0.01f)
	        {
	            radiance = atlasSample.rgb;
	        }
	        else
	        {
	            // R5 fallback: sample SDF albedo + scene directional light
	            float3 albedoSample = globalAlbedoTex.SampleLevel(linearSampler, uvw, 0).rgb;
	            float3 sunDir = normalize(ddgi.sceneLightDirection.xyz);
	            float3 sunColor = ddgi.sceneLightColor.rgb;
	            float NdotL = saturate(dot(hitNormal, sunDir));
	            float3 directRadiance = sunColor * NdotL;
	            directRadiance += evaluateFlaxRadianceSources(hitPos, hitNormal);
	            // Lambert: reflected radiance uses energy-conserving 1/PI.
	            radiance = albedoSample * directRadiance * (1.0f / kFlaxDDGIPi);
	            // Small ambient so dark faces get minimal colored bounce
	            radiance += albedoSample * 0.04f;
	        }
	    }
	    else
	    {
	        // Miss — very dim blue sky
	        radiance = float3(0.01f, 0.015f, 0.03f);
	    }

        // Write to trace buffer (column = ray, row = batch probe)
        probesTrace[uint2(r, batchIndex)] = float4(radiance, hit.hitTime);
    }
}
