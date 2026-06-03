#pragma once

// Shared render data contracts (RenderParams, GPUDrivenSceneView, scene upload
// results, debug options, etc.). Split out of RenderDevice.h so passes and other
// consumers can depend on the data types without pulling in the full device header.

#include "../common/Common.h"
#include "../common/Handles.h"
#include "../shaders/shader_io.h"
#include "../rhi/RHITypes.h"
#include "../rhi/RHICommandList.h"
#include "../scene/SceneLight.h"
#include "../scene/SceneAsset.h"
#include "../loader/GltfLoader.h"
#include "Pass.h"

#include <array>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <span>
#include <string>
#include <vector>

namespace demo {

struct SceneUploadResult;

struct DirectionalLightSettings
{
  // World-space light travel direction, from the light toward the scene.
  glm::vec3 direction{glm::normalize(glm::vec3(-0.45f, -0.8f, -0.25f))};
  float     shadowDistance{100.0f};
  glm::vec3 color{glm::vec3(1.0f, 0.95f, 0.85f) * 3.0f};
  float     shadowStrength{0.9f};
  glm::vec3 ambient{0.1f, 0.12f, 0.15f};
  float     normalBias{0.0025f};
  float     depthBias{0.0015f};
};

struct DebugPassOptions
{
  bool  enabled{true};
  bool  showSceneBounds{true};
  bool  showShadowFrustum{true};
  bool  showViewFrustum{false};
  bool  showLightDirection{true};
  bool  showPointLights{true};
  bool  enablePointLights{true};
  bool  showViewportAxis{true};
  bool  showLightCoarseCullingHeatmap{false};
  bool  enableClusteredLighting{true};
  bool  showClusteredLightingHeatmap{false};
  bool  showClusteredLightingOverflow{false};
  bool  showGPUCullingOverlay{false};
  bool  showPassGpuProfile{true};
  bool  enableGPUFrustumCulling{true};
  bool  enableGPUOcclusionCulling{true};
  bool  enableGPUMeshletOcclusionCulling{false};
  bool  enableGPUMeshletConeCulling{true};
  bool  showCullDistance{false};
  float cullDistance{25.0f};
  float pointLightMaxRadius{4.0f};
  float pointLightIntensityScale{4.0f};
  bool  enablePostProcessing{true};
  bool  enableBloom{true};
  bool  enableAdaptiveExposure{false};
  bool  enableColorGrading{true};
  bool  enableLensEffects{false};
  bool  enableTAA{true};
  bool  enableIBL{true};
  bool  showVelocity{false};
  int   upscalingMode{1};  // 0=Off, 1=TAA, 2=Spatial fallback
  float postExposure{1.0f};
  float bloomIntensity{0.35f};
  float bloomThreshold{0.0f};
  float taaJitterScale{1.0f};
  float taaBlendWeight{0.90f};
  float renderScale{1.0f};
  float exposureTargetLuminance{0.18f};
  float minAutoExposure{0.25f};
  float maxAutoExposure{4.0f};
  float colorSaturation{1.0f};
  float colorContrast{1.0f};
  float colorGamma{1.0f};
  float colorLutStrength{0.35f};
  float vignetteIntensity{0.15f};
  float lensDirtIntensity{0.0f};
  float iblIntensity{1.0f};
  int   iblDebugMode{0};  // 0=Off, 1=Diffuse, 2=Specular, 3=Fallback ambient, 4=Environment
  bool  enableAO{true};
  float aoRadius{12.0f};
  float aoIntensity{1.0f};
  bool  enableSSR{false};
  int   ssrMaxSteps{32};
  float ssrThickness{0.03f};
  bool  enableShadowAtlas{false};

  // CSM Shadow cascade debug visualization
  bool  showShadowCascades{true};      // Show cascade frustum splits
  int   cascadeIndex{-1};              // -1 = all cascades, 0-3 = specific cascade
  bool  cascadeOverlayMode{false};     // Screen-space cascade overlay
  float cascadeOverlayAlpha{0.25f};    // Overlay transparency
};

enum class GPUDrivenSceneAuthority : uint32_t
{
  none = 0,
  persistentCullObjects = 1,
  futureSceneObjects = 2,
};

enum class GPUDrivenIndirectSourceKind : uint32_t
{
  none = 0,
  gpuCullingOpaqueIndirect = 1,
};

struct ShadowPackedMesh
{
  size_t   meshIndex{0};
  uint32_t indexCount{0};
  uint32_t firstIndex{0};
  int32_t  vertexOffset{0};
  glm::vec4 boundsSphere{0.0f};
  shaderio::DrawUniforms drawData{};
};

struct GPUDrivenSceneView
{
  // Future phases will promote this buffer to be the authoritative scene-object source.
  uint64_t                           gpuSceneObjectBufferAddress{0};
  // Phase 1 authoritative object source for GPU culling and indirect command generation.
  uint64_t                           gpuCullObjectBufferAddress{0};
  VkBuffer                           gpuCullObjectBuffer{VK_NULL_HANDLE};
  VkBuffer                           gpuCullMeshletBuffer{VK_NULL_HANDLE};
  VkBuffer                           gpuCullSceneObjectBuffer{VK_NULL_HANDLE};
  uint32_t                           objectCount{0};
  const shaderio::GPUCullObject*     overlayObjects{nullptr};
  uint32_t                           overlayObjectCount{0};
  bool                               usePersistentCullingObjects{false};
  GPUDrivenSceneAuthority            authority{GPUDrivenSceneAuthority::none};
  GPUDrivenIndirectSourceKind        indirectSource{GPUDrivenIndirectSourceKind::none};
  uint32_t                           indirectCommandStride{0};
  const MeshHandle*                  meshHandles{nullptr};
  uint32_t                           meshHandleCount{0};
  const MeshHandle*                  drawMeshHandles{nullptr};
  uint32_t                           drawMeshHandleCount{0};
  const size_t*                      shadowCasterMeshIndices{nullptr};
  uint32_t                           shadowCasterCount{0};
  VkBuffer                           shadowPackedVertexBuffer{VK_NULL_HANDLE};
  VkBuffer                           shadowPackedIndexBuffer{VK_NULL_HANDLE};
  const ShadowPackedMesh*            shadowPackedMeshes{nullptr};
  uint32_t                           shadowPackedMeshCount{0};
  glm::vec3                          sceneBoundsMin{0.0f};
  glm::vec3                          sceneBoundsMax{0.0f};
  bool                               sceneBoundsValid{false};
  VkFormat                           sceneDepthFormat{VK_FORMAT_UNDEFINED};
  VkImage                            sceneDepthImage{VK_NULL_HANDLE};
  rhi::TextureViewHandle             sceneDepthView{};
  VkExtent2D                         sceneDepthExtent{};
  std::array<VkImage, kPackedGBufferTargetCount>     gbufferImages{};
  std::array<rhi::TextureViewHandle, kPackedGBufferTargetCount> gbufferViews{};
  VkImage                            outputImage{VK_NULL_HANDLE};
  rhi::TextureViewHandle             outputView{};
  VkImage                            sceneColorHdrImage{VK_NULL_HANDLE};
  rhi::TextureViewHandle             sceneColorHdrView{};
  VkImage                            bloomHalfImage{VK_NULL_HANDLE};
  rhi::TextureViewHandle             bloomHalfView{};
  VkExtent2D                         bloomHalfExtent{};
  VkImage                            bloomQuarterImage{VK_NULL_HANDLE};
  rhi::TextureViewHandle             bloomQuarterView{};
  VkExtent2D                         bloomQuarterExtent{};
  VkImage                            bloomEighthImage{VK_NULL_HANDLE};
  rhi::TextureViewHandle             bloomEighthView{};
  VkExtent2D                         bloomEighthExtent{};
  VkImage                            bloomSixteenthImage{VK_NULL_HANDLE};
  rhi::TextureViewHandle             bloomSixteenthView{};
  VkExtent2D                         bloomSixteenthExtent{};
  VkImage                            bloomThirtySecondImage{VK_NULL_HANDLE};
  rhi::TextureViewHandle             bloomThirtySecondView{};
  VkExtent2D                         bloomThirtySecondExtent{};
  VkImage                            bloomUpsampleSixteenthImage{VK_NULL_HANDLE};
  rhi::TextureViewHandle             bloomUpsampleSixteenthView{};
  VkExtent2D                         bloomUpsampleSixteenthExtent{};
  VkImage                            bloomUpsampleEighthImage{VK_NULL_HANDLE};
  rhi::TextureViewHandle             bloomUpsampleEighthView{};
  VkExtent2D                         bloomUpsampleEighthExtent{};
  VkImage                            bloomUpsampleQuarterImage{VK_NULL_HANDLE};
  rhi::TextureViewHandle             bloomUpsampleQuarterView{};
  VkExtent2D                         bloomUpsampleQuarterExtent{};
  VkImage                            bloomOutputImage{VK_NULL_HANDLE};
  rhi::TextureViewHandle             bloomOutputView{};
  VkExtent2D                         bloomOutputExtent{};
  VkImage                            colorGradingLutImage{VK_NULL_HANDLE};
  rhi::TextureViewHandle             colorGradingLutView{};
  VkExtent2D                         colorGradingLutExtent{};
  VkImage                            velocityImage{VK_NULL_HANDLE};
  rhi::TextureViewHandle             velocityView{};
  VkImage                            sceneColorHistoryReadImage{VK_NULL_HANDLE};
  rhi::TextureViewHandle             sceneColorHistoryReadView{};
  VkImage                            sceneColorHistoryWriteImage{VK_NULL_HANDLE};
  rhi::TextureViewHandle             sceneColorHistoryWriteView{};
  VkImage                            depthPyramidImage{VK_NULL_HANDLE};
  const rhi::TextureViewHandle*      depthPyramidMipViews{nullptr};
  uint32_t                           depthPyramidMipCount{0};
  TextureHandle                      depthPyramidSourceDepth{};
  uint64_t                           depthPyramidGeneration{0};
  bool                               depthPyramidValid{false};
};

struct RenderParams
{
  rhi::Extent2D                          viewportSize{};
  float                                  deltaTime{0.0F};
  float                                  timeSeconds{0.0F};
  MaterialHandle                         materialHandle{};
  rhi::ClearColorValue                   clearColor{0.2F, 0.2F, 0.3F, 1.0F};
  std::function<void(rhi::CommandList&)> recordUi;
  glm::vec4                              viewportImageRect{0.0f};  // x, y, width, height in ImGui screen space
  // glTF model data for rendering
  const SceneUploadResult*               gltfModel{nullptr};
  // Camera data (pointer to App-owned CameraUniforms)
  const shaderio::CameraUniforms*       cameraUniforms{nullptr};
  DirectionalLightSettings               lightSettings{};
  std::span<const SceneLight>             sceneLights{};
  std::span<const SceneNode>              sceneLightSceneNodes{};
  std::span<const GltfNodeData>           sceneLightGltfNodes{};
  DebugPassOptions                       debugOptions{};
  const GPUDrivenSceneView*              gpuDrivenSceneView{nullptr};
};

struct SceneUploadResult
{
  struct SceneDrawRecord
  {
    uint32_t        instanceIndex{UINT32_MAX};
    uint32_t        meshIndex{UINT32_MAX};
    uint32_t        materialIndex{UINT32_MAX};
    MeshHandle      meshHandle{kNullMeshHandle};
    MaterialHandle  materialHandle{kNullMaterialHandle};
    glm::mat4       worldTransform{1.0f};
    glm::vec4       boundsSphere{0.0f};
    uint32_t        alphaMode{0u};
    float           alphaCutoff{0.5f};
  };

  std::vector<MeshHandle>     meshes;
  std::vector<MaterialHandle> materials;
  std::vector<TextureHandle>  textures;
  std::vector<SceneDrawRecord> drawRecords;
  std::vector<uint32_t>        instanceToDrawRecord;
  std::vector<uint32_t>        drawCommandToDrawRecord;
  std::vector<MeshHandle>      drawMeshHandles;
  std::vector<size_t>          opaqueDrawIndices;
  std::vector<size_t>          alphaTestDrawIndices;
  std::vector<size_t>          transparentDrawIndices;
  std::vector<size_t>          shadowCasterDrawIndices;
  std::vector<float>           transparentDrawDistances;

  // Pre-built mesh lists for each pass type
  std::vector<size_t> opaqueMeshIndices;        // Indices into meshes for OPAQUE
  std::vector<size_t> alphaTestMeshIndices;     // Indices into meshes for MASK
  std::vector<size_t> transparentMeshIndices;   // Indices into meshes for BLEND
  std::vector<size_t> shadowCasterIndices;      // OPAQUE + MASK (skip BLEND)

  // Cached sorting data for transparent meshes
  std::vector<float> transparentDistances;      // Distance from last camera position
  glm::vec3 lastSortCameraPos{0.0f};           // Camera position used for last sort
  bool transparentSortDirty{true};              // Force re-sort on first frame

  utils::Buffer                 shadowPackedVertexBuffer{};
  utils::Buffer                 shadowPackedIndexBuffer{};
  std::vector<demo::ShadowPackedMesh> shadowPackedMeshes;
};

using GltfUploadResult = SceneUploadResult;

struct RuntimeProfileSnapshot
{
  std::vector<std::string> passNames;
  std::vector<double>      cpuPassDurationsMs;
  std::vector<double>      gpuPassDurationsMs;
  bool                     gpuValid{false};
};

struct GPUCullOverlayObject
{
  glm::vec3 center{0.0f};
  float     radius{0.0f};
  uint32_t  flags{0u};
  uint32_t  result{shaderio::LGPUCullResultVisible};
};

}  // namespace demo
