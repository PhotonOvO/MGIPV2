"""Source contracts for forward-transparent preservation of shadow-reactive HDR alpha."""

import re
import unittest
from pathlib import Path


REPO_ROOT = Path(__file__).resolve().parents[1]
RENDER_DEVICE = (REPO_ROOT / "render" / "RenderDevice.cpp").read_text(
    encoding="utf-8"
)
RHI_TYPES = (REPO_ROOT / "rhi" / "RHITypes.h").read_text(encoding="utf-8")
VULKAN_PIPELINES = (
    REPO_ROOT / "rhi" / "vulkan" / "VulkanPipelineConversions.h"
).read_text(encoding="utf-8")


def braced_source(source: str, marker: str) -> str:
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


def normalized_assignment(block: str, field: str) -> str:
    match = re.search(rf"\.{re.escape(field)}\s*=\s*(.*?),", block, re.DOTALL)
    if match is None:
        raise AssertionError(f"Missing assignment for {field}")
    return re.sub(r"\s+", "", match.group(1))


class ShadowReactiveForwardAlphaContractTests(unittest.TestCase):
    def test_forward_transparent_rgb_blend_equation_is_unchanged(self) -> None:
        forward_blend = braced_source(
            RENDER_DEVICE,
            "const rhi::BlendAttachmentState forwardBlend",
        )

        expected_rgb_blend = {
            "blendEnable": "true",
            "srcColorBlendFactor": "rhi::BlendFactor::srcAlpha",
            "dstColorBlendFactor": "rhi::BlendFactor::oneMinusSrcAlpha",
            "colorBlendOp": "rhi::BlendOp::add",
        }
        for field, expected in expected_rgb_blend.items():
            with self.subTest(field=field):
                self.assertEqual(normalized_assignment(forward_blend, field), expected)

    def test_forward_transparent_writes_rgb_only_and_preserves_hdr_alpha(self) -> None:
        forward_blend = braced_source(
            RENDER_DEVICE,
            "const rhi::BlendAttachmentState forwardBlend",
        )

        self.assertEqual(
            normalized_assignment(forward_blend, "colorWriteMask"),
            "rhi::ColorComponentFlags::r"
            "|rhi::ColorComponentFlags::g"
            "|rhi::ColorComponentFlags::b",
        )
        self.assertNotIn("ColorComponentFlags::a", forward_blend)
        self.assertNotIn("ColorComponentFlags::all", forward_blend)

    def test_scene_color_hdr_forward_mdi_pipeline_inherits_the_rgb_only_blend(self) -> None:
        forward_pipeline = braced_source(
            RENDER_DEVICE,
            "// Create Forward pipeline for transparent objects",
        )

        self.assertIn(".blendStates = std::span{&forwardBlend, 1}", forward_pipeline)
        self.assertIn(
            "rhi::GraphicsPipelineDesc forwardMdiGraphicsDesc = forwardGraphicsDesc;",
            forward_pipeline,
        )
        self.assertIn(
            "forwardMdiGraphicsDesc.renderingInfo.colorFormats = std::span{&hdrSceneColorFormat, 1};",
            forward_pipeline,
        )
        self.assertNotIn("forwardMdiGraphicsDesc.blendStates", forward_pipeline)

    def test_rhi_supports_an_rgb_mask_without_alpha(self) -> None:
        color_flags = braced_source(
            RHI_TYPES,
            "enum class ColorComponentFlags",
        )
        color_mask_mapping = braced_source(
            VULKAN_PIPELINES,
            "VkColorComponentFlags toVkColorMask",
        )

        for component in ("r", "g", "b", "a"):
            with self.subTest(component=component):
                self.assertRegex(
                    color_flags,
                    rf"\b{component}\s*=\s*1u\s*<<",
                )
                self.assertIn(
                    f"ColorComponentFlags::{component}",
                    color_mask_mapping,
                )
        self.assertIn("VK_COLOR_COMPONENT_R_BIT", color_mask_mapping)
        self.assertIn("VK_COLOR_COMPONENT_G_BIT", color_mask_mapping)
        self.assertIn("VK_COLOR_COMPONENT_B_BIT", color_mask_mapping)
        self.assertIn("VK_COLOR_COMPONENT_A_BIT", color_mask_mapping)


if __name__ == "__main__":
    unittest.main()
