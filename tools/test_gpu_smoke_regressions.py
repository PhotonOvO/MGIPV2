from pathlib import Path
import unittest


ROOT = Path(__file__).resolve().parents[1]


def read(relative_path: str) -> str:
    return (ROOT / relative_path).read_text(encoding="utf-8")


class GpuSmokeRegressionTests(unittest.TestCase):
    def test_gpu_smoke_defaults_are_fixed_for_reference_comparison(self) -> None:
        app = read("app/MinimalLatestApp.cpp")
        app_header = read("app/MinimalLatestApp.h")
        render_types = read("render/RenderTypes.h")
        ddgi_config = read("render/DDGIConfig.h")

        self.assertIn("MinimalLatestApp app({1920, 1080})", app)
        self.assertIn("size = {1920, 1080}", app_header)
        self.assertIn("bool enablePostProcessing{false}", render_types)
        self.assertIn("bool enableAO{false}", render_types)
        self.assertIn("bool enableSSR{false}", render_types)
        self.assertIn("maxUpdatedProbesPerFrame{2048u}", ddgi_config)
        self.assertIn("raysPerProbe{256u}", ddgi_config)
        self.assertIn("ddgiWeight{0.65f}", ddgi_config)

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
        first_table = init.index("table = device.createArgumentTable(m_initArgsLayout)")
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
        self.assertIn("writeFlaxDDGIDataToBuffer(frameIndex)", execute)
        self.assertNotIn("updateArgumentTable", execute)

    def test_flax_probe_budget_uses_deterministic_compaction_and_slack(self) -> None:
        pass_source = read("render/passes/FlaxDDGIPass.cpp")
        pass_header = read("render/passes/FlaxDDGIPass.h")
        classify = read("shaders/ddgi_flax_classify.slang")
        init_args = read("shaders/ddgi_flax_init_args.slang")
        common = read("shaders/flax_ddgi_common.slang")
        shader_io = read("shaders/shader_io.h")

        self.assertIn("m_probeUpdateOffsets", pass_header)
        self.assertIn("m_priorityProbeUpdateOffsets", pass_header)
        self.assertIn("highGeometryComplexity", classify)
        self.assertIn("activeProbes[priorityBase + probeIndex] = 1u", classify)
        self.assertNotIn("InterlockedAdd(activeProbes", classify)
        self.assertIn("for (uint probeIndex = 0u;", init_args)
        self.assertIn("activeProbes[priorityBase + priorityCount++] = probeIndex", init_args)
        self.assertIn("InterlockedAdd(updateProbesInitArgs[initArgs.scratchOffset]", init_args)
        self.assertIn("uint priorityBase = 4u", common)
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
        self.assertIn("m_outputState.publishPending()", pass_source)
        self.assertIn("getLightingOutputSelection", pass_header)
        self.assertIn("flaxOutput.parity", renderer)
        self.assertIn("getFlaxDDGIPublishedOutputSelection", debug_pass)
        self.assertNotIn("sourceFrame & 1u", pass_source)
        self.assertIn("probesDistanceHist", distance)
        self.assertNotIn("float2 historyData = probesDistance.Load", distance)

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

    def test_frame_timeline_uses_cpu_monotonic_signal_value(self) -> None:
        source = read("rhi/vulkan/VulkanFrameContext.cpp")
        submit = source[source.index("SubmissionReceipt VulkanFrameContext::submitCurrentFrame") :]

        self.assertIn("Timeline semaphore near overflow", submit)
        self.assertIn("m_timelineSemaphore->getCurrentValue()", submit)
        self.assertIn("m_frameCounter > currentTimelineValue", submit)
        self.assertIn("m_frameCounter = signalValue", submit)
        self.assertIn("m_timelineSemaphore->init(m_device, 0)", submit)

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
        reset = reset[: reset.index("const uint32_t cascadeCount")]

        self.assertIn("clearColorTexture", reset)
        self.assertIn("m_outputState.invalidate()", reset)
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

    def test_shadow_upload_reacquires_registry_record_after_staging_allocation(self) -> None:
        render_device = read("render/RenderDevice.cpp")
        upload = render_device[render_device.index("RenderDevice::createShadowPackedUploadBuffer") :]
        upload = upload[: upload.index("RenderDevice::rebuildShadowPackedBuffers")]

        staging_allocation = upload.index("upload.init(")
        record_lookup = upload.index("tryGetBuffer(buffer)")

        self.assertGreater(record_lookup, staging_allocation)

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
        self.assertIn("HazardFlags::textureWrites | rhi::HazardFlags::readBeforeWrite", pass_source)

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
        vulkan = read("rhi/vulkan/VulkanCommandBuffer.cpp")
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
