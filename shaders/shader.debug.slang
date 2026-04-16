// Debug pass shader: overlays line primitives in world space

#include "shader_io.h"

struct VertexInput
{
    [[vk::location(LVDebugPosition)]] float3 position : POSITION;
    [[vk::location(LVDebugColor)]]    float4 color    : COLOR0;
};

struct VertexOutput
{
    float4 position : SV_Position;
    float4 color    : COLOR0;
};

[[vk::binding(LBindCamera, LSetScene)]]
ConstantBuffer<CameraUniforms> camera;

[[vk::push_constant]]
ConstantBuffer<PushConstantGPUCullDebug> gpuCullDebugPushConst;

static const uint kGPUCullDebugRingCount = 3u;
static const float kGPUCullDebugTau = 6.28318530718;

float4 getGPUCullDebugColor(uint result, uint flags)
{
    if(result == LGPUCullResultFrustumCulled)
    {
        return float4(1.0, 0.36, 0.36, 0.90);
    }
    if(result == LGPUCullResultOcclusionCulled)
    {
        return float4(1.0, 0.69, 0.36, 0.92);
    }
    if((flags & LGPUCullFlagTransparent) != 0u)
    {
        return float4(0.36, 0.82, 1.0, 0.78);
    }
    return float4(0.36, 0.86, 0.47, 0.72);
}

float3 getGPUCullDebugUnitOffset(uint ringIndex, float angle)
{
    const float c = cos(angle);
    const float s = sin(angle);
    switch(ringIndex)
    {
        case 0u:
            return float3(c, s, 0.0);
        case 1u:
            return float3(0.0, c, s);
        default:
            return float3(c, 0.0, s);
    }
}

[shader("vertex")]
VertexOutput vertexMain(VertexInput input)
{
    VertexOutput output;
    output.position = mul(camera.viewProjection, float4(input.position, 1.0));
    output.color = input.color;
    return output;
}

[shader("vertex")]
VertexOutput vertexCullMain(uint vertexIndex : SV_VertexID, uint instanceIndex : SV_InstanceID)
{
    VertexOutput output;
    output.position = float4(0.0, 0.0, 0.0, 1.0);
    output.color = float4(0.0, 0.0, 0.0, 0.0);

    if(instanceIndex >= gpuCullDebugPushConst.objectCount)
    {
        return output;
    }

    GPUCullObject* objects = (GPUCullObject*)gpuCullDebugPushConst.objectBufferAddress;
    uint* results = (uint*)gpuCullDebugPushConst.resultBufferAddress;

    const GPUCullObject objectData = objects[instanceIndex];
    const float radius = max(objectData.sphereCenterRadius.w, 0.0);
    if(radius <= 1e-5)
    {
        return output;
    }

    const uint segmentCount = max(gpuCullDebugPushConst.segmentCount, 3u);
    const uint verticesPerRing = segmentCount * 2u;
    const uint ringIndex = min(vertexIndex / verticesPerRing, kGPUCullDebugRingCount - 1u);
    const uint localVertexIndex = vertexIndex % verticesPerRing;
    const uint segmentIndex = localVertexIndex / 2u;
    const uint endpointIndex = localVertexIndex & 1u;
    const float angle = (float(segmentIndex + endpointIndex) / float(segmentCount)) * kGPUCullDebugTau;

    const float3 center = objectData.sphereCenterRadius.xyz;
    const float3 offset = getGPUCullDebugUnitOffset(ringIndex, angle) * radius;
    output.position = mul(camera.viewProjection, float4(center + offset, 1.0));
    output.color = getGPUCullDebugColor(results[instanceIndex], objectData.flags);
    return output;
}

[shader("fragment")]
float4 fragmentMain(VertexOutput input) : SV_Target
{
    return input.color;
}
