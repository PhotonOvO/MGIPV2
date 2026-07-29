import re
import unittest
from pathlib import Path


REPO_ROOT = Path(__file__).resolve().parents[1]
APP_HEADER = (REPO_ROOT / "app" / "MinimalLatestApp.h").read_text(encoding="utf-8")


def braced_source(source: str, pattern: str) -> str:
    match = re.search(pattern, source)
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


class CameraResizeUniformContractTests(unittest.TestCase):
    def test_resize_rebuilds_uniforms_before_the_frame_uses_them(self) -> None:
        run_body = braced_source(APP_HEADER, r"\bvoid\s+run\s*\(\s*\)")
        resize_block = braced_source(
            run_body,
            r"\bif\s*\(\s*requestedViewportSize\.width\s*>\s*0",
        )

        perspective = resize_block.index("m_camera.setPerspective(")
        refresh = resize_block.index("refreshActiveCameraUniforms();", perspective)
        bind_uniforms = run_body.index(
            "frameParams.cameraUniforms = &m_cameraUniforms;"
        )
        render = run_body.index("m_renderer.render(frameParams);")
        resize_in_run = run_body.index("m_camera.setPerspective(")
        refresh_in_run = run_body.index("refreshActiveCameraUniforms();", resize_in_run)

        self.assertLess(perspective, refresh)
        self.assertLess(resize_in_run, refresh_in_run)
        self.assertLess(refresh_in_run, bind_uniforms)
        self.assertLess(bind_uniforms, render)

    def test_resize_refresh_does_not_advance_automation_history(self) -> None:
        run_body = braced_source(APP_HEADER, r"\bvoid\s+run\s*\(\s*\)")
        resize_block = braced_source(
            run_body,
            r"\bif\s*\(\s*requestedViewportSize\.width\s*>\s*0",
        )
        update_body = braced_source(
            APP_HEADER,
            r"inline\s+void\s+MinimalLatestApp::updateActiveCamera\s*\(",
        )
        refresh_body = braced_source(
            APP_HEADER,
            r"inline\s+void\s+MinimalLatestApp::refreshActiveCameraUniforms\s*\(",
        )
        rendered_body = braced_source(
            APP_HEADER,
            r"inline\s+void\s+MinimalLatestApp::onAutomationFrameRendered\s*\(",
        )

        self.assertNotIn("applyAutomationCameraPose", resize_block)
        self.assertNotIn("onAutomationFrameRendered", resize_block)
        self.assertNotIn("m_automationCurrentPose =", resize_block)
        self.assertNotIn("m_automationPreviousPose =", resize_block)
        self.assertNotIn("m_automationFrame", update_body)
        self.assertNotIn("m_automationPreviousPose =", update_body)
        self.assertIn("refreshActiveCameraUniforms();", update_body)
        self.assertNotIn("m_camera.update()", refresh_body)
        self.assertNotIn("m_sceneCameraNavigation.update()", refresh_body)
        self.assertNotIn("applyAutomationCameraPose", refresh_body)
        self.assertNotIn("onAutomationFrameRendered", refresh_body)
        self.assertNotIn("m_automationPreviousPose =", refresh_body)

        wait = run_body.index("waitForAutomationCaptureHandshake();")
        render = run_body.index("m_renderer.render(frameParams);")
        advance = run_body.index("onAutomationFrameRendered();")
        self.assertLess(wait, render)
        self.assertLess(render, advance)
        self.assertIn(
            "m_automationPreviousPose = m_automationCurrentPose;",
            rendered_body,
        )

    def test_scene_graph_edits_refresh_the_active_camera_before_render_params(
        self,
    ) -> None:
        run_body = braced_source(APP_HEADER, r"\bvoid\s+run\s*\(\s*\)")
        apply_body = braced_source(
            APP_HEADER,
            r"inline\s+void\s+MinimalLatestApp::applySceneGraphTransforms\s*\(",
        )
        refresh_body = braced_source(
            APP_HEADER,
            r"inline\s+void\s+MinimalLatestApp::refreshActiveCameraUniforms\s*\(",
        )

        draw_scene_graph = run_body.index("drawSceneGraphUI();")
        bind_uniforms = run_body.index(
            "frameParams.cameraUniforms = &m_cameraUniforms;"
        )
        self.assertLess(draw_scene_graph, bind_uniforms)

        scene_asset_update = apply_body.index("m_sceneAssetView =")
        scene_asset_refresh = apply_body.index(
            "refreshActiveCameraUniforms();", scene_asset_update
        )
        legacy_update = apply_body.rindex(
            "updateSceneNodeWorldTransform(rootNodeIndex"
        )
        legacy_refresh = apply_body.index(
            "refreshActiveCameraUniforms();", legacy_update
        )
        self.assertLess(scene_asset_update, scene_asset_refresh)
        self.assertLess(legacy_update, legacy_refresh)
        self.assertEqual(2, apply_body.count("refreshActiveCameraUniforms();"))

        self.assertNotIn("m_camera.update()", refresh_body)
        self.assertNotIn("m_sceneCameraNavigation.update()", refresh_body)
        self.assertNotIn("m_automationPreviousPose =", refresh_body)
        self.assertNotIn("onAutomationFrameRendered", refresh_body)

    def test_refreshed_uniforms_use_the_current_projection_for_all_camera_modes(
        self,
    ) -> None:
        refresh_body = braced_source(
            APP_HEADER,
            r"inline\s+void\s+MinimalLatestApp::refreshActiveCameraUniforms\s*\(",
        )
        scene_camera_body = braced_source(
            APP_HEADER,
            r"inline\s+bool\s+MinimalLatestApp::populateActiveSceneCameraUniforms\s*\(",
        )

        self.assertIn("m_camera.getProjectionMatrix()", refresh_body)
        self.assertIn("m_camera.getProjectionMatrix()", scene_camera_body)
        self.assertIn(
            "frameParams.cameraUniforms = &m_cameraUniforms;",
            APP_HEADER,
        )


if __name__ == "__main__":
    unittest.main()
