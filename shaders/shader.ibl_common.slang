#ifndef SHADER_IBL_COMMON_SLANG
#define SHADER_IBL_COMMON_SLANG

#include "shader_io.h"

struct IBLGeneratePushConstants
{
    uint width;
    uint height;
    uint sourceWidth;
    uint sourceHeight;
    uint sourceMaxMip;
    uint sampleCount;
    float roughness;
    uint reserved;
};

STATIC_CONST float IBL_PI = 3.14159265359;

float2 directionToEquirectUv(float3 direction)
{
    float3 d = normalize(direction);
    float phi = atan2(d.z, d.x);
    float theta = asin(clamp(d.y, -1.0, 1.0));
    return float2(0.5 + phi / (2.0 * IBL_PI), 0.5 - theta / IBL_PI);
}

float3 cubeTexelDirection(uint face, uint2 texel, uint2 size)
{
    float2 uv = (float2(texel) + 0.5) / float2(size);
    float2 p = uv * 2.0 - 1.0;
    p.y = -p.y;

    if(face == 0u) return normalize(float3( 1.0, p.y, -p.x));
    if(face == 1u) return normalize(float3(-1.0, p.y,  p.x));
    if(face == 2u) return normalize(float3( p.x, 1.0, -p.y));
    if(face == 3u) return normalize(float3( p.x,-1.0,  p.y));
    if(face == 4u) return normalize(float3( p.x, p.y,  1.0));
    return normalize(float3(-p.x, p.y, -1.0));
}

float radicalInverseVdC(uint bits)
{
    bits = (bits << 16u) | (bits >> 16u);
    bits = ((bits & 0x55555555u) << 1u) | ((bits & 0xAAAAAAAAu) >> 1u);
    bits = ((bits & 0x33333333u) << 2u) | ((bits & 0xCCCCCCCCu) >> 2u);
    bits = ((bits & 0x0F0F0F0Fu) << 4u) | ((bits & 0xF0F0F0F0u) >> 4u);
    bits = ((bits & 0x00FF00FFu) << 8u) | ((bits & 0xFF00FF00u) >> 8u);
    return float(bits) * 2.3283064365386963e-10;
}

float2 hammersley(uint i, uint n)
{
    return float2(float(i) / float(n), radicalInverseVdC(i));
}

void buildOrthonormalBasis(float3 n, out float3 tangent, out float3 bitangent)
{
    float3 up = abs(n.z) < 0.999 ? float3(0.0, 0.0, 1.0) : float3(1.0, 0.0, 0.0);
    tangent = normalize(cross(up, n));
    bitangent = cross(n, tangent);
}

float3 tangentToWorld(float3 v, float3 n)
{
    float3 tangent;
    float3 bitangent;
    buildOrthonormalBasis(n, tangent, bitangent);
    return normalize(tangent * v.x + bitangent * v.y + n * v.z);
}

float3 importanceSampleGGX(float2 xi, float roughness, float3 n)
{
    float a = roughness * roughness;
    float phi = 2.0 * IBL_PI * xi.x;
    float cosTheta = sqrt((1.0 - xi.y) / (1.0 + (a * a - 1.0) * xi.y));
    float sinTheta = sqrt(max(1.0 - cosTheta * cosTheta, 0.0));
    float3 h = float3(cos(phi) * sinTheta, sin(phi) * sinTheta, cosTheta);
    return tangentToWorld(h, n);
}

float distributionGGX(float nDotH, float roughness)
{
    float a = roughness * roughness;
    float a2 = a * a;
    float denom = nDotH * nDotH * (a2 - 1.0) + 1.0;
    return a2 / max(IBL_PI * denom * denom, 1e-5);
}

float geometrySchlickGGXIBL(float nDotV, float roughness)
{
    float a = roughness;
    float k = (a * a) * 0.5;
    return nDotV / max(nDotV * (1.0 - k) + k, 1e-4);
}

float geometrySmithIBL(float3 n, float3 v, float3 l, float roughness)
{
    return geometrySchlickGGXIBL(max(dot(n, v), 0.0), roughness)
         * geometrySchlickGGXIBL(max(dot(n, l), 0.0), roughness);
}

#endif
