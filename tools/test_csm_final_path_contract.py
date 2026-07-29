"""Narrow source contracts for the final CSM camera, basis, culling, and stage path."""

import re
import unittest
from pathlib import Path


REPO_ROOT = Path(__file__).resolve().parents[1]
CSM_SOURCE = (REPO_ROOT / "render" / "CSMShadowResources.cpp").read_text(
    encoding="utf-8"
)
CSM_HEADER = (REPO_ROOT / "render" / "CSMShadowResources.h").read_text(
    encoding="utf-8"
)
GPU_HEADER = (REPO_ROOT / "render" / "GPUDrivenRenderer.h").read_text(
    encoding="utf-8"
)
APP_HEADER = (REPO_ROOT / "app" / "MinimalLatestApp.h").read_text(
    encoding="utf-8"
)
RENDER_TYPES = (REPO_ROOT / "render" / "RenderTypes.h").read_text(
    encoding="utf-8"
)
RENDER_DEVICE = (REPO_ROOT / "render" / "RenderDevice.cpp").read_text(
    encoding="utf-8"
)
GPU_RENDERER = (REPO_ROOT / "render" / "GPUDrivenRenderer.cpp").read_text(
    encoding="utf-8"
)
CSM_PASS = (
    REPO_ROOT / "render" / "passes" / "GPUDrivenCSMShadowPass.cpp"
).read_text(encoding="utf-8")
CSM_STABILITY_TEST = (
    REPO_ROOT / "tests" / "csm_shadow_stability_tests.cpp"
).read_text(encoding="utf-8")
LIGHT_SHADER = (REPO_ROOT / "shaders" / "shader.light.slang").read_text(
    encoding="utf-8"
)
SHADER_IO = (REPO_ROOT / "shaders" / "shader_io.h").read_text(encoding="utf-8")
SCENE_RESOURCES_HEADER = (REPO_ROOT / "render" / "SceneResources.h").read_text(
    encoding="utf-8"
)


def braced_source(source: str, pattern: str) -> str:
    match = re.search(pattern, source, re.MULTILINE | re.DOTALL)
    if match is None:
        raise AssertionError(f"Could not find source pattern: {pattern}")

    opening_brace = source.find("{", match.end())
    if opening_brace < 0:
        raise AssertionError(f"Could not find block for source pattern: {pattern}")

    depth = 0
    for index in range(opening_brace, len(source)):
        if source[index] == "{":
            depth += 1
        elif source[index] == "}":
            depth -= 1
            if depth == 0:
                return source[opening_brace : index + 1]

    raise AssertionError(f"Unterminated block for source pattern: {pattern}")


class CSMUnjitteredFallbackContractTests(unittest.TestCase):
    def test_shadow_fit_camera_helper_reconstructs_projection_and_has_fallback(self) -> None:
        helper = braced_source(
            RENDER_TYPES,
            r"\bmakeUnjitteredShadowFitCamera\s*\(",
        )

        self.assertIn(
            "const glm::mat4 inverseView = glm::inverse(camera.view);", helper
        )
        self.assertIn("camera.unjitteredViewProjection", helper)
        self.assertIn(
            "const glm::mat4 fallbackViewProjection = camera.projection * camera.view;",
            helper,
        )
        self.assertIn(
            "const glm::mat4 shadowFitProjection = selectedViewProjection * inverseView;",
            helper,
        )
        self.assertIn("std::isfinite", helper)
        self.assertIn("glm::determinant", helper)
        self.assertIn("identityTemporalSentinel", helper)

    def test_render_device_and_gpu_driven_csm_updates_use_shadow_fit_camera(self) -> None:
        draw_frame = braced_source(
            RENDER_DEVICE,
            r"\bvoid\s+RenderDevice::drawFrame\s*\(",
        )
        gpu_render = braced_source(
            GPU_RENDERER,
            r"\bvoid\s+GPUDrivenRenderer::render\s*\(",
        )

        for body in (draw_frame, gpu_render):
            self.assertIn("makeUnjitteredShadowFitCamera", body)
            self.assertRegex(
                body,
                r"updateCascadeMatrices\s*\(\s*shadowFitCamera\s*,",
            )
            self.assertNotRegex(
                body,
                r"updateCascadeMatrices\s*\(\s*\*params\.cameraUniforms\s*,",
            )

    def test_legacy_single_shadow_fit_is_debug_only_but_csm_light_params_are_not(self) -> None:
        lighting_state = braced_source(
            RENDER_DEVICE,
            r"\bRenderDevice::FrameLightingState\s+"
            r"RenderDevice::buildFrameLightingState\s*\(",
        )
        debug_fit = braced_source(
            lighting_state,
            r"\bif\s*\(\s*params\.debugOptions\.enabled\s*\)",
        )

        self.assertIn("makeUnjitteredShadowFitCamera(camera)", debug_fit)
        self.assertIn("state.shadowCamera.viewProjection", debug_fit)
        self.assertIn("state.shadowFrustumCorners", debug_fit)
        self.assertNotIn("state.lightParams.worldToShadow", debug_fit)
        self.assertIn(
            "// Populate CSM cascade matrices and split distances for LightParams",
            lighting_state,
        )
        self.assertIn("state.lightParams.worldToShadow[i]", lighting_state)
        self.assertGreater(
            lighting_state.index("state.lightParams.worldToShadow[i]"),
            lighting_state.index("state.shadowFrustumCorners"),
        )


class CSMShaderTemporalContractTests(unittest.TestCase):
    def test_cpu_stable_projection_grid_consumes_shared_pcf_guard(self) -> None:
        grid = braced_source(
            CSM_SOURCE,
            r"\bStableProjectionGrid\s+makeStableProjectionGrid\s*\(",
        )

        def uint_assignment(identifier: str) -> str:
            match = re.search(
                rf"\bconst\s+uint32_t\s+{identifier}\s*=\s*(.*?);",
                grid,
                re.DOTALL,
            )
            self.assertIsNotNone(match, f"missing {identifier} dataflow")
            assert match is not None
            return re.sub(r"\s+", " ", match.group(1)).strip()

        # Pin the entire guard-to-usable-resolution chain. Replacing the shared
        # guard with LCascadePcfRadius + 1 or a literal must fail this contract.
        self.assertEqual(
            uint_assignment("requestedGuardTexels"),
            "static_cast<uint32_t>(std::max(shaderio::LCascadePcfGuardTexels, 0))",
        )
        self.assertEqual(
            uint_assignment("guardTexels"),
            "std::min(requestedGuardTexels, maxGuardTexels)",
        )
        self.assertEqual(
            uint_assignment("receiverTexels"),
            "std::max(safeResolution - guardTexels * 2u, 1u)",
        )
        self.assertRegex(
            grid,
            r"const\s+float\s+texelSize\s*=\s*receiverDiameter\s*/\s*"
            r"static_cast<float>\(receiverTexels\)\s*;",
        )
        self.assertEqual(grid.count("shaderio::LCascadePcfGuardTexels"), 1)
        self.assertNotIn("shaderio::LCascadePcfRadius", grid)

    def test_cascade_selection_and_blend_are_projection_coverage_driven(self) -> None:
        projection = braced_source(
            LIGHT_SHADER,
            r"\bfloat4\s+projectCSMShadowCascade\s*\(",
        )
        coverage = braced_source(
            LIGHT_SHADER,
            r"\bfloat\s+cascadeCoverageBlendWeight\s*\(",
        )
        sample = braced_source(
            LIGHT_SHADER,
            r"\bfloat\s+sampleCSMShadow\s*\(",
        )

        self.assertIn("lighting.light.worldToShadow[cascadeIdx]", projection)
        self.assertIn("pcfSafeMarginUv", projection)
        self.assertIn("edgeDistanceUv >= pcfSafeMarginUv", projection)
        self.assertIn("shadowNdc.z > 0.0 && shadowNdc.z < 1.0", projection)
        self.assertIn("validProjection ? 1.0 : 0.0", projection)

        self.assertIn("edgeDistanceUv / texelSize", coverage)
        self.assertIn("floor(max(edgeDistanceUv / texelSize, 0.0))", coverage)
        self.assertIn("LCascadePcfGuardTexels", coverage)
        self.assertIn("LCascadeCoverageBlendTexels", coverage)
        self.assertIn(
            "LCascadePcfGuardTexels - LCascadeCoverageBlendTexels",
            coverage,
        )
        self.assertNotIn("+ 3", coverage)
        self.assertIn("smoothstep", coverage)

        compact_shader_io = re.sub(r"\s+", " ", SHADER_IO)
        self.assertIn("LCascadeCoverageBlendTexels = 4;", compact_shader_io)
        self.assertIn(
            "LCascadePcfGuardTexels = LCascadePcfRadius + 1 + "
            "LCascadeCoverageBlendTexels;",
            compact_shader_io,
        )

        self.assertIn("int selectedCascade = -1;", sample)
        self.assertIn("selectedCascade < 0", sample)
        self.assertIn("projection.w > 0.5", sample)
        self.assertIn("const int fallbackCascade = selectedCascade + 1;", sample)
        self.assertIn("fallbackProjection.w > 0.5", sample)
        self.assertIn("float fallbackVisibility = 1.0;", sample)
        fallback_region = sample[sample.index("const float coverageBlend") :]
        self.assertNotIn("for(int cascadeIdx", fallback_region)
        self.assertNotIn("cascadeIdx > selectedCascade", fallback_region)
        self.assertIn("cascadeCoverageBlendWeight(selectedProjection.xy)", sample)
        self.assertIn("lerp(visibility, fallbackVisibility, coverageBlend)", sample)
        self.assertEqual(sample.count("projectCSMShadowCascade"), 2)

        self.assertNotIn("viewDepth", sample)
        self.assertNotIn("camera.view", sample)
        self.assertNotIn("camera.cameraPosition", sample)
        self.assertNotIn("radialDistance", sample)
        self.assertNotIn("distanceFade", sample)
        self.assertNotIn("getCascadeSplitDistance", LIGHT_SHADER)
        self.assertNotIn("selectCascadeIndex", LIGHT_SHADER)
        self.assertNotIn("LCascadeBlendRegion", sample)
        self.assertNotIn("LCascadeBlendMinDistance", sample)
        self.assertNotIn("cascadeBlend", sample)
        self.assertIn("return visibility;", sample)
        self.assertGreaterEqual(sample.count("return 1.0;"), 1)

    def test_world_to_shadow_stays_clip_space_and_shader_maps_ndc_to_uv(self) -> None:
        projection = braced_source(
            LIGHT_SHADER,
            r"\bfloat4\s+projectCSMShadowCascade\s*\(",
        )
        render_device_lighting = braced_source(
            RENDER_DEVICE,
            r"\bRenderDevice::FrameLightingState\s+"
            r"RenderDevice::buildFrameLightingState\s*\(",
        )
        gpu_lighting = braced_source(
            GPU_RENDERER,
            r"\bvoid\s+GPUDrivenRenderer::updateGPUDrivenLights\s*\(",
        )

        self.assertIn(
            "state.lightParams.worldToShadow[i] = shadowData->cascadeViewProjection[i];",
            render_device_lighting,
        )
        self.assertIn(
            "lightingUniforms.light.worldToShadow[i] = shadowData->cascadeViewProjection[i];",
            gpu_lighting,
        )
        self.assertIn("float3 shadowNdc = shadowClip.xyz / shadowClip.w;", projection)
        self.assertIn("float2 shadowUv = shadowNdc.xy * 0.5 + 0.5;", projection)
        self.assertNotIn("worldToShadowTexture", projection)
        self.assertNotIn("makeNdcToShadowTextureMatrix", projection)

    def test_light_writes_neutral_safe_shadow_signal_to_hdr_alpha(self) -> None:
        directional = braced_source(
            LIGHT_SHADER,
            r"\bDirectionalLightEvaluation\s+evaluateDirectionalLight\s*\(",
        )
        legacy_light = braced_source(
            LIGHT_SHADER,
            r"\bfloat4\s+fragmentMain\s*\(",
        )
        hdr_light = braced_source(
            LIGHT_SHADER,
            r"\bfloat4\s+fragmentHdrMain\s*\(",
        )
        sky = braced_source(
            LIGHT_SHADER,
            r"\bfloat4\s+fragmentSkyboxMain\s*\(",
        )

        self.assertIn("struct DirectionalLightEvaluation", LIGHT_SHADER)
        self.assertIn("float shadowSignal;", LIGHT_SHADER)
        self.assertIn("saturate(lighting.light.lightDirectionAndShadowStrength.w)", directional)
        self.assertIn("shadowStrength > 0.0 ? sampleCSMShadow(worldPos) : 1.0", directional)
        self.assertIn("directVisibility = lerp(1.0, shadowVisibility, shadowStrength)", directional)
        self.assertIn("result.shadowSignal = directVisibility;", directional)

        for light_body in (legacy_light, hdr_light):
            self.assertIn("DirectionalLightEvaluation directional", light_body)
            self.assertIn("directional.color", light_body)
            self.assertIn("directional.shadowSignal", light_body)
        self.assertNotIn(
            "ambient + directional + pointLights",
            legacy_light + hdr_light,
        )
        self.assertIn("evaluateEnvironmentBackground(input.uv), 1.0", sky)

    def test_taa_rejects_changed_shadow_history_and_preserves_current_signal(self) -> None:
        taa = braced_source(
            LIGHT_SHADER,
            r"\bfloat4\s+fragmentTAAResolveMain\s*\(",
        )

        self.assertIn("const float4 currentSceneColor", taa)
        self.assertIn("kSceneColorHdrIndex", taa)
        self.assertIn("currentSceneColor.a", taa)
        self.assertIn("const float historyAlpha", taa)
        self.assertIn("kSceneColorHistoryReadIndex", taa)
        self.assertIn("SampleLevel(historyUv, 0.0).a", taa)
        self.assertIn("abs(currentShadowSignal - historyShadowSignal)", taa)
        self.assertIn("smoothstep(0.01, 0.05, shadowSignalDelta)", taa)
        self.assertIn("blendWeight *= 1.0 - shadowReactiveFactor;", taa)
        self.assertRegex(
            taa,
            r"(?s)if\s*\(\s*!historyOutOfBounds\s*&&\s*motionPixels\s*<\s*0\.5\s*\)"
            r"\s*\{\s*if\s*\(\s*shadowReactiveFactor\s*<=\s*0\.0\s*\)"
            r"\s*\{.*?const\s+float\s+kFireflyRatio",
        )

        lottes_index = taa.index("if(useLottes")
        final_reactive_gate = taa.index(
            "blendWeight *= 1.0 - shadowReactiveFactor;"
        )
        mix_index = taa.index("float3 mixed")
        self.assertLess(lottes_index, final_reactive_gate)
        self.assertLess(final_reactive_gate, mix_index)
        after_final_gate = taa[
            final_reactive_gate
            + len("blendWeight *= 1.0 - shadowReactiveFactor;") : mix_index
        ]
        self.assertNotRegex(
            after_final_gate,
            r"\bblendWeight\s*(?:=|\+=|-=|\*=|/=)",
        )

        # Semantic regression: even if Lottes raises a 0.9 history weight, a fully
        # reactive shadow transition must make the final history contribution zero.
        lottes_weight = 1.0 - (1.0 - 0.9) * 0.5 * 0.5
        self.assertGreater(lottes_weight, 0.9)
        self.assertEqual(lottes_weight * (1.0 - 1.0), 0.0)

        # The old firefly cap would clamp current=1.0 against history=0.05 to 0.25.
        # Any positive shadow reactive factor bypasses that history-guided cap.
        def apply_firefly_cap(current: float, history: float, reactive: float) -> float:
            if reactive <= 0.0:
                return min(current, history * 4.0 + 0.05)
            return current

        self.assertAlmostEqual(apply_firefly_cap(1.0, 0.05, 0.0), 0.25)
        self.assertEqual(apply_firefly_cap(1.0, 0.05, 1.0), 1.0)

        self.assertIn("return float4(max(currentColor, 0.0), currentShadowSignal);", taa)
        self.assertIn("return float4(max(resolved, 0.0), currentShadowSignal);", taa)
        self.assertNotIn("return float4(max(currentColor, 0.0), 1.0);", taa)
        self.assertNotIn("return float4(max(resolved, 0.0), 1.0);", taa)

        sample_post = braced_source(
            LIGHT_SHADER,
            r"\bfloat3\s+samplePostTexture\s*\(",
        )
        final_color = braced_source(
            LIGHT_SHADER,
            r"\bfloat4\s+fragmentFinalColorMain\s*\(",
        )
        self.assertIn(".Sample(uv).rgb", sample_post)
        self.assertIn(".Sample(input.uv).rgb", final_color)
        self.assertIn(
            "kSceneColorHdrFormat = rhi::TextureFormat::rgba16Sfloat",
            SCENE_RESOURCES_HEADER,
        )


class CSMFinalPathContractTests(unittest.TestCase):
    def test_light_basis_uses_scale_safe_double_history_transport(self) -> None:
        update_body = braced_source(
            CSM_SOURCE,
            r"\bvoid\s+CSMShadowResources::updateCascadeMatrices\s*\("
            r"\s*const\s+shaderio::CameraUniforms&\s+camera\s*,"
            r"\s*const\s+glm::vec3&\s+lightDir\s*,"
            r"\s*float\s+requestedMaxShadowDistance",
        )
        basis_body = braced_source(
            CSM_SOURCE,
            r"\bCSMShadowResources::resolveStableLightUp\s*\(",
        )
        scale_safe_normalize = braced_source(
            CSM_SOURCE,
            r"\bbool\s+tryScaleSafeNormalize\s*\(",
        )
        reset_body = braced_source(
            CSM_SOURCE,
            r"\bvoid\s+CSMShadowResources::resetLightBasisHistory\s*\(",
        )
        init_body = braced_source(
            CSM_SOURCE,
            r"\bvoid\s+CSMShadowResources::init\s*\(",
        )
        deinit_body = braced_source(
            CSM_SOURCE,
            r"\bvoid\s+CSMShadowResources::deinit\s*\(",
        )

        self.assertIn("resolveStableLightUp(lightDirection)", update_body)
        self.assertNotIn("makeStableLightUp", CSM_SOURCE)
        self.assertNotRegex(
            update_body,
            r"abs\s*\(\s*lightDirection\.y\s*\)\s*[><=]+\s*0\.95",
        )
        for state_member in (
            "m_previousLightDirection",
            "m_previousLightUp",
            "m_previousLightBasisValid",
            "m_lightBasisProjectionChartSelected",
        ):
            self.assertIn(state_member, CSM_HEADER)
        for basis_member in (
            "m_previousLightDirection",
            "m_previousLightUp",
            "m_previousLightBasisValid",
            "m_lightBasisProjectionChartSelected",
        ):
            self.assertIn(basis_member, basis_body)
        self.assertIn("isFiniteVector(m_previousLightDirection)", basis_body)
        self.assertIn("isFiniteVector(m_previousLightUp)", basis_body)
        for double_vector in (
            "glm::dvec3 previousDirection;",
            "glm::dvec3 previousUp;",
            "glm::dvec3 direction;",
            "glm::dvec3 transportedUp;",
        ):
            self.assertIn(double_vector, basis_body)
        self.assertIn(
            "tryScaleSafeNormalize(glm::dvec3(m_previousLightDirection), "
            "previousDirection)",
            basis_body,
        )
        self.assertIn(
            "tryScaleSafeNormalize(glm::dvec3(m_previousLightUp), previousUp)",
            basis_body,
        )
        self.assertIn(
            "tryScaleSafeNormalize(glm::dvec3(normalizedDirection), direction)",
            basis_body,
        )
        self.assertIn(
            "previousUp -= previousDirection * glm::dot(previousUp, "
            "previousDirection);",
            basis_body,
        )
        self.assertIn("resolvedUp = glm::vec3(transportedUp);", basis_body)

        self.assertIn(
            "const glm::dvec3 absoluteValue = glm::abs(value);",
            scale_safe_normalize,
        )
        self.assertIn("const double largestComponent =", scale_safe_normalize)
        self.assertIn(
            "const glm::dvec3 scaledValue = value / largestComponent;",
            scale_safe_normalize,
        )
        self.assertIn(
            "const double scaledLengthSq = glm::dot(scaledValue, scaledValue);",
            scale_safe_normalize,
        )
        self.assertIn(
            "normalized = scaledValue / std::sqrt(scaledLengthSq);",
            scale_safe_normalize,
        )
        self.assertNotIn("glm::dot(value, value)", scale_safe_normalize)
        self.assertNotIn("kLightBasisVectorEpsilon", scale_safe_normalize)
        self.assertIn("m_lightBasisProjectionChartSelected = false;", reset_body)
        self.assertIn("resetLightBasisHistory();", init_body)
        self.assertIn("resetLightBasisHistory();", deinit_body)
        self.assertIn("if (!lightDirectionInputValid)", update_body)
        self.assertIn("isFiniteNonZeroVector(lightDir)", update_body)
        self.assertIn("resetLightBasisHistory();", update_body)

    def test_light_basis_uses_cancellation_safe_complementary_quaternion_charts(self) -> None:
        basis_body = braced_source(
            CSM_SOURCE,
            r"\bCSMShadowResources::resolveStableLightUp\s*\(",
        )
        scale_safe_normalize = braced_source(
            CSM_SOURCE,
            r"\bbool\s+tryScaleSafeNormalize\s*\(",
        )

        self.assertIn("Complementary quaternion transport charts", basis_body)
        self.assertIn(
            "const glm::dvec3 forwardDifference = previousDirection + direction;",
            basis_body,
        )
        self.assertIn(
            "const glm::dvec3 antipodalDifference = direction - previousDirection;",
            basis_body,
        )
        self.assertIn(
            "const double forwardScale = "
            "0.5 * glm::dot(forwardDifference, forwardDifference);",
            basis_body,
        )
        self.assertIn(
            "const double antipodalScale = "
            "0.5 * glm::dot(antipodalDifference, antipodalDifference);",
            basis_body,
        )
        self.assertIn(
            "previousUp * forwardScale - forwardDifference * "
            "previousUpDotDirection",
            basis_body,
        )
        self.assertIn(
            "previousUp * antipodalScale - antipodalDifference * "
            "previousUpDotDirection",
            basis_body,
        )
        self.assertIn(
            "selectedAntipodalChart ? antipodalTransportRaw : forwardTransportRaw",
            basis_body,
        )
        self.assertIn(
            "selectedAntipodalChart ? forwardTransportRaw : antipodalTransportRaw",
            basis_body,
        )
        self.assertIn(
            "const glm::dvec3& selectedPoleDifference =",
            basis_body,
        )
        self.assertIn(
            "selectedAntipodalChart ? antipodalDifference : forwardDifference",
            basis_body,
        )
        self.assertIn(
            "const double selectedPoleChordLengthSq =",
            basis_body,
        )
        self.assertIn(
            "glm::dot(selectedPoleDifference, selectedPoleDifference)",
            basis_body,
        )
        smoother_step = braced_source(
            CSM_SOURCE,
            r"\bdouble\s+smootherStep01\s*\(",
        )
        self.assertIn(
            "const double t = std::clamp(value, 0.0, 1.0);", smoother_step
        )
        self.assertIn(
            "return t * t * t * (t * (t * 6.0 - 15.0) + 10.0);",
            smoother_step,
        )

        self.assertRegex(
            CSM_SOURCE,
            r"constexpr\s+double\s+kLightBasisRegularizationInnerChord\s*=\s*"
            r"16\.0\s*\*\s*static_cast<double>\("
            r"std::numeric_limits<float>::epsilon\(\)\)\s*;",
        )
        self.assertRegex(
            CSM_SOURCE,
            r"constexpr\s+double\s+kLightBasisRegularizationInnerChordSq\s*=\s*"
            r"kLightBasisRegularizationInnerChord\s*\*\s*"
            r"kLightBasisRegularizationInnerChord\s*;",
        )
        self.assertRegex(
            CSM_SOURCE,
            r"constexpr\s+double\s+kLightBasisRegularizationOuterChord\s*=\s*"
            r"1024\.0\s*\*\s*static_cast<double>\("
            r"std::numeric_limits<float>::epsilon\(\)\)\s*;",
        )
        self.assertRegex(
            CSM_SOURCE,
            r"constexpr\s+double\s+kLightBasisRegularizationOuterChordSq\s*=\s*"
            r"kLightBasisRegularizationOuterChord\s*\*\s*"
            r"kLightBasisRegularizationOuterChord\s*;",
        )

        common_up_projection = (
            "glm::dvec3 commonUp = previousUp - direction * "
            "glm::dot(previousUp, direction);"
        )
        self.assertEqual(basis_body.count(common_up_projection), 2)
        self.assertLess(
            basis_body.index("glm::dvec3 analyticUp;"),
            basis_body.index(common_up_projection),
        )
        inner_pole_branch = braced_source(
            basis_body,
            r"\bif\s*\(\s*selectedPoleChordLengthSq\s*"
            r"<=\s*kLightBasisRegularizationInnerChordSq\s*\)",
        )
        annulus_branch = braced_source(
            basis_body,
            r"\belse\s+if\s*\(\s*analyticUpValid\s*"
            r"&&\s*selectedPoleChordLengthSq\s*"
            r"<\s*kLightBasisRegularizationOuterChordSq\s*\)",
        )
        analytic_outside_branch = braced_source(
            basis_body,
            r"\belse\s+if\s*\(\s*analyticUpValid\s*\)"
            r"(?=\s*\{\s*transportedUp\s*=\s*analyticUp\s*;\s*\}"
            r"\s*else\s*\{)",
        )
        alternate_chart_fallback = braced_source(
            basis_body,
            r"\belse\s*(?=\{\s*if\s*\(\s*!tryScaleSafeNormalize\s*\("
            r"\s*alternateRaw\s*,\s*transportedUp\s*\)\s*\))",
        )
        state_toggle = (
            "m_lightBasisProjectionChartSelected = !selectedAntipodalChart;"
        )

        self.assertIn(common_up_projection, inner_pole_branch)
        self.assertIn(
            "if (tryScaleSafeNormalize(commonUp, commonUp))",
            inner_pole_branch,
        )
        inner_analytic_fallback = braced_source(
            inner_pole_branch,
            r"\belse\s+if\s*\(\s*analyticUpValid\s*\)",
        )
        self.assertIn("transportedUp = commonUp;", inner_pole_branch)
        self.assertIn("transportedUp = analyticUp;", inner_analytic_fallback)
        self.assertNotIn("resetLightBasisHistory();", inner_analytic_fallback)
        self.assertIn(
            "else if (!tryScaleSafeNormalize(alternateRaw, transportedUp))",
            inner_pole_branch,
        )
        self.assertNotIn("analyticWeight", inner_pole_branch)
        self.assertEqual(inner_pole_branch.count(state_toggle), 1)

        self.assertIn(common_up_projection, annulus_branch)
        annulus_common_fallback = braced_source(
            annulus_branch,
            r"\bif\s*\(\s*!tryScaleSafeNormalize\s*\(\s*commonUp\s*,"
            r"\s*commonUp\s*\)\s*\)",
        )
        self.assertIn("commonUp = analyticUp;", annulus_common_fallback)
        self.assertNotIn("resetLightBasisHistory();", annulus_common_fallback)

        for annulus_token in (
            "glm::dvec3 commonRight = glm::cross(direction, commonUp);",
            "tryScaleSafeNormalize(commonRight, commonRight)",
            "const double selectedPoleChord = std::sqrt(selectedPoleChordLengthSq);",
            "const double analyticWeight = smootherStep01(",
            "(selectedPoleChord - innerChord) / (outerChord - innerChord)",
            "const double rollCosine =",
            "const double rollSine =",
            "const double analyticHalfCosine =",
            "const double analyticHalfSine =",
            "const double blendedHalfLength =",
            "std::hypot(blendedHalfCosine, blendedHalfSine)",
            "commonUp * blendedRollCosine + commonRight * blendedRollSine",
        ):
            self.assertIn(annulus_token, annulus_branch)
        self.assertNotIn(state_toggle, annulus_branch)
        self.assertIn("transportedUp = analyticUp;", analytic_outside_branch)
        self.assertNotIn("commonUp", analytic_outside_branch)
        self.assertNotIn("resetLightBasisHistory();", analytic_outside_branch)
        self.assertNotIn(state_toggle, analytic_outside_branch)
        self.assertIn(
            "if (!tryScaleSafeNormalize(alternateRaw, transportedUp))",
            alternate_chart_fallback,
        )
        self.assertIn("resetLightBasisHistory();", alternate_chart_fallback)
        self.assertEqual(alternate_chart_fallback.count(state_toggle), 1)
        self.assertEqual(basis_body.count(state_toggle), 2)

        inner_index = basis_body.index("kLightBasisRegularizationInnerChordSq")
        annulus_index = basis_body.index("kLightBasisRegularizationOuterChordSq")
        analytic_outside_index = basis_body.index(
            "else if (analyticUpValid)", annulus_index
        )
        fallback_index = basis_body.index(
            "if (!tryScaleSafeNormalize(alternateRaw, transportedUp))",
            analytic_outside_index,
        )
        self.assertLess(inner_index, annulus_index)
        self.assertLess(annulus_index, analytic_outside_index)
        self.assertLess(analytic_outside_index, fallback_index)

        zero_projection_test = braced_source(
            CSM_STABILITY_TEST,
            r"\bvoid\s+testAnalyticChartsHandleExactPreviousUpDirections\s*\(",
        )
        for zero_projection_token in (
            "std::array<bool, 2>{false, true}",
            "std::array<float, 2>{-1.0f, 1.0f}",
            "camera, 0.0f, 0.0f, historyUpSign, primeProjectionChart",
            "exact.realizedProjectedLengthSq == 0.0f",
            "const glm::dvec3 chartDifference = primeProjectionChart",
            "? direction - previousDirection",
            ": previousDirection + direction",
            "zero commonUp projection bypassed a valid selected analytic chart",
            "useHistoryRightTangent",
            "std::nextafter(",
            "kMaximumNextafterInputAngleRadians",
            "kMaximumNextafterBasisAngleRadians",
        ):
            self.assertIn(zero_projection_token, zero_projection_test)
        self.assertIn(
            "testAnalyticChartsHandleExactPreviousUpDirections();",
            CSM_STABILITY_TEST,
        )

        component_ulp_test = braced_source(
            CSM_STABILITY_TEST,
            r"\bvoid\s+testComponentUlpPoleBoundaryIsRegularized\s*\(",
        )
        for component_ulp_token in (
            "0.0312809013f, -0.0925535858f, -0.0779765174f",
            "std::nextafter(",
            "kLightBasisRegularizationInnerChord",
            "kLightBasisRegularizationOuterChord",
            "basisPairAngle < inputPairAngle * 3.0f + 1.0e-8f",
        ):
            self.assertIn(component_ulp_token, component_ulp_test)

        annulus_boundary_test = braced_source(
            CSM_STABILITY_TEST,
            r"\bvoid\s+testRegularizationAnnulusBoundariesAreSmooth\s*\(",
        )
        for boundary_token in (
            "std::array<bool, 2>{false, true}",
            "kLightBasisRegularizationInnerChord",
            "kLightBasisRegularizationOuterChord",
            "std::nextafter(boundaryRadius, 0.0f)",
            "std::nextafter(boundaryRadius, std::numeric_limits<float>::infinity())",
            "expanded radial pair did not straddle the realized annulus boundary",
            "regularization signed-roll cut leaked onto an annulus boundary",
        ):
            self.assertIn(boundary_token, annulus_boundary_test)

        random_history_test = braced_source(
            CSM_STABILITY_TEST,
            r"\bvoid\s+testRegularizationRandomNonAxisHistories\s*\(",
        )
        for random_history_token in (
            "kHistoryCount = 24u",
            "std::array<bool, 2>{false, true}",
            "0.125f",
            "7.0f",
            "std::nextafter(",
            "inputPairAngle * 512.0f + 1.0e-6f",
        ):
            self.assertIn(random_history_token, random_history_test)

        # Outside the regularization annulus, a finite selected raw chart reaches
        # the alternate fallback only at exact zero: scale-safe normalization has
        # no empirical magnitude threshold.
        self.assertIn(
            "if (!(largestComponent > 0.0) || !std::isfinite(largestComponent))",
            scale_safe_normalize,
        )
        self.assertIn(
            "if (!(scaledLengthSq > 0.0) || !std::isfinite(scaledLengthSq))",
            scale_safe_normalize,
        )
        self.assertNotRegex(
            scale_safe_normalize,
            r"(?:largestComponent|scaledLengthSq)\s*[<>]=?\s*1(?:\.0*)?e-",
        )

        for forbidden in (
            "std::atan2",
            "glm::mix",
            "smoothstep",
            "chartBlend",
            "ChartBlend",
            "reliability",
            "Reliability",
            "bridge",
            "Bridge",
            "fade",
            "Fade",
            "signedProjectionRoll",
            "appliedRoll",
            "antipodalRollWeight",
            "transportedRight",
            "resolvedUp = -resolvedUp",
            "previousUpProjectionValid",
            "transportedUpValid",
            "transportRotationDefined",
        ):
            self.assertNotIn(forbidden, basis_body)

        boundary_test = braced_source(
            CSM_STABILITY_TEST,
            r"\bvoid\s+testNearAntipodalBoundaryPairIsContinuous\s*\(",
        )
        self.assertIn("-0.999901f", boundary_test)
        self.assertIn("-0.999899f", boundary_test)
        self.assertIn("lowerSide.axes.up, upperSide.axes.up", boundary_test)
        self.assertIn("lowerSide.axes.right, upperSide.axes.right", boundary_test)

        approach_test = braced_source(
            CSM_STABILITY_TEST,
            r"\bvoid\s+testLightBasisContinuouslyApproachesAntipode\s*\(",
        )
        self.assertIn("-1.0f", approach_test)
        self.assertIn("kMaxAdjacentBasisAngleCosine", approach_test)
        self.assertIn("previousAxes.up, sample.axes.up", approach_test)
        self.assertIn("previousAxes.right, sample.axes.right", approach_test)

        sweep_test = braced_source(
            CSM_STABILITY_TEST,
            r"\bvoid\s+testLightBasisContinuousAcrossFormerAntipodalThresholdSweep\s*\(",
        )
        self.assertIn("kSweepSampleCount = 41u", sweep_test)
        self.assertIn("kSweepStep = -1.0e-6f", sweep_test)
        self.assertIn("kMaxSweepStepAngleCosine", sweep_test)
        fixture = braced_source(
            CSM_STABILITY_TEST,
            r"\bevaluateNearAntipodalBasis\s*\(",
        )
        self.assertIn("verifyLightFrameAxes", fixture)

    def test_projection_singularity_float32_and_path_scans_are_pinned(self) -> None:
        fixture = braced_source(
            CSM_STABILITY_TEST,
            r"\bevaluateProjectionSingularityBasis\s*\(",
        )
        self.assertIn("const glm::vec3 initialDirection(0.0f, -1.0f, 0.0f)", fixture)
        self.assertIn("historyUpSign", fixture)
        self.assertIn("requestedDirectionDot", fixture)
        self.assertIn("previousUpProjection", fixture)
        self.assertIn("verifyLightFrameAxes", fixture)

        projection_boundary_test = braced_source(
            CSM_STABILITY_TEST,
            r"\bvoid\s+testProjectionReliabilityBoundaryIsContinuousInFloat32\s*\(",
        )
        self.assertIn("std::nextafter(1.0e-4f, 0.0f)", projection_boundary_test)
        self.assertIn("std::numeric_limits<float>::infinity()", projection_boundary_test)
        self.assertIn("kFormerProjectionLengthSqThreshold = 1.0e-8f", projection_boundary_test)
        self.assertIn("std::array<float, 2>{-1.0f, 1.0f}", projection_boundary_test)
        self.assertIn("realizedProjectedLengthSq <", projection_boundary_test)
        self.assertIn("realizedProjectedLengthSq >", projection_boundary_test)
        self.assertIn("lowerSide.axes.up, upperSide.axes.up", projection_boundary_test)
        self.assertIn("lowerSide.axes.right, upperSide.axes.right", projection_boundary_test)

        direction_dot_test = braced_source(
            CSM_STABILITY_TEST,
            r"\bvoid\s+testDirectionDotZeroCrossingNearProjectionSingularityIsContinuous\s*\(",
        )
        self.assertIn("kDirectionDotEpsilon = 1.0e-7f", direction_dot_test)
        self.assertIn("-kDirectionDotEpsilon", direction_dot_test)
        self.assertIn("kDirectionDotEpsilon, historyUpSign", direction_dot_test)
        self.assertIn("std::array<float, 2>{-1.0f, 1.0f}", direction_dot_test)
        self.assertIn("negativeSide.axes.up, positiveSide.axes.up", direction_dot_test)
        self.assertIn("negativeSide.axes.right, positiveSide.axes.right", direction_dot_test)

        random_path_test = braced_source(
            CSM_STABILITY_TEST,
            r"\bvoid\s+testLightBasisRandomSmallStepSpherePath\s*\(",
        )
        self.assertIn("kRandomStepCount = 2048u", random_path_test)
        self.assertIn("verifyLightFrameAxes", random_path_test)
        self.assertIn("maxLightBasisAngleRadians", random_path_test)
        self.assertIn("kMaximumBasisStepRadians", random_path_test)

        closed_loop_test = braced_source(
            CSM_STABILITY_TEST,
            r"\bvoid\s+testLightBasisClosedGreatCircleScan\s*\(",
        )
        self.assertIn("kLoopStepCount = 1440u", closed_loop_test)
        self.assertIn("verifyLightFrameAxes", closed_loop_test)
        self.assertIn("maxLightBasisAngleRadians", closed_loop_test)
        self.assertIn("kMaximumBasisStepRadians", closed_loop_test)
        self.assertIn(
            "CSM basis continuity max adjacent angles (degrees)",
            CSM_STABILITY_TEST,
        )

    def test_opposed_chart_teleport_regression_and_neighborhood_are_pinned(self) -> None:
        fixture = braced_source(
            CSM_STABILITY_TEST,
            r"\bevaluateChartOverlapTeleport\s*\(",
        )
        self.assertIn("if (primeProjectionChart)", fixture)
        self.assertIn("csm.updateCascadeMatrices(camera, -initialDirection", fixture)
        self.assertIn("csm.updateCascadeMatrices(camera, initialDirection", fixture)
        self.assertIn("transportedLengthSq", fixture)
        self.assertIn("glm::dot(transportedUp, projectedUp)", fixture)
        self.assertIn("glm::dot(realizedDirection, historyAxes.right)", fixture)
        self.assertIn("verifyLightFrameAxes", fixture)

        pair_test = braced_source(
            CSM_STABILITY_TEST,
            r"\bvoid\s+testOpposedChartTeleportPerturbationDoesNotFlipRoll\s*\(",
        )
        self.assertIn("-1.0f + std::sqrt(1.0e-3f)", pair_test)
        self.assertIn("kChartPlaneNormalEpsilon = 1.0e-7f", pair_test)
        self.assertIn("std::array<bool, 2>{false, true}", pair_test)
        self.assertIn("realizedTransportedLengthSq, 1.0e-3f", pair_test)
        self.assertIn("realizedChartAlignment < -0.9999f", pair_test)
        self.assertIn("historyAxes.back, negativeSide.axes.back", pair_test)
        self.assertIn("primeProjectionChart", pair_test)
        self.assertIn(
            "glm::dot(negativeSide.realizedDirection, "
            "positiveSide.realizedDirection) > 0.999999f",
            pair_test,
        )
        self.assertIn("maxLightBasisAngleRadians", pair_test)
        self.assertIn("kMaximumPairAngleRadians", pair_test)
        self.assertIn("pairAngle < kMaximumPairAngleRadians", pair_test)

        scan_test = braced_source(
            CSM_STABILITY_TEST,
            r"\bvoid\s+testOpposedChartLargeTeleportNeighborhoodScan\s*\(",
        )
        self.assertIn("-1.0f + std::sqrt(1.0e-3f)", scan_test)
        self.assertIn("kDirectionDotOffsets", scan_test)
        self.assertIn("kPlaneSampleCount = 129u", scan_test)
        self.assertIn("kPlaneNormalExtent = 2.0e-4f", scan_test)
        self.assertIn("std::array<bool, 2>{false, true}", scan_test)
        self.assertIn("previousAxesInRow", scan_test)
        self.assertIn("previousRowAxes", scan_test)
        self.assertIn("maxLightBasisAngleRadians", scan_test)
        self.assertIn("kMaximumAdjacentAngleRadians", scan_test)
        self.assertIn("teleport_pair=", CSM_STABILITY_TEST)
        self.assertIn("teleport_scan=", CSM_STABILITY_TEST)

        persisted_projection_test = braced_source(
            CSM_STABILITY_TEST,
            r"\bvoid\s+testPersistedProjectionChartCrossesPreviousUpContinuously\s*\(",
        )
        self.assertIn(
            "std::nextafter(1.0e-4f, std::numeric_limits<float>::infinity())",
            persisted_projection_test,
        )
        self.assertIn(
            "std::array<float, 2>{-1.0f, 1.0f}",
            persisted_projection_test,
        )
        self.assertIn("historyUpSign, true", persisted_projection_test)
        self.assertIn(
            "kMaximumBasisPairAngleRadians = 5.0e-4f",
            persisted_projection_test,
        )
        self.assertIn(
            "basisPairAngle < kMaximumBasisPairAngleRadians",
            persisted_projection_test,
        )

        transport_ulp_test = braced_source(
            CSM_STABILITY_TEST,
            r"\bvoid\s+testTransportChartOneUlpTargetMapIsContinuous\s*\(",
        )
        self.assertIn(
            "kTransportTangentCoefficient = -0.000244140625f",
            transport_ulp_test,
        )
        self.assertIn("std::nextafter(", transport_ulp_test)
        self.assertIn(
            "inputPairAngle > 0.0f && inputPairAngle < 5.0e-11f",
            transport_ulp_test,
        )
        self.assertIn(
            "kMaximumBasisPairAngleRadians = 1.0e-7f",
            transport_ulp_test,
        )
        self.assertIn(
            "basisPairAngle < kMaximumBasisPairAngleRadians",
            transport_ulp_test,
        )

        antipodal_neighborhood_test = braced_source(
            CSM_STABILITY_TEST,
            r"\bvoid\s+testAntipodalChartNearbyTargetMapIsContinuous\s*\(",
        )
        self.assertIn("-0.00134547600375f", antipodal_neighborhood_test)
        self.assertIn("-0.00131901197346f", antipodal_neighborhood_test)
        self.assertIn(
            "nearlyEqual(inputPairAngle, 5.0e-5f, 1.0e-7f)",
            antipodal_neighborhood_test,
        )
        self.assertIn(
            "kMaximumBasisPairAngleRadians = 2.0e-4f",
            antipodal_neighborhood_test,
        )
        self.assertIn(
            "basisPairAngle < kMaximumBasisPairAngleRadians",
            antipodal_neighborhood_test,
        )

        exact_poles_test = braced_source(
            CSM_STABILITY_TEST,
            r"\bvoid\s+testComplementaryChartsHandleExactPoles\s*\(",
        )
        self.assertIn(
            "csm.updateCascadeMatrices(camera, -initialDirection",
            exact_poles_test,
        )
        self.assertIn(
            "glm::dot(initialAxes.up, antipodalAxes.up) > 0.99999f",
            exact_poles_test,
        )
        self.assertIn(
            "glm::dot(initialAxes.up, returnedAxes.up) > 0.99999f",
            exact_poles_test,
        )
        self.assertIn(
            "maxLightBasisAngleRadians(returnedAxes, settledAxes) < 1.0e-6f",
            exact_poles_test,
        )

    def test_cascade_xy_culling_is_receiver_projection_driven(self) -> None:
        update_body = braced_source(
            CSM_SOURCE,
            r"\bvoid\s+CSMShadowResources::updateCascadeMatrices\s*\("
            r"\s*const\s+shaderio::CameraUniforms&\s+camera\s*,"
            r"\s*const\s+glm::vec3&\s+lightDir\s*,"
            r"\s*float\s+requestedMaxShadowDistance",
        )
        culling_start = update_body.index("const float cullingGuard")
        culling_end = update_body.index("// near/far use", culling_start)
        culling_region = update_body[culling_start:culling_end]

        for expected in (
            "const float cullLeft = left - cullingGuard;",
            "const float cullRight = right + cullingGuard;",
            "const float cullBottom = bottom - cullingGuard;",
            "const float cullTop = top + cullingGuard;",
        ):
            self.assertIn(expected, culling_region)

        self.assertNotIn("casterMinLightSpace.x", culling_region)
        self.assertNotIn("casterMaxLightSpace.x", culling_region)
        self.assertNotIn("casterMinLightSpace.y", culling_region)
        self.assertNotIn("casterMaxLightSpace.y", culling_region)
        self.assertNotIn("casterDepthRange", culling_region)
        self.assertIn("stableCasterMaxZ - stableCasterMinZ", update_body)
        self.assertIn("? stableCasterMaxZ", update_body)
        self.assertIn("normalBiasWorld", culling_region)
        self.assertIn("cascadeData.cullingGuardWorld = cullingGuard;", update_body)
        self.assertIn("float cullingGuardWorld{0.0f};", CSM_HEADER)
        self.assertIn("float normalBiasWorld{0.0f};", CSM_HEADER)


    def test_actual_normal_bias_reaches_cascade_fit_callers(self) -> None:
        update_body = braced_source(
            CSM_SOURCE,
            r"\bvoid\s+CSMShadowResources::updateCascadeMatrices\s*\("
            r"\s*const\s+shaderio::CameraUniforms&\s+camera\s*,"
            r"\s*const\s+glm::vec3&\s+lightDir\s*,"
            r"\s*float\s+requestedMaxShadowDistance",
        )
        draw_frame = braced_source(
            RENDER_DEVICE,
            r"\bvoid\s+RenderDevice::drawFrame\s*\(",
        )
        gpu_render = braced_source(
            GPU_RENDERER,
            r"\bvoid\s+GPUDrivenRenderer::render\s*\(",
        )
        csm_pass_execute = braced_source(
            CSM_PASS,
            r"\bvoid\s+GPUDrivenCSMShadowPass::execute\s*\(",
        )

        self.assertIn("float requestedNormalBiasWorld", CSM_SOURCE)
        self.assertIn("std::isfinite(requestedNormalBiasWorld)", update_body)
        self.assertIn("std::max(requestedNormalBiasWorld, 0.0f)", update_body)
        self.assertIn("m_frameData.normalBiasWorld = normalBiasWorld;", update_body)
        self.assertIn(
            "const float casterNormalBias = csmFrameData.normalBiasWorld;",
            csm_pass_execute,
        )
        self.assertNotIn(
            "context.params->lightSettings.normalBias",
            csm_pass_execute,
        )
        normal_bias_test = braced_source(
            CSM_STABILITY_TEST,
            r"\bvoid\s+testNormalBiasSanitization\s*\(",
        )
        self.assertIn("quiet_NaN()", normal_bias_test)
        self.assertIn("infinity()", normal_bias_test)
        self.assertIn("{-3.0f, 0.0f}", normal_bias_test)
        self.assertIn("{2.0f, 2.0f}", normal_bias_test)
        self.assertIn("testNormalBiasSanitization();", CSM_STABILITY_TEST)
        for caller in (draw_frame, gpu_render):
            self.assertRegex(
                caller,
                r"sceneBounds(?:\.valid|Valid)\s*,\s*"
                r"params\.lightSettings\.normalBias\s*\)",
            )

    def test_runtime_caster_updates_shadow_packed_gpu_and_cpu_sources(self) -> None:
        helper = braced_source(
            RENDER_TYPES,
            r"\bupdateShadowPackedMeshRuntimeState\s*\(",
        )
        scene_rebuild = braced_source(
            RENDER_DEVICE,
            r"\bvoid\s+RenderDevice::rebuildShadowPackedBuffers\s*\("
            r"\s*const\s+SceneAsset&",
        )
        mesh_update = braced_source(
            GPU_RENDERER,
            r"\bvoid\s+GPUDrivenRenderer::updateMeshTransform\s*\(",
        )
        instance_update = braced_source(
            GPU_RENDERER,
            r"\bvoid\s+GPUDrivenRenderer::updateSceneInstanceTransform\s*\(",
        )
        runtime_sync = braced_source(
            GPU_RENDERER,
            r"\bvoid\s+GPUDrivenRenderer::syncActiveSceneRuntimeState\s*\(",
        )
        apply_graph = braced_source(
            APP_HEADER,
            r"\binline\s+void\s+MinimalLatestApp::applySceneGraphTransforms\s*\(",
        )
        shadow_upload = braced_source(
            RENDER_DEVICE,
            r"\bvoid\s+RenderDevice::updateShadowCullingBuffers\s*\(",
        )

        self.assertIn("uint32_t drawRecordIndex{UINT32_MAX};", RENDER_TYPES)
        self.assertIn("packedMesh.drawData.prevModelMatrix", helper)
        self.assertIn("packedMesh.drawData.modelMatrix = worldTransform;", helper)
        self.assertIn("packedMesh.boundsSphere = boundsSphere;", helper)
        self.assertIn(".drawRecordIndex = drawRecordIndex", scene_rebuild)
        self.assertIn("m_activeUploadResultStorage.shadowPackedMeshes", mesh_update)
        self.assertIn("updateShadowPackedMeshRuntimeState", mesh_update)
        self.assertIn("computeTransformedBoundsSphere(*meshRecord, transform)", instance_update)
        self.assertIn("packedMesh.drawRecordIndex == drawRecordIndex", instance_update)
        self.assertIn("updateShadowPackedMeshRuntimeState", instance_update)
        self.assertIn("ioResult.drawRecords = m_activeUploadResultStorage.drawRecords;", runtime_sync)
        self.assertIn(
            "ioResult.shadowPackedMeshes = m_activeUploadResultStorage.shadowPackedMeshes;",
            runtime_sync,
        )
        self.assertIn("syncActiveSceneRuntimeState", GPU_HEADER)
        self.assertEqual(apply_graph.count("syncActiveSceneRuntimeState"), 2)
        self.assertIn("packedMesh.boundsSphere", shadow_upload)
        self.assertIn("packedMesh.drawData", shadow_upload)
        self.assertIn("current shadow frame-slot", runtime_sync)
    def test_depth_write_dependency_uses_raster_depth_stage(self) -> None:
        dependencies = braced_source(
            CSM_PASS,
            r"\bGPUDrivenCSMShadowPass::getDependencies\s*\(",
        )

        self.assertIn("rhi::StageFlags::rasterDepthOut", dependencies)
        self.assertIn("rhi::HazardFlags::depthStencil", dependencies)
        self.assertIn("rhi::ResourceState::DepthStencilAttachment", dependencies)
        self.assertNotIn("rhi::ShaderStage::fragment", dependencies)


if __name__ == "__main__":
    unittest.main()
