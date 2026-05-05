#pragma once

#include "volk.h"
#define VMA_DYNAMIC_VULKAN_FUNCTIONS 1
#ifdef __clang__
#pragma clang diagnostic push
#pragma clang diagnostic ignored "-Wnullability-completeness"
#pragma clang diagnostic ignored "-Wmissing-field-initializers"
#pragma clang diagnostic ignored "-Wunused-variable"
#pragma clang diagnostic ignored "-Wunused-function"
#endif
#pragma warning(push)
#pragma warning(disable : 4100)
#pragma warning(disable : 4189)
#pragma warning(disable : 4127)
#pragma warning(disable : 4324)
#pragma warning(disable : 4505)
#include "vk_mem_alloc.h"
#pragma warning(pop)
#ifdef __clang__
#pragma clang diagnostic pop
#endif

#include "vulkan/vk_enum_string_helper.h"
#ifdef _WIN32
#define GLFW_EXPOSE_NATIVE_WIN32
#elif defined(__linux__)
#include <signal.h>
#endif
#include <GLFW/glfw3.h>

// GLM configuration for Vulkan:
// - Depth range [0, 1] (Vulkan standard, OpenGL uses [-1, 1])
// - Note: Y-axis still needs manual flip in projection matrix for Vulkan NDC
#define GLM_FORCE_DEPTH_ZERO_TO_ONE
#include <glm/glm.hpp>
#include "logger.h"
#include "debug_util.h"
#include "backends/imgui_impl_glfw.h"
#include "backends/imgui_impl_vulkan.h"
#include "imgui.h"
#include "imgui_internal.h"
#include "stb_image.h"
#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#undef APIENTRY
#include <Windows.h>
#include <timeapi.h>
#endif
#include <array>
#include <chrono>
#include <cmath>
#include <filesystem>
#include <iostream>
#include <limits>
#include <span>
#include <thread>
#include <unordered_map>
#include <vector>
#ifdef USE_SLANG
#include "_autogen/shader.comp.slang.h"
#include "_autogen/shader.rast.slang.h"
#include "_autogen/shader.light.slang.h"
#include "_autogen/shader.gbuffer.slang.h"
#include "_autogen/shader.depth_prepass.slang.h"
#include "_autogen/shader.depth_pyramid.slang.h"
#include "_autogen/shader.forward.slang.h"
#include "_autogen/shader.shadow.slang.h"
#include "_autogen/shader.debug.slang.h"
#include "_autogen/shader.light_culling.slang.h"
#include "_autogen/shader.gpu_culling.slang.h"
#include "_autogen/shader.shadow_culling.slang.h"
#else
#include "_autogen/shader.frag.glsl.h"
#include "_autogen/shader.vert.glsl.h"
#include "_autogen/shader.comp.glsl.h"
#endif

namespace shaderio {
using namespace glm;
#include "../shaders/shader_io.h"
}  // namespace shaderio

#ifdef NDEBUG
#define ASSERT(condition, message)                                                                                     \
  do                                                                                                                   \
  {                                                                                                                    \
    if(!(condition))                                                                                                   \
    {                                                                                                                  \
      throw std::runtime_error(message);                                                                               \
    }                                                                                                                  \
  } while(false)
#else
#define ASSERT(condition, message) assert((condition) && (message))
#endif

//--- Geometry -------------------------------------------------------------------------------------------------------------

/*--
 * Structure to hold the vertex data (see in shader_io.h), consisting only of a position, color and texture coordinates
 * Later we create a buffer with this data and use it to render a triangle.
-*/
struct Vertex : public shaderio::Vertex
{
  /*--
   * The binding description is used to describe at which rate to load data from memory throughout the vertices.
  -*/
  static auto getBindingDescription()
  {
    return std::to_array<VkVertexInputBindingDescription>(
        {{.binding = 0, .stride = sizeof(Vertex), .inputRate = VK_VERTEX_INPUT_RATE_VERTEX}});
  }

  /*--
   * The attribute descriptions describe how to extract a vertex attribute from
   * a chunk of vertex data originating from a binding description.
   * See in the vertex shader how the location is used to access the data.
  -*/
  static auto getAttributeDescriptions()
  {
    return std::to_array<VkVertexInputAttributeDescription>(
        {{.location = shaderio::LVPosition, .format = VK_FORMAT_R32G32B32_SFLOAT, .offset = uint32_t(offsetof(Vertex, position))},
         {.location = shaderio::LVColor, .format = VK_FORMAT_R32G32B32_SFLOAT, .offset = uint32_t(offsetof(Vertex, color))},
         {.location = shaderio::LVTexCoord, .format = VK_FORMAT_R32G32_SFLOAT, .offset = uint32_t(offsetof(Vertex, texCoord))}});
  }
};

// 2x3 vertices with a position, color and texCoords, make two CCW triangles
static const auto s_vertices = std::to_array<shaderio::Vertex>({
    {{0.0F, -0.5F, 0.5F}, {1.0F, 0.0F, 0.0F}, {0.5F, 0.5F}},  // Colored triangle
    {{-0.5F, 0.5F, 0.5F}, {0.0F, 0.0F, 1.0F}, {0.5F, 0.5F}},
    {{0.5F, 0.5F, 0.5F}, {0.0F, 1.0F, 0.0F}, {0.5F, 0.5F}},
    //
    {{0.1F, -0.4F, 0.75F}, {.3F, .3F, .3F}, {0.5F, 1.0F}},  // White triangle (textured)
    {{-0.4F, 0.6F, 0.25F}, {1.0F, 1.0F, 1.0F}, {1.0F, 0.0F}},
    {{0.6F, 0.6F, 0.75F}, {.7F, .7F, .7F}, {0.0F, 0.0F}},
});


// Points stored in a buffer and retrieved using buffer reference (flashing points)
static const auto s_points = std::to_array<glm::vec2>({{0.05F, 0.0F}, {-0.05F, 0.0F}, {0.0F, -0.05F}, {0.0F, 0.05F}});


//--- Vulkan Helpers ------------------------------------------------------------------------------------------------------------
#ifdef NDEBUG
#define VK_CHECK(vkFnc) vkFnc
#else
#define VK_CHECK(vkFnc)                                                                                                \
  {                                                                                                                    \
    if(const VkResult checkResult = (vkFnc); checkResult != VK_SUCCESS)                                                \
    {                                                                                                                  \
      const char* errMsg = string_VkResult(checkResult);                                                               \
      LOGE("Vulkan error: %s", errMsg);                                                                                \
      ASSERT(checkResult == VK_SUCCESS, errMsg);                                                                       \
    }                                                                                                                  \
  }
#endif

namespace utils {

/*--
 * A buffer is a region of memory used to store data.
 * It is used to store vertex data, index data, uniform data, and other types of data.
 * There is a VkBuffer object that represents the buffer, and a VmaAllocation object that represents the memory allocation.
 * The address is used to access the buffer in the shader.
-*/
struct Buffer
{
  VkBuffer        buffer{};      // Vulkan Buffer
  VmaAllocation   allocation{};  // Memory associated with the buffer
  VkDeviceAddress address{};     // Address of the buffer in the shader
  void*           mapped{};      // Persistent mapped pointer for host-visible allocations when available
};

/*--
 * An image is a region of memory used to store image data.
 * It is used to store texture data, framebuffer data, and other types of data.
-*/
struct Image
{
  VkImage       image{};       // Vulkan Image
  VmaAllocation allocation{};  // Memory associated with the image
};

/*-- 
 * The image resource is an image with an image view and a layout.
 * and other information like format and extent.
-*/
struct ImageResource : Image
{
  VkImageView   view{};    // Image view
  VkExtent2D    extent{};  // Size of the image
  VkImageLayout layout{};  // Layout of the image (color attachment, shader read, ...)
};

/*- Not implemented here -*/
struct AccelerationStructure
{
  VkAccelerationStructureKHR accel{};
  VmaAllocation              allocation{};
  VkDeviceAddress            deviceAddress{};
  VkDeviceSize               size{};
  Buffer                     buffer;  // Underlying buffer
};

/*--
 * A queue is a sequence of commands that are executed in order.
 * The queue is used to submit command buffers to the GPU.
 * The family index is used to identify the queue family (graphic, compute, transfer, ...) .
 * The queue index is used to identify the queue in the family, multiple queues can be in the same family.
-*/
struct QueueInfo
{
  uint32_t familyIndex = ~0U;  // Family index of the queue (graphic, compute, transfer, ...)
  uint32_t queueIndex  = ~0U;  // Index of the queue in the family
  VkQueue  queue{};            // The queue object
};

/*-- 
 * Combines hash values using the FNV-1a based algorithm 
-*/
static std::size_t hashCombine(std::size_t seed, auto const& value)
{
  return seed ^ (std::hash<std::decay_t<decltype(value)>>{}(value) + 0x9e3779b9 + (seed << 6) + (seed >> 2));
}

/*--
 * Initialize a newly created image to GENERAL layout (used for color/depth buffers)
-*/
static void cmdInitImageLayout(VkCommandBuffer cmd, VkImage image, VkImageAspectFlags aspectMask = VK_IMAGE_ASPECT_COLOR_BIT)
{
  const VkImageMemoryBarrier2 barrier{
      .sType         = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER_2,
      .srcStageMask  = VK_PIPELINE_STAGE_2_TOP_OF_PIPE_BIT,
      .srcAccessMask = VK_ACCESS_2_NONE,
      .dstStageMask  = VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT | VK_PIPELINE_STAGE_2_TRANSFER_BIT,
      .dstAccessMask = VK_ACCESS_2_MEMORY_READ_BIT | VK_ACCESS_2_MEMORY_WRITE_BIT | VK_ACCESS_2_TRANSFER_WRITE_BIT,
      .oldLayout     = VK_IMAGE_LAYOUT_UNDEFINED,
      .newLayout     = VK_IMAGE_LAYOUT_GENERAL,
      .srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
      .dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
      .image               = image,
      .subresourceRange    = {aspectMask, 0, VK_REMAINING_MIP_LEVELS, 0, VK_REMAINING_ARRAY_LAYERS}};

  const VkDependencyInfo depInfo{.sType = VK_STRUCTURE_TYPE_DEPENDENCY_INFO, .imageMemoryBarrierCount = 1, .pImageMemoryBarriers = &barrier};

  vkCmdPipelineBarrier2(cmd, &depInfo);
}

/*--
 * Transition swapchain image layout for the presentation/rendering cycle:
 * - UNDEFINED -> PRESENT_SRC_KHR (swapchain initialization)
 * - PRESENT_SRC_KHR <-> GENERAL (rendering cycle)
-*/
static void cmdTransitionSwapchainLayout(VkCommandBuffer cmd, VkImage image, VkImageLayout oldLayout, VkImageLayout newLayout)
{
  VkPipelineStageFlags2 srcStage = 0, dstStage = 0;
  VkAccessFlags2        srcAccess = 0, dstAccess = 0;

  if(oldLayout == VK_IMAGE_LAYOUT_UNDEFINED && newLayout == VK_IMAGE_LAYOUT_PRESENT_SRC_KHR)
  {
    // Swapchain initialization
    srcStage  = VK_PIPELINE_STAGE_2_TOP_OF_PIPE_BIT;
    srcAccess = VK_ACCESS_2_NONE;
    dstStage  = VK_PIPELINE_STAGE_2_COLOR_ATTACHMENT_OUTPUT_BIT;
    dstAccess = VK_ACCESS_2_NONE;
  }
  else if(oldLayout == VK_IMAGE_LAYOUT_PRESENT_SRC_KHR && newLayout == VK_IMAGE_LAYOUT_GENERAL)
  {
    // Before rendering
    srcStage  = VK_PIPELINE_STAGE_2_COLOR_ATTACHMENT_OUTPUT_BIT;
    srcAccess = VK_ACCESS_2_NONE;
    dstStage = VK_PIPELINE_STAGE_2_COLOR_ATTACHMENT_OUTPUT_BIT | VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT | VK_PIPELINE_STAGE_2_TRANSFER_BIT;
    dstAccess = VK_ACCESS_2_COLOR_ATTACHMENT_WRITE_BIT | VK_ACCESS_2_MEMORY_READ_BIT | VK_ACCESS_2_MEMORY_WRITE_BIT;
  }
  else if(oldLayout == VK_IMAGE_LAYOUT_GENERAL && newLayout == VK_IMAGE_LAYOUT_PRESENT_SRC_KHR)
  {
    // After rendering
    srcStage = VK_PIPELINE_STAGE_2_COLOR_ATTACHMENT_OUTPUT_BIT | VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT | VK_PIPELINE_STAGE_2_TRANSFER_BIT;
    srcAccess = VK_ACCESS_2_COLOR_ATTACHMENT_WRITE_BIT | VK_ACCESS_2_MEMORY_READ_BIT | VK_ACCESS_2_MEMORY_WRITE_BIT;
    dstStage  = VK_PIPELINE_STAGE_2_COLOR_ATTACHMENT_OUTPUT_BIT;
    dstAccess = VK_ACCESS_2_NONE;
  }
  else
  {
    ASSERT(false, "Unsupported swapchain layout transition!");
    srcStage = dstStage = VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT;
    srcAccess = dstAccess = VK_ACCESS_2_MEMORY_READ_BIT | VK_ACCESS_2_MEMORY_WRITE_BIT;
  }

  const VkImageMemoryBarrier2 barrier{.sType               = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER_2,
                                      .srcStageMask        = srcStage,
                                      .srcAccessMask       = srcAccess,
                                      .dstStageMask        = dstStage,
                                      .dstAccessMask       = dstAccess,
                                      .oldLayout           = oldLayout,
                                      .newLayout           = newLayout,
                                      .srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
                                      .dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
                                      .image               = image,
                                      .subresourceRange    = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1}};

  const VkDependencyInfo depInfo{.sType = VK_STRUCTURE_TYPE_DEPENDENCY_INFO, .imageMemoryBarrierCount = 1, .pImageMemoryBarriers = &barrier};

  vkCmdPipelineBarrier2(cmd, &depInfo);
}

/*-- 
 *  This helper returns the access mask for a given stage mask.
-*/
static VkAccessFlags2 inferAccessMaskFromStage(VkPipelineStageFlags2 stage, bool src)
{
  VkAccessFlags2 access = 0;

  // Shader stages: default to READ|WRITE for src (to flush writes), READ for dst (to consume)
  const bool hasCompute  = (stage & VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT) != 0;
  const bool hasFragment = (stage & VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT) != 0;
  const bool hasVertex   = (stage & VK_PIPELINE_STAGE_2_VERTEX_SHADER_BIT) != 0;
  if(hasCompute || hasFragment || hasVertex)
  {
    access |= src ? (VK_ACCESS_2_SHADER_READ_BIT | VK_ACCESS_2_SHADER_WRITE_BIT) : VK_ACCESS_2_SHADER_READ_BIT;
  }

  if((stage & VK_PIPELINE_STAGE_2_VERTEX_ATTRIBUTE_INPUT_BIT) != 0)
    access |= VK_ACCESS_2_VERTEX_ATTRIBUTE_READ_BIT;  // Always read-only
  if((stage & VK_PIPELINE_STAGE_2_TRANSFER_BIT) != 0)
    access |= src ? VK_ACCESS_2_TRANSFER_READ_BIT : VK_ACCESS_2_TRANSFER_WRITE_BIT;
  ASSERT(access != 0, "Missing stage implementation");
  return access;
}

/*--
 * This useful function simplifies the addition of buffer barriers, by inferring 
 * the access masks from the stage masks, and adding the buffer barrier to the command buffer.
-*/
static void cmdBufferMemoryBarrier(VkCommandBuffer       commandBuffer,
                                   VkBuffer              buffer,
                                   VkPipelineStageFlags2 srcStageMask,
                                   VkPipelineStageFlags2 dstStageMask,
                                   VkAccessFlags2        srcAccessMask       = 0,  // Default to infer if not provided
                                   VkAccessFlags2        dstAccessMask       = 0,  // Default to infer if not provided
                                   VkDeviceSize          offset              = 0,
                                   VkDeviceSize          size                = VK_WHOLE_SIZE,
                                   uint32_t              srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
                                   uint32_t              dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED)
{
  // Infer access masks if not explicitly provided
  if(srcAccessMask == 0)
  {
    srcAccessMask = inferAccessMaskFromStage(srcStageMask, true);
  }
  if(dstAccessMask == 0)
  {
    dstAccessMask = inferAccessMaskFromStage(dstStageMask, false);
  }

  const std::array<VkBufferMemoryBarrier2, 1> bufferBarrier{{{.sType        = VK_STRUCTURE_TYPE_BUFFER_MEMORY_BARRIER_2,
                                                              .srcStageMask = srcStageMask,
                                                              .srcAccessMask       = srcAccessMask,
                                                              .dstStageMask        = dstStageMask,
                                                              .dstAccessMask       = dstAccessMask,
                                                              .srcQueueFamilyIndex = srcQueueFamilyIndex,
                                                              .dstQueueFamilyIndex = dstQueueFamilyIndex,
                                                              .buffer              = buffer,
                                                              .offset              = offset,
                                                              .size                = size}}};

  const VkDependencyInfo depInfo{.sType                    = VK_STRUCTURE_TYPE_DEPENDENCY_INFO,
                                 .bufferMemoryBarrierCount = uint32_t(bufferBarrier.size()),
                                 .pBufferMemoryBarriers    = bufferBarrier.data()};
  vkCmdPipelineBarrier2(commandBuffer, &depInfo);
}


/*--
 * A helper function to find a supported format from a list of candidates.
 * For example, we can use this function to find a supported depth format.
-*/
static VkFormat findSupportedFormat(VkPhysicalDevice             physicalDevice,
                                    const std::vector<VkFormat>& candidates,
                                    VkImageTiling                tiling,
                                    VkFormatFeatureFlags2        features)
{
  for(const VkFormat format : candidates)
  {
    VkFormatProperties2 props{.sType = VK_STRUCTURE_TYPE_FORMAT_PROPERTIES_2};
    vkGetPhysicalDeviceFormatProperties2(physicalDevice, format, &props);

    if(tiling == VK_IMAGE_TILING_LINEAR && (props.formatProperties.linearTilingFeatures & features) == features)
    {
      return format;
    }
    if(tiling == VK_IMAGE_TILING_OPTIMAL && (props.formatProperties.optimalTilingFeatures & features) == features)
    {
      return format;
    }
  }
  ASSERT(false, "failed to find supported format!");
  return VK_FORMAT_UNDEFINED;
}

/*--
 * A helper function to find the depth format that is supported by the physical device.
-*/
static VkFormat findDepthFormat(VkPhysicalDevice physicalDevice)
{
  return findSupportedFormat(physicalDevice,
                             {VK_FORMAT_D32_SFLOAT, VK_FORMAT_D16_UNORM, VK_FORMAT_D32_SFLOAT_S8_UINT, VK_FORMAT_D24_UNORM_S8_UINT},
                             VK_IMAGE_TILING_OPTIMAL, VK_FORMAT_FEATURE_DEPTH_STENCIL_ATTACHMENT_BIT);
}

/*--
 * A helper function to create a shader module from a Spir-V code.
-*/
static VkShaderModule createShaderModule(VkDevice device, const std::span<const uint32_t>& code)
{
  const VkShaderModuleCreateInfo createInfo{.sType    = VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO,
                                            .codeSize = code.size() * sizeof(uint32_t),
                                            .pCode    = static_cast<const uint32_t*>(code.data())};
  VkShaderModule                 shaderModule{};
  VK_CHECK(vkCreateShaderModule(device, &createInfo, nullptr, &shaderModule));
  return shaderModule;
}

//--- Command Buffer ------------------------------------------------------------------------------------------------------------

/*-- Simple helper for the creation of a temporary command buffer, use to record the commands to upload data, or transition images. -*/
static VkCommandBuffer beginSingleTimeCommands(VkDevice device, VkCommandPool cmdPool)
{
  const VkCommandBufferAllocateInfo allocInfo{.sType              = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO,
                                              .commandPool        = cmdPool,
                                              .level              = VK_COMMAND_BUFFER_LEVEL_PRIMARY,
                                              .commandBufferCount = 1};
  VkCommandBuffer                   cmd{};
  VK_CHECK(vkAllocateCommandBuffers(device, &allocInfo, &cmd));
  const VkCommandBufferBeginInfo beginInfo{.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO,
                                           .flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT};
  VK_CHECK(vkBeginCommandBuffer(cmd, &beginInfo));
  return cmd;
}
/*-- 
 * Submit the temporary command buffer, wait until the command is finished, and clean up. 
 * This is a blocking function and should be used only for small operations 
--*/
static void endSingleTimeCommands(VkCommandBuffer cmd, VkDevice device, VkCommandPool cmdPool, VkQueue queue)
{
  // Submit and clean up
  VK_CHECK(vkEndCommandBuffer(cmd));

  // Create fence for synchronization
  const VkFenceCreateInfo fenceInfo{.sType = VK_STRUCTURE_TYPE_FENCE_CREATE_INFO};
  std::array<VkFence, 1>  fence{};
  VK_CHECK(vkCreateFence(device, &fenceInfo, nullptr, fence.data()));

  const VkCommandBufferSubmitInfo cmdBufferInfo{.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_SUBMIT_INFO, .commandBuffer = cmd};
  const std::array<VkSubmitInfo2, 1> submitInfo{
      {{.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO_2, .commandBufferInfoCount = 1, .pCommandBufferInfos = &cmdBufferInfo}}};
  VK_CHECK(vkQueueSubmit2(queue, uint32_t(submitInfo.size()), submitInfo.data(), fence[0]));
  VK_CHECK(vkWaitForFences(device, uint32_t(fence.size()), fence.data(), VK_TRUE, UINT64_MAX));

  // Cleanup
  vkDestroyFence(device, fence[0], nullptr);
  vkFreeCommandBuffers(device, cmdPool, 1, &cmd);
}

// Helper to chain Vulkan structures to the pNext chain
// Uses VkBaseOutStructure for type-safe chaining following Vulkan conventions
template <typename MainT, typename NewT>
static void pNextChainPushFront(MainT* mainStruct, NewT* newStruct)
{
  // Cast to VkBaseOutStructure for proper pNext handling
  auto* newBase  = reinterpret_cast<VkBaseOutStructure*>(newStruct);
  auto* mainBase = reinterpret_cast<VkBaseOutStructure*>(mainStruct);

  newBase->pNext  = mainBase->pNext;
  mainBase->pNext = newBase;
}

// Validation settings: to fine tune what is checked
struct ValidationSettings
{
  VkBool32 fine_grained_locking{VK_TRUE};
  VkBool32 validate_core{VK_TRUE};
  VkBool32 check_image_layout{VK_TRUE};
  VkBool32 check_command_buffer{VK_TRUE};
  VkBool32 check_object_in_use{VK_TRUE};
  VkBool32 check_query{VK_TRUE};
  VkBool32 check_shaders{VK_TRUE};
  VkBool32 check_shaders_caching{VK_TRUE};
  VkBool32 unique_handles{VK_TRUE};
  VkBool32 object_lifetime{VK_TRUE};
  VkBool32 stateless_param{VK_TRUE};
  std::vector<const char*> debug_action{"VK_DBG_LAYER_ACTION_LOG_MSG", "VK_DBG_LAYER_ACTION_BREAK"};  // Log and break on validation errors
  std::vector<const char*> report_flags{"error", "warn"};  // Enable both errors and warnings
  std::vector<const char*> message_id_filter{
      "WARNING-legacy-gpdp2",
      "VUID-VkPrivateDataSlotCreateInfo-flags-zerobitmask",
      "VUID-vkCmdPipelineBarrier-pImageMemoryBarriers-02819",
      "VUID-vkCmdPipelineBarrier-pImageMemoryBarriers-02820",  // Nsight injection: access/stage mask mismatch
      "VUID-vkCmdDraw-None-09600",  // RenderDoc injection: swapchain layout mismatch in captured descriptor
      "VUID-vkBindBufferMemory-memory-02985",  // Nsight injection: external memory import mismatch
      "VUID-vkGetBufferOpaqueCaptureAddress-pInfo-10725",  // Nsight injection: buffer address capture replay
      "VUID-VkMemoryAllocateInfo-pNext-02806",  // Nsight injection: dedicated buffer + host import conflict
      "VUID-VkPresentInfoKHR-pImageIndices-01430",  // RenderDoc/Nsight injection: swapchain layout mismatch
      "VUID-vkCmdCopyBuffer-renderpass",  // Nsight capture: injected staging copy during active dynamic rendering
      "VUID-vkResetFences-pFences-01123",  // RenderDoc capture: injected fence reset can race captured queue work
      "76fd6b",  // Nsight: same error in hex format
      "90590b31",  // Nsight: VUID-vkBindBufferMemory-memory-02985 hex
      "48a6c111",  // Nsight: VUID-vkGetBufferOpaqueCaptureAddress-pInfo-10725 hex
      "af8d561d",  // Nsight: VUID-vkCmdPipelineBarrier-pImageMemoryBarriers-02820 hex
      "48ad24c6",  // RenderDoc/Nsight: VUID-VkPresentInfoKHR-pImageIndices-01430 hex
      "7cb9a424",  // Nsight: VUID-vkCmdCopyBuffer-renderpass hex
      "68a5074e"  // RenderDoc capture: VUID-vkResetFences-pFences-01123 hex
  };


  VkBaseInStructure* buildPNextChain()
  {
    layerSettings = std::vector<VkLayerSettingEXT>{
        {layerName, "fine_grained_locking", VK_LAYER_SETTING_TYPE_BOOL32_EXT, 1, &fine_grained_locking},
        {layerName, "validate_core", VK_LAYER_SETTING_TYPE_BOOL32_EXT, 1, &validate_core},
        {layerName, "check_image_layout", VK_LAYER_SETTING_TYPE_BOOL32_EXT, 1, &check_image_layout},
        {layerName, "check_command_buffer", VK_LAYER_SETTING_TYPE_BOOL32_EXT, 1, &check_command_buffer},
        {layerName, "check_object_in_use", VK_LAYER_SETTING_TYPE_BOOL32_EXT, 1, &check_object_in_use},
        {layerName, "check_query", VK_LAYER_SETTING_TYPE_BOOL32_EXT, 1, &check_query},
        {layerName, "check_shaders", VK_LAYER_SETTING_TYPE_BOOL32_EXT, 1, &check_shaders},
        {layerName, "check_shaders_caching", VK_LAYER_SETTING_TYPE_BOOL32_EXT, 1, &check_shaders_caching},
        {layerName, "unique_handles", VK_LAYER_SETTING_TYPE_BOOL32_EXT, 1, &unique_handles},
        {layerName, "object_lifetime", VK_LAYER_SETTING_TYPE_BOOL32_EXT, 1, &object_lifetime},
        {layerName, "stateless_param", VK_LAYER_SETTING_TYPE_BOOL32_EXT, 1, &stateless_param},
        {layerName, "debug_action", VK_LAYER_SETTING_TYPE_STRING_EXT, uint32_t(debug_action.size()), debug_action.data()},
        {layerName, "report_flags", VK_LAYER_SETTING_TYPE_STRING_EXT, uint32_t(report_flags.size()), report_flags.data()},
        {layerName, "message_id_filter", VK_LAYER_SETTING_TYPE_STRING_EXT, uint32_t(message_id_filter.size()),
         message_id_filter.data()},

    };
    layerSettingsCreateInfo = {
        .sType        = VK_STRUCTURE_TYPE_LAYER_SETTINGS_CREATE_INFO_EXT,
        .settingCount = uint32_t(layerSettings.size()),
        .pSettings    = layerSettings.data(),
    };

    return reinterpret_cast<VkBaseInStructure*>(&layerSettingsCreateInfo);
  }

  static constexpr const char*   layerName{"VK_LAYER_KHRONOS_validation"};
  std::vector<VkLayerSettingEXT> layerSettings;
  VkLayerSettingsCreateInfoEXT   layerSettingsCreateInfo{};
};

//--- Vulkan Context Configuration ------------------------------------------------------------------------------------------------------------

/*--
 * Configuration for device extensions.
 * - name: The extension name (e.g., VK_KHR_SWAPCHAIN_EXTENSION_NAME)
 * - required: If true, the application will assert if the extension is not available
 * - featureStruct: Pointer to the feature structure to enable (or nullptr if no feature struct needed)
-*/
struct ExtensionConfig
{
  const char* name          = nullptr;
  bool        required      = false;
  void*       featureStruct = nullptr;
};

/*--
 * Configuration structure for Context initialization.
 * This allows customization of instance/device extensions, layers, and features.
 * 
 * Usage:
 *   ContextCreateInfo config;
 *   config.deviceExtensions.push_back({VK_KHR_RAY_TRACING_PIPELINE_EXTENSION_NAME, true, &rtFeatures, VK_STRUCTURE_TYPE_...});
 *   context.init(config);
-*/
struct ContextCreateInfo
{
  // Instance configuration
  std::vector<const char*> instanceExtensions = {VK_KHR_GET_SURFACE_CAPABILITIES_2_EXTENSION_NAME};
  std::vector<const char*> instanceLayers;

  // API version
  uint32_t apiVersion = VK_API_VERSION_1_4;

  // Validation layers
#ifdef NDEBUG
  bool enableValidationLayers = false;
#else
  bool enableValidationLayers = true;
#endif

  // Device extensions with their configuration
  // Note: These are the extensions that will be requested from the device
  // The Context will check availability and enable them based on the 'required' flag
  std::vector<ExtensionConfig> deviceExtensions;
};

static std::string findFile(const std::string& filename, const std::vector<std::string>& searchPaths)
{
  for(const auto& path : searchPaths)
  {
    const std::filesystem::path filePath = std::filesystem::path(path) / filename;
    if(std::filesystem::exists(filePath))
    {
      return filePath.string();
    }
  }
  LOGE("File not found: %s", filename.c_str());
  LOGI("Search under: ");
  for(const auto& path : searchPaths)
  {
    LOGI("  %s", path.c_str());
  }
  return "";
}

//--- Normal Packing Utilities (RGB10A2) ------------------------------------------------------------------------------------------------

/*--
 * Pack a world-space normal [-1,1] to RGB10A2 format (10 bits per channel, 2 bits alpha)
 * This provides higher precision than RGBA8 for normal storage
--*/
inline uint32_t packNormalRGB10A2(const glm::vec3& normal)
{
  const uint32_t x = static_cast<uint32_t>(glm::clamp(normal.x * 0.5f + 0.5f, 0.0f, 1.0f) * 1023.0f);
  const uint32_t y = static_cast<uint32_t>(glm::clamp(normal.y * 0.5f + 0.5f, 0.0f, 1.0f) * 1023.0f);
  const uint32_t z = static_cast<uint32_t>(glm::clamp(normal.z * 0.5f + 0.5f, 0.0f, 1.0f) * 1023.0f);
  return (z << 20) | (y << 10) | x;
}

/*--
 * Unpack a normal from RGB10A2 format back to world-space [-1,1]
--*/
inline glm::vec3 unpackNormalRGB10A2(uint32_t packed)
{
  const float x = static_cast<float>(packed & 0x3FF) / 1023.0f * 2.0f - 1.0f;
  const float y = static_cast<float>((packed >> 10) & 0x3FF) / 1023.0f * 2.0f - 1.0f;
  const float z = static_cast<float>((packed >> 20) & 0x3FF) / 1023.0f * 2.0f - 1.0f;
  return glm::normalize(glm::vec3(x, y, z));
}

}  // namespace utils
