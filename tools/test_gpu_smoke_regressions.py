from pathlib import Path
import unittest


ROOT = Path(__file__).resolve().parents[1]


def read(relative_path: str) -> str:
    return (ROOT / relative_path).read_text(encoding="utf-8")


class GpuSmokeRegressionTests(unittest.TestCase):
    def test_flax_active_probe_counter_is_reset_before_classification(self) -> None:
        source = read("render/passes/FlaxDDGIPass.cpp")
        execute = source[source.index("void FlaxDDGIPass::execute") :]
        classify = execute.index('cmd.beginEvent("DDGI.Classify")')
        reset = execute.index("getActiveProbes(0)")

        self.assertLess(reset, classify)
        self.assertIn("fillBuffer", execute[reset:classify])
        self.assertIn("StageFlags::transfer", execute[reset:classify])

    def test_flax_indirect_dispatch_is_clamped_to_capacity_and_budget(self) -> None:
        shader = read("shaders/ddgi_flax_init_args.slang")
        pass_source = read("render/passes/FlaxDDGIPass.cpp")

        self.assertIn("maxActiveProbes", shader)
        self.assertIn("maxUpdatedProbesPerFrame", shader)
        self.assertIn("uint totalActiveCount = min(activeProbes[0]", shader)
        self.assertIn("uint updateCount = totalActiveCount", shader)
        self.assertNotIn("activeProbes[0] = updateCount", shader)
        self.assertIn("m_initArgsPipeline", pass_source)
        self.assertIn("setRootConstants", pass_source[pass_source.index('cmd.beginEvent("DDGI.InitArgs")') :])

    def test_flax_probe_updates_dispatch_only_active_probe_tiles(self) -> None:
        init_args = read("shaders/ddgi_flax_init_args.slang")
        distance = read("shaders/ddgi_flax_update_distance.slang")
        irradiance = read("shaders/ddgi_flax_update_irradiance.slang")
        pass_source = read("render/passes/FlaxDDGIPass.cpp")

        self.assertIn("updateProbesInitArgs[3]", init_args)
        self.assertIn("updateProbesInitArgs[6]", init_args)
        self.assertNotIn("findProbeBatchSlot", distance)
        self.assertNotIn("findProbeBatchSlotIrr", irradiance)
        self.assertIn("SV_GroupID", distance)
        self.assertIn("SV_GroupID", irradiance)
        self.assertEqual(pass_source.count("dispatchIndirect("), 3)
        self.assertIn("kTraceIndirectOffset", pass_source)
        self.assertIn("kDistanceIndirectOffset", pass_source)
        self.assertIn("kIrradianceIndirectOffset", pass_source)
        self.assertIn("IndirectPass::Trace", pass_source)
        self.assertIn("IndirectPass::Distance", pass_source)
        self.assertIn("IndirectPass::Irradiance", pass_source)

    def test_flax_sdf_sampler_is_always_valid_before_argument_tables(self) -> None:
        source = read("render/passes/FlaxDDGIPass.cpp")
        init = source[source.index("void FlaxDDGIPass::initResources") :]
        init = init[: init.index("void FlaxDDGIPass::shutdownResources")]

        sampler_creation = init.index("m_fallbackSampler = device.createSampler")
        first_table = init.index("m_classifyTable = device.createArgumentTable")
        self.assertLess(sampler_creation, first_table)
        self.assertIn("if (m_fallbackSampler.isNull())", init[sampler_creation:first_table])

        classify_table = init[first_table : init.index("// InitArgs table")]
        trace_table = init[init.index("// TraceRays table") : init.index("// Distance table")]
        self.assertIn("writeSampler(m_classifyTable, 3, sampler)", classify_table)
        self.assertIn("writeCombinedTextureRO(m_traceRaysTable, 3", trace_table)
        self.assertIn("writeSampler(m_traceRaysTable, 4, sampler)", trace_table)

    def test_flax_classification_decodes_global_sdf_encoding(self) -> None:
        shader = read("shaders/ddgi_flax_classify.slang")

        self.assertIn("encodedSDF * 2.0f - 1.0f", shader)
        self.assertIn("maxEncodeDistance", shader)
        self.assertNotIn(
            "globalSDFTex.SampleLevel(linearSampler, sdfUvw, 0) * (boundsMax.x - boundsMin.x)",
            shader,
        )

    def test_flax_ddgi_data_descriptor_tracks_frame_buffer(self) -> None:
        source = read("render/passes/FlaxDDGIPass.cpp")
        execute = source[source.index("void FlaxDDGIPass::execute") :]
        classify = execute.index('cmd.beginEvent("DDGI.Classify")')

        self.assertIn("context.frameIndex % kMaxFramesInFlight", execute[:classify])
        self.assertIn("bindFlaxDDGIDataBuffer(frameIndex)", execute[:classify])
        self.assertIn("writeFlaxDDGIDataToBuffer(frameIndex)", execute[:classify])
        self.assertNotIn("m_uniformFrameIndex", source)

    def test_flax_probe_budget_rotates_across_the_volume(self) -> None:
        pass_source = read("render/passes/FlaxDDGIPass.cpp")
        pass_header = read("render/passes/FlaxDDGIPass.h")
        classify = read("shaders/ddgi_flax_classify.slang")
        shader_io = read("shaders/shader_io.h")

        self.assertIn("probeUpdateOffset", pass_header)
        self.assertIn("activeProbes[1 + slot] = probeIndex", classify)
        self.assertNotIn("relativeProbeIndex", classify)
        trace = read("shaders/ddgi_flax_trace_rays.slang")
        self.assertIn("selectedActiveSlot", trace)
        self.assertIn("m_probeUpdateOffset", pass_source)
        self.assertIn("maxUpdatedProbesPerFrame", pass_source)
        self.assertIn("uvec4 updateParams", shader_io)

    def test_flax_trace_uses_runtime_global_sdf_bounds(self) -> None:
        shader = read("shaders/ddgi_flax_trace_rays.slang")

        self.assertIn("ddgi.sdfBoundsMinAndVoxel", shader)
        self.assertIn("ddgi.sdfBoundsMaxAndRes", shader)
        self.assertNotIn("float3(-16, -16, -16)", shader)
        self.assertNotIn("float3(16, 16, 16)", shader)

    def test_flax_history_resources_ping_pong_with_frame_parity(self) -> None:
        pass_source = read("render/passes/FlaxDDGIPass.cpp")
        pass_header = read("render/passes/FlaxDDGIPass.h")
        distance = read("shaders/ddgi_flax_update_distance.slang")

        self.assertIn("bindFlaxHistoryParity", pass_header)
        self.assertIn("sourceFrame & 1u", pass_source)
        self.assertIn("m_probesIrradianceViewB", pass_source)
        self.assertIn("m_probesDistanceViewB", pass_source)
        self.assertIn("probesDistanceHist", distance)
        self.assertNotIn("float2 historyData = probesDistance.Load", distance)

    def test_flax_runtime_exposes_only_implemented_cascades(self) -> None:
        renderer = read("render/GPUDrivenRenderer.cpp")
        pass_source = read("render/passes/FlaxDDGIPass.cpp")

        self.assertIn("kFlaxImplementedCascadeCount", renderer)
        self.assertIn("kFlaxImplementedCascadeCount", pass_source)
        self.assertNotIn("const uint32_t cascadeCount = std::min(config.maxCascades", renderer)

    def test_flax_debug_modes_sample_flax_owned_atlases(self) -> None:
        shader = read("shaders/shader.light.slang")
        debug = shader[shader.index("const uint ddgiDebugMode") :]
        debug = debug[: debug.index("const bool useIBL")]

        self.assertIn("evaluateFlaxDDGIIrradiance", debug)
        self.assertIn("if(flaxDDGIEnabled())", debug)

    def test_incomplete_surface_atlas_falls_back_to_global_albedo(self) -> None:
        shader = read("shaders/ddgi_flax_trace_rays.slang")

        self.assertIn("kUseSurfaceAtlas = false", shader)
        self.assertIn("globalAlbedoTex.SampleLevel", shader)

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
        reset = source[source.index("isNewFlaxGIDebugRequest(debugOptions->flaxGIResetRequestId") :]
        reset = reset[: reset.index("const uint32_t cascadeCount")]

        self.assertIn("clearColorTexture", reset)
        self.assertIn("StageFlags::transfer", reset)
        self.assertIn("StageFlags::compute", reset)
        self.assertIn("HazardFlags::textureWrites", reset)

    def test_flax_sampled_descriptors_match_general_layout(self) -> None:
        source = read("render/passes/FlaxDDGIPass.cpp")
        writer = source[source.index("auto writeTextureRO") :]
        writer = writer[: writer.index("auto writeBufferRW")]

        self.assertIn("ArgumentAccessIntent::readWrite", writer)
        self.assertNotIn("ArgumentAccessIntent::sampledRead", writer)

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
        table = source[source.index("// TraceRays table") :]
        table = table[: table.index("// Distance table")]

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
        self.assertIn("historyData.x * ddgi.rayMaxDistance", distance)

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


if __name__ == "__main__":
    unittest.main()
