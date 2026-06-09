#pragma once

// D-07 backend-internal interop 面：所有 native handle 穿透与 resolve* 访问的单一载体。
// 公共 rhi::Device 接口中已无对应 getter/resolve*；合法调用方须将 rhi::Device* static_cast
// 到 VulkanDeviceInterop* 后访问，仅允许 .rhi-boundary-allow 认可的文件使用。
//
// 用途：
//   - render/SceneResources.cpp、render/IBLResources.cpp、render/GPUDrivenRenderer.cpp
//     中合法的 native debug-naming / descriptor 写入路径通过 resolveTexture/resolveTextureView/
//     resolveSampler 获取 typed Vulkan handle，无需 reinterpret_cast<VkXxx>(uint64_t)。
//   - swapchain init / debug bridge 通过 nativeInstance/nativePhysicalDevice/nativeDevice 获取
//     底层 Vulkan 对象，无需通过公共 getBackend* 接口。

#include "../RHIHandles.h"

#include <vulkan/vulkan.h>

namespace demo::rhi::vulkan
{

/// VulkanDeviceInterop：Vulkan backend-internal native 穿透接口。
///
/// 提供两组纯虚 accessor：
///   1. native device accessor（4 个）：替代公共契约的 getBackend* / getFeaturesChainHead
///   2. resolve accessor（3 个）：替代公共契约的 resolveTexture*/resolveSamplerBackendHandle，
///      直接返回 typed Vulkan handle，消费端无需二次 reinterpret_cast
///
/// VulkanDevice 多重继承本接口并 override 所有方法。
class VulkanDeviceInterop
{
public:
    virtual ~VulkanDeviceInterop() = default;

    // -------------------------------------------------------------------------
    // Native device accessor（D-07）
    // -------------------------------------------------------------------------

    /// 返回 Vulkan instance handle（替代公共 getBackendInstanceHandle）。
    [[nodiscard]] virtual VkInstance nativeInstance() const = 0;

    /// 返回 Vulkan physical device handle（替代公共 getBackendPhysicalDeviceHandle）。
    [[nodiscard]] virtual VkPhysicalDevice nativePhysicalDevice() const = 0;

    /// 返回 Vulkan logical device handle（替代公共 getBackendDeviceHandle）。
    [[nodiscard]] virtual VkDevice nativeDevice() const = 0;

    /// 返回 feature pNext 链头指针（替代公共 getFeaturesChainHead，提供 typed 返回）。
    [[nodiscard]] virtual VkBaseOutStructure* nativeFeaturesChainHead() const = 0;

    // -------------------------------------------------------------------------
    // Resolve accessor（D-07 / resolve* 下沉）
    // -------------------------------------------------------------------------

    /// 将 RHI TextureHandle 解析为 VkImage（typed，替代 resolveTextureBackendHandle）。
    [[nodiscard]] virtual VkImage resolveTexture(rhi::TextureHandle handle) const = 0;

    /// 将 RHI TextureViewHandle 解析为 VkImageView（typed，替代 resolveTextureViewBackendHandle）。
    [[nodiscard]] virtual VkImageView resolveTextureView(rhi::TextureViewHandle handle) const = 0;

    /// 将 RHI SamplerHandle 解析为 VkSampler（typed，替代 resolveSamplerBackendHandle）。
    [[nodiscard]] virtual VkSampler resolveSampler(rhi::SamplerHandle handle) const = 0;
};

} // namespace demo::rhi::vulkan
