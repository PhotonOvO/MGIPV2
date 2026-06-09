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
		/// Opaque ImGui texture identifier — matches ImTextureID (uint64_t / ImU64).
		/// Callers store this type; the ImGui/Vulkan backend is never referenced
		/// outside of DebugInteropBridge.cpp.
		using TextureID = uint64_t;

		/// Image layout hint for registerTexture.
		/// Only General is needed today; extend as required.
		enum class ImageLayout : uint32_t
		{
			General = 1, ///< VK_IMAGE_LAYOUT_GENERAL
		};

		struct InitInfo
		{
			rhi::Device* rhiDevice{nullptr};
			rhi::TextureFormat swapchainFormat{rhi::TextureFormat::undefined};
			uint32_t minImageCount{2};
			uint32_t imageCount{3};
			void* window{nullptr}; // GLFWwindow* on desktop, ANativeWindow* on Android
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

		/// Register an RHI texture view with the ImGui Vulkan backend.
		/// Wraps ImGui_ImplVulkan_AddTexture; all native conversion is done internally.
		/// Returns an opaque TextureID to be stored by the caller and later passed to
		/// unregisterTexture. Returns nullptr when ImGui is not initialised.
		TextureID registerTexture(rhi::Device& device,
		                          rhi::SamplerHandle sampler,
		                          rhi::TextureViewHandle view,
		                          ImageLayout layout = ImageLayout::General);

		/// Unregister a previously registered texture from the ImGui Vulkan backend.
		/// Wraps ImGui_ImplVulkan_RemoveTexture. No-op when id is nullptr.
		void unregisterTexture(TextureID id);

	private:
		bool m_initialized{false};
		// m_uiDescriptorPool stores the native pool handle as uint64_t.
		// The actual native type is only named inside DebugInteropBridge.cpp.
		uint64_t m_uiDescriptorPool{0};
		// m_nativeDevice stores the native device handle as uint64_t (used by shutdown).
		uint64_t m_nativeDevice{0};
	};
} // namespace demo
