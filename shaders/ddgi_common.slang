// DDGI octahedral-mapping / probe-math common module (Wave D2-3).
//
// Translated from LuxGI DDGICommon.glsl (octEncode/octDecode/
// normalizedOctCoord/getProbeId, F:\GitHub\LuxGI .../DDGI/DDGICommon.glsl) and
// hosts the spherical Fibonacci direction generator shared by the ray trace
// (ddgi_gi_sdf_rays.slang) and probe update (ddgi_*_probe_update.slang)
// kernels — both sides MUST reconstruct identical per-ray directions.
//
// Atlas layout contract (must stay in sync with DDGIProbeVolume::atlasExtent):
//   tile  = probeSideLength + 2 border texels (1px ring per probe);
//   width = tile * gridDims.x * gridDims.y + 2 outer edge texels;
//   height= tile * gridDims.z + 2 outer edge texels;
//   probe tile (tx, ty) interior origin = (tx, ty) * tile + 2 (atlas texels
//   x == 0 and x == width - 1 are the unused 1px outer guard band).
//
// Include-only module: excluded from the compile_slang list in CMakeLists.txt
// (same pattern as sdf_common.slang).

#ifndef DDGI_COMMON_SLANG
#define DDGI_COMMON_SLANG

static const float kDDGICommonPi = 3.14159265359;

// Spherical Fibonacci direction i of n (LuxGI DDGICommon.glsl:42-52).
float3 sphericalFibonacci(float i, float n)
{
    const float phiGolden = sqrt(5.0) * 0.5 + 0.5;
    const float fraction = (i * (phiGolden - 1.0)) - floor(i * (phiGolden - 1.0));
    const float phi = 2.0 * kDDGICommonPi * fraction;
    const float cosTheta = 1.0 - (2.0 * i + 1.0) * (1.0 / n);
    const float sinTheta = sqrt(clamp(1.0 - cosTheta * cosTheta, 0.0, 1.0));
    return float3(cos(phi) * sinTheta, sin(phi) * sinTheta, cosTheta);
}

float signNotZero(float k)
{
    return (k >= 0.0) ? 1.0 : -1.0;
}

float2 signNotZero(float2 v)
{
    return float2(signNotZero(v.x), signNotZero(v.y));
}

// Octahedral encode: unit direction -> [-1, 1]^2 (LuxGI DDGICommon.glsl:65-72).
float2 octEncode(float3 v)
{
    const float l1norm = abs(v.x) + abs(v.y) + abs(v.z);
    float2 result = v.xy * (1.0 / l1norm);
    if (v.z < 0.0)
    {
        result = (float2(1.0, 1.0) - abs(result.yx)) * signNotZero(result.xy);
    }
    return result;
}

// Octahedral decode: [-1, 1]^2 -> unit direction (LuxGI DDGICommon.glsl:74-82).
float3 octDecode(float2 o)
{
    float3 v = float3(o.x, o.y, 1.0 - abs(o.x) - abs(o.y));
    if (v.z < 0.0)
    {
        v.xy = (float2(1.0, 1.0) - abs(v.yx)) * signNotZero(v.xy);
    }
    return normalize(v);
}

// Normalized oct coord of an atlas texel: maps the probe-interior texel the
// atlas coordinate falls into onto [-1, 1]^2 at pixel centers. atlasCoord must
// be an interior texel (the -2 removes the outer edge + per-tile border, cf.
// LuxGI DDGICommon.glsl:85-92).
float2 normalizedOctCoord(int2 atlasCoord, int probeSideLength)
{
    const int probeWithBorderSide = probeSideLength + 2;
    const int2 octFragCoord = int2((atlasCoord.x - 2) % probeWithBorderSide,
                                   (atlasCoord.y - 2) % probeWithBorderSide);
    return (float2(octFragCoord) + float2(0.5, 0.5)) * (2.0 / float(probeSideLength))
        - float2(1.0, 1.0);
}

// Probe index of the atlas texel (LuxGI DDGICommon.glsl:94-99). probesPerRow
// is recovered from the atlas width via the layout contract above.
int getProbeId(int2 texel, int atlasWidth, int probeSideLength)
{
    const int probeWithBorderSide = probeSideLength + 2;
    const int probesPerRow = (atlasWidth - 2) / probeWithBorderSide;
    return (texel.x / probeWithBorderSide) + probesPerRow * (texel.y / probeWithBorderSide);
}

// Interior top-left atlas texel of a probe tile (inverse of getProbeId; cf.
// LuxGI textureCoordFromDirection probeTopLeftPosition + (2, 2)).
int2 probeAtlasInteriorOrigin(int probeIndex, int probesPerRow, int probeSideLength)
{
    const int probeWithBorderSide = probeSideLength + 2;
    return int2(probeIndex % probesPerRow, probeIndex / probesPerRow) * probeWithBorderSide
        + int2(2, 2);
}

#endif // DDGI_COMMON_SLANG
