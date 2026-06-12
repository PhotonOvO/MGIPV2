// DDGI irradiance border update (Wave D2-3).
//
// One 8x4 workgroup per probe tile: copies the probe's interior edge texels
// into the 1px border ring around the 8x8 octahedral tile following the
// octahedral wrap rule (row/column mirror + diagonal corners), so bilinear
// sampling across tile edges stays correct. Offset table translated verbatim
// from LuxGI BorderUpdate.glsl (irradiance variant, 36 entries: 32 edge
// copies + 4 corner copies done by threads 0-3).
//
// Offsets are relative to the tile's border origin
// (workGroupID * (side + 2) + 1): xy = source (interior), zw = destination
// (border). No uniforms needed — the dispatch shape covers every probe.

[[vk::binding(0, 0)]]
RWTexture2D<float4> irradianceAtlas;

static const int kProbeSideLength = 8;
static const uint kNumThreads = 32; // 8 x 4

static const int4 kOffsets[36] = {
    int4(8, 1, 1, 0),
    int4(7, 1, 2, 0),
    int4(6, 1, 3, 0),
    int4(5, 1, 4, 0),
    int4(4, 1, 5, 0),
    int4(3, 1, 6, 0),
    int4(2, 1, 7, 0),
    int4(1, 1, 8, 0),
    int4(8, 8, 1, 9),
    int4(7, 8, 2, 9),
    int4(6, 8, 3, 9),
    int4(5, 8, 4, 9),
    int4(4, 8, 5, 9),
    int4(3, 8, 6, 9),
    int4(2, 8, 7, 9),
    int4(1, 8, 8, 9),
    int4(1, 8, 0, 1),
    int4(1, 7, 0, 2),
    int4(1, 6, 0, 3),
    int4(1, 5, 0, 4),
    int4(1, 4, 0, 5),
    int4(1, 3, 0, 6),
    int4(1, 2, 0, 7),
    int4(1, 1, 0, 8),
    int4(8, 8, 9, 1),
    int4(8, 7, 9, 2),
    int4(8, 6, 9, 3),
    int4(8, 5, 9, 4),
    int4(8, 4, 9, 5),
    int4(8, 3, 9, 6),
    int4(8, 2, 9, 7),
    int4(8, 1, 9, 8),
    int4(8, 8, 0, 0),
    int4(1, 8, 9, 0),
    int4(8, 1, 0, 9),
    int4(1, 1, 9, 9)
};

void copyTexel(int2 baseCoord, uint index)
{
    const int2 srcCoord = baseCoord + kOffsets[index].xy;
    const int2 dstCoord = baseCoord + kOffsets[index].zw;
    irradianceAtlas[uint2(dstCoord)] = irradianceAtlas[uint2(srcCoord)];
}

[shader("compute")]
[numthreads(8, 4, 1)]
void kernelDDGIIrradianceBorderUpdate(uint3 groupId : SV_GroupID,
                                      uint localIndex : SV_GroupIndex)
{
    // Tile border origin: top-left border texel of this probe's tile
    // (LuxGI BorderUpdate.glsl:150 — the +1 skips the atlas outer edge).
    const int2 baseCoord = int2(groupId.xy) * (kProbeSideLength + 2) + int2(1, 1);

    copyTexel(baseCoord, localIndex);

    if (localIndex < 4u)
    {
        copyTexel(baseCoord, kNumThreads + localIndex);
    }
}
