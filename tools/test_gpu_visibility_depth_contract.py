from __future__ import annotations

import os
import re
import shutil
import subprocess
import tempfile
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


class GPUVisibilityDepthContractTests(unittest.TestCase):
    def test_visibility_sort_zero_singleton_and_multi_element_contract(self) -> None:
        header = read("render/GPUDrivenRenderer.h")
        sort_pass = read("render/passes/GPUDrivenVisibilitySortPass.cpp")

        dispatch_snapshot = function_source(
            header,
            "VisibilitySortDispatch getVisibilitySortDispatch",
        )
        self.assertIn("if (frameIndex >= m_visibilitySortFrames.size())", dispatch_snapshot)
        self.assertIn("const bool copyBuffersValid =", dispatch_snapshot)
        self.assertIn("const bool bitonicDispatchReady =", dispatch_snapshot)
        self.assertIn("f.paddedElementCount <= 1u", dispatch_snapshot)
        self.assertIn("f.paddedElementCount > 0u", dispatch_snapshot)
        self.assertNotIn(
            "m_visibilitySortPipelineHandle.isNull() || frameIndex",
            dispatch_snapshot,
        )

        record = function_source(sort_pass, "void recordVisibilitySort")
        valid_guard = record.index("if (!sort.valid)")
        key_copy = record.index(
            "copyEnc->copyBuffer(sort.uploadKeyBufferHandle"
        )
        value_copy = record.index(
            "copyEnc->copyBuffer(sort.uploadValueBufferHandle"
        )
        copy_barrier = record.index(
            "context.commandBuffer->barrier(rhi::StageFlags::transfer"
        )
        singleton_exit = record.index("if (sort.paddedElementCount <= 1u)")
        bitonic_loop = record.index(
            "for (uint32_t level = 2u; level <= sort.paddedElementCount"
        )

        self.assertLess(valid_guard, key_copy)
        self.assertLess(key_copy, value_copy)
        self.assertLess(value_copy, copy_barrier)
        self.assertLess(copy_barrier, singleton_exit)
        self.assertLess(singleton_exit, bitonic_loop)
        self.assertNotIn("sort.pipelineHandle.isNull()", record[:key_copy])
        self.assertNotIn("sort.argumentTable.isNull()", record[:key_copy])
        self.assertIn("rhi::StageFlags::compute", record[copy_barrier:singleton_exit])
        self.assertIn(
            "rhi::HazardFlags::bufferWrites",
            record[copy_barrier:singleton_exit],
        )

    def test_culling_producer_barrier_covers_patch_and_indirect_consumers(self) -> None:
        culling = function_source(
            read("render/passes/GPUDrivenCullingPass.cpp"),
            "void GPUDrivenCullingPass::execute",
        )
        barrier_match = re.search(
            r"cmdBuffer->barrier\(\s*"
            r"rhi::StageFlags::compute,\s*"
            r"rhi::StageFlags::compute\s*\|\s*rhi::StageFlags::commandInput,\s*"
            r"rhi::HazardFlags::drawArguments\s*\|\s*"
            r"rhi::HazardFlags::bufferWrites\s*\);",
            culling,
            re.DOTALL,
        )
        self.assertIsNotNone(barrier_match)
        assert barrier_match is not None
        self.assertLess(culling.index("enc->dispatch("), barrier_match.start())
        self.assertLess(barrier_match.end(), culling.index("cmdBuffer->endEvent();"))
        self.assertIn("visibility-patch compute shader", culling)
        self.assertIn("multi-element bitonic-sort barrier", culling)

        renderer = read("render/GPUDrivenRenderer.cpp")
        self.assertLess(
            renderer.index("m_passExecutor.addPass(*m_gpuCullingPass);"),
            renderer.index("m_passExecutor.addPass(*m_gbufferPass);"),
        )

    def test_meshlet_tombstones_fail_closed_before_classification_and_counts(self) -> None:
        culling_shader = read("shaders/shader.gpu_culling.slang")
        main = function_source(culling_shader, "void gpuCullingMain")

        tombstone_guard = main.index(
            "if(useMeshletCulling && meshlets[objectIndex].objectIndex == 0xffffffffu)"
        )
        live_total = main.index("InterlockedAdd(cullStats[0].totalCount, 1u);")
        object_read = main.index(
            "const GPUCullObject objectData = gpuCullObjects[objectIndex];"
        )
        classification = main.index("const bool isTransparent")
        draw_total = main.index("InterlockedAdd(drawCounts[0].totalCount, 1u);")
        self.assertLess(tombstone_guard, live_total)
        self.assertLess(tombstone_guard, object_read)
        self.assertLess(tombstone_guard, classification)
        self.assertLess(tombstone_guard, draw_total)
        self.assertLess(live_total, object_read)
        self.assertNotIn("cullStats[0].totalCount = objectCount", main)

        guard_body = main[tombstone_guard:live_total]
        for field_write in (
            "tombstoneCommand.indexCount = 0u;",
            "tombstoneCommand.instanceCount = 0u;",
            "tombstoneCommand.firstIndex = 0u;",
            "tombstoneCommand.vertexOffset = 0;",
            "tombstoneCommand.firstInstance = 0u;",
            "indirectCommands[objectIndex] = tombstoneCommand;",
        ):
            self.assertIn(field_write, guard_body)
        self.assertIn("return;", guard_body)
        self.assertNotIn("InterlockedAdd", guard_body)
        self.assertNotIn("gpuCullObjects[objectIndex]", guard_body)
        self.assertNotIn("drawCounts[0]", guard_body)
        self.assertNotIn("cullStats[0]", guard_body)
        self.assertNotIn("fallback", main[object_read:])

        visibility_patch = read("shaders/shader.transparent_visibility_patch.slang")
        self.assertIn("return command.instanceCount != 0u;", visibility_patch)

        model = read("tests/gpu_visibility_depth_model_tests.cpp")
        for model_contract in (
            "deleting a meshlet before first render increments no culling or draw count",
            "first-render tombstone overwrites an uninitialized or stale raw indirect command with zero",
            "tombstone and partially culled live meshlets publish non-executable raw commands",
            "each indirect category count equals the commands actually patched",
            "total indirect count equals the complete set of actually patched live commands",
        ):
            self.assertIn(model_contract, model)
    def test_visibility_patch_barrier_contract_covers_scan_and_category_reuse(self) -> None:
        renderer = read("render/GPUDrivenRenderer.cpp")
        header = read("render/GPUDrivenRenderer.h")

        raw_barrier = function_source(
            renderer, "void recordVisibilityPatchScanReadAfterWriteBarrier"
        )
        self.assertEqual(raw_barrier.count("rhi::StageFlags::compute"), 2)
        self.assertIn("rhi::HazardFlags::bufferWrites", raw_barrier)

        war_barrier = function_source(
            renderer, "void recordVisibilityPatchPrefixReuseWriteAfterReadBarrier"
        )
        self.assertEqual(war_barrier.count("rhi::StageFlags::compute"), 2)
        self.assertIn("rhi::HazardFlags::readBeforeWrite", war_barrier)

        indirect_barrier = function_source(
            renderer, "void recordVisibilityPatchIndirectReadBarrier"
        )
        self.assertIn("rhi::StageFlags::compute", indirect_barrier)
        self.assertIn("rhi::StageFlags::commandInput", indirect_barrier)
        self.assertIn("rhi::HazardFlags::drawArguments", indirect_barrier)
        self.assertIn("rhi::HazardFlags::bufferWrites", indirect_barrier)

        patch = function_source(
            renderer, "bool GPUDrivenRenderer::prepareAndDispatchVisibilityPatch"
        )
        first_dispatch = patch.index("dispatchPatch(pushConstants);")
        self.assertLess(
            patch.index("if (frameResources.prefixReuseBarrierPending)"),
            first_dispatch,
        )
        self.assertLess(
            patch.index(
                "recordVisibilityPatchPrefixReuseWriteAfterReadBarrier(cmdBuffer);"
            ),
            first_dispatch,
        )
        self.assertEqual(
            patch.count("recordVisibilityPatchScanReadAfterWriteBarrier(cmdBuffer);"),
            2,
        )
        self.assertIn("scanBufferIndex = 1u - scanBufferIndex;", patch)
        self.assertLess(
            patch.index("frameResources.prefixReuseBarrierPending = true;"),
            patch.index("recordVisibilityPatchIndirectReadBarrier(cmdBuffer);"),
        )
        self.assertIn("bool prefixReuseBarrierPending{false};", header)

    def test_previous_raw_bootstrap_requires_matching_topology_identity(self) -> None:
        renderer = read("render/GPUDrivenRenderer.cpp")
        header = read("render/GPUDrivenRenderer.h")

        advance = function_source(
            renderer, "void GPUDrivenRenderer::advanceSceneTopologyVersion"
        )
        self.assertIn("++m_sceneTopologyVersion;", advance)
        self.assertIn("invalidateSortedBootstrapStates();", advance)
        self.assertIn("invalidateRawCullingBootstrapStates();", advance)
        self.assertEqual(renderer.count("++m_sceneTopologyVersion;"), 1)

        record = function_source(
            renderer, "void GPUDrivenRenderer::recordRawCullingBootstrapState"
        )
        self.assertIn(".sceneTopologyVersion = m_sceneTopologyVersion", record)
        self.assertIn(".valid = true", record)

        previous = function_source(
            renderer,
            "GPUDrivenRenderer::PreviousRawCullingBootstrap "
            "GPUDrivenRenderer::getPreviousRawCullingBootstrap",
        )
        generation_check = previous.index(
            "frameState.sceneTopologyVersion != m_sceneTopologyVersion"
        )
        rhi_handle_lookup = previous.index(
            "m_renderer.getPreviousGPUCullingIndirectBufferRHIHandle"
        )
        self.assertLess(generation_check, rhi_handle_lookup)
        self.assertNotIn("Opaque", previous)
        self.assertIn("!frameState.valid || frameState.objectCount == 0u", previous)
        self.assertIn(".objectCount = frameState.objectCount", previous)
        self.assertIn(".valid = true", previous)

        self.assertIn("sceneTopologyVersion", header)
        self.assertIn(
            "PreviousRawCullingBootstrap getPreviousRawCullingBootstrap",
            header,
        )

    def test_temporal_count_reset_is_gpu_ordered_before_culling(self) -> None:
        render_device = read("render/RenderDevice.cpp")
        reset = function_source(
            render_device,
            "static void recordGPUCullingDrawCountReset",
        )

        pre_clear_barrier = re.search(
            r"cmdBuffer\.barrier\(\s*"
            r"rhi::StageFlags::compute\s*\|\s*rhi::StageFlags::commandInput,\s*"
            r"rhi::StageFlags::transfer,\s*"
            r"rhi::HazardFlags::readBeforeWrite\s*\);",
            reset,
            re.DOTALL,
        )
        self.assertIsNotNone(pre_clear_barrier)
        assert pre_clear_barrier is not None

        fill = reset.index(
            "clear->fillBuffer(drawCountBuffer, 0, "
            "sizeof(shaderio::GPUCullDrawCounts), 0u);"
        )
        post_clear_barrier = re.search(
            r"cmdBuffer\.barrier\(\s*"
            r"rhi::StageFlags::transfer,\s*"
            r"rhi::StageFlags::compute\s*\|\s*rhi::StageFlags::commandInput,\s*"
            r"rhi::HazardFlags::bufferWrites\s*\|\s*"
            r"rhi::HazardFlags::storageBufferReadWrite\s*\|\s*"
            r"rhi::HazardFlags::drawArguments\s*\);",
            reset,
            re.DOTALL,
        )
        self.assertIsNotNone(post_clear_barrier)
        assert post_clear_barrier is not None
        self.assertLess(pre_clear_barrier.end(), fill)
        self.assertLess(fill, post_clear_barrier.start())
        self.assertNotIn("writeHostVisibleBuffer", reset)
        self.assertNotIn("waitIdle", reset)
        self.assertNotIn("readBeforeWrite", reset[fill:])

        ensure = function_source(
            render_device,
            "void RenderDevice::ensureGPUCullingBuffers",
        )
        draw_count_desc = ensure[
            ensure.index("frameUserData.gpuCullingDrawCountBuffer =") :
            ensure.index("frameUserData.gpuCullingStatsBuffer =")
        ]
        self.assertIn("rhi::BufferUsageFlags::transferDst", draw_count_desc)
        self.assertIn("rhi::BufferUsageFlags::storage", draw_count_desc)
        self.assertIn("rhi::BufferUsageFlags::indirect", draw_count_desc)

        update = function_source(
            render_device,
            "void RenderDevice::updateGPUCullingBuffers",
        )
        self.assertNotIn("zeroDrawCounts", update)
        self.assertIsNone(
            re.search(
                r"writeHostVisibleBuffer\([^;]*gpuCullingDrawCountBuffer",
                update,
                re.DOTALL,
            )
        )

        draw_frame = function_source(render_device, "void RenderDevice::drawFrame")
        update_position = draw_frame.index(
            "updateGPUCullingBuffers(currentFrameIndex, params);"
        )
        reset_position = draw_frame.index(
            "recordGPUCullingDrawCountReset("
            "cmdBuffer, frameUserData.gpuCullingStatsBufferRHI,"
        )
        shadow_position = draw_frame.index(
            "updateShadowCullingBuffers(currentFrameIndex, params);"
        )
        execute_position = draw_frame.index("passExecutor.execute(")
        self.assertLess(update_position, reset_position)
        self.assertLess(reset_position, shadow_position)
        self.assertLess(reset_position, execute_position)

        depth_prepass = read("render/passes/GPUDrivenDepthPrepass.cpp")
        self.assertNotIn("recordGPUCullingDrawCountReset", depth_prepass)
        self.assertNotIn("fillBuffer", depth_prepass)

    def test_count_clear_vulkan_barrier_has_exact_atomic_and_indirect_masks(
        self,
    ) -> None:
        stage_header = read("rhi/RHIStageBarrier.h")
        mapping = read("rhi/vulkan/VulkanBarrierConversions.h")

        self.assertIn(
            "storageBufferReadWrite = 1u << 6u",
            stage_header,
        )

        producer_access = function_source(
            mapping,
            "constexpr VkAccessFlags2 inferProducerAccess",
        )
        self.assertIn("StageFlags producerStages", producer_access)
        self.assertIn("hasStage(producerStages, StageFlags::transfer)", producer_access)
        self.assertIn("VK_ACCESS_2_TRANSFER_WRITE_BIT", producer_access)

        consumer_access = function_source(
            mapping,
            "constexpr VkAccessFlags2 inferConsumerAccess",
        )
        self.assertIn(
            "hasHazard(hazards, HazardFlags::storageBufferReadWrite)",
            consumer_access,
        )
        self.assertIn("VK_ACCESS_2_SHADER_STORAGE_READ_BIT", consumer_access)
        self.assertIn("VK_ACCESS_2_SHADER_STORAGE_WRITE_BIT", consumer_access)
        self.assertIn("VK_ACCESS_2_INDIRECT_COMMAND_READ_BIT", consumer_access)

        make_barrier = function_source(
            mapping,
            "constexpr VkMemoryBarrier2 makeMemoryBarrier2",
        )
        self.assertIn(
            ".srcAccessMask = inferProducerAccess(hazards, producer)",
            make_barrier,
        )
        self.assertIn(
            ".dstAccessMask = inferConsumerAccess(hazards, consumer)",
            make_barrier,
        )
        conversion_test = " ".join(
            read("tests/vulkan_conversion_tests.cpp").split()
        )
        self.assertIn(
            "HazardFlags::bufferWrites | HazardFlags::storageBufferReadWrite | "
            "HazardFlags::drawArguments",
            conversion_test,
        )
        self.assertNotIn("HazardFlags::readBeforeWrite", conversion_test)
        self.assertIn(
            "barrier.srcStageMask == VK_PIPELINE_STAGE_2_ALL_TRANSFER_BIT",
            conversion_test,
        )
        self.assertIn(
            "barrier.srcAccessMask == VK_ACCESS_2_TRANSFER_WRITE_BIT",
            conversion_test,
        )
        self.assertIn(
            "VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT | "
            "VK_PIPELINE_STAGE_2_DRAW_INDIRECT_BIT",
            conversion_test,
        )
        self.assertIn(
            "VK_ACCESS_2_SHADER_STORAGE_READ_BIT | "
            "VK_ACCESS_2_SHADER_STORAGE_WRITE_BIT | "
            "VK_ACCESS_2_INDIRECT_COMMAND_READ_BIT",
            conversion_test,
        )

        runtime_barrier = function_source(
            read("rhi/vulkan/VulkanCommandBuffer.cpp"),
            "void VulkanCommandBuffer::barrier",
        )
        self.assertIn(
            "makeMemoryBarrier2(producer, consumer, hazards)",
            runtime_barrier,
        )

    def test_depth_prepass_never_queries_unversioned_previous_raw_streams(self) -> None:
        depth_prepass = read("render/passes/GPUDrivenDepthPrepass.cpp")

        self.assertIn("getPreviousRawCullingBootstrap", depth_prepass)
        self.assertIn("previousRawBootstrap.valid", depth_prepass)
        for direct_raw_getter in (
            "getPreviousGPUCullingIndirectBufferOpaque",
            "getPreviousGPUCullingDrawCountBufferOpaque",
            "getPreviousGPUCullingObjectCount",
            "getPreviousGPUCullingIndirectBufferRHIHandle",
            "getPreviousGPUCullingDrawCountBufferRHIHandle",
        ):
            self.assertNotIn(direct_raw_getter, depth_prepass)

    def test_raw_bootstrap_publication_tracks_only_current_culling_dispatch(self) -> None:
        renderer = read("render/GPUDrivenRenderer.cpp")
        gbuffer = read("render/passes/GPUDrivenGBufferPass.cpp")

        self.assertIn("invalidateRawCullingBootstrapState(frameIndex);", renderer)
        self.assertIn(
            "m_transparentVisibilityPatchFrames[frameIndex]."
            "prefixReuseBarrierPending = false;",
            renderer,
        )
        self.assertIn("const bool currentRawCullingProduced =", gbuffer)
        self.assertIn(
            "publishRawCullingBootstrapStateForFrame("
            "context.frameIndex, currentIndirectObjectCount);",
            gbuffer,
        )
        self.assertIn(
            "if (context.drawStream != nullptr && currentRawCullingProduced)",
            gbuffer,
        )

    def test_previous_visible_depth_is_only_a_hiz_bootstrap(self) -> None:
        renderer = read("render/GPUDrivenRenderer.cpp")
        order = [
            "m_passExecutor.addPass(*m_depthPrepass);",
            "m_passExecutor.addPass(*m_depthPyramidPass);",
            "m_passExecutor.addPass(*m_gpuCullingPass);",
            "m_passExecutor.addPass(*m_visibilitySortPass);",
            "m_passExecutor.addPass(*m_gbufferPass);",
        ]
        positions = [renderer.index(marker) for marker in order]
        self.assertEqual(positions, sorted(positions))

    def test_gbuffer_clears_and_rebuilds_reverse_z_depth(self) -> None:
        gbuffer = read("render/passes/GPUDrivenGBufferPass.cpp")
        self.assertRegex(
            gbuffer,
            re.compile(
                r"kPassSceneDepthHandle,\s*ResourceAccess::write,\s*"
                r"rhi::ShaderStage::fragment,\s*"
                r"rhi::ResourceState::DepthStencilAttachment"
            ),
        )
        self.assertRegex(
            gbuffer,
            re.compile(
                r"const rhi::DepthTargetDesc depthTarget\{.*?"
                r"\.state = rhi::ResourceState::DepthStencilAttachment,.*?"
                r"\.loadOp = rhi::LoadOp::clear,.*?"
                r"\.storeOp = rhi::StoreOp::store,.*?"
                r"\.clearValue = \{0\.0f, 0\}",
                re.DOTALL,
            ),
        )
        self.assertNotIn("rhi::ResourceState::DepthStencilReadOnly", gbuffer)

    def test_current_visibility_patch_precedes_canonical_depth_rebuild(self) -> None:
        gbuffer = read("render/passes/GPUDrivenGBufferPass.cpp")
        self.assertLess(
            gbuffer.index("prepareAndDispatchVisibilityPatch"),
            gbuffer.index("beginRenderPass(passDesc)"),
        )

    def test_gbuffer_pipeline_uses_reverse_z_write_compare(self) -> None:
        render_device = read("render/RenderDevice.cpp")
        gbuffer_pipeline = render_device[
            render_device.index("rhi::GraphicsPipelineDesc gbufferGraphicsDesc") :
            render_device.index("// Create Shadow pipeline")
        ]
        self.assertIn(
            "rhi::DepthState{true, true, rhi::CompareOp::greaterOrEqual}",
            gbuffer_pipeline,
        )
        self.assertNotIn(
            "rhi::DepthState{true, false, rhi::CompareOp::greaterOrEqual}",
            gbuffer_pipeline,
        )

    def test_depth_pyramid_uses_reverse_z_farthest_reduction(self) -> None:
        depth_pyramid = read("shaders/shader.depth_pyramid.slang")

        self.assertIn("float min4(float a, float b, float c, float d)", depth_pyramid)
        self.assertIn("return min(min(a, b), min(c, d));", depth_pyramid)
        self.assertIn("v = min4(d0, d1, d2, d3);", depth_pyramid)
        self.assertEqual(depth_pyramid.count("float r = min4("), 4)
        self.assertNotIn("float max4(", depth_pyramid)
        self.assertNotIn("v = max4(", depth_pyramid)

        self.assertIn("float loadSourceDepthOrFar(uint2 coord)", depth_pyramid)
        self.assertRegex(
            depth_pyramid,
            re.compile(
                r"coord\.x >= sourceSize\.x \|\| coord\.y >= sourceSize\.y\)\s*"
                r"\{\s*.*?return 0\.0;",
                re.DOTALL,
            ),
        )
        fixed_clear_mips = re.findall(
            r"^\s+CLEAR_UNUSED_MIP\((\d+)u\)$",
            depth_pyramid,
            re.MULTILINE,
        )
        self.assertEqual(
            [int(mip_level) for mip_level in fixed_clear_mips],
            list(range(5, 32)),
        )
        self.assertIn("#define CLEAR_UNUSED_MIP(MIP_LEVEL)", depth_pyramid)
        self.assertNotIn("for(uint mipLevel = kGeneratedMipCount", depth_pyramid)
        self.assertIn(
            "depthPyramidTexture[MIP_LEVEL][dispatchThreadId.xy] = 0.0;",
            depth_pyramid,
        )
        self.assertNotIn("depthPyramidTexture[mipLevel]", depth_pyramid)

    def test_hiz_culling_samples_every_overlapped_footprint_texel(self) -> None:
        culling_shader = read("shaders/shader.gpu_culling.slang")

        self.assertIn("kDepthPyramidGeneratedMipCount = 5u", culling_shader)
        self.assertIn(
            "const uint usableMipCount = min(mipCount, kDepthPyramidGeneratedMipCount);",
            culling_shader,
        )
        self.assertIn("trySelectConservativeMip(", culling_shader)
        self.assertIn("sampleHiZFootprintMinDepth(", culling_shader)
        loader = function_source(culling_shader, "float loadDepthPyramidMip")
        for mip_level in range(5):
            self.assertIn(
                f"depthPyramidTexture[{mip_level}].Load",
                loader,
            )
        self.assertNotIn("NonUniformResourceIndex", culling_shader)
        self.assertNotIn("depthPyramidTexture[mipLevel]", culling_shader)
        self.assertRegex(
            culling_shader,
            re.compile(
                r"for\(int y = sampleMin\.y; y <= sampleMax\.y; \+\+y\).*?"
                r"for\(int x = sampleMin\.x; x <= sampleMax\.x; \+\+x\).*?"
                r"min\(conservativeDepth,\s*"
                r"loadDepthPyramidMip\(mipLevel, int2\(x, y\)\)\)",
                re.DOTALL,
            ),
        )
        self.assertNotIn("sampleHiZCenterClusterDepth", culling_shader)
        self.assertNotIn("centerCoord", culling_shader)
        self.assertIn(
            "pixelMax.x > representedPixelMax.x + kHiZPixelBoundaryEpsilon",
            culling_shader,
        )
        self.assertIn("if(hizDepth <= depthEpsilon)", culling_shader)
        self.assertIn(
            "return nearestDepth + depthEpsilon < hizDepth;",
            culling_shader,
        )

    def test_hiz_descriptor_arrays_use_only_constant_indices_in_source_and_spirv(
        self,
    ) -> None:
        culling_shader = read("shaders/shader.gpu_culling.slang")
        depth_pyramid_shader = read("shaders/shader.depth_pyramid.slang")
        self.assertNotIn("NonUniformResourceIndex", culling_shader)
        self.assertNotIn("depthPyramidTexture[mipLevel]", culling_shader)
        self.assertNotIn("depthPyramidTexture[mipLevel]", depth_pyramid_shader)

        cache_paths = [
            REPO_ROOT / "out" / "build" / "x64-debug" / "CMakeCache.txt",
            *sorted((REPO_ROOT / "out" / "build").glob("**/CMakeCache.txt")),
        ]
        cache_values: dict[str, str] = {}
        for cache_path in dict.fromkeys(cache_paths):
            if not cache_path.is_file():
                continue
            for line in cache_path.read_text(
                encoding="utf-8", errors="replace"
            ).splitlines():
                for key in (
                    "Slang_SLANGC_EXECUTABLE",
                    "Vulkan_GLSLANG_VALIDATOR_EXECUTABLE",
                ):
                    prefix = f"{key}:FILEPATH="
                    if line.startswith(prefix) and key not in cache_values:
                        cache_values[key] = line[len(prefix) :]

        slangc = Path(cache_values.get("Slang_SLANGC_EXECUTABLE", ""))
        glslang = Path(
            cache_values.get("Vulkan_GLSLANG_VALIDATOR_EXECUTABLE", "")
        )
        spirv_dis_name = "spirv-dis.exe" if os.name == "nt" else "spirv-dis"
        spirv_val_name = "spirv-val.exe" if os.name == "nt" else "spirv-val"
        spirv_dis = glslang.with_name(spirv_dis_name) if glslang.name else Path()
        spirv_val = glslang.with_name(spirv_val_name) if glslang.name else Path()
        if not spirv_dis.is_file():
            discovered_spirv_dis = shutil.which(spirv_dis_name)
            spirv_dis = (
                Path(discovered_spirv_dis) if discovered_spirv_dis else Path()
            )
        if not spirv_val.is_file():
            discovered_spirv_val = shutil.which(spirv_val_name)
            spirv_val = (
                Path(discovered_spirv_val) if discovered_spirv_val else Path()
            )

        self.assertTrue(slangc.is_file(), "Slang compiler was not found")
        self.assertTrue(spirv_dis.is_file(), "spirv-dis was not found")
        self.assertTrue(spirv_val.is_file(), "spirv-val was not found")

        disassemblies: dict[str, str] = {}
        with tempfile.TemporaryDirectory(prefix="mgif-hiz-contract-") as temp_dir:
            for shader_name in (
                "shader.gpu_culling.slang",
                "shader.depth_pyramid.slang",
            ):
                output_spv = Path(temp_dir) / f"{shader_name}.spv"
                compile_result = subprocess.run(
                    [
                        str(slangc),
                        "-emit-spirv-directly",
                        "-matrix-layout-column-major",
                        "-force-glsl-scalar-layout",
                        "-fvk-use-entrypoint-name",
                        "-lang",
                        "slang",
                        "-profile",
                        "spirv_1_6",
                        "-target",
                        "spirv",
                        "-O0",
                        "-g0",
                        f"-I{REPO_ROOT}",
                        "-o",
                        str(output_spv),
                        str(REPO_ROOT / "shaders" / shader_name),
                    ],
                    cwd=REPO_ROOT,
                    check=False,
                    capture_output=True,
                    text=True,
                )
                self.assertEqual(
                    compile_result.returncode,
                    0,
                    compile_result.stdout + compile_result.stderr,
                )
                validation_result = subprocess.run(
                    [
                        str(spirv_val),
                        "--target-env",
                        "vulkan1.3",
                        str(output_spv),
                    ],
                    cwd=REPO_ROOT,
                    check=False,
                    capture_output=True,
                    text=True,
                )
                self.assertEqual(
                    validation_result.returncode,
                    0,
                    validation_result.stdout + validation_result.stderr,
                )
                disassembly_result = subprocess.run(
                    [str(spirv_dis), str(output_spv)],
                    cwd=REPO_ROOT,
                    check=False,
                    capture_output=True,
                    text=True,
                )
                self.assertEqual(
                    disassembly_result.returncode,
                    0,
                    disassembly_result.stdout + disassembly_result.stderr,
                )
                disassemblies[shader_name] = disassembly_result.stdout

        culling_disassembly = disassemblies["shader.gpu_culling.slang"]
        depth_pyramid_disassembly = disassemblies["shader.depth_pyramid.slang"]
        self.assertIn("OpCapability Shader", culling_disassembly)
        self.assertIn("OpCapability Shader", depth_pyramid_disassembly)
        for forbidden in (
            "OpCapability ShaderNonUniform",
            "OpCapability SampledImageArrayDynamicIndexing",
            "OpCapability SampledImageArrayNonUniformIndexing",
            "OpDecorate NonUniform",
            " NonUniform",
        ):
            self.assertNotIn(forbidden, culling_disassembly)
        for forbidden in (
            "OpCapability ShaderNonUniform",
            "OpCapability StorageImageArrayDynamicIndexing",
            "OpCapability StorageImageArrayNonUniformIndexing",
            "OpDecorate NonUniform",
            " NonUniform",
        ):
            self.assertNotIn(forbidden, depth_pyramid_disassembly)

        def constant_depth_descriptor_indices(disassembly: str) -> set[int]:
            access_lines = [
                line
                for line in disassembly.splitlines()
                if "OpAccessChain" in line and "%depthPyramidTexture" in line
            ]
            self.assertTrue(access_lines, "no depth pyramid descriptor access found")
            indices: set[int] = set()
            for access_line in access_lines:
                constant_match = re.search(
                    r"%(?:u?int)_(\d+)\s*$",
                    access_line,
                )
                self.assertIsNotNone(
                    constant_match,
                    f"dynamic depth descriptor access remains: {access_line}",
                )
                indices.add(int(constant_match.group(1)))
            return indices

        self.assertEqual(
            constant_depth_descriptor_indices(culling_disassembly),
            set(range(5)),
        )
        self.assertEqual(
            constant_depth_descriptor_indices(depth_pyramid_disassembly),
            set(range(32)),
        )

    def test_meshlet_bounds_and_cone_transforms_are_conservative(self) -> None:
        culling_shader = read("shaders/shader.gpu_culling.slang")
        model = read("tests/gpu_visibility_depth_model_tests.cpp")
        sphere_transform = function_source(
            culling_shader, "float4 transformMeshletLocalBoundsSphere"
        )
        cone_transform = function_source(
            culling_shader, "bool tryTransformMeshletConeAxis"
        )
        cone_culling = function_source(
            culling_shader, "bool isMeshletConeCulled"
        )

        self.assertIn("const float frobeniusScale = sqrt(", sphere_transform)
        self.assertEqual(sphere_transform.count("dot(row"), 6)
        self.assertNotIn("maxScale", sphere_transform)
        self.assertNotIn("length(col", sphere_transform)

        self.assertIn("maxRowLengthSq - minRowLengthSq > gramTolerance", cone_transform)
        self.assertIn("abs(dot(row0, row1)) > gramTolerance", cone_transform)
        self.assertIn("const float3 cofactorRow0 = cross(row1, row2);", cone_transform)
        self.assertIn("const float determinant = dot(row0, cofactorRow0);", cone_transform)
        self.assertIn("pipeline is fixed CCW with back-face culling", cone_transform)
        self.assertIn("determinant <= determinantTolerance", cone_transform)
        self.assertRegex(
            cone_transform,
            r"const float3 inverseTransposeAxis\s*=\s*float3\(",
        )
        self.assertNotIn("determinantSign", cone_transform)
        self.assertNotIn("abs(determinant)", cone_transform)
        self.assertIn("any(isnan(row0))", cone_transform)
        self.assertIn("any(isinf(row0))", cone_transform)
        self.assertIn(
            "if(!tryTransformMeshletConeAxis(meshlet.objectIndex, localAxis, worldAxis))",
            cone_culling,
        )
        self.assertIn(
            "coneCutoff + kMeshletConeSimilarityCutoffSlack",
            cone_culling,
        )

        self.assertIn(
            "max column length underestimates parent non-uniform scale times child rotation",
            model,
        )
        self.assertIn(
            "direct linear normal-cone transform falsely culls a parent non-uniform scale ",
            model,
        )
        self.assertIn(
            "times child rotation/shear counterexample",
            model,
        )
        self.assertIn(
            "negative determinant diag(-1,1,1) conservatively disables meshlet cone culling",
            model,
        )
        self.assertIn(
            "negative non-uniform scale times rotation conservatively disables meshlet cone culling",
            model,
        )
        self.assertIn(
            "positive determinant similarity transform keeps conservative cone culling enabled",
            model,
        )
        self.assertIn("&& determinant > determinantTolerance;", model)
        self.assertIn("determinant <= determinantTolerance", model)
        self.assertNotIn("determinantSign", model)
        self.assertIn(
            "near-zero, singular, and non-finite transforms conservatively disable meshlet cone culling",
            model,
        )

    def test_hiz_projection_uses_conservative_eight_corner_bounds_for_all_modes(
        self,
    ) -> None:
        culling_shader = read("shaders/shader.gpu_culling.slang")
        projection = function_source(
            culling_shader, "bool tryProjectConservativeSphereBounds"
        )
        occlusion = function_source(culling_shader, "bool isOccluded")
        main = function_source(culling_shader, "void gpuCullingMain")

        self.assertIn(
            "for(uint cornerIndex = 0u; cornerIndex < 8u; ++cornerIndex)",
            projection,
        )
        self.assertIn(
            "mul(gpuCullingUniforms.projectionMatrix, float4(cornerView, 1.0))",
            projection,
        )
        self.assertIn("cornerClip.w <= clipTolerance", projection)
        self.assertIn("cornerClip.z < -clipTolerance", projection)
        self.assertIn("cornerClip.z > cornerClip.w + clipTolerance", projection)
        self.assertIn("minNdc = min(minNdc, cornerNdc.xy);", projection)
        self.assertIn("maxNdc = max(maxNdc, cornerNdc.xy);", projection)
        self.assertIn(
            "nearestDepth = max(nearestDepth, saturate(cornerNdc.z));",
            projection,
        )

        self.assertNotIn("projectionScale", occlusion)
        self.assertNotIn("nearestPositiveZ", occlusion)
        self.assertEqual(main.count("isOccluded(center, radius)"), 1)
        self.assertLess(
            main.index("if(useMeshletCulling)"),
            main.index("if(isOccluded(center, radius))"),
        )

    def test_hiz_host_dispatch_matches_shader_16x16_threads(self) -> None:
        depth_pyramid_shader = read("shaders/shader.depth_pyramid.slang")
        depth_pyramid_host = read("render/HiZDepthPyramid.cpp")

        self.assertIn("static const uint kGroupSizeX = 16;", depth_pyramid_shader)
        self.assertIn("static const uint kGroupSizeY = 16;", depth_pyramid_shader)
        self.assertIn(
            ".groupCountX = (m_size.width + 15u) / 16u,",
            depth_pyramid_host,
        )
        self.assertIn(
            ".groupCountY = (m_size.height + 15u) / 16u,",
            depth_pyramid_host,
        )
        self.assertNotIn("(m_size.width + 7u) / 8u", depth_pyramid_host)
        self.assertNotIn("(m_size.height + 7u) / 8u", depth_pyramid_host)

    def test_cpp_hiz_model_uses_vulkan_floor_mip_dimensions(self) -> None:
        model = read("tests/gpu_visibility_depth_model_tests.cpp")
        vulkan_dimension = function_source(model, "uint32_t vulkanMipDimension")
        source_to_mip0 = function_source(
            model, "DepthImage reduceSourceDepthToPyramidMip0"
        )
        vulkan_reduction = function_source(
            model, "DepthImage reduceVulkanPyramidMipMin2x2"
        )

        self.assertIn("dimension >>= 1u;", vulkan_dimension)
        self.assertIn("(source.width + 1u) / 2u", source_to_mip0)
        self.assertIn(
            "vulkanMipDimension(source.width, 1u)",
            vulkan_reduction,
        )
        self.assertNotIn("(source.width + 1u) / 2u", vulkan_reduction)
        self.assertIn(
            "Vulkan image mip dimensions floor 5x3 to 2x1 instead of ceil 3x2",
            model,
        )
        self.assertIn(
            "a floor-sized mip represents the first eight source pixels "
            "but not the omitted edge strip",
            model,
        )

    def test_depth_prepass_alpha_mask_matches_gbuffer_with_or_without_texture(self) -> None:
        depth_prepass = read("shaders/shader.depth_prepass.slang")
        gbuffer = read("shaders/shader.gbuffer.slang")

        self.assertEqual(depth_prepass.count("if(!alphaTestEnabled)"), 2)
        self.assertNotIn("!alphaTestEnabled ||", depth_prepass)

        self.assertIn("float alpha = draw.baseColorFactor.a;", depth_prepass)
        self.assertIn("if(draw.baseColorTextureIndex >= 0)", depth_prepass)
        self.assertIn(
            "alpha *= inTexture[baseColorTextureIndex].Sample(input.uv).a;",
            depth_prepass,
        )
        self.assertIn("if(alpha < draw.alphaCutoff)", depth_prepass)

        self.assertIn("float alpha = mdiDraw.baseColorFactor.a;", depth_prepass)
        self.assertIn("if(mdiDraw.baseColorTextureIndex >= 0)", depth_prepass)
        self.assertIn("if(alpha < mdiDraw.alphaCutoff)", depth_prepass)

        self.assertRegex(
            gbuffer,
            re.compile(
                r"float4 color = draw\.baseColorFactor;.*?"
                r"if\(draw\.baseColorTextureIndex >= 0\).*?"
                r"texColor\.a \* draw\.baseColorFactor\.a.*?"
                r"if \(color\.a < draw\.alphaCutoff\)",
                re.DOTALL,
            ),
        )

    def test_bootstrap_and_hiz_keep_reverse_z_semantics(self) -> None:
        depth_prepass = read("render/passes/GPUDrivenDepthPrepass.cpp")
        render_device = read("render/RenderDevice.cpp")
        culling_shader = read("shaders/shader.gpu_culling.slang")

        self.assertIn(".clearValue = {0.0f, 0}", depth_prepass)
        self.assertIn(
            "rhi::DepthState{true, true, rhi::CompareOp::greater}",
            render_device,
        )
        self.assertIn(
            "return nearestDepth + depthEpsilon < hizDepth;",
            culling_shader,
        )


if __name__ == "__main__":
    unittest.main()
