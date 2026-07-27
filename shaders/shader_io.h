#ifndef HOST_DEVICE_H
#define HOST_DEVICE_H

#ifdef __SLANG__
typealias vec2 = float2;
typealias vec3 = float3;
typealias vec4 = float4;
typealias ivec4 = int4;
typealias uvec3 = uint3;
typealias uvec4 = uint4;
typealias mat4 = float4x4;
#define STATIC_CONST static const
#else
#include <cstdint>
// GLM configuration for Vulkan (must be defined before including glm)
#define GLM_FORCE_DEPTH_ZERO_TO_ONE
#include <glm/glm.hpp>
using vec2 = glm::vec2;
using vec3 = glm::vec3;
using vec4 = glm::vec4;
using ivec4 = glm::ivec4;
using uvec3 = glm::uvec3;
using uvec4 = glm::uvec4;
using mat4 = glm::mat4;
#define STATIC_CONST static const
#endif

// Layout constants
// Set 0: Pass-global textures (bindless)
STATIC_CONST int LSetTextures  = 0;
STATIC_CONST int LBindTextures = 0;
STATIC_CONST int LBindShadowMap = 1;
STATIC_CONST int LBindIBLIrradiance = 9;
STATIC_CONST int LBindIBLPrefiltered = 10;
STATIC_CONST int LBindIBLBrdfLut = 11;

// Set 1: Scene-level uniform buffers
STATIC_CONST int LSetScene      = 1;
STATIC_CONST int LBindSceneInfo = 0;      // SceneInfo for compute
STATIC_CONST int LBindCamera    = 1;      // Camera uniform buffer
STATIC_CONST int LBindLighting  = 2;      // Scene lighting/shadow data
STATIC_CONST int LBindLightCulling = 3;   // Scene light-culling data
STATIC_CONST int LBindPostProcess = 4;    // Per-pass post-process uniforms

// Set 2: Draw-level dynamic uniforms
STATIC_CONST int LSetDraw       = 2;
STATIC_CONST int LBindDrawModel = 0;      // Per-draw model matrix
STATIC_CONST int LBindDrawModelMdi = 1;   // Per-draw model data for MDI / indexed via draw ID

// Vertex layout
STATIC_CONST int LVPosition = 0;
STATIC_CONST int LVColor    = 1;
STATIC_CONST int LVTexCoord = 2;

// Camera uniform buffer (per-scene)
struct CameraUniforms
{
  mat4 view;
  mat4 projection;
  mat4 viewProjection;
  mat4 inverseViewProjection;
  mat4 prevView;
  mat4 prevProjection;
  mat4 prevViewProjection;
  mat4 unjitteredViewProjection;
  mat4 unjitteredInverseViewProjection;
  mat4 prevUnjitteredViewProjection;
  mat4 prevJitteredViewProjection;
  vec3 cameraPosition;
  float shadowConstantBias;
  vec4 shadowDirectionAndSlopeBias;
};

// Per-draw uniform buffer (dynamic)
struct DrawUniforms
{
  mat4 modelMatrix;
  mat4 prevModelMatrix;
  vec4 baseColorFactor;
  int32_t baseColorTextureIndex;   // Bindless texture index, -1 = no texture
  int32_t normalTextureIndex;      // -1 = no texture
  int32_t metallicRoughnessTextureIndex;  // -1 = no texture
  int32_t occlusionTextureIndex;   // -1 = no texture
  float metallicFactor;
  float roughnessFactor;
  float normalScale;
  float occlusionStrength;
  vec4 emissiveFactor;
  int32_t alphaMode;      // 0=OPAQUE, 1=MASK, 2=BLEND
  float alphaCutoff;      // Default 0.5, used for MASK mode
  int32_t emissiveTextureIndex;    // -1 = no texture
  int32_t materialWorkflow;        // 0=metallic-roughness, 1=specular-glossiness approximation
};

struct SceneInfo
{
  uint64_t dataBufferAddress;
  vec2     resolution;
  float    animValue;
  int32_t  numData;
  int32_t  texId;
};

struct PushConstant
{
  vec3 color;
};

struct PostProcessUniforms
{
  vec4 params0;  // exposure, bloom intensity, bloom threshold, bloom enabled
  vec4 params1;  // source texel size x/y, output texel size x/y
  vec4 params2;  // adaptive enabled, target luminance, min exposure, max exposure
  vec4 params3;  // saturation, contrast, gamma, vignette intensity
  vec4 params4;  // lens enabled, lens dirt, color LUT strength, AgX highlight compression
  vec4 params5;  // TAA enabled, AgX shadow compression, blend weight, show velocity
};

struct GPUDrivenAOPushConstants
{
  vec4 params0;  // outputWidth, outputHeight, radius, intensity
  vec4 params1;  // invInputWidth, invInputHeight, depthScale, normalWeight
};

struct GPUDrivenSSRPushConstants
{
  vec4 params0;  // outputWidth, outputHeight, maxSteps, thickness
  vec4 params1;  // stride, maxDistance, confidenceScale, frameIndex (float cast of frameIndex % 64)
};

struct PushConstantCompute
{
  uint64_t bufferAddress;
  float    rotationAngle;
  int32_t  numVertex;
};

struct Vertex
{
  vec3 position;
  vec3 color;
  vec2 texCoord;
};

// glTF vertex format: Position(12) + Normal(12) + TexCoord(8) = 32 bytes
STATIC_CONST int LVGltfPosition = 0;
STATIC_CONST int LVGltfNormal   = 1;
STATIC_CONST int LVGltfTexCoord = 2;

struct VertexGltf
{
  vec3 position;
  vec3 normal;
  vec2 texCoord;
};

// Push constant for glTF rendering (legacy, will be removed)
struct PushConstantGltf
{
  mat4 model;
  mat4 viewProjection;
  vec4 baseColorFactor;
  uint32_t materialIndex;
  uint32_t _padding[3];
};

// Tangent vertex location
STATIC_CONST int LVGltfTangent = 3;

// Alpha mode constants (matches glTF spec)
STATIC_CONST int LAlphaOpaque   = 0;
STATIC_CONST int LAlphaMask     = 1;
STATIC_CONST int LAlphaBlend    = 2;

// Light type constants
STATIC_CONST uint32_t LLightTypeDirectional = 0;
STATIC_CONST uint32_t LLightTypePoint       = 1;
STATIC_CONST uint32_t LLightTypeSpot        = 2;

// Tiled light culling constants
STATIC_CONST int LTileSizeX        = 16;
STATIC_CONST int LTileSizeY        = 16;
STATIC_CONST int LMaxLightsPerTile = 32;
STATIC_CONST int LDepthPyramidMaxMips = 32;

// Mobile clustered lighting constants
STATIC_CONST uint32_t LClusterGridSizeX = 16u;
STATIC_CONST uint32_t LClusterGridSizeY = 9u;
STATIC_CONST uint32_t LClusterGridSizeZ = 24u;
STATIC_CONST uint32_t LClusterCount = LClusterGridSizeX * LClusterGridSizeY * LClusterGridSizeZ;
STATIC_CONST uint32_t LMaxLightsPerCluster = 32u;
STATIC_CONST uint32_t LClusterOverflowBit = 0x80000000u;
STATIC_CONST uint32_t LClusterCountMask = 0x7fffffffu;
STATIC_CONST uint32_t LClusterLightTypePointBit = 0x00000000u;
STATIC_CONST uint32_t LClusterLightTypeSpotBit = 0x40000000u;
STATIC_CONST uint32_t LClusterLightIndexMask = 0x3fffffffu;

// CSM (Cascaded Shadow Maps) constants
STATIC_CONST int LCascadeCount = 4;  // Number of shadow cascades
STATIC_CONST float LCascadeSplitLambda = 0.5f;  // Practical split (log + linear mix)
STATIC_CONST float LCascadeBlendRegion = 0.0f;  // Hard boundaries (no blending)

// Cascade debug overlay mode
STATIC_CONST int LCascadeOverlayModeOff = 0;
STATIC_CONST int LCascadeOverlayModeFrustum = 1;
STATIC_CONST int LCascadeOverlayModeScreen = 2;

struct LightData
{
  vec3     positionOrDirection;  // Directional: direction to light, others: position
  float    intensity;
  vec3     color;
  float    range;
  vec3     spotDirection;
  float    spotInnerAngle;
  uint32_t lightType;
  float    spotOuterAngle;
  float    _padding[2];
};

struct LightListUniforms
{
  uint32_t numLights;
  uint32_t numDirectionalLights;
  uint32_t numPointLights;
  uint32_t numSpotLights;
  vec3     ambientColor;
  float    _padding;
};

struct LightCoarseCullingUniforms
{
  mat4 viewProjection;
  vec4 cameraRight;
  vec4 cameraUp;
  vec4 screenTileInfo;  // x = width, y = height, z = tileCountX, w = tileCountY
  vec4 lightCountInfo;  // x = pointLightCount, y = spotLightCount
  vec4 debugInfo;       // x = show coarse culling heatmap
};

struct DepthPyramidUniforms
{
  uint32_t sourceWidth;
  uint32_t sourceHeight;
  uint32_t _padding0;
  uint32_t _padding1;
  uint32_t pyramidWidth;
  uint32_t pyramidHeight;
  uint32_t mipCount;
  uint32_t _padding2;
};

// Global SDF composition (DDGI Wave D1-2)
STATIC_CONST int LGlobalSDFMaxMeshSDFs = 8;
STATIC_CONST int LGlobalSDFMipCount = 3;

// Shared root constants for the Global SDF clear/mipmap dispatches.
struct GlobalSDFDispatchPush
{
  vec4 params0;  // x = destination resolution (cubic), y = clear value (clear only), z/w reserved
};

struct GlobalSDFComposeUniforms
{
  vec4 volumeBoundsMin;  // xyz = global volume world-space min, w = voxel size
  vec4 volumeBoundsMax;  // xyz = global volume world-space max, w = max encode distance
  uint32_t resolution;   // global volume voxel resolution (cubic)
  uint32_t numMeshSDFs;  // valid entries in meshBounds arrays (<= LGlobalSDFMaxMeshSDFs)
  uint32_t _padding0;
  uint32_t _padding1;
  vec4 meshBoundsMin[LGlobalSDFMaxMeshSDFs];  // xyz = padded mesh AABB min, w unused
  vec4 meshBoundsMax[LGlobalSDFMaxMeshSDFs];  // xyz = padded mesh AABB max, w = mesh max encode distance
  vec4 meshTextureDimensions[LGlobalSDFMaxMeshSDFs]; // xyz = source voxel dimensions
};

// DDGI SDF ray trace (Wave D2-2). Per-frame uniform buffer instead of root
// constants: rotation matrix + volume params + lighting exceed the 128B push
// budget (same fallback as GlobalSDFComposeUniforms).
struct DDGIRayTraceUniforms
{
  vec4 rotationCol0;    // xyz = per-frame random rotation matrix column 0
  vec4 rotationCol1;    // xyz = column 1
  vec4 rotationCol2;    // xyz = column 2
  vec4 volumeBoundsMin; // xyz = global SDF world-space min, w = voxel size
  vec4 volumeBoundsMax; // xyz = global SDF world-space max, w = max encode distance
  vec4 sunDirection;    // xyz = light travel direction (light -> scene), w = max trace distance
  vec4 sunColor;        // xyz = sun radiance, w = sphere-march step scale
  vec4 skyColorAlbedo;  // xyz = constant miss/sky radiance, w = constant hit albedo
  // DDGI infinite bounce (Wave D4-1): SampleProbe volume constants so the hit
  // shading can sample LAST frame's probe atlases (read by parity, see
  // DDGIProbeVolume). Mirrors the LightParams ddgi* packing (Wave D3-1).
  vec4 ddgiGridOriginAndSpacing;  // xyz = probe grid world-space origin, w = probeSpacing
  vec4 ddgiGridDims;              // xyz = probe grid dims, w unused
  vec4 ddgiSampleParams0;         // x = normalBias, y = ddgiGamma, z = irradiance texel side, w = depth texel side
  vec4 ddgiSampleParams1;         // x = irr atlas width, y = irr atlas height, z = depth atlas width, w = depth atlas height
  uint64_t probePositionAddress;  // BDA of float4[totalProbes] probe world positions
  uint32_t raysPerProbe;
  uint32_t totalProbes;
  uint32_t maxSteps;    // sphere-march step budget (64-128)
  uint32_t resolution;  // global SDF voxel resolution (cubic, mip 0)
  uint32_t firstFrame;  // 1 = history atlases hold no valid data: skip probe
                        //     sampling, use the constant-sky indirect fallback
  uint32_t debugMode;   // 0 = normal shading, 1 = valid-hit albedo, 2 = branch colors
};

// DDGI probe irradiance/depth update (Wave D2-3). 96 bytes: fits the 128B
// root-constant budget, so this goes through setRootConstants (unlike
// DDGIRayTraceUniforms). Shared by the irradiance and depth update kernels;
// the border update kernels take no constants at all.
struct DDGIProbeUpdatePush
{
  vec4 rotationCol0;  // xyz = per-frame ray rotation column 0 — MUST equal the
  vec4 rotationCol1;  //       same-frame DDGIRayTraceUniforms rotation (ray
  vec4 rotationCol2;  //       directions are reconstructed, not stored)
  uint32_t raysPerProbe;
  uint32_t probesPerRow;  // atlas tiles per row = gridDims.x * gridDims.y
  uint32_t firstFrame;    // 1 = write results directly (skip history blend)
  float hysteresis;       // temporal blend: mix(new, history, hysteresis)
  float ddgiGamma;        // irradiance storage gamma (pow(x, 1/gamma) encode)
  float depthSharpness;   // depth weight exponent pow(dot, sharpness)
  float maxDistance;      // clamp for the stored ray-hit distance (depth only)
  // Staggered update (Wave D4-2): only probes with
  // probeIndex % updateStride == updateOffset recompute this frame; the rest
  // pass-through copy their history texels so both ping-pong atlases stay
  // complete. updateStride <= 1 degenerates to a full per-frame update.
  uint32_t updateOffset;
  uint32_t updateStride;
  uint32_t _ddgiProbeUpdatePadding0;
  uint32_t _ddgiProbeUpdatePadding1;
  uint32_t _ddgiProbeUpdatePadding2;
};

// DDGI probe visualization debug pass (Wave D3-2). Procedural UV-sphere
// tessellation shared by the C++ draw call (vertexCount = stacks*slices*6)
// and the shader-side SV_VertexID decoder.
STATIC_CONST int LDDGIProbeVisStacks = 12;
STATIC_CONST int LDDGIProbeVisSlices = 24;

// 160 bytes. C++ natural alignment matches the shader scalar/vector offsets
// (uint64 at offset 64); the final scalar block remains a 16-byte multiple.
struct DDGIProbeVisualizationUniforms
{
  mat4 viewProjection;            // jittered VP (matches the depth buffer)
  uint64_t probePositionAddress;  // BDA of float4[totalProbes] world positions
  uint32_t totalProbes;
  uint32_t irradianceAtlasWidth;  // full atlas extent in texels
  uint32_t irradianceAtlasHeight;
  uint32_t irradianceSideLength;  // irradiance texels per probe (no border)
  float probeRadius;              // debug sphere world-space radius (0.1)
  float ddgiGamma;                // decode: pow(x, gamma*0.5) then square
  uvec4 flaxProbeCountsAndMode;   // xyz = selected counts, w bits: Flax=1, active-only=2
  vec4 flaxProbeOriginAndSpacing; // xyz = snapped origin, w = probe spacing
  ivec4 flaxProbeScrollAndCascade;// xyz = scroll offset, w = selected cascade
  float debugScale;               // visualization-only radiance multiplier
  float debugChromaBoost;         // debug-only, luminance-preserving color separation
  float _probeVisPadding1;
  float _probeVisPadding2;
};

struct LightCullingUniforms
{
  vec4 screenSizeAndClipPlanes;  // xy = screen size, z = near plane, w = far plane
  mat4 viewMatrix;
  mat4 projectionMatrix;
  mat4 invProjectionMatrix;
};

struct ClusteredLightUniforms
{
  vec4 screenSizeAndClusterInfo;  // xy = render extent, z = clusterCountX, w = clusterCountY
  vec4 clusterZAndLightInfo;      // x = clusterCountZ, y = max lights/cluster, z = point count, w = spot count
  vec4 clipInfo;                  // x = near, y = far, z = z slicing scale, w = z slicing bias
  vec4 debugInfo;                 // x = enabled, y = heatmap, z = overflow highlight, w = fallback
  mat4 viewMatrix;
  mat4 projectionMatrix;
  mat4 invProjectionMatrix;
  mat4 invViewMatrix;
};

STATIC_CONST uint32_t LGPUCullingThreadCount       = 64;
STATIC_CONST uint32_t LGPUCullingFrustumPlaneCount = 6;

struct GPUCullObject
{
  vec4 sphereCenterRadius;  // xyz = world-space center, w = world-space radius
  uint32_t indexCount;
  uint32_t firstIndex;
  int32_t  vertexOffset;
  uint32_t flags;
};

struct GPUSceneObject
{
  vec4     worldMatrixRows[3];  // 48 bytes, row-major 3x4 affine transform
  vec4     boundsSphere;        // xyz = center, w = radius
  uint32_t materialIndex;
  uint32_t meshIndex;
  uint32_t flags;
  uint32_t _padding;
};

struct GPUSceneInfo
{
  uint64_t objectBufferAddress;
  uint64_t cullObjectBufferAddress;
  uint32_t objectCount;
  uint32_t _padding0;
};

struct GPUCullIndirectCommand
{
  uint32_t indexCount;
  uint32_t instanceCount;
  uint32_t firstIndex;
  int32_t  vertexOffset;
  uint32_t firstInstance;
};

struct ShadowCullObject
{
  vec4     sphereCenterRadius;
  uint32_t indexCount;
  uint32_t firstIndex;
  int32_t  vertexOffset;
  uint32_t firstInstance;
};

struct ShadowCullPushConstants
{
  vec4     frustumPlanes[LGPUCullingFrustumPlaneCount];
  uint32_t objectCount;
  uint32_t _padding0;
  uint32_t _padding1;
  uint32_t _padding2;
};

struct GPUCullStats
{
  uint32_t visibleCount;
  uint32_t frustumCulledCount;
  uint32_t occlusionCulledCount;
  uint32_t totalCount;
  uint32_t opaqueVisibleCount;
  uint32_t transparentVisibleCount;
  uint32_t opaqueCount;
  uint32_t transparentCount;
  uint32_t hizCandidateCount;
  uint32_t hizTestedCount;
  uint32_t hizRejectedLargeCount;
  uint32_t hizRejectedNearCount;
  uint32_t hizRejectedOffscreenCount;
  uint32_t meshletConeCulledCount;
  uint32_t _padding1;
  uint32_t _padding2;
};

struct GPUCullDrawCounts
{
  uint32_t opaqueCount;
  uint32_t alphaTestCount;
  uint32_t transparentCount;
  uint32_t totalCount;
};

struct GPUBatchBuildStats
{
  uint32_t visibleCount;
  uint32_t batchCount;
  uint32_t sortPassCount;
  uint32_t _padding0;
};

struct BitonicSortPushConstants
{
  uint32_t elementCount;
  uint32_t level;
  uint32_t levelMask;
  uint32_t descending;
};

struct TransparentVisibilityPatchPushConstants
{
  uint32_t elementCount;
  uint32_t categoryMask;
  uint32_t categoryValue;
  uint32_t outputOffset;
  uint32_t mode;
  uint32_t scanOffset;
  uint32_t scanBufferIndex;
  uint32_t _padding0;
};

struct Meshlet
{
  vec4     boundsSphere;
  vec4     coneAxisCutoff;
  uint32_t vertexOffset;
  uint32_t indexOffset;
  uint32_t indexCount;
  uint32_t materialIndex;
  uint32_t objectIndex;
  uint32_t flags;
  uint32_t localIndex;
  uint32_t _padding;
};

STATIC_CONST uint32_t LGPUCullFlagFrustumCulling = 0x1u;
STATIC_CONST uint32_t LGPUCullFlagOcclusionCulling = 0x2u;
STATIC_CONST uint32_t LGPUCullFlagTransparent = 0x4u;
STATIC_CONST uint32_t LGPUCullFlagAlphaMask = 0x8u;

STATIC_CONST uint32_t LGPUCullResultVisible = 0u;
STATIC_CONST uint32_t LGPUCullResultFrustumCulled = 1u;
STATIC_CONST uint32_t LGPUCullResultOcclusionCulled = 2u;
STATIC_CONST uint32_t LGPUCullResultConeCulled = 3u;

struct GPUCullingUniforms
{
  mat4 viewMatrix;
  mat4 projectionMatrix;
  mat4 viewProjectionMatrix;
  vec4 frustumPlanes[LGPUCullingFrustumPlaneCount];
  vec4 cameraRight;
  vec4 cameraUp;
  vec4 screenSizeAndPyramidSize;  // xy = screen size, zw = depth pyramid size
  vec4 cullingInfo;               // x = object count, y = mip count, z = use occlusion, w = depth epsilon
  vec4 cullingControls;           // x = enable frustum, y = enable occlusion, z = meshlet path, w = meshlet cone
  vec4 cameraPositionAndMeshletInfo;  // xyz = camera position, w = reserved
};

// ---------------------------------------------------------------------------
// Flax-style DDGI data (FGI-013). Cascade count capped at 4; layout mirrors
// Flax DDGIData with vec4-friendly alignment. Sampled by Flax-style DDGI
// compute passes and the cascaded probe sampling helper.
// CPU mirror: render/DDGIConfig.h or future FlaxDDGIData struct.
// ---------------------------------------------------------------------------
STATIC_CONST int LFlaxDDGIMaxCascades = 4;
STATIC_CONST int LFlaxDDGIMaxRadianceSources = 16;
STATIC_CONST uint32_t LFlaxRadianceSourcePoint = 0u;
STATIC_CONST uint32_t LFlaxRadianceSourceSpot = 1u;
STATIC_CONST uint32_t LFlaxRadianceSourceEmissive = 2u;

struct FlaxDDGIRadianceSource
{
  vec4 positionAndRange;  // xyz = world position, w = influence range
  vec4 colorAndType;      // rgb = emitted radiance, w = LFlaxRadianceSource*
  vec4 directionAndCone;  // xyz = spot travel direction / emissive normal, w = cos(outer cone)
  vec4 shapeAxisXAndHalfWidth;   // emissive rectangle only
  vec4 shapeAxisYAndHalfHeight;  // emissive rectangle only
};

struct FlaxDDGIData
{
  vec4 probesOriginAndSpacing[LFlaxDDGIMaxCascades];  // xyz = cascade origin, w = probe spacing
  vec4 blendOrigin[LFlaxDDGIMaxCascades];             // xyz = blend origin, w unused
  ivec4 probesScrollOffsets[LFlaxDDGIMaxCascades];     // xyz = cumulative ring offset, w unused
  ivec4 probesScrollDeltas[LFlaxDDGIMaxCascades];      // xyz = current-frame origin delta, w unused
  uvec3 probesCounts;                                   // grid resolution per cascade
  uint32_t cascadesCount;                               // active cascade count (1-4)
  float irradianceGamma;
  float probeHistoryWeight;
  float rayMaxDistance;
  float indirectLightingIntensity;
  vec3 viewPos;
  uint32_t raysCount;
  vec4 fallbackIrradiance;                              // rgb = fallback color, a unused
	vec4 sdfBoundsMinAndVoxel;                            // xyz = bounds min, w = voxel size
	vec4 sdfBoundsMaxAndRes;                              // xyz = bounds max, w = resolution as float
	vec4 sceneLightDirection;                              // xyz = direction to light, w unused
	vec4 sceneLightColor;                                  // rgb = light intensity, w unused
	uvec4 updateParams[LFlaxDDGIMaxCascades];              // x = regular offset, y = cascade budget,
	                                                        // z = priority offset, w = unlimited flag
	uvec4 radianceSourceParams;                             // x = valid source count
	vec4 environmentParams;                                 // x = environment valid, y = intensity
	FlaxDDGIRadianceSource radianceSources[LFlaxDDGIMaxRadianceSources];
};

// ---------------------------------------------------------------------------
// Flax-style Global Surface Atlas data (FGI-013).
// CPU mirror: future GlobalSurfaceAtlas pass or config struct.
// ---------------------------------------------------------------------------
STATIC_CONST int LGlobalSurfaceAtlasChunksResolution = 40;
STATIC_CONST int LGlobalSurfaceAtlasChunksGroupSize = 4;
STATIC_CONST int LGlobalSurfaceAtlasTileDataStride = 5;

struct GlobalSurfaceAtlasData
{
  vec3 viewPos;
  float padding0;
  float resolution;     // atlas texture resolution (square)
  float chunkSize;      // world-space chunk size for culling
  uint32_t objectsCount;
  uint32_t padding1;
  uint32_t padding2;
  uint32_t padding3;
};

struct SurfaceAtlasCullPush
{
  uint32_t objectCount;
  uint32_t chunkCount;
  float chunkSize;
  float atlasCoverageDistance;
  vec3 viewPos;
  uint32_t _padding;
};

struct SurfaceAtlasLightingPush
{
  vec4 atlasResolutionAndPadding; // xy = resolution, z = enabled, w unused
};

// Light parameters for PBR lighting pass (scene-level UBO)
struct LightParams
{
  mat4 worldToShadow[LCascadeCount];      // Per-cascade matrices
  vec4 cascadeSplitDistances;             // x=c0, y=c1, z=c2, w=c3 far distances
  vec4 lightDirectionAndShadowStrength;   // xyz = shading direction to light, w = shadow strength
  vec4 lightColorAndNormalBias;           // rgb = light intensity, w = normal bias
  vec4 ambientColorAndTexelSize;          // rgb = ambient term, w = 1 / shadow map size
  vec4 shadowMetrics;                     // x = texelSize, y = baseBias, z = slopeBias, w = cascadeCount
  vec4 iblParams;                         // x = enabled, y = intensity, z = max env mip, w = valid env texture
  vec4 iblDebugInfo;                      // x = debug mode, yzw = reserved
  vec4 phase7Info;                        // x = AO enabled, y = SSR enabled, zw = reserved until atlas sampling lands

  // DDGI lighting-pass sampling (Wave D3-1). Runtime gate only: when
  // ddgiGridDimsAndEnabled.w <= 0.5 (default zero-init) the lighting shader
  // takes the original ambient/IBL path with numerically identical results.
  // All-vec4 packing keeps the C++/std140 offsets trivially in sync; the BDA
  // address sits on a 16-byte boundary (same pattern as DDGIRayTraceUniforms).
  vec4 ddgiGridDimsAndEnabled;            // xyz = probe grid dims, w = enabled (1/0)
  vec4 ddgiOriginAndSpacing;              // xyz = probe grid world-space origin, w = probeSpacing
  vec4 ddgiParams0;                       // x = ddgiWeight, y = ddgiGamma, z = normalBias, w = irradiance texel side
  vec4 ddgiParams1;                       // x = depth texel side, y = irr atlas width, z = irr atlas height, w = depth atlas width
	vec4 ddgiParams2;                       // x = depth atlas height, y = FlaxGI debug mode, z = debug scale, w = reserved
	uint64_t ddgiProbePositionAddress;      // BDA of float4[totalProbes] probe positions (0 when disabled)
	uint32_t _ddgiLightPadding0;
	uint32_t _ddgiLightPadding1;

	// Flax-style DDGI sampling (FGI-061): packed into LightParams for the
	// lighting pass. Gated by ddgiFlaxEnabledAndCascades.x > 0.5.
	// z = 1 when Flax textures (data/distance/irradiance) are bound in
	// the bindless texture array; 0 = safe fallback to ambient/IBL.
	vec4 ddgiFlaxEnabledAndCascades;        // x = enabled, y = cascade count, z = textures bound, w = view bias
	vec4 ddgiFlaxOriginAndSpacing[4];       // xyz = cascade origin, w = spacing
	vec4 ddgiFlaxBlendOrigin[4];            // xyz = blend origin, w unused
	ivec4 ddgiFlaxScrollOffsets[4];
	uvec4 ddgiFlaxCountsAndRays;            // xyz = probesCounts, w = raysCount
	vec4 ddgiFlaxGammaWeightMaxDist;        // x = gamma, y = historyWeight, z = maxDist, w = intensity
	vec4 ddgiFlaxFallbackIrradiance;        // rgb = fallback, w = normal bias
	uvec4 ddgiFlaxRadianceSourceParams;      // x = valid source count
	FlaxDDGIRadianceSource ddgiFlaxRadianceSources[LFlaxDDGIMaxRadianceSources];
};

struct LightingUniforms
{
  LightParams light;
};

struct ShadowUniforms
{
  // Per-cascade matrices
  mat4 cascadeViewProjection[LCascadeCount];
  mat4 cascadeWorldToShadowTexture[LCascadeCount];

  // Cascade split distances (view-space depth)
  vec4 cascadeSplitDistances;  // x=c0 far, y=c1 far, z=c2 far, w=c3 far

  // Light parameters (unchanged)
  vec4 lightDirectionAndIntensity;

  // Shadow parameters
  vec4 shadowMapMetrics;  // x=1/shadowSize, y=maxShadowDistance, z=unused, w=cascadeCount

  // Per-cascade bias (scaled)
  vec4 cascadeBiasScale;  // x=baseConstantBias, y=baseSlopeBias, z=scaleFactor(0.5), w=normalBias
};

// Debug line vertex format
STATIC_CONST int LVDebugPosition = 0;
STATIC_CONST int LVDebugColor    = 1;

struct DebugLineVertex
{
  vec3 position;
  vec4 color;
};

struct PushConstantGPUCullDebug
{
  uint64_t objectBufferAddress;
  uint64_t resultBufferAddress;
  uint32_t objectCount;
  uint32_t segmentCount;
  uint32_t _padding0;
  uint32_t _padding1;
};

// Vertex with tangent for GBuffer pass
struct VertexGltfTangent
{
  vec3 position;
  vec3 normal;
  vec2 texCoord;
  vec4 tangent;  // xyz = tangent direction, w = handedness
};

// Push constant for GBuffer pass with PBR material params (legacy)
struct PushConstantGBuffer
{
  mat4 modelMatrix;
  mat4 viewProjectionMatrix;

  // Material factors
  vec4 baseColorFactor;
  float metallicFactor;
  float roughnessFactor;
  float normalScale;
  float occlusionStrength;
  vec3 emissiveFactor;
  float _padding;

  // Texture indices (bindless)
  uint32_t baseColorTextureIndex;
  uint32_t metallicRoughnessTextureIndex;
  uint32_t normalTextureIndex;
  uint32_t occlusionTextureIndex;
  uint32_t emissiveTextureIndex;

  // Flags (0/1)
  uint32_t hasBaseColorTexture;
  uint32_t hasMetallicRoughnessTexture;
  uint32_t hasNormalTexture;
  uint32_t hasOcclusionTexture;
  uint32_t hasEmissiveTexture;
  uint32_t _padding2[3];
};


#endif  // HOST_DEVICE_H
