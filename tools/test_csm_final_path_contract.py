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
        self.assertIn("float2 shadowUv = demoNdcToFramebufferUv(shadowNdc.xy);", projection)
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
