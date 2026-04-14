#ifndef HOST_DEVICE_H
#define HOST_DEVICE_H

#ifdef __SLANG__
typealias vec2 = float2;
typealias vec3 = float3;
typealias vec4 = float4;
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
using mat4 = glm::mat4;
#define STATIC_CONST static const
#endif

// Layout constants
// Set 0: Pass-global textures (bindless)
STATIC_CONST int LSetTextures  = 0;
STATIC_CONST int LBindTextures = 0;

// Set 1: Scene-level uniform buffers
STATIC_CONST int LSetScene      = 1;
STATIC_CONST int LBindSceneInfo = 0;      // SceneInfo for compute
STATIC_CONST int LBindCamera    = 1;      // Camera uniform buffer

// Set 2: Draw-level dynamic uniforms
STATIC_CONST int LSetDraw       = 2;
STATIC_CONST int LBindDrawModel = 0;      // Per-draw model matrix

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
  vec3 cameraPosition;
  float shadowConstantBias;
  vec4 shadowDirectionAndSlopeBias;
};

// Per-draw uniform buffer (dynamic)
struct DrawUniforms
{
  mat4 modelMatrix;
  vec4 baseColorFactor;
  int32_t baseColorTextureIndex;   // Bindless texture index, -1 = no texture
  int32_t normalTextureIndex;      // -1 = no texture
  int32_t metallicRoughnessTextureIndex;  // -1 = no texture
  int32_t occlusionTextureIndex;   // -1 = no texture
  float metallicFactor;
  float roughnessFactor;
  float normalScale;
  int32_t alphaMode;      // 0=OPAQUE, 1=MASK, 2=BLEND
  float alphaCutoff;      // Default 0.5, used for MASK mode
  float _padding[2];
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

// Light parameters for PBR lighting pass (push constants)
struct LightParams
{
  mat4 worldToShadow[LCascadeCount];      // Per-cascade matrices
  vec4 cascadeSplitDistances;             // x=c0, y=c1, z=c2, w=c3 far distances
  vec4 lightDirectionAndShadowStrength;   // xyz = shading direction to light, w = shadow strength
  vec4 lightColorAndNormalBias;           // rgb = light intensity, w = normal bias
  vec4 ambientColorAndTexelSize;          // rgb = ambient term, w = 1 / shadow map size
  vec4 shadowMetrics;                     // x = texelSize, y = baseBias, z = slopeBias, w = cascadeCount
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
