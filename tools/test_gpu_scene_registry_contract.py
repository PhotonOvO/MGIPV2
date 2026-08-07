from __future__ import annotations

import re
import unittest
from pathlib import Path


REPO_ROOT = Path(__file__).resolve().parents[1]


def read(relative_path: str) -> str:
    return (REPO_ROOT / relative_path).read_text(encoding="utf-8")


def braced_source(source: str, signature: str) -> str:
    match = re.search(signature, source)
    if match is None:
        raise AssertionError(f"missing source signature: {signature}")
    open_brace = source.find("{", match.end())
    if open_brace < 0:
        raise AssertionError(f"missing opening brace after: {signature}")

    depth = 0
    for index in range(open_brace, len(source)):
        if source[index] == "{":
            depth += 1
        elif source[index] == "}":
            depth -= 1
            if depth == 0:
                return source[match.start() : index + 1]
    raise AssertionError(f"missing closing brace after: {signature}")


class GPUSceneRegistryContractTests(unittest.TestCase):
    def test_each_recorded_copy_owns_an_aligned_non_overlapping_staging_slice(self) -> None:
        source = read("render/GPUSceneRegistry.cpp")
        copy_ranges = braced_source(
            source,
            r"uint64_t\s+copyRangesToGpu\s*\(",
        )
        sync = braced_source(
            source,
            r"void\s+GPUSceneRegistry::syncToGpu\s*\(",
        )

        self.assertIn("const uint64_t sourceOffset = alignStagingOffset(stagingOffset)", copy_ranges)
        self.assertIn("static_cast<std::byte*>(stagingMapped) + sourceOffset", copy_ranges)
        self.assertRegex(
            copy_ranges,
            r"copy\.copyBuffer\s*\(\s*stagingBufferHandle\s*,\s*sourceOffset\s*,",
        )
        self.assertIn("stagingOffset = sourceOffset + byteCount", copy_ranges)
        self.assertNotRegex(
            copy_ranges,
            r"copy\.copyBuffer\s*\(\s*stagingBufferHandle\s*,\s*0\s*,",
        )

        first_size = sync.index(
            "requiredStagingBytesForRanges<shaderio::GPUSceneObject>"
        )
        second_size = sync.index(
            "requiredStagingBytesForRanges<shaderio::GPUCullObject>"
        )
        ensure = sync.index("ensureStagingCapacity(requiredStagingBytes)")
        begin_copy = sync.index("cmd.beginComputePass()")
        self.assertLess(first_size, second_size)
        self.assertLess(second_size, ensure)
        self.assertLess(ensure, begin_copy)

    def test_staging_growth_unmaps_retires_recreates_and_remaps(self) -> None:
        source = read("render/GPUSceneRegistry.cpp")
        header = read("render/GPUSceneRegistry.h")
        ensure = braced_source(
            source,
            r"void\s+GPUSceneRegistry::ensureStagingCapacity\s*\(",
        )

        self.assertIn("m_updateBufferCapacityBytes", header)
        self.assertIn("while (newCapacityBytes < requiredBytes)", ensure)
        self.assertIn("alignStagingOffset", ensure)
        unmap = ensure.index("unmapBuffer(m_updateBufferRHI)")
        destroy = ensure.index("destroyBuffer(m_updateBufferRHI)")
        create = ensure.index("createBuffer(rhi::BufferDesc{")
        map_buffer = ensure.index("m_updateBufferMapped = m_rhiDevice->mapBuffer(m_updateBufferRHI)")
        publish_capacity = ensure.index("m_updateBufferCapacityBytes = newCapacityBytes")
        self.assertLess(unmap, destroy)
        self.assertLess(destroy, create)
        self.assertLess(create, map_buffer)
        self.assertLess(map_buffer, publish_capacity)
        self.assertIn(".size = newCapacityBytes", ensure)
        self.assertIn("staging buffer mapping failed", ensure)

    def test_initialized_updates_have_war_before_copy_and_raw_after_copy(self) -> None:
        source = read("render/GPUSceneRegistry.cpp")
        sync = braced_source(
            source,
            r"void\s+GPUSceneRegistry::syncToGpu\s*\(",
        )
        ensure_capacity = braced_source(
            source,
            r"void\s+GPUSceneRegistry::ensureCapacity\s*\(",
        )

        initialized_guard = sync.index("if (m_gpuBuffersInitialized)")
        prior_read_barrier = sync.index("rhi::HazardFlags::readBeforeWrite")
        begin_copy = sync.index("cmd.beginComputePass()")
        end_copy = sync.index("cmd.endEncoding()")
        publish_barrier = sync.index("rhi::HazardFlags::bufferWrites")
        mark_initialized = sync.index("m_gpuBuffersInitialized = true")
        self.assertLess(initialized_guard, prior_read_barrier)
        self.assertLess(prior_read_barrier, begin_copy)
        self.assertLess(begin_copy, end_copy)
        self.assertLess(end_copy, publish_barrier)
        self.assertLess(publish_barrier, mark_initialized)
        self.assertIn(
            "rhi::StageFlags::compute | rhi::StageFlags::vertexShader",
            sync,
        )
        self.assertIn("rhi::StageFlags::transfer", sync)
        self.assertIn("m_gpuBuffersInitialized = false", ensure_capacity)

    def test_transfer_war_lowering_uses_transfer_write_access(self) -> None:
        source = read("rhi/vulkan/VulkanBarrierConversions.h")
        consumer_access = braced_source(
            source,
            r"VkAccessFlags2\s+inferConsumerAccess\s*\(",
        )

        self.assertIn("StageFlags consumerStages", consumer_access)
        self.assertIn("hasStage(consumerStages, StageFlags::transfer)", consumer_access)
        self.assertIn("VK_ACCESS_2_TRANSFER_WRITE_BIT", consumer_access)
        self.assertIn("VK_ACCESS_2_SHADER_STORAGE_WRITE_BIT", consumer_access)
        self.assertIn("inferConsumerAccess(hazards, consumer)", source)

    def test_uploads_and_frames_submit_through_the_public_queue_contract(self) -> None:
        device_contract = read("rhi/RHIDevice.h")
        queue_contract = read("rhi/RHIQueue.h")
        vulkan_queue = read("rhi/vulkan/VulkanQueue.cpp")
        frame_scheduler = read("render/FrameScheduler.cpp")
        upload_manager = read("render/UploadManager.cpp")
        renderer = read("render/GPUDrivenRenderer.cpp")

        flush_scene = braced_source(
            renderer,
            r"void\s+GPUDrivenRenderer::flushPendingSceneUploads\s*\(",
        )

        self.assertNotIn("executeImmediateUpload", device_contract)
        self.assertIn("virtual SubmissionToken submit", queue_contract)
        self.assertIn("device.getQueue(rhi::QueueClass::graphics)", frame_scheduler)
        self.assertIn("device.getQueue(rhi::QueueClass::graphics)", upload_manager)
        self.assertIn("device.getQueue(rhi::QueueClass::transfer)", upload_manager)
        self.assertIn("device.createCommandAllocator(rhi::QueueClass::graphics)", frame_scheduler)
        self.assertIn("device.createCommandAllocator(queueClass)", upload_manager)
        self.assertIn("rhi::QueueClass::graphics, slotCount", upload_manager)
        self.assertIn("rhi::QueueClass::transfer, slotCount", upload_manager)
        self.assertIn("m_queue->submit", frame_scheduler)
        self.assertIn("channel.queue->submit", upload_manager)
        self.assertIn("!transfer->info().dedicated", upload_manager)
        self.assertIn("vkQueueSubmit2(m_queue", vulkan_queue)
        self.assertIn("m_renderer.executeUploadCommand", flush_scene)
        self.assertNotIn("waitForIdle", flush_scene)
    def test_model_covers_non_contiguous_descendant_dirty_ranges(self) -> None:
        model = read("tests/gpu_scene_registry_model_tests.cpp")
        cmake = read("CMakeLists.txt")

        self.assertIn("firstDescendant = 1u", model)
        self.assertIn("secondDescendant = 4u", model)
        self.assertIn("commandBuffer.copies.size() == 4u", model)
        self.assertIn("sourceSlicesDoNotOverlap(commandBuffer.copies)", model)
        self.assertIn("gpu_scene_registry_model_tests", cmake)
        self.assertIn("add_test(NAME gpu_scene_registry_model", cmake)


if __name__ == "__main__":
    unittest.main()
