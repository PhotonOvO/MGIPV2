import re
import unittest
from pathlib import Path


REPO_ROOT = Path(__file__).resolve().parents[1]
COMMON_SHADER = (REPO_ROOT / "shaders" / "flax_ddgi_common.slang").read_text(encoding="utf-8")
INACTIVE_SHADER = (REPO_ROOT / "shaders" / "ddgi_flax_update_inactive.slang").read_text(encoding="utf-8")
CLASSIFY_SHADER = (REPO_ROOT / "shaders" / "ddgi_flax_classify.slang").read_text(encoding="utf-8")
INIT_ARGS_SHADER = (REPO_ROOT / "shaders" / "ddgi_flax_init_args.slang").read_text(encoding="utf-8")
TRACE_SHADER = (REPO_ROOT / "shaders" / "ddgi_flax_trace_rays.slang").read_text(encoding="utf-8")
DISTANCE_SHADER = (REPO_ROOT / "shaders" / "ddgi_flax_update_distance.slang").read_text(encoding="utf-8")
IRRADIANCE_SHADER = (REPO_ROOT / "shaders" / "ddgi_flax_update_irradiance.slang").read_text(encoding="utf-8")
SDF_COMMON_SHADER = (REPO_ROOT / "shaders" / "sdf_common.slang").read_text(encoding="utf-8")
LIGHT_SHADER = (REPO_ROOT / "shaders" / "shader.light.slang").read_text(encoding="utf-8")
SHADER_IO = (REPO_ROOT / "shaders" / "shader_io.h").read_text(encoding="utf-8")
APP_HEADER = (REPO_ROOT / "app" / "MinimalLatestApp.h").read_text(encoding="utf-8")
PROBE_DEBUG_PASS = (REPO_ROOT / "render" / "passes" / "DDGIDebugPass.cpp").read_text(encoding="utf-8")
PROBE_VIS_SHADER = (REPO_ROOT / "shaders" / "ddgi_probe_visualization.slang").read_text(encoding="utf-8")
RENDERER_CPP = (REPO_ROOT / "render" / "GPUDrivenRenderer.cpp").read_text(encoding="utf-8")


class FlaxGIShaderContractTests(unittest.TestCase):
    def test_probe_atlas_resolutions_are_named_and_not_interchanged(self) -> None:
        self.assertIn("kDDGIProbeIrradianceResolution = 6", COMMON_SHADER)
        self.assertIn("kDDGIProbeDistanceResolution   = 14", COMMON_SHADER)
        self.assertIn("kDDGIProbeDistanceResolution", COMMON_SHADER)
        self.assertIn("kDDGIProbeIrradianceResolution", COMMON_SHADER)

    def test_probe_sampling_uses_directional_distance_and_normal_irradiance(self) -> None:
        self.assertIn("float2 flaxOctahedralEncode", COMMON_SHADER)
        self.assertIn("float3 probeToPointDir = -pointToProbeDir", COMMON_SHADER)
        self.assertIn("float2 probeToPointOct = flaxOctahedralEncode(probeToPointDir)", COMMON_SHADER)
        self.assertIn("float2 surfaceNormalOct = flaxOctahedralEncode(worldNormal)", COMMON_SHADER)
        self.assertRegex(
            COMMON_SHADER,
            r"getProbeUV\(ddgi, cascadeIndex, probeIndex,\s*probeToPointOct,\s*"
            r"kDDGIProbeDistanceResolution\)",
        )
        self.assertRegex(
            COMMON_SHADER,
            r"getProbeUV\(ddgi, cascadeIndex, probeIndex, surfaceNormalOct,\s*"
            r"kDDGIProbeIrradianceResolution\)",
        )
        self.assertNotRegex(
            COMMON_SHADER,
            r"float2\(0,\s*0\),\s*kDDGIProbeDistanceResolution",
        )

    def test_distance_visibility_uses_both_moments_continuously(self) -> None:
        self.assertIn("float2 distanceMoments", COMMON_SHADER)
        self.assertIn("float variance", COMMON_SHADER)
        self.assertIn("float chebyshevWeight", COMMON_SHADER)
        self.assertIn("rawChebyshevWeight * rawChebyshevWeight", COMMON_SHADER)
        self.assertIn("conservativeVisibility, rawChebyshevWeight, frontFacing", COMMON_SHADER)
        self.assertIn("float wrapShading", COMMON_SHADER)
        self.assertIn("wrapShading * wrapShading + 0.2f", COMMON_SHADER)
        self.assertIn("kFlaxDDGIMinVisibility", COMMON_SHADER)
        self.assertNotIn("surfaceSideWeight", COMMON_SHADER)

    def test_thin_surface_trace_uses_fine_mip_and_conservative_steps(self) -> None:
        self.assertIn("sampleGlobalSDFFineDistance", SDF_COMMON_SHADER)
        self.assertIn("float fineDistance", SDF_COMMON_SHADER)
        self.assertIn("float nextFineDistance", SDF_COMMON_SHADER)
        self.assertIn("config.voxelSize * 0.25f", SDF_COMMON_SHADER)
        self.assertIn("stepDistance - voxelHalf", SDF_COMMON_SHADER)
        self.assertIn("sdfConfig.maxSteps = 128", TRACE_SHADER)

    def test_sampling_preserves_a_continuous_low_backface_weight(self) -> None:
        self.assertIn("float normalAlignment", COMMON_SHADER)
        self.assertIn("float wrapShading", COMMON_SHADER)
        self.assertIn("wrapShading * wrapShading + 0.2f", COMMON_SHADER)
        self.assertNotIn("surfaceSideWeight", COMMON_SHADER)

    def test_probe_attention_is_recomputed_instead_of_decaying_each_frame(self) -> None:
        active_classification = CLASSIFY_SHADER[
            CLASSIFY_SHADER.index("// Active probe") :
            CLASSIFY_SHADER.index("// Relocate away from surfaces")
        ]
        self.assertIn("float viewAttention", active_classification)
        self.assertIn("float geometryAttention", active_classification)
        self.assertIn(
            "attention = clamp(viewAttention * geometryAttention", active_classification
        )
        self.assertNotIn("attention *=", active_classification)

    def test_distance_encoding_changes_invalidate_flax_resources(self) -> None:
        setter = RENDERER_CPP[
            RENDERER_CPP.index("void GPUDrivenRenderer::setEditableDDGIConfig") :
            RENDERER_CPP.index("void GPUDrivenRenderer::resetDDGIHistory")
        ]
        self.assertIn("previousConfig.maxDistance != config.maxDistance", setter)
        self.assertIn("previousConfig.ddgiGamma != config.ddgiGamma", setter)

    def test_flax_sampling_uses_ddgi_normal_bias_not_shadow_bias(self) -> None:
        self.assertIn(
            "float normalBias = lighting.light.ddgiFlaxFallbackIrradiance.w",
            LIGHT_SHADER,
        )
        self.assertNotIn(
            "float normalBias = lighting.light.lightColorAndNormalBias.w",
            LIGHT_SHADER,
        )
        self.assertIn("ddgiConfig.normalBias", RENDERER_CPP)
        self.assertIn("rgb = fallback, w = normal bias", SHADER_IO)

    def test_empty_history_is_not_blended_for_budget_delayed_probe(self) -> None:
        self.assertIn("bool historyValid", IRRADIANCE_SHADER)
        self.assertIn("historyIrradiance.a > 0.0f", IRRADIANCE_SHADER)
        self.assertIn("bool historyValid", DISTANCE_SHADER)
        self.assertIn("any(historyData > float2(0.0f, 0.0f))", DISTANCE_SHADER)
        self.assertIn("probeState == kDDGIProbeStateActivated || !historyValid", COMMON_SHADER)

    def test_probe_relocation_pushes_negative_sdf_toward_free_space(self) -> None:
        self.assertIn("float relocationDistance = max(voxelLimit - sdf, 0.0f)", CLASSIFY_SHADER)
        self.assertIn("sdfNormal * (relocationDistance * speed)", CLASSIFY_SHADER)
        self.assertNotIn("sdfNormal * ((sdf + voxelLimit) * speed)", CLASSIFY_SHADER)
        self.assertIn("float relocatedSDF", CLASSIFY_SHADER)
        self.assertIn("relocatedSDF <= 0.0f", CLASSIFY_SHADER)
        self.assertIn("state = kDDGIProbeStateInactive", CLASSIFY_SHADER)
        self.assertIn("if (sdf <= voxelLimit)", CLASSIFY_SHADER)
        self.assertNotIn("if (sdfDist < voxelLimit)", CLASSIFY_SHADER)

    def test_zero_distance_trace_hits_remain_occluders(self) -> None:
        self.assertIn("traceData.w < 0.0f", DISTANCE_SHADER)
        self.assertIn("? distanceLimit : min(traceData.w, distanceLimit)", DISTANCE_SHADER)
        self.assertNotIn("traceData.w <= 0.0f", DISTANCE_SHADER)

    def test_relocated_probe_invalidates_history_for_its_next_update(self) -> None:
        self.assertIn("bool relocationChanged", CLASSIFY_SHADER)
    def test_distance_moments_use_flax_directional_reaction_filter(self) -> None:
        self.assertIn("flaxProbeDistanceLimit(ddgi, cascadeIndex)", DISTANCE_SHADER)
        self.assertIn("min(traceData.w, distanceLimit)", DISTANCE_SHADER)
        self.assertIn("rayWeight = pow(rayWeight, 10.0f)", DISTANCE_SHADER)
        self.assertIn("sumDist += rayDistance * rayWeight", DISTANCE_SHADER)
        self.assertIn("sumDist2 += rayDistance * rayDistance * rayWeight", DISTANCE_SHADER)
        self.assertIn("relocationChanged =", CLASSIFY_SHADER)
        self.assertRegex(
            CLASSIFY_SHADER,
            r"relocationChanged\)\s*state = kDDGIProbeStateActivated",
        )


    def test_probe_visualization_controls_belong_to_flax_debug_ui(self) -> None:
        before_flax_ui, flax_ui = APP_HEADER.split(
            "inline void MinimalLatestApp::drawFlaxDebugUI()", maxsplit=1
        )
        self.assertNotIn("DDGI Probe Visualize", before_flax_ui)
        self.assertIn("Visualize FlaxGI Probes", flax_ui)
        self.assertIn("Probe Visualize Scale", flax_ui)

    def test_probe_visualization_binds_flax_owned_resources(self) -> None:
        self.assertIn("getFlaxDDGIResources()", PROBE_DEBUG_PASS)
        self.assertIn("getProbesIrradianceWrite(parity)", PROBE_DEBUG_PASS)
        self.assertIn("getProbesData()", PROBE_DEBUG_PASS)
        self.assertIn("m_usesFlaxResources", PROBE_DEBUG_PASS)
        self.assertIn("m_ddgiDebugPass->initResources", RENDERER_CPP)

    def test_flax_probe_visualization_uses_flax_grid_and_atlas_layout(self) -> None:
        self.assertIn("Texture2D<float4> flaxProbeData", PROBE_VIS_SHADER)
        self.assertIn("flaxProbeTextureCoordFromDirection", PROBE_VIS_SHADER)
        self.assertIn("flaxProbeCountsAndMode", PROBE_VIS_SHADER)
        self.assertIn("probeData.w <= -1.0f", PROBE_VIS_SHADER)
        self.assertIn("probeVisUniforms.debugScale", PROBE_VIS_SHADER)

    def test_flax_probe_visualization_honors_cascade_selector(self) -> None:
        self.assertIn("ddgiDebugCascadeIndex", PROBE_DEBUG_PASS)
        self.assertIn("writeUniforms(frameIndex, cascadeIndex, context)", PROBE_DEBUG_PASS)
        self.assertIn("cascades[cascadeIndex]", PROBE_DEBUG_PASS)
        self.assertIn("static_cast<int32_t>(cascadeIndex)", PROBE_DEBUG_PASS)
        self.assertIn("drawCascade", PROBE_DEBUG_PASS)

    def test_flax_probe_visualization_offsets_cascade_atlas_rows(self) -> None:
        self.assertIn("flaxProbeCascadeIndex()", PROBE_VIS_SHADER)
        self.assertIn(
            "coords.z + flaxProbeCascadeIndex() * counts.z",
            PROBE_VIS_SHADER,
        )

    def test_flax_probe_visualization_can_show_inactive_grid_coverage(self) -> None:
        self.assertIn("onlyActiveFlaxProbes()", PROBE_VIS_SHADER)
        self.assertIn(
            "if (probeData.w <= -1.0f && onlyActiveFlaxProbes())",
            PROBE_VIS_SHADER,
        )
        self.assertIn("output.probeState", PROBE_VIS_SHADER)
        self.assertIn("Only Active Probes", APP_HEADER)

    def test_inactive_probe_sanitation_is_deterministic(self) -> None:
        self.assertIn("Deterministic inactive-probe sanitation", INACTIVE_SHADER)
        self.assertIn("probesData[texel] = float4(0.0f, 0.0f, 0.0f, -1.0f)", INACTIVE_SHADER)
        self.assertNotIn("neighborData", INACTIVE_SHADER)
        self.assertNotIn("encodeFallbackCoords", INACTIVE_SHADER)

    def test_update_budget_compacts_deterministically_and_reuses_slack(self) -> None:
        self.assertIn("curvatureMagnitude", CLASSIFY_SHADER)
        self.assertIn("highGeometryComplexity", CLASSIFY_SHADER)
        self.assertIn("activeProbes[priorityBase + probeIndex] = 1u", CLASSIFY_SHADER)
        self.assertNotIn("InterlockedAdd(activeProbes", CLASSIFY_SHADER)
        self.assertIn("CS_CompactActiveProbes", INIT_ARGS_SHADER)
        self.assertIn("activeProbes[priorityBase + priorityCount++] = probeIndex", INIT_ARGS_SHADER)
        self.assertIn("activeProbes[regularBase + regularCount++] = probeIndex", INIT_ARGS_SHADER)
        self.assertIn("initArgs.scratchOffset", INIT_ARGS_SHADER)
        self.assertIn("availableSlack", INIT_ARGS_SHADER)
        self.assertIn("uint priorityBase = 4u", COMMON_SHADER)
        self.assertIn("uint updateCount = min(activeProbes[3], activeCount)", COMMON_SHADER)
        self.assertIn("ddgi.updateParams[cascadeIndex].z", COMMON_SHADER)
        self.assertIn("uint regularSlots", COMMON_SHADER)
        self.assertIn("update every active probe exactly", COMMON_SHADER)

    def test_uninitialized_probe_sampling_uses_configured_fallback(self) -> None:
        self.assertIn("state != kDDGIProbeStateActive", COMMON_SHADER)
        self.assertIn("encodedIrradiance.a <= 0.0f", COMMON_SHADER)
        self.assertIn("validTrilinearWeight += weight", COMMON_SHADER)
        self.assertIn("inactiveTrilinearWeight += weight", COMMON_SHADER)
        self.assertIn("float averageContributionScale", COMMON_SHADER)
        self.assertIn("ddgi.fallbackIrradiance.rgb * fallbackWeight", COMMON_SHADER)
        self.assertNotIn("ddgi.fallbackIrradiance.rgb * weight", COMMON_SHADER)


if __name__ == "__main__":
    unittest.main()
