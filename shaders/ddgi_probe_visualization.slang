// DDGI probe visualization debug draw (Wave D3-2).
//
// Instanced procedural UV-sphere per probe (instance id == probe index):
// the vertex shader synthesizes sphere vertices directly from SV_VertexID
// (no vertex/index buffer at all — lowest-risk variant of the plan's
// "CPU-generated icosphere buffer" option, matching the engine's existing
// buffer-less debug draws, cf. shader.debug.slang vertexCullMain), fetches
// the probe world position through the probe-position buffer BDA and scales
// the unit sphere by probeRadius (0.1).
//
// The fragment shader samples the irradiance atlas with the interpolated
// sphere normal via octahedral addressing (LuxGI ProbeVisualization.frag
// approach: textureCoordFromDirection + gamma decode pow(x, gamma*0.5)
// squared), so each sphere shows the directional irradiance the probe would
// contribute along that surface direction.

#include "shader_io.h"
#include "ddgi_sample_probe.slang"

[[vk::binding(0, 0)]]
Sampler2D irradianceAtlas;

[[vk::binding(1, 0)]]
ConstantBuffer<DDGIProbeVisualizationUniforms> probeVisUniforms;

struct ProbeVisVertexOutput
{
    float4 position : SV_Position;
    float3 normalWS : NORMAL0;
    nointerpolation uint probeIndex : TEXCOORD0;
};

// Unit-sphere direction of one UV-sphere triangle-list vertex. Quad corner
// order (two CCW triangles): (0,0) (1,0) (1,1) / (0,0) (1,1) (0,1) — encoded
// as bit masks over the 6 in-quad vertex ids. Pole quads degenerate to zero
// area and rasterize to nothing (harmless for a debug draw).
float3 probeVisSphereDirection(uint vertexId)
{
    const uint quadId = vertexId / 6u;
    const uint corner = vertexId % 6u;
    const int cornerX = int((22u >> corner) & 1u); // ids 1,2,4 -> +1 slice
    const int cornerY = int((52u >> corner) & 1u); // ids 2,4,5 -> +1 stack
    const uint slice = quadId % uint(LDDGIProbeVisSlices);
    const uint stack = quadId / uint(LDDGIProbeVisSlices);
    const float u = float(int(slice) + cornerX) / float(LDDGIProbeVisSlices);
    const float v = float(int(stack) + cornerY) / float(LDDGIProbeVisStacks);
    const float phi = u * 2.0 * kDDGICommonPi;
    const float theta = v * kDDGICommonPi;
    const float sinTheta = sin(theta);
    return float3(sinTheta * cos(phi), cos(theta), sinTheta * sin(phi));
}

[shader("vertex")]
ProbeVisVertexOutput vertexProbeVisMain(uint vertexId : SV_VertexID,
                                        uint instanceId : SV_InstanceID)
{
    ProbeVisVertexOutput output;
    output.probeIndex = instanceId;
    output.normalWS = float3(0.0, 1.0, 0.0);
    output.position = float4(0.0, 0.0, 0.0, 1.0); // degenerate (zero area)
    if (instanceId >= probeVisUniforms.totalProbes)
    {
        return output;
    }

    // Probe world position through the position-buffer BDA (same pattern as
    // ddgi_gi_sdf_rays.slang / ddgi_sample_probe.slang).
    float4* probePositions = (float4*)probeVisUniforms.probePositionAddress;
    const float3 probeCenter = probePositions[instanceId].xyz;

    const float3 dir = probeVisSphereDirection(vertexId);
    const float3 worldPos = probeCenter + dir * probeVisUniforms.probeRadius;
    output.position = mul(probeVisUniforms.viewProjection, float4(worldPos, 1.0));
    output.normalWS = dir;
    return output;
}

[shader("fragment")]
float4 fragmentProbeVisMain(ProbeVisVertexOutput input) : SV_Target
{
    const float3 n = normalize(input.normalWS);
    const int2 atlasSize = int2(int(probeVisUniforms.irradianceAtlasWidth),
                                int(probeVisUniforms.irradianceAtlasHeight));
    const float2 uv = ddgiTextureCoordFromDirection(
        n, int(input.probeIndex), atlasSize, int(probeVisUniforms.irradianceSideLength));

    float3 irradiance = irradianceAtlas.SampleLevel(uv, 0.0).rgb;
    // Gamma decode: pow(x, gamma * 0.5) then square (LuxGI DDGICommon.glsl
    // storage convention, same as the lighting SampleProbe path).
    const float halfGamma = probeVisUniforms.ddgiGamma * 0.5;
    irradiance = pow(irradiance, float3(halfGamma, halfGamma, halfGamma));
    irradiance *= irradiance;
    return float4(irradiance, 1.0);
}
