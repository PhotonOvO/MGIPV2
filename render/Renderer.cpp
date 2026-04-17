#include "Renderer.h"
#include "../rhi/vulkan/VulkanPipelines.h"
#include "../rhi/vulkan/VulkanDescriptor.h"
#include "../rhi/vulkan/VulkanSwapchain.h"
#include "../rhi/vulkan/VulkanDevice.h"
#include "../rhi/vulkan/VulkanFrameContext.h"
#include "FrameSubmission.h"
#include "ClipSpaceConvention.h"
#include "ImguiAxis.h"
#include "CSMShadowResources.h"

#include <cstdio>
#include <cstring>
#include <random>
#include <type_traits>

#include "../rhi/vulkan/VulkanCommandList.h"

namespace demo {

namespace {

constexpr uint32_t kPerFrameTransientAllocatorSize = 4u << 20;
constexpr uint32_t kLightPassTextureCount         = 4;
constexpr uint32_t kLightCoarseCullingThreadCount = 64;
constexpr uint32_t kTestPointLightCount           = 128;

[[nodiscard]] uint32_t alignUp(uint32_t value, uint32_t alignment)
{
  const uint32_t safeAlignment = alignment == 0 ? 1u : alignment;
  const uint32_t mask          = safeAlignment - 1u;
  return (value + mask) & ~mask;
}

[[nodiscard]] glm::vec4 normalizePlane(glm::vec4 plane)
{
  const float length = glm::length(glm::vec3(plane));
  return length > 0.0f ? plane / length : plane;
}

[[nodiscard]] std::array<glm::vec4, shaderio::LGPUCullingFrustumPlaneCount> extractFrustumPlanes(const glm::mat4& viewProjection)
{
  const glm::mat4 transposed = glm::transpose(viewProjection);
  return {
      normalizePlane(transposed[3] + transposed[0]),
      normalizePlane(transposed[3] - transposed[0]),
      normalizePlane(transposed[3] + transposed[1]),
      normalizePlane(transposed[3] - transposed[1]),
      normalizePlane(transposed[3] + transposed[2]),
      normalizePlane(transposed[3] - transposed[2]),
  };
}

struct OverlayCircle
{
  ImVec2 center{};
  float  radius{0.0f};
};

[[nodiscard]] bool projectWorldToViewportCircle(const shaderio::CameraUniforms& camera,
                                                const glm::vec4&               viewportRect,
                                                const glm::vec3&               worldCenter,
                                                float                          worldRadius,
                                                OverlayCircle&                 outCircle)
{
  if(viewportRect.z <= 1.0f || viewportRect.w <= 1.0f)
  {
    return false;
  }

  const glm::mat4 inverseView = glm::inverse(camera.view);
  const glm::vec3 right = glm::normalize(glm::vec3(inverseView[0]));

  const auto projectPoint = [&](const glm::vec3& point, ImVec2& screenPoint) -> bool {
    const glm::vec4 clip = camera.viewProjection * glm::vec4(point, 1.0f);
    if(std::abs(clip.w) <= 1e-5f)
    {
      return false;
    }

    const glm::vec3 ndc = glm::vec3(clip) / clip.w;
    screenPoint.x = viewportRect.x + (ndc.x * 0.5f + 0.5f) * viewportRect.z;
    screenPoint.y = viewportRect.y + (ndc.y * 0.5f + 0.5f) * viewportRect.w;
    return true;
  };

  ImVec2 centerPoint{};
  ImVec2 radiusPoint{};
  if(!projectPoint(worldCenter, centerPoint) || !projectPoint(worldCenter + right * worldRadius, radiusPoint))
  {
    return false;
  }

  outCircle.center = centerPoint;
  outCircle.radius = std::max(1.0f, std::sqrt((radiusPoint.x - centerPoint.x) * (radiusPoint.x - centerPoint.x)
                                              + (radiusPoint.y - centerPoint.y) * (radiusPoint.y - centerPoint.y)));
  return true;
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
                                  VmaAllocationCreateFlags     flags       = {},
                                  bool                         enableExternalHostMemory = false)
{
  const bool hostAccessibleBuffer = memoryUsage == VMA_MEMORY_USAGE_CPU_ONLY || memoryUsage == VMA_MEMORY_USAGE_CPU_TO_GPU
                                    || memoryUsage == VMA_MEMORY_USAGE_GPU_TO_CPU
                                    || (flags & (VMA_ALLOCATION_CREATE_HOST_ACCESS_SEQUENTIAL_WRITE_BIT
                                                 | VMA_ALLOCATION_CREATE_HOST_ACCESS_RANDOM_BIT
                                                 | VMA_ALLOCATION_CREATE_MAPPED_BIT)) != 0;

  const VkBufferUsageFlags2CreateInfoKHR usageInfo{
      .sType = VK_STRUCTURE_TYPE_BUFFER_USAGE_FLAGS_2_CREATE_INFO_KHR,
      .usage = usage | VK_BUFFER_USAGE_2_SHADER_DEVICE_ADDRESS_BIT_KHR,
  };

  VkExternalMemoryBufferCreateInfo externalMemoryBufferCreateInfo{
      .sType       = VK_STRUCTURE_TYPE_EXTERNAL_MEMORY_BUFFER_CREATE_INFO,
      .pNext       = &usageInfo,
      .handleTypes = VK_EXTERNAL_MEMORY_HANDLE_TYPE_HOST_ALLOCATION_BIT_EXT,
  };

  const void* bufferCreatePNext = (enableExternalHostMemory && hostAccessibleBuffer)
                                      ? static_cast<const void*>(&externalMemoryBufferCreateInfo)
                                      : static_cast<const void*>(&usageInfo);

  const VkBufferCreateInfo bufferInfo{
      .sType       = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO,
      .pNext       = bufferCreatePNext,
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
  utils::Buffer stagingBuffer = upload::createUploadStagingBuffer(device, allocator, data);
  stagingBuffers.push_back(stagingBuffer);
  return stagingBuffer;
}

static utils::Buffer createBufferAndUploadData(const VkDevice               device,
                                               const VmaAllocator           allocator,
                                               std::vector<utils::Buffer>&  stagingBuffers,
                                               const VkCommandBuffer        cmd,
                                               std::span<const std::byte>   data,
                                               const VkBufferUsageFlags2KHR usage,
                                               const upload::StaticBufferUploadPolicy& uploadPolicy)
{
  return upload::createStaticBufferWithUpload(device, allocator, cmd, data, usage, uploadPolicy, &stagingBuffers);
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
  deviceCreateInfo.deviceExtensions.push_back({VK_EXT_EXTERNAL_MEMORY_HOST_EXTENSION_NAME, false, nullptr});
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
  m_device.staticBufferUploadPolicy =
      upload::buildStaticBufferUploadPolicy(m_device.device->getPhysicalMemoryProperties());
  if(m_device.staticBufferUploadPolicy.allowDirectHostVisibleDeviceLocalUpload)
  {
    LOGI("Static buffer upload: staging fallback with ReBAR direct-write for buffers up to %.1f MiB",
         static_cast<double>(m_device.staticBufferUploadPolicy.directUploadThreshold) / (1024.0 * 1024.0));
  }
  else
  {
    LOGI("Static buffer upload: host staging to device-local buffers");
  }

  m_device.samplerPool.init(nativeDevice);

  m_meshPool.init(nativeDevice, m_device.allocator, m_device.staticBufferUploadPolicy);

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
  createFrameSubmission(m_swapchainDependent.swapchain->getRequestedImageCount());
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
    createDepthPyramidResources();
    createGPUCullingResources();
    createLightCoarseCullingResources();

    // Initialize CSM shadow cascade resources
    CSMShadowResources::CreateInfo csmInfo{
        .cascadeCount      = 4,
        .cascadeResolution = 1024,
        .shadowFormat      = VK_FORMAT_D32_SFLOAT,
    };
    m_csmShadowResources.init(nativeDevice, m_device.allocator, cmd, csmInfo);

    // Create per-frame GBuffer descriptor sets for LightPass.
    {
      // Binding 0: Array of 5 sampled images (GBuffer0/1/2 + Depth + Shadow)
      const VkDescriptorSetLayoutBinding textureBinding{
          shaderio::LBindTextures,
          VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER,
          kLightPassTextureCount,
          VK_SHADER_STAGE_FRAGMENT_BIT,
          nullptr
      };
      const VkDescriptorSetLayoutBinding shadowMapBinding{
          shaderio::LBindShadowMap,
          VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER,
          1,
          VK_SHADER_STAGE_FRAGMENT_BIT,
          nullptr
      };

      const VkDescriptorSetLayoutBinding pointLightBinding{
          2, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 1, VK_SHADER_STAGE_FRAGMENT_BIT, nullptr
      };
      const VkDescriptorSetLayoutBinding pointCoarseBoundsBinding{
          3, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 1, VK_SHADER_STAGE_FRAGMENT_BIT, nullptr
      };
      const VkDescriptorSetLayoutBinding lightCoarseCullingUniformBinding{
          4, VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER, 1, VK_SHADER_STAGE_FRAGMENT_BIT, nullptr
      };

      const std::array<VkDescriptorSetLayoutBinding, 5> bindings = {
          textureBinding,
          shadowMapBinding,
          pointLightBinding,
          pointCoarseBoundsBinding,
          lightCoarseCullingUniformBinding,
      };

      const VkDescriptorSetLayoutCreateInfo layoutInfo{
          .sType        = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO,
          .bindingCount = static_cast<uint32_t>(bindings.size()),
          .pBindings    = bindings.data(),
      };
      VK_CHECK(vkCreateDescriptorSetLayout(nativeDevice, &layoutInfo, nullptr, &m_device.gbufferTextureSetLayout));
      DBG_VK_NAME(m_device.gbufferTextureSetLayout);

      const uint32_t frameCount = static_cast<uint32_t>(m_perFrame.frameUserData.size());
      ASSERT(frameCount > 0, "Renderer::init requires per-frame data before allocating LightPass descriptor sets");

      std::vector<VkDescriptorSetLayout> setLayouts(frameCount, m_device.gbufferTextureSetLayout);
      m_device.gbufferTextureSets.resize(frameCount, VK_NULL_HANDLE);
      const VkDescriptorSetAllocateInfo allocInfo{
          .sType              = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO,
          .descriptorPool     = m_device.descriptorPool,
          .descriptorSetCount = frameCount,
          .pSetLayouts        = setLayouts.data(),
      };
      VK_CHECK(vkAllocateDescriptorSets(nativeDevice, &allocInfo, m_device.gbufferTextureSets.data()));
      for(VkDescriptorSet descriptorSet : m_device.gbufferTextureSets)
      {
        DBG_VK_NAME(descriptorSet);
      }
    }

    utils::endSingleTimeCommands(cmd, nativeDevice, m_device.transientCmdPool, nativeGraphicsQueue);
  }

  // Update GBuffer texture descriptor set
  updateGBufferTextureDescriptorSet();

  createGraphicDescriptorSet();
  prebuildRequiredPipelineVariants();

  {
    VkCommandBuffer cmd = utils::beginSingleTimeCommands(nativeDevice, m_device.transientCmdPool);
    m_device.vertexBuffer = createBufferAndUploadData(nativeDevice, m_device.allocator, m_device.stagingBuffers, cmd,
                                                      std::as_bytes(std::span{s_vertices}),
                                                      VK_BUFFER_USAGE_VERTEX_BUFFER_BIT | VK_BUFFER_USAGE_STORAGE_BUFFER_BIT,
                                                      m_device.staticBufferUploadPolicy);
    DBG_VK_NAME(m_device.vertexBuffer.buffer);

    m_device.pointsBuffer = createBufferAndUploadData(nativeDevice, m_device.allocator, m_device.stagingBuffers, cmd,
                                                      std::as_bytes(std::span{s_points}), VK_BUFFER_USAGE_STORAGE_BUFFER_BIT,
                                                      m_device.staticBufferUploadPolicy);
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
  m_depthPrepass        = std::make_unique<DepthPrepass>(this);
  m_depthPyramidPass    = std::make_unique<DepthPyramidPass>(this);
  m_gpuCullingPass      = std::make_unique<GPUCullingPass>(this);
  m_lightCullingPass    = std::make_unique<LightCullingPass>(this);
  m_csmShadowPass       = std::make_unique<CSMShadowPass>(this);
  m_lightPass           = std::make_unique<LightPass>(this);
  m_forwardPass         = std::make_unique<ForwardPass>(this);
  m_debugPass           = std::make_unique<DebugPass>(this);
  m_presentPass         = std::make_unique<PresentPass>(this);
  m_imguiPass           = std::make_unique<ImguiPass>(this);
  m_passExecutor.clear();
  m_passExecutor.addPass(*m_depthPrepass);
  m_passExecutor.addPass(*m_depthPyramidPass);
  m_passExecutor.addPass(*m_gpuCullingPass);
  m_passExecutor.addPass(*m_lightCullingPass);
  m_passExecutor.addPass(*m_csmShadowPass);
  m_passExecutor.addPass(*m_gbufferPass);
  // m_passExecutor.addPass(*m_animateVerticesPass);
  // m_passExecutor.addPass(*m_sceneOpaquePass);
  m_passExecutor.addPass(*m_lightPass);
  m_passExecutor.addPass(*m_forwardPass);
  m_passExecutor.addPass(*m_debugPass);
  m_passExecutor.addPass(*m_presentPass);
  m_passExecutor.addPass(*m_imguiPass);
  createPassGpuProfileResources();
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
  if(m_device.depthPyramidPipelineLayout != VK_NULL_HANDLE)
  {
    vkDestroyPipelineLayout(device, m_device.depthPyramidPipelineLayout, nullptr);
    m_device.depthPyramidPipelineLayout = VK_NULL_HANDLE;
  }
  if(m_device.depthPyramidSetLayout != VK_NULL_HANDLE)
  {
    vkDestroyDescriptorSetLayout(device, m_device.depthPyramidSetLayout, nullptr);
    m_device.depthPyramidSetLayout = VK_NULL_HANDLE;
  }
  m_device.depthPyramidDescriptorSet = VK_NULL_HANDLE;
  destroyBuffer(m_device.allocator, m_depthPyramidUniformBuffer);
  if(m_device.gpuCullingPipelineLayout != VK_NULL_HANDLE)
  {
    vkDestroyPipelineLayout(device, m_device.gpuCullingPipelineLayout, nullptr);
    m_device.gpuCullingPipelineLayout = VK_NULL_HANDLE;
  }
  m_device.gpuCullingDescriptorSets.clear();
  if(m_device.gpuCullingSetLayout != VK_NULL_HANDLE)
  {
    vkDestroyDescriptorSetLayout(device, m_device.gpuCullingSetLayout, nullptr);
    m_device.gpuCullingSetLayout = VK_NULL_HANDLE;
  }
  if(m_device.lightCoarseCullingPipelineLayout != VK_NULL_HANDLE)
  {
    vkDestroyPipelineLayout(device, m_device.lightCoarseCullingPipelineLayout, nullptr);
    m_device.lightCoarseCullingPipelineLayout = VK_NULL_HANDLE;
  }
  m_device.lightCoarseCullingDescriptorSets.clear();
  if(m_device.lightCoarseCullingSetLayout != VK_NULL_HANDLE)
  {
    vkDestroyDescriptorSetLayout(device, m_device.lightCoarseCullingSetLayout, nullptr);
    m_device.lightCoarseCullingSetLayout = VK_NULL_HANDLE;
  }
  destroyPassGpuProfileResources();
  m_lightResources.deinit();
  // Note: gbufferTextureSets are freed automatically when descriptorPool is destroyed
  m_device.gbufferTextureSets.clear();
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
  if(m_device.debugPipelineLayout)
  {
    m_device.debugPipelineLayout->deinit();
    m_device.debugPipelineLayout.reset();
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
    destroyBuffer(m_device.allocator, frameUserData.lightingBuffer);
    destroyBuffer(m_device.allocator, frameUserData.lightCullingBuffer);
    destroyBuffer(m_device.allocator, frameUserData.gpuCullingObjectBuffer);
    destroyBuffer(m_device.allocator, frameUserData.gpuCullingIndirectBuffer);
    destroyBuffer(m_device.allocator, frameUserData.gpuCullingStatsBuffer);
    destroyBuffer(m_device.allocator, frameUserData.gpuCullingUniformBuffer);
    destroyBuffer(m_device.allocator, frameUserData.gpuCullingResultBuffer);
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

  m_csmShadowResources.deinit();
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

PipelineHandle Renderer::getDepthPrepassOpaquePipelineHandle() const
{
  return m_depthPrepassOpaquePipeline;
}

PipelineHandle Renderer::getDepthPrepassAlphaTestPipelineHandle() const
{
  return m_depthPrepassAlphaTestPipeline;
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

PipelineHandle Renderer::getShadowPipelineHandle() const
{
  return m_shadowPipeline;
}

PipelineHandle Renderer::getDebugPipelineHandle() const
{
  return m_debugPipeline;
}

PipelineHandle Renderer::getGPUCullingDebugPipelineHandle() const
{
  return m_gpuCullingDebugPipeline;
}

PipelineHandle Renderer::getCSMShadowPipelineHandle() const
{
  // TODO(Task 8): Create actual CSM shadow pipeline
  // For now, reuse regular shadow pipeline
  return m_shadowPipeline;
}

PipelineHandle Renderer::getLightCullingPipelineHandle() const
{
  return m_pointLightCoarseCullingPipeline;
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

VkImageView Renderer::getShadowMapView() const
{
  return m_csmShadowResources.getCascadeView();
}

VkImage Renderer::getShadowMapImage() const
{
  return m_csmShadowResources.getCascadeImage();
}

shaderio::ShadowUniforms* Renderer::getShadowUniformsData()
{
  return m_csmShadowResources.getShadowUniformsData();
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

  cacheGPUCullingStats(m_perFrame.frameContext->getCurrentFrameIndex());

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
  if(params.debugOptions.showViewportAxis && params.cameraUniforms != nullptr)
  {
    ui::DrawAxisInRect(params.viewportImageRect, params.cameraUniforms->view);
  }

  if(params.debugOptions.showGPUCullingOverlay || params.debugOptions.showPassGpuProfile)
  {
    drawGPUInfoOverlay(params);
  }

  if(params.recordUi)
  {
    params.recordUi(cmd);
  }
}

void Renderer::beginPresentPass(rhi::CommandList& cmd)
{
  const VkImage swapchainImage = getCurrentSwapchainImage();
  if(swapchainImage != VK_NULL_HANDLE)
  {
    const VkCommandBuffer vkCmd = rhi::vulkan::getNativeCommandBuffer(cmd);
    const VkImageMemoryBarrier2 barrier{
        .sType               = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER_2,
        .srcStageMask        = VK_PIPELINE_STAGE_2_TRANSFER_BIT | VK_PIPELINE_STAGE_2_ALL_GRAPHICS_BIT,
        .srcAccessMask       = VK_ACCESS_2_TRANSFER_WRITE_BIT | VK_ACCESS_2_MEMORY_WRITE_BIT,
        .dstStageMask        = VK_PIPELINE_STAGE_2_COLOR_ATTACHMENT_OUTPUT_BIT,
        .dstAccessMask       = VK_ACCESS_2_COLOR_ATTACHMENT_WRITE_BIT,
        .oldLayout           = VK_IMAGE_LAYOUT_GENERAL,
        .newLayout           = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL,
        .srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
        .dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
        .image               = swapchainImage,
        .subresourceRange    = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1},
    };
    const VkDependencyInfo dependencyInfo{
        .sType                   = VK_STRUCTURE_TYPE_DEPENDENCY_INFO,
        .imageMemoryBarrierCount = 1,
        .pImageMemoryBarriers    = &barrier,
    };
    vkCmdPipelineBarrier2(vkCmd, &dependencyInfo);
  }

  beginDynamicRenderingToSwapchain(cmd);
}

void Renderer::endPresentPass(rhi::CommandList& cmd)
{
  endDynamicRenderingToSwapchain(cmd);

  // Close the swapchain image layout explicitly at the end of the final UI pass.
  // This keeps presentation correctness local to the presentation path instead of
  // relying on PassExecutor's state inference after the pass graph has completed.
  VkImage swapchainImage = getCurrentSwapchainImage();
  if(swapchainImage == VK_NULL_HANDLE)
  {
    return;
  }

  const VkCommandBuffer vkCmd = rhi::vulkan::getNativeCommandBuffer(cmd);
  const VkImageMemoryBarrier2 barrier{
      .sType               = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER_2,
      .srcStageMask        = VK_PIPELINE_STAGE_2_COLOR_ATTACHMENT_OUTPUT_BIT,
      .srcAccessMask       = VK_ACCESS_2_COLOR_ATTACHMENT_WRITE_BIT,
      .dstStageMask        = VK_PIPELINE_STAGE_2_BOTTOM_OF_PIPE_BIT,
      .dstAccessMask       = VK_ACCESS_2_NONE,
      .oldLayout           = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL,
      .newLayout           = VK_IMAGE_LAYOUT_PRESENT_SRC_KHR,
      .srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
      .dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
      .image               = swapchainImage,
      .subresourceRange    = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1},
  };
  const VkDependencyInfo dependencyInfo{
      .sType                   = VK_STRUCTURE_TYPE_DEPENDENCY_INFO,
      .imageMemoryBarrierCount = 1,
      .pImageMemoryBarriers    = &barrier,
  };
  vkCmdPipelineBarrier2(vkCmd, &dependencyInfo);

  cmd.setResourceState(rhi::ResourceHandle{rhi::ResourceKind::Texture, kPassSwapchainHandle.index, kPassSwapchainHandle.generation},
                       rhi::ResourceState::Present);
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
    frameUserData.lightingBuffer =
        createBuffer(device, m_device.allocator, sizeof(shaderio::LightingUniforms), VK_BUFFER_USAGE_2_UNIFORM_BUFFER_BIT_KHR,
                     VMA_MEMORY_USAGE_CPU_TO_GPU, VMA_ALLOCATION_CREATE_HOST_ACCESS_SEQUENTIAL_WRITE_BIT);
    frameUserData.lightCullingBuffer =
        createBuffer(device, m_device.allocator, sizeof(shaderio::LightCullingUniforms), VK_BUFFER_USAGE_2_UNIFORM_BUFFER_BIT_KHR,
                     VMA_MEMORY_USAGE_CPU_TO_GPU, VMA_ALLOCATION_CREATE_HOST_ACCESS_SEQUENTIAL_WRITE_BIT);
    frameUserData.gpuCullingUniformBuffer =
        createBuffer(device, m_device.allocator, sizeof(shaderio::GPUCullingUniforms), VK_BUFFER_USAGE_2_UNIFORM_BUFFER_BIT_KHR,
                     VMA_MEMORY_USAGE_CPU_TO_GPU, VMA_ALLOCATION_CREATE_HOST_ACCESS_SEQUENTIAL_WRITE_BIT);
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
  resolvePassGpuProfileResults(currentFrameIndex);
  m_perFrame.frameUserData[currentFrameIndex].transientAllocator.reset();

  return acquireSwapchainImage(*m_swapchainDependent.swapchain, m_swapchainDependent.currentImageIndex);
}

void Renderer::updateGBufferTextureDescriptorSet()
{
  if(m_device.gbufferTextureSets.empty() || m_perFrame.frameUserData.empty())
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
  if(m_csmShadowResources.getCascadeView() == VK_NULL_HANDLE)
  {
    LOGW("CSM cascade shadow view is null, skipping GBuffer descriptor update");
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

  // Write descriptor array for GBuffer textures (indices 0-2) + scene depth (index 3).
  std::array<VkDescriptorImageInfo, kLightPassTextureCount> imageInfos{};
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
  const VkDescriptorImageInfo shadowMapInfo{
      .sampler     = linearSampler,
      .imageView   = m_csmShadowResources.getCascadeView(),
      .imageLayout = VK_IMAGE_LAYOUT_GENERAL,
  };

  const uint32_t frameCount = static_cast<uint32_t>(m_perFrame.frameUserData.size());
  ASSERT(m_device.gbufferTextureSets.size() == frameCount, "LightPass descriptor set count must match frame count");
  for(uint32_t frameIndex = 0; frameIndex < frameCount; ++frameIndex)
  {
    const VkDescriptorBufferInfo pointLightBufferInfo{
        .buffer = m_lightResources.getPointLightBuffer(frameIndex),
        .offset = 0,
        .range  = VK_WHOLE_SIZE,
    };
    const VkDescriptorBufferInfo pointCoarseBoundsBufferInfo{
        .buffer = m_lightResources.getPointCoarseBoundsBuffer(frameIndex),
        .offset = 0,
        .range  = VK_WHOLE_SIZE,
    };
    const VkDescriptorBufferInfo coarseCullingUniformBufferInfo{
        .buffer = m_lightResources.getCoarseCullingUniformBuffer(frameIndex),
        .offset = 0,
        .range  = sizeof(shaderio::LightCoarseCullingUniforms),
    };

    const std::array<VkWriteDescriptorSet, 5> writes{{
        VkWriteDescriptorSet{
            .sType           = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET,
            .dstSet          = m_device.gbufferTextureSets[frameIndex],
            .dstBinding      = shaderio::LBindTextures,
            .dstArrayElement = 0,
            .descriptorCount = kLightPassTextureCount,
            .descriptorType  = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER,
            .pImageInfo      = imageInfos.data(),
        },
        VkWriteDescriptorSet{
            .sType           = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET,
            .dstSet          = m_device.gbufferTextureSets[frameIndex],
            .dstBinding      = shaderio::LBindShadowMap,
            .dstArrayElement = 0,
            .descriptorCount = 1,
            .descriptorType  = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER,
            .pImageInfo      = &shadowMapInfo,
        },
        VkWriteDescriptorSet{
            .sType           = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET,
            .dstSet          = m_device.gbufferTextureSets[frameIndex],
            .dstBinding      = 2,
            .dstArrayElement = 0,
            .descriptorCount = 1,
            .descriptorType  = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER,
            .pBufferInfo     = &pointLightBufferInfo,
        },
        VkWriteDescriptorSet{
            .sType           = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET,
            .dstSet          = m_device.gbufferTextureSets[frameIndex],
            .dstBinding      = 3,
            .dstArrayElement = 0,
            .descriptorCount = 1,
            .descriptorType  = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER,
            .pBufferInfo     = &pointCoarseBoundsBufferInfo,
        },
        VkWriteDescriptorSet{
            .sType           = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET,
            .dstSet          = m_device.gbufferTextureSets[frameIndex],
            .dstBinding      = 4,
            .dstArrayElement = 0,
            .descriptorCount = 1,
            .descriptorType  = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER,
            .pBufferInfo     = &coarseCullingUniformBufferInfo,
        },
    }};

    vkUpdateDescriptorSets(nativeDevice, static_cast<uint32_t>(writes.size()), writes.data(), 0, nullptr);
  }
}

void Renderer::updateLightingUniformBuffer(uint32_t frameIndex, const shaderio::LightingUniforms& lightingUniforms)
{
  if(frameIndex >= m_perFrame.frameUserData.size())
  {
    return;
  }

  PerFrameResources::FrameUserData& frameUserData = m_perFrame.frameUserData[frameIndex];
  void* mappedData = nullptr;
  VK_CHECK(vmaMapMemory(m_device.allocator, frameUserData.lightingBuffer.allocation, &mappedData));
  std::memcpy(mappedData, &lightingUniforms, sizeof(lightingUniforms));
  VK_CHECK(vmaFlushAllocation(m_device.allocator, frameUserData.lightingBuffer.allocation, 0, sizeof(lightingUniforms)));
  vmaUnmapMemory(m_device.allocator, frameUserData.lightingBuffer.allocation);
}

void Renderer::updateLightCullingUniformBuffer(uint32_t frameIndex, const shaderio::LightCullingUniforms& cullingUniforms)
{
  if(frameIndex >= m_perFrame.frameUserData.size())
  {
    return;
  }

  PerFrameResources::FrameUserData& frameUserData = m_perFrame.frameUserData[frameIndex];
  void* mappedData = nullptr;
  VK_CHECK(vmaMapMemory(m_device.allocator, frameUserData.lightCullingBuffer.allocation, &mappedData));
  std::memcpy(mappedData, &cullingUniforms, sizeof(cullingUniforms));
  VK_CHECK(vmaFlushAllocation(m_device.allocator, frameUserData.lightCullingBuffer.allocation, 0, sizeof(cullingUniforms)));
  vmaUnmapMemory(m_device.allocator, frameUserData.lightCullingBuffer.allocation);
}

void Renderer::ensureGPUCullingBuffers(PerFrameResources::FrameUserData& frameUserData, uint32_t requiredMeshCount)
{
  const uint32_t requiredCapacity = std::max(requiredMeshCount, 1u);
  if(frameUserData.gpuCullingMeshCapacity >= requiredCapacity)
  {
    return;
  }

  const VkDevice device = fromNativeHandle<VkDevice>(m_device.device->getNativeDevice());
  destroyBuffer(m_device.allocator, frameUserData.gpuCullingObjectBuffer);
  destroyBuffer(m_device.allocator, frameUserData.gpuCullingIndirectBuffer);
  destroyBuffer(m_device.allocator, frameUserData.gpuCullingStatsBuffer);
  destroyBuffer(m_device.allocator, frameUserData.gpuCullingResultBuffer);

  frameUserData.gpuCullingObjectBuffer =
      createBuffer(device,
                   m_device.allocator,
                   sizeof(shaderio::GPUCullObject) * requiredCapacity,
                   VK_BUFFER_USAGE_2_STORAGE_BUFFER_BIT_KHR,
                   VMA_MEMORY_USAGE_CPU_TO_GPU,
                   VMA_ALLOCATION_CREATE_HOST_ACCESS_SEQUENTIAL_WRITE_BIT);
  frameUserData.gpuCullingIndirectBuffer =
      createBuffer(device,
                   m_device.allocator,
                   sizeof(shaderio::GPUCullIndirectCommand) * requiredCapacity,
                   VK_BUFFER_USAGE_2_STORAGE_BUFFER_BIT_KHR | VK_BUFFER_USAGE_2_INDIRECT_BUFFER_BIT_KHR,
                   VMA_MEMORY_USAGE_GPU_ONLY);
  frameUserData.gpuCullingStatsBuffer =
      createBuffer(device,
                   m_device.allocator,
                   sizeof(shaderio::GPUCullStats),
                   VK_BUFFER_USAGE_2_STORAGE_BUFFER_BIT_KHR,
                   VMA_MEMORY_USAGE_CPU_TO_GPU,
                   VMA_ALLOCATION_CREATE_HOST_ACCESS_SEQUENTIAL_WRITE_BIT);
  frameUserData.gpuCullingResultBuffer =
      createBuffer(device,
                   m_device.allocator,
                   sizeof(uint32_t) * requiredCapacity,
                   VK_BUFFER_USAGE_2_STORAGE_BUFFER_BIT_KHR,
                   VMA_MEMORY_USAGE_CPU_TO_GPU,
                   VMA_ALLOCATION_CREATE_HOST_ACCESS_SEQUENTIAL_WRITE_BIT);
  frameUserData.gpuCullingMeshCapacity = requiredCapacity;
  frameUserData.gpuCullingResults.resize(requiredCapacity, shaderio::LGPUCullResultVisible);
}

void Renderer::updateGPUCullingBuffers(uint32_t frameIndex, const RenderParams& params)
{
  if(frameIndex >= m_perFrame.frameUserData.size())
  {
    return;
  }

  PerFrameResources::FrameUserData& frameUserData = m_perFrame.frameUserData[frameIndex];
  const uint32_t objectCount =
      params.gltfModel != nullptr ? static_cast<uint32_t>(params.gltfModel->meshes.size()) : 0u;

  ensureGPUCullingBuffers(frameUserData, objectCount);
  updateGPUCullingDescriptorSet(frameIndex);

  if(frameUserData.gpuCullingObjectBuffer.buffer == VK_NULL_HANDLE
     || frameUserData.gpuCullingIndirectBuffer.buffer == VK_NULL_HANDLE
     || frameUserData.gpuCullingStatsBuffer.buffer == VK_NULL_HANDLE)
  {
    return;
  }

  std::vector<shaderio::GPUCullObject> objects(objectCount);
  if(params.gltfModel != nullptr)
  {
    for(uint32_t meshIndex = 0; meshIndex < objectCount; ++meshIndex)
    {
      const MeshRecord* meshRecord = m_meshPool.tryGet(params.gltfModel->meshes[meshIndex]);
      if(meshRecord == nullptr)
      {
        continue;
      }

      const glm::vec3 center = 0.5f * (meshRecord->worldBoundsMin + meshRecord->worldBoundsMax);
      const float radius = glm::length(meshRecord->worldBoundsMax - center);

      uint32_t flags = shaderio::LGPUCullFlagFrustumCulling | shaderio::LGPUCullFlagOcclusionCulling;
      if(meshRecord->materialIndex >= 0 && meshRecord->materialIndex < static_cast<int32_t>(params.gltfModel->materials.size()))
      {
        const MaterialHandle materialHandle = params.gltfModel->materials[meshRecord->materialIndex];
        if(getMaterialTextureIndices(materialHandle, params.gltfModel).alphaMode == shaderio::LAlphaBlend)
        {
          flags = shaderio::LGPUCullFlagFrustumCulling | shaderio::LGPUCullFlagTransparent;
        }
      }

      objects[meshIndex] = shaderio::GPUCullObject{
          .sphereCenterRadius = glm::vec4(center, radius),
          .indexCount         = meshRecord->indexCount,
          .firstIndex         = 0u,
          .vertexOffset       = 0,
          .flags              = flags,
      };
    }
  }

  void* mappedData = nullptr;
  VK_CHECK(vmaMapMemory(m_device.allocator, frameUserData.gpuCullingObjectBuffer.allocation, &mappedData));
  if(!objects.empty())
  {
    std::memcpy(mappedData, objects.data(), sizeof(shaderio::GPUCullObject) * objects.size());
  }
  VK_CHECK(vmaFlushAllocation(m_device.allocator,
                              frameUserData.gpuCullingObjectBuffer.allocation,
                              0,
                              sizeof(shaderio::GPUCullObject) * std::max<size_t>(objects.size(), 1u)));
  vmaUnmapMemory(m_device.allocator, frameUserData.gpuCullingObjectBuffer.allocation);

  const shaderio::GPUCullStats zeroStats{};
  VK_CHECK(vmaMapMemory(m_device.allocator, frameUserData.gpuCullingStatsBuffer.allocation, &mappedData));
  std::memcpy(mappedData, &zeroStats, sizeof(zeroStats));
  VK_CHECK(vmaFlushAllocation(m_device.allocator, frameUserData.gpuCullingStatsBuffer.allocation, 0, sizeof(zeroStats)));
  vmaUnmapMemory(m_device.allocator, frameUserData.gpuCullingStatsBuffer.allocation);

  if(frameUserData.gpuCullingResultBuffer.allocation != nullptr)
  {
    VK_CHECK(vmaMapMemory(m_device.allocator, frameUserData.gpuCullingResultBuffer.allocation, &mappedData));
    std::memset(mappedData, 0, sizeof(uint32_t) * objectCount);
    VK_CHECK(vmaFlushAllocation(m_device.allocator, frameUserData.gpuCullingResultBuffer.allocation, 0, sizeof(uint32_t) * std::max<uint32_t>(objectCount, 1u)));
    vmaUnmapMemory(m_device.allocator, frameUserData.gpuCullingResultBuffer.allocation);
  }

  const shaderio::GPUCullingUniforms uniforms = buildGPUCullingUniforms(params, objectCount);
  VK_CHECK(vmaMapMemory(m_device.allocator, frameUserData.gpuCullingUniformBuffer.allocation, &mappedData));
  std::memcpy(mappedData, &uniforms, sizeof(uniforms));
  VK_CHECK(vmaFlushAllocation(m_device.allocator, frameUserData.gpuCullingUniformBuffer.allocation, 0, sizeof(uniforms)));
  vmaUnmapMemory(m_device.allocator, frameUserData.gpuCullingUniformBuffer.allocation);
}

void Renderer::cacheGPUCullingStats(uint32_t frameIndex)
{
  if(frameIndex >= m_perFrame.frameUserData.size())
  {
    return;
  }

  PerFrameResources::FrameUserData& frameUserData = m_perFrame.frameUserData[frameIndex];
  if(frameUserData.gpuCullingStatsBuffer.allocation == nullptr)
  {
    return;
  }

  void* mappedData = nullptr;
  VK_CHECK(vmaMapMemory(m_device.allocator, frameUserData.gpuCullingStatsBuffer.allocation, &mappedData));
  VK_CHECK(vmaInvalidateAllocation(m_device.allocator, frameUserData.gpuCullingStatsBuffer.allocation, 0, sizeof(shaderio::GPUCullStats)));
  std::memcpy(&m_lastGPUCullingStats, mappedData, sizeof(m_lastGPUCullingStats));
  vmaUnmapMemory(m_device.allocator, frameUserData.gpuCullingStatsBuffer.allocation);

  m_lastGPUCullingOverlayObjects.clear();
  if(frameUserData.gpuCullingResultBuffer.allocation == nullptr || frameUserData.gpuCullingResults.empty())
  {
    return;
  }

  VK_CHECK(vmaMapMemory(m_device.allocator, frameUserData.gpuCullingResultBuffer.allocation, &mappedData));
  VK_CHECK(vmaInvalidateAllocation(m_device.allocator,
                                   frameUserData.gpuCullingResultBuffer.allocation,
                                   0,
                                   sizeof(uint32_t) * frameUserData.gpuCullingResults.size()));
  std::memcpy(frameUserData.gpuCullingResults.data(),
              mappedData,
              sizeof(uint32_t) * frameUserData.gpuCullingResults.size());
  vmaUnmapMemory(m_device.allocator, frameUserData.gpuCullingResultBuffer.allocation);

  const size_t objectCount = std::min<size_t>(m_lastGPUCullingStats.totalCount, frameUserData.gpuCullingResults.size());
  m_lastGPUCullingOverlayObjects.reserve(objectCount);
  if(objectCount == 0 || frameUserData.gpuCullingObjectBuffer.allocation == nullptr)
  {
    return;
  }

  VK_CHECK(vmaMapMemory(m_device.allocator, frameUserData.gpuCullingObjectBuffer.allocation, &mappedData));
  VK_CHECK(vmaInvalidateAllocation(m_device.allocator,
                                   frameUserData.gpuCullingObjectBuffer.allocation,
                                   0,
                                   sizeof(shaderio::GPUCullObject) * objectCount));
  const auto* objectData = static_cast<const shaderio::GPUCullObject*>(mappedData);
  for(size_t objectIndex = 0; objectIndex < objectCount; ++objectIndex)
  {
    const shaderio::GPUCullObject& object = objectData[objectIndex];
    if(object.indexCount == 0u && object.sphereCenterRadius.w == 0.0f)
    {
      continue;
    }

    m_lastGPUCullingOverlayObjects.push_back(GPUCullOverlayObject{
        .center = glm::vec3(object.sphereCenterRadius),
        .radius = object.sphereCenterRadius.w,
        .flags  = object.flags,
        .result = frameUserData.gpuCullingResults[objectIndex],
    });
  }
  vmaUnmapMemory(m_device.allocator, frameUserData.gpuCullingObjectBuffer.allocation);
}

void Renderer::drawGPUCullingOverlay(const RenderParams& params) const
{
  ImGui::Text("Visible %u / %u", m_lastGPUCullingStats.visibleCount, m_lastGPUCullingStats.totalCount);
  ImGui::Text("Opaque %u / %u", m_lastGPUCullingStats.opaqueVisibleCount, m_lastGPUCullingStats.opaqueCount);
  ImGui::Text("Transparent %u / %u",
              m_lastGPUCullingStats.transparentVisibleCount,
              m_lastGPUCullingStats.transparentCount);
  ImGui::Text("Frustum Culled %u", m_lastGPUCullingStats.frustumCulledCount);
  ImGui::Text("Occlusion Culled %u", m_lastGPUCullingStats.occlusionCulledCount);

  ImGui::SeparatorText("Legend");
  const struct LegendEntry
  {
    const char* label;
    ImU32       color;
  } legends[] = {
      {"Visible Opaque", IM_COL32(92, 220, 120, 210)},
      {"Visible Transparent", IM_COL32(92, 210, 255, 210)},
      {"Frustum Culled", IM_COL32(255, 92, 92, 210)},
      {"Occlusion Culled", IM_COL32(255, 176, 92, 210)},
  };

  for(const LegendEntry& entry : legends)
  {
    ImGui::ColorButton(entry.label,
                       ImGui::ColorConvertU32ToFloat4(entry.color),
                       ImGuiColorEditFlags_NoTooltip | ImGuiColorEditFlags_NoDragDrop,
                       ImVec2(12.0f, 12.0f));
    ImGui::SameLine();
    ImGui::TextUnformatted(entry.label);
  }
}

void Renderer::createPassGpuProfileResources()
{
  destroyPassGpuProfileResources();

  if(m_device.device == nullptr)
  {
    return;
  }

  VkPhysicalDeviceProperties2 deviceProperties2{.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PROPERTIES_2};
  vkGetPhysicalDeviceProperties2(fromNativeHandle<VkPhysicalDevice>(m_device.device->getNativePhysicalDevice()), &deviceProperties2);

  m_passGpuProfile.timestampPeriodNs = deviceProperties2.properties.limits.timestampPeriod;
  m_passGpuProfile.queryCount = static_cast<uint32_t>(m_passExecutor.getPassCount() * 2);
  m_passGpuProfile.passNames.clear();
  m_passGpuProfile.latestPassDurationsMs.clear();
  m_passGpuProfile.latestValid = false;
  m_passGpuProfile.frames.clear();

  if(m_passGpuProfile.queryCount == 0 || m_perFrame.frameUserData.empty())
  {
    return;
  }

  m_passGpuProfile.passNames.reserve(m_passExecutor.getPassCount());
  for(size_t passIndex = 0; passIndex < m_passExecutor.getPassCount(); ++passIndex)
  {
    const PassNode* pass = m_passExecutor.getPass(passIndex);
    m_passGpuProfile.passNames.push_back(pass != nullptr ? pass->getName() : "Unknown");
  }
  m_passGpuProfile.latestPassDurationsMs.assign(m_passExecutor.getPassCount(), 0.0);

  const VkDevice device = fromNativeHandle<VkDevice>(m_device.device->getNativeDevice());
  m_passGpuProfile.frames.resize(m_perFrame.frameUserData.size());
  for(PassGpuProfileFrame& frame : m_passGpuProfile.frames)
  {
    const VkQueryPoolCreateInfo queryPoolInfo{
        .sType      = VK_STRUCTURE_TYPE_QUERY_POOL_CREATE_INFO,
        .queryType  = VK_QUERY_TYPE_TIMESTAMP,
        .queryCount = m_passGpuProfile.queryCount,
    };
    VK_CHECK(vkCreateQueryPool(device, &queryPoolInfo, nullptr, &frame.queryPool));
    DBG_VK_NAME(frame.queryPool);
    frame.passDurationsMs.assign(m_passExecutor.getPassCount(), 0.0);
    frame.valid = false;
    frame.hasRecordedQueries = false;
  }
}

void Renderer::destroyPassGpuProfileResources()
{
  if(m_device.device != nullptr)
  {
    const VkDevice device = fromNativeHandle<VkDevice>(m_device.device->getNativeDevice());
    for(PassGpuProfileFrame& frame : m_passGpuProfile.frames)
    {
      if(frame.queryPool != VK_NULL_HANDLE)
      {
        vkDestroyQueryPool(device, frame.queryPool, nullptr);
        frame.queryPool = VK_NULL_HANDLE;
      }
      frame.passDurationsMs.clear();
      frame.valid = false;
      frame.hasRecordedQueries = false;
    }
  }

  m_passGpuProfile.frames.clear();
  m_passGpuProfile.passNames.clear();
  m_passGpuProfile.latestPassDurationsMs.clear();
  m_passGpuProfile.latestValid = false;
  m_passGpuProfile.queryCount = 0;
  m_passGpuProfile.timestampPeriodNs = 0.0f;
}

void Renderer::resolvePassGpuProfileResults(uint32_t frameIndex)
{
  if(frameIndex >= m_passGpuProfile.frames.size() || m_passGpuProfile.queryCount == 0)
  {
    return;
  }

  PassGpuProfileFrame& frame = m_passGpuProfile.frames[frameIndex];
  if(frame.queryPool == VK_NULL_HANDLE || !frame.hasRecordedQueries)
  {
    return;
  }

  std::vector<uint64_t> queryData(static_cast<size_t>(m_passGpuProfile.queryCount) * 2u, 0ull);
  const VkResult result = vkGetQueryPoolResults(fromNativeHandle<VkDevice>(m_device.device->getNativeDevice()),
                                                frame.queryPool,
                                                0,
                                                m_passGpuProfile.queryCount,
                                                sizeof(uint64_t) * queryData.size(),
                                                queryData.data(),
                                                sizeof(uint64_t) * 2u,
                                                VK_QUERY_RESULT_64_BIT | VK_QUERY_RESULT_WITH_AVAILABILITY_BIT);
  if(result != VK_SUCCESS)
  {
    frame.valid = false;
    return;
  }

  bool anyValidPass = false;
  for(size_t passIndex = 0; passIndex < frame.passDurationsMs.size(); ++passIndex)
  {
    const size_t beginIndex = passIndex * 4u;
    const size_t endIndex = beginIndex + 2u;
    const bool beginAvailable = queryData[beginIndex + 1u] != 0ull;
    const bool endAvailable = queryData[endIndex + 1u] != 0ull;
    if(!beginAvailable || !endAvailable || queryData[endIndex] < queryData[beginIndex])
    {
      frame.passDurationsMs[passIndex] = 0.0;
      continue;
    }

    const uint64_t delta = queryData[endIndex] - queryData[beginIndex];
    frame.passDurationsMs[passIndex] = static_cast<double>(delta) * static_cast<double>(m_passGpuProfile.timestampPeriodNs) * 1e-6;
    anyValidPass = true;
  }

  frame.valid = anyValidPass;
  if(anyValidPass)
  {
    m_passGpuProfile.latestPassDurationsMs = frame.passDurationsMs;
    m_passGpuProfile.latestValid = true;
  }
}

void Renderer::resetPassGpuProfileQueries(const rhi::CommandList& cmd, uint32_t frameIndex)
{
  if(frameIndex >= m_passGpuProfile.frames.size() || m_passGpuProfile.queryCount == 0)
  {
    return;
  }

  const VkQueryPool queryPool = m_passGpuProfile.frames[frameIndex].queryPool;
  if(queryPool == VK_NULL_HANDLE)
  {
    return;
  }

  vkCmdResetQueryPool(rhi::vulkan::getNativeCommandBuffer(cmd), queryPool, 0, m_passGpuProfile.queryCount);
  m_passGpuProfile.frames[frameIndex].valid = false;
  m_passGpuProfile.frames[frameIndex].hasRecordedQueries = false;
}

void Renderer::writePassGpuProfileTimestamp(const PassContext& context, uint32_t passIndex, bool isBegin) const
{
  if(context.cmd == nullptr || context.frameIndex >= m_passGpuProfile.frames.size() || m_passGpuProfile.queryCount == 0)
  {
    return;
  }

  const VkQueryPool queryPool = m_passGpuProfile.frames[context.frameIndex].queryPool;
  if(queryPool == VK_NULL_HANDLE)
  {
    return;
  }

  const uint32_t queryIndex = passIndex * 2u + (isBegin ? 0u : 1u);
  if(queryIndex >= m_passGpuProfile.queryCount)
  {
    return;
  }

  vkCmdWriteTimestamp2(rhi::vulkan::getNativeCommandBuffer(*context.cmd),
                       isBegin ? VK_PIPELINE_STAGE_2_TOP_OF_PIPE_BIT : VK_PIPELINE_STAGE_2_BOTTOM_OF_PIPE_BIT,
                       queryPool,
                       queryIndex);
}

void Renderer::drawPassGpuProfileOverlay(const RenderParams& params) const
{
  if(!m_passGpuProfile.latestValid || m_passGpuProfile.latestPassDurationsMs.empty())
  {
    ImGui::TextUnformatted("Waiting for GPU timestamps...");
    return;
  }

  double totalMs = 0.0;
  for(double durationMs : m_passGpuProfile.latestPassDurationsMs)
  {
    totalMs += durationMs;
  }

  ImGui::Text("Tracked Passes: %d", static_cast<int>(m_passGpuProfile.passNames.size()));
  ImGui::Text("Total: %.3f ms", totalMs);
  ImGui::Separator();
  for(size_t passIndex = 0; passIndex < m_passGpuProfile.passNames.size()
                         && passIndex < m_passGpuProfile.latestPassDurationsMs.size();
      ++passIndex)
  {
    ImGui::Text("%-18s %.3f ms", m_passGpuProfile.passNames[passIndex].c_str(),
                m_passGpuProfile.latestPassDurationsMs[passIndex]);
  }
}

void Renderer::drawGPUInfoOverlay(const RenderParams& params) const
{
  ImGuiWindowFlags flags = ImGuiWindowFlags_AlwaysAutoResize;
  if(params.viewportImageRect.z > 1.0f && params.viewportImageRect.w > 1.0f)
  {
    ImGui::SetNextWindowPos(ImVec2(params.viewportImageRect.x + params.viewportImageRect.z - 360.0f,
                                   params.viewportImageRect.y + 12.0f),
                            ImGuiCond_Always);
  }
  ImGui::SetNextWindowBgAlpha(0.78f);
  if(!ImGui::Begin("GPU Info", nullptr, flags))
  {
    ImGui::End();
    return;
  }

  if(params.debugOptions.showGPUCullingOverlay
     && ImGui::CollapsingHeader("GPU Culling", ImGuiTreeNodeFlags_DefaultOpen))
  {
    drawGPUCullingOverlay(params);
  }

  if(params.debugOptions.showPassGpuProfile
     && ImGui::CollapsingHeader("GPU Pass Profile", ImGuiTreeNodeFlags_DefaultOpen))
  {
    drawPassGpuProfileOverlay(params);
  }

  ImGui::End();
}

void Renderer::PassProfilingHooks::beforePass(const PassContext& context, const PassNode& pass, uint32_t passIndex) const
{
  (void)pass;
  if(renderer != nullptr)
  {
    renderer->writePassGpuProfileTimestamp(context, passIndex, true);
  }
}

void Renderer::PassProfilingHooks::afterPass(const PassContext& context, const PassNode& pass, uint32_t passIndex) const
{
  (void)pass;
  if(renderer != nullptr)
  {
    renderer->writePassGpuProfileTimestamp(context, passIndex, false);
  }
}

void Renderer::createGPUCullingResources()
{
  const VkDevice nativeDevice = fromNativeHandle<VkDevice>(m_device.device->getNativeDevice());
  const uint32_t frameCount = std::max<uint32_t>(1u, static_cast<uint32_t>(m_perFrame.frameUserData.size()));

  const std::array<VkDescriptorSetLayoutBinding, 6> bindings{{
      VkDescriptorSetLayoutBinding{0, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 1, VK_SHADER_STAGE_COMPUTE_BIT, nullptr},
      VkDescriptorSetLayoutBinding{1, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 1, VK_SHADER_STAGE_COMPUTE_BIT, nullptr},
      VkDescriptorSetLayoutBinding{2, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 1, VK_SHADER_STAGE_COMPUTE_BIT, nullptr},
      VkDescriptorSetLayoutBinding{3, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 1, VK_SHADER_STAGE_COMPUTE_BIT, nullptr},
      VkDescriptorSetLayoutBinding{4, VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE, shaderio::LDepthPyramidMaxMips, VK_SHADER_STAGE_COMPUTE_BIT, nullptr},
      VkDescriptorSetLayoutBinding{5, VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER, 1, VK_SHADER_STAGE_COMPUTE_BIT, nullptr},
  }};

  const VkDescriptorSetLayoutCreateInfo setLayoutInfo{
      .sType        = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO,
      .bindingCount = static_cast<uint32_t>(bindings.size()),
      .pBindings    = bindings.data(),
  };
  VK_CHECK(vkCreateDescriptorSetLayout(nativeDevice, &setLayoutInfo, nullptr, &m_device.gpuCullingSetLayout));
  DBG_VK_NAME(m_device.gpuCullingSetLayout);

  std::vector<VkDescriptorSetLayout> setLayouts(frameCount, m_device.gpuCullingSetLayout);
  m_device.gpuCullingDescriptorSets.resize(frameCount, VK_NULL_HANDLE);
  const VkDescriptorSetAllocateInfo allocInfo{
      .sType              = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO,
      .descriptorPool     = m_device.descriptorPool,
      .descriptorSetCount = frameCount,
      .pSetLayouts        = setLayouts.data(),
  };
  VK_CHECK(vkAllocateDescriptorSets(nativeDevice, &allocInfo, m_device.gpuCullingDescriptorSets.data()));

  const VkPipelineLayoutCreateInfo pipelineLayoutInfo{
      .sType          = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO,
      .setLayoutCount = 1,
      .pSetLayouts    = &m_device.gpuCullingSetLayout,
  };
  VK_CHECK(vkCreatePipelineLayout(nativeDevice, &pipelineLayoutInfo, nullptr, &m_device.gpuCullingPipelineLayout));
  DBG_VK_NAME(m_device.gpuCullingPipelineLayout);

  for(uint32_t frameIndex = 0; frameIndex < frameCount; ++frameIndex)
  {
    updateGPUCullingDescriptorSet(frameIndex);
  }
}

void Renderer::updateGPUCullingDescriptorSet(uint32_t frameIndex)
{
  if(frameIndex >= m_perFrame.frameUserData.size() || frameIndex >= m_device.gpuCullingDescriptorSets.size()
     || m_device.gpuCullingDescriptorSets[frameIndex] == VK_NULL_HANDLE)
  {
    return;
  }

  PerFrameResources::FrameUserData& frameUserData = m_perFrame.frameUserData[frameIndex];
  if(frameUserData.gpuCullingObjectBuffer.buffer == VK_NULL_HANDLE
     || frameUserData.gpuCullingIndirectBuffer.buffer == VK_NULL_HANDLE
     || frameUserData.gpuCullingStatsBuffer.buffer == VK_NULL_HANDLE
     || frameUserData.gpuCullingUniformBuffer.buffer == VK_NULL_HANDLE
     || frameUserData.gpuCullingResultBuffer.buffer == VK_NULL_HANDLE)
  {
    return;
  }

  SceneResources& sceneResources = m_swapchainDependent.sceneResources;
  const uint32_t mipCount = sceneResources.getDepthPyramidMipCount();
  if(mipCount == 0)
  {
    return;
  }

  const VkDevice nativeDevice = fromNativeHandle<VkDevice>(m_device.device->getNativeDevice());
  const std::array<VkDescriptorBufferInfo, 5> bufferInfos{{
      VkDescriptorBufferInfo{frameUserData.gpuCullingObjectBuffer.buffer, 0, VK_WHOLE_SIZE},
      VkDescriptorBufferInfo{frameUserData.gpuCullingIndirectBuffer.buffer, 0, VK_WHOLE_SIZE},
      VkDescriptorBufferInfo{frameUserData.gpuCullingStatsBuffer.buffer, 0, sizeof(shaderio::GPUCullStats)},
      VkDescriptorBufferInfo{frameUserData.gpuCullingResultBuffer.buffer, 0, VK_WHOLE_SIZE},
      VkDescriptorBufferInfo{frameUserData.gpuCullingUniformBuffer.buffer, 0, sizeof(shaderio::GPUCullingUniforms)},
  }};

  std::array<VkDescriptorImageInfo, shaderio::LDepthPyramidMaxMips> pyramidMipInfos{};
  for(uint32_t i = 0; i < static_cast<uint32_t>(pyramidMipInfos.size()); ++i)
  {
    pyramidMipInfos[i] = VkDescriptorImageInfo{
        .sampler     = VK_NULL_HANDLE,
        .imageView   = sceneResources.getDepthPyramidMipView(std::min(i, mipCount - 1u)),
        .imageLayout = VK_IMAGE_LAYOUT_GENERAL,
    };
  }

  const std::array<VkWriteDescriptorSet, 6> writes{{
      VkWriteDescriptorSet{
          .sType           = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET,
          .dstSet          = m_device.gpuCullingDescriptorSets[frameIndex],
          .dstBinding      = 0,
          .descriptorCount = 1,
          .descriptorType  = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER,
          .pBufferInfo     = &bufferInfos[0],
      },
      VkWriteDescriptorSet{
          .sType           = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET,
          .dstSet          = m_device.gpuCullingDescriptorSets[frameIndex],
          .dstBinding      = 1,
          .descriptorCount = 1,
          .descriptorType  = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER,
          .pBufferInfo     = &bufferInfos[1],
      },
      VkWriteDescriptorSet{
          .sType           = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET,
          .dstSet          = m_device.gpuCullingDescriptorSets[frameIndex],
          .dstBinding      = 2,
          .descriptorCount = 1,
          .descriptorType  = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER,
          .pBufferInfo     = &bufferInfos[2],
      },
      VkWriteDescriptorSet{
          .sType           = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET,
          .dstSet          = m_device.gpuCullingDescriptorSets[frameIndex],
          .dstBinding      = 3,
          .descriptorCount = 1,
          .descriptorType  = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER,
          .pBufferInfo     = &bufferInfos[3],
      },
      VkWriteDescriptorSet{
          .sType           = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET,
          .dstSet          = m_device.gpuCullingDescriptorSets[frameIndex],
          .dstBinding      = 4,
          .descriptorCount = static_cast<uint32_t>(pyramidMipInfos.size()),
          .descriptorType  = VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE,
          .pImageInfo      = pyramidMipInfos.data(),
      },
      VkWriteDescriptorSet{
          .sType           = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET,
          .dstSet          = m_device.gpuCullingDescriptorSets[frameIndex],
          .dstBinding      = 5,
          .descriptorCount = 1,
          .descriptorType  = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER,
          .pBufferInfo     = &bufferInfos[4],
      },
  }};
  vkUpdateDescriptorSets(nativeDevice, static_cast<uint32_t>(writes.size()), writes.data(), 0, nullptr);
}

void Renderer::createLightCoarseCullingResources()
{
  const VkDevice nativeDevice = fromNativeHandle<VkDevice>(m_device.device->getNativeDevice());
  const uint32_t frameCount = std::max<uint32_t>(1U, static_cast<uint32_t>(m_perFrame.frameUserData.size()));

  m_lightResources.init(*m_device.device, m_device.allocator, LightResources::CreateInfo{
      .maxPointLights = 256,
      .maxSpotLights  = 128,
      .frameCount     = frameCount,
  });

  const std::array<VkDescriptorSetLayoutBinding, 5> bindings{{
      VkDescriptorSetLayoutBinding{0, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 1, VK_SHADER_STAGE_COMPUTE_BIT, nullptr},
      VkDescriptorSetLayoutBinding{1, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 1, VK_SHADER_STAGE_COMPUTE_BIT, nullptr},
      VkDescriptorSetLayoutBinding{2, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 1, VK_SHADER_STAGE_COMPUTE_BIT, nullptr},
      VkDescriptorSetLayoutBinding{3, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 1, VK_SHADER_STAGE_COMPUTE_BIT, nullptr},
      VkDescriptorSetLayoutBinding{4, VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER, 1, VK_SHADER_STAGE_COMPUTE_BIT, nullptr},
  }};

  const VkDescriptorSetLayoutCreateInfo setLayoutInfo{
      .sType        = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO,
      .bindingCount = static_cast<uint32_t>(bindings.size()),
      .pBindings    = bindings.data(),
  };
  VK_CHECK(vkCreateDescriptorSetLayout(nativeDevice, &setLayoutInfo, nullptr, &m_device.lightCoarseCullingSetLayout));
  DBG_VK_NAME(m_device.lightCoarseCullingSetLayout);

  std::vector<VkDescriptorSetLayout> setLayouts(frameCount, m_device.lightCoarseCullingSetLayout);
  m_device.lightCoarseCullingDescriptorSets.resize(frameCount, VK_NULL_HANDLE);
  const VkDescriptorSetAllocateInfo allocInfo{
      .sType              = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO,
      .descriptorPool     = m_device.descriptorPool,
      .descriptorSetCount = frameCount,
      .pSetLayouts        = setLayouts.data(),
  };
  VK_CHECK(vkAllocateDescriptorSets(nativeDevice, &allocInfo, m_device.lightCoarseCullingDescriptorSets.data()));

  for(uint32_t frameIndex = 0; frameIndex < frameCount; ++frameIndex)
  {
    const std::array<VkDescriptorBufferInfo, 5> bufferInfos{{
        VkDescriptorBufferInfo{m_lightResources.getPointLightBuffer(frameIndex), 0, VK_WHOLE_SIZE},
        VkDescriptorBufferInfo{m_lightResources.getSpotLightBuffer(frameIndex), 0, VK_WHOLE_SIZE},
        VkDescriptorBufferInfo{m_lightResources.getPointCoarseBoundsBuffer(frameIndex), 0, VK_WHOLE_SIZE},
        VkDescriptorBufferInfo{m_lightResources.getSpotCoarseBoundsBuffer(frameIndex), 0, VK_WHOLE_SIZE},
        VkDescriptorBufferInfo{m_lightResources.getCoarseCullingUniformBuffer(frameIndex), 0, sizeof(shaderio::LightCoarseCullingUniforms)},
    }};

    std::array<VkWriteDescriptorSet, 5> writes{};
    for(uint32_t binding = 0; binding < static_cast<uint32_t>(writes.size()); ++binding)
    {
      writes[binding] = VkWriteDescriptorSet{
          .sType           = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET,
          .dstSet          = m_device.lightCoarseCullingDescriptorSets[frameIndex],
          .dstBinding      = binding,
          .dstArrayElement = 0,
          .descriptorCount = 1,
          .descriptorType  = binding == 4 ? VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER : VK_DESCRIPTOR_TYPE_STORAGE_BUFFER,
          .pBufferInfo     = &bufferInfos[binding],
      };
    }
    vkUpdateDescriptorSets(nativeDevice, static_cast<uint32_t>(writes.size()), writes.data(), 0, nullptr);
  }

  const VkPipelineLayoutCreateInfo pipelineLayoutInfo{
      .sType                  = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO,
      .setLayoutCount         = 1,
      .pSetLayouts            = &m_device.lightCoarseCullingSetLayout,
      .pushConstantRangeCount = 0,
      .pPushConstantRanges    = nullptr,
  };
  VK_CHECK(vkCreatePipelineLayout(nativeDevice, &pipelineLayoutInfo, nullptr, &m_device.lightCoarseCullingPipelineLayout));
  DBG_VK_NAME(m_device.lightCoarseCullingPipelineLayout);
}

void Renderer::createDepthPyramidResources()
{
  const VkDevice nativeDevice = fromNativeHandle<VkDevice>(m_device.device->getNativeDevice());

  m_depthPyramidUniformBuffer = createBuffer(nativeDevice,
                                             m_device.allocator,
                                             sizeof(shaderio::DepthPyramidUniforms),
                                             VK_BUFFER_USAGE_2_UNIFORM_BUFFER_BIT_KHR,
                                             VMA_MEMORY_USAGE_CPU_TO_GPU,
                                             VMA_ALLOCATION_CREATE_HOST_ACCESS_SEQUENTIAL_WRITE_BIT);
  DBG_VK_NAME(m_depthPyramidUniformBuffer.buffer);

  const std::array<VkDescriptorSetLayoutBinding, 3> bindings{{
      VkDescriptorSetLayoutBinding{0, VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE, 1, VK_SHADER_STAGE_COMPUTE_BIT, nullptr},
      VkDescriptorSetLayoutBinding{1, VK_DESCRIPTOR_TYPE_STORAGE_IMAGE, shaderio::LDepthPyramidMaxMips, VK_SHADER_STAGE_COMPUTE_BIT, nullptr},
      VkDescriptorSetLayoutBinding{2, VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER, 1, VK_SHADER_STAGE_COMPUTE_BIT, nullptr},
  }};

  const VkDescriptorSetLayoutCreateInfo setLayoutInfo{
      .sType        = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO,
      .bindingCount = static_cast<uint32_t>(bindings.size()),
      .pBindings    = bindings.data(),
  };
  VK_CHECK(vkCreateDescriptorSetLayout(nativeDevice, &setLayoutInfo, nullptr, &m_device.depthPyramidSetLayout));
  DBG_VK_NAME(m_device.depthPyramidSetLayout);

  const VkDescriptorSetAllocateInfo allocInfo{
      .sType              = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO,
      .descriptorPool     = m_device.descriptorPool,
      .descriptorSetCount = 1,
      .pSetLayouts        = &m_device.depthPyramidSetLayout,
  };
  VK_CHECK(vkAllocateDescriptorSets(nativeDevice, &allocInfo, &m_device.depthPyramidDescriptorSet));
  DBG_VK_NAME(m_device.depthPyramidDescriptorSet);

  const VkPipelineLayoutCreateInfo pipelineLayoutInfo{
      .sType                  = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO,
      .setLayoutCount         = 1,
      .pSetLayouts            = &m_device.depthPyramidSetLayout,
      .pushConstantRangeCount = 0,
      .pPushConstantRanges    = nullptr,
  };
  VK_CHECK(vkCreatePipelineLayout(nativeDevice, &pipelineLayoutInfo, nullptr, &m_device.depthPyramidPipelineLayout));
  DBG_VK_NAME(m_device.depthPyramidPipelineLayout);

  updateDepthPyramidDescriptorSet();
}

void Renderer::updateDepthPyramidDescriptorSet()
{
  if(m_device.depthPyramidDescriptorSet == VK_NULL_HANDLE)
  {
    return;
  }

  SceneResources& sceneResources = m_swapchainDependent.sceneResources;
  if(sceneResources.getDepthImageView() == VK_NULL_HANDLE || sceneResources.getDepthPyramidMipCount() == 0)
  {
    return;
  }

  const VkDescriptorImageInfo sourceDepthInfo{
      .sampler     = VK_NULL_HANDLE,
      .imageView   = sceneResources.getDepthImageView(),
      .imageLayout = VK_IMAGE_LAYOUT_GENERAL,
  };

  std::array<VkDescriptorImageInfo, shaderio::LDepthPyramidMaxMips> pyramidMipInfos{};
  const uint32_t mipCount = sceneResources.getDepthPyramidMipCount();
  for(uint32_t i = 0; i < static_cast<uint32_t>(pyramidMipInfos.size()); ++i)
  {
    pyramidMipInfos[i] = VkDescriptorImageInfo{
        .sampler     = VK_NULL_HANDLE,
        .imageView   = sceneResources.getDepthPyramidMipView(std::min(i, mipCount - 1u)),
        .imageLayout = VK_IMAGE_LAYOUT_GENERAL,
    };
  }

  const VkDescriptorBufferInfo uniformInfo{
      .buffer = m_depthPyramidUniformBuffer.buffer,
      .offset = 0,
      .range  = sizeof(shaderio::DepthPyramidUniforms),
  };

  const std::array<VkWriteDescriptorSet, 3> writes{{
      VkWriteDescriptorSet{
          .sType           = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET,
          .dstSet          = m_device.depthPyramidDescriptorSet,
          .dstBinding      = 0,
          .dstArrayElement = 0,
          .descriptorCount = 1,
          .descriptorType  = VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE,
          .pImageInfo      = &sourceDepthInfo,
      },
      VkWriteDescriptorSet{
          .sType           = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET,
          .dstSet          = m_device.depthPyramidDescriptorSet,
          .dstBinding      = 1,
          .dstArrayElement = 0,
          .descriptorCount = static_cast<uint32_t>(pyramidMipInfos.size()),
          .descriptorType  = VK_DESCRIPTOR_TYPE_STORAGE_IMAGE,
          .pImageInfo      = pyramidMipInfos.data(),
      },
      VkWriteDescriptorSet{
          .sType           = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET,
          .dstSet          = m_device.depthPyramidDescriptorSet,
          .dstBinding      = 2,
          .dstArrayElement = 0,
          .descriptorCount = 1,
          .descriptorType  = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER,
          .pBufferInfo     = &uniformInfo,
      },
  }};

  vkUpdateDescriptorSets(fromNativeHandle<VkDevice>(m_device.device->getNativeDevice()),
                         static_cast<uint32_t>(writes.size()), writes.data(), 0, nullptr);
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
  updateDepthPyramidDescriptorSet();
  for(uint32_t frameIndex = 0; frameIndex < m_perFrame.frameUserData.size(); ++frameIndex)
  {
    updateGPUCullingDescriptorSet(frameIndex);
  }
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

  // Update CSM cascade matrices based on current camera and light direction
  if(params.cameraUniforms != nullptr)
  {
    m_csmShadowResources.updateCascadeMatrices(*params.cameraUniforms, params.lightSettings.direction);
  }

  m_frameLightingState = buildFrameLightingState(params);
  ensureTestPointLights(params);
  buildDebugDrawList(params);
  updateLightingUniformBuffer(currentFrameIndex, shaderio::LightingUniforms{m_frameLightingState.lightParams});
  updateLightCullingUniformBuffer(currentFrameIndex, buildLightCullingUniforms(params));
  updateGPUCullingBuffers(currentFrameIndex, params);

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
  m_passExecutor.bindBuffer({
      .handle       = kPassPointLightBufferHandle,
      .nativeBuffer = reinterpret_cast<uint64_t>(m_lightResources.getPointLightBuffer(currentFrameIndex)),
  });
  m_passExecutor.bindBuffer({
      .handle       = kPassSpotLightBufferHandle,
      .nativeBuffer = reinterpret_cast<uint64_t>(m_lightResources.getSpotLightBuffer(currentFrameIndex)),
  });
  m_passExecutor.bindBuffer({
      .handle       = kPassPointLightCoarseBoundsHandle,
      .nativeBuffer = reinterpret_cast<uint64_t>(m_lightResources.getPointCoarseBoundsBuffer(currentFrameIndex)),
  });
  m_passExecutor.bindBuffer({
      .handle       = kPassSpotLightCoarseBoundsHandle,
      .nativeBuffer = reinterpret_cast<uint64_t>(m_lightResources.getSpotCoarseBoundsBuffer(currentFrameIndex)),
  });
  m_passExecutor.bindBuffer({
      .handle       = kPassLightCoarseCullingUniformHandle,
      .nativeBuffer = reinterpret_cast<uint64_t>(m_lightResources.getCoarseCullingUniformBuffer(currentFrameIndex)),
  });
  m_passExecutor.bindBuffer({
      .handle       = kPassGPUCullObjectBufferHandle,
      .nativeBuffer = reinterpret_cast<uint64_t>(frameUserData.gpuCullingObjectBuffer.buffer),
  });
  m_passExecutor.bindBuffer({
      .handle       = kPassGPUCullIndirectBufferHandle,
      .nativeBuffer = reinterpret_cast<uint64_t>(frameUserData.gpuCullingIndirectBuffer.buffer),
  });
  m_passExecutor.bindBuffer({
      .handle       = kPassGPUCullStatsBufferHandle,
      .nativeBuffer = reinterpret_cast<uint64_t>(frameUserData.gpuCullingStatsBuffer.buffer),
  });
  m_passExecutor.bindBuffer({
      .handle       = kPassGPUCullUniformBufferHandle,
      .nativeBuffer = reinterpret_cast<uint64_t>(frameUserData.gpuCullingUniformBuffer.buffer),
  });
  m_passExecutor.bindBuffer({
      .handle       = kPassGPUCullResultBufferHandle,
      .nativeBuffer = reinterpret_cast<uint64_t>(frameUserData.gpuCullingResultBuffer.buffer),
  });
  m_passExecutor.bindTexture({
      .handle       = kPassGBuffer0Handle,
      .nativeImage  = reinterpret_cast<uint64_t>(m_swapchainDependent.sceneResources.getColorImage(0)),
      .aspect       = rhi::TextureAspect::color,
      .initialState = rhi::ResourceState::general,
      .isSwapchain  = false,
  });
  m_passExecutor.bindTexture({
      .handle       = kPassGBuffer1Handle,
      .nativeImage  = reinterpret_cast<uint64_t>(m_swapchainDependent.sceneResources.getColorImage(1)),
      .aspect       = rhi::TextureAspect::color,
      .initialState = rhi::ResourceState::general,
      .isSwapchain  = false,
  });
  m_passExecutor.bindTexture({
      .handle       = kPassGBuffer2Handle,
      .nativeImage  = reinterpret_cast<uint64_t>(m_swapchainDependent.sceneResources.getColorImage(2)),
      .aspect       = rhi::TextureAspect::color,
      .initialState = rhi::ResourceState::general,
      .isSwapchain  = false,
  });
  m_passExecutor.bindTexture({
      .handle       = kPassSceneDepthHandle,
      .nativeImage  = reinterpret_cast<uint64_t>(m_swapchainDependent.sceneResources.getDepthImage()),
      .aspect       = rhi::TextureAspect::depth,
      .initialState = rhi::ResourceState::general,
      .isSwapchain  = false,
  });
  m_passExecutor.bindTexture({
      .handle       = kPassShadowHandle,
      .nativeImage  = reinterpret_cast<uint64_t>(m_swapchainDependent.sceneResources.getShadowMapImage()),
      .aspect       = rhi::TextureAspect::depth,
      .initialState = rhi::ResourceState::general,
      .isSwapchain  = false,
  });
  m_passExecutor.bindTexture({
      .handle       = kPassCSMShadowHandle,
      .nativeImage  = reinterpret_cast<uint64_t>(m_csmShadowResources.getCascadeImage()),
      .aspect       = rhi::TextureAspect::depth,
      .initialState = rhi::ResourceState::general,
      .isSwapchain  = false,
  });
  m_passExecutor.bindTexture({
      .handle       = kPassDepthPyramidHandle,
      .nativeImage  = reinterpret_cast<uint64_t>(m_swapchainDependent.sceneResources.getDepthPyramidImage()),
      .aspect       = rhi::TextureAspect::color,
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
  resetPassGpuProfileQueries(cmd, currentFrameIndex);
  m_passExecutor.execute(context, &m_passProfilingHooks);
  if(currentFrameIndex < m_passGpuProfile.frames.size())
  {
    m_passGpuProfile.frames[currentFrameIndex].hasRecordedQueries = true;
  }

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
      .imageLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL,
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

void Renderer::executeLightCoarseCullingPass(rhi::CommandList& cmd, const RenderParams& params)
{
  if(params.cameraUniforms == nullptr || m_device.lightCoarseCullingPipelineLayout == VK_NULL_HANDLE)
  {
    return;
  }

  const uint32_t currentFrameIndex = m_perFrame.frameContext->getCurrentFrameIndex();
  if(currentFrameIndex >= m_device.lightCoarseCullingDescriptorSets.size())
  {
    return;
  }

  const VkDescriptorSet descriptorSet = m_device.lightCoarseCullingDescriptorSets[currentFrameIndex];
  if(descriptorSet == VK_NULL_HANDLE)
  {
    return;
  }

  m_lightResources.updatePointLights(currentFrameIndex, m_testPointLights);
  m_lightResources.updateSpotLights(currentFrameIndex, m_testSpotLights);

  const VkCommandBuffer vkCmd = rhi::vulkan::getNativeCommandBuffer(cmd);
  vkCmdBindDescriptorSets(vkCmd, VK_PIPELINE_BIND_POINT_COMPUTE, m_device.lightCoarseCullingPipelineLayout,
                          0, 1, &descriptorSet, 0, nullptr);

  const shaderio::CameraUniforms& camera = *params.cameraUniforms;
  const glm::mat4 inverseView = glm::inverse(camera.view);
  const VkExtent2D extent = m_swapchainDependent.sceneResources.getSize();
  const uint32_t tileCountX = (extent.width + shaderio::LTileSizeX - 1u) / shaderio::LTileSizeX;
  const uint32_t tileCountY = (extent.height + shaderio::LTileSizeY - 1u) / shaderio::LTileSizeY;
  const uint32_t pointLightCount =
      std::min<uint32_t>(static_cast<uint32_t>(m_testPointLights.size()), m_lightResources.getMaxPointLights());
  const uint32_t spotLightCount =
      std::min<uint32_t>(static_cast<uint32_t>(m_testSpotLights.size()), m_lightResources.getMaxSpotLights());

  const shaderio::LightCoarseCullingUniforms coarseCullingUniforms{
      .viewProjection = camera.viewProjection,
      .cameraRight = glm::vec4(glm::normalize(glm::vec3(inverseView[0])), 0.0f),
      .cameraUp = glm::vec4(glm::normalize(glm::vec3(inverseView[1])), 0.0f),
      .screenTileInfo = glm::vec4(extent.width, extent.height, tileCountX, tileCountY),
      .lightCountInfo = glm::vec4(pointLightCount, spotLightCount, 0.0f, 0.0f),
      .debugInfo = glm::vec4(params.debugOptions.showLightCoarseCullingHeatmap ? 1.0f : 0.0f, 0.0f, 0.0f, 0.0f),
  };
  m_lightResources.updateCoarseCullingUniforms(currentFrameIndex, coarseCullingUniforms);

  const auto dispatchLightKernel = [&](PipelineHandle pipelineHandle, uint32_t lightCount) {
    if(pipelineHandle.isNull() || lightCount == 0)
    {
      return;
    }

    vkCmdBindPipeline(vkCmd, VK_PIPELINE_BIND_POINT_COMPUTE,
                      reinterpret_cast<VkPipeline>(getPipelineOpaque(
                          pipelineHandle, static_cast<uint32_t>(VK_PIPELINE_BIND_POINT_COMPUTE))));
    vkCmdDispatch(vkCmd, (lightCount + kLightCoarseCullingThreadCount - 1u) / kLightCoarseCullingThreadCount, 1, 1);
  };

  dispatchLightKernel(m_pointLightCoarseCullingPipeline, pointLightCount);
  dispatchLightKernel(m_spotLightCoarseCullingPipeline, spotLightCount);

  const VkMemoryBarrier2 memoryBarrier{
      .sType         = VK_STRUCTURE_TYPE_MEMORY_BARRIER_2,
      .srcStageMask  = VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT,
      .srcAccessMask = VK_ACCESS_2_SHADER_WRITE_BIT,
      .dstStageMask  = VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT | VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT,
      .dstAccessMask = VK_ACCESS_2_SHADER_READ_BIT,
  };
  const VkDependencyInfo dependencyInfo{
      .sType              = VK_STRUCTURE_TYPE_DEPENDENCY_INFO,
      .memoryBarrierCount = 1,
      .pMemoryBarriers    = &memoryBarrier,
  };
  vkCmdPipelineBarrier2(vkCmd, &dependencyInfo);
}

void Renderer::executeDepthPyramidPass(rhi::CommandList& cmd, const RenderParams&)
{
  if(m_depthPyramidPipeline.isNull() || m_device.depthPyramidPipelineLayout == VK_NULL_HANDLE
     || m_device.depthPyramidDescriptorSet == VK_NULL_HANDLE)
  {
    return;
  }

  SceneResources& sceneResources = m_swapchainDependent.sceneResources;
  const VkExtent2D sourceExtent = sceneResources.getSize();
  const VkExtent2D pyramidExtent = sceneResources.getDepthPyramidExtent();
  const uint32_t mipCount = std::min<uint32_t>(sceneResources.getDepthPyramidMipCount(), shaderio::LDepthPyramidMaxMips);
  if(sourceExtent.width == 0 || sourceExtent.height == 0 || pyramidExtent.width == 0 || pyramidExtent.height == 0 || mipCount == 0)
  {
    return;
  }

  const shaderio::DepthPyramidUniforms uniforms{
      .sourceWidth   = sourceExtent.width,
      .sourceHeight  = sourceExtent.height,
      ._padding0     = 0u,
      ._padding1     = 0u,
      .pyramidWidth  = pyramidExtent.width,
      .pyramidHeight = pyramidExtent.height,
      .mipCount      = mipCount,
      ._padding2     = 0u,
  };

  void* mappedData = nullptr;
  VK_CHECK(vmaMapMemory(m_device.allocator, m_depthPyramidUniformBuffer.allocation, &mappedData));
  std::memcpy(mappedData, &uniforms, sizeof(uniforms));
  VK_CHECK(vmaFlushAllocation(m_device.allocator, m_depthPyramidUniformBuffer.allocation, 0, sizeof(uniforms)));
  vmaUnmapMemory(m_device.allocator, m_depthPyramidUniformBuffer.allocation);

  const VkCommandBuffer vkCmd = rhi::vulkan::getNativeCommandBuffer(cmd);
  vkCmdBindPipeline(vkCmd, VK_PIPELINE_BIND_POINT_COMPUTE,
                    reinterpret_cast<VkPipeline>(getPipelineOpaque(m_depthPyramidPipeline,
                                                                    static_cast<uint32_t>(VK_PIPELINE_BIND_POINT_COMPUTE))));
  vkCmdBindDescriptorSets(vkCmd, VK_PIPELINE_BIND_POINT_COMPUTE, m_device.depthPyramidPipelineLayout,
                          0, 1, &m_device.depthPyramidDescriptorSet, 0, nullptr);
  vkCmdDispatch(vkCmd, (pyramidExtent.width + 7u) / 8u, (pyramidExtent.height + 7u) / 8u, 1u);
}

void Renderer::executeGPUCullingPass(rhi::CommandList& cmd, const RenderParams& params)
{
  if(params.cameraUniforms == nullptr || params.gltfModel == nullptr || m_gpuCullingPipeline.isNull()
     || m_device.gpuCullingPipelineLayout == VK_NULL_HANDLE)
  {
    return;
  }

  const uint32_t currentFrameIndex = m_perFrame.frameContext->getCurrentFrameIndex();
  if(currentFrameIndex >= m_device.gpuCullingDescriptorSets.size())
  {
    return;
  }

  PerFrameResources::FrameUserData& frameUserData = m_perFrame.frameUserData[currentFrameIndex];
  const uint32_t objectCount = static_cast<uint32_t>(params.gltfModel->meshes.size());
  if(objectCount == 0 || frameUserData.gpuCullingIndirectBuffer.buffer == VK_NULL_HANDLE)
  {
    return;
  }

  const VkDescriptorSet descriptorSet = m_device.gpuCullingDescriptorSets[currentFrameIndex];
  if(descriptorSet == VK_NULL_HANDLE)
  {
    return;
  }

  const VkCommandBuffer vkCmd = rhi::vulkan::getNativeCommandBuffer(cmd);
  vkCmdBindPipeline(vkCmd,
                    VK_PIPELINE_BIND_POINT_COMPUTE,
                    reinterpret_cast<VkPipeline>(getPipelineOpaque(m_gpuCullingPipeline,
                                                                    static_cast<uint32_t>(VK_PIPELINE_BIND_POINT_COMPUTE))));
  vkCmdBindDescriptorSets(vkCmd, VK_PIPELINE_BIND_POINT_COMPUTE, m_device.gpuCullingPipelineLayout, 0, 1, &descriptorSet, 0,
                          nullptr);
  vkCmdDispatch(vkCmd, (objectCount + shaderio::LGPUCullingThreadCount - 1u) / shaderio::LGPUCullingThreadCount, 1u, 1u);

  const VkBufferMemoryBarrier2 indirectBarrier{
      .sType               = VK_STRUCTURE_TYPE_BUFFER_MEMORY_BARRIER_2,
      .srcStageMask        = VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT,
      .srcAccessMask       = VK_ACCESS_2_SHADER_WRITE_BIT,
      .dstStageMask        = VK_PIPELINE_STAGE_2_DRAW_INDIRECT_BIT,
      .dstAccessMask       = VK_ACCESS_2_INDIRECT_COMMAND_READ_BIT,
      .srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
      .dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
      .buffer              = frameUserData.gpuCullingIndirectBuffer.buffer,
      .offset              = 0,
      .size                = VK_WHOLE_SIZE,
  };
  const VkDependencyInfo dependencyInfo{
      .sType                    = VK_STRUCTURE_TYPE_DEPENDENCY_INFO,
      .bufferMemoryBarrierCount = 1,
      .pBufferMemoryBarriers    = &indirectBarrier,
  };
  vkCmdPipelineBarrier2(vkCmd, &dependencyInfo);
}

void Renderer::DebugDrawList::addLine(const glm::vec3& a, const glm::vec3& b, const glm::vec4& color)
{
  vertices.push_back(shaderio::DebugLineVertex{a, color});
  vertices.push_back(shaderio::DebugLineVertex{b, color});
}

void Renderer::DebugDrawList::addAabb(const Aabb& bounds, const glm::vec4& color)
{
  if(!bounds.valid)
  {
    return;
  }

  const std::array<glm::vec3, 8> corners{{
      {bounds.min.x, bounds.min.y, bounds.min.z},
      {bounds.max.x, bounds.min.y, bounds.min.z},
      {bounds.min.x, bounds.max.y, bounds.min.z},
      {bounds.max.x, bounds.max.y, bounds.min.z},
      {bounds.min.x, bounds.min.y, bounds.max.z},
      {bounds.max.x, bounds.min.y, bounds.max.z},
      {bounds.min.x, bounds.max.y, bounds.max.z},
      {bounds.max.x, bounds.max.y, bounds.max.z},
  }};
  addFrustum(corners, color);
}

void Renderer::DebugDrawList::addFrustum(const std::array<glm::vec3, 8>& corners, const glm::vec4& color)
{
  static constexpr std::array<std::pair<uint32_t, uint32_t>, 12> kEdges{{
      {0, 1}, {1, 3}, {3, 2}, {2, 0},
      {4, 5}, {5, 7}, {7, 6}, {6, 4},
      {0, 4}, {1, 5}, {2, 6}, {3, 7},
  }};

  for(const auto& [a, b] : kEdges)
  {
    addLine(corners[a], corners[b], color);
  }
}

void Renderer::DebugDrawList::addSphere(const glm::vec3& center, float radius, const glm::vec4& color, uint32_t segments)
{
  if(radius <= 0.0f || segments < 3)
  {
    return;
  }

  const float delta = 6.28318530718f / static_cast<float>(segments);
  for(uint32_t i = 0; i < segments; ++i)
  {
    const float angle0 = delta * static_cast<float>(i);
    const float angle1 = delta * static_cast<float>(i + 1);
    addLine(center + glm::vec3(std::cos(angle0) * radius, 0.0f, std::sin(angle0) * radius),
            center + glm::vec3(std::cos(angle1) * radius, 0.0f, std::sin(angle1) * radius), color);
    addLine(center + glm::vec3(0.0f, std::cos(angle0) * radius, std::sin(angle0) * radius),
            center + glm::vec3(0.0f, std::cos(angle1) * radius, std::sin(angle1) * radius), color);
    addLine(center + glm::vec3(std::cos(angle0) * radius, std::sin(angle0) * radius, 0.0f),
            center + glm::vec3(std::cos(angle1) * radius, std::sin(angle1) * radius, 0.0f), color);
  }
}

void Renderer::DebugDrawList::addArrow(const glm::vec3& origin, const glm::vec3& direction, float length, const glm::vec4& color)
{
  if(length <= 0.0f)
  {
    return;
  }

  const glm::vec3 dir = glm::normalize(direction);
  const glm::vec3 end = origin + dir * length;
  addLine(origin, end, color);

  const glm::vec3 reference = std::abs(dir.y) > 0.95f ? glm::vec3(1.0f, 0.0f, 0.0f) : glm::vec3(0.0f, 1.0f, 0.0f);
  const glm::vec3 tangent = glm::normalize(glm::cross(dir, reference));
  const glm::vec3 bitangent = glm::normalize(glm::cross(dir, tangent));
  const float headLength = length * 0.15f;
  addLine(end, end - dir * headLength + tangent * headLength * 0.5f, color);
  addLine(end, end - dir * headLength - tangent * headLength * 0.5f, color);
  addLine(end, end - dir * headLength + bitangent * headLength * 0.5f, color);
  addLine(end, end - dir * headLength - bitangent * headLength * 0.5f, color);
}

Renderer::Aabb Renderer::computeSceneBounds(const GltfUploadResult* gltfModel) const
{
  Aabb bounds{};
  if(gltfModel == nullptr || gltfModel->meshes.empty())
  {
    return bounds;
  }

  bounds.min = glm::vec3(std::numeric_limits<float>::max());
  bounds.max = glm::vec3(std::numeric_limits<float>::lowest());
  bounds.valid = false;

  for(const MeshHandle meshHandle : gltfModel->meshes)
  {
    const MeshRecord* mesh = m_meshPool.tryGet(meshHandle);
    if(mesh == nullptr)
    {
      continue;
    }

    bounds.min = glm::min(bounds.min, mesh->worldBoundsMin);
    bounds.max = glm::max(bounds.max, mesh->worldBoundsMax);
    bounds.valid = true;
  }

  return bounds;
}

void Renderer::ensureTestPointLights(const RenderParams& params)
{
  glm::vec3 minBounds(-12.0f, 0.5f, -12.0f);
  glm::vec3 maxBounds(12.0f, 6.0f, 12.0f);
  if(m_frameLightingState.sceneBounds.valid)
  {
    minBounds = m_frameLightingState.sceneBounds.min;
    maxBounds = m_frameLightingState.sceneBounds.max;
    const glm::vec3 size = glm::max(maxBounds - minBounds, glm::vec3(1.0f));
    minBounds += size * 0.08f;
    maxBounds -= size * 0.08f;
  }

  const glm::vec3 sceneMinBounds = minBounds;
  const glm::vec3 sceneMaxBounds = maxBounds;
  const glm::vec3 sceneBoundsSize = glm::max(sceneMaxBounds - sceneMinBounds, glm::vec3(1.0f));
  const float sceneBoundsEpsilon = std::max(0.1f, glm::length(sceneBoundsSize) * 0.01f);
  const bool sceneBoundsChanged =
      !m_testPointLightSceneBounds.valid ||
      glm::length(m_testPointLightSceneBounds.min - sceneMinBounds) > sceneBoundsEpsilon ||
      glm::length(m_testPointLightSceneBounds.max - sceneMaxBounds) > sceneBoundsEpsilon;

  if(sceneBoundsChanged)
  {
    m_testPointLights.clear();
    m_testPointLightMotions.clear();
    m_testPointLightSceneBounds.min = sceneMinBounds;
    m_testPointLightSceneBounds.max = sceneMaxBounds;
    m_testPointLightSceneBounds.valid = true;
  }

  if(m_frameLightingState.sceneBounds.valid && params.cameraUniforms != nullptr)
  {
    const glm::mat4 inverseView = glm::inverse(params.cameraUniforms->view);
    const glm::vec3 cameraPosition = params.cameraUniforms->cameraPosition;
    const glm::vec3 cameraForward = -glm::normalize(glm::vec3(inverseView[2]));
    const float sceneDiagonal = glm::length(sceneBoundsSize);
    const float focusDistance = glm::clamp(sceneDiagonal * 0.08f, 8.0f, 35.0f);
    const float fieldRadius = glm::clamp(sceneDiagonal * 0.16f, 14.0f, 70.0f);
    const glm::vec3 focus = glm::clamp(cameraPosition + cameraForward * focusDistance, sceneMinBounds, sceneMaxBounds);
    const glm::vec3 halfExtent(fieldRadius, std::max(6.0f, fieldRadius * 0.45f), fieldRadius);
    const glm::vec3 localMin = glm::max(sceneMinBounds, focus - halfExtent);
    const glm::vec3 localMax = glm::min(sceneMaxBounds, focus + halfExtent);
    if(glm::all(glm::greaterThan(localMax - localMin, glm::vec3(0.25f))))
    {
      minBounds = localMin;
      maxBounds = localMax;
    }
  }

  std::mt19937 rng(0x5EED1234u);
  std::uniform_real_distribution<float> unit(0.0f, 1.0f);
  std::uniform_real_distribution<float> phaseDistribution(0.0f, 6.28318530718f);
  std::uniform_real_distribution<float> speedDistribution(0.25f, 1.2f);
  std::uniform_real_distribution<float> amplitudeDistribution(0.06f, 0.18f);

  const glm::vec3 boundsSize = glm::max(maxBounds - minBounds, glm::vec3(1.0f));
  if(m_testPointLights.empty())
  {
    m_testPointLights.reserve(kTestPointLightCount);
    m_testPointLightMotions.reserve(kTestPointLightCount);
    for(uint32_t i = 0; i < kTestPointLightCount; ++i)
    {
      TestPointLightMotion motion{};
      motion.baseT = glm::vec3(unit(rng), unit(rng), unit(rng));
      motion.phase = glm::vec3(phaseDistribution(rng), phaseDistribution(rng), phaseDistribution(rng));
      motion.speed = glm::vec3(speedDistribution(rng), speedDistribution(rng), speedDistribution(rng));
      motion.amplitude = boundsSize * glm::vec3(amplitudeDistribution(rng), amplitudeDistribution(rng), amplitudeDistribution(rng));
      motion.radiusT = unit(rng);
      motion.intensityT = unit(rng);
      m_testPointLightMotions.push_back(motion);

      shaderio::LightData light{};
      light.positionOrDirection = glm::mix(minBounds, maxBounds, motion.baseT);
      light.color = glm::vec3(0.35f + unit(rng) * 0.65f,
                              0.35f + unit(rng) * 0.65f,
                              0.35f + unit(rng) * 0.65f);
      light.spotDirection = glm::vec3(0.0f, -1.0f, 0.0f);
      light.spotInnerAngle = 0.0f;
      light.lightType = shaderio::LLightTypePoint;
      light.spotOuterAngle = 0.0f;
      m_testPointLights.push_back(light);
    }

    m_testSpotLights.clear();
    LOGI("Generated %zu animated random point lights for LightCoarseCulling", m_testPointLights.size());
  }

  const float time = params.timeSeconds;
  const float maxRadius = std::max(0.1f, params.debugOptions.pointLightMaxRadius);
  const float minRadius = std::min(maxRadius, std::max(0.25f, maxRadius * 0.35f));
  const float intensityScale = std::max(0.0f, params.debugOptions.pointLightIntensityScale);
  for(size_t i = 0; i < m_testPointLights.size() && i < m_testPointLightMotions.size(); ++i)
  {
    const TestPointLightMotion& motion = m_testPointLightMotions[i];
    const glm::vec3 basePosition = glm::mix(minBounds, maxBounds, motion.baseT);
    const glm::vec3 offset(
        std::sin(time * motion.speed.x + motion.phase.x) * motion.amplitude.x,
        std::sin(time * motion.speed.y + motion.phase.y) * motion.amplitude.y,
        std::cos(time * motion.speed.z + motion.phase.z) * motion.amplitude.z);
    m_testPointLights[i].positionOrDirection = glm::clamp(basePosition + offset, minBounds, maxBounds);
    m_testPointLights[i].range = glm::mix(minRadius, maxRadius, motion.radiusT);
    m_testPointLights[i].intensity = glm::mix(8.0f, 26.0f, motion.intensityT) * intensityScale;
  }
}

std::array<glm::vec3, 8> Renderer::computePerspectiveFrustumCorners(const shaderio::CameraUniforms& cameraUniforms,
                                                                    float nearDistance,
                                                                    float farDistance) const
{
  const glm::mat4 inverseView = glm::inverse(cameraUniforms.view);
  const glm::vec3 position = cameraUniforms.cameraPosition;
  const glm::vec3 right = glm::normalize(glm::vec3(inverseView[0]));
  const glm::vec3 up = glm::normalize(glm::vec3(inverseView[1]));
  const glm::vec3 forward = -glm::normalize(glm::vec3(inverseView[2]));

  const float tanHalfFovX = 1.0f / std::abs(cameraUniforms.projection[0][0]);
  const float tanHalfFovY = 1.0f / std::abs(cameraUniforms.projection[1][1]);

  const float nearHalfWidth = nearDistance * tanHalfFovX;
  const float nearHalfHeight = nearDistance * tanHalfFovY;
  const float farHalfWidth = farDistance * tanHalfFovX;
  const float farHalfHeight = farDistance * tanHalfFovY;

  const glm::vec3 nearCenter = position + forward * nearDistance;
  const glm::vec3 farCenter = position + forward * farDistance;

  return {{
      nearCenter - right * nearHalfWidth - up * nearHalfHeight,
      nearCenter + right * nearHalfWidth - up * nearHalfHeight,
      nearCenter - right * nearHalfWidth + up * nearHalfHeight,
      nearCenter + right * nearHalfWidth + up * nearHalfHeight,
      farCenter - right * farHalfWidth - up * farHalfHeight,
      farCenter + right * farHalfWidth - up * farHalfHeight,
      farCenter - right * farHalfWidth + up * farHalfHeight,
      farCenter + right * farHalfWidth + up * farHalfHeight,
  }};
}

std::array<glm::vec3, 8> Renderer::computeOrthoFrustumCorners(const glm::mat4& inverseViewProjection) const
{
  const clipspace::ProjectionConvention projectionConvention =
      clipspace::getProjectionConvention(clipspace::BackendConvention::vulkan);
  const std::array<glm::vec3, 8> clipCorners{{
      {-1.0f, -1.0f, projectionConvention.ndcNearZ},
      { 1.0f, -1.0f, projectionConvention.ndcNearZ},
      {-1.0f,  1.0f, projectionConvention.ndcNearZ},
      { 1.0f,  1.0f, projectionConvention.ndcNearZ},
      {-1.0f, -1.0f, projectionConvention.ndcFarZ},
      { 1.0f, -1.0f, projectionConvention.ndcFarZ},
      {-1.0f,  1.0f, projectionConvention.ndcFarZ},
      { 1.0f,  1.0f, projectionConvention.ndcFarZ},
  }};

  std::array<glm::vec3, 8> worldCorners{};
  for(size_t i = 0; i < clipCorners.size(); ++i)
  {
    const glm::vec4 world = inverseViewProjection * glm::vec4(clipCorners[i], 1.0f);
    worldCorners[i] = glm::vec3(world) / world.w;
  }
  return worldCorners;
}

shaderio::LightCullingUniforms Renderer::buildLightCullingUniforms(const RenderParams& params) const
{
  shaderio::LightCullingUniforms uniforms{};
  const VkExtent2D extent = m_swapchainDependent.sceneResources.getSize();

  shaderio::CameraUniforms camera{};
  if(params.cameraUniforms != nullptr)
  {
    camera = *params.cameraUniforms;
  }
  else
  {
    camera.view = glm::lookAt(glm::vec3(8.0f, 2.0f, 0.0f), glm::vec3(0.0f), glm::vec3(0.0f, 1.0f, 0.0f));
    camera.projection = clipspace::makePerspectiveProjection(
        glm::radians(45.0f), 16.0f / 9.0f, 0.1f, 100.0f,
        clipspace::getProjectionConvention(clipspace::BackendConvention::vulkan));
    camera.viewProjection = camera.projection * camera.view;
    camera.inverseViewProjection = glm::inverse(camera.viewProjection);
    camera.cameraPosition = glm::vec3(8.0f, 2.0f, 0.0f);
  }

  const clipspace::ProjectionConvention projectionConvention =
      clipspace::getProjectionConvention(clipspace::BackendConvention::vulkan);
  const float nearPlane = std::abs(clipspace::extractPerspectiveNearPlane(camera.projection, projectionConvention));
  const float farPlane = std::abs(clipspace::extractPerspectiveFarPlane(camera.projection, projectionConvention));

  uniforms.screenSizeAndClipPlanes = glm::vec4(
      static_cast<float>(extent.width),
      static_cast<float>(extent.height),
      nearPlane,
      farPlane);
  uniforms.viewMatrix = camera.view;
  uniforms.projectionMatrix = camera.projection;
  uniforms.invProjectionMatrix = glm::inverse(camera.projection);
  return uniforms;
}

shaderio::GPUCullingUniforms Renderer::buildGPUCullingUniforms(const RenderParams& params, uint32_t objectCount) const
{
  shaderio::GPUCullingUniforms uniforms{};
  const VkExtent2D screenExtent = m_swapchainDependent.sceneResources.getSize();
  const VkExtent2D pyramidExtent = m_swapchainDependent.sceneResources.getDepthPyramidExtent();
  const uint32_t mipCount =
      std::min<uint32_t>(m_swapchainDependent.sceneResources.getDepthPyramidMipCount(), shaderio::LDepthPyramidMaxMips);

  shaderio::CameraUniforms camera{};
  if(params.cameraUniforms != nullptr)
  {
    camera = *params.cameraUniforms;
  }
  else
  {
    camera.view = glm::lookAt(glm::vec3(8.0f, 2.0f, 0.0f), glm::vec3(0.0f), glm::vec3(0.0f, 1.0f, 0.0f));
    camera.projection = clipspace::makePerspectiveProjection(
        glm::radians(45.0f), 16.0f / 9.0f, 0.1f, 100.0f,
        clipspace::getProjectionConvention(clipspace::BackendConvention::vulkan));
    camera.viewProjection = camera.projection * camera.view;
    camera.inverseViewProjection = glm::inverse(camera.viewProjection);
    camera.cameraPosition = glm::vec3(8.0f, 2.0f, 0.0f);
  }

  const glm::mat4 inverseView = glm::inverse(camera.view);
  const auto frustumPlanes = extractFrustumPlanes(camera.viewProjection);

  uniforms.viewMatrix = camera.view;
  uniforms.projectionMatrix = camera.projection;
  uniforms.viewProjectionMatrix = camera.viewProjection;
  for(uint32_t planeIndex = 0; planeIndex < shaderio::LGPUCullingFrustumPlaneCount; ++planeIndex)
  {
    uniforms.frustumPlanes[planeIndex] = frustumPlanes[planeIndex];
  }
  uniforms.cameraRight = glm::vec4(glm::normalize(glm::vec3(inverseView[0])), 0.0f);
  uniforms.cameraUp = glm::vec4(glm::normalize(glm::vec3(inverseView[1])), 0.0f);
  uniforms.screenSizeAndPyramidSize =
      glm::vec4(static_cast<float>(screenExtent.width),
                static_cast<float>(screenExtent.height),
                static_cast<float>(pyramidExtent.width),
                static_cast<float>(pyramidExtent.height));
  uniforms.cullingInfo = glm::vec4(static_cast<float>(objectCount), static_cast<float>(mipCount), 1.0f, 1e-4f);
  return uniforms;
}

Renderer::FrameLightingState Renderer::buildFrameLightingState(const RenderParams& params) const
{
  FrameLightingState state{};
  const shaderio::CameraUniforms fallbackCamera{
      .view = glm::lookAt(glm::vec3(8.0f, 2.0f, 0.0f), glm::vec3(0.0f), glm::vec3(0.0f, 1.0f, 0.0f)),
      .projection = [] {
        return clipspace::makePerspectiveProjection(
            glm::radians(45.0f), 16.0f / 9.0f, 0.1f, 100.0f,
            clipspace::getProjectionConvention(clipspace::BackendConvention::vulkan));
      }(),
      .viewProjection = glm::mat4(1.0f),
      .inverseViewProjection = glm::mat4(1.0f),
      .cameraPosition = glm::vec3(8.0f, 2.0f, 0.0f),
      .shadowConstantBias = 0.0f,
      .shadowDirectionAndSlopeBias = glm::vec4(0.0f),
  };

  shaderio::CameraUniforms camera = params.cameraUniforms != nullptr ? *params.cameraUniforms : fallbackCamera;
  if(params.cameraUniforms == nullptr)
  {
    camera.viewProjection = camera.projection * camera.view;
    camera.inverseViewProjection = glm::inverse(camera.viewProjection);
    camera.shadowConstantBias = 0.0f;
    camera.shadowDirectionAndSlopeBias = glm::vec4(0.0f);
  }

  state.sceneBounds = computeSceneBounds(params.gltfModel);

  const clipspace::ProjectionConvention projectionConvention =
      clipspace::getProjectionConvention(clipspace::BackendConvention::vulkan);
  const float cameraNear = std::abs(clipspace::extractPerspectiveNearPlane(camera.projection, projectionConvention));
  const float cameraFar = std::abs(clipspace::extractPerspectiveFarPlane(camera.projection, projectionConvention));
  state.shadowDistance = glm::clamp(params.lightSettings.shadowDistance, cameraNear + 0.5f, std::max(cameraFar, cameraNear + 1.0f));
  state.viewFrustumCorners = computePerspectiveFrustumCorners(camera, cameraNear, state.shadowDistance);

  std::array<glm::vec3, 8> shadowFitCorners = state.viewFrustumCorners;
  if(state.sceneBounds.valid)
  {
    shadowFitCorners = {{
        {state.sceneBounds.min.x, state.sceneBounds.min.y, state.sceneBounds.min.z},
        {state.sceneBounds.max.x, state.sceneBounds.min.y, state.sceneBounds.min.z},
        {state.sceneBounds.min.x, state.sceneBounds.max.y, state.sceneBounds.min.z},
        {state.sceneBounds.max.x, state.sceneBounds.max.y, state.sceneBounds.min.z},
        {state.sceneBounds.min.x, state.sceneBounds.min.y, state.sceneBounds.max.z},
        {state.sceneBounds.max.x, state.sceneBounds.min.y, state.sceneBounds.max.z},
        {state.sceneBounds.min.x, state.sceneBounds.max.y, state.sceneBounds.max.z},
        {state.sceneBounds.max.x, state.sceneBounds.max.y, state.sceneBounds.max.z},
    }};
  }

  glm::vec3 center(0.0f);
  for(const glm::vec3& corner : shadowFitCorners)
  {
    center += corner;
  }
  center /= static_cast<float>(shadowFitCorners.size());

  const glm::vec3 lightTravelDir = glm::normalize(params.lightSettings.direction);
  const glm::vec3 dirToLight = -lightTravelDir;
  const glm::vec3 upReference =
      std::abs(lightTravelDir.y) > 0.95f ? glm::vec3(0.0f, 0.0f, 1.0f) : glm::vec3(0.0f, 1.0f, 0.0f);

  float radius = 0.0f;
  for(const glm::vec3& corner : shadowFitCorners)
  {
    radius = std::max(radius, glm::length(corner - center));
  }
  radius = std::max(radius, 5.0f);

  state.lightAnchor = center;
  const glm::vec3 lightPosition = center - lightTravelDir * (radius + 10.0f);
  const glm::mat4 lightView = glm::lookAt(lightPosition, center, upReference);

  glm::vec3 minExtents(std::numeric_limits<float>::max());
  glm::vec3 maxExtents(std::numeric_limits<float>::lowest());
  for(const glm::vec3& corner : shadowFitCorners)
  {
    const glm::vec3 lightSpace = glm::vec3(lightView * glm::vec4(corner, 1.0f));
    minExtents = glm::min(minExtents, lightSpace);
    maxExtents = glm::max(maxExtents, lightSpace);
  }

  const float xyExtent = std::max(maxExtents.x - minExtents.x, maxExtents.y - minExtents.y) * 0.5f + 2.0f;
  glm::vec2 lightSpaceCenter((minExtents.x + maxExtents.x) * 0.5f, (minExtents.y + maxExtents.y) * 0.5f);
  const float texelWorldSize = (xyExtent * 2.0f) / static_cast<float>(SceneResources::kShadowMapSize);
  lightSpaceCenter = glm::floor(lightSpaceCenter / texelWorldSize) * texelWorldSize;

  minExtents.x = lightSpaceCenter.x - xyExtent;
  maxExtents.x = lightSpaceCenter.x + xyExtent;
  minExtents.y = lightSpaceCenter.y - xyExtent;
  maxExtents.y = lightSpaceCenter.y + xyExtent;
  minExtents.z -= radius * 2.0f + 20.0f;
  maxExtents.z += 20.0f;

  const glm::mat4 lightProjection = clipspace::makeOrthographicProjection(
      minExtents.x, maxExtents.x, minExtents.y, maxExtents.y, -maxExtents.z, -minExtents.z, projectionConvention);
  state.shadowCamera.view = lightView;
  state.shadowCamera.projection = lightProjection;
  state.shadowCamera.viewProjection = lightProjection * lightView;
  state.shadowCamera.inverseViewProjection = glm::inverse(state.shadowCamera.viewProjection);
  state.shadowCamera.cameraPosition = lightPosition;
  state.shadowCamera.shadowConstantBias = params.lightSettings.depthBias;
  state.shadowCamera.shadowDirectionAndSlopeBias = glm::vec4(dirToLight, params.lightSettings.normalBias);
  state.shadowFrustumCorners = computeOrthoFrustumCorners(glm::inverse(state.shadowCamera.viewProjection));

  // Populate CSM cascade matrices and split distances for LightParams
  const shaderio::ShadowUniforms* shadowData = m_csmShadowResources.getShadowUniformsData();
  for(int i = 0; i < shaderio::LCascadeCount; ++i)
  {
    state.lightParams.worldToShadow[i] = shadowData->cascadeViewProjection[i];
  }
  state.lightParams.cascadeSplitDistances = shadowData->cascadeSplitDistances;
  state.lightParams.lightDirectionAndShadowStrength =
      glm::vec4(dirToLight, params.lightSettings.shadowStrength);
  state.lightParams.lightColorAndNormalBias = glm::vec4(params.lightSettings.color, params.lightSettings.normalBias);
  state.lightParams.ambientColorAndTexelSize =
      glm::vec4(params.lightSettings.ambient, 1.0f / static_cast<float>(m_csmShadowResources.getCascadeResolution()));
  state.lightParams.shadowMetrics = glm::vec4(
      1.0f / static_cast<float>(m_csmShadowResources.getCascadeResolution()),
      params.lightSettings.depthBias,
      params.lightSettings.normalBias,
      static_cast<float>(shaderio::LCascadeCount));
  return state;
}

void Renderer::buildDebugDrawList(const RenderParams& params)
{
  m_debugDrawList.clear();
  if(!params.debugOptions.enabled)
  {
    return;
  }

  if(params.debugOptions.showSceneBounds)
  {
    m_debugDrawList.addAabb(m_frameLightingState.sceneBounds, glm::vec4(0.20f, 0.85f, 0.35f, 0.90f));
  }
  if(params.debugOptions.showShadowFrustum)
  {
    m_debugDrawList.addFrustum(m_frameLightingState.shadowFrustumCorners, glm::vec4(0.95f, 0.75f, 0.20f, 0.90f));
  }
  if(params.debugOptions.showViewFrustum)
  {
    m_debugDrawList.addFrustum(m_frameLightingState.viewFrustumCorners, glm::vec4(0.25f, 0.65f, 1.00f, 0.85f));
  }
  if(params.debugOptions.showLightDirection)
  {
    m_debugDrawList.addArrow(m_frameLightingState.lightAnchor, glm::normalize(params.lightSettings.direction), 6.0f,
                             glm::vec4(1.00f, 0.55f, 0.10f, 0.95f));
  }
  if(params.debugOptions.showPointLights)
  {
    float markerRadius = 0.18f;
    if(m_frameLightingState.sceneBounds.valid)
    {
      const glm::vec3 boundsSize = glm::max(m_frameLightingState.sceneBounds.max - m_frameLightingState.sceneBounds.min,
                                            glm::vec3(1.0f));
      markerRadius = glm::clamp(glm::length(boundsSize) * 0.0025f, 0.18f, 1.5f);
    }
    for(const shaderio::LightData& light : m_testPointLights)
    {
      const glm::vec4 color(light.color, 0.85f);
      m_debugDrawList.addSphere(light.positionOrDirection, markerRadius, color, 12);
      m_debugDrawList.addSphere(light.positionOrDirection, light.range, glm::vec4(light.color, 0.22f), 24);
    }
  }
  if(params.debugOptions.showCullDistance && params.cameraUniforms != nullptr)
  {
    m_debugDrawList.addSphere(params.cameraUniforms->cameraPosition, params.debugOptions.cullDistance,
                              glm::vec4(0.95f, 0.20f, 0.30f, 0.70f), 48);
  }
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
      .clearValue  = {{{0.0f, 0}}},
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
  createDepthPyramidPipeline();
  createGPUCullingPipeline();
  createLightCoarseCullingPipelines();
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
      .depthState        = rhi::DepthState{true, true, rhi::CompareOp::greaterOrEqual},
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
            .visibility      = rhi::ResourceVisibility::allGraphics,
        },
        rhi::BindTableLayoutEntry{
            .logicalIndex    = shaderio::LBindLighting,
            .resourceType    = rhi::BindlessResourceType::uniformBuffer,
            .descriptorCount = 1,
            .visibility      = rhi::ResourceVisibility::fragment,
        },
        rhi::BindTableLayoutEntry{
            .logicalIndex    = shaderio::LBindLightCulling,
            .resourceType    = rhi::BindlessResourceType::uniformBuffer,
            .descriptorCount = 1,
            .visibility      = rhi::ResourceVisibility::compute,
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
      VkDescriptorBufferInfo lightingBufferInfo{
          .buffer = m_perFrame.frameUserData[i].lightingBuffer.buffer,
          .offset = 0,
          .range  = sizeof(shaderio::LightingUniforms),
      };
      VkDescriptorBufferInfo lightCullingBufferInfo{
          .buffer = m_perFrame.frameUserData[i].lightCullingBuffer.buffer,
          .offset = 0,
          .range  = sizeof(shaderio::LightCullingUniforms),
      };
      const std::array<VkWriteDescriptorSet, 3> cameraWrites{{
          VkWriteDescriptorSet{
              .sType           = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET,
              .dstSet          = cameraTable->getVkDescriptorSet(),
              .dstBinding      = shaderio::LBindCamera,
              .dstArrayElement = 0,
              .descriptorCount = 1,
              .descriptorType  = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER_DYNAMIC,
              .pBufferInfo     = &cameraBufferInfo,
          },
          VkWriteDescriptorSet{
              .sType           = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET,
              .dstSet          = cameraTable->getVkDescriptorSet(),
              .dstBinding      = shaderio::LBindLighting,
              .dstArrayElement = 0,
              .descriptorCount = 1,
              .descriptorType  = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER,
              .pBufferInfo     = &lightingBufferInfo,
          },
          VkWriteDescriptorSet{
              .sType           = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET,
              .dstSet          = cameraTable->getVkDescriptorSet(),
              .dstBinding      = shaderio::LBindLightCulling,
              .dstArrayElement = 0,
              .descriptorCount = 1,
              .descriptorType  = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER,
              .pBufferInfo     = &lightCullingBufferInfo,
          },
      }};
      vkUpdateDescriptorSets(fromNativeHandle<VkDevice>(m_device.device->getNativeDevice()),
                             static_cast<uint32_t>(cameraWrites.size()), cameraWrites.data(), 0, nullptr);

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

    rhi::ShaderReflectionData debugReflection{};
    debugReflection.name = "shader.debug.gpu-cull";
    debugReflection.entryPoints = {
        rhi::ShaderEntryPoint{"vertexCullMain", rhi::ShaderStageFlagBits::vertex},
        rhi::ShaderEntryPoint{"fragmentMain", rhi::ShaderStageFlagBits::fragment},
    };
    debugReflection.resourceBindings = {
        rhi::ShaderResourceBinding{"camera", rhi::ShaderResourceType::uniformBuffer, rhi::DescriptorType::uniformBufferDynamic,
                                   rhi::ShaderStageFlagBits::vertex, shaderio::LSetScene, shaderio::LBindCamera, 1, 0},
    };
    debugReflection.pushConstantRanges = {
        rhi::PushConstantRange{rhi::ShaderStageFlagBits::vertex, 0, sizeof(shaderio::PushConstantGPUCullDebug)},
    };

    rhi::PipelineLayoutDesc debugLayoutDesc = rhi::derivePipelineLayoutDesc(debugReflection);
    debugLayoutDesc.debugName = "debug-layout";

    const std::array<rhi::vulkan::VulkanPipelineLayoutBindingMapping, 2> debugLayoutMappings{
        rhi::vulkan::makePipelineLayoutBindingMapping(shaderio::LSetTextures, textureLayoutHandle),
        rhi::vulkan::makePipelineLayoutBindingMapping(shaderio::LSetScene, cameraLayoutHandle),
    };
    const rhi::vulkan::VulkanPipelineLayoutLowering debugLayoutLowering{
        .setLayouts     = debugLayoutMappings.data(),
        .setLayoutCount = static_cast<uint32_t>(debugLayoutMappings.size()),
    };

    auto debugPipelineLayout = std::make_unique<rhi::vulkan::VulkanPipelineLayout>();
    debugPipelineLayout->init(static_cast<void*>(fromNativeHandle<VkDevice>(m_device.device->getNativeDevice())),
                              debugLayoutDesc, debugLayoutLowering);
    DBG_VK_NAME(reinterpret_cast<VkPipelineLayout>(debugPipelineLayout->getNativeHandle()));
    m_device.debugPipelineLayout = std::move(debugPipelineLayout);
  }

  // Create light pipeline for LightPass (fullscreen triangle sampling GBuffer)
#ifdef USE_SLANG
  {
    if(m_device.lightPipelineLayout == VK_NULL_HANDLE)
    {
      ASSERT(!m_perFrame.frameUserData.empty(), "LightPass requires per-frame scene bind groups");
      const VkDescriptorSetLayout sceneSetLayout = reinterpret_cast<VkDescriptorSetLayout>(
          getBindGroupLayoutOpaque(m_perFrame.frameUserData.front().cameraBindGroup, BindGroupSetSlot::shaderSpecific));
      const std::array<VkDescriptorSetLayout, 2> setLayouts{m_device.gbufferTextureSetLayout, sceneSetLayout};
      const VkPipelineLayoutCreateInfo layoutInfo{
          .sType          = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO,
          .setLayoutCount = static_cast<uint32_t>(setLayouts.size()),
          .pSetLayouts    = setLayouts.data(),
      };
      VK_CHECK(vkCreatePipelineLayout(fromNativeHandle<VkDevice>(m_device.device->getNativeDevice()),
                                      &layoutInfo,
                                      nullptr,
                                      &m_device.lightPipelineLayout));
      DBG_VK_NAME(m_device.lightPipelineLayout);
    }

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

  // Create depth prepass pipelines (Opaque + AlphaTest variants)
  {
    VkShaderModule depthPrepassShaderModule =
        utils::createShaderModule(fromNativeHandle<VkDevice>(m_device.device->getNativeDevice()),
                                  {shader_depth_prepass_slang, std::size(shader_depth_prepass_slang)});
    DBG_VK_NAME(depthPrepassShaderModule);

    const std::array<rhi::VertexBindingDesc, 1> depthBindings{{
        {.binding = 0, .stride = 48, .inputRate = rhi::VertexInputRate::perVertex}
    }};
    const std::array<rhi::VertexAttributeDesc, 4> depthAttributes{{
        {.location = shaderio::LVGltfPosition, .binding = 0, .format = rhi::VertexFormat::r32g32b32Sfloat, .offset = 0},
        {.location = shaderio::LVGltfNormal,   .binding = 0, .format = rhi::VertexFormat::r32g32b32Sfloat, .offset = 12},
        {.location = shaderio::LVGltfTexCoord, .binding = 0, .format = rhi::VertexFormat::r32g32Sfloat,    .offset = 24},
        {.location = shaderio::LVGltfTangent,  .binding = 0, .format = rhi::VertexFormat::r32g32b32a32Sfloat, .offset = 32},
    }};
    const rhi::VertexInputLayoutDesc depthVertexInput{
        .bindings       = depthBindings.data(),
        .bindingCount   = static_cast<uint32_t>(depthBindings.size()),
        .attributes     = depthAttributes.data(),
        .attributeCount = static_cast<uint32_t>(depthAttributes.size()),
    };

    const std::array<rhi::DynamicState, 2> depthDynamicStates{{
        rhi::DynamicState::viewport,
        rhi::DynamicState::scissor,
    }};
    const std::array<rhi::PipelineShaderStageDesc, 2> depthStages{{
        {.stage = rhi::ShaderStage::vertex, .shaderModule = reinterpret_cast<uint64_t>(depthPrepassShaderModule), .entryPoint = "vertexMain"},
        {.stage = rhi::ShaderStage::fragment, .shaderModule = reinterpret_cast<uint64_t>(depthPrepassShaderModule), .entryPoint = "fragmentMain"},
    }};

    rhi::GraphicsPipelineDesc depthDesc{
        .layout            = m_device.gbufferPipelineLayout.get(),
        .shaderStages      = depthStages.data(),
        .shaderStageCount  = static_cast<uint32_t>(depthStages.size()),
        .vertexInput       = depthVertexInput,
        .rasterState       = rhi::RasterState{},
        .depthState        = rhi::DepthState{true, true, rhi::CompareOp::greater},
        .blendStates       = nullptr,
        .blendStateCount   = 0,
        .dynamicStates     = depthDynamicStates.data(),
        .dynamicStateCount = static_cast<uint32_t>(depthDynamicStates.size()),
        .renderingInfo =
            {
                .colorFormats     = nullptr,
                .colorFormatCount = 0,
                .depthFormat      = toPortableTextureFormat(m_swapchainDependent.sceneResources.getDepthFormat()),
            },
    };
    depthDesc.rasterState.topology    = rhi::PrimitiveTopology::triangleList;
    depthDesc.rasterState.cullMode    = rhi::CullMode::back;
    depthDesc.rasterState.frontFace   = rhi::FrontFace::counterClockwise;
    depthDesc.rasterState.sampleCount = rhi::SampleCount::count1;

    rhi::SpecializationConstant specConstantAlphaTest(0, 0, sizeof(uint32_t));
    std::array<rhi::PipelineShaderStageDesc, 2> depthShaderStages = depthStages;
    depthDesc.shaderStages = depthShaderStages.data();

    uint32_t alphaTestFalse = VK_FALSE;
    rhi::SpecializationData specDataFalse{&alphaTestFalse, sizeof(uint32_t)};
    depthShaderStages[1].specializationData = specDataFalse;
    depthShaderStages[1].specializationConstants = &specConstantAlphaTest;
    depthShaderStages[1].specializationConstantCount = 1;

    rhi::vulkan::GraphicsPipelineCreateInfo depthCreateInfo{
        .key =
            {
                .shaderIdentity        = rhi::vulkan::PipelineShaderIdentity::raster,
                .specializationVariant = 10,
            },
        .desc = depthDesc,
    };
    const VkPipeline depthOpaquePipeline =
        rhi::vulkan::createGraphicsPipeline(fromNativeHandle<VkDevice>(m_device.device->getNativeDevice()), depthCreateInfo);
    DBG_VK_NAME(depthOpaquePipeline);
    m_depthPrepassOpaquePipeline = registerPipeline(static_cast<uint32_t>(VK_PIPELINE_BIND_POINT_GRAPHICS),
                                                    reinterpret_cast<uint64_t>(depthOpaquePipeline),
                                                    depthCreateInfo.key.specializationVariant);

    uint32_t alphaTestTrue = VK_TRUE;
    rhi::SpecializationData specDataTrue{&alphaTestTrue, sizeof(uint32_t)};
    depthShaderStages[1].specializationData = specDataTrue;
    depthCreateInfo.key.specializationVariant = 11;
    const VkPipeline depthAlphaTestPipeline =
        rhi::vulkan::createGraphicsPipeline(fromNativeHandle<VkDevice>(m_device.device->getNativeDevice()), depthCreateInfo);
    DBG_VK_NAME(depthAlphaTestPipeline);
    m_depthPrepassAlphaTestPipeline = registerPipeline(static_cast<uint32_t>(VK_PIPELINE_BIND_POINT_GRAPHICS),
                                                       reinterpret_cast<uint64_t>(depthAlphaTestPipeline),
                                                       depthCreateInfo.key.specializationVariant);

    vkDestroyShaderModule(fromNativeHandle<VkDevice>(m_device.device->getNativeDevice()), depthPrepassShaderModule, nullptr);
  }

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
        .depthState        = rhi::DepthState{true, false, rhi::CompareOp::greaterOrEqual},
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

  // Create Shadow pipeline
  {
    VkShaderModule shadowShaderModule = utils::createShaderModule(fromNativeHandle<VkDevice>(m_device.device->getNativeDevice()),
                                                                  {shader_shadow_slang, std::size(shader_shadow_slang)});
    DBG_VK_NAME(shadowShaderModule);

    const std::array<rhi::VertexBindingDesc, 1> shadowBindings{{
        {.binding = 0, .stride = 48, .inputRate = rhi::VertexInputRate::perVertex}
    }};
    const std::array<rhi::VertexAttributeDesc, 4> shadowAttributes{{
        {.location = shaderio::LVGltfPosition, .binding = 0, .format = rhi::VertexFormat::r32g32b32Sfloat, .offset = 0},
        {.location = shaderio::LVGltfNormal,   .binding = 0, .format = rhi::VertexFormat::r32g32b32Sfloat, .offset = 12},
        {.location = shaderio::LVGltfTexCoord, .binding = 0, .format = rhi::VertexFormat::r32g32Sfloat, .offset = 24},
        {.location = shaderio::LVGltfTangent,  .binding = 0, .format = rhi::VertexFormat::r32g32b32a32Sfloat, .offset = 32},
    }};
    const rhi::VertexInputLayoutDesc shadowVertexInput{
        .bindings       = shadowBindings.data(),
        .bindingCount   = static_cast<uint32_t>(shadowBindings.size()),
        .attributes     = shadowAttributes.data(),
        .attributeCount = static_cast<uint32_t>(shadowAttributes.size()),
    };
    const std::array<rhi::DynamicState, 2> shadowDynamicStates{{
        rhi::DynamicState::viewport,
        rhi::DynamicState::scissor,
    }};
    const std::array<rhi::PipelineShaderStageDesc, 2> shadowStages{{
        {.stage = rhi::ShaderStage::vertex, .shaderModule = reinterpret_cast<uint64_t>(shadowShaderModule), .entryPoint = "vertexMain"},
        {.stage = rhi::ShaderStage::fragment, .shaderModule = reinterpret_cast<uint64_t>(shadowShaderModule), .entryPoint = "fragmentMain"},
    }};
    rhi::GraphicsPipelineDesc shadowDesc{
        .layout            = m_device.gbufferPipelineLayout.get(),
        .shaderStages      = shadowStages.data(),
        .shaderStageCount  = static_cast<uint32_t>(shadowStages.size()),
        .vertexInput       = shadowVertexInput,
        .rasterState       = rhi::RasterState{},
        .depthState        = rhi::DepthState{true, true, rhi::CompareOp::greaterOrEqual},
        .blendStates       = nullptr,
        .blendStateCount   = 0,
        .dynamicStates     = shadowDynamicStates.data(),
        .dynamicStateCount = static_cast<uint32_t>(shadowDynamicStates.size()),
        .renderingInfo =
            {
                .colorFormats     = nullptr,
                .colorFormatCount = 0,
                .depthFormat      = toPortableTextureFormat(m_csmShadowResources.getShadowFormat()),
            },
    };
    shadowDesc.rasterState.topology    = rhi::PrimitiveTopology::triangleList;
    shadowDesc.rasterState.cullMode    = rhi::CullMode::none;
    shadowDesc.rasterState.frontFace   = rhi::FrontFace::counterClockwise;
    shadowDesc.rasterState.sampleCount = rhi::SampleCount::count1;

    rhi::vulkan::GraphicsPipelineCreateInfo shadowCreateInfo{
        .key =
            {
                .shaderIdentity        = rhi::vulkan::PipelineShaderIdentity::raster,
                .specializationVariant = 6,
            },
        .desc = shadowDesc,
    };
    const VkPipeline shadowPipeline =
        rhi::vulkan::createGraphicsPipeline(fromNativeHandle<VkDevice>(m_device.device->getNativeDevice()), shadowCreateInfo);
    DBG_VK_NAME(shadowPipeline);
    m_shadowPipeline = registerPipeline(static_cast<uint32_t>(VK_PIPELINE_BIND_POINT_GRAPHICS),
                                        reinterpret_cast<uint64_t>(shadowPipeline),
                                        shadowCreateInfo.key.specializationVariant);

    vkDestroyShaderModule(fromNativeHandle<VkDevice>(m_device.device->getNativeDevice()), shadowShaderModule, nullptr);
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
        .depthState        = rhi::DepthState{true, false, rhi::CompareOp::greaterOrEqual},  // Test enabled, write disabled
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

  // Create Debug line pipeline
  {
    VkShaderModule debugShaderModule = utils::createShaderModule(fromNativeHandle<VkDevice>(m_device.device->getNativeDevice()),
                                                                 {shader_debug_slang, std::size(shader_debug_slang)});
    DBG_VK_NAME(debugShaderModule);

    const std::array<rhi::VertexBindingDesc, 1> debugBindings{{
        {.binding = 0, .stride = sizeof(shaderio::DebugLineVertex), .inputRate = rhi::VertexInputRate::perVertex}
    }};
    const std::array<rhi::VertexAttributeDesc, 2> debugAttributes{{
        {.location = shaderio::LVDebugPosition, .binding = 0, .format = rhi::VertexFormat::r32g32b32Sfloat,
         .offset = static_cast<uint32_t>(offsetof(shaderio::DebugLineVertex, position))},
        {.location = shaderio::LVDebugColor, .binding = 0, .format = rhi::VertexFormat::r32g32b32a32Sfloat,
         .offset = static_cast<uint32_t>(offsetof(shaderio::DebugLineVertex, color))},
    }};
    const rhi::VertexInputLayoutDesc debugVertexInput{
        .bindings       = debugBindings.data(),
        .bindingCount   = static_cast<uint32_t>(debugBindings.size()),
        .attributes     = debugAttributes.data(),
        .attributeCount = static_cast<uint32_t>(debugAttributes.size()),
    };
    const std::array<rhi::DynamicState, 2> debugDynamicStates{{
        rhi::DynamicState::viewport,
        rhi::DynamicState::scissor,
    }};
    const rhi::BlendAttachmentState debugBlend{
        .blendEnable         = true,
        .srcColorBlendFactor = rhi::BlendFactor::srcAlpha,
        .dstColorBlendFactor = rhi::BlendFactor::oneMinusSrcAlpha,
        .colorBlendOp        = rhi::BlendOp::add,
        .srcAlphaBlendFactor = rhi::BlendFactor::one,
        .dstAlphaBlendFactor = rhi::BlendFactor::oneMinusSrcAlpha,
        .alphaBlendOp        = rhi::BlendOp::add,
        .colorWriteMask      = rhi::ColorComponentFlags::all,
    };
    const std::array<rhi::PipelineShaderStageDesc, 2> debugStages{{
        {.stage = rhi::ShaderStage::vertex, .shaderModule = reinterpret_cast<uint64_t>(debugShaderModule), .entryPoint = "vertexMain"},
        {.stage = rhi::ShaderStage::fragment, .shaderModule = reinterpret_cast<uint64_t>(debugShaderModule), .entryPoint = "fragmentMain"},
    }};
    const rhi::TextureFormat outputFormat = toPortableTextureFormat(VK_FORMAT_B8G8R8A8_UNORM);
    rhi::GraphicsPipelineDesc debugDesc{
        .layout            = m_device.gbufferPipelineLayout.get(),
        .shaderStages      = debugStages.data(),
        .shaderStageCount  = static_cast<uint32_t>(debugStages.size()),
        .vertexInput       = debugVertexInput,
        .rasterState       = rhi::RasterState{},
        .depthState        = rhi::DepthState{false, false, rhi::CompareOp::always},
        .blendStates       = &debugBlend,
        .blendStateCount   = 1,
        .dynamicStates     = debugDynamicStates.data(),
        .dynamicStateCount = static_cast<uint32_t>(debugDynamicStates.size()),
        .renderingInfo =
            {
                .colorFormats     = &outputFormat,
                .colorFormatCount = 1,
                .depthFormat      = rhi::TextureFormat::undefined,
            },
    };
    debugDesc.rasterState.topology    = rhi::PrimitiveTopology::lineList;
    debugDesc.rasterState.cullMode    = rhi::CullMode::none;
    debugDesc.rasterState.frontFace   = rhi::FrontFace::counterClockwise;
    debugDesc.rasterState.sampleCount = rhi::SampleCount::count1;

    rhi::vulkan::GraphicsPipelineCreateInfo debugCreateInfo{
        .key =
            {
                .shaderIdentity        = rhi::vulkan::PipelineShaderIdentity::raster,
                .specializationVariant = 7,
            },
        .desc = debugDesc,
    };
    const VkPipeline debugPipeline =
        rhi::vulkan::createGraphicsPipeline(fromNativeHandle<VkDevice>(m_device.device->getNativeDevice()), debugCreateInfo);
    DBG_VK_NAME(debugPipeline);
    m_debugPipeline = registerPipeline(static_cast<uint32_t>(VK_PIPELINE_BIND_POINT_GRAPHICS),
                                       reinterpret_cast<uint64_t>(debugPipeline),
                                       debugCreateInfo.key.specializationVariant);

    const std::array<rhi::PipelineShaderStageDesc, 2> debugCullStages{{
        {.stage = rhi::ShaderStage::vertex, .shaderModule = reinterpret_cast<uint64_t>(debugShaderModule), .entryPoint = "vertexCullMain"},
        {.stage = rhi::ShaderStage::fragment, .shaderModule = reinterpret_cast<uint64_t>(debugShaderModule), .entryPoint = "fragmentMain"},
    }};
    rhi::GraphicsPipelineDesc debugCullDesc{
        .layout            = m_device.debugPipelineLayout.get(),
        .shaderStages      = debugCullStages.data(),
        .shaderStageCount  = static_cast<uint32_t>(debugCullStages.size()),
        .vertexInput       = {},
        .rasterState       = rhi::RasterState{},
        .depthState        = rhi::DepthState{false, false, rhi::CompareOp::always},
        .blendStates       = &debugBlend,
        .blendStateCount   = 1,
        .dynamicStates     = debugDynamicStates.data(),
        .dynamicStateCount = static_cast<uint32_t>(debugDynamicStates.size()),
        .renderingInfo =
            {
                .colorFormats     = &outputFormat,
                .colorFormatCount = 1,
                .depthFormat      = rhi::TextureFormat::undefined,
            },
    };
    debugCullDesc.rasterState.topology    = rhi::PrimitiveTopology::lineList;
    debugCullDesc.rasterState.cullMode    = rhi::CullMode::none;
    debugCullDesc.rasterState.frontFace   = rhi::FrontFace::counterClockwise;
    debugCullDesc.rasterState.sampleCount = rhi::SampleCount::count1;

    rhi::vulkan::GraphicsPipelineCreateInfo debugCullCreateInfo{
        .key =
            {
                .shaderIdentity        = rhi::vulkan::PipelineShaderIdentity::raster,
                .specializationVariant = 8,
            },
        .desc = debugCullDesc,
    };
    const VkPipeline debugCullPipeline =
        rhi::vulkan::createGraphicsPipeline(fromNativeHandle<VkDevice>(m_device.device->getNativeDevice()), debugCullCreateInfo);
    DBG_VK_NAME(debugCullPipeline);
    m_gpuCullingDebugPipeline = registerPipeline(static_cast<uint32_t>(VK_PIPELINE_BIND_POINT_GRAPHICS),
                                                 reinterpret_cast<uint64_t>(debugCullPipeline),
                                                 debugCullCreateInfo.key.specializationVariant);

    vkDestroyShaderModule(fromNativeHandle<VkDevice>(m_device.device->getNativeDevice()), debugShaderModule, nullptr);
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

  ImGui::GetIO().ConfigFlags = ImGuiConfigFlags_DockingEnable;
}

void Renderer::createDescriptorPool()
{
  VkPhysicalDeviceProperties2 deviceProperties2{.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PROPERTIES_2};
  vkGetPhysicalDeviceProperties2(fromNativeHandle<VkPhysicalDevice>(m_device.device->getNativePhysicalDevice()), &deviceProperties2);
  const auto& deviceProperties = deviceProperties2.properties;

  {
    m_materials.maxTextures = std::min(m_materials.maxTextures, deviceProperties.limits.maxDescriptorSetSampledImages);
    const uint32_t maxDescriptorSets = 32U;
    const uint32_t dynamicUniformCount =
        std::max(1U, std::min(maxDescriptorSets, deviceProperties.limits.maxDescriptorSetUniformBuffersDynamic));
    const uint32_t frameCount = std::max<uint32_t>(1U, static_cast<uint32_t>(m_perFrame.frameUserData.size()));
    const std::array<VkDescriptorPoolSize, 6> poolSizes{{
        {VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, m_materials.maxTextures},
        {VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE, 4U + frameCount * shaderio::LDepthPyramidMaxMips},
        {VK_DESCRIPTOR_TYPE_STORAGE_IMAGE, shaderio::LDepthPyramidMaxMips},
        {VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER_DYNAMIC, dynamicUniformCount},
        {VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER, 6U + frameCount * 3U},  // LightPass camera + pre-scene UBOs + GPU culling UBOs
        {VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, frameCount * 10U},
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

void Renderer::createLightCoarseCullingPipelines()
{
  if(m_device.lightCoarseCullingPipelineLayout == VK_NULL_HANDLE)
  {
    return;
  }

#ifdef USE_SLANG
  const VkDevice nativeDevice = fromNativeHandle<VkDevice>(m_device.device->getNativeDevice());
  VkShaderModule shaderModule = utils::createShaderModule(nativeDevice, {shader_light_culling_slang, std::size(shader_light_culling_slang)});
  DBG_VK_NAME(shaderModule);

  const auto createPipeline = [&](const char* entryPoint, uint32_t specializationVariant) {
    const VkPipelineShaderStageCreateInfo shaderStage{
        .sType  = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO,
        .stage  = VK_SHADER_STAGE_COMPUTE_BIT,
        .module = shaderModule,
        .pName  = entryPoint,
    };
    const VkComputePipelineCreateInfo pipelineInfo{
        .sType  = VK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO,
        .stage  = shaderStage,
        .layout = m_device.lightCoarseCullingPipelineLayout,
    };

    VkPipeline pipeline = VK_NULL_HANDLE;
    VK_CHECK(vkCreateComputePipelines(nativeDevice, VK_NULL_HANDLE, 1, &pipelineInfo, nullptr, &pipeline));
    DBG_VK_NAME(pipeline);
    return registerPipeline(static_cast<uint32_t>(VK_PIPELINE_BIND_POINT_COMPUTE),
                            reinterpret_cast<uint64_t>(pipeline),
                            specializationVariant);
  };

  m_pointLightCoarseCullingPipeline = createPipeline("kernelPointLightCoarseCulling", 8);
  m_spotLightCoarseCullingPipeline = createPipeline("kernelSpotLightCoarseCulling", 9);

  vkDestroyShaderModule(nativeDevice, shaderModule, nullptr);
#endif
}

void Renderer::createDepthPyramidPipeline()
{
  if(m_device.depthPyramidPipelineLayout == VK_NULL_HANDLE)
  {
    return;
  }

#ifdef USE_SLANG
  const VkDevice nativeDevice = fromNativeHandle<VkDevice>(m_device.device->getNativeDevice());
  VkShaderModule shaderModule =
      utils::createShaderModule(nativeDevice, {shader_depth_pyramid_slang, std::size(shader_depth_pyramid_slang)});
  DBG_VK_NAME(shaderModule);

  const VkPipelineShaderStageCreateInfo shaderStage{
      .sType  = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO,
      .stage  = VK_SHADER_STAGE_COMPUTE_BIT,
      .module = shaderModule,
      .pName  = "depthPyramid",
  };

  const VkComputePipelineCreateInfo pipelineInfo{
      .sType  = VK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO,
      .stage  = shaderStage,
      .layout = m_device.depthPyramidPipelineLayout,
  };

  VkPipeline pipeline = VK_NULL_HANDLE;
  VK_CHECK(vkCreateComputePipelines(nativeDevice, VK_NULL_HANDLE, 1, &pipelineInfo, nullptr, &pipeline));
  DBG_VK_NAME(pipeline);
  m_depthPyramidPipeline = registerPipeline(static_cast<uint32_t>(VK_PIPELINE_BIND_POINT_COMPUTE),
                                            reinterpret_cast<uint64_t>(pipeline),
                                            12);

  vkDestroyShaderModule(nativeDevice, shaderModule, nullptr);
#endif
}

void Renderer::createGPUCullingPipeline()
{
  if(m_device.gpuCullingPipelineLayout == VK_NULL_HANDLE)
  {
    return;
  }

#ifdef USE_SLANG
  const VkDevice nativeDevice = fromNativeHandle<VkDevice>(m_device.device->getNativeDevice());
  VkShaderModule shaderModule =
      utils::createShaderModule(nativeDevice, {shader_gpu_culling_slang, std::size(shader_gpu_culling_slang)});
  DBG_VK_NAME(shaderModule);

  const VkPipelineShaderStageCreateInfo shaderStage{
      .sType  = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO,
      .stage  = VK_SHADER_STAGE_COMPUTE_BIT,
      .module = shaderModule,
      .pName  = "gpuCullingMain",
  };
  const VkComputePipelineCreateInfo pipelineInfo{
      .sType  = VK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO,
      .stage  = shaderStage,
      .layout = m_device.gpuCullingPipelineLayout,
  };

  VkPipeline pipeline = VK_NULL_HANDLE;
  VK_CHECK(vkCreateComputePipelines(nativeDevice, VK_NULL_HANDLE, 1, &pipelineInfo, nullptr, &pipeline));
  DBG_VK_NAME(pipeline);
  m_gpuCullingPipeline =
      registerPipeline(static_cast<uint32_t>(VK_PIPELINE_BIND_POINT_COMPUTE), reinterpret_cast<uint64_t>(pipeline), 13);

  vkDestroyShaderModule(nativeDevice, shaderModule, nullptr);
#endif
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
  m_lightPipeline = kNullPipelineHandle;
  m_depthPrepassOpaquePipeline = kNullPipelineHandle;
  m_depthPrepassAlphaTestPipeline = kNullPipelineHandle;
  m_depthPyramidPipeline = kNullPipelineHandle;
  m_gpuCullingPipeline = kNullPipelineHandle;
  m_gbufferOpaquePipeline = kNullPipelineHandle;
  m_gbufferAlphaTestPipeline = kNullPipelineHandle;
  m_shadowPipeline = kNullPipelineHandle;
  m_forwardPipeline = kNullPipelineHandle;
  m_debugPipeline = kNullPipelineHandle;
  m_gpuCullingDebugPipeline = kNullPipelineHandle;
  m_pointLightCoarseCullingPipeline = kNullPipelineHandle;
  m_spotLightCoarseCullingPipeline = kNullPipelineHandle;
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

    const size_t pixelSize = imageData.pixels.size();
    utils::Buffer stagingBuffer =
        upload::createUploadStagingBuffer(device,
                                          m_device.allocator,
                                          std::span<const std::byte>(reinterpret_cast<const std::byte*>(imageData.pixels.data()),
                                                                     pixelSize));

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

void Renderer::updateMeshTransform(MeshHandle handle, const glm::mat4& transform)
{
  m_meshPool.updateTransform(handle, transform);
}

uint64_t Renderer::getLightPipelineLayout() const
{
  return reinterpret_cast<uint64_t>(m_device.lightPipelineLayout);
}

uint64_t Renderer::getLightCullingPipelineLayout() const
{
  return 0;
}

uint64_t Renderer::getDebugPipelineLayout() const
{
  return m_device.debugPipelineLayout ? m_device.debugPipelineLayout->getNativeHandle() : 0;
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
  if(m_device.gbufferTextureSets.empty())
  {
    return 0;
  }

  uint32_t frameIndex = 0;
  if(m_perFrame.frameContext != nullptr)
  {
    frameIndex = m_perFrame.frameContext->getCurrentFrameIndex();
  }
  if(frameIndex >= m_device.gbufferTextureSets.size())
  {
    return 0;
  }
  return reinterpret_cast<uint64_t>(m_device.gbufferTextureSets[frameIndex]);
}

uint64_t Renderer::getLightCullingDescriptorSet() const
{
  return 0;
}

uint64_t Renderer::getGPUCullingObjectBufferAddress(uint32_t frameIndex) const
{
  if(frameIndex >= m_perFrame.frameUserData.size())
  {
    return 0;
  }
  return static_cast<uint64_t>(m_perFrame.frameUserData[frameIndex].gpuCullingObjectBuffer.address);
}

uint64_t Renderer::getGPUCullingResultBufferAddress(uint32_t frameIndex) const
{
  if(frameIndex >= m_perFrame.frameUserData.size())
  {
    return 0;
  }
  return static_cast<uint64_t>(m_perFrame.frameUserData[frameIndex].gpuCullingResultBuffer.address);
}

uint64_t Renderer::getGPUCullingIndirectBufferOpaque(uint32_t frameIndex) const
{
  if(frameIndex >= m_perFrame.frameUserData.size())
  {
    return 0;
  }
  return reinterpret_cast<uint64_t>(m_perFrame.frameUserData[frameIndex].gpuCullingIndirectBuffer.buffer);
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
