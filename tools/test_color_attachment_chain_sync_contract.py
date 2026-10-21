#!/usr/bin/env python3
"""Static contract for the SceneColorHDR same-layout attachment chain."""

from __future__ import annotations

import re
import unittest
from pathlib import Path


ROOT = Path(__file__).resolve().parents[1]
PASS_EXECUTOR = (ROOT / "render/PassExecutor.cpp").read_text(encoding="utf-8")
GPU_DRIVEN_RENDERER = (ROOT / "render/GPUDrivenRenderer.cpp").read_text(encoding="utf-8")
VULKAN_BARRIER_CONVERSIONS = (ROOT / "rhi/vulkan/VulkanBarrierConversions.h").read_text(encoding="utf-8")

CHAIN = (
    ("GPUDrivenLightPass", "render/passes/GPUDrivenLightPass.cpp", "write"),
    ("GPUDrivenSkyboxPass", "render/passes/GPUDrivenSkyboxPass.cpp", "readWrite"),
    ("GPUDrivenForwardPass", "render/passes/GPUDrivenForwardPass.cpp", "readWrite"),
    ("DDGIDebugPass", "render/passes/DDGIDebugPass.cpp", "readWrite"),
)

SCENE_COLOR_DEPENDENCY = re.compile(
    r"PassResourceDependency::texture\(\s*"
    r"kPassSceneColorHdrHandle\s*,\s*"
    r"ResourceAccess::(?P<access>read|write|readWrite)\s*,\s*"
    r"rhi::StageFlags::(?P<stage>\w+)\s*,\s*"
    r"rhi::HazardFlags::(?P<hazard>\w+)\s*,\s*"
    r"rhi::ResourceState::(?P<state>\w+)\s*\)",
    re.DOTALL,
)


def read(relative_path: str) -> str:
    return (ROOT / relative_path).read_text(encoding="utf-8")


def braced_source(source: str, marker: str) -> str:
    start = source.index(marker)
    brace = source.index("{", start)
    depth = 0
    for index in range(brace, len(source)):
        if source[index] == "{":
            depth += 1
        elif source[index] == "}":
            depth -= 1
            if depth == 0:
                return source[start : index + 1]
    raise AssertionError(f"unterminated braced source after {marker!r}")


def scene_color_dependency(owner: str, relative_path: str) -> dict[str, str]:
    dependencies = braced_source(read(relative_path), f"{owner}::getDependencies() const")
    matches = list(SCENE_COLOR_DEPENDENCY.finditer(dependencies))
    if len(matches) != 1:
        raise AssertionError(
            f"{owner} must declare exactly one explicit SceneColorHDR attachment dependency; "
            f"found {len(matches)}"
        )
    if dependencies.count("kPassSceneColorHdrHandle") != 1:
        raise AssertionError(f"{owner} has an unrecognized SceneColorHDR dependency form")
    return matches[0].groupdict()


class ColorAttachmentChainSyncContractTests(unittest.TestCase):
    def test_scene_color_attachment_dependencies_use_raster_output_access(self) -> None:
        # A pass that only writes the target can keep COLOR_ATTACHMENT_OPTIMAL. A pass that also
        # samples it cannot, because a combined image sampler may not read that layout, so its
        # declaration has to be General. The state therefore follows from the access, and pinning
        # one name for the whole chain would demand a layout Vulkan does not allow.
        for owner, relative_path, expected_access in CHAIN:
            with self.subTest(pass_name=owner):
                dependency = scene_color_dependency(owner, relative_path)
                self.assertEqual(dependency["access"], expected_access)
                self.assertEqual(dependency["stage"], "rasterColorOut")
                self.assertEqual(dependency["hazard"], "textureWrites")
                expected_state = "ColorAttachment" if expected_access == "write" else "General"
                self.assertEqual(dependency["state"], expected_state)

    def test_light_skybox_forward_ddgi_share_one_scene_color_layout(self) -> None:
        # Passes that write without sampling keep COLOR_ATTACHMENT_OPTIMAL; the rest settle in
        # General. Both groups are legitimate, and what has to hold is that each pass lands in the
        # state its own access requires, so the chain never flips back and forth.
        attachment_owners: list[str] = []
        for pass_path in sorted((ROOT / "render/passes").glob("*.cpp")):
            source = pass_path.read_text(encoding="utf-8")
            if "kPassSceneColorHdrHandle" not in source:
                continue
            for match in SCENE_COLOR_DEPENDENCY.finditer(source):
                # Record both groups so the assertion below proves every expected pass declares a
                # state, rather than only that the write-only passes do.
                attachment_owners.append(f"{pass_path.stem}:{match.group('state')}")

        self.assertCountEqual(
            attachment_owners,
            [
                "GPUDrivenLightPass:ColorAttachment",
                "GPUDrivenSkyboxPass:General",
                "GPUDrivenForwardPass:General",
                "DDGIDebugPass:General",
            ],
        )

        order_markers = (
            "m_passExecutor.addPass(*m_lightPass);",
            "m_passExecutor.addPass(*m_skyboxPass);",
            "m_passExecutor.addPass(*m_forwardPass);",
            "m_passExecutor.addPass(*m_ddgiDebugPass);",
        )
        positions = [GPU_DRIVEN_RENDERER.index(marker) for marker in order_markers]
        self.assertEqual(positions, sorted(positions))

    def test_same_layout_barriers_propagate_raster_color_stages(self) -> None:
        requires_barrier = braced_source(PASS_EXECUTOR, "bool requiresBarrier(")
        requires_boundary = braced_source(PASS_EXECUTOR, "bool requiresResourceBoundary(")
        execute = braced_source(PASS_EXECUTOR, "void PassExecutor::execute(")

        self.assertIn(
            "return !(previous == ResourceAccess::read && next == ResourceAccess::read);",
            requires_barrier,
        )
        self.assertIn("return previous != next;", requires_boundary)

        barrier_call = re.search(
            r"context\.commandBuffer->barrier\(\s*"
            r"textureState->stages\s*,\s*"
            r"dependencyStages\(dependency\)\s*,\s*"
            r"textureState->hazards\s*\|\s*dependencyHazards\(dependency\)\s*\)",
            execute,
            re.DOTALL,
        )
        self.assertIsNotNone(barrier_call)
        self.assertIn("textureState->stages = dependencyStages(dependency);", execute)
        self.assertIn("textureState->state = requiredState;", execute)

        dependencies = [
            scene_color_dependency(owner, relative_path)
            for owner, relative_path, _ in CHAIN
        ]
        # The chain may leave COLOR_ATTACHMENT_OPTIMAL once, when the first pass that also samples
        # the target forces General, and must then stay there: flipping back would add a
        # transition per pass.
        left_attachment = False
        for previous, current in zip(dependencies, dependencies[1:]):
            with self.subTest(previous=previous, current=current):
                if previous["state"] != current["state"]:
                    self.assertEqual(
                        (previous["state"], current["state"]),
                        ("ColorAttachment", "General"),
                    )
                    self.assertFalse(left_attachment)
                    left_attachment = True
                self.assertFalse(
                    previous["access"] == "read" and current["access"] == "read"
                )
                self.assertEqual(previous["stage"], "rasterColorOut")
                self.assertEqual(current["stage"], "rasterColorOut")

    def test_vulkan_backend_maps_raster_color_producers_to_attachment_output(self) -> None:
        stage_map = braced_source(VULKAN_BARRIER_CONVERSIONS, "toVkPipelineStage2(")
        producer_access = braced_source(VULKAN_BARRIER_CONVERSIONS, "inferProducerAccess(")

        self.assertRegex(
            stage_map,
            r"StageFlags::rasterColorOut\)\)\s*out\s*\|=\s*"
            r"VK_PIPELINE_STAGE_2_COLOR_ATTACHMENT_OUTPUT_BIT",
        )
        self.assertRegex(
            producer_access,
            r"StageFlags::rasterColorOut\)\)\s*out\s*\|=\s*"
            r"VK_ACCESS_2_COLOR_ATTACHMENT_WRITE_BIT",
        )


if __name__ == "__main__":
    unittest.main()
