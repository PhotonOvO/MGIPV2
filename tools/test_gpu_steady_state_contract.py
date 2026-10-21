from __future__ import annotations

import re
import unittest
from pathlib import Path


REPO_ROOT = Path(__file__).resolve().parents[1]


def read(relative_path: str) -> str:
    return (REPO_ROOT / relative_path).read_text(encoding="utf-8")


def function_source(source: str, marker: str) -> str:
    start = source.index(marker)
    opening_brace = source.index("{", start)
    depth = 0
    for index in range(opening_brace, len(source)):
        character = source[index]
        if character == "{":
            depth += 1
        elif character == "}":
            depth -= 1
            if depth == 0:
                return source[start : index + 1]

    raise AssertionError(f"unterminated function after marker: {marker}")


class GPUSteadyStateContractTests(unittest.TestCase):
    def test_gpu_batch_builder_is_removed(self) -> None:
        self.assertFalse((REPO_ROOT / "render/GPUBatchBuilder.h").exists())
        self.assertFalse((REPO_ROOT / "render/GPUBatchBuilder.cpp").exists())
        self.assertNotIn("GPUBatchBuilder", read("CMakeLists.txt"))
        self.assertIn("struct GPUBatchBuildStats", read("shaders/shader_io.h"))

    def test_gpu_culling_buffers_guard_early_returns(self) -> None:
        source = function_source(
            read("render/RenderDevice.cpp"),
            "void RenderDevice::ensureGPUCullingBuffers",
        )
        guard = "if (frameUserData.gpuCullingMeshCapacity >= requiredCapacity)"
        guard_source = function_source(source, guard)
        self.assertRegex(guard_source, re.escape(guard) + r"\s*\{\s*return;\s*\}")
        early_return = source.index("return;", source.index(guard))
        self.assertLess(early_return, source.index("createBuffer"))

    def test_visibility_sort_capacity_guard_early_returns(self) -> None:
        source = function_source(
            read("render/GPUDrivenRenderer.cpp"),
            "void GPUDrivenRenderer::ensureVisibilitySortCapacity",
        )
        self.assertIn("growSortBuffers", source)
        self.assertIn("growPatchBuffers", source)
        guard = "if (!growSortBuffers && !growPatchBuffers)"
        guard_source = function_source(source, guard)
        self.assertRegex(guard_source, re.escape(guard) + r"\s*\{\s*return;\s*\}")
        early_return = source.index("return;", source.index(guard))
        self.assertLess(early_return, source.index("createBuffer"))

    def test_persistent_indirect_stream_guard_early_returns(self) -> None:
        source = function_source(
            read("render/RenderDevice.cpp"),
            "void RenderDevice::ensureGPUDrivenPersistentIndirectStreamBuffer",
        )
        guard = (
            "if (frameUserData.gpuDrivenPersistentIndirectStreamCapacity >= requiredCapacity)"
        )
        guard_source = function_source(source, guard)
        self.assertRegex(guard_source, re.escape(guard) + r"\s*\{\s*return;\s*\}")
        early_return = source.index("return;", source.index(guard))
        self.assertLess(early_return, source.index("createBuffer"))

    def test_stable_recording_budget_predicate_exists(self) -> None:
        source = read("rhi/RHIDebugCounters.h")
        predicate = function_source(source, "stableRecordingBudgetMet")
        self.assertIn("descriptorAllocations == 0", predicate)
        self.assertIn("pipelineCreations == 0", predicate)
        self.assertIn("argumentLayoutCreations == 0", predicate)
        self.assertIn("hotPathCounterDelta", source)

    def test_metrics_output_reports_hot_path_counters(self) -> None:
        source = read("app/MinimalLatestApp.h")
        self.assertIn("hot_path_counters", source)
        self.assertIn("descriptor_allocations", source)


if __name__ == "__main__":
    unittest.main()
