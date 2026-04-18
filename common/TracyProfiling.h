#pragma once

#ifdef TRACY_ENABLE

#include <cstddef>  // for strlen
#include <tracy/Tracy.hpp>
#include <tracy/TracyVulkan.hpp>

namespace demo {
namespace profiling {

// Tracy Vulkan context wrapper
class TracyVulkanContext
{
public:
  TracyVulkanContext(VkPhysicalDevice physicalDevice, VkDevice device, VkQueue queue, VkCommandPool cmdPool)
    : m_ctx(tracy::CreateVkContext(physicalDevice, device, queue, cmdPool))
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
#define TRACY_GPU_ZONE_END(ctx, cmdBuf) TracyVkZoneEnd(ctx->context(), cmdBuf)
#define TRACY_GPU_COLLECT(ctx) TracyVkCollect(ctx->context())

// Named scoped zones
#define TRACY_ZONE_SCOPED(name) ZoneScopedN(name)
#define TRACY_ZONE_TEXT(text) ZoneText(text, strlen(text))

}  // namespace profiling
}  // namespace demo

#else

// No-op versions when Tracy is disabled
namespace demo {
namespace profiling {

class TracyVulkanContext
{
public:
  TracyVulkanContext(void*, void*, void*, void*) {}
  tracy::VkCtx* context() const { return nullptr; }
};

#define TRACY_CPU_ZONE(name)
#define TRACY_CPU_ZONE_END(name)
#define TRACY_CPU_FRAME_MARK()
#define TRACY_GPU_ZONE(ctx, cmdBuf, name)
#define TRACY_GPU_ZONE_END(ctx, cmdBuf)
#define TRACY_GPU_COLLECT(ctx)
#define TRACY_ZONE_SCOPED(name)
#define TRACY_ZONE_TEXT(text)

}  // namespace profiling
}  // namespace demo

#endif