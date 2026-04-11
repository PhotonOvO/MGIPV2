#ifndef SHADER_IBL_H
#define SHADER_IBL_H

#include "shader_io.h"

//------------------------------------------------------------------------------
// IBL Constants
//------------------------------------------------------------------------------

STATIC_CONST float IBL_PI = 3.14159265359f;

//------------------------------------------------------------------------------
// IBL Sampling Functions
//------------------------------------------------------------------------------

// Sample prefiltered environment map at given roughness level
vec3 samplePrefilteredEnvMap(SamplerCube envMap, vec3 direction, float roughness, uint maxMipLevel)
{
    float mipLevel = roughness * float(maxMipLevel);
    return envMap.SampleLevel(direction, mipLevel).rgb;
}

// Sample DFG LUT (integration of BRDF over hemisphere)
vec2 sampleDFGLUT(Sampler2D lutTexture, float NdotV, float roughness)
{
    // NdotV is x-axis, roughness is y-axis
    vec2 uv = vec2(NdotV, roughness);
    return lutTexture.Sample(uv).rg;
}

//------------------------------------------------------------------------------
// IBL Contribution Calculation
//------------------------------------------------------------------------------

vec3 computeIBLContribution(
    vec3 N,                    // Surface normal
    vec3 V,                    // View direction
    vec3 baseColor,            // Material base color
    float metallic,            // Material metallic
    float roughness,           // Material roughness
    SamplerCube prefilteredMap,// Prefiltered environment cube map
    Sampler2D dfgLUT,          // DFG integration LUT
    uint maxMipLevel,          // Max mip level in prefiltered map
    vec3 irradiance            // Irradiance contribution
)
{
    // Calculate reflection direction
    vec3 R = reflect(-V, N);

    // F0 for Fresnel calculation (0.04 for non-metals, baseColor for metals)
    vec3 F0 = lerp(vec3(0.04f, 0.04f, 0.04f), baseColor, metallic);

    // NdotV for LUT lookup (clamped to avoid artifacts)
    float NdotV = max(dot(N, V), 0.001f);

    // Sample prefiltered environment map
    vec3 prefilteredColor = samplePrefilteredEnvMap(prefilteredMap, R, roughness, maxMipLevel);

    // Sample DFG LUT
    vec2 dfg = sampleDFGLUT(dfgLUT, NdotV, roughness);

    // Calculate Fresnel using LUT values
    vec3 F = F0 * dfg.x + dfg.y;

    // Calculate specular IBL contribution
    vec3 specularIBL = prefilteredColor * F;

    // Calculate diffuse IBL (irradiance)
    // For metals, diffuse is zero; for non-metals, use irradiance * albedo
    vec3 kD = (1.0f - F) * (1.0f - metallic);
    vec3 diffuseIBL = kD * irradiance * baseColor / IBL_PI;

    return diffuseIBL + specularIBL;
}

#endif // SHADER_IBL_H