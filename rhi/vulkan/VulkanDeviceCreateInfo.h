#pragma once

// D-08 sink carrier: Vulkan-shaped fields from rhi::DeviceCreateInfo (extension/layer
// requests, featuresStruct) are moved into this backend-internal struct.
// Application-layer Vulkan init paths use VulkanDeviceCreateInfo instead of the
// removed fields in rhi::DeviceCreateInfo.
// D3D12/Metal build paths must not include this header (file lives in rhi/vulkan/).

#include "../RHIDevice.h"

#include <vector>

namespace demo::rhi::vulkan
{

// Vulkan-specific extension/layer request (isomorphic to VulkanCommon.h's ExtensionConfig).
// name:           Extension name (e.g. VK_KHR_SWAPCHAIN_EXTENSION_NAME)
// required:       If true, abort when the extension is unavailable
// featuresStruct: Pointer to the corresponding pNext feature struct (nullptr if not needed)
struct ExtensionRequest
{
    const char* name{nullptr};
    bool        required{false};
    void*       featuresStruct{nullptr};
};

// Vulkan device creation parameters (D-08 sink struct).
// base:               Backend-neutral public fields (CapabilityRequirements + enableValidationLayers)
// deviceExtensions:   Vulkan device extension request list (with optional featureStruct pNext chain)
// instanceExtensions: Vulkan instance extension name list
// instanceLayers:     Vulkan instance layer name list (e.g. VK_LAYER_KHRONOS_validation)
struct VulkanDeviceCreateInfo
{
    rhi::DeviceCreateInfo          base{};
    std::vector<ExtensionRequest>  deviceExtensions;
    std::vector<const char*>       instanceExtensions;
    std::vector<const char*>       instanceLayers;
};

} // namespace demo::rhi::vulkan
