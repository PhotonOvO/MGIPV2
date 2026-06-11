#include "shader_io.h"

// Experimental Hi-Z accelerated march (GPU Pro 5, Uludag).  Compile-time switch:
// the screen-space DDA below remains the shipping path while this is false.
STATIC_CONST bool kUseHiZMarch = false;

// Interleaved gradient noise (Jimenez 2014).  Produces a spatially-varying value in [0, 1)
// that tiles well across 8x8 thread groups and changes each frame via the frame offset.
float ign(uint2 p, uint frame)
{
    p += frame * uint2(47u, 17u);
    return frac(52.9829189 * frac(0.06711056 * float(p.x) + 0.00583715 * float(p.y)));
}

[[vk::binding(0, 0)]]
Texture2D<float4> sceneColorHistory;
[[vk::binding(1, 0)]]
Texture2D<float> depthTexture;
[[vk::binding(2, 0)]]
Texture2D<float4> normalMaterialTexture;
[[vk::binding(3, 0)]]
Texture2D<float4> baseColorRoughnessTexture;
[[vk::binding(4, 0)]]
RWTexture2D<float4> ssrRaw;
[[vk::binding(5, 0)]]
ConstantBuffer<CameraUniforms> camera;
// Hi-Z depth pyramid (max-reduced reversed-Z: each cell stores the *closest*
// surface of its footprint).  Only read by the kUseHiZMarch path; the engine
// pads unused descriptor slots, so never index beyond the real mip count.
[[vk::binding(6, 0)]]
Texture2D<float> depthPyramidTexture[LDepthPyramidMaxMips];

[[vk::push_constant]]
ConstantBuffer<GPUDrivenSSRPushConstants> ssrPush;

float3 decodeNormalOct(float2 encoded)
{
    float2 f = encoded * 2.0 - 1.0;
    float3 n = float3(f.x, f.y, 1.0 - abs(f.x) - abs(f.y));
    if(n.z < 0.0)
    {
        n.xy = (1.0 - abs(n.yx)) * float2(n.x >= 0.0 ? 1.0 : -1.0, n.y >= 0.0 ? 1.0 : -1.0);
    }
    return normalize(n);
}

float3 reconstructWorldPosition(float2 uv, float depth)
{
    float4 clipPos = float4(uv * 2.0 - 1.0, depth, 1.0);
    float4 worldPos = mul(camera.inverseViewProjection, clipPos);
    return worldPos.xyz / max(worldPos.w, 1.0e-5);
}

float edgeFade(float2 uv)
{
    float2 edgeDistance = min(uv, 1.0 - uv);
    return saturate(min(edgeDistance.x, edgeDistance.y) * 12.0);
}

// Extract the near-plane distance from the projection matrix at runtime.
// The engine uses a reversed-Z finite perspective (glm::perspective with near/far
// swapped); for that convention, in GLM column-major terms:
//   projection[2][2] = a,  projection[3][2] = b
// In Slang/HLSL row-major, GLM [col][row] maps to [row][col], so:
//   a = camera.projection[2][2],  b = camera.projection[2][3]
// The reversed-Z near-plane formula (same as clipspace::extractPerspectiveNearPlane):
//   near = b / (a + 1)
float extractNear()
{
    const float a = camera.projection[2][2];
    const float b = camera.projection[2][3];
    const float denom = a + 1.0;
    return abs(denom) > 1.0e-5 ? abs(b / denom) : 0.1;
}

// Convert a reversed-Z NDC depth to positive view-space distance.
// Reversed-Z convention: depth == 1.0 at the near plane, depth -> 0 at infinity.
// Formula: linearZ = near / depth   (matches the matrix derivation above).
float linearizeDepth(float ndcDepth, float near)
{
    return near / max(ndcDepth, 1.0e-7);
}

// GGX importance sampling -- self-contained so ssr_trace does not depend on ibl_common.
// Builds a half-vector H in world space distributed according to the GGX NDF.
// xi: uniform 2D sample in [0,1)^2.  alpha = roughness^2.
float3 importanceSampleGGX(float2 xi, float alpha, float3 N)
{
    const float phi      = 2.0 * 3.14159265359 * xi.x;
    const float cosTheta = sqrt(max((1.0 - xi.y) / max(1.0 + (alpha * alpha - 1.0) * xi.y, 1.0e-7), 0.0));
    const float sinTheta = sqrt(max(1.0 - cosTheta * cosTheta, 0.0));

    // Tangent-space half-vector.
    const float3 H_ts = float3(sinTheta * cos(phi), sinTheta * sin(phi), cosTheta);

    // Build an orthonormal tangent frame around N.
    const float3 up     = abs(N.z) < 0.999 ? float3(0.0, 0.0, 1.0) : float3(1.0, 0.0, 0.0);
    const float3 T      = normalize(cross(up, N));
    const float3 B      = cross(N, T);

    // Transform to world space.
    return normalize(T * H_ts.x + B * H_ts.y + N * H_ts.z);
}

// Refine a DDA hit via 6 bisection steps between the last miss (prevUv, prevLinearZ)
// and the first crossing (hitUv, hitLinearZ).  All UVs are in normalised [0,1] half-res
// space; the function reads from depthTexture (full-res) by scaling by 2.  The refined
// UV is returned; the colour sample is taken by the caller at the refined coordinate.
float2 refineHit(float2 prevUv, float prevLinearZ,
                 float2 hitUv,  float hitLinearZ,
                 uint2  outputSize, float near)
{
    float2 lo = prevUv;
    float2 hi = hitUv;
    float  loZ = prevLinearZ;
    float  hiZ = hitLinearZ;
    const float2 halfTexelSize = float2(1.0) / float2(outputSize);

    [unroll]
    for(int i = 0; i < 6; ++i)
    {
        float2 mid = (lo + hi) * 0.5;

        // Map normalised half-res UV to half-res integer texel, then to full-res.
        float2 halfCoordF = mid / halfTexelSize - 0.5;
        uint2  halfCoord  = uint2(clamp(int2(halfCoordF), int2(0, 0), int2(outputSize) - int2(1, 1)));
        uint2  fullCoordM = halfCoord * 2u + uint2(1u, 1u);
        float sceneNdc = depthTexture.Load(int3(fullCoordM, 0));
        float sceneZ   = linearizeDepth(max(sceneNdc, 1.0e-7), near);

        // Midpoint ray depth via 1/viewZ interpolation for perspective correctness.
        float invLoZ  = 1.0 / max(loZ, 1.0e-7);
        float invHiZ  = 1.0 / max(hiZ, 1.0e-7);
        float midZ    = 1.0 / max((invLoZ + invHiZ) * 0.5, 1.0e-7);

        if(midZ > sceneZ)
        {
            // Ray is farther than the scene at mid: the crossing lies in [lo, mid].
            hi  = mid;
            hiZ = midZ;
        }
        else
        {
            lo  = mid;
            loZ = midZ;
        }
    }
    return (lo + hi) * 0.5;
}

// Experimental Hi-Z traversal (GPU Pro 5, Uludag).  Marches a ray expressed in
// (uv, reversed-Z NDC depth) space -- all three components interpolate linearly
// along a screen-space line.  Per iteration the ray either skips the whole current
// pyramid cell (when the in-cell segment stays strictly nearer than the cell's
// closest surface) and coarsens one mip, or refines one mip; a crossing at mip 0
// is the hit candidate, validated against the linear-depth thickness budget like
// the DDA path.  The pyramid's max reduction over reversed-Z depths yields the
// per-cell *closest* surface, which is exactly the conservative bound this
// traversal needs to skip empty space safely.
bool hiZMarch(float3 rayStart, float3 rayDelta, float near, float thickness,
              out float2 hitUv, out float hitT)
{
    hitUv = rayStart.xy;
    hitT = 0.0;

    uint2 pyramidSize;
    depthPyramidTexture[0].GetDimensions(pyramidSize.x, pyramidSize.y);
    if(pyramidSize.x == 0u || pyramidSize.y == 0u)
    {
        return false;
    }

    // Coarsest mip the traversal may climb to.  Kept conservative: descriptor
    // slots beyond the pyramid's real mip count are padded with the last valid
    // view, and indexing those with a shifted mipSize would misalign cells.
    const int kMaxMip = 6;
    const uint kMaxIterations = 64u;

    int mip = 0;
    float t = 1.0e-4;  // nudge off the origin cell so the first test is not self-occluded

    [loop]
    for(uint iter = 0u; iter < kMaxIterations; ++iter)
    {
        const float3 p = rayStart + rayDelta * t;
        if(t > 1.0 || any(p.xy < float2(0.0)) || any(p.xy > float2(1.0)))
        {
            return false;
        }

        const int2 mipSize = max(int2(pyramidSize) >> mip, int2(1, 1));
        const int2 cell = clamp(int2(p.xy * float2(mipSize)), int2(0, 0), mipSize - int2(1, 1));
        const float cellClosest = depthPyramidTexture[mip].Load(int3(cell, 0));

        // Parametric distance to the cell exit along the screen-space direction.
        const float2 cellMin = float2(cell) / float2(mipSize);
        const float2 cellMax = float2(cell + int2(1, 1)) / float2(mipSize);
        float2 tEdges = float2(1.0e30, 1.0e30);
        if(abs(rayDelta.x) > 1.0e-8)
        {
            tEdges.x = ((rayDelta.x > 0.0 ? cellMax.x : cellMin.x) - rayStart.x) / rayDelta.x;
        }
        if(abs(rayDelta.y) > 1.0e-8)
        {
            tEdges.y = ((rayDelta.y > 0.0 ? cellMax.y : cellMin.y) - rayStart.y) / rayDelta.y;
        }
        const float tExit = min(min(tEdges.x, tEdges.y) + 1.0e-5, 1.0);

        // Reversed-Z: larger NDC depth is closer.  The segment [t, tExit] cannot hit
        // anything in this cell while it stays strictly nearer than the closest surface.
        const float zNow = p.z;
        const float zExit = rayStart.z + rayDelta.z * tExit;
        if(min(zNow, zExit) > cellClosest)
        {
            t = tExit;
            mip = min(mip + 1, kMaxMip);
            continue;
        }

        if(mip > 0)
        {
            mip -= 1;  // potential occluder below: refine without advancing
            continue;
        }

        // Mip 0 crossing: locate the parametric crossing of the cell's depth plane.
        float tCross = t;
        if(abs(rayDelta.z) > 1.0e-8)
        {
            tCross = clamp((cellClosest - rayStart.z) / rayDelta.z, t, tExit);
        }
        // Thickness validation in linear view space mirrors the DDA hit test.
        const float rayNdcZ = rayStart.z + rayDelta.z * tCross;
        const float rayLinearZ = linearizeDepth(max(rayNdcZ, 1.0e-7), near);
        const float sceneLinearZ = linearizeDepth(max(cellClosest, 1.0e-7), near);
        const float depthDelta = rayLinearZ - sceneLinearZ;
        if(depthDelta > -1.0e-4 && depthDelta < thickness)
        {
            hitUv = rayStart.xy + rayDelta.xy * tCross;
            hitT = tCross;
            return true;
        }

        // Crossing too thick (ray passes behind a thin surface): step past this
        // cell at the fine mip and keep marching.
        t = tExit;
    }
    return false;
}

[shader("compute")]
[numthreads(8, 8, 1)]
void kernelSSRTrace(uint3 dispatchThreadId : SV_DispatchThreadID)
{
    const uint2 outputSize = uint2(ssrPush.params0.xy);
    if(dispatchThreadId.x >= outputSize.x || dispatchThreadId.y >= outputSize.y)
    {
        return;
    }

    // SSR runs at half resolution.  Map the half-res thread to the corresponding
    // full-res texel (centre of the 2x2 block, offset by (1,1) to avoid the
    // exact top-left corner which can land on tile boundaries).
    const uint2 inputSize = max(outputSize * 2u, uint2(1u, 1u));
    const uint2 fullCoord = min(dispatchThreadId.xy * 2u + uint2(1u, 1u), inputSize - uint2(1u, 1u));
    const float2 uv = (float2(fullCoord) + 0.5) / float2(inputSize);
    const float depth = depthTexture.Load(int3(fullCoord, 0));
    if(depth <= 0.0001)
    {
        ssrRaw[dispatchThreadId.xy] = float4(0.0);
        return;
    }

    const float4 baseRoughness = baseColorRoughnessTexture.Load(int3(fullCoord, 0));
    const float roughness = saturate(baseRoughness.a);
    if(roughness > 0.72)
    {
        ssrRaw[dispatchThreadId.xy] = float4(0.0);
        return;
    }

    const float4 normalPacked = normalMaterialTexture.Load(int3(fullCoord, 0));
    const float metallic = saturate(normalPacked.b);
    const float3 n = decodeNormalOct(normalPacked.rg);
    const float3 worldPos = reconstructWorldPosition(uv, depth);
    const float3 viewDir = normalize(worldPos - camera.cameraPosition);

    const uint maxSteps = max(uint(ssrPush.params0.z), 1u);
    // thickness is in metres; passed directly from the C++ push constant.
    // RenderTypes.h ssrThickness default changed to 0.3 m (see that file).
    const float thickness = max(ssrPush.params0.w, 0.001);
    const float maxDistance = max(ssrPush.params1.y, 0.01);
    const float confidenceScale = ssrPush.params1.z;
    const uint frameIndex = uint(ssrPush.params1.w);

    // Per-pixel jitter applied to the DDA first-step phase; see Stage 2 comment.
    // Two independent noise values drive GGX sampling (xi0 for phi, xi1 for theta).
    // The offset uint2(73, 151) ensures xi0 and xi1 are decorrelated spatially.
    const float xi0 = ign(dispatchThreadId.xy,              frameIndex);
    const float xi1 = ign(dispatchThreadId.xy + uint2(73u, 151u), frameIndex);

    // GGX importance-sampled ray direction.
    // For near-mirror surfaces (roughness < 0.05) skip the perturbation entirely
    // to avoid noise on surfaces where the GGX lobe collapses to a delta function.
    float3 rayDir;
    const float3 specularDir = normalize(reflect(viewDir, n));
    if(roughness < 0.05)
    {
        rayDir = specularDir;
    }
    else
    {
        const float alpha = roughness * roughness;
        const float3 H = importanceSampleGGX(float2(xi0, xi1), alpha, n);
        rayDir = normalize(reflect(viewDir, H));
        // If the perturbed direction faces away from the surface (can happen near
        // grazing angles), fall back to the mirror direction.
        if(dot(rayDir, n) <= 0.0)
        {
            rayDir = specularDir;
        }
    }
    if(dot(rayDir, n) <= 0.0)
    {
        ssrRaw[dispatchThreadId.xy] = float4(0.0);
        return;
    }
    const float roughnessFade = saturate(1.0 - roughness / 0.72);
    const float materialFade = saturate(0.35 + metallic * 0.65);

    // xi0 also serves as the DDA first-step jitter; reusing it avoids an extra
    // noise evaluation while keeping the per-step-phase and per-direction offsets
    // correlated (which is harmless: TAA decorrelates them across frames).
    const float jitter = xi0;

    // -----------------------------------------------------------------------
    // Screen-space DDA setup (McGuire & Mara 2014 style).
    // Project the ray end-point (worldPos + rayDir * maxDistance) into clip space.
    // -----------------------------------------------------------------------
    const float near = extractNear();
    const float startLinearZ = linearizeDepth(depth, near);

    // End point: clamp to a reasonable depth so the projection does not diverge.
    float3 endWorld = worldPos + rayDir * maxDistance;
    float4 endClip = mul(camera.viewProjection, float4(endWorld, 1.0));
    float2 endUv;
    float  endRayDepth;
    if(endClip.w <= 1.0e-5)
    {
        ssrRaw[dispatchThreadId.xy] = float4(0.0);
        return;
    }
    float3 endNdc = endClip.xyz / endClip.w;
    endUv = endNdc.xy * 0.5 + 0.5;
    endRayDepth = endNdc.z;

    // Clamp end UV so the DDA does not exit the screen immediately.
    endUv = clamp(endUv, float2(0.001), float2(0.999));
    // endLinearZ from the projected depth of the end point.
    const float endLinearZ = endRayDepth > 1.0e-5 ? linearizeDepth(endRayDepth, near) : maxDistance;

    // Convert start/end to half-resolution texel coordinates.
    const float2 halfTexelSize = float2(1.0) / float2(outputSize);

    if(kUseHiZMarch)
    {
        // Experimental Hi-Z path: march in (uv, NDC depth) space against the depth
        // pyramid.  Confidence mirrors the DDA path with progress fade by hit t.
        float3 hizColor = float3(0.0);
        float  hizConfidence = 0.0;
        float2 hizUv;
        float  hizT;
        if(hiZMarch(float3(uv, depth),
                    float3(endUv - uv, endRayDepth - depth),
                    near, thickness, hizUv, hizT))
        {
            const float2 hizCoordF = hizUv / halfTexelSize - 0.5;
            const uint2  hizCoord  = uint2(clamp(int2(hizCoordF),
                                                 int2(0, 0),
                                                 int2(outputSize) - int2(1, 1)));
            hizColor = sceneColorHistory.Load(int3(hizCoord * 2u + uint2(1u, 1u), 0)).rgb;
            hizConfidence = saturate((1.0 - hizT) * edgeFade(hizUv)
                                     * roughnessFade * materialFade * confidenceScale);
        }
        ssrRaw[dispatchThreadId.xy] = float4(hizColor, hizConfidence);
        return;
    }

    const float2 startTexel = uv / halfTexelSize;          // half-res texel space
    const float2 endTexel   = endUv / halfTexelSize;

    // Number of DDA steps is bounded by the pixel-space distance of the segment.
    const float2 delta = endTexel - startTexel;
    const float pixelDistance = max(abs(delta.x), abs(delta.y));
    const uint numSteps = min(maxSteps, uint(ceil(pixelDistance)));
    if(numSteps == 0u)
    {
        ssrRaw[dispatchThreadId.xy] = float4(0.0);
        return;
    }

    // 1/viewZ of start and end for perspective-correct depth interpolation along
    // the screen-space line (see McGuire & Mara: interpolate 1/w linearly).
    const float invStartZ = 1.0 / max(startLinearZ, 1.0e-7);
    const float invEndZ   = 1.0 / max(endLinearZ,   1.0e-7);

    float3 hitColor    = float3(0.0);
    float  confidence  = 0.0;
    float2 prevUv      = uv;
    float  prevLinearZ = startLinearZ;
    bool   hit         = false;

    [loop]
    for(uint stepIndex = 1u; stepIndex <= numSteps; ++stepIndex)
    {
        // t is the normalised progress along the DDA segment; the jitter shifts
        // the first step so the sampling phase differs per pixel and per frame.
        const float t = (float(stepIndex) - 1.0 + (0.5 + jitter)) / float(numSteps);

        // Linear interpolation in UV space (correct for DDA; depth is handled separately).
        const float2 sampleUv = lerp(uv, endUv, t);

        // Perspective-correct depth interpolation: lerp 1/viewZ, then invert.
        const float invRayZ    = lerp(invStartZ, invEndZ, t);
        const float rayLinearZ = 1.0 / max(invRayZ, 1.0e-7);

        // Map sample UV to half-resolution integer coordinate.
        const float2 sampleCoordF = sampleUv / halfTexelSize - 0.5;
        const uint2  sampleCoord  = uint2(clamp(int2(sampleCoordF),
                                                int2(0, 0),
                                                int2(outputSize) - int2(1, 1)));
        const float sceneNdc   = depthTexture.Load(int3(sampleCoord * 2u + uint2(1u, 1u), 0));
        if(sceneNdc <= 0.0001)
        {
            prevUv      = sampleUv;
            prevLinearZ = rayLinearZ;
            continue;  // background pixel, keep marching
        }

        const float sceneLinearZ = linearizeDepth(sceneNdc, near);

        // Hit test in linear-Z space.
        // reversed-Z: depth==1 is near, depth->0 is far, so larger NDC depth is closer.
        // In linear view-space, larger value means farther from camera.
        // The ray is "behind" the surface when rayLinearZ > sceneLinearZ (ray is farther).
        // Accept the hit if the penetration depth is within the thickness budget.
        const float delta = rayLinearZ - sceneLinearZ;
        if(delta > 0.0 && delta < thickness)
        {
            // Binary-search refinement between the previous step (miss) and this step (hit).
            const float2 refinedUv = refineHit(prevUv, prevLinearZ,
                                               sampleUv, rayLinearZ,
                                               outputSize, near);
            const float2 refinedCoordF = refinedUv / halfTexelSize - 0.5;
            const uint2  refinedCoord  = uint2(clamp(int2(refinedCoordF),
                                                     int2(0, 0),
                                                     int2(outputSize) - int2(1, 1)));
            hitColor  = sceneColorHistory.Load(int3(refinedCoord * 2u + uint2(1u, 1u), 0)).rgb;
            const float progressFade = 1.0 - t;  // confidence diminishes further along the ray
            confidence = saturate(progressFade * edgeFade(refinedUv)
                                  * roughnessFade * materialFade * confidenceScale);
            hit = true;
            break;
        }

        prevUv      = sampleUv;
        prevLinearZ = rayLinearZ;
    }

    ssrRaw[dispatchThreadId.xy] = float4(hitColor, confidence);
}
