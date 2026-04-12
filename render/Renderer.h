#pragma once

#include "../common/Common.h"
#include "../common/Handles.h"
#include "../common/HandlePool.h"
#include "BindGroups.h"
#include "DrawStream.h"
#include "DrawStreamDecoder.h"
#include <memory>
#include "PassExecutor.h"
#include "passes/AnimateVerticesPass.h"
#include "passes/SceneOpaquePass.h"
#include "passes/PresentPass.h"
#include "passes/ImguiPass.h"
#include "passes/GBufferPass.h"
#include "passes/LightPass.h"
#include "passes/ForwardPass.h"
#include "passes/LightCullingPass.h"
#include "passes/ShadowPass.h"
#include "passes/ShadowDebugPass.h"
#include "MeshPool.h"
#include "../loader/GltfLoader.h"
#include "SceneResources.h"
#include "LightResources.h"
#include "IBLResources.h"
#include "ShadowResources.h"
#include "TransientAllocator.h"
#include "../rhi/RHICommandList.h"
#include "../rhi/RHIFrameContext.h"
#include "../rhi/RHIDevice.h"
#include "../rhi/RHIBindlessTypes.h"
#include "../rhi/RHIPipeline.h"
#include "../rhi/RHISwapchain.h"
#include "../rhi/RHISurface.h"
#include <functional>
#include <optional>
#include <span>
#include <unordered_map>

namespace demo {
namespace rhi {
namespace vulkan {
class VulkanBindTableLayout;  // Forward declaration for shared layout ownership
}
}

namespace rhi {
struct BindTableWrite;
}

struct GltfUploadResult;

struct RenderParams
{
  rhi::Extent2D                          viewportSize{};
  float                                  deltaTime{0.0F};
  float                                  timeSeconds{0.0F};
  MaterialHandle                         materialHandle{};
  rhi::ClearColorValue                   clearColor{0.2F, 0.2F, 0.3F, 1.0F};
  std::function<void(rhi::CommandList&)> recordUi;
  // glTF model data for rendering
  const GltfUploadResult*                gltfModel{nullptr};
  // Camera data (pointer to App-owned CameraUniforms)
  const shaderio::CameraUniforms*       cameraUniforms{nullptr};
  // Light direction for shadow pass (normalized, pointing FROM light TO scene)
  glm::vec3                              lightDirection{0.0f, -1.0f, 0.0f};  // Default: from above
  // Shadow debug mode: 0=normal, 1=shadow factor, 2=shadow UV, 3=view depth, 4=light clip XY, 5=shadow depth
  int                                    shadowDebugMode{0};
};

struct GltfUploadResult
{
  std::vector<MeshHandle>     meshes;
  std::vector<MaterialHandle> materials;
  std::vector<TextureHandle>  textures;
};

class Renderer
{
public:
  static constexpr uint32_t kDemoMaterialSlotCount = 2;

  enum class GraphicsPipelineVariant : uint32_t
  {
    nonTextured = 0,
    textured    = 1,
    light       = 2,
  };

  Renderer() = default;

  void init(GLFWwindow* window, rhi::Surface& surface, bool vSync);
  void shutdown(rhi::Surface& surface);
  void requestSwapchainRebuild()
  {
    if(m_swapchainDependent.swapchain)
    {
      m_swapchainDependent.swapchain->requestRebuild();
    }
  }
  void resize(rhi::Extent2D size);
  void render(const RenderParams& params);

  // Pass execution helpers (wrappers for per-pass commands)
  void executeComputePass(rhi::CommandList& cmd, const RenderParams& params) const;
  void executeGraphicsPass(rhi::CommandList& cmd, const RenderParams& params, std::span<const StreamEntry> drawStream);
  void executeImGuiPass(rhi::CommandList& cmd, const RenderParams& params);
  void beginPresentPass(rhi::CommandList& cmd);
  void endPresentPass(rhi::CommandList& cmd);
  uint32_t           allocateDrawDynamicOffset(rhi::ResourceIndex materialIndex, const RenderParams& params);
  rhi::ResourceIndex resolveMaterialResourceIndex(MaterialHandle handle) const;
  [[nodiscard]] rhi::ResourceIndex getSceneBindlessResourceIndex() const { return kSceneBindlessInfoIndex; }

  TextureHandle  getViewportTextureHandle() const;
  ImTextureID    getViewportTextureID(TextureHandle handle) const;
  MaterialHandle getMaterialHandle(uint32_t slot) const;
  PipelineHandle getGraphicsPipelineHandle(GraphicsPipelineVariant variant) const;

  // glTF model support
  GltfUploadResult uploadGltfModel(const GltfModel& model, VkCommandBuffer cmd);
  void             destroyGltfResources(const GltfUploadResult& result);

  // Execute upload commands with internal command buffer management
  void executeUploadCommand(std::function<void(VkCommandBuffer)> uploadFn);

  MeshPool& getMeshPool() { return m_meshPool; }
  SceneResources& getSceneResources() { return m_swapchainDependent.sceneResources; }
  IBLResources& getIBLResources() { return m_iblResources; }
  ShadowResources& getShadowResources() { return m_shadowResources; }
  void      waitForIdle();

  // LightPass support
  PipelineHandle getLightPipelineHandle() const;
  PipelineHandle getGBufferOpaquePipelineHandle() const;
  PipelineHandle getGBufferAlphaTestPipelineHandle() const;
  PipelineHandle getForwardPipelineHandle() const;
  uint64_t       getLightPipelineLayout() const;
  uint64_t       getGraphicsPipelineLayout() const;  // Graphics pipeline layout for descriptor binding
  uint64_t       getGBufferPipelineLayout() const;   // GBuffer-specific pipeline layout
  uint64_t       getGBufferColorDescriptorSet() const;  // Material bindless texture array
  uint64_t       getGBufferTextureDescriptorSet() const; // GBuffer textures for LightPass
  uint64_t       getPipelineOpaque(PipelineHandle handle, uint32_t expectedBindPoint) const;

  // ShadowPass support
  PipelineHandle getShadowPipelineOpaqueHandle() const;
  PipelineHandle getShadowPipelineAlphaTestHandle() const;
  uint64_t       getShadowPipelineLayout() const;
  shaderio::ShadowUniforms* getShadowUniformsData();  // CPU-side shadow uniforms for debug visualization
  uint64_t       getShadowUniformsDescriptorSet() const;  // GPU descriptor set for shadow uniforms UBO
  VkImageView    getShadowMapView() const;  // For ImGui visualization

  // LightCullingPass support
  PipelineHandle getLightCullingPipelineHandle() const;
  uint64_t       getLightCullingPipelineLayout() const;
  uint64_t       getLightCullingDescriptorSet() const;

  // DebugLinePass support
  PipelineHandle getDebugLinePipelineHandle() const;

  // Get descriptor set from bind group (for descriptor set binding)
  uint64_t getBindGroupDescriptorSet(BindGroupHandle handle, BindGroupSetSlot slot) const {
      return getBindGroupDescriptorSetOpaque(handle, slot);
  }

  // Per-frame bind group accessors for dynamic uniform buffers
  BindGroupHandle getCameraBindGroup(uint32_t frameIndex) const;
  BindGroupHandle getDrawBindGroup(uint32_t frameIndex) const;

  // BindGroup creation (new RHI interface)
  rhi::BindGroupLayoutHandle createBindGroupLayout(const rhi::BindGroupLayoutDesc& desc);
  rhi::BindGroupHandle createBindGroup(const rhi::BindGroupDesc& desc);
  void destroyBindGroupLayout(rhi::BindGroupLayoutHandle handle);
  void destroyBindGroup(rhi::BindGroupHandle handle);

  // Get material baseColorFactor and texture info for glTF rendering
  glm::vec4 getMaterialBaseColorFactor(MaterialHandle handle) const;
  int32_t getMaterialBaseColorTextureIndex(MaterialHandle materialHandle, const GltfUploadResult* gltfModel) const;

  // Material texture indices struct for GBuffer rendering
  struct MaterialTextureIndices {
    int32_t baseColor = -1;
    int32_t normal = -1;
    int32_t metallicRoughness = -1;
    int32_t occlusion = -1;
    float metallicFactor = 1.0f;
    float roughnessFactor = 1.0f;
    float normalScale = 1.0f;
    int32_t alphaMode = 0;    // 0=OPAQUE, 1=MASK, 2=BLEND
    float alphaCutoff = 0.5f;
  };
  MaterialTextureIndices getMaterialTextureIndices(MaterialHandle materialHandle, const GltfUploadResult* gltfModel) const;

  // RHI accessors (replacing native accessors)
  rhi::TextureViewHandle getCurrentSwapchainView() const;
  rhi::TextureViewHandle getGBufferView(uint32_t index) const;
  rhi::TextureViewHandle getDepthView() const;
  rhi::BindGroupHandle getGlobalBindlessGroup() const;

  void updateBindlessTexture(uint32_t index, TextureHandle textureHandle);
  // Get the base index for glTF textures in the bindless array
  static constexpr uint32_t getGltfTextureBaseIndex() { return kDemoMaterialSlotCount; }

  VkExtent2D getSwapchainExtent() const { return m_swapchainDependent.windowSize; }
  VkImageView getCurrentSwapchainImageView() const;
  VkImage getCurrentSwapchainImage() const;
  VkImageView getOutputTextureView() const;
  VkImageView getDepthTextureView() const;
  uint64_t    getDeviceOpaque() const { return m_device.device ? m_device.device->getNativeDevice() : 0; }

private:
  class SamplerCache
  {
  public:
    SamplerCache() = default;
    ~SamplerCache() { assert(m_device == VK_NULL_HANDLE && "Missing deinit()"); }

    void init(VkDevice device) { m_device = device; }
    void deinit()
    {
      for(const auto& entry : m_samplerMap)
      {
        vkDestroySampler(m_device, entry.second, nullptr);
      }
      m_samplerMap.clear();
      m_device = VK_NULL_HANDLE;
    }
    VkSampler acquireSampler(const VkSamplerCreateInfo& createInfo)
    {
      if(auto it = m_samplerMap.find(createInfo); it != m_samplerMap.end())
      {
        return it->second;
      }

      VkSampler sampler{VK_NULL_HANDLE};
      VK_CHECK(vkCreateSampler(m_device, &createInfo, nullptr, &sampler));
      m_samplerMap[createInfo] = sampler;
      return sampler;
    }

  private:
    struct SamplerCreateInfoHash
    {
      std::size_t operator()(const VkSamplerCreateInfo& info) const
      {
        std::size_t seed{0};
        seed = utils::hashCombine(seed, info.magFilter);
        seed = utils::hashCombine(seed, info.minFilter);
        seed = utils::hashCombine(seed, info.mipmapMode);
        seed = utils::hashCombine(seed, info.addressModeU);
        seed = utils::hashCombine(seed, info.addressModeV);
        seed = utils::hashCombine(seed, info.addressModeW);
        seed = utils::hashCombine(seed, info.mipLodBias);
        seed = utils::hashCombine(seed, info.anisotropyEnable);
        seed = utils::hashCombine(seed, info.maxAnisotropy);
        seed = utils::hashCombine(seed, info.compareEnable);
        seed = utils::hashCombine(seed, info.compareOp);
        seed = utils::hashCombine(seed, info.minLod);
        seed = utils::hashCombine(seed, info.maxLod);
        seed = utils::hashCombine(seed, info.borderColor);
        seed = utils::hashCombine(seed, info.unnormalizedCoordinates);
        return seed;
      }
    };

    struct SamplerCreateInfoEqual
    {
      bool operator()(const VkSamplerCreateInfo& lhs, const VkSamplerCreateInfo& rhs) const
      {
        return std::memcmp(&lhs, &rhs, sizeof(VkSamplerCreateInfo)) == 0;
      }
    };

    VkDevice m_device{VK_NULL_HANDLE};
    std::unordered_map<VkSamplerCreateInfo, VkSampler, SamplerCreateInfoHash, SamplerCreateInfoEqual> m_samplerMap;
  };

  // Created during Renderer::init() after feature negotiation.
  // Destroyed during Renderer::shutdown() after vkDeviceWaitIdle.
  // Rebuild trigger: none while device is alive; recreated only on full renderer/device re-init.
  struct DeviceLifetimeResources
  {
    struct PipelineRecord
    {
      uint32_t bindPoint{0};
      uint64_t nativePipeline{0};
      uint32_t specializationVariant{0};
    };

    std::unique_ptr<rhi::Device> device;
    VmaAllocator                 allocator{nullptr};
    std::vector<utils::Buffer>   stagingBuffers;
    SamplerCache                 samplerPool;

    utils::Buffer                              vertexBuffer;
    utils::Buffer                              pointsBuffer;
    VkCommandPool                              transientCmdPool{};
    VkDescriptorPool                           descriptorPool{};
    VkDescriptorPool                           uiDescriptorPool{};
    VkDescriptorSetLayout                      gbufferTextureSetLayout{nullptr};
    VkDescriptorSet                            gbufferTextureSet{nullptr};
    VkPipelineLayout                           lightPipelineLayout{nullptr};
    std::unique_ptr<rhi::PipelineLayout>       graphicPipelineLayout;
    std::unique_ptr<rhi::PipelineLayout>       computePipelineLayout;
    std::unique_ptr<rhi::PipelineLayout>       gbufferPipelineLayout;  // Separate layout for GBuffer pass
    std::unique_ptr<rhi::PipelineLayout>       shadowPipelineLayout;   // Shadow pass layout (depth-only)
    std::unique_ptr<rhi::PipelineLayout>       lightCullingPipelineLayout;  // Compute pipeline layout
    VkDescriptorSetLayout                      shadowUniformsSetLayout{nullptr};  // Shadow uniforms UBO layout
    VkDescriptorSet                            shadowUniformsDescriptorSet{VK_NULL_HANDLE};  // Shadow uniforms UBO set
    VkDescriptorSetLayout                      lightCullingSetLayout{nullptr};
    HandlePool<PipelineHandle, PipelineRecord> pipelineRegistry;

    struct PrebuiltPipelineVariants
    {
      PipelineHandle graphicsTextured{};
      PipelineHandle graphicsNonTextured{};
      PipelineHandle compute{};
    } prebuiltPipelines;
  };

  // Created during init(), reinitialized whenever swapchain indicates rebuild, and destroyed before device teardown.
  // Rebuild trigger: WSI/surface resize, suboptimal/out-of-date acquire or explicit requestSwapchainRebuild().
  struct SwapchainDependentResources
  {
    std::unique_ptr<rhi::Swapchain> swapchain;
    SceneResources                  sceneResources;
    VkExtent2D                      windowSize{800, 600};
    VkExtent2D                      viewportSize{800, 600};
    VkFormat                        swapchainImageFormat{VK_FORMAT_B8G8R8A8_UNORM};
    uint32_t                        currentImageIndex{0};
    std::vector<rhi::ResourceState> imageStates;  // Track per-image layout state
    bool                            vSync{true};
  };

  // Created after swapchain frame-count is known.
  // Reset/reuse trigger: every frame-ring iteration waits timeline then resets that slot's command pool.
  // Recreated only if frame-count policy changes (future task) or full renderer re-init.
  struct PerFrameResources
  {
    std::unique_ptr<rhi::FrameContext> frameContext;

    struct FrameUserData
    {
      TransientAllocator transientAllocator{};
      BindGroupHandle    sceneBindGroup{kNullBindGroupHandle};
      BindGroupHandle    cameraBindGroup{kNullBindGroupHandle};
      BindGroupHandle    drawBindGroup{kNullBindGroupHandle};
    };

    std::vector<FrameUserData> frameUserData;
    uint64_t                   frameCounter{1};
  };

  // Pass-scoped scratch and pass-owned descriptors will move here in later tasks.
  // Lifetime trigger: reset each recorded pass; no persistent renderer-owned pass data yet.
  struct PerPassResources
  {
    DrawStream                                  drawStream;
    std::vector<DrawStreamDecoder::DecodedDraw> decodedDraws;
  };

  // Per-frame passes
  std::unique_ptr<ShadowPass>          m_shadowPass;
  std::unique_ptr<GBufferPass>         m_gbufferPass;
  std::unique_ptr<AnimateVerticesPass> m_animateVerticesPass;
  std::unique_ptr<SceneOpaquePass>     m_sceneOpaquePass;
  std::unique_ptr<LightPass>           m_lightPass;
  std::unique_ptr<LightCullingPass>    m_lightCullingPass;
  std::unique_ptr<ForwardPass>         m_forwardPass;
  std::unique_ptr<PresentPass>         m_presentPass;
  std::unique_ptr<ImguiPass>           m_imguiPass;
  std::unique_ptr<ShadowDebugPass>     m_shadowDebugPass;
  demo::PassExecutor                   m_passExecutor;

  // glTF support
  MeshPool m_meshPool;

  // IBL support (swapchain-dependent for now)
  IBLResources m_iblResources;

  // Shadow support (single directional shadow map)
  ShadowResources m_shadowResources;

  // Light pipeline
  PipelineHandle m_lightPipeline{};
  PipelineHandle m_gbufferOpaquePipeline{};      // GBuffer Opaque variant
  PipelineHandle m_gbufferAlphaTestPipeline{};   // GBuffer AlphaTest variant
  PipelineHandle m_forwardPipeline{};            // Forward pass for transparent

  // Shadow pipeline (depth-only for CSM)
  PipelineHandle m_shadowOpaquePipeline{};       // Shadow Opaque variant
  PipelineHandle m_shadowAlphaTestPipeline{};    // Shadow AlphaTest variant

  // Light culling compute pipeline
  PipelineHandle m_lightCullingPipeline{};
  VkDescriptorSet m_lightCullingDescriptorSet{VK_NULL_HANDLE};  // Light buffer + depth + output

  // Debug line pipeline (for visualization)
  PipelineHandle m_debugLinePipeline{};

  // GBuffer uniform buffer bind groups (per-frame)
  // BindGroupHandle getCameraBindGroup(uint32_t frameIndex) const;  // Moved to public
  // BindGroupHandle getDrawBindGroup(uint32_t frameIndex) const;    // Moved to public

  // Draw-call-scoped transient CPU/GPU data staging bucket.
  // Lifetime trigger: rebuilt per draw packet emission/consumption; currently no persistent owner fields.
  struct PerDrawData
  {
  };

  // Material/texture domain resources.
  // Created during init(), updated when bound material texture set changes, destroyed during shutdown.
  // Rebuild trigger: descriptor/layout recreation policy or material-set growth changes.
  struct MaterialResources
  {
    enum class TextureRuntimeKind
    {
      materialSampled,
      viewportAttachment,
      outputTexture,  // OutputTexture for PBR lighting result
    };

    struct TextureHotData
    {
      TextureRuntimeKind runtimeKind{TextureRuntimeKind::materialSampled};
      uint32_t           viewportAttachmentIndex{0};
      VkImageView        sampledImageView{VK_NULL_HANDLE};
      VkImageLayout      sampledImageLayout{VK_IMAGE_LAYOUT_UNDEFINED};
    };

    struct TextureColdData
    {
      utils::ImageResource ownedImage{};
      VkExtent2D           sourceExtent{};
    };

    struct TextureRecord
    {
      TextureHotData  hot{};
      TextureColdData cold{};
    };

    struct MaterialRecord
    {
      // PBR Texture handles (each independent for sharing)
      TextureHandle baseColorTexture{kNullTextureHandle};
      TextureHandle metallicRoughnessTexture{kNullTextureHandle};
      TextureHandle normalTexture{kNullTextureHandle};
      TextureHandle occlusionTexture{kNullTextureHandle};
      TextureHandle emissiveTexture{kNullTextureHandle};

      // Legacy compatibility
      TextureHandle      sampledTexture{};

      // PBR Factors (fallback when texture missing)
      glm::vec4 baseColorFactor{1.0f};
      float     metallicFactor{1.0f};
      float     roughnessFactor{1.0f};
      float     normalScale{1.0f};
      float     occlusionStrength{1.0f};
      glm::vec3 emissiveFactor{0.0f};

      // Alpha properties
      int32_t alphaMode = 0;        // 0=OPAQUE, 1=MASK, 2=BLEND
      float alphaCutoff = 0.5f;     // for MASK mode

      // Bindless descriptor slot
      rhi::ResourceIndex descriptorIndex{0};
      const char*        debugName{"material"};
    };

    HandlePool<TextureHandle, TextureRecord>       texturePool;
    HandlePool<MaterialHandle, MaterialRecord>     materialPool;
    HandlePool<BindGroupHandle, BindGroupResource> bindGroupPool;
    // New RHI interface handle pools
    HandlePool<rhi::BindGroupLayoutHandle, std::unique_ptr<rhi::BindTableLayout>> bindGroupLayoutPool;
    HandlePool<rhi::BindGroupHandle, std::unique_ptr<rhi::BindGroup>> bindGroupRhiPool;
    MaterialHandle                                 sampleMaterials[kDemoMaterialSlotCount]{};
    TextureHandle                                  viewportTextureHandle{};
    BindGroupHandle                                materialBindGroup{};
    uint32_t                                       maxTextures{10000};
  };

  void              createTransientCommandPool();
  void              createFrameSubmission(uint32_t numFrames);
  void              rebuildSwapchainDependentResources(std::optional<VkExtent2D> requestedViewportSize = std::nullopt);
  bool              prepareFrameResources();
  rhi::CommandList& beginCommandRecording();
  void              drawFrame(rhi::CommandList& cmd, const RenderParams& params);
  void              endFrame(rhi::CommandList& cmd);
  void              beginDynamicRenderingToSwapchain(const rhi::CommandList& cmd) const;
  void              endDynamicRenderingToSwapchain(const rhi::CommandList& cmd);
  void              recordComputeCommands(rhi::CommandList& cmd, const RenderParams& params) const;
  void recordGraphicCommands(rhi::CommandList& cmd, const RenderParams& params, std::span<const StreamEntry> drawStream);
  void                 prebuildRequiredPipelineVariants();
  void                 createPrebuiltGraphicsPipelineVariants();
  void                 createPrebuiltComputePipelineVariant();
  void                 initImGui(GLFWwindow* window);
  void                 createDescriptorPool();
  void                 createMaterialBindGroup();     // Create material bind group early for pipeline layout
  void                 createGraphicDescriptorSet();
  void                 updateGraphicsDescriptorSet();
  void                 updateGBufferTextureDescriptorSet();
  void                 destroyBindGroups();
  utils::ImageResource loadAndCreateImage(rhi::CommandList& cmd, const std::string& filename);
  PipelineHandle       selectComputePipelineHandle() const;
  PipelineHandle       selectGraphicsPipelineHandle(GraphicsPipelineVariant variant) const;
  rhi::ResourceIndex   getBindGroupPrimaryLogicalIndex(BindGroupHandle handle, BindGroupSetSlot expectedSlot) const;
  const MaterialResources::MaterialRecord*  tryGetMaterial(MaterialHandle handle) const;
  const MaterialResources::TextureHotData*  tryGetTextureHot(TextureHandle handle) const;
  const MaterialResources::TextureColdData* tryGetTextureCold(TextureHandle handle) const;
  BindGroupHandle                           createBindGroup(BindGroupDesc desc);
  void           updateBindGroup(BindGroupHandle handle, const rhi::BindTableWrite* writes, uint32_t writeCount) const;
  // destroyBindGroup is provided by public RHI interface (line 145)
  PipelineHandle registerPipeline(uint32_t bindPoint, uint64_t nativePipeline, uint32_t specializationVariant);
  void           destroyPipelines();
  const DeviceLifetimeResources::PipelineRecord* tryGetPipelineRecord(PipelineHandle handle) const;
  const BindGroupResource* tryGetBindGroup(BindGroupHandle handle) const;
  uint64_t                 getBindGroupLayoutOpaque(BindGroupHandle handle, BindGroupSetSlot expectedSlot) const;
  uint64_t                 getBindGroupDescriptorSetOpaque(BindGroupHandle handle, BindGroupSetSlot expectedSlot) const;
  static std::optional<uint32_t> mapSetSlotToLegacyShaderSet(BindGroupSetSlot slot);

  DeviceLifetimeResources     m_device;
  SwapchainDependentResources m_swapchainDependent;
  PerFrameResources           m_perFrame;
  PerPassResources            m_perPass;
  PerDrawData                 m_perDraw;
  MaterialResources           m_materials;
  DrawStreamDecoder           m_drawStreamDecoder;
};

}  // namespace demo
