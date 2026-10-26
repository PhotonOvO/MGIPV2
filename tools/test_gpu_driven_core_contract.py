"""Core invariants of the GPU-driven visibility path.

These assertions guard the fixes that landed in commit 6a97b1c. They are deliberately
token-based rather than formatting-based: the C++ sources mix tabs and spaces, so an
indented full-text match would break on unrelated reformatting.

Every assertion here maps to a specific defect that was found and fixed:

  1. GPU->CPU readback used mapBuffer on gpuOnly buffers, so it always returned null.
  2. The readback read the slot being recorded instead of the last submitted slot.
  3. Nothing asserted that the copy to staging happened after the culling dispatch.
  4. A stub shader named meshlet_culling was compiled but never referenced.
  5. Transparent sort keys were still computed per draw on the CPU.
  6. The padding key relied on an implicit category-bit mismatch.
  7. SSR allocated a fresh ArgumentTable every frame.
  8. Capacity guards were assumed to early-return before allocating; nothing checked.
"""

from __future__ import annotations

import re
import unittest
from pathlib import Path


REPO_ROOT = Path(__file__).resolve().parents[1]


def read(relative_path: str) -> str:
    return (REPO_ROOT / relative_path).read_text(encoding="utf-8")


def function_source(source: str, marker: str) -> str:
    """Return the braced body that follows `marker`, including the signature."""
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


def buffer_desc_block(source: str, assignment: str) -> str:
    """Return the `createBuffer(rhi::BufferDesc{...})` block assigned to `assignment`."""
    start = source.index(assignment)
    open_marker = source.index("rhi::BufferDesc{", start)
    depth = 0
    for index in range(open_marker + len("rhi::BufferDesc"), len(source)):
        character = source[index]
        if character == "{":
            depth += 1
        elif character == "}":
            depth -= 1
            if depth == 0:
                return source[start : index + 1]

    raise AssertionError(f"unterminated BufferDesc for {assignment}")


class GPUCullingReadbackContractTests(unittest.TestCase):
    @classmethod
    def setUpClass(cls) -> None:
        cls.render_device = read("render/RenderDevice.cpp")

    def test_staging_buffers_are_cpu_visible_and_device_buffers_are_not(self) -> None:
        # Defect 1: mapBuffer only returns a mapped pointer for cpuToGpu/gpuToCpu memory
        # (rhi/vulkan/VulkanDevice.cpp isCpuVisible). A gpuOnly buffer therefore maps to
        # null and the readback silently yields zeros.
        staging_pairs = (
            ("frameUserData.gpuCullingStatsStaging =", "gpuCullingStatsStaging"),
            ("frameUserData.gpuCullingDrawCountStaging =", "gpuCullingDrawCountStaging"),
            ("frameUserData.gpuCullingResultStaging =", "gpuCullingResultStaging"),
        )
        for assignment, name in staging_pairs:
            with self.subTest(staging=name):
                block = buffer_desc_block(self.render_device, assignment)
                self.assertIn("rhi::MemoryUsage::gpuToCpu", block)
                self.assertIn("rhi::BufferUsageFlags::transferDst", block)
                self.assertNotIn("rhi::MemoryUsage::gpuOnly", block)

        device_local = (
            ("frameUserData.gpuCullingStatsBuffer =", "gpuCullingStatsBuffer"),
            ("frameUserData.gpuCullingDrawCountBuffer =", "gpuCullingDrawCountBuffer"),
            ("frameUserData.gpuCullingResultBuffer =", "gpuCullingResultBuffer"),
        )
        for assignment, name in device_local:
            with self.subTest(device_local=name):
                block = buffer_desc_block(self.render_device, assignment)
                self.assertIn("rhi::MemoryUsage::gpuOnly", block)
                # The copy source must be transfer-readable on every backend.
                self.assertIn("rhi::BufferUsageFlags::transferSrc", block)

    def test_readback_targets_the_last_submitted_slot(self) -> None:
        # Defect 2: the caller runs right after beginFrame(), so frameIndex is the slot
        # being recorded. Reading it yields the previous cycle's values or zeros.
        body = function_source(self.render_device, "void RenderDevice::cacheGPUCullingStats")

        self.assertIn("const uint32_t readSlot = (frameIndex + slotCount - 1u) % slotCount;", body)
        self.assertIn("const rhi::SubmissionToken readToken = m_frameSlotSubmissions[readSlot];", body)
        # Every buffer access must go through readSlot, never the raw frameIndex.
        self.assertIn("m_perFrame.frameUserData[readSlot]", body)
        self.assertNotIn("m_perFrame.frameUserData[frameIndex]", body)

    def test_readback_is_gated_on_completion_without_waiting(self) -> None:
        body = function_source(self.render_device, "void RenderDevice::cacheGPUCullingStats")

        self.assertIn("isComplete(readToken)", body)
        # A blocking wait on the hot path would defeat the point of the non-blocking gate.
        self.assertNotIn("waitForFrame(", body)
        self.assertNotIn("waitForAllFrames(", body)
        # Failure must preserve the previous values and count the miss, not zero them.
        self.assertIn("++m_statsReadFailures;", body)
        self.assertNotIn("m_lastGPUCullingStats = {};", body)
        self.assertNotIn("m_lastGPUCullingDrawCounts = {};", body)

    def test_staging_copy_is_recorded_after_the_culling_dispatch(self) -> None:
        # Defect 3: the staging copy is only correct if every culling write happens first.
        culling_pass = read("render/passes/GPUDrivenCullingPass.cpp")

        dispatch = culling_pass.index(".groupCountX = (objectCount + shaderio::LGPUCullingThreadCount - 1u) /")
        # Match the compute->transfer barrier on tokens only; the call spans lines with
        # tabs whose exact depth is not part of the contract.
        compute_to_transfer = culling_pass.index("rhi::StageFlags::transfer,")
        first_copy = culling_pass.index("copy->copyBuffer(statsBuffer, 0u, statsStaging, 0u,")
        self.assertLess(dispatch, compute_to_transfer)
        self.assertLess(compute_to_transfer, first_copy)
        # The device-local buffers must be transitioned to TransferSrc before the copy.
        self.assertIn("rhi::ResourceState::TransferSrc", culling_pass)
        self.assertLess(
            culling_pass.index("rhi::ResourceState::TransferSrc"),
            first_copy,
        )


class MeshletStubContractTests(unittest.TestCase):
    def test_meshlet_culling_stub_is_gone(self) -> None:
        # Defect 4: the stub was compiled by the shaders/*.slang glob but referenced by nothing.
        self.assertFalse((REPO_ROOT / "shaders" / "shader.meshlet_culling.slang").exists())
        self.assertFalse((REPO_ROOT / "shaders" / "shader.meshlet_culling.glsl").exists())

    def test_cmake_excludes_the_meshlet_culling_name(self) -> None:
        # Deleting the file is not enough: the glob would pick it up again if reintroduced.
        cmake = read("CMakeLists.txt")
        self.assertRegex(cmake, r'EXCLUDE REGEX "meshlet_culling\\\\\.slang\$"')


class GPUSortKeyContractTests(unittest.TestCase):
    def test_transparent_sort_keys_are_not_computed_on_the_cpu(self) -> None:
        # Defect 5: a per-draw distanceSquared computation in the frame path.
        renderer = read("render/GPUDrivenRenderer.cpp")
        marker = "const auto appendTransparentDraws"
        start = renderer.index(marker)
        body = renderer[start : renderer.index("};", start)]

        self.assertIn("m_visibilitySortInputSceneObjects.push_back", body)
        self.assertNotIn("distanceSquared", body)
        self.assertNotIn("encodeSortableFloatKey", body)
        self.assertNotIn("std::sqrt", body)

    def test_keygen_shader_matches_the_cpu_key_encoding(self) -> None:
        shader = read("shaders/shader.visibility_sort_keygen.slang")

        self.assertIn("0x80000000u", shader)  # transparent category
        self.assertIn("0x3fffffffu", shader)  # key mask
        self.assertIn("0xffffffffu", shader)  # padding
        self.assertIn(">> 2u", shader)
        self.assertIn("asuint", shader)
        # The shader must actually be wired into a pipeline, not just present on disk.
        renderer = read("render/GPUDrivenRenderer.cpp")
        self.assertIn("visibilitySortKeygenMain", renderer)
        self.assertIn("shader_visibility_sort_keygen_slang", renderer)

    def test_padding_key_cannot_match_any_real_category(self) -> None:
        # Defect 6: the transparent visibility patch filters padding slots by comparing the
        # category bits. That only works because 0xffffffff's category bits match no
        # category; a value change would silently leak padding into the draw stream.
        category_mask = 0xC0000000
        categories = (0x00000000, 0x40000000, 0x80000000)  # opaque, alpha, transparent
        padding_key = 0xFFFFFFFF

        self.assertEqual(category_mask, padding_key & category_mask)
        for category in categories:
            with self.subTest(category=hex(category)):
                self.assertNotEqual(category, padding_key & category_mask)
        # The shader must state the same mask/category pair the CPU uses.
        renderer = read("render/GPUDrivenRenderer.cpp")
        self.assertIn("kVisibilitySortCategoryMask = 0xc0000000u", renderer)
        self.assertIn("kVisibilitySortKeyMask = 0x3fffffffu", renderer)


class SteadyStateAllocationContractTests(unittest.TestCase):
    def test_ssr_argument_table_is_persistent_not_per_frame(self) -> None:
        # Defect 7: a per-frame createTemporaryArgumentTable in the SSR path, which
        # allocates a descriptor set every frame on Vulkan.
        renderer = read("render/GPUDrivenRenderer.cpp")
        body = function_source(renderer, "rhi::ArgumentTableHandle GPUDrivenRenderer::acquireSSRTempArgumentTable")

        self.assertIn('createPersistentArgumentTable(m_ssrLayoutHandle, "gpu-driven-ssr-temp")', body)
        self.assertNotIn("createTemporaryArgumentTable", body)
        # Reuse is only safe if the binding signature is compared before rewriting.
        for signature_member in (
            "cache.historyView",
            "cache.depthView",
            "cache.gbufferView0",
            "cache.gbufferView1",
            "cache.rawView",
            "cache.cameraBuffer",
            "cache.cameraOffset",
        ):
            with self.subTest(member=signature_member):
                self.assertIn(signature_member, body)

    def test_capacity_guards_early_return_before_allocating(self) -> None:
        # Defect 8: the guards existed but nothing stopped someone from moving the
        # allocation above the early return.
        render_device = read("render/RenderDevice.cpp")
        guarded = (
            (
                "void RenderDevice::ensureGPUCullingBuffers",
                "if (frameUserData.gpuCullingMeshCapacity >= requiredCapacity)",
            ),
            (
                "void RenderDevice::ensureGPUDrivenPersistentIndirectStreamBuffer",
                "if (frameUserData.gpuDrivenPersistentIndirectStreamCapacity >= requiredCapacity)",
            ),
        )
        for marker, guard in guarded:
            with self.subTest(function=marker):
                body = function_source(render_device, marker)
                self.assertIn(guard, body)
                early_return = body.index("return;", body.index(guard))
                self.assertLess(early_return, body.index("createBuffer"))

        sort_guard = function_source(
            read("render/GPUDrivenRenderer.cpp"),
            "void GPUDrivenRenderer::ensureVisibilitySortCapacity",
        )
        self.assertIn("if (!growSortBuffers && !growPatchBuffers)", sort_guard)
        sort_early_return = sort_guard.index(
            "return;", sort_guard.index("if (!growSortBuffers && !growPatchBuffers)")
        )
        self.assertLess(sort_early_return, sort_guard.index("createBuffer"))


class MeshLodContractTests(unittest.TestCase):
    def test_lod_chain_is_requested_and_rebased(self) -> None:
        # LOD levels are authored relative to their own payload but drawn from the shared index
        # buffer, so registration must rebase them; an unrebas ed offset addresses wrong geometry.
        projection = read("scene/editor/SceneProjection.cpp")
        self.assertIn("buildOptions.buildMeshLod = true;", projection)

        registry = read("render/GPUSceneRegistry.cpp")
        self.assertIn("const uint32_t derivedBase = desc.firstIndex + desc.indexCount;", registry)
        self.assertIn("derivedBase + desc.lodLevels[level].firstIndex", registry)

        mesh_pool = read("render/MeshPool.cpp")
        self.assertIn("meshData.derivedIndices", mesh_pool)
        self.assertIn("sourceIndexBytes", mesh_pool)

    def test_lod_selection_falls_back_to_the_source_range(self) -> None:
        # lodLevels[0] is the first DERIVED level and is already coarser than the source. When
        # even that exceeds the pixel budget the source range must win, which means selection
        # starts at "none chosen" rather than at level 0.
        shader = read("shaders/shader.gpu_culling.slang")
        self.assertIn("int chosenLevel = -1;", shader)
        self.assertIn("kLodPixelErrorThreshold", shader)
        self.assertIn("command.vertexOffset", shader)
        # A level switch must not touch the draw identity or the shared vertex base.
        lod_block_start = shader.index("if(objectData.lodLevelCount > 0u)")
        lod_block = shader[lod_block_start : shader.index("command.instanceCount = 1u;", lod_block_start)]
        # Only assignments matter; the explanatory comment names both fields.
        self.assertNotIn("command.firstInstance", lod_block)
        self.assertNotIn("command.vertexOffset", lod_block)


class HotPathBudgetContractTests(unittest.TestCase):
    def test_stable_recording_budget_predicate_still_covers_descriptors(self) -> None:
        counters = read("rhi/RHIDebugCounters.h")
        predicate = function_source(counters, "stableRecordingBudgetMet")

        for term in (
            "descriptorAllocations == 0",
            "pipelineCreations == 0",
            "argumentLayoutCreations == 0",
            "textureViewCreations == 0",
            "commandRecordingHeapAllocations == 0",
        ):
            with self.subTest(term=term):
                self.assertIn(term, predicate)

    def test_metrics_output_can_observe_the_descriptor_counter(self) -> None:
        app = read("app/MinimalLatestApp.h")
        self.assertIn("hot_path_counters", app)
        self.assertIn("descriptor_allocations", app)


class StatsVisibilityContractTests(unittest.TestCase):
    def test_ui_distinguishes_no_data_from_zero(self) -> None:
        # Without this, a failed or pending readback is indistinguishable from "0 visible".
        app = read("app/MinimalLatestApp.h")
        self.assertIn("isGPUCullingStatsValid", app)
        self.assertIn("No data", app)
        renderer_header = read("render/GPUDrivenRenderer.h")
        self.assertIn("isGPUCullingStatsValid", renderer_header)


if __name__ == "__main__":
    unittest.main(verbosity=2)
