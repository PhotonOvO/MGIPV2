#pragma once

#include "../common/Common.h"
#include "../rhi/RHIResourceLifetime.h"

namespace utils {

class ResourceAllocator
{
public:
  enum class OwnershipSeam : uint8_t
  {
    LegacyRendererOwnsPhysicalLifetime = 0,
    PortableRhiOwnsPhysicalLifetime,
  };

  ResourceAllocator() = default;
  ~ResourceAllocator() { assert(m_allocator == nullptr && "Missing deinit()"); }
  operator VmaAllocator() const { return m_allocator; }

  [[nodiscard]] static constexpr OwnershipSeam ownershipSeam()
  {
    return OwnershipSeam::LegacyRendererOwnsPhysicalLifetime;
  }
  [[nodiscard]] static constexpr demo::rhi::OwnershipSplit portableOwnershipSplit()
  {
    return demo::rhi::OwnershipSplit{true, true};
  }
  [[nodiscard]] static constexpr demo::rhi::ResourceLifetimeTier defaultLifetimeTier()
  {
    return demo::rhi::ResourceLifetimeTier::Device;
  }

  // Initialization of VMA allocator.
  void init(VmaAllocatorCreateInfo allocatorInfo)
  {
    // #TODO : VK_EXT_memory_priority ? VMA_ALLOCATOR_CREATE_EXT_MEMORY_PRIORITY_BIT

    allocatorInfo.flags |= VMA_ALLOCATOR_CREATE_BUFFER_DEVICE_ADDRESS_BIT;  // allow querying for the GPU address of a buffer
    allocatorInfo.flags |= VMA_ALLOCATOR_CREATE_KHR_MAINTENANCE4_BIT;
    allocatorInfo.flags |= VMA_ALLOCATOR_CREATE_KHR_MAINTENANCE5_BIT;  // allow using VkBufferUsageFlags2CreateInfoKHR

    m_device = allocatorInfo.device;
    // Because we use VMA_DYNAMIC_VULKAN_FUNCTIONS
    const VmaVulkanFunctions functions = {
        .vkGetInstanceProcAddr = vkGetInstanceProcAddr,
        .vkGetDeviceProcAddr   = vkGetDeviceProcAddr,
    };
    allocatorInfo.pVulkanFunctions = &functions;
    vmaCreateAllocator(&allocatorInfo, &m_allocator);
  }

  // De-initialization of VMA allocator.
  void deinit()
  {
    if(!m_stagingBuffers.empty())
      LOGW("Warning: Staging buffers were not freed before destroying the allocator");
    freeStagingBuffers();
    vmaDestroyAllocator(m_allocator);
    *this = {};
  }

  /*-- Create a buffer -*/
  /* 
   * UBO: VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT
   *        + VMA_MEMORY_USAGE_CPU_TO_GPU
   * SSBO: VK_BUFFER_USAGE_STORAGE_BUFFER_BIT
   *        + VMA_MEMORY_USAGE_CPU_TO_GPU // Use this if the CPU will frequently update the buffer
   *        + VMA_MEMORY_USAGE_GPU_ONLY // Use this if the CPU will rarely update the buffer
   *        + VMA_MEMORY_USAGE_GPU_TO_CPU  // Use this when you need to read back data from the SSBO to the CPU
   *      ----
   *        + VMA_ALLOCATION_CREATE_MAPPED_BIT // Automatically maps the buffer upon creation
   *        + VMA_ALLOCATION_CREATE_HOST_ACCESS_SEQUENTIAL_WRITE_BIT // If the CPU will sequentially write to the buffer's memory,
   */
  Buffer createBuffer(VkDeviceSize             size,
                      VkBufferUsageFlags2KHR   usage,
                      VmaMemoryUsage           memoryUsage = VMA_MEMORY_USAGE_AUTO,
                      VmaAllocationCreateFlags flags       = {}) const
  {
    // This can be used only with maintenance5
    const VkBufferUsageFlags2CreateInfoKHR bufferUsageFlags2CreateInfo{
        .sType = VK_STRUCTURE_TYPE_BUFFER_USAGE_FLAGS_2_CREATE_INFO_KHR,
        .usage = usage | VK_BUFFER_USAGE_2_SHADER_DEVICE_ADDRESS_BIT_KHR,
    };

    const VkBufferCreateInfo bufferInfo{
        .sType       = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO,
        .pNext       = &bufferUsageFlags2CreateInfo,
        .size        = size,
        .usage       = 0,
        .sharingMode = VK_SHARING_MODE_EXCLUSIVE,  // Only one queue family will access i
    };

    VmaAllocationCreateInfo allocInfo              = {.flags = flags, .usage = memoryUsage};
    const VkDeviceSize      dedicatedMemoryMinSize = 64ULL * 1024;  // 64 KB
    if(size > dedicatedMemoryMinSize)
    {
      allocInfo.flags |= VMA_ALLOCATION_CREATE_DEDICATED_MEMORY_BIT;  // Use dedicated memory for large buffers
    }

    // Create the buffer
    Buffer            resultBuffer;
    VmaAllocationInfo allocInfoOut{};
    VK_CHECK(vmaCreateBuffer(m_allocator, &bufferInfo, &allocInfo, &resultBuffer.buffer, &resultBuffer.allocation, &allocInfoOut));

    // Get the GPU address of the buffer
    const VkBufferDeviceAddressInfo info = {.sType  = VK_STRUCTURE_TYPE_BUFFER_DEVICE_ADDRESS_INFO,
                                            .buffer = resultBuffer.buffer};
    resultBuffer.address                 = vkGetBufferDeviceAddress(m_device, &info);

    {  // Find leaks
      static uint32_t counter = 0U;
      if(m_leakID == counter)
      {
#if defined(_MSVC_LANG)
        __debugbreak();
#endif
      }
      std::string allocID = std::string("allocID: ") + std::to_string(counter++);
      vmaSetAllocationName(m_allocator, resultBuffer.allocation, allocID.c_str());
    }

    return resultBuffer;
  }

  //*-- Destroy a buffer -*/
  void destroyBuffer(Buffer buffer) const { vmaDestroyBuffer(m_allocator, buffer.buffer, buffer.allocation); }

  /*--
   * Create a staging buffer, copy data into it, and track it.
   * This method accepts data, handles the mapping, copying, and unmapping
   * automatically.
  -*/
  template <typename T>
  Buffer createStagingBuffer(const std::span<T>& vectorData)
  {
    const VkDeviceSize bufferSize = sizeof(T) * vectorData.size();

    // Create a staging buffer
    Buffer stagingBuffer = createBuffer(bufferSize, VK_BUFFER_USAGE_2_TRANSFER_SRC_BIT_KHR, VMA_MEMORY_USAGE_CPU_TO_GPU,
                                        VMA_ALLOCATION_CREATE_HOST_ACCESS_SEQUENTIAL_WRITE_BIT);

    // Track the staging buffer for later cleanup
    m_stagingBuffers.push_back(stagingBuffer);

    // Map and copy data to the staging buffer
    void* data = nullptr;
    vmaMapMemory(m_allocator, stagingBuffer.allocation, &data);
    memcpy(data, vectorData.data(), (size_t)bufferSize);
    vmaUnmapMemory(m_allocator, stagingBuffer.allocation);
    return stagingBuffer;
  }

  /*--
   * Create a buffer (GPU only) with data, this is done using a staging buffer
   * The staging buffer is a buffer that is used to transfer data from the CPU
   * to the GPU.
   * and cannot be freed until the data is transferred. So the command buffer
   * must be submitted, then
   * the staging buffer can be cleared using the freeStagingBuffers function.
  -*/
  template <typename T>
  Buffer createBufferAndUploadData(VkCommandBuffer cmd, const std::span<T>& vectorData, VkBufferUsageFlags2KHR usageFlags)
  {
    // Create staging buffer and upload data
    Buffer stagingBuffer = createStagingBuffer(vectorData);

    // Create the final buffer in GPU memory
    const VkDeviceSize bufferSize = sizeof(T) * vectorData.size();
    Buffer buffer = createBuffer(bufferSize, usageFlags | VK_BUFFER_USAGE_2_TRANSFER_DST_BIT_KHR, VMA_MEMORY_USAGE_GPU_ONLY);

    const std::array<VkBufferCopy, 1> copyRegion{{{.size = bufferSize}}};
    vkCmdCopyBuffer(cmd, stagingBuffer.buffer, buffer.buffer, uint32_t(copyRegion.size()), copyRegion.data());

    return buffer;
  }

  /*--
   * Create an image in GPU memory. This does not adding data to the image.
   * This is only creating the image in GPU memory.
   * See createImageAndUploadData for creating an image and uploading data.
  -*/
  Image createImage(const VkImageCreateInfo& imageInfo) const
  {
    const VmaAllocationCreateInfo createInfo{.usage = VMA_MEMORY_USAGE_GPU_ONLY};

    Image             image;
    VmaAllocationInfo allocInfo{};
    VK_CHECK(vmaCreateImage(m_allocator, &imageInfo, &createInfo, &image.image, &image.allocation, &allocInfo));
    return image;
  }

  /*-- Destroy image --*/
  void destroyImage(Image& image) const { vmaDestroyImage(m_allocator, image.image, image.allocation); }

  void destroyImageResource(ImageResource& imageRessource) const
  {
    destroyImage(imageRessource);
    vkDestroyImageView(m_device, imageRessource.view, nullptr);
  }

  /*-- Create an image and upload data using a staging buffer --*/
  template <typename T>
  ImageResource createImageAndUploadData(VkCommandBuffer cmd, const std::span<T>& vectorData, const VkImageCreateInfo& _imageInfo, VkImageLayout finalLayout)
  {
    // Create staging buffer and upload data
    Buffer stagingBuffer = createStagingBuffer(vectorData);

    // Create image in GPU memory
    VkImageCreateInfo imageInfo = _imageInfo;
    imageInfo.usage |= VK_IMAGE_USAGE_TRANSFER_DST_BIT;  // We will copy data to this image
    Image image = createImage(imageInfo);

    // Transition image layout for copying data
    cmdInitImageLayout(cmd, image.image);

    // Copy buffer data to the image
    const std::array<VkBufferImageCopy, 1> copyRegion{
        {{.imageSubresource = {.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT, .layerCount = 1}, .imageExtent = imageInfo.extent}}};

    vkCmdCopyBufferToImage(cmd, stagingBuffer.buffer, image.image, VK_IMAGE_LAYOUT_GENERAL, uint32_t(copyRegion.size()),
                           copyRegion.data());

    ImageResource resultImage(image);
    resultImage.layout = finalLayout;
    return resultImage;
  }

  /*--
   * The staging buffers are buffers that are used to transfer data from the CPU to the GPU.
   * They cannot be freed until the data is transferred. So the command buffer must be completed, then the staging buffer can be cleared.
  -*/
  void freeStagingBuffers()
  {
    for(const auto& buffer : m_stagingBuffers)
    {
      destroyBuffer(buffer);
    }
    m_stagingBuffers.clear();
  }

  /*-- When leak are reported, set the ID of the leak here --*/
  void setLeakID(uint32_t id) { m_leakID = id; }

private:
  VmaAllocator        m_allocator{};
  VkDevice            m_device{};
  std::vector<Buffer> m_stagingBuffers;
  uint32_t            m_leakID = ~0U;
};

}  // namespace utils
