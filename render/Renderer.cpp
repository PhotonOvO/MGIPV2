#include "Renderer.h"
#include "../rhi/vulkan/VulkanPipelines.h"
#include "../rhi/vulkan/VulkanDescriptor.h"
#include "../rhi/vulkan/VulkanSwapchain.h"
#include "../rhi/vulkan/VulkanDevice.h"
#include "../rhi/vulkan/VulkanFrameContext.h"
#include "FrameSubmission.h"

#include <cstring>
#include <type_traits>

#include "../rhi/vulkan/VulkanCommandList.h"

namespace demo {

namespace {

constexpr uint32_t kPerFrameTransientAllocatorSize = 1u << 20;

[[nodiscard]] uint32_t alignUp(uint32_t value, uint32_t alignment)
{
  const uint32_t safeAlignment = alignment == 0 ? 1u : alignment;
  const uint32_t mask          = safeAlignment - 1u;
  return (value + mask) & ~mask;
}

}  // namespace

static VkFormat selectSwapchainImageFormat(VkPhysicalDevice physicalDevice, VkSurfaceKHR surface)
{
  const VkPhysicalDeviceSurfaceInfo2KHR surfaceInfo2{.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_SURFACE_INFO_2_KHR, .surface = surface};

  uint32_t formatCount = 0;
  VK_CHECK(vkGetPhysicalDeviceSurfaceFormats2KHR(physicalDevice, &surfaceInfo2, &formatCount, nullptr));
  ASSERT(formatCount > 0, "Renderer::init requires at least one surface format");

  std::vector<VkSurfaceFormat2KHR> formats(formatCount, {.sType = VK_STRUCTURE_TYPE_SURFACE_FORMAT_2_KHR});
  VK_CHECK(vkGetPhysicalDeviceSurfaceFormats2KHR(physicalDevice, &surfaceInfo2, &formatCount, formats.data()));

  for(const VkSurfaceFormat2KHR& format2 : formats)
  {
    if(format2.surfaceFormat.format == VK_FORMAT_B8G8R8A8_UNORM && format2.surfaceFormat.colorSpace == VK_COLOR_SPACE_SRGB_NONLINEAR_KHR)
    {
      return format2.surfaceFormat.format;
    }
  }

  return formats.front().surfaceFormat.format;
}

template <typename T>
static T fromNativeHandle(uint64_t handle)
{
  // Always reinterpret the 64-bit handle as the target Vulkan handle type.
  // This avoids issues when VkInstance/VkDevice/etc. are opaque handles (pointer-like)
  // and cannot be created via static_cast from a integer.
  return reinterpret_cast<T>(static_cast<uintptr_t>(handle));
}

static bool isValidExtent(rhi::Extent2D extent)
{
  return extent.width > 0 && extent.height > 0;
}

static bool isValidExtent(VkExtent2D extent)
{
  return extent.width > 0 && extent.height > 0;
}

static bool extentChanged(VkExtent2D a, rhi::Extent2D b)
{
  return a.width != b.width || a.height != b.height;
}

static bool extentChanged(VkExtent2D a, VkExtent2D b)
{
  return a.width != b.width || a.height != b.height;
}

static VmaAllocator createAllocator(const VkPhysicalDevice physicalDevice, const VkDevice device, const VkInstance instance, const uint32_t apiVersion)
{
  VmaAllocatorCreateInfo allocatorInfo{
      .physicalDevice   = physicalDevice,
      .device           = device,
      .instance         = instance,
      .vulkanApiVersion = apiVersion,
  };
  allocatorInfo.flags |= VMA_ALLOCATOR_CREATE_BUFFER_DEVICE_ADDRESS_BIT;
  allocatorInfo.flags |= VMA_ALLOCATOR_CREATE_KHR_MAINTENANCE4_BIT;
  allocatorInfo.flags |= VMA_ALLOCATOR_CREATE_KHR_MAINTENANCE5_BIT;

  const VmaVulkanFunctions functions{
      .vkGetInstanceProcAddr = vkGetInstanceProcAddr,
      .vkGetDeviceProcAddr   = vkGetDeviceProcAddr,
  };
  allocatorInfo.pVulkanFunctions = &functions;

  VmaAllocator allocator{nullptr};
  VK_CHECK(vmaCreateAllocator(&allocatorInfo, &allocator));
  return allocator;
}

static utils::Buffer createBuffer(const VkDevice               device,
                                  const VmaAllocator           allocator,
                                  const VkDeviceSize           size,
                                  const VkBufferUsageFlags2KHR usage,
                                  const VmaMemoryUsage         memoryUsage = VMA_MEMORY_USAGE_AUTO,
                                  VmaAllocationCreateFlags     flags       = {})
{
  const VkBufferUsageFlags2CreateInfoKHR usageInfo{
      .sType = VK_STRUCTURE_TYPE_BUFFER_USAGE_FLAGS_2_CREATE_INFO_KHR,
      .usage = usage | VK_BUFFER_USAGE_2_SHADER_DEVICE_ADDRESS_BIT_KHR,
  };
  const VkBufferCreateInfo bufferInfo{
      .sType       = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO,
      .pNext       = &usageInfo,
      .size        = size,
      .usage       = 0,
      .sharingMode = VK_SHARING_MODE_EXCLUSIVE,
  };

  VmaAllocationCreateInfo allocInfo{.flags = flags, .usage = memoryUsage};
  if(size > 64ULL * 1024)
  {
    allocInfo.flags |= VMA_ALLOCATION_CREATE_DEDICATED_MEMORY_BIT;
  }

  utils::Buffer     buffer{};
  VmaAllocationInfo allocationInfo{};
  VK_CHECK(vmaCreateBuffer(allocator, &bufferInfo, &allocInfo, &buffer.buffer, &buffer.allocation, &allocationInfo));

  const VkBufferDeviceAddressInfo addressInfo{.sType = VK_STRUCTURE_TYPE_BUFFER_DEVICE_ADDRESS_INFO, .buffer = buffer.buffer};
  buffer.address = vkGetBufferDeviceAddress(device, &addressInfo);
  return buffer;
}

static void destroyBuffer(const VmaAllocator allocator, utils::Buffer& buffer)
{
  if(buffer.buffer != VK_NULL_HANDLE)
  {
    vmaDestroyBuffer(allocator, buffer.buffer, buffer.allocation);
    buffer = {};
  }
}

static utils::Buffer createStagingBuffer(const VkDevice              device,
                                         const VmaAllocator          allocator,
                                         std::vector<utils::Buffer>& stagingBuffers,
                                         std::span<const std::byte>  data)
{
  utils::Buffer stagingBuffer = createBuffer(device, allocator, data.size_bytes(), VK_BUFFER_USAGE_2_TRANSFER_SRC_BIT_KHR,
                                             VMA_MEMORY_USAGE_CPU_TO_GPU, VMA_ALLOCATION_CREATE_HOST_ACCESS_SEQUENTIAL_WRITE_BIT);

  void* mappedData{nullptr};
  VK_CHECK(vmaMapMemory(allocator, stagingBuffer.allocation, &mappedData));
  std::memcpy(mappedData, data.data(), data.size_bytes());
  vmaUnmapMemory(allocator, stagingBuffer.allocation);
  stagingBuffers.push_back(stagingBuffer);
  return stagingBuffer;
}

static utils::Buffer createBufferAndUploadData(const VkDevice               device,
                                               const VmaAllocator           allocator,
                                               std::vector<utils::Buffer>&  stagingBuffers,
                                               const VkCommandBuffer        cmd,
                                               std::span<const std::byte>   data,
                                               const VkBufferUsageFlags2KHR usage)
{
  utils::Buffer stagingBuffer = createStagingBuffer(device, allocator, stagingBuffers, data);
  utils::Buffer gpuBuffer     = createBuffer(device, allocator, data.size_bytes(),
                                             usage | VK_BUFFER_USAGE_2_TRANSFER_DST_BIT_KHR, VMA_MEMORY_USAGE_GPU_ONLY);

  const VkBufferCopy copyRegion{.size = data.size_bytes()};
  vkCmdCopyBuffer(cmd, stagingBuffer.buffer, gpuBuffer.buffer, 1, &copyRegion);
  return gpuBuffer;
}

static utils::Image createImage(const VmaAllocator allocator, const VkImageCreateInfo& imageInfo)
{
  const VmaAllocationCreateInfo allocationInfo{.usage = VMA_MEMORY_USAGE_GPU_ONLY};
  utils::Image                  image{};
  VmaAllocationInfo             allocInfo{};
  VK_CHECK(vmaCreateImage(allocator, &imageInfo, &allocationInfo, &image.image, &image.allocation, &allocInfo));
  return image;
}

static utils::ImageResource createImageAndUploadData(const VkDevice              device,
                                                     const VmaAllocator          allocator,
                                                     std::vector<utils::Buffer>& stagingBuffers,
                                                     const VkCommandBuffer       cmd,
                                                     std::span<const std::byte>  data,
                                                     VkImageCreateInfo           imageInfo,
                                                     const VkImageLayout         finalLayout)
{
  utils::Buffer stagingBuffer = createStagingBuffer(device, allocator, stagingBuffers, data);
  imageInfo.usage |= VK_IMAGE_USAGE_TRANSFER_DST_BIT;

  utils::Image image = createImage(allocator, imageInfo);
  utils::cmdInitImageLayout(cmd, image.image);

  const VkBufferImageCopy copyRegion{
      .imageSubresource = {.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT, .layerCount = 1},
      .imageExtent      = imageInfo.extent,
  };
  vkCmdCopyBufferToImage(cmd, stagingBuffer.buffer, image.image, VK_IMAGE_LAYOUT_GENERAL, 1, &copyRegion);

  utils::ImageResource resource{};
  resource.image      = image.image;
  resource.allocation = image.allocation;
  resource.layout     = finalLayout;
  return resource;
}

static void destroyImageResource(const VkDevice device, const VmaAllocator allocator, utils::ImageResource& image)
{
  if(image.view != VK_NULL_HANDLE)
  {
    vkDestroyImageView(device, image.view, nullptr);
  }
  if(image.image != VK_NULL_HANDLE)
  {
    vmaDestroyImage(allocator, image.image, image.allocation);
  }
  image = {};
}

static void freeStagingBuffers(const VmaAllocator allocator, std::vector<utils::Buffer>& stagingBuffers)
{
  for(utils::Buffer& buffer : stagingBuffers)
  {
    destroyBuffer(allocator, buffer);
  }
  stagingBuffers.clear();
}

static rhi::TextureFormat toPortableTextureFormat(VkFormat format)
{
  switch(format)
  {
    case VK_FORMAT_R8G8B8A8_UNORM:
      return rhi::TextureFormat::rgba8Unorm;
    case VK_FORMAT_B8G8R8A8_UNORM:
      return rhi::TextureFormat::bgra8Unorm;
    case VK_FORMAT_D16_UNORM:
      return rhi::TextureFormat::d16Unorm;
    case VK_FORMAT_D32_SFLOAT:
      return rhi::TextureFormat::d32Sfloat;
    case VK_FORMAT_D24_UNORM_S8_UINT:
      return rhi::TextureFormat::d24UnormS8;
    case VK_FORMAT_D32_SFLOAT_S8_UINT:
      return rhi::TextureFormat::d32SfloatS8;
    default:
      return rhi::TextureFormat::undefined;
  }
}

static rhi::ShaderReflectionData buildRasterShaderReflection()
{
  rhi::ShaderReflectionData reflection{};
  reflection.name        = "shader.rast";
  reflection.entryPoints = {
      rhi::ShaderEntryPoint{"vertexMain", rhi::ShaderStageFlagBits::vertex},
      rhi::ShaderEntryPoint{"fragmentMain", rhi::ShaderStageFlagBits::fragment},
  };
  reflection.resourceBindings = {
      rhi::ShaderResourceBinding{"textures", rhi::ShaderResourceType::sampler, rhi::DescriptorType::combinedImageSampler,
                                 rhi::ShaderStageFlagBits::fragment, static_cast<uint32_t>(shaderio::LSetTextures),
                                 static_cast<uint32_t>(shaderio::LBindTextures), Renderer::kDemoMaterialSlotCount, 0},
      rhi::ShaderResourceBinding{"sceneInfo", rhi::ShaderResourceType::uniformBuffer, rhi::DescriptorType::uniformBufferDynamic,
                                 rhi::ShaderStageFlagBits::vertex | rhi::ShaderStageFlagBits::fragment,
                                 static_cast<uint32_t>(shaderio::LSetScene), static_cast<uint32_t>(shaderio::LBindSceneInfo), 1, 0},
  };
  reflection.pushConstantRanges = {
      rhi::PushConstantRange{rhi::ShaderStageFlagBits::vertex | rhi::ShaderStageFlagBits::fragment, 0,
                             std::max(sizeof(shaderio::PushConstant), sizeof(shaderio::PushConstantGltf))},
  };
  reflection.pushConstantSize        = std::max(sizeof(shaderio::PushConstant), sizeof(shaderio::PushConstantGltf));
  reflection.specializationConstants = {
      rhi::SpecializationConstant{0, 0, sizeof(VkBool32)},
  };
  return reflection;
}

static rhi::ShaderReflectionData buildComputeShaderReflection()
{
  rhi::ShaderReflectionData reflection{};
  reflection.name        = "shader.comp";
  reflection.entryPoints = {
      rhi::ShaderEntryPoint{"main", rhi::ShaderStageFlagBits::compute},
  };
  reflection.pushConstantRanges = {
      rhi::PushConstantRange{rhi::ShaderStageFlagBits::compute, 0, sizeof(shaderio::PushConstantCompute)},
  };
  reflection.pushConstantSize = sizeof(shaderio::PushConstantCompute);
  return reflection;
}

std::optional<uint32_t> Renderer::mapSetSlotToLegacyShaderSet(BindGroupSetSlot slot)
{
  switch(slot)
  {
    case BindGroupSetSlot::material:
      return static_cast<uint32_t>(shaderio::LSetTextures);
    case BindGroupSetSlot::drawDynamic:
      return static_cast<uint32_t>(shaderio::LSetScene);
    default:
      return std::nullopt;
  }
}

void Renderer::init(GLFWwindow* window, rhi::Surface& surface, bool vSync)
{
  m_swapchainDependent.vSync = vSync;
  m_materials                = MaterialResources{};

  VkPhysicalDeviceExtendedDynamicState3FeaturesEXT dynamicState3Features{
      .sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_EXTENDED_DYNAMIC_STATE_3_FEATURES_EXT};
  VkPhysicalDeviceUnifiedImageLayoutsFeaturesKHR unifiedImageLayoutsFeature{
      .sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_UNIFIED_IMAGE_LAYOUTS_FEATURES_KHR};

  rhi::DeviceCreateInfo deviceCreateInfo;
  uint32_t              glfwExtensionCount = 0;
  const char**          glfwExtensions     = glfwGetRequiredInstanceExtensions(&glfwExtensionCount);
  for(uint32_t i = 0; i < glfwExtensionCount; ++i)
  {
    deviceCreateInfo.instanceExtensions.push_back(glfwExtensions[i]);
  }
  // Debug utils for event markers (RenderDoc, PIX, etc.)
  deviceCreateInfo.instanceExtensions.push_back(VK_EXT_DEBUG_UTILS_EXTENSION_NAME);
  deviceCreateInfo.deviceExtensions.push_back({VK_KHR_SWAPCHAIN_EXTENSION_NAME, true, nullptr});
  deviceCreateInfo.deviceExtensions.push_back({VK_KHR_UNIFIED_IMAGE_LAYOUTS_EXTENSION_NAME, false, &unifiedImageLayoutsFeature});
  deviceCreateInfo.deviceExtensions.push_back({VK_EXT_EXTENDED_DYNAMIC_STATE_3_EXTENSION_NAME, false, &dynamicState3Features});

  m_device.device = std::make_unique<rhi::vulkan::VulkanDevice>();
  m_device.device->init(deviceCreateInfo);

  const rhi::CapabilityReport capabilityReport = m_device.device->queryCapabilities();
  ASSERT(m_device.device->supports(rhi::CapabilityTier::Core), "Renderer::init requires RHI Core capability tier");
  ASSERT(capabilityReport.coreGraphics && capabilityReport.coreCompute && capabilityReport.coreBindless,
         "Renderer::init requires graphics+compute+bindless capability floor");

  const VkInstance nativeInstance = fromNativeHandle<VkInstance>(m_device.device->getNativeInstance());
  const VkPhysicalDevice nativePhysicalDevice = fromNativeHandle<VkPhysicalDevice>(m_device.device->getNativePhysicalDevice());
  const VkDevice       nativeDevice        = fromNativeHandle<VkDevice>(m_device.device->getNativeDevice());
  const rhi::QueueInfo graphicsQueueInfo   = m_device.device->getGraphicsQueue();
  const VkQueue        nativeGraphicsQueue = fromNativeHandle<VkQueue>(graphicsQueueInfo.nativeHandle);

  rhi::WindowHandle windowHandle{window};
  surface.init(static_cast<void*>(nativeInstance), static_cast<void*>(nativePhysicalDevice), windowHandle);

  m_device.allocator = createAllocator(nativePhysicalDevice, nativeDevice, nativeInstance, m_device.device->getApiVersion());

  m_device.samplerPool.init(nativeDevice);

  m_meshPool.init(nativeDevice, m_device.allocator);

  const VkSurfaceKHR nativeSurface = reinterpret_cast<VkSurfaceKHR>(surface.getNativeHandle());
  ASSERT(nativeSurface != VK_NULL_HANDLE, "Renderer::init requires a valid initialized surface");
  DBG_VK_NAME(nativeSurface);
  m_swapchainDependent.swapchainImageFormat = selectSwapchainImageFormat(nativePhysicalDevice, nativeSurface);

  createTransientCommandPool();

  auto nativeSwapchain = std::make_unique<rhi::vulkan::VulkanSwapchain>();
  nativeSwapchain->init(static_cast<void*>(nativePhysicalDevice), static_cast<void*>(nativeDevice),
                        static_cast<void*>(nativeGraphicsQueue), static_cast<void*>(nativeSurface),
                        static_cast<void*>(m_device.transientCmdPool), m_swapchainDependent.vSync);
  m_swapchainDependent.swapchain = std::move(nativeSwapchain);
  m_swapchainDependent.swapchain->rebuild();
  const rhi::Extent2D swapchainExtent = m_swapchainDependent.swapchain->getExtent();
  m_swapchainDependent.windowSize     = VkExtent2D{swapchainExtent.width, swapchainExtent.height};

  m_swapchainDependent.currentImageIndex = 0;
  m_swapchainDependent.imageStates.assign(
      m_swapchainDependent.swapchain->getMaxFramesInFlight(),
      rhi::ResourceState::Undefined);

  // Create material bind group BEFORE createFrameSubmission() because it's needed for pipeline layout
  createMaterialBindGroup();
  createFrameSubmission(m_swapchainDependent.swapchain->getMaxFramesInFlight());
  createDescriptorPool();
  initImGui(window);

  const VkSamplerCreateInfo info{.sType = VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO, .magFilter = VK_FILTER_LINEAR, .minFilter = VK_FILTER_LINEAR};
  const VkSampler linearSampler = m_device.samplerPool.acquireSampler(info);
  DBG_VK_NAME(linearSampler);

  {
    VkCommandBuffer cmd = utils::beginSingleTimeCommands(nativeDevice, m_device.transientCmdPool);

    const VkFormat             depthFormat = utils::findDepthFormat(nativePhysicalDevice);
    SceneResources::CreateInfo sceneResourcesInit{
        .size          = m_swapchainDependent.windowSize,
        .color         = {
            VK_FORMAT_R8G8B8A8_UNORM,  // GBuffer0: BaseColor
            VK_FORMAT_R8G8B8A8_UNORM,  // GBuffer1: Normal
            VK_FORMAT_R8G8B8A8_UNORM,  // GBuffer2: MaterialParams
        },
        .depth         = depthFormat,
        .linearSampler = linearSampler,
    };
    m_swapchainDependent.sceneResources.init(*m_device.device, m_device.allocator, cmd, sceneResourcesInit);

    // Create GBuffer descriptor set layout and set for LightPass
    {
      // Binding 0: Array of 4 sampled images (GBuffer0/1/2 + Depth)
      const VkDescriptorSetLayoutBinding textureBinding{
          0, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 4, VK_SHADER_STAGE_FRAGMENT_BIT, nullptr
      };

      // Binding 1: Camera uniform buffer (dynamic, but we'll use fixed offset for LightPass)
      const VkDescriptorSetLayoutBinding cameraBinding{
          1, VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER, 1, VK_SHADER_STAGE_FRAGMENT_BIT, nullptr
      };

      const std::array<VkDescriptorSetLayoutBinding, 2> bindings = {textureBinding, cameraBinding};

      const VkDescriptorSetLayoutCreateInfo layoutInfo{
          .sType        = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO,
          .bindingCount = static_cast<uint32_t>(bindings.size()),
          .pBindings    = bindings.data(),
      };
      VK_CHECK(vkCreateDescriptorSetLayout(nativeDevice, &layoutInfo, nullptr, &m_device.gbufferTextureSetLayout));
      DBG_VK_NAME(m_device.gbufferTextureSetLayout);

      // Allocate descriptor set
      const VkDescriptorSetAllocateInfo allocInfo{
          .sType              = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO,
          .descriptorPool     = m_device.descriptorPool,
          .descriptorSetCount = 1,
          .pSetLayouts        = &m_device.gbufferTextureSetLayout,
      };
      VK_CHECK(vkAllocateDescriptorSets(nativeDevice, &allocInfo, &m_device.gbufferTextureSet));
      DBG_VK_NAME(m_device.gbufferTextureSet);
    }

    utils::endSingleTimeCommands(cmd, nativeDevice, m_device.transientCmdPool, nativeGraphicsQueue);
  }

  // Update GBuffer texture descriptor set
  updateGBufferTextureDescriptorSet();

  // Create pipeline layout for LightPass (with push constants)
  {
    // Push constants for light parameters (48 bytes: 3 vec3 + padding)
    const VkPushConstantRange pushConstantRange{
        .stageFlags = VK_SHADER_STAGE_FRAGMENT_BIT,
        .offset     = 0,
        .size       = sizeof(shaderio::LightParams),
    };

    const VkPipelineLayoutCreateInfo layoutInfo{
        .sType          = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO,
        .setLayoutCount = 1,
        .pSetLayouts    = &m_device.gbufferTextureSetLayout,
        .pushConstantRangeCount = 1,
        .pPushConstantRanges    = &pushConstantRange,
    };
    VK_CHECK(vkCreatePipelineLayout(nativeDevice, &layoutInfo, nullptr, &m_device.lightPipelineLayout));
    DBG_VK_NAME(m_device.lightPipelineLayout);
  }

  createGraphicDescriptorSet();
  prebuildRequiredPipelineVariants();

  {
    VkCommandBuffer cmd = utils::beginSingleTimeCommands(nativeDevice, m_device.transientCmdPool);
    m_device.vertexBuffer = createBufferAndUploadData(nativeDevice, m_device.allocator, m_device.stagingBuffers, cmd,
                                                      std::as_bytes(std::span{s_vertices}),
                                                      VK_BUFFER_USAGE_VERTEX_BUFFER_BIT | VK_BUFFER_USAGE_STORAGE_BUFFER_BIT);
    DBG_VK_NAME(m_device.vertexBuffer.buffer);

    m_device.pointsBuffer = createBufferAndUploadData(nativeDevice, m_device.allocator, m_device.stagingBuffers, cmd,
                                                      std::as_bytes(std::span{s_points}), VK_BUFFER_USAGE_STORAGE_BUFFER_BIT);
    DBG_VK_NAME(m_device.pointsBuffer.buffer);

    const std::vector<std::string> searchPaths = {".", "resources", "../resources", "../../resources"};
    std::string                    filename    = utils::findFile("image1.jpg", searchPaths);
    ASSERT(!filename.empty(), "Could not load texture image!");
    rhi::vulkan::VulkanCommandList initCommandList{};
    initCommandList.setCommandBuffer(cmd);
    utils::ImageResource materialImage0   = loadAndCreateImage(initCommandList, filename);
    const TextureHandle  materialTexture0 = m_materials.texturePool.emplace(MaterialResources::TextureRecord{
        .hot =
            {
                .runtimeKind        = MaterialResources::TextureRuntimeKind::materialSampled,
                .sampledImageView   = materialImage0.view,
                .sampledImageLayout = materialImage0.layout,
            },
        .cold =
            {
                .ownedImage   = materialImage0,
                .sourceExtent = materialImage0.extent,
            },
    });

    filename = utils::findFile("image2.jpg", searchPaths);
    ASSERT(!filename.empty(), "Could not load texture image!");
    utils::ImageResource materialImage1   = loadAndCreateImage(initCommandList, filename);
    const TextureHandle  materialTexture1 = m_materials.texturePool.emplace(MaterialResources::TextureRecord{
        .hot =
            {
                .runtimeKind        = MaterialResources::TextureRuntimeKind::materialSampled,
                .sampledImageView   = materialImage1.view,
                .sampledImageLayout = materialImage1.layout,
            },
        .cold =
            {
                .ownedImage   = materialImage1,
                .sourceExtent = materialImage1.extent,
            },
    });

    m_materials.sampleMaterials[0]    = m_materials.materialPool.emplace(MaterialResources::MaterialRecord{
        .sampledTexture  = materialTexture0,
        .descriptorIndex = 0,
        .debugName       = "image1-material",
    });
    m_materials.sampleMaterials[1]    = m_materials.materialPool.emplace(MaterialResources::MaterialRecord{
        .sampledTexture  = materialTexture1,
        .descriptorIndex = 1,
        .debugName       = "image2-material",
    });
    m_materials.viewportTextureHandle = m_materials.texturePool.emplace(MaterialResources::TextureRecord{
        .hot =
            {
                .runtimeKind             = MaterialResources::TextureRuntimeKind::outputTexture,
                .viewportAttachmentIndex = 0,
            },
    });

    utils::endSingleTimeCommands(cmd, nativeDevice, m_device.transientCmdPool, nativeGraphicsQueue);
  }
  freeStagingBuffers(m_device.allocator, m_device.stagingBuffers);

  updateGraphicsDescriptorSet();

  // Initialize passes and pass executor
  m_gbufferPass         = std::make_unique<GBufferPass>(this);
  m_animateVerticesPass = std::make_unique<AnimateVerticesPass>(this);
  m_sceneOpaquePass     = std::make_unique<SceneOpaquePass>(this);
  m_lightPass           = std::make_unique<LightPass>(this);
  m_lightCullingPass    = std::make_unique<LightCullingPass>(this);
  m_forwardPass         = std::make_unique<ForwardPass>(this);
  m_presentPass         = std::make_unique<PresentPass>(this);
  m_imguiPass           = std::make_unique<ImguiPass>(this);
  m_passExecutor.clear();
  m_passExecutor.addPass(*m_gbufferPass);
  // m_passExecutor.addPass(*m_animateVerticesPass);
  // m_passExecutor.addPass(*m_sceneOpaquePass);
  m_passExecutor.addPass(*m_lightCullingPass);
  m_passExecutor.addPass(*m_lightPass);
  // m_passExecutor.addPass(*m_forwardPass); 
  m_passExecutor.addPass(*m_presentPass);
  m_passExecutor.addPass(*m_imguiPass);
}

void Renderer::shutdown(rhi::Surface& surface)
{
  m_device.device->waitIdle();
  VkDevice device = fromNativeHandle<VkDevice>(m_device.device->getNativeDevice());

  if(m_swapchainDependent.swapchain)
  {
    auto* vkSwapchain = static_cast<rhi::vulkan::VulkanSwapchain*>(m_swapchainDependent.swapchain.get());
    vkSwapchain->deinit();
    m_swapchainDependent.swapchain.reset();
  }
  m_device.samplerPool.deinit();

  // Destroy bind groups FIRST (they use descriptor pools)
  destroyBindGroups();

  // Shutdown ImGui Vulkan backend BEFORE destroying uiDescriptorPool
  // ImGui_ImplVulkan_Shutdown() frees descriptor sets from uiDescriptorPool
  ImGui_ImplVulkan_Shutdown();
  ImGui_ImplGlfw_Shutdown();
  ImGui::DestroyContext();

  // Destroy Vulkan objects that are not managed by smart pointers
  if(m_device.lightPipelineLayout != VK_NULL_HANDLE)
  {
    vkDestroyPipelineLayout(device, m_device.lightPipelineLayout, nullptr);
    m_device.lightPipelineLayout = VK_NULL_HANDLE;
  }
  // Note: gbufferTextureSet is freed automatically when descriptorPool is destroyed
  m_device.gbufferTextureSet = VK_NULL_HANDLE;
  if(m_device.gbufferTextureSetLayout != VK_NULL_HANDLE)
  {
    vkDestroyDescriptorSetLayout(device, m_device.gbufferTextureSetLayout, nullptr);
    m_device.gbufferTextureSetLayout = VK_NULL_HANDLE;
  }
  if(m_device.uiDescriptorPool != VK_NULL_HANDLE)
  {
    vkDestroyDescriptorPool(device, m_device.uiDescriptorPool, nullptr);
    m_device.uiDescriptorPool = VK_NULL_HANDLE;
  }
  if(m_device.descriptorPool != VK_NULL_HANDLE)
  {
    vkDestroyDescriptorPool(device, m_device.descriptorPool, nullptr);
    m_device.descriptorPool = VK_NULL_HANDLE;
  }
  if(m_device.transientCmdPool != VK_NULL_HANDLE)
  {
    vkDestroyCommandPool(device, m_device.transientCmdPool, nullptr);
    m_device.transientCmdPool = VK_NULL_HANDLE;
  }

  destroyPipelines();
  if(m_device.graphicPipelineLayout)
  {
    m_device.graphicPipelineLayout->deinit();
    m_device.graphicPipelineLayout.reset();
  }
  if(m_device.computePipelineLayout)
  {
    m_device.computePipelineLayout->deinit();
    m_device.computePipelineLayout.reset();
  }
  if(m_device.gbufferPipelineLayout)
  {
    m_device.gbufferPipelineLayout->deinit();
    m_device.gbufferPipelineLayout.reset();
  }
  // Per-frame bind groups already destroyed by destroyBindGroups() above
  // Just cleanup the transient allocators
  for(auto& frameUserData : m_perFrame.frameUserData)
  {
    frameUserData.transientAllocator.destroy();
  }
  m_perFrame.frameUserData.clear();
  if(m_perFrame.frameContext)
  {
    m_perFrame.frameContext->deinit();
    m_perFrame.frameContext.reset();
  }

  destroyBuffer(m_device.allocator, m_device.vertexBuffer);
  destroyBuffer(m_device.allocator, m_device.pointsBuffer);
  std::vector<TextureHandle> texturesToDestroy;
  m_materials.texturePool.forEachActive(
      [&](TextureHandle handle, const MaterialResources::TextureRecord&) { texturesToDestroy.push_back(handle); });
  for(const TextureHandle handle : texturesToDestroy)
  {
    const MaterialResources::TextureColdData* textureCold = tryGetTextureCold(handle);
    if(textureCold != nullptr && textureCold->ownedImage.image != VK_NULL_HANDLE)
    {
      utils::ImageResource image = textureCold->ownedImage;
      destroyImageResource(device, m_device.allocator, image);
    }
    m_materials.texturePool.destroy(handle);
  }

  std::vector<MaterialHandle> materialsToDestroy;
  m_materials.materialPool.forEachActive(
      [&](MaterialHandle handle, const MaterialResources::MaterialRecord&) { materialsToDestroy.push_back(handle); });
  for(const MaterialHandle handle : materialsToDestroy)
  {
    m_materials.materialPool.destroy(handle);
  }

  m_swapchainDependent.sceneResources.deinit();
  m_meshPool.deinit();
  freeStagingBuffers(m_device.allocator, m_device.stagingBuffers);
  if(m_device.allocator != nullptr)
  {
    vmaDestroyAllocator(m_device.allocator);
    m_device.allocator = nullptr;
  }
  surface.deinit();
  if(m_device.device)
  {
    m_device.device->deinit();
    m_device.device.reset();
  }
}

void Renderer::resize(rhi::Extent2D size)
{
  rebuildSwapchainDependentResources(VkExtent2D{size.width, size.height});
}

TextureHandle Renderer::getViewportTextureHandle() const
{
  return m_materials.viewportTextureHandle;
}

ImTextureID Renderer::getViewportTextureID(TextureHandle handle) const
{
  const MaterialResources::TextureHotData* textureHot = tryGetTextureHot(handle);
  if(textureHot == nullptr)
  {
    LOGW("Renderer::getViewportTextureID rejected stale/invalid texture handle (index=%u generation=%u)", handle.index,
         handle.generation);
    return ImTextureID{};
  }

  if(textureHot->runtimeKind == MaterialResources::TextureRuntimeKind::viewportAttachment)
  {
    return m_swapchainDependent.sceneResources.getImTextureID(textureHot->viewportAttachmentIndex);
  }

  if(textureHot->runtimeKind == MaterialResources::TextureRuntimeKind::outputTexture)
  {
    return m_swapchainDependent.sceneResources.getOutputTextureImID();
  }

  return ImTextureID{};
}

MaterialHandle Renderer::getMaterialHandle(uint32_t slot) const
{
  if(slot < kDemoMaterialSlotCount)
  {
    return m_materials.sampleMaterials[slot];
  }
  return kNullMaterialHandle;
}

PipelineHandle Renderer::getGraphicsPipelineHandle(GraphicsPipelineVariant variant) const
{
  return selectGraphicsPipelineHandle(variant);
}

PipelineHandle Renderer::getLightPipelineHandle() const
{
  return m_lightPipeline;
}

PipelineHandle Renderer::getGBufferOpaquePipelineHandle() const
{
  return m_gbufferOpaquePipeline;
}

PipelineHandle Renderer::getGBufferAlphaTestPipelineHandle() const
{
  return m_gbufferAlphaTestPipeline;
}

PipelineHandle Renderer::getForwardPipelineHandle() const
{
  return m_forwardPipeline;
}

VkImageView Renderer::getCurrentSwapchainImageView() const
{
  if(m_swapchainDependent.swapchain == nullptr)
  {
    return VK_NULL_HANDLE;
  }
  return fromNativeHandle<VkImageView>(
      m_swapchainDependent.swapchain->getNativeImageView(m_swapchainDependent.currentImageIndex));
}

VkImage Renderer::getCurrentSwapchainImage() const
{
  if(m_swapchainDependent.swapchain == nullptr)
  {
    return VK_NULL_HANDLE;
  }
  return fromNativeHandle<VkImage>(
      m_swapchainDependent.swapchain->getNativeImage(m_swapchainDependent.currentImageIndex));
}

VkImageView Renderer::getOutputTextureView() const
{
  return m_swapchainDependent.sceneResources.getOutputTextureView();
}

void Renderer::render(const RenderParams& params)
{
  if(params.viewportSize.width > 0 && params.viewportSize.height > 0
     && (params.viewportSize.width != m_swapchainDependent.viewportSize.width
         || params.viewportSize.height != m_swapchainDependent.viewportSize.height))
  {
    resize(params.viewportSize);
  }

  if(!prepareFrameResources())
    return;

  rhi::CommandList& cmd = beginCommandRecording();
  drawFrame(cmd, params);
  endFrame(cmd);
}

// Pass execution wrappers (used by PassNode implementations)
void Renderer::executeComputePass(rhi::CommandList& cmd, const RenderParams& params) const
{
  recordComputeCommands(cmd, params);
}

void Renderer::executeGraphicsPass(rhi::CommandList& cmd, const RenderParams& params, std::span<const StreamEntry> drawStream)
{
  recordGraphicCommands(cmd, params, drawStream);
}

void Renderer::executeImGuiPass(rhi::CommandList& cmd, const RenderParams& params)
{
  if(params.recordUi)
  {
    params.recordUi(cmd);
  }
}

void Renderer::beginPresentPass(rhi::CommandList& cmd)
{
  beginDynamicRenderingToSwapchain(cmd);
}

void Renderer::endPresentPass(rhi::CommandList& cmd)
{
  endDynamicRenderingToSwapchain(cmd);

  // Transition swapchain image to PRESENT_SRC_KHR for presentation
  VkImage swapchainImage = getCurrentSwapchainImage();
  if(swapchainImage != VK_NULL_HANDLE)
  {
    cmd.transitionTexture(rhi::TextureBarrierDesc{
        .texture     = rhi::TextureHandle{kPassSwapchainHandle.index, kPassSwapchainHandle.generation},
        .nativeImage = reinterpret_cast<uint64_t>(swapchainImage),
        .aspect      = rhi::TextureAspect::color,
        .srcStage    = rhi::PipelineStage::FragmentShader,
        .dstStage    = rhi::PipelineStage::BottomOfPipe,
        .srcAccess   = rhi::ResourceAccess::write,
        .dstAccess   = rhi::ResourceAccess::read,
        .oldState    = rhi::ResourceState::General,
        .newState    = rhi::ResourceState::Present,
        .isSwapchain = true,
    });
  }
}

void Renderer::createTransientCommandPool()
{
  const rhi::QueueInfo          graphicsQueueInfo = m_device.device->getGraphicsQueue();
  const VkDevice                device            = fromNativeHandle<VkDevice>(m_device.device->getNativeDevice());
  const VkCommandPoolCreateInfo commandPoolCreateInfo{
      .sType            = VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO,
      .flags            = VK_COMMAND_POOL_CREATE_TRANSIENT_BIT,
      .queueFamilyIndex = graphicsQueueInfo.familyIndex,
  };
  VK_CHECK(vkCreateCommandPool(device, &commandPoolCreateInfo, nullptr, &m_device.transientCmdPool));
  DBG_VK_NAME(m_device.transientCmdPool);
}

void Renderer::createFrameSubmission(uint32_t numFrames)
{
  const rhi::QueueInfo graphicsQueueInfo = m_device.device->getGraphicsQueue();
  const VkDevice       device            = fromNativeHandle<VkDevice>(m_device.device->getNativeDevice());

  m_perFrame.frameContext = std::make_unique<rhi::vulkan::VulkanFrameContext>();
  m_perFrame.frameContext->init(static_cast<void*>(device), graphicsQueueInfo.familyIndex, numFrames);
  m_perFrame.frameContext->setSwapchain(m_swapchainDependent.swapchain.get());

  m_perFrame.frameUserData.resize(numFrames);
  for(auto& frameUserData : m_perFrame.frameUserData)
  {
    frameUserData.transientAllocator.init(*m_device.device, m_device.allocator, kPerFrameTransientAllocatorSize);
  }

  m_perFrame.frameCounter = 1;
}

bool Renderer::prepareFrameResources()
{
  rebuildSwapchainDependentResources();

  ASSERT(m_perFrame.frameContext != nullptr, "Per-frame FrameContext must be initialized");
  m_perFrame.frameContext->beginFrame();

  const uint32_t currentFrameIndex = m_perFrame.frameContext->getCurrentFrameIndex();
  ASSERT(currentFrameIndex < m_perFrame.frameUserData.size(), "Current frame index must map to frame user data");
  m_perFrame.frameUserData[currentFrameIndex].transientAllocator.reset();

  return acquireSwapchainImage(*m_swapchainDependent.swapchain, m_swapchainDependent.currentImageIndex);
}

void Renderer::updateGBufferTextureDescriptorSet()
{
  if(m_device.gbufferTextureSet == VK_NULL_HANDLE)
  {
    return;
  }

  const VkDevice nativeDevice = fromNativeHandle<VkDevice>(m_device.device->getNativeDevice());
  SceneResources& sceneResources = m_swapchainDependent.sceneResources;

  // Verify the SceneResources has valid image views
  for(uint32_t i = 0; i < 3; ++i)
  {
    const VkDescriptorImageInfo& sceneDesc = sceneResources.getDescriptorImageInfo(i);
    if(sceneDesc.imageView == VK_NULL_HANDLE)
    {
      LOGW("SceneResources image view %u is null, skipping GBuffer descriptor update", i);
      return;
    }
  }
  if(sceneResources.getDepthImageView() == VK_NULL_HANDLE)
  {
    LOGW("SceneResources depth image view is null, skipping GBuffer descriptor update");
    return;
  }

  const VkSampler linearSampler = m_device.samplerPool.acquireSampler(VkSamplerCreateInfo{
      .sType       = VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO,
      .magFilter   = VK_FILTER_LINEAR,
      .minFilter   = VK_FILTER_LINEAR,
      .addressModeU = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE,
      .addressModeV = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE,
      .addressModeW = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE,
      .maxLod      = VK_LOD_CLAMP_NONE,
  });

  // Write descriptor array for GBuffer textures (indices 0-2) + depth (index 3)
  std::array<VkDescriptorImageInfo, 4> imageInfos{};
  for(uint32_t i = 0; i < 3; ++i)
  {
    const VkDescriptorImageInfo& sceneDesc = sceneResources.getDescriptorImageInfo(i);
    imageInfos[i] = VkDescriptorImageInfo{
        .sampler     = linearSampler,
        .imageView   = sceneDesc.imageView,
        .imageLayout = VK_IMAGE_LAYOUT_GENERAL,
    };
  }
  // Depth texture at index 3
  imageInfos[3] = VkDescriptorImageInfo{
      .sampler     = linearSampler,
      .imageView   = sceneResources.getDepthImageView(),
      .imageLayout = VK_IMAGE_LAYOUT_GENERAL,
  };

  // Camera uniform buffer binding (binding 1)
  // We use a fixed buffer for LightPass camera data - allocate from transient allocator
  // For now, we'll update this per-frame in LightPass using dynamic UBO
  // Create a dedicated camera buffer for LightPass
  const VkDescriptorBufferInfo cameraBufferInfo{
      .buffer = reinterpret_cast<VkBuffer>(m_perFrame.frameUserData[0].transientAllocator.getBufferOpaque()),
      .offset = 0,
      .range  = sizeof(shaderio::CameraUniforms),
  };

  const std::array<VkWriteDescriptorSet, 2> writes = {
      // Texture array (binding 0, 4 textures)
      VkWriteDescriptorSet{
          .sType           = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET,
          .dstSet          = m_device.gbufferTextureSet,
          .dstBinding      = 0,
          .dstArrayElement = 0,
          .descriptorCount = 4,
          .descriptorType  = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER,
          .pImageInfo      = imageInfos.data(),
      },
      // Camera uniform buffer (binding 1)
      VkWriteDescriptorSet{
          .sType           = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET,
          .dstSet          = m_device.gbufferTextureSet,
          .dstBinding      = 1,
          .dstArrayElement = 0,
          .descriptorCount = 1,
          .descriptorType  = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER,
          .pBufferInfo     = &cameraBufferInfo,
      },
  };

  vkUpdateDescriptorSets(nativeDevice, static_cast<uint32_t>(writes.size()), writes.data(), 0, nullptr);
}

void Renderer::rebuildSwapchainDependentResources(std::optional<VkExtent2D> requestedViewportSize)
{
  if(requestedViewportSize.has_value() && isValidExtent(requestedViewportSize.value()))
  {
    m_swapchainDependent.viewportSize = requestedViewportSize.value();
  }

  bool swapchainRebuilt = false;
  if(m_swapchainDependent.swapchain->needsRebuild())
  {
    m_swapchainDependent.swapchain->rebuild();
    const rhi::Extent2D extent             = m_swapchainDependent.swapchain->getExtent();
    m_swapchainDependent.windowSize        = VkExtent2D{extent.width, extent.height};
    m_swapchainDependent.currentImageIndex = 0;
    m_swapchainDependent.imageStates.assign(
        m_swapchainDependent.swapchain->getMaxFramesInFlight(),
        rhi::ResourceState::Undefined);
    swapchainRebuilt                       = true;
  }

  const VkExtent2D gBufferSize = m_swapchainDependent.sceneResources.getSize();
  if(!extentChanged(gBufferSize, m_swapchainDependent.viewportSize))
  {
    return;
  }

  if(!swapchainRebuilt)
  {
    m_device.device->waitIdle();
  }

  const VkDevice  device        = fromNativeHandle<VkDevice>(m_device.device->getNativeDevice());
  const VkQueue   graphicsQueue = fromNativeHandle<VkQueue>(m_device.device->getGraphicsQueue().nativeHandle);
  VkCommandBuffer cmd           = utils::beginSingleTimeCommands(device, m_device.transientCmdPool);
  m_swapchainDependent.sceneResources.update(cmd, m_swapchainDependent.viewportSize);
  utils::endSingleTimeCommands(cmd, device, m_device.transientCmdPool, graphicsQueue);

  // Update GBuffer texture descriptor set after potential SceneResources resize
  updateGBufferTextureDescriptorSet();
}

rhi::CommandList& Renderer::beginCommandRecording()
{
  ASSERT(m_perFrame.frameContext != nullptr, "Per-frame FrameContext must be initialized");
  rhi::FrameData& frame = m_perFrame.frameContext->getCurrentFrame();
  ASSERT(frame.commandList != nullptr, "Current frame command list must be valid");
  return *frame.commandList;
}

void Renderer::drawFrame(rhi::CommandList& cmd, const RenderParams& params)
{
  const uint32_t currentFrameIndex = m_perFrame.frameContext->getCurrentFrameIndex();
  auto&          frameUserData     = m_perFrame.frameUserData[currentFrameIndex];

  // Get the actual initial state of the current swapchain image
  // Newly created/rebuilt swapchain images start as UNDEFINED
  if(m_swapchainDependent.currentImageIndex >= m_swapchainDependent.imageStates.size())
  {
    m_swapchainDependent.imageStates.resize(
        m_swapchainDependent.swapchain->getMaxFramesInFlight(),
        rhi::ResourceState::Undefined);
  }
  const rhi::ResourceState swapchainInitialState =
      m_swapchainDependent.imageStates[m_swapchainDependent.currentImageIndex];

  // Route through pass executor to orchestrate multi-pass rendering
  m_passExecutor.clearResourceBindings();
  m_passExecutor.bindBuffer({
      .handle       = kPassVertexBufferHandle,
      .nativeBuffer = reinterpret_cast<uint64_t>(m_device.vertexBuffer.buffer),
  });
  m_passExecutor.bindBuffer({
      .handle       = kTransientAllocatorBufferHandle,
      .nativeBuffer = frameUserData.transientAllocator.getBufferOpaque(),
  });
  m_passExecutor.bindTexture({
      .handle       = kPassGBufferColorHandle,
      .nativeImage  = reinterpret_cast<uint64_t>(m_swapchainDependent.sceneResources.getColorImage()),
      .aspect       = rhi::TextureAspect::color,
      .initialState = rhi::ResourceState::general,
      .isSwapchain  = false,
  });
  m_passExecutor.bindTexture({
      .handle       = kPassGBufferDepthHandle,
      .nativeImage  = reinterpret_cast<uint64_t>(m_swapchainDependent.sceneResources.getDepthImage()),
      .aspect       = rhi::TextureAspect::depth,
      .initialState = rhi::ResourceState::general,
      .isSwapchain  = false,
  });
  m_passExecutor.bindTexture({
      .handle       = kPassOutputHandle,
      .nativeImage  = reinterpret_cast<uint64_t>(m_swapchainDependent.sceneResources.getOutputTextureImage()),
      .aspect       = rhi::TextureAspect::color,
      .initialState = rhi::ResourceState::general,
      .isSwapchain  = false,
  });
  m_passExecutor.bindTexture({
      .handle       = kPassSwapchainHandle,
      .nativeImage  = m_swapchainDependent.swapchain->getNativeImage(m_swapchainDependent.currentImageIndex),
      .aspect       = rhi::TextureAspect::color,
      .initialState = swapchainInitialState,
      .isSwapchain  = true,
  });

  m_perPass.drawStream.clear();
  demo::PassContext context{
      &cmd, &frameUserData.transientAllocator, currentFrameIndex, 0, &params, &m_perPass.drawStream, params.gltfModel,
      m_materials.materialBindGroup};
  m_passExecutor.execute(context);

  // Update swapchain image state after rendering
  m_swapchainDependent.imageStates[m_swapchainDependent.currentImageIndex] = rhi::ResourceState::Present;
}

void Renderer::endFrame(rhi::CommandList& cmd)
{
  (void)cmd;
  ASSERT(m_perFrame.frameContext != nullptr, "Per-frame FrameContext must be initialized");

  submitFrame(*m_perFrame.frameContext, cmd);

  m_perFrame.frameCounter++;

  presentFrame(*m_swapchainDependent.swapchain);
}

void Renderer::beginDynamicRenderingToSwapchain(const rhi::CommandList& cmd) const
{
  const VkImageView swapchainImageView =
      fromNativeHandle<VkImageView>(m_swapchainDependent.swapchain->getNativeImageView(m_swapchainDependent.currentImageIndex));
  const std::array<VkRenderingAttachmentInfo, 1> colorAttachment{{{
      .sType       = VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO_KHR,
      .imageView   = swapchainImageView,
      .imageLayout = VK_IMAGE_LAYOUT_GENERAL,
      .loadOp      = VK_ATTACHMENT_LOAD_OP_LOAD,  // Preserve LightPass/ForwardPass output
      .storeOp     = VK_ATTACHMENT_STORE_OP_STORE,
  }}};

  const VkRenderingInfo renderingInfo{
      .sType                = VK_STRUCTURE_TYPE_RENDERING_INFO,
      .renderArea           = {{0, 0}, m_swapchainDependent.windowSize},
      .layerCount           = 1,
      .colorAttachmentCount = uint32_t(colorAttachment.size()),
      .pColorAttachments    = colorAttachment.data(),
  };

  rhi::vulkan::cmdBeginRendering(cmd, renderingInfo);
}

void Renderer::endDynamicRenderingToSwapchain(const rhi::CommandList& cmd)
{
  rhi::vulkan::cmdEndRendering(cmd);
}

void Renderer::recordComputeCommands(rhi::CommandList& cmd, const RenderParams& params) const
{
  DBG_VK_SCOPE(rhi::vulkan::getNativeCommandBuffer(cmd));
  ASSERT(m_device.computePipelineLayout != nullptr, "Compute pipeline layout must be initialized before compute recording");
  const VkPipelineLayout computePipelineLayout =
      reinterpret_cast<VkPipelineLayout>(m_device.computePipelineLayout->getNativeHandle());

  const shaderio::PushConstantCompute pushValues{
      .bufferAddress = m_device.vertexBuffer.address,
      .rotationAngle = 1.2f * params.deltaTime,
      .numVertex     = 3,
  };

  const VkPushConstantsInfo pushInfo{
      .sType      = VK_STRUCTURE_TYPE_PUSH_CONSTANTS_INFO,
      .layout     = computePipelineLayout,
      .stageFlags = VK_SHADER_STAGE_COMPUTE_BIT,
      .offset     = 0,
      .size       = sizeof(shaderio::PushConstantCompute),
      .pValues    = &pushValues,
  };
  rhi::vulkan::cmdPushConstants(cmd, pushInfo);

  const PipelineHandle computePipelineHandle = selectComputePipelineHandle();
  rhi::vulkan::cmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_COMPUTE,
                               reinterpret_cast<VkPipeline>(getPipelineOpaque(
                                   computePipelineHandle, static_cast<uint32_t>(VK_PIPELINE_BIND_POINT_COMPUTE))));
  rhi::vulkan::cmdDispatch(cmd, 1, 1, 1);
}

rhi::ResourceIndex Renderer::resolveMaterialResourceIndex(MaterialHandle handle) const
{
  const MaterialResources::MaterialRecord* materialRecord = tryGetMaterial(handle);
  if(materialRecord == nullptr)
  {
    LOGW("Renderer::resolveMaterialResourceIndex rejected stale/invalid material handle (index=%u generation=%u)",
         handle.index, handle.generation);
    return rhi::kInvalidResourceIndex;
  }

  const MaterialResources::TextureHotData* textureHot = tryGetTextureHot(materialRecord->sampledTexture);
  if(textureHot == nullptr)
  {
    LOGW("Renderer::resolveMaterialResourceIndex encountered stale/invalid texture handle (index=%u generation=%u)",
         materialRecord->sampledTexture.index, materialRecord->sampledTexture.generation);
    return rhi::kInvalidResourceIndex;
  }

  if(textureHot->runtimeKind != MaterialResources::TextureRuntimeKind::materialSampled)
  {
    return rhi::kInvalidResourceIndex;
  }

  return materialRecord->descriptorIndex;
}

uint32_t Renderer::allocateDrawDynamicOffset(rhi::ResourceIndex materialIndex, const RenderParams& params)
{
  VkPhysicalDeviceProperties2 deviceProperties2{.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PROPERTIES_2};
  vkGetPhysicalDeviceProperties2(fromNativeHandle<VkPhysicalDevice>(m_device.device->getNativePhysicalDevice()), &deviceProperties2);
  const uint32_t dynamicAlignment = alignUp(uint32_t(sizeof(shaderio::SceneInfo)),
                                            uint32_t(deviceProperties2.properties.limits.minUniformBufferOffsetAlignment));

  const float time        = static_cast<float>(5.0 * params.timeSeconds);
  const float sineValue   = std::sin(time) + 1.0f;
  const float mappedValue = 0.5f * sineValue + 0.5f;

  shaderio::SceneInfo sceneInfo{};
  sceneInfo.animValue         = mappedValue;
  sceneInfo.dataBufferAddress = m_device.pointsBuffer.address;
  sceneInfo.resolution = glm::vec2(m_swapchainDependent.viewportSize.width, m_swapchainDependent.viewportSize.height);
  sceneInfo.numData    = uint32_t(s_points.size());
  sceneInfo.texId      = materialIndex;

  const uint32_t      currentFrameIndex  = m_perFrame.frameContext->getCurrentFrameIndex();
  TransientAllocator& transientAllocator = m_perFrame.frameUserData[currentFrameIndex].transientAllocator;
  const TransientAllocator::Allocation allocation =
      transientAllocator.allocate(uint32_t(sizeof(shaderio::SceneInfo)), dynamicAlignment);
  std::memcpy(allocation.cpuPtr, &sceneInfo, sizeof(sceneInfo));
  transientAllocator.flushAllocation(allocation, uint32_t(sizeof(sceneInfo)));
  ASSERT(allocation.handle == kTransientAllocatorBufferHandle, "Scene dynamic allocations must originate from transient allocator handle");
  return allocation.offset;
}

void Renderer::recordGraphicCommands(rhi::CommandList& cmd, const RenderParams& params, std::span<const StreamEntry> drawStream)
{
  DBG_VK_SCOPE(rhi::vulkan::getNativeCommandBuffer(cmd));
  ASSERT(m_device.graphicPipelineLayout != nullptr, "Graphics pipeline layout must be initialized before graphics recording");
  const VkPipelineLayout graphicsPipelineLayout =
      reinterpret_cast<VkPipelineLayout>(m_device.graphicPipelineLayout->getNativeHandle());

  if(!m_drawStreamDecoder.decode(DrawStream(drawStream.begin(), drawStream.end()), m_perPass.decodedDraws))
  {
    LOGW("Renderer::recordGraphicCommands received malformed DrawStream; skipping pass");
    return;
  }

  std::vector<DrawPacket> oraclePackets;
  if(!m_drawStreamDecoder.decodeToDrawPackets(DrawStream(drawStream.begin(), drawStream.end()), oraclePackets))
  {
    LOGW("Renderer::recordGraphicCommands stream->packet oracle decode failed; skipping pass");
    return;
  }
  ASSERT(oraclePackets.size() == m_perPass.decodedDraws.size(), "Draw stream oracle packet count must match decoded draw count");

  const VkViewport viewport{
      0.0F, 0.0F, float(m_swapchainDependent.viewportSize.width), float(m_swapchainDependent.viewportSize.height),
      0.0F, 1.0F};
  const VkRect2D scissor{{0, 0}, m_swapchainDependent.viewportSize};

  shaderio::PushConstant    pushValues{};
  const VkPushConstantsInfo pushInfo{
      .sType      = VK_STRUCTURE_TYPE_PUSH_CONSTANTS_INFO,
      .layout     = graphicsPipelineLayout,
      .stageFlags = VK_SHADER_STAGE_ALL_GRAPHICS,
      .offset     = 0,
      .size       = sizeof(shaderio::PushConstant),
      .pValues    = &pushValues,
  };

  const std::array<VkRenderingAttachmentInfo, 1> colorAttachment{{{
      .sType       = VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO,
      .imageView   = m_swapchainDependent.sceneResources.getColorImageView(),
      .imageLayout = VK_IMAGE_LAYOUT_GENERAL,
      .loadOp      = VK_ATTACHMENT_LOAD_OP_CLEAR,
      .storeOp     = VK_ATTACHMENT_STORE_OP_STORE,
      .clearValue  = {{{params.clearColor.r, params.clearColor.g, params.clearColor.b, params.clearColor.a}}},
  }}};

  const VkRenderingAttachmentInfo depthAttachment{
      .sType       = VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO,
      .imageView   = m_swapchainDependent.sceneResources.getDepthImageView(),
      .imageLayout = VK_IMAGE_LAYOUT_GENERAL,
      .loadOp      = VK_ATTACHMENT_LOAD_OP_CLEAR,
      .storeOp     = VK_ATTACHMENT_STORE_OP_STORE,
      .clearValue  = {{{1.0f, 0}}},
  };

  const VkRenderingInfo renderingInfo{
      .sType                = VK_STRUCTURE_TYPE_RENDERING_INFO,
      .renderArea           = {{0, 0}, m_swapchainDependent.sceneResources.getSize()},
      .layerCount           = 1,
      .colorAttachmentCount = uint32_t(colorAttachment.size()),
      .pColorAttachments    = colorAttachment.data(),
      .pDepthAttachment     = &depthAttachment,
  };

  rhi::vulkan::cmdBeginRendering(cmd, renderingInfo);

  rhi::vulkan::cmdSetViewport(cmd, viewport);
  rhi::vulkan::cmdSetScissor(cmd, scissor);

  const std::optional<uint32_t> materialSetIndex = mapSetSlotToLegacyShaderSet(BindGroupSetSlot::material);
  ASSERT(materialSetIndex.has_value(), "material bind-group slot must map to active shader set");
  const std::optional<uint32_t> sceneSetIndex = mapSetSlotToLegacyShaderSet(BindGroupSetSlot::drawDynamic);
  ASSERT(sceneSetIndex.has_value(), "drawDynamic bind-group slot must map to active shader set");

  const uint64_t materialBindTableHandle =
      getBindGroupDescriptorSetOpaque(m_materials.materialBindGroup, BindGroupSetSlot::material);
  rhi::vulkan::cmdBindDescriptorSetOpaque(cmd, static_cast<uint32_t>(VK_PIPELINE_BIND_POINT_GRAPHICS),
                                          m_device.graphicPipelineLayout->getNativeHandle(), materialSetIndex.value(),
                                          materialBindTableHandle, 0, nullptr);

  const uint32_t currentFrameIndex = m_perFrame.frameContext->getCurrentFrameIndex();
  const auto&    frameUserData     = m_perFrame.frameUserData[currentFrameIndex];
  const uint64_t sceneBindTableHandle = getBindGroupDescriptorSetOpaque(frameUserData.sceneBindGroup, BindGroupSetSlot::drawDynamic);
  for(const DrawStreamDecoder::DecodedDraw& decodedDraw : m_perPass.decodedDraws)
  {
    ASSERT(decodedDraw.state.dynamicBufferIndex == getSceneBindlessResourceIndex(),
           "Draw stream dynamic data must reference scene bindless logical index");
    ASSERT(rhi::isValidResourceIndex(decodedDraw.state.materialIndex), "Draw stream must provide a valid material bindless logical index");
    ASSERT(decodedDraw.state.dynamicOffset != kDrawStreamInvalidDynamicOffset,
           "Draw stream must provide valid dynamic offset before draw");
    const uint32_t dynamicSceneOffset = decodedDraw.state.dynamicOffset;
    rhi::vulkan::cmdBindDescriptorSetOpaque(cmd, static_cast<uint32_t>(VK_PIPELINE_BIND_POINT_GRAPHICS),
                                            m_device.graphicPipelineLayout->getNativeHandle(), sceneSetIndex.value(),
                                            sceneBindTableHandle, 1, &dynamicSceneOffset);

    rhi::vulkan::cmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS,
                                 reinterpret_cast<VkPipeline>(this->getPipelineOpaque(
                                     decodedDraw.state.pipeline, static_cast<uint32_t>(VK_PIPELINE_BIND_POINT_GRAPHICS))));

    // Check if we're using a glTF mesh or the default vertex buffer
    const MeshRecord* meshRecord = m_meshPool.tryGet(decodedDraw.state.mesh);
    if(meshRecord != nullptr)
    {
      // Bind glTF mesh vertex and index buffers
      const VkDeviceSize vertexOffset = 0;
      VkBuffer vertexBuffer = meshRecord->getNativeVertexBuffer();
      VkBuffer indexBuffer = meshRecord->getNativeIndexBuffer();
      rhi::vulkan::cmdBindVertexBuffers(cmd, 0, 1, &vertexBuffer, &vertexOffset);
      rhi::vulkan::cmdBindIndexBuffer(cmd, indexBuffer, 0, VK_INDEX_TYPE_UINT32);

      // Use default push constants for now
      pushValues.color = glm::vec3(1, 1, 1);
      rhi::vulkan::cmdPushConstants(cmd, pushInfo);

      // Indexed draw
      if(decodedDraw.isIndexed)
      {
        rhi::vulkan::cmdDrawIndexed(cmd, decodedDraw.indexCount, decodedDraw.instanceCount,
                                    decodedDraw.firstIndex, decodedDraw.vertexOffsetIndexed, decodedDraw.firstInstance);
      }
      else
      {
        rhi::vulkan::cmdDraw(cmd, decodedDraw.vertexCount, decodedDraw.instanceCount, 0, 0);
      }
    }
    else
    {
      // Use default vertex buffer (triangle)
      const VkDeviceSize drawVertexOffset = static_cast<VkDeviceSize>(decodedDraw.vertexOffset) * sizeof(shaderio::Vertex);
      const VkDeviceSize drawOffsets[] = {drawVertexOffset};
      rhi::vulkan::cmdBindVertexBuffers(cmd, 0, 1, &m_device.vertexBuffer.buffer, drawOffsets);

      pushValues.color = (decodedDraw.vertexOffset == 0) ? glm::vec3(1, 0, 0) : glm::vec3(0, 1, 0);
      rhi::vulkan::cmdPushConstants(cmd, pushInfo);

      rhi::vulkan::cmdDraw(cmd, decodedDraw.vertexCount, decodedDraw.instanceCount, 0, 0);
    }
  }

  rhi::vulkan::cmdEndRendering(cmd);
}

void Renderer::prebuildRequiredPipelineVariants()
{
  createPrebuiltGraphicsPipelineVariants();
  createPrebuiltComputePipelineVariant();
}

void Renderer::createPrebuiltGraphicsPipelineVariants()
{
#ifdef USE_SLANG
  const char* vertEntryName = "vertexMain";
  const char* fragEntryName = "fragmentMain";

  VkShaderModule vertShaderModule = utils::createShaderModule(fromNativeHandle<VkDevice>(m_device.device->getNativeDevice()),
                                                              {shader_rast_slang, std::size(shader_rast_slang)});
  DBG_VK_NAME(vertShaderModule);
  VkShaderModule fragShaderModule = vertShaderModule;
#else
  const char* vertEntryName = "main";
  const char* fragEntryName = "main";

  VkShaderModule vertShaderModule = utils::createShaderModule(fromNativeHandle<VkDevice>(m_device.device->getNativeDevice()),
                                                              {shader_vert_glsl, std::size(shader_vert_glsl)});
  DBG_VK_NAME(vertShaderModule);
  VkShaderModule fragShaderModule = utils::createShaderModule(fromNativeHandle<VkDevice>(m_device.device->getNativeDevice()),
                                                              {shader_frag_glsl, std::size(shader_frag_glsl)});
  DBG_VK_NAME(fragShaderModule);
#endif

  const auto bindingDescription    = Vertex::getBindingDescription();
  const auto attributeDescriptions = Vertex::getAttributeDescriptions();

  const uint64_t materialLayoutHandle = getBindGroupLayoutOpaque(m_materials.materialBindGroup, BindGroupSetSlot::material);
  ASSERT(!m_perFrame.frameUserData.empty(), "Per-frame resources must exist before graphics pipeline creation");
  const uint64_t sceneLayoutHandle =
      getBindGroupLayoutOpaque(m_perFrame.frameUserData.front().sceneBindGroup, BindGroupSetSlot::drawDynamic);

  const rhi::ShaderReflectionData rasterReflection = buildRasterShaderReflection();
  rhi::PipelineLayoutDesc         layoutDesc       = rhi::derivePipelineLayoutDesc(rasterReflection);
  layoutDesc.debugName                             = "graphics-layout";

  const std::array<rhi::vulkan::VulkanPipelineLayoutBindingMapping, 2> layoutMappings{{
      rhi::vulkan::makePipelineLayoutBindingMapping(static_cast<uint32_t>(shaderio::LSetTextures), materialLayoutHandle),
      rhi::vulkan::makePipelineLayoutBindingMapping(static_cast<uint32_t>(shaderio::LSetScene), sceneLayoutHandle),
  }};
  const rhi::vulkan::VulkanPipelineLayoutLowering                      layoutLowering{
      .setLayouts     = layoutMappings.data(),
      .setLayoutCount = static_cast<uint32_t>(layoutMappings.size()),
  };

  auto graphicsPipelineLayout = std::make_unique<rhi::vulkan::VulkanPipelineLayout>();
  graphicsPipelineLayout->init(static_cast<void*>(fromNativeHandle<VkDevice>(m_device.device->getNativeDevice())),
                               layoutDesc, layoutLowering);
  DBG_VK_NAME(reinterpret_cast<VkPipelineLayout>(graphicsPipelineLayout->getNativeHandle()));
  m_device.graphicPipelineLayout = std::move(graphicsPipelineLayout);

  const std::array<rhi::VertexBindingDesc, 1>   vertexBindings{{
      rhi::VertexBindingDesc{
          .binding   = bindingDescription[0].binding,
          .stride    = bindingDescription[0].stride,
          .inputRate = rhi::VertexInputRate::perVertex,
      },
  }};
  const std::array<rhi::VertexAttributeDesc, 3> vertexAttributes{{
      rhi::VertexAttributeDesc{
          .location = attributeDescriptions[0].location,
          .binding  = attributeDescriptions[0].binding,
          .format   = rhi::VertexFormat::r32g32b32Sfloat,
          .offset   = attributeDescriptions[0].offset,
      },
      rhi::VertexAttributeDesc{
          .location = attributeDescriptions[1].location,
          .binding  = attributeDescriptions[1].binding,
          .format   = rhi::VertexFormat::r32g32b32Sfloat,
          .offset   = attributeDescriptions[1].offset,
      },
      rhi::VertexAttributeDesc{
          .location = attributeDescriptions[2].location,
          .binding  = attributeDescriptions[2].binding,
          .format   = rhi::VertexFormat::r32g32Sfloat,
          .offset   = attributeDescriptions[2].offset,
      },
  }};
  const rhi::VertexInputLayoutDesc              vertexInput{
      .bindings       = vertexBindings.data(),
      .bindingCount   = static_cast<uint32_t>(vertexBindings.size()),
      .attributes     = vertexAttributes.data(),
      .attributeCount = static_cast<uint32_t>(vertexAttributes.size()),
  };

  const std::array<rhi::DynamicState, 2>         dynamicStates{{
      rhi::DynamicState::viewport,
      rhi::DynamicState::scissor,
  }};
  const std::array<rhi::BlendAttachmentState, 1> blendStates{{
      rhi::BlendAttachmentState{
          .blendEnable         = true,
          .srcColorBlendFactor = rhi::BlendFactor::srcAlpha,
          .dstColorBlendFactor = rhi::BlendFactor::oneMinusSrcAlpha,
          .colorBlendOp        = rhi::BlendOp::add,
          .srcAlphaBlendFactor = rhi::BlendFactor::srcAlpha,
          .dstAlphaBlendFactor = rhi::BlendFactor::oneMinusSrcAlpha,
          .alphaBlendOp        = rhi::BlendOp::add,
          .colorWriteMask      = rhi::ColorComponentFlags::all,
      },
  }};

  std::array<rhi::PipelineShaderStageDesc, 2> shaderStages{{
      rhi::PipelineShaderStageDesc{
          .stage        = rhi::ShaderStage::vertex,
          .shaderModule = reinterpret_cast<uint64_t>(vertShaderModule),
          .entryPoint   = vertEntryName,
      },
      rhi::PipelineShaderStageDesc{
          .stage                 = rhi::ShaderStage::fragment,
          .shaderModule          = reinterpret_cast<uint64_t>(fragShaderModule),
          .entryPoint            = fragEntryName,
          .specializationVariant = 1,
      },
  }};

  // Specialization constants for useTexture (constant_id = 0)
  // Textured variant: useTexture = true (must be VkBool32 = uint32_t = 4 bytes)
  uint32_t                                useTextureTrue = VK_TRUE;
  rhi::SpecializationConstant             specConstantTrue(0, 0, sizeof(uint32_t));
  rhi::SpecializationData                 specDataTrue{&useTextureTrue, sizeof(uint32_t)};
  shaderStages[1].specializationData         = specDataTrue;
  shaderStages[1].specializationConstants    = &specConstantTrue;
  shaderStages[1].specializationConstantCount = 1;

  const std::array<rhi::TextureFormat, 1> colorFormats{{
      toPortableTextureFormat(m_swapchainDependent.sceneResources.getColorFormat()),
  }};

  rhi::GraphicsPipelineDesc graphicsDesc{
      .layout            = m_device.graphicPipelineLayout.get(),
      .shaderStages      = shaderStages.data(),
      .shaderStageCount  = static_cast<uint32_t>(shaderStages.size()),
      .vertexInput       = vertexInput,
      .rasterState       = rhi::RasterState{},
      .depthState        = rhi::DepthState{true, true, rhi::CompareOp::lessOrEqual},
      .blendStates       = blendStates.data(),
      .blendStateCount   = static_cast<uint32_t>(blendStates.size()),
      .dynamicStates     = dynamicStates.data(),
      .dynamicStateCount = static_cast<uint32_t>(dynamicStates.size()),
      .renderingInfo =
          {
              .colorFormats     = colorFormats.data(),
              .colorFormatCount = static_cast<uint32_t>(colorFormats.size()),
              .depthFormat      = toPortableTextureFormat(m_swapchainDependent.sceneResources.getDepthFormat()),
          },
  };
  graphicsDesc.rasterState.topology    = rhi::PrimitiveTopology::triangleList;
  graphicsDesc.rasterState.cullMode    = rhi::CullMode::none;
  graphicsDesc.rasterState.frontFace   = rhi::FrontFace::counterClockwise;
  graphicsDesc.rasterState.sampleCount = rhi::SampleCount::count1;

  rhi::vulkan::GraphicsPipelineCreateInfo graphicsCreateInfo{
      .key =
          {
              .shaderIdentity        = rhi::vulkan::PipelineShaderIdentity::raster,
              .specializationVariant = 1,
          },
      .desc = graphicsDesc,
  };
  const VkPipeline graphicsPipelineWithTexture =
      rhi::vulkan::createGraphicsPipeline(fromNativeHandle<VkDevice>(m_device.device->getNativeDevice()), graphicsCreateInfo);
  DBG_VK_NAME(graphicsPipelineWithTexture);
  m_device.prebuiltPipelines.graphicsTextured = registerPipeline(static_cast<uint32_t>(VK_PIPELINE_BIND_POINT_GRAPHICS),
                                                                 reinterpret_cast<uint64_t>(graphicsPipelineWithTexture),
                                                                 graphicsCreateInfo.key.specializationVariant);

  graphicsCreateInfo.key.specializationVariant = 0;
  shaderStages[1].specializationVariant        = 0;
  // Non-textured variant: useTexture = false (must be VkBool32 = uint32_t = 4 bytes)
  uint32_t useTextureFalse = VK_FALSE;
  rhi::SpecializationConstant specConstantFalse(0, 0, sizeof(uint32_t));
  rhi::SpecializationData     specDataFalse{&useTextureFalse, sizeof(uint32_t)};
  shaderStages[1].specializationData         = specDataFalse;
  shaderStages[1].specializationConstants    = &specConstantFalse;
  shaderStages[1].specializationConstantCount = 1;
  const VkPipeline graphicsPipelineWithoutTexture =
      rhi::vulkan::createGraphicsPipeline(fromNativeHandle<VkDevice>(m_device.device->getNativeDevice()), graphicsCreateInfo);
  DBG_VK_NAME(graphicsPipelineWithoutTexture);
  m_device.prebuiltPipelines.graphicsNonTextured =
      registerPipeline(static_cast<uint32_t>(VK_PIPELINE_BIND_POINT_GRAPHICS),
                       reinterpret_cast<uint64_t>(graphicsPipelineWithoutTexture), graphicsCreateInfo.key.specializationVariant);

  vkDestroyShaderModule(fromNativeHandle<VkDevice>(m_device.device->getNativeDevice()), vertShaderModule, nullptr);
#ifndef USE_SLANG
  vkDestroyShaderModule(fromNativeHandle<VkDevice>(m_device.device->getNativeDevice()), fragShaderModule, nullptr);
#endif

  // Create GBuffer pipeline layout with uniform buffer bindings
  {
    // Set 1: Camera uniform buffer layout (dynamic)
    std::vector<rhi::BindTableLayoutEntry> cameraLayoutEntries{
        rhi::BindTableLayoutEntry{
            .logicalIndex    = shaderio::LBindCamera,
            .resourceType    = rhi::BindlessResourceType::uniformBufferDynamic,
            .descriptorCount = 1,
            .visibility      = rhi::ResourceVisibility::vertex,
        },
    };

    // Create per-frame camera bind groups (each with its own layout)
    VkDescriptorSetLayout cameraSetLayout = VK_NULL_HANDLE;
    for(uint32_t i = 0; i < m_perFrame.frameUserData.size(); ++i)
    {
      auto* cameraLayout = new rhi::vulkan::VulkanBindTableLayout();
      cameraLayout->init(static_cast<void*>(fromNativeHandle<VkDevice>(m_device.device->getNativeDevice())), cameraLayoutEntries);
      DBG_VK_NAME(cameraLayout->getVkDescriptorSetLayout());

      auto* cameraTable = new rhi::vulkan::VulkanBindTable();
      cameraTable->init(static_cast<void*>(fromNativeHandle<VkDevice>(m_device.device->getNativeDevice())),
                        *cameraLayout, 1);
      DBG_VK_NAME(cameraTable->getVkDescriptorSet());

      BindGroupDesc cameraBindGroupDesc{
          .slot                = BindGroupSetSlot::shaderSpecific,
          .layout              = cameraLayout,
          .table               = cameraTable,
          .primaryLogicalIndex = shaderio::LBindCamera,
          .debugName           = "gbuffer-camera",
      };
      m_perFrame.frameUserData[i].cameraBindGroup = createBindGroup(cameraBindGroupDesc);

      // Initialize the descriptor to point to this frame's transient allocator buffer
      // Dynamic offsets will be used at draw time to access specific allocations
      // For dynamic UBOs, range must be <= maxUniformBufferRange and is the size of the uniform struct
      VkDescriptorBufferInfo cameraBufferInfo{
          .buffer = reinterpret_cast<VkBuffer>(m_perFrame.frameUserData[i].transientAllocator.getBufferOpaque()),
          .offset = 0,
          .range  = sizeof(shaderio::CameraUniforms),
      };
      VkWriteDescriptorSet cameraWrite{
          .sType           = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET,
          .dstSet          = cameraTable->getVkDescriptorSet(),
          .dstBinding      = shaderio::LBindCamera,
          .dstArrayElement = 0,
          .descriptorCount = 1,
          .descriptorType  = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER_DYNAMIC,
          .pBufferInfo     = &cameraBufferInfo,
      };
      vkUpdateDescriptorSets(fromNativeHandle<VkDevice>(m_device.device->getNativeDevice()),
                             1, &cameraWrite, 0, nullptr);

      // Save the layout handle for pipeline layout creation (use first one, they're all identical)
      if(i == 0)
      {
        cameraSetLayout = cameraLayout->getVkDescriptorSetLayout();
      }
    }

    // Set 2: Draw uniform buffer layout (dynamic)
    std::vector<rhi::BindTableLayoutEntry> drawLayoutEntries{
        rhi::BindTableLayoutEntry{
            .logicalIndex    = shaderio::LBindDrawModel,
            .resourceType    = rhi::BindlessResourceType::uniformBufferDynamic,
            .descriptorCount = 1,
            .visibility      = rhi::ResourceVisibility::vertex | rhi::ResourceVisibility::fragment,
        },
    };

    // Create per-frame draw bind groups (each with its own layout)
    VkDescriptorSetLayout drawSetLayout = VK_NULL_HANDLE;
    for(uint32_t i = 0; i < m_perFrame.frameUserData.size(); ++i)
    {
      auto* drawLayout = new rhi::vulkan::VulkanBindTableLayout();
      drawLayout->init(static_cast<void*>(fromNativeHandle<VkDevice>(m_device.device->getNativeDevice())), drawLayoutEntries);
      DBG_VK_NAME(drawLayout->getVkDescriptorSetLayout());

      auto* drawTable = new rhi::vulkan::VulkanBindTable();
      drawTable->init(static_cast<void*>(fromNativeHandle<VkDevice>(m_device.device->getNativeDevice())),
                      *drawLayout, 1);
      DBG_VK_NAME(drawTable->getVkDescriptorSet());

      BindGroupDesc drawBindGroupDesc{
          .slot                = BindGroupSetSlot::shaderSpecific,
          .layout              = drawLayout,
          .table               = drawTable,
          .primaryLogicalIndex = shaderio::LBindDrawModel,
          .debugName           = "gbuffer-draw",
      };
      m_perFrame.frameUserData[i].drawBindGroup = createBindGroup(drawBindGroupDesc);

      // Initialize the descriptor to point to this frame's transient allocator buffer
      // For dynamic UBOs, range must be <= maxUniformBufferRange and is the size of the uniform struct
      VkDescriptorBufferInfo drawBufferInfo{
          .buffer = reinterpret_cast<VkBuffer>(m_perFrame.frameUserData[i].transientAllocator.getBufferOpaque()),
          .offset = 0,
          .range  = sizeof(shaderio::DrawUniforms),
      };
      VkWriteDescriptorSet drawWrite{
          .sType           = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET,
          .dstSet          = drawTable->getVkDescriptorSet(),
          .dstBinding      = shaderio::LBindDrawModel,
          .dstArrayElement = 0,
          .descriptorCount = 1,
          .descriptorType  = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER_DYNAMIC,
          .pBufferInfo     = &drawBufferInfo,
      };
      vkUpdateDescriptorSets(fromNativeHandle<VkDevice>(m_device.device->getNativeDevice()),
                             1, &drawWrite, 0, nullptr);

      // Save the layout handle for pipeline layout creation (use first one, they're all identical)
      if(i == 0)
      {
        drawSetLayout = drawLayout->getVkDescriptorSetLayout();
      }
    }

    // Create GBuffer pipeline layout
    const uint64_t textureLayoutHandle = getBindGroupLayoutOpaque(m_materials.materialBindGroup, BindGroupSetSlot::material);
    const uint64_t cameraLayoutHandle = reinterpret_cast<uint64_t>(cameraSetLayout);
    const uint64_t drawLayoutHandle = reinterpret_cast<uint64_t>(drawSetLayout);

    rhi::ShaderReflectionData gbufferReflection{};
    gbufferReflection.name = "shader.gbuffer";
    gbufferReflection.entryPoints = {
        rhi::ShaderEntryPoint{"vertexMain", rhi::ShaderStageFlagBits::vertex},
        rhi::ShaderEntryPoint{"fragmentMain", rhi::ShaderStageFlagBits::fragment},
    };
    gbufferReflection.resourceBindings = {
        rhi::ShaderResourceBinding{"textures", rhi::ShaderResourceType::sampler, rhi::DescriptorType::combinedImageSampler,
                                   rhi::ShaderStageFlagBits::fragment, shaderio::LSetTextures, shaderio::LBindTextures,
                                   Renderer::kDemoMaterialSlotCount, 0},
        rhi::ShaderResourceBinding{"camera", rhi::ShaderResourceType::uniformBuffer, rhi::DescriptorType::uniformBufferDynamic,
                                   rhi::ShaderStageFlagBits::vertex, shaderio::LSetScene, shaderio::LBindCamera, 1, 0},
        rhi::ShaderResourceBinding{"draw", rhi::ShaderResourceType::uniformBuffer, rhi::DescriptorType::uniformBufferDynamic,
                                   rhi::ShaderStageFlagBits::vertex | rhi::ShaderStageFlagBits::fragment,
                                   shaderio::LSetDraw, shaderio::LBindDrawModel, 1, 0},
    };
    gbufferReflection.pushConstantRanges = {};  // No push constants for GBuffer

    rhi::PipelineLayoutDesc gbufferLayoutDesc = rhi::derivePipelineLayoutDesc(gbufferReflection);
    gbufferLayoutDesc.debugName = "gbuffer-layout";

    const std::array<rhi::vulkan::VulkanPipelineLayoutBindingMapping, 3> gbufferLayoutMappings{
        rhi::vulkan::makePipelineLayoutBindingMapping(shaderio::LSetTextures, textureLayoutHandle),
        rhi::vulkan::makePipelineLayoutBindingMapping(shaderio::LSetScene, cameraLayoutHandle),
        rhi::vulkan::makePipelineLayoutBindingMapping(shaderio::LSetDraw, drawLayoutHandle),
    };

    const rhi::vulkan::VulkanPipelineLayoutLowering gbufferLayoutLowering{
        .setLayouts     = gbufferLayoutMappings.data(),
        .setLayoutCount = static_cast<uint32_t>(gbufferLayoutMappings.size()),
    };

    auto gbufferPipelineLayout = std::make_unique<rhi::vulkan::VulkanPipelineLayout>();
    gbufferPipelineLayout->init(static_cast<void*>(fromNativeHandle<VkDevice>(m_device.device->getNativeDevice())),
                                gbufferLayoutDesc, gbufferLayoutLowering);
    DBG_VK_NAME(reinterpret_cast<VkPipelineLayout>(gbufferPipelineLayout->getNativeHandle()));
    m_device.gbufferPipelineLayout = std::move(gbufferPipelineLayout);
  }

  // Create light pipeline for LightPass (fullscreen triangle sampling GBuffer)
#ifdef USE_SLANG
  {
    VkShaderModule lightShaderModule = utils::createShaderModule(fromNativeHandle<VkDevice>(m_device.device->getNativeDevice()),
                                                                {shader_light_slang, std::size(shader_light_slang)});
    DBG_VK_NAME(lightShaderModule);

    // Shader stages
    const std::array<VkPipelineShaderStageCreateInfo, 2> shaderStages{{
        {
            .sType  = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO,
            .stage  = VK_SHADER_STAGE_VERTEX_BIT,
            .module = lightShaderModule,
            .pName  = "vertexMain",
        },
        {
            .sType  = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO,
            .stage  = VK_SHADER_STAGE_FRAGMENT_BIT,
            .module = lightShaderModule,
            .pName  = "fragmentMain",
        },
    }};

    // Vertex input (empty - fullscreen triangle generated in VS)
    const VkPipelineVertexInputStateCreateInfo vertexInput{
        .sType                           = VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO,
        .vertexBindingDescriptionCount   = 0,
        .vertexAttributeDescriptionCount = 0,
    };

    // Input assembly
    const VkPipelineInputAssemblyStateCreateInfo inputAssembly{
        .sType                  = VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO,
        .topology               = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST,
        .primitiveRestartEnable = VK_FALSE,
    };

    // Rasterizer
    const VkPipelineRasterizationStateCreateInfo rasterizer{
        .sType                   = VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO,
        .depthClampEnable        = VK_FALSE,
        .rasterizerDiscardEnable = VK_FALSE,
        .polygonMode             = VK_POLYGON_MODE_FILL,
        .cullMode                = VK_CULL_MODE_NONE,
        .frontFace               = VK_FRONT_FACE_COUNTER_CLOCKWISE,
        .depthBiasEnable         = VK_FALSE,
        .lineWidth               = 1.0f,
    };

    // Multisampling
    const VkPipelineMultisampleStateCreateInfo multisampling{
        .sType                = VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO,
        .rasterizationSamples = VK_SAMPLE_COUNT_1_BIT,
        .sampleShadingEnable  = VK_FALSE,
    };

    // Color blend
    const VkPipelineColorBlendAttachmentState colorBlendAttachment{
        .blendEnable         = VK_FALSE,
        .colorWriteMask      = VK_COLOR_COMPONENT_R_BIT | VK_COLOR_COMPONENT_G_BIT | VK_COLOR_COMPONENT_B_BIT | VK_COLOR_COMPONENT_A_BIT,
    };
    const VkPipelineColorBlendStateCreateInfo colorBlending{
        .sType           = VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO,
        .logicOpEnable   = VK_FALSE,
        .attachmentCount = 1,
        .pAttachments    = &colorBlendAttachment,
    };

    // Dynamic state
    const std::array<VkDynamicState, 2> dynamicStates{VK_DYNAMIC_STATE_VIEWPORT_WITH_COUNT, VK_DYNAMIC_STATE_SCISSOR_WITH_COUNT};
    const VkPipelineDynamicStateCreateInfo dynamicState{
        .sType             = VK_STRUCTURE_TYPE_PIPELINE_DYNAMIC_STATE_CREATE_INFO,
        .dynamicStateCount = static_cast<uint32_t>(dynamicStates.size()),
        .pDynamicStates    = dynamicStates.data(),
    };

    // Depth stencil (disabled for light pass)
    const VkPipelineDepthStencilStateCreateInfo depthStencil{
        .sType                 = VK_STRUCTURE_TYPE_PIPELINE_DEPTH_STENCIL_STATE_CREATE_INFO,
        .depthTestEnable       = VK_FALSE,
        .depthWriteEnable      = VK_FALSE,
        .depthCompareOp        = VK_COMPARE_OP_ALWAYS,
        .depthBoundsTestEnable = VK_FALSE,
        .stencilTestEnable     = VK_FALSE,
    };

    // Viewport state (required even with dynamic viewport/scissor)
    const VkPipelineViewportStateCreateInfo viewportState{
        .sType         = VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO,
        .viewportCount = 0,  // Set dynamically with VK_DYNAMIC_STATE_VIEWPORT_WITH_COUNT
        .scissorCount  = 0,  // Set dynamically with VK_DYNAMIC_STATE_SCISSOR_WITH_COUNT
    };

    // Pipeline rendering info
    const VkFormat colorFormat = m_swapchainDependent.swapchainImageFormat;
    const VkPipelineRenderingCreateInfo renderingInfo{
        .sType                   = VK_STRUCTURE_TYPE_PIPELINE_RENDERING_CREATE_INFO,
        .colorAttachmentCount    = 1,
        .pColorAttachmentFormats = &colorFormat,
    };

    // Graphics pipeline create info
    const VkGraphicsPipelineCreateInfo pipelineInfo{
        .sType               = VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO,
        .pNext               = &renderingInfo,
        .stageCount          = static_cast<uint32_t>(shaderStages.size()),
        .pStages             = shaderStages.data(),
        .pVertexInputState   = &vertexInput,
        .pInputAssemblyState = &inputAssembly,
        .pViewportState      = &viewportState,
        .pRasterizationState = &rasterizer,
        .pMultisampleState   = &multisampling,
        .pDepthStencilState  = &depthStencil,
        .pColorBlendState    = &colorBlending,
        .pDynamicState       = &dynamicState,
        .layout              = m_device.lightPipelineLayout,
        .renderPass          = VK_NULL_HANDLE,
        .subpass             = 0,
    };

    VkPipeline lightPipeline = VK_NULL_HANDLE;
    VK_CHECK(vkCreateGraphicsPipelines(fromNativeHandle<VkDevice>(m_device.device->getNativeDevice()),
                                        VK_NULL_HANDLE, 1, &pipelineInfo, nullptr, &lightPipeline));
    DBG_VK_NAME(lightPipeline);
    m_lightPipeline = registerPipeline(static_cast<uint32_t>(VK_PIPELINE_BIND_POINT_GRAPHICS),
                                       reinterpret_cast<uint64_t>(lightPipeline),
                                       2);  // specialization variant

    vkDestroyShaderModule(fromNativeHandle<VkDevice>(m_device.device->getNativeDevice()), lightShaderModule, nullptr);
  }
#endif

  // Create GBuffer pipelines (Opaque + AlphaTest variants)
  {
    VkShaderModule gbufferShaderModule = utils::createShaderModule(fromNativeHandle<VkDevice>(m_device.device->getNativeDevice()),
                                                                {shader_gbuffer_slang, std::size(shader_gbuffer_slang)});
    DBG_VK_NAME(gbufferShaderModule);

    // GBuffer vertex input: Position(12) + Normal(12) + TexCoord(8) + Tangent(16) = 48 bytes
    const std::array<rhi::VertexBindingDesc, 1> gbufferBindings{{
        {.binding = 0, .stride = 48, .inputRate = rhi::VertexInputRate::perVertex}
    }};

    const std::array<rhi::VertexAttributeDesc, 4> gbufferAttributes{{
        {.location = shaderio::LVGltfPosition, .binding = 0, .format = rhi::VertexFormat::r32g32b32Sfloat, .offset = 0},    // Position
        {.location = shaderio::LVGltfNormal,   .binding = 0, .format = rhi::VertexFormat::r32g32b32Sfloat, .offset = 12},   // Normal
        {.location = shaderio::LVGltfTexCoord, .binding = 0, .format = rhi::VertexFormat::r32g32Sfloat,    .offset = 24},   // TexCoord
        {.location = shaderio::LVGltfTangent,  .binding = 0, .format = rhi::VertexFormat::r32g32b32a32Sfloat, .offset = 32}, // Tangent
    }};

    const rhi::VertexInputLayoutDesc gbufferVertexInput{
        .bindings       = gbufferBindings.data(),
        .bindingCount   = static_cast<uint32_t>(gbufferBindings.size()),
        .attributes     = gbufferAttributes.data(),
        .attributeCount = static_cast<uint32_t>(gbufferAttributes.size()),
    };

    const std::array<rhi::DynamicState, 2> gbufferDynamicStates{{
        rhi::DynamicState::viewport,
        rhi::DynamicState::scissor,
    }};

    // No blending for GBuffer
    const std::array<rhi::BlendAttachmentState, 3> gbufferBlendStates{{
        rhi::BlendAttachmentState{.blendEnable = false, .colorWriteMask = rhi::ColorComponentFlags::all},
        rhi::BlendAttachmentState{.blendEnable = false, .colorWriteMask = rhi::ColorComponentFlags::all},
        rhi::BlendAttachmentState{.blendEnable = false, .colorWriteMask = rhi::ColorComponentFlags::all},
    }};

    // GBuffer formats: 3 color attachments + depth
    const std::array<rhi::TextureFormat, 3> gbufferColorFormats{{
        rhi::TextureFormat::rgba8Unorm,  // GBuffer0: BaseColor
        rhi::TextureFormat::rgba8Unorm,  // GBuffer1: Normal
        rhi::TextureFormat::rgba8Unorm,  // GBuffer2: Material
    }};

    // Specialization constant for alpha test (must be VkBool32 = uint32_t = 4 bytes)
    rhi::SpecializationConstant specConstantAlphaTest(0, 0, sizeof(uint32_t));

    std::array<rhi::PipelineShaderStageDesc, 2> gbufferShaderStages{{
        rhi::PipelineShaderStageDesc{
            .stage = rhi::ShaderStage::vertex,
            .shaderModule = reinterpret_cast<uint64_t>(gbufferShaderModule),
            .entryPoint = "vertexMain",
        },
        rhi::PipelineShaderStageDesc{
            .stage = rhi::ShaderStage::fragment,
            .shaderModule = reinterpret_cast<uint64_t>(gbufferShaderModule),
            .entryPoint = "fragmentMain",
        },
    }};

    rhi::GraphicsPipelineDesc gbufferGraphicsDesc{
        .layout            = m_device.gbufferPipelineLayout.get(),
        .shaderStages      = gbufferShaderStages.data(),
        .shaderStageCount  = static_cast<uint32_t>(gbufferShaderStages.size()),
        .vertexInput       = gbufferVertexInput,
        .rasterState       = rhi::RasterState{},
        .depthState        = rhi::DepthState{true, true, rhi::CompareOp::less},
        .blendStates       = gbufferBlendStates.data(),
        .blendStateCount   = static_cast<uint32_t>(gbufferBlendStates.size()),
        .dynamicStates     = gbufferDynamicStates.data(),
        .dynamicStateCount = static_cast<uint32_t>(gbufferDynamicStates.size()),
        .renderingInfo =
            {
                .colorFormats     = gbufferColorFormats.data(),
                .colorFormatCount = static_cast<uint32_t>(gbufferColorFormats.size()),
                .depthFormat      = toPortableTextureFormat(m_swapchainDependent.sceneResources.getDepthFormat()),
            },
    };
    gbufferGraphicsDesc.rasterState.topology    = rhi::PrimitiveTopology::triangleList;
    gbufferGraphicsDesc.rasterState.cullMode    = rhi::CullMode::back;
    gbufferGraphicsDesc.rasterState.frontFace   = rhi::FrontFace::counterClockwise;
    gbufferGraphicsDesc.rasterState.sampleCount = rhi::SampleCount::count1;

    // Variant 0: Opaque (alphaTestEnabled = false)
    uint32_t alphaTestFalse = VK_FALSE;
    rhi::SpecializationData specDataFalse{&alphaTestFalse, sizeof(uint32_t)};
    gbufferShaderStages[1].specializationData = specDataFalse;
    gbufferShaderStages[1].specializationConstants = &specConstantAlphaTest;
    gbufferShaderStages[1].specializationConstantCount = 1;

    rhi::vulkan::GraphicsPipelineCreateInfo gbufferCreateInfo{
        .key =
            {
                .shaderIdentity        = rhi::vulkan::PipelineShaderIdentity::raster,
                .specializationVariant = 3,  // GBuffer Opaque variant
            },
        .desc = gbufferGraphicsDesc,
    };
    const VkPipeline gbufferOpaquePipeline =
        rhi::vulkan::createGraphicsPipeline(fromNativeHandle<VkDevice>(m_device.device->getNativeDevice()), gbufferCreateInfo);
    DBG_VK_NAME(gbufferOpaquePipeline);
    m_gbufferOpaquePipeline = registerPipeline(static_cast<uint32_t>(VK_PIPELINE_BIND_POINT_GRAPHICS),
                                               reinterpret_cast<uint64_t>(gbufferOpaquePipeline),
                                               gbufferCreateInfo.key.specializationVariant);

    // Variant 1: AlphaTest (alphaTestEnabled = true)
    uint32_t alphaTestTrue = VK_TRUE;
    rhi::SpecializationData specDataTrue{&alphaTestTrue, sizeof(uint32_t)};
    gbufferShaderStages[1].specializationData = specDataTrue;
    gbufferCreateInfo.key.specializationVariant = 4;  // GBuffer AlphaTest variant

    const VkPipeline gbufferAlphaTestPipeline =
        rhi::vulkan::createGraphicsPipeline(fromNativeHandle<VkDevice>(m_device.device->getNativeDevice()), gbufferCreateInfo);
    DBG_VK_NAME(gbufferAlphaTestPipeline);
    m_gbufferAlphaTestPipeline = registerPipeline(static_cast<uint32_t>(VK_PIPELINE_BIND_POINT_GRAPHICS),
                                                   reinterpret_cast<uint64_t>(gbufferAlphaTestPipeline),
                                                   gbufferCreateInfo.key.specializationVariant);

    vkDestroyShaderModule(fromNativeHandle<VkDevice>(m_device.device->getNativeDevice()), gbufferShaderModule, nullptr);
  }

  // Create Forward pipeline for transparent objects
  {
    VkShaderModule forwardShaderModule = utils::createShaderModule(fromNativeHandle<VkDevice>(m_device.device->getNativeDevice()),
                                                                {shader_forward_slang, std::size(shader_forward_slang)});
    DBG_VK_NAME(forwardShaderModule);

    // Same vertex input as GBuffer
    const std::array<rhi::VertexBindingDesc, 1> forwardBindings{{
        {.binding = 0, .stride = 48, .inputRate = rhi::VertexInputRate::perVertex}
    }};

    const std::array<rhi::VertexAttributeDesc, 4> forwardAttributes{{
        {.location = shaderio::LVGltfPosition, .binding = 0, .format = rhi::VertexFormat::r32g32b32Sfloat, .offset = 0},
        {.location = shaderio::LVGltfNormal,   .binding = 0, .format = rhi::VertexFormat::r32g32b32Sfloat, .offset = 12},
        {.location = shaderio::LVGltfTexCoord, .binding = 0, .format = rhi::VertexFormat::r32g32Sfloat,    .offset = 24},
        {.location = shaderio::LVGltfTangent,  .binding = 0, .format = rhi::VertexFormat::r32g32b32a32Sfloat, .offset = 32},
    }};

    const rhi::VertexInputLayoutDesc forwardVertexInput{
        .bindings       = forwardBindings.data(),
        .bindingCount   = static_cast<uint32_t>(forwardBindings.size()),
        .attributes     = forwardAttributes.data(),
        .attributeCount = static_cast<uint32_t>(forwardAttributes.size()),
    };

    const std::array<rhi::DynamicState, 2> forwardDynamicStates{{
        rhi::DynamicState::viewport,
        rhi::DynamicState::scissor,
    }};

    // Alpha blending for transparent objects
    const rhi::BlendAttachmentState forwardBlend{
        .blendEnable = true,
        .srcColorBlendFactor = rhi::BlendFactor::srcAlpha,
        .dstColorBlendFactor = rhi::BlendFactor::oneMinusSrcAlpha,
        .colorBlendOp = rhi::BlendOp::add,
        .srcAlphaBlendFactor = rhi::BlendFactor::one,
        .dstAlphaBlendFactor = rhi::BlendFactor::oneMinusSrcAlpha,
        .alphaBlendOp = rhi::BlendOp::add,
        .colorWriteMask = rhi::ColorComponentFlags::all,
    };

    std::array<rhi::PipelineShaderStageDesc, 2> forwardShaderStages{{
        rhi::PipelineShaderStageDesc{
            .stage = rhi::ShaderStage::vertex,
            .shaderModule = reinterpret_cast<uint64_t>(forwardShaderModule),
            .entryPoint = "vertexMain",
        },
        rhi::PipelineShaderStageDesc{
            .stage = rhi::ShaderStage::fragment,
            .shaderModule = reinterpret_cast<uint64_t>(forwardShaderModule),
            .entryPoint = "fragmentMain",
        },
    }};

    // Render to swapchain format
    const rhi::TextureFormat swapchainFormat = toPortableTextureFormat(m_swapchainDependent.swapchainImageFormat);
    const rhi::TextureFormat depthFormat = toPortableTextureFormat(m_swapchainDependent.sceneResources.getDepthFormat());

    rhi::GraphicsPipelineDesc forwardGraphicsDesc{
        .layout            = m_device.gbufferPipelineLayout.get(),
        .shaderStages      = forwardShaderStages.data(),
        .shaderStageCount  = static_cast<uint32_t>(forwardShaderStages.size()),
        .vertexInput       = forwardVertexInput,
        .rasterState       = rhi::RasterState{},
        .depthState        = rhi::DepthState{true, false, rhi::CompareOp::lessOrEqual},  // Test enabled, write disabled
        .blendStates       = &forwardBlend,
        .blendStateCount   = 1,
        .dynamicStates     = forwardDynamicStates.data(),
        .dynamicStateCount = static_cast<uint32_t>(forwardDynamicStates.size()),
        .renderingInfo =
            {
                .colorFormats     = &swapchainFormat,
                .colorFormatCount = 1,
                .depthFormat      = depthFormat,
            },
    };
    forwardGraphicsDesc.rasterState.topology    = rhi::PrimitiveTopology::triangleList;
    forwardGraphicsDesc.rasterState.cullMode    = rhi::CullMode::back;
    forwardGraphicsDesc.rasterState.frontFace   = rhi::FrontFace::counterClockwise;
    forwardGraphicsDesc.rasterState.sampleCount = rhi::SampleCount::count1;

    rhi::vulkan::GraphicsPipelineCreateInfo forwardCreateInfo{
        .key =
            {
                .shaderIdentity        = rhi::vulkan::PipelineShaderIdentity::raster,
                .specializationVariant = 5,  // Forward variant
            },
        .desc = forwardGraphicsDesc,
    };
    const VkPipeline forwardPipeline =
        rhi::vulkan::createGraphicsPipeline(fromNativeHandle<VkDevice>(m_device.device->getNativeDevice()), forwardCreateInfo);
    DBG_VK_NAME(forwardPipeline);
    m_forwardPipeline = registerPipeline(static_cast<uint32_t>(VK_PIPELINE_BIND_POINT_GRAPHICS),
                                         reinterpret_cast<uint64_t>(forwardPipeline),
                                         forwardCreateInfo.key.specializationVariant);

    vkDestroyShaderModule(fromNativeHandle<VkDevice>(m_device.device->getNativeDevice()), forwardShaderModule, nullptr);
  }
}

void Renderer::initImGui(GLFWwindow* window)
{
  IMGUI_CHECKVERSION();
  ImGui::CreateContext();
  ImGui::StyleColorsDark();

  ImGui_ImplGlfw_InitForVulkan(window, true);
  static VkFormat           imageFormats[] = {m_swapchainDependent.swapchainImageFormat};
  const rhi::QueueInfo      graphicsQueue  = m_device.device->getGraphicsQueue();
  ImGui_ImplVulkan_InitInfo initInfo       = {
      .Instance       = fromNativeHandle<VkInstance>(m_device.device->getNativeInstance()),
      .PhysicalDevice = fromNativeHandle<VkPhysicalDevice>(m_device.device->getNativePhysicalDevice()),
      .Device         = fromNativeHandle<VkDevice>(m_device.device->getNativeDevice()),
      .QueueFamily    = graphicsQueue.familyIndex,
      .Queue          = fromNativeHandle<VkQueue>(graphicsQueue.nativeHandle),
      .DescriptorPool = m_device.uiDescriptorPool,
      .MinImageCount  = 2,
      .ImageCount     = m_swapchainDependent.swapchain->getMaxFramesInFlight(),
      .PipelineInfoMain =
          {
              .PipelineRenderingCreateInfo =
                  {
                      .sType                   = VK_STRUCTURE_TYPE_PIPELINE_RENDERING_CREATE_INFO,
                      .colorAttachmentCount    = 1,
                      .pColorAttachmentFormats = imageFormats,
                  },
          },
      .UseDynamicRendering = true,
  };

  initInfo.PipelineInfoForViewports = initInfo.PipelineInfoMain;

  ImGui_ImplVulkan_Init(&initInfo);

  ImGui::GetIO().ConfigFlags = ImGuiConfigFlags_DockingEnable | ImGuiConfigFlags_ViewportsEnable;
}

void Renderer::createDescriptorPool()
{
  VkPhysicalDeviceProperties2 deviceProperties2{.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PROPERTIES_2};
  vkGetPhysicalDeviceProperties2(fromNativeHandle<VkPhysicalDevice>(m_device.device->getNativePhysicalDevice()), &deviceProperties2);
  const auto& deviceProperties = deviceProperties2.properties;

  {
    m_materials.maxTextures = std::min(m_materials.maxTextures, deviceProperties.limits.maxDescriptorSetSampledImages);
    const uint32_t maxDescriptorSets = 20U;
    const uint32_t dynamicUniformCount =
        std::max(1U, std::min(maxDescriptorSets, deviceProperties.limits.maxDescriptorSetUniformBuffersDynamic));
    const std::array<VkDescriptorPoolSize, 3> poolSizes{{
        {VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, m_materials.maxTextures},
        {VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER_DYNAMIC, dynamicUniformCount},
        {VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER, 5U},  // For LightPass camera uniform buffer
    }};
    const VkDescriptorPoolCreateInfo          poolInfo = {
        .sType         = VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO,
        .flags         = VK_DESCRIPTOR_POOL_CREATE_FREE_DESCRIPTOR_SET_BIT,
        .maxSets       = maxDescriptorSets,
        .poolSizeCount = uint32_t(poolSizes.size()),
        .pPoolSizes    = poolSizes.data(),
    };
    VK_CHECK(vkCreateDescriptorPool(fromNativeHandle<VkDevice>(m_device.device->getNativeDevice()), &poolInfo, nullptr,
                                    &m_device.descriptorPool));
    DBG_VK_NAME(m_device.descriptorPool);
    LOGI("Created application descriptor pool: %u textures, %u dynamic UBOs, %u sets", m_materials.maxTextures,
         dynamicUniformCount, maxDescriptorSets);
  }

  {
    uint32_t uiPoolSize                 = std::min(20U, deviceProperties.limits.maxDescriptorSetSampledImages);
    uint32_t maxDescriptorSets          = std::min(uiPoolSize, deviceProperties.limits.maxDescriptorSetUniformBuffers);
    VkDescriptorPoolSize       poolSize = {VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, uiPoolSize};
    VkDescriptorPoolCreateInfo poolInfo = {
        .sType         = VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO,
        .flags         = VK_DESCRIPTOR_POOL_CREATE_FREE_DESCRIPTOR_SET_BIT,
        .maxSets       = maxDescriptorSets,
        .poolSizeCount = 1,
        .pPoolSizes    = &poolSize,
    };

    VK_CHECK(vkCreateDescriptorPool(fromNativeHandle<VkDevice>(m_device.device->getNativeDevice()), &poolInfo, nullptr,
                                    &m_device.uiDescriptorPool));
    DBG_VK_NAME(m_device.uiDescriptorPool);
    LOGI("Created UI descriptor pool: %u textures, %u sets", uiPoolSize, maxDescriptorSets);
  }
}

void Renderer::createMaterialBindGroup()
{
  // Create material bind group early - needed for pipeline layout creation
  std::vector<rhi::BindTableLayoutEntry> layoutEntries{rhi::BindTableLayoutEntry{
      .logicalIndex    = kMaterialBindlessTexturesIndex,
      .resourceType    = rhi::BindlessResourceType::sampledTexture,
      .descriptorCount = m_materials.maxTextures,
      .visibility      = rhi::ResourceVisibility::allGraphics,
  }};

  auto* materialLayout = new rhi::vulkan::VulkanBindTableLayout();
  materialLayout->init(static_cast<void*>(fromNativeHandle<VkDevice>(m_device.device->getNativeDevice())), layoutEntries);

  auto* materialTable = new rhi::vulkan::VulkanBindTable();
  materialTable->init(static_cast<void*>(fromNativeHandle<VkDevice>(m_device.device->getNativeDevice())),
                      *materialLayout, m_materials.maxTextures);

  BindGroupDesc materialBindGroupDesc{
      .slot                = BindGroupSetSlot::material,
      .layout              = materialLayout,
      .table               = materialTable,
      .primaryLogicalIndex = kMaterialBindlessTexturesIndex,
      .debugName           = "material-texture-bind-group",
  };
  m_materials.materialBindGroup = createBindGroup(std::move(materialBindGroupDesc));
}

void Renderer::createGraphicDescriptorSet()
{
  // Create per-frame scene bind groups - must be called after createFrameSubmission()
  {
    std::vector<rhi::BindTableLayoutEntry> layoutEntries{rhi::BindTableLayoutEntry{
        .logicalIndex    = kSceneBindlessInfoIndex,
        .resourceType    = rhi::BindlessResourceType::uniformBuffer,
        .descriptorCount = 1,
        .visibility      = rhi::ResourceVisibility::allGraphics,
    }};

    for(uint32_t i = 0; i < m_perFrame.frameUserData.size(); ++i)
    {
      auto* sceneLayout = new rhi::vulkan::VulkanBindTableLayout();
      sceneLayout->init(static_cast<void*>(fromNativeHandle<VkDevice>(m_device.device->getNativeDevice())), layoutEntries);

      auto* sceneTable = new rhi::vulkan::VulkanBindTable();
      sceneTable->init(static_cast<void*>(fromNativeHandle<VkDevice>(m_device.device->getNativeDevice())), *sceneLayout, 1);

      BindGroupDesc sceneBindGroupDesc{
          .slot                = BindGroupSetSlot::drawDynamic,
          .layout              = sceneLayout,
          .table               = sceneTable,
          .primaryLogicalIndex = kSceneBindlessInfoIndex,
          .debugName           = "scene-dynamic-bind-group",
      };
      m_perFrame.frameUserData[i].sceneBindGroup = createBindGroup(std::move(sceneBindGroupDesc));
    }
  }
}

void Renderer::updateGraphicsDescriptorSet()
{
  const VkSampler sampler = m_device.samplerPool.acquireSampler({
      .sType        = VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO,
      .magFilter    = VK_FILTER_LINEAR,
      .minFilter    = VK_FILTER_LINEAR,
      .mipmapMode   = VK_SAMPLER_MIPMAP_MODE_LINEAR,
      .addressModeU = VK_SAMPLER_ADDRESS_MODE_REPEAT,
      .addressModeV = VK_SAMPLER_ADDRESS_MODE_REPEAT,
      .addressModeW = VK_SAMPLER_ADDRESS_MODE_REPEAT,
      .maxLod       = VK_LOD_CLAMP_NONE,
  });
  DBG_VK_NAME(sampler);

  std::array<VkDescriptorImageInfo, kDemoMaterialSlotCount> imageInfos{};
  for(uint32_t slot = 0; slot < kDemoMaterialSlotCount; ++slot)
  {
    const MaterialHandle                     materialHandle = m_materials.sampleMaterials[slot];
    const MaterialResources::MaterialRecord* material       = tryGetMaterial(materialHandle);
    ASSERT(material != nullptr, "Sample material handle must resolve to an active pool entry");
    ASSERT(material->descriptorIndex < kDemoMaterialSlotCount,
           "Sample material descriptor index must stay in [0, kDemoMaterialSlotCount)");

    const MaterialResources::TextureHotData* textureHot = tryGetTextureHot(material->sampledTexture);
    ASSERT(textureHot != nullptr, "Material texture handle must resolve to an active pool entry");
    ASSERT(textureHot->runtimeKind == MaterialResources::TextureRuntimeKind::materialSampled,
           "Material descriptor writes require material-sampled textures");

    imageInfos[material->descriptorIndex] = VkDescriptorImageInfo{
        .sampler     = sampler,
        .imageView   = textureHot->sampledImageView,
        .imageLayout = textureHot->sampledImageLayout,
    };
  }

  std::vector<rhi::DescriptorImageInfo> descriptorImageInfos(imageInfos.size());
  for(size_t i = 0; i < imageInfos.size(); ++i)
  {
    descriptorImageInfos[i] = rhi::DescriptorImageInfo{
        .sampler     = reinterpret_cast<uint64_t>(imageInfos[i].sampler),
        .imageView   = reinterpret_cast<uint64_t>(imageInfos[i].imageView),
        .imageLayout = static_cast<uint32_t>(imageInfos[i].imageLayout),
    };
  }

  const rhi::BindTableWrite materialWrite{
      .dstIndex        = kMaterialBindlessTexturesIndex,
      .dstArrayElement = 0,
      .resourceType    = rhi::BindlessResourceType::sampledTexture,
      .descriptorCount = static_cast<uint32_t>(descriptorImageInfos.size()),
      .pImageInfo      = descriptorImageInfos.data(),
      .visibility      = rhi::ResourceVisibility::allGraphics,
      .updateFlags     = rhi::BindlessUpdateFlags::immediateVisibility,
  };
  updateBindGroup(m_materials.materialBindGroup, &materialWrite, 1);

  for(auto& frameUserData : m_perFrame.frameUserData)
  {
    const VkDescriptorBufferInfo sceneBufferInfo = {
        .buffer = reinterpret_cast<VkBuffer>(frameUserData.transientAllocator.getBufferOpaque()),
        .offset = 0,
        .range  = sizeof(shaderio::SceneInfo),
    };
    const rhi::DescriptorBufferInfo sceneBufferWrite{
        .buffer = reinterpret_cast<uint64_t>(sceneBufferInfo.buffer),
        .offset = sceneBufferInfo.offset,
        .range  = sceneBufferInfo.range,
    };
    const rhi::BindTableWrite sceneWrite{
        .dstIndex        = kSceneBindlessInfoIndex,
        .dstArrayElement = 0,
        .resourceType    = rhi::BindlessResourceType::uniformBuffer,
        .descriptorCount = 1,
        .pBufferInfo     = &sceneBufferWrite,
        .visibility      = rhi::ResourceVisibility::allGraphics,
        .updateFlags     = rhi::BindlessUpdateFlags::immediateVisibility,
    };
    updateBindGroup(frameUserData.sceneBindGroup, &sceneWrite, 1);
  }
}

rhi::BindGroupLayoutHandle Renderer::createBindGroupLayout(const rhi::BindGroupLayoutDesc& desc)
{
  // Convert BindGroupLayoutDesc to BindTableLayoutEntry format
  std::vector<rhi::BindTableLayoutEntry> tableEntries;
  tableEntries.reserve(desc.entryCount);
  for(uint32_t i = 0; i < desc.entryCount; ++i)
  {
    tableEntries.push_back(rhi::BindTableLayoutEntry{
        .logicalIndex    = desc.entries[i].binding,
        .resourceType    = desc.entries[i].type,
        .descriptorCount = desc.entries[i].count,
        .visibility      = static_cast<rhi::ResourceVisibility>(desc.entries[i].visibility),
    });
  }

  auto layout = std::make_unique<rhi::vulkan::VulkanBindTableLayout>();
  layout->init(reinterpret_cast<void*>(static_cast<uintptr_t>(m_device.device->getNativeDevice())), tableEntries);
  return m_materials.bindGroupLayoutPool.emplace(std::move(layout));
}

rhi::BindGroupHandle Renderer::createBindGroup(const rhi::BindGroupDesc& desc)
{
  // TODO: Implement BindGroup creation using new RHI interface
  // This will be implemented in a later task
  ASSERT(false, "createBindGroup(rhi::BindGroupDesc) not yet implemented");
  return rhi::BindGroupHandle{};
}

void Renderer::destroyBindGroupLayout(rhi::BindGroupLayoutHandle handle)
{
  // TODO: Implement BindGroupLayout destruction
  // This will be implemented in a later task
}

void Renderer::destroyBindGroup(rhi::BindGroupHandle handle)
{
  BindGroupResource* bindGroup = m_materials.bindGroupPool.tryGet(handle);
  if(bindGroup == nullptr)
  {
    return;
  }

  delete bindGroup->desc.table;
  bindGroup->desc.table = nullptr;
  delete bindGroup->desc.layout;
  bindGroup->desc.layout = nullptr;

  m_materials.bindGroupPool.destroy(handle);
}

BindGroupHandle Renderer::createBindGroup(BindGroupDesc desc)
{
  ASSERT(isStableBindGroupSetSlot(desc.slot), "BindGroupDesc slot must be one of stable set slots 0..3");
  ASSERT(desc.layout != nullptr, "BindGroupDesc layout must be valid");
  ASSERT(desc.table != nullptr, "BindGroupDesc table must be valid");
  ASSERT(rhi::isValidResourceIndex(desc.primaryLogicalIndex), "BindGroupDesc primaryLogicalIndex must be valid");

  return m_materials.bindGroupPool.emplace(BindGroupResource{.desc = std::move(desc)});
}

void Renderer::updateBindGroup(BindGroupHandle handle, const rhi::BindTableWrite* writes, uint32_t writeCount) const
{
  if(writeCount == 0 || writes == nullptr)
  {
    return;
  }

  const BindGroupResource* bindGroup = tryGetBindGroup(handle);
  ASSERT(bindGroup != nullptr, "BindGroupHandle must resolve before update");
  bindGroup->desc.table->update(writeCount, writes);
}

void Renderer::destroyBindGroups()
{
  std::vector<BindGroupHandle> bindGroups;
  m_materials.bindGroupPool.forEachActive(
      [&](BindGroupHandle handle, const BindGroupResource&) { bindGroups.push_back(handle); });
  for(BindGroupHandle handle : bindGroups)
  {
    destroyBindGroup(handle);
  }
  m_materials.materialBindGroup = kNullBindGroupHandle;
  for(auto& frameUserData : m_perFrame.frameUserData)
  {
    frameUserData.sceneBindGroup = kNullBindGroupHandle;
    frameUserData.cameraBindGroup = kNullBindGroupHandle;
    frameUserData.drawBindGroup = kNullBindGroupHandle;  
  }
}

const BindGroupResource* Renderer::tryGetBindGroup(BindGroupHandle handle) const
{
  return m_materials.bindGroupPool.tryGet(handle);
}

uint64_t Renderer::getBindGroupLayoutOpaque(BindGroupHandle handle, BindGroupSetSlot expectedSlot) const
{
  const BindGroupResource* bindGroup = tryGetBindGroup(handle);
  ASSERT(bindGroup != nullptr, "BindGroupHandle must resolve to an active bind-group");
  ASSERT(bindGroup->desc.slot == expectedSlot, "BindGroup slot mismatch for requested layout");
  return bindGroup->desc.layout->getNativeHandle();
}

uint64_t Renderer::getBindGroupDescriptorSetOpaque(BindGroupHandle handle, BindGroupSetSlot expectedSlot) const
{
  const BindGroupResource* bindGroup = tryGetBindGroup(handle);
  ASSERT(bindGroup != nullptr, "BindGroupHandle must resolve to an active bind-group");
  ASSERT(bindGroup->desc.slot == expectedSlot, "BindGroup slot mismatch for requested bind table");
  return bindGroup->desc.table->getNativeHandle();
}

rhi::ResourceIndex Renderer::getBindGroupPrimaryLogicalIndex(BindGroupHandle handle, BindGroupSetSlot expectedSlot) const
{
  const BindGroupResource* bindGroup = tryGetBindGroup(handle);
  ASSERT(bindGroup != nullptr, "BindGroupHandle must resolve to an active bind-group");
  ASSERT(bindGroup->desc.slot == expectedSlot, "BindGroup slot mismatch for requested primary logical index");
  return bindGroup->desc.primaryLogicalIndex;
}

utils::ImageResource Renderer::loadAndCreateImage(rhi::CommandList& cmd, const std::string& filename)
{
  int            w = 0, h = 0, comp = 0, req_comp{4};
  const stbi_uc* data = stbi_load(filename.c_str(), &w, &h, &comp, req_comp);
  ASSERT(data != nullptr, "Could not load texture image!");
  const VkFormat format = VK_FORMAT_R8G8B8A8_UNORM;

  const VkImageCreateInfo imageInfo = {
      .sType       = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO,
      .imageType   = VK_IMAGE_TYPE_2D,
      .format      = format,
      .extent      = {uint32_t(w), uint32_t(h), 1},
      .mipLevels   = 1,
      .arrayLayers = 1,
      .samples     = VK_SAMPLE_COUNT_1_BIT,
      .usage       = VK_IMAGE_USAGE_SAMPLED_BIT,
  };

  const std::span<const std::byte> dataSpan(reinterpret_cast<const std::byte*>(data), static_cast<size_t>(w * h * 4));
  utils::ImageResource             image =
      createImageAndUploadData(fromNativeHandle<VkDevice>(m_device.device->getNativeDevice()), m_device.allocator,
                               m_device.stagingBuffers, rhi::vulkan::getNativeCommandBuffer(cmd), dataSpan, imageInfo,
                               VK_IMAGE_LAYOUT_GENERAL);
  DBG_VK_NAME(image.image);
  image.extent = {uint32_t(w), uint32_t(h)};

  const VkImageViewCreateInfo viewInfo = {
      .sType            = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO,
      .image            = image.image,
      .viewType         = VK_IMAGE_VIEW_TYPE_2D,
      .format           = format,
      .subresourceRange = {.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT, .levelCount = 1, .layerCount = 1},
  };
  VK_CHECK(vkCreateImageView(fromNativeHandle<VkDevice>(m_device.device->getNativeDevice()), &viewInfo, nullptr, &image.view));
  DBG_VK_NAME(image.view);
  stbi_image_free(const_cast<stbi_uc*>(data));

  return image;
}

void Renderer::createPrebuiltComputePipelineVariant()
{
  const rhi::ShaderReflectionData computeReflection = buildComputeShaderReflection();
  rhi::PipelineLayoutDesc         layoutDesc        = rhi::derivePipelineLayoutDesc(computeReflection);
  layoutDesc.debugName                              = "compute-layout";

  auto computePipelineLayout = std::make_unique<rhi::vulkan::VulkanPipelineLayout>();
  computePipelineLayout->init(static_cast<void*>(fromNativeHandle<VkDevice>(m_device.device->getNativeDevice())), layoutDesc);
  DBG_VK_NAME(reinterpret_cast<VkPipelineLayout>(computePipelineLayout->getNativeHandle()));
  m_device.computePipelineLayout = std::move(computePipelineLayout);

#ifdef USE_SLANG
  VkShaderModule compute = utils::createShaderModule(fromNativeHandle<VkDevice>(m_device.device->getNativeDevice()),
                                                     {shader_comp_slang, std::size(shader_comp_slang)});
#else
  VkShaderModule compute = utils::createShaderModule(fromNativeHandle<VkDevice>(m_device.device->getNativeDevice()),
                                                     {shader_comp_glsl, std::size(shader_comp_glsl)});
#endif
  DBG_VK_NAME(compute);

  const rhi::ComputePipelineDesc computeDesc{
      .layout = m_device.computePipelineLayout.get(),
      .shaderStage =
          {
              .stage        = rhi::ShaderStage::compute,
              .shaderModule = reinterpret_cast<uint64_t>(compute),
              .entryPoint   = "main",
          },
  };
  const rhi::vulkan::ComputePipelineCreateInfo computeCreateInfo{
      .key =
          {
              .shaderIdentity        = rhi::vulkan::PipelineShaderIdentity::compute,
              .specializationVariant = 0,
          },
      .desc          = computeDesc,
      .pipelineFlags = 0,
  };
  const VkPipeline computePipeline =
      rhi::vulkan::createComputePipeline(fromNativeHandle<VkDevice>(m_device.device->getNativeDevice()), computeCreateInfo);
  DBG_VK_NAME(computePipeline);
  m_device.prebuiltPipelines.compute =
      registerPipeline(static_cast<uint32_t>(VK_PIPELINE_BIND_POINT_COMPUTE),
                       reinterpret_cast<uint64_t>(computePipeline), computeCreateInfo.key.specializationVariant);

  vkDestroyShaderModule(fromNativeHandle<VkDevice>(m_device.device->getNativeDevice()), compute, nullptr);
}

PipelineHandle Renderer::registerPipeline(uint32_t bindPoint, uint64_t nativePipeline, uint32_t specializationVariant)
{
  ASSERT(nativePipeline != 0, "Pipeline registry entries require a valid native pipeline");
  return m_device.pipelineRegistry.emplace(DeviceLifetimeResources::PipelineRecord{
      .bindPoint             = bindPoint,
      .nativePipeline        = nativePipeline,
      .specializationVariant = specializationVariant,
  });
}

void Renderer::destroyPipelines()
{
  std::vector<PipelineHandle> handles;
  m_device.pipelineRegistry.forEachActive(
      [&](PipelineHandle handle, const DeviceLifetimeResources::PipelineRecord&) { handles.push_back(handle); });

  VkDevice device = fromNativeHandle<VkDevice>(m_device.device->getNativeDevice());
  for(const PipelineHandle handle : handles)
  {
    DeviceLifetimeResources::PipelineRecord* record = m_device.pipelineRegistry.tryGet(handle);
    if(record != nullptr && record->nativePipeline != 0)
    {
      vkDestroyPipeline(device, reinterpret_cast<VkPipeline>(record->nativePipeline), nullptr);
      record->nativePipeline = 0;
    }
    m_device.pipelineRegistry.destroy(handle);
  }

  m_device.prebuiltPipelines.compute             = kNullPipelineHandle;
  m_device.prebuiltPipelines.graphicsTextured    = kNullPipelineHandle;
  m_device.prebuiltPipelines.graphicsNonTextured = kNullPipelineHandle;
}

PipelineHandle Renderer::selectComputePipelineHandle() const
{
  return m_device.prebuiltPipelines.compute;
}

PipelineHandle Renderer::selectGraphicsPipelineHandle(GraphicsPipelineVariant variant) const
{
  switch(variant)
  {
    case GraphicsPipelineVariant::textured:
      return m_device.prebuiltPipelines.graphicsTextured;
    case GraphicsPipelineVariant::nonTextured:
      return m_device.prebuiltPipelines.graphicsNonTextured;
    default:
      ASSERT(false, "Unsupported graphics pipeline variant");
      return kNullPipelineHandle;
  }
}

const Renderer::DeviceLifetimeResources::PipelineRecord* Renderer::tryGetPipelineRecord(PipelineHandle handle) const
{
  return m_device.pipelineRegistry.tryGet(handle);
}

uint64_t Renderer::getPipelineOpaque(PipelineHandle handle, uint32_t expectedBindPoint) const
{
  const DeviceLifetimeResources::PipelineRecord* record = tryGetPipelineRecord(handle);
  ASSERT(record != nullptr, "PipelineHandle must resolve to an active pipeline record");
  ASSERT(record->bindPoint == expectedBindPoint, "PipelineHandle bind-point mismatch");
  ASSERT(record->nativePipeline != 0, "Pipeline record must own a valid native pipeline");
  return record->nativePipeline;
}

const Renderer::MaterialResources::TextureHotData* Renderer::tryGetTextureHot(TextureHandle handle) const
{
  const MaterialResources::TextureRecord* textureRecord = m_materials.texturePool.tryGet(handle);
  return textureRecord != nullptr ? &textureRecord->hot : nullptr;
}

const Renderer::MaterialResources::MaterialRecord* Renderer::tryGetMaterial(MaterialHandle handle) const
{
  return m_materials.materialPool.tryGet(handle);
}

const Renderer::MaterialResources::TextureColdData* Renderer::tryGetTextureCold(TextureHandle handle) const
{
  const MaterialResources::TextureRecord* textureRecord = m_materials.texturePool.tryGet(handle);
  return textureRecord != nullptr ? &textureRecord->cold : nullptr;
}

void Renderer::waitForIdle()
{
  m_device.device->waitIdle();
}

void Renderer::executeUploadCommand(std::function<void(VkCommandBuffer)> uploadFn)
{
  const VkDevice       device       = fromNativeHandle<VkDevice>(m_device.device->getNativeDevice());
  const rhi::QueueInfo graphicsInfo = m_device.device->getGraphicsQueue();
  const VkQueue        graphicsQueue = fromNativeHandle<VkQueue>(graphicsInfo.nativeHandle);

  VkCommandBuffer cmd = utils::beginSingleTimeCommands(device, m_device.transientCmdPool);
  uploadFn(cmd);
  utils::endSingleTimeCommands(cmd, device, m_device.transientCmdPool, graphicsQueue);

  // Free staging buffers after GPU sync (upload is complete)
  freeStagingBuffers(m_device.allocator, m_device.stagingBuffers);
  m_meshPool.freeStagingBuffers();
}

GltfUploadResult Renderer::uploadGltfModel(const GltfModel& model, VkCommandBuffer cmd)
{
  GltfUploadResult result;
  const VkDevice   device = fromNativeHandle<VkDevice>(m_device.device->getNativeDevice());

  // Upload textures
  for(const auto& imageData : model.images)
  {
    if(imageData.pixels.empty() || imageData.width <= 0 || imageData.height <= 0)
    {
      continue;
    }

    const VkFormat format = (imageData.channels == 4) ? VK_FORMAT_R8G8B8A8_UNORM : VK_FORMAT_R8G8B8_UNORM;
    const VkImageCreateInfo imageInfo = {
        .sType         = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO,
        .imageType     = VK_IMAGE_TYPE_2D,
        .format        = format,
        .extent        = {static_cast<uint32_t>(imageData.width), static_cast<uint32_t>(imageData.height), 1},
        .mipLevels     = 1,
        .arrayLayers   = 1,
        .samples       = VK_SAMPLE_COUNT_1_BIT,
        .usage         = VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT,
    };

    utils::Image image = createImage(m_device.allocator, imageInfo);
    utils::cmdInitImageLayout(cmd, image.image);

    const VkBufferImageCopy copyRegion = {
        .imageSubresource = {.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT, .layerCount = 1},
        .imageExtent      = imageInfo.extent,
    };

    const size_t       pixelSize     = imageData.pixels.size();
    utils::Buffer      stagingBuffer = createBuffer(device, m_device.allocator, pixelSize, VK_BUFFER_USAGE_2_TRANSFER_SRC_BIT_KHR,
                                               VMA_MEMORY_USAGE_CPU_TO_GPU, VMA_ALLOCATION_CREATE_HOST_ACCESS_SEQUENTIAL_WRITE_BIT);

    void* mapped = nullptr;
    VK_CHECK(vmaMapMemory(m_device.allocator, stagingBuffer.allocation, &mapped));
    std::memcpy(mapped, imageData.pixels.data(), pixelSize);
    vmaUnmapMemory(m_device.allocator, stagingBuffer.allocation);

    vkCmdCopyBufferToImage(cmd, stagingBuffer.buffer, image.image, VK_IMAGE_LAYOUT_GENERAL, 1, &copyRegion);

    const VkImageViewCreateInfo viewInfo = {
        .sType            = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO,
        .image            = image.image,
        .viewType         = VK_IMAGE_VIEW_TYPE_2D,
        .format           = format,
        .subresourceRange = {.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT, .levelCount = 1, .layerCount = 1},
    };

    VkImageView imageView = VK_NULL_HANDLE;
    VK_CHECK(vkCreateImageView(device, &viewInfo, nullptr, &imageView));

    // Create ImageResource
    utils::ImageResource imageResource{};
    imageResource.image      = image.image;
    imageResource.allocation = image.allocation;
    imageResource.view       = imageView;
    imageResource.layout     = VK_IMAGE_LAYOUT_GENERAL;
    imageResource.extent     = {imageInfo.extent.width, imageInfo.extent.height};

    TextureHandle texHandle = m_materials.texturePool.emplace(MaterialResources::TextureRecord{
        .hot =
            {
                .runtimeKind        = MaterialResources::TextureRuntimeKind::materialSampled,
                .sampledImageView   = imageView,
                .sampledImageLayout = VK_IMAGE_LAYOUT_GENERAL,
            },
        .cold =
            {
                .ownedImage   = imageResource,
                .sourceExtent = {imageInfo.extent.width, imageInfo.extent.height},
            },
    });

    result.textures.push_back(texHandle);
    m_device.stagingBuffers.push_back(stagingBuffer);
  }

  // Create materials with PBR properties
  for(const auto& matData : model.materials)
  {
    MaterialResources::MaterialRecord record;

    // Base color texture
    if(matData.baseColorTexture >= 0 && matData.baseColorTexture < static_cast<int>(result.textures.size()))
    {
      record.baseColorTexture = result.textures[matData.baseColorTexture];
      record.sampledTexture = record.baseColorTexture;  // Legacy compatibility
    }

    // Metallic-Roughness texture
    if(matData.metallicRoughnessTexture >= 0 && matData.metallicRoughnessTexture < static_cast<int>(result.textures.size()))
    {
      record.metallicRoughnessTexture = result.textures[matData.metallicRoughnessTexture];
    }

    // Normal texture
    if(matData.normalTexture >= 0 && matData.normalTexture < static_cast<int>(result.textures.size()))
    {
      record.normalTexture = result.textures[matData.normalTexture];
    }

    // Occlusion texture
    if(matData.occlusionTexture >= 0 && matData.occlusionTexture < static_cast<int>(result.textures.size()))
    {
      record.occlusionTexture = result.textures[matData.occlusionTexture];
    }

    // Emissive texture
    if(matData.emissiveTexture >= 0 && matData.emissiveTexture < static_cast<int>(result.textures.size()))
    {
      record.emissiveTexture = result.textures[matData.emissiveTexture];
    }

    // Factors
    record.baseColorFactor = matData.baseColorFactor;
    record.metallicFactor = matData.metallicFactor;
    record.roughnessFactor = matData.roughnessFactor;
    record.normalScale = matData.normalScale;
    record.occlusionStrength = matData.occlusionStrength;
    record.emissiveFactor = matData.emissiveFactor;

    // Alpha properties
    record.alphaMode = matData.alphaMode;
    record.alphaCutoff = matData.alphaCutoff;

    record.descriptorIndex = static_cast<rhi::ResourceIndex>(result.materials.size());
    record.debugName = matData.name.empty() ? "gltf-material" : matData.name.c_str();

    MaterialHandle matHandle = m_materials.materialPool.emplace(std::move(record));
    result.materials.push_back(matHandle);
  }

  // Upload meshes
  for(const auto& meshData : model.meshes)
  {
    MeshHandle meshHandle = m_meshPool.uploadMesh(meshData, cmd);
    result.meshes.push_back(meshHandle);
  }

  // Update bindless texture array with glTF textures
  // Use index offset to avoid conflict with sample materials
  const uint32_t gltfTextureBaseIndex = kDemoMaterialSlotCount;  // Start after predefined slots
  for(size_t i = 0; i < result.textures.size(); ++i)
  {
    const uint32_t bindlessIndex = gltfTextureBaseIndex + static_cast<uint32_t>(i);
    updateBindlessTexture(bindlessIndex, result.textures[i]);
  }

  return result;
}

void Renderer::destroyGltfResources(const GltfUploadResult& result)
{
  VkDevice device = fromNativeHandle<VkDevice>(m_device.device->getNativeDevice());

  // Destroy meshes
  for(MeshHandle handle : result.meshes)
  {
    m_meshPool.destroyMesh(handle);
  }

  // Destroy materials
  for(MaterialHandle handle : result.materials)
  {
    m_materials.materialPool.destroy(handle);
  }

  // Destroy textures
  for(TextureHandle handle : result.textures)
  {
    const MaterialResources::TextureColdData* cold = tryGetTextureCold(handle);
    if(cold && cold->ownedImage.image != VK_NULL_HANDLE)
    {
      utils::ImageResource image = cold->ownedImage;
      destroyImageResource(device, m_device.allocator, image);
    }
    m_materials.texturePool.destroy(handle);
  }
}

uint64_t Renderer::getLightPipelineLayout() const
{
  return reinterpret_cast<uint64_t>(m_device.lightPipelineLayout);
}

uint64_t Renderer::getGraphicsPipelineLayout() const
{
  return m_device.graphicPipelineLayout ? m_device.graphicPipelineLayout->getNativeHandle() : 0;
}

uint64_t Renderer::getGBufferPipelineLayout() const
{
  return m_device.gbufferPipelineLayout ? m_device.gbufferPipelineLayout->getNativeHandle() : 0;
}

uint64_t Renderer::getGBufferColorDescriptorSet() const
{
  return getBindGroupDescriptorSetOpaque(m_materials.materialBindGroup, BindGroupSetSlot::material);
}

uint64_t Renderer::getGBufferTextureDescriptorSet() const
{
  return reinterpret_cast<uint64_t>(m_device.gbufferTextureSet);
}

BindGroupHandle Renderer::getCameraBindGroup(uint32_t frameIndex) const
{
  if(frameIndex < m_perFrame.frameUserData.size())
  {
    return m_perFrame.frameUserData[frameIndex].cameraBindGroup;
  }
  return kNullBindGroupHandle;
}

BindGroupHandle Renderer::getDrawBindGroup(uint32_t frameIndex) const
{
  if(frameIndex < m_perFrame.frameUserData.size())
  {
    return m_perFrame.frameUserData[frameIndex].drawBindGroup;
  }
  return kNullBindGroupHandle;
}

rhi::BindGroupHandle Renderer::getGlobalBindlessGroup() const
{
  return m_materials.materialBindGroup;
}

glm::vec4 Renderer::getMaterialBaseColorFactor(MaterialHandle handle) const
{
  const MaterialResources::MaterialRecord* material = tryGetMaterial(handle);
  if(material)
  {
    return material->baseColorFactor;
  }
  return glm::vec4(1.0f);
}

int32_t Renderer::getMaterialBaseColorTextureIndex(MaterialHandle materialHandle, const GltfUploadResult* gltfModel) const
{
  if(!gltfModel)
  {
    return -1;
  }

  const MaterialResources::MaterialRecord* material = tryGetMaterial(materialHandle);
  if(!material || material->baseColorTexture.isNull())
  {
    return -1;
  }

  // Find texture index in gltfModel->textures
  const uint32_t gltfTextureBaseIndex = getGltfTextureBaseIndex();
  for(size_t i = 0; i < gltfModel->textures.size(); ++i)
  {
    if(gltfModel->textures[i] == material->baseColorTexture)
    {
      return static_cast<int32_t>(gltfTextureBaseIndex + i);
    }
  }

  return -1;
}

Renderer::MaterialTextureIndices Renderer::getMaterialTextureIndices(MaterialHandle materialHandle, const GltfUploadResult* gltfModel) const
{
  MaterialTextureIndices result;

  if(!gltfModel)
  {
    return result;
  }

  const MaterialResources::MaterialRecord* material = tryGetMaterial(materialHandle);
  if(!material)
  {
    return result;
  }

  // Fill PBR factors
  result.metallicFactor = material->metallicFactor;
  result.roughnessFactor = material->roughnessFactor;
  result.normalScale = material->normalScale;

  const uint32_t gltfTextureBaseIndex = getGltfTextureBaseIndex();

  // Helper to find texture index
  auto findTextureIndex = [&](TextureHandle handle) -> int32_t {
    if(handle.isNull())
    {
      return -1;
    }
    for(size_t i = 0; i < gltfModel->textures.size(); ++i)
    {
      if(gltfModel->textures[i] == handle)
      {
        return static_cast<int32_t>(gltfTextureBaseIndex + i);
      }
    }
    return -1;
  };

  result.baseColor = findTextureIndex(material->baseColorTexture);
  result.normal = findTextureIndex(material->normalTexture);
  result.metallicRoughness = findTextureIndex(material->metallicRoughnessTexture);
  result.occlusion = findTextureIndex(material->occlusionTexture);

  // Fill alpha properties
  result.alphaMode = material->alphaMode;
  result.alphaCutoff = material->alphaCutoff;

  return result;
}

void Renderer::updateBindlessTexture(uint32_t index, TextureHandle textureHandle)
{
  const MaterialResources::TextureHotData* textureHot = tryGetTextureHot(textureHandle);
  if(!textureHot || textureHot->runtimeKind != MaterialResources::TextureRuntimeKind::materialSampled)
  {
    return;
  }

  const VkSampler sampler = m_device.samplerPool.acquireSampler({
      .sType        = VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO,
      .magFilter    = VK_FILTER_LINEAR,
      .minFilter    = VK_FILTER_LINEAR,
      .mipmapMode   = VK_SAMPLER_MIPMAP_MODE_LINEAR,
      .addressModeU = VK_SAMPLER_ADDRESS_MODE_REPEAT,
      .addressModeV = VK_SAMPLER_ADDRESS_MODE_REPEAT,
      .addressModeW = VK_SAMPLER_ADDRESS_MODE_REPEAT,
      .maxLod       = VK_LOD_CLAMP_NONE,
  });

  const VkDescriptorImageInfo imageInfo{
      .sampler     = sampler,
      .imageView   = textureHot->sampledImageView,
      .imageLayout = textureHot->sampledImageLayout,
  };

  const rhi::DescriptorImageInfo descriptorImageInfo{
      .sampler     = reinterpret_cast<uint64_t>(imageInfo.sampler),
      .imageView   = reinterpret_cast<uint64_t>(imageInfo.imageView),
      .imageLayout = static_cast<uint32_t>(imageInfo.imageLayout),
  };

  const rhi::BindTableWrite textureWrite{
      .dstIndex        = kMaterialBindlessTexturesIndex,
      .dstArrayElement = index,
      .resourceType    = rhi::BindlessResourceType::sampledTexture,
      .descriptorCount = 1,
      .pImageInfo      = &descriptorImageInfo,
      .visibility      = rhi::ResourceVisibility::allGraphics,
      .updateFlags     = rhi::BindlessUpdateFlags::immediateVisibility,
  };
  updateBindGroup(m_materials.materialBindGroup, &textureWrite, 1);
}

}  // namespace demo
