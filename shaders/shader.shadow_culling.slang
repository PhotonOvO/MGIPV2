#include "shader_io.h"

[[vk::binding(0, 0)]]
StructuredBuffer<ShadowCullObject> shadowCullObjects;

[[vk::binding(1, 0)]]
RWStructuredBuffer<GPUCullIndirectCommand> shadowIndirectCommands;

[[vk::push_constant]]
ConstantBuffer<ShadowCullPushConstants> shadowCullPushConstants;

bool isInsideShadowFrustum(float3 center, float radius)
{
    for(uint planeIndex = 0; planeIndex < uint(LGPUCullingFrustumPlaneCount); ++planeIndex)
    {
        const float4 plane = shadowCullPushConstants.frustumPlanes[planeIndex];
        const float distanceToPlane = dot(plane.xyz, center) + plane.w;
        if(distanceToPlane < -radius)
        {
            return false;
        }
    }
    return true;
}

[numthreads(LGPUCullingThreadCount, 1, 1)]
[shader("compute")]
void shadowCullingMain(uint3 dispatchThreadId : SV_DispatchThreadID)
{
    const uint objectIndex = dispatchThreadId.x;
    if(objectIndex >= shadowCullPushConstants.objectCount)
    {
        return;
    }

    const ShadowCullObject objectData = shadowCullObjects[objectIndex];
    GPUCullIndirectCommand command;
    command.indexCount = objectData.indexCount;
    command.instanceCount = 0u;
    command.firstIndex = objectData.firstIndex;
    command.vertexOffset = objectData.vertexOffset;
    command.firstInstance = objectData.firstInstance;

    if(isInsideShadowFrustum(objectData.sphereCenterRadius.xyz, max(objectData.sphereCenterRadius.w, 0.0)))
    {
        command.instanceCount = 1u;
    }

    shadowIndirectCommands[objectIndex] = command;
}
