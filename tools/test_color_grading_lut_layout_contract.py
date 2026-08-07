"""Source contracts for the immutable built-in color-grading LUT."""

import re
import unittest
from pathlib import Path


REPO_ROOT = Path(__file__).resolve().parents[1]


def read(relative_path: str) -> str:
    return (REPO_ROOT / relative_path).read_text(encoding="utf-8")


SCENE_RESOURCES = read("render/SceneResources.cpp")
GPU_DRIVEN_RENDERER = read("render/GPUDrivenRenderer.cpp")
RHI_ARGUMENT_TABLE = read("rhi/RHIArgumentTable.h")
VULKAN_BARRIER_CONVERSIONS = read("rhi/vulkan/VulkanBarrierConversions.h")
VULKAN_DEVICE = read("rhi/vulkan/VulkanDevice.cpp")


def extract_braced_block(source: str, marker: str) -> str:
    start = source.index(marker)
    opening_brace = source.index("{", start)
    depth = 0
    for index in range(opening_brace, len(source)):
        if source[index] == "{":
            depth += 1
        elif source[index] == "}":
            depth -= 1
            if depth == 0:
                return source[start : index + 1]
    raise AssertionError(f"Unterminated source block: {marker}")


class ColorGradingLutLayoutContractTests(unittest.TestCase):
    def test_lut_usage_is_upload_then_sampled_only(self) -> None:
        create_start = SCENE_RESOURCES.index(
            "m_resources.colorGradingLutImage.image ="
        )
        create_end = SCENE_RESOURCES.index(
            '"BuiltInColorGradingLUT"));',
            create_start,
        )
        create_call = SCENE_RESOURCES[create_start:create_end]

        usage_flags = re.findall(
            r"rhi::TextureUsageFlags::(\w+)",
            create_call,
        )
        self.assertEqual(usage_flags, ["sampled", "transferDst"])

    def test_upload_finishes_in_shader_read_without_later_transfer_or_write(self) -> None:
        upload_barrier = extract_braced_block(
            SCENE_RESOURCES,
            "const rhi::TextureBarrier lutUploadBarrier",
        )
        ready_barrier = extract_braced_block(
            SCENE_RESOURCES,
            "const rhi::TextureBarrier lutReadyBarrier",
        )

        self.assertRegex(
            upload_barrier,
            r"\.before\s*=\s*rhi::ResourceState::Undefined",
        )
        self.assertRegex(
            upload_barrier,
            r"\.after\s*=\s*rhi::ResourceState::TransferDst",
        )
        self.assertRegex(
            ready_barrier,
            r"\.before\s*=\s*rhi::ResourceState::TransferDst",
        )
        self.assertRegex(
            ready_barrier,
            r"\.after\s*=\s*rhi::ResourceState::ShaderRead",
        )
        self.assertNotIn("ResourceState::General", ready_barrier)

        upload_transition = SCENE_RESOURCES.index(
            "cmdBuffer.resourceBarrier(std::span{&lutUploadBarrier, 1}"
        )
        upload_copy = SCENE_RESOURCES.index(
            "upload.recordTextureUpload("
            "slice, m_resources.colorGradingLutImage.image, region);"
        )
        upload_execution = SCENE_RESOURCES.index(
            "upload.executeUploads(cmdBuffer);",
            upload_copy,
        )
        ready_transition = SCENE_RESOURCES.index(
            "cmdBuffer.resourceBarrier(std::span{&lutReadyBarrier, 1}",
            upload_execution,
        )
        self.assertLess(upload_transition, upload_copy)
        self.assertLess(upload_copy, upload_execution)
        self.assertLess(upload_execution, ready_transition)

        self.assertEqual(
            SCENE_RESOURCES.count(
                ".texture = m_resources.colorGradingLutImage.image"
            ),
            3,
        )
        self.assertEqual(
            SCENE_RESOURCES.count(
                "upload.recordTextureUpload("
                "slice, m_resources.colorGradingLutImage.image, region);"
            ),
            1,
        )

    def test_combined_descriptor_keeps_the_default_sampled_read_intent(self) -> None:
        self.assertRegex(
            RHI_ARGUMENT_TABLE,
            r"ArgumentAccessIntent\s+accessIntent\s*"
            r"\{\s*ArgumentAccessIntent::sampledRead\s*\}",
        )

        update_lighting = extract_braced_block(
            GPU_DRIVEN_RENDERER,
            "void GPUDrivenRenderer::updateLightingArgumentTable(",
        )
        self.assertIn(
            "texViews[kGPUDrivenLightPassColorGradingLutIndex] = "
            "viewOr(sceneView.colorGradingLutView, fallbackColor);",
            update_lighting,
        )
        self.assertIn(
            ".type = rhi::ArgumentType::combinedImageSampler,",
            update_lighting,
        )
        self.assertNotIn(
            "writes[kGPUDrivenLightPassColorGradingLutIndex].accessIntent",
            update_lighting,
        )

        combined_case_start = VULKAN_DEVICE.index(
            "case ArgumentType::combinedImageSampler:"
        )
        combined_case_end = VULKAN_DEVICE.index(
            "default: // buffer types",
            combined_case_start,
        )
        combined_case = VULKAN_DEVICE[combined_case_start:combined_case_end]
        self.assertIn(
            "w.accessIntent == ArgumentAccessIntent::readWrite",
            combined_case,
        )
        self.assertIn("VK_IMAGE_LAYOUT_GENERAL", combined_case)
        self.assertIn(
            "VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL",
            combined_case,
        )

        layout_mapping = extract_braced_block(
            VULKAN_BARRIER_CONVERSIONS,
            "[[nodiscard]] constexpr VkImageLayout toVkImageLayout(ResourceState state)",
        )
        self.assertIn(
            "case ResourceState::ShaderRead: "
            "return VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;",
            layout_mapping,
        )


if __name__ == "__main__":
    unittest.main()
