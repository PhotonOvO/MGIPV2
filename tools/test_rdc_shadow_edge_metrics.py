import gc
import json
import math
import unittest
from unittest import mock

import numpy as np

from tools.rdc_shadow_edge_metrics import (
    CameraMatrices,
    ShadowEdgeMetricsConfig,
    ShadowFrame,
    STATUS_FAIL,
    STATUS_INCONCLUSIVE,
    STATUS_PASS,
    _difference_quantile,
    _edge_distance_p95,
    evaluate_shadow_edge_metrics,
)


TOP_DOWN_CLIP_FROM_WORLD = np.asarray(
    (
        (1.0, 0.0, 0.0, 0.0),
        (0.0, 0.0, 1.0, 0.0),
        (0.0, 1.0, 0.0, 0.0),
        (0.0, 0.0, 0.0, 1.0),
    ),
    dtype=np.float64,
)
MGIF_Y_CONVENTION = "top_to_negative_one"


def camera(
    shape: tuple[int, int],
    *,
    horizontal_motion_px: float = 0.0,
    vertical_motion_px: float = 0.0,
    pitch_radians: float = 0.0,
    y_convention: str = MGIF_Y_CONVENTION,
) -> CameraMatrices:
    height, width = shape
    sine = math.sin(pitch_radians)
    cosine = math.cos(pitch_radians)
    clip_from_world = TOP_DOWN_CLIP_FROM_WORLD.copy()
    clip_from_world[1] = (0.0, -sine, cosine, 0.0)
    clip_from_world[2] = (0.0, cosine, sine, 0.0)
    clip_from_world[0, 3] = -2.0 * horizontal_motion_px / width
    y_motion_sign = (
        -1.0 if y_convention == MGIF_Y_CONVENTION else 1.0
    )
    clip_from_world[1, 3] = (
        y_motion_sign * 2.0 * vertical_motion_px / height
    )
    view_from_world = clip_from_world.copy()
    return CameraMatrices(
        clip_from_world=clip_from_world,
        framebuffer_y_to_ndc=y_convention,
        world_from_clip=np.linalg.inv(clip_from_world),
        view_from_world=view_from_world,
        world_from_view=np.linalg.inv(view_from_world),
        viewport=(0, 0, width, height),
    )


def reverse_z_camera(shape: tuple[int, int], near: float = 0.1) -> CameraMatrices:
    height, width = shape
    focal_y = 1.0 / math.tan(math.radians(60.0) * 0.5)
    focal_x = focal_y * height / width
    clip_from_world = np.asarray(
        (
            (focal_x, 0.0, 0.0, 0.0),
            (0.0, focal_y, 0.0, 0.0),
            (0.0, 0.0, 0.0, near),
            (0.0, 0.0, 1.0, 0.0),
        ),
        dtype=np.float64,
    )
    return CameraMatrices(
        clip_from_world=clip_from_world,
        framebuffer_y_to_ndc=MGIF_Y_CONVENTION,
        world_from_clip=np.linalg.inv(clip_from_world),
        view_from_world=np.eye(4, dtype=np.float64),
        world_from_view=np.eye(4, dtype=np.float64),
        viewport=(0, 0, width, height),
    )


def soft_step(
    shape: tuple[int, int],
    edge_x: float,
    *,
    softness_px: float = 2.5,
    darkness: float = 0.45,
) -> np.ndarray:
    x = np.arange(shape[1], dtype=np.float32)[None, :]
    exponent = np.clip(
        -(x - float(edge_x)) / float(softness_px),
        -80.0,
        80.0,
    )
    transition = 1.0 / (1.0 + np.exp(exponent))
    return np.broadcast_to(
        1.0 - darkness * transition,
        shape,
    ).astype(np.float32, copy=True)


def soft_horizontal_step(
    shape: tuple[int, int],
    edge_y: float,
    *,
    softness_px: float = 2.5,
    darkness: float = 0.45,
) -> np.ndarray:
    y = np.arange(shape[0], dtype=np.float32)[:, None]
    exponent = np.clip(
        -(y - float(edge_y)) / float(softness_px),
        -80.0,
        80.0,
    )
    transition = 1.0 / (1.0 + np.exp(exponent))
    return np.broadcast_to(
        1.0 - darkness * transition,
        shape,
    ).astype(np.float32, copy=True)


def soft_circle(
    shape: tuple[int, int],
    center_x: float,
    center_y: float,
    *,
    radius_px: float = 10.0,
    softness_px: float = 1.75,
    darkness: float = 0.5,
) -> np.ndarray:
    y, x = np.ogrid[: shape[0], : shape[1]]
    radius = np.hypot(x - center_x, y - center_y)
    interior = 1.0 / (
        1.0 + np.exp((radius - radius_px) / softness_px)
    )
    return (1.0 - darkness * interior).astype(np.float32)


def pitched_plane_buffers(
    shape: tuple[int, int],
    pitch_radians: float,
    shadow_world_z: float,
    *,
    plane_world_y: float = 0.5,
) -> tuple[np.ndarray, np.ndarray]:
    height, width = shape
    del width
    sine = math.sin(pitch_radians)
    cosine = math.cos(pitch_radians)
    y = np.arange(height, dtype=np.float32)[:, None]
    ndc_y = 2.0 * (y + 0.5) / height - 1.0
    world_z = (ndc_y + sine * plane_world_y) / cosine
    depth = sine * world_z + cosine * plane_world_y
    softness_world = 5.0 * 2.0 / height
    exponent = np.clip(
        -(world_z - shadow_world_z) / softness_world,
        -80.0,
        80.0,
    )
    lighting = 1.0 - 0.45 / (1.0 + np.exp(exponent))
    return (
        np.broadcast_to(depth, shape).astype(np.float32, copy=True),
        np.broadcast_to(lighting, shape).astype(np.float32, copy=True),
    )


def frame(
    shape: tuple[int, int],
    lighting,
    *,
    depth: np.ndarray | None = None,
    base_color=0.72,
    normal: np.ndarray | tuple[float, float, float] = (0.0, 1.0, 0.0),
    horizontal_camera_motion_px: float = 0.0,
    vertical_camera_motion_px: float = 0.0,
    history=None,
    final=None,
    camera_override: CameraMatrices | None = None,
) -> ShadowFrame:
    if depth is None:
        depth = np.full(shape, 0.5, dtype=np.float32)
    return ShadowFrame(
        scene_depth=depth,
        world_normals=np.asarray(normal, dtype=np.float32),
        base_color=base_color,
        scene_color_hdr=lighting,
        history_write=lighting if history is None else history,
        final=lighting if final is None else final,
        camera=(
            camera_override
            if camera_override is not None
            else camera(
                shape,
                horizontal_motion_px=horizontal_camera_motion_px,
                vertical_motion_px=vertical_camera_motion_px,
            )
        ),
    )

def assert_failed_stage(
    test: unittest.TestCase,
    result,
    stage_name: str,
) -> None:
    test.assertEqual(STATUS_FAIL, result.status, result.to_dict())
    test.assertEqual(
        STATUS_FAIL,
        result.stages[stage_name].status,
        result.to_dict(),
    )


class ShadowEdgeMetricsTests(unittest.TestCase):
    def test_low_occupancy_64px_soft_edge_moving_one_pixel_fails(self) -> None:
        shape = (160, 224)
        before_lighting = soft_circle(shape, 104.0, 80.0)
        after_lighting = soft_circle(shape, 105.0, 80.0)

        result = evaluate_shadow_edge_metrics(
            frame(shape, before_lighting),
            frame(shape, after_lighting),
        )

        assert_failed_stage(self, result, "scene_color_hdr")
        metric = result.stages["scene_color_hdr"]
        self.assertGreaterEqual(metric.edge_pixels_before, 48)
        self.assertGreater(metric.edge_chamfer_p95_px, 0.25)

    def test_camera_motion_with_world_static_shadow_reprojects_and_passes(self) -> None:
        shape = (192, 320)
        camera_motion_px = 3.0
        before_lighting = soft_step(shape, 180.0)
        after_lighting = soft_step(shape, 180.0 - camera_motion_px)

        result = evaluate_shadow_edge_metrics(
            frame(shape, before_lighting),
            frame(
                shape,
                after_lighting,
                horizontal_camera_motion_px=camera_motion_px,
            ),
        )

        self.assertEqual(STATUS_PASS, result.status, result.to_dict())
        self.assertGreaterEqual(
            result.geometry["valid_reprojection_ratio"], 0.95
        )
        self.assertLessEqual(
            result.stages["scene_color_hdr"].edge_chamfer_p95_px,
            0.25,
        )

    def test_vertical_camera_motion_respects_explicit_y_convention(self) -> None:
        shape = (192, 320)
        motion_px = 4.0
        before_lighting = soft_horizontal_step(shape, 104.0)
        after_lighting = soft_horizontal_step(shape, 104.0 - motion_px)
        for convention in (
            "top_to_negative_one",
            "top_to_positive_one",
        ):
            with self.subTest(convention=convention):
                result = evaluate_shadow_edge_metrics(
                    frame(
                        shape,
                        before_lighting,
                        camera_override=camera(
                            shape, y_convention=convention
                        ),
                    ),
                    frame(
                        shape,
                        after_lighting,
                        camera_override=camera(
                            shape,
                            vertical_motion_px=motion_px,
                            y_convention=convention,
                        ),
                    ),
                )
                self.assertEqual(STATUS_PASS, result.status, result.to_dict())
                self.assertEqual(
                    convention,
                    result.geometry["framebuffer_y_to_ndc_before"],
                )

    def test_camera_pitch_reprojects_static_horizontal_shadow(self) -> None:
        shape = (200, 320)
        shadow_world_z = 0.12
        pitch = math.radians(5.0)
        before_depth, before_lighting = pitched_plane_buffers(
            shape, 0.0, shadow_world_z
        )
        after_depth, after_lighting = pitched_plane_buffers(
            shape, pitch, shadow_world_z
        )

        result = evaluate_shadow_edge_metrics(
            frame(
                shape,
                before_lighting,
                depth=before_depth,
                camera_override=camera(shape),
            ),
            frame(
                shape,
                after_lighting,
                depth=after_depth,
                camera_override=camera(shape, pitch_radians=pitch),
            ),
        )

        self.assertEqual(STATUS_PASS, result.status, result.to_dict())
        self.assertGreaterEqual(
            result.geometry["valid_reprojection_ratio"], 0.95
        )

    def test_reverse_z_50m_90m_occlusion_is_rejected_both_directions(self) -> None:
        shape = (144, 224)
        near = 0.1
        depth_50m = np.full(shape, near / 50.0, dtype=np.float32)
        depth_90m = np.full(shape, near / 90.0, dtype=np.float32)
        split_depth = depth_50m.copy()
        split_depth[:, : shape[1] // 2] = near / 90.0
        lighting = soft_step(shape, 150.0)
        reverse_camera = reverse_z_camera(shape, near)
        config = ShadowEdgeMetricsConfig(world_up=(0.0, 0.0, 1.0))
        self.assertLess(abs(near / 50.0 - near / 90.0), 0.002)

        cases = (
            ("50_to_90", depth_50m, split_depth, "after"),
            ("90_to_50", split_depth, depth_50m, "before"),
        )
        for name, before_depth, after_depth, edge_side in cases:
            with self.subTest(direction=name):
                result = evaluate_shadow_edge_metrics(
                    frame(
                        shape,
                        lighting,
                        depth=before_depth,
                        normal=(0.0, 0.0, 1.0),
                        camera_override=reverse_camera,
                    ),
                    frame(
                        shape,
                        lighting,
                        depth=after_depth,
                        normal=(0.0, 0.0, 1.0),
                        camera_override=reverse_camera,
                    ),
                    config,
                )
                self.assertEqual(
                    STATUS_INCONCLUSIVE, result.status, result.to_dict()
                )
                self.assertLess(
                    result.geometry["valid_reprojection_ratio"], 0.95
                )
                self.assertEqual(
                    "world-space point distance",
                    result.geometry["reprojection_geometry_basis"],
                )
                self.assertGreater(
                    result.geometry[f"geometry_edge_pixels_{edge_side}"], 0
                )
    def test_stable_gbuffer_with_moving_shadow_fails(self) -> None:
        shape = (192, 320)
        before_lighting = soft_step(shape, 155.0)
        after_lighting = soft_step(shape, 156.0)

        result = evaluate_shadow_edge_metrics(
            frame(shape, before_lighting),
            frame(shape, after_lighting),
        )

        assert_failed_stage(self, result, "scene_color_hdr")
        self.assertGreaterEqual(
            result.geometry["valid_reprojection_ratio"], 0.99
        )

    def test_hdr_stable_but_history_and_final_ghost_fail(self) -> None:
        shape = (192, 320)
        current = soft_step(shape, 150.0)
        stale = soft_step(shape, 154.0)
        ghosted = 0.5 * current + 0.5 * stale

        result = evaluate_shadow_edge_metrics(
            frame(shape, current, history=current, final=current),
            frame(shape, current, history=ghosted, final=ghosted),
        )

        self.assertEqual(
            STATUS_PASS,
            result.stages["scene_color_hdr"].status,
            result.to_dict(),
        )
        assert_failed_stage(self, result, "history_write")
        self.assertEqual(STATUS_FAIL, result.stages["final"].status)

    def test_material_and_geometry_edges_are_excluded_not_misdiagnosed(self) -> None:
        shape = (192, 360)
        motion_px = 2

        before_base = np.full(shape, 0.72, dtype=np.float32)
        before_base[:, 88:] = 0.38
        after_base = np.full(shape, 0.72, dtype=np.float32)
        after_base[:, 88 - motion_px :] = 0.38

        before_depth = np.full(shape, 0.48, dtype=np.float32)
        before_depth[:, 170:] = 0.58
        after_depth = np.full(shape, 0.48, dtype=np.float32)
        after_depth[:, 170 - motion_px :] = 0.58

        before_shadow = soft_step(shape, 270.0)
        after_shadow = soft_step(shape, 270.0 - motion_px)
        before_lighting = before_shadow * before_base
        after_lighting = after_shadow * after_base

        result = evaluate_shadow_edge_metrics(
            frame(
                shape,
                before_lighting,
                depth=before_depth,
                base_color=before_base,
            ),
            frame(
                shape,
                after_lighting,
                depth=after_depth,
                base_color=after_base,
                horizontal_camera_motion_px=motion_px,
            ),
        )

        self.assertEqual(STATUS_PASS, result.status, result.to_dict())
        self.assertGreater(
            result.geometry["geometry_edge_pixels_before"], 0
        )
        self.assertGreater(
            result.geometry["geometry_edge_pixels_after"], 0
        )

    def test_no_receiver_roi_is_inconclusive(self) -> None:
        shape = (128, 192)
        lighting = soft_step(shape, 96.0)

        result = evaluate_shadow_edge_metrics(
            frame(shape, lighting, normal=(1.0, 0.0, 0.0)),
            frame(shape, lighting, normal=(1.0, 0.0, 0.0)),
        )

        self.assertEqual(STATUS_INCONCLUSIVE, result.status, result.to_dict())
        self.assertFalse(result.passed)
        self.assertEqual({}, result.stages)

    def test_high_disocclusion_is_inconclusive(self) -> None:
        shape = (160, 256)
        lighting = soft_step(shape, 150.0)
        before_depth = np.full(shape, 0.5, dtype=np.float32)
        after_depth = before_depth.copy()
        after_depth[:, :64] = 0.72

        result = evaluate_shadow_edge_metrics(
            frame(shape, lighting, depth=before_depth),
            frame(shape, lighting, depth=after_depth),
        )

        self.assertEqual(STATUS_INCONCLUSIVE, result.status, result.to_dict())
        self.assertFalse(result.passed)
        self.assertLess(
            result.geometry["valid_reprojection_ratio"],
            0.95,
        )

    def test_color_inputs_broadcast_all_scalar_gray_rgb_combinations(self) -> None:
        shape = (72, 112)
        lighting = soft_step(shape, 62.0)
        base_forms = (
            0.72,
            np.asarray((0.72,), dtype=np.float32),
            np.asarray((0.72, 0.72, 0.72), dtype=np.float32),
            np.full(shape, 0.72, dtype=np.float32),
            np.full(shape + (1,), 0.72, dtype=np.float32),
            np.full(shape + (3,), 0.72, dtype=np.float32),
        )
        for before_base in base_forms:
            for after_base in base_forms:
                with self.subTest(
                    before_base=np.asarray(before_base).shape,
                    after_base=np.asarray(after_base).shape,
                ):
                    result = evaluate_shadow_edge_metrics(
                        frame(shape, lighting, base_color=before_base),
                        frame(shape, lighting, base_color=after_base),
                    )
                    self.assertEqual(
                        STATUS_PASS, result.status, result.to_dict()
                    )

        lighting_forms = (
            lighting,
            lighting[..., None],
            np.repeat(lighting[..., None], 3, axis=2),
        )
        for before_lighting in lighting_forms:
            for after_lighting in lighting_forms:
                with self.subTest(
                    before_lighting=before_lighting.shape,
                    after_lighting=after_lighting.shape,
                ):
                    result = evaluate_shadow_edge_metrics(
                        frame(shape, before_lighting),
                        frame(shape, after_lighting),
                    )
                    self.assertEqual(
                        STATUS_PASS, result.status, result.to_dict()
                    )

        constant_result = evaluate_shadow_edge_metrics(
            frame(shape, 0.8),
            frame(shape, np.asarray((0.8, 0.8, 0.8), dtype=np.float32)),
        )
        self.assertNotEqual("error", constant_result.status)
        self.assertFalse(constant_result.passed)

    def test_shape_mismatch_and_memory_error_return_error_status(self) -> None:
        shape = (96, 128)
        lighting = soft_step(shape, 70.0)
        mismatched = np.ones((shape[0], shape[1] + 1), dtype=np.float32)
        result = evaluate_shadow_edge_metrics(
            frame(shape, lighting),
            frame(shape, mismatched),
        )
        self.assertEqual("error", result.status, result.to_dict())
        self.assertIn("expected spatial shape", result.reasons[0])

        invalid_y_camera = CameraMatrices(
            clip_from_world=np.eye(4, dtype=np.float64),
            framebuffer_y_to_ndc="implicit_or_unknown",
            view_from_world=np.eye(4, dtype=np.float64),
        )
        invalid_y_result = evaluate_shadow_edge_metrics(
            frame(shape, lighting, camera_override=invalid_y_camera),
            frame(shape, lighting, camera_override=invalid_y_camera),
        )
        self.assertEqual("error", invalid_y_result.status)
        self.assertIn("framebuffer_y_to_ndc", invalid_y_result.reasons[0])
        with mock.patch(
            "tools.rdc_shadow_edge_metrics._prepare_frame",
            side_effect=MemoryError("synthetic allocation failure"),
        ):
            memory_result = evaluate_shadow_edge_metrics(
                frame(shape, lighting), frame(shape, lighting)
            )
        self.assertEqual("error", memory_result.status)
        self.assertIn("synthetic allocation failure", memory_result.reasons[0])

    def test_scalar_vs_hxwx1_base_color_99_percent_change_is_bounded(self) -> None:
        shape = (160, 224)
        lighting = soft_step(shape, 140.0)
        changed_base = np.full(shape + (1,), 0.1, dtype=np.float32)
        changed_base[:16, :14, 0] = 0.72

        result = evaluate_shadow_edge_metrics(
            frame(shape, lighting, base_color=0.72),
            frame(shape, lighting, base_color=changed_base),
        )

        self.assertEqual(STATUS_INCONCLUSIVE, result.status, result.to_dict())
        self.assertFalse(result.passed)
        self.assertLess(result.geometry["valid_reprojection_ratio"], 0.02)
    def test_residual_p99_exactly_rejects_value_above_release_limit(self) -> None:
        sample_count = 10_000
        before = np.zeros((100, 100), dtype=np.float32)
        after = np.zeros_like(before)
        after_flat = after.reshape(-1)
        after_flat[9_800:9_950] = np.float32(0.0500051)
        after_flat[9_950:] = np.float32(1.0)
        roi = np.ones_like(before, dtype=bool)

        residual_p99 = _difference_quantile(
            before,
            after,
            roi,
            scale=1.0,
            quantile=0.99,
            bins=4096,
            chunk_rows=17,
        )

        self.assertEqual(sample_count, int(np.count_nonzero(roi)))
        self.assertAlmostEqual(0.0500051, residual_p99, places=7)
        self.assertGreater(
            residual_p99,
            ShadowEdgeMetricsConfig().max_normalized_residual_p99,
        )

    def test_chamfer_p95_uses_nearest_rank_for_48_points(self) -> None:
        query_edges = np.zeros((3, 200), dtype=bool)
        reference_edges = np.zeros_like(query_edges)
        x_positions = 2 + 4 * np.arange(48)
        query_edges[1, x_positions] = True
        reference_edges[1, x_positions[:45]] = True
        reference_edges[1, x_positions[45:] + 1] = True

        chamfer_p95 = _edge_distance_p95(
            query_edges,
            reference_edges,
            search_radius=4,
        )

        self.assertGreaterEqual(chamfer_p95, 1.0)
        self.assertGreater(
            chamfer_p95,
            ShadowEdgeMetricsConfig().max_edge_chamfer_p95_px,
        )
    def test_float32_histogram_refinement_uses_float64_edge_membership(
        self,
    ) -> None:
        shape = (720, 1280)
        rng = np.random.default_rng(7)
        u = rng.random(shape, dtype=np.float32)
        lighting = np.exp(u).astype(np.float32) - np.float32(1.0e-4)
        shared_depth = np.full(shape, 0.5, dtype=np.float32)
        original_histogram = np.histogram

        self.assertGreater(lighting.size, 262_144)
        self.assertEqual(np.dtype(np.float32), lighting.dtype)
        self.assertTrue(lighting.flags.c_contiguous)
        with mock.patch(
            "tools.rdc_shadow_edge_metrics.np.histogram",
            wraps=original_histogram,
        ) as histogram:
            result = evaluate_shadow_edge_metrics(
                frame(shape, lighting, depth=shared_depth),
                frame(shape, lighting, depth=shared_depth),
            )

        self.assertGreater(histogram.call_count, 0)
        self.assertEqual(STATUS_PASS, result.status, result.to_dict())
    def test_720p_1080p_and_4k_distinct_stages_use_consistent_thresholds(self) -> None:
        observed = []
        for shape in ((720, 1280), (1080, 1920), (2160, 3840)):
            with self.subTest(shape=shape):
                before_lighting = soft_step(shape, shape[1] * 0.57)
                after_lighting = soft_step(shape, shape[1] * 0.57 + 1.0)
                shared_depth = np.full(shape, 0.5, dtype=np.float32)
                before_history = before_lighting.copy()
                before_final = before_lighting.copy()
                after_history = after_lighting.copy()
                after_final = after_lighting.copy()
                self.assertFalse(
                    np.shares_memory(before_lighting, before_history)
                )
                result = evaluate_shadow_edge_metrics(
                    frame(
                        shape,
                        before_lighting,
                        depth=shared_depth,
                        history=before_history,
                        final=before_final,
                    ),
                    frame(
                        shape,
                        after_lighting,
                        depth=shared_depth,
                        history=after_history,
                        final=after_final,
                    ),
                )
                self.assertEqual(STATUS_FAIL, result.status, result.to_dict())
                metric = result.stages["scene_color_hdr"]
                self.assertGreater(metric.edge_chamfer_p95_px, 0.25)
                for stage_name in ("scene_color_hdr", "history_write", "final"):
                    self.assertEqual(
                        STATUS_FAIL,
                        result.stages[stage_name].status,
                        result.to_dict(),
                    )
                observed.append(metric.edge_chamfer_p95_px)
                del (
                    result,
                    before_lighting,
                    after_lighting,
                    before_history,
                    before_final,
                    after_history,
                    after_final,
                    shared_depth,
                )
                gc.collect()

        self.assertLess(max(observed) - min(observed), 0.1, observed)

    def test_view_space_normals_and_result_are_integration_friendly(self) -> None:
        shape = (128, 192)
        lighting = soft_step(shape, 100.0)
        base = frame(shape, lighting)
        view_normal_frame = ShadowFrame(
            scene_depth=base.scene_depth,
            view_normals=np.asarray((0.0, 0.0, 1.0), dtype=np.float32),
            base_color=base.base_color,
            scene_color_hdr=base.scene_color_hdr,
            history_write=base.history_write,
            final=base.final,
            camera=base.camera,
        )

        result = evaluate_shadow_edge_metrics(
            view_normal_frame,
            view_normal_frame,
        )

        self.assertEqual(STATUS_PASS, result.status, result.to_dict())
        encoded = json.dumps(result.to_dict(), sort_keys=True)
        self.assertIn("mgif-shadow-edge-roi-metrics-v1", encoded)
        self.assertIn('"passed": true', encoded)
        self.assertTrue(
            math.isfinite(
                result.stages["scene_color_hdr"].normalized_residual_p99
            )
        )


if __name__ == "__main__":
    unittest.main()
