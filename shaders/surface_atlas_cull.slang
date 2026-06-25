// surface_atlas_cull.slang — Global Surface Atlas chunk culling (FGI-033)
// Compute pass: culls atlas objects into spatial chunks for efficient lookup.
// Dispatch: ceil(objectCount / 64) groups of 64 threads.
// Each thread checks one object, atomically appending its address into each
// overlapping chunk's packed list in culledObjects.

#include "shader_io.h"
#include "surface_atlas_common.slang"

[[vk::binding(0, 0)]] RWStructuredBuffer<uint>  objectsList    : register(u0); // object indices
[[vk::binding(1, 0)]] RWStructuredBuffer<uint>  chunks         : register(u1); // per-chunk atomics
[[vk::binding(2, 0)]] RWStructuredBuffer<uint>  culledObjects  : register(u2); // packed addresses
[[vk::binding(3, 0)]] RWStructuredBuffer<float4> objects       : register(u3); // object data

[[vk::push_constant]] ConstantBuffer<SurfaceAtlasCullPush> pc;

static const uint kChunkRes = 40;
static const uint kMaxPerChunk = 256;

[numthreads(64, 1, 1)]
void CS_CullObjects(uint3 dispatchThreadId : SV_DispatchThreadID)
{
    uint objectIndex = dispatchThreadId.x;
    if (objectIndex >= pc.objectCount)
        return;

    uint objAddr = objectsList[objectIndex];
    float4 objBounds = loadObjectBounds(objects, objAddr);
    float3 objMin = objBounds.xyz - objBounds.w;
    float3 objMax = objBounds.xyz + objBounds.w;

    float halfRes = float(kChunkRes) * 0.5f;
    int3 chunkMin = int3(floor((objMin - pc.viewPos) / pc.chunkSize + halfRes));
    int3 chunkMax = int3(floor((objMax - pc.viewPos) / pc.chunkSize + halfRes));
    chunkMin = clamp(chunkMin, int3(0,0,0), int3(kChunkRes - 1, kChunkRes - 1, kChunkRes - 1));
    chunkMax = clamp(chunkMax, int3(0,0,0), int3(kChunkRes - 1, kChunkRes - 1, kChunkRes - 1));

    const uint chunkResSq = kChunkRes * kChunkRes;
    const uint headerSlots = chunkResSq * kChunkRes; // one counter per chunk

    for (int cz = chunkMin.z; cz <= chunkMax.z; cz++)
    {
        for (int cy = chunkMin.y; cy <= chunkMax.y; cy++)
        {
            for (int cx = chunkMin.x; cx <= chunkMax.x; cx++)
            {
                uint chunkIdx = uint(cz * int(chunkResSq) + cy * int(kChunkRes) + cx);
                uint slot;
                InterlockedAdd(chunks[chunkIdx], 1, slot);

                if (slot < kMaxPerChunk)
                {
                    uint writeIdx = headerSlots + chunkIdx * kMaxPerChunk + slot;
                    culledObjects[writeIdx] = objAddr;
                }
            }
        }
    }
}
