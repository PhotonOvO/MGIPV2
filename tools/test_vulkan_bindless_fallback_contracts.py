"""Source contracts for the Vulkan bindless fallback path."""

import re
import unittest
from pathlib import Path


REPO_ROOT = Path(__file__).resolve().parents[1]


def read(relative_path: str) -> str:
    return (REPO_ROOT / relative_path).read_text(encoding="utf-8")


VULKAN_DEVICE = read("rhi/vulkan/VulkanDevice.cpp")
VULKAN_DEVICE_HEADER = read("rhi/vulkan/VulkanDevice.h")
RENDER_DEVICE = read("render/RenderDevice.cpp")
ARCHITECTURE_BASELINE = read("doc/ARCHITECTURE_BASELINE.md")


def extract_function(source: str, signature: str) -> str:
    start = source.index(signature)
    brace = source.index("{", start)
    depth = 0
    for index in range(brace, len(source)):
        char = source[index]
        if char == "{":
            depth += 1
        elif char == "}":
            depth -= 1
            if depth == 0:
                return source[start : index + 1]
    raise AssertionError(f"Unterminated function: {signature}")



class VulkanBindlessFallbackContractTests(unittest.TestCase):
    def test_pool_budget_is_backend_owned_and_grows_on_exhaustion(self) -> None:
        self.assertNotIn("configureArgumentPoolCapacity", RENDER_DEVICE)
        self.assertNotIn("configureArgumentPoolCapacity", VULKAN_DEVICE_HEADER)
        self.assertNotIn("configureArgumentPoolCapacity", VULKAN_DEVICE)
        self.assertIn("kDefaultCombinedImageSamplerPoolCapacity = 16384u", VULKAN_DEVICE)
        self.assertIn(
            "{VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, "
            "m_combinedImageSamplerPoolCapacity}",
            VULKAN_DEVICE,
        )
        self.assertIn("VK_ERROR_OUT_OF_POOL_MEMORY", VULKAN_DEVICE)
        self.assertIn("VK_ERROR_FRAGMENTED_POOL", VULKAN_DEVICE)
        self.assertNotIn("kAuthoritativeFrameSlotCount", VULKAN_DEVICE)
    def test_pool_grows_and_retries_fragmented_or_exhausted_allocations(self) -> None:
        create_table = extract_function(
            VULKAN_DEVICE,
            "ArgumentTableHandle VulkanDevice::createArgumentTable(",
        )
        self.assertIn("VK_ERROR_OUT_OF_POOL_MEMORY", create_table)
        self.assertIn("VK_ERROR_FRAGMENTED_POOL", create_table)
        self.assertGreaterEqual(create_table.count("vkCreateDescriptorPool"), 2)
        self.assertGreaterEqual(create_table.count("vkAllocateDescriptorSets"), 2)
        self.assertIn("m_argumentPools.push_back(retryPool)", create_table)
        self.assertIn("m_argumentSetPools.emplace", create_table)
        self.assertNotIn("VK_DESCRIPTOR_POOL_CREATE_UPDATE_AFTER_BIND_BIT", create_table)
        self.assertNotIn("VkPhysicalDeviceDescriptorIndexingProperties", create_table)

    def test_bindless_layout_is_partially_bound_without_update_after_bind(self) -> None:
        create_layout = extract_function(
            VULKAN_DEVICE,
            "ArgumentLayoutHandle VulkanDevice::createArgumentLayout(",
        )
        bindless_block = re.search(
            r"if\s*\(b\.bindless\)\s*\{(?P<body>.*?)\n\t\t\t\}",
            create_layout,
            re.DOTALL,
        )
        self.assertIsNotNone(bindless_block)
        self.assertIn(
            "VK_DESCRIPTOR_BINDING_PARTIALLY_BOUND_BIT",
            bindless_block.group("body"),
        )
        self.assertIn(
            "m_features12.descriptorBindingPartiallyBound == VK_TRUE",
            bindless_block.group("body"),
        )
        self.assertEqual(
            create_layout.count("VK_DESCRIPTOR_BINDING_PARTIALLY_BOUND_BIT"),
            1,
        )
        self.assertNotIn("VK_DESCRIPTOR_BINDING_UPDATE_AFTER_BIND_BIT", create_layout)
        self.assertNotIn(
            "VK_DESCRIPTOR_SET_LAYOUT_CREATE_UPDATE_AFTER_BIND_POOL_BIT",
            create_layout,
        )
        self.assertIn("bindingFlags(static_cast<uint32_t>(desc.bindings.size()), 0)", create_layout)
        self.assertIn("vkGetDescriptorSetLayoutSupport", create_layout)

    def test_supported_descriptor_features_are_not_forced_or_overrequired(self) -> None:
        init_logical_device = extract_function(
            VULKAN_DEVICE,
            "void VulkanDevice::initLogicalDevice(",
        )
        detect_capabilities = extract_function(
            VULKAN_DEVICE,
            "void VulkanDevice::detectCapabilities(",
        )
        self.assertIn(
            "VkPhysicalDeviceVulkan12Features supportedFeatures12",
            init_logical_device,
        )
        self.assertIn("m_features12 = supportedFeatures12;", init_logical_device)
        self.assertIn(
            "m_deviceFeatures.pNext = m_featuresChainHead;", init_logical_device
        )
        self.assertNotRegex(
            init_logical_device,
            r"descriptorBindingVariableDescriptorCount\s*=\s*VK_TRUE",
        )
        self.assertNotIn("descriptorBindingVariableDescriptorCount", detect_capabilities)
        self.assertNotIn("descriptorBindingSampledImageUpdateAfterBind", detect_capabilities)

    def test_renderer_fallback_texture_locals_describe_white_and_normal_semantics(
        self,
    ) -> None:
        renderer_init = extract_function(
            RENDER_DEVICE,
            "void RenderDevice::init(",
        )

        for expected_name in (
            "whiteFallbackImage",
            "whiteFallbackTexture",
            "normalFallbackImage",
            "normalFallbackTexture",
        ):
            self.assertIn(expected_name, renderer_init)

        for obsolete_name in (
            "materialImage0",
            "materialImage1",
            "materialTexture0",
            "materialTexture1",
        ):
            self.assertNotIn(obsolete_name, renderer_init)

        self.assertNotIn("image1.jpg", renderer_init)
        self.assertNotIn("image2.jpg", renderer_init)

    def test_document_separates_historical_images_from_current_fallbacks(self) -> None:
        self.assertIn("**Historical Snapshot**", ARCHITECTURE_BASELINE)
        self.assertIn("**Current Renderer Delta**", ARCHITECTURE_BASELINE)
        self.assertIn(
            "Historical baseline: load image1.jpg and image2.jpg",
            ARCHITECTURE_BASELINE,
        )
        self.assertIn(
            "renderer-owned 1x1 white/normal RGBA fallback textures",
            ARCHITECTURE_BASELINE,
        )
        self.assertNotIn(
            "### 5.1 Current Resource Members",
            ARCHITECTURE_BASELINE,
        )
        self.assertIn(
            "fixed two-element `utils::ImageResource` array",
            ARCHITECTURE_BASELINE,
        )
        self.assertNotIn("m_image[2]", ARCHITECTURE_BASELINE)


if __name__ == "__main__":
    unittest.main()
