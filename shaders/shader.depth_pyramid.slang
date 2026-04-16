#include "shader_io.h"

[[vk::binding(0, 0)]]
Texture2D<float> depthTexture;

[[vk::binding(1, 0)]]
RWTexture2D<float> depthPyramidTexture[LDepthPyramidMaxMips];

[[vk::binding(2, 0)]]
ConstantBuffer<DepthPyramidUniforms> depthPyramidUniforms;

static const uint kGroupSizeX = 16;
static const uint kGroupSizeY = 16;

// 用于在组内保存“当前正在被继续 reduce 的那一层”。
// 初始先存 mip0 的 16x16。
// 然后前 8x8 线程覆盖写 mip1。
// 然后前 4x4 覆盖写 mip2。
// ...
groupshared float gsMip[kGroupSizeY][kGroupSizeX];

uint2 getSourceSize()
{
    return uint2(depthPyramidUniforms.sourceWidth, depthPyramidUniforms.sourceHeight);
}

uint2 getMipSize(uint mipLevel)
{
    return max(
        uint2(1u, 1u),
        uint2(depthPyramidUniforms.pyramidWidth, depthPyramidUniforms.pyramidHeight) >> mipLevel);
}

float loadSourceDepthClamped(uint2 coord)
{
    uint2 sourceSize = getSourceSize();
    coord = min(coord, sourceSize - uint2(1u, 1u));
    return depthTexture.Load(int3(coord, 0));
}

float min4(float a, float b, float c, float d)
{
    return min(min(a, b), min(c, d));
}

[numthreads(kGroupSizeX, kGroupSizeY, 1)]
[shader("compute")]
void depthPyramid(uint3 groupId: SV_GroupID,
                  uint3 groupThreadId: SV_GroupThreadID,
                  uint3 dispatchThreadId: SV_DispatchThreadID)
{
    uint mipCount = min(depthPyramidUniforms.mipCount, uint(LDepthPyramidMaxMips));
    uint2 localXY = groupThreadId.xy;

    // -----------------------------
    // Stage 0: 生成 mip0
    // -----------------------------
    // 每个 group 负责一个 16x16 的 mip0 tile
    uint2 mip0Coord = groupId.xy * uint2(kGroupSizeX, kGroupSizeY) + localXY;
    uint2 mip0Size = getMipSize(0);

    float v = 1.0;

    if (mip0Coord.x < mip0Size.x && mip0Coord.y < mip0Size.y)
    {
        // mip0 的每个像素对应 source 的 2x2
        uint2 srcBase = mip0Coord * 2u;

        float d0 = loadSourceDepthClamped(srcBase + uint2(0, 0));
        float d1 = loadSourceDepthClamped(srcBase + uint2(1, 0));
        float d2 = loadSourceDepthClamped(srcBase + uint2(0, 1));
        float d3 = loadSourceDepthClamped(srcBase + uint2(1, 1));

        v = min4(d0, d1, d2, d3);

        if (mipCount > 0)
        {
            depthPyramidTexture[0][mip0Coord] = v;
        }
    }

    gsMip[localXY.y][localXY.x] = v;
    GroupMemoryBarrierWithGroupSync();

    // -----------------------------
    // Stage 1: mip1 (16x16 -> 8x8)
    // -----------------------------
    if (mipCount > 1)
    {
        if (localXY.x < 8 && localXY.y < 8)
        {
            uint2 mip1Coord = groupId.xy * uint2(8, 8) + localXY;
            uint2 mip1Size = getMipSize(1);

            float r = min4(
                gsMip[localXY.y * 2 + 0][localXY.x * 2 + 0],
                gsMip[localXY.y * 2 + 0][localXY.x * 2 + 1],
                           gsMip[localXY.y * 2 + 1][localXY.x * 2 + 0],
                gsMip[localXY.y * 2 + 1][localXY.x * 2 + 1]);

            gsMip[localXY.y][localXY.x] = r;

            if (mip1Coord.x < mip1Size.x && mip1Coord.y < mip1Size.y)
            {
                depthPyramidTexture[1][mip1Coord] = r;
            }
        }

        GroupMemoryBarrierWithGroupSync();
    }

    // -----------------------------
    // Stage 2: mip2 (8x8 -> 4x4)
    // -----------------------------
    if (mipCount > 2)
    {
        if (localXY.x < 4 && localXY.y < 4)
        {
            uint2 mip2Coord = groupId.xy * uint2(4, 4) + localXY;
            uint2 mip2Size = getMipSize(2);

            float r = min4(
                gsMip[localXY.y * 2 + 0][localXY.x * 2 + 0],
                gsMip[localXY.y * 2 + 0][localXY.x * 2 + 1],
                           gsMip[localXY.y * 2 + 1][localXY.x * 2 + 0],
                gsMip[localXY.y * 2 + 1][localXY.x * 2 + 1]);

            gsMip[localXY.y][localXY.x] = r;

            if (mip2Coord.x < mip2Size.x && mip2Coord.y < mip2Size.y)
            {
                depthPyramidTexture[2][mip2Coord] = r;
            }
        }

        GroupMemoryBarrierWithGroupSync();
    }

    // -----------------------------
    // Stage 3: mip3 (4x4 -> 2x2)
    // -----------------------------
    if (mipCount > 3)
    {
        if (localXY.x < 2 && localXY.y < 2)
        {
            uint2 mip3Coord = groupId.xy * uint2(2, 2) + localXY;
            uint2 mip3Size = getMipSize(3);

            float r = min4(
                gsMip[localXY.y * 2 + 0][localXY.x * 2 + 0],
                gsMip[localXY.y * 2 + 0][localXY.x * 2 + 1],
                           gsMip[localXY.y * 2 + 1][localXY.x * 2 + 0],
                gsMip[localXY.y * 2 + 1][localXY.x * 2 + 1]);

            gsMip[localXY.y][localXY.x] = r;

            if (mip3Coord.x < mip3Size.x && mip3Coord.y < mip3Size.y)
            {
                depthPyramidTexture[3][mip3Coord] = r;
            }
        }

        GroupMemoryBarrierWithGroupSync();
    }

    // -----------------------------
    // Stage 4: mip4 (2x2 -> 1x1)
    // -----------------------------
    if (mipCount > 4)
    {
        if (localXY.x == 0 && localXY.y == 0)
        {
            uint2 mip4Coord = groupId.xy;
            uint2 mip4Size = getMipSize(4);

            float r = min4(
                gsMip[0][0],
                gsMip[0][1],
                gsMip[1][0],
                gsMip[1][1]);

            gsMip[0][0] = r;

            if (mip4Coord.x < mip4Size.x && mip4Coord.y < mip4Size.y)
            {
                depthPyramidTexture[4][mip4Coord] = r;
            }
        }

        GroupMemoryBarrierWithGroupSync();
    }
}