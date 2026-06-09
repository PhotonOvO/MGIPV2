// DebugInteropBridge.cpp — Sanctioned native exception per D-08/D-09.
// This file is the ONLY location in render/ that is allowed to include
// vulkan.h, imgui_impl_vulkan.h, and imgui_impl_glfw.h directly.
// Allow-list entry in .rhi-boundary-allow:
//   debug_bridge: render/DebugInteropBridge.cpp

#include "DebugInteropBridge.h"

// Native Vulkan headers — only permitted inside this .cpp (D-08).
#include "../rhi/vulkan/internal/VulkanCommon.h"

// ImGui backends — native interop, allowed only in this bridge file.
#include <imgui.h>
#include <backends/imgui_impl_vulkan.h>
#ifdef __ANDROID__
#  include <backends/imgui_impl_android.h>
#else
#  include <backends/imgui_impl_glfw.h>
#endif

#include "../common/Logger.h"

#include <array>
#include <cassert>
#include <cstdint>

namespace demo
{
	// ---------------------------------------------------------------------------
	// Internal helpers
	// ---------------------------------------------------------------------------

	/// Cast a stored uint64_t back to the requested Vulkan handle type.
	template <typename T>
	static T vkHandleFromU64(uint64_t h)
	{
		return reinterpret_cast<T>(static_cast<uintptr_t>(h));
	}

	/// Convert rhi::TextureFormat -> VkFormat for the subset used by swapchain / ImGui.
	static VkFormat toVkFormatBridge(rhi::TextureFormat format)
	{
		switch (format)
		{
		case rhi::TextureFormat::rgba8Unorm: return VK_FORMAT_R8G8B8A8_UNORM;
		case rhi::TextureFormat::bgra8Unorm: return VK_FORMAT_B8G8R8A8_UNORM;
		case rhi::TextureFormat::rgba16Sfloat: return VK_FORMAT_R16G16B16A16_SFLOAT;
		default: return VK_FORMAT_B8G8R8A8_UNORM;
		}
	}

	// ---------------------------------------------------------------------------
	// DebugInteropBridge::init
	// ---------------------------------------------------------------------------

	void DebugInteropBridge::init(const InitInfo& info)
	{
		assert(info.rhiDevice != nullptr && "DebugInteropBridge::init: rhiDevice must not be null");

		LOGI("DebugInteropBridge::init: begin");

		// --- 1. Cache native device handle (used in shutdown). ----------------
		m_nativeDevice = info.rhiDevice->getBackendDeviceHandle();

		// --- 2. Create ImGui-exclusive descriptor pool (D-09). ----------------
		// Dear ImGui Vulkan backend switched to separate sampled-image + sampler
		// descriptor sets in 2026. Keep combined-image-sampler capacity too so the
		// pool remains compatible with older backend revisions in existing build trees.
		constexpr uint32_t kUiTextureDescriptorCount = 20U;
		constexpr uint32_t kUiSamplerDescriptorCount = 2U;
		constexpr uint32_t kUiMaxDescriptorSets = kUiTextureDescriptorCount + kUiSamplerDescriptorCount;

		const std::array<VkDescriptorPoolSize, 3> poolSizes{
			{
				{VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE, kUiTextureDescriptorCount},
				{VK_DESCRIPTOR_TYPE_SAMPLER, kUiSamplerDescriptorCount},
				{VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, kUiTextureDescriptorCount},
			}
		};

		const VkDescriptorPoolCreateInfo poolCreateInfo{
			.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO,
			.flags = VK_DESCRIPTOR_POOL_CREATE_FREE_DESCRIPTOR_SET_BIT,
			.maxSets = kUiMaxDescriptorSets,
			.poolSizeCount = static_cast<uint32_t>(poolSizes.size()),
			.pPoolSizes = poolSizes.data(),
		};

		VkDescriptorPool pool = VK_NULL_HANDLE;
		const VkDevice vkDevice = vkHandleFromU64<VkDevice>(m_nativeDevice);
		VK_CHECK(vkCreateDescriptorPool(vkDevice, &poolCreateInfo, nullptr, &pool));
		m_uiDescriptorPool = reinterpret_cast<uintptr_t>(pool);

		LOGI("DebugInteropBridge::init: created UI descriptor pool (sampled:%u sampler:%u sets:%u)",
		     kUiTextureDescriptorCount, kUiSamplerDescriptorCount, kUiMaxDescriptorSets);

		// --- 3. Initialise ImGui context. ------------------------------------
		IMGUI_CHECKVERSION();
		ImGui::CreateContext();
		ImGui::StyleColorsDark();

		// --- 4. Platform backend init. ----------------------------------------
#ifdef __ANDROID__
		ImGui_ImplAndroid_Init(static_cast<ANativeWindow*>(info.window));
#else
		ImGui_ImplGlfw_InitForVulkan(static_cast<GLFWwindow*>(info.window), true);
#endif

		// --- 5. Vulkan backend init. ------------------------------------------
		const rhi::QueueInfo graphicsQueue = info.rhiDevice->getGraphicsQueue();
		VkFormat imageFormats[] = {toVkFormatBridge(info.swapchainFormat)};

		ImGui_ImplVulkan_InitInfo initInfo = {
			.Instance = vkHandleFromU64<VkInstance>(info.rhiDevice->getBackendInstanceHandle()),
			.PhysicalDevice = vkHandleFromU64<VkPhysicalDevice>(info.rhiDevice->getBackendPhysicalDeviceHandle()),
			.Device = vkDevice,
			.QueueFamily = graphicsQueue.familyIndex,
			.Queue = vkHandleFromU64<VkQueue>(graphicsQueue.backendHandle),
			.DescriptorPool = pool,
			.MinImageCount = info.minImageCount,
			.ImageCount = info.imageCount,
			.PipelineInfoMain =
			{
				.PipelineRenderingCreateInfo =
				{
					.sType = VK_STRUCTURE_TYPE_PIPELINE_RENDERING_CREATE_INFO,
					.colorAttachmentCount = 1,
					.pColorAttachmentFormats = imageFormats,
				},
			},
			.UseDynamicRendering = true,
		};
		initInfo.PipelineInfoForViewports = initInfo.PipelineInfoMain;

		ImGui_ImplVulkan_Init(&initInfo);
		LOGI("DebugInteropBridge::init: ImGui Vulkan backend initialized");

		ImGui::GetIO().ConfigFlags = ImGuiConfigFlags_DockingEnable;

		m_initialized = true;
		LOGI("DebugInteropBridge::init: completed");
	}

	// ---------------------------------------------------------------------------
	// DebugInteropBridge::newFrame
	// ---------------------------------------------------------------------------

	void DebugInteropBridge::newFrame()
	{
		if (!m_initialized)
			return;

		ImGui_ImplVulkan_NewFrame();
#ifdef __ANDROID__
		ImGui_ImplAndroid_NewFrame();
#else
		ImGui_ImplGlfw_NewFrame();
#endif
		ImGui::NewFrame();
	}

	// ---------------------------------------------------------------------------
	// DebugInteropBridge::renderImGui
	// ---------------------------------------------------------------------------

	void DebugInteropBridge::renderImGui(rhi::CommandBuffer& cmdBuffer)
	{
		if (!m_initialized)
			return;

		const VkCommandBuffer vkCmd = static_cast<VkCommandBuffer>(cmdBuffer.getBackendHandle());
		if (vkCmd != VK_NULL_HANDLE)
		{
			ImGui_ImplVulkan_RenderDrawData(ImGui::GetDrawData(), vkCmd);
		}
	}

	// ---------------------------------------------------------------------------
	// DebugInteropBridge::shutdown
	// ---------------------------------------------------------------------------
	// Destruction order (RESEARCH §Trap 3, T-04-09):
	//   1. ImGui_ImplVulkan_Shutdown()  — frees descriptor sets from uiDescriptorPool
	//   2. Platform shutdown            — ImGui_ImplGlfw_Shutdown / ImGui_ImplAndroid_Shutdown
	//   3. ImGui::DestroyContext()
	//   4. vkDestroyDescriptorPool      — pool safe to destroy only after sets are freed

	void DebugInteropBridge::shutdown()
	{
		if (!m_initialized)
			return;

		// Step 1: release ImGui Vulkan resources (frees descriptor sets from uiDescriptorPool)
		ImGui_ImplVulkan_Shutdown();

		// Step 2: platform backend shutdown
#ifdef __ANDROID__
		ImGui_ImplAndroid_Shutdown();
#else
		ImGui_ImplGlfw_Shutdown();
#endif

		// Step 3: destroy ImGui context
		ImGui::DestroyContext();

		// Step 4: destroy the UI descriptor pool (now safe — descriptor sets have been freed)
		VkDescriptorPool pool = vkHandleFromU64<VkDescriptorPool>(m_uiDescriptorPool);
		if (pool != VK_NULL_HANDLE)
		{
			VkDevice vkDevice = vkHandleFromU64<VkDevice>(m_nativeDevice);
			vkDestroyDescriptorPool(vkDevice, pool, nullptr);
			m_uiDescriptorPool = 0;
		}

		m_nativeDevice = 0;
		m_initialized = false;

		LOGI("DebugInteropBridge::shutdown: completed");
	}

	// ---------------------------------------------------------------------------
	// DebugInteropBridge::registerTexture
	// ---------------------------------------------------------------------------

	DebugInteropBridge::TextureID DebugInteropBridge::registerTexture(
		rhi::Device& device,
		rhi::SamplerHandle sampler,
		rhi::TextureViewHandle view,
		ImageLayout layout)
	{
		if (!m_initialized || ImGui::GetCurrentContext() == nullptr ||
			ImGui::GetIO().BackendPlatformUserData == nullptr)
		{
			return 0u;
		}

		const VkImageView vkView = reinterpret_cast<VkImageView>(
			static_cast<uintptr_t>(device.resolveTextureViewBackendHandle(view)));

		VkImageLayout vkLayout = VK_IMAGE_LAYOUT_GENERAL;
		switch (layout)
		{
		case ImageLayout::General: vkLayout = VK_IMAGE_LAYOUT_GENERAL;
			break;
		}

		// Note: sampler parameter is accepted for API symmetry but the new
		// imgui_impl_vulkan API (1.92+) manages its own sampler internally.
		// VkDescriptorSet is cast to uint64_t (= ImU64 = ImTextureID).
		VkDescriptorSet descSet = ImGui_ImplVulkan_AddTexture(vkView, vkLayout);
		return reinterpret_cast<uintptr_t>(descSet);
	}

	// ---------------------------------------------------------------------------
	// DebugInteropBridge::unregisterTexture
	// ---------------------------------------------------------------------------

	void DebugInteropBridge::unregisterTexture(TextureID id)
	{
		if (id == 0u)
			return;
		if (ImGui::GetCurrentContext() == nullptr ||
			ImGui::GetIO().BackendPlatformUserData == nullptr)
		{
			return;
		}

		VkDescriptorSet descSet = reinterpret_cast<VkDescriptorSet>(static_cast<uintptr_t>(id));
		ImGui_ImplVulkan_RemoveTexture(descSet);
	}
} // namespace demo
