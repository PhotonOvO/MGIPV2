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
        self.assertIn("chebyshevWeight * chebyshevWeight * chebyshevWeight", COMMON_SHADER)
        self.assertIn("float wrapShading", COMMON_SHADER)
        self.assertIn("max(0.1f, wrapShading * wrapShading)", COMMON_SHADER)
        self.assertIn("minWeightThreshold", COMMON_SHADER)
        self.assertIn("backFaceWeight * backFaceWeight", COMMON_SHADER)
        self.assertNotIn("wrapShading * wrapShading + 0.2f", COMMON_SHADER)
        self.assertNotIn("distVis = 0.2f", COMMON_SHADER)

    def test_thin_surface_trace_uses_fine_mip_and_conservative_steps(self) -> None:
        self.assertIn("sampleGlobalSDFFineDistance", SDF_COMMON_SHADER)
        self.assertIn("float fineDistance", SDF_COMMON_SHADER)
        self.assertIn("float nextFineDistance", SDF_COMMON_SHADER)
        self.assertIn("config.voxelSize * 0.25f", SDF_COMMON_SHADER)
        self.assertIn("stepDistance - voxelHalf", SDF_COMMON_SHADER)
        self.assertIn("sdfConfig.maxSteps = 128", TRACE_SHADER)

    def test_sampling_rejects_strongly_backfacing_probe_leaks(self) -> None:
        self.assertIn("float normalAlignment", COMMON_SHADER)
        self.assertIn("float surfaceSideWeight", COMMON_SHADER)
        self.assertIn("smoothstep(-0.35f, 0.0f, normalAlignment)", COMMON_SHADER)
        self.assertIn("backFaceWeight *= surfaceSideWeight", COMMON_SHADER)

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
        self.assertIn("!historyValid", IRRADIANCE_SHADER)
        self.assertIn("bool historyValid", DISTANCE_SHADER)
        self.assertIn("any(historyData > float2(0.0f, 0.0f))", DISTANCE_SHADER)
        self.assertIn("!historyValid", DISTANCE_SHADER)

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
        self.assertIn("if (hitDist >= 0.0f)", DISTANCE_SHADER)
        self.assertNotIn("if (hitDist > 0.0f)", DISTANCE_SHADER)

    def test_relocated_probe_invalidates_history_for_its_next_update(self) -> None:
        self.assertIn("bool relocationChanged", CLASSIFY_SHADER)
    def test_distance_moments_use_flax_directional_reaction_filter(self) -> None:
        self.assertIn("float distanceLimit = probesSpacing * 1.5f", DISTANCE_SHADER)
        self.assertIn(
            "float rayDistance = min(hitDist, distanceLimit)", DISTANCE_SHADER
        )
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

    def test_inactive_fallback_uses_encoded_logical_probe_coordinates(self) -> None:
        self.assertIn("getLogicalProbeCoordsFromDataTexel", COMMON_SHADER)
        self.assertIn("uint3 targetProbeCoords", INACTIVE_SHADER)
        self.assertIn("uint3 neighborProbeCoords", INACTIVE_SHADER)
        self.assertIn("encodeFallbackCoords(neighborProbeCoords)", INACTIVE_SHADER)
        self.assertNotIn("float3(neighborCoord.x", INACTIVE_SHADER)

    def test_fallback_coordinates_are_bounds_checked_before_use(self) -> None:
        self.assertIn("fallbackCoordsValid(ddgi,", COMMON_SHADER)
        self.assertIn("fallbackCoordsValid(ddgi,", INACTIVE_SHADER)

    def test_update_budget_rotates_over_the_compacted_active_list(self) -> None:
        self.assertNotIn("relativeProbeIndex", CLASSIFY_SHADER)
        self.assertIn("state != kDDGIProbeStateInactive", CLASSIFY_SHADER)
        self.assertIn("activeProbes[1 + slot] = probeIndex", CLASSIFY_SHADER)

        self.assertIn("uint totalActiveCount", INIT_ARGS_SHADER)
        self.assertIn("uint updateCount", INIT_ARGS_SHADER)
        self.assertNotIn("activeProbes[0] = updateCount", INIT_ARGS_SHADER)

        for shader in (TRACE_SHADER, DISTANCE_SHADER, IRRADIANCE_SHADER):
            self.assertIn("totalActiveCount", shader)
            self.assertIn("selectedActiveSlot", shader)
            self.assertIn("ddgi.updateParams.x", shader)

    def test_sampling_fallback_updates_coordinate_and_index_spaces_together(self) -> None:
        fallback_block = re.search(
            r"// Inactive probe -> use fallback\s*"
            r"if \(state == kDDGIProbeStateInactive\)(.*?)"
            r"// Decode real probe position",
            COMMON_SHADER,
            re.DOTALL,
        )
        self.assertIsNotNone(fallback_block)
        block = fallback_block.group(1)
        self.assertIn("probeCoords = fallbackCoords;", block)
        self.assertIn("getScrollingProbeIndex(ddgi, cascadeIndex, probeCoords)", block)


if __name__ == "__main__":
    unittest.main()
