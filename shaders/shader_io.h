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
  vec3 cameraPosition;
  float _padding;
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

// Light types
STATIC_CONST int LLightTypeDirectional = 0;
STATIC_CONST int LLightTypePoint       = 1;
STATIC_CONST int LLightTypeSpot        = 2;

// Single light data (64 bytes aligned)
struct LightData
{
  vec3 positionOrDirection;  // Direction for directional, position for point/spot
  float intensity;           // Light intensity multiplier
  vec3 color;                // RGB light color
  float range;               // Point/spot light range (unused for directional)
  vec3 spotDirection;        // Spot light direction (unused for point/directional)
  float spotInnerAngle;      // Spot light inner cone angle (cos)
  uint32_t lightType;        // 0=directional, 1=point, 2=spot
  float spotOuterAngle;      // Spot light outer cone angle (cos)
  float _padding[2];
};

// Light list uniform buffer
struct LightListUniforms
{
  uint32_t numLights;
  uint32_t numDirectionalLights;
  uint32_t numPointLights;
  uint32_t numSpotLights;
  vec3 ambientColor;
  float _padding;
};

// Tile/Cluster culling config
STATIC_CONST int LTileSizeX = 16;
STATIC_CONST int LTileSizeY = 16;
STATIC_CONST int LMaxLightsPerTile = 32;

// IBL texture indices (for bindless access)
STATIC_CONST int kIBLPrefilteredMapIndex = 10;  // Environment cube map
STATIC_CONST int kIBLDFGLUTIndex = 11;           // DFG LUT texture
STATIC_CONST int kIBLIrradianceMapIndex = 12;   // Irradiance cube map

// Light parameters for PBR lighting pass (push constants)
struct LightParams
{
  vec3 lightDirection;      // Direction TO light source (normalized)
  float _pad0;              // Padding for vec4 alignment
  vec3 lightColor;          // RGB light intensity
  float _pad1;              // Padding for vec4 alignment
  vec3 ambientColor;        // Ambient contribution
  int debugMode;            // 0=normal, 1=shadow, 2=cascade, 3=depth, 4=lightSpaceXY, 5=lightSpaceZ
};

// Vertex with tangent for GBuffer pass
struct VertexGltfTangent
{
  vec3 position;
  vec3 normal;
  vec2 texCoord;
  vec4 tangent;  // xyz = tangent direction, w = handedness
};

struct ShadowUniforms
{
  mat4 lightViewProjectionMatrix;   // Light clip-space matrix for the shadow pass
  mat4 worldToShadowTextureMatrix;  // World-space -> shadow UV/depth in [0, 1]
  vec4 lightDirectionAndIntensity;  // xyz = light direction FROM light TO scene, w = shadow intensity
  vec4 shadowMapMetrics;            // x = texel size, y = max shadow distance, z = receiver bias, w = reserved
  vec4 shadowBiasAndKernel;         // x = depth bias constant, y = depth bias slope, z = map size, w = PCF radius
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
