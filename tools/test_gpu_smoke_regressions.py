from pathlib import Path
import re
import unittest


ROOT = Path(__file__).resolve().parents[1]


def read(relative_path: str) -> str:
    return (ROOT / relative_path).read_text(encoding="utf-8")


def function_source(source: str, signature: str) -> str:
    start = source.index(signature)
    opening_brace = source.index("{", start)
    depth = 0
    for index in range(opening_brace, len(source)):
        if source[index] == "{":
            depth += 1
        elif source[index] == "}":
            depth -= 1
            if depth == 0:
                return source[start : index + 1]
    raise ValueError(f"Unterminated function: {signature}")


def braced_source(source: str, start_pattern: str) -> str:
    match = re.search(start_pattern, source, re.MULTILINE)
    if match is None:
        raise ValueError(f"Source pattern not found: {start_pattern}")

    opening_brace = source.find("{", match.end())
    if opening_brace < 0:
        raise ValueError(f"Opening brace not found after: {start_pattern}")

    depth = 0
    for index in range(opening_brace, len(source)):
        if source[index] == "{":
            depth += 1
        elif source[index] == "}":
            depth -= 1
            if depth == 0:
                return source[match.start() : index + 1]
    raise ValueError(f"Unterminated source block: {start_pattern}")


def without_cpp_comments(source: str) -> str:
    return re.sub(r"//[^\n]*|/\*.*?\*/", "", source, flags=re.DOTALL)


class GpuSmokeRegressionTests(unittest.TestCase):
    def test_gpu_smoke_defaults_are_fixed_for_reference_comparison(self) -> None:
        app = read("app/MinimalLatestApp.cpp")
        app_header = read("app/MinimalLatestApp.h")
        render_types = read("render/RenderTypes.h")
        ddgi_config = read("render/DDGIConfig.h")

        self.assertIn("const AutomationOptions automationOptions = parseAutomationOptions", app)
        self.assertIn("MinimalLatestApp app({1920, 1080}, automationOptions)", app)
        self.assertIn("size = {1920, 1080}", app_header)
        self.assertIn("AutomationOptions automationOptions = {}", app_header)
        self.assertIn("bool enablePostProcessing{false}", render_types)
        self.assertIn("bool enableAO{false}", render_types)
        self.assertIn("bool enableSSR{false}", render_types)
        self.assertIn("maxUpdatedProbesPerFrame{4096u}", ddgi_config)
        self.assertIn("raysPerProbe{256u}", ddgi_config)
        self.assertIn("ddgiWeight{1.0f}", ddgi_config)

    def test_flax_active_probe_metadata_and_budget_scratch_are_reset(self) -> None:
        source = read("render/passes/FlaxDDGIPass.cpp")
        execute = source[source.index("void FlaxDDGIPass::execute") :]
        classify = execute.index('cmd.beginEvent("DDGI.Classify")')
        reset = execute.index("clear->fillBuffer")

        self.assertLess(reset, classify)
        reset_block = execute[reset:classify]
        self.assertIn("4u * sizeof(uint32_t)", reset_block)
        self.assertIn("argsScratchOffset", reset_block)
        self.assertIn("getUpdateProbesInitArgs()", reset_block)
        self.assertIn("StageFlags::transfer", reset_block)

    def test_flax_indirect_dispatch_uses_dynamic_global_budget(self) -> None:
        shader = read("shaders/ddgi_flax_init_args.slang")
        pass_source = read("render/passes/FlaxDDGIPass.cpp")

        self.assertIn("CS_CompactActiveProbes", shader)
        self.assertIn("baseUpdateBudget", shader)
        self.assertIn("scratchOffset", shader)
        self.assertIn("activeProbes[3] = initialUpdateCount", shader)
        self.assertIn("uint availableSlack", shader)
        self.assertIn("activeProbes[3] = updateCount", shader)
        self.assertIn("m_compactPipeline", pass_source)
        self.assertIn('cmd.beginEvent("DDGI.Compact")', pass_source)
        init = pass_source[pass_source.index('cmd.beginEvent("DDGI.InitArgs")') :]
        self.assertIn("setRootConstants", init)
        self.assertIn("StageFlags::commandInput", init)

    def test_flax_probe_updates_dispatch_logical_budget_tiles(self) -> None:
        init_args = read("shaders/ddgi_flax_init_args.slang")
        distance = read("shaders/ddgi_flax_update_distance.slang")
        irradiance = read("shaders/ddgi_flax_update_irradiance.slang")
        pass_source = read("render/passes/FlaxDDGIPass.cpp")

        self.assertIn("updateProbesInitArgs[argsOffset + 3u]", init_args)
        self.assertIn("updateProbesInitArgs[argsOffset + 6u]", init_args)
        self.assertIn("getProbeUpdateSelection", distance)
        self.assertIn("getProbeUpdateSelection", irradiance)
        self.assertIn("SV_GroupID", distance)
        self.assertIn("SV_GroupID", irradiance)
        self.assertEqual(pass_source.count("dispatchIndirect("), 3)
        self.assertIn("indirectOffset(cascadeIndex", pass_source)
        self.assertIn("IndirectPass::Trace", pass_source)
        self.assertIn("IndirectPass::Distance", pass_source)
        self.assertIn("IndirectPass::Irradiance", pass_source)

    def test_flax_sdf_sampler_is_always_valid_before_argument_tables(self) -> None:
        source = read("render/passes/FlaxDDGIPass.cpp")
        init = source[source.index("void FlaxDDGIPass::initResources") :]
        init = init[: init.index("void FlaxDDGIPass::shutdownResources")]

        sampler_creation = init.index("m_fallbackSampler = device.createSampler")
        sampler_selection = init.index("rhi::SamplerHandle sdfSampler")
        first_table = init.index("table = device.createArgumentTable(rhi::ArgumentTableCreateDesc{.layout = m_initArgsLayout")
        self.assertLess(sampler_creation, sampler_selection)
        self.assertLess(sampler_selection, first_table)
        self.assertIn("if(sdfSampler.isNull())", init[sampler_selection:first_table])

        self.assertIn("writeSampler(classifyTable, 3, sdfSampler)", init)
        self.assertIn("writeCombinedTextureRO(traceRaysTable, 3", init)
        self.assertIn("writeSampler(traceRaysTable, 4, sdfSampler)", init)

    def test_flax_classification_decodes_global_sdf_encoding(self) -> None:
        shader = read("shaders/ddgi_flax_classify.slang")

        self.assertIn("encodedSDF * 2.0f - 1.0f", shader)
        self.assertIn("maxEncodeDistance", shader)
        self.assertNotIn(
            "globalSDFTex.SampleLevel(linearSampler, sdfUvw, 0) * (boundsMax.x - boundsMin.x)",
            shader,
        )

    def test_flax_descriptors_are_prebuilt_per_frame_cascade_and_parity(self) -> None:
        source = read("render/passes/FlaxDDGIPass.cpp")
        init = source[source.index("void FlaxDDGIPass::initResources") :]
        init = init[: init.index("void FlaxDDGIPass::shutdownResources")]
        execute = source[source.index("void FlaxDDGIPass::execute") :]

        self.assertIn("m_frameCount = std::max(frameCount, 1u)", init)
        self.assertIn("frameCascadeTableCount = m_frameCount * cascadeCount", init)
        self.assertIn("m_classifyTables.resize(frameCascadeTableCount)", init)
        self.assertIn("m_distanceTables.resize(historyTableCount)", init)
        self.assertIn("historyTableIndex(frameIndex, cascadeIndex, parity)", init)
        self.assertIn("writeUniformBuffer(classifyTable, 4, ddgiBuffer", init)
        self.assertIn("writeFlaxDDGIDataToBuffer(frameIndex, currentSpatial)", execute)
        self.assertNotIn("updateArgumentTable", execute)

    def test_flax_probe_budget_uses_deterministic_compaction_and_slack(self) -> None:
        pass_source = read("render/passes/FlaxDDGIPass.cpp")
        pass_header = read("render/passes/FlaxDDGIPass.h")
        classify = read("shaders/ddgi_flax_classify.slang")
        init_args = read("shaders/ddgi_flax_init_args.slang")
        common = read("shaders/flax_ddgi_common.slang")
        shader_io = read("shaders/shader_io.h")
        resources = read("render/FlaxDDGIResources.cpp")

        self.assertIn("m_probeUpdateOffsets", pass_header)
        self.assertIn("m_priorityProbeUpdateOffsets", pass_header)
        self.assertIn("highGeometryComplexity", classify)
        self.assertIn("activeProbes[priorityBase + probeIndex] = 1u", classify)
        self.assertNotIn("InterlockedAdd(activeProbes", classify)
        self.assertIn("static const uint kCompactGroupSize = 256u", init_args)
        self.assertIn("[numthreads(kCompactGroupSize, 1, 1)]", init_args)
        self.assertIn("groupshared uint priorityPrefix[kCompactGroupSize]", init_args)
        self.assertIn("GroupMemoryBarrierWithGroupSync()", init_args)
        self.assertIn("activeProbes[priorityListBase + priorityWrite++] = probeIndex", init_args)
        self.assertIn("InterlockedAdd(updateProbesInitArgs[initArgs.scratchOffset]", init_args)
        self.assertIn("uint priorityListBase = 4u + 2u * totalProbes", common)
        self.assertIn("uint regularListBase = priorityListBase + totalProbes", common)
        self.assertIn("4u + 4u * probeCount", resources)
        self.assertIn("uint updateCount = min(activeProbes[3], activeCount)", common)
        self.assertIn("kFlaxDDGIHighPriorityBudgetNumerator", common)
        self.assertIn("uint regularSlots", common)
        self.assertIn("update every active probe exactly", common)
        self.assertIn("computeFlaxGIHighPriorityUpdateBudget", pass_source)
        self.assertIn("computeFlaxGICascadeUpdateBudget", pass_source)
        self.assertIn("uvec4 updateParams[LFlaxDDGIMaxCascades]", shader_io)

    def test_flax_trace_uses_runtime_global_sdf_bounds(self) -> None:
        shader = read("shaders/ddgi_flax_trace_rays.slang")

        self.assertIn("ddgi.sdfBoundsMinAndVoxel", shader)
        self.assertIn("ddgi.sdfBoundsMaxAndRes", shader)
        self.assertNotIn("float3(-16, -16, -16)", shader)
        self.assertNotIn("float3(16, 16, 16)", shader)

    def test_flax_history_ping_pong_uses_published_output_state(self) -> None:
        pass_source = read("render/passes/FlaxDDGIPass.cpp")
        pass_header = read("render/passes/FlaxDDGIPass.h")
        model = read("render/FlaxGIDebugModel.h")
        renderer = read("render/GPUDrivenRenderer.cpp")
        debug_pass = read("render/passes/DDGIDebugPass.cpp")
        distance = read("shaders/ddgi_flax_update_distance.slang")

        self.assertIn("FlaxGIOutputState", model)
        self.assertIn("m_outputState.nextWriteParity", pass_source)
        self.assertIn("m_outputState.publishPending(currentSpatial)", pass_source)
        self.assertIn("getLightingOutputSelection", pass_header)
        self.assertIn("flaxOutput.atlas.parity", renderer)
        self.assertIn("getFlaxDDGIPublishedOutputSelection", debug_pass)
        self.assertNotIn("sourceFrame & 1u", pass_source)
        self.assertIn("probesDistanceHist", distance)
        self.assertNotIn("float2 historyData = probesDistance.Load", distance)

    def test_flax_renderer_lighting_snapshot_is_consumed_atomically(self) -> None:
        renderer = read("render/GPUDrivenRenderer.cpp")
        update_lights = braced_source(
            renderer,
            r"\bvoid\s+GPUDrivenRenderer::updateGPUDrivenLights\s*\(",
        )
        update_table = braced_source(
            renderer,
            r"\bvoid\s+GPUDrivenRenderer::updateLightingArgumentTable\s*\(",
        )
        compact_lights = re.sub(r"\s+", "", without_cpp_comments(update_lights))
        compact_table = re.sub(r"\s+", "", without_cpp_comments(update_table))

        self.assertEqual(
            len(re.findall(r"\bgetLightingOutputSnapshot\s*\(", update_lights)),
            1,
        )
        self.assertIn("constFlaxGIOutputSnapshotflaxOutput=", compact_lights)
        self.assertIn(
            "getLightingOutputSnapshot(params.debugOptions)",
            compact_lights,
        )
        self.assertEqual(
            compact_lights.count(
                "updateLightingArgumentTable(frameIndex,flaxOutput);"
            ),
            1,
        )
        self.assertIn(
            "voidGPUDrivenRenderer::updateLightingArgumentTable("
            "uint32_tframeIndex,constFlaxGIOutputSnapshot&flaxOutput)",
            compact_table,
        )
        self.assertNotRegex(
            update_table,
            r"\bgetLightingOutput(?:Snapshot|Selection)\s*\(",
        )
        self.assertNotIn("getLightingOutputSelection", renderer)

        spatial_ubo_contracts = (
            "constuint32_tcascadeCount=flaxOutput.isValid()?"
            "std::min(flaxOutput.spatial.cascadeCount,"
            "kFlaxGIMaxPublishedCascades):0u;",
            "lightingUniforms.light.ddgiFlaxEnabledAndCascades=glm::vec4("
            "flaxTexturesReady?1.0f:0.0f,static_cast<float>(cascadeCount),",
            "constFlaxGICascadeSpatialSnapshot&cascade="
            "flaxOutput.spatial.cascades[c];",
            "lightingUniforms.light.ddgiFlaxOriginAndSpacing[c]=glm::vec4("
            "cascade.origin[0],cascade.origin[1],cascade.origin[2],cascade.spacing);",
            "lightingUniforms.light.ddgiFlaxBlendOrigin[c]=glm::vec4("
            "cascade.blendOrigin[0],cascade.blendOrigin[1],"
            "cascade.blendOrigin[2],0.0f);",
            "lightingUniforms.light.ddgiFlaxScrollOffsets[c]=glm::ivec4("
            "cascade.scrollOffset[0],cascade.scrollOffset[1],"
            "cascade.scrollOffset[2],0);",
            "lightingUniforms.light.ddgiFlaxCountsAndRays=glm::uvec4("
            "flaxOutput.spatial.probeCounts[0],"
            "flaxOutput.spatial.probeCounts[1],"
            "flaxOutput.spatial.probeCounts[2],ddgiConfig.raysPerProbe);",
        )
        for contract in spatial_ubo_contracts:
            with self.subTest(spatial_ubo=contract):
                self.assertIn(contract, compact_lights)

        self.assertNotIn("m_flaxDDGICascades", compact_lights)
        self.assertNotIn("m_flaxDDGIResources", compact_lights)

        for accessor in (
            "getProbesDistanceOutputView",
            "getProbesIrradianceOutputView",
        ):
            with self.subTest(descriptor_view=accessor):
                self.assertIn(
                    f"{accessor}(flaxOutput.atlas.parity)",
                    compact_table,
                )
        self.assertNotIn("flaxOutput.parity", compact_table)

    def test_flax_runtime_dispatches_all_configured_cascades(self) -> None:
        renderer = read("render/GPUDrivenRenderer.cpp")
        pass_source = read("render/passes/FlaxDDGIPass.cpp")

        self.assertNotIn("kFlaxImplementedCascadeCount", renderer)
        self.assertNotIn("kFlaxImplementedCascadeCount", pass_source)
        self.assertIn("std::min(config.maxCascades, FlaxDDGIResources::kMaxCascades)", renderer)
        self.assertIn("for(uint32_t cascadeIndex = 0; cascadeIndex < cascadeCount", pass_source)
        self.assertIn("indirectOffset(cascadeIndex", pass_source)

    def test_flax_debug_modes_sample_flax_owned_atlases(self) -> None:
        shader = read("shaders/shader.light.slang")
        debug = shader[shader.index("const uint ddgiDebugMode") :]
        debug = debug[: debug.index("const bool useIBL")]

        self.assertIn("evaluateFlaxDDGIIrradiance", debug)
        self.assertIn("if(flaxDDGIEnabled())", debug)

    def test_incomplete_surface_atlas_falls_back_to_global_albedo(self) -> None:
        shader = read("shaders/ddgi_flax_trace_rays.slang")

        self.assertIn("kUseSurfaceAtlas = false", shader)
        self.assertIn("loadGlobalAlbedoNearest(uvw)", shader)
        self.assertIn("globalAlbedoTex.Load", shader)
        self.assertNotIn("globalAlbedoTex.SampleLevel", shader)

    def test_flax_emissive_meshes_feed_probe_and_direct_area_lighting(self) -> None:
        shader_io = read("shaders/shader_io.h")
        trace = read("shaders/ddgi_flax_trace_rays.slang")
        common = read("shaders/flax_ddgi_common.slang")
        light = read("shaders/shader.light.slang")
        renderer = read("render/GPUDrivenRenderer.cpp")

        self.assertIn("shapeAxisXAndHalfWidth", shader_io)
        self.assertIn("shapeAxisYAndHalfHeight", shader_io)
        self.assertIn("flaxPointOnEmissiveSource", trace)
        self.assertIn("kFlaxEmissiveTransportScale = 0.8f", common)
        self.assertIn("radiance = source.colorAndType.rgb", trace)
        self.assertIn("flaxEmissiveSampleRadiance", common)
        self.assertIn("evaluateFlaxEmissiveAreaLights", light)
        self.assertIn("ddgiFlaxRadianceSources", renderer)

    def test_flax_distance_storage_type_matches_rg16_texture(self) -> None:
        distance = read("shaders/ddgi_flax_update_distance.slang")

        self.assertIn("RWTexture2D<float2> probesDistance", distance)
        self.assertNotIn("RWTexture2D<float4> probesDistance", distance)

    def test_frame_timeline_uses_queue_owned_monotonic_submission_tokens(self) -> None:
        source = read("rhi/vulkan/VulkanQueue.cpp")
        submit = source[source.index("SubmissionToken VulkanQueue::submit") :]

        self.assertIn("m_lastSubmittedValue + 1u", submit)
        self.assertIn("m_lastSubmittedValue = signalValue", submit)
        self.assertIn("SubmissionToken token{m_identity, signalValue}", submit)
        self.assertIn("vkQueueSubmit2(m_queue", submit)
        self.assertIn("recreateExhaustedTimeline()", submit)
        self.assertIn("createTimeline(0)", source)
    def test_render_device_frame_slot_callback_runs_after_prepare_before_recording(self) -> None:
        source = read("render/RenderDevice.cpp")
        render = braced_source(
            source,
            r"\bbool\s+RenderDevice::renderWithPassExecutor\s*\(",
        )

        prepare = re.search(r"\bprepareFrameResources\s*\(\s*\)", render)
        callback = re.search(
            r"\bonFrameSlotReady\s*\(\s*preparedFrameIndex\s*\)",
            render,
        )
        recording = re.search(r"\bbeginCommandRecording\s*\(\s*\)", render)

        self.assertIsNotNone(prepare)
        self.assertIsNotNone(callback)
        self.assertIsNotNone(recording)
        self.assertLess(prepare.start(), callback.start())
        self.assertLess(callback.start(), recording.start())

    def test_gpu_driven_slot_ready_callback_owns_per_frame_preparation(self) -> None:
        source = read("render/GPUDrivenRenderer.cpp")
        render = braced_source(
            source,
            r"\bvoid\s+GPUDrivenRenderer::render\s*\(\s*const\s+RenderParams&\s+params\s*\)",
        )
        slot_ready = braced_source(
            render,
            r"\[\s*&\s*\]\s*\(\s*uint32_t\s+frameIndex\s*\)",
        )

        call_patterns = {
            "persistent draw upload": r"\buploadPersistentDrawData\s*\(\s*frameIndex\s*\)",
            "GPU-driven lights": (
                r"\bupdateGPUDrivenLights\s*\(\s*[^,]+,\s*frameIndex\s*\)"
            ),
            "visibility sort inputs": (
                r"\bprepareVisibilitySortInputs\s*\(\s*frameIndex\s*\)"
            ),
            "phase 7 descriptors": (
                r"\bupdatePhase7Descriptors\s*\(\s*frameIndex\s*\)"
            ),
        }
        prepared_assignment = re.search(
            r"\bpreparedFrameIndex\s*=\s*frameIndex\s*;",
            slot_ready,
        )
        self.assertIsNotNone(prepared_assignment)
        for label, pattern in call_patterns.items():
            with self.subTest(call=label):
                call = re.search(pattern, slot_ready, re.DOTALL)
                self.assertIsNotNone(call)
                self.assertLess(call.start(), prepared_assignment.start())

        post_submit = render[render.index(slot_ready) + len(slot_ready) :]
        self.assertRegex(
            post_submit,
            r"(?s)\bif\s*\(\s*!frameSubmitted\s*\).*?\breturn\s*;"
            r".*?\bconst\s+uint32_t\s+frameIndex\s*=\s*preparedFrameIndex\s*;",
        )
        self.assertNotRegex(
            post_submit,
            r"\bconst\s+uint32_t\s+frameIndex\s*=\s*getCurrentFrameIndexHint\s*\(",
        )

    def test_meshlet_upload_waits_for_all_frames_only_when_data_changed(self) -> None:
        source = read("render/GPUDrivenRenderer.cpp")
        header = read("render/GPUDrivenRenderer.h")
        render = braced_source(
            source,
            r"\bvoid\s+GPUDrivenRenderer::render\s*\(\s*const\s+RenderParams&\s+params\s*\)",
        )
        wait_for_idle = braced_source(
            source,
            r"\bvoid\s+GPUDrivenRenderer::waitForIdle\s*\(",
        )
        drain_and_upload = braced_source(
            source,
            r"\bvoid\s+GPUDrivenRenderer::flushPendingMeshletUpload\s*\(",
        )
        upload_after_idle = braced_source(
            source,
            r"\bvoid\s+GPUDrivenRenderer::uploadPendingMeshletsAfterIdle\s*\(",
        )
        slot_ready = braced_source(
            render,
            r"\[\s*&\s*\]\s*\(\s*uint32_t\s+frameIndex\s*\)",
        )
        scene_upload = braced_source(
            source,
            r"\bvoid\s+GPUDrivenRenderer::flushPendingSceneUploads\s*\(",
        )

        dirty_guard = re.search(
            r"\bif\s*\(\s*!m_enableExperimentalMeshletPath\s*\|\|\s*"
            r"!m_meshletUploadDirty\s*\)",
            drain_and_upload,
        )
        idle_wait = re.search(
            r"\bm_renderer\.waitForIdle\s*\(\s*\)\s*;",
            drain_and_upload,
        )
        drain_upload_after_idle = re.search(
            r"\buploadPendingMeshletsAfterIdle\s*\(\s*\)\s*;",
            drain_and_upload,
        )
        external_idle_wait = re.search(
            r"\bm_renderer\.waitForIdle\s*\(\s*\)\s*;",
            wait_for_idle,
        )
        external_upload_after_idle = re.search(
            r"\buploadPendingMeshletsAfterIdle\s*\(\s*\)\s*;",
            wait_for_idle,
        )
        after_idle_dirty_guard = re.search(
            r"\bif\s*\(\s*!m_enableExperimentalMeshletPath\s*\|\|\s*"
            r"!m_meshletUploadDirty\s*\)",
            upload_after_idle,
        )
        mapped_upload = re.search(
            r"\bm_meshletBuffer\.uploadMeshlets\s*\(",
            upload_after_idle,
        )
        dirty_clear = re.search(
            r"\bm_meshletUploadDirty\s*=\s*false\s*;",
            upload_after_idle,
        )
        render_flush = re.search(
            r"\bflushPendingMeshletUpload\s*\(\s*\)\s*;",
            render,
        )
        pass_submit = re.search(r"\bsubmitPassGraph\s*\(", render)

        self.assertIsNotNone(dirty_guard)
        self.assertIsNotNone(idle_wait)
        self.assertIsNotNone(drain_upload_after_idle)
        self.assertIsNotNone(external_idle_wait)
        self.assertIsNotNone(external_upload_after_idle)
        self.assertIsNotNone(after_idle_dirty_guard)
        self.assertIsNotNone(mapped_upload)
        self.assertIsNotNone(dirty_clear)
        self.assertIsNotNone(render_flush)
        self.assertIsNotNone(pass_submit)
        self.assertLess(dirty_guard.start(), idle_wait.start())
        self.assertLess(idle_wait.start(), drain_upload_after_idle.start())
        self.assertLess(external_idle_wait.start(), external_upload_after_idle.start())
        self.assertLess(after_idle_dirty_guard.start(), mapped_upload.start())
        self.assertLess(mapped_upload.start(), dirty_clear.start())
        self.assertLess(render_flush.start(), pass_submit.start())
        self.assertIn("current-slot wait only retires", drain_and_upload)
        self.assertIn("another slot may still be reading", drain_and_upload)
        self.assertRegex(header, r"\bvoid\s+waitForIdle\s*\(\s*\)\s*;")
        self.assertRegex(
            header,
            r"\bvoid\s+uploadPendingMeshletsAfterIdle\s*\(\s*\)\s*;",
        )
        self.assertIn("bool m_meshletUploadDirty{false}", header)
        self.assertEqual(
            len(re.findall(r"\bm_renderer\.waitForIdle\s*\(\s*\)\s*;", wait_for_idle)),
            1,
        )
        self.assertNotRegex(
            wait_for_idle,
            r"\bflushPendingMeshletUpload\s*\(",
        )
        self.assertNotRegex(
            upload_after_idle,
            r"\bwaitForIdle\s*\(",
        )
        self.assertNotRegex(
            drain_and_upload,
            r"\bm_meshletBuffer\.uploadMeshlets\s*\(",
        )
        self.assertNotRegex(
            slot_ready,
            r"\bflushPendingMeshletUpload\s*\(",
        )
        self.assertNotRegex(
            scene_upload,
            r"\bm_meshletBuffer\.uploadMeshlets\s*\(",
        )
        self.assertEqual(
            len(re.findall(r"\bm_meshletBuffer\.uploadMeshlets\s*\(", source)),
            1,
        )

        transform_updates = (
            braced_source(source, r"\bvoid\s+GPUDrivenRenderer::updateMeshTransform\s*\("),
            braced_source(
                source,
                r"\bvoid\s+GPUDrivenRenderer::updateSceneInstanceTransform\s*\(",
            ),
        )
        for transform_update in transform_updates:
            self.assertNotRegex(
                transform_update,
                r"\bm_meshletUploadDirty\s*=\s*true\s*;",
            )

        rebuilds = (
            braced_source(
                source,
                r"\bvoid\s+GPUDrivenRenderer::rebuildGPUDrivenScene\s*"
                r"\(\s*const\s+GltfModel&",
            ),
            braced_source(
                source,
                r"\bvoid\s+GPUDrivenRenderer::rebuildGPUDrivenScene\s*"
                r"\(\s*const\s+SceneAsset&",
            ),
        )
        for rebuild in rebuilds:
            with self.subTest(rebuild=rebuild[:80]):
                self.assertRegex(
                    rebuild,
                    r"\bif\s*\(\s*m_enableExperimentalMeshletPath\s*&&\s*"
                    r"appendedMeshlets\s*\)",
                )
                self.assertRegex(
                    rebuild,
                    r"\bm_meshletUploadDirty\s*=\s*true\s*;",
                )
                self.assertNotRegex(
                    rebuild,
                    r"\bm_meshletBuffer\.uploadMeshlets\s*\(",
                )

    def test_imgui_uses_authoritative_frame_resource_count(self) -> None:
        source = read("render/RenderDevice.cpp")
        init_imgui = braced_source(
            source,
            r"\bvoid\s+RenderDevice::initImGui\s*\(",
        )

        self.assertRegex(
            init_imgui,
            r"\.frameCount\s*=\s*getFrameResourceCount\s*\(\s*\)\s*,",
        )
        self.assertNotRegex(
            init_imgui,
            r"\b(?:getMaxFramesInFlight|getSwapchainImageCount)\s*\(",
        )

    def test_bindless_material_upload_layout_matches_sampled_descriptor_contract(
        self,
    ) -> None:
        render_device = read("render/RenderDevice.cpp")
        argument_table = read("rhi/RHIArgumentTable.h")
        vulkan_device = read("rhi/vulkan/VulkanDevice.cpp")

        material_uploads = {
            "default material texture": braced_source(
                render_device,
                r"\bRenderDevice::UploadedImageHandles\s+"
                r"RenderDevice::uploadRawRgba8Image\s*\(",
            ),
            "SceneAsset material textures": braced_source(
                render_device,
                r"\bSceneUploadResult\s+RenderDevice::commitSceneUploadPlan\s*\(",
            ),
            "glTF material textures": braced_source(
                render_device,
                r"\bvoid\s+RenderDevice::uploadGltfModelBatch\s*\(",
            ),
        }
        for label, upload in material_uploads.items():
            with self.subTest(upload=label):
                upload_end_barriers = re.findall(
                    r"\bconst\s+rhi::TextureBarrier\s+uploadEndBarrier\b",
                    upload,
                )
                self.assertEqual(len(upload_end_barriers), 1)
                upload_end = braced_source(
                    upload,
                    r"\bconst\s+rhi::TextureBarrier\s+uploadEndBarrier\b",
                )
                self.assertRegex(
                    upload_end,
                    r"\.before\s*=\s*rhi::ResourceState::TransferDst\s*,",
                )
                self.assertRegex(
                    upload_end,
                    r"\.after\s*=\s*rhi::ResourceState::ShaderRead\s*,",
                )
                self.assertNotRegex(
                    upload_end,
                    r"\.after\s*=\s*rhi::ResourceState::General\s*,",
                )

        for label in ("SceneAsset material textures", "glTF material textures"):
            with self.subTest(sampled_only=label):
                upload = material_uploads[label]
                self.assertIn("TextureUsageFlags::sampled", upload)
                self.assertNotIn("TextureUsageFlags::storage", upload)
                self.assertIn("TextureRuntimeKind::materialSampled", upload)
                self.assertRegex(upload, r"\bupdateBindlessTexture\s*\(")

        material_writers = (
            braced_source(
                render_device,
                r"\bvoid\s+RenderDevice::updateGraphicsArgumentTables\s*\(",
            ),
            braced_source(
                render_device,
                r"\bvoid\s+RenderDevice::syncMaterialArgumentTable\s*\(",
            ),
        )
        for writer in material_writers:
            self.assertIn("ArgumentType::combinedImageSampler", writer)
            self.assertNotIn("ArgumentAccessIntent::readWrite", writer)

        self.assertRegex(
            argument_table,
            r"\bArgumentAccessIntent\s+accessIntent\s*"
            r"\{\s*ArgumentAccessIntent::sampledRead\s*\}",
        )
        combined_start = vulkan_device.rindex(
            "case ArgumentType::combinedImageSampler:"
        )
        combined_descriptor = vulkan_device[
            combined_start : vulkan_device.index("break;", combined_start)
        ]
        self.assertRegex(
            combined_descriptor,
            r"(?s)w\.accessIntent\s*==\s*ArgumentAccessIntent::readWrite"
            r".*?\?\s*VK_IMAGE_LAYOUT_GENERAL"
            r"\s*:\s*VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL",
        )

    def test_material_bindless_metadata_reaches_vulkan_layout_flags(self) -> None:
        render_device = read("render/RenderDevice.cpp")
        vulkan_device = read("rhi/vulkan/VulkanDevice.cpp")

        helper_declaration = re.search(
            r"\brhi::ArgumentBinding\s+makeArgumentBinding\s*\("
            r"\s*uint32_t\s+logicalIndex\s*,"
            r"\s*rhi::BindlessResourceType\s+resourceType\s*,"
            r"\s*uint32_t\s+descriptorCount\s*,"
            r"\s*rhi::ResourceVisibility\s+visibility\s*,"
            r"\s*bool\s+bindless\s*=\s*false\s*\)\s*;",
            render_device,
            re.DOTALL,
        )
        self.assertIsNotNone(helper_declaration)

        helper_definition = braced_source(
            render_device,
            r"\brhi::ArgumentBinding\s+makeArgumentBinding\s*\("
            r"(?=[^;{]*\bbool\s+bindless\s*\)\s*\{)",
        )
        self.assertRegex(helper_definition, r"\.bindless\s*=\s*bindless\s*,")
        self.assertNotRegex(helper_definition, r"\.bindless\s*=\s*false\s*,")

        material_functions = (
            braced_source(
                render_device,
                r"\bvoid\s+RenderDevice::createMaterialArgumentTable\s*\(",
            ),
            braced_source(
                render_device,
                r"\bvoid\s+RenderDevice::createGraphicsArgumentTables\s*\(",
            ),
        )
        material_bindless_call = (
            r"\bmakeArgumentBinding\s*\("
            r"\s*kMaterialBindlessTexturesIndex\s*,"
            r"\s*rhi::BindlessResourceType::sampledTexture\s*,"
            r"\s*m_materials\.maxTextures\s*,"
            r"\s*rhi::ResourceVisibility::allGraphics\s*,"
            r"\s*true\s*\)"
        )
        for material_function in material_functions:
            self.assertEqual(
                len(re.findall(material_bindless_call, material_function, re.DOTALL)),
                1,
            )
        self.assertEqual(
            len(re.findall(material_bindless_call, render_device, re.DOTALL)),
            2,
        )
        explicit_bindless_calls = re.findall(
            r"\bmakeArgumentBinding\s*\([^()]*,\s*true\s*\)",
            render_device,
            re.DOTALL,
        )
        self.assertEqual(len(explicit_bindless_calls), 2)
        for bindless_call in explicit_bindless_calls:
            self.assertRegex(bindless_call, material_bindless_call)

        graphics_tables = material_functions[1]
        self.assertRegex(
            graphics_tables,
            r"\bmakeArgumentBinding\s*\("
            r"\s*kSceneBindlessInfoIndex\s*,"
            r"\s*rhi::BindlessResourceType::uniformBuffer\s*,"
            r"\s*1\s*,"
            r"\s*rhi::ResourceVisibility::allGraphics\s*\)",
        )

        create_layout = braced_source(
            vulkan_device,
            r"\bArgumentLayoutHandle\s+VulkanDevice::createArgumentLayout\s*\(",
        )
        bindless_block = braced_source(
            create_layout,
            r"\bif\s*\(\s*b\.bindless\s*\)",
        )
        self.assertIn(
            "m_features12.descriptorBindingPartiallyBound == VK_TRUE",
            bindless_block,
        )
        self.assertIn(
            "VK_DESCRIPTOR_BINDING_PARTIALLY_BOUND_BIT",
            bindless_block,
        )
        self.assertEqual(
            create_layout.count("VK_DESCRIPTOR_BINDING_PARTIALLY_BOUND_BIT"),
            1,
        )

    def test_csm_reuses_indirect_arguments_with_war_and_raw_barriers(self) -> None:
        source = read("render/passes/GPUDrivenCSMShadowPass.cpp")
        execute = braced_source(
            source,
            r"\bvoid\s+GPUDrivenCSMShadowPass::execute\s*\(",
        )
        draw_loop_region = execute[
            execute.index("const CSMShadowResources::FrameData& csmFrameData") :
        ]
        cascade_loop = braced_source(
            draw_loop_region,
            r"\bfor\s*\(\s*uint32_t\s+cascadeIndex\s*=\s*0u?\s*;",
        )
        reused_arguments = braced_source(
            cascade_loop,
            r"\bif\s*\(\s*cascadeIndex\s*>\s*0u?\s*\)",
        )

        war_barrier = re.search(
            r"context\.commandBuffer->barrier\s*\(\s*(?P<source_stages>[^,]+?)\s*,"
            r"\s*rhi::StageFlags::compute\s*,"
            r"\s*rhi::HazardFlags::readBeforeWrite\s*\)",
            reused_arguments,
            re.DOTALL,
        )
        self.assertIsNotNone(war_barrier)
        self.assertRegex(
            war_barrier.group("source_stages"),
            r"\brhi::StageFlags::commandInput\b",
        )
        self.assertRegex(
            war_barrier.group("source_stages"),
            r"\brhi::StageFlags::compute\b",
        )

        raw_barrier = re.search(
            r"context\.commandBuffer->barrier\s*\("
            r"\s*rhi::StageFlags::compute\s*,"
            r"\s*rhi::StageFlags::commandInput\s*,"
            r"\s*rhi::HazardFlags::drawArguments\s*\)",
            cascade_loop,
            re.DOTALL,
        )
        self.assertIsNotNone(raw_barrier)
        self.assertLess(cascade_loop.index(reused_arguments), raw_barrier.start())

    def test_csm_shadow_resources_have_no_legacy_mapped_uniform_buffer(self) -> None:
        header = read("render/CSMShadowResources.h")
        source = read("render/CSMShadowResources.cpp")
        class_source = braced_source(header, r"\bclass\s+CSMShadowResources\b")
        resources_code = without_cpp_comments(class_source + "\n" + source)

        self.assertRegex(
            class_source,
            r"\bshaderio::ShadowUniforms\s+m_shadowUniformsData\s*\{\s*\}",
        )
        self.assertNotRegex(
            resources_code,
            r"\bm_shadowUniform(?:Buffer|Mapped)\b",
        )
        self.assertNotRegex(
            resources_code,
            r"\b(?:mapBuffer|unmapBuffer)\s*\(",
        )
        self.assertNotRegex(
            resources_code,
            r"\b(?:std::)?memcpy\s*\(",
        )

    def test_gpu_driven_per_frame_resources_use_authoritative_frame_count(self) -> None:
        source = read("render/GPUDrivenRenderer.cpp")
        header = read("render/GPUDrivenRenderer.h")
        frame_count_accessor = braced_source(
            header,
            r"\buint32_t\s+getFrameSlotCount\s*\(\s*\)\s*const",
        )

        self.assertRegex(
            frame_count_accessor,
            r"\breturn\s+m_renderer\.getFrameResourceCount\s*\(\s*\)\s*;",
        )
        self.assertNotRegex(
            frame_count_accessor,
            r"\bgetSwapchainImageCount\s*\(\s*\)",
        )

        per_frame_functions = {
            "renderer init": r"\bvoid\s+GPUDrivenRenderer::init\s*\(",
            "Flax DDGI init": (
                r"\bvoid\s+GPUDrivenRenderer::initFlaxDDGIResources\s*\("
            ),
            "DDGI probe init": (
                r"\bvoid\s+GPUDrivenRenderer::initDDGIProbeResources\s*\("
            ),
            "editable DDGI reinit": (
                r"\bvoid\s+GPUDrivenRenderer::setEditableDDGIConfig\s*\("
            ),
            "sorted bootstrap state": (
                r"\bvoid\s+GPUDrivenRenderer::recordSortedBootstrapState\s*\("
            ),
            "persistent draw state": (
                r"\bvoid\s+GPUDrivenRenderer::preparePersistentDrawData\s*\("
            ),
            "lighting init": (
                r"\bvoid\s+GPUDrivenRenderer::initLightingResources\s*\("
            ),
            "phase 7 init": (
                r"\bvoid\s+GPUDrivenRenderer::initPhase7Resources\s*\("
            ),
            "visibility sort init": (
                r"\bvoid\s+GPUDrivenRenderer::initVisibilitySortResources\s*\("
            ),
            "visibility patch init": (
                r"\bvoid\s+GPUDrivenRenderer::initTransparentVisibilityPatchResources\s*\("
            ),
        }
        authoritative_count = (
            r"\b(?:getFrameSlotCount|m_renderer\.getFrameResourceCount)\s*\(\s*\)"
        )
        for label, pattern in per_frame_functions.items():
            with self.subTest(function=label):
                function = braced_source(source, pattern)
                self.assertRegex(function, authoritative_count)
                self.assertNotRegex(
                    function,
                    r"\bgetSwapchainImageCount\s*\(\s*\)",
                )

        previous_frame = braced_source(
            source,
            r"\buint32_t\s+GPUDrivenRenderer::getPreviousFrameIndex\s*\(",
        )
        self.assertRegex(previous_frame, authoritative_count)
        self.assertRegex(
            previous_frame,
            r"\breturn\s*\(\s*frameIndex\s*\+\s*frameCount\s*-\s*1u?\s*\)"
            r"\s*%\s*frameCount\s*;",
        )
        self.assertNotRegex(
            previous_frame,
            r"\bgetSwapchainImageCount\s*\(\s*\)",
        )

    def test_taa_resolve_reprojects_the_unjittered_target_with_explicit_lod(self) -> None:
        shader = read("shaders/shader.light.slang")
        taa = function_source(shader, "float4 fragmentTAAResolveMain(VSOutput input)")

        self.assertRegex(
            taa,
            r"(?:const\s+)?float2\s+texel\s*=\s*postProcess\.params1\.zw\s*;",
        )
        self.assertRegex(
            taa,
            r"(?:const\s+)?float2\s+jitterUv\s*=\s*jitterPx\s*\*\s*texel\s*;",
        )
        self.assertRegex(
            taa,
            r"(?:const\s+)?float2\s+targetCurrentUv\s*=\s*useFilterInput\s*"
            r"\?\s*input\.uv\s*:\s*input\.uv\s*\+\s*jitterUv\s*;",
        )
        self.assertRegex(
            taa,
            r"inTexture\s*\[\s*kVelocityIndex\s*\]\s*\.SampleLevel\s*"
            r"\(\s*targetCurrentUv\s*-\s*jitterUv\s*,\s*0(?:\.0)?\s*\)\s*\.xy",
        )
        self.assertRegex(
            taa,
            r"(?:const\s+)?float2\s+historyUv\s*=\s*targetCurrentUv\s*-\s*velocity\s*;",
        )

    def test_taa_motion_gating_wraps_static_history_heuristics(self) -> None:
        shader = read("shaders/shader.light.slang")
        taa = function_source(shader, "float4 fragmentTAAResolveMain(VSOutput input)")

        self.assertRegex(
            taa,
            r"(?s)if\s*\(\s*!historyOutOfBounds\s*&&\s*motionPixels\s*<\s*0\.5"
            r"\s*\)\s*\{.*?const\s+float\s+kFireflyRatio",
        )
        self.assertRegex(
            taa,
            r"(?s)if\s*\(\s*useLottes\s*&&\s*!historyOutOfBounds\s*&&\s*"
            r"motionPixels\s*<\s*0\.5\s*\)\s*\{.*?"
            r"blendWeight\s*=\s*1\.0\s*-\s*\(\s*1\.0\s*-\s*blendWeight\s*\)",
        )

    def test_taa_affine_flow_reprojection_accounts_for_jitter(self) -> None:
        def add(left: tuple[float, float], right: tuple[float, float]) -> tuple[float, float]:
            return left[0] + right[0], left[1] + right[1]

        def subtract(left: tuple[float, float], right: tuple[float, float]) -> tuple[float, float]:
            return left[0] - right[0], left[1] - right[1]

        def transform(
            matrix: tuple[tuple[float, float], tuple[float, float]],
            vector: tuple[float, float],
        ) -> tuple[float, float]:
            return (
                matrix[0][0] * vector[0] + matrix[0][1] * vector[1],
                matrix[1][0] * vector[0] + matrix[1][1] * vector[1],
            )

        def assert_vector_almost_equal(
            actual: tuple[float, float],
            expected: tuple[float, float],
        ) -> None:
            self.assertAlmostEqual(actual[0], expected[0])
            self.assertAlmostEqual(actual[1], expected[1])

        pixel_uv = (0.61, 0.43)
        jitter_uv = (0.003, -0.002)
        flow_matrix = ((0.20, -0.10), (0.05, 0.30))
        flow_offset = (0.012, -0.018)

        def velocity(uv: tuple[float, float]) -> tuple[float, float]:
            return add(transform(flow_matrix, uv), flow_offset)

        target_current_uv = pixel_uv
        velocity_sample_uv = subtract(target_current_uv, jitter_uv)
        expected_history_uv = subtract(
            target_current_uv,
            add(transform(flow_matrix, velocity_sample_uv), flow_offset),
        )
        old_history_uv = subtract(target_current_uv, velocity(pixel_uv))
        old_residual = subtract(old_history_uv, expected_history_uv)
        expected_old_residual = transform(flow_matrix, (-jitter_uv[0], -jitter_uv[1]))
        corrected_history_uv = subtract(target_current_uv, velocity(velocity_sample_uv))

        assert_vector_almost_equal(old_residual, expected_old_residual)
        assert_vector_almost_equal(corrected_history_uv, expected_history_uv)

        static_velocity_sample_uv = subtract(target_current_uv, jitter_uv)
        static_history_uv = subtract(target_current_uv, (0.0, 0.0))
        assert_vector_almost_equal(static_velocity_sample_uv, subtract(pixel_uv, jitter_uv))
        assert_vector_almost_equal(static_history_uv, target_current_uv)

        no_filter_target_uv = add(pixel_uv, jitter_uv)
        no_filter_velocity_sample_uv = subtract(no_filter_target_uv, jitter_uv)
        no_filter_history_uv = subtract(no_filter_target_uv, velocity(no_filter_velocity_sample_uv))
        assert_vector_almost_equal(no_filter_velocity_sample_uv, pixel_uv)
        assert_vector_almost_equal(
            no_filter_history_uv,
            subtract(add(pixel_uv, jitter_uv), velocity(pixel_uv)),
        )

    def test_flax_lighting_descriptors_are_in_range_and_ready(self) -> None:
        renderer = read("render/GPUDrivenRenderer.cpp")
        pass_header = read("render/passes/FlaxDDGIPass.h")

        self.assertIn("kGPUDrivenLightPassTextureCount = kPackedGBufferTargetCount + 23u", renderer)
        self.assertIn("kGPUDrivenLightPassFlaxProbesDataIndex", renderer)
        self.assertIn("kGPUDrivenLightPassFlaxProbesDistanceIndex", renderer)
        self.assertIn("kGPUDrivenLightPassFlaxProbesIrradianceIndex", renderer)
        self.assertIn("m_flaxDDGIPass->isReady()", renderer)
        self.assertIn("getProbesDataView", pass_header)
        self.assertIn("getProbesDistanceOutputView", pass_header)
        self.assertIn("getProbesIrradianceOutputView", pass_header)

    def test_flax_texture_views_never_use_undefined_format(self) -> None:
        source = read("render/passes/FlaxDDGIPass.cpp")
        init = source[source.index("void FlaxDDGIPass::initResources") :]
        init = init[: init.index("void FlaxDDGIPass::shutdownResources")]

        self.assertNotIn("TextureFormat::undefined", init)
        self.assertIn("TextureFormat::rgba8Snorm", init)
        self.assertIn("TextureFormat::rgba16Sfloat", init)
        self.assertIn("TextureFormat::rg16Sfloat", init)
        self.assertIn("TextureFormat::r16Sfloat", init)
        self.assertIn("TextureFormat::d16Unorm", init)

    def test_flax_clearable_textures_have_transfer_dst_usage(self) -> None:
        source = read("render/FlaxDDGIResources.cpp")

        for debug_name in (
            '"FlaxDDGI.ProbesData"',
            '"FlaxDDGI.ProbesIrradiance"',
            '"FlaxDDGI.ProbesDistance"',
        ):
            name_index = source.index(debug_name)
            resource_desc = source[max(0, name_index - 700) : name_index]
            self.assertIn("TextureUsageFlags::transferDst", resource_desc, debug_name)

    def test_flax_clearable_indirect_args_have_transfer_dst_usage(self) -> None:
        source = read("render/FlaxDDGIResources.cpp")
        debug_name = '"FlaxDDGI.UpdateProbesInitArgs"'
        name_index = source.index(debug_name)
        resource_desc = source[max(0, name_index - 700) : name_index]

        self.assertIn("BufferUsageFlags::transferDst", resource_desc, debug_name)

    def test_flax_textures_transition_and_clear_before_first_compute(self) -> None:
        source = read("render/passes/FlaxDDGIPass.cpp")
        header = read("render/passes/FlaxDDGIPass.h")
        execute = source[source.index("void FlaxDDGIPass::execute") :]
        classify = execute.index('cmd.beginEvent("DDGI.Classify")')
        initialization = execute[:classify]

        self.assertIn("m_textureLayoutsInitialized", header)
        self.assertIn("if (!m_textureLayoutsInitialized)", initialization)
        self.assertIn("ResourceState::Undefined", initialization)
        self.assertIn("ResourceState::General", initialization)
        self.assertIn("getProbesTrace()", initialization)
        self.assertIn("getProbesData()", initialization)
        self.assertIn("getProbesIrradianceWrite(0)", initialization)
        self.assertIn("getProbesIrradianceRead(0)", initialization)
        self.assertIn("getProbesDistanceWrite(0)", initialization)
        self.assertIn("getProbesDistanceRead(0)", initialization)
        self.assertIn("clearColorTexture", initialization)
        self.assertIn("HazardFlags::textureWrites", initialization)

    def test_flax_auxiliary_texture_views_are_owned_and_destroyed(self) -> None:
        source = read("render/passes/FlaxDDGIPass.cpp")
        header = read("render/passes/FlaxDDGIPass.h")
        shutdown = source[source.index("void FlaxDDGIPass::shutdownResources") :]

        for view in (
            "m_classifySDFView",
            "m_traceSDFView",
            "m_atlasDepthView",
            "m_atlasLightingView",
        ):
            self.assertIn(view, header)
            self.assertIn(f"destroyView({view})", shutdown)

    def test_flax_reset_clears_are_visible_to_compute(self) -> None:
        source = read("render/passes/FlaxDDGIPass.cpp")
        reset = source[source.index("if (resetRequested)") :]
        reset = reset[: reset.index("if(!updateRequested)")]

        self.assertIn("clearColorTexture", reset)
        self.assertIn("m_outputState.invalidate()", reset)
        self.assertIn("recordTextureWriteDependency", reset)
        self.assertIn("StageFlags::transfer", reset)
        self.assertIn("StageFlags::compute", reset)
        self.assertIn("HazardFlags::textureWrites", reset)

    def test_flax_sampled_descriptors_match_resource_layouts(self) -> None:
        source = read("render/passes/FlaxDDGIPass.cpp")
        writer = source[source.index("auto writeTextureRO") :]
        writer = writer[: writer.index("auto writeBufferRW")]

        self.assertIn("ArgumentAccessIntent::readWrite", writer)
        self.assertIn("auto writeTextureSampled", writer)
        self.assertIn("ArgumentAccessIntent::sampledRead", writer)
        self.assertIn("writeTextureSampled(", source)
        self.assertIn("getIBLEnvironmentView()", source)

    def test_empty_surface_atlas_does_not_read_uninitialized_buffers(self) -> None:
        shader = read("shaders/surface_atlas_common.slang")
        sample = shader[shader.index("float4 sampleGlobalSurfaceAtlas") :]
        first_chunk_read = sample.index("chunks[chunkAddress]")

        self.assertIn("data.objectsCount == 0", sample[:first_chunk_read])

    def test_flax_trace_sdf_binding_matches_combined_sampler_shader(self) -> None:
        source = read("render/passes/FlaxDDGIPass.cpp")
        shader = read("shaders/ddgi_flax_trace_rays.slang")
        layout = source[source.index("// traceRays:") :]
        layout = layout[: layout.index("// distance:")]
        table = source[source.index("rhi::ArgumentTableHandle& traceRaysTable") :]
        table = table[: table.index("for(uint32_t parity")]

        self.assertIn("Sampler3D<float> globalSDFTex", shader)
        self.assertIn("ArgumentType::combinedImageSampler", layout)
        self.assertIn("writeCombinedTextureRO", table)

    def test_flax_small_scene_coverage_focuses_on_scene_bounds(self) -> None:
        renderer = read("render/GPUDrivenRenderer.cpp")
        resources = read("render/FlaxDDGIResources.cpp")

        self.assertIn("computeFlaxCoverageCenter", renderer)
        self.assertIn("m_sceneView.sceneBoundsValid", renderer)
        self.assertIn("sceneBoundsCenter", renderer)
        self.assertIn("coverageCenter", resources)
        self.assertNotIn("floor(cameraPosition / cascade.probeSpacing)", resources)

    def test_flax_coverage_prefers_transformed_scene_bounds_over_baked_sdf(self) -> None:
        renderer = read("render/GPUDrivenRenderer.cpp")
        coverage = renderer[renderer.index("GPUDrivenRenderer::computeFlaxCoverageCenter") :]
        coverage = coverage[: coverage.index("GPUDrivenRenderer::updateFlaxCascadeScheduling")]

        scene_bounds = coverage.index("if (m_sceneView.sceneBoundsValid)")
        global_sdf_bounds = coverage.index("m_globalSDFPass->getVolume().worldBoundsMin")

        self.assertLess(scene_bounds, global_sdf_bounds)

    def test_flax_compact_scene_center_contains_scene_and_stays_near_camera(self) -> None:
        renderer = read("render/GPUDrivenRenderer.cpp")
        coverage = renderer[renderer.index("GPUDrivenRenderer::computeFlaxCoverageCenter") :]
        coverage = coverage[: coverage.index("GPUDrivenRenderer::updateFlaxCascadeScheduling")]

        compact_scene = coverage[coverage.index("if (compactSceneFits)") :]
        compact_scene = compact_scene[: compact_scene.index("const bool cameraInsideCoverageBounds")]

        self.assertIn("minimumCenter = coverageBoundsMax - coverageHalfExtent", compact_scene)
        self.assertIn("maximumCenter = coverageBoundsMin + coverageHalfExtent", compact_scene)
        self.assertIn("glm::clamp(cameraPosition, minimumCenter, maximumCenter)", compact_scene)
        self.assertNotIn("keepInnerCascadeNearCamera(sceneBoundsCenter)", compact_scene)

    def test_flax_cascade_density_stays_config_driven_and_camera_local(self) -> None:
        resources = read("render/FlaxDDGIResources.cpp")
        renderer = read("render/GPUDrivenRenderer.cpp")
        coverage = renderer[renderer.index("GPUDrivenRenderer::computeFlaxCoverageCenter") :]
        coverage = coverage[: coverage.index("GPUDrivenRenderer::updateFlaxCascadeScheduling")]

        self.assertIn("cascade.probeSpacing = config.probeSpacing * spacingMultiplier", resources)
        self.assertNotIn("kMinProbeSpacingVoxelScale", resources)
        self.assertNotIn("computeRequiredCoverageSpacing", resources)
        self.assertIn("cameraInsideCoverageBounds", coverage)
        self.assertIn("return cameraPosition", coverage)

    def test_flax_coarse_sdf_relocation_does_not_change_probe_density(self) -> None:
        classify = read("shaders/ddgi_flax_classify.slang")

        self.assertIn("relocationClearance = min(voxelLimit, probesSpacing * 0.35f)", classify)
        self.assertIn("relocateLimit = probesSpacing * 0.45f", classify)
        self.assertIn("cappedRelocationDistance", classify)
        self.assertIn("deepInsideTolerance", classify)

    def test_flax_probe_debug_defaults_to_local_active_cascade(self) -> None:
        render_types = read("render/RenderTypes.h")

        self.assertIn("int ddgiDebugCascadeIndex{0}", render_types)
        self.assertIn("bool ddgiDebugActiveProbes{true}", render_types)

    def test_flax_spacing_changes_request_history_reset(self) -> None:
        resources = read("render/FlaxDDGIResources.cpp")
        renderer = read("render/GPUDrivenRenderer.cpp")
        pass_header = read("render/passes/FlaxDDGIPass.h")
        pass_source = read("render/passes/FlaxDDGIPass.cpp")

        self.assertIn("spacingChanged", resources)
        self.assertIn("requestRuntimeReset", renderer)
        self.assertIn("requestRuntimeReset", pass_header)
        self.assertIn("m_runtimeResetRequested", pass_source)
        self.assertIn("m_runtimeResetRequested = false", pass_source)

    def test_flax_ibl_exclusion_reaches_probe_trace_and_resets_history(self) -> None:
        renderer = read("render/GPUDrivenRenderer.cpp")
        update = renderer[renderer.index("const bool disableIBLForFlax") :]
        update = update[: update.index("lightingUniforms.light.iblParams")]

        self.assertIn(
            "params.debugOptions.enableIBL && !disableIBLForFlax "
            "&& getIBLEnvironmentLoaded()",
            update,
        )
        self.assertIn("flaxIBLEnvironmentChanged", update)
        self.assertIn("m_flaxDDGIPass->requestRuntimeReset()", update)
        self.assertIn(
            "m_cachedIBLEnvironmentIntensity =\n\t\t\t"
            "flaxIBLEnvironmentEnabled ? requestedIBLEnvironmentIntensity : 0.0f",
            update,
        )

    def test_final_color_uses_blender_compatible_agx_display_transform(self) -> None:
        shader = read("shaders/shader.light.slang")
        final_color = shader[shader.index("float4 fragmentFinalColorMain") :]
        final_color = final_color[: final_color.index("float4 fragmentVelocityMain")]

        self.assertIn("vec3 toneMapAgX(vec3 color)", shader)
        self.assertIn(
            "vec3(0.842479062253094, 0.0784335999999992, 0.0792237451477643)",
            shader,
        )
        self.assertIn(
            "vec3(1.19687900512017, -0.0980208811401368, -0.0990297440797205)",
            shader,
        )
        self.assertIn("color = toneMapAgX(max(color, 0.0));", final_color)
        self.assertNotIn("color = toneMapACES(max(color, 0.0));", final_color)

    def test_flax_global_sdf_preserves_sponza_curtain_resolution(self) -> None:
        global_sdf = read("render/passes/GlobalSDFPass.h")

        self.assertIn("static constexpr uint32_t kResolution = 256u;", global_sdf)
        self.assertNotIn("static constexpr uint32_t kResolution = 128u;", global_sdf)

    def test_scene_bounds_refresh_tracks_runtime_instance_transforms(self) -> None:
        renderer = read("render/GPUDrivenRenderer.cpp")
        refresh = renderer[renderer.index("GPUDrivenRenderer::refreshSceneView") :]
        refresh = refresh[: refresh.index("GPUDrivenRenderer::recordDepthPrepassVisibilitySource")]
        update = renderer[renderer.index("GPUDrivenRenderer::updateSceneInstanceTransform") :]
        update = update[: update.index("GPUDrivenRenderer::tryGetMeshDrawIndex")]

        bounds = refresh[refresh.index("glm::vec3 boundsMin(std::numeric_limits<float>::max())") :]
        runtime_draw_bounds = bounds.index("m_sceneDrawRecords.empty()")
        static_shadow_bounds = bounds.index("m_activeUploadResult->shadowPackedMeshes.empty()")

        self.assertLess(runtime_draw_bounds, static_shadow_bounds)
        self.assertIn("drawRecord.boundsSphere = boundsSphere;", update)

    def test_shadow_upload_records_through_public_buffer_handle(self) -> None:
        render_device = read("render/RenderDevice.cpp")
        upload = render_device[render_device.index("RenderDevice::createShadowPackedUploadBuffer") :]
        upload = upload[: upload.index("RenderDevice::rebuildShadowPackedBuffers")]

        buffer_create = upload.index("m_device.device->createBuffer")
        staging_allocation = upload.index("upload.init(")
        handle_upload = upload.index("upload.recordBufferUpload(slice, buffer")

        self.assertLess(buffer_create, staging_allocation)
        self.assertLess(staging_allocation, handle_upload)
        self.assertNotIn("tryGetBuffer", upload)
    def test_flax_partial_updates_mirror_both_history_atlases(self) -> None:
        pass_source = read("render/passes/FlaxDDGIPass.cpp")
        distance = read("shaders/ddgi_flax_update_distance.slang")
        irradiance = read("shaders/ddgi_flax_update_irradiance.slang")

        self.assertIn("writeTextureRW(distanceTable, 5, distanceRead)", pass_source)
        self.assertIn("writeTextureRW(irradianceTable, 3, irradianceRead)", pass_source)
        self.assertIn("RWTexture2D<float2> probesDistanceHist", distance)
        self.assertIn("probesDistanceHist[texel] = encodedMoments", distance)
        self.assertIn("RWTexture2D<float4> probesIrradianceHist", irradiance)
        self.assertIn("probesIrradianceHist[texel] = encodedIrradiance", irradiance)

    def test_flax_gi_coverage_and_trace_distance_are_distinct_controls(self) -> None:
        ui = read("app/MinimalLatestApp.h")
        pass_source = read("render/passes/FlaxDDGIPass.cpp")

        self.assertIn('"Coverage Radius"', ui)
        self.assertIn('"Ray Max Distance"', ui)
        self.assertIn("ddgi.rayMaxDistance = config.maxDistance", pass_source)

    def test_flax_classifier_rejects_probes_outside_global_sdf(self) -> None:
        shader = read("shaders/ddgi_flax_classify.slang")
        bounds = shader.index("bool outsideSDFBounds")
        sample = shader.index("globalSDFTex.SampleLevel")

        self.assertLess(bounds, sample)
        self.assertIn("kDDGIProbeStateInactive", shader[bounds:sample])
        self.assertIn("return", shader[bounds:sample])
        self.assertNotIn("clamp((probePosition - boundsMin)", shader)

    def test_flax_newly_valid_probes_ignore_empty_history(self) -> None:
        classify = read("shaders/ddgi_flax_classify.slang")

        self.assertIn("newlyActivated", classify)
        self.assertIn("kDDGIProbeStateActivated", classify)

    def test_flax_history_blends_in_linear_units(self) -> None:
        irradiance = read("shaders/ddgi_flax_update_irradiance.slang")
        distance = read("shaders/ddgi_flax_update_distance.slang")

        self.assertIn("historyLinear", irradiance)
        self.assertIn("ddgi.irradianceGamma", irradiance[irradiance.index("historyLinear") :])
        self.assertNotIn(
            "lerp(irradiance, historyIrradiance.rgb, historyWeight)", irradiance
        )
        self.assertIn("historyData.x * distanceLimit", distance)
        self.assertIn("historySecondMoment", distance)
        self.assertNotIn("lerp(varDist", distance)

    def test_flax_distance_moments_include_misses_and_store_raw_moments(self) -> None:
        distance = read("shaders/ddgi_flax_update_distance.slang")
        trace = read("shaders/ddgi_flax_trace_rays.slang")
        common = read("shaders/flax_ddgi_common.slang")

        self.assertIn("traceData.w < 0.0f", distance)
        self.assertIn("? distanceLimit", distance)
        self.assertIn("secondMoment / (distanceLimit * distanceLimit)", distance)
        self.assertIn("secondMoment - meanDistance * meanDistance", common)
        self.assertIn("float storedDistance = distanceLimit", trace)

    def test_flax_visibility_never_hard_rejects_an_entire_probe_cage(self) -> None:
        common = read("shaders/flax_ddgi_common.slang")

        self.assertIn("wrapShading * wrapShading + 0.2f", common)
        self.assertIn("kFlaxDDGIMinVisibility", common)
        self.assertIn("rawChebyshevWeight", common)
        self.assertIn("conservativeVisibility, rawChebyshevWeight, frontFacing", common)
        self.assertIn("kFlaxDDGIFrontFacingMinVisibility", common)
        self.assertNotIn("surfaceSideWeight", common)

    def test_flax_probe_weights_remain_continuous_across_cells(self) -> None:
        common = read("shaders/flax_ddgi_common.slang")

        self.assertIn(
            "float contribWeight = weight * backFaceWeight * distVis", common
        )
        self.assertNotIn("lowWeightThreshold", common)
        self.assertNotIn("contribWeight *= (contribWeight * contribWeight)", common)

    def test_flax_distance_history_rejects_large_moment_disagreement(self) -> None:
        common = read("shaders/flax_ddgi_common.slang")
        distance = read("shaders/ddgi_flax_update_distance.slang")

        self.assertIn("flaxDistanceHistoryWeight", common)
        self.assertIn("historyRejectionThreshold", common)
        self.assertIn("flaxDistanceHistoryWeight(", distance)
        self.assertNotIn(
            "? 0.0f : ddgi.probeHistoryWeight", distance
        )

    def test_flax_probe_updates_write_interior_and_wrapped_border_texels(self) -> None:
        common = read("shaders/flax_ddgi_common.slang")
        irradiance = read("shaders/ddgi_flax_update_irradiance.slang")
        distance = read("shaders/ddgi_flax_update_distance.slang")

        self.assertIn("flaxProbeTileInteriorTexel", common)
        self.assertIn("kDDGIProbeIrradianceResolution", irradiance)
        self.assertIn("flaxProbeTileInteriorTexel", irradiance)
        self.assertIn("kDDGIProbeDistanceResolution", distance)
        self.assertIn("flaxProbeTileInteriorTexel", distance)
        self.assertNotIn("/ float(kTileSize)) * 2.0f", irradiance)
        self.assertNotIn("/ float(kTileSize)) * 2.0f", distance)

    def test_flax_valid_probes_renormalize_before_fallback(self) -> None:
        common = read("shaders/flax_ddgi_common.slang")
        inactive = read("shaders/ddgi_flax_update_inactive.slang")

        self.assertIn("validTrilinearWeight += weight", common)
        self.assertIn("inactiveTrilinearWeight += weight", common)
        self.assertIn("float averageContributionScale", common)
        self.assertIn("ddgi.fallbackIrradiance.rgb * fallbackWeight", common)
        self.assertNotIn("ddgi.fallbackIrradiance.rgb * weight", common)
        self.assertNotIn("totalWeight += weight", common)
        self.assertNotIn("fallbackCoordsValid(ddgi, probeData.xyz)", common)
        self.assertNotIn("int step = 1", inactive)
        self.assertNotIn("neighborData", inactive)

    def test_flax_trace_bias_is_scale_aware_and_preserves_probe_distance(self) -> None:
        trace = read("shaders/ddgi_flax_trace_rays.slang")

        self.assertIn("flaxProbeRayStartOffset", trace)
        self.assertIn("rayStartOffset + hit.hitTime", trace)
        self.assertIn("frontFaceHit", trace)
        self.assertNotIn("rayDir * 0.1f", trace)

    def test_flax_probe_tiles_preserve_directional_radiance(self) -> None:
        common = read("shaders/flax_ddgi_common.slang")
        irradiance = read("shaders/ddgi_flax_update_irradiance.slang")
        distance = read("shaders/ddgi_flax_update_distance.slang")

        self.assertIn("flaxSphericalFibonacci", common)
        self.assertIn("flaxOctahedralDecode", common)
        self.assertIn("dot(texelDirection, rayDirection)", irradiance)
        self.assertIn("dot(texelDirection, rayDirection)", distance)

    def test_flax_trace_receives_local_and_emissive_radiance_sources(self) -> None:
        shader_io = read("shaders/shader_io.h")
        renderer = read("render/GPUDrivenRenderer.cpp")
        pass_source = read("render/passes/FlaxDDGIPass.cpp")
        trace = read("shaders/ddgi_flax_trace_rays.slang")

        self.assertIn("FlaxDDGIRadianceSource", shader_io)
        self.assertIn("radianceSources", shader_io)
        self.assertIn("rebuildFlaxRadianceSources", renderer)
        self.assertIn("emissiveFactor", renderer)
        self.assertIn("getFlaxRadianceSources", pass_source)
        self.assertIn("evaluateFlaxRadianceSources", trace)


    def test_flax_scrolling_uses_cumulative_ring_offset_and_frame_delta(self) -> None:
        resources = read("render/FlaxDDGIResources.cpp")
        shader_io = read("shaders/shader_io.h")
        common = read("shaders/flax_ddgi_common.slang")
        classify = read("shaders/ddgi_flax_classify.slang")
        pass_source = read("render/passes/FlaxDDGIPass.cpp")

        self.assertIn("previous[c].scrollOffset + cascade.scrollDelta", resources)
        self.assertIn("cascade.scrollDelta = glm::ivec3(0, 0, 0)", resources)
        self.assertIn("probesScrollDeltas", shader_io)
        self.assertIn("ddgi.probesScrollDeltas[cascadeIndex]", classify)
        self.assertIn("ddgi.probesScrollOffsets[cascadeIndex]", common)
        world_position = common[common.index("float3 getProbeWorldPosition") :]
        world_position = world_position[: world_position.index("// --- Probe state")]
        self.assertNotIn("probesScrollOffsets", world_position)
        self.assertIn("ddgi.probesScrollDeltas[c]", pass_source)

    def test_flax_cascade_sampling_checks_bounds_blends_edges_and_decodes_energy(self) -> None:
        common = read("shaders/flax_ddgi_common.slang")

        self.assertIn("if (any(edgeDistances < float3(0,0,0)))", common)
        self.assertIn("saturate(edgeDistance / blendWidth)", common)
        self.assertIn("ddgi.fallbackIrradiance.rgb * remainingWeight", common)
        self.assertIn("* kFlaxDDGIPi", common)
        self.assertNotIn("* (2.0f * kFlaxDDGIPi)", common)

    def test_flax_trace_uses_environment_and_sdf_shadow_visibility(self) -> None:
        trace = read("shaders/ddgi_flax_trace_rays.slang")
        pass_source = read("render/passes/FlaxDDGIPass.cpp")

        self.assertIn("Texture2D<float4> environmentTex", trace)
        self.assertIn("ddgi.environmentParams.x > 0.5f", trace)
        self.assertIn("flaxSDFVisible", trace)
        self.assertIn("evaluateFlaxRadianceSources", trace)
        self.assertIn("bindings.push_back({12", pass_source)
        self.assertIn("getIBLEnvironmentView()", pass_source)
        self.assertIn("kFlaxDDGIMultiBounceFeedback = 0.5f", trace)
        self.assertIn("sampleFlaxDDGIIrradiance(", trace)
        self.assertIn("bouncedIrradiance * kFlaxDDGIMultiBounceFeedback", trace)
        self.assertIn("bindings.push_back({15", pass_source)
        self.assertIn(
            "recordTextureWriteDependency(cmd, traceWrite, flaxTextureRange)",
            pass_source,
        )
        self.assertIn("rhi::HazardFlags::readBeforeWrite", pass_source)
        self.assertNotIn(
            "HazardFlags::textureWrites | rhi::HazardFlags::readBeforeWrite",
            pass_source,
        )

    def test_flax_probe_becomes_active_only_after_irradiance_is_written(self) -> None:
        irradiance = read("shaders/ddgi_flax_update_irradiance.slang")
        common = read("shaders/flax_ddgi_common.slang")

        store = irradiance.index("probesIrradianceOut[texel]")
        activate = irradiance.index("kDDGIProbeStateActive", store)
        self.assertLess(store, activate)
        self.assertIn("state != kDDGIProbeStateActive", common)
        self.assertIn("encodedIrradiance.a <= 0.0f", common)

    def test_flax_cross_frame_fragment_read_before_compute_write_is_barriered(self) -> None:
        stage_barrier = read("rhi/RHIStageBarrier.h")
        vulkan = read("rhi/vulkan/VulkanBarrierConversions.h")
        pass_source = read("render/passes/FlaxDDGIPass.cpp")

        self.assertIn("readBeforeWrite", stage_barrier)
        self.assertIn("VK_ACCESS_2_MEMORY_READ_BIT", vulkan)
        self.assertIn("VK_ACCESS_2_SHADER_SAMPLED_READ_BIT", vulkan)
        self.assertIn("VK_ACCESS_2_MEMORY_WRITE_BIT", vulkan)
        self.assertIn("VK_ACCESS_2_SHADER_STORAGE_WRITE_BIT", vulkan)
        self.assertIn("StageFlags::fragmentShader", pass_source)
        self.assertIn("HazardFlags::readBeforeWrite", pass_source)

    def test_flax_lighting_uses_ddgi_weight_and_applies_intensity_once(self) -> None:
        light = read("shaders/shader.light.slang")
        common = read("shaders/flax_ddgi_common.slang")

        self.assertIn(
            "lerp(ambientTerm, ddgiDiffuse, saturate(lighting.light.ddgiParams0.x))", light
        )
        self.assertIn(
            "lerp(diffuse, ddgiDiffuse, saturate(lighting.light.ddgiParams0.x))", light
        )
        self.assertNotIn(
            "saturate(lighting.light.ddgiFlaxGammaWeightMaxDist.w)", light
        )
        self.assertEqual(
            common.count("return result * ddgi.indirectLightingIntensity;"), 1
        )

    def test_flax_coverage_center_preserves_camera_local_cascades(self) -> None:
        renderer = read("render/GPUDrivenRenderer.cpp")
        coverage = renderer[renderer.index("GPUDrivenRenderer::computeFlaxCoverageCenter") :]
        coverage = coverage[: coverage.index("GPUDrivenRenderer::updateFlaxCascadeScheduling")]

        self.assertIn("cameraInsideCoverageBounds", coverage)
        self.assertIn("return cameraPosition", coverage)
        self.assertIn("cameraContainmentHalfExtent", coverage)
        self.assertIn("minimumCenter = coverageBoundsMax - coverageHalfExtent", coverage)
        self.assertIn("maximumCenter = coverageBoundsMin + coverageHalfExtent", coverage)
        compact_scene = coverage[
            coverage.index("if (compactSceneFits)") :
            coverage.index("const bool cameraInsideCoverageBounds")
        ]
        self.assertIn("glm::ceil(minimumCenter / config.probeSpacing)", compact_scene)
        self.assertIn("glm::floor(maximumCenter / config.probeSpacing)", compact_scene)
        self.assertIn("glm::round(cameraPosition / config.probeSpacing)", compact_scene)
        self.assertIn("preferredGridCenter, minimumGridCenter", compact_scene)
        self.assertNotIn("keepInnerCascadeNearCamera", coverage)

    def test_flax_debug_snapshot_reports_all_runtime_cascades(self) -> None:
        renderer = read("render/GPUDrivenRenderer.cpp")

        self.assertIn(
            "snapshot.implementedCascadeCount = readiness.flaxResourcesReady\n"
            "\t\t\t? readiness.flaxCascadeCount : 0u;",
            renderer,
        )


if __name__ == "__main__":
    unittest.main()
