#pragma once

// D-08 下沉载体：将 rhi::DeviceCreateInfo 中 Vulkan 形状字段（extension/layer 请求、
// featuresStruct）下沉到此 backend-internal struct。
// 应用层 Vulkan 路径用 VulkanDeviceCreateInfo 代替原 rhi::DeviceCreateInfo 的 Vulkan 字段部分。
// D3D12/Metal 构建路径不应 include 此头文件（文件置于 rhi/vulkan/ 目录下）。

#include "../RHIDevice.h"

#include <vector>

namespace demo::rhi::vulkan
{

/// Vulkan-specific extension/layer request（与 VulkanCommon.h 的 ExtensionConfig 同构）。
/// name:           extension 名（如 VK_KHR_SWAPCHAIN_EXTENSION_NAME）
/// required:       若为 true，extension 不可用时应 abort
/// featuresStruct: 指向对应 pNext feature struct 的指针（nullptr 表示不需要 feature struct）
struct ExtensionRequest
{
    const char* name{nullptr};
    bool        required{false};
    void*       featuresStruct{nullptr};
};

/// Vulkan 设备创建参数（D-08 下沉 struct）。
/// base:               backend-neutral 公共字段（CapabilityRequirements + enableValidationLayers）
/// deviceExtensions:   Vulkan device extension 请求列表（含可选 featureStruct pNext 挂链）
/// instanceExtensions: Vulkan instance extension 名称列表
/// instanceLayers:     Vulkan instance layer 名称列表（如 VK_LAYER_KHRONOS_validation）
struct VulkanDeviceCreateInfo
{
    rhi::DeviceCreateInfo          base{};
    std::vector<ExtensionRequest>  deviceExtensions;
    std::vector<const char*>       instanceExtensions;
    std::vector<const char*>       instanceLayers;
};

} // namespace demo::rhi::vulkan
