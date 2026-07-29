#!/usr/bin/env python3
"""Static contract for Flax DDGI clear/reset and compute synchronization."""

from __future__ import annotations

import re
import unittest
from pathlib import Path


ROOT = Path(__file__).resolve().parents[1]
FLAX_PASS = (ROOT / "render/passes/FlaxDDGIPass.cpp").read_text(encoding="utf-8")
VULKAN_COMMAND_BUFFER = (
    ROOT / "rhi/vulkan/VulkanCommandBuffer.cpp"
).read_text(encoding="utf-8")


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


def barrier_calls(source: str) -> list[str]:
    return re.findall(
        r"cmd\.barrier\((.*?)\);",
        source,
        flags=re.DOTALL,
    )


class FlaxDDGIClearSyncContractTests(unittest.TestCase):
    def test_pass_local_write_dependency_is_an_image_barrier(self) -> None:
        helper = braced_source(FLAX_PASS, "recordTextureWriteDependency(")

        self.assertIn("std::array<rhi::TextureBarrier", helper)
        self.assertIn(".before = rhi::ResourceState::General", helper)
        self.assertIn(".after = rhi::ResourceState::General", helper)
        self.assertIn("cmd.resourceBarrier(", helper)
        self.assertNotIn("cmd.barrier(", helper)

    def test_pass_local_write_dependency_is_a_buffer_barrier(self) -> None:
        helper = braced_source(FLAX_PASS, "recordBufferWriteDependency(")

        self.assertIn("std::vector<rhi::BufferBarrier>", helper)
        self.assertIn(".before = rhi::ResourceState::General", helper)
        self.assertIn(".after = rhi::ResourceState::General", helper)
        self.assertIn(".offset = 0", helper)
        self.assertIn(".size = 0", helper)
        self.assertIn("cmd.resourceBarrier(", helper)
        self.assertNotIn("cmd.barrier(", helper)

    def test_vulkan_general_to_general_barrier_covers_write_after_write(self) -> None:
        resource_barrier = braced_source(
            VULKAN_COMMAND_BUFFER,
            "void VulkanCommandBuffer::resourceBarrier(",
        )

        self.assertIn(
            ".srcStageMask = VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT",
            resource_barrier,
        )
        self.assertIn(
            ".srcAccessMask = VK_ACCESS_2_MEMORY_WRITE_BIT",
            resource_barrier,
        )
        self.assertIn(
            ".dstStageMask = VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT",
            resource_barrier,
        )
        self.assertIn(
            ".dstAccessMask = VK_ACCESS_2_MEMORY_READ_BIT | "
            "VK_ACCESS_2_MEMORY_WRITE_BIT",
            resource_barrier,
        )

    def test_initial_clear_is_published_before_any_reset_or_compute_write(self) -> None:
        execute = braced_source(FLAX_PASS, "void FlaxDDGIPass::execute(")
        initialization = braced_source(
            execute,
            "if (!m_textureLayoutsInitialized)",
        )

        self.assertEqual(initialization.count("cmd.clearColorTexture("), 7)
        last_clear = initialization.rindex("cmd.clearColorTexture(")
        publish = initialization.index("recordTextureWriteDependency(", last_clear)
        initialized = initialization.index("m_textureLayoutsInitialized = true", publish)
        initialized_this_frame = initialization.index(
            "initializedThisFrame = true",
            initialized,
        )
        self.assertLess(last_clear, publish)
        self.assertLess(publish, initialized)
        self.assertLess(initialized, initialized_this_frame)

    def test_next_frame_waits_for_all_prior_reads_and_resource_writes(self) -> None:
        execute = braced_source(FLAX_PASS, "void FlaxDDGIPass::execute(")
        initialized_path = braced_source(execute, "else\n  {")

        read_to_write = initialized_path.index(
            "rhi::HazardFlags::readBeforeWrite"
        )
        prior_texture_write = initialized_path.index(
            "recordTextureWriteDependency("
        )
        prior_buffer_write = initialized_path.index(
            "recordBufferWriteDependency("
        )
        self.assertIn("rhi::StageFlags::compute", initialized_path)
        self.assertIn("rhi::StageFlags::fragmentShader", initialized_path)
        self.assertIn("rhi::StageFlags::transfer", initialized_path)
        self.assertIn("rhi::StageFlags::commandInput", initialized_path)
        self.assertIn("if(updateRequested)", initialized_path)
        self.assertLess(read_to_write, prior_texture_write)
        self.assertLess(prior_texture_write, prior_buffer_write)

    def test_read_before_write_is_never_mixed_with_write_hazards(self) -> None:
        for call in barrier_calls(FLAX_PASS):
            if "rhi::HazardFlags::readBeforeWrite" not in call:
                continue
            self.assertNotIn("rhi::HazardFlags::textureWrites", call)
            self.assertNotIn("rhi::HazardFlags::bufferWrites", call)
            self.assertNotIn("rhi::HazardFlags::storageBufferReadWrite", call)

    def test_runtime_reset_clear_is_not_duplicated_on_initialization_frame(self) -> None:
        execute = braced_source(FLAX_PASS, "void FlaxDDGIPass::execute(")
        reset = braced_source(execute, "if (resetRequested)")

        self.assertIn("if (!initializedThisFrame)", reset)
        reset_clear = braced_source(reset, "if (!initializedThisFrame)")
        self.assertEqual(reset_clear.count("cmd.clearColorTexture("), 5)

    def test_runtime_reset_clear_is_published_before_compute(self) -> None:
        execute = braced_source(FLAX_PASS, "void FlaxDDGIPass::execute(")
        reset = braced_source(execute, "if (resetRequested)")
        reset_clear = braced_source(reset, "if (!initializedThisFrame)")

        last_clear = reset_clear.rindex("cmd.clearColorTexture(")
        publish = reset_clear.index("recordTextureWriteDependency(", last_clear)
        self.assertLess(last_clear, publish)

    def test_freeze_fast_path_and_reset_only_path_preserve_order(self) -> None:
        execute = braced_source(FLAX_PASS, "void FlaxDDGIPass::execute(")
        idle_return = execute.index(
            "if(!updateRequested && !resetRequested) return;"
        )
        begin_event = execute.index('cmd.beginEvent("FlaxDDGIPass")')
        reset = execute.index("if (resetRequested)")
        reset_only_return = execute.index("if(!updateRequested)", reset)

        self.assertLess(idle_return, begin_event)
        self.assertLess(begin_event, reset)
        self.assertLess(reset, reset_only_return)

    def test_transfer_clear_feeds_storage_read_write_consumers(self) -> None:
        execute = braced_source(FLAX_PASS, "void FlaxDDGIPass::execute(")
        clear_start = execute.index("rhi::ComputeEncoder* clear")
        clear_end = execute.index(
            "if (m_ddgiUniformBuffers[frameIndex].isNull())",
            clear_start,
        )
        clear = execute[clear_start:clear_end]

        self.assertIn("rhi::StageFlags::transfer", clear)
        self.assertIn("rhi::StageFlags::compute", clear)
        self.assertIn("rhi::HazardFlags::bufferWrites", clear)
        self.assertIn("rhi::HazardFlags::storageBufferReadWrite", clear)

    def test_classify_publishes_probe_data_and_active_lists_per_cascade(self) -> None:
        execute = braced_source(FLAX_PASS, "void FlaxDDGIPass::execute(")
        classify = braced_source(execute, 'cmd.beginEvent("DDGI.Classify")')
        cascade_loop = braced_source(
            classify,
            "for(uint32_t cascadeIndex = 0; cascadeIndex < cascadeCount;",
        )

        self.assertIn("m_flaxResources->getProbesData()", cascade_loop)
        self.assertIn("recordTextureWriteDependency(", cascade_loop)
        self.assertIn("rhi::HazardFlags::bufferWrites", cascade_loop)
        self.assertIn(
            "rhi::HazardFlags::storageBufferReadWrite",
            cascade_loop,
        )
        self.assertLess(
            cascade_loop.index("cmd.endEncoding()"),
            cascade_loop.index("recordTextureWriteDependency("),
        )

    def test_compact_and_init_args_publish_rw_buffers_per_cascade(self) -> None:
        execute = braced_source(FLAX_PASS, "void FlaxDDGIPass::execute(")
        compact = braced_source(execute, 'cmd.beginEvent("DDGI.Compact")')
        compact_loop = braced_source(
            compact,
            "for(uint32_t cascadeIndex = 0; cascadeIndex < cascadeCount;",
        )
        init_args = braced_source(execute, 'cmd.beginEvent("DDGI.InitArgs")')

        self.assertIn("cmd.endEncoding()", compact_loop)
        self.assertIn("rhi::HazardFlags::bufferWrites", compact_loop)
        self.assertIn(
            "rhi::HazardFlags::storageBufferReadWrite",
            compact_loop,
        )
        self.assertIn("rhi::HazardFlags::bufferWrites", init_args)
        self.assertIn(
            "rhi::HazardFlags::storageBufferReadWrite",
            init_args,
        )
        self.assertIn("rhi::HazardFlags::drawArguments", init_args)

    def test_classify_and_update_inactive_publish_probe_data_writes(self) -> None:
        execute = braced_source(FLAX_PASS, "void FlaxDDGIPass::execute(")
        update_inactive = braced_source(
            execute,
            'cmd.beginEvent("DDGI.UpdateInactive")',
        )

        self.assertIn("m_flaxResources->getProbesData()", update_inactive)
        self.assertIn("recordTextureWriteDependency(", update_inactive)

    def test_trace_splits_raw_waw_from_history_war(self) -> None:
        execute = braced_source(FLAX_PASS, "void FlaxDDGIPass::execute(")
        trace = braced_source(execute, 'cmd.beginEvent("DDGI.TraceRays")')

        publish = trace.index("recordTextureWriteDependency(")
        history_war = trace.index("rhi::HazardFlags::readBeforeWrite")
        self.assertIn("m_flaxResources->getProbesTrace()", trace)
        self.assertLess(publish, history_war)

    def test_distance_and_irradiance_publish_both_physical_histories(self) -> None:
        execute = braced_source(FLAX_PASS, "void FlaxDDGIPass::execute(")
        distance = braced_source(
            execute,
            'cmd.beginEvent("DDGI.UpdateDistance")',
        )
        irradiance = braced_source(
            execute,
            'cmd.beginEvent("DDGI.UpdateIrradiance")',
        )

        for suffix in ("Write(0)", "Read(0)"):
            self.assertIn(f"getProbesDistance{suffix}", distance)
            self.assertIn(f"getProbesIrradiance{suffix}", irradiance)
        self.assertIn("m_flaxResources->getProbesData()", irradiance)
        self.assertIn("recordTextureWriteDependency(", distance)
        self.assertIn("recordTextureWriteDependency(", irradiance)

    def test_shutdown_forgets_external_texture_state(self) -> None:
        shutdown = braced_source(
            FLAX_PASS,
            "void FlaxDDGIPass::shutdownResources()",
        )
        self.assertIn("m_textureLayoutsInitialized = false", shutdown)


if __name__ == "__main__":
    unittest.main()
