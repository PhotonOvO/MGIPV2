#include "shader_io.h"

// Transparent visibility-sort key generation (runs before shader.bitonic_sort.slang).
// Input:  sortKeys[i]   = GPUSceneObject index of the transparent draw (CPU upload)
//         sortValues[i] = draw index (CPU upload, passed through unchanged)
// Output: sortKeys[i]   = 0x80000000u | ((asuint(distanceSquared) >> 2u) & 0x3fffffffu)
//         slots >= activeCount are padding and get 0xffffffffu in key and value, whose
//         category bits (0xc0000000u) match no opaque/alpha/transparent category.
// The CPU mirror of the push constants is GPUDrivenRenderer::VisibilitySortKeygenPushConstants.
struct VisibilitySortKeygenPushConstants
{
    float4 cameraPosition; // xyz = camera world position, w unused
    uint activeCount;
    uint paddedCount;
    uint sceneObjectCount;
    uint _padding0;
};

[[vk::binding(0, 0)]]
RWStructuredBuffer<uint> sortKeys;

[[vk::binding(1, 0)]]
RWStructuredBuffer<uint> sortValues;

[[vk::binding(2, 0)]]
StructuredBuffer<GPUSceneObject> sceneObjects;

[[vk::push_constant]]
VisibilitySortKeygenPushConstants pushConstants;

static const uint kVisibilitySortCategoryTransparent = 0x80000000u;
static const uint kVisibilitySortKeyMask = 0x3fffffffu;
static const uint kVisibilitySortPadding = 0xffffffffu;

[numthreads(64, 1, 1)]
[shader("compute")]
void visibilitySortKeygenMain(uint3 dispatchThreadId : SV_DispatchThreadID)
{
    const uint index = dispatchThreadId.x;
    if(index >= pushConstants.paddedCount)
    {
        return;
    }

    if(index >= pushConstants.activeCount)
    {
        sortKeys[index] = kVisibilitySortPadding;
        sortValues[index] = kVisibilitySortPadding;
        return;
    }

    const uint sceneObjectIndex = sortKeys[index];
    float distanceSquared = 0.0;
    if(sceneObjectIndex < pushConstants.sceneObjectCount)
    {
        // Row-major 3x4 affine transform: the translation (world-space origin of the
        // object, equal to transform[3].xyz) is the w component of each row.
        const float3 objectCenter = float3(sceneObjects[sceneObjectIndex].worldMatrixRows[0].w,
                                           sceneObjects[sceneObjectIndex].worldMatrixRows[1].w,
                                           sceneObjects[sceneObjectIndex].worldMatrixRows[2].w);
        const float3 cameraToObject = objectCenter - pushConstants.cameraPosition.xyz;
        distanceSquared = dot(cameraToObject, cameraToObject);
    }

    // Same encoding as the former CPU path: the bit pattern of a non-negative float is
    // monotonic, so the shifted bits order draws by distance.
    sortKeys[index] = kVisibilitySortCategoryTransparent
        | ((asuint(distanceSquared) >> 2u) & kVisibilitySortKeyMask);
}
