#pragma once

// D-07 backend-internal interop interface: single carrier for all native handle
// passthrough and resolve* access.
// Public rhi::Device interface has no corresponding getter/resolve*; legitimate
// callers must static_cast rhi::Device* to VulkanDeviceInterop* — only files
// approved in .rhi-boundary-allow may do this.
//
// Usage:
//   - render/SceneResources.cpp, render/IBLResources.cpp, render/GPUDrivenRenderer.cpp
//     obtain typed Vulkan handles for debug-naming / descriptor writes via
//     resolveTexture/resolveTextureView/resolveSampler — no double reinterpret_cast.
//   - Swapchain init / debug bridge obtain underlying Vulkan objects via
//     nativeInstance/nativePhysicalDevice/nativeDevice without going through
//     public getBackend* methods.

#include "../RHIHandles.h"

#include <vulkan/vulkan.h>

namespace demo::rhi::vulkan
{

// VulkanDeviceInterop: Vulkan backend-internal native passthrough interface.
//
// Two groups of pure-virtual accessors:
//   1. Native device accessors (4): replace public getBackend* / getFeaturesChainHead.
//   2. Resolve accessors (3): replace public resolveTexture*/resolveSamplerBackendHandle,
//      returning typed Vulkan handles so consumers need no secondary reinterpret_cast.
//
// VulkanDevice multiply-inherits this interface and overrides all methods.
class VulkanDeviceInterop
{
public:
    virtual ~VulkanDeviceInterop() = default;

    // -------------------------------------------------------------------------
    // Native device accessors (D-07)
    // -------------------------------------------------------------------------

    // Returns the Vulkan instance handle (replaces public getBackendInstanceHandle).
    [[nodiscard]] virtual VkInstance nativeInstance() const = 0;

    // Returns the Vulkan physical device handle (replaces public getBackendPhysicalDeviceHandle).
    [[nodiscard]] virtual VkPhysicalDevice nativePhysicalDevice() const = 0;

    // Returns the Vulkan logical device handle (replaces public getBackendDeviceHandle).
    [[nodiscard]] virtual VkDevice nativeDevice() const = 0;

    // Returns the feature pNext chain head pointer (typed, replaces getFeaturesChainHead).
    [[nodiscard]] virtual VkBaseOutStructure* nativeFeaturesChainHead() const = 0;

    // -------------------------------------------------------------------------
    // Resolve accessors (D-07 / resolve* sink)
    // -------------------------------------------------------------------------

    // Resolves an RHI TextureHandle to VkImage (typed, replaces resolveTextureBackendHandle).
    [[nodiscard]] virtual VkImage resolveTexture(rhi::TextureHandle handle) const = 0;

    // Resolves an RHI TextureViewHandle to VkImageView (typed, replaces resolveTextureViewBackendHandle).
    [[nodiscard]] virtual VkImageView resolveTextureView(rhi::TextureViewHandle handle) const = 0;

    // Resolves an RHI SamplerHandle to VkSampler (typed, replaces resolveSamplerBackendHandle).
    [[nodiscard]] virtual VkSampler resolveSampler(rhi::SamplerHandle handle) const = 0;
};

} // namespace demo::rhi::vulkan
