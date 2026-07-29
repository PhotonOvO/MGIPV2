"""Source contracts for deterministic GPU-driven CSM cascade clearing."""

import re
import unittest
from pathlib import Path


REPO_ROOT = Path(__file__).resolve().parents[1]
PASS_SOURCE = (
    REPO_ROOT / "render" / "passes" / "GPUDrivenCSMShadowPass.cpp"
).read_text(encoding="utf-8")


def braced_source(source: str, pattern: str) -> str:
    match = re.search(pattern, source, re.MULTILINE | re.DOTALL)
    if match is None:
        raise AssertionError(f"Could not find source pattern: {pattern}")

    opening_brace = source.find("{", match.end())
    if opening_brace < 0:
        raise AssertionError(f"Could not find block for source pattern: {pattern}")

    depth = 0
    for index in range(opening_brace, len(source)):
        if source[index] == "{":
            depth += 1
        elif source[index] == "}":
            depth -= 1
            if depth == 0:
                return source[opening_brace : index + 1]

    raise AssertionError(f"Unterminated block for source pattern: {pattern}")


class GPUDrivenCSMClearContractTests(unittest.TestCase):
    def test_clear_only_helper_clears_and_stores_every_cascade(self) -> None:
        begin_body = braced_source(
            PASS_SOURCE,
            r"\brhi::RenderEncoder\s*\*\s*beginCascadeDepthPass\s*\(",
        )
        clear_body = braced_source(
            PASS_SOURCE,
            r"\bvoid\s+recordClearOnlyCascades\s*\(",
        )

        self.assertIn(".loadOp = rhi::LoadOp::clear", begin_body)
        self.assertIn(".storeOp = rhi::StoreOp::store", begin_body)
        self.assertIn(".clearValue = {0.0f, 0}", begin_body)
        self.assertIn("renderer.getCSMCascadeViewHandle(cascadeIndex)", begin_body)
        self.assertRegex(
            clear_body,
            r"for\s*\(\s*uint32_t\s+cascadeIndex\s*=\s*0\s*;"
            r"\s*cascadeIndex\s*<\s*cascadeCount\s*;",
        )
        self.assertIn(
            "beginCascadeDepthPass(commandBuffer, renderer, extent, cascadeIndex)",
            clear_body,
        )
        self.assertIn("commandBuffer.endEncoding()", clear_body)

    def test_all_non_recording_failures_share_one_clear_only_fallback(self) -> None:
        execute_body = braced_source(
            PASS_SOURCE,
            r"\bvoid\s+GPUDrivenCSMShadowPass::execute\s*\(",
        )
        first_guard = re.search(
            r"\bif\s*\((.*?)\)\s*\{\s*return;\s*\}",
            execute_body,
            re.MULTILINE | re.DOTALL,
        )
        self.assertIsNotNone(first_guard)
        hard_condition = first_guard.group(1)
        self.assertIn("m_renderer == nullptr", hard_condition)
        self.assertIn("context.commandBuffer == nullptr", hard_condition)
        self.assertNotIn("context.params", hard_condition)
        self.assertNotIn("context.transientAllocator", hard_condition)
        self.assertNotIn("context.executor", hard_condition)

        fallback_body = braced_source(
            execute_body,
            r"\bif\s*\(\s*!canDrawShadows\s*\)",
        )
        self.assertIn(
            "recordClearOnlyCascades(*context.commandBuffer, *m_renderer, "
            "cascadeExtent, cascadeCount)",
            fallback_body,
        )
        self.assertIn("context.commandBuffer->endEvent()", fallback_body)
        self.assertIn("return;", fallback_body)

        pre_fallback = execute_body[: execute_body.index("if (!canDrawShadows)")]
        self.assertEqual(1, pre_fallback.count("return;"))

    def test_draw_preconditions_cover_scene_pipeline_tables_indirect_and_geometry(self) -> None:
        execute_body = braced_source(
            PASS_SOURCE,
            r"\bvoid\s+GPUDrivenCSMShadowPass::execute\s*\(",
        )
        can_draw_start = execute_body.index("const bool canDrawShadows")
        can_draw_end = execute_body.index(
            'context.commandBuffer->beginEvent("GPUDrivenCSMShadow")',
            can_draw_start,
        )
        can_draw_contract = execute_body[can_draw_start:can_draw_end]

        required_checks = (
            "hasPassInputs",
            "hasShadowCasters",
            "shadowData != nullptr",
            "!csmPipeline.isNull()",
            "!computePipeline.isNull()",
            "!computeTable.isNull()",
            "!materialTable.isNull()",
            "!cameraTable.isNull()",
            "hasDrawArgumentTables",
            "hasShadowIndirectBuffer",
            "hasGeometryBuffers",
            "shadowIndirectCapacity >= shadowMeshCount",
        )
        for required_check in required_checks:
            with self.subTest(required_check=required_check):
                self.assertIn(required_check, can_draw_contract)

        preflight_loop = braced_source(
            execute_body,
            r"for\s*\(\s*uint32_t\s+cascadeIndex\s*=\s*0\s*;"
            r"\s*cascadeIndex\s*<\s*cascadeCount\s*;",
        )
        self.assertIn("getCSMShadowMDIDrawArgumentTable", preflight_loop)
        self.assertIn("hasDrawArgumentTables", preflight_loop)

    def test_normal_path_has_no_mid_loop_return_and_preserves_sync_and_bias(self) -> None:
        execute_body = braced_source(
            PASS_SOURCE,
            r"\bvoid\s+GPUDrivenCSMShadowPass::execute\s*\(",
        )
        normal_path = execute_body[execute_body.index("const CSMShadowResources::FrameData&") :]
        cascade_loop = braced_source(
            normal_path,
            r"for\s*\(\s*uint32_t\s+cascadeIndex\s*=\s*0\s*;"
            r"\s*cascadeIndex\s*<\s*cascadeCount\s*;",
        )

        self.assertNotIn("return;", cascade_loop)
        self.assertIn("rhi::HazardFlags::readBeforeWrite", cascade_loop)
        self.assertIn("rhi::HazardFlags::drawArguments", cascade_loop)
        self.assertIn("beginCascadeDepthPass", cascade_loop)
        self.assertIn("DrawStreamRecorder::recordIndexedIndirect", cascade_loop)
        self.assertLess(
            cascade_loop.index("rhi::HazardFlags::readBeforeWrite"),
            cascade_loop.index("cenc->dispatch"),
        )
        self.assertLess(
            cascade_loop.index("cenc->dispatch"),
            cascade_loop.index("rhi::HazardFlags::drawArguments"),
        )
        self.assertLess(
            cascade_loop.index("rhi::HazardFlags::drawArguments"),
            cascade_loop.index("DrawStreamRecorder::recordIndexedIndirect"),
        )
        self.assertIn("cascadeCamera.shadowConstantBias = 0.0f", cascade_loop)
        self.assertIn("-csmFrameData.lightDirection", cascade_loop)


if __name__ == "__main__":
    unittest.main()
