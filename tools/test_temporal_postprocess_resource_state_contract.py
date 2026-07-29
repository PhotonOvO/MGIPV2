from __future__ import annotations

import re
import unittest
from pathlib import Path


ROOT = Path(__file__).resolve().parents[1]
LIGHT_PASS = (ROOT / "render/passes/GPUDrivenLightPass.cpp").read_text(encoding="utf-8")
TAA_PASS = (ROOT / "render/passes/GPUDrivenTAAResolvePass.cpp").read_text(encoding="utf-8")
BLOOM_PASS = (ROOT / "render/passes/GPUDrivenBloomPrefilterPass.cpp").read_text(encoding="utf-8")
BLOOM_DOWNSAMPLE_PASS = (
    ROOT / "render/passes/GPUDrivenBloomDownsamplePass.cpp"
).read_text(encoding="utf-8")
FINAL_PASS = (ROOT / "render/passes/GPUDrivenFinalColorPass.cpp").read_text(encoding="utf-8")
RENDERER_CPP = (ROOT / "render/GPUDrivenRenderer.cpp").read_text(encoding="utf-8")
RENDERER_H = (ROOT / "render/GPUDrivenRenderer.h").read_text(encoding="utf-8")
RENDER_DEVICE_CPP = (ROOT / "render/RenderDevice.cpp").read_text(encoding="utf-8")
RENDER_DEVICE_H = (ROOT / "render/RenderDevice.h").read_text(encoding="utf-8")
ANDROID_APP = (ROOT / "app/AndroidNativeApp.cpp").read_text(encoding="utf-8")


def dependency_has_state(source: str, handle: str, state: str) -> bool:
    pattern = re.compile(
        rf"PassResourceDependency::texture\(\s*{re.escape(handle)}\s*,"
        rf"\s*ResourceAccess::read\s*,\s*rhi::ShaderStage::fragment\s*,"
        rf"\s*rhi::ResourceState::{re.escape(state)}\s*\)",
        re.DOTALL,
    )
    return pattern.search(source) is not None


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


class TemporalPostProcessResourceStateContractTests(unittest.TestCase):
    def test_static_persistent_textures_bind_their_physical_terminal_state(self) -> None:
        self.assertIn("struct StaticPassTextureStates", RENDER_DEVICE_H)
        self.assertIn(
            "std::array<rhi::ResourceState, 3> gbuffer", RENDER_DEVICE_H
        )
        static_bind = function_source(
            RENDER_DEVICE_CPP, "void RenderDevice::bindStaticPassResources"
        )
        for index in range(3):
            self.assertRegex(
                static_bind,
                re.compile(
                    rf"kPassGBuffer{index}Handle.*?"
                    rf"\.initialState = textureStates\.gbuffer\[{index}\]",
                    re.DOTALL,
                ),
            )
        self.assertRegex(
            static_bind,
            re.compile(
                r"kPassSceneColorHdrHandle.*?"
                r"\.initialState = textureStates\.sceneColorHdr",
                re.DOTALL,
            ),
        )
        self.assertRegex(
            static_bind,
            re.compile(
                r"kPassVelocityHandle.*?\.initialState = textureStates\.velocity",
                re.DOTALL,
            ),
        )

        renderer_bind = function_source(
            RENDERER_CPP, "void GPUDrivenRenderer::bindStaticPassResources"
        )
        self.assertIn("trackPersistentTextureIdentity(", renderer_bind)
        self.assertIn(
            "textureStates.gbuffer[gbufferIndex] = "
            "m_gbufferResourceStates[gbufferIndex].state",
            renderer_bind,
        )
        self.assertIn(
            "textureStates.sceneColorHdr = m_sceneColorHdrResourceState.state",
            renderer_bind,
        )
        self.assertIn(
            "textureStates.velocity = m_velocityResourceState.state",
            renderer_bind,
        )
        self.assertIn(
            "m_renderer.bindStaticPassResources(m_passExecutor, textureStates)",
            renderer_bind,
        )

    def test_physical_identity_change_resets_to_scene_resource_creation_layout(self) -> None:
        track = function_source(
            RENDERER_CPP, "void GPUDrivenRenderer::trackPersistentTextureIdentity"
        )
        self.assertIn("textureState.texture != texture", track)
        self.assertIn("textureState.texture = texture;", track)
        self.assertIn(
            "textureState.state = rhi::ResourceState::General;", track
        )

        commit = function_source(
            RENDERER_CPP, "void GPUDrivenRenderer::commitPersistentTextureState"
        )
        self.assertIn("textureState.texture == texture", commit)
        self.assertIn("textureState.state = terminalState;", commit)

    def test_submitted_graph_commits_terminal_states_after_noop_return(self) -> None:
        render = function_source(RENDERER_CPP, "void GPUDrivenRenderer::render(")
        failed_frame = render.index("if (!frameSubmitted)")
        failed_return = render.index("return;", failed_frame)
        first_commit = render.index("commitPersistentTextureState(", failed_return)
        self.assertLess(failed_return, first_commit)
        self.assertIn("preparedGBufferImages", render)
        self.assertIn("preparedSceneColorHdrImage", render)
        self.assertIn("preparedVelocityImage", render)
        self.assertGreaterEqual(
            render.count("rhi::ResourceState::ShaderRead", first_commit), 3
        )
        self.assertIn(
            "passes that early-out for scene suspension or disabled TAA/post options",
            render,
        )

    def test_lifecycle_and_recreation_reset_all_temporal_resource_state(self) -> None:
        reset = function_source(
            RENDERER_CPP,
            "void GPUDrivenRenderer::resetTemporalAndPersistentTextureStates",
        )
        for reset_statement in (
            "m_taaHistoryValid = false;",
            "m_taaHistoryWrittenThisFrame = false;",
            "m_taaHistoryWriteIndex = 0u;",
            "m_sceneColorHistoryStates.fill(rhi::ResourceState::General);",
            "m_bloomResourceStates.fill(rhi::ResourceState::General);",
            "m_gbufferResourceStates.fill(PersistentTextureState{});",
            "m_sceneColorHdrResourceState = {};",
            "m_velocityResourceState = {};",
        ):
            self.assertIn(reset_statement, reset)

        init = function_source(RENDERER_CPP, "void GPUDrivenRenderer::init(")
        self.assertLess(
            init.index("resetTemporalAndPersistentTextureStates();"),
            init.index("bindStaticPassResources();"),
        )

        resize = function_source(RENDERER_CPP, "void GPUDrivenRenderer::resize(")
        self.assertLess(
            resize.index("resetTemporalAndPersistentTextureStates();"),
            resize.index("bindStaticPassResources();"),
        )

        shutdown = function_source(RENDERER_CPP, "void GPUDrivenRenderer::shutdown(")
        self.assertIn("m_passExecutor.clearResourceBindings();", shutdown)
        self.assertIn("m_passExecutor.setResourceTable(nullptr);", shutdown)
        self.assertIn("resetTemporalAndPersistentTextureStates();", shutdown)

        android_lifecycle = function_source(ANDROID_APP, "void handleCommand(")
        self.assertRegex(
            android_lifecycle,
            re.compile(r"APP_CMD_INIT_WINDOW.*?demoApp->init", re.DOTALL),
        )
        self.assertRegex(
            android_lifecycle,
            re.compile(r"APP_CMD_TERM_WINDOW.*?demoApp->shutdown", re.DOTALL),
        )

    def test_sampled_pass_dependencies_explicitly_require_shader_read(self) -> None:
        for handle in ("kPassGBuffer0Handle", "kPassGBuffer1Handle", "kPassGBuffer2Handle"):
            self.assertTrue(dependency_has_state(LIGHT_PASS, handle, "ShaderRead"), handle)

        for handle in (
            "kPassSceneColorHdrHandle",
            "kPassVelocityHandle",
            "kPassSceneColorHistoryReadHandle",
        ):
            self.assertTrue(dependency_has_state(TAA_PASS, handle, "ShaderRead"), handle)

        for handle in ("kPassSceneColorHdrHandle", "kPassSceneColorHistoryWriteHandle"):
            self.assertTrue(dependency_has_state(BLOOM_PASS, handle, "ShaderRead"), handle)

        for handle in (
            "kPassSceneColorHdrHandle",
            "kPassSceneColorHistoryWriteHandle",
            "kPassBloomOutputHandle",
            "kPassBloomHalfHandle",
            "kPassBloomQuarterHandle",
            "kPassVelocityHandle",
        ):
            self.assertTrue(dependency_has_state(FINAL_PASS, handle, "ShaderRead"), handle)

    def test_taa_history_write_is_published_to_current_frame_consumers(self) -> None:
        self.assertRegex(
            TAA_PASS,
            re.compile(
                r"kPassSceneColorHistoryWriteHandle\s*,\s*ResourceAccess::write.*?"
                r"rhi::ResourceState::ColorAttachment",
                re.DOTALL,
            ),
        )
        self.assertTrue(
            dependency_has_state(BLOOM_PASS, "kPassSceneColorHistoryWriteHandle", "ShaderRead")
        )
        self.assertTrue(
            dependency_has_state(FINAL_PASS, "kPassSceneColorHistoryWriteHandle", "ShaderRead")
        )

    def test_history_layout_is_tracked_by_physical_parity_across_frames(self) -> None:
        self.assertIn("std::array<rhi::ResourceState, 2> m_sceneColorHistoryStates", RENDERER_H)
        self.assertIn(
            ".initialState = m_sceneColorHistoryStates[historyReadIndex]", RENDERER_CPP
        )
        self.assertIn(
            ".initialState = m_sceneColorHistoryStates[historyWriteIndex]", RENDERER_CPP
        )
        self.assertIn(
            "m_sceneColorHistoryStates[preparedHistoryReadIndex] = "
            "rhi::ResourceState::ShaderRead",
            RENDERER_CPP,
        )
        self.assertIn(
            "m_sceneColorHistoryStates[preparedHistoryWriteIndex] = "
            "rhi::ResourceState::ShaderRead",
            RENDERER_CPP,
        )
        self.assertIn(
            "m_sceneColorHistoryStates.fill(rhi::ResourceState::General)", RENDERER_CPP
        )

    def test_taa_history_commit_requires_an_actual_resolve_draw(self) -> None:
        readiness = TAA_PASS.index("const bool taaWritesHistoryThisFrame")
        begin_event = TAA_PASS.index('beginEvent("GPUDrivenTAAResolve")')
        draw = TAA_PASS.index("enc->draw(", readiness)
        publish = TAA_PASS.index("markTAAResolveHistoryWrittenThisFrame()", draw)
        self.assertLess(readiness, begin_event)
        self.assertLess(draw, publish)
        self.assertIn("getTAAResolveExecutionPipelineHandle()", TAA_PASS)
        self.assertIn(
            "return m_taaHistoryWrittenThisFrame ? m_gpuDrivenTAAResolvePipeline : PipelineHandle{}",
            RENDERER_H,
        )
        self.assertIn("getTAAResolvePipelineHandle().isNull()", FINAL_PASS)
        self.assertIn("m_taaHistoryWrittenThisFrame = false;", RENDERER_CPP)
        self.assertRegex(
            RENDERER_CPP,
            re.compile(
                r"if \(m_taaHistoryWrittenThisFrame\)\s*\{\s*"
                r"m_taaHistoryValid = true;\s*m_taaHistoryWriteIndex \^= 1u;\s*"
                r"\}\s*else\s*\{\s*m_taaHistoryValid = false;",
                re.DOTALL,
            ),
        )

    def test_taa_history_parity_advances_only_on_a_successful_write(self) -> None:
        self.assertIn("uint32_t m_taaHistoryWriteIndex{0u}", RENDERER_H)
        self.assertIn(
            "const uint32_t historyWriteIndex = m_taaHistoryWriteIndex", RENDERER_CPP
        )
        self.assertIn("const uint32_t historyReadIndex = historyWriteIndex ^ 1u", RENDERER_CPP)
        self.assertNotRegex(
            RENDERER_CPP,
            re.compile(r"historyWriteIndex\s*=.*m_temporalFrameCounter"),
        )

    def test_bloom_private_chain_uses_shader_read_at_every_sample_boundary(self) -> None:
        for handle in (
            "kPassBloomHalfHandle",
            "kPassBloomQuarterHandle",
            "kPassBloomEighthHandle",
            "kPassBloomSixteenthHandle",
            "kPassBloomThirtySecondHandle",
            "kPassBloomUpsampleSixteenthHandle",
            "kPassBloomUpsampleEighthHandle",
            "kPassBloomUpsampleQuarterHandle",
            "kPassBloomOutputHandle",
        ):
            self.assertTrue(
                dependency_has_state(BLOOM_DOWNSAMPLE_PASS, handle, "ShaderRead"), handle
            )

        self.assertIn(
            "transitionStep(step, rhi::ResourceState::ShaderRead, "
            "rhi::ResourceState::ColorAttachment)",
            BLOOM_DOWNSAMPLE_PASS,
        )
        self.assertIn(
            "transitionStep(step, rhi::ResourceState::ColorAttachment, "
            "rhi::ResourceState::ShaderRead)",
            BLOOM_DOWNSAMPLE_PASS,
        )
        self.assertNotIn(
            "transitionStep(step, rhi::ResourceState::ColorAttachment, "
            "rhi::ResourceState::General)",
            BLOOM_DOWNSAMPLE_PASS,
        )
        self.assertIn("std::array<rhi::ResourceState, 9> m_bloomResourceStates", RENDERER_H)
        self.assertIn(".initialState = m_bloomResourceStates[bloomIndex]", RENDERER_CPP)
        self.assertIn(
            "m_bloomResourceStates[bloomIndex] = rhi::ResourceState::ShaderRead",
            RENDERER_CPP,
        )

    def test_noop_post_passes_do_not_request_general_for_scene_color(self) -> None:
        for source in (TAA_PASS, BLOOM_PASS):
            self.assertNotRegex(
                source,
                re.compile(
                    r"kPassSceneColorHdrHandle\s*,\s*ResourceAccess::read\s*,"
                    r"\s*rhi::ShaderStage::fragment\s*\)",
                    re.DOTALL,
                ),
            )
            self.assertTrue(
                dependency_has_state(source, "kPassSceneColorHdrHandle", "ShaderRead")
            )


if __name__ == "__main__":
    unittest.main()
