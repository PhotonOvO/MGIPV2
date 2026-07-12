// Shared DDGI probe relocation/classification.
//
// Runs once after probe resource initialization and before GISDFRays. Probe
// positions are moved away from GlobalSDF surfaces using the same two-stage
// strategy as Flax DDGI: an SDF-gradient step followed by a bounded 4x4x4
// best-position search. Positions that cannot reach safe free space are
// marked inactive in float4.w and ignored by probe sampling.

#include "shader_io.h"
#include "sdf_common.slang"

[[vk::binding(0, 0)]]
Sampler3D<float> globalSDFTexture;

[[vk::binding(1, 0)]]
ConstantBuffer<DDGIRayTraceUniforms> relocateUniforms;

GlobalSDFTraceConfig makeRelocateTraceConfig()
{
    GlobalSDFTraceConfig config;
    config.boundsMin = relocateUniforms.volumeBoundsMin.xyz;
    config.boundsMax = relocateUniforms.volumeBoundsMax.xyz;
    config.voxelSize = relocateUniforms.volumeBoundsMin.w;
    config.maxEncodeDistance = relocateUniforms.volumeBoundsMax.w;
    config.resolution = float(relocateUniforms.resolution);
    config.stepScale = relocateUniforms.sunColor.w;
    config.maxSteps = relocateUniforms.maxSteps;
    return config;
}

float3 clampProbePosition(GlobalSDFTraceConfig config, float3 position)
{
    const float margin = config.voxelSize;
    return clamp(position, config.boundsMin + margin, config.boundsMax - margin);
}

[shader("compute")]
[numthreads(64, 1, 1)]
void kernelDDGIProbeRelocate(uint3 dispatchThreadId : SV_DispatchThreadID)
{
    const uint probeIndex = dispatchThreadId.x;
    if (probeIndex >= relocateUniforms.totalProbes)
    {
        return;
    }

    const uint3 probeCounts = uint3(relocateUniforms.ddgiGridDims.xyz);
    const uint probesPerPlane = probeCounts.x * probeCounts.y;
    const uint3 probeCoords = uint3(
        probeIndex % probeCounts.x,
        (probeIndex / probeCounts.x) % probeCounts.y,
        probeIndex / probesPerPlane);
    const float probeSpacing = relocateUniforms.ddgiGridOriginAndSpacing.w;
    const float3 basePosition = relocateUniforms.ddgiGridOriginAndSpacing.xyz
        + float3(probeCoords) * probeSpacing;

    const GlobalSDFTraceConfig config = makeRelocateTraceConfig();
    const float clearance = config.voxelSize * 1.5;
    const float relocateLimit = probeSpacing * 0.5;
    float3 bestPosition = clampProbePosition(config, basePosition);
    float bestDistance = sampleGlobalSDFWorldDistance(globalSDFTexture, config, bestPosition);

    if (bestDistance <= clearance)
    {
        const float3 sdfNormal =
            computeGlobalSDFGradientNormal(globalSDFTexture, config, bestPosition);
        const float moveDistance = clamp(clearance - bestDistance + config.voxelSize * 0.25,
                                         0.0, relocateLimit);
        const float3 gradientPosition =
            clampProbePosition(config, bestPosition + sdfNormal * moveDistance);
        const float gradientDistance =
            sampleGlobalSDFWorldDistance(globalSDFTexture, config, gradientPosition);
        if (gradientDistance > bestDistance)
        {
            bestDistance = gradientDistance;
            bestPosition = gradientPosition;
        }

        if (bestDistance <= clearance)
        {
            // Mirrors Flax's bounded 4x4x4 best-position search. 0.707 keeps
            // corner candidates near the configured relocation radius.
            for (uint z = 0u; z < 4u; ++z)
            {
                for (uint y = 0u; y < 4u; ++y)
                {
                    for (uint x = 0u; x < 4u; ++x)
                    {
                        const float3 searchCoord = float3(x, y, z) * (2.0 / 3.0) - 1.0;
                        const float3 candidate = clampProbePosition(
                            config, basePosition + searchCoord * (relocateLimit * 0.707));
                        const float candidateDistance =
                            sampleGlobalSDFWorldDistance(globalSDFTexture, config, candidate);
                        if (candidateDistance > bestDistance)
                        {
                            bestDistance = candidateDistance;
                            bestPosition = candidate;
                        }
                    }
                }
            }
        }
    }

    float4* probePositions = (float4*)relocateUniforms.probePositionAddress;
    const float active = bestDistance > clearance ? 1.0 : 0.0;
    probePositions[probeIndex] = float4(active > 0.5 ? bestPosition : basePosition, active);
}
