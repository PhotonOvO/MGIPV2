#!/usr/bin/env python3
"""Resolution-independent shadow-edge stability metrics.

This module is intentionally independent from the RenderDoc comparator.  It
only depends on NumPy and consumes already-decoded frame buffers.

Matrix convention
-----------------
Matrices multiply column vectors. ``clip_from_world`` maps world positions to
unjittered clip space. ``world_from_clip`` is its inverse and may be omitted.
Depth buffers contain post-divide device depth. The default depth range is
Vulkan/D3D style ``[0, 1]``; OpenGL ``[-1, 1]`` is also supported.

``framebuffer_y_to_ndc`` is mandatory because Vulkan applications commonly
choose one of two valid Y mappings:

* ``"top_to_negative_one"``: framebuffer top maps to NDC -1 and bottom to +1.
  This is MGIF's positive-height viewport with a Y-flipped projection.
* ``"top_to_positive_one"``: framebuffer top maps to NDC +1 and bottom to -1.

Normals must already be decoded to signed vectors. Supply either world-space
or view-space normals. Camera view transforms are also used to reconstruct
linear view depth; device-depth deltas are never used as geometry distances.
Color-like inputs accept scalar, C, HxW, or HxWxC values with C=1/3/4. They
are canonicalized to HxWxC; grayscale is channel-broadcast across frame pairs
and alpha is ignored.
"""

from __future__ import annotations

from dataclasses import asdict, dataclass, field
import math
from typing import Any, Mapping, Sequence

import numpy as np


STATUS_PASS = "pass"
STATUS_FAIL = "fail"
STATUS_INCONCLUSIVE = "inconclusive"
STATUS_ERROR = "error"

STAGE_NAMES = ("scene_color_hdr", "history_write", "final")
FRAMEBUFFER_TOP_TO_NDC_NEGATIVE_ONE = "top_to_negative_one"
FRAMEBUFFER_TOP_TO_NDC_POSITIVE_ONE = "top_to_positive_one"
FRAMEBUFFER_Y_TO_NDC_CONVENTIONS = {
    FRAMEBUFFER_TOP_TO_NDC_NEGATIVE_ONE,
    FRAMEBUFFER_TOP_TO_NDC_POSITIVE_ONE,
}


@dataclass(frozen=True)
class CameraMatrices:
    """Unjittered camera transforms and viewport for one frame."""

    clip_from_world: Any
    framebuffer_y_to_ndc: str
    world_from_clip: Any | None = None
    view_from_world: Any | None = None
    world_from_view: Any | None = None
    viewport: tuple[int, int, int, int] | None = None
    depth_range: str = "zero_to_one"


@dataclass(frozen=True)
class ShadowFrame:
    """Buffers required for one side of a shadow stability comparison."""

    scene_depth: Any
    base_color: Any
    scene_color_hdr: Any
    history_write: Any
    final: Any
    camera: CameraMatrices
    world_normals: Any | None = None
    view_normals: Any | None = None


@dataclass(frozen=True)
class ShadowEdgeMetricsConfig:
    """Release thresholds plus conservative ROI construction controls."""

    min_valid_reprojection_ratio: float = 0.95
    max_edge_chamfer_p95_px: float = 0.25
    max_outside_1px_mismatch: float = 0.005
    max_normalized_residual_p99: float = 0.05

    world_up: tuple[float, float, float] = (0.0, 1.0, 0.0)
    max_receiver_tilt_degrees: float = 25.0
    max_reprojection_normal_degrees: float = 12.0
    world_reprojection_abs_tolerance: float = 1.0e-3
    world_reprojection_relative_tolerance: float = 2.0e-4
    world_reprojection_pixel_tolerance: float = 0.25
    base_color_reprojection_tolerance: float = 0.08

    linear_depth_edge_abs_threshold: float = 0.02
    linear_depth_edge_relative_threshold: float = 0.02
    normal_edge_degrees: float = 18.0
    base_color_edge_threshold: float = 0.08
    geometry_edge_margin_px: int = 2

    min_receiver_fraction: float = 0.01
    min_roi_fraction: float = 0.005
    min_receiver_pixels_at_1080p: int = 512
    min_roi_pixels_at_1080p: int = 256
    min_edge_pixels_at_1080p: int = 16
    min_splat_weight: float = 0.25

    min_log_luma_edge_gradient: float = 0.005
    adaptive_edge_quantile: float = 0.99
    adaptive_edge_fraction: float = 0.25
    log_luma_epsilon: float = 1.0e-4
    residual_scale_floor: float = 0.25
    chamfer_search_radius_px: int = 8
    projection_chunk_rows: int = 128
    quantile_histogram_bins: int = 4096


@dataclass
class StageMetrics:
    status: str
    reasons: list[str]
    edge_pixels_before: int
    edge_pixels_after: int
    edge_chamfer_before_to_after_p95_px: float | None
    edge_chamfer_after_to_before_p95_px: float | None
    edge_chamfer_p95_px: float | None
    outside_1px_mismatch: float | None
    normalized_residual_p99: float | None
    residual_scale_log_luma: float | None
    edge_gradient_threshold_before: float | None
    edge_gradient_threshold_after: float | None
    checks: dict[str, bool | None] = field(default_factory=dict)


@dataclass
class ShadowEdgeMetricsResult:
    status: str
    reasons: list[str]
    geometry: dict[str, Any]
    stages: dict[str, StageMetrics]
    thresholds: dict[str, float]
    schema: str = "mgif-shadow-edge-roi-metrics-v1"

    @property
    def passed(self) -> bool:
        return self.status == STATUS_PASS

    def to_dict(self) -> dict[str, Any]:
        result = asdict(self)
        result["passed"] = self.passed
        return result


def evaluate_shadow_edge_metrics(
    before: ShadowFrame,
    after: ShadowFrame,
    config: ShadowEdgeMetricsConfig | None = None,
) -> ShadowEdgeMetricsResult:
    """Evaluate reprojected shadow-edge stability.

    A result can be ``pass``, ``fail``, ``inconclusive``, or ``error``.
    Inconclusive/error results never set :attr:`ShadowEdgeMetricsResult.passed`.
    """

    cfg = config or ShadowEdgeMetricsConfig()
    thresholds = _threshold_dict(cfg)
    try:
        prepared_before = _prepare_frame(before)
        prepared_after = _prepare_frame(after)
        _align_color_channels(prepared_before, prepared_after)
        geometry_state = _build_reprojected_roi(
            prepared_before,
            prepared_after,
            cfg,
        )
        if geometry_state["status"] != STATUS_PASS:
            return ShadowEdgeMetricsResult(
                status=geometry_state["status"],
                reasons=list(geometry_state["reasons"]),
                geometry=geometry_state["report"],
                stages={},
                thresholds=thresholds,
            )

        stages: dict[str, StageMetrics] = {}
        stage_cache: dict[tuple[int, int], StageMetrics] = {}
        for stage_name in STAGE_NAMES:
            before_stage = prepared_before["stages"][stage_name]
            after_stage = prepared_after["stages"][stage_name]
            cache_key = (
                _array_storage_key(before_stage),
                _array_storage_key(after_stage),
            )
            if cache_key not in stage_cache:
                stage_cache[cache_key] = _evaluate_stage(
                    stage_name,
                    before_stage,
                    after_stage,
                    geometry_state,
                    cfg,
                )
            stages[stage_name] = stage_cache[cache_key]

        failed = [
            name for name, stage in stages.items() if stage.status == STATUS_FAIL
        ]
        inconclusive = [
            name
            for name, stage in stages.items()
            if stage.status == STATUS_INCONCLUSIVE
        ]
        errors = [
            name for name, stage in stages.items() if stage.status == STATUS_ERROR
        ]
        if errors:
            status = STATUS_ERROR
            reasons = [f"{name}: stage evaluation error" for name in errors]
        elif failed:
            status = STATUS_FAIL
            reasons = [f"{name}: release threshold exceeded" for name in failed]
        elif inconclusive:
            status = STATUS_INCONCLUSIVE
            reasons = [
                f"{name}: insufficient soft-shadow edge evidence"
                for name in inconclusive
            ]
        else:
            status = STATUS_PASS
            reasons = []

        return ShadowEdgeMetricsResult(
            status=status,
            reasons=reasons,
            geometry=geometry_state["report"],
            stages=stages,
            thresholds=thresholds,
        )
    except (
        MemoryError,
        ValueError,
        TypeError,
        np.linalg.LinAlgError,
        FloatingPointError,
    ) as exc:
        return ShadowEdgeMetricsResult(
            status=STATUS_ERROR,
            reasons=[str(exc)],
            geometry={},
            stages={},
            thresholds=thresholds,
        )


def _array_storage_key(array: np.ndarray) -> tuple[Any, ...]:
    interface = array.__array_interface__
    return (
        int(interface["data"][0]),
        tuple(array.shape),
        tuple(array.strides),
        array.dtype.str,
    )

def _threshold_dict(config: ShadowEdgeMetricsConfig) -> dict[str, float]:
    return {
        "min_valid_reprojection_ratio": config.min_valid_reprojection_ratio,
        "max_edge_chamfer_p95_px": config.max_edge_chamfer_p95_px,
        "max_outside_1px_mismatch": config.max_outside_1px_mismatch,
        "max_normalized_residual_p99": config.max_normalized_residual_p99,
    }


def _prepare_frame(frame: ShadowFrame) -> dict[str, Any]:
    depth = np.asarray(frame.scene_depth)
    if depth.ndim != 2:
        raise ValueError(f"scene_depth must be HxW, got {depth.shape}")
    depth = depth.astype(np.float32, copy=False)
    shape = depth.shape

    camera = _prepare_camera(frame.camera, shape)
    normals = _prepare_world_normals(frame, shape, camera)
    base_color = _prepare_color(frame.base_color, shape, "base_color")
    stages = {
        "scene_color_hdr": _prepare_color(
            frame.scene_color_hdr, shape, "scene_color_hdr"
        ),
        "history_write": _prepare_color(
            frame.history_write, shape, "history_write"
        ),
        "final": _prepare_color(frame.final, shape, "final"),
    }
    return {
        "shape": shape,
        "depth": depth,
        "normals": normals,
        "base_color": base_color,
        "stages": stages,
        "camera": camera,
    }


def _prepare_camera(camera: CameraMatrices, shape: tuple[int, int]) -> dict[str, Any]:
    clip_from_world = _matrix4(camera.clip_from_world, "clip_from_world")
    if camera.world_from_clip is None:
        world_from_clip = np.linalg.inv(clip_from_world)
    else:
        world_from_clip = _matrix4(camera.world_from_clip, "world_from_clip")

    depth_range = str(camera.depth_range).lower()
    if depth_range not in {"zero_to_one", "minus_one_to_one"}:
        raise ValueError(
            "depth_range must be 'zero_to_one' or 'minus_one_to_one'"
        )
    y_convention = str(camera.framebuffer_y_to_ndc).lower()
    if y_convention not in FRAMEBUFFER_Y_TO_NDC_CONVENTIONS:
        choices = ", ".join(sorted(FRAMEBUFFER_Y_TO_NDC_CONVENTIONS))
        raise ValueError(
            f"framebuffer_y_to_ndc must be one of: {choices}"
        )

    height, width = shape
    viewport = camera.viewport or (0, 0, width, height)
    if len(viewport) != 4:
        raise ValueError("viewport must be (x, y, width, height)")
    vx, vy, vw, vh = (int(value) for value in viewport)
    if vw <= 1 or vh <= 1:
        raise ValueError(f"viewport is too small: {viewport}")
    if vx < 0 or vy < 0 or vx + vw > width or vy + vh > height:
        raise ValueError(
            f"viewport {viewport} lies outside image shape {shape}"
        )

    view_from_world = (
        _matrix4(camera.view_from_world, "view_from_world")
        if camera.view_from_world is not None
        else None
    )
    world_from_view = (
        _matrix4(camera.world_from_view, "world_from_view")
        if camera.world_from_view is not None
        else None
    )
    if view_from_world is None and world_from_view is None:
        raise ValueError(
            "CameraMatrices requires view_from_world or world_from_view "
            "for linear-depth/world-geometry validation"
        )
    if view_from_world is None:
        view_from_world = np.linalg.inv(world_from_view)
    if world_from_view is None:
        world_from_view = np.linalg.inv(view_from_world)

    return {
        "clip_from_world": clip_from_world,
        "world_from_clip": world_from_clip,
        "view_from_world": view_from_world,
        "world_from_view": world_from_view,
        "viewport": (vx, vy, vw, vh),
        "depth_range": depth_range,
        "framebuffer_y_to_ndc": y_convention,
    }


def _matrix4(value: Any, name: str) -> np.ndarray:
    matrix = np.asarray(value, dtype=np.float64)
    if matrix.shape != (4, 4):
        raise ValueError(f"{name} must be 4x4, got {matrix.shape}")
    if not np.all(np.isfinite(matrix)):
        raise ValueError(f"{name} contains non-finite values")
    return matrix


def _prepare_world_normals(
    frame: ShadowFrame,
    shape: tuple[int, int],
    camera: Mapping[str, Any],
) -> np.ndarray:
    if frame.world_normals is not None:
        normals = _prepare_normals(frame.world_normals, shape, "world_normals")
    elif frame.view_normals is not None:
        normals = _prepare_normals(frame.view_normals, shape, "view_normals")
        rotation = camera["world_from_view"][:3, :3].astype(np.float32)
        if normals.ndim == 1:
            normals = rotation @ normals
        else:
            normals = normals @ rotation.T
    else:
        raise ValueError("supply either world_normals or view_normals")
    return _normalize_normals(normals)


def _prepare_normals(value: Any, shape: tuple[int, int], name: str) -> np.ndarray:
    array = np.asarray(value, dtype=np.float32)
    if array.shape == (3,):
        return array
    if array.shape != shape + (3,):
        raise ValueError(f"{name} must be length 3 or HxWx3, got {array.shape}")
    return array


def _normalize_normals(normals: np.ndarray) -> np.ndarray:
    if normals.ndim == 1:
        length = float(np.linalg.norm(normals))
        if not math.isfinite(length) or length <= 1.0e-8:
            return np.zeros(3, dtype=np.float32)
        return (normals / length).astype(np.float32, copy=False)
    lengths = np.linalg.norm(normals, axis=-1, keepdims=True)
    return np.divide(
        normals,
        lengths,
        out=np.zeros_like(normals, dtype=np.float32),
        where=lengths > 1.0e-8,
    )


def _prepare_color(value: Any, shape: tuple[int, int], name: str) -> np.ndarray:
    """Canonicalize color-like input to an HxWxC float32 view/array."""

    array = np.asarray(value)
    if array.ndim == 0:
        array = array.reshape(1, 1, 1)
    elif array.ndim == 1 and array.shape[0] in {1, 3, 4}:
        array = array.reshape(1, 1, array.shape[0])
    elif array.shape == shape:
        array = array[..., None]
    elif (
        array.ndim == 3
        and array.shape[:2] == shape
        and array.shape[2] in {1, 3, 4}
    ):
        pass
    else:
        raise ValueError(
            f"{name} must be scalar, C, HxW, or HxWxC with C=1/3/4; "
            f"expected spatial shape {shape}, got {array.shape}"
        )

    array = array.astype(np.float32, copy=False)
    if array.shape[-1] == 4:
        array = array[..., :3]
    return np.broadcast_to(array, shape + (array.shape[-1],))


def _align_color_channels(
    before: dict[str, Any],
    after: dict[str, Any],
) -> None:
    pairs = [(before, after, "base_color")]
    for stage_name in STAGE_NAMES:
        pairs.append((before["stages"], after["stages"], stage_name))

    for before_owner, after_owner, key in pairs:
        before_color = before_owner[key]
        after_color = after_owner[key]
        before_channels = before_color.shape[2]
        after_channels = after_color.shape[2]
        target_channels = max(before_channels, after_channels)
        if target_channels not in {1, 3}:
            raise ValueError(f"{key} has unsupported channel count")
        if before_channels not in {1, target_channels}:
            raise ValueError(
                f"{key} channel mismatch: {before_channels} vs {after_channels}"
            )
        if after_channels not in {1, target_channels}:
            raise ValueError(
                f"{key} channel mismatch: {before_channels} vs {after_channels}"
            )
        if before_channels != target_channels:
            before_owner[key] = np.broadcast_to(
                before_color,
                before_color.shape[:2] + (target_channels,),
            )
        if after_channels != target_channels:
            after_owner[key] = np.broadcast_to(
                after_color,
                after_color.shape[:2] + (target_channels,),
            )

def _build_reprojected_roi(
    before: Mapping[str, Any],
    after: Mapping[str, Any],
    config: ShadowEdgeMetricsConfig,
) -> dict[str, Any]:
    before_shape = before["shape"]
    after_shape = after["shape"]
    chunk_rows = max(1, int(config.projection_chunk_rows))
    before_valid_depth = _valid_depth_mask(
        before["depth"], before["camera"]["depth_range"]
    )
    after_valid_depth = _valid_depth_mask(
        after["depth"], after["camera"]["depth_range"]
    )
    before_viewport = _viewport_mask(
        before_shape, before["camera"]["viewport"]
    )
    after_viewport = _viewport_mask(after_shape, after["camera"]["viewport"])

    world_up = np.asarray(config.world_up, dtype=np.float32)
    up_length = float(np.linalg.norm(world_up))
    if up_length <= 1.0e-8:
        raise ValueError("world_up must be non-zero")
    world_up /= up_length
    receiver_cos = math.cos(math.radians(config.max_receiver_tilt_degrees))

    before_horizontal = _normal_alignment(before["normals"], world_up)
    after_horizontal = _normal_alignment(after["normals"], world_up)
    before_receiver = (
        before_viewport
        & before_valid_depth
        & np.isfinite(before_horizontal)
        & (np.abs(before_horizontal) >= receiver_cos)
    )
    after_receiver = (
        after_viewport
        & after_valid_depth
        & np.isfinite(after_horizontal)
        & (np.abs(after_horizontal) >= receiver_cos)
    )

    before_linear_depth = _linear_view_depth_image(before, chunk_rows)
    before_geometry_edges = _geometry_edge_mask(
        before_linear_depth,
        before["normals"],
        before["base_color"],
        before_valid_depth & before_viewport,
        config,
    )
    del before_linear_depth
    after_linear_depth = _linear_view_depth_image(after, chunk_rows)
    after_geometry_edges = _geometry_edge_mask(
        after_linear_depth,
        after["normals"],
        after["base_color"],
        after_valid_depth & after_viewport,
        config,
    )
    del after_linear_depth
    before_receiver &= ~before_geometry_edges
    after_receiver &= ~after_geometry_edges

    before_receiver_count = int(np.count_nonzero(before_receiver))
    after_receiver_count = int(np.count_nonzero(after_receiver))
    required_before = max(
        _scaled_pixel_minimum(
            before_shape, config.min_receiver_pixels_at_1080p
        ),
        int(math.ceil(config.min_receiver_fraction * np.prod(before_shape))),
    )
    required_after = max(
        _scaled_pixel_minimum(
            after_shape, config.min_receiver_pixels_at_1080p
        ),
        int(math.ceil(config.min_receiver_fraction * np.prod(after_shape))),
    )
    base_report = {
        "before_shape": list(before_shape),
        "after_shape": list(after_shape),
        "framebuffer_y_to_ndc_before": before["camera"][
            "framebuffer_y_to_ndc"
        ],
        "framebuffer_y_to_ndc_after": after["camera"][
            "framebuffer_y_to_ndc"
        ],
        "geometry_depth_basis": "absolute linear view-Z",
        "reprojection_geometry_basis": "world-space point distance",
        "receiver_pixels_before": before_receiver_count,
        "receiver_pixels_after": after_receiver_count,
        "required_receiver_pixels_before": required_before,
        "required_receiver_pixels_after": required_after,
    }
    if (
        before_receiver_count < required_before
        or after_receiver_count < required_after
    ):
        return {
            "status": STATUS_INCONCLUSIVE,
            "reasons": ["no sufficiently large near-horizontal receiver ROI"],
            "report": base_report,
        }

    projected_x, projected_y = _project_before_to_after(before, after)
    valid_reprojection = np.zeros(before_shape, dtype=bool)
    in_bounds_count = 0
    same_surface_count = 0
    after_vx, after_vy, after_vw, after_vh = after["camera"]["viewport"]
    normal_cos = math.cos(
        math.radians(config.max_reprojection_normal_degrees)
    )
    maximum_world_error = 0.0

    for row0 in range(0, before_shape[0], chunk_rows):
        row1 = min(before_shape[0], row0 + chunk_rows)
        local_y, local_x = np.nonzero(before_receiver[row0:row1])
        if local_y.size == 0:
            continue
        source_y = local_y + row0
        source_x = local_x
        target_x = projected_x[source_y, source_x]
        target_y = projected_y[source_y, source_x]
        in_bounds = (
            np.isfinite(target_x)
            & np.isfinite(target_y)
            & (target_x >= after_vx)
            & (target_x <= after_vx + after_vw - 1)
            & (target_y >= after_vy)
            & (target_y <= after_vy + after_vh - 1)
        )
        in_bounds_count += int(np.count_nonzero(in_bounds))
        if not np.any(in_bounds):
            continue

        source_y = source_y[in_bounds]
        source_x = source_x[in_bounds]
        target_x = target_x[in_bounds]
        target_y = target_y[in_bounds]
        sampled_depth = _bilinear_sample(after["depth"], target_x, target_y)
        source_depth = before["depth"][source_y, source_x]

        source_world = _reconstruct_world_positions(
            before["camera"], source_x, source_y, source_depth
        )
        sampled_world, sampled_world_h = _reconstruct_world_positions(
            after["camera"],
            target_x,
            target_y,
            sampled_depth,
            return_homogeneous=True,
        )
        world_delta = np.linalg.norm(source_world - sampled_world, axis=-1)
        source_view_depth = _view_depth_from_world(
            before["camera"], source_world
        )
        sampled_view_depth = _view_depth_from_world(
            after["camera"], sampled_world
        )
        pixel_footprint = _world_pixel_footprint(
            after["camera"], sampled_world_h, sampled_world
        )
        world_tolerance = (
            config.world_reprojection_abs_tolerance
            + config.world_reprojection_relative_tolerance
            * np.maximum(source_view_depth, sampled_view_depth)
            + config.world_reprojection_pixel_tolerance * pixel_footprint
        )
        same_surface = (
            np.isfinite(sampled_depth)
            & np.all(np.isfinite(source_world), axis=-1)
            & np.all(np.isfinite(sampled_world), axis=-1)
            & np.isfinite(world_delta)
            & (world_delta <= world_tolerance)
        )
        if world_delta.size:
            finite_error = world_delta[np.isfinite(world_delta)]
            if finite_error.size:
                maximum_world_error = max(
                    maximum_world_error, float(np.max(finite_error))
                )

        before_normals = _gather(before["normals"], source_x, source_y)
        after_normals = _bilinear_sample(
            after["normals"], target_x, target_y
        )
        after_normals = _normalize_normals(after_normals)
        normal_dot = np.sum(before_normals * after_normals, axis=-1)
        same_surface &= np.isfinite(normal_dot) & (normal_dot >= normal_cos)
        same_surface &= np.abs(after_normals @ world_up) >= receiver_cos

        before_base = _gather(before["base_color"], source_x, source_y)
        after_base = _bilinear_sample(
            after["base_color"], target_x, target_y
        )
        color_delta = _color_distance(before_base, after_base)
        same_surface &= (
            np.isfinite(color_delta)
            & (color_delta <= config.base_color_reprojection_tolerance)
        )

        nearest_x = np.rint(target_x).astype(np.int64)
        nearest_y = np.rint(target_y).astype(np.int64)
        same_surface &= ~after_geometry_edges[nearest_y, nearest_x]

        same_surface_count += int(np.count_nonzero(same_surface))
        valid_reprojection[
            source_y[same_surface],
            source_x[same_surface],
        ] = True

    support_ratio = (
        float(in_bounds_count / before_receiver_count)
        if before_receiver_count
        else 0.0
    )
    valid_ratio = (
        float(same_surface_count / before_receiver_count)
        if before_receiver_count
        else 0.0
    )
    same_surface_ratio = (
        float(same_surface_count / in_bounds_count)
        if in_bounds_count
        else 0.0
    )

    splat_weight = _forward_splat_weight(
        projected_x,
        projected_y,
        valid_reprojection,
        after_shape,
        after["camera"]["viewport"],
        chunk_rows,
    )
    roi = (
        after_receiver
        & np.isfinite(splat_weight)
        & (splat_weight >= config.min_splat_weight)
    )
    roi = _erode_mask(roi, 2)
    roi_count = int(np.count_nonzero(roi))
    required_roi = max(
        _scaled_pixel_minimum(after_shape, config.min_roi_pixels_at_1080p),
        int(math.ceil(config.min_roi_fraction * np.prod(after_shape))),
    )
    report = {
        **base_report,
        "in_bounds_reprojection_pixels": in_bounds_count,
        "valid_reprojection_pixels": same_surface_count,
        "support_ratio": support_ratio,
        "valid_reprojection_ratio": valid_ratio,
        "same_surface_ratio_within_viewport": same_surface_ratio,
        "disocclusion_ratio_within_viewport": 1.0 - same_surface_ratio,
        "maximum_tested_world_error": maximum_world_error,
        "roi_pixels": roi_count,
        "required_roi_pixels": required_roi,
        "geometry_edge_pixels_before": int(
            np.count_nonzero(before_geometry_edges)
        ),
        "geometry_edge_pixels_after": int(
            np.count_nonzero(after_geometry_edges)
        ),
    }
    if valid_ratio < config.min_valid_reprojection_ratio:
        return {
            "status": STATUS_INCONCLUSIVE,
            "reasons": [
                "valid reprojection ratio is below the release minimum; "
                "disocclusion or viewport loss is too high"
            ],
            "report": report,
        }
    if roi_count < required_roi:
        return {
            "status": STATUS_INCONCLUSIVE,
            "reasons": ["no valid edge-free reprojected receiver ROI"],
            "report": report,
        }

    return {
        "status": STATUS_PASS,
        "reasons": [],
        "report": report,
        "roi": roi,
        "valid_reprojection": valid_reprojection,
        "projected_x": projected_x,
        "projected_y": projected_y,
        "splat_weight": splat_weight,
        "after_shape": after_shape,
        "after_viewport": after["camera"]["viewport"],
        "chunk_rows": chunk_rows,
    }

def _valid_depth_mask(depth: np.ndarray, depth_range: str) -> np.ndarray:
    finite = np.isfinite(depth)
    epsilon = 1.0e-7
    if depth_range == "zero_to_one":
        return finite & (depth > epsilon) & (depth < 1.0 - epsilon)
    return finite & (depth > -1.0 + epsilon) & (depth < 1.0 - epsilon)


def _viewport_mask(
    shape: tuple[int, int], viewport: Sequence[int]
) -> np.ndarray:
    vx, vy, vw, vh = viewport
    mask = np.zeros(shape, dtype=bool)
    mask[vy : vy + vh, vx : vx + vw] = True
    return mask


def _normal_alignment(normals: np.ndarray, direction: np.ndarray) -> np.ndarray:
    if normals.ndim == 1:
        return np.full((), float(normals @ direction), dtype=np.float32)
    return normals @ direction


def _framebuffer_x_to_ndc(x: np.ndarray, viewport: Sequence[int]) -> np.ndarray:
    vx, _, vw, _ = viewport
    return 2.0 * (x + 0.5 - vx) / float(vw) - 1.0


def _framebuffer_y_to_ndc(
    y: np.ndarray,
    viewport: Sequence[int],
    convention: str,
) -> np.ndarray:
    _, vy, _, vh = viewport
    normalized = (y + 0.5 - vy) / float(vh)
    if convention == FRAMEBUFFER_TOP_TO_NDC_NEGATIVE_ONE:
        return 2.0 * normalized - 1.0
    return 1.0 - 2.0 * normalized


def _ndc_x_to_framebuffer(
    ndc_x: np.ndarray, viewport: Sequence[int]
) -> np.ndarray:
    vx, _, vw, _ = viewport
    return vx + (ndc_x + 1.0) * 0.5 * vw - 0.5


def _ndc_y_to_framebuffer(
    ndc_y: np.ndarray,
    viewport: Sequence[int],
    convention: str,
) -> np.ndarray:
    _, vy, _, vh = viewport
    if convention == FRAMEBUFFER_TOP_TO_NDC_NEGATIVE_ONE:
        return vy + (ndc_y + 1.0) * 0.5 * vh - 0.5
    return vy + (1.0 - ndc_y) * 0.5 * vh - 0.5


def _reconstruct_world_positions(
    camera: Mapping[str, Any],
    x: np.ndarray,
    y: np.ndarray,
    depth: np.ndarray,
    *,
    return_homogeneous: bool = False,
) -> Any:
    x_values = np.asarray(x, dtype=np.float64).reshape(-1)
    y_values = np.asarray(y, dtype=np.float64).reshape(-1)
    depth_values = np.asarray(depth, dtype=np.float64).reshape(-1)
    if not (x_values.size == y_values.size == depth_values.size):
        raise ValueError("position/depth reconstruction inputs must match")

    clip = np.empty((x_values.size, 4), dtype=np.float64)
    clip[:, 0] = _framebuffer_x_to_ndc(x_values, camera["viewport"])
    clip[:, 1] = _framebuffer_y_to_ndc(
        y_values,
        camera["viewport"],
        camera["framebuffer_y_to_ndc"],
    )
    clip[:, 2] = depth_values
    clip[:, 3] = 1.0
    world_h = clip @ camera["world_from_clip"].T
    world_w = world_h[:, 3]
    valid_w = np.isfinite(world_w) & (np.abs(world_w) > 1.0e-12)
    world = np.full((x_values.size, 3), np.nan, dtype=np.float64)
    world[valid_w] = world_h[valid_w, :3] / world_w[valid_w, None]
    if return_homogeneous:
        return world, world_h
    return world


def _view_depth_from_world(
    camera: Mapping[str, Any], world: np.ndarray
) -> np.ndarray:
    view_from_world = camera["view_from_world"]
    view_z = world @ view_from_world[2, :3] + view_from_world[2, 3]
    return np.abs(view_z)


def _world_pixel_footprint(
    camera: Mapping[str, Any],
    world_h: np.ndarray,
    world: np.ndarray,
) -> np.ndarray:
    world_from_clip = camera["world_from_clip"]
    _, _, viewport_width, viewport_height = camera["viewport"]
    delta_x = 2.0 / float(viewport_width)
    delta_y = 2.0 / float(viewport_height)
    if (
        camera["framebuffer_y_to_ndc"]
        == FRAMEBUFFER_TOP_TO_NDC_POSITIVE_ONE
    ):
        delta_y = -delta_y

    neighbor_h = world_h + delta_x * world_from_clip[:, 0]
    neighbor_w = neighbor_h[:, 3]
    neighbor = np.full_like(world, np.nan)
    valid_w = np.isfinite(neighbor_w) & (np.abs(neighbor_w) > 1.0e-12)
    neighbor[valid_w] = (
        neighbor_h[valid_w, :3] / neighbor_w[valid_w, None]
    )
    footprint = np.linalg.norm(neighbor - world, axis=-1)

    neighbor_h = world_h + delta_y * world_from_clip[:, 1]
    neighbor_w = neighbor_h[:, 3]
    valid_w = np.isfinite(neighbor_w) & (np.abs(neighbor_w) > 1.0e-12)
    neighbor.fill(np.nan)
    neighbor[valid_w] = (
        neighbor_h[valid_w, :3] / neighbor_w[valid_w, None]
    )
    np.maximum(
        footprint,
        np.linalg.norm(neighbor - world, axis=-1),
        out=footprint,
    )
    return footprint

def _linear_view_depth_image(
    frame: Mapping[str, Any], chunk_rows: int
) -> np.ndarray:
    height, width = frame["shape"]
    linear_depth = np.full((height, width), np.nan, dtype=np.float32)
    vx, vy, vw, vh = frame["camera"]["viewport"]
    columns = np.arange(vx, vx + vw, dtype=np.float64)
    view_from_clip = (
        frame["camera"]["view_from_world"]
        @ frame["camera"]["world_from_clip"]
    )
    for row0 in range(vy, vy + vh, chunk_rows):
        row1 = min(vy + vh, row0 + chunk_rows)
        rows = np.arange(row0, row1, dtype=np.float64)
        grid_x = np.broadcast_to(columns[None, :], (row1 - row0, vw))
        grid_y = np.broadcast_to(rows[:, None], (row1 - row0, vw))
        flat_count = grid_x.size
        clip = np.empty((flat_count, 4), dtype=np.float64)
        clip[:, 0] = _framebuffer_x_to_ndc(
            grid_x.reshape(-1), frame["camera"]["viewport"]
        )
        clip[:, 1] = _framebuffer_y_to_ndc(
            grid_y.reshape(-1),
            frame["camera"]["viewport"],
            frame["camera"]["framebuffer_y_to_ndc"],
        )
        clip[:, 2] = frame["depth"][
            row0:row1, vx : vx + vw
        ].reshape(-1)
        clip[:, 3] = 1.0
        view_h = clip @ view_from_clip.T
        view_w = view_h[:, 3]
        valid_w = np.isfinite(view_w) & (np.abs(view_w) > 1.0e-12)
        view_depth = np.full(flat_count, np.nan, dtype=np.float64)
        view_depth[valid_w] = np.abs(
            view_h[valid_w, 2] / view_w[valid_w]
        )
        linear_depth[row0:row1, vx : vx + vw] = view_depth.reshape(
            row1 - row0, vw
        ).astype(np.float32)
    return linear_depth

def _geometry_edge_mask(
    linear_depth: np.ndarray,
    normals: np.ndarray,
    base_color: np.ndarray,
    valid: np.ndarray,
    config: ShadowEdgeMetricsConfig,
) -> np.ndarray:
    edge = ~valid.copy()
    height, width = linear_depth.shape
    normal_cos = math.cos(math.radians(config.normal_edge_degrees))
    chunk_rows = max(1, int(config.projection_chunk_rows))
    for dy, dx in ((0, 1), (1, 0)):
        usable_height = height - dy
        usable_width = width - dx
        for row0 in range(0, usable_height, chunk_rows):
            row1 = min(usable_height, row0 + chunk_rows)
            y0 = slice(row0, row1)
            y1 = slice(row0 + dy, row1 + dy)
            x0 = slice(0, usable_width)
            x1 = slice(dx, dx + usable_width)
            pair_valid = valid[y0, x0] & valid[y1, x1]

            left_depth = linear_depth[y0, x0]
            right_depth = linear_depth[y1, x1]
            depth_tolerance = (
                config.linear_depth_edge_abs_threshold
                + config.linear_depth_edge_relative_threshold
                * np.maximum(np.abs(left_depth), np.abs(right_depth))
            )
            pair_edge = pair_valid & (
                np.abs(left_depth - right_depth) > depth_tolerance
            )

            if normals.ndim != 1:
                normal_dot = np.sum(
                    normals[y0, x0] * normals[y1, x1], axis=-1
                )
                pair_edge |= pair_valid & (normal_dot < normal_cos)

            color_delta = _color_distance(
                base_color[y0, x0], base_color[y1, x1]
            )
            pair_edge |= pair_valid & (
                color_delta > config.base_color_edge_threshold
            )

            edge[y0, x0] |= pair_edge
            edge[y1, x1] |= pair_edge

    margin = max(0, int(config.geometry_edge_margin_px))
    return _dilate_mask(edge, margin)


def _project_before_to_after(
    before: Mapping[str, Any],
    after: Mapping[str, Any],
) -> tuple[np.ndarray, np.ndarray]:
    height, width = before["shape"]
    projected_x = np.full((height, width), np.nan, dtype=np.float32)
    projected_y = np.full((height, width), np.nan, dtype=np.float32)
    transform = (
        after["camera"]["clip_from_world"]
        @ before["camera"]["world_from_clip"]
    ).astype(np.float64)
    before_vx, before_vy, before_vw, before_vh = before["camera"]["viewport"]
    chunk_rows = 128
    columns = np.arange(before_vx, before_vx + before_vw, dtype=np.float64)

    for row0 in range(before_vy, before_vy + before_vh, chunk_rows):
        row1 = min(before_vy + before_vh, row0 + chunk_rows)
        rows = np.arange(row0, row1, dtype=np.float64)
        grid_x = np.broadcast_to(columns[None, :], (row1 - row0, before_vw))
        grid_y = np.broadcast_to(rows[:, None], (row1 - row0, before_vw))
        flat_count = grid_x.size
        clip = np.empty((flat_count, 4), dtype=np.float64)
        clip[:, 0] = _framebuffer_x_to_ndc(
            grid_x.reshape(-1), before["camera"]["viewport"]
        )
        clip[:, 1] = _framebuffer_y_to_ndc(
            grid_y.reshape(-1),
            before["camera"]["viewport"],
            before["camera"]["framebuffer_y_to_ndc"],
        )
        clip[:, 2] = before["depth"][
            row0:row1, before_vx : before_vx + before_vw
        ].reshape(-1)
        clip[:, 3] = 1.0
        target_clip = clip @ transform.T
        target_w = target_clip[:, 3]
        valid_w = np.isfinite(target_w) & (np.abs(target_w) > 1.0e-12)
        target_ndc_x = np.full(flat_count, np.nan, dtype=np.float64)
        target_ndc_y = np.full(flat_count, np.nan, dtype=np.float64)
        target_ndc_x[valid_w] = target_clip[valid_w, 0] / target_w[valid_w]
        target_ndc_y[valid_w] = target_clip[valid_w, 1] / target_w[valid_w]
        target_x = _ndc_x_to_framebuffer(
            target_ndc_x, after["camera"]["viewport"]
        )
        target_y = _ndc_y_to_framebuffer(
            target_ndc_y,
            after["camera"]["viewport"],
            after["camera"]["framebuffer_y_to_ndc"],
        )

        target_slice = (
            slice(row0, row1),
            slice(before_vx, before_vx + before_vw),
        )
        projected_x[target_slice] = target_x.reshape(
            row1 - row0, before_vw
        ).astype(np.float32)
        projected_y[target_slice] = target_y.reshape(
            row1 - row0, before_vw
        ).astype(np.float32)
    return projected_x, projected_y


def _gather(array: np.ndarray, x: np.ndarray, y: np.ndarray) -> np.ndarray:
    if array.ndim == 1:
        return np.broadcast_to(array, x.shape + array.shape)
    return array[y, x]


def _bilinear_sample(
    array: np.ndarray,
    x: np.ndarray,
    y: np.ndarray,
) -> np.ndarray:
    if array.ndim == 1:
        return np.broadcast_to(array, x.shape + array.shape)
    height, width = array.shape[:2]
    x0 = np.floor(x).astype(np.int64)
    y0 = np.floor(y).astype(np.int64)
    x0 = np.clip(x0, 0, width - 1)
    y0 = np.clip(y0, 0, height - 1)
    x1 = np.minimum(x0 + 1, width - 1)
    y1 = np.minimum(y0 + 1, height - 1)
    wx = (x - x0).astype(np.float32)
    wy = (y - y0).astype(np.float32)
    if array.ndim == 3:
        wx = wx[:, None]
        wy = wy[:, None]
    top = array[y0, x0] * (1.0 - wx) + array[y0, x1] * wx
    bottom = array[y1, x0] * (1.0 - wx) + array[y1, x1] * wx
    return top * (1.0 - wy) + bottom * wy


def _color_distance(left: np.ndarray, right: np.ndarray) -> np.ndarray:
    if (
        left.shape != right.shape
        or left.ndim < 2
        or left.shape[-1] not in {1, 3}
    ):
        raise ValueError(
            f"color sample shape mismatch: {left.shape} vs {right.shape}"
        )
    return np.max(np.abs(left - right), axis=-1)

def _forward_splat_weight(
    projected_x: np.ndarray,
    projected_y: np.ndarray,
    valid: np.ndarray,
    target_shape: tuple[int, int],
    viewport: Sequence[int],
    chunk_rows: int,
) -> np.ndarray:
    weight = np.zeros(target_shape, dtype=np.float32)
    for row0 in range(0, valid.shape[0], chunk_rows):
        row1 = min(valid.shape[0], row0 + chunk_rows)
        local_y, local_x = np.nonzero(valid[row0:row1])
        if local_y.size == 0:
            continue
        source_y = local_y + row0
        x = projected_x[source_y, local_x]
        y = projected_y[source_y, local_x]
        _splat_chunk(weight, None, x, y, None, viewport)
    return weight


def _forward_splat_values(
    values: np.ndarray,
    geometry_state: Mapping[str, Any],
) -> np.ndarray:
    target_shape = geometry_state["after_shape"]
    weighted_sum = np.zeros(target_shape, dtype=np.float32)
    valid = geometry_state["valid_reprojection"]
    projected_x = geometry_state["projected_x"]
    projected_y = geometry_state["projected_y"]
    chunk_rows = geometry_state["chunk_rows"]
    viewport = geometry_state["after_viewport"]
    for row0 in range(0, valid.shape[0], chunk_rows):
        row1 = min(valid.shape[0], row0 + chunk_rows)
        local_y, local_x = np.nonzero(valid[row0:row1])
        if local_y.size == 0:
            continue
        source_y = local_y + row0
        x = projected_x[source_y, local_x]
        y = projected_y[source_y, local_x]
        source_values = values[source_y, local_x]
        _splat_chunk(
            None,
            weighted_sum,
            x,
            y,
            source_values,
            viewport,
        )
    weight = geometry_state["splat_weight"]
    valid_weight = weight > 1.0e-8
    np.divide(weighted_sum, weight, out=weighted_sum, where=valid_weight)
    weighted_sum[~valid_weight] = np.nan
    return weighted_sum


def _splat_chunk(
    weight_target: np.ndarray | None,
    value_target: np.ndarray | None,
    x: np.ndarray,
    y: np.ndarray,
    values: np.ndarray | None,
    viewport: Sequence[int],
) -> None:
    vx, vy, vw, vh = viewport
    x0 = np.floor(x).astype(np.int64)
    y0 = np.floor(y).astype(np.int64)
    wx = (x - x0).astype(np.float32)
    wy = (y - y0).astype(np.float32)
    for target_x, target_y, contribution in (
        (x0, y0, (1.0 - wx) * (1.0 - wy)),
        (x0 + 1, y0, wx * (1.0 - wy)),
        (x0, y0 + 1, (1.0 - wx) * wy),
        (x0 + 1, y0 + 1, wx * wy),
    ):
        inside = (
            (target_x >= vx)
            & (target_x < vx + vw)
            & (target_y >= vy)
            & (target_y < vy + vh)
            & (contribution > 0.0)
        )
        if not np.any(inside):
            continue
        target_x = target_x[inside]
        target_y = target_y[inside]
        contribution = contribution[inside]
        if weight_target is not None:
            np.add.at(weight_target, (target_y, target_x), contribution)
        if value_target is not None and values is not None:
            np.add.at(
                value_target,
                (target_y, target_x),
                contribution * values[inside],
            )

def _evaluate_stage(
    stage_name: str,
    before_color: np.ndarray,
    after_color: np.ndarray,
    geometry_state: Mapping[str, Any],
    config: ShadowEdgeMetricsConfig,
) -> StageMetrics:
    del stage_name
    roi = geometry_state["roi"]
    before_luma = _log_luma(before_color, config.log_luma_epsilon)
    after_luma = _log_luma(after_color, config.log_luma_epsilon)
    before_warped = _forward_splat_values(before_luma, geometry_state)
    del before_luma
    finite_roi = roi.copy()
    finite_roi &= np.isfinite(before_warped)
    finite_roi &= np.isfinite(after_luma)
    finite_roi = _erode_mask(finite_roi, 2)
    roi_values = int(np.count_nonzero(finite_roi))
    minimum_roi = _scaled_pixel_minimum(
        geometry_state["after_shape"],
        config.min_roi_pixels_at_1080p,
    )
    if roi_values < minimum_roi:
        return StageMetrics(
            status=STATUS_INCONCLUSIVE,
            reasons=["insufficient finite stage pixels inside ROI"],
            edge_pixels_before=0,
            edge_pixels_after=0,
            edge_chamfer_before_to_after_p95_px=None,
            edge_chamfer_after_to_before_p95_px=None,
            edge_chamfer_p95_px=None,
            outside_1px_mismatch=None,
            normalized_residual_p99=None,
            residual_scale_log_luma=None,
            edge_gradient_threshold_before=None,
            edge_gradient_threshold_after=None,
        )

    before_edges, before_threshold = _soft_edge_mask(
        before_warped, finite_roi, config
    )
    after_edges, after_threshold = _soft_edge_mask(
        after_luma, finite_roi, config
    )
    before_count = int(np.count_nonzero(before_edges))
    after_count = int(np.count_nonzero(after_edges))
    minimum_edges = _scaled_pixel_minimum(
        geometry_state["after_shape"],
        config.min_edge_pixels_at_1080p,
    )

    q05, q95 = _masked_quantiles(
        (before_warped, after_luma),
        finite_roi,
        (0.05, 0.95),
        config.quantile_histogram_bins,
        geometry_state["chunk_rows"],
    )
    residual_scale = max(
        float(q95 - q05),
        float(config.residual_scale_floor),
    )
    residual_p99 = _difference_quantile(
        before_warped,
        after_luma,
        finite_roi,
        residual_scale,
        0.99,
        config.quantile_histogram_bins,
        geometry_state["chunk_rows"],
    )
    residual_pass = residual_p99 <= config.max_normalized_residual_p99

    if before_count < minimum_edges and after_count < minimum_edges:
        return StageMetrics(
            status=(
                STATUS_FAIL if not residual_pass else STATUS_INCONCLUSIVE
            ),
            reasons=(
                ["normalized residual exceeds release threshold"]
                if not residual_pass
                else ["too few soft-shadow edge pixels"]
            ),
            edge_pixels_before=before_count,
            edge_pixels_after=after_count,
            edge_chamfer_before_to_after_p95_px=None,
            edge_chamfer_after_to_before_p95_px=None,
            edge_chamfer_p95_px=None,
            outside_1px_mismatch=None,
            normalized_residual_p99=residual_p99,
            residual_scale_log_luma=residual_scale,
            edge_gradient_threshold_before=before_threshold,
            edge_gradient_threshold_after=after_threshold,
            checks={
                "normalized_residual_p99": residual_pass,
                "edge_support": False,
            },
        )

    if before_count == 0 or after_count == 0:
        before_to_after = (
            float(config.chamfer_search_radius_px + 1)
            if before_count
            else None
        )
        after_to_before = (
            float(config.chamfer_search_radius_px + 1)
            if after_count
            else None
        )
        chamfer = float(config.chamfer_search_radius_px + 1)
        mismatch = 1.0
    else:
        before_to_after = _edge_distance_p95(
            before_edges,
            after_edges,
            config.chamfer_search_radius_px,
        )
        after_to_before = _edge_distance_p95(
            after_edges,
            before_edges,
            config.chamfer_search_radius_px,
        )
        chamfer = max(before_to_after, after_to_before)
        mismatch = _outside_band_mismatch(
            before_edges, after_edges, radius=1
        )

    chamfer_pass = chamfer <= config.max_edge_chamfer_p95_px
    mismatch_pass = mismatch <= config.max_outside_1px_mismatch
    checks = {
        "edge_support": (
            before_count >= minimum_edges and after_count >= minimum_edges
        ),
        "edge_chamfer_p95_px": chamfer_pass,
        "outside_1px_mismatch": mismatch_pass,
        "normalized_residual_p99": residual_pass,
    }
    reasons = [
        name.replace("_", " ")
        for name, passed in checks.items()
        if passed is False
    ]
    status = STATUS_PASS if all(checks.values()) else STATUS_FAIL
    return StageMetrics(
        status=status,
        reasons=reasons,
        edge_pixels_before=before_count,
        edge_pixels_after=after_count,
        edge_chamfer_before_to_after_p95_px=before_to_after,
        edge_chamfer_after_to_before_p95_px=after_to_before,
        edge_chamfer_p95_px=chamfer,
        outside_1px_mismatch=mismatch,
        normalized_residual_p99=residual_p99,
        residual_scale_log_luma=residual_scale,
        edge_gradient_threshold_before=before_threshold,
        edge_gradient_threshold_after=after_threshold,
        checks=checks,
    )


def _interval_finite_values(
    block: np.ndarray,
    lower: float,
    upper: float,
    upper_inclusive: bool,
) -> np.ndarray:
    if block.size == 0:
        return np.empty(0, dtype=block.dtype)
    finite = block[np.isfinite(block)]
    if finite.size == 0:
        return finite

    # Histogram edges are float64. NumPy's weak scalar promotion can round a
    # Python float edge down to float32 when it is compared directly with a
    # float32 array, assigning an edge-adjacent sample to a different interval
    # than np.histogram. Promote only this bounded block for identical edge
    # membership while retaining the original values for exact selection.
    comparison_values = finite.astype(np.float64, copy=False)
    membership = comparison_values >= np.float64(lower)
    if upper_inclusive:
        membership &= comparison_values <= np.float64(upper)
    else:
        membership &= comparison_values < np.float64(upper)
    return finite[membership]


def _streaming_nearest_rank_quantile(
    block_factory: Any,
    quantile: float,
    bins: int,
    *,
    exact_value_limit: int = 262_144,
    max_refinements: int = 16,
) -> float:
    """Exact nearest-rank quantile with bounded histogram refinement.

    Only the selected histogram bin is retained between passes. Once that bin
    is small enough, ``partition`` selects the exact order statistic. This
    avoids both the memory cost of collecting the full ROI and the optimistic
    bias of returning a histogram-bin midpoint.
    """

    minimum = math.inf
    maximum = -math.inf
    count = 0
    for block in block_factory():
        if block.size == 0:
            continue
        finite = block[np.isfinite(block)]
        if finite.size == 0:
            continue
        minimum = min(minimum, float(np.min(finite)))
        maximum = max(maximum, float(np.max(finite)))
        count += int(finite.size)
    if count == 0:
        return float("nan")
    if minimum == maximum:
        return minimum

    requested_quantile = min(max(float(quantile), 0.0), 1.0)
    rank = max(1, min(count, int(math.ceil(requested_quantile * count))))
    active_lower = minimum
    active_upper = maximum
    active_upper_inclusive = True
    active_count = count
    bin_count = max(256, int(bins))
    exact_limit = max(1, int(exact_value_limit))

    for _ in range(max(1, int(max_refinements))):
        if active_count <= exact_limit:
            selected_parts = []
            selected_count = 0
            for block in block_factory():
                selected = _interval_finite_values(
                    block,
                    active_lower,
                    active_upper,
                    active_upper_inclusive,
                )
                if selected.size:
                    selected_parts.append(selected)
                    selected_count += int(selected.size)
            if selected_count != active_count or rank > selected_count:
                raise FloatingPointError(
                    "quantile refinement interval changed unexpectedly"
                )
            selected_values = (
                selected_parts[0].copy()
                if len(selected_parts) == 1
                else np.concatenate(selected_parts)
            )
            return float(np.partition(selected_values, rank - 1)[rank - 1])

        edges = np.linspace(
            active_lower,
            active_upper,
            bin_count + 1,
            dtype=np.float64,
        )
        if not np.all(np.isfinite(edges)) or not np.all(
            edges[1:] > edges[:-1]
        ):
            raise FloatingPointError(
                "quantile histogram cannot refine a finite interval"
            )

        histogram = np.zeros(bin_count, dtype=np.int64)
        observed_minimum = math.inf
        observed_maximum = -math.inf
        observed_count = 0
        for block in block_factory():
            selected = _interval_finite_values(
                block,
                active_lower,
                active_upper,
                active_upper_inclusive,
            )
            if selected.size == 0:
                continue
            histogram += np.histogram(selected, bins=edges)[0]
            observed_minimum = min(
                observed_minimum, float(np.min(selected))
            )
            observed_maximum = max(
                observed_maximum, float(np.max(selected))
            )
            observed_count += int(selected.size)

        if observed_count != active_count or rank > observed_count:
            raise FloatingPointError(
                "quantile histogram population changed unexpectedly"
            )
        if observed_minimum == observed_maximum:
            return observed_minimum

        cumulative = np.cumsum(histogram)
        selected_bin = int(
            np.searchsorted(cumulative, rank, side="left")
        )
        selected_bin = min(max(selected_bin, 0), bin_count - 1)
        count_before = (
            int(cumulative[selected_bin - 1]) if selected_bin else 0
        )
        rank -= count_before
        active_count = int(histogram[selected_bin])
        active_lower = float(edges[selected_bin])
        active_upper = float(edges[selected_bin + 1])
        if selected_bin != bin_count - 1:
            active_upper_inclusive = False

    # Never return an optimistic approximation for a release-gating metric.
    # A pathological distribution that defeats bounded refinement is an
    # evaluation error rather than a possible false pass.
    raise FloatingPointError(
        "quantile refinement did not reach an exact order statistic"
    )


def _histogram_quantiles(
    block_factory: Any,
    quantiles: Sequence[float],
    bins: int,
) -> tuple[float, ...]:
    return tuple(
        _streaming_nearest_rank_quantile(block_factory, quantile, bins)
        for quantile in quantiles
    )

def _masked_quantiles(
    arrays: Sequence[np.ndarray],
    mask: np.ndarray,
    quantiles: Sequence[float],
    bins: int,
    chunk_rows: int,
) -> tuple[float, ...]:
    def blocks():
        for array in arrays:
            for row0 in range(0, array.shape[0], chunk_rows):
                row1 = min(array.shape[0], row0 + chunk_rows)
                local_mask = mask[row0:row1]
                if np.any(local_mask):
                    yield array[row0:row1][local_mask]

    return _histogram_quantiles(blocks, quantiles, bins)


def _difference_quantile(
    before: np.ndarray,
    after: np.ndarray,
    mask: np.ndarray,
    scale: float,
    quantile: float,
    bins: int,
    chunk_rows: int,
) -> float:
    def blocks():
        for row0 in range(0, before.shape[0], chunk_rows):
            row1 = min(before.shape[0], row0 + chunk_rows)
            local_mask = mask[row0:row1]
            if not np.any(local_mask):
                continue
            difference = np.abs(
                before[row0:row1] - after[row0:row1]
            )
            yield difference[local_mask] / scale

    return _histogram_quantiles(blocks, (quantile,), bins)[0]

def _log_luma(color: np.ndarray, epsilon: float) -> np.ndarray:
    if color.ndim != 3 or color.shape[2] not in {1, 3}:
        raise ValueError(f"color must be canonical HxWx1/3, got {color.shape}")
    if color.shape[2] == 1:
        luma = np.array(color[..., 0], dtype=np.float32, copy=True)
    else:
        luma = np.multiply(color[..., 0], 0.2126, dtype=np.float32)
        luma += color[..., 1] * np.float32(0.7152)
        luma += color[..., 2] * np.float32(0.0722)
    np.maximum(luma, 0.0, out=luma)
    luma += np.float32(epsilon)
    np.log(luma, out=luma)
    return luma


def _soft_edge_mask(
    field: np.ndarray,
    roi: np.ndarray,
    config: ShadowEdgeMetricsConfig,
) -> tuple[np.ndarray, float]:
    smooth, smooth_valid = _masked_binomial_blur(field, roi)
    valid = roi.copy()
    valid &= smooth_valid
    valid &= np.isfinite(smooth)
    gx = np.zeros_like(smooth, dtype=np.float32)
    gy = np.zeros_like(smooth, dtype=np.float32)
    gx[:, 1:-1] = 0.5 * (smooth[:, 2:] - smooth[:, :-2])
    gy[1:-1, :] = 0.5 * (smooth[2:, :] - smooth[:-2, :])
    magnitude = np.empty_like(smooth, dtype=np.float32)
    np.hypot(gx, gy, out=magnitude)
    del smooth, smooth_valid
    if not np.any(valid):
        return np.zeros_like(roi), float(config.min_log_luma_edge_gradient)
    adaptive = _masked_quantiles(
        (magnitude,),
        valid,
        (config.adaptive_edge_quantile,),
        config.quantile_histogram_bins,
        config.projection_chunk_rows,
    )[0]
    threshold = max(
        float(config.min_log_luma_edge_gradient),
        adaptive * float(config.adaptive_edge_fraction),
    )
    candidates = valid
    candidates &= magnitude >= threshold
    edges = _nonmaximum_suppression(
        magnitude,
        gx,
        gy,
        candidates,
        config.projection_chunk_rows,
    )
    return edges, threshold

def _masked_binomial_blur(
    field: np.ndarray,
    mask: np.ndarray,
) -> tuple[np.ndarray, np.ndarray]:
    values = np.where(mask, field, 0.0).astype(np.float32, copy=False)
    weights = mask.astype(np.float32)
    for axis in (1, 0):
        values = _binomial_pass(values, axis)
        weights = _binomial_pass(weights, axis)
    smooth = np.divide(
        values,
        weights,
        out=np.full_like(values, np.nan),
        where=weights > 0.99,
    )
    return smooth, weights > 0.99


def _binomial_pass(array: np.ndarray, axis: int) -> np.ndarray:
    result = array * 2.0
    if axis == 1:
        result[:, 1:] += array[:, :-1]
        result[:, :-1] += array[:, 1:]
    else:
        result[1:, :] += array[:-1, :]
        result[:-1, :] += array[1:, :]
    return result * 0.25


def _nonmaximum_suppression(
    magnitude: np.ndarray,
    gx: np.ndarray,
    gy: np.ndarray,
    candidates: np.ndarray,
    chunk_rows: int,
) -> np.ndarray:
    """Four-direction NMS using row slices instead of eight full images."""

    height, width = magnitude.shape
    edges = np.zeros_like(candidates)
    if height < 3 or width < 3:
        return edges
    block_rows = max(1, int(chunk_rows))
    for row0 in range(1, height - 1, block_rows):
        row1 = min(height - 1, row0 + block_rows)
        rows = slice(row0, row1)
        up_rows = slice(row0 - 1, row1 - 1)
        down_rows = slice(row0 + 1, row1 + 1)
        center = magnitude[rows, 1:-1]
        gx_block = gx[rows, 1:-1]
        gy_block = gy[rows, 1:-1]
        candidate_block = candidates[rows, 1:-1]
        abs_x = np.abs(gx_block)
        abs_y = np.abs(gy_block)
        horizontal = abs_x >= 2.41421356 * abs_y
        vertical = abs_y >= 2.41421356 * abs_x
        diagonal = ~(horizontal | vertical)
        keep = np.zeros_like(candidate_block)
        keep |= (
            horizontal
            & (center >= magnitude[rows, :-2])
            & (center > magnitude[rows, 2:])
        )
        keep |= (
            vertical
            & (center >= magnitude[up_rows, 1:-1])
            & (center > magnitude[down_rows, 1:-1])
        )
        same_sign = np.signbit(gx_block) == np.signbit(gy_block)
        keep |= (
            diagonal
            & same_sign
            & (center >= magnitude[up_rows, :-2])
            & (center > magnitude[down_rows, 2:])
        )
        keep |= (
            diagonal
            & ~same_sign
            & (center >= magnitude[up_rows, 2:])
            & (center > magnitude[down_rows, :-2])
        )
        edges[rows, 1:-1] = candidate_block & keep
    return edges

def _edge_distance_p95(
    query_edges: np.ndarray,
    reference_edges: np.ndarray,
    search_radius: int,
) -> float:
    """Bounded exact pixel Chamfer p95 without storing all distances."""

    radius = max(1, int(search_radius))
    radius_squared = radius * radius
    sentinel_squared = (radius + 1) * (radius + 1)
    reference_rows = [
        np.flatnonzero(reference_edges[row]).astype(np.int32, copy=False)
        for row in range(reference_edges.shape[0])
    ]
    histogram = np.zeros(sentinel_squared + 1, dtype=np.int64)
    total = 0
    for query_y in range(query_edges.shape[0]):
        query_x = np.flatnonzero(query_edges[query_y]).astype(
            np.int32, copy=False
        )
        if query_x.size == 0:
            continue
        best_squared = np.full(
            query_x.shape, sentinel_squared, dtype=np.int32
        )
        source_y0 = max(0, query_y - radius)
        source_y1 = min(reference_edges.shape[0], query_y + radius + 1)
        for source_y in range(source_y0, source_y1):
            source_x = reference_rows[source_y]
            if source_x.size == 0:
                continue
            insertion = np.searchsorted(source_x, query_x)
            right_index = np.minimum(insertion, source_x.size - 1)
            left_index = np.maximum(insertion - 1, 0)
            dx = np.minimum(
                np.abs(source_x[right_index] - query_x),
                np.abs(source_x[left_index] - query_x),
            ).astype(np.int32, copy=False)
            dy = source_y - query_y
            candidate_squared = dx * dx + dy * dy
            best_squared = np.minimum(best_squared, candidate_squared)
        best_squared[best_squared > radius_squared] = sentinel_squared
        histogram += np.bincount(
            best_squared, minlength=sentinel_squared + 1
        )
        total += int(best_squared.size)
    if total == 0:
        return float(radius + 1)
    target = max(1, int(math.ceil(0.95 * total)))
    squared_distance = int(
        np.searchsorted(np.cumsum(histogram), target, side="left")
    )
    return math.sqrt(float(squared_distance))

def _outside_band_mismatch(
    before_edges: np.ndarray,
    after_edges: np.ndarray,
    radius: int,
) -> float:
    before_count = int(np.count_nonzero(before_edges))
    after_count = int(np.count_nonzero(after_edges))
    total = before_count + after_count
    if total == 0:
        return 0.0
    after_band = _dilate_mask(after_edges, radius)
    before_band = _dilate_mask(before_edges, radius)
    unmatched = int(np.count_nonzero(before_edges & ~after_band))
    unmatched += int(np.count_nonzero(after_edges & ~before_band))
    return float(unmatched / total)


def _dilate_mask(mask: np.ndarray, radius: int) -> np.ndarray:
    radius = max(0, int(radius))
    result = mask.copy()
    if radius == 0:
        return result
    height, width = mask.shape
    for dy in range(-radius, radius + 1):
        for dx in range(-radius, radius + 1):
            if max(abs(dx), abs(dy)) > radius:
                continue
            source_y0 = max(0, -dy)
            source_y1 = height - max(0, dy)
            source_x0 = max(0, -dx)
            source_x1 = width - max(0, dx)
            target_y0 = max(0, dy)
            target_y1 = height - max(0, -dy)
            target_x0 = max(0, dx)
            target_x1 = width - max(0, -dx)
            result[target_y0:target_y1, target_x0:target_x1] |= mask[
                source_y0:source_y1, source_x0:source_x1
            ]
    return result


def _erode_mask(mask: np.ndarray, radius: int) -> np.ndarray:
    radius = max(0, int(radius))
    if radius == 0:
        return mask.copy()
    result = ~_dilate_mask(~mask, radius)
    result[:radius, :] = False
    result[-radius:, :] = False
    result[:, :radius] = False
    result[:, -radius:] = False
    return result


def _scaled_pixel_minimum(
    shape: tuple[int, int],
    pixels_at_1080p: int,
) -> int:
    reference_diagonal = math.hypot(1920.0, 1080.0)
    diagonal = math.hypot(float(shape[1]), float(shape[0]))
    return max(1, int(math.ceil(pixels_at_1080p * diagonal / reference_diagonal)))


__all__ = [
    "CameraMatrices",
    "FRAMEBUFFER_TOP_TO_NDC_NEGATIVE_ONE",
    "FRAMEBUFFER_TOP_TO_NDC_POSITIVE_ONE",
    "ShadowEdgeMetricsConfig",
    "ShadowEdgeMetricsResult",
    "ShadowFrame",
    "StageMetrics",
    "STATUS_ERROR",
    "STATUS_FAIL",
    "STATUS_INCONCLUSIVE",
    "STATUS_PASS",
    "evaluate_shadow_edge_metrics",
]
