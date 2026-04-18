#pragma once

#ifdef TRACY_ENABLE

#include <cstddef>
#include <volk.h>  // volk provides Vulkan function pointers

// Tracy needs to use symbol table for Vulkan function access when VK_NO_PROTOTYPES is defined
#define TRACY_VK_USE_SYMBOL_TABLE

#include <tracy/Tracy.hpp>
#include <tracy/TracyVulkan.hpp>

namespace demo {
namespace profiling {

// Tracy Vulkan context wrapper
// Uses Tracy's TracyVkContext macro for proper initialization
class TracyVulkanContext
{
public:
  TracyVulkanContext(VkInstance instance, VkPhysicalDevice physicalDevice, VkDevice device, VkQueue queue, VkCommandBuffer cmdBuffer)
#ifdef TRACY_VK_USE_SYMBOL_TABLE
    : m_ctx(tracy::CreateVkContext(instance, physicalDevice, device, queue, cmdBuffer,
                                   vkGetInstanceProcAddr, vkGetDeviceProcAddr))
#else
    : m_ctx(tracy::CreateVkContext(physicalDevice, device, queue, cmdBuffer, nullptr, nullptr))
#endif
  {
  }

  ~TracyVulkanContext()
  {
    if(m_ctx)
    {
      tracy::DestroyVkContext(m_ctx);
    }
  }

  tracy::VkCtx* context() const { return m_ctx; }

private:
  tracy::VkCtx* m_ctx;
};

// CPU zone macros
#define TRACY_CPU_ZONE(name) TracyCZoneN(name, true)
#define TRACY_CPU_ZONE_END(name) TracyCZoneEnd(name)
#define TRACY_CPU_FRAME_MARK() FrameMark

// GPU zone macros (requires TracyVulkanContext)
#define TRACY_GPU_ZONE(ctx, cmdBuf, name) TracyVkZone(ctx->context(), cmdBuf, name)
#define TRACY_GPU_ZONE_TRANSIENT(ctx, cmdBuf, name) TracyVkZoneTransient(ctx->context(), tracy_gpu_transient_zone, cmdBuf, name, true)
#define TRACY_GPU_COLLECT(ctx, cmdBuf) TracyVkCollect(ctx->context(), cmdBuf)

// Named scoped zones
#define TRACY_ZONE_SCOPED(name) ZoneScopedN(name)
#define TRACY_ZONE_TEXT(text) ZoneText(text)

}  // namespace profiling
}  // namespace demo

#else

// No-op versions when Tracy is disabled
namespace demo {
namespace profiling {

class TracyVulkanContext
{
public:
  TracyVulkanContext(void*, void*, void*, void*, void*) {}
  void* context() const { return nullptr; }
};

#define TRACY_CPU_ZONE(name)
#define TRACY_CPU_ZONE_END(name)
#define TRACY_CPU_FRAME_MARK()
#define TRACY_GPU_ZONE(ctx, cmdBuf, name)
#define TRACY_GPU_ZONE_TRANSIENT(ctx, cmdBuf, name)
#define TRACY_GPU_COLLECT(ctx, cmdBuf)
#define TRACY_ZONE_SCOPED(name)
#define TRACY_ZONE_TEXT(text)

}  // namespace profiling
}  // namespace demo

#endif