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

// Global SDF (same as existing VKDemo SDF)
[[vk::binding(3, 0)]] Sampler3D<float> globalSDFTex       : register(t1);

// Surface Atlas resources (for hit shading)
[[vk::binding(4, 0)]] SamplerState linearSampler            : register(s0);
[[vk::binding(5, 0)]] Texture2D<float> atlasDepth           : register(t2);
[[vk::binding(6, 0)]] Texture2D<float4> atlasLighting       : register(t3);
[[vk::binding(7, 0)]] RWStructuredBuffer<uint> chunks       : register(u2);
[[vk::binding(8, 0)]] RWStructuredBuffer<uint> culledObjs   : register(u3);
[[vk::binding(9, 0)]] RWStructuredBuffer<float4> objects    : register(u4);

[[vk::push_constant]] ConstantBuffer<FlaxDDGIData> ddgi;

static const float kPI = 3.14159265359f;
static const uint kBatchSize = 256;

// Spherical Fibonacci direction
float3 sphericalFibonacci(float i, float n)
{
    float phiGolden = sqrt(5.0f) * 0.5f + 0.5f;
    float fraction = (i * (phiGolden - 1.0f)) - floor(i * (phiGolden - 1.0f));
    float phi = 2.0f * kPI * fraction;
    float cosTheta = 1.0f - (2.0f * i + 1.0f) / n;
    float sinTheta = sqrt(clamp(1.0f - cosTheta * cosTheta, 0.0f, 1.0f));
    return float3(cos(phi) * sinTheta, sin(phi) * sinTheta, cosTheta);
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

    // GlobalSDF config (should match GlobalSDFPass)
    GlobalSDFTraceConfig sdfConfig;
    sdfConfig.boundsMin = float3(-16,-16,-16);
    sdfConfig.boundsMax = float3(16,16,16);
    sdfConfig.voxelSize = 32.0f / 128.0f;
    sdfConfig.maxEncodeDistance = 32.0f;
    sdfConfig.resolution = 128.0f;
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
        float3 rayDir = sphericalFibonacci(float(r), float(raysPerProbe));
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
            float eps = 1.0f / sdfConfig.resolution;
            float xp = globalSDFTex.SampleLevel(uvw + float3(eps,0,0), 0);
            float xn = globalSDFTex.SampleLevel(uvw - float3(eps,0,0), 0);
            float yp = globalSDFTex.SampleLevel(uvw + float3(0,eps,0), 0);
            float yn = globalSDFTex.SampleLevel(uvw - float3(0,eps,0), 0);
            float zp = globalSDFTex.SampleLevel(uvw + float3(0,0,eps), 0);
            float zn = globalSDFTex.SampleLevel(uvw - float3(0,0,eps), 0);
            float3 gradient = float3(xp - xn, yp - yn, zp - zn);
            float3 hitNormal = length(gradient) > 1e-6f ? normalize(gradient) : float3(0,1,0);

            // Try Surface Atlas sampling for colored hit
            float4 atlasSample = sampleGlobalSurfaceAtlas(
                atlasData, chunks, culledObjs, objects,
                atlasDepth, atlasLighting, linearSampler,
                hitPos, hitNormal, 20.0f);

            if (atlasSample.a > 0.01f)
                radiance = atlasSample.rgb;
            else
                radiance = float3(0.1f, 0.15f, 0.2f); // sky fallback
        }
        else
        {
            // Miss — sample sky/environment
            radiance = float3(0.1f, 0.15f, 0.2f);
        }

        // Write to trace buffer (column = ray, row = batch probe)
        probesTrace[uint2(r, batchIndex)] = float4(radiance, hit.hitTime);
    }
}
