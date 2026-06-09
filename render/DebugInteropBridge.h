#pragma once

// DebugInteropBridge: sanctioned native exception for ImGui Vulkan backend.
// This header exposes ONLY RHI types. No native Vulkan or ImGui backend headers
// are included here; they live exclusively in DebugInteropBridge.cpp (D-08).
// Allow-list entry: debug_bridge: render/DebugInteropBridge.cpp

#include "../rhi/RHICommandBuffer.h"
#include "../rhi/RHIDevice.h"
#include "../rhi/RHITypes.h"

#include <cstdint>

namespace demo
{

/// Bridges ImGui's Vulkan backend into the render layer.
/// Per D-08/D-09: ImGui draw data rendering, ImGui-exclusive descriptor pool,
/// and platform (GLFW/Android) ImGui backend init/shutdown are the only native
/// code allowed in this class. All native Vulkan handle types stay in
/// the .cpp translation unit.
class DebugInteropBridge
{
public:
    struct InitInfo
    {
        rhi::Device*       rhiDevice{nullptr};
        rhi::TextureFormat swapchainFormat{rhi::TextureFormat::undefined};
        uint32_t           minImageCount{2};
        uint32_t           imageCount{3};
        void*              window{nullptr};   // GLFWwindow* on desktop, ANativeWindow* on Android
    };

    /// Initialise ImGui context, platform backend, and Vulkan backend.
    /// Creates the ImGui-exclusive descriptor pool internally (D-09).
    /// Must be called once after swapchain creation.
    void init(const InitInfo& info);

    /// Wrap ImGui_ImplVulkan_NewFrame() + platform NewFrame() + ImGui::NewFrame().
    /// Called at the start of each UI frame (replaces RenderDevice::beginUiFrame native calls).
    void newFrame();

    /// Render the ImGui draw data into the active render pass on cmdBuffer.
    /// Wraps ImGui_ImplVulkan_RenderDrawData.
    void renderImGui(rhi::CommandBuffer& cmdBuffer);

    /// Shut down ImGui backend in the correct order (D-09, RESEARCH traps §3):
    ///   ImGui_ImplVulkan_Shutdown -> platform shutdown -> ImGui::DestroyContext -> vkDestroyDescriptorPool
    void shutdown();

    bool isInitialized() const { return m_initialized; }

private:
    bool     m_initialized{false};
    // m_uiDescriptorPool stores the native pool handle as uint64_t.
    // The actual native type is only named inside DebugInteropBridge.cpp.
    uint64_t m_uiDescriptorPool{0};
    // m_nativeDevice stores the native device handle as uint64_t (used by shutdown).
    uint64_t m_nativeDevice{0};
};

} // namespace demo
