#!/usr/bin/env python3
"""Compare CSM motion in RenderDoc captures without hard-coded IDs.

The file is intentionally dual-purpose:

* Executed normally, it accepts either two captures or an mgif motion-smoke
  manifest, manages isolated ``rdc`` sessions, and emits a JSON report.
* Executed by ``rdc script``, it runs inside the RenderDoc replay daemon and
  performs capture discovery/export through the RenderDoc Python API.

No capture is modified. Every session opened by the host side is closed in a
``finally`` block.
"""

from __future__ import annotations

import argparse
import ast
import ctypes
import hashlib
import importlib.metadata
import json
import math
import os
import re
import shutil
import signal
import stat
import subprocess
import sys
import tempfile
import time
import traceback
import uuid
from datetime import datetime, timezone
from pathlib import Path
from typing import Any, Iterable


TOOL_VERSION = "2.9"
TOOLCHAIN_EVIDENCE_SCHEMA = "mgif-renderdoc-toolchain-evidence-v1"
TOOL_FILE_EVIDENCE_SCHEMA = "mgif-renderdoc-tool-file-evidence-v1"
RUNTIME_DEPENDENCY_EVIDENCE_SCHEMA = "mgif-renderdoc-runtime-dependency-evidence-v1"
STATE_PUBLICATION_BOUNDARY_SCHEMA = "mgif-rdc-state-publication-boundary-v2"
WINDOWS_FILETIME_TICK_NS = 100
STATE_VOLUME_EVIDENCE_SCHEMA = "mgif-rdc-state-volume-evidence-v1"
STATE_HANDLE_SNAPSHOT_SCHEMA = "mgif-rdc-state-file-handle-snapshot-v1"
RDC_CLI_SESSION_ORDERING_SCHEMA = "mgif-rdc-cli-session-ordering-v1"
WINDOWS_FILETIME_EPOCH_OFFSET_TICKS = 11644473600 * 10_000_000
SUPPORTED_STATE_FILE_SYSTEMS = frozenset({"NTFS", "REFS"})
WINDOWS_DRIVE_FIXED = 3
WINDOWS_VOLUME_NAME_GUID = 0x1
WINDOWS_FILE_NAME_NORMALIZED = 0x0
WINDOWS_FINAL_PATH_FLAGS = WINDOWS_VOLUME_NAME_GUID | WINDOWS_FILE_NAME_NORMALIZED
TOOLCHAIN_REQUIRED_ROLES = (
    "harness",
    "comparator",
    "roi",
    "rdc_cli_package",
    "renderdoc_module",
)
NPY_EXPORT_EVIDENCE_SCHEMA = "mgif-renderdoc-npy-export-evidence-v1"
RELEASE_INPUTS_SCHEMA = "mgif-shadow-edge-extractor-inputs-v2"
DEFAULT_TOTAL_TIMEOUT_SECONDS = 7200.0
CAPTURE_CLEANUP_RESERVE_SECONDS = 90.0
CAPTURE_EMERGENCY_CLEANUP_TIMEOUT_SECONDS = 30.0
COMPARATOR_EXTRACT_BYTES_PER_CAPTURE = 1536 * 1024 * 1024
COMPARATOR_SESSION_BYTES_PER_CAPTURE = 256 * 1024 * 1024
COMPARATOR_MAX_RDC_BYTES = 1024 * 1024 * 1024
SMOKE_MAX_CANDIDATES_PER_BOUNDARY = 2
COMPARATOR_MAX_READBACK_BYTES = 512 * 1024 * 1024
NPY_HEADER_RESERVE_BYTES = 64 * 1024
DISK_SAFETY_MIN_BYTES = 4 * 1024 * 1024 * 1024
DISK_SAFETY_FRACTION = 0.25
ROLE_ORDER = ("csm", "light", "taa", "final")
SMOKE_MANIFEST_SCHEMA = "mgif-csm-shadow-motion-smoke-v1"
FINAL_CAPTURE_SET_SCHEMA = "mgif-csm-final-capture-set-v1"
SMOKE_BOUNDARIES = ("last-moving", "first-still", "settled")
SAME_POSE_PAIRS = (
    ("last-moving", "first-still"),
    ("first-still", "settled"),
)
DEFAULT_POSE_TOLERANCE = 1.0e-5
DEFAULT_MIN_CSM_ACTIVE_PIXELS = 64
DEFAULT_MIN_CSM_ACTIVE_FRACTION = 1.0e-5
RELEASE_SMOKE_SEQUENCE = {
    "warmup_frames": 8,
    "motion_frames": 24,
    "hold_frames": 8,
}
RELEASE_BOUNDARY_FRAMES = {
    "last-moving": (
        RELEASE_SMOKE_SEQUENCE["warmup_frames"]
        + RELEASE_SMOKE_SEQUENCE["motion_frames"]
        - 1
    ),
    "first-still": (
        RELEASE_SMOKE_SEQUENCE["warmup_frames"]
        + RELEASE_SMOKE_SEQUENCE["motion_frames"]
    ),
    "settled": (
        RELEASE_SMOKE_SEQUENCE["warmup_frames"]
        + RELEASE_SMOKE_SEQUENCE["motion_frames"]
        + RELEASE_SMOKE_SEQUENCE["hold_frames"]
        - 1
    ),
}
AUTOMATION_FRAME_MARKER_RE = re.compile(
    r"^CSM_AUTOMATION_FRAME\s+mode=(?P<mode>[^\s]+)\s+"
    r"boundary=(?P<boundary>last-moving|first-still|settled)\s+"
    r"frame=(?P<frame>[0-9]+)$"
)
TAA_HISTORY_RESOURCE_RE = re.compile(r"^GPUDrivenSceneColorHistory(?P<index>[01])$")
RELEASE_RESOURCE_NAMES = {
    "scene_depth": "SceneDepth",
    "base_color": "SceneColor0",
    "packed_normal": "SceneColor1",
    "scene_color_hdr": "GPUDrivenSceneColorHDR",
    "velocity": "GPUDrivenVelocity",
    "final": "OutputTexture",
}
RELEASE_CASE_MODES = ("csm-translate-stop", "csm-rotate-stop")
RELEASE_RENDER_MODES = ("taa-on", "no-post")

RELEASE_CANONICAL_THRESHOLDS = {
    "max_shift": 128,
    "max_csm_registration_shift": 0,
    "max_csm_matrix_delta": 1.0e-5,
    "pose_tolerance": 1.0e-5,
    "depth_epsilon": 1.0e-6,
    "color_epsilon": 1.0e-3,
    "max_csm_residual_fraction": 0.001,
    "max_csm_residual_mae": 1.0e-5,
    "max_csm_active_changed_fraction": 0.001,
    "max_csm_foreground_mismatch_fraction": 0.0001,
    "max_csm_max_abs": 1.0e-4,
    "min_csm_active_pixels": 64,
    "min_csm_active_fraction": 1.0e-5,
    "max_csm_center_motion_texels": 1.0e-3,
    "max_light_residual_fraction": 0.01,
    "max_taa_residual_fraction": 0.01,
    "max_final_residual_fraction": 0.01,
    "max_light_max_abs": 0.01,
    "max_taa_max_abs": 0.01,
    "max_final_max_abs": 0.01,
    "power2_tolerance": 0.05,
    "grid_phase_tolerance": 0.10,
    "cascade_blend_fraction": 0.10,
}
RELEASE_ROI_CONFIG = {
    "min_valid_reprojection_ratio": 0.95,
    "max_edge_chamfer_p95_px": 0.25,
    "max_outside_1px_mismatch": 0.005,
    "max_normalized_residual_p99": 0.05,
    "world_up": (0.0, 1.0, 0.0),
    "max_receiver_tilt_degrees": 25.0,
    "max_reprojection_normal_degrees": 12.0,
    "world_reprojection_abs_tolerance": 1.0e-3,
    "world_reprojection_relative_tolerance": 2.0e-4,
    "world_reprojection_pixel_tolerance": 0.25,
    "base_color_reprojection_tolerance": 0.08,
    "linear_depth_edge_abs_threshold": 0.02,
    "linear_depth_edge_relative_threshold": 0.02,
    "normal_edge_degrees": 18.0,
    "base_color_edge_threshold": 0.08,
    "geometry_edge_margin_px": 2,
    "min_receiver_fraction": 0.01,
    "min_roi_fraction": 0.005,
    "min_receiver_pixels_at_1080p": 512,
    "min_roi_pixels_at_1080p": 256,
    "min_edge_pixels_at_1080p": 16,
    "min_splat_weight": 0.25,
    "min_log_luma_edge_gradient": 0.005,
    "adaptive_edge_quantile": 0.99,
    "adaptive_edge_fraction": 0.25,
    "log_luma_epsilon": 1.0e-4,
    "residual_scale_floor": 0.25,
    "chamfer_search_radius_px": 8,
    "projection_chunk_rows": 128,
    "quantile_histogram_bins": 4096,
}
EXECUTABLE_BINDING_SCHEMA = "mgif-executable-evidence-binding-v2"
RDC_CLEANUP_SCHEMA = "rdc-session-cleanup-v2"
RENDERDOC_AUTOGENERATED_RESOURCE_NAME_RE = re.compile(
    r"^(?P<base>"
    r"(?:(?:1D|2D|3D|Cube)(?:\s+Array)?\s+)?"
    r"(?:Image|Texture|Color Attachment|Depth Attachment|Stencil Attachment|"
    r"Depth(?:/|[-\s])Stencil Attachment|Swapchain Image)"
    r")\s+(?P<resource_id>[0-9]+)$",
    re.IGNORECASE,
)


# ---------------------------------------------------------------------------
# RenderDoc daemon-side extraction. These functions are only called when this
# file is executed by ``rdc script`` and the injected globals are present.
# ---------------------------------------------------------------------------


def _d_enum_name(value: Any) -> str:
    try:
        from rdc.handlers._helpers import _enum_name

        return str(_enum_name(value))
    except Exception:
        text = str(value)
        return text.rsplit(".", 1)[-1]


def _d_action_name(action: Any) -> str:
    custom = str(getattr(action, "customName", "") or "")
    if custom:
        return custom
    try:
        return str(action.GetName(state.structured_file))
    except Exception:
        return f"EID {int(getattr(action, 'eventId', 0))}"


def _d_walk_actions(
    actions: Iterable[Any],
    ancestors: tuple[str, ...] = (),
    path: tuple[int, ...] = (),
) -> Iterable[tuple[Any, tuple[str, ...], tuple[int, ...]]]:
    for index, action in enumerate(actions):
        name = _d_action_name(action)
        current_path = path + (index,)
        yield action, ancestors, current_path
        yield from _d_walk_actions(
            getattr(action, "children", []),
            ancestors + (name,),
            current_path,
        )


def _d_action_event_ids(action: Any) -> list[int]:
    values = [
        int(getattr(event, "eventId", 0))
        for event in getattr(action, "events", [])
        if int(getattr(event, "eventId", 0)) > 0
    ]
    event_id = int(getattr(action, "eventId", 0))
    if event_id > 0:
        values.append(event_id)
    for child in getattr(action, "children", []):
        values.extend(_d_action_event_ids(child))
    return values


def _d_marker_record(
    action: Any,
    ancestors: tuple[str, ...],
    path: tuple[int, ...],
) -> dict[str, Any]:
    event_ids = _d_action_event_ids(action)
    return {
        "_action": action,
        "name": _d_action_name(action),
        "path": list(path),
        "ancestors": list(ancestors),
        "event_id": int(getattr(action, "eventId", 0)),
        "begin_eid": min(event_ids) if event_ids else 0,
        "end_eid": max(event_ids) if event_ids else 0,
        "event_count": len(set(event_ids)),
    }


def _d_marker_kind(name: str) -> str | None:
    compact = re.sub(r"[^a-z0-9]+", "", name.lower())
    if "csm" in compact or (
        "shadow" in compact and ("cascade" in compact or "directional" in compact)
    ):
        return "csm"
    if (
        "lightpass" in compact
        or "deferredlighting" in compact
        or "lightingpass" in compact
    ) and "culling" not in compact:
        return "light"
    if "taa" in compact or (
        "temporal" in compact
        and ("resolve" in compact or "antialias" in compact)
    ):
        return "taa"
    if (
        "finalcolor" in compact
        or "finalcolour" in compact
        or "tonemap" in compact
        or "compositefinal" in compact
    ):
        return "final"
    if "present" in compact or "swapchain" in compact:
        return "present"
    return None


def _d_collect_automation_frame_markers() -> list[dict[str, Any]]:
    rows: list[dict[str, Any]] = []
    for action, ancestors, path in _d_walk_actions(controller.GetRootActions()):
        name = _d_action_name(action).strip()
        match = AUTOMATION_FRAME_MARKER_RE.fullmatch(name)
        if match is None:
            continue
        compact_ancestors = [
            re.sub(r"[^a-z0-9]+", "", ancestor.lower())
            for ancestor in ancestors
        ]
        rows.append(
            {
                "name": name,
                "mode": match.group("mode"),
                "boundary": match.group("boundary"),
                "frame": int(match.group("frame")),
                "path": list(path),
                "ancestors": list(ancestors),
                "event_id": int(getattr(action, "eventId", 0)),
                "nested_in_gpu_driven_csm_shadow": any(
                    "gpudrivencsmshadow" in ancestor
                    for ancestor in compact_ancestors
                ),
            }
        )
    return rows


def _d_choose_markers() -> tuple[dict[str, dict[str, Any]], list[dict[str, Any]]]:
    candidates: dict[str, list[dict[str, Any]]] = {
        "csm": [],
        "light": [],
        "taa": [],
        "final": [],
        "present": [],
    }
    public_rows: list[dict[str, Any]] = []
    for action, ancestors, path in _d_walk_actions(controller.GetRootActions()):
        kind = _d_marker_kind(_d_action_name(action))
        if kind is None:
            continue
        row = _d_marker_record(action, ancestors, path)
        candidates[kind].append(row)
        public_rows.append({key: value for key, value in row.items() if key != "_action"})

    selected: dict[str, dict[str, Any]] = {}
    for kind, rows in candidates.items():
        if not rows:
            continue
        # TAA release evidence must bind the inner resolve marker. Other roles
        # keep the existing widest-marker preference.
        def marker_rank(row: dict[str, Any]) -> tuple[int, int, int, int]:
            exact_inner_taa = int(
                kind == "taa" and row["name"].strip() == "GPUDrivenTAAResolve"
            )
            return (
                exact_inner_taa,
                row["event_count"],
                row["end_eid"] - row["begin_eid"],
                row["end_eid"],
            )

        best_rank = max(marker_rank(row) for row in rows)
        tied = [row for row in rows if marker_rank(row) == best_rank]
        selected[kind] = max(
            tied,
            key=lambda row: (tuple(row["path"]), row["name"]),
        )
        selected[kind]["selection"] = {
            "candidate_count": len(rows),
            "top_tie_count": len(tied),
            "ambiguous": len(tied) != 1,
            "rank": list(best_rank),
            "tied_candidates": [
                {
                    "name": row["name"],
                    "path": row["path"],
                    "event_id": row["event_id"],
                }
                for row in tied
            ],
        }
    return selected, public_rows


def _d_flag(action: Any, name: str) -> bool:
    flag_value = getattr(getattr(rd, "ActionFlags", object()), name, None)
    if flag_value is None:
        return False
    try:
        return bool(action.flags & flag_value)
    except Exception:
        return False


def _d_is_draw(action: Any) -> bool:
    name = _d_action_name(action).lower()
    return _d_flag(action, "Drawcall") or (
        "draw" in name and "begin" not in name and "end" not in name
    )


def _d_is_dispatch(action: Any) -> bool:
    return _d_flag(action, "Dispatch") or "dispatch" in _d_action_name(action).lower()


def _d_is_copy(action: Any) -> bool:
    name = _d_action_name(action).lower()
    return (
        _d_flag(action, "Copy")
        or "blit" in name
        or "copyimage" in name
        or "resolveimage" in name
    )


def _d_is_begin_pass(action: Any) -> bool:
    name = _d_action_name(action).lower()
    return _d_flag(action, "BeginPass") or "beginrender" in name or "beginpass" in name


def _d_is_end_pass(action: Any) -> bool:
    name = _d_action_name(action).lower()
    return _d_flag(action, "EndPass") or "endrender" in name or "endpass" in name


def _d_flat_actions(action: Any) -> list[Any]:
    rows: list[Any] = []

    def walk(current: Any) -> None:
        rows.append(current)
        for child in getattr(current, "children", []):
            walk(child)

    walk(action)
    rows.sort(key=lambda item: int(getattr(item, "eventId", 0)))
    return rows


def _d_pass_segments(marker: dict[str, Any]) -> list[dict[str, Any]]:
    segments: list[dict[str, Any]] = []
    active: dict[str, Any] | None = None
    for action in _d_flat_actions(marker["_action"]):
        eid = int(getattr(action, "eventId", 0))
        if _d_is_begin_pass(action):
            if active is not None and active.get("last_draw_eid"):
                segments.append(active)
            active = {
                "begin_eid": eid,
                "end_eid": eid,
                "last_draw_eid": None,
                "draw_count": 0,
            }
        elif active is not None and _d_is_draw(action):
            active["last_draw_eid"] = eid
            active["draw_count"] += 1
            active["end_eid"] = eid
        elif active is not None and _d_is_end_pass(action):
            active["end_eid"] = eid
            if active.get("last_draw_eid"):
                segments.append(active)
            active = None
    if active is not None and active.get("last_draw_eid"):
        segments.append(active)
    return segments


def _d_executable_eids(marker: dict[str, Any]) -> list[int]:
    eids: set[int] = set()
    for segment in _d_pass_segments(marker):
        if segment["last_draw_eid"]:
            eids.add(int(segment["last_draw_eid"]))
    for action in _d_flat_actions(marker["_action"]):
        if _d_is_dispatch(action) or _d_is_copy(action):
            eids.add(int(getattr(action, "eventId", 0)))
    if not eids and marker["event_id"]:
        eids.add(int(marker["event_id"]))
    return sorted(eid for eid in eids if eid > 0)


def _d_texture_metadata(resource_id: int) -> dict[str, Any]:
    texture = state.tex_map.get(int(resource_id))
    description = getattr(state, "res_rid_map", {}).get(int(resource_id))
    autogenerated_name = getattr(description, "autogeneratedName", None)
    if texture is None:
        return {
            "resource_id": int(resource_id),
            "name": state.res_names.get(int(resource_id), ""),
            "autogenerated_name": (
                bool(autogenerated_name)
                if autogenerated_name is not None
                else None
            ),
            "is_texture": False,
        }
    fmt = texture.format
    return {
        "resource_id": int(resource_id),
        "name": state.res_names.get(int(resource_id), ""),
        "autogenerated_name": (
            bool(autogenerated_name)
            if autogenerated_name is not None
            else None
        ),
        "is_texture": True,
        "width": int(texture.width),
        "height": int(texture.height),
        "depth": int(getattr(texture, "depth", 1)),
        "array_size": int(getattr(texture, "arraysize", 1)),
        "mips": int(texture.mips),
        "samples": int(getattr(texture, "msSamp", 1)),
        "byte_size": int(getattr(texture, "byteSize", 0)),
        "format": fmt.Name() if hasattr(fmt, "Name") else str(fmt),
        "format_type": _d_enum_name(getattr(fmt, "type", "")),
        "format_type_value": int(getattr(fmt, "type", 0)),
        "component_type": _d_enum_name(getattr(fmt, "compType", "")),
        "component_type_value": int(getattr(fmt, "compType", 0)),
        "component_count": int(getattr(fmt, "compCount", 0)),
        "component_byte_width": int(getattr(fmt, "compByteWidth", 0)),
        "bgra": bool(fmt.BGRAOrder()) if hasattr(fmt, "BGRAOrder") else False,
        "srgb": bool(fmt.SRGBCorrected()) if hasattr(fmt, "SRGBCorrected") else False,
        "creation_flags": int(getattr(texture, "creationFlags", 0)),
    }


def _d_view_record(view: Any) -> dict[str, Any] | None:
    resource_id = int(getattr(view, "resource", 0))
    if resource_id == 0:
        return None
    row = _d_texture_metadata(resource_id)
    row.update(
        {
            "first_mip": int(getattr(view, "firstMip", 0)),
            "first_slice": int(getattr(view, "firstSlice", 0)),
            "num_mips": int(getattr(view, "numMips", 1)),
            "num_slices": int(getattr(view, "numSlices", 1)),
        }
    )
    return row


def _d_reflection_resource_record(
    reflections: dict[int, dict[str, list[Any]]],
    access: Any,
) -> dict[str, Any]:
    type_name = _d_enum_name(access.type)
    category = "rw" if type_name.startswith("ReadWrite") else "ro"
    resources = reflections.get(int(access.stage), {}).get(category, [])
    index = int(access.index)
    reflected = resources[index] if 0 <= index < len(resources) else None
    return {
        "binding": str(getattr(reflected, "name", "") or "") if reflected else "",
        "reflection_index": index,
        "fixed_bind_number": (
            int(getattr(reflected, "fixedBindNumber", -1)) if reflected else -1
        ),
        "fixed_bind_set_or_space": (
            int(getattr(reflected, "fixedBindSetOrSpace", -1)) if reflected else -1
        ),
        "bind_array_size": (
            int(getattr(reflected, "bindArraySize", 0)) if reflected else 0
        ),
    }


def _d_reflection_resource_name(
    reflections: dict[int, dict[str, list[Any]]],
    access: Any,
) -> str:
    return str(_d_reflection_resource_record(reflections, access)["binding"])


def _d_pipeline_snapshot(eid: int) -> dict[str, Any]:
    controller.SetFrameEvent(int(eid), True)
    pipe = controller.GetPipelineState()
    depth = _d_view_record(pipe.GetDepthTarget())
    outputs = []
    for target_index, target in enumerate(pipe.GetOutputTargets()):
        row = _d_view_record(target)
        if row is not None:
            row["target_index"] = int(target_index)
            outputs.append(row)

    reflections: dict[int, dict[str, list[Any]]] = {}
    for stage_value in range(6):
        reflection = pipe.GetShaderReflection(stage_value)
        if reflection is None:
            continue
        reflections[int(stage_value)] = {
            "ro": list(getattr(reflection, "readOnlyResources", [])),
            "rw": list(getattr(reflection, "readWriteResources", [])),
        }

    descriptors = []
    if hasattr(pipe, "GetAllUsedDescriptors"):
        for used in pipe.GetAllUsedDescriptors(True):
            descriptor = used.descriptor
            resource_id = int(getattr(descriptor, "resource", 0))
            if resource_id == 0:
                continue
            access = used.access
            reflection_record = _d_reflection_resource_record(reflections, access)
            descriptors.append(
                {
                    "stage": _d_enum_name(access.stage),
                    "stage_value": int(access.stage),
                    "type": _d_enum_name(access.type),
                    "array_element": int(getattr(access, "arrayElement", -1)),
                    **reflection_record,
                    "resource": _d_texture_metadata(resource_id),
                }
            )

    shaders = {}
    for stage_name, stage_value in (
        ("vs", getattr(rd.ShaderStage, "Vertex", 0)),
        ("ps", getattr(rd.ShaderStage, "Fragment", 4)),
        ("cs", getattr(rd.ShaderStage, "Compute", 5)),
    ):
        shader_id = int(pipe.GetShader(stage_value))
        if shader_id:
            shaders[stage_name] = {
                "resource_id": shader_id,
                "entry_point": str(pipe.GetShaderEntryPoint(stage_value) or ""),
            }
    try:
        graphics_pipeline = int(pipe.GetGraphicsPipelineObject())
    except Exception:
        graphics_pipeline = 0

    return {
        "eid": int(eid),
        "depth": depth,
        "outputs": outputs,
        "descriptors": descriptors,
        "shaders": shaders,
        "graphics_pipeline": graphics_pipeline,
    }


def _d_usage_name(value: Any) -> str:
    return _d_enum_name(value)


def _d_is_write_usage(name: str) -> bool:
    lower = name.lower()
    compact = re.sub(r"[^a-z0-9]+", "", lower)
    return any(
        token in lower
        for token in (
            "write",
            "target",
            "depthstencil",
            "copydst",
            "resolvedst",
            "clear",
            "discard",
        )
    ) or any(
        token in compact
        for token in (
            "rwresource",
            "readwrite",
            "storageimage",
            "unorderedaccess",
            "uav",
        )
    )


def _d_is_read_usage(name: str) -> bool:
    lower = name.lower()
    compact = re.sub(r"[^a-z0-9]+", "", lower)
    return any(
        token in lower
        for token in ("read", "sample", "copysrc", "resolvesrc", "indirect")
    ) or any(
        token in compact
        for token in (
            "rwresource",
            "readwrite",
            "storageimage",
            "unorderedaccess",
            "uav",
        )
    ) or compact.endswith("resource")


def _d_build_usage_map() -> dict[int, list[dict[str, Any]]]:
    usage_map: dict[int, list[dict[str, Any]]] = {}
    for resource_id, texture in state.tex_map.items():
        entries = []
        for usage in controller.GetUsage(texture.resourceId):
            entries.append(
                {
                    "eid": int(usage.eventId),
                    "usage": _d_usage_name(usage.usage),
                }
            )
        if entries:
            usage_map[int(resource_id)] = entries
    return usage_map


def _d_entries_in_span(
    usage_map: dict[int, list[dict[str, Any]]],
    resource_id: int,
    marker: dict[str, Any] | None,
) -> list[dict[str, Any]]:
    if marker is None:
        return []
    return [
        entry
        for entry in usage_map.get(int(resource_id), [])
        if marker["begin_eid"] <= entry["eid"] <= marker["end_eid"]
    ]


def _d_written_in(
    usage_map: dict[int, list[dict[str, Any]]],
    resource_id: int,
    marker: dict[str, Any] | None,
) -> bool:
    return any(
        _d_is_write_usage(entry["usage"])
        for entry in _d_entries_in_span(usage_map, resource_id, marker)
    )


def _d_read_in(
    usage_map: dict[int, list[dict[str, Any]]],
    resource_id: int,
    marker: dict[str, Any] | None,
) -> bool:
    return any(
        _d_is_read_usage(entry["usage"])
        for entry in _d_entries_in_span(usage_map, resource_id, marker)
    )


def _d_latest_write_eid(
    usage_map: dict[int, list[dict[str, Any]]],
    resource_id: int,
    marker: dict[str, Any] | None,
    fallback: int,
) -> int:
    writes = [
        entry["eid"]
        for entry in _d_entries_in_span(usage_map, resource_id, marker)
        if _d_is_write_usage(entry["usage"])
    ]
    return max(writes) if writes else int(fallback)


def _d_depth_like(meta: dict[str, Any]) -> bool:
    text = f"{meta.get('format', '')} {meta.get('component_type', '')}".lower()
    return "depth" in text or re.search(r"(^|[^a-z])d(16|24|32)", text) is not None


def _d_color_like(meta: dict[str, Any]) -> bool:
    return bool(meta.get("is_texture")) and not _d_depth_like(meta)


def _d_collect_pipeline_data(
    markers: dict[str, dict[str, Any]],
) -> tuple[dict[str, list[dict[str, Any]]], dict[str, list[dict[str, Any]]]]:
    snapshots: dict[str, list[dict[str, Any]]] = {}
    segments: dict[str, list[dict[str, Any]]] = {}
    for kind, marker in markers.items():
        eids = _d_executable_eids(marker)
        action_by_eid = {
            int(getattr(action, "eventId", 0)): action
            for action in _d_flat_actions(marker["_action"])
            if int(getattr(action, "eventId", 0)) > 0
        }
        snapshots[kind] = []
        for eid in eids:
            try:
                snapshot = _d_pipeline_snapshot(eid)
                action = action_by_eid.get(int(eid))
                if action is not None:
                    instance_count_raw = int(getattr(action, "numInstances", 0) or 0)
                    snapshot["action"] = {
                        "name": _d_action_name(action),
                        "is_draw": _d_is_draw(action),
                        "vertex_count": int(getattr(action, "numIndices", 0) or 0),
                        "instance_count_raw": instance_count_raw,
                        "instance_count": instance_count_raw,
                    }
                snapshots[kind].append(snapshot)
            except Exception as exc:
                snapshots[kind].append({"eid": eid, "error": str(exc)})
        segments[kind] = _d_pass_segments(marker)
    return snapshots, segments


def _d_pick_csm(
    markers: dict[str, dict[str, Any]],
    snapshots: dict[str, list[dict[str, Any]]],
    usage_map: dict[int, list[dict[str, Any]]],
) -> tuple[dict[str, Any] | None, list[dict[str, Any]]]:
    marker = markers.get("csm")
    if marker is None:
        return None, []
    scores: dict[int, float] = {}
    reasons: dict[int, list[str]] = {}
    cascade_views: list[dict[str, Any]] = []

    for snapshot in snapshots.get("csm", []):
        depth = snapshot.get("depth")
        if not depth:
            continue
        resource_id = int(depth["resource_id"])
        scores[resource_id] = scores.get(resource_id, 0.0) + 100.0
        reasons.setdefault(resource_id, []).append(
            f"depth attachment at EID {snapshot['eid']}"
        )
        view = {
            "eid": int(snapshot["eid"]),
            "resource_id": resource_id,
            "first_slice": int(depth.get("first_slice", 0)),
            "num_slices": int(depth.get("num_slices", 1)),
        }
        cascade_views.append(view)

    light_descriptors = [
        descriptor
        for snapshot in snapshots.get("light", [])
        for descriptor in snapshot.get("descriptors", [])
    ]
    for resource_id, meta_source in state.tex_map.items():
        meta = _d_texture_metadata(int(resource_id))
        score = scores.get(int(resource_id), 0.0)
        local_reasons = reasons.setdefault(int(resource_id), [])
        if _d_depth_like(meta):
            score += 20.0
        if int(meta.get("array_size", 1)) > 1:
            score += 10.0 + min(16.0, float(meta["array_size"]))
        if _d_written_in(usage_map, int(resource_id), marker):
            score += 30.0
            local_reasons.append("written inside CSM marker span")
        for descriptor in light_descriptors:
            if int(descriptor["resource"]["resource_id"]) != int(resource_id):
                continue
            binding = descriptor.get("binding", "").lower()
            score += 80.0
            local_reasons.append(
                f"read by light pass binding '{descriptor.get('binding', '')}'"
            )
            if "shadow" in binding or "csm" in binding or "cascade" in binding:
                score += 80.0
        if score > 0:
            scores[int(resource_id)] = score

    if not scores:
        return None, []
    best_score = max(scores.values())
    tied_resource_ids = sorted(
        resource_id
        for resource_id, score in scores.items()
        if score == best_score
    )
    resource_id = tied_resource_ids[0]
    selected_views = [
        view for view in cascade_views if view["resource_id"] == resource_id
    ]
    meta = _d_texture_metadata(resource_id)
    if not selected_views:
        selected_views = [
            {
                "eid": int(marker["end_eid"]),
                "resource_id": resource_id,
                "first_slice": index,
                "num_slices": 1,
            }
            for index in range(int(meta.get("array_size", 1)))
        ]
    return (
        {
            "resource": meta,
            "score": scores[resource_id],
            "reasons": reasons.get(resource_id, []),
            "selection": {
                "candidate_count": len(scores),
                "top_tie_count": len(tied_resource_ids),
                "ambiguous": len(tied_resource_ids) != 1,
                "top_score": best_score,
                "tied_resource_ids": tied_resource_ids,
                "candidates": [
                    {
                        "resource_id": candidate_id,
                        "score": scores[candidate_id],
                        "resource": _d_texture_metadata(candidate_id),
                        "reasons": reasons.get(candidate_id, []),
                    }
                    for candidate_id in sorted(
                        scores, key=lambda key: (-scores[key], key)
                    )
                ],
            },
            "marker": {
                key: value for key, value in marker.items() if key != "_action"
            },
        },
        selected_views,
    )


def _d_candidate_color_ids(
    marker_kind: str,
    marker: dict[str, Any],
    snapshots: dict[str, list[dict[str, Any]]],
    usage_map: dict[int, list[dict[str, Any]]],
) -> set[int]:
    candidates: set[int] = set()
    for snapshot in snapshots.get(marker_kind, []):
        for output in snapshot.get("outputs", []):
            candidates.add(int(output["resource_id"]))
        for descriptor in snapshot.get("descriptors", []):
            type_name = descriptor.get("type", "").lower()
            if "readwrite" in type_name:
                resource_id = int(descriptor["resource"]["resource_id"])
                if resource_id in state.tex_map:
                    candidates.add(resource_id)
    for resource_id in state.tex_map:
        entries = _d_entries_in_span(usage_map, int(resource_id), marker)
        if _d_written_in(usage_map, int(resource_id), marker) or (
            marker_kind == "taa"
            and any(entry["usage"].lower() == "barrier" for entry in entries)
        ):
            candidates.add(int(resource_id))
    return candidates


def _d_pick_color_stage(
    kind: str,
    markers: dict[str, dict[str, Any]],
    snapshots: dict[str, list[dict[str, Any]]],
    usage_map: dict[int, list[dict[str, Any]]],
) -> dict[str, Any] | None:
    marker = markers.get(kind)
    if marker is None:
        return None
    next_kind = {"light": "taa", "taa": "final", "final": "present"}.get(kind)
    next_marker = markers.get(next_kind) if next_kind else None
    scores: dict[int, float] = {}
    reasons: dict[int, list[str]] = {}
    output_ids = {
        int(output["resource_id"])
        for snapshot in snapshots.get(kind, [])
        for output in snapshot.get("outputs", [])
    }
    next_descriptor_ids = {
        int(descriptor["resource"]["resource_id"])
        for snapshot in snapshots.get(next_kind, [])
        for descriptor in snapshot.get("descriptors", [])
        if descriptor.get("resource", {}).get("is_texture")
    } if next_kind else set()

    for resource_id in _d_candidate_color_ids(kind, marker, snapshots, usage_map):
        meta = _d_texture_metadata(resource_id)
        if not _d_color_like(meta):
            continue
        score = 0.0
        local_reasons: list[str] = []
        if resource_id in output_ids:
            score += 100.0
            local_reasons.append("bound color output in marker")
        if _d_written_in(usage_map, resource_id, marker):
            score += 40.0
            local_reasons.append("written inside marker span")
        marker_entries = _d_entries_in_span(usage_map, resource_id, marker)
        if kind == "taa" and any(
            entry["usage"].lower() == "barrier" for entry in marker_entries
        ):
            score += 30.0
            local_reasons.append("barrier-transitioned inside TAA marker span")
        if next_marker is not None and _d_read_in(usage_map, resource_id, next_marker):
            score += 70.0
            local_reasons.append(f"read inside {next_kind} marker span")
        if resource_id in next_descriptor_ids:
            score += 100.0
            local_reasons.append(f"bound descriptor in {next_kind} marker")
        name = str(meta.get("name", "")).lower()
        role_tokens = {
            "light": ("light", "hdr", "scene", "color"),
            "taa": ("taa", "temporal", "history", "resolve"),
            "final": ("final", "color", "tonemap", "composite"),
        }[kind]
        if any(token in name for token in role_tokens):
            score += 15.0
        pixels = int(meta.get("width", 0)) * int(meta.get("height", 0))
        score += min(25.0, math.log2(max(1, pixels)) if pixels else 0.0)
        if int(meta.get("samples", 1)) == 1:
            score += 3.0
        scores[resource_id] = score
        reasons[resource_id] = local_reasons

    if not scores:
        return None
    best_score = max(scores.values())
    tied_resource_ids = sorted(
        resource_id
        for resource_id, score in scores.items()
        if score == best_score
    )
    resource_id = tied_resource_ids[0]
    fallback_eid = max(
        [
            int(snapshot.get("eid", 0))
            for snapshot in snapshots.get(kind, [])
            if "error" not in snapshot
        ]
        or [int(marker["end_eid"])]
    )
    return {
        "resource": _d_texture_metadata(resource_id),
        "score": scores[resource_id],
        "reasons": reasons[resource_id],
        "selection": {
            "candidate_count": len(scores),
            "top_tie_count": len(tied_resource_ids),
            "ambiguous": len(tied_resource_ids) != 1,
            "top_score": best_score,
            "tied_resource_ids": tied_resource_ids,
            "candidates": [
                {
                    "resource_id": candidate_id,
                    "score": scores[candidate_id],
                    "resource": _d_texture_metadata(candidate_id),
                    "reasons": reasons.get(candidate_id, []),
                }
                for candidate_id in sorted(
                    scores, key=lambda key: (-scores[key], key)
                )
            ],
        },
        "passthrough": kind == "taa" and not _d_written_in(
            usage_map, resource_id, marker
        ),
        "eid": _d_latest_write_eid(
            usage_map, resource_id, marker, fallback_eid
        ),
        "marker": {key: value for key, value in marker.items() if key != "_action"},
    }


def _d_is_rgba16f(resource: dict[str, Any]) -> bool:
    compact = re.sub(r"[^A-Z0-9]+", "", str(resource.get("format", "")).upper())
    return "R16G16B16A16" in compact and (
        "SFLOAT" in compact or "FLOAT" in compact
    )


def _d_history_identity(resource: dict[str, Any]) -> dict[str, Any]:
    row = dict(resource)
    match = TAA_HISTORY_RESOURCE_RE.fullmatch(str(row.get("name", "")))
    row["logical_name"] = "GPUDrivenSceneColorHistory" if match else None
    row["physical_index"] = int(match.group("index")) if match else None
    return row


def _d_last_draw_snapshot(
    kind: str,
    markers: dict[str, dict[str, Any]],
    snapshots: dict[str, list[dict[str, Any]]],
    pass_segments: dict[str, list[dict[str, Any]]],
    *,
    require_single_draw: bool,
) -> dict[str, Any]:
    result: dict[str, Any] = {
        "kind": kind,
        "marker": None,
        "segments": list(pass_segments.get(kind, [])),
        "draw_count": 0,
        "last_draw_eid": None,
        "snapshot": None,
        "errors": [],
    }
    marker = markers.get(kind)
    if marker is None:
        result["errors"].append(f"{kind} marker is missing")
        return result
    result["marker"] = {
        key: value for key, value in marker.items() if key != "_action"
    }
    segments = result["segments"]
    result["draw_count"] = sum(int(row.get("draw_count", 0)) for row in segments)
    draw_eids = [
        int(row["last_draw_eid"])
        for row in segments
        if int(row.get("last_draw_eid", 0) or 0) > 0
    ]
    if not draw_eids:
        result["errors"].append(f"{kind} marker has no render-pass draw")
        return result
    if require_single_draw and (len(segments) != 1 or result["draw_count"] != 1):
        result["errors"].append(
            f"{kind} must contain exactly one render-pass draw; "
            f"segments={len(segments)} draw_count={result['draw_count']}"
        )
    draw_eid = max(draw_eids)
    result["last_draw_eid"] = draw_eid
    matches = [
        row
        for row in snapshots.get(kind, [])
        if int(row.get("eid", 0) or 0) == draw_eid and not row.get("error")
    ]
    if len(matches) != 1:
        result["errors"].append(
            f"{kind} last draw EID {draw_eid} matched {len(matches)} pipeline snapshots"
        )
        return result
    result["snapshot"] = matches[0]
    return result


def _d_exact_output(
    snapshot: dict[str, Any],
    *,
    label: str,
    name_pattern: re.Pattern[str],
    require_rgba16f: bool = False,
) -> tuple[dict[str, Any] | None, list[str]]:
    matches = [
        row
        for row in snapshot.get("outputs", [])
        if name_pattern.fullmatch(str(row.get("name", "")))
        and (not require_rgba16f or _d_is_rgba16f(row))
    ]
    if len(matches) != 1:
        return None, [
            f"{label} expected exactly one matching color output, found {len(matches)}"
        ]
    return dict(matches[0]), []


def _d_exact_fragment_descriptor(
    snapshot: dict[str, Any],
    *,
    array_element: int,
    label: str,
    name_pattern: re.Pattern[str],
) -> tuple[dict[str, Any] | None, list[str]]:
    fragment_value = int(getattr(rd.ShaderStage, "Fragment", 4))
    matches = [
        row
        for row in snapshot.get("descriptors", [])
        if int(row.get("stage_value", -1)) == fragment_value
        and int(row.get("array_element", -1)) == array_element
        and name_pattern.fullmatch(str(row.get("resource", {}).get("name", "")))
    ]
    if len(matches) != 1:
        return None, [
            f"{label} descriptor array element {array_element} matched "
            f"{len(matches)} expected resources"
        ]
    return dict(matches[0]), []


def _d_matrix4_from_shader_row(row: dict[str, Any]) -> list[list[float]] | None:
    values = row.get("value")
    if not isinstance(values, list) or len(values) < 16:
        return None
    converted = [float(value) for value in values[:16]]
    if not all(math.isfinite(value) for value in converted):
        return None
    return [converted[index : index + 4] for index in range(0, 16, 4)]


def _d_unjittered_camera_evidence(
    eid: int,
    viewport_resource: dict[str, Any],
) -> dict[str, Any]:
    import numpy as np

    result: dict[str, Any] = {
        "eid": int(eid),
        "matrix_convention": "column_vectors",
        "renderdoc_value_layout": "logical_rows",
        "framebuffer_y_to_ndc": "top_to_negative_one",
        "depth_range": "zero_to_one",
        "viewport": [
            0,
            0,
            int(viewport_resource.get("width", 0)),
            int(viewport_resource.get("height", 0)),
        ],
        "clip_from_world": None,
        "world_from_clip": None,
        "view_from_world": None,
        "world_from_view": None,
        "source_paths": {},
        "inverse_max_abs_error": None,
        "errors": [],
    }
    try:
        rows = _d_shader_matrices(eid)
    except Exception as exc:
        result["errors"].append(
            f"camera shader matrix extraction failed: {type(exc).__name__}: {exc}"
        )
        return result

    def choose(leaf: str, *, required: bool) -> dict[str, Any] | None:
        candidates = [
            row
            for row in rows
            if str(row.get("path", "")).casefold().endswith(f".{leaf}".casefold())
            and _d_matrix4_from_shader_row(row) is not None
        ]
        ps_candidates = [
            row for row in candidates if str(row.get("path", "")).startswith("ps.")
        ]
        selected_pool = ps_candidates or candidates
        unique = {
            tuple(value for matrix_row in _d_matrix4_from_shader_row(row) or [] for value in matrix_row)
            for row in selected_pool
        }
        if not selected_pool:
            if required:
                result["errors"].append(f"camera.{leaf} was not found at EID {eid}")
            return None
        if len(unique) != 1:
            result["errors"].append(
                f"camera.{leaf} is ambiguous across {len(selected_pool)} shader paths"
            )
            return None
        selected = selected_pool[0]
        result["source_paths"][leaf] = [
            str(row.get("path", "")) for row in selected_pool
        ]
        return selected

    clip_row = choose("unjitteredViewProjection", required=True)
    inverse_row = choose("unjitteredInverseViewProjection", required=True)
    view_row = choose("view", required=True)
    clip = _d_matrix4_from_shader_row(clip_row) if clip_row else None
    inverse = _d_matrix4_from_shader_row(inverse_row) if inverse_row else None
    result["clip_from_world"] = clip
    result["world_from_clip"] = inverse
    if view_row is not None:
        view = _d_matrix4_from_shader_row(view_row)
        result["view_from_world"] = view
        try:
            result["world_from_view"] = np.linalg.inv(
                np.asarray(view, dtype=np.float64)
            ).tolist()
        except Exception as exc:
            result["errors"].append(f"camera.view inverse failed: {exc}")
    if clip is not None and inverse is not None:
        product = np.asarray(clip, dtype=np.float64) @ np.asarray(
            inverse, dtype=np.float64
        )
        inverse_error = float(np.max(np.abs(product - np.eye(4, dtype=np.float64))))
        result["inverse_max_abs_error"] = inverse_error
        if not math.isfinite(inverse_error) or inverse_error > 5.0e-3:
            result["errors"].append(
                f"unjittered camera matrices are not inverse pairs: max_abs={inverse_error:g}"
            )
    if result["viewport"][2] <= 1 or result["viewport"][3] <= 1:
        result["errors"].append("camera viewport dimensions are invalid")
    return result


def _d_direct_stage_record(
    kind: str,
    resource: dict[str, Any],
    eid: int,
    marker: dict[str, Any],
) -> dict[str, Any]:
    row = dict(resource)
    if kind == "taa":
        row = _d_history_identity(row)
    return {
        "resource": row,
        "score": None,
        "reasons": ["release evidence exact draw/output binding"],
        "selection": {
            "candidate_count": 1,
            "top_tie_count": 1,
            "ambiguous": False,
            "selection_basis": "exact release draw binding",
        },
        "passthrough": False,
        "eid": int(eid),
        "marker": {key: value for key, value in marker.items() if key != "_action"},
        "actual_draw_binding": True,
    }


def _d_taa_absence_evidence() -> dict[str, Any]:
    result: dict[str, Any] = {
        "required_absent": True,
        "inner_markers": [],
        "resolve_draws": [],
        "inspected_draw_count": 0,
        "errors": [],
        "passed": False,
    }
    fragment_stage = rd.ShaderStage.Fragment
    for action, ancestors, path in _d_walk_actions(controller.GetRootActions()):
        name = _d_action_name(action).strip()
        if name == "GPUDrivenTAAResolve":
            result["inner_markers"].append(
                {
                    "event_id": int(getattr(action, "eventId", 0)),
                    "path": list(path),
                    "ancestors": list(ancestors),
                }
            )
        if not _d_is_draw(action):
            continue
        eid = int(getattr(action, "eventId", 0))
        if eid <= 0:
            result["errors"].append(
                f"draw {name!r} has no positive event ID during TAA absence proof"
            )
            continue
        result["inspected_draw_count"] += 1
        try:
            controller.SetFrameEvent(eid, True)
            pipe_state = controller.GetPipelineState()
            entry = str(pipe_state.GetShaderEntryPoint(fragment_stage) or "")
            shader_id = int(pipe_state.GetShader(fragment_stage))
        except Exception as exc:
            result["errors"].append(
                f"cannot inspect draw EID {eid} for TAA absence: "
                f"{type(exc).__name__}: {exc}"
            )
            continue
        if entry == "fragmentTAAResolveMain":
            result["resolve_draws"].append(
                {
                    "event_id": eid,
                    "name": name,
                    "fragment_entry": entry,
                    "fragment_shader": shader_id,
                    "path": list(path),
                    "ancestors": list(ancestors),
                }
            )
    if result["inner_markers"]:
        result["errors"].append(
            f"found {len(result['inner_markers'])} inner GPUDrivenTAAResolve marker(s)"
        )
    if result["resolve_draws"]:
        result["errors"].append(
            f"found {len(result['resolve_draws'])} fragmentTAAResolveMain draw(s)"
        )
    result["passed"] = not result["errors"]
    return result


def _d_build_release_resource_evidence(
    markers: dict[str, dict[str, Any]],
    snapshots: dict[str, list[dict[str, Any]]],
    pass_segments: dict[str, list[dict[str, Any]]],
    usage_map: dict[int, list[dict[str, Any]]],
    *,
    render_mode: str,
) -> dict[str, Any]:
    if render_mode not in RELEASE_RENDER_MODES:
        raise RuntimeError(f"unsupported release render mode {render_mode!r}")
    taa_required = render_mode == "taa-on"
    result: dict[str, Any] = {
        "schema": "mgif-csm-release-resource-binding-v1",
        "render_mode": render_mode,
        "taa_required": taa_required,
        "passed": False,
        "passes": {},
        "draw_evidence": {},
        "taa_resolve": {},
        "taa_absence": None,
        "no_post": None,
        "resources": {},
        "camera": None,
        "errors": [],
    }
    errors = result["errors"]
    required_roles = ("light", "taa", "final") if taa_required else ("light", "final")
    pass_records = {
        role: _d_last_draw_snapshot(
            role,
            markers,
            snapshots,
            pass_segments,
            require_single_draw=True,
        )
        for role in required_roles
    }
    result["passes"] = pass_records
    for role, record in pass_records.items():
        errors.extend(f"{role}: {error}" for error in record["errors"])
    if not taa_required:
        taa_absence = _d_taa_absence_evidence()
        result["taa_absence"] = taa_absence
        if markers.get("taa") is not None:
            errors.append("no-post release unexpectedly selected a TAA marker")
        errors.extend(
            f"taa absence: {error}" for error in taa_absence.get("errors", [])
        )
    if errors:
        return result

    light_snapshot = pass_records["light"]["snapshot"]
    final_snapshot = pass_records["final"]["snapshot"]
    taa_snapshot = pass_records.get("taa", {}).get("snapshot")
    assert isinstance(light_snapshot, dict)
    assert isinstance(final_snapshot, dict)
    if taa_required:
        assert isinstance(taa_snapshot, dict)

    draw_contracts = {
        "light": {
            "marker_name": "GPUDrivenLightPass",
            "fragment_entry": "fragmentHdrMain",
        },
        "final": {
            "marker_name": "GPUDrivenFinalColor",
            "fragment_entry": "fragmentFinalColorMain",
        },
    }
    if taa_required:
        draw_contracts["taa"] = {
            "marker_name": "GPUDrivenTAAResolve",
            "fragment_entry": "fragmentTAAResolveMain",
        }
    draw_evidence: dict[str, dict[str, Any]] = {}
    for role in required_roles:
        contract = draw_contracts[role]
        marker = markers.get(role, {})
        snapshot = pass_records[role].get("snapshot")
        if not isinstance(snapshot, dict):
            errors.append(f"{role}: draw snapshot is missing")
            continue
        marker_name = str(marker.get("name", "")).strip()
        if marker_name != contract["marker_name"]:
            errors.append(
                f"{role}: marker {marker_name!r} is not exact inner "
                f"{contract['marker_name']!r}"
            )
        snapshot_eid = int(snapshot.get("eid", 0) or 0)
        last_draw_eid = int(pass_records[role].get("last_draw_eid", 0) or 0)
        if snapshot_eid <= 0 or snapshot_eid != last_draw_eid:
            errors.append(f"{role}: snapshot EID does not match last_draw_eid")
        action = snapshot.get("action")
        if not isinstance(action, dict) or action.get("is_draw") is not True:
            errors.append(f"{role}: last_draw_eid snapshot is not a draw action")
        elif (
            int(action.get("vertex_count", 0) or 0) != 3
            or int(action.get("instance_count_raw", -1)) != 1
            or int(action.get("instance_count", -1)) != 1
        ):
            errors.append(
                f"{role}: fullscreen action is not raw Draw(3,1): "
                f"{action.get('name')!r}, instances={action.get('instance_count_raw')!r}"
            )
        fragment = snapshot.get("shaders", {}).get("ps")
        if not isinstance(fragment, dict):
            errors.append(f"{role}: draw has no fragment shader")
        else:
            if fragment.get("entry_point") != contract["fragment_entry"]:
                errors.append(
                    f"{role}: fragment entry is not {contract['fragment_entry']}: "
                    f"{fragment.get('entry_point')!r}"
                )
            if int(fragment.get("resource_id", 0) or 0) <= 0:
                errors.append(f"{role}: fragment shader resource is invalid")
        graphics_pipeline = int(snapshot.get("graphics_pipeline", 0) or 0)
        if graphics_pipeline <= 0:
            errors.append(f"{role}: draw has no graphics pipeline")
        draw_evidence[role] = {
            "marker_name": marker_name,
            "expected_marker_name": contract["marker_name"],
            "draw_eid": last_draw_eid,
            "draw_count": int(pass_records[role].get("draw_count", 0) or 0),
            "snapshot_eid": snapshot_eid,
            "snapshot_matches_last_draw": snapshot_eid == last_draw_eid,
            "action": action,
            "fragment_shader": fragment,
            "expected_fragment_entry": contract["fragment_entry"],
            "graphics_pipeline": graphics_pipeline,
        }
    result["draw_evidence"] = draw_evidence

    scene_hdr_output, output_errors = _d_exact_output(
        light_snapshot,
        label="light SceneColorHDR",
        name_pattern=re.compile(r"^GPUDrivenSceneColorHDR$"),
        require_rgba16f=True,
    )
    errors.extend(output_errors)
    final_output, output_errors = _d_exact_output(
        final_snapshot,
        label="final OutputTexture",
        name_pattern=re.compile(r"^OutputTexture$"),
    )
    errors.extend(output_errors)
    history_write_output: dict[str, Any] | None = None
    if taa_required and isinstance(taa_snapshot, dict):
        history_write_output, output_errors = _d_exact_output(
            taa_snapshot,
            label="TAA historyWrite",
            name_pattern=TAA_HISTORY_RESOURCE_RE,
            require_rgba16f=True,
        )
        errors.extend(output_errors)

    descriptor_specs: list[tuple[dict[str, Any], int, str, Any]] = [
        (light_snapshot, 0, "base_color", re.compile(r"^SceneColor0$")),
        (light_snapshot, 1, "packed_normal", re.compile(r"^SceneColor1$")),
        (light_snapshot, 3, "scene_depth", re.compile(r"^SceneDepth$")),
    ]
    if taa_required and isinstance(taa_snapshot, dict):
        descriptor_specs.extend(
            [
                (
                    taa_snapshot,
                    4,
                    "scene_color_hdr",
                    re.compile(r"^GPUDrivenSceneColorHDR$"),
                ),
                (taa_snapshot, 7, "velocity", re.compile(r"^GPUDrivenVelocity$")),
                (taa_snapshot, 8, "history_read", TAA_HISTORY_RESOURCE_RE),
                (final_snapshot, 9, "final_history_write", TAA_HISTORY_RESOURCE_RE),
            ]
        )
    else:
        descriptor_specs.append(
            (
                final_snapshot,
                4,
                "final_scene_color_hdr",
                re.compile(r"^GPUDrivenSceneColorHDR$"),
            )
        )
    descriptors: dict[str, dict[str, Any]] = {}
    for snapshot, array_element, descriptor_label, pattern in descriptor_specs:
        descriptor, descriptor_errors = _d_exact_fragment_descriptor(
            snapshot,
            array_element=array_element,
            label=descriptor_label,
            name_pattern=pattern,
        )
        errors.extend(descriptor_errors)
        if descriptor is not None:
            descriptors[descriptor_label] = descriptor

    if scene_hdr_output is not None:
        result["resources"]["scene_color_hdr"] = scene_hdr_output
    if final_output is not None:
        result["resources"]["final"] = final_output
    for descriptor_label in ("base_color", "packed_normal", "scene_depth"):
        if descriptor_label in descriptors:
            result["resources"][descriptor_label] = dict(
                descriptors[descriptor_label]["resource"]
            )
    if taa_required:
        if history_write_output is not None:
            result["resources"]["history_write"] = _d_history_identity(
                history_write_output
            )
        if "velocity" in descriptors:
            result["resources"]["velocity"] = dict(
                descriptors["velocity"]["resource"]
            )
        if "history_read" in descriptors:
            result["resources"]["history_read"] = _d_history_identity(
                descriptors["history_read"]["resource"]
            )
        if scene_hdr_output is not None and "scene_color_hdr" in descriptors:
            if int(scene_hdr_output["resource_id"]) != int(
                descriptors["scene_color_hdr"]["resource"]["resource_id"]
            ):
                errors.append("TAA descriptor 4 does not bind the Light SceneColorHDR output")
        if history_write_output is not None and "final_history_write" in descriptors:
            if int(history_write_output["resource_id"]) != int(
                descriptors["final_history_write"]["resource"]["resource_id"]
            ):
                errors.append("final descriptor 9 does not bind selected TAA historyWrite")
        history_read = result["resources"].get("history_read")
        history_write = result["resources"].get("history_write")
        if history_read is not None and history_write is not None:
            if int(history_read["resource_id"]) == int(history_write["resource_id"]):
                errors.append("TAA historyRead and historyWrite are the same resource")
            if history_read.get("physical_index") == history_write.get("physical_index"):
                errors.append("TAA historyRead/historyWrite physical indices are equal")
    elif scene_hdr_output is not None and "final_scene_color_hdr" in descriptors:
        if int(scene_hdr_output["resource_id"]) != int(
            descriptors["final_scene_color_hdr"]["resource"]["resource_id"]
        ):
            errors.append(
                "no-post final descriptor 4 does not bind the Light SceneColorHDR output"
            )

    output_write_contracts: list[tuple[str, dict[str, Any] | None, str]] = [
        ("light", scene_hdr_output, "selected Light SceneColorHDR"),
        ("final", final_output, "selected final OutputTexture"),
    ]
    if taa_required:
        output_write_contracts.insert(
            1,
            ("taa", history_write_output, "selected TAA historyWrite"),
        )
    for role, output, output_label in output_write_contracts:
        if output is None:
            continue
        written = _d_written_in(
            usage_map,
            int(output["resource_id"]),
            markers.get(role),
        )
        if not written:
            errors.append(f"{output_label} has no write usage inside {role} marker")
        draw_row = draw_evidence.get(role)
        if isinstance(draw_row, dict):
            draw_row["color_output"] = dict(output)
            draw_row["output_written_in_marker"] = written

    camera_viewport_resource = result["resources"].get("scene_depth")
    if isinstance(camera_viewport_resource, dict):
        result["camera"] = _d_unjittered_camera_evidence(
            int(pass_records["light"]["last_draw_eid"]),
            camera_viewport_resource,
        )
        errors.extend(
            f"camera: {error}" for error in result["camera"].get("errors", [])
        )
    else:
        errors.append("scene depth resource is unavailable for camera viewport binding")

    if taa_required and isinstance(taa_snapshot, dict):
        taa_marker = markers.get("taa", {})
        taa_action = draw_evidence.get("taa", {}).get("action")
        taa_ps = draw_evidence.get("taa", {}).get("fragment_shader")
        history_valid = _d_history_valid_evidence(
            int(pass_records["taa"]["last_draw_eid"])
        )
        result["taa_resolve"] = {
            "required": True,
            "inner_marker_name": str(taa_marker.get("name", "")),
            "outer_marker_rejected": "GPUDrivenTAAResolvePass",
            "draw_eid": pass_records["taa"]["last_draw_eid"],
            "draw_count": pass_records["taa"]["draw_count"],
            "snapshot_eid": taa_snapshot.get("eid"),
            "snapshot_matches_last_draw": draw_evidence.get("taa", {}).get(
                "snapshot_matches_last_draw"
            ),
            "action": taa_action,
            "fragment_shader": taa_ps,
            "graphics_pipeline": taa_snapshot.get("graphics_pipeline"),
            "history_write": result["resources"].get("history_write"),
            "history_read": result["resources"].get("history_read"),
            "scene_color_hdr": result["resources"].get("scene_color_hdr"),
            "velocity": result["resources"].get("velocity"),
            "history_valid": history_valid,
            "descriptor_bindings": {
                descriptor_label: descriptor
                for descriptor_label, descriptor in descriptors.items()
            },
            "passthrough": False,
        }
        if history_valid.get("readable") is not True:
            errors.append(
                "TAA historyValid is unreadable: "
                + "; ".join(str(error) for error in history_valid.get("errors", []))
            )
        else:
            value = float(history_valid.get("value"))
            if not math.isfinite(value) or abs(value - 1.0) > 1.0e-6:
                errors.append(f"TAA historyValid is {value!r}, expected exactly 1")
    else:
        result["taa_resolve"] = {
            "required": False,
            "absent": result.get("taa_absence", {}).get("passed") is True,
            "absence_evidence": result.get("taa_absence"),
            "passthrough": None,
        }
        result["no_post"] = {
            "required": True,
            "taa_absence": result.get("taa_absence"),
            "descriptor_bindings": {
                "final_scene_color_hdr": descriptors.get("final_scene_color_hdr")
            },
            "scene_color_hdr": result["resources"].get("scene_color_hdr"),
            "final": result["resources"].get("final"),
        }
    result["passed"] = not errors
    return result
def _d_flatten_shader_variable(variable: Any, prefix: str) -> list[dict[str, Any]]:
    name = str(getattr(variable, "name", "") or "")
    path = f"{prefix}.{name}" if prefix and name else name or prefix
    members = list(getattr(variable, "members", []))
    if members:
        rows = []
        for index, member in enumerate(members):
            member_name = str(getattr(member, "name", "") or f"[{index}]")
            child_prefix = f"{path}.{member_name}" if path else member_name
            rows.extend(_d_flatten_shader_variable(member, child_prefix.rsplit(".", 1)[0]))
        return rows

    rows_count = int(getattr(variable, "rows", 0))
    columns_count = int(getattr(variable, "columns", 0))
    count = max(1, rows_count * columns_count)
    value = getattr(variable, "value", None)
    values: list[float] = []
    if value is not None:
        type_name = str(getattr(variable, "type", "")).lower()
        lane_names = (
            ("u32v", "s32v", "f32v")
            if "uint" in type_name or "bool" in type_name
            else ("s32v", "u32v", "f32v")
            if "sint" in type_name or type_name.startswith("int")
            else ("f32v", "f64v", "u32v", "s32v")
        )
        for lane_name in lane_names:
            lane = getattr(value, lane_name, None)
            if lane is not None:
                try:
                    values = [float(item) for item in list(lane)[:count]]
                    break
                except Exception:
                    continue
    return [
        {
            "path": path,
            "type": str(getattr(variable, "type", "")),
            "rows": rows_count,
            "columns": columns_count,
            "value": values,
        }
    ]


def _d_shader_matrices(eid: int) -> list[dict[str, Any]]:
    from rdc.handlers._helpers import get_pipeline_for_stage

    controller.SetFrameEvent(int(eid), True)
    pipe_state = controller.GetPipelineState()
    rows: list[dict[str, Any]] = []
    for stage_name, stage_value in (
        ("vs", getattr(rd.ShaderStage, "Vertex", 0)),
        ("ps", getattr(rd.ShaderStage, "Fragment", 4)),
        ("cs", getattr(rd.ShaderStage, "Compute", 5)),
    ):
        reflection = pipe_state.GetShaderReflection(stage_value)
        if reflection is None:
            continue
        shader_id = pipe_state.GetShader(stage_value)
        if int(shader_id) == 0:
            continue
        entry = pipe_state.GetShaderEntryPoint(stage_value)
        pipeline = get_pipeline_for_stage(pipe_state, stage_value)
        for block_index, block_definition in enumerate(
            getattr(reflection, "constantBlocks", [])
        ):
            try:
                bound = pipe_state.GetConstantBlock(stage_value, block_index, 0)
                descriptor = bound.descriptor
                variables = controller.GetCBufferVariableContents(
                    pipeline,
                    shader_id,
                    stage_value,
                    entry,
                    block_index,
                    descriptor.resource,
                    descriptor.byteOffset,
                    descriptor.byteSize,
                )
            except Exception:
                continue
            prefix = f"{stage_name}.{getattr(block_definition, 'name', '')}"
            for variable in variables:
                for flattened in _d_flatten_shader_variable(variable, prefix):
                    if (
                        flattened["rows"] in (3, 4)
                        and flattened["columns"] in (3, 4)
                        and len(flattened["value"])
                        >= flattened["rows"] * flattened["columns"]
                    ):
                        rows.append(flattened)
    return rows


def _d_history_valid_evidence(eid: int) -> dict[str, Any]:
    result: dict[str, Any] = {
        "readable": False,
        "value": None,
        "paths": [],
        "errors": [],
    }
    fragment_stage = rd.ShaderStage.Fragment
    try:
        from rdc.handlers._helpers import get_pipeline_for_stage

        controller.SetFrameEvent(int(eid), True)
        pipe_state = controller.GetPipelineState()
        reflection = pipe_state.GetShaderReflection(fragment_stage)
        shader_id = pipe_state.GetShader(fragment_stage)
        if reflection is None or int(shader_id) == 0:
            raise RuntimeError("fragment reflection or shader is unavailable")
        pipeline = get_pipeline_for_stage(pipe_state, fragment_stage)
        entry_point = pipe_state.GetShaderEntryPoint(fragment_stage)
    except Exception as exc:
        result["errors"].append(
            f"historyValid pipeline setup failed: {type(exc).__name__}: {exc}"
        )
        return result

    def flatten(variable: Any, prefix: str) -> list[tuple[str, list[float]]]:
        name = str(getattr(variable, "name", "") or "")
        path = f"{prefix}.{name}" if prefix and name else name or prefix
        members = list(getattr(variable, "members", []))
        if members:
            rows: list[tuple[str, list[float]]] = []
            for member in members:
                rows.extend(flatten(member, path))
            return rows
        value = getattr(variable, "value", None)
        if value is None:
            return []
        for lane_name in ("f32v", "f64v", "u32v", "s32v"):
            lane = getattr(value, lane_name, None)
            if lane is None:
                continue
            try:
                return [(path, [float(item) for item in list(lane)])]
            except Exception:
                continue
        return []

    candidates: list[tuple[str, float]] = []
    for block_index, block_definition in enumerate(
        getattr(reflection, "constantBlocks", [])
    ):
        try:
            bound = pipe_state.GetConstantBlock(fragment_stage, block_index, 0)
            descriptor = bound.descriptor
            variables = controller.GetCBufferVariableContents(
                pipeline,
                shader_id,
                fragment_stage,
                entry_point,
                block_index,
                descriptor.resource,
                descriptor.byteOffset,
                descriptor.byteSize,
            )
        except Exception as exc:
            result["errors"].append(
                f"constant block {block_index} unreadable: "
                f"{type(exc).__name__}: {exc}"
            )
            continue
        block_name = str(getattr(block_definition, "name", "") or "")
        for variable in variables:
            for path, values in flatten(variable, block_name):
                if path.rsplit(".", 1)[-1].casefold() == "params5" and len(values) >= 2:
                    candidates.append((path, float(values[1])))
    result["paths"] = [path for path, _ in candidates]
    if len(candidates) == 1:
        result["readable"] = True
        result["value"] = candidates[0][1]
    elif not candidates:
        result["errors"].append("postProcess.params5.y historyValid was not found")
    else:
        result["errors"].append(
            f"historyValid is ambiguous across {len(candidates)} params5 paths"
        )
    return result


def _d_format_name(texture: Any) -> str:
    fmt = texture.format
    return str(fmt.Name() if hasattr(fmt, "Name") else fmt)


_D_EXPORT_BUDGET_STATE: dict[str, Any] | None = None


def _d_initialize_export_budget(output_dir: Path) -> dict[str, Any]:
    global _D_EXPORT_BUDGET_STATE
    try:
        export_budget_bytes = int(args.get("export_budget_bytes", 0))
        single_readback_cap_bytes = int(args.get("single_readback_cap_bytes", 0))
        safety_margin_bytes = int(args.get("export_safety_margin_bytes", 0))
    except (TypeError, ValueError) as exc:
        raise RuntimeError(f"invalid daemon export budget arguments: {exc}") from exc
    if export_budget_bytes <= 0 or single_readback_cap_bytes <= 0:
        raise RuntimeError("daemon export/readback budgets must be positive")
    if safety_margin_bytes <= 0:
        raise RuntimeError("daemon export budget requires a positive safety margin")
    resolved = output_dir.resolve()
    try:
        free_bytes = int(shutil.disk_usage(resolved).free)
    except Exception as exc:
        raise RuntimeError(
            f"cannot query extractor output free space {resolved}: "
            f"{type(exc).__name__}: {exc}"
        ) from exc
    required_free_bytes = export_budget_bytes + safety_margin_bytes
    if free_bytes < required_free_bytes:
        raise RuntimeError(
            f"extractor output disk preflight failed: free={free_bytes}, "
            f"required={required_free_bytes}"
        )
    _D_EXPORT_BUDGET_STATE = {
        "schema": "mgif-rdc-daemon-export-budget-v1",
        "output_directory": str(resolved),
        "export_budget_bytes": export_budget_bytes,
        "single_readback_cap_bytes": single_readback_cap_bytes,
        "safety_margin_bytes": safety_margin_bytes,
        "free_bytes_at_start": free_bytes,
        "required_free_bytes_at_start": required_free_bytes,
        "readback_expected_bytes": 0,
        "readback_actual_bytes": 0,
        "npy_committed_bytes": 0,
        "readbacks": [],
        "writes": [],
    }
    return _D_EXPORT_BUDGET_STATE


def _d_export_budget() -> dict[str, Any]:
    if not isinstance(_D_EXPORT_BUDGET_STATE, dict):
        raise RuntimeError("daemon export budget was not initialized")
    return _D_EXPORT_BUDGET_STATE


def _d_reserve_readback(
    contract: dict[str, Any],
    *,
    role: str,
) -> dict[str, Any]:
    budget = _d_export_budget()
    expected = int(contract.get("expected_raw_bytes", 0))
    single_cap = int(budget["single_readback_cap_bytes"])
    total_cap = int(budget["export_budget_bytes"])
    if expected <= 0:
        raise RuntimeError(f"{role}: expected readback byte count is not positive")
    if expected > single_cap:
        raise RuntimeError(
            f"{role}: expected GetTextureData payload exceeds the single-readback cap: "
            f"expected={expected}, cap={single_cap}"
        )
    next_total = int(budget["readback_expected_bytes"]) + expected
    if next_total > total_cap:
        raise RuntimeError(
            f"{role}: cumulative expected GetTextureData bytes exceed the per-capture "
            f"budget: next={next_total}, cap={total_cap}"
        )
    reservation = {
        "role": role,
        "expected_bytes": expected,
        "single_readback_cap_bytes": single_cap,
        "cumulative_expected_bytes": next_total,
        "actual_bytes": None,
        "completed": False,
    }
    budget["readback_expected_bytes"] = next_total
    budget["readbacks"].append(reservation)
    return reservation


def _d_complete_readback(
    reservation: dict[str, Any],
    *,
    actual_bytes: int,
) -> dict[str, Any]:
    budget = _d_export_budget()
    actual = int(actual_bytes)
    expected = int(reservation.get("expected_bytes", -1))
    if actual != expected:
        raise RuntimeError(
            f"{reservation.get('role')}: actual GetTextureData bytes {actual} "
            f"do not equal reserved exact bytes {expected}"
        )
    if actual > int(budget["single_readback_cap_bytes"]):
        raise RuntimeError(
            f"{reservation.get('role')}: actual GetTextureData bytes exceed the cap"
        )
    next_actual = int(budget["readback_actual_bytes"]) + actual
    if next_actual > int(budget["export_budget_bytes"]):
        raise RuntimeError(
            f"{reservation.get('role')}: cumulative actual GetTextureData bytes "
            f"exceed the per-capture cap"
        )
    budget["readback_actual_bytes"] = next_actual
    reservation["actual_bytes"] = actual
    reservation["completed"] = True
    return dict(reservation)


def _d_reserve_output_write(
    path: Path,
    *,
    payload_bytes: int,
    role: str,
    kind: str,
) -> dict[str, Any]:
    budget = _d_export_budget()
    resolved = path.resolve()
    if resolved.exists():
        raise RuntimeError(f"{role}: output already exists before write: {resolved}")
    payload = int(payload_bytes)
    if payload <= 0:
        raise RuntimeError(f"{role}: output reservation is not positive")
    estimated_write_bytes = payload + NPY_HEADER_RESERVE_BYTES
    committed = int(budget["npy_committed_bytes"])
    max_bytes = int(budget["export_budget_bytes"])
    if committed + estimated_write_bytes > max_bytes:
        raise RuntimeError(
            f"{role}: {kind} export would exceed the per-capture output budget: "
            f"next={committed + estimated_write_bytes}, cap={max_bytes}"
        )
    remaining_output_budget = max_bytes - committed
    try:
        free_bytes = int(shutil.disk_usage(resolved.parent).free)
    except Exception as exc:
        raise RuntimeError(
            f"{role}: cannot recheck free space before {kind} write: "
            f"{type(exc).__name__}: {exc}"
        ) from exc
    required_free_bytes = remaining_output_budget + int(budget["safety_margin_bytes"])
    if free_bytes < required_free_bytes:
        raise RuntimeError(
            f"{role}: insufficient free space before {kind} write: "
            f"free={free_bytes}, required={required_free_bytes}"
        )
    reservation = {
        "role": role,
        "kind": kind,
        "path": str(resolved),
        "payload_bytes": payload,
        "estimated_write_bytes": estimated_write_bytes,
        "free_bytes_before_write": free_bytes,
        "required_free_bytes_before_write": required_free_bytes,
        "actual_file_bytes": None,
        "completed": False,
    }
    budget["writes"].append(reservation)
    return reservation


def _d_complete_output_write(
    reservation: dict[str, Any],
) -> dict[str, Any]:
    budget = _d_export_budget()
    path = Path(str(reservation["path"]))
    actual = int(path.stat().st_size)
    if actual <= 0:
        raise RuntimeError(f"{reservation.get('role')}: output file is empty")
    if actual > int(reservation["estimated_write_bytes"]):
        raise RuntimeError(
            f"{reservation.get('role')}: output file exceeded the pre-write reservation: "
            f"actual={actual}, reserved={reservation['estimated_write_bytes']}"
        )
    next_total = int(budget["npy_committed_bytes"]) + actual
    if next_total > int(budget["export_budget_bytes"]):
        raise RuntimeError(
            f"{reservation.get('role')}: actual output bytes exceed the per-capture budget"
        )
    budget["npy_committed_bytes"] = next_total
    reservation["actual_file_bytes"] = actual
    reservation["completed"] = True
    return dict(reservation)


def _d_export_budget_snapshot() -> dict[str, Any]:
    budget = _d_export_budget()
    return json.loads(json.dumps(budget))


def _d_texture_readback_contract(
    texture: Any,
    subresource: Any,
    *,
    depth: bool,
) -> dict[str, Any]:
    fmt = texture.format
    width = int(getattr(texture, "width", 0))
    height = int(getattr(texture, "height", 0))
    texture_depth = int(getattr(texture, "depth", 1))
    samples = int(getattr(texture, "msSamp", 1))
    array_size = int(getattr(texture, "arraysize", 1))
    mip = int(getattr(subresource, "mip", -1))
    slice_index = int(getattr(subresource, "slice", -1))
    sample = int(getattr(subresource, "sample", -1))
    format_name = _d_format_name(texture)
    compact_format = re.sub(r"[^A-Z0-9]+", "", format_name.upper())
    format_type = _d_enum_name(getattr(fmt, "type", ""))
    component_type = _d_enum_name(getattr(fmt, "compType", ""))
    component_count = int(getattr(fmt, "compCount", 0))
    component_byte_width = int(getattr(fmt, "compByteWidth", 0))
    bgra = bool(fmt.BGRAOrder()) if hasattr(fmt, "BGRAOrder") else False

    errors: list[str] = []
    if width <= 0 or height <= 0:
        errors.append(f"invalid texture extent {width}x{height}")
    if samples != 1:
        errors.append(f"GetTextureData export requires samples==1, got {samples}")
    if texture_depth != 1:
        errors.append(f"GetTextureData export requires depth==1, got {texture_depth}")
    if mip != 0:
        errors.append(f"GetTextureData export requires mip==0, got {mip}")
    if sample != 0:
        errors.append(f"GetTextureData export requires sample==0, got {sample}")
    if array_size <= 0 or slice_index < 0 or slice_index >= array_size:
        errors.append(
            f"GetTextureData slice {slice_index} is outside array size {array_size}"
        )

    packed_decode: str | None = None
    decoded_channel_count = component_count
    bytes_per_pixel = 0
    if "R11G11B10" in compact_format:
        bytes_per_pixel = 4
        decoded_channel_count = 3
        packed_decode = "R11G11B10"
    elif "R9G9B9E5" in compact_format:
        bytes_per_pixel = 4
        decoded_channel_count = 3
        packed_decode = "R9G9B9E5"
    elif depth or component_type.lower() == "depth":
        decoded_channel_count = 1
        if "S8" in compact_format:
            errors.append(
                "combined depth/stencil GetTextureData layout is not accepted; "
                f"a depth-only D16/D24/D32 source is required, got {format_name}"
            )
        elif "D16" in compact_format:
            bytes_per_pixel = 2
            packed_decode = "D16_UNORM"
        elif "D24" in compact_format:
            bytes_per_pixel = 4
            packed_decode = "D24_UNORM_X8"
        elif "D32" in compact_format:
            bytes_per_pixel = 4
            packed_decode = "D32_FLOAT"
        else:
            errors.append(
                "unsupported depth readback layout; an explicit depth-only "
                f"D16/D24/D32 format is required, got {format_name}"
            )
    else:
        if format_type.lower() != "regular":
            errors.append(f"unsupported non-regular readback format {format_name}")
        if component_count <= 0 or component_count > 4:
            errors.append(
                f"unsupported component count {component_count} for {format_name}"
            )
        if component_byte_width not in (1, 2, 4, 8):
            errors.append(
                f"unsupported component byte width {component_byte_width} for {format_name}"
            )
        if not errors:
            bytes_per_pixel = component_count * component_byte_width

    if bytes_per_pixel <= 0:
        errors.append(f"bytes-per-pixel is not provable for {format_name}")
    expected_raw_bytes = (
        width * height * bytes_per_pixel
        if width > 0 and height > 0 and bytes_per_pixel > 0
        else 0
    )
    if errors:
        raise RuntimeError("; ".join(errors))
    return {
        "schema": "mgif-renderdoc-tight-readback-v1",
        "width": width,
        "height": height,
        "texture_depth": texture_depth,
        "array_size": array_size,
        "samples": samples,
        "mip": mip,
        "slice": slice_index,
        "sample": sample,
        "bytes_per_pixel": bytes_per_pixel,
        "expected_raw_bytes": expected_raw_bytes,
        "tightly_packed_required": True,
        "packed_decode": packed_decode,
        "decoded_channel_count": decoded_channel_count,
        "source_format": {
            "name": format_name,
            "format_type": format_type,
            "component_type": component_type,
            "component_count": component_count,
            "component_byte_width": component_byte_width,
            "bgra": bgra,
            "bytes_per_pixel": bytes_per_pixel,
        },
    }


def _d_validate_raw_texture_bytes(raw: bytes, contract: dict[str, Any]) -> None:
    expected = int(contract.get("expected_raw_bytes", -1))
    actual = len(raw)
    contract["raw_byte_length"] = actual
    contract["raw_byte_length_exact"] = actual == expected
    if actual != expected:
        raise RuntimeError(
            "GetTextureData returned a non-tight or incomplete payload: "
            f"got {actual} bytes, expected exactly {expected} "
            f"({contract.get('width')}x{contract.get('height')}x"
            f"{contract.get('bytes_per_pixel')})"
        )


def _d_color_channel_orders(contract: dict[str, Any]) -> tuple[list[str], list[str]]:
    count = int(contract.get("decoded_channel_count", 0))
    packed = contract.get("packed_decode")
    if packed in ("R11G11B10", "R9G9B9E5"):
        return ["R", "G", "B"], ["R", "G", "B"]
    if count < 1 or count > 4:
        raise RuntimeError(f"unsupported decoded channel count {count}")
    canonical = list(("R", "G", "B", "A")[:count])
    source = list(canonical)
    if contract.get("source_format", {}).get("bgra") and count >= 3:
        source[0], source[2] = source[2], source[0]
    return source, canonical


def _d_validate_saved_npy(
    path: Path,
    *,
    expected_shape: tuple[int, ...],
    expected_dtype: str,
    channel_order: list[str],
    source_format: dict[str, Any],
    source_resource_id: int,
    role: str,
    evidence_only: bool,
    dimension_class: str,
) -> dict[str, Any]:
    import numpy as np

    errors: list[str] = []
    try:
        array = np.load(path, allow_pickle=False, mmap_mode="r")
    except Exception as exc:
        raise RuntimeError(f"cannot reopen exported NPY {path}: {exc}") from exc
    observed_shape = tuple(int(value) for value in array.shape)
    observed_dtype = str(array.dtype)
    if observed_shape != expected_shape:
        errors.append(
            f"shape {observed_shape!r} does not equal expected {expected_shape!r}"
        )
    if observed_dtype != str(np.dtype(expected_dtype)):
        errors.append(
            f"dtype {observed_dtype!r} does not equal expected "
            f"{str(np.dtype(expected_dtype))!r}"
        )
    expected_channels = 1 if len(expected_shape) == 2 else int(expected_shape[-1])
    if len(channel_order) != expected_channels:
        errors.append(
            f"channel order {channel_order!r} does not describe {expected_channels} channels"
        )
    if not isinstance(source_format, dict) or not source_format.get("name"):
        errors.append("source format evidence is missing")
    if int(source_format.get("bytes_per_pixel", 0) or 0) <= 0:
        errors.append("source format bytes_per_pixel is invalid")
    if int(source_resource_id) <= 0:
        errors.append("source resource ID is invalid")
    if dimension_class not in ("screen", "csm"):
        errors.append(f"invalid dimension class {dimension_class!r}")
    validation = {
        "schema": NPY_EXPORT_EVIDENCE_SCHEMA,
        "passed": not errors,
        "errors": errors,
        "path": str(path.resolve()),
        "role": role,
        "shape": list(observed_shape),
        "dtype": observed_dtype,
        "channel_order": list(channel_order),
        "source_format": dict(source_format),
        "source_resource_id": int(source_resource_id),
        "screen_extent": {
            "width": int(expected_shape[1]),
            "height": int(expected_shape[0]),
        },
        "dimension_class": dimension_class,
        "evidence_only": bool(evidence_only),
        "allow_pickle": False,
        "reopened_after_write": True,
    }
    del array
    if errors:
        raise RuntimeError(
            f"exported NPY validation failed for {role}: " + "; ".join(errors)
        )
    return validation


def _d_decode_regular_texture(
    texture: Any,
    raw: bytes,
    *,
    depth: bool,
    contract: dict[str, Any] | None = None,
) -> Any:
    import numpy as np

    if contract is None:
        subresource = type("ReadbackSubresource", (), {"mip": 0, "slice": 0, "sample": 0})()
        contract = _d_texture_readback_contract(texture, subresource, depth=depth)
    _d_validate_raw_texture_bytes(raw, contract)
    width = int(contract["width"])
    height = int(contract["height"])
    source_format = contract["source_format"]
    format_name = str(source_format["name"])
    component_name = str(source_format["component_type"])
    component_count = int(source_format["component_count"])
    byte_width = int(source_format["component_byte_width"])
    packed_decode = contract.get("packed_decode")

    if packed_decode == "R11G11B10":
        from rdc.handlers._helpers import _unpack_r11g11b10

        words = np.frombuffer(raw, dtype="<u4")
        array = _unpack_r11g11b10(words).reshape((height, width, 3))
        return np.asarray(array, dtype=np.float32)
    if packed_decode == "R9G9B9E5":
        from rdc.handlers._helpers import _unpack_r9g9b9e5

        words = np.frombuffer(raw, dtype="<u4")
        array = _unpack_r9g9b9e5(words).reshape((height, width, 3))
        return np.asarray(array, dtype=np.float32)

    if depth or component_name.lower() == "depth":
        if packed_decode == "D16_UNORM":
            array = np.frombuffer(raw, dtype="<u2").astype(np.float32) / 65535.0
        elif packed_decode == "D24_UNORM_X8":
            words = np.frombuffer(raw, dtype="<u4")
            array = (words & np.uint32(0x00FFFFFF)).astype(np.float32) / 16777215.0
        elif packed_decode == "D32_FLOAT":
            array = np.frombuffer(raw, dtype="<f4").astype(np.float32, copy=False)
        else:
            raise RuntimeError(f"unsupported depth format {format_name}")
        if array.size != width * height:
            raise RuntimeError(
                f"decoded depth element count {array.size} does not equal "
                f"{width * height} for {format_name}"
            )
        return array.reshape((height, width))

    kind = component_name.lower()
    if kind == "float":
        dtype_map = {2: "<f2", 4: "<f4", 8: "<f8"}
    elif kind in ("unorm", "uint", "typeless"):
        dtype_map = {1: "u1", 2: "<u2", 4: "<u4"}
    elif kind in ("snorm", "sint"):
        dtype_map = {1: "i1", 2: "<i2", 4: "<i4"}
    else:
        raise RuntimeError(
            f"unsupported component type {component_name} for {format_name}"
        )
    if byte_width not in dtype_map or component_count <= 0:
        raise RuntimeError(f"unsupported layout {format_name}")
    expected = width * height * component_count
    array = np.frombuffer(raw, dtype=dtype_map[byte_width])
    if array.size != expected:
        raise RuntimeError(
            f"decoded element count mismatch for {format_name}: got {array.size}, "
            f"expected {expected}"
        )
    array = array.reshape((height, width, component_count))
    if kind == "unorm":
        array = array.astype(np.float32) / float((1 << (8 * byte_width)) - 1)
    elif kind == "snorm":
        denominator = float((1 << (8 * byte_width - 1)) - 1)
        array = np.maximum(-1.0, array.astype(np.float32) / denominator)
    else:
        array = array.astype(np.float32)
    if bool(source_format.get("bgra")) and component_count >= 3:
        order = list(range(component_count))
        order[0], order[2] = order[2], order[0]
        array = array[..., order]
    return np.asarray(array, dtype=np.float32)


def _d_export_array(
    output_dir: Path,
    label: str,
    resource_id: int,
    eid: int,
    slice_index: int,
    *,
    depth: bool,
    allow_png_fallback: bool,
    role: str | None = None,
    evidence_only: bool = False,
    dimension_class: str = "screen",
) -> dict[str, Any]:
    import numpy as np

    controller.SetFrameEvent(int(eid), True)
    texture = state.tex_map[int(resource_id)]
    subresource = rd.Subresource()
    subresource.mip = 0
    subresource.slice = int(slice_index)
    subresource.sample = 0
    contract = _d_texture_readback_contract(texture, subresource, depth=depth)
    role_name = str(role or label)
    readback_reservation = _d_reserve_readback(contract, role=role_name)
    raw = bytes(controller.GetTextureData(texture.resourceId, subresource))
    _d_validate_raw_texture_bytes(raw, contract)
    readback_budget = _d_complete_readback(
        readback_reservation,
        actual_bytes=len(raw),
    )
    safe_label = re.sub(r"[^A-Za-z0-9_.-]+", "_", label)
    npy_path = output_dir / f"{safe_label}.npy"
    try:
        array = _d_decode_regular_texture(
            texture,
            raw,
            depth=depth,
            contract=contract,
        )
        array = np.asarray(array, dtype=np.float32)
        if depth:
            source_channel_order = ["D"]
            channel_order = ["D"]
        else:
            source_channel_order, channel_order = _d_color_channel_orders(contract)
        write_reservation = _d_reserve_output_write(
            npy_path,
            payload_bytes=int(array.nbytes),
            role=role_name,
            kind="npy",
        )
        np.save(npy_path, array, allow_pickle=False)
        write_budget = _d_complete_output_write(write_reservation)
        validation = _d_validate_saved_npy(
            npy_path,
            expected_shape=tuple(int(value) for value in array.shape),
            expected_dtype="float32",
            channel_order=channel_order,
            source_format=contract["source_format"],
            source_resource_id=int(resource_id),
            role=role_name,
            evidence_only=evidence_only,
            dimension_class=dimension_class,
        )
        return {
            "kind": "npy",
            "path": npy_path.name,
            "shape": [int(value) for value in array.shape],
            "dtype": str(array.dtype),
            "channel_order": channel_order,
            "source_channel_order": source_channel_order,
            "source_format": dict(contract["source_format"]),
            "source_resource_id": int(resource_id),
            "screen_extent": validation["screen_extent"],
            "dimension_class": dimension_class,
            "evidence_only": bool(evidence_only),
            "readback_contract": contract,
            "readback_budget": readback_budget,
            "write_budget": write_budget,
            "npy_validation": validation,
            "slice": int(slice_index),
            "mip": 0,
            "sample": 0,
            "eid": int(eid),
        }
    except Exception as decode_error:
        if depth:
            raise
        if not allow_png_fallback:
            raise RuntimeError(
                f"release gate forbids PNG fallback for {label}: {decode_error}"
            ) from decode_error
        from rdc.handlers._helpers import _decode_texture_png

        png = _decode_texture_png(rd, texture, raw, 0, is_depth=False)
        if png is None:
            raise RuntimeError(
                f"cannot decode {texture.format.Name()}: {decode_error}"
            ) from decode_error
        png_path = output_dir / f"{safe_label}.png"
        write_reservation = _d_reserve_output_write(
            png_path,
            payload_bytes=len(png),
            role=role_name,
            kind="png",
        )
        png_path.write_bytes(png)
        write_budget = _d_complete_output_write(write_reservation)
        return {
            "kind": "png",
            "path": png_path.name,
            "source_format": dict(contract["source_format"]),
            "source_resource_id": int(resource_id),
            "readback_contract": contract,
            "readback_budget": readback_budget,
            "write_budget": write_budget,
            "slice": int(slice_index),
            "mip": 0,
            "sample": 0,
            "eid": int(eid),
            "decode_warning": str(decode_error),
        }


def _d_export_world_normals(
    output_dir: Path,
    label: str,
    resource_id: int,
    eid: int,
) -> dict[str, Any]:
    import numpy as np

    controller.SetFrameEvent(int(eid), True)
    texture = state.tex_map[int(resource_id)]
    subresource = rd.Subresource()
    subresource.mip = 0
    subresource.slice = 0
    subresource.sample = 0
    contract = _d_texture_readback_contract(texture, subresource, depth=False)
    readback_reservation = _d_reserve_readback(
        contract,
        role="world_normals",
    )
    raw = bytes(controller.GetTextureData(texture.resourceId, subresource))
    _d_validate_raw_texture_bytes(raw, contract)
    readback_budget = _d_complete_readback(
        readback_reservation,
        actual_bytes=len(raw),
    )
    packed = _d_decode_regular_texture(
        texture,
        raw,
        depth=False,
        contract=contract,
    )
    if packed.ndim != 3 or packed.shape[2] < 2:
        raise RuntimeError(
            f"packed normal resource requires at least two channels, got {packed.shape}"
        )
    encoded = packed[..., :2].astype(np.float32, copy=False)
    f = encoded * np.float32(2.0) - np.float32(1.0)
    normals = np.empty((*f.shape[:2], 3), dtype=np.float32)
    normals[..., 0:2] = f
    normals[..., 2] = np.float32(1.0) - np.abs(f[..., 0]) - np.abs(f[..., 1])
    folded = normals[..., 2] < 0.0
    if np.any(folded):
        original_x = normals[..., 0].copy()
        original_y = normals[..., 1].copy()
        sign_x = np.where(original_x >= 0.0, 1.0, -1.0)
        sign_y = np.where(original_y >= 0.0, 1.0, -1.0)
        normals[..., 0] = np.where(
            folded, (1.0 - np.abs(original_y)) * sign_x, normals[..., 0]
        )
        normals[..., 1] = np.where(
            folded, (1.0 - np.abs(original_x)) * sign_y, normals[..., 1]
        )
    lengths = np.linalg.norm(normals, axis=2, keepdims=True)
    if not np.all(np.isfinite(lengths)) or np.any(lengths <= 1.0e-8):
        raise RuntimeError("decoded world normal texture contains invalid vectors")
    normals /= lengths
    safe_label = re.sub(r"[^A-Za-z0-9_.-]+", "_", label)
    npy_path = output_dir / f"{safe_label}.npy"
    write_reservation = _d_reserve_output_write(
        npy_path,
        payload_bytes=int(normals.nbytes),
        role="world_normals",
        kind="npy",
    )
    np.save(npy_path, normals, allow_pickle=False)
    write_budget = _d_complete_output_write(write_reservation)
    validation = _d_validate_saved_npy(
        npy_path,
        expected_shape=tuple(int(value) for value in normals.shape),
        expected_dtype="float32",
        channel_order=["X", "Y", "Z"],
        source_format=contract["source_format"],
        source_resource_id=int(resource_id),
        role="world_normals",
        evidence_only=False,
        dimension_class="screen",
    )
    source_channel_order, _ = _d_color_channel_orders(contract)
    return {
        "kind": "npy",
        "path": npy_path.name,
        "shape": [int(value) for value in normals.shape],
        "dtype": str(normals.dtype),
        "channel_order": ["X", "Y", "Z"],
        "source_channel_order": source_channel_order,
        "source_format": dict(contract["source_format"]),
        "source_resource_id": int(resource_id),
        "screen_extent": validation["screen_extent"],
        "dimension_class": "screen",
        "evidence_only": False,
        "readback_contract": contract,
        "readback_budget": readback_budget,
        "write_budget": write_budget,
        "npy_validation": validation,
        "slice": 0,
        "mip": 0,
        "sample": 0,
        "eid": int(eid),
        "decode": "shader.light.slang decodeNormalOct world-space normal",
    }


def _d_validate_release_export_set(
    exports: dict[str, dict[str, Any]],
    resources: dict[str, dict[str, Any]],
    *,
    render_mode: str,
) -> dict[str, Any]:
    if render_mode not in RELEASE_RENDER_MODES:
        raise RuntimeError(f"invalid release export render mode {render_mode!r}")
    common_roles = (
        "scene_depth",
        "base_color",
        "world_normals",
        "scene_color_hdr",
        "final",
    )
    required_roles = (
        common_roles[:4] + ("history_read", "history_write") + common_roles[4:]
        if render_mode == "taa-on"
        else common_roles
    )
    errors: list[str] = []
    extents: dict[str, tuple[int, int]] = {}
    expected_orders = {
        "scene_depth": ["D"],
        "base_color": ["R", "G", "B", "A"],
        "world_normals": ["X", "Y", "Z"],
        "scene_color_hdr": ["R", "G", "B", "A"],
        "history_read": ["R", "G", "B", "A"],
        "history_write": ["R", "G", "B", "A"],
        "final": ["R", "G", "B", "A"],
    }
    for role in required_roles:
        export = exports.get(role)
        if not isinstance(export, dict):
            errors.append(f"{role}: export is missing")
            continue
        if export.get("kind") != "npy":
            errors.append(f"{role}: release input is not NPY")
        if export.get("dtype") != "float32":
            errors.append(f"{role}: dtype is not float32")
        if export.get("channel_order") != expected_orders[role]:
            errors.append(
                f"{role}: channel order {export.get('channel_order')!r} does not "
                f"equal {expected_orders[role]!r}"
            )
        if export.get("dimension_class") != "screen":
            errors.append(f"{role}: dimension class is not screen")
        expected_evidence_only = role == "history_read"
        if export.get("evidence_only") is not expected_evidence_only:
            errors.append(
                f"{role}: evidence_only must be {expected_evidence_only}"
            )
        validation = export.get("npy_validation")
        if not isinstance(validation, dict) or validation.get("passed") is not True:
            errors.append(f"{role}: immediate NPY validation did not pass")
        readback_budget = export.get("readback_budget")
        if not isinstance(readback_budget, dict) or readback_budget.get("completed") is not True:
            errors.append(f"{role}: readback byte budget evidence did not complete")
        write_budget = export.get("write_budget")
        if not isinstance(write_budget, dict) or write_budget.get("completed") is not True:
            errors.append(f"{role}: pre-write NPY budget evidence did not complete")
        extent = export.get("screen_extent")
        if not isinstance(extent, dict):
            errors.append(f"{role}: screen extent is missing")
        else:
            width = int(extent.get("width", 0) or 0)
            height = int(extent.get("height", 0) or 0)
            if width <= 0 or height <= 0:
                errors.append(f"{role}: screen extent is invalid")
            else:
                extents[role] = (width, height)
        resource_key = "packed_normal" if role == "world_normals" else role
        resource = resources.get(resource_key)
        if not isinstance(resource, dict):
            errors.append(f"{role}: source resource evidence is missing")
            continue
        if int(export.get("source_resource_id", 0) or 0) != int(
            resource.get("resource_id", -1)
        ):
            errors.append(f"{role}: source resource ID does not match binding evidence")
        source_format = export.get("source_format")
        if not isinstance(source_format, dict) or source_format.get("name") != resource.get(
            "format"
        ):
            errors.append(f"{role}: source format does not match resource metadata")
    unexpected_history = [
        role for role in ("history_read", "history_write") if role in exports
    ]
    if render_mode == "no-post" and unexpected_history:
        errors.append(
            f"no-post release exports contain forbidden TAA history roles: {unexpected_history}"
        )
    unique_extents = sorted(set(extents.values()))
    if len(extents) == len(required_roles) and len(unique_extents) != 1:
        errors.append(f"release screen resource extents disagree: {extents!r}")
    return {
        "schema": "mgif-shadow-edge-extractor-input-validation-v1",
        "render_mode": render_mode,
        "passed": not errors,
        "errors": errors,
        "screen_extent": (
            {"width": unique_extents[0][0], "height": unique_extents[0][1]}
            if len(unique_extents) == 1
            else None
        ),
        "history_read_semantics": (
            "evidence-only" if render_mode == "taa-on" else "not-applicable-no-post"
        ),
        "roles": {
            role: {
                "shape": exports.get(role, {}).get("shape"),
                "dtype": exports.get(role, {}).get("dtype"),
                "channel_order": exports.get(role, {}).get("channel_order"),
                "source_format": exports.get(role, {}).get("source_format"),
                "evidence_only": exports.get(role, {}).get("evidence_only"),
            }
            for role in required_roles
        },
    }
def _d_extract_main() -> dict[str, Any]:
    output_dir = Path(args["output"]).resolve()
    release_gate = str(args.get("release_gate", "0")).lower() in (
        "1",
        "true",
        "yes",
        "on",
    )
    release_render_mode = str(
        args.get("release_render_mode", "diagnostic")
    )
    if release_gate and release_render_mode not in RELEASE_RENDER_MODES:
        raise RuntimeError(
            f"release extractor render mode is invalid: {release_render_mode!r}"
        )
    output_dir.mkdir(parents=True, exist_ok=True)
    _d_initialize_export_budget(output_dir)
    markers, marker_candidates = _d_choose_markers()
    automation_frame_markers = _d_collect_automation_frame_markers()
    usage_map = _d_build_usage_map()
    snapshots, pass_segments = _d_collect_pipeline_data(markers)

    csm, cascade_views = _d_pick_csm(markers, snapshots, usage_map)
    stages = {
        kind: _d_pick_color_stage(kind, markers, snapshots, usage_map)
        for kind in ("light", "taa", "final")
    }
    release_resource_evidence = (
        _d_build_release_resource_evidence(
            markers,
            snapshots,
            pass_segments,
            usage_map,
            render_mode=release_render_mode,
        )
        if release_gate
        else None
    )
    if release_gate and release_resource_evidence.get("passed") is True:
        release_resources = release_resource_evidence["resources"]
        release_passes = release_resource_evidence["passes"]
        stages["light"] = _d_direct_stage_record(
            "light",
            release_resources["scene_color_hdr"],
            int(release_passes["light"]["last_draw_eid"]),
            markers["light"],
        )
        if release_render_mode == "taa-on":
            stages["taa"] = _d_direct_stage_record(
                "taa",
                release_resources["history_write"],
                int(release_passes["taa"]["last_draw_eid"]),
                markers["taa"],
            )
        else:
            stages["taa"] = None
        stages["final"] = _d_direct_stage_record(
            "final",
            release_resources["final"],
            int(release_passes["final"]["last_draw_eid"]),
            markers["final"],
        )
    discovery_errors = []
    if csm is None:
        discovery_errors.append("CSM depth resource was not discovered")
    if release_gate and release_resource_evidence.get("passed") is not True:
        discovery_errors.extend(
            f"release resource binding: {error}"
            for error in release_resource_evidence.get("errors", [])
        )
    required_stage_kinds = (
        ("light", "taa", "final")
        if not release_gate or release_render_mode == "taa-on"
        else ("light", "final")
    )
    for kind in required_stage_kinds:
        if stages.get(kind) is None:
            discovery_errors.append(f"{kind} target was not discovered")

    exports: dict[str, Any] = {"csm": [], "light": None, "taa": None, "final": None}
    release_inputs: dict[str, Any] | None = None
    projection_sources: list[dict[str, Any]] = []
    if csm is not None:
        resource_id = int(csm["resource"]["resource_id"])
        array_size = int(csm["resource"].get("array_size", 1))
        if release_gate and array_size != 4:
            discovery_errors.append(
                f"release CSM depth array must have exactly four layers, got {array_size}"
            )
        else:
            slice_to_eid: dict[int, int] = {}
            for view in cascade_views:
                first_slice = int(view.get("first_slice", 0))
                count = int(view.get("num_slices", 0))
                if count <= 0:
                    if release_gate:
                        discovery_errors.append(
                            "release CSM attachment view reported non-positive num_slices"
                        )
                    continue
                if first_slice < 0 or first_slice + count > array_size:
                    if release_gate:
                        discovery_errors.append(
                            "release CSM attachment view exceeds the exact four-layer array: "
                            f"first_slice={first_slice}, num_slices={count}, "
                            f"array_size={array_size}"
                        )
                    continue
                for slice_index in range(first_slice, first_slice + count):
                    slice_to_eid[slice_index] = max(
                        int(view["eid"]),
                        slice_to_eid.get(slice_index, 0),
                    )
            fallback_eid = _d_latest_write_eid(
                usage_map,
                resource_id,
                markers.get("csm"),
                int(markers["csm"]["end_eid"]),
            )
            export_layers = tuple(range(4)) if release_gate else tuple(range(array_size))
            if release_gate:
                missing_layers = sorted(set(export_layers) - set(slice_to_eid))
                unexpected_layers = sorted(set(slice_to_eid) - set(export_layers))
                if missing_layers:
                    discovery_errors.append(
                        f"release CSM attachment evidence is missing layers {missing_layers}"
                    )
                if unexpected_layers:
                    discovery_errors.append(
                        f"release CSM attachment evidence has unexpected layers "
                        f"{unexpected_layers}"
                    )
            for slice_index in export_layers:
                eid = slice_to_eid.get(slice_index)
                if eid is None:
                    if release_gate:
                        continue
                    eid = fallback_eid
                exports["csm"].append(
                    _d_export_array(
                        output_dir,
                        f"csm_cascade_{slice_index}",
                        resource_id,
                        eid,
                        slice_index,
                        depth=True,
                        allow_png_fallback=not release_gate,
                        role=f"csm[{slice_index}]",
                        dimension_class="csm",
                    )
                )
                try:
                    matrices = _d_shader_matrices(eid)
                    projection_sources.append(
                        {
                            "cascade": slice_index,
                            "eid": eid,
                            "matrices": matrices,
                        }
                    )
                    if release_gate and not matrices:
                        discovery_errors.append(
                            f"release CSM cascade {slice_index} has no projection matrix evidence"
                        )
                except Exception as exc:
                    projection_sources.append(
                        {
                            "cascade": slice_index,
                            "eid": eid,
                            "matrices": [],
                            "error": str(exc),
                        }
                    )
                    if release_gate:
                        discovery_errors.append(
                            f"release CSM cascade {slice_index} projection extraction failed: {exc}"
                        )
    if release_gate and release_resource_evidence.get("passed") is True:
        try:
            resources = release_resource_evidence["resources"]
            passes = release_resource_evidence["passes"]
            light_eid = int(passes["light"]["last_draw_eid"])
            final_eid = int(passes["final"]["last_draw_eid"])
            taa_eid = (
                int(passes["taa"]["last_draw_eid"])
                if release_render_mode == "taa-on"
                else None
            )
            scene_hdr_eid = taa_eid if taa_eid is not None else final_eid
            release_exports = {
                "scene_depth": _d_export_array(
                    output_dir,
                    "release_scene_depth",
                    int(resources["scene_depth"]["resource_id"]),
                    light_eid,
                    0,
                    depth=True,
                    allow_png_fallback=False,
                    role="scene_depth",
                ),
                "base_color": _d_export_array(
                    output_dir,
                    "release_base_color",
                    int(resources["base_color"]["resource_id"]),
                    light_eid,
                    0,
                    depth=False,
                    allow_png_fallback=False,
                    role="base_color",
                ),
                "world_normals": _d_export_world_normals(
                    output_dir,
                    "release_world_normals",
                    int(resources["packed_normal"]["resource_id"]),
                    light_eid,
                ),
                "scene_color_hdr": _d_export_array(
                    output_dir,
                    "release_scene_color_hdr",
                    int(resources["scene_color_hdr"]["resource_id"]),
                    scene_hdr_eid,
                    0,
                    depth=False,
                    allow_png_fallback=False,
                    role="scene_color_hdr",
                ),
                "final": _d_export_array(
                    output_dir,
                    "release_final",
                    int(resources["final"]["resource_id"]),
                    final_eid,
                    0,
                    depth=False,
                    allow_png_fallback=False,
                    role="final",
                ),
            }
            if release_render_mode == "taa-on":
                assert taa_eid is not None
                release_exports.update(
                    {
                        "history_read": _d_export_array(
                            output_dir,
                            "release_history_read",
                            int(resources["history_read"]["resource_id"]),
                            taa_eid,
                            0,
                            depth=False,
                            allow_png_fallback=False,
                            role="history_read",
                            evidence_only=True,
                        ),
                        "history_write": _d_export_array(
                            output_dir,
                            "release_history_write",
                            int(resources["history_write"]["resource_id"]),
                            final_eid,
                            0,
                            depth=False,
                            allow_png_fallback=False,
                            role="history_write",
                        ),
                    }
                )
            release_validation = _d_validate_release_export_set(
                release_exports,
                resources,
                render_mode=release_render_mode,
            )
            if release_validation.get("passed") is not True:
                raise RuntimeError(
                    "release export-set validation failed: "
                    + "; ".join(release_validation.get("errors", []))
                )
            release_inputs = {
                "schema": RELEASE_INPUTS_SCHEMA,
                "render_mode": release_render_mode,
                "passed": True,
                "camera": release_resource_evidence["camera"],
                "screen_extent": release_validation["screen_extent"],
                "validation": release_validation,
                "resources": {
                    role: {
                        "resource": resources[
                            "packed_normal" if role == "world_normals" else role
                        ],
                        "export": export,
                        "evidence_only": role == "history_read",
                    }
                    for role, export in release_exports.items()
                },
                "errors": [],
            }
            exports["light"] = release_exports["scene_color_hdr"]
            exports["taa"] = (
                release_exports["history_write"]
                if release_render_mode == "taa-on"
                else None
            )
            exports["final"] = release_exports["final"]
        except Exception as exc:
            release_inputs = {
                "schema": RELEASE_INPUTS_SCHEMA,
                "render_mode": release_render_mode,
                "passed": False,
                "camera": release_resource_evidence.get("camera"),
                "screen_extent": None,
                "validation": None,
                "resources": {},
                "errors": [f"{type(exc).__name__}: {exc}"],
            }
            discovery_errors.append(f"release input export failed: {exc}")
    else:
        for kind, stage in stages.items():
            if stage is None:
                continue
            resource_id = int(stage["resource"]["resource_id"])
            try:
                exports[kind] = _d_export_array(
                    output_dir,
                    kind,
                    resource_id,
                    int(stage["eid"]),
                    0,
                    depth=False,
                    allow_png_fallback=not release_gate,
                    role=kind,
                    dimension_class="screen",
                )
            except Exception as exc:
                stage["export_error"] = str(exc)
                discovery_errors.append(f"{kind} export failed: {exc}")

    public_markers = {
        kind: {key: value for key, value in marker.items() if key != "_action"}
        for kind, marker in markers.items()
    }
    usage_summary = {}
    selected_resources = {
        "csm": int(csm["resource"]["resource_id"]) if csm else None,
        **{
            kind: int(stage["resource"]["resource_id"]) if stage else None
            for kind, stage in stages.items()
        },
    }
    for kind, resource_id in selected_resources.items():
        if resource_id is None:
            continue
        usage_summary[kind] = usage_map.get(resource_id, [])

    manifest = {
        "tool_version": TOOL_VERSION,
        "extractor_tool_evidence": _collect_tool_file_evidence(
            Path(__file__),
            role="comparator",
        ),
        "release_gate": release_gate,
        "release_render_mode": release_render_mode,
        "export_budget": _d_export_budget_snapshot(),
        "capture": str(getattr(state, "capture", "")),
        "markers": public_markers,
        "marker_candidates": marker_candidates,
        "automation_frame_markers": automation_frame_markers,
        "pass_segments": pass_segments,
        "pipeline_snapshots": snapshots,
        "discovery": {"csm": csm, **stages},
        "release_resource_evidence": release_resource_evidence,
        "release_inputs": release_inputs,
        "projection_sources": projection_sources,
        "exports": exports,
        "selected_resource_usage": usage_summary,
        "errors": discovery_errors,
    }
    manifest_path = output_dir / "manifest.json"
    manifest_path.write_text(
        json.dumps(manifest, indent=2, sort_keys=True),
        encoding="utf-8",
    )
    return {
        "manifest": str(manifest_path),
        "selected_resources": selected_resources,
        "errors": discovery_errors,
    }


# ---------------------------------------------------------------------------
# Host-side session management, comparison, projection analysis, and CLI.
# ---------------------------------------------------------------------------


class ComparatorError(RuntimeError):
    """Fatal comparator failure."""


if os.name == "nt":
    from ctypes import wintypes

    class _HostFileTime(ctypes.Structure):
        _fields_ = [
            ("dwLowDateTime", wintypes.DWORD),
            ("dwHighDateTime", wintypes.DWORD),
        ]

    _HOST_KERNEL32 = ctypes.WinDLL("kernel32", use_last_error=True)
    _HOST_KERNEL32.OpenProcess.argtypes = [
        wintypes.DWORD,
        wintypes.BOOL,
        wintypes.DWORD,
    ]
    _HOST_KERNEL32.OpenProcess.restype = wintypes.HANDLE
    _HOST_KERNEL32.GetProcessId.argtypes = [wintypes.HANDLE]
    _HOST_KERNEL32.GetProcessId.restype = wintypes.DWORD
    _HOST_KERNEL32.GetProcessTimes.argtypes = [
        wintypes.HANDLE,
        ctypes.POINTER(_HostFileTime),
        ctypes.POINTER(_HostFileTime),
        ctypes.POINTER(_HostFileTime),
        ctypes.POINTER(_HostFileTime),
    ]
    _HOST_KERNEL32.GetProcessTimes.restype = wintypes.BOOL
    _HOST_KERNEL32.GetFileTime.argtypes = [
        wintypes.HANDLE,
        ctypes.POINTER(_HostFileTime),
        ctypes.POINTER(_HostFileTime),
        ctypes.POINTER(_HostFileTime),
    ]
    _HOST_KERNEL32.GetFileTime.restype = wintypes.BOOL
    _HOST_KERNEL32.GetFinalPathNameByHandleW.argtypes = [
        wintypes.HANDLE,
        wintypes.LPWSTR,
        wintypes.DWORD,
        wintypes.DWORD,
    ]
    _HOST_KERNEL32.GetFinalPathNameByHandleW.restype = wintypes.DWORD
    _HOST_KERNEL32.GetDriveTypeW.argtypes = [wintypes.LPCWSTR]
    _HOST_KERNEL32.GetDriveTypeW.restype = wintypes.UINT
    _HOST_KERNEL32.GetVolumeInformationByHandleW.argtypes = [
        wintypes.HANDLE,
        wintypes.LPWSTR,
        wintypes.DWORD,
        ctypes.POINTER(wintypes.DWORD),
        ctypes.POINTER(wintypes.DWORD),
        ctypes.POINTER(wintypes.DWORD),
        wintypes.LPWSTR,
        wintypes.DWORD,
    ]
    _HOST_KERNEL32.GetVolumeInformationByHandleW.restype = wintypes.BOOL
    _HOST_KERNEL32.QueryFullProcessImageNameW.argtypes = [
        wintypes.HANDLE,
        wintypes.DWORD,
        wintypes.LPWSTR,
        ctypes.POINTER(wintypes.DWORD),
    ]
    _HOST_KERNEL32.QueryFullProcessImageNameW.restype = wintypes.BOOL
    _HOST_KERNEL32.WaitForSingleObject.argtypes = [
        wintypes.HANDLE,
        wintypes.DWORD,
    ]
    _HOST_KERNEL32.WaitForSingleObject.restype = wintypes.DWORD
    _HOST_KERNEL32.TerminateProcess.argtypes = [
        wintypes.HANDLE,
        wintypes.UINT,
    ]
    _HOST_KERNEL32.TerminateProcess.restype = wintypes.BOOL
    _HOST_KERNEL32.CloseHandle.argtypes = [wintypes.HANDLE]
    _HOST_KERNEL32.CloseHandle.restype = wintypes.BOOL

    _HOST_PROCESS_TERMINATE = 0x0001
    _HOST_PROCESS_QUERY_LIMITED_INFORMATION = 0x1000
    _HOST_SYNCHRONIZE = 0x00100000
    _HOST_WAIT_OBJECT_0 = 0x00000000
    _HOST_WAIT_TIMEOUT = 0x00000102
    _HOST_WAIT_FAILED = 0xFFFFFFFF
    _HOST_WINDOWS_EPOCH_OFFSET_SECONDS = 11644473600
    _HOST_WINDOWS_TICKS_PER_SECOND = 10_000_000


def _host_normalized_image(value: str | Path) -> str:
    return os.path.normcase(os.path.abspath(str(value)))


def _host_process_images_match(expected: str | Path, observed: str | Path) -> bool:
    try:
        return os.path.samefile(expected, observed)
    except (OSError, ValueError):
        return _host_normalized_image(expected) == _host_normalized_image(observed)


class _StableReplayProcessIdentity:
    """One held native process identity used for replay-daemon cleanup only."""

    def __init__(
        self,
        *,
        pid: int,
        identity: str,
        creation_time_key: str,
        creation_time_unix_seconds: float,
        image_path: str,
        native_handle: int,
        terminate_access: bool,
        backend: str,
        creation_filetime_ticks: int | None = None,
    ) -> None:
        self.pid = int(pid)
        self.identity = identity
        self.creation_time_key = creation_time_key
        self.creation_time_unix_seconds = float(creation_time_unix_seconds)
        self.image_path = image_path
        self._native_handle = native_handle
        self._terminate_access = bool(terminate_access)
        self._backend = backend
        self._creation_filetime_ticks = creation_filetime_ticks
        self._creation_time_unix_ns = (
            (int(creation_filetime_ticks) - WINDOWS_FILETIME_EPOCH_OFFSET_TICKS)
            * WINDOWS_FILETIME_TICK_NS
            if isinstance(creation_filetime_ticks, int)
            and not isinstance(creation_filetime_ticks, bool)
            and creation_filetime_ticks > WINDOWS_FILETIME_EPOCH_OFFSET_TICKS
            else None
        )
        self._closed = False

    @classmethod
    def acquire(
        cls,
        pid: int,
        *,
        expected_image: Path | None = None,
        require_terminate: bool = True,
    ) -> "_StableReplayProcessIdentity":
        if pid <= 0:
            raise ComparatorError(f"cannot acquire stable replay identity for invalid pid {pid}")
        if os.name == "nt":
            access = _HOST_PROCESS_QUERY_LIMITED_INFORMATION | _HOST_SYNCHRONIZE
            if require_terminate:
                access |= _HOST_PROCESS_TERMINATE
            handle = _HOST_KERNEL32.OpenProcess(access, False, pid)
            if not handle:
                error_code = ctypes.get_last_error()
                raise ComparatorError(
                    f"OpenProcess failed for replay daemon pid {pid} with error "
                    f"{error_code}: {ctypes.FormatError(error_code).strip()}"
                )
            try:
                observed_pid = int(_HOST_KERNEL32.GetProcessId(handle))
                if observed_pid != pid:
                    raise ComparatorError(
                        f"stable replay handle pid mismatch: requested={pid}, "
                        f"observed={observed_pid}"
                    )
                creation = _HostFileTime()
                exit_time = _HostFileTime()
                kernel_time = _HostFileTime()
                user_time = _HostFileTime()
                if not _HOST_KERNEL32.GetProcessTimes(
                    handle,
                    ctypes.byref(creation),
                    ctypes.byref(exit_time),
                    ctypes.byref(kernel_time),
                    ctypes.byref(user_time),
                ):
                    error_code = ctypes.get_last_error()
                    raise ComparatorError(
                        f"GetProcessTimes failed for replay daemon pid {pid} with "
                        f"error {error_code}: {ctypes.FormatError(error_code).strip()}"
                    )
                creation_ticks = (
                    int(creation.dwHighDateTime) << 32
                ) | int(creation.dwLowDateTime)
                if creation_ticks <= 0:
                    raise ComparatorError(
                        f"replay daemon pid {pid} has an invalid creation time"
                    )
                capacity = wintypes.DWORD(32768)
                buffer = ctypes.create_unicode_buffer(capacity.value)
                if not _HOST_KERNEL32.QueryFullProcessImageNameW(
                    handle,
                    0,
                    buffer,
                    ctypes.byref(capacity),
                ):
                    error_code = ctypes.get_last_error()
                    raise ComparatorError(
                        f"QueryFullProcessImageNameW failed for replay daemon pid {pid} "
                        f"with error {error_code}: "
                        f"{ctypes.FormatError(error_code).strip()}"
                    )
                image_path = buffer.value
                if expected_image is not None and not _host_process_images_match(
                    expected_image,
                    image_path,
                ):
                    raise ComparatorError(
                        f"replay daemon image mismatch for pid {pid}: "
                        f"observed={image_path!r}, expected={str(expected_image)!r}"
                    )
                creation_key = f"winfiletime:{creation_ticks}"
                return cls(
                    pid=pid,
                    identity=f"{pid}@{creation_key}",
                    creation_time_key=creation_key,
                    creation_time_unix_seconds=(
                        creation_ticks / _HOST_WINDOWS_TICKS_PER_SECOND
                        - _HOST_WINDOWS_EPOCH_OFFSET_SECONDS
                    ),
                    image_path=image_path,
                    native_handle=int(handle),
                    terminate_access=require_terminate,
                    backend="windows-process-handle",
                    creation_filetime_ticks=creation_ticks,
                )
            except Exception:
                _HOST_KERNEL32.CloseHandle(handle)
                raise

        if not hasattr(os, "pidfd_open"):
            raise ComparatorError(
                "stable replay process identity requires pidfd_open outside Windows"
            )
        try:
            import psutil

            pidfd = os.pidfd_open(pid, 0)
            try:
                process = psutil.Process(pid)
                creation_time = float(process.create_time())
                image_path = str(process.exe())
                if expected_image is not None and not _host_process_images_match(
                    expected_image,
                    image_path,
                ):
                    raise ComparatorError(
                        f"replay daemon image mismatch for pid {pid}: "
                        f"observed={image_path!r}, expected={str(expected_image)!r}"
                    )
                creation_key = f"unix:{creation_time!r}"
                return cls(
                    pid=pid,
                    identity=f"{pid}@{creation_key}",
                    creation_time_key=creation_key,
                    creation_time_unix_seconds=creation_time,
                    image_path=image_path,
                    native_handle=pidfd,
                    terminate_access=require_terminate,
                    backend="linux-pidfd",
                )
            except Exception:
                os.close(pidfd)
                raise
        except (OSError, ImportError) as exc:
            raise ComparatorError(
                f"failed to acquire stable replay pidfd identity for pid {pid}: {exc}"
            ) from exc

    def metadata(self) -> dict[str, Any]:
        return {
            "pid": self.pid,
            "identity": self.identity,
            "creation_time_key": self.creation_time_key,
            "creation_time_unix_seconds": self.creation_time_unix_seconds,
            "image_path": self.image_path,
            "backend": self._backend,
            "native_handle_held": not self._closed,
            "terminate_access": self._terminate_access,
            "creation_filetime_ticks": self._creation_filetime_ticks,
            "creation_time_unix_ns": self._creation_time_unix_ns,
        }

    def creation_time_matches(self, create_time: float) -> bool:
        if os.name != "nt":
            return float(create_time) == self.creation_time_unix_seconds
        expected_ticks = round(
            (float(create_time) + _HOST_WINDOWS_EPOCH_OFFSET_SECONDS)
            * _HOST_WINDOWS_TICKS_PER_SECOND
        )
        observed_ticks = int(self.creation_time_key.split(":", 1)[1])
        return abs(expected_ticks - observed_ticks) <= 32

    def is_running(self) -> bool:
        if self._closed:
            raise ComparatorError(
                f"stable replay handle is closed for {self.identity}"
            )
        if os.name == "nt":
            result = int(_HOST_KERNEL32.WaitForSingleObject(self._native_handle, 0))
            if result == _HOST_WAIT_TIMEOUT:
                return True
            if result == _HOST_WAIT_OBJECT_0:
                return False
            error_code = ctypes.get_last_error()
            raise ComparatorError(
                f"WaitForSingleObject failed for {self.identity}: result={result}, "
                f"error={error_code}: {ctypes.FormatError(error_code).strip()}"
            )
        import select

        poller = select.poll()
        poller.register(self._native_handle, select.POLLIN)
        return not bool(poller.poll(0))

    def terminate(self, *, timeout: float) -> dict[str, Any]:
        result: dict[str, Any] = {
            **self.metadata(),
            "same_native_handle": True,
            "tree_cleanup_requested": False,
            "termination_requested": False,
            "passed": False,
        }
        if not math.isfinite(timeout) or timeout <= 0.0:
            result["error"] = f"invalid replay termination timeout {timeout!r}"
            return result
        try:
            running_before = self.is_running()
        except Exception as exc:
            result["error"] = (
                f"stable replay liveness check failed closed: "
                f"{type(exc).__name__}: {exc}"
            )
            return result
        result["running_before"] = running_before
        if not running_before:
            result.update(
                {
                    "already_exited": True,
                    "running_after": False,
                    "passed": True,
                }
            )
            return result
        if not self._terminate_access:
            result["error"] = (
                "stable replay handle lacks terminate access; refusing PID fallback"
            )
            return result
        result["termination_requested"] = True
        if os.name == "nt":
            if not _HOST_KERNEL32.TerminateProcess(self._native_handle, 1):
                error_code = ctypes.get_last_error()
                try:
                    if not self.is_running():
                        result.update(
                            {
                                "original_exited_before_terminate": True,
                                "running_after": False,
                                "passed": True,
                            }
                        )
                        return result
                except Exception as exc:
                    result["post_terminate_check_error"] = (
                        f"{type(exc).__name__}: {exc}"
                    )
                result["error"] = (
                    "TerminateProcess failed on the held replay handle; "
                    f"error={error_code}: {ctypes.FormatError(error_code).strip()}; "
                    "refusing PID fallback"
                )
                return result
            timeout_ms = min(0xFFFFFFFE, max(1, math.ceil(timeout * 1000.0)))
            wait_result = int(
                _HOST_KERNEL32.WaitForSingleObject(self._native_handle, timeout_ms)
            )
            if wait_result == _HOST_WAIT_OBJECT_0:
                result.update({"running_after": False, "passed": True})
                return result
            if wait_result == _HOST_WAIT_TIMEOUT:
                result.update(
                    {
                        "running_after": True,
                        "error": "timed out waiting on held replay process handle",
                    }
                )
                return result
            error_code = ctypes.get_last_error()
            result["error"] = (
                f"replay handle wait failed with {error_code}: "
                f"{ctypes.FormatError(error_code).strip()}"
            )
            return result

        if not hasattr(signal, "pidfd_send_signal"):
            result["error"] = "pidfd_send_signal unavailable; refusing PID fallback"
            return result
        import select

        signal.pidfd_send_signal(self._native_handle, signal.SIGTERM, None, 0)
        poller = select.poll()
        poller.register(self._native_handle, select.POLLIN)
        if poller.poll(max(1, math.ceil(timeout * 1000.0))):
            result.update({"running_after": False, "passed": True, "signal": "SIGTERM"})
            return result
        signal.pidfd_send_signal(self._native_handle, signal.SIGKILL, None, 0)
        if poller.poll(2000):
            result.update({"running_after": False, "passed": True, "signal": "SIGKILL"})
            return result
        result.update(
            {
                "running_after": True,
                "error": "timed out waiting on held replay pidfd after SIGKILL",
            }
        )
        return result

    def close(self) -> dict[str, Any]:
        result = {
            "identity": self.identity,
            "closed_before": self._closed,
            "closed": False,
        }
        if self._closed:
            result["closed"] = True
            return result
        try:
            if os.name == "nt":
                if not _HOST_KERNEL32.CloseHandle(self._native_handle):
                    error_code = ctypes.get_last_error()
                    result["error"] = (
                        f"CloseHandle failed with {error_code}: "
                        f"{ctypes.FormatError(error_code).strip()}"
                    )
                    return result
            else:
                os.close(self._native_handle)
            self._closed = True
            result["closed"] = True
            return result
        except OSError as exc:
            result["error"] = f"{type(exc).__name__}: {exc}"
            return result


def _canonical_json_sha256(value: Any) -> str:
    payload = json.dumps(
        value,
        sort_keys=True,
        separators=(",", ":"),
        ensure_ascii=True,
        allow_nan=False,
    ).encode("utf-8")
    return hashlib.sha256(payload).hexdigest()


def _sha256_file(path: Path) -> str:
    digest = hashlib.sha256()
    with path.open("rb") as stream:
        while True:
            chunk = stream.read(1024 * 1024)
            if not chunk:
                break
            digest.update(chunk)
    return digest.hexdigest()


def _file_identity(path: Path) -> dict[str, Any]:
    stat = path.stat()
    return {
        "path": str(path.resolve()),
        "size_bytes": int(stat.st_size),
        "sha256": _sha256_file(path),
    }


def _python_tool_version(source: str, *, role: str, path: Path) -> str:
    try:
        tree = ast.parse(source, filename=str(path))
    except SyntaxError as exc:
        raise ComparatorError(f"cannot parse {role} tool source {path}: {exc}") from exc
    if role in ("harness", "comparator"):
        for node in tree.body:
            if not isinstance(node, (ast.Assign, ast.AnnAssign)):
                continue
            targets = node.targets if isinstance(node, ast.Assign) else [node.target]
            value = node.value
            if any(isinstance(target, ast.Name) and target.id == "TOOL_VERSION" for target in targets):
                if isinstance(value, ast.Constant) and isinstance(value.value, str):
                    return value.value
        raise ComparatorError(f"{role} tool {path} has no literal TOOL_VERSION")
    if role == "roi":
        for node in tree.body:
            if not isinstance(node, ast.ClassDef) or node.name != "ShadowEdgeMetricsResult":
                continue
            for member in node.body:
                if not isinstance(member, ast.AnnAssign):
                    continue
                if not isinstance(member.target, ast.Name) or member.target.id != "schema":
                    continue
                if isinstance(member.value, ast.Constant) and isinstance(member.value.value, str):
                    return member.value.value
        raise ComparatorError(
            f"ROI tool {path} has no literal ShadowEdgeMetricsResult.schema"
        )
    raise ComparatorError(f"unknown tool evidence role {role!r}")


def _collect_tool_file_evidence(path: Path, *, role: str) -> dict[str, Any]:
    try:
        resolved = path.expanduser().resolve(strict=True)
    except (OSError, RuntimeError) as exc:
        raise ComparatorError(
            f"cannot resolve {role} tool path {path}: {type(exc).__name__}: {exc}"
        ) from exc
    if not resolved.is_file() or not resolved.is_absolute():
        raise ComparatorError(f"{role} tool path is not an absolute file: {resolved}")
    digest = hashlib.sha256()
    payload = bytearray()
    try:
        with resolved.open("rb") as stream:
            stat_before = os.fstat(stream.fileno())
            while True:
                chunk = stream.read(1024 * 1024)
                if not chunk:
                    break
                digest.update(chunk)
                payload.extend(chunk)
            stat_after = os.fstat(stream.fileno())
        path_stat = resolved.stat()
    except OSError as exc:
        raise ComparatorError(
            f"cannot read {role} tool {resolved}: {type(exc).__name__}: {exc}"
        ) from exc
    stable_fields = ("st_dev", "st_ino", "st_size", "st_mtime_ns")
    changed = [
        field
        for field in stable_fields
        if getattr(stat_before, field, None) != getattr(stat_after, field, None)
        or getattr(stat_after, field, None) != getattr(path_stat, field, None)
    ]
    if changed:
        raise ComparatorError(
            f"{role} tool changed while hashing {resolved}: {changed}"
        )
    if int(stat_after.st_size) <= 0 or len(payload) != int(stat_after.st_size):
        raise ComparatorError(f"{role} tool has an invalid stable size: {resolved}")
    try:
        source = bytes(payload).decode("utf-8-sig")
    except UnicodeDecodeError as exc:
        raise ComparatorError(f"{role} tool is not UTF-8 source: {resolved}") from exc
    return {
        "schema": TOOL_FILE_EVIDENCE_SCHEMA,
        "role": role,
        "absolute_path": str(resolved),
        "version": _python_tool_version(source, role=role, path=resolved),
        "size_bytes": int(stat_after.st_size),
        "sha256": digest.hexdigest(),
        "read_consistent": True,
    }


def _stable_dependency_file_record(
    path: Path,
    *,
    relative_path: str,
    role: str,
) -> dict[str, Any]:
    try:
        resolved = path.resolve(strict=True)
        with resolved.open("rb") as stream:
            stat_before = os.fstat(stream.fileno())
            digest = hashlib.sha256()
            size = 0
            while True:
                chunk = stream.read(1024 * 1024)
                if not chunk:
                    break
                digest.update(chunk)
                size += len(chunk)
            stat_after = os.fstat(stream.fileno())
        path_after = resolved.stat()
    except OSError as exc:
        raise ComparatorError(
            f"cannot hash {role} dependency file {path}: {type(exc).__name__}: {exc}"
        ) from exc
    if not all(
        stat.S_ISREG(candidate.st_mode)
        for candidate in (stat_before, stat_after, path_after)
    ):
        raise ComparatorError(
            f"{role} dependency path is not a regular file: {resolved}"
        )
    stable_fields = ("st_dev", "st_ino", "st_size", "st_mtime_ns")
    changed = [
        field
        for field in stable_fields
        if getattr(stat_before, field, None) != getattr(stat_after, field, None)
        or getattr(stat_after, field, None) != getattr(path_after, field, None)
    ]
    if changed or size != int(stat_after.st_size):
        raise ComparatorError(
            f"{role} dependency file changed or had an inconsistent read while hashing: "
            f"{resolved}, changed={changed}, bytes={size}"
        )
    return {
        "relative_path": relative_path.replace("\\", "/"),
        "absolute_path": str(resolved),
        "size_bytes": size,
        "sha256": digest.hexdigest(),
    }


def _rdc_cli_ast_name(node: ast.AST) -> str:
    if isinstance(node, ast.Name):
        return node.id
    if isinstance(node, ast.Attribute):
        prefix = _rdc_cli_ast_name(node.value)
        return f"{prefix}.{node.attr}" if prefix else node.attr
    return ""


def _rdc_cli_function(
    tree: ast.Module,
    name: str,
    *,
    source_path: Path,
) -> ast.FunctionDef:
    matches = [
        node
        for node in tree.body
        if isinstance(node, ast.FunctionDef) and node.name == name
    ]
    if len(matches) != 1:
        raise ComparatorError(
            f"rdc-cli ordering proof expected one {name} function in {source_path}, "
            f"found {len(matches)}"
        )
    return matches[0]


def _rdc_cli_unique_call_line(
    function: ast.FunctionDef,
    target: str,
    *,
    source_path: Path,
) -> int:
    lines = sorted(
        int(node.lineno)
        for node in ast.walk(function)
        if isinstance(node, ast.Call) and _rdc_cli_ast_name(node.func) == target
    )
    if len(lines) != 1:
        raise ComparatorError(
            f"rdc-cli ordering proof expected one {target} call in "
            f"{source_path}:{function.name}, found {lines!r}"
        )
    return lines[0]


def _collect_rdc_cli_session_ordering_evidence(
    package_root: Path,
    package_version: str,
    records: list[dict[str, Any]],
) -> dict[str, Any]:
    required_sources = (
        "rdc/services/session_service.py",
        "rdc/session_state.py",
        "rdc/_platform.py",
    )
    records_by_path = {
        str(record.get("relative_path", "")): record
        for record in records
        if isinstance(record, dict)
    }
    trees: dict[str, ast.Module] = {}
    source_evidence: dict[str, dict[str, Any]] = {}
    for relative in required_sources:
        record = records_by_path.get(relative)
        if not isinstance(record, dict):
            raise ComparatorError(
                f"rdc-cli ordering proof is missing hashed source {relative}"
            )
        source_path = Path(str(record.get("absolute_path", ""))).resolve(strict=True)
        try:
            payload = source_path.read_bytes()
        except OSError as exc:
            raise ComparatorError(
                f"rdc-cli ordering source is unreadable {source_path}: "
                f"{type(exc).__name__}: {exc}"
            ) from exc
        digest = hashlib.sha256(payload).hexdigest()
        if len(payload) != record.get("size_bytes") or digest != record.get("sha256"):
            raise ComparatorError(
                f"rdc-cli ordering source changed after package hashing: {source_path}"
            )
        try:
            source = payload.decode("utf-8-sig")
            trees[relative] = ast.parse(source, filename=str(source_path))
        except (UnicodeDecodeError, SyntaxError) as exc:
            raise ComparatorError(
                f"rdc-cli ordering source cannot be parsed {source_path}: "
                f"{type(exc).__name__}: {exc}"
            ) from exc
        source_evidence[relative] = {
            "absolute_path": str(source_path),
            "size_bytes": len(payload),
            "sha256": digest,
        }

    service_path = Path(source_evidence[required_sources[0]]["absolute_path"])
    service_tree = trees[required_sources[0]]
    open_session = _rdc_cli_function(
        service_tree,
        "open_session",
        source_path=service_path,
    )
    imports_create_session = any(
        isinstance(node, ast.ImportFrom)
        and node.module == "rdc.session_state"
        and any(alias.name == "create_session" for alias in node.names)
        for node in service_tree.body
    )
    start_line = _rdc_cli_unique_call_line(
        open_session,
        "start_daemon",
        source_path=service_path,
    )
    ping_line = _rdc_cli_unique_call_line(
        open_session,
        "wait_for_ping",
        source_path=service_path,
    )
    publish_line = _rdc_cli_unique_call_line(
        open_session,
        "create_session",
        source_path=service_path,
    )

    state_path = Path(source_evidence[required_sources[1]]["absolute_path"])
    state_tree = trees[required_sources[1]]
    create_session = _rdc_cli_function(
        state_tree,
        "create_session",
        source_path=state_path,
    )
    save_session = _rdc_cli_function(
        state_tree,
        "save_session",
        source_path=state_path,
    )
    save_line = _rdc_cli_unique_call_line(
        create_session,
        "save_session",
        source_path=state_path,
    )
    secure_write_line = _rdc_cli_unique_call_line(
        save_session,
        "_platform.secure_write_text",
        source_path=state_path,
    )

    platform_path = Path(source_evidence[required_sources[2]]["absolute_path"])
    platform_tree = trees[required_sources[2]]
    secure_write = _rdc_cli_function(
        platform_tree,
        "secure_write_text",
        source_path=platform_path,
    )
    windows_write_line = _rdc_cli_unique_call_line(
        secure_write,
        "path.write_text",
        source_path=platform_path,
    )
    verified = (
        imports_create_session
        and start_line < ping_line < publish_line
        and save_line > int(create_session.lineno)
        and secure_write_line > int(save_session.lineno)
        and windows_write_line > int(secure_write.lineno)
    )
    if not verified:
        raise ComparatorError(
            "rdc-cli source does not prove daemon start/ping before named state publication"
        )
    return {
        "schema": RDC_CLI_SESSION_ORDERING_SCHEMA,
        "verified": True,
        "package_version": package_version,
        "source": "AST inspection of the fully hashed installed rdc-cli package",
        "daemon_created_before_state_publication": True,
        "daemon_ready_before_state_publication": True,
        "state_publication_call_chain": [
            "open_session.start_daemon",
            "open_session.wait_for_ping",
            "open_session.create_session",
            "create_session.save_session",
            "save_session._platform.secure_write_text",
            "secure_write_text.path.write_text",
        ],
        "call_lines": {
            "start_daemon": start_line,
            "wait_for_ping": ping_line,
            "create_session": publish_line,
            "save_session": save_line,
            "secure_write_text": secure_write_line,
            "windows_path_write_text": windows_write_line,
        },
        "source_files": source_evidence,
    }

def _collect_rdc_cli_package_evidence() -> dict[str, Any]:
    try:
        import rdc

        distribution = importlib.metadata.distribution("rdc-cli")
    except Exception as exc:
        raise ComparatorError(
            f"rdc-cli package evidence is unavailable: {type(exc).__name__}: {exc}"
        ) from exc
    package_root = Path(rdc.__file__).resolve(strict=True).parent
    records: list[dict[str, Any]] = []
    for entry in sorted(
        list(distribution.files or ()),
        key=lambda value: str(value).replace("\\", "/"),
    ):
        relative = str(entry).replace("\\", "/")
        parts = tuple(part.casefold() for part in Path(relative).parts)
        if "__pycache__" in parts or Path(relative).suffix.casefold() in (".pyc", ".pyo"):
            continue
        resolved = Path(distribution.locate_file(entry))
        if not resolved.is_file():
            raise ComparatorError(
                f"rdc-cli distribution file is missing: {relative} -> {resolved}"
            )
        records.append(
            _stable_dependency_file_record(
                resolved,
                relative_path=relative,
                role="rdc_cli_package",
            )
        )
    required_suffixes = {
        "rdc/__init__.py",
        "rdc/daemon_client.py",
        "rdc/protocol.py",
        "rdc/services/session_service.py",
        "rdc/session_state.py",
        "rdc/_platform.py",
    }
    observed = {record["relative_path"] for record in records}
    missing = sorted(required_suffixes - observed)
    if not records or missing:
        raise ComparatorError(
            f"rdc-cli full distribution evidence is incomplete: missing={missing!r}"
        )
    package_version = str(getattr(rdc, "__version__", "") or "")
    distribution_version = str(distribution.version)
    if not package_version or package_version != distribution_version:
        raise ComparatorError(
            f"rdc-cli package/distribution version mismatch: "
            f"package={package_version!r}, distribution={distribution_version!r}"
        )
    session_ordering = _collect_rdc_cli_session_ordering_evidence(
        package_root,
        distribution_version,
        records,
    )
    canonical = [
        {
            "relative_path": record["relative_path"],
            "size_bytes": record["size_bytes"],
            "sha256": record["sha256"],
        }
        for record in records
    ]
    return {
        "schema": RUNTIME_DEPENDENCY_EVIDENCE_SCHEMA,
        "role": "rdc_cli_package",
        "absolute_path": str(package_root),
        "version": distribution_version,
        "size_bytes": sum(int(record["size_bytes"]) for record in records),
        "sha256": _canonical_json_sha256(canonical),
        "read_consistent": True,
        "file_count": len(records),
        "zero_byte_file_count": sum(
            int(record["size_bytes"]) == 0 for record in records
        ),
        "files": records,
        "exclusions": ["__pycache__", "*.pyc", "*.pyo"],
        "session_publication_ordering": session_ordering,
    }


def _collect_renderdoc_module_evidence() -> dict[str, Any]:
    try:
        from rdc.discover import find_renderdoc

        renderdoc_module = find_renderdoc()
    except Exception as exc:
        raise ComparatorError(
            f"RenderDoc module discovery failed: {type(exc).__name__}: {exc}"
        ) from exc
    if renderdoc_module is None:
        raise ComparatorError("RenderDoc module discovery returned None")
    module_file = getattr(renderdoc_module, "__file__", None)
    if not module_file:
        raise ComparatorError("loaded RenderDoc module has no __file__ identity")
    record = _stable_dependency_file_record(
        Path(str(module_file)),
        relative_path=Path(str(module_file)).name,
        role="renderdoc_module",
    )
    try:
        version = str(renderdoc_module.GetVersionString())
    except Exception as exc:
        raise ComparatorError(
            f"RenderDoc module version is unavailable: {type(exc).__name__}: {exc}"
        ) from exc
    if not version:
        raise ComparatorError("RenderDoc module version is empty")
    return {
        "schema": RUNTIME_DEPENDENCY_EVIDENCE_SCHEMA,
        "role": "renderdoc_module",
        "absolute_path": record["absolute_path"],
        "version": version,
        "size_bytes": record["size_bytes"],
        "sha256": record["sha256"],
        "read_consistent": True,
        "file_count": 1,
        "files": [record],
    }

def _validate_runtime_dependency_evidence(
    record: Any,
    *,
    role: str,
) -> list[str]:
    errors: list[str] = []
    if not isinstance(record, dict):
        return [f"{role} runtime dependency evidence is missing"]
    if record.get("schema") != RUNTIME_DEPENDENCY_EVIDENCE_SCHEMA:
        errors.append(f"{role} runtime dependency schema is invalid")
    if record.get("role") != role:
        errors.append(f"{role} runtime dependency role differs")
    absolute_path = record.get("absolute_path")
    if not isinstance(absolute_path, str) or not Path(absolute_path).is_absolute():
        errors.append(f"{role} runtime dependency root path is not absolute")
    if not isinstance(record.get("version"), str) or not record.get("version"):
        errors.append(f"{role} runtime dependency version is missing")
    if record.get("read_consistent") is not True:
        errors.append(f"{role} runtime dependency read was not stable")

    files = record.get("files")
    if not isinstance(files, list) or not files:
        errors.append(f"{role} runtime dependency file list is missing")
        return errors
    if record.get("file_count") != len(files):
        errors.append(f"{role} runtime dependency file_count differs")

    canonical: list[dict[str, Any]] = []
    seen_relative: set[str] = set()
    total_size = 0
    zero_byte_count = 0
    for index, file_record in enumerate(files):
        label = f"{role}.files[{index}]"
        if not isinstance(file_record, dict):
            errors.append(f"{label} is not an object")
            continue
        relative = file_record.get("relative_path")
        absolute = file_record.get("absolute_path")
        size = file_record.get("size_bytes")
        sha256 = str(file_record.get("sha256", "")).lower()
        if (
            not isinstance(relative, str)
            or not relative
            or "\\" in relative
            or Path(relative).is_absolute()
        ):
            errors.append(f"{label}.relative_path is invalid")
            continue
        if relative in seen_relative:
            errors.append(f"{label}.relative_path is duplicated")
        seen_relative.add(relative)
        if not isinstance(absolute, str) or not Path(absolute).is_absolute():
            errors.append(f"{label}.absolute_path is not absolute")
        if not isinstance(size, int) or isinstance(size, bool) or size < 0:
            errors.append(f"{label}.size_bytes must be an integer >= 0")
            continue
        if re.fullmatch(r"[0-9a-f]{64}", sha256) is None:
            errors.append(f"{label}.sha256 is invalid")
            continue
        total_size += size
        zero_byte_count += int(size == 0)
        canonical.append(
            {
                "relative_path": relative,
                "size_bytes": size,
                "sha256": sha256,
            }
        )
    if record.get("size_bytes") != total_size:
        errors.append(f"{role} runtime dependency aggregate size differs")
    if record.get("zero_byte_file_count", zero_byte_count) != zero_byte_count:
        errors.append(f"{role} runtime dependency zero-byte count differs")
    if len(canonical) == len(files):
        observed_sha = str(record.get("sha256", "")).lower()
        expected_sha = (
            _canonical_json_sha256(canonical)
            if role == "rdc_cli_package"
            else canonical[0]["sha256"]
            if role == "renderdoc_module" and len(canonical) == 1
            else None
        )
        if expected_sha is None or observed_sha != expected_sha:
            errors.append(f"{role} runtime dependency aggregate SHA-256 differs")

    if role == "rdc_cli_package":
        required = {
            "rdc/__init__.py",
            "rdc/daemon_client.py",
            "rdc/protocol.py",
            "rdc/services/session_service.py",
            "rdc/session_state.py",
            "rdc/_platform.py",
        }
        missing = sorted(required - seen_relative)
        if missing:
            errors.append(f"rdc_cli_package evidence misses required files: {missing!r}")
        if record.get("exclusions") != ["__pycache__", "*.pyc", "*.pyo"]:
            errors.append("rdc_cli_package exclusions differ from the canonical set")
        ordering = record.get("session_publication_ordering")
        if not isinstance(ordering, dict):
            errors.append("rdc_cli_package session publication ordering evidence is missing")
        else:
            expected_chain = [
                "open_session.start_daemon",
                "open_session.wait_for_ping",
                "open_session.create_session",
                "create_session.save_session",
                "save_session._platform.secure_write_text",
                "secure_write_text.path.write_text",
            ]
            if ordering.get("schema") != RDC_CLI_SESSION_ORDERING_SCHEMA:
                errors.append("rdc_cli_package session ordering schema is invalid")
            if ordering.get("verified") is not True:
                errors.append("rdc_cli_package session ordering was not verified")
            if ordering.get("package_version") != record.get("version"):
                errors.append("rdc_cli_package session ordering version differs")
            if ordering.get("daemon_created_before_state_publication") is not True:
                errors.append("rdc_cli_package does not prove daemon creation before state")
            if ordering.get("daemon_ready_before_state_publication") is not True:
                errors.append("rdc_cli_package does not prove daemon readiness before state")
            if ordering.get("state_publication_call_chain") != expected_chain:
                errors.append("rdc_cli_package session publication call chain differs")
            call_lines = ordering.get("call_lines")
            if not isinstance(call_lines, dict):
                errors.append("rdc_cli_package session ordering call lines are missing")
            else:
                try:
                    start_line = int(call_lines["start_daemon"])
                    ping_line = int(call_lines["wait_for_ping"])
                    publish_line = int(call_lines["create_session"])
                except (KeyError, TypeError, ValueError):
                    errors.append("rdc_cli_package session ordering call lines are invalid")
                else:
                    if not start_line < ping_line < publish_line:
                        errors.append("rdc_cli_package daemon/state call order is invalid")
            ordering_sources = ordering.get("source_files")
            required_ordering_sources = {
                "rdc/services/session_service.py",
                "rdc/session_state.py",
                "rdc/_platform.py",
            }
            if (
                not isinstance(ordering_sources, dict)
                or set(ordering_sources) != required_ordering_sources
            ):
                errors.append("rdc_cli_package ordering source set differs")
            else:
                package_files = {
                    item.get("relative_path"): item
                    for item in files
                    if isinstance(item, dict)
                }
                for relative in sorted(required_ordering_sources):
                    source_record = ordering_sources.get(relative)
                    package_record = package_files.get(relative)
                    if not isinstance(source_record, dict) or not isinstance(package_record, dict):
                        errors.append(f"rdc_cli_package ordering source is missing: {relative}")
                        continue
                    for field in ("absolute_path", "size_bytes", "sha256"):
                        if source_record.get(field) != package_record.get(field):
                            errors.append(
                                f"rdc_cli_package ordering source {relative} differs in {field}"
                            )
    elif role == "renderdoc_module":
        if len(files) != 1:
            errors.append("renderdoc_module evidence must contain exactly one file")
        elif isinstance(absolute_path, str) and isinstance(files[0], dict):
            file_absolute = files[0].get("absolute_path")
            if not isinstance(file_absolute, str) or _path_key(
                Path(file_absolute)
            ) != _path_key(Path(absolute_path)):
                errors.append("renderdoc_module root/file absolute paths differ")
    return errors

def _toolchain_snapshot(tool_paths: dict[str, Path]) -> dict[str, Any]:
    tools = {
        role: _collect_tool_file_evidence(path, role=role)
        for role, path in sorted(tool_paths.items())
    }
    tools["rdc_cli_package"] = _collect_rdc_cli_package_evidence()
    tools["renderdoc_module"] = _collect_renderdoc_module_evidence()
    canonical_tools = {
        role: {
            key: record[key]
            for key in ("role", "absolute_path", "version", "size_bytes", "sha256")
        }
        for role, record in tools.items()
    }
    return {
        "schema": TOOLCHAIN_EVIDENCE_SCHEMA,
        "captured_utc": datetime.now(timezone.utc).isoformat(timespec="milliseconds"),
        "tools": tools,
        "bundle_sha256": _canonical_json_sha256(canonical_tools),
    }


def _compare_toolchain_snapshots(
    before: dict[str, Any],
    after: dict[str, Any],
    *,
    required_roles: tuple[str, ...],
) -> dict[str, Any]:
    errors: list[str] = []
    role_results: dict[str, Any] = {}
    before_tools = before.get("tools") if isinstance(before, dict) else None
    after_tools = after.get("tools") if isinstance(after, dict) else None
    if not isinstance(before_tools, dict) or not isinstance(after_tools, dict):
        return {
            "passed": False,
            "required_roles": list(required_roles),
            "roles": {},
            "errors": ["toolchain snapshots do not contain tool maps"],
        }
    if set(before_tools) != set(required_roles):
        errors.append(
            f"before tool roles {sorted(before_tools)} do not equal {sorted(required_roles)}"
        )
    if set(after_tools) != set(required_roles):
        errors.append(
            f"after tool roles {sorted(after_tools)} do not equal {sorted(required_roles)}"
        )
    identity_fields = ("role", "absolute_path", "version", "size_bytes", "sha256")
    for role in required_roles:
        left = before_tools.get(role)
        right = after_tools.get(role)
        checks = {
            field: isinstance(left, dict)
            and isinstance(right, dict)
            and left.get(field) == right.get(field)
            for field in identity_fields
        }
        checks["absolute_path"] = checks["absolute_path"] and Path(
            str(left.get("absolute_path"))
        ).is_absolute()
        checks["read_consistent"] = (
            isinstance(left, dict)
            and isinstance(right, dict)
            and left.get("read_consistent") is True
            and right.get("read_consistent") is True
        )
        role_errors = [field for field, passed in checks.items() if not passed]
        if role in ("rdc_cli_package", "renderdoc_module"):
            for side_name, side_record in (("before", left), ("after", right)):
                role_errors.extend(
                    f"{side_name}: {message}"
                    for message in _validate_runtime_dependency_evidence(
                        side_record,
                        role=role,
                    )
                )
        if role_errors:
            errors.append(f"{role}: tool evidence mismatch in {role_errors}")
        role_results[role] = {
            "passed": not role_errors,
            "checks": checks,
            "errors": role_errors,
        }
    if before.get("bundle_sha256") != after.get("bundle_sha256"):
        errors.append("toolchain bundle SHA-256 changed")
    return {
        "passed": not errors,
        "required_roles": list(required_roles),
        "roles": role_results,
        "before_bundle_sha256": before.get("bundle_sha256"),
        "after_bundle_sha256": after.get("bundle_sha256"),
        "errors": errors,
    }


def _shadow_edge_module_path() -> Path:
    CameraMatrices, _, _, _ = _shadow_edge_module_api()
    module = sys.modules.get(CameraMatrices.__module__)
    module_file = getattr(module, "__file__", None) if module is not None else None
    if not module_file:
        raise ComparatorError("loaded ROI module has no __file__ identity")
    return Path(str(module_file)).resolve(strict=True)


def _runtime_tool_paths() -> dict[str, Path]:
    return {
        "comparator": Path(__file__).resolve(strict=True),
        "roi": _shadow_edge_module_path(),
    }


def _validate_manifest_toolchain_evidence(
    source: dict[str, Any],
    *,
    runtime_before: dict[str, Any],
) -> dict[str, Any]:
    errors: list[str] = []
    record = source.get("toolchain_evidence")
    if not isinstance(record, dict):
        return {
            "schema": TOOLCHAIN_EVIDENCE_SCHEMA,
            "passed": False,
            "errors": ["manifest toolchain_evidence is missing"],
        }
    if record.get("schema") != TOOLCHAIN_EVIDENCE_SCHEMA:
        errors.append("manifest toolchain evidence schema is invalid")
    before = record.get("before_all_cases")
    after = record.get("after_all_cases")
    comparison = record.get("comparison")
    if not isinstance(before, dict) or not isinstance(after, dict):
        errors.append("manifest toolchain before/after snapshots are missing")
        before = before if isinstance(before, dict) else {}
        after = after if isinstance(after, dict) else {}
    recomputed = _compare_toolchain_snapshots(
        before,
        after,
        required_roles=TOOLCHAIN_REQUIRED_ROLES,
    )
    errors.extend(f"manifest snapshot: {error}" for error in recomputed["errors"])
    if not isinstance(comparison, dict) or comparison.get("passed") is not True:
        errors.append("manifest toolchain comparison did not pass")
    if record.get("passed") is not True or record.get("errors") not in ([], None):
        errors.append("manifest toolchain evidence is not terminally passed")
    before_tools = before.get("tools", {}) if isinstance(before, dict) else {}
    expected_runtime = {
        role: before_tools.get(role)
        for role in ("comparator", "roi", "rdc_cli_package", "renderdoc_module")
    }
    runtime_tools = runtime_before.get("tools", {})
    for role in ("comparator", "roi", "rdc_cli_package", "renderdoc_module"):
        expected = expected_runtime.get(role)
        observed = runtime_tools.get(role)
        if not isinstance(expected, dict) or not isinstance(observed, dict):
            errors.append(f"{role}: manifest/runtime tool evidence is missing")
            continue
        for field in ("absolute_path", "version", "size_bytes", "sha256"):
            if expected.get(field) != observed.get(field):
                errors.append(f"{role}: manifest/runtime {field} differs")
    harness = before_tools.get("harness") if isinstance(before_tools, dict) else None
    current_all: dict[str, Any] | None = None
    if not isinstance(harness, dict):
        errors.append("harness tool evidence is missing")
    else:
        harness_path_value = harness.get("absolute_path")
        if not isinstance(harness_path_value, str) or not Path(harness_path_value).is_absolute():
            errors.append("harness tool path is not absolute")
        else:
            try:
                current_all = _toolchain_snapshot(
                    {
                        "harness": Path(harness_path_value),
                        **_runtime_tool_paths(),
                    }
                )
                current_comparison = _compare_toolchain_snapshots(
                    before,
                    current_all,
                    required_roles=TOOLCHAIN_REQUIRED_ROLES,
                )
                errors.extend(
                    f"current toolchain: {error}"
                    for error in current_comparison["errors"]
                )
            except Exception as exc:
                errors.append(
                    f"current toolchain collection failed: {type(exc).__name__}: {exc}"
                )
    expected_bundle = before.get("bundle_sha256") if isinstance(before, dict) else None
    for index, case in enumerate(source.get("cases", [])):
        if not isinstance(case, dict):
            continue
        if case.get("toolchain_bundle_sha256") != expected_bundle:
            errors.append(
                f"cases[{index}].toolchain_bundle_sha256 does not bind the manifest tools"
            )
    return {
        "schema": TOOLCHAIN_EVIDENCE_SCHEMA,
        "passed": not errors,
        "manifest": record,
        "recomputed_comparison": recomputed,
        "runtime_before": runtime_before,
        "current_all_tools": current_all,
        "errors": errors,
    }

def _parse_utc(value: Any, label: str) -> datetime:
    if not isinstance(value, str) or not value.strip():
        raise ComparatorError(f"{label} must be a non-empty UTC timestamp")
    text = value.strip()
    if text.endswith("Z"):
        text = text[:-1] + "+00:00"
    try:
        parsed = datetime.fromisoformat(text)
    except ValueError as exc:
        raise ComparatorError(f"{label} is not an ISO-8601 timestamp: {value!r}") from exc
    if parsed.tzinfo is None:
        raise ComparatorError(f"{label} must include a timezone: {value!r}")
    return parsed.astimezone(timezone.utc)


def _path_key(path: Path) -> str:
    return os.path.normcase(str(path.resolve()))


def _path_is_within(path: Path, root: Path) -> bool:
    try:
        path.resolve().relative_to(root.resolve())
        return True
    except ValueError:
        return False


def _manifest_integer(value: Any) -> int | None:
    if isinstance(value, bool) or not isinstance(value, int):
        return None
    return value


def _expected_capture_sha256(entry: dict[str, Any]) -> str | None:
    direct = entry.get("capture_sha256")
    if isinstance(direct, str) and direct.strip():
        return direct.strip().lower()
    for key in ("capture_hash", "file_hash", "capture_file"):
        value = entry.get(key)
        if isinstance(value, str) and re.fullmatch(r"[0-9a-fA-F]{64}", value.strip()):
            return value.strip().lower()
        if not isinstance(value, dict):
            continue
        algorithm = str(value.get("algorithm", "sha256")).lower().replace("-", "")
        candidate = value.get("value", value.get("sha256"))
        if algorithm == "sha256" and isinstance(candidate, str) and candidate.strip():
            return candidate.strip().lower()
    return None


def _release_gate_enabled(
    options: argparse.Namespace,
    source: dict[str, Any],
) -> tuple[bool, str]:
    if options.release_gate is True:
        return True, "explicit"
    if options.release_gate is False:
        return False, "explicitly_disabled"
    final_manifest = bool(source.get("completed_utc")) and source.get("status") != "running"
    return final_manifest, "auto_final_smoke_manifest" if final_manifest else "auto_diagnostic"


def _release_case_contract(render_mode: str) -> dict[str, dict[str, str]]:
    if render_mode not in RELEASE_RENDER_MODES:
        raise ComparatorError(f"unsupported release render mode {render_mode!r}")
    return {
        mode: {"render_mode": render_mode, "gi_mode": "no-ddgi"}
        for mode in RELEASE_CASE_MODES
    }


def _release_manifest_profile(source: dict[str, Any]) -> dict[str, Any]:
    errors: list[str] = []
    cases = source.get("cases")
    if not isinstance(cases, list):
        cases = []
        errors.append("release manifest cases are not an array")
    typed_cases = [case for case in cases if isinstance(case, dict)]
    if len(cases) != 2 or len(typed_cases) != 2:
        errors.append(
            f"release requires exactly two object cases, found {len(cases)} total / "
            f"{len(typed_cases)} objects"
        )
    observed_modes = [str(case.get("mode", "")) for case in typed_cases]
    if len(observed_modes) != 2 or set(observed_modes) != set(RELEASE_CASE_MODES):
        errors.append(
            f"release case modes are {sorted(observed_modes)!r}, expected "
            f"{list(RELEASE_CASE_MODES)!r}"
        )
    render_modes = {
        str(case.get("render_mode", ""))
        for case in typed_cases
        if isinstance(case.get("render_mode"), str)
    }
    render_mode = next(iter(render_modes)) if len(render_modes) == 1 else None
    if render_mode not in RELEASE_RENDER_MODES:
        errors.append(
            "release cases must use one uniform render mode from "
            f"{list(RELEASE_RENDER_MODES)!r}, got {sorted(render_modes)!r}"
        )
        render_mode = None
    gi_modes = {str(case.get("gi_mode", "")) for case in typed_cases}
    if gi_modes != {"no-ddgi"}:
        errors.append(
            f"release cases must all use no-ddgi, got {sorted(gi_modes)!r}"
        )
    contract = _release_case_contract(render_mode) if render_mode is not None else {}
    expected_names = {
        f"{mode}__{render_mode}__no-ddgi"
        for mode in RELEASE_CASE_MODES
    } if render_mode is not None else set()
    observed_names = [str(case.get("name", "")) for case in typed_cases]
    if expected_names and (
        len(observed_names) != 2 or set(observed_names) != expected_names
    ):
        errors.append(
            f"release case names are {sorted(observed_names)!r}, expected "
            f"{sorted(expected_names)!r}"
        )
    options = source.get("options")
    if not isinstance(options, dict):
        errors.append("release manifest options are missing")
    elif render_mode is not None:
        if options.get("render_modes") != [render_mode]:
            errors.append(
                f"release options.render_modes must be exactly [{render_mode!r}]"
            )
        if options.get("gi_modes") != ["no-ddgi"]:
            errors.append("release options.gi_modes must be exactly ['no-ddgi']")
        option_modes = options.get("modes")
        if not isinstance(option_modes, list) or len(option_modes) != 2 or set(
            option_modes
        ) != set(RELEASE_CASE_MODES):
            errors.append(
                "release options.modes must contain exactly translate-stop and rotate-stop"
            )
    return {
        "passed": not errors,
        "render_mode": render_mode,
        "case_contract": contract,
        "expected_case_names": sorted(expected_names),
        "observed_case_names": observed_names,
        "errors": errors,
    }


def _validate_release_threshold_configuration(
    options: argparse.Namespace,
) -> dict[str, Any]:
    observed: dict[str, Any] = {}
    errors: list[str] = []
    for field, expected in RELEASE_CANONICAL_THRESHOLDS.items():
        value = getattr(options, field, None)
        if field == "max_csm_registration_shift" and value is None:
            value = 0
        observed[field] = value
        if isinstance(expected, int):
            matches = isinstance(value, int) and not isinstance(value, bool) and value == expected
        else:
            matches = (
                isinstance(value, (int, float))
                and not isinstance(value, bool)
                and math.isfinite(float(value))
                and float(value) == float(expected)
            )
        if not matches:
            errors.append(
                f"release threshold --{field.replace('_', '-')}={value!r}, "
                f"expected canonical {expected!r}"
            )
    roi_runtime: dict[str, Any] | None = None
    try:
        _, ShadowEdgeMetricsConfig, _, _ = _shadow_edge_module_api()
        roi_config = ShadowEdgeMetricsConfig(**RELEASE_ROI_CONFIG)
        roi_runtime = {
            field: getattr(roi_config, field)
            for field in RELEASE_ROI_CONFIG
        }
        for field, expected in RELEASE_ROI_CONFIG.items():
            observed_value = roi_runtime.get(field)
            if isinstance(expected, tuple):
                matches = tuple(observed_value) == expected
            elif isinstance(expected, float):
                matches = (
                    isinstance(observed_value, (int, float))
                    and math.isfinite(float(observed_value))
                    and float(observed_value) == expected
                )
            else:
                matches = observed_value == expected
            if not matches:
                errors.append(
                    f"release ROI threshold {field}={observed_value!r}, "
                    f"expected canonical {expected!r}"
                )
    except Exception as exc:
        errors.append(
            f"canonical ROI configuration could not be constructed: "
            f"{type(exc).__name__}: {exc}"
        )
    return {
        "schema": "mgif-csm-release-thresholds-v1",
        "passed": not errors,
        "cli_thresholds": observed,
        "canonical_cli_thresholds": dict(RELEASE_CANONICAL_THRESHOLDS),
        "roi_thresholds": roi_runtime,
        "canonical_roi_thresholds": dict(RELEASE_ROI_CONFIG),
        "errors": errors,
    }


def _validate_release_cli_contract(
    options: argparse.Namespace,
) -> dict[str, Any]:
    errors: list[str] = []
    if options.case:
        errors.append(
            "--case is forbidden with --release-gate; release must replay and "
            "extract both target cases and all six RDCs"
        )
    thresholds = _validate_release_threshold_configuration(options)
    errors.extend(thresholds.get("errors", []))
    return {
        "schema": "mgif-csm-release-cli-contract-v1",
        "passed": not errors,
        "case_filter_forbidden": True,
        "selected_cases": list(options.case),
        "thresholds": thresholds,
        "errors": errors,
    }

def _rdc_status_classification(record: Any) -> str:
    if not isinstance(record, dict):
        return "unavailable"
    explicit_value = record.get("classification")
    explicit = (
        str(explicit_value).strip().lower()
        if isinstance(explicit_value, str)
        else None
    )
    rc_key_present = "returncode" in record or "rc" in record
    rc_value = record.get("returncode", record.get("rc"))
    rc = _manifest_integer(rc_value) if rc_key_present else None
    stdout = str(record.get("stdout", "") or "").strip()
    stderr = str(record.get("stderr", "") or "").strip()
    inferred: str | None = None
    if rc == 0:
        inferred = "active"
    elif (
        rc == 1
        and stdout == ""
        and stderr.lower() == "error: no active session"
    ):
        inferred = "inactive"
    elif rc_key_present:
        inferred = "invalid"
    if inferred is not None:
        if explicit in ("active", "inactive") and explicit != inferred:
            return "conflict"
        return inferred
    if explicit in ("active", "inactive"):
        return explicit
    return "unavailable"


def _state_file_absence(record: dict[str, Any]) -> bool | None:
    state_file = record.get("state_file")
    if not isinstance(state_file, dict):
        return None
    evidence: list[bool] = []
    absent_after_cleanup = state_file.get("absent_after_cleanup")
    if isinstance(absent_after_cleanup, bool):
        evidence.append(absent_after_cleanup)
    after_cleanup = state_file.get("after_cleanup")
    if isinstance(after_cleanup, dict):
        exists = after_cleanup.get("exists")
        if isinstance(exists, bool):
            evidence.append(not exists)
    if not evidence:
        return None
    return all(evidence)


def _difference_value_is_zero(value: Any) -> bool | None:
    if isinstance(value, bool):
        return not value
    if isinstance(value, int):
        return value == 0
    if isinstance(value, (list, tuple, set)):
        return len(value) == 0
    if isinstance(value, dict):
        nested_results: list[bool] = []
        for key, nested in value.items():
            compact = re.sub(r"[^a-z0-9]+", "", str(key).lower())
            if any(
                token in compact
                for token in (
                    "new",
                    "added",
                    "created",
                    "difference",
                    "diff",
                    "delta",
                    "count",
                )
            ):
                result = _difference_value_is_zero(nested)
                if result is not None:
                    nested_results.append(result)
        if nested_results:
            return all(nested_results)
        return len(value) == 0
    return None


def _cleanup_difference_is_zero(
    cleanup: dict[str, Any],
    *,
    subject: str,
) -> bool | None:
    evidence: list[bool] = []

    def collect(value: Any, *, under_subject: bool = False) -> None:
        if isinstance(value, dict):
            for key, nested in value.items():
                compact = re.sub(r"[^a-z0-9]+", "", str(key).lower())
                current_subject = under_subject or subject in compact
                if current_subject and any(
                    token in compact
                    for token in (
                        "new",
                        "added",
                        "created",
                        "difference",
                        "diff",
                        "delta",
                    )
                ):
                    result = _difference_value_is_zero(nested)
                    if result is not None:
                        evidence.append(result)
                if isinstance(nested, (dict, list, tuple)):
                    collect(nested, under_subject=current_subject)
        elif isinstance(value, (list, tuple)):
            for nested in value:
                collect(nested, under_subject=under_subject)

    collect(cleanup)
    if not evidence:
        return None
    return all(evidence)


def _validate_available_cleanup_evidence(
    record: Any,
    *,
    label: str,
    error_fields: tuple[str, ...],
) -> list[str]:
    if not isinstance(record, dict):
        return [f"{label} is missing"]
    errors: list[str] = []
    if record.get("available") is not True:
        errors.append(f"{label}.available is not true")
    for field in error_fields:
        value = record.get(field)
        if not isinstance(value, list):
            errors.append(f"{label}.{field} must be an array")
        elif value:
            errors.append(f"{label}.{field} is not empty: {value!r}")
    if record.get("error"):
        errors.append(f"{label}.error is present: {record['error']!r}")
    if "errors" not in error_fields and record.get("errors"):
        errors.append(f"{label}.errors is not empty: {record['errors']!r}")
    return errors

def _named_replay_sessions(
    cleanup: dict[str, Any],
) -> tuple[bool, list[dict[str, Any]]]:
    for key in (
        "named_replay_sessions",
        "named_sessions",
        "replay_sessions",
        "replay_session_cleanup",
        "replay_session_cleanups",
        "session_cleanups",
        "sessions",
    ):
        if key not in cleanup:
            continue
        value = cleanup[key]
        if isinstance(value, list):
            return True, [
                row if isinstance(row, dict) else {"invalid_record": row}
                for row in value
            ]
        if isinstance(value, dict):
            if "post_status" in value or "session" in value or "name" in value:
                return True, [value]
            rows: list[dict[str, Any]] = []
            for session_name, row in value.items():
                if not isinstance(row, dict):
                    continue
                normalized = dict(row)
                normalized.setdefault("session", session_name)
                rows.append(normalized)
            return True, rows
        return True, []
    return False, []


def _validate_stable_process_identity_evidence(
    stable: Any,
    *,
    label: str,
) -> list[str]:
    errors: list[str] = []
    if not isinstance(stable, dict):
        return [f"{label}: stable process identity is missing"]
    pid = _manifest_integer(stable.get("pid"))
    if pid is None or pid <= 0:
        errors.append(f"{label}: stable process PID is not positive")
    if stable.get("native_handle_held") is not True:
        errors.append(f"{label}: native process handle was not held")
    if stable.get("terminate_access") is not True:
        errors.append(f"{label}: held process handle lacks terminate access")
    creation_key = stable.get("creation_time_key")
    creation_ticks = _manifest_integer(stable.get("creation_filetime_ticks"))
    creation_unix_ns = _manifest_integer(stable.get("creation_time_unix_ns"))
    if creation_ticks is None or creation_ticks <= WINDOWS_FILETIME_EPOCH_OFFSET_TICKS:
        errors.append(f"{label}: exact Windows creation FILETIME is missing")
    elif creation_key != f"winfiletime:{creation_ticks}":
        errors.append(f"{label}: creation key does not match exact Windows FILETIME")
    elif creation_unix_ns != (
        creation_ticks - WINDOWS_FILETIME_EPOCH_OFFSET_TICKS
    ) * WINDOWS_FILETIME_TICK_NS:
        errors.append(f"{label}: creation nanoseconds do not match exact Windows FILETIME")
    if not isinstance(stable.get("image_path"), str) or not stable.get("image_path"):
        errors.append(f"{label}: process image identity is missing")
    return errors

def _validate_direct_shutdown_cleanup_evidence(
    cleanup: Any,
    *,
    label: str,
    require_handle_close: bool,
) -> list[str]:
    errors: list[str] = []
    if not isinstance(cleanup, dict):
        return [f"{label}: cleanup evidence is missing"]
    if cleanup.get("close_subprocess_used") is not False:
        errors.append(f"{label}: close subprocess use was not explicitly forbidden")
    if cleanup.get("status_subprocess_used") is not False:
        errors.append(f"{label}: status subprocess use was not explicitly forbidden")
    direct = cleanup.get("direct_shutdown")
    if not isinstance(direct, dict):
        errors.append(f"{label}: direct token shutdown evidence is missing")
        return errors
    if direct.get("schema") != "mgif-rdc-direct-token-shutdown-v1":
        errors.append(f"{label}: direct token shutdown schema is invalid")
    if direct.get("passed") is not True or direct.get("errors") not in ([], None):
        errors.append(f"{label}: direct token shutdown did not pass without errors")
    for field in (
        "subprocess_used",
        "pid_only_fallback",
        "port_scan_fallback",
        "tree_cleanup_requested",
    ):
        if direct.get(field) is not False:
            errors.append(f"{label}: direct token shutdown {field} is not false")
    errors.extend(
        _validate_stable_process_identity_evidence(
            direct.get("stable_process_identity"),
            label=f"{label}: direct token identity",
        )
    )
    if direct.get("owned_daemon_absent") is not True:
        errors.append(f"{label}: direct token shutdown did not prove daemon absence")
    if direct.get("state_file_absent") is not True:
        errors.append(f"{label}: direct token shutdown did not prove state-file absence")
    direct_status = direct.get("post_status")
    if (
        _rdc_status_classification(direct_status) != "inactive"
        or not isinstance(direct_status, dict)
        or direct_status.get("subprocess_used") is not False
    ):
        errors.append(f"{label}: direct token post-status is not handle/state inactive")
    recovery = direct.get("same_handle_recovery")
    if not isinstance(recovery, dict):
        errors.append(f"{label}: same-handle recovery evidence is missing")
    else:
        if recovery.get("same_native_handle") is not True:
            errors.append(f"{label}: recovery did not use the same native handle")
        if recovery.get("pid_only_fallback") is not False:
            errors.append(f"{label}: recovery allowed a PID-only fallback")
        if recovery.get("tree_cleanup_requested") is not False:
            errors.append(f"{label}: recovery requested PID-only tree cleanup")
        if recovery.get("passed") is not True:
            errors.append(f"{label}: same-handle recovery evidence did not pass")
    if require_handle_close:
        handle_close = cleanup.get("daemon_process_handle_close")
        if not isinstance(handle_close, dict) or handle_close.get("closed") is not True:
            errors.append(f"{label}: owned replay daemon handle close is unproven")
    return errors

def _validate_release_cleanup_contract(source: dict[str, Any]) -> list[str]:
    errors: list[str] = []
    status_after = source.get("rdc_status_after")
    status_classification = _rdc_status_classification(status_after)
    if status_classification != "inactive":
        errors.append(
            "top-level rdc_status_after must prove no active legacy session: "
            "rdc-cli 0.6.1 requires rc=1, empty stdout, "
            "stderr='error: no active session'"
        )

    cleanup = source.get("rdc_session_cleanup")
    if not isinstance(cleanup, dict):
        return [*errors, "top-level rdc_session_cleanup is missing"]
    if cleanup.get("schema") != RDC_CLEANUP_SCHEMA:
        errors.append(
            f"top-level rdc_session_cleanup.schema must be {RDC_CLEANUP_SCHEMA!r}"
        )
    if cleanup.get("passed") is not True or cleanup.get("closed") is not True:
        errors.append("top-level rdc_session_cleanup did not pass/close")
    cleanup_errors = cleanup.get("errors")
    if not isinstance(cleanup_errors, list):
        errors.append("top-level rdc_session_cleanup.errors must be an array")
    elif cleanup_errors:
        errors.append(
            f"top-level rdc_session_cleanup.errors is not empty: {cleanup_errors!r}"
        )
    ownership_model = cleanup.get("ownership_model")
    if not isinstance(ownership_model, dict):
        errors.append("top-level rdc_session_cleanup.ownership_model is missing")
    elif ownership_model.get("external_additions_are_diagnostic") is not True:
        errors.append(
            "rdc cleanup ownership model must classify external additions as diagnostic"
        )

    has_named_sessions, named_sessions = _named_replay_sessions(cleanup)
    if not has_named_sessions or not named_sessions:
        errors.append("structured cleanup has no named replay sessions")
    seen_sessions: set[str] = set()
    for index, session in enumerate(named_sessions):
        label = f"rdc_session_cleanup.named_replay_sessions[{index}]"
        name = session.get("session")
        if not isinstance(name, str) or not name.strip():
            errors.append(f"{label}.session is missing")
        elif name in seen_sessions:
            errors.append(f"{label}.session is duplicated: {name!r}")
        else:
            seen_sessions.add(name)
        if session.get("passed") is not True:
            errors.append(f"{label}.passed is not true")
        errors.extend(
            _validate_direct_shutdown_cleanup_evidence(
                session,
                label=label,
                require_handle_close=True,
            )
        )
        session_errors = session.get("errors")
        if not isinstance(session_errors, list) or session_errors:
            errors.append(f"{label}.errors is missing or non-empty: {session_errors!r}")
        post_status = session.get("post_status")
        if _rdc_status_classification(post_status) != "inactive":
            errors.append(f"{label}.post_status does not prove an inactive session")
        absent = _state_file_absence(session)
        if absent is not True:
            errors.append(f"{label} state file absence was not proven")
        ownership = session.get("daemon_ownership")
        if not isinstance(ownership, dict):
            errors.append(f"{label}.daemon_ownership is missing")
        else:
            if ownership.get("established") is not True:
                errors.append(f"{label}.daemon_ownership.established is not true")
            if ownership.get("errors") not in ([], None):
                errors.append(f"{label}.daemon_ownership.errors is not empty")
            for field in (
                "state_path_match",
                "state_capture_path_match",
                "daemon_capture_path_metadata_match",
                "stable_image_match",
                "state_publication_boundary_exact",
                "state_file_volume_verified",
                "exact_creation_order_clock_match",
                "strict_creation_precedes_publication",
                "snapshot_evidence_complete",
                "process_created_before_state_file",
            ):
                if ownership.get(field) is not True:
                    errors.append(f"{label}.daemon_ownership.{field} is not true")
            stable = ownership.get("stable_process_identity")
            errors.extend(
                _validate_stable_process_identity_evidence(
                    stable,
                    label=f"{label}.daemon_ownership",
                )
            )
        residue = session.get("owned_daemon_residue")
        if not isinstance(residue, list) or residue:
            errors.append(f"{label}.owned_daemon_residue is missing or non-empty")

    for snapshot_name in ("rdc_resources_before", "rdc_resources_after"):
        snapshot = source.get(snapshot_name)
        errors.extend(
            _validate_available_cleanup_evidence(
                snapshot,
                label=f"top-level {snapshot_name}",
                error_fields=("errors",),
            )
        )
        if not isinstance(snapshot, dict) or snapshot.get(
            "process_access_denied_count"
        ) != 0:
            errors.append(
                f"top-level {snapshot_name}.process_access_denied_count must be 0"
            )

    run_diff = cleanup.get("run_resource_diff")
    errors.extend(
        _validate_available_cleanup_evidence(
            run_diff,
            label="structured rdc_session_cleanup.run_resource_diff",
            error_fields=("before_errors", "after_errors"),
        )
    )
    if isinstance(run_diff, dict):
        for field in (
            "before_process_access_denied_count",
            "after_process_access_denied_count",
        ):
            if run_diff.get(field) != 0:
                errors.append(f"run_resource_diff.{field} must be 0")
        # Raw additions may belong to another user-owned qrenderdoc/daemon.
        # The release gate only rejects additions/residue established as ours.
        for field in ("added_daemons", "added_session_files"):
            if not isinstance(run_diff.get(field), list):
                errors.append(f"run_resource_diff.{field} must be an array")

    hard_empty_fields = (
        "owned_added_session_files",
        "owned_added_daemons",
        "owned_session_file_residue",
        "owned_daemon_residue",
    )
    for field in hard_empty_fields:
        value = cleanup.get(field)
        if not isinstance(value, list):
            errors.append(f"rdc_session_cleanup.{field} must be an array")
        elif value:
            errors.append(f"rdc_session_cleanup.{field} is not empty")
    for field in ("external_added_session_files", "external_added_daemons"):
        if not isinstance(cleanup.get(field), list):
            errors.append(f"rdc_session_cleanup.{field} must be an array")
    return errors
def _executable_content_tuple(record: Any) -> tuple[str, int, str] | None:
    if not isinstance(record, dict):
        return None
    path = record.get("resolved_path", record.get("path"))
    size = record.get("size_bytes")
    sha256 = str(record.get("sha256", "")).lower()
    if not isinstance(path, str) or not path.strip():
        return None
    if not isinstance(size, int) or isinstance(size, bool) or size <= 0:
        return None
    if re.fullmatch(r"[0-9a-f]{64}", sha256) is None:
        return None
    return (_path_key(Path(path)), size, sha256)


def _validate_release_executable_evidence(
    source: dict[str, Any],
) -> dict[str, Any]:
    result: dict[str, Any] = {
        "schema": EXECUTABLE_BINDING_SCHEMA,
        "passed": False,
        "source_current_identity": None,
        "launch_current_identity": None,
        "case_results": {},
        "errors": [],
    }
    errors = result["errors"]
    evidence = source.get("executable_evidence")
    if not isinstance(evidence, dict):
        errors.append("top-level executable_evidence is missing")
        return result
    if evidence.get("schema") != EXECUTABLE_BINDING_SCHEMA:
        errors.append(
            f"executable_evidence.schema must be {EXECUTABLE_BINDING_SCHEMA!r}"
        )
    if evidence.get("required") is not True or evidence.get("passed") is not True:
        errors.append("top-level executable_evidence is not required/passed")
    if evidence.get("errors") not in ([], None):
        errors.append("top-level executable_evidence.errors is not empty")

    source_binding = evidence.get("source")
    launch_binding = evidence.get("launch_image")
    if not isinstance(source_binding, dict) or not isinstance(launch_binding, dict):
        errors.append("source or immutable launch executable evidence is missing")
        return result
    source_before = _executable_content_tuple(source_binding.get("before_copy"))
    source_after = _executable_content_tuple(source_binding.get("after_all_cases"))
    launch_before = _executable_content_tuple(launch_binding.get("baseline"))
    launch_after = _executable_content_tuple(launch_binding.get("after_all_cases"))
    for label, value in (
        ("source.before_copy", source_before),
        ("source.after_all_cases", source_after),
        ("launch_image.baseline", launch_before),
        ("launch_image.after_all_cases", launch_after),
    ):
        if value is None:
            errors.append(f"executable_evidence.{label} is invalid")
    if source_before is not None and source_after is not None and source_before != source_after:
        errors.append("source executable before/after evidence differs")
    if launch_before is not None and launch_after is not None and launch_before != launch_after:
        errors.append("immutable launch image before/after evidence differs")
    if source_before is not None and launch_before is not None:
        if source_before[1:] != launch_before[1:]:
            errors.append("immutable launch image content does not match source executable")
    for label, comparison in (
        ("source.after_all_cases_comparison", source_binding.get("after_all_cases_comparison")),
        ("launch_image.source_copy_comparison", launch_binding.get("source_copy_comparison")),
        ("launch_image.after_all_cases_comparison", launch_binding.get("after_all_cases_comparison")),
    ):
        if not isinstance(comparison, dict) or comparison.get("passed") is not True:
            errors.append(f"executable_evidence.{label}.passed is not true")
    immutable_lock = launch_binding.get("immutable_lock")
    if not isinstance(immutable_lock, dict):
        errors.append("launch_image.immutable_lock is missing")
    else:
        for field in (
            "native_handle_held",
            "write_share_denied",
            "delete_share_denied",
        ):
            if immutable_lock.get(field) is not True:
                errors.append(f"launch_image.immutable_lock.{field} is not true")
    if launch_binding.get("sha_named") is not True:
        errors.append("immutable launch image is not SHA-named")
    if launch_binding.get("read_only_mode_set") is not True:
        errors.append("immutable launch image read-only mode was not set")
    if launch_binding.get("lock_close_after_all_cases", {}).get("closed") is not True:
        errors.append("immutable launch image lock closure was not proven")
    dependency_search = evidence.get("dependency_search")
    if not isinstance(dependency_search, dict) or any(
        dependency_search.get(field) is not True
        for field in (
            "working_directory_is_source_build_directory",
            "source_build_directory_prepended_to_path",
        )
    ):
        errors.append("immutable launch dependency-search policy is incomplete")

    source_path = Path(source_binding.get("resolved_path", ""))
    launch_path = Path(launch_binding.get("resolved_path", ""))
    if source_before is not None:
        if _path_key(Path(str(source.get("executable", "")))) != source_before[0]:
            errors.append("top-level executable path does not match source evidence")
    if launch_before is not None:
        if _path_key(Path(str(source.get("launch_executable", "")))) != launch_before[0]:
            errors.append("top-level launch_executable path does not match launch evidence")
    for label, path, expected in (
        ("source", source_path, source_after),
        ("launch", launch_path, launch_after),
    ):
        try:
            current = _file_identity(path)
            result[f"{label}_current_identity"] = current
            current_tuple = _executable_content_tuple(current)
            if expected is None or current_tuple != expected:
                errors.append(f"current {label} executable no longer matches manifest evidence")
        except Exception as exc:
            errors.append(f"current {label} executable identity failed: {exc}")

    cases = [case for case in source.get("cases", []) if isinstance(case, dict)]
    top_case_checks = evidence.get("case_checks")
    if not isinstance(top_case_checks, list) or len(top_case_checks) != len(cases):
        errors.append("executable_evidence.case_checks does not cover every case")
        top_case_checks = []
    checks_by_name = {
        str(check.get("case")): check
        for check in top_case_checks
        if isinstance(check, dict) and check.get("case")
    }
    for case in cases:
        case_name = str(case.get("name", ""))
        case_errors: list[str] = []
        case_evidence = case.get("executable_evidence")
        if not isinstance(case_evidence, dict):
            case_errors.append("case executable_evidence is missing")
        else:
            if case_evidence.get("schema") != EXECUTABLE_BINDING_SCHEMA:
                case_errors.append("case executable schema mismatch")
            if case_evidence.get("passed") is not True:
                case_errors.append("case executable evidence did not pass")
            if case_evidence.get("fail_closed") is True:
                case_errors.append("case executable evidence reports fail_closed")
            if _executable_content_tuple(
                case_evidence.get("source_before_launch")
            ) != source_before:
                case_errors.append("case source_before_launch differs from run baseline")
            if _executable_content_tuple(
                case_evidence.get("launch_image_before_launch")
            ) != launch_before:
                case_errors.append("case launch_image_before_launch differs from run baseline")
            for field in (
                "source_baseline_comparison",
                "launch_image_baseline_comparison",
            ):
                if case_evidence.get(field, {}).get("passed") is not True:
                    case_errors.append(f"case {field}.passed is not true")
            case_lock = case_evidence.get("immutable_lock")
            if not isinstance(case_lock, dict) or any(
                case_lock.get(field) is not True
                for field in (
                    "native_handle_held",
                    "write_share_denied",
                    "delete_share_denied",
                )
            ):
                case_errors.append("case immutable launch lock evidence is incomplete")
            binding = case_evidence.get("target_process_binding")
            if not isinstance(binding, dict):
                case_errors.append("target_process_binding is missing")
            else:
                if binding.get("passed") is not True:
                    case_errors.append("target_process_binding.passed is not true")
                target_pid = _manifest_integer(binding.get("pid"))
                if target_pid is None or target_pid <= 0:
                    case_errors.append("target process PID is not positive")
                creation_ticks = _manifest_integer(
                    binding.get("creation_filetime_ticks")
                )
                creation_key = binding.get("creation_time_key")
                if creation_ticks is None or creation_ticks <= 0:
                    case_errors.append("target exact creation FILETIME is missing")
                elif creation_key != f"winfiletime:{creation_ticks}":
                    case_errors.append("target creation key does not match its FILETIME")
                expected_stable_identity = (
                    f"{target_pid}@{creation_key}"
                    if target_pid is not None and target_pid > 0 and creation_key
                    else None
                )
                if binding.get("stable_identity") != expected_stable_identity:
                    case_errors.append("target stable identity does not bind PID and FILETIME")
                if binding.get("termination_authority_established") is not True:
                    case_errors.append("target launch-owned termination authority is missing")
                if binding.get("duplicated_from_launcher_handle") is not True:
                    case_errors.append("target identity was not duplicated from the launcher handle")
                if binding.get("pid_lookup_used") is not False:
                    case_errors.append("target identity used a PID lookup instead of launch ownership")
                target_control_pid = _manifest_integer(binding.get("target_control_pid"))
                if (
                    target_pid is None
                    or target_control_pid != target_pid
                    or binding.get("target_control_pid_matches_owned_handle") is not True
                ):
                    case_errors.append("TargetControl PID does not equal the launch-owned handle PID")
                if binding.get("image_path_matches") is not True:
                    case_errors.append("target process image path was not bound")
                if binding.get("same_native_handle_reserved_for_cleanup") is not True:
                    case_errors.append("target cleanup did not reserve the same native handle")
                cleanup_verification = binding.get("cleanup_verification")
                if not isinstance(cleanup_verification, dict):
                    case_errors.append("target same-handle cleanup verification is missing")
                elif (
                    cleanup_verification.get("passed") is not True
                    or cleanup_verification.get("termination_passed") is not True
                    or cleanup_verification.get("same_native_handle") is not True
                    or cleanup_verification.get("handle_closed") is not True
                    or cleanup_verification.get("original_running_after") is not False
                ):
                    case_errors.append("target same-handle cleanup verification failed")
                if launch_before is not None:
                    expected_path, expected_size, expected_sha = launch_before
                    if _path_key(Path(str(binding.get("expected_resolved_path", "")))) != expected_path:
                        case_errors.append("target binding expected path mismatch")
                    if binding.get("expected_size_bytes") != expected_size:
                        case_errors.append("target binding expected size mismatch")
                    if str(binding.get("expected_sha256", "")).lower() != expected_sha:
                        case_errors.append("target binding expected SHA-256 mismatch")
                    if _path_key(Path(str(binding.get("observed_image_path", "")))) != expected_path:
                        case_errors.append("target process observed image is not the immutable copy")
                if not binding.get("stable_identity"):
                    case_errors.append("target process stable creation-time identity is missing")
        top_check = checks_by_name.get(case_name)
        if top_check is None:
            case_errors.append("top-level case_checks entry is missing")
        elif isinstance(case_evidence, dict):
            normalized_top = {key: value for key, value in top_check.items() if key != "case"}
            if _canonical_json_sha256(normalized_top) != _canonical_json_sha256(case_evidence):
                case_errors.append("top-level and per-case executable evidence differ")
        result["case_results"][case_name] = {
            "passed": not case_errors,
            "errors": case_errors,
        }
        errors.extend(f"{case_name}: {error}" for error in case_errors)
    result["passed"] = not errors
    return result


def _validate_release_global_capture_uniqueness(
    source: dict[str, Any],
    manifest_path: Path,
) -> dict[str, Any]:
    paths: list[str] = []
    hashes: list[str] = []
    entries: list[dict[str, Any]] = []
    errors: list[str] = []
    for case in source.get("cases", []):
        if not isinstance(case, dict):
            continue
        for boundary in SMOKE_BOUNDARIES:
            entry = case.get("boundaries", {}).get(boundary)
            if not isinstance(entry, dict):
                errors.append(f"{case.get('name')}/{boundary}: boundary metadata is missing")
                continue
            path_value = entry.get("capture_path")
            if not isinstance(path_value, str) or not path_value.strip():
                errors.append(f"{case.get('name')}/{boundary}: capture_path is missing")
                continue
            path = Path(path_value)
            if not path.is_absolute():
                path = manifest_path.parent / path
            canonical = _path_key(path)
            sha256 = _expected_capture_sha256(entry)
            if sha256 is None or re.fullmatch(r"[0-9a-f]{64}", sha256) is None:
                errors.append(f"{case.get('name')}/{boundary}: capture SHA-256 is missing")
                continue
            paths.append(canonical)
            hashes.append(sha256)
            entries.append(
                {
                    "case": case.get("name"),
                    "boundary": boundary,
                    "canonical_path": canonical,
                    "sha256": sha256,
                }
            )
    if len(entries) != 6:
        errors.append(f"release must bind exactly six boundary captures, found {len(entries)}")
    if len(paths) == 6 and len(set(paths)) != 6:
        errors.append("release six-capture canonical paths are not unique")
    if len(hashes) == 6 and len(set(hashes)) != 6:
        errors.append("release six-capture SHA-256 values are not unique")
    return {
        "passed": not errors,
        "entry_count": len(entries),
        "unique_path_count": len(set(paths)),
        "unique_sha256_count": len(set(hashes)),
        "entries": entries,
        "errors": errors,
    }

def _validate_release_capture_copy_budget(source: dict[str, Any]) -> dict[str, Any]:
    record = source.get("capture_copy_budget")
    errors: list[str] = []
    if not isinstance(record, dict):
        return {
            "schema": "mgif-renderdoc-capture-copy-budget-v1",
            "passed": False,
            "errors": ["top-level capture_copy_budget is missing"],
        }
    expected_candidate_count = 2 * len(SMOKE_BOUNDARIES) * SMOKE_MAX_CANDIDATES_PER_BOUNDARY
    for field, expected in (
        ("schema", "mgif-renderdoc-capture-copy-budget-v1"),
        ("case_count", 2),
        ("boundary_count", len(SMOKE_BOUNDARIES)),
        ("max_candidates_per_boundary", SMOKE_MAX_CANDIDATES_PER_BOUNDARY),
        ("max_candidate_count", expected_candidate_count),
        ("single_rdc_cap_bytes", COMPARATOR_MAX_RDC_BYTES),
    ):
        if record.get(field) != expected:
            errors.append(
                f"capture_copy_budget.{field}={record.get(field)!r}, expected {expected!r}"
            )
    if record.get("within_count_budget") is not True:
        errors.append("capture_copy_budget count budget did not pass")
    if record.get("within_actual_byte_budget") is not True:
        errors.append("capture_copy_budget actual byte budget did not pass")
    candidate_count = _manifest_integer(record.get("candidate_count"))
    actual_bytes = _manifest_integer(record.get("actual_candidate_bytes"))
    max_bytes = _manifest_integer(record.get("max_candidate_bytes"))
    if candidate_count is None or not (6 <= candidate_count <= expected_candidate_count):
        errors.append("capture_copy_budget candidate_count is outside [6, 12]")
    if (
        actual_bytes is None
        or max_bytes is None
        or actual_bytes <= 0
        or max_bytes != expected_candidate_count * COMPARATOR_MAX_RDC_BYTES
        or actual_bytes > max_bytes
    ):
        errors.append("capture_copy_budget actual/max byte accounting is invalid")
    reservations = record.get("reservations")
    if not isinstance(reservations, list) or (
        candidate_count is not None and len(reservations) != candidate_count
    ):
        errors.append("capture_copy_budget reservations do not match candidate_count")
        reservations = []
    for index, reservation in enumerate(reservations):
        if not isinstance(reservation, dict):
            errors.append(f"capture_copy_budget reservation {index} is invalid")
            continue
        if reservation.get("completed") is not True:
            errors.append(f"capture_copy_budget reservation {index} is incomplete")
        declared = _manifest_integer(reservation.get("declared_bytes"))
        actual = _manifest_integer(reservation.get("actual_bytes"))
        free = _manifest_integer(reservation.get("free_bytes_before_copy"))
        required = _manifest_integer(reservation.get("required_free_bytes_before_copy"))
        if declared is None or not (0 < declared <= COMPARATOR_MAX_RDC_BYTES):
            errors.append(f"capture_copy_budget reservation {index} declared bytes are invalid")
        if actual is None or not (0 < actual <= COMPARATOR_MAX_RDC_BYTES):
            errors.append(f"capture_copy_budget reservation {index} actual bytes are invalid")
        if free is None or required is None or free < required:
            errors.append(f"capture_copy_budget reservation {index} free-space proof is invalid")
    return {
        "schema": "mgif-renderdoc-capture-copy-budget-v1",
        "passed": not errors,
        "record": record,
        "errors": errors,
    }

def _validate_release_capture_set_evidence(
    source: dict[str, Any],
    manifest_path: Path,
    *,
    render_mode: str | None,
) -> dict[str, Any]:
    record = source.get("capture_set_validation")
    errors: list[str] = []
    if not isinstance(record, dict):
        return {
            "schema": FINAL_CAPTURE_SET_SCHEMA,
            "passed": False,
            "errors": ["top-level capture_set_validation is missing"],
        }
    if record.get("schema") != FINAL_CAPTURE_SET_SCHEMA:
        errors.append("capture_set_validation schema is invalid")
    if record.get("passed") is not True or record.get("errors") not in ([], None):
        errors.append("capture_set_validation did not pass cleanly")
    if record.get("rehash_after_all_cases") is not True:
        errors.append("capture_set_validation did not rehash after all cases")
    for field, expected in (
        ("expected_capture_count", 6),
        ("entry_count", 6),
        ("unique_canonical_path_count", 6),
        ("unique_sha256_count", 6),
        ("single_rdc_cap_bytes", COMPARATOR_MAX_RDC_BYTES),
    ):
        if record.get(field) != expected:
            errors.append(
                f"capture_set_validation.{field}={record.get(field)!r}, "
                f"expected {expected!r}"
            )
    if _path_key(Path(str(record.get("manifest_path", "")))) != _path_key(
        manifest_path
    ):
        errors.append("capture_set_validation.manifest_path does not bind this manifest")
    formal = record.get("formal_release")
    if not isinstance(formal, dict):
        errors.append("capture_set_validation.formal_release is missing")
    else:
        for field in ("applicable", "passed"):
            if formal.get(field) is not True:
                errors.append(f"capture_set_validation.formal_release.{field} is not true")
        if formal.get("render_mode") != render_mode:
            errors.append("capture_set_validation formal render mode differs")
        if formal.get("required_case_count") != 2:
            errors.append("capture_set_validation formal case count is not two")
        if formal.get("required_capture_count") != 6:
            errors.append("capture_set_validation formal capture count is not six")
        if formal.get("required_gi_mode") != "no-ddgi":
            errors.append("capture_set_validation formal GI mode is not no-ddgi")
        if formal.get("required_sequence") != RELEASE_SMOKE_SEQUENCE:
            errors.append("capture_set_validation formal sequence is not canonical 8/24/8")
        required_modes = formal.get("required_modes")
        if not isinstance(required_modes, list) or len(required_modes) != 2 or set(
            required_modes
        ) != set(RELEASE_CASE_MODES):
            errors.append("capture_set_validation formal modes are incomplete")
        if formal.get("errors") not in ([], None):
            errors.append("capture_set_validation formal errors are not empty")

    expected_entries: dict[tuple[str, str], tuple[str, str]] = {}
    for case in source.get("cases", []):
        if not isinstance(case, dict):
            continue
        case_name = str(case.get("name", ""))
        for boundary in SMOKE_BOUNDARIES:
            boundary_record = case.get("boundaries", {}).get(boundary)
            if not isinstance(boundary_record, dict):
                continue
            path_value = boundary_record.get("capture_path")
            digest = _expected_capture_sha256(boundary_record)
            if not isinstance(path_value, str) or not path_value.strip() or digest is None:
                continue
            path = Path(path_value)
            if not path.is_absolute():
                path = manifest_path.parent / path
            expected_entries[(case_name, boundary)] = (_path_key(path), digest)
    entries = record.get("entries")
    observed_entries: dict[tuple[str, str], tuple[str, str]] = {}
    if not isinstance(entries, list) or len(entries) != 6:
        errors.append("capture_set_validation.entries must contain exactly six records")
        entries = []
    for index, entry in enumerate(entries):
        if not isinstance(entry, dict):
            errors.append(f"capture_set_validation.entries[{index}] is not an object")
            continue
        key = (str(entry.get("case", "")), str(entry.get("boundary", "")))
        canonical_path = entry.get("canonical_path")
        digest = str(entry.get("sha256", "")).lower()
        if key in observed_entries:
            errors.append(f"capture_set_validation duplicate entry {key!r}")
            continue
        if not isinstance(canonical_path, str) or not canonical_path.strip():
            errors.append(f"capture_set_validation entry {key!r} path is missing")
            continue
        if re.fullmatch(r"[0-9a-f]{64}", digest) is None:
            errors.append(f"capture_set_validation entry {key!r} SHA-256 is invalid")
            continue
        observed_entries[key] = (_path_key(Path(canonical_path)), digest)
        if entry.get("passed") is not True:
            errors.append(f"capture_set_validation entry {key!r} did not pass")
        checks = entry.get("checks")
        if not isinstance(checks, dict) or not checks or any(
            value is not True for value in checks.values()
        ):
            errors.append(f"capture_set_validation entry {key!r} checks are incomplete")
        if entry.get("render_mode") != render_mode:
            errors.append(f"capture_set_validation entry {key!r} render mode differs")
        if entry.get("gi_mode") != "no-ddgi":
            errors.append(f"capture_set_validation entry {key!r} GI mode differs")
        size_bytes = entry.get("size_bytes")
        if not isinstance(size_bytes, int) or isinstance(size_bytes, bool) or not (
            0 < size_bytes <= COMPARATOR_MAX_RDC_BYTES
        ):
            errors.append(f"capture_set_validation entry {key!r} size is invalid")
    if expected_entries != observed_entries:
        errors.append(
            "capture_set_validation entries do not exactly match the six manifest "
            "boundary path/SHA bindings"
        )
    return {
        "schema": FINAL_CAPTURE_SET_SCHEMA,
        "passed": not errors,
        "render_mode": render_mode,
        "expected_entries": {
            f"{case}/{boundary}": {"canonical_path": value[0], "sha256": value[1]}
            for (case, boundary), value in sorted(expected_entries.items())
        },
        "observed_entries": {
            f"{case}/{boundary}": {"canonical_path": value[0], "sha256": value[1]}
            for (case, boundary), value in sorted(observed_entries.items())
        },
        "record": record,
        "errors": errors,
    }

def _validate_formal_source_executable_contract(source: dict[str, Any]) -> list[str]:
    errors: list[str] = []
    record = source.get("formal_source_executable_contract")
    if not isinstance(record, dict):
        return ["formal_source_executable_contract is missing"]
    if record.get("schema") != "mgif-formal-source-executable-contract-v1":
        errors.append("formal source executable schema is invalid")
    if record.get("applicable") is not True or record.get("passed") is not True:
        errors.append("formal source executable contract is not applicable/passed")
    if record.get("immutable_copy_required") is not True:
        errors.append("formal source executable did not require an immutable SHA copy")
    if record.get("errors") not in ([], None):
        errors.append("formal source executable contract contains errors")
    canonical_value = record.get("canonical_source_executable")
    selected_value = record.get("selected_source_executable")
    source_value = source.get("executable")
    if not all(
        isinstance(value, str) and value.strip()
        for value in (canonical_value, selected_value, source_value)
    ):
        errors.append("formal source executable paths are missing")
        return errors
    canonical = Path(canonical_value)
    selected = Path(selected_value)
    source_path = Path(source_value)
    if not all(path.is_absolute() for path in (canonical, selected, source_path)):
        errors.append("formal source executable paths must be absolute")
    if not (
        _path_key(canonical) == _path_key(selected) == _path_key(source_path)
    ):
        errors.append("formal source executable paths do not bind the launched source")
    suffix = tuple(part.casefold() for part in canonical.parts[-4:])
    if suffix != ("out", "build", "x64-debug", "demo.exe"):
        errors.append("formal source must be the rebuilt out/build/x64-debug/Demo.exe")
    if canonical.name.casefold() == "demo.csm_shadow_reactive_test.exe":
        errors.append("stale Demo.csm_shadow_reactive_test.exe is forbidden")
    checks = record.get("checks")
    if not isinstance(checks, dict) or any(
        checks.get(field) is not True
        for field in (
            "canonical_source_path",
            "canonical_filename",
            "stale_reactive_test_name_rejected",
        )
    ):
        errors.append("formal source executable checks are incomplete")
    return errors

def _validate_release_manifest_contract(
    source: dict[str, Any],
    manifest_path: Path,
) -> list[str]:
    errors: list[str] = []
    profile = _release_manifest_profile(source)
    errors.extend(f"release profile: {error}" for error in profile["errors"])
    copy_budget = _validate_release_capture_copy_budget(source)
    errors.extend(f"capture budget: {error}" for error in copy_budget["errors"])
    release_contract = profile.get("case_contract", {})
    capture_set = _validate_release_capture_set_evidence(
        source,
        manifest_path,
        render_mode=profile.get("render_mode"),
    )
    errors.extend(f"capture set: {error}" for error in capture_set["errors"])
    if source.get("schema") != SMOKE_MANIFEST_SCHEMA:
        errors.append(f"top-level schema must be {SMOKE_MANIFEST_SCHEMA!r}")
    if source.get("status") != "passed":
        errors.append(f"top-level status is {source.get('status')!r}, expected 'passed'")
    if source.get("error"):
        errors.append("top-level manifest contains an error")
    try:
        started = _parse_utc(source.get("started_utc"), "manifest.started_utc")
        completed = _parse_utc(source.get("completed_utc"), "manifest.completed_utc")
        if completed < started:
            errors.append("manifest.completed_utc precedes manifest.started_utc")
    except ComparatorError as exc:
        errors.append(str(exc))
    recorded_manifest = source.get("manifest_path")
    if not isinstance(recorded_manifest, str) or not recorded_manifest.strip():
        errors.append("top-level manifest_path is missing")
    elif _path_key(Path(recorded_manifest)) != _path_key(manifest_path):
        errors.append(
            f"top-level manifest_path does not bind this file: {recorded_manifest!r}"
        )
    output_directory = source.get("output_directory")
    if not isinstance(output_directory, str) or not output_directory.strip():
        errors.append("top-level output_directory is missing")
    elif _path_key(Path(output_directory)) != _path_key(manifest_path.parent):
        errors.append("top-level output_directory does not match the manifest directory")
    boundary_order = source.get("boundary_order")
    if not isinstance(boundary_order, list) or tuple(boundary_order[:3]) != SMOKE_BOUNDARIES:
        errors.append(
            "top-level boundary_order must begin with last-moving, first-still, settled"
        )
    manifest_options = source.get("options")
    if not isinstance(manifest_options, dict):
        errors.append("top-level options must be an object")
    else:
        for field, expected in RELEASE_SMOKE_SEQUENCE.items():
            observed = _manifest_integer(manifest_options.get(field))
            if observed is None:
                errors.append(
                    f"top-level options.{field} must be integer {expected}"
                )
            elif observed != expected:
                errors.append(
                    f"top-level options.{field}={observed}, expected {expected} "
                    "for the release smoke sequence"
                )
    errors.extend(
        f"formal source executable: {error}"
        for error in _validate_formal_source_executable_contract(source)
    )
    toolchain = source.get("toolchain_evidence")
    if not isinstance(toolchain, dict):
        errors.append("top-level toolchain_evidence is missing")
    else:
        if toolchain.get("schema") != TOOLCHAIN_EVIDENCE_SCHEMA:
            errors.append("top-level toolchain_evidence schema is invalid")
        if toolchain.get("required") is not True:
            errors.append("top-level toolchain evidence is not required")
        if toolchain.get("passed") is not True or toolchain.get("errors") not in ([], None):
            errors.append("top-level toolchain evidence did not pass cleanly")
        if not isinstance(toolchain.get("before_all_cases"), dict) or not isinstance(
            toolchain.get("after_all_cases"), dict
        ):
            errors.append("top-level toolchain before/after snapshots are missing")
        if not isinstance(toolchain.get("comparison"), dict) or toolchain.get(
            "comparison", {}
        ).get("passed") is not True:
            errors.append("top-level toolchain comparison did not pass")
    disk_preflight = source.get("disk_preflight")
    if not isinstance(disk_preflight, dict):
        errors.append("top-level capture disk_preflight is missing")
    else:
        if disk_preflight.get("schema") != "mgif-csm-smoke-disk-preflight-v1":
            errors.append("top-level capture disk_preflight schema is invalid")
        if disk_preflight.get("passed") is not True or disk_preflight.get(
            "errors"
        ) not in ([], None):
            errors.append("top-level capture disk_preflight did not pass cleanly")
        estimate = disk_preflight.get("estimate")
        if not isinstance(estimate, dict):
            errors.append("top-level capture disk estimate is missing")
        else:
            estimated = _manifest_integer(estimate.get("estimated_bytes"))
            safety = _manifest_integer(estimate.get("safety_margin_bytes"))
            required = _manifest_integer(estimate.get("required_free_bytes"))
            free = _manifest_integer(disk_preflight.get("free_bytes"))
            if (
                estimated is None
                or safety is None
                or required is None
                or free is None
                or estimated <= 0
                or safety <= 0
                or required != estimated + safety
                or free < required
            ):
                errors.append("top-level capture disk estimate/free-space proof is invalid")
    doctor = source.get("rdc_doctor")
    if not isinstance(doctor, dict) or doctor.get("returncode") != 0:
        errors.append("top-level rdc_doctor.returncode is not 0")
    errors.extend(_validate_release_cleanup_contract(source))
    executable_validation = _validate_release_executable_evidence(source)
    errors.extend(executable_validation["errors"])
    global_capture_uniqueness = _validate_release_global_capture_uniqueness(
        source,
        manifest_path,
    )
    errors.extend(global_capture_uniqueness["errors"])
    cross_case = source.get("cross_case_pose_validation")
    if not isinstance(cross_case, dict) or cross_case.get("passed") is not True:
        errors.append("top-level cross_case_pose_validation.passed is not true")
    cases = source.get("cases")
    if not isinstance(cases, list) or not cases:
        errors.append("top-level cases must be a non-empty array")
        return errors
    if len(cases) != len(RELEASE_CASE_MODES):
        errors.append(
            "release manifest must contain exactly two target cases: "
            "translate-stop and rotate-stop"
        )
    observed_modes = {
        str(case.get("mode")) for case in cases if isinstance(case, dict)
    }
    if observed_modes != set(RELEASE_CASE_MODES):
        errors.append(
            f"release case modes are {sorted(observed_modes)!r}, expected "
            f"{sorted(RELEASE_CASE_MODES)!r}"
        )
    names: set[str] = set()
    for index, case in enumerate(cases):
        label = f"cases[{index}]"
        if not isinstance(case, dict):
            errors.append(f"{label} is not an object")
            continue
        name = case.get("name")
        if not isinstance(name, str) or not name:
            errors.append(f"{label}.name is missing")
        elif name in names:
            errors.append(f"duplicate case name {name!r}")
        else:
            names.add(name)
        if case.get("status") != "passed":
            errors.append(f"{label}.status is not 'passed'")
        if case.get("error"):
            errors.append(f"{label} contains an error")
        mode = case.get("mode")
        if not isinstance(mode, str) or not mode:
            errors.append(f"{label}.mode is missing")
        elif mode in release_contract:
            expected_case = release_contract[mode]
            expected_name = f"{mode}__{expected_case['render_mode']}__{expected_case['gi_mode']}"
            if name != expected_name:
                errors.append(f"{label}.name={name!r}, expected {expected_name!r}")
            for field, expected_value in expected_case.items():
                if case.get(field) != expected_value:
                    errors.append(
                        f"{label}.{field}={case.get(field)!r}, expected {expected_value!r}"
                    )
        case_order = case.get("boundary_order")
        if not isinstance(case_order, list) or tuple(case_order[:3]) != SMOKE_BOUNDARIES:
            errors.append(f"{label}.boundary_order is invalid")
        declared_frames = case.get("expected_automation_frames")
        if not isinstance(declared_frames, dict):
            errors.append(f"{label}.expected_automation_frames is not an object")
        else:
            for boundary, expected_frame in RELEASE_BOUNDARY_FRAMES.items():
                observed_frame = _manifest_integer(declared_frames.get(boundary))
                if observed_frame != expected_frame:
                    errors.append(
                        f"{label}.expected_automation_frames.{boundary}="
                        f"{declared_frames.get(boundary)!r}, expected {expected_frame}"
                    )
        boundaries = case.get("boundaries")
        if not isinstance(boundaries, dict):
            errors.append(f"{label}.boundaries is not an object")
        else:
            for boundary, expected_frame in RELEASE_BOUNDARY_FRAMES.items():
                boundary_entry = boundaries.get(boundary)
                if not isinstance(boundary_entry, dict):
                    errors.append(f"{label}.boundaries.{boundary} is missing")
                    continue
                ready = boundary_entry.get("ready")
                if not isinstance(ready, dict):
                    errors.append(f"{label}.boundaries.{boundary}.ready is missing")
                    continue
                ready_frame = _manifest_integer(ready.get("frame"))
                if ready_frame != expected_frame:
                    errors.append(
                        f"{label}.boundaries.{boundary}.ready.frame="
                        f"{ready.get('frame')!r}, expected {expected_frame}"
                    )
        if not isinstance(case.get("pose_validation"), dict) or case["pose_validation"].get("passed") is not True:
            errors.append(f"{label}.pose_validation.passed is not true")
        if not isinstance(case.get("cleanup"), dict) or case["cleanup"].get("passed") is not True:
            errors.append(f"{label}.cleanup.passed is not true")
        try:
            case_started = _parse_utc(case.get("started_utc"), f"{label}.started_utc")
            case_completed = _parse_utc(case.get("completed_utc"), f"{label}.completed_utc")
            if case_completed < case_started:
                errors.append(f"{label}.completed_utc precedes started_utc")
        except ComparatorError as exc:
            errors.append(str(exc))
    return errors


def _disk_space_preflight(
    work_root: Path,
    *,
    capture_count: int,
    disk_usage_fn: Any = shutil.disk_usage,
) -> dict[str, Any]:
    if capture_count <= 0:
        raise ComparatorError("disk preflight capture_count must be positive")
    resolved = work_root.resolve()
    try:
        usage = disk_usage_fn(resolved)
        free_bytes = int(usage.free)
    except Exception as exc:
        raise ComparatorError(
            f"cannot query free space for comparator work directory {resolved}: "
            f"{type(exc).__name__}: {exc}"
        ) from exc
    extraction_bytes = capture_count * COMPARATOR_EXTRACT_BYTES_PER_CAPTURE
    session_bytes = capture_count * COMPARATOR_SESSION_BYTES_PER_CAPTURE
    estimated_bytes = extraction_bytes + session_bytes
    safety_margin_bytes = max(
        DISK_SAFETY_MIN_BYTES,
        int(math.ceil(estimated_bytes * DISK_SAFETY_FRACTION)),
    )
    required_free_bytes = estimated_bytes + safety_margin_bytes
    errors = []
    if free_bytes < required_free_bytes:
        errors.append(
            "insufficient free space: "
            f"free={free_bytes} required={required_free_bytes}"
        )
    return {
        "schema": "mgif-rdc-comparator-disk-preflight-v1",
        "passed": not errors,
        "work_directory": str(resolved),
        "capture_count": capture_count,
        "estimate": {
            "extraction_bytes_per_capture": COMPARATOR_EXTRACT_BYTES_PER_CAPTURE,
            "session_bytes_per_capture": COMPARATOR_SESSION_BYTES_PER_CAPTURE,
            "extraction_bytes": extraction_bytes,
            "session_bytes": session_bytes,
            "estimated_bytes": estimated_bytes,
            "safety_margin_fraction": DISK_SAFETY_FRACTION,
            "safety_margin_min_bytes": DISK_SAFETY_MIN_BYTES,
            "safety_margin_bytes": safety_margin_bytes,
            "required_free_bytes": required_free_bytes,
        },
        "free_bytes": free_bytes,
        "free_after_required_bytes": free_bytes - required_free_bytes,
        "errors": errors,
    }


def _comparison_capture_count(options: argparse.Namespace) -> int:
    if options.manifest is None:
        return 2
    source = _load_smoke_manifest(options.manifest.resolve())
    case_count = sum(isinstance(case, dict) for case in source.get("cases", []))
    return max(1, case_count) * len(SMOKE_BOUNDARIES)

def _require_disk_space(preflight: dict[str, Any]) -> None:
    if preflight.get("passed") is not True:
        raise ComparatorError(
            "comparator work-directory disk preflight failed closed: "
            + "; ".join(str(error) for error in preflight.get("errors", []))
        )


def _deadline_timeout(
    overall_deadline: float,
    requested_timeout: float,
    *,
    stage: str,
    cleanup_reserve: float = 0.0,
) -> float:
    remaining = overall_deadline - time.monotonic() - cleanup_reserve
    if remaining <= 0.0:
        raise ComparatorError(
            f"total comparator deadline exhausted before {stage}; "
            f"cleanup reserve={cleanup_reserve:g}s"
        )
    return min(float(requested_timeout), remaining)


def _deadline_evidence(
    *,
    started_monotonic: float,
    overall_deadline: float,
    configured_seconds: float,
) -> dict[str, Any]:
    now = time.monotonic()
    elapsed = max(0.0, now - started_monotonic)
    remaining = overall_deadline - now
    return {
        "schema": "mgif-rdc-comparator-deadline-v1",
        "configured_seconds": float(configured_seconds),
        "elapsed_seconds": elapsed,
        "remaining_seconds": max(0.0, remaining),
        "cleanup_reserve_seconds": CAPTURE_CLEANUP_RESERVE_SECONDS,
        "exceeded": remaining < 0.0,
    }


def _require_deadline(overall_deadline: float, *, stage: str) -> None:
    if time.monotonic() >= overall_deadline:
        raise ComparatorError(f"total comparator deadline exceeded at {stage}")

def _run_command(
    command: list[str],
    *,
    timeout: float,
    check: bool = True,
) -> subprocess.CompletedProcess[str]:
    try:
        process = subprocess.run(
            command,
            text=True,
            encoding="utf-8",
            errors="replace",
            capture_output=True,
            timeout=timeout,
            check=False,
        )
    except subprocess.TimeoutExpired as exc:
        stdout = exc.stdout or ""
        stderr = exc.stderr or ""
        if isinstance(stdout, bytes):
            stdout = stdout.decode("utf-8", errors="replace")
        if isinstance(stderr, bytes):
            stderr = stderr.decode("utf-8", errors="replace")
        raise ComparatorError(
            f"command timed out after {timeout:g}s: {' '.join(command)}\n"
            f"stdout:\n{stdout[-4000:]}\n"
            f"stderr:\n{stderr[-4000:]}"
        ) from exc
    if check and process.returncode != 0:
        raise ComparatorError(
            f"command failed ({process.returncode}): {' '.join(command)}\n"
            f"stdout:\n{process.stdout[-4000:]}\n"
            f"stderr:\n{process.stderr[-4000:]}"
        )
    return process


def _host_rdc_data_directory() -> Path:
    override = os.environ.get("RDC_DATA_DIR")
    if override:
        return Path(override).expanduser().resolve()
    if os.name == "nt":
        base = os.environ.get("LOCALAPPDATA", str(Path.home()))
        return (Path(base) / "rdc").resolve()
    return (Path.home() / ".rdc").resolve()


def _host_rdc_session_state_path(session: str) -> Path:
    if re.fullmatch(r"[A-Za-z0-9_-]{1,64}", session) is None:
        raise ComparatorError(f"invalid rdc session name: {session!r}")
    return _host_rdc_data_directory() / "sessions" / f"{session}.json"


def _host_filetime_ticks(value: Any) -> int:
    return (int(value.dwHighDateTime) << 32) | int(value.dwLowDateTime)


def _host_state_file_handle_snapshot(handle: Any) -> dict[str, Any]:
    result: dict[str, Any] = {
        "schema": STATE_HANDLE_SNAPSHOT_SCHEMA,
        "valid": False,
        "source": (
            "msvcrt.get_osfhandle + GetFileTime + "
            "GetFinalPathNameByHandleW(VOLUME_NAME_GUID|FILE_NAME_NORMALIZED) + "
            "GetVolumeInformationByHandleW + GetDriveTypeW"
        ),
        "path_fallback_used": False,
        "original_path_used": False,
        "final_path_flags": {
            "value": WINDOWS_FINAL_PATH_FLAGS,
            "volume_name": "VOLUME_NAME_GUID",
            "file_name": "FILE_NAME_NORMALIZED",
        },
    }
    if os.name != "nt":
        result["error"] = "state-file handle proof requires Windows native APIs"
        return result
    try:
        import msvcrt

        native_handle_value = int(msvcrt.get_osfhandle(handle.fileno()))
    except (ImportError, OSError, ValueError) as exc:
        result["error"] = (
            "held state-file native handle is unavailable: "
            f"{type(exc).__name__}: {exc}"
        )
        return result
    if native_handle_value <= 0:
        result["error"] = "held state-file native handle is invalid"
        return result
    native_handle = wintypes.HANDLE(native_handle_value)

    last_write = _HostFileTime()
    ctypes.set_last_error(0)
    if not _HOST_KERNEL32.GetFileTime(
        native_handle, None, None, ctypes.byref(last_write)
    ):
        result["error"] = (
            "GetFileTime failed for held state-file handle: "
            f"winerror={ctypes.get_last_error()}"
        )
        return result
    filetime_ticks = _host_filetime_ticks(last_write)
    if filetime_ticks <= WINDOWS_FILETIME_EPOCH_OFFSET_TICKS:
        result["error"] = "held state-file last-write FILETIME is invalid"
        return result

    ctypes.set_last_error(0)
    required_chars = int(
        _HOST_KERNEL32.GetFinalPathNameByHandleW(
            native_handle,
            None,
            0,
            WINDOWS_FINAL_PATH_FLAGS,
        )
    )
    if required_chars <= 0:
        result["error"] = (
            "GetFinalPathNameByHandleW size query failed for held state-file handle: "
            f"winerror={ctypes.get_last_error()}"
        )
        return result
    final_path_buffer = ctypes.create_unicode_buffer(required_chars + 1)
    ctypes.set_last_error(0)
    copied_chars = int(
        _HOST_KERNEL32.GetFinalPathNameByHandleW(
            native_handle,
            final_path_buffer,
            len(final_path_buffer),
            WINDOWS_FINAL_PATH_FLAGS,
        )
    )
    if copied_chars <= 0 or copied_chars >= len(final_path_buffer):
        result["error"] = (
            "GetFinalPathNameByHandleW failed for held state-file handle: "
            f"copied={copied_chars}, capacity={len(final_path_buffer)}, "
            f"winerror={ctypes.get_last_error()}"
        )
        return result
    final_path_guid = final_path_buffer.value
    volume_match = re.match(
        r"^\\\\\?\\Volume\{[0-9A-Fa-f]{8}-[0-9A-Fa-f]{4}-"
        r"[0-9A-Fa-f]{4}-[0-9A-Fa-f]{4}-[0-9A-Fa-f]{12}\}\\",
        final_path_guid,
    )
    if volume_match is None:
        result["error"] = (
            "held state-file final path is not a normalized volume-GUID path; "
            "UNC, remote, device, or unresolved identities are not accepted"
        )
        result["final_path_guid"] = final_path_guid
        return result
    volume_guid_root = volume_match.group(0)

    volume_serial = wintypes.DWORD()
    maximum_component_length = wintypes.DWORD()
    file_system_flags = wintypes.DWORD()
    file_system_buffer = ctypes.create_unicode_buffer(261)
    ctypes.set_last_error(0)
    if not _HOST_KERNEL32.GetVolumeInformationByHandleW(
        native_handle,
        None,
        0,
        ctypes.byref(volume_serial),
        ctypes.byref(maximum_component_length),
        ctypes.byref(file_system_flags),
        file_system_buffer,
        len(file_system_buffer),
    ):
        result["error"] = (
            "GetVolumeInformationByHandleW failed for held state-file handle: "
            f"winerror={ctypes.get_last_error()}"
        )
        return result
    drive_type_code = int(_HOST_KERNEL32.GetDriveTypeW(volume_guid_root))
    drive_type_names = {
        0: "unknown",
        1: "no-root",
        2: "removable",
        3: "fixed",
        4: "remote",
        5: "cdrom",
        6: "ramdisk",
    }
    result.update(
        {
            "valid": True,
            "native_handle_value": native_handle_value,
            "filetime_ticks": filetime_ticks,
            "final_path_guid": final_path_guid,
            "volume_guid_root": volume_guid_root,
            "volume_serial_number": int(volume_serial.value),
            "maximum_component_length": int(maximum_component_length.value),
            "file_system_flags": int(file_system_flags.value),
            "file_system": file_system_buffer.value.upper(),
            "drive_type_code": drive_type_code,
            "drive_type": drive_type_names.get(drive_type_code, "invalid"),
        }
    )
    return result


def _host_evaluate_state_file_handle_policy(
    before: Any,
    after: Any,
) -> dict[str, Any]:
    result: dict[str, Any] = {
        "schema": STATE_VOLUME_EVIDENCE_SCHEMA,
        "verified": False,
        "source": "two snapshots from the same held state-file native handle",
        "path_fallback_used": False,
        "original_path_used": False,
        "same_handle_path_identity": False,
        "raw_filetime_stable": False,
        "local": False,
        "supported_file_system": False,
        "native_filetime_semantics": False,
        "required_drive_type": "fixed",
        "required_file_systems": sorted(SUPPORTED_STATE_FILE_SYSTEMS),
        "errors": [],
    }
    errors: list[str] = result["errors"]
    expected_source = (
        "msvcrt.get_osfhandle + GetFileTime + "
        "GetFinalPathNameByHandleW(VOLUME_NAME_GUID|FILE_NAME_NORMALIZED) + "
        "GetVolumeInformationByHandleW + GetDriveTypeW"
    )
    expected_flags = {
        "value": WINDOWS_FINAL_PATH_FLAGS,
        "volume_name": "VOLUME_NAME_GUID",
        "file_name": "FILE_NAME_NORMALIZED",
    }
    volume_guid_root_pattern = re.compile(
        r"^\\\\\?\\Volume\{[0-9A-Fa-f]{8}-[0-9A-Fa-f]{4}-"
        r"[0-9A-Fa-f]{4}-[0-9A-Fa-f]{4}-[0-9A-Fa-f]{12}\}\\$"
    )
    for label, snapshot in (("before-read", before), ("after-read", after)):
        if not isinstance(snapshot, dict):
            errors.append(f"{label} held-handle snapshot is not an object")
            continue
        if snapshot.get("valid") is not True:
            errors.append(
                f"{label} held-handle snapshot is invalid: {snapshot.get('error')!r}"
            )
        if snapshot.get("schema") != STATE_HANDLE_SNAPSHOT_SCHEMA:
            errors.append(f"{label} held-handle snapshot schema is invalid")
        if snapshot.get("source") != expected_source:
            errors.append(f"{label} held-handle snapshot source is not native-handle proof")
        if snapshot.get("path_fallback_used") is not False:
            errors.append(f"{label} held-handle snapshot used a path fallback")
        if snapshot.get("original_path_used") is not False:
            errors.append(f"{label} held-handle snapshot used the original path as proof")
        if snapshot.get("final_path_flags") != expected_flags:
            errors.append(
                f"{label} held-handle snapshot did not use GUID plus normalized final-path flags"
            )
        native_handle_value = snapshot.get("native_handle_value")
        if (
            isinstance(native_handle_value, bool)
            or not isinstance(native_handle_value, int)
            or native_handle_value <= 0
        ):
            errors.append(f"{label} native handle identity is unavailable")
        final_path_guid = snapshot.get("final_path_guid")
        volume_guid_root = snapshot.get("volume_guid_root")
        if (
            not isinstance(final_path_guid, str)
            or not final_path_guid
            or not isinstance(volume_guid_root, str)
            or volume_guid_root_pattern.fullmatch(volume_guid_root) is None
            or not final_path_guid.startswith(volume_guid_root)
        ):
            errors.append(
                f"{label} final path is not bound to a normalized volume-GUID root"
            )
        for field, minimum in (
            ("volume_serial_number", 0),
            ("maximum_component_length", 1),
            ("file_system_flags", 0),
        ):
            value = snapshot.get(field)
            if isinstance(value, bool) or not isinstance(value, int) or value < minimum:
                errors.append(f"{label} {field} is unavailable or invalid")
        drive_type_code = snapshot.get("drive_type_code")
        if isinstance(drive_type_code, bool) or not isinstance(drive_type_code, int):
            errors.append(f"{label} drive type is unavailable")
        if not isinstance(snapshot.get("file_system"), str) or not snapshot.get(
            "file_system"
        ):
            errors.append(f"{label} filesystem identity is unavailable")
    if errors:
        result["error"] = "; ".join(errors)
        return result
    identity_fields = (
        "native_handle_value",
        "final_path_guid",
        "volume_guid_root",
        "volume_serial_number",
        "maximum_component_length",
        "file_system_flags",        "file_system",
        "drive_type_code",
    )
    mismatched_identity = [
        field for field in identity_fields if before.get(field) != after.get(field)
    ]
    result["same_handle_path_identity"] = not mismatched_identity
    if mismatched_identity:
        errors.append(
            "held state-file identity changed across read: "
            f"{mismatched_identity}"
        )

    before_ticks = before.get("filetime_ticks")
    after_ticks = after.get("filetime_ticks")
    result["raw_filetime_stable"] = (
        isinstance(before_ticks, int)
        and not isinstance(before_ticks, bool)
        and before_ticks > WINDOWS_FILETIME_EPOCH_OFFSET_TICKS
        and before_ticks == after_ticks
    )
    if result["raw_filetime_stable"] is not True:
        errors.append(
            "held state-file raw last-write FILETIME changed across read or is invalid"
        )

    drive_type_code = before.get("drive_type_code")
    file_system = str(before.get("file_system", "")).upper()
    local = drive_type_code == WINDOWS_DRIVE_FIXED
    supported = file_system in SUPPORTED_STATE_FILE_SYSTEMS
    result.update(
        {
            "drive_type_code": drive_type_code,
            "drive_type": before.get("drive_type"),
            "file_system": file_system,
            "local": local,
            "supported_file_system": supported,
            "native_filetime_semantics": local and supported,
            "filetime_ticks": before_ticks if result["raw_filetime_stable"] else None,
            "final_path_guid": before.get("final_path_guid"),
            "volume_guid_root": before.get("volume_guid_root"),
            "volume_serial_number": before.get("volume_serial_number"),
            "before_snapshot": before,
            "after_snapshot": after,
        }
    )
    if not local:
        errors.append(
            "state-file handle volume is not a verified local fixed drive; "
            f"drive_type={before.get('drive_type')!r}, code={drive_type_code!r}"
        )
    if not supported:
        errors.append(
            "state-file handle volume does not have explicitly supported FILETIME "
            f"semantics; file_system={file_system!r}"
        )
    result["verified"] = not errors
    if errors:
        result["error"] = "; ".join(errors)
    return result


def _host_state_publication_boundary_evidence(
    before_snapshot: Any,
    after_snapshot: Any,
    modified_ns: Any,
) -> dict[str, Any]:
    result: dict[str, Any] = {
        "schema": STATE_PUBLICATION_BOUNDARY_SCHEMA,
        "exact": False,
        "clock": "windows-filetime-100ns",
        "resolution_ns": WINDOWS_FILETIME_TICK_NS,
        "source": (
            "GetFileTime before/after read on the same held state-file handle, "
            "with GUID final-path and handle-derived volume proof"
        ),
        "native_filetime_read_before_after": False,
        "state_file_handle_held_during_read": True,
        "same_handle_path_identity": False,
        "raw_filetime_stable": False,
        "path_fallback_used": False,
        "divisibility_used_as_granularity_proof": False,
        "timestamp_equality_accepted": False,
        "ordering_rule": (
            "process_creation_filetime_ticks < state_publication_filetime_ticks"
        ),
    }
    policy = _host_evaluate_state_file_handle_policy(before_snapshot, after_snapshot)
    result["volume"] = policy
    result["same_handle_path_identity"] = policy.get("same_handle_path_identity") is True
    result["raw_filetime_stable"] = policy.get("raw_filetime_stable") is True
    if policy.get("verified") is not True:
        result["error"] = (
            "held state-file handle cannot prove a trustworthy publication boundary: "
            f"{policy.get('error', 'unknown handle policy error')}"
        )
        return result
    if isinstance(modified_ns, bool) or not isinstance(modified_ns, int) or modified_ns <= 0:
        result["error"] = "state-file modification time is not a positive integer"
        return result
    filetime_ticks = policy.get("filetime_ticks")
    if (
        isinstance(filetime_ticks, bool)
        or not isinstance(filetime_ticks, int)
        or filetime_ticks <= WINDOWS_FILETIME_EPOCH_OFFSET_TICKS
    ):
        result["error"] = "state-file publication FILETIME is invalid"
        return result
    raw_modified_ns = (
        filetime_ticks - WINDOWS_FILETIME_EPOCH_OFFSET_TICKS
    ) * WINDOWS_FILETIME_TICK_NS
    if modified_ns != raw_modified_ns:
        result["error"] = (
            "Python stat modification time disagrees with native held-handle FILETIME: "
            f"stat={modified_ns}, native={raw_modified_ns}"
        )
        return result
    result.update(
        {
            "exact": True,
            "modified_ns": modified_ns,
            "filetime_ticks": filetime_ticks,
            "raw_last_write_filetime_ticks_before": before_snapshot.get(
                "filetime_ticks"
            ),
            "raw_last_write_filetime_ticks_after": after_snapshot.get(
                "filetime_ticks"
            ),
            "native_filetime_read_before_after": True,
            "stat_mtime_matches_native_filetime": True,
            "positive_post_state_tolerance_ticks": 0,
        }
    )
    return result

def _read_host_session_state(
    path: Path,
) -> tuple[dict[str, Any], dict[str, Any] | None]:
    record: dict[str, Any] = {
        "path": str(path.resolve()),
        "exists": False,
        "valid": False,
        "read_consistent": False,
    }
    payload: dict[str, Any] | None = None
    try:
        with path.open("rb") as handle:
            stat_before = os.fstat(handle.fileno())
            handle_snapshot_before = _host_state_file_handle_snapshot(handle)
            payload_bytes = handle.read()
            stat_after = os.fstat(handle.fileno())
            handle_snapshot_after = _host_state_file_handle_snapshot(handle)
            publication_boundary = _host_state_publication_boundary_evidence(
                handle_snapshot_before,
                handle_snapshot_after,
                int(stat_after.st_mtime_ns),
            )
        path_after = path.stat()
        stable_fields = ("st_dev", "st_ino", "st_size", "st_mtime_ns")
        changed = [
            field
            for field in stable_fields
            if getattr(stat_before, field, None) != getattr(stat_after, field, None)
            or getattr(stat_after, field, None) != getattr(path_after, field, None)
        ]
        if changed:
            raise ComparatorError(
                f"named session state changed while reading {path}: {changed}"
            )
        record["publication_boundary"] = publication_boundary
        if publication_boundary.get("exact") is not True:
            raise ComparatorError(
                "named session state has no exact publication boundary: "
                f"{publication_boundary.get('error', 'unknown error')}"
            )
        decoded = json.loads(payload_bytes.decode("utf-8-sig"))
        if not isinstance(decoded, dict):
            raise ComparatorError("named session state is not a JSON object")
        payload = decoded
        state = {
            key: payload.get(key)
            for key in ("capture", "current_eid", "opened_at", "host", "port", "pid")
        }
        try:
            pid = int(state.get("pid", 0))
            port = int(state.get("port", 0))
        except (TypeError, ValueError):
            pid = 0
            port = 0
        host = str(state.get("host", "") or "")
        token = payload.get("token")
        token_valid = isinstance(token, str) and bool(token)
        fingerprint = {
            "device": int(getattr(stat_after, "st_dev", 0)),
            "inode": int(getattr(stat_after, "st_ino", 0)),
            "size_bytes": int(stat_after.st_size),
            "modified_ns": int(stat_after.st_mtime_ns),
            "sha256": hashlib.sha256(payload_bytes).hexdigest(),
        }
        record.update(
            {
                "exists": True,
                "size_bytes": int(stat_after.st_size),
                "modified_ns": int(stat_after.st_mtime_ns),
                "read_consistent": True,
                "fingerprint": fingerprint,
                "state": state,
                "token_present": token_valid,
                "token_sha256": (
                    hashlib.sha256(token.encode("utf-8")).hexdigest()
                    if token_valid
                    else None
                ),
                "valid": (
                    pid > 0
                    and host == "127.0.0.1"
                    and 0 < port <= 65535
                    and token_valid
                ),
            }
        )
    except FileNotFoundError:
        return record, None
    except Exception as exc:
        record["error"] = f"{type(exc).__name__}: {exc}"
    return record, payload


def _host_session_state_record(path: Path) -> dict[str, Any]:
    record, _ = _read_host_session_state(path)
    return record


def _host_state_fingerprint(record: dict[str, Any]) -> dict[str, Any] | None:
    fingerprint = record.get("fingerprint") if isinstance(record, dict) else None
    if not isinstance(fingerprint, dict):
        return None
    required = ("device", "inode", "size_bytes", "modified_ns", "sha256")
    if any(key not in fingerprint for key in required):
        return None
    return {key: fingerprint[key] for key in required}


def _load_exact_host_shutdown_credentials(
    path: Path,
    *,
    expected_record: dict[str, Any],
) -> dict[str, Any]:
    observed, payload = _read_host_session_state(path)
    expected_fingerprint = _host_state_fingerprint(expected_record)
    observed_fingerprint = _host_state_fingerprint(observed)
    if (
        expected_record.get("exists") is not True
        or expected_record.get("valid") is not True
        or expected_record.get("read_consistent") is not True
        or expected_fingerprint is None
    ):
        raise ComparatorError("expected named-session state evidence is incomplete")
    if (
        observed.get("exists") is not True
        or observed.get("valid") is not True
        or observed.get("read_consistent") is not True
        or observed_fingerprint != expected_fingerprint
        or not isinstance(payload, dict)
    ):
        raise ComparatorError(
            "named-session state identity changed before direct token shutdown"
        )
    token = payload.get("token")
    state = observed.get("state")
    if not isinstance(token, str) or not token or not isinstance(state, dict):
        raise ComparatorError("named-session shutdown token/state is unavailable")
    return {
        "host": str(state["host"]),
        "port": int(state["port"]),
        "pid": int(state["pid"]),
        "capture": state.get("capture"),
        "token": token,
        "token_sha256": observed.get("token_sha256"),
        "fingerprint": observed_fingerprint,
    }


def _remove_exact_host_session_state(
    path: Path,
    *,
    expected_record: dict[str, Any],
) -> dict[str, Any]:
    result: dict[str, Any] = {
        "path": str(path.resolve()),
        "removed": False,
        "already_absent": False,
        "passed": False,
    }
    observed = _host_session_state_record(path)
    if observed.get("exists") is not True:
        result.update({"already_absent": True, "passed": True})
        return result
    if _host_state_fingerprint(observed) != _host_state_fingerprint(expected_record):
        result["error"] = (
            "named-session state identity changed; refusing to remove a replacement file"
        )
        return result
    try:
        path.unlink()
    except Exception as exc:
        result["error"] = f"exact state-file removal failed: {type(exc).__name__}: {exc}"
        return result
    after = _host_session_state_record(path)
    result["after"] = after
    result["removed"] = after.get("exists") is False
    result["passed"] = result["removed"]
    if not result["passed"]:
        result["error"] = "exact named-session state file still exists after unlink"
    return result

def _wait_for_host_session_state(
    path: Path,
    *,
    timeout: float,
) -> dict[str, Any]:
    if not math.isfinite(timeout) or timeout <= 0.0:
        raise ComparatorError(f"invalid session-state wait timeout {timeout!r}")
    deadline = time.monotonic() + timeout
    last = _host_session_state_record(path)
    while time.monotonic() < deadline:
        last = _host_session_state_record(path)
        if last.get("exists") is True and last.get("valid") is True:
            return last
        time.sleep(0.02)
    raise ComparatorError(
        f"named replay session state did not become valid: {path}; last={last!r}"
    )


def _host_daemon_capture_from_command(command: list[str]) -> str | None:
    for index, token in enumerate(command):
        if token == "--capture" and index + 1 < len(command):
            return command[index + 1]
        if token.startswith("--capture="):
            return token.split("=", 1)[1]
    return None


def _host_collect_daemon_metadata(
    process_identity: Any,
) -> dict[str, Any]:
    try:
        import psutil
    except ImportError as exc:
        raise ComparatorError(
            "psutil is required to bind replay daemon command/capture metadata"
        ) from exc
    try:
        process = psutil.Process(int(process_identity.pid))
        create_time_before = float(process.create_time())
        image_path = str(process.exe())
        command = [str(token) for token in process.cmdline()]
        process_name = str(process.name() or "")
        create_time_after = float(process.create_time())
    except psutil.AccessDenied as exc:
        raise ComparatorError(
            f"access denied while binding replay daemon pid {process_identity.pid}"
        ) from exc
    except (psutil.NoSuchProcess, psutil.ZombieProcess) as exc:
        raise ComparatorError(
            f"replay daemon pid {process_identity.pid} exited before ownership binding"
        ) from exc
    if create_time_before != create_time_after:
        raise ComparatorError(
            f"replay daemon pid {process_identity.pid} creation time changed while reading metadata"
        )
    if not process_identity.creation_time_matches(create_time_before):
        raise ComparatorError(
            "replay daemon psutil creation time does not match the held native handle"
        )
    if not _host_process_images_match(process_identity.image_path, image_path):
        raise ComparatorError(
            "replay daemon psutil image does not match the held native handle image"
        )
    if process_identity.is_running() is not True:
        raise ComparatorError("replay daemon exited while ownership was being established")
    stable_metadata = process_identity.metadata()
    return {
        "pid": int(process_identity.pid),
        "identity": stable_metadata.get("identity"),
        "creation_time_key": stable_metadata.get("creation_time_key"),
        "creation_filetime_ticks": stable_metadata.get("creation_filetime_ticks"),
        "creation_time_unix_ns": stable_metadata.get("creation_time_unix_ns"),
        "create_time": create_time_before,
        "image_path": image_path,
        "process_name": process_name,
        "command": command,
        "is_rdc_daemon": any("rdc.daemon_server" in token for token in command),
        "capture": _host_daemon_capture_from_command(command),
        "native_handle_verified_during_metadata_collection": True,
    }


def _validate_owned_replay_daemon_binding(
    *,
    state_record: dict[str, Any],
    expected_state_path: Path,
    capture: Path,
    stable_process_identity: dict[str, Any] | None,
    process_metadata: dict[str, Any] | None,
    open_started_wall_ns: int,
) -> dict[str, Any]:
    result: dict[str, Any] = {
        "established": False,
        "basis": (
            "new exact named-session state plus positive PID, one held native handle, "
            "exact creation FILETIME within this open and strictly earlier than the exact state "
            "publication FILETIME with zero positive tolerance, plus matching image/cmdline"
        ),
        "state_path_match": False,
        "state_capture_path_match": False,
        "daemon_capture_path_metadata_match": False,
        "stable_image_match": False,
        "creation_time_matches_handle": False,
        "command_bound_to_held_identity": False,
        "state_publication_boundary_exact": False,
        "state_file_volume_verified": False,
        "exact_creation_order_clock_match": False,
        "strict_creation_precedes_publication": False,
        "creation_equals_publication": False,
        "process_created_for_open": False,
        "state_pid_matches_handle": False,
        "snapshot_evidence_complete": False,
        "process_created_before_state_file": False,
        "stable_process_identity": stable_process_identity,
        "process_metadata": process_metadata,
        "errors": [],
    }
    errors = result["errors"]
    if state_record.get("exists") is not True or state_record.get("valid") is not True:
        errors.append("named session state file is missing or invalid after open")
        return result
    if state_record.get("read_consistent") is not True or state_record.get("error"):
        errors.append("named session state file was not read consistently")
        return result
    try:
        result["state_path_match"] = _path_key(
            Path(str(state_record.get("path", "")))
        ) == _path_key(expected_state_path)
    except Exception as exc:
        errors.append(f"named session state path is invalid: {exc}")
        return result
    if result["state_path_match"] is not True:
        errors.append("named session state path does not match the allocated path")
    state = state_record.get("state")
    if not isinstance(state, dict):
        errors.append("named session state has no parsed object")
        return result
    try:
        state_pid = int(state.get("pid", 0))
    except (TypeError, ValueError):
        state_pid = 0
    if state_pid <= 0:
        errors.append("named session state PID is not positive")
    state_capture = state.get("capture")
    if not isinstance(state_capture, str) or not state_capture.strip():
        errors.append("named session state capture path is missing")
    else:
        try:
            result["state_capture_path_match"] = _path_key(
                Path(state_capture)
            ) == _path_key(capture)
        except Exception as exc:
            errors.append(f"named session capture path is invalid: {exc}")
        if result["state_capture_path_match"] is not True:
            errors.append("named session state capture path does not match the replay capture")
    if not isinstance(stable_process_identity, dict):
        errors.append("held stable replay process identity is missing")
        return result
    if not isinstance(process_metadata, dict):
        errors.append("replay daemon process metadata is missing")
        return result
    result["snapshot_evidence_complete"] = True
    result["state_pid_matches_handle"] = (
        state_pid > 0 and state_pid == int(stable_process_identity.get("pid", 0))
    )
    if result["state_pid_matches_handle"] is not True:
        errors.append("state PID does not match the held replay process handle PID")
    if stable_process_identity.get("native_handle_held") is not True:
        errors.append("replay process native handle was not held")
    if stable_process_identity.get("terminate_access") is not True:
        errors.append("replay process handle lacks terminate access")
    if not stable_process_identity.get("creation_time_key"):
        errors.append("replay process handle has no creation-time identity")
    result["stable_image_match"] = _host_process_images_match(
        str(stable_process_identity.get("image_path", "")),
        str(process_metadata.get("image_path", "")),
    )
    if result["stable_image_match"] is not True:
        errors.append("replay process image metadata differs from the held handle")
    creation_ticks = stable_process_identity.get("creation_filetime_ticks")
    creation_unix_ns = stable_process_identity.get("creation_time_unix_ns")
    publication_boundary = state_record.get("publication_boundary")
    publication_volume = (
        publication_boundary.get("volume")
        if isinstance(publication_boundary, dict)
        else None
    )
    expected_creation_unix_ns: int | None = None
    if (
        isinstance(creation_ticks, bool)
        or not isinstance(creation_ticks, int)
        or creation_ticks <= WINDOWS_FILETIME_EPOCH_OFFSET_TICKS
        or stable_process_identity.get("creation_time_key")
        != f"winfiletime:{creation_ticks}"
    ):
        errors.append("replay process has no exact Windows creation FILETIME identity")
    else:
        expected_creation_unix_ns = (
            creation_ticks - WINDOWS_FILETIME_EPOCH_OFFSET_TICKS
        ) * WINDOWS_FILETIME_TICK_NS
    result["creation_time_matches_handle"] = (
        expected_creation_unix_ns is not None
        and creation_unix_ns == expected_creation_unix_ns
        and process_metadata.get("identity")
        == stable_process_identity.get("identity")
        and process_metadata.get("creation_time_key")
        == stable_process_identity.get("creation_time_key")
        and process_metadata.get("creation_filetime_ticks") == creation_ticks
        and process_metadata.get("creation_time_unix_ns") == expected_creation_unix_ns
        and process_metadata.get("native_handle_verified_during_metadata_collection")
        is True
    )
    if result["creation_time_matches_handle"] is not True:
        errors.append("replay process metadata is not exactly bound to the held handle")
    if (
        not isinstance(publication_boundary, dict)
        or publication_boundary.get("schema") != STATE_PUBLICATION_BOUNDARY_SCHEMA
        or publication_boundary.get("exact") is not True
        or publication_boundary.get("clock") != "windows-filetime-100ns"
        or publication_boundary.get("resolution_ns") != WINDOWS_FILETIME_TICK_NS
        or publication_boundary.get("source")
        != (
            "GetFileTime before/after read on the same held state-file handle, "
            "with GUID final-path and handle-derived volume proof"
        )
        or publication_boundary.get("native_filetime_read_before_after") is not True
        or publication_boundary.get("state_file_handle_held_during_read") is not True
        or publication_boundary.get("same_handle_path_identity") is not True
        or publication_boundary.get("raw_filetime_stable") is not True
        or publication_boundary.get("path_fallback_used") is not False
        or publication_boundary.get("divisibility_used_as_granularity_proof") is not False
        or publication_boundary.get("timestamp_equality_accepted") is not False
        or publication_boundary.get("ordering_rule")
        != "process_creation_filetime_ticks < state_publication_filetime_ticks"
        or publication_boundary.get("raw_last_write_filetime_ticks_before")
        != publication_boundary.get("filetime_ticks")
        or publication_boundary.get("raw_last_write_filetime_ticks_after")
        != publication_boundary.get("filetime_ticks")
        or publication_boundary.get("stat_mtime_matches_native_filetime") is not True
        or not isinstance(publication_volume, dict)
        or publication_volume.get("schema") != STATE_VOLUME_EVIDENCE_SCHEMA
        or publication_volume.get("verified") is not True
        or publication_volume.get("same_handle_path_identity") is not True
        or publication_volume.get("raw_filetime_stable") is not True
        or publication_volume.get("path_fallback_used") is not False
        or publication_volume.get("original_path_used") is not False
        or publication_volume.get("local") is not True
        or publication_volume.get("supported_file_system") is not True
        or publication_volume.get("native_filetime_semantics") is not True
        or publication_volume.get("drive_type_code") != WINDOWS_DRIVE_FIXED
        or publication_volume.get("file_system") not in SUPPORTED_STATE_FILE_SYSTEMS
        or publication_boundary.get("positive_post_state_tolerance_ticks") != 0
        or publication_boundary.get("modified_ns") != state_record.get("modified_ns")
    ):
        errors.append(
            "named-session state publication boundary is missing, inexact, or mismatched"
        )
        publication_ticks = None
    else:
        publication_ticks = publication_boundary.get("filetime_ticks")
        if (
            isinstance(publication_ticks, bool)
            or not isinstance(publication_ticks, int)
            or publication_ticks <= WINDOWS_FILETIME_EPOCH_OFFSET_TICKS
        ):
            errors.append("named-session state publication FILETIME is invalid")
            publication_ticks = None
        else:
            result["state_publication_boundary"] = publication_boundary
            result["state_publication_boundary_exact"] = True
            result["state_file_volume_verified"] = True
            result["exact_creation_order_clock_match"] = True
    valid_open_boundary = (
        isinstance(open_started_wall_ns, int)
        and not isinstance(open_started_wall_ns, bool)
        and open_started_wall_ns > 0
    )
    if not valid_open_boundary:
        errors.append("rdc open start has no exact integer wall-clock boundary")
    result["creation_equals_publication"] = (
        isinstance(creation_ticks, int)
        and not isinstance(creation_ticks, bool)
        and isinstance(publication_ticks, int)
        and creation_ticks == publication_ticks
    )
    result["strict_creation_precedes_publication"] = (
        isinstance(creation_ticks, int)
        and not isinstance(creation_ticks, bool)
        and isinstance(publication_ticks, int)
        and creation_ticks < publication_ticks
    )
    result["process_created_before_state_file"] = result[
        "strict_creation_precedes_publication"
    ]
    result["process_created_for_open"] = (
        result["process_created_before_state_file"] is True
        and isinstance(expected_creation_unix_ns, int)
        and valid_open_boundary
        and expected_creation_unix_ns >= open_started_wall_ns
    )
    if result["creation_equals_publication"] is True:
        errors.append(
            "held process creation FILETIME equals the exact named-session publication "
            "FILETIME; equality is ambiguous and grants no ownership"
        )
    elif result["process_created_before_state_file"] is not True:
        errors.append(
            "held process was created after the exact named-session publication boundary; "
            "state PID was reused before stable acquisition"
        )
    if result["process_created_for_open"] is not True:
        errors.append(
            "state PID creation FILETIME is not within this exact rdc open interval"
        )
    command = process_metadata.get("command")
    result["command_bound_to_held_identity"] = (
        result["creation_time_matches_handle"] is True
        and isinstance(command, list)
        and any("rdc.daemon_server" in str(token) for token in command)
    )
    if result["command_bound_to_held_identity"] is not True:
        errors.append("held process command line is not bound to rdc.daemon_server")
    if process_metadata.get("is_rdc_daemon") is not True:
        errors.append("held process command is not rdc.daemon_server")
    daemon_capture = process_metadata.get("capture")
    if not isinstance(daemon_capture, str) or not daemon_capture.strip():
        errors.append("rdc daemon command has no capture path")
    else:
        try:
            result["daemon_capture_path_metadata_match"] = _path_key(
                Path(daemon_capture)
            ) == _path_key(capture)
        except Exception as exc:
            errors.append(f"rdc daemon capture path is invalid: {exc}")
        if result["daemon_capture_path_metadata_match"] is not True:
            errors.append("rdc daemon command capture path does not match replay capture")
    result["established"] = not errors
    return result


def _acquire_owned_replay_daemon(
    *,
    state_record: dict[str, Any],
    expected_state_path: Path,
    capture: Path,
    open_started_wall_ns: int,
    process_identity_factory: Any = _StableReplayProcessIdentity.acquire,
    process_metadata_collector: Any = _host_collect_daemon_metadata,
) -> tuple[Any, dict[str, Any]]:
    state = state_record.get("state")
    try:
        pid = int(state.get("pid", 0)) if isinstance(state, dict) else 0
    except (TypeError, ValueError):
        pid = 0
    if pid <= 0:
        raise ComparatorError("cannot acquire replay daemon ownership without a positive state PID")
    process_identity: Any | None = None
    try:
        process_identity = process_identity_factory(
            pid,
            require_terminate=True,
        )
        stable_metadata = process_identity.metadata()
        process_metadata = process_metadata_collector(process_identity)
        ownership = _validate_owned_replay_daemon_binding(
            state_record=state_record,
            expected_state_path=expected_state_path,
            capture=capture,
            stable_process_identity=stable_metadata,
            process_metadata=process_metadata,
            open_started_wall_ns=open_started_wall_ns,
        )
        if ownership.get("established") is not True or ownership.get("errors"):
            raise ComparatorError(
                "replay daemon ownership was not established: "
                + "; ".join(str(error) for error in ownership.get("errors", []))
            )
        return process_identity, ownership
    except Exception:
        if process_identity is not None:
            process_identity.close()
        raise


def _wait_for_owned_replay_process_exit(
    process_identity: Any,
    *,
    timeout: float,
) -> dict[str, Any]:
    result: dict[str, Any] = {
        "same_native_handle": True,
        "exited": False,
        "passed": False,
    }
    deadline = time.monotonic() + max(0.0, timeout)
    while True:
        try:
            if process_identity.is_running() is False:
                result.update({"exited": True, "running_after": False, "passed": True})
                return result
        except Exception as exc:
            result["error"] = f"held-handle liveness check failed: {type(exc).__name__}: {exc}"
            return result
        remaining = deadline - time.monotonic()
        if remaining <= 0.0:
            result.update(
                {
                    "timed_out": True,
                    "running_after": True,
                    "error": "timed out waiting on the held replay process handle",
                }
            )
            return result
        time.sleep(min(0.02, remaining))


def _shutdown_owned_replay_session_direct(
    *,
    state_path: Path,
    state_after_open: dict[str, Any],
    process_identity: Any,
    ownership: dict[str, Any],
    capture: Path,
    timeout: float,
    send_request_fn: Any = None,
    shutdown_request_fn: Any = None,
) -> dict[str, Any]:
    result: dict[str, Any] = {
        "schema": "mgif-rdc-direct-token-shutdown-v1",
        "transport": "rdc.daemon_client.send_request",
        "request": "rdc.protocol.shutdown_request",
        "subprocess_used": False,
        "pid_only_fallback": False,
        "port_scan_fallback": False,
        "tree_cleanup_requested": False,
        "graceful_requested": False,
        "graceful_accepted": False,
        "owned_daemon_absent": False,
        "state_file_absent": False,
        "passed": False,
        "errors": [],
    }
    errors: list[str] = result["errors"]
    if not math.isfinite(timeout) or timeout <= 0.0:
        errors.append(f"invalid direct shutdown timeout: {timeout!r}")
        return result
    if ownership.get("established") is not True or ownership.get("errors"):
        errors.append("replay daemon ownership is not established without errors")
        return result
    try:
        metadata = process_identity.metadata()
    except Exception as exc:
        errors.append(f"stable replay daemon metadata failed: {type(exc).__name__}: {exc}")
        return result
    result["stable_process_identity"] = metadata
    state = state_after_open.get("state")
    ownership_stable = ownership.get("stable_process_identity")
    if (
        metadata.get("native_handle_held") is not True
        or metadata.get("terminate_access") is not True
        or not isinstance(state, dict)
        or int(state.get("pid", 0) or 0) != int(metadata.get("pid", 0) or 0)
        or not isinstance(ownership_stable, dict)
        or str(ownership_stable.get("identity", ""))
        != str(metadata.get("identity", ""))
    ):
        errors.append(
            "direct shutdown lacks one exact owned replay-daemon handle/state identity binding"
        )
        return result
    try:
        credentials = _load_exact_host_shutdown_credentials(
            state_path,
            expected_record=state_after_open,
        )
    except Exception as exc:
        errors.append(f"shutdown credential binding failed: {type(exc).__name__}: {exc}")
        return result
    if credentials["pid"] != int(metadata["pid"]):
        errors.append("shutdown credential PID differs from the held replay-daemon handle")
        return result
    try:
        capture_matches = _path_key(Path(str(credentials["capture"]))) == _path_key(capture)
    except Exception:
        capture_matches = False
    if not capture_matches:
        errors.append("shutdown credential capture differs from the owned replay capture")
        return result
    result["endpoint"] = {
        "host": credentials["host"],
        "port": credentials["port"],
        "token_sha256": credentials["token_sha256"],
        "state_fingerprint": credentials["fingerprint"],
    }
    if send_request_fn is None or shutdown_request_fn is None:
        try:
            from rdc.daemon_client import send_request
            from rdc.protocol import shutdown_request
        except Exception as exc:
            errors.append(
                f"direct rdc shutdown API import failed: {type(exc).__name__}: {exc}"
            )
            return result
        send_request_fn = send_request
        shutdown_request_fn = shutdown_request

    deadline = time.monotonic() + timeout
    rpc_budget = min(5.0, max(0.05, timeout * 0.40))
    result["graceful_requested"] = True
    graceful_error: str | None = None
    try:
        response = send_request_fn(
            credentials["host"],
            credentials["port"],
            shutdown_request_fn(credentials["token"], request_id=92),
            timeout=rpc_budget,
        )
        result["response"] = {
            "has_error": isinstance(response, dict) and "error" in response,
            "ok": (
                isinstance(response, dict)
                and isinstance(response.get("result"), dict)
                and response["result"].get("ok") is True
            ),
        }
        result["graceful_accepted"] = result["response"]["ok"] is True
        if result["graceful_accepted"] is not True:
            graceful_error = "direct shutdown response did not contain result.ok=true"
    except Exception as exc:
        graceful_error = f"direct shutdown request failed: {type(exc).__name__}: {exc}"
        result["graceful_error"] = graceful_error

    remaining = max(0.0, deadline - time.monotonic())
    graceful_wait = _wait_for_owned_replay_process_exit(
        process_identity,
        timeout=min(max(0.01, remaining), max(0.05, timeout * 0.30)),
    )
    result["graceful_wait"] = graceful_wait
    recovery: dict[str, Any] = {
        "attempted": False,
        "same_native_handle": True,
        "pid_only_fallback": False,
        "tree_cleanup_requested": False,
        "passed": graceful_wait.get("passed") is True,
    }
    if graceful_wait.get("passed") is not True:
        recovery["attempted"] = True
        try:
            termination = process_identity.terminate(
                timeout=max(0.05, deadline - time.monotonic())
            )
        except Exception as exc:
            termination = {
                "passed": False,
                "same_native_handle": True,
                "error": f"{type(exc).__name__}: {exc}",
            }
        recovery["termination"] = termination
        recovery["passed"] = (
            termination.get("passed") is True
            and termination.get("same_native_handle") is True
            and termination.get("running_after") is False
            and termination.get("tree_cleanup_requested") is not True
        )
        if recovery["passed"] is not True:
            errors.append(
                "owned replay daemon did not exit through direct token shutdown or its held handle"
            )
    result["same_handle_recovery"] = recovery
    try:
        result["owned_daemon_absent"] = process_identity.is_running() is False
    except Exception as exc:
        errors.append(f"final held-handle liveness check failed: {type(exc).__name__}: {exc}")
    if result["owned_daemon_absent"] is not True:
        errors.append("owned replay daemon remains present on its held process handle")
    if result["owned_daemon_absent"] is True:
        state_removal = _remove_exact_host_session_state(
            state_path,
            expected_record=state_after_open,
        )
    else:
        state_removal = {
            "path": str(state_path.resolve()),
            "removed": False,
            "passed": False,
            "error": "daemon is not absent; refusing to remove its session state",
        }
    result["state_file_cleanup"] = state_removal
    final_state = _host_session_state_record(state_path)
    result["final_state"] = final_state
    result["state_file_absent"] = final_state.get("exists") is False
    if state_removal.get("passed") is not True or result["state_file_absent"] is not True:
        errors.append("exact named-session state file absence was not proven")
    result["post_status"] = {
        "classification": (
            "inactive"
            if result["owned_daemon_absent"] is True
            and result["state_file_absent"] is True
            else "error"
        ),
        "inactive": (
            result["owned_daemon_absent"] is True
            and result["state_file_absent"] is True
        ),
        "basis": "held replay-daemon handle absent plus exact state file absent",
        "subprocess_used": False,
    }
    if graceful_error is not None and recovery.get("passed") is not True:
        errors.append(graceful_error)
    result["passed"] = not errors and result["post_status"]["inactive"] is True
    return result

def _strict_path_exists(path: Path, *, label: str) -> bool:
    try:
        path.stat()
        return True
    except FileNotFoundError:
        return False
    except OSError as exc:
        raise ComparatorError(
            f"cannot inspect {label} {path}: {type(exc).__name__}: {exc}"
        ) from exc


def _require_single_rdc_size_budget(size_bytes: int, *, label: str) -> int:
    size = int(size_bytes)
    if size <= 0:
        raise ComparatorError(f"RDC is empty or invalid: {label}, size={size}")
    if size > COMPARATOR_MAX_RDC_BYTES:
        raise ComparatorError(
            f"RDC exceeds the comparator single-file cap: size={size}, "
            f"cap={COMPARATOR_MAX_RDC_BYTES}, path={label}"
        )
    return size

class ComparatorRuntimeBudget:
    """Hard RDC size/count and live free-space budget for extraction."""

    def __init__(
        self,
        work_root: Path,
        *,
        capture_count: int,
        disk_preflight: dict[str, Any],
        disk_usage_fn: Any = shutil.disk_usage,
    ) -> None:
        if capture_count <= 0:
            raise ComparatorError("runtime extraction budget capture_count must be positive")
        estimate = disk_preflight.get("estimate")
        if not isinstance(estimate, dict) or disk_preflight.get("passed") is not True:
            raise ComparatorError("runtime extraction budget requires a passed preflight")
        self._work_root = work_root.resolve()
        self._capture_count = int(capture_count)
        self._started_count = 0
        self._disk_usage_fn = disk_usage_fn
        self._safety_margin_bytes = int(estimate.get("safety_margin_bytes", 0))
        if self._safety_margin_bytes <= 0:
            raise ComparatorError("runtime extraction budget has no safety margin")
        self._records: list[dict[str, Any]] = []

    def before_capture(self, capture: Path) -> dict[str, Any]:
        resolved = capture.resolve(strict=True)
        size_bytes = _require_single_rdc_size_budget(
            int(resolved.stat().st_size),
            label=str(resolved),
        )
        if self._started_count >= self._capture_count:
            raise ComparatorError(
                f"comparator extraction count budget exhausted: "
                f"max={self._capture_count}"
            )
        remaining_slots = self._capture_count - self._started_count
        required_free_bytes = (
            remaining_slots
            * (
                COMPARATOR_EXTRACT_BYTES_PER_CAPTURE
                + COMPARATOR_SESSION_BYTES_PER_CAPTURE
            )
            + self._safety_margin_bytes
        )
        try:
            free_bytes = int(self._disk_usage_fn(self._work_root).free)
        except Exception as exc:
            raise ComparatorError(
                f"cannot recheck comparator free space before opening {resolved}: "
                f"{type(exc).__name__}: {exc}"
            ) from exc
        if free_bytes < required_free_bytes:
            raise ComparatorError(
                f"insufficient free space before opening RDC {resolved}: "
                f"free={free_bytes}, required={required_free_bytes}"
            )
        self._started_count += 1
        record = {
            "capture": str(resolved),
            "capture_size_bytes": size_bytes,
            "single_rdc_cap_bytes": COMPARATOR_MAX_RDC_BYTES,
            "extraction_index": self._started_count,
            "capture_count_budget": self._capture_count,
            "free_bytes_before_open": free_bytes,
            "required_free_bytes_before_open": required_free_bytes,
            "passed": True,
        }
        self._records.append(record)
        return dict(record)

    def snapshot(self) -> dict[str, Any]:
        return {
            "capture_count_budget": self._capture_count,
            "started_capture_count": self._started_count,
            "single_rdc_cap_bytes": COMPARATOR_MAX_RDC_BYTES,
            "single_readback_cap_bytes": COMPARATOR_MAX_READBACK_BYTES,
            "npy_bytes_per_capture_budget": COMPARATOR_EXTRACT_BYTES_PER_CAPTURE,
            "safety_margin_bytes": self._safety_margin_bytes,
            "within_capture_count_budget": self._started_count <= self._capture_count,
            "records": [dict(record) for record in self._records],
        }


def _session_cleanup_terminal_result(
    *,
    direct_shutdown_passed: bool,
    owned_daemon_absent: bool,
    state_absent: bool,
    handle_closed: bool,
    errors: list[str],
    deadline_exceeded: bool,
) -> dict[str, Any]:
    closed = bool(
        direct_shutdown_passed
        and owned_daemon_absent
        and state_absent
        and handle_closed
        and not errors
    )
    return {
        "closed": closed,
        "passed": bool(closed and not deadline_exceeded),
        "direct_shutdown_passed": bool(direct_shutdown_passed),
        "owned_daemon_absent": bool(owned_daemon_absent),
        "state_absent": bool(state_absent),
        "handle_closed": bool(handle_closed),
        "deadline_exceeded": bool(deadline_exceeded),
        "errors": list(errors),
    }

def _extract_capture(
    rdc_executable: str,
    capture: Path,
    output_dir: Path,
    *,
    timeout: float,
    tag: str,
    release_gate: bool,
    release_render_mode: str | None,
    overall_deadline: float,
    runtime_budget: ComparatorRuntimeBudget,
) -> dict[str, Any]:
    capture = capture.resolve(strict=True)
    if release_gate and release_render_mode not in RELEASE_RENDER_MODES:
        raise ComparatorError(
            f"release extraction requires one canonical render mode, got "
            f"{release_render_mode!r}"
        )
    runtime_budget_record = runtime_budget.before_capture(capture)
    session = f"csmcmp_{os.getpid()}_{tag}_{uuid.uuid4().hex[:8]}"
    base = [rdc_executable, "--session", session]
    state_path = _host_rdc_session_state_path(session)
    if _strict_path_exists(
        state_path,
        label="pre-open RenderDoc session state file",
    ):
        raise ComparatorError(
            f"allocated RenderDoc session state already exists: {state_path}"
        )
    manifest: dict[str, Any] | None = None
    open_attempted = False
    open_started_wall_ns = 0
    state_after_open: dict[str, Any] | None = None
    daemon_process_identity: Any | None = None
    daemon_ownership: dict[str, Any] = {
        "established": False,
        "errors": ["replay daemon ownership was not established"],
    }
    cleanup: dict[str, Any] = {
        "schema": "mgif-rdc-comparator-replay-session-cleanup-v4",
        "session": session,
        "attempted": False,
        "closed": False,
        "passed": False,
        "close": None,
        "post_status": None,
        "daemon_ownership": daemon_ownership,
        "owned_daemon_absent": False,
        "owned_daemon_residue": [],
        "stable_daemon_process_identity": None,
        "daemon_process_handle_close": None,
        "verified_daemon_recovery": {
            "attempted": False,
            "passed": False,
            "same_native_handle": True,
            "pid_only_fallback": False,
        },
        "state_file": {
            "path": str(state_path.resolve()),
            "exists_before": False,
            "after_open": None,
            "after_close_attempt": None,
            "after_cleanup": None,
            "exists_after_cleanup": None,
            "absent_after_cleanup": False,
        },
        "deadline": {
            "cleanup_reserve_seconds": CAPTURE_CLEANUP_RESERVE_SECONDS,
            "emergency_cleanup_timeout_seconds": (
                CAPTURE_EMERGENCY_CLEANUP_TIMEOUT_SECONDS
            ),
            "exceeded_before_cleanup": False,
            "exceeded": False,
            "emergency_cleanup_used": False,
        },
        "runtime_budget": runtime_budget_record,
        "diagnostics": [],
        "errors": [],
    }
    before_identity = _file_identity(capture) if release_gate else None
    try:
        open_timeout = _deadline_timeout(
            overall_deadline,
            timeout,
            stage=f"rdc open for {capture}",
            cleanup_reserve=CAPTURE_CLEANUP_RESERVE_SECONDS,
        )
        open_attempted = True
        open_started_wall_ns = time.time_ns()
        _run_command(
            base + ["open", str(capture)],
            timeout=open_timeout,
        )
        state_wait_timeout = _deadline_timeout(
            overall_deadline,
            min(10.0, timeout),
            stage=f"named session state for {capture}",
            cleanup_reserve=CAPTURE_CLEANUP_RESERVE_SECONDS,
        )
        state_after_open = _wait_for_host_session_state(
            state_path,
            timeout=state_wait_timeout,
        )
        cleanup["state_file"]["after_open"] = state_after_open
        daemon_process_identity, daemon_ownership = _acquire_owned_replay_daemon(
            state_record=state_after_open,
            expected_state_path=state_path,
            capture=capture,
            open_started_wall_ns=open_started_wall_ns,
        )
        cleanup["daemon_ownership"] = daemon_ownership
        cleanup["stable_daemon_process_identity"] = (
            daemon_process_identity.metadata()
        )
        script_timeout = _deadline_timeout(
            overall_deadline,
            timeout,
            stage=f"rdc script for {capture}",
            cleanup_reserve=CAPTURE_CLEANUP_RESERVE_SECONDS,
        )
        result = _run_command(
            base
            + [
                "script",
                str(Path(__file__).resolve()),
                "--arg",
                f"output={output_dir}",
                "--arg",
                f"release_gate={int(release_gate)}",
                "--arg",
                f"release_render_mode={release_render_mode or 'diagnostic'}",
                "--arg",
                f"export_budget_bytes={COMPARATOR_EXTRACT_BYTES_PER_CAPTURE}",
                "--arg",
                f"single_readback_cap_bytes={COMPARATOR_MAX_READBACK_BYTES}",
                "--arg",
                f"export_safety_margin_bytes={DISK_SAFETY_MIN_BYTES}",
                "--json",
            ],
            timeout=script_timeout,
        )
        try:
            envelope = json.loads(result.stdout)
        except json.JSONDecodeError as exc:
            raise ComparatorError(
                f"rdc script returned invalid JSON for {capture}: "
                f"{result.stdout[-4000:]}"
            ) from exc
        if envelope.get("stderr"):
            raise ComparatorError(
                f"rdc script error for {capture}: {envelope['stderr']}"
            )
        manifest_path = output_dir / "manifest.json"
        if not manifest_path.exists():
            raise ComparatorError(
                f"extractor did not create {manifest_path} for {capture}"
            )
        manifest = json.loads(manifest_path.read_text(encoding="utf-8"))
    finally:
        active_exception = sys.exc_info()[0] is not None
        cleanup["attempted"] = open_attempted
        cleanup["deadline"]["exceeded_before_cleanup"] = (
            time.monotonic() >= overall_deadline
        )

        def cleanup_timeout(stage: str) -> float:
            remaining = overall_deadline - time.monotonic()
            if remaining > 0.05:
                return min(60.0, remaining)
            cleanup["deadline"]["emergency_cleanup_used"] = True
            cleanup["deadline"]["exceeded"] = True
            cleanup["diagnostics"].append(
                f"outer deadline exhausted before {stage}; using bounded emergency cleanup"
            )
            return CAPTURE_EMERGENCY_CLEANUP_TIMEOUT_SECONDS

        terminal_errors: list[str] = []
        cleanup["close_subprocess_used"] = False
        cleanup["status_subprocess_used"] = False
        if open_attempted:
            if daemon_process_identity is None:
                candidate_state = state_after_open or _host_session_state_record(state_path)
                cleanup["state_file"]["after_open"] = candidate_state
                if candidate_state.get("exists") is True and candidate_state.get("valid") is True:
                    try:
                        daemon_process_identity, daemon_ownership = (
                            _acquire_owned_replay_daemon(
                                state_record=candidate_state,
                                expected_state_path=state_path,
                                capture=capture,
                                open_started_wall_ns=open_started_wall_ns,
                            )
                        )
                        state_after_open = candidate_state
                        cleanup["daemon_ownership"] = daemon_ownership
                        cleanup["stable_daemon_process_identity"] = (
                            daemon_process_identity.metadata()
                        )
                    except Exception as exc:
                        terminal_errors.append(
                            "replay daemon ownership acquisition failed closed during cleanup: "
                            f"{type(exc).__name__}: {exc}"
                        )
                else:
                    terminal_errors.append(
                        "replay daemon ownership cannot be established because the exact "
                        "named-session state PID/token is missing or invalid"
                    )

            if daemon_process_identity is None or state_after_open is None:
                direct_shutdown = {
                    "schema": "mgif-rdc-direct-token-shutdown-v1",
                    "subprocess_used": False,
                    "pid_only_fallback": False,
                    "port_scan_fallback": False,
                    "tree_cleanup_requested": False,
                    "passed": False,
                    "errors": [
                        "no held owned replay-daemon handle/state exists; refusing PID-only cleanup"
                    ],
                    "post_status": {
                        "classification": "error",
                        "inactive": False,
                        "subprocess_used": False,
                    },
                }
            else:
                try:
                    direct_shutdown = _shutdown_owned_replay_session_direct(
                        state_path=state_path,
                        state_after_open=state_after_open,
                        process_identity=daemon_process_identity,
                        ownership=daemon_ownership,
                        capture=capture,
                        timeout=cleanup_timeout("direct session-token shutdown"),
                    )
                except Exception as exc:
                    direct_shutdown = {
                        "schema": "mgif-rdc-direct-token-shutdown-v1",
                        "subprocess_used": False,
                        "pid_only_fallback": False,
                        "port_scan_fallback": False,
                        "tree_cleanup_requested": False,
                        "passed": False,
                        "errors": [
                            f"direct token shutdown raised: {type(exc).__name__}: {exc}"
                        ],
                        "post_status": {
                            "classification": "error",
                            "inactive": False,
                            "subprocess_used": False,
                        },
                    }
            cleanup["direct_shutdown"] = direct_shutdown
            cleanup["close"] = {
                "backend": "direct-session-token-protocol",
                "subprocess_used": False,
                "accepted": direct_shutdown.get("graceful_accepted") is True,
            }
            cleanup["close_accepted"] = direct_shutdown.get("graceful_accepted") is True
            cleanup["post_status"] = direct_shutdown.get(
                "post_status",
                {
                    "classification": "error",
                    "inactive": False,
                    "subprocess_used": False,
                },
            )
            cleanup["verified_daemon_recovery"] = direct_shutdown.get(
                "same_handle_recovery",
                {
                    "attempted": False,
                    "same_native_handle": True,
                    "pid_only_fallback": False,
                    "tree_cleanup_requested": False,
                    "passed": False,
                },
            )
            if direct_shutdown.get("passed") is not True:
                terminal_errors.extend(
                    f"direct token shutdown: {error}"
                    for error in direct_shutdown.get("errors", [])
                )
            state_after_close = _host_session_state_record(state_path)
            cleanup["state_file"]["after_close_attempt"] = state_after_close
            cleanup["state_file"]["after_cleanup"] = state_after_close
            cleanup["state_file"]["exists_after_cleanup"] = state_after_close.get(
                "exists"
            )
            cleanup["state_file"]["absent_after_cleanup"] = (
                state_after_close.get("exists") is False
            )
            cleanup["owned_daemon_absent"] = (
                direct_shutdown.get("owned_daemon_absent") is True
            )
            cleanup["owned_daemon_residue"] = (
                []
                if cleanup["owned_daemon_absent"]
                else [cleanup.get("stable_daemon_process_identity")]
            )
            if cleanup.get("daemon_ownership", {}).get("established") is not True:
                terminal_errors.append("replay daemon ownership was not established")
            if cleanup.get("daemon_ownership", {}).get("errors") not in ([], None):
                terminal_errors.append(
                    "replay daemon ownership contains errors: "
                    f"{cleanup['daemon_ownership'].get('errors')!r}"
                )
            if _rdc_status_classification(cleanup.get("post_status")) != "inactive":
                terminal_errors.append(
                    "direct shutdown did not prove held-handle/state-file inactivity"
                )
            if cleanup["state_file"]["absent_after_cleanup"] is not True:
                terminal_errors.append(
                    f"named replay session state file remains: {state_path}"
                )
            if cleanup["owned_daemon_absent"] is not True:
                terminal_errors.append(
                    "owned replay daemon is not proven absent through the held handle"
                )
        else:
            terminal_errors.append("rdc open was never attempted; no replay cleanup proof exists")
        handle_close: dict[str, Any]
        if daemon_process_identity is not None:
            try:
                handle_close = daemon_process_identity.close()
            except Exception as exc:
                handle_close = {
                    "closed": False,
                    "error": f"{type(exc).__name__}: {exc}",
                }
        else:
            handle_close = {
                "opened": False,
                "closed": False,
                "error": "no owned replay daemon handle was established",
            }
        cleanup["daemon_process_handle_close"] = handle_close
        if handle_close.get("closed") is not True:
            terminal_errors.append(
                f"owned replay daemon handle did not close: {handle_close!r}"
            )
        cleanup["deadline"]["exceeded"] = (
            cleanup["deadline"]["exceeded"]
            or time.monotonic() >= overall_deadline
        )
        cleanup["errors"] = terminal_errors
        cleanup["closed"] = bool(
            open_attempted
            and cleanup.get("daemon_ownership", {}).get("established") is True
            and cleanup.get("direct_shutdown", {}).get("passed") is True
            and cleanup.get("direct_shutdown", {}).get("subprocess_used") is False
            and cleanup.get("direct_shutdown", {}).get("pid_only_fallback") is False
            and cleanup.get("direct_shutdown", {}).get("port_scan_fallback") is False
            and cleanup.get("direct_shutdown", {}).get("tree_cleanup_requested") is False
            and cleanup.get("owned_daemon_absent") is True
            and _rdc_status_classification(cleanup.get("post_status")) == "inactive"
            and cleanup.get("state_file", {}).get("absent_after_cleanup") is True
            and handle_close.get("closed") is True
            and not terminal_errors
        )
        cleanup["passed"] = bool(
            cleanup["closed"]
            and cleanup["deadline"]["exceeded"] is not True
        )
        if terminal_errors and active_exception:
            print(
                f"warning: RenderDoc session cleanup failed for {session}: "
                + "; ".join(terminal_errors),
                file=sys.stderr,
            )
    if manifest is None:
        raise ComparatorError(f"no extraction manifest was produced for {capture}")
    if cleanup.get("passed") is not True:
        raise ComparatorError(
            f"failed to clean owned RenderDoc replay session {session}: "
            + "; ".join(str(error) for error in cleanup.get("errors", []))
        )
    if release_gate:
        assert before_identity is not None
        after_identity = _file_identity(capture)
        if before_identity != after_identity:
            raise ComparatorError(
                f"capture changed while being extracted: {capture}"
            )
        manifest["capture_file_identity"] = before_identity
        replay_capture = str(manifest.get("capture", "") or "").strip()
        replay_match: bool | None = None
        if replay_capture:
            replay_path = Path(replay_capture)
            if replay_path.is_absolute():
                replay_match = _path_key(replay_path) == _path_key(capture)
                if not replay_match:
                    raise ComparatorError(
                        f"RenderDoc replay opened {replay_capture}, expected {capture}"
                    )
            elif replay_path.name:
                replay_match = replay_path.name.lower() == capture.name.lower()
        manifest["capture_file_identity"]["renderdoc_reported_capture"] = (
            replay_capture or None
        )
        manifest["capture_file_identity"]["renderdoc_reported_path_match"] = (
            replay_match
        )
    manifest["session"] = session
    manifest["session_cleanup"] = cleanup
    manifest["runtime_disk_budget"] = runtime_budget_record
    return manifest
def _load_export(root: Path, export: dict[str, Any]) -> Any:
    import numpy as np

    path = root / export["path"]
    if export["kind"] == "npy":
        return np.load(path, allow_pickle=False).astype(np.float32, copy=False)
    if export["kind"] == "png":
        from PIL import Image

        image = np.asarray(Image.open(path).convert("RGBA"), dtype=np.float32)
        return image / 255.0
    raise ComparatorError(f"unknown export kind: {export['kind']}")


def _grayscale(array: Any) -> Any:
    import numpy as np

    if array.ndim == 2:
        return array.astype(np.float32, copy=False)
    channels = min(3, array.shape[-1])
    if channels == 1:
        return array[..., 0].astype(np.float32, copy=False)
    weights = np.array([0.2126, 0.7152, 0.0722], dtype=np.float32)[:channels]
    weights /= np.sum(weights)
    return np.sum(array[..., :channels] * weights, axis=-1, dtype=np.float32)


def _clear_value(depth: Any) -> float:
    import numpy as np

    finite = depth[np.isfinite(depth)]
    if finite.size == 0:
        return 1.0
    zero_count = int(np.count_nonzero(np.abs(finite) <= 1.0e-7))
    one_count = int(np.count_nonzero(np.abs(finite - 1.0) <= 1.0e-7))
    if zero_count or one_count:
        return 0.0 if zero_count > one_count else 1.0
    return float(np.median(finite))


def _overlap_slices(
    height: int,
    width: int,
    dx: int,
    dy: int,
) -> tuple[slice, slice, slice, slice] | None:
    before_x0 = max(0, -dx)
    before_x1 = min(width, width - dx)
    before_y0 = max(0, -dy)
    before_y1 = min(height, height - dy)
    after_x0 = before_x0 + dx
    after_x1 = before_x1 + dx
    after_y0 = before_y0 + dy
    after_y1 = before_y1 + dy
    if before_x1 <= before_x0 or before_y1 <= before_y0:
        return None
    return (
        slice(before_y0, before_y1),
        slice(before_x0, before_x1),
        slice(after_y0, after_y1),
        slice(after_x0, after_x1),
    )


def _registration_score(
    before: Any,
    after: Any,
    dx: int,
    dy: int,
    *,
    depth: bool,
    clear_before: float | None,
    clear_after: float | None,
    stride: int,
) -> float:
    import numpy as np

    slices = _overlap_slices(before.shape[0], before.shape[1], dx, dy)
    if slices is None:
        return float("inf")
    by, bx, ay, ax = slices
    left = before[by, bx][::stride, ::stride]
    right = after[ay, ax][::stride, ::stride]
    finite = np.isfinite(left) & np.isfinite(right)
    if not np.any(finite):
        return float("inf")
    if depth:
        assert clear_before is not None and clear_after is not None
        left_active = np.abs(left - clear_before) > 1.0e-7
        right_active = np.abs(right - clear_after) > 1.0e-7
        union = finite & (left_active | right_active)
        if not np.any(union):
            return float(np.mean(np.abs(left[finite] - right[finite])))
        mismatch = np.mean((left_active != right_active)[union])
        mae = np.mean(np.abs(left[union] - right[union]))
        return float(mismatch + mae)
    scale_values = np.concatenate(
        [np.abs(left[finite]).reshape(-1), np.abs(right[finite]).reshape(-1)]
    )
    scale = float(np.quantile(scale_values, 0.95)) if scale_values.size else 1.0
    scale = max(scale, 1.0e-6)
    return float(np.mean(np.abs(left[finite] - right[finite])) / scale)


def _find_integer_registration(
    before_array: Any,
    after_array: Any,
    *,
    max_shift: int,
    depth: bool,
) -> dict[str, Any]:
    import numpy as np

    before = _grayscale(before_array)
    after = _grayscale(after_array)
    if before.shape != after.shape:
        return {
            "available": False,
            "reason": f"shape mismatch {before.shape} vs {after.shape}",
            "dx": 0,
            "dy": 0,
        }
    height, width = before.shape
    clear_before = _clear_value(before) if depth else None
    clear_after = _clear_value(after) if depth else None
    downsample = max(1, int(math.ceil(max(height, width) / 512.0)))
    coarse_radius = int(math.ceil(max_shift / downsample))
    best = (float("inf"), 0, 0)
    for coarse_dy in range(-coarse_radius, coarse_radius + 1):
        for coarse_dx in range(-coarse_radius, coarse_radius + 1):
            dx = coarse_dx * downsample
            dy = coarse_dy * downsample
            if abs(dx) > max_shift or abs(dy) > max_shift:
                continue
            score = _registration_score(
                before,
                after,
                dx,
                dy,
                depth=depth,
                clear_before=clear_before,
                clear_after=clear_after,
                stride=downsample,
            )
            if (score, abs(dx) + abs(dy), dy, dx) < (
                best[0],
                abs(best[1]) + abs(best[2]),
                best[2],
                best[1],
            ):
                best = (score, dx, dy)
    coarse_dx, coarse_dy = best[1], best[2]
    refine_radius = max(1, downsample)
    for dy in range(
        max(-max_shift, coarse_dy - refine_radius),
        min(max_shift, coarse_dy + refine_radius) + 1,
    ):
        for dx in range(
            max(-max_shift, coarse_dx - refine_radius),
            min(max_shift, coarse_dx + refine_radius) + 1,
        ):
            score = _registration_score(
                before,
                after,
                dx,
                dy,
                depth=depth,
                clear_before=clear_before,
                clear_after=clear_after,
                stride=downsample,
            )
            if (score, abs(dx) + abs(dy), dy, dx) < (
                best[0],
                abs(best[1]) + abs(best[2]),
                best[2],
                best[1],
            ):
                best = (score, dx, dy)
    if not math.isfinite(best[0]):
        return {
            "available": False,
            "reason": "no finite pixels were available for registration",
            "dx": 0,
            "dy": 0,
        }
    return {
        "available": True,
        "dx": int(best[1]),
        "dy": int(best[2]),
        "score": float(best[0]),
        "search_radius": int(max_shift),
        "evaluation_stride": int(downsample),
        "clear_before": clear_before,
        "clear_after": clear_after,
    }


def _array_diff_metrics(
    before: Any,
    after: Any,
    *,
    dx: int,
    dy: int,
    epsilon: float,
    depth: bool,
) -> dict[str, Any]:
    import numpy as np

    if before.shape[:2] != after.shape[:2]:
        return {
            "available": False,
            "reason": f"shape mismatch {before.shape} vs {after.shape}",
        }
    slices = _overlap_slices(before.shape[0], before.shape[1], dx, dy)
    if slices is None:
        return {"available": False, "reason": "registration has no overlap"}
    by, bx, ay, ax = slices
    left_export = before[by, bx]
    right_export = after[ay, ax]
    if left_export.ndim != right_export.ndim:
        return {
            "available": False,
            "reason": f"rank mismatch {left_export.shape} vs {right_export.shape}",
        }
    if left_export.ndim == 3:
        common_channels = min(
            left_export.shape[-1],
            right_export.shape[-1],
        )
        status_left = left_export[..., :common_channels]
        status_right = right_export[..., :common_channels]
        compared_channels = min(common_channels, 3)
        left = status_left[..., :compared_channels]
        right = status_right[..., :compared_channels]
    else:
        status_left = left_export
        status_right = right_export
        left = left_export
        right = right_export

    export_left_finite = np.isfinite(left_export)
    export_right_finite = np.isfinite(right_export)
    status_left_finite = np.isfinite(status_left)
    status_right_finite = np.isfinite(status_right)
    finite_nonfinite_mismatch = status_left_finite ^ status_right_finite
    both_nonfinite = (~status_left_finite) & (~status_right_finite)
    left_finite = np.isfinite(left)
    right_finite = np.isfinite(right)
    finite = left_finite & right_finite

    if left_export.ndim == 3:
        export_left_finite_pixel = np.all(export_left_finite, axis=-1)
        export_right_finite_pixel = np.all(export_right_finite, axis=-1)
        status_finite_pixel = np.all(
            status_left_finite & status_right_finite,
            axis=-1,
        )
        finite_nonfinite_mismatch_pixel = np.any(
            finite_nonfinite_mismatch,
            axis=-1,
        )
        both_nonfinite_pixel = (
            np.any(~status_left_finite, axis=-1)
            & np.any(~status_right_finite, axis=-1)
        )
        finite_pixel = np.all(finite, axis=-1)
    else:
        export_left_finite_pixel = export_left_finite
        export_right_finite_pixel = export_right_finite
        status_finite_pixel = status_left_finite & status_right_finite
        finite_nonfinite_mismatch_pixel = finite_nonfinite_mismatch
        both_nonfinite_pixel = both_nonfinite
        finite_pixel = finite

    total_values_before = int(export_left_finite.size)
    total_values_after = int(export_right_finite.size)
    total_common_values = int(status_left_finite.size)
    total_compared_values = int(left_finite.size)
    total_pixels = int(export_left_finite_pixel.size)

    def fraction(count: int, total: int) -> float:
        return float(count / total) if total else 0.0

    finite_values_before = int(np.count_nonzero(export_left_finite))
    finite_values_after = int(np.count_nonzero(export_right_finite))
    finite_common_values = int(
        np.count_nonzero(status_left_finite & status_right_finite)
    )
    finite_compared_values = int(np.count_nonzero(finite))
    finite_pixels_before = int(np.count_nonzero(export_left_finite_pixel))
    finite_pixels_after = int(np.count_nonzero(export_right_finite_pixel))
    finite_common_pixels = int(np.count_nonzero(status_finite_pixel))
    finite_compared_pixels = int(np.count_nonzero(finite_pixel))
    nonfinite_values_before = total_values_before - finite_values_before
    nonfinite_values_after = total_values_after - finite_values_after
    nonfinite_pixels_before = total_pixels - finite_pixels_before
    nonfinite_pixels_after = total_pixels - finite_pixels_after
    mismatch_values = int(np.count_nonzero(finite_nonfinite_mismatch))
    mismatch_pixels = int(np.count_nonzero(finite_nonfinite_mismatch_pixel))
    both_nonfinite_values = int(np.count_nonzero(both_nonfinite))
    both_nonfinite_pixels = int(np.count_nonzero(both_nonfinite_pixel))
    finite_coverage = {
        "total_values_before": total_values_before,
        "total_values_after": total_values_after,
        "finite_values_before": finite_values_before,
        "finite_values_after": finite_values_after,
        "finite_value_fraction_before": fraction(
            finite_values_before,
            total_values_before,
        ),
        "finite_value_fraction_after": fraction(
            finite_values_after,
            total_values_after,
        ),
        "total_common_values": total_common_values,
        "finite_common_values": finite_common_values,
        "finite_common_value_fraction": fraction(
            finite_common_values,
            total_common_values,
        ),
        "total_compared_values": total_compared_values,
        "finite_compared_values": finite_compared_values,
        "finite_compared_value_fraction": fraction(
            finite_compared_values,
            total_compared_values,
        ),
        "total_pixels": total_pixels,
        "finite_pixels_before": finite_pixels_before,
        "finite_pixels_after": finite_pixels_after,
        "finite_pixel_fraction_before": fraction(
            finite_pixels_before,
            total_pixels,
        ),
        "finite_pixel_fraction_after": fraction(
            finite_pixels_after,
            total_pixels,
        ),
        "finite_common_pixels": finite_common_pixels,
        "finite_common_pixel_fraction": fraction(
            finite_common_pixels,
            total_pixels,
        ),
        "finite_compared_pixels": finite_compared_pixels,
        "finite_compared_pixel_fraction": fraction(
            finite_compared_pixels,
            total_pixels,
        ),
    }
    nonfinite_counts = {
        "values_before": nonfinite_values_before,
        "values_after": nonfinite_values_after,
        "pixels_before": nonfinite_pixels_before,
        "pixels_after": nonfinite_pixels_after,
        "nan_values_before": int(np.count_nonzero(np.isnan(left_export))),
        "nan_values_after": int(np.count_nonzero(np.isnan(right_export))),
        "inf_values_before": int(np.count_nonzero(np.isinf(left_export))),
        "inf_values_after": int(np.count_nonzero(np.isinf(right_export))),
        "finite_nonfinite_mismatch_values": mismatch_values,
        "finite_nonfinite_mismatch_pixels": mismatch_pixels,
        "both_nonfinite_values": both_nonfinite_values,
        "both_nonfinite_pixels": both_nonfinite_pixels,
    }
    result: dict[str, Any] = {
        "dx": int(dx),
        "dy": int(dy),
        "overlap_width": int(left_export.shape[1]),
        "overlap_height": int(left_export.shape[0]),
        "valid_pixels": finite_compared_pixels,
        "finite_coverage": finite_coverage,
        "nonfinite_counts": nonfinite_counts,
        "has_nonfinite": bool(
            nonfinite_values_before or nonfinite_values_after
        ),
        "has_finite_nonfinite_mismatch": bool(mismatch_values),
    }
    if finite_compared_values == 0 or finite_compared_pixels == 0:
        result.update(
            {
                "available": False,
                "reason": "no finite overlap",
            }
        )
        return result

    valid_values = np.abs(left[finite] - right[finite])
    if left.ndim == 3:
        valid_pixels = np.max(
            np.abs(left[finite_pixel] - right[finite_pixel]),
            axis=-1,
        )
    else:
        valid_pixels = valid_values
    changed = valid_pixels > float(epsilon)
    result.update(
        {
            "available": True,
            "changed_pixels": int(np.count_nonzero(changed)),
            "changed_fraction": float(np.mean(changed)),
            "mae": float(np.mean(valid_values, dtype=np.float64)),
            "rmse": float(
                math.sqrt(np.mean(valid_values.astype(np.float64) ** 2))
            ),
            "p95_abs": float(np.quantile(valid_values, 0.95)),
            "max_abs": float(np.max(valid_values)),
            "epsilon": float(epsilon),
        }
    )
    if depth:
        clear_left = _clear_value(left)
        clear_right = _clear_value(right)
        left_active = np.zeros(left.shape, dtype=bool)
        right_active = np.zeros(right.shape, dtype=bool)
        left_active[left_finite] = (
            np.abs(left[left_finite] - clear_left) > 1.0e-7
        )
        right_active[right_finite] = (
            np.abs(right[right_finite] - clear_right) > 1.0e-7
        )
        active_union = finite_pixel & (left_active | right_active)
        active_count = int(np.count_nonzero(active_union))
        finite_count = finite_compared_pixels
        before_active_count = int(np.count_nonzero(finite_pixel & left_active))
        after_active_count = int(np.count_nonzero(finite_pixel & right_active))
        result.update(
            {
                "before_active_pixels": before_active_count,
                "after_active_pixels": after_active_count,
                "before_active_fraction": before_active_count / finite_count,
                "after_active_fraction": after_active_count / finite_count,
                "active_union_fraction": active_count / finite_count,
            }
        )
        if active_count:
            active_diff = np.abs(left[active_union] - right[active_union])
            result.update(
                {
                    "clear_before": clear_left,
                    "clear_after": clear_right,
                    "active_union_pixels": active_count,
                    "active_changed_fraction": float(
                        np.mean(active_diff > float(epsilon))
                    ),
                    "active_mae": float(
                        np.mean(active_diff, dtype=np.float64)
                    ),
                    "foreground_mismatch_fraction": float(
                        np.mean(
                            (left_active != right_active)[active_union]
                        )
                    ),
                }
            )
        else:
            result.update(
                {
                    "clear_before": clear_left,
                    "clear_after": clear_right,
                    "active_union_pixels": 0,
                    "active_changed_fraction": 0.0,
                    "active_mae": 0.0,
                    "foreground_mismatch_fraction": 0.0,
                }
            )
    return result


def _release_nonfinite_errors(
    metric: dict[str, Any],
    *,
    label: str,
) -> list[str]:
    counts = metric.get("nonfinite_counts")
    if not isinstance(counts, dict):
        return []
    before_values = int(counts.get("values_before", 0) or 0)
    after_values = int(counts.get("values_after", 0) or 0)
    mismatch_values = int(
        counts.get("finite_nonfinite_mismatch_values", 0) or 0
    )
    both_nonfinite_values = int(counts.get("both_nonfinite_values", 0) or 0)
    if not (before_values or after_values or mismatch_values):
        return []
    return [
        f"{label} contains non-finite values: "
        f"before={before_values}, after={after_values}, "
        f"finite/non-finite mismatch={mismatch_values}, "
        f"both-non-finite={both_nonfinite_values}"
    ]


def _compare_export_pair(
    before_root: Path,
    after_root: Path,
    before_export: dict[str, Any],
    after_export: dict[str, Any],
    *,
    max_shift: int,
    epsilon: float,
    depth: bool,
) -> dict[str, Any]:
    before = _load_export(before_root, before_export)
    after = _load_export(after_root, after_export)
    registration = _find_integer_registration(
        before,
        after,
        max_shift=max_shift,
        depth=depth,
    )
    unregistered = _array_diff_metrics(
        before,
        after,
        dx=0,
        dy=0,
        epsilon=epsilon,
        depth=depth,
    )
    if registration["available"]:
        residual = _array_diff_metrics(
            before,
            after,
            dx=int(registration["dx"]),
            dy=int(registration["dy"]),
            epsilon=epsilon,
            depth=depth,
        )
    else:
        residual = {"available": False, "reason": registration["reason"]}
    return {
        "before_export": before_export,
        "after_export": after_export,
        "registration": registration,
        "unregistered": unregistered,
        "residual": residual,
    }


def _matrix_from_entry(entry: dict[str, Any]) -> Any | None:
    import numpy as np

    rows = int(entry.get("rows", 0))
    columns = int(entry.get("columns", 0))
    values = entry.get("value", [])
    if rows < 3 or columns < 4 or len(values) < rows * columns:
        return None
    matrix = np.asarray(values[: rows * columns], dtype=np.float64).reshape(
        (rows, columns)
    )
    if rows == 3:
        matrix = np.vstack([matrix, np.array([0.0, 0.0, 0.0, 1.0])])
    if columns == 3:
        matrix = np.column_stack(
            [matrix, np.array([0.0, 0.0, 0.0, 1.0])]
        )
    matrix = matrix[:4, :4]
    return matrix if np.all(np.isfinite(matrix)) else None


def _matrix_geometry(matrix: Any, orientation: str, resolution: int) -> dict[str, Any] | None:
    import numpy as np

    working = matrix if orientation == "row_major" else matrix.T
    x_vector = working[0, :3]
    y_vector = working[1, :3]
    scale_x = float(np.linalg.norm(x_vector))
    scale_y = float(np.linalg.norm(y_vector))
    if not math.isfinite(scale_x) or not math.isfinite(scale_y):
        return None
    if scale_x <= 1.0e-12 or scale_y <= 1.0e-12:
        return None
    basis_x = x_vector / scale_x
    basis_y = y_vector / scale_y
    orthogonality = float(abs(np.dot(basis_x, basis_y)))
    center_x = float(-working[0, 3] / scale_x)
    center_y = float(-working[1, 3] / scale_y)
    if not math.isfinite(center_x) or not math.isfinite(center_y):
        return None
    return {
        "orientation": orientation,
        "scale_x_clip_per_world": scale_x,
        "scale_y_clip_per_world": scale_y,
        "world_units_per_texel_x": 2.0 / (scale_x * resolution),
        "world_units_per_texel_y": 2.0 / (scale_y * resolution),
        "basis_x": [float(value) for value in basis_x],
        "basis_y": [float(value) for value in basis_y],
        "center_x_basis_units": center_x,
        "center_y_basis_units": center_y,
        "xy_orthogonality_error": orthogonality,
        "xy_scale_anisotropy": abs(scale_x - scale_y)
        / max(scale_x, scale_y),
        "assumption": "matrix maps world coordinates to [-1,1] clip XY",
    }


def _projection_path_score(path: str) -> float:
    lower = path.lower()
    compact = re.sub(r"[^a-z0-9]+", "", lower)
    score = 0.0
    for token, weight in (
        ("shadow", 20.0),
        ("cascade", 20.0),
        ("light", 10.0),
        ("viewproj", 12.0),
        ("view_projection", 12.0),
        ("matrix", 2.0),
        ("model", -20.0),
        ("world", -8.0),
        ("inverse", -20.0),
        ("prev", -40.0),
        ("jitter", -20.0),
    ):
        if token in lower:
            score += weight
    if compact.endswith("viewprojection"):
        score += 40.0
    elif compact.endswith("projection"):
        score += 15.0
    return score


def _select_projection_geometry(
    manifest: dict[str, Any],
    *,
    resolution: int,
) -> dict[str, Any]:
    sources = manifest.get("projection_sources", [])
    if not sources:
        return {"available": False, "reason": "no CSM shader matrices were extracted"}
    per_cascade: dict[int, dict[str, dict[str, Any]]] = {}
    source_eids: dict[int, int] = {}
    for source in sources:
        cascade = int(source["cascade"])
        source_eids[cascade] = int(source.get("eid", 0))
        paths = {}
        for entry in source.get("matrices", []):
            matrix = _matrix_from_entry(entry)
            if matrix is not None:
                paths[str(entry["path"])] = {"entry": entry, "matrix": matrix}
        per_cascade[cascade] = paths
    common_paths: set[str] | None = None
    for paths in per_cascade.values():
        common_paths = set(paths) if common_paths is None else common_paths & set(paths)
    if not common_paths:
        return {
            "available": False,
            "reason": "no matrix path was common to every cascade draw",
            "matrix_paths_by_cascade": {
                str(cascade): sorted(paths) for cascade, paths in per_cascade.items()
            },
        }

    candidates: list[tuple[float, str, str, list[dict[str, Any]]]] = []
    for path in sorted(common_paths):
        for orientation in ("row_major", "column_major"):
            geometries = []
            valid = True
            for cascade in sorted(per_cascade):
                geometry = _matrix_geometry(
                    per_cascade[cascade][path]["matrix"],
                    orientation,
                    resolution,
                )
                if geometry is None:
                    valid = False
                    break
                geometry["cascade"] = cascade
                geometry["eid"] = source_eids.get(cascade, 0)
                matrix = per_cascade[cascade][path]["matrix"]
                geometry["matrix_values"] = [
                    float(value) for value in matrix.reshape(-1)
                ]
                geometries.append(geometry)
            if not valid:
                continue
            scales = [
                math.sqrt(
                    geometry["scale_x_clip_per_world"]
                    * geometry["scale_y_clip_per_world"]
                )
                for geometry in geometries
            ]
            variation = max(scales) / max(min(scales), 1.0e-12)
            monotonic = sum(
                1 for left, right in zip(scales, scales[1:]) if right <= left
            )
            square_error = sum(
                geometry["xy_scale_anisotropy"] for geometry in geometries
            )
            orthogonal_error = sum(
                geometry["xy_orthogonality_error"] for geometry in geometries
            )
            score = (
                _projection_path_score(path)
                + 3.0 * monotonic
                + min(12.0, math.log2(max(1.0, variation)) * 3.0)
                - 10.0 * square_error
                - 10.0 * orthogonal_error
            )
            candidates.append((score, path, orientation, geometries))
    if not candidates:
        return {"available": False, "reason": "no plausible projection matrix found"}
    best_score = max(candidate[0] for candidate in candidates)
    tied = [
        candidate
        for candidate in candidates
        if math.isclose(candidate[0], best_score, rel_tol=0.0, abs_tol=1.0e-12)
    ]
    best = min(tied, key=lambda candidate: (candidate[1], candidate[2]))
    return {
        "available": True,
        "matrix_path": best[1],
        "orientation": best[2],
        "selection_score": best[0],
        "selection_ambiguous": len(tied) != 1,
        "top_tie_count": len(tied),
        "tied_candidates": [
            {"matrix_path": candidate[1], "orientation": candidate[2]}
            for candidate in tied
        ],
        "cascades": best[3],
    }


def _nearest_power_of_two_ratio(ratio: float) -> tuple[float, int, float]:
    if ratio <= 0.0 or not math.isfinite(ratio):
        return float("nan"), 0, float("inf")
    exponent = int(round(math.log2(ratio)))
    nearest = 2.0**exponent
    relative_error = abs(ratio - nearest) / nearest
    return nearest, exponent, relative_error


def _analyze_cascade_nesting(
    geometry: dict[str, Any],
    *,
    power2_tolerance: float,
    phase_tolerance: float,
) -> dict[str, Any]:
    import numpy as np

    if not geometry.get("available"):
        return geometry
    cascades = geometry["cascades"]
    pairs = []
    all_nested = True
    for fine, coarse in zip(cascades, cascades[1:]):
        fine_texel_x = float(fine["world_units_per_texel_x"])
        fine_texel_y = float(fine["world_units_per_texel_y"])
        coarse_texel_x = float(coarse["world_units_per_texel_x"])
        coarse_texel_y = float(coarse["world_units_per_texel_y"])
        ratio_x = coarse_texel_x / fine_texel_x
        ratio_y = coarse_texel_y / fine_texel_y
        nearest_x, exponent_x, error_x = _nearest_power_of_two_ratio(ratio_x)
        nearest_y, exponent_y, error_y = _nearest_power_of_two_ratio(ratio_y)
        basis_x_alignment = float(
            abs(np.dot(fine["basis_x"], coarse["basis_x"]))
        )
        basis_y_alignment = float(
            abs(np.dot(fine["basis_y"], coarse["basis_y"]))
        )
        phase_x = (
            float(coarse["center_x_basis_units"])
            - float(fine["center_x_basis_units"])
        ) / fine_texel_x
        phase_y = (
            float(coarse["center_y_basis_units"])
            - float(fine["center_y_basis_units"])
        ) / fine_texel_y
        phase_error_x = abs(phase_x - round(phase_x))
        phase_error_y = abs(phase_y - round(phase_y))
        near_power2 = (
            error_x <= power2_tolerance
            and error_y <= power2_tolerance
            and exponent_x == exponent_y
        )
        aligned = (
            basis_x_alignment >= 0.999
            and basis_y_alignment >= 0.999
            and phase_error_x <= phase_tolerance
            and phase_error_y <= phase_tolerance
        )
        nested = near_power2 and aligned
        all_nested = all_nested and nested
        pairs.append(
            {
                "fine_cascade": int(fine["cascade"]),
                "coarse_cascade": int(coarse["cascade"]),
                "texel_ratio_x": ratio_x,
                "texel_ratio_y": ratio_y,
                "nearest_power_of_two_x": nearest_x,
                "nearest_power_of_two_y": nearest_y,
                "power_exponent_x": exponent_x,
                "power_exponent_y": exponent_y,
                "power_ratio_relative_error_x": error_x,
                "power_ratio_relative_error_y": error_y,
                "near_same_power_of_two": near_power2,
                "basis_alignment_x": basis_x_alignment,
                "basis_alignment_y": basis_y_alignment,
                "center_delta_fine_texels_x": phase_x,
                "center_delta_fine_texels_y": phase_y,
                "phase_error_to_integer_texel_x": phase_error_x,
                "phase_error_to_integer_texel_y": phase_error_y,
                "grid_aligned": aligned,
                "nested": nested,
            }
        )
    return {
        **geometry,
        "adjacent_pairs": pairs,
        "all_adjacent_pairs_nested": bool(pairs) and all_nested,
        "power2_tolerance": power2_tolerance,
        "phase_tolerance_texels": phase_tolerance,
    }


def _compare_projection_motion(
    before_geometry: dict[str, Any],
    after_geometry: dict[str, Any],
    *,
    matrix_epsilon: float,
) -> dict[str, Any]:
    if not before_geometry.get("available") or not after_geometry.get("available"):
        return {
            "available": False,
            "matrix_comparison_available": False,
            "reason": "projection geometry unavailable in one or both captures",
        }
    before_by_cascade = {
        int(row["cascade"]): row for row in before_geometry["cascades"]
    }
    after_by_cascade = {
        int(row["cascade"]): row for row in after_geometry["cascades"]
    }
    same_matrix_path = (
        before_geometry.get("matrix_path") == after_geometry.get("matrix_path")
    )
    rows = []
    for cascade in sorted(set(before_by_cascade) & set(after_by_cascade)):
        before = before_by_cascade[cascade]
        after = after_by_cascade[cascade]
        scale_change_x = (
            float(after["scale_x_clip_per_world"])
            / float(before["scale_x_clip_per_world"])
            - 1.0
        )
        scale_change_y = (
            float(after["scale_y_clip_per_world"])
            / float(before["scale_y_clip_per_world"])
            - 1.0
        )
        center_delta_x = (
            float(after["center_x_basis_units"])
            - float(before["center_x_basis_units"])
        ) / float(before["world_units_per_texel_x"])
        center_delta_y = (
            float(after["center_y_basis_units"])
            - float(before["center_y_basis_units"])
        ) / float(before["world_units_per_texel_y"])
        before_values = [float(value) for value in before.get("matrix_values", [])]
        after_values = [float(value) for value in after.get("matrix_values", [])]
        matrix_result: dict[str, Any]
        if not same_matrix_path:
            matrix_result = {
                "available": False,
                "reason": "selected CSM matrix paths differ between captures",
            }
        elif not before_values or len(before_values) != len(after_values):
            matrix_result = {
                "available": False,
                "reason": "CSM matrix element counts differ between captures",
            }
        else:
            differences = [
                abs(left - right)
                for left, right in zip(before_values, after_values)
            ]
            matrix_result = {
                "available": True,
                "element_count": len(differences),
                "changed_elements": sum(
                    difference > matrix_epsilon for difference in differences
                ),
                "changed_fraction": sum(
                    difference > matrix_epsilon for difference in differences
                )
                / len(differences),
                "mae": sum(differences) / len(differences),
                "max_abs": max(differences),
                "epsilon": matrix_epsilon,
            }
        rows.append(
            {
                "cascade": cascade,
                "relative_scale_change_x": scale_change_x,
                "relative_scale_change_y": scale_change_y,
                "center_motion_before_texels_x": center_delta_x,
                "center_motion_before_texels_y": center_delta_y,
                "nearest_integer_center_motion_x": round(center_delta_x),
                "nearest_integer_center_motion_y": round(center_delta_y),
                "fractional_center_motion_x": center_delta_x
                - round(center_delta_x),
                "fractional_center_motion_y": center_delta_y
                - round(center_delta_y),
                "matrix": matrix_result,
            }
        )
    return {
        "available": bool(rows),
        "same_matrix_path": same_matrix_path,
        "matrix_path_before": before_geometry.get("matrix_path"),
        "matrix_path_after": after_geometry.get("matrix_path"),
        "matrix_comparison_available": bool(rows)
        and all(row["matrix"].get("available") for row in rows),
        "matrix_epsilon": matrix_epsilon,
        "cascades": rows,
    }


def _threshold_value(
    result: dict[str, Any],
    key: str,
    fallback: float = float("inf"),
) -> float:
    if not result.get("available"):
        return fallback
    value = result.get(key)
    return float(value) if value is not None else fallback


def _build_origin_assessment(
    comparisons: dict[str, Any],
    violations: list[dict[str, Any]],
    *,
    blend_fraction: float,
    cascade_nesting: dict[str, Any],
) -> dict[str, Any]:
    failed_roles = {violation["role"] for violation in violations}
    if "csm" in failed_roles:
        primary = "raw_csm"
        explanation = (
            "One or more CSM cascade layers retain depth differences after the "
            "best integer registration."
        )
    elif "light" in failed_roles:
        primary = "light_pass"
        explanation = (
            "Registered CSM depth is within threshold, while the light target "
            "first exceeds its configured residual threshold."
        )
    elif "taa" in failed_roles:
        primary = "taa"
        explanation = (
            "CSM and light residuals are within threshold, while TAA first "
            "exceeds its configured residual threshold."
        )
    elif "final" in failed_roles:
        primary = "final"
        explanation = (
            "Only the final target exceeds its configured residual threshold."
        )
    else:
        primary = "none_significant"
        explanation = "All discovered stages are within configured thresholds."

    integer_csm_motion = any(
        abs(int(row["registration"].get("dx", 0)))
        + abs(int(row["registration"].get("dy", 0)))
        > 0
        and row["residual"].get("available")
        and row["residual"].get("changed_fraction", 1.0)
        < row["unregistered"].get("changed_fraction", 0.0)
        for row in comparisons.get("csm", [])
    )
    nested = cascade_nesting.get("all_adjacent_pairs_nested")
    blending_risk = bool(
        integer_csm_motion
        and blend_fraction > 0.0
        and nested is not True
    )
    return {
        "primary": primary,
        "explanation": explanation,
        "integer_registered_csm_motion_detected": integer_csm_motion,
        "cascade_blend_fraction": blend_fraction,
        "independent_grid_blending_edge_risk": blending_risk,
        "caveat": (
            "Independent/non-nested cascade grids combined with cascade "
            f"blending ({blend_fraction:.1%}) can create motion-only shadow "
            "edge shifts before TAA, even when each cascade mostly differs by "
            "an integer texel translation."
            if blending_risk
            else None
        ),
    }


def _parse_args(argv: list[str] | None = None) -> argparse.Namespace:
    parser = argparse.ArgumentParser(
        description=(
            "Semantically discover and compare CSM, lighting, TAA, and final "
            "targets without hard-coded resource IDs. Accept either two .rdc "
            "captures or an mgif CSM motion smoke manifest."
        )
    )
    parser.add_argument(
        "before",
        nargs="?",
        type=Path,
        help="Before .rdc capture, or a smoke manifest.json when used alone",
    )
    parser.add_argument(
        "after",
        nargs="?",
        type=Path,
        help="After .rdc capture",
    )
    parser.add_argument(
        "--manifest",
        type=Path,
        help="mgif-csm-shadow-motion-smoke-v1 manifest.json",
    )
    parser.add_argument(
        "--case",
        action="append",
        default=[],
        help="Only compare this manifest case name (repeatable)",
    )
    release_group = parser.add_mutually_exclusive_group()
    release_group.add_argument(
        "--release-gate",
        dest="release_gate",
        action="store_true",
        default=None,
        help="Enable strict final smoke-manifest publication checks",
    )
    release_group.add_argument(
        "--no-release-gate",
        dest="release_gate",
        action="store_false",
        help="Disable automatic release gating for a final smoke manifest",
    )
    parser.add_argument(
        "--self-test",
        action="store_true",
        help="Run pure-Python release-gate self-tests and exit",
    )
    parser.add_argument(
        "-o",
        "--output",
        type=Path,
        help="Write the JSON report to this path",
    )
    parser.add_argument(
        "--json",
        action="store_true",
        help="Print the complete JSON report to stdout",
    )
    parser.add_argument("--rdc", default="rdc", help="rdc executable")
    parser.add_argument(
        "--timeout",
        type=float,
        default=600.0,
        help="Timeout in seconds for each rdc open/script operation (default: 600)",
    )
    parser.add_argument(
        "--total-timeout",
        type=float,
        default=DEFAULT_TOTAL_TIMEOUT_SECONDS,
        help=(
            "Total wall-clock deadline in seconds, including extraction, comparison, "
            "and reserved per-session cleanup time (default: 7200)"
        ),
    )
    parser.add_argument(
        "--max-shift",
        type=int,
        default=128,
        help="Maximum diagnostic integer-registration search in pixels (default: 128)",
    )
    parser.add_argument(
        "--max-csm-registration-shift",
        type=int,
        help=(
            "Maximum absolute CSM layer registration dx/dy. Manifest same-pose "
            "comparisons default to 0; direct capture-pair mode leaves it ungated."
        ),
    )
    parser.add_argument(
        "--max-csm-matrix-delta",
        type=float,
        default=1.0e-5,
        help="Maximum per-element CSM matrix delta for same-pose pairs (default: 1e-5)",
    )
    parser.add_argument(
        "--pose-tolerance",
        type=float,
        default=DEFAULT_POSE_TOLERANCE,
        help="Manifest pose-contract tolerance (default: 1e-5)",
    )
    parser.add_argument(
        "--depth-epsilon",
        type=float,
        default=1.0e-6,
        help="Depth value difference counted as changed (default: 1e-6)",
    )
    parser.add_argument(
        "--color-epsilon",
        type=float,
        default=1.0e-3,
        help="Color channel difference counted as changed (default: 1e-3)",
    )
    parser.add_argument(
        "--max-csm-residual-fraction",
        type=float,
        default=0.001,
        help="Maximum aligned CSM changed fraction (default: 0.001)",
    )
    parser.add_argument(
        "--max-csm-residual-mae",
        type=float,
        default=1.0e-5,
        help="Maximum aligned CSM depth MAE (default: 1e-5)",
    )
    parser.add_argument(
        "--max-csm-active-changed-fraction",
        type=float,
        default=0.001,
        help="Maximum changed fraction within active CSM texels (default: 0.001)",
    )
    parser.add_argument(
        "--max-csm-foreground-mismatch-fraction",
        type=float,
        default=0.0001,
        help="Maximum CSM foreground/background mismatch fraction (default: 0.0001)",
    )
    parser.add_argument(
        "--max-csm-max-abs",
        type=float,
        default=1.0e-4,
        help="Maximum absolute CSM depth delta (default: 1e-4)",
    )
    parser.add_argument(
        "--min-csm-active-pixels",
        type=int,
        default=DEFAULT_MIN_CSM_ACTIVE_PIXELS,
        help="Minimum active pixels required in every CSM layer (default: 64)",
    )
    parser.add_argument(
        "--min-csm-active-fraction",
        type=float,
        default=DEFAULT_MIN_CSM_ACTIVE_FRACTION,
        help="Minimum active fraction required in every CSM layer (default: 1e-5)",
    )
    parser.add_argument(
        "--max-csm-center-motion-texels",
        type=float,
        default=1.0e-3,
        help="Maximum same-pose CSM projection-center motion in texels (default: 1e-3)",
    )
    parser.add_argument(
        "--max-light-residual-fraction",
        type=float,
        default=0.01,
        help="Maximum Light changed fraction (default: 0.01)",
    )
    parser.add_argument(
        "--max-taa-residual-fraction",
        type=float,
        default=0.01,
        help="Maximum TAA changed fraction (default: 0.01)",
    )
    parser.add_argument(
        "--max-final-residual-fraction",
        type=float,
        default=0.01,
        help="Maximum final-target changed fraction (default: 0.01)",
    )
    parser.add_argument(
        "--max-light-max-abs",
        type=float,
        default=0.01,
        help="Maximum absolute same-pixel Light delta in release mode (default: 0.01)",
    )
    parser.add_argument(
        "--max-taa-max-abs",
        type=float,
        default=0.01,
        help="Maximum absolute same-pixel TAA delta in release mode (default: 0.01)",
    )
    parser.add_argument(
        "--max-final-max-abs",
        type=float,
        default=0.01,
        help="Maximum absolute same-pixel final delta in release mode (default: 0.01)",
    )
    parser.add_argument(
        "--power2-tolerance",
        type=float,
        default=0.05,
        help="Relative tolerance for adjacent texel-size power-of-two ratios",
    )
    parser.add_argument(
        "--grid-phase-tolerance",
        type=float,
        default=0.10,
        help="Texel tolerance for nested-grid phase alignment",
    )
    parser.add_argument(
        "--cascade-blend-fraction",
        type=float,
        default=0.10,
        help="Known/assumed cascade blend fraction for the risk note",
    )
    parser.add_argument(
        "--work-dir",
        type=Path,
        help="Use this directory for extracted arrays instead of a temporary one",
    )
    parser.add_argument(
        "--keep-work-dir",
        action="store_true",
        help="Keep automatically-created extraction files",
    )
    return parser.parse_args(argv)


def _validate_args(options: argparse.Namespace) -> None:
    if (
        options.manifest is None
        and options.before is not None
        and options.after is None
        and options.before.suffix.lower() == ".json"
    ):
        options.manifest = options.before
        options.before = None

    if options.manifest is not None:
        if options.before is not None or options.after is not None:
            raise ComparatorError(
                "use either --manifest/one manifest positional path or two .rdc paths"
            )
        if not options.manifest.is_file():
            raise ComparatorError(f"manifest does not exist: {options.manifest}")
    else:
        if options.before is None or options.after is None:
            raise ComparatorError("provide two .rdc captures or one smoke manifest.json")
        for label, path in (("before", options.before), ("after", options.after)):
            if not path.is_file():
                raise ComparatorError(f"{label} capture does not exist: {path}")
            if path.suffix.lower() != ".rdc":
                raise ComparatorError(f"{label} capture is not an .rdc file: {path}")
        if options.case:
            raise ComparatorError("--case is only valid with a smoke manifest")
        if options.release_gate is True:
            raise ComparatorError("--release-gate requires a smoke manifest")

    if not math.isfinite(options.timeout) or options.timeout <= 0.0:
        raise ComparatorError("--timeout must be a positive finite number")
    if not math.isfinite(options.total_timeout) or options.total_timeout <= 0.0:
        raise ComparatorError("--total-timeout must be a positive finite number")
    if options.total_timeout <= CAPTURE_CLEANUP_RESERVE_SECONDS:
        raise ComparatorError(
            "--total-timeout must exceed the reserved per-session cleanup budget "
            f"({CAPTURE_CLEANUP_RESERVE_SECONDS:g}s)"
        )
    if options.max_shift < 0:
        raise ComparatorError("--max-shift must be nonnegative")
    if (
        options.max_csm_registration_shift is not None
        and options.max_csm_registration_shift < 0
    ):
        raise ComparatorError("--max-csm-registration-shift must be nonnegative")
    if options.min_csm_active_pixels < 1:
        raise ComparatorError("--min-csm-active-pixels must be at least 1")
    for name in (
        "max_csm_matrix_delta",
        "pose_tolerance",
        "depth_epsilon",
        "color_epsilon",
        "max_csm_residual_fraction",
        "max_csm_residual_mae",
        "max_csm_active_changed_fraction",
        "max_csm_foreground_mismatch_fraction",
        "max_csm_max_abs",
        "min_csm_active_fraction",
        "max_csm_center_motion_texels",
        "max_light_residual_fraction",
        "max_taa_residual_fraction",
        "max_final_residual_fraction",
        "max_light_max_abs",
        "max_taa_max_abs",
        "max_final_max_abs",
        "power2_tolerance",
        "grid_phase_tolerance",
        "cascade_blend_fraction",
    ):
        value = float(getattr(options, name))
        if not math.isfinite(value) or value < 0.0:
            raise ComparatorError(
                f"--{name.replace('_', '-')} must be a finite nonnegative number"
            )
    if shutil.which(options.rdc) is None and not Path(options.rdc).is_file():
        raise ComparatorError(f"rdc executable not found: {options.rdc}")


def _discovery_summary(manifest: dict[str, Any]) -> dict[str, Any]:
    summary = {}
    for role in ROLE_ORDER:
        entry = manifest.get("discovery", {}).get(role)
        if entry is None:
            summary[role] = None
            continue
        summary[role] = {
            "resource": entry.get("resource"),
            "eid": entry.get("eid"),
            "score": entry.get("score"),
            "reasons": entry.get("reasons"),
            "selection": entry.get("selection"),
            "marker": entry.get("marker"),
            "export_error": entry.get("export_error"),
            "passthrough": entry.get("passthrough", False),
        }
    return summary


RELEASE_EXPORT_CHANNEL_ORDERS = {
    "scene_depth": ["D"],
    "base_color": ["R", "G", "B", "A"],
    "world_normals": ["X", "Y", "Z"],
    "scene_color_hdr": ["R", "G", "B", "A"],
    "history_read": ["R", "G", "B", "A"],
    "history_write": ["R", "G", "B", "A"],
    "final": ["R", "G", "B", "A"],
}


def _validate_host_npy_export(
    root: Path,
    export: Any,
    *,
    label: str,
    role: str,
    resource: dict[str, Any],
    expected_channel_order: list[str],
    expected_evidence_only: bool,
    expected_dimension_class: str,
) -> tuple[list[str], tuple[int, int] | None]:
    import numpy as np

    errors: list[str] = []
    if not isinstance(export, dict):
        return [f"{label}: export metadata is missing"], None
    if export.get("kind") != "npy":
        errors.append(f"{label}: export kind is not NPY")
    path_value = export.get("path")
    resolved_path: Path | None = None
    if not isinstance(path_value, str) or not path_value.strip():
        errors.append(f"{label}: NPY path is missing")
    else:
        try:
            resolved_path = (root / path_value).resolve(strict=True)
        except (OSError, RuntimeError) as exc:
            errors.append(f"{label}: NPY path cannot be resolved: {exc}")
        else:
            if not _path_is_within(resolved_path, root):
                errors.append(f"{label}: NPY path escapes extraction root")
            if resolved_path.suffix.lower() != ".npy":
                errors.append(f"{label}: export path is not .npy")
    observed_shape: tuple[int, ...] | None = None
    observed_dtype: str | None = None
    if resolved_path is not None and not errors:
        try:
            array = np.load(resolved_path, allow_pickle=False, mmap_mode="r")
            observed_shape = tuple(int(value) for value in array.shape)
            observed_dtype = str(array.dtype)
            del array
        except Exception as exc:
            errors.append(f"{label}: cannot reopen NPY: {type(exc).__name__}: {exc}")
    declared_shape = export.get("shape")
    if not (
        isinstance(declared_shape, list)
        and all(isinstance(value, int) and value > 0 for value in declared_shape)
    ):
        errors.append(f"{label}: declared shape is invalid")
        declared_shape_tuple: tuple[int, ...] | None = None
    else:
        declared_shape_tuple = tuple(declared_shape)
    if observed_shape is not None and declared_shape_tuple != observed_shape:
        errors.append(
            f"{label}: actual NPY shape {observed_shape!r} differs from "
            f"declared {declared_shape_tuple!r}"
        )
    if export.get("dtype") != "float32" or observed_dtype not in (None, "float32"):
        errors.append(
            f"{label}: dtype must be float32, declared={export.get('dtype')!r} "
            f"actual={observed_dtype!r}"
        )
    if export.get("channel_order") != expected_channel_order:
        errors.append(
            f"{label}: channel order {export.get('channel_order')!r} does not "
            f"equal {expected_channel_order!r}"
        )
    expected_rank = 2 if len(expected_channel_order) == 1 else 3
    if declared_shape_tuple is not None:
        if len(declared_shape_tuple) != expected_rank:
            errors.append(f"{label}: NPY rank does not match channel contract")
        elif expected_rank == 3 and declared_shape_tuple[-1] != len(
            expected_channel_order
        ):
            errors.append(f"{label}: NPY channel count does not match channel order")
    if export.get("evidence_only") is not expected_evidence_only:
        errors.append(
            f"{label}: evidence_only must be {expected_evidence_only}"
        )
    if export.get("dimension_class") != expected_dimension_class:
        errors.append(
            f"{label}: dimension_class must be {expected_dimension_class!r}"
        )
    if int(export.get("source_resource_id", 0) or 0) != int(
        resource.get("resource_id", -1)
    ):
        errors.append(f"{label}: source resource ID differs from binding evidence")
    source_format = export.get("source_format")
    if not isinstance(source_format, dict):
        errors.append(f"{label}: source format evidence is missing")
        source_format = {}
    format_contract = {
        "name": resource.get("format"),
        "format_type": resource.get("format_type"),
        "component_type": resource.get("component_type"),
        "component_count": resource.get("component_count"),
        "component_byte_width": resource.get("component_byte_width"),
        "bgra": resource.get("bgra"),
    }
    for field, expected in format_contract.items():
        if source_format.get(field) != expected:
            errors.append(
                f"{label}: source_format.{field}={source_format.get(field)!r}, "
                f"expected {expected!r}"
            )
    bytes_per_pixel = int(source_format.get("bytes_per_pixel", 0) or 0)
    if bytes_per_pixel <= 0:
        errors.append(f"{label}: source bytes_per_pixel is invalid")
    readback = export.get("readback_contract")
    if not isinstance(readback, dict):
        errors.append(f"{label}: tight readback contract is missing")
        readback = {}
    expected_width = int(resource.get("width", 0) or 0)
    expected_height = int(resource.get("height", 0) or 0)
    readback_checks = {
        "width": readback.get("width") == expected_width,
        "height": readback.get("height") == expected_height,
        "texture_depth": readback.get("texture_depth") == 1,
        "samples": readback.get("samples") == 1,
        "mip": readback.get("mip") == 0,
        "sample": readback.get("sample") == 0,
        "tightly_packed_required": readback.get("tightly_packed_required") is True,
        "raw_byte_length_exact": readback.get("raw_byte_length_exact") is True,
    }
    failed_readback = [name for name, passed in readback_checks.items() if not passed]
    if failed_readback:
        errors.append(f"{label}: readback contract failed {failed_readback}")
    expected_raw = expected_width * expected_height * bytes_per_pixel
    if int(readback.get("expected_raw_bytes", -1) or -1) != expected_raw:
        errors.append(f"{label}: expected raw byte length is inconsistent")
    if int(readback.get("raw_byte_length", -1) or -1) != expected_raw:
        errors.append(f"{label}: actual raw byte length is not exact")
    if readback.get("source_format") != source_format:
        errors.append(f"{label}: readback/export source formats differ")
    validation = export.get("npy_validation")
    if not isinstance(validation, dict):
        errors.append(f"{label}: immediate NPY validation evidence is missing")
        validation = {}
    if validation.get("schema") != NPY_EXPORT_EVIDENCE_SCHEMA:
        errors.append(f"{label}: immediate NPY validation schema is invalid")
    if validation.get("passed") is not True or validation.get("errors") not in ([], None):
        errors.append(f"{label}: immediate NPY validation did not pass cleanly")
    for field, expected in (
        ("shape", list(observed_shape) if observed_shape is not None else declared_shape),
        ("dtype", observed_dtype or export.get("dtype")),
        ("channel_order", expected_channel_order),
        ("source_format", source_format),
        ("source_resource_id", int(resource.get("resource_id", -1))),
        ("dimension_class", expected_dimension_class),
        ("evidence_only", expected_evidence_only),
    ):
        if validation.get(field) != expected:
            errors.append(f"{label}: npy_validation.{field} is inconsistent")
    if validation.get("reopened_after_write") is not True:
        errors.append(f"{label}: NPY was not proven reopened after write")
    if int(resource.get("samples", 0) or 0) != 1:
        errors.append(f"{label}: bound resource samples is not exactly one")
    if int(resource.get("depth", 0) or 0) != 1:
        errors.append(f"{label}: bound resource depth is not exactly one")
    readback_budget = export.get("readback_budget")
    if not isinstance(readback_budget, dict):
        errors.append(f"{label}: readback budget evidence is missing")
    else:
        expected_budget = _manifest_integer(readback_budget.get("expected_bytes"))
        actual_budget = _manifest_integer(readback_budget.get("actual_bytes"))
        if readback_budget.get("completed") is not True:
            errors.append(f"{label}: readback budget did not complete")
        if expected_budget != expected_raw or actual_budget != expected_raw:
            errors.append(f"{label}: readback budget bytes do not equal exact raw bytes")
        if readback_budget.get("single_readback_cap_bytes") != COMPARATOR_MAX_READBACK_BYTES:
            errors.append(f"{label}: readback single-file cap differs from comparator policy")
        if expected_budget is None or expected_budget > COMPARATOR_MAX_READBACK_BYTES:
            errors.append(f"{label}: readback exceeds the single-readback cap")
    write_budget = export.get("write_budget")
    if not isinstance(write_budget, dict):
        errors.append(f"{label}: pre-write NPY budget evidence is missing")
    else:
        actual_file_bytes = _manifest_integer(write_budget.get("actual_file_bytes"))
        estimated_write_bytes = _manifest_integer(
            write_budget.get("estimated_write_bytes")
        )
        free_before_write = _manifest_integer(write_budget.get("free_bytes_before_write"))
        required_before_write = _manifest_integer(
            write_budget.get("required_free_bytes_before_write")
        )
        if write_budget.get("completed") is not True:
            errors.append(f"{label}: pre-write NPY budget did not complete")
        if (
            actual_file_bytes is None
            or estimated_write_bytes is None
            or actual_file_bytes <= 0
            or actual_file_bytes > estimated_write_bytes
            or actual_file_bytes > COMPARATOR_EXTRACT_BYTES_PER_CAPTURE
        ):
            errors.append(f"{label}: NPY write budget byte accounting is invalid")
        if free_before_write is None or required_before_write is None or (
            free_before_write < required_before_write
        ):
            errors.append(f"{label}: NPY pre-write free-space proof is invalid")
        if resolved_path is not None and actual_file_bytes is not None:
            try:
                if int(resolved_path.stat().st_size) != actual_file_bytes:
                    errors.append(f"{label}: NPY actual file size differs from budget evidence")
            except OSError as exc:
                errors.append(f"{label}: cannot restat NPY for budget validation: {exc}")
    extent: tuple[int, int] | None = None
    if declared_shape_tuple is not None and len(declared_shape_tuple) >= 2:
        extent = (declared_shape_tuple[1], declared_shape_tuple[0])
        declared_extent = export.get("screen_extent")
        expected_extent = {"width": extent[0], "height": extent[1]}
        if declared_extent != expected_extent:
            errors.append(f"{label}: screen extent does not match NPY shape")
        if expected_dimension_class == "screen" and (
            extent[0] != expected_width or extent[1] != expected_height
        ):
            errors.append(f"{label}: NPY shape does not match bound screen resource")
    return errors, extent

def _validate_release_resource_binding_evidence(
    manifest: dict[str, Any],
    root: Path,
    *,
    label: str,
    render_mode: str,
) -> list[str]:
    errors: list[str] = []
    if render_mode not in RELEASE_RENDER_MODES:
        return [f"{label}: invalid release render mode {render_mode!r}"]
    taa_required = render_mode == "taa-on"
    evidence = manifest.get("release_resource_evidence")
    if not isinstance(evidence, dict):
        return [f"{label}: release_resource_evidence is missing"]
    if evidence.get("schema") != "mgif-csm-release-resource-binding-v1":
        errors.append(f"{label}: release resource binding schema is invalid")
    if evidence.get("render_mode") != render_mode:
        errors.append(f"{label}: release resource binding render mode differs")
    if evidence.get("taa_required") is not taa_required:
        errors.append(f"{label}: release resource binding TAA requirement differs")
    if evidence.get("passed") is not True:
        errors.append(f"{label}: release resource binding did not pass")
    if evidence.get("errors") not in ([], None):
        errors.append(f"{label}: release resource binding errors are not empty")

    passes = evidence.get("passes")
    if not isinstance(passes, dict):
        errors.append(f"{label}: release pass evidence is missing")
        passes = {}
    draw_evidence = evidence.get("draw_evidence")
    if not isinstance(draw_evidence, dict):
        errors.append(f"{label}: fullscreen draw evidence is missing")
        draw_evidence = {}
    bound_resources = evidence.get("resources")
    if not isinstance(bound_resources, dict):
        errors.append(f"{label}: bound release resources are missing")
        bound_resources = {}
    required_resource_keys = {
        "scene_depth",
        "base_color",
        "packed_normal",
        "scene_color_hdr",
        "final",
    }
    if taa_required:
        required_resource_keys.update({"velocity", "history_read", "history_write"})
    missing_resources = sorted(required_resource_keys - set(bound_resources))
    if missing_resources:
        errors.append(f"{label}: bound release resources are missing {missing_resources}")
    if not taa_required:
        forbidden = sorted(
            {"velocity", "history_read", "history_write"} & set(bound_resources)
        )
        if forbidden:
            errors.append(f"{label}: no-post bound forbidden TAA resources {forbidden}")

    draw_contracts: dict[str, tuple[str, str, str, Any]] = {
        "light": (
            "GPUDrivenLightPass",
            "fragmentHdrMain",
            "scene_color_hdr",
            re.compile(r"^GPUDrivenSceneColorHDR$"),
        ),
        "final": (
            "GPUDrivenFinalColor",
            "fragmentFinalColorMain",
            "final",
            re.compile(r"^OutputTexture$"),
        ),
    }
    if taa_required:
        draw_contracts["taa"] = (
            "GPUDrivenTAAResolve",
            "fragmentTAAResolveMain",
            "history_write",
            TAA_HISTORY_RESOURCE_RE,
        )
    if set(passes) != set(draw_contracts):
        errors.append(
            f"{label}: release pass roles {sorted(passes)!r} do not equal "
            f"{sorted(draw_contracts)!r}"
        )
    if set(draw_evidence) != set(draw_contracts):
        errors.append(
            f"{label}: release draw roles {sorted(draw_evidence)!r} do not equal "
            f"{sorted(draw_contracts)!r}"
        )
    for role, (marker_name, fragment_entry, resource_key, output_pattern) in draw_contracts.items():
        pass_record = passes.get(role)
        if not isinstance(pass_record, dict):
            errors.append(f"{label}: {role} pass evidence is missing")
            continue
        if int(pass_record.get("draw_count", 0) or 0) != 1:
            errors.append(f"{label}: {role} pass must contain exactly one draw")
        last_draw_eid = int(pass_record.get("last_draw_eid", 0) or 0)
        if last_draw_eid <= 0:
            errors.append(f"{label}: {role} last_draw_eid is invalid")
        snapshot = pass_record.get("snapshot")
        if not isinstance(snapshot, dict) or int(snapshot.get("eid", 0) or 0) != last_draw_eid:
            errors.append(f"{label}: {role} snapshot EID does not match last_draw_eid")

        draw = draw_evidence.get(role)
        if not isinstance(draw, dict):
            errors.append(f"{label}: {role} draw evidence is missing")
            continue
        if draw.get("marker_name") != marker_name:
            errors.append(f"{label}: {role} did not select exact inner marker {marker_name}")
        if draw.get("expected_marker_name") != marker_name:
            errors.append(f"{label}: {role} expected marker evidence is inconsistent")
        if int(draw.get("draw_count", 0) or 0) != 1:
            errors.append(f"{label}: {role} draw evidence count is not one")
        if draw.get("snapshot_matches_last_draw") is not True:
            errors.append(f"{label}: {role} snapshot did not bind last_draw_eid")
        if int(draw.get("draw_eid", 0) or 0) != last_draw_eid:
            errors.append(f"{label}: {role} draw evidence EID differs from pass evidence")
        action = draw.get("action")
        if not isinstance(action, dict) or action.get("is_draw") is not True:
            errors.append(f"{label}: {role} action is not a draw")
        elif (
            int(action.get("vertex_count", 0) or 0) != 3
            or int(action.get("instance_count_raw", -1)) != 1
            or int(action.get("instance_count", -1)) != 1
        ):
            errors.append(f"{label}: {role} action is not raw Draw(3,1)")
        fragment = draw.get("fragment_shader")
        if not isinstance(fragment, dict) or fragment.get("entry_point") != fragment_entry:
            errors.append(f"{label}: {role} fragment entry is not {fragment_entry}")
        elif int(fragment.get("resource_id", 0) or 0) <= 0:
            errors.append(f"{label}: {role} fragment shader resource is invalid")
        if int(draw.get("graphics_pipeline", 0) or 0) <= 0:
            errors.append(f"{label}: {role} graphics pipeline is invalid")
        output = draw.get("color_output")
        selected_resource = bound_resources.get(resource_key)
        if not isinstance(output, dict) or output_pattern.fullmatch(
            str(output.get("name", ""))
        ) is None:
            errors.append(f"{label}: {role} exact color output identity is invalid")
        elif not isinstance(selected_resource, dict) or int(
            output.get("resource_id", 0) or 0
        ) != int(selected_resource.get("resource_id", -1)):
            errors.append(f"{label}: {role} color output differs from selected resource")
        if draw.get("output_written_in_marker") is not True:
            errors.append(f"{label}: {role} output has no write usage inside marker")
        if isinstance(snapshot, dict):
            if snapshot.get("action") != action:
                errors.append(f"{label}: {role} draw action differs from pipeline snapshot")
            if snapshot.get("shaders", {}).get("ps") != fragment:
                errors.append(
                    f"{label}: {role} fragment shader differs from pipeline snapshot"
                )
            if int(snapshot.get("graphics_pipeline", 0) or 0) != int(
                draw.get("graphics_pipeline", 0) or 0
            ):
                errors.append(
                    f"{label}: {role} graphics pipeline differs from pipeline snapshot"
                )
            snapshot_outputs = [
                row
                for row in snapshot.get("outputs", [])
                if isinstance(row, dict)
                and isinstance(output, dict)
                and int(row.get("resource_id", 0) or 0)
                == int(output.get("resource_id", -1))
            ]
            if len(snapshot_outputs) != 1:
                errors.append(
                    f"{label}: {role} output is not uniquely present in pipeline snapshot"
                )

    taa = evidence.get("taa_resolve")
    if not isinstance(taa, dict):
        errors.append(f"{label}: taa_resolve evidence is missing")
        taa = {}
    if taa_required:
        if taa.get("required") is not True:
            errors.append(f"{label}: TAA resolve is not marked required")
        if taa.get("inner_marker_name") != "GPUDrivenTAAResolve":
            errors.append(f"{label}: inner GPUDrivenTAAResolve marker was not selected")
        if taa.get("snapshot_matches_last_draw") is not True:
            errors.append(f"{label}: TAA snapshot did not bind the actual resolve draw")
        if taa.get("passthrough") is not False:
            errors.append(f"{label}: taa.passthrough is forbidden for release")
        history_valid = taa.get("history_valid")
        if not isinstance(history_valid, dict):
            errors.append(f"{label}: TAA historyValid evidence is missing")
        elif history_valid.get("readable") is not True:
            errors.append(f"{label}: TAA historyValid is unreadable")
        else:
            value = history_valid.get("value")
            if not isinstance(value, (int, float)) or not math.isfinite(float(value)):
                errors.append(f"{label}: TAA historyValid is non-finite")
            elif abs(float(value) - 1.0) > 1.0e-6:
                errors.append(f"{label}: TAA historyValid={value!r}, expected 1")
        history_read = taa.get("history_read")
        history_write = taa.get("history_write")
        if not isinstance(history_read, dict) or not isinstance(history_write, dict):
            errors.append(f"{label}: TAA historyRead/historyWrite evidence is missing")
        else:
            for history_role, history in (
                ("historyRead", history_read),
                ("historyWrite", history_write),
            ):
                if history.get("logical_name") != "GPUDrivenSceneColorHistory":
                    errors.append(f"{label}: {history_role} logical resource is invalid")
                if history.get("physical_index") not in (0, 1):
                    errors.append(f"{label}: {history_role} physical index is invalid")
            if history_read.get("physical_index") == history_write.get("physical_index"):
                errors.append(f"{label}: historyRead equals historyWrite")
        descriptors = taa.get("descriptor_bindings")
        if not isinstance(descriptors, dict):
            errors.append(f"{label}: TAA/final descriptor bindings are missing")
        else:
            descriptor_contract = {
                "scene_color_hdr": (4, re.compile(r"^GPUDrivenSceneColorHDR$")),
                "velocity": (7, re.compile(r"^GPUDrivenVelocity$")),
                "history_read": (8, TAA_HISTORY_RESOURCE_RE),
                "final_history_write": (9, TAA_HISTORY_RESOURCE_RE),
            }
            for descriptor_role, (array_element, pattern) in descriptor_contract.items():
                row = descriptors.get(descriptor_role)
                if not isinstance(row, dict):
                    errors.append(f"{label}: descriptor {descriptor_role} is missing")
                    continue
                if int(row.get("array_element", -1)) != array_element:
                    errors.append(
                        f"{label}: descriptor {descriptor_role} array element is wrong"
                    )
                if pattern.fullmatch(str(row.get("resource", {}).get("name", ""))) is None:
                    errors.append(f"{label}: descriptor {descriptor_role} resource is wrong")
    else:
        absence = evidence.get("taa_absence")
        if not isinstance(absence, dict) or absence.get("passed") is not True:
            errors.append(f"{label}: no-post TAA absence proof did not pass")
        else:
            if absence.get("errors") not in ([], None):
                errors.append(f"{label}: no-post TAA absence proof contains errors")
            if absence.get("inner_markers") != []:
                errors.append(f"{label}: no-post contains an inner TAA marker")
            if absence.get("resolve_draws") != []:
                errors.append(f"{label}: no-post contains a TAA resolve draw")
            if int(absence.get("inspected_draw_count", 0) or 0) <= 0:
                errors.append(f"{label}: no-post TAA absence inspected no draws")
        if taa.get("required") is not False or taa.get("absent") is not True:
            errors.append(f"{label}: no-post taa_resolve does not prove absence")
        no_post = evidence.get("no_post")
        if not isinstance(no_post, dict) or no_post.get("required") is not True:
            errors.append(f"{label}: no-post binding evidence is missing")
        else:
            descriptor = no_post.get("descriptor_bindings", {}).get(
                "final_scene_color_hdr"
            )
            if not isinstance(descriptor, dict):
                errors.append(f"{label}: no-post final descriptor 4 evidence is missing")
            else:
                if int(descriptor.get("array_element", -1)) != 4:
                    errors.append(f"{label}: no-post final descriptor index is not 4")
                descriptor_resource = descriptor.get("resource", {})
                scene_hdr = bound_resources.get("scene_color_hdr", {})
                if int(descriptor_resource.get("resource_id", 0) or 0) != int(
                    scene_hdr.get("resource_id", -1)
                ):
                    errors.append(
                        f"{label}: no-post final descriptor 4 does not bind SceneColorHDR"
                    )

    discovery = manifest.get("discovery")
    if not isinstance(discovery, dict):
        errors.append(f"{label}: discovery map is missing")
        discovery = {}
    selected_resource_keys = {
        "light": "scene_color_hdr",
        "final": "final",
    }
    if taa_required:
        selected_resource_keys["taa"] = "history_write"
    for role, resource_key in selected_resource_keys.items():
        selected = discovery.get(role)
        expected = bound_resources.get(resource_key)
        if not isinstance(selected, dict):
            errors.append(f"{label}: selected {role} resource is missing")
            continue
        if selected.get("passthrough") is not False:
            errors.append(f"{label}: selected {role} stage is passthrough")
        if selected.get("actual_draw_binding") is not True:
            errors.append(f"{label}: selected {role} stage lacks actual draw binding")
        if not isinstance(expected, dict) or int(
            selected.get("resource", {}).get("resource_id", 0) or 0
        ) != int(expected.get("resource_id", -1)):
            errors.append(f"{label}: selected {role} resource binding is inconsistent")
    if not taa_required and discovery.get("taa") is not None:
        errors.append(f"{label}: no-post selected a TAA discovery resource")

    release_inputs = manifest.get("release_inputs")
    if not isinstance(release_inputs, dict):
        errors.append(f"{label}: release_inputs is missing")
        return errors
    if release_inputs.get("schema") != RELEASE_INPUTS_SCHEMA:
        errors.append(f"{label}: release_inputs schema is invalid")
    if release_inputs.get("render_mode") != render_mode:
        errors.append(f"{label}: release_inputs render mode differs")
    if release_inputs.get("passed") is not True:
        errors.append(f"{label}: release_inputs did not pass")
    if release_inputs.get("errors") not in ([], None):
        errors.append(f"{label}: release_inputs errors are not empty")
    validation = release_inputs.get("validation")
    if not isinstance(validation, dict):
        errors.append(f"{label}: release input validation evidence is missing")
        validation = {}
    if validation.get("schema") != "mgif-shadow-edge-extractor-input-validation-v1":
        errors.append(f"{label}: release input validation schema is invalid")
    if validation.get("render_mode") != render_mode:
        errors.append(f"{label}: release input validation render mode differs")
    if validation.get("passed") is not True or validation.get("errors") not in ([], None):
        errors.append(f"{label}: release input validation did not pass cleanly")
    expected_history_semantics = (
        "evidence-only" if taa_required else "not-applicable-no-post"
    )
    if validation.get("history_read_semantics") != expected_history_semantics:
        errors.append(f"{label}: HistoryRead semantics are invalid for {render_mode}")
    resources = release_inputs.get("resources")
    if not isinstance(resources, dict):
        errors.append(f"{label}: release_inputs.resources is missing")
        resources = {}
    common_roles = (
        "scene_depth",
        "base_color",
        "world_normals",
        "scene_color_hdr",
        "final",
    )
    required_roles = (
        common_roles[:4] + ("history_read", "history_write") + common_roles[4:]
        if taa_required
        else common_roles
    )
    if set(resources) != set(required_roles):
        errors.append(
            f"{label}: release input roles {sorted(resources)!r} do not equal "
            f"{sorted(required_roles)!r}"
        )
    extents: dict[str, tuple[int, int]] = {}
    for role in required_roles:
        row = resources.get(role)
        if not isinstance(row, dict):
            errors.append(f"{label}: release input {role} is missing")
            continue
        expected_evidence_only = role == "history_read"
        if row.get("evidence_only") is not expected_evidence_only:
            errors.append(
                f"{label}: release input {role}.evidence_only must be "
                f"{expected_evidence_only}"
            )
        resource_key = "packed_normal" if role == "world_normals" else role
        expected_resource = bound_resources.get(resource_key)
        row_resource = row.get("resource")
        if not isinstance(row_resource, dict) or not isinstance(expected_resource, dict):
            errors.append(f"{label}: release input {role} resource evidence is missing")
            continue
        if int(row_resource.get("resource_id", 0) or 0) != int(
            expected_resource.get("resource_id", -1)
        ):
            errors.append(f"{label}: release input {role} resource binding differs")
        export_errors, extent = _validate_host_npy_export(
            root,
            row.get("export"),
            label=f"{label}: release input {role}",
            role=role,
            resource=row_resource,
            expected_channel_order=RELEASE_EXPORT_CHANNEL_ORDERS[role],
            expected_evidence_only=expected_evidence_only,
            expected_dimension_class="screen",
        )
        errors.extend(export_errors)
        if extent is not None:
            extents[role] = extent
    unique_extents = sorted(set(extents.values()))
    if len(extents) != len(required_roles) or len(unique_extents) != 1:
        errors.append(f"{label}: release screen dimensions are not identical: {extents!r}")
        screen_extent = None
    else:
        screen_extent = {
            "width": unique_extents[0][0],
            "height": unique_extents[0][1],
        }
    if release_inputs.get("screen_extent") != screen_extent:
        errors.append(f"{label}: release_inputs.screen_extent is inconsistent")
    if validation.get("screen_extent") != screen_extent:
        errors.append(f"{label}: validation.screen_extent is inconsistent")

    camera = release_inputs.get("camera")
    if not isinstance(camera, dict):
        errors.append(f"{label}: unjittered camera evidence is missing")
    else:
        if camera.get("errors") not in ([], None):
            errors.append(f"{label}: unjittered camera evidence contains errors")
        if camera.get("matrix_convention") != "column_vectors":
            errors.append(f"{label}: camera matrix convention is invalid")
        if camera.get("framebuffer_y_to_ndc") != "top_to_negative_one":
            errors.append(f"{label}: camera framebuffer Y-to-NDC convention is invalid")
        if camera.get("depth_range") != "zero_to_one":
            errors.append(f"{label}: camera depth range is not Vulkan zero_to_one")
        for field in (
            "clip_from_world",
            "world_from_clip",
            "view_from_world",
            "world_from_view",
        ):
            matrix = camera.get(field)
            if not (
                isinstance(matrix, list)
                and len(matrix) == 4
                and all(isinstance(row, list) and len(row) == 4 for row in matrix)
            ):
                errors.append(f"{label}: camera.{field} is not a 4x4 matrix")
        inverse_error = camera.get("inverse_max_abs_error")
        if not isinstance(inverse_error, (int, float)) or not math.isfinite(
            float(inverse_error)
        ) or float(inverse_error) > 5.0e-3:
            errors.append(f"{label}: camera inverse-pair error is invalid")
        viewport = camera.get("viewport")
        if screen_extent is not None and viewport != [
            0,
            0,
            screen_extent["width"],
            screen_extent["height"],
        ]:
            errors.append(f"{label}: camera viewport does not match screen resources")
    return errors

def _validate_taa_history_ping_pong(
    extracted: dict[str, tuple[dict[str, Any], Path]],
) -> dict[str, Any]:
    observed: dict[str, dict[str, Any]] = {}
    errors: list[str] = []
    expected_write = {
        boundary: RELEASE_BOUNDARY_FRAMES[boundary] & 1
        for boundary in SMOKE_BOUNDARIES
    }
    for boundary in SMOKE_BOUNDARIES:
        manifest = extracted[boundary][0]
        taa = manifest.get("release_resource_evidence", {}).get("taa_resolve", {})
        write = taa.get("history_write") if isinstance(taa, dict) else None
        read = taa.get("history_read") if isinstance(taa, dict) else None
        write_index = write.get("physical_index") if isinstance(write, dict) else None
        read_index = read.get("physical_index") if isinstance(read, dict) else None
        frame_markers = manifest.get("automation_frame_markers", [])
        frame = (
            frame_markers[0].get("frame")
            if isinstance(frame_markers, list)
            and len(frame_markers) == 1
            and isinstance(frame_markers[0], dict)
            else None
        )
        observed[boundary] = {
            "frame": frame,
            "history_write_physical_index": write_index,
            "history_read_physical_index": read_index,
            "expected_write_physical_index": expected_write[boundary],
        }
        if frame != RELEASE_BOUNDARY_FRAMES[boundary]:
            errors.append(f"{boundary}: TAA ping-pong frame marker is invalid")
        if write_index != expected_write[boundary]:
            errors.append(
                f"{boundary}: historyWrite index {write_index!r}, expected "
                f"{expected_write[boundary]} for frame {RELEASE_BOUNDARY_FRAMES[boundary]}"
            )
        if read_index not in (0, 1) or read_index == write_index:
            errors.append(f"{boundary}: historyRead is not the opposite physical image")
    sequence = [
        observed[boundary]["history_write_physical_index"]
        for boundary in SMOKE_BOUNDARIES
    ]
    if sequence != [1, 0, 1]:
        errors.append(f"TAA historyWrite sequence is {sequence!r}, expected [1, 0, 1]")
    for before, after in SAME_POSE_PAIRS:
        frame_delta = RELEASE_BOUNDARY_FRAMES[after] - RELEASE_BOUNDARY_FRAMES[before]
        expected_toggle = frame_delta & 1
        before_index = observed[before]["history_write_physical_index"]
        after_index = observed[after]["history_write_physical_index"]
        if before_index in (0, 1) and after_index in (0, 1):
            if (before_index ^ after_index) != expected_toggle:
                errors.append(
                    f"{before}->{after}: history ping-pong toggle does not match "
                    f"frame delta parity {frame_delta & 1}"
                )
    return {
        "passed": not errors,
        "logical_resource": "GPUDrivenSceneColorHistory",
        "expected_write_sequence": [1, 0, 1],
        "observed": observed,
        "errors": errors,
    }

def _validate_release_extraction_contract(
    manifest: dict[str, Any],
    root: Path,
    *,
    label: str,
    render_mode: str,
) -> list[str]:
    errors: list[str] = []
    if render_mode not in RELEASE_RENDER_MODES:
        return [f"{label}: invalid release render mode {render_mode!r}"]
    taa_required = render_mode == "taa-on"
    if manifest.get("tool_version") != TOOL_VERSION:
        errors.append(
            f"{label}: extraction tool_version is {manifest.get('tool_version')!r}, "
            f"expected {TOOL_VERSION!r}"
        )
    extractor_tool = manifest.get("extractor_tool_evidence")
    try:
        current_tool = _collect_tool_file_evidence(
            Path(__file__),
            role="comparator",
        )
    except Exception as exc:
        errors.append(
            f"{label}: current comparator tool evidence failed: "
            f"{type(exc).__name__}: {exc}"
        )
        current_tool = None
    if not isinstance(extractor_tool, dict):
        errors.append(f"{label}: extractor_tool_evidence is missing")
    elif current_tool is not None:
        for field in ("role", "absolute_path", "version", "size_bytes", "sha256"):
            if extractor_tool.get(field) != current_tool.get(field):
                errors.append(f"{label}: extractor tool {field} differs from comparator")
        if extractor_tool.get("read_consistent") is not True:
            errors.append(f"{label}: extractor tool read was not stable")
    if manifest.get("release_gate") is not True:
        errors.append(f"{label}: extractor did not run in release-gate mode")
    if manifest.get("release_render_mode") != render_mode:
        errors.append(f"{label}: extractor release render mode differs")
    if manifest.get("errors"):
        errors.extend(f"{label}: extraction: {error}" for error in manifest["errors"])

    budget = manifest.get("export_budget")
    if not isinstance(budget, dict):
        errors.append(f"{label}: daemon export budget evidence is missing")
    else:
        if budget.get("schema") != "mgif-rdc-daemon-export-budget-v1":
            errors.append(f"{label}: daemon export budget schema is invalid")
        if budget.get("export_budget_bytes") != COMPARATOR_EXTRACT_BYTES_PER_CAPTURE:
            errors.append(f"{label}: daemon per-capture export budget differs")
        if budget.get("single_readback_cap_bytes") != COMPARATOR_MAX_READBACK_BYTES:
            errors.append(f"{label}: daemon single-readback cap differs")
        free_start = _manifest_integer(budget.get("free_bytes_at_start"))
        required_start = _manifest_integer(budget.get("required_free_bytes_at_start"))
        if free_start is None or required_start is None or free_start < required_start:
            errors.append(f"{label}: daemon initial free-space proof is invalid")
        for field in (
            "readback_expected_bytes",
            "readback_actual_bytes",
            "npy_committed_bytes",
        ):
            value = _manifest_integer(budget.get(field))
            if value is None or value < 0 or value > COMPARATOR_EXTRACT_BYTES_PER_CAPTURE:
                errors.append(f"{label}: daemon export budget {field} is invalid")
        if budget.get("readback_expected_bytes") != budget.get("readback_actual_bytes"):
            errors.append(f"{label}: daemon expected/actual readback totals differ")
        readbacks = budget.get("readbacks")
        writes = budget.get("writes")
        if not isinstance(readbacks, list) or not readbacks:
            errors.append(f"{label}: daemon readback budget has no records")
        elif any(
            not isinstance(row, dict)
            or row.get("completed") is not True
            or row.get("actual_bytes") != row.get("expected_bytes")
            or int(row.get("actual_bytes", 0) or 0) > COMPARATOR_MAX_READBACK_BYTES
            for row in readbacks
        ):
            errors.append(f"{label}: daemon readback budget contains an invalid record")
        if not isinstance(writes, list) or not writes:
            errors.append(f"{label}: daemon NPY write budget has no records")
        elif any(
            not isinstance(row, dict)
            or row.get("completed") is not True
            or int(row.get("actual_file_bytes", 0) or 0) <= 0
            or int(row.get("actual_file_bytes", 0) or 0)
            > int(row.get("estimated_write_bytes", -1) or -1)
            or int(row.get("free_bytes_before_write", -1) or -1)
            < int(row.get("required_free_bytes_before_write", 0) or 0)
            for row in writes
        ):
            errors.append(f"{label}: daemon NPY write budget contains an invalid record")

    cleanup = manifest.get("session_cleanup")
    if not isinstance(cleanup, dict):
        errors.append(f"{label}: RenderDoc extraction cleanup is missing")
    else:
        if cleanup.get("schema") != "mgif-rdc-comparator-replay-session-cleanup-v4":
            errors.append(f"{label}: RenderDoc extraction cleanup schema is invalid")
        if cleanup.get("closed") is not True or cleanup.get("passed") is not True:
            errors.append(f"{label}: RenderDoc extraction session was not closed/passed")
        if cleanup.get("errors") not in ([], None):
            errors.append(f"{label}: RenderDoc extraction cleanup contains errors")
        errors.extend(
            _validate_direct_shutdown_cleanup_evidence(
                cleanup,
                label=f"{label}: extraction cleanup",
                require_handle_close=True,
            )
        )
        if _rdc_status_classification(cleanup.get("post_status")) != "inactive":
            errors.append(f"{label}: extraction post-status is not exactly inactive")
        state_file = cleanup.get("state_file")
        if not isinstance(state_file, dict) or state_file.get(
            "absent_after_cleanup"
        ) is not True:
            errors.append(f"{label}: extraction session state file absence is unproven")
        deadline = cleanup.get("deadline")
        if not isinstance(deadline, dict) or deadline.get("exceeded") is not False:
            errors.append(f"{label}: extraction deadline did not finish cleanly")
        ownership = cleanup.get("daemon_ownership")
        if not isinstance(ownership, dict):
            errors.append(f"{label}: replay daemon ownership is missing")
        else:
            if ownership.get("established") is not True:
                errors.append(f"{label}: replay daemon ownership was not established")
            if ownership.get("errors") not in ([], None):
                errors.append(f"{label}: replay daemon ownership contains errors")
            for field in (
                "state_path_match",
                "state_capture_path_match",
                "daemon_capture_path_metadata_match",
                "stable_image_match",
                "creation_time_matches_handle",
                "command_bound_to_held_identity",
                "state_publication_boundary_exact",
                "state_file_volume_verified",
                "exact_creation_order_clock_match",
                "strict_creation_precedes_publication",
                "process_created_for_open",
                "state_pid_matches_handle",
                "snapshot_evidence_complete",
                "process_created_before_state_file",
            ):
                if ownership.get(field) is not True:
                    errors.append(f"{label}: replay daemon ownership {field} is not true")
            stable = ownership.get("stable_process_identity")
            errors.extend(
                _validate_stable_process_identity_evidence(
                    stable,
                    label=f"{label}: replay daemon ownership",
                )
            )
        if cleanup.get("owned_daemon_absent") is not True:
            errors.append(f"{label}: owned replay daemon absence is unproven")
        if cleanup.get("owned_daemon_residue") != []:
            errors.append(f"{label}: owned replay daemon residue is not empty")
        handle_close = cleanup.get("daemon_process_handle_close")
        if not isinstance(handle_close, dict) or handle_close.get("closed") is not True:
            errors.append(f"{label}: replay daemon held handle did not close")

    identity = manifest.get("capture_file_identity")
    if not isinstance(identity, dict):
        errors.append(f"{label}: capture_file_identity is missing")
    else:
        if not re.fullmatch(r"[0-9a-f]{64}", str(identity.get("sha256", ""))):
            errors.append(f"{label}: capture_file_identity.sha256 is invalid")
        size_bytes = _manifest_integer(identity.get("size_bytes"))
        if size_bytes is None or not (0 < size_bytes <= COMPARATOR_MAX_RDC_BYTES):
            errors.append(f"{label}: capture_file_identity.size_bytes violates the RDC cap")
        if identity.get("renderdoc_reported_path_match") is not True:
            errors.append(f"{label}: RenderDoc replay path was not exactly rebound")
    runtime_disk_budget = manifest.get("runtime_disk_budget")
    if not isinstance(runtime_disk_budget, dict) or runtime_disk_budget.get("passed") is not True:
        errors.append(f"{label}: host runtime disk budget is missing or failed")
    else:
        if runtime_disk_budget.get("single_rdc_cap_bytes") != COMPARATOR_MAX_RDC_BYTES:
            errors.append(f"{label}: host runtime RDC cap differs")
        identity_size = identity.get("size_bytes", -1) if isinstance(identity, dict) else -1
        if runtime_disk_budget.get("capture_size_bytes") != identity_size:
            errors.append(f"{label}: host runtime RDC size does not match identity")
        if int(runtime_disk_budget.get("free_bytes_before_open", -1) or -1) < int(
            runtime_disk_budget.get("required_free_bytes_before_open", 0) or 0
        ):
            errors.append(f"{label}: host pre-open free-space proof is invalid")

    markers = manifest.get("markers")
    if not isinstance(markers, dict):
        errors.append(f"{label}: selected marker map is missing")
        markers = {}
    discovery = manifest.get("discovery")
    if not isinstance(discovery, dict):
        errors.append(f"{label}: discovery map is missing")
        discovery = {}
    required_roles = ("csm", "light", "taa", "final") if taa_required else (
        "csm",
        "light",
        "final",
    )
    for role in required_roles:
        marker = markers.get(role)
        if not isinstance(marker, dict):
            errors.append(f"{label}: {role} marker is missing")
        elif marker.get("selection", {}).get("ambiguous") is not False:
            errors.append(f"{label}: {role} marker selection is ambiguous")
        selected = discovery.get(role)
        if not isinstance(selected, dict):
            errors.append(f"{label}: {role} resource discovery is missing")
        elif selected.get("selection", {}).get("ambiguous") is not False:
            errors.append(f"{label}: {role} resource selection is ambiguous")
    if not taa_required:
        if markers.get("taa") is not None:
            errors.append(f"{label}: no-post selected a TAA marker")
        if discovery.get("taa") is not None:
            errors.append(f"{label}: no-post selected a TAA resource")
    snapshots = manifest.get("pipeline_snapshots")
    if not isinstance(snapshots, dict):
        errors.append(f"{label}: pipeline_snapshots is missing")
    else:
        for role in required_roles:
            rows = snapshots.get(role)
            if not isinstance(rows, list) or not rows:
                errors.append(f"{label}: {role} pipeline snapshots are empty")
                continue
            for row in rows:
                if not isinstance(row, dict) or row.get("error"):
                    errors.append(f"{label}: {role} pipeline snapshot contains an error")
                    break
        if not taa_required and snapshots.get("taa") not in ([], None):
            errors.append(f"{label}: no-post contains TAA pipeline snapshots")

    exports = manifest.get("exports")
    if not isinstance(exports, dict):
        errors.append(f"{label}: exports map is missing")
        return errors
    csm_exports = exports.get("csm")
    csm_selected = discovery.get("csm")
    csm_resource = (
        csm_selected.get("resource") if isinstance(csm_selected, dict) else None
    )
    if not isinstance(csm_resource, dict):
        errors.append(f"{label}: CSM resource metadata is missing")
    else:
        if int(csm_resource.get("array_size", 0) or 0) != 4:
            errors.append(f"{label}: release CSM array size is not exactly four")
        if int(csm_resource.get("samples", 0) or 0) != 1:
            errors.append(f"{label}: release CSM samples is not exactly one")
        if int(csm_resource.get("depth", 0) or 0) != 1:
            errors.append(f"{label}: release CSM texture depth is not exactly one")
    if not isinstance(csm_exports, list) or len(csm_exports) != 4:
        errors.append(f"{label}: release requires exactly four CSM exports")
        csm_exports = []
    observed_slices: list[int] = []
    if isinstance(csm_resource, dict):
        for index, export in enumerate(csm_exports):
            try:
                observed_slices.append(int(export.get("slice", -1)))
            except (AttributeError, TypeError, ValueError):
                observed_slices.append(-1)
            export_errors, _ = _validate_host_npy_export(
                root,
                export,
                label=f"{label}: csm[{index}]",
                role=f"csm[{index}]",
                resource=csm_resource,
                expected_channel_order=["D"],
                expected_evidence_only=False,
                expected_dimension_class="csm",
            )
            errors.extend(export_errors)
    if observed_slices != [0, 1, 2, 3]:
        errors.append(
            f"{label}: CSM export slices are {observed_slices!r}, expected [0, 1, 2, 3]"
        )
    projection_sources = manifest.get("projection_sources")
    if not isinstance(projection_sources, list) or len(projection_sources) != 4:
        errors.append(f"{label}: release requires four CSM projection evidences")
    else:
        projection_layers: list[int] = []
        for row in projection_sources:
            if not isinstance(row, dict):
                projection_layers.append(-1)
                continue
            projection_layers.append(int(row.get("cascade", -1)))
            if int(row.get("eid", 0) or 0) <= 0:
                errors.append(f"{label}: CSM projection evidence has invalid EID")
            if row.get("error"):
                errors.append(f"{label}: CSM projection evidence contains an error")
            matrices = row.get("matrices")
            if not isinstance(matrices, list) or not matrices:
                errors.append(f"{label}: CSM projection evidence has no matrices")
        if projection_layers != [0, 1, 2, 3]:
            errors.append(
                f"{label}: CSM projection cascades are {projection_layers!r}, "
                "expected [0, 1, 2, 3]"
            )

    screen_export_roles = ("light", "taa", "final") if taa_required else (
        "light",
        "final",
    )
    for role in screen_export_roles:
        row = exports.get(role)
        if not isinstance(row, dict):
            errors.append(f"{label}: {role} export is missing")
        elif row.get("kind") != "npy":
            errors.append(
                f"{label}: {role} uses {row.get('kind')!r}; "
                "release gate forbids PNG fallback"
            )
    if not taa_required and exports.get("taa") is not None:
        errors.append(f"{label}: no-post contains a TAA export")
    errors.extend(
        _validate_release_resource_binding_evidence(
            manifest,
            root,
            label=label,
            render_mode=render_mode,
        )
    )
    return errors

def _validate_automation_frame_marker(
    manifest: dict[str, Any],
    case: dict[str, Any],
    boundary: str,
) -> dict[str, Any]:
    errors: list[str] = []
    markers = manifest.get("automation_frame_markers")
    if not isinstance(markers, list):
        markers = []
    if len(markers) != 1:
        errors.append(
            f"expected exactly one CSM_AUTOMATION_FRAME marker, found {len(markers)}"
        )
        marker: dict[str, Any] | None = None
    else:
        marker = markers[0] if isinstance(markers[0], dict) else None
        if marker is None:
            errors.append("CSM_AUTOMATION_FRAME marker record is invalid")
    ready = case.get("boundaries", {}).get(boundary, {}).get("ready", {})
    expected_frame = ready.get("frame") if isinstance(ready, dict) else None
    declared_frame = case.get("expected_automation_frames", {}).get(boundary)
    if expected_frame is None:
        errors.append(f"manifest ready.frame is missing for {boundary}")
    if declared_frame is not None and declared_frame != expected_frame:
        errors.append(
            f"manifest expected_automation_frames.{boundary}={declared_frame!r} "
            f"does not match ready.frame={expected_frame!r}"
        )
    if marker is not None:
        expected = {
            "mode": case.get("mode"),
            "boundary": boundary,
            "frame": expected_frame,
        }
        for key, value in expected.items():
            if marker.get(key) != value:
                errors.append(
                    f"RDC marker {key}={marker.get(key)!r}, expected {value!r}"
                )
        if marker.get("nested_in_gpu_driven_csm_shadow") is not True:
            errors.append(
                "CSM_AUTOMATION_FRAME is not nested in GPUDrivenCSMShadow"
            )
    return {
        "passed": not errors,
        "expected": {
            "mode": case.get("mode"),
            "boundary": boundary,
            "frame": expected_frame,
        },
        "observed": marker,
        "errors": errors,
    }


def _resource_name_semantics(resource: dict[str, Any]) -> dict[str, str]:
    observed = str(resource.get("name", "") or "")
    try:
        resource_id = int(resource.get("resource_id"))
    except (TypeError, ValueError):
        resource_id = None
    autogenerated = resource.get("autogenerated_name")

    def strip_matching_id() -> str | None:
        match = re.fullmatch(r"(?P<base>.*\S)\s+(?P<resource_id>[0-9]+)", observed)
        if match is None or resource_id is None:
            return None
        if int(match.group("resource_id")) != resource_id:
            return None
        return match.group("base")

    if autogenerated is True:
        return {
            "kind": "renderdoc_autogenerated",
            "normalized": strip_matching_id() or observed,
        }
    if autogenerated is False:
        return {
            "kind": "application_debug_name",
            "normalized": observed,
        }

    fallback_match = RENDERDOC_AUTOGENERATED_RESOURCE_NAME_RE.fullmatch(observed)
    if (
        fallback_match is not None
        and resource_id is not None
        and int(fallback_match.group("resource_id")) == resource_id
    ):
        return {
            "kind": "renderdoc_autogenerated",
            "normalized": fallback_match.group("base"),
        }
    return {
        "kind": "unclassified",
        "normalized": observed,
    }


def _resource_semantic_descriptor(
    manifest: dict[str, Any],
    role: str,
) -> dict[str, Any] | None:
    selected = manifest.get("discovery", {}).get(role)
    if not isinstance(selected, dict) or not isinstance(selected.get("resource"), dict):
        return None
    resource = selected["resource"]
    resource_name = _resource_name_semantics(resource)
    taa_history_match = (
        TAA_HISTORY_RESOURCE_RE.fullmatch(str(resource.get("name", "")))
        if role == "taa"
        else None
    )
    resource_fields = (
        "is_texture",
        "width",
        "height",
        "depth",
        "array_size",
        "mips",
        "samples",
        "byte_size",
        "format",
        "format_type",
        "component_type",
        "component_count",
        "component_byte_width",
        "bgra",
        "srgb",
        "creation_flags",
    )
    semantic_resource = {
        key: resource.get(key)
        for key in resource_fields
    }
    semantic_resource.update(
        {
            "name": (
                "GPUDrivenSceneColorHistory"
                if taa_history_match is not None
                else resource_name["normalized"]
            ),
            "name_kind": (
                "application_debug_logical_history"
                if taa_history_match is not None
                else resource_name["kind"]
            ),
        }
    )
    marker = selected.get("marker", {})
    exports = manifest.get("exports", {}).get(role)
    if role == "csm":
        export_semantics = [
            {
                key: export.get(key)
                for key in ("kind", "shape", "dtype", "slice")
            }
            for export in exports
        ] if isinstance(exports, list) else None
    else:
        export_semantics = {
            key: exports.get(key)
            for key in ("kind", "shape", "dtype", "slice")
        } if isinstance(exports, dict) else None
    usage_counts: dict[str, int] = {}
    for entry in manifest.get("selected_resource_usage", {}).get(role, []):
        if not isinstance(entry, dict):
            continue
        usage = str(entry.get("usage", ""))
        usage_counts[usage] = usage_counts.get(usage, 0) + 1
    return {
        "role": role,
        "resource": semantic_resource,
        "marker_name": marker.get("name"),
        "marker_ancestors": marker.get("ancestors"),
        "passthrough": bool(selected.get("passthrough", False)),
        "exports": export_semantics,
        "usage_counts": usage_counts,
    }


def _validate_three_capture_resource_semantics(
    extracted: dict[str, tuple[dict[str, Any], Path]],
    *,
    render_mode: str,
) -> dict[str, Any]:
    errors: list[str] = []
    roles: dict[str, Any] = {}
    required_roles = (
        ROLE_ORDER if render_mode == "taa-on" else ("csm", "light", "final")
    )
    for role in required_roles:
        per_boundary: dict[str, Any] = {}
        hashes: dict[str, str] = {}
        for boundary in SMOKE_BOUNDARIES:
            manifest = extracted[boundary][0]
            descriptor = _resource_semantic_descriptor(manifest, role)
            if descriptor is None:
                errors.append(f"{boundary}: {role} semantic descriptor is unavailable")
                continue
            fingerprint = _canonical_json_sha256(descriptor)
            hashes[boundary] = fingerprint
            selected = manifest.get("discovery", {}).get(role, {})
            resource = selected.get("resource", {}) if isinstance(selected, dict) else {}
            history_match = (
                TAA_HISTORY_RESOURCE_RE.fullmatch(str(resource.get("name", "")))
                if role == "taa"
                else None
            )
            per_boundary[boundary] = {
                "fingerprint": fingerprint,
                "descriptor": descriptor,
                "resource_name_evidence": {
                    "observed": resource.get("name"),
                    "autogenerated_name": resource.get("autogenerated_name"),
                    "logical_name": (
                        "GPUDrivenSceneColorHistory" if history_match else None
                    ),
                    "physical_index": (
                        int(history_match.group("index")) if history_match else None
                    ),
                    **_resource_name_semantics(resource),
                },
            }
        if len(set(hashes.values())) > 1:
            errors.append(
                f"{role} resource semantic fingerprint differs across boundaries: {hashes}"
            )
        roles[role] = per_boundary
    return {"passed": not errors, "roles": roles, "errors": errors}


def _validate_release_capture_bindings(
    manifest_path: Path,
    source: dict[str, Any],
    case: dict[str, Any],
    captures: dict[str, Path],
) -> dict[str, Any]:
    errors: list[str] = []
    identities: dict[str, dict[str, Any]] = {}
    try:
        run_started = _parse_utc(source.get("started_utc"), "manifest.started_utc")
        run_completed = _parse_utc(source.get("completed_utc"), "manifest.completed_utc")
        case_started = _parse_utc(case.get("started_utc"), "case.started_utc")
        case_completed = _parse_utc(case.get("completed_utc"), "case.completed_utc")
    except ComparatorError as exc:
        errors.append(str(exc))
        run_started = case_started = datetime.min.replace(tzinfo=timezone.utc)
        run_completed = case_completed = datetime.max.replace(tzinfo=timezone.utc)
    case_directory_value = case.get("case_directory")
    case_directory = Path(case_directory_value) if isinstance(case_directory_value, str) else None
    paths: dict[str, str] = {}
    hashes: dict[str, str] = {}
    capture_ids: dict[str, int] = {}
    for boundary in SMOKE_BOUNDARIES:
        path = captures.get(boundary)
        entry = case.get("boundaries", {}).get(boundary)
        if path is None or not isinstance(entry, dict):
            errors.append(f"{boundary}: resolved capture or manifest entry is missing")
            continue
        resolved = path.resolve()
        paths[boundary] = _path_key(resolved)
        declared_path = entry.get("capture_path")
        if not isinstance(declared_path, str) or not declared_path.strip():
            errors.append(f"{boundary}: capture_path is missing")
        else:
            declared = Path(declared_path)
            if not declared.is_absolute():
                declared = manifest_path.parent / declared
            if _path_key(declared) != _path_key(resolved):
                errors.append(f"{boundary}: release gate forbids capture path fallback")
        if case_directory is None or not _path_is_within(resolved, case_directory):
            errors.append(f"{boundary}: capture is outside the current case_directory")
        identity = _file_identity(resolved)
        identities[boundary] = identity
        hashes[boundary] = identity["sha256"]
        recorded_size = entry.get("capture_size_bytes")
        if not isinstance(recorded_size, int) or recorded_size != identity["size_bytes"]:
            errors.append(
                f"{boundary}: capture_size_bytes={recorded_size!r} does not match "
                f"{identity['size_bytes']}"
            )
        expected_hash = _expected_capture_sha256(entry)
        identity["manifest_sha256"] = expected_hash
        if expected_hash is not None and expected_hash != identity["sha256"]:
            errors.append(f"{boundary}: capture SHA-256 does not match manifest metadata")
        capture_id = entry.get("capture_id")
        capture_id_int = _manifest_integer(capture_id)
        if capture_id_int is None or capture_id_int < 0:
            errors.append(
                f"{boundary}: capture_id must be a non-negative integer"
            )
        else:
            capture_ids[boundary] = capture_id_int
        renderdoc_capture = entry.get("renderdoc_capture")
        if not isinstance(renderdoc_capture, dict):
            errors.append(f"{boundary}: renderdoc_capture metadata is missing")
        else:
            observed_id = _manifest_integer(renderdoc_capture.get("captureId"))
            if observed_id is None or observed_id < 0:
                errors.append(
                    f"{boundary}: renderdoc_capture.captureId must be a "
                    "non-negative integer"
                )
            elif capture_id_int is not None and observed_id != capture_id_int:
                errors.append(
                    f"{boundary}: renderdoc_capture.captureId does not match capture_id"
                )
        if entry.get("validated_target_frame") is not True:
            errors.append(f"{boundary}: validated_target_frame is not true")
        pose_binding = entry.get("capture_pose_binding")
        if not isinstance(pose_binding, dict) or pose_binding.get("passed") is not True:
            errors.append(f"{boundary}: capture_pose_binding.passed is not true")
        if not isinstance(entry.get("capture_observation"), dict):
            errors.append(f"{boundary}: capture_observation metadata is missing")
        for field in ("capture_observed_utc", "copy_started_utc", "capture_copied_utc"):
            try:
                timestamp = _parse_utc(entry.get(field), f"{boundary}.{field}")
                if not (run_started <= timestamp <= run_completed):
                    errors.append(f"{boundary}.{field} is outside the manifest run window")
                if not (case_started <= timestamp <= case_completed):
                    errors.append(f"{boundary}.{field} is outside the case run window")
            except ComparatorError as exc:
                errors.append(str(exc))
    if len(paths) == len(SMOKE_BOUNDARIES) and len(set(paths.values())) != len(paths):
        errors.append("three boundary captures must resolve to three different files")
    if len(hashes) == len(SMOKE_BOUNDARIES) and len(set(hashes.values())) != len(hashes):
        errors.append("three boundary captures must have three different SHA-256 hashes")
    if len(capture_ids) == len(SMOKE_BOUNDARIES) and len(set(capture_ids.values())) != len(capture_ids):
        errors.append("three boundary capture_id values must be different")
    return {
        "passed": not errors,
        "identities": identities,
        "paths": paths,
        "capture_ids": capture_ids,
        "errors": errors,
    }

def _csm_exports_by_layer(
    manifest: dict[str, Any],
    *,
    label: str,
    errors: list[str],
) -> dict[int, dict[str, Any]]:
    exports = manifest.get("exports", {}).get("csm", [])
    by_layer: dict[int, dict[str, Any]] = {}
    for index, export in enumerate(exports):
        try:
            layer = int(export.get("slice", index))
        except (TypeError, ValueError):
            errors.append(f"{label}: invalid CSM layer index in export {index}")
            continue
        if layer in by_layer:
            errors.append(f"{label}: duplicate CSM layer export {layer}")
            continue
        by_layer[layer] = export
    return by_layer


def _shadow_edge_module_api() -> tuple[Any, Any, Any, Any]:
    try:
        from rdc_shadow_edge_metrics import (
            CameraMatrices,
            ShadowEdgeMetricsConfig,
            ShadowFrame,
            evaluate_shadow_edge_metrics,
        )
    except ImportError:
        import importlib.util

        candidates = (
            Path(__file__).resolve().with_name("rdc_shadow_edge_metrics.py"),
            Path.cwd() / "tools" / "rdc_shadow_edge_metrics.py",
        )
        module_path = next((path for path in candidates if path.is_file()), None)
        if module_path is None:
            raise
        module_name = "mgif_rdc_shadow_edge_metrics"
        spec = importlib.util.spec_from_file_location(module_name, module_path)
        if spec is None or spec.loader is None:
            raise ImportError(f"cannot load shadow-edge module from {module_path}")
        module = importlib.util.module_from_spec(spec)
        sys.modules[module_name] = module
        spec.loader.exec_module(module)
        CameraMatrices = module.CameraMatrices
        ShadowEdgeMetricsConfig = module.ShadowEdgeMetricsConfig
        ShadowFrame = module.ShadowFrame
        evaluate_shadow_edge_metrics = module.evaluate_shadow_edge_metrics
    return (
        CameraMatrices,
        ShadowEdgeMetricsConfig,
        ShadowFrame,
        evaluate_shadow_edge_metrics,
    )


def _release_input_array(
    manifest: dict[str, Any],
    root: Path,
    role: str,
) -> Any:
    inputs = manifest.get("release_inputs")
    if not isinstance(inputs, dict) or inputs.get("passed") is not True:
        raise ComparatorError(f"release inputs are unavailable for {role}")
    row = inputs.get("resources", {}).get(role)
    if not isinstance(row, dict) or not isinstance(row.get("export"), dict):
        raise ComparatorError(f"release input {role} is missing")
    return _load_export(root, row["export"])


def _release_shadow_frame(
    manifest: dict[str, Any],
    root: Path,
    *,
    render_mode: str,
) -> Any:
    import numpy as np

    CameraMatrices, _, ShadowFrame, _ = _shadow_edge_module_api()
    inputs = manifest.get("release_inputs")
    if not isinstance(inputs, dict):
        raise ComparatorError("release_inputs is missing")
    if inputs.get("render_mode") != render_mode:
        raise ComparatorError("release input render mode differs from comparison profile")
    camera = inputs.get("camera")
    if not isinstance(camera, dict):
        raise ComparatorError("release camera evidence is missing")
    clip_from_world = np.asarray(camera.get("clip_from_world"), dtype=np.float64)
    world_from_clip = np.asarray(camera.get("world_from_clip"), dtype=np.float64)
    view_from_world_value = camera.get("view_from_world")
    view_from_world = (
        np.asarray(view_from_world_value, dtype=np.float64)
        if view_from_world_value is not None
        else None
    )
    world_from_view_value = camera.get("world_from_view")
    world_from_view = (
        np.asarray(world_from_view_value, dtype=np.float64)
        if world_from_view_value is not None
        else None
    )
    viewport_value = camera.get("viewport")
    viewport = (
        tuple(int(value) for value in viewport_value)
        if isinstance(viewport_value, list) and len(viewport_value) == 4
        else None
    )
    scene_color_hdr = _release_input_array(manifest, root, "scene_color_hdr")
    history_write = (
        _release_input_array(manifest, root, "history_write")
        if render_mode == "taa-on"
        else scene_color_hdr
    )
    return ShadowFrame(
        scene_depth=_release_input_array(manifest, root, "scene_depth"),
        world_normals=_release_input_array(manifest, root, "world_normals"),
        base_color=_release_input_array(manifest, root, "base_color"),
        scene_color_hdr=scene_color_hdr,
        history_write=history_write,
        final=_release_input_array(manifest, root, "final"),
        camera=CameraMatrices(
            clip_from_world=clip_from_world,
            framebuffer_y_to_ndc=str(camera.get("framebuffer_y_to_ndc", "")),
            world_from_clip=world_from_clip,
            view_from_world=view_from_world,
            world_from_view=world_from_view,
            viewport=viewport,
            depth_range=str(camera.get("depth_range", "zero_to_one")),
        ),
    )


def _evaluate_release_shadow_edge_metrics(
    before_manifest: dict[str, Any],
    after_manifest: dict[str, Any],
    before_dir: Path,
    after_dir: Path,
    *,
    render_mode: str,
) -> dict[str, Any]:
    required_stage_keys = (
        ("scene_color_hdr", "history_write", "final")
        if render_mode == "taa-on"
        else ("scene_color_hdr", "final")
    )
    required_stages = {
        "Light": "scene_color_hdr",
        "final": "final",
    }
    if render_mode == "taa-on":
        required_stages["HistoryWrite"] = "history_write"
    result: dict[str, Any] = {
        "schema": "mgif-shadow-edge-release-gate-v1",
        "render_mode": render_mode,
        "status": "error",
        "passed": False,
        "required_stages": required_stages,
        "no_post_history_adapter": (
            "scene_color_hdr supplied only to satisfy ShadowFrame API; "
            "history_write stage is not publication-gated"
            if render_mode == "no-post"
            else None
        ),
        "canonical_config": dict(RELEASE_ROI_CONFIG),
        "metrics": None,
        "errors": [],
    }
    try:
        _, ShadowEdgeMetricsConfig, _, evaluate_shadow_edge_metrics = (
            _shadow_edge_module_api()
        )
        before_frame = _release_shadow_frame(
            before_manifest,
            before_dir,
            render_mode=render_mode,
        )
        after_frame = _release_shadow_frame(
            after_manifest,
            after_dir,
            render_mode=render_mode,
        )
        evaluated = evaluate_shadow_edge_metrics(
            before_frame,
            after_frame,
            ShadowEdgeMetricsConfig(**RELEASE_ROI_CONFIG),
        )
        metrics = evaluated.to_dict()
        result["metrics"] = metrics
        result["status"] = str(evaluated.status)
        stage_statuses = {
            stage: getattr(stage_result, "status", None)
            for stage, stage_result in evaluated.stages.items()
        }
        result["stage_statuses"] = stage_statuses
        missing_or_nonpass = [
            stage
            for stage in required_stage_keys
            if stage_statuses.get(stage) != "pass"
        ]
        if evaluated.status != "pass":
            result["errors"].append(
                f"shadow-edge ROI status is {evaluated.status!r}, expected 'pass'"
            )
        if missing_or_nonpass:
            result["errors"].append(
                "shadow-edge ROI required stages are non-pass or absent: "
                f"{missing_or_nonpass!r}"
            )
        result["passed"] = not result["errors"]
    except Exception as exc:
        result["errors"].append(f"{type(exc).__name__}: {exc}")
    return result

def _compare_extracted_pair(
    options: argparse.Namespace,
    *,
    before_capture: Path,
    after_capture: Path,
    before_manifest: dict[str, Any],
    after_manifest: dict[str, Any],
    before_dir: Path,
    after_dir: Path,
    work_root: Path,
    same_current_pose: bool,
    pair_name: str | None = None,
    release_gate: bool = False,
    release_render_mode: str | None = None,
) -> dict[str, Any]:
    started = time.time()
    if release_gate and release_render_mode not in RELEASE_RENDER_MODES:
        raise ComparatorError(
            f"release pair comparison requires a canonical render mode, got "
            f"{release_render_mode!r}"
        )
    screen_roles = (
        ("light", "taa", "final")
        if not release_gate or release_render_mode == "taa-on"
        else ("light", "final")
    )
    discovery_errors = [
        *[f"before: {error}" for error in before_manifest.get("errors", [])],
        *[f"after: {error}" for error in after_manifest.get("errors", [])],
    ]

    comparisons: dict[str, Any] = {
        "csm": [],
        "light": None,
        "taa": None,
        "final": None,
        "shadow_edge_metrics": None,
    }
    before_cascades = _csm_exports_by_layer(
        before_manifest,
        label="before",
        errors=discovery_errors,
    )
    after_cascades = _csm_exports_by_layer(
        after_manifest,
        label="after",
        errors=discovery_errors,
    )
    if set(before_cascades) != set(after_cascades):
        discovery_errors.append(
            "CSM layer sets differ: "
            f"{sorted(before_cascades)} vs {sorted(after_cascades)}"
        )
    if release_gate:
        for side, cascades in (("before", before_cascades), ("after", after_cascades)):
            if set(cascades) != {0, 1, 2, 3}:
                discovery_errors.append(
                    f"{side} release CSM layers are {sorted(cascades)!r}, "
                    "expected exactly [0, 1, 2, 3]"
                )
    for cascade in sorted(set(before_cascades) & set(after_cascades)):
        row = _compare_export_pair(
            before_dir,
            after_dir,
            before_cascades[cascade],
            after_cascades[cascade],
            max_shift=options.max_shift,
            epsilon=options.depth_epsilon,
            depth=True,
        )
        row["cascade"] = cascade
        comparisons["csm"].append(row)

    for role in screen_roles:
        before_export = before_manifest.get("exports", {}).get(role)
        after_export = after_manifest.get("exports", {}).get(role)
        if not before_export or not after_export:
            discovery_errors.append(f"{role} export missing in one or both captures")
            continue
        comparisons[role] = _compare_export_pair(
            before_dir,
            after_dir,
            before_export,
            after_export,
            max_shift=options.max_shift,
            epsilon=options.color_epsilon,
            depth=False,
        )

    csm_meta = (
        before_manifest.get("discovery", {})
        .get("csm", {})
        .get("resource", {})
    )
    csm_resolution = int(csm_meta.get("width", 1))
    before_projection = _analyze_cascade_nesting(
        _select_projection_geometry(before_manifest, resolution=csm_resolution),
        power2_tolerance=options.power2_tolerance,
        phase_tolerance=options.grid_phase_tolerance,
    )
    after_csm_meta = (
        after_manifest.get("discovery", {})
        .get("csm", {})
        .get("resource", {})
    )
    after_projection = _analyze_cascade_nesting(
        _select_projection_geometry(
            after_manifest,
            resolution=int(after_csm_meta.get("width", csm_resolution)),
        ),
        power2_tolerance=options.power2_tolerance,
        phase_tolerance=options.grid_phase_tolerance,
    )
    projection_motion = _compare_projection_motion(
        before_projection,
        after_projection,
        matrix_epsilon=options.max_csm_matrix_delta,
    )
    projection = {
        "before": before_projection,
        "after": after_projection,
        "motion": projection_motion,
    }
    if release_gate:
        if before_projection.get("selection_ambiguous") is not False:
            discovery_errors.append("before CSM projection matrix selection is ambiguous")
        if after_projection.get("selection_ambiguous") is not False:
            discovery_errors.append("after CSM projection matrix selection is ambiguous")
    if same_current_pose and not projection_motion.get("matrix_comparison_available"):
        discovery_errors.append(
            "same-current-pose CSM per-layer matrix comparison is unavailable: "
            f"{projection_motion.get('reason', 'selected paths/layers do not match')}"
        )

    violations: list[dict[str, Any]] = []
    registration_limit = options.max_csm_registration_shift
    if same_current_pose and (release_gate or registration_limit is None):
        registration_limit = 0
    csm_metric_key = "unregistered" if same_current_pose else "residual"
    csm_metric_prefix = "same_texel" if same_current_pose else "registered"
    if not comparisons["csm"]:
        discovery_errors.append("no comparable CSM layers were exported")
    for row in comparisons["csm"]:
        metric = row[csm_metric_key]
        if release_gate:
            discovery_errors.extend(
                _release_nonfinite_errors(
                    row["unregistered"],
                    label=f"CSM layer {row['cascade']} full-frame comparison",
                )
            )
        if not metric.get("available"):
            discovery_errors.append(
                f"CSM layer {row['cascade']} {csm_metric_key} comparison unavailable: "
                f"{metric.get('reason', 'unknown reason')}"
            )
        else:
            for side in ("before", "after"):
                pixels_key = f"{side}_active_pixels"
                fraction_key = f"{side}_active_fraction"
                active_pixels = int(metric.get(pixels_key, 0) or 0)
                active_fraction = float(metric.get(fraction_key, 0.0) or 0.0)
                if active_pixels <= 0:
                    discovery_errors.append(
                        f"CSM layer {row['cascade']} {side} active coverage is empty"
                    )
                elif release_gate and (
                    active_pixels < options.min_csm_active_pixels
                    or active_fraction < options.min_csm_active_fraction
                ):
                    discovery_errors.append(
                        f"CSM layer {row['cascade']} {side} active coverage is too low: "
                        f"pixels={active_pixels}, fraction={active_fraction:g}; "
                        f"required pixels>={options.min_csm_active_pixels} and "
                        f"fraction>={options.min_csm_active_fraction:g}"
                    )
            base_thresholds = (
                ("changed_fraction", options.max_csm_residual_fraction),
                ("mae", options.max_csm_residual_mae),
            )
            strict_thresholds = (
                (
                    "active_changed_fraction",
                    options.max_csm_active_changed_fraction,
                ),
                (
                    "foreground_mismatch_fraction",
                    options.max_csm_foreground_mismatch_fraction,
                ),
                ("max_abs", options.max_csm_max_abs),
            )
            for metric_name, threshold in (
                base_thresholds + (strict_thresholds if release_gate else ())
            ):
                value = metric.get(metric_name)
                if value is None or not math.isfinite(float(value)):
                    discovery_errors.append(
                        f"CSM layer {row['cascade']} {csm_metric_prefix}_{metric_name} "
                        "is unavailable or non-finite"
                    )
                    continue
                numeric_value = float(value)
                if numeric_value > threshold:
                    violations.append(
                        {
                            "role": "csm",
                            "cascade": row["cascade"],
                            "metric": f"{csm_metric_prefix}_{metric_name}",
                            "value": numeric_value,
                            "threshold": threshold,
                        }
                    )
        registration = row["registration"]
        if not registration.get("available"):
            if registration_limit is not None:
                discovery_errors.append(
                    f"CSM layer {row['cascade']} registration unavailable: "
                    f"{registration.get('reason', 'unknown reason')}"
                )
        elif registration_limit is not None:
            shift_value = max(
                abs(int(registration.get("dx", 0))),
                abs(int(registration.get("dy", 0))),
            )
            if shift_value > registration_limit:
                violations.append(
                    {
                        "role": "csm",
                        "cascade": row["cascade"],
                        "metric": "diagnostic_integer_registration_shift_pixels",
                        "value": shift_value,
                        "threshold": registration_limit,
                        "dx": registration.get("dx"),
                        "dy": registration.get("dy"),
                    }
                )
    if same_current_pose and projection_motion.get("matrix_comparison_available"):
        for row in projection_motion.get("cascades", []):
            matrix = row["matrix"]
            value = _threshold_value(matrix, "max_abs")
            if value > options.max_csm_matrix_delta:
                violations.append(
                    {
                        "role": "csm",
                        "cascade": int(row["cascade"]),
                        "metric": "matrix_max_abs_delta",
                        "value": value,
                        "threshold": options.max_csm_matrix_delta,
                    }
                )
            if release_gate:
                for axis in ("x", "y"):
                    center_value = row.get(f"center_motion_before_texels_{axis}")
                    if center_value is None or not math.isfinite(float(center_value)):
                        discovery_errors.append(
                            f"CSM layer {row['cascade']} center motion {axis} is unavailable"
                        )
                        continue
                    center_abs = abs(float(center_value))
                    if center_abs > options.max_csm_center_motion_texels:
                        violations.append(
                            {
                                "role": "csm",
                                "cascade": int(row["cascade"]),
                                "metric": f"projection_center_motion_texels_{axis}",
                                "value": center_abs,
                                "signed_value": float(center_value),
                                "threshold": options.max_csm_center_motion_texels,
                            }
                        )

    stage_thresholds = {
        "light": options.max_light_residual_fraction,
        "taa": options.max_taa_residual_fraction,
        "final": options.max_final_residual_fraction,
    }
    stage_metric_key = "unregistered" if same_current_pose else "residual"
    stage_metric_name = (

        "same_pixel_changed_fraction"

        if same_current_pose

        else "registered_changed_fraction"

    )
    stage_max_abs_thresholds = {
        "light": options.max_light_max_abs,
        "taa": options.max_taa_max_abs,
        "final": options.max_final_max_abs,
    }
    for role, threshold in stage_thresholds.items():
        comparison = comparisons.get(role)
        if comparison is None:
            continue
        metric = comparison[stage_metric_key]
        if release_gate:
            discovery_errors.extend(
                _release_nonfinite_errors(
                    comparison["unregistered"],
                    label=f"{role} full-frame comparison",
                )
            )
        if not metric.get("available"):
            discovery_errors.append(
                f"{role} {stage_metric_key} comparison unavailable: "
                f"{metric.get('reason', 'unknown reason')}"
            )
            continue
        value = float(metric["changed_fraction"])
        if value > threshold:
            violations.append(
                {
                    "role": role,
                    "metric": stage_metric_name,
                    "value": value,
                    "threshold": threshold,
                }
            )
        if release_gate:
            max_abs = metric.get("max_abs")
            if max_abs is None or not math.isfinite(float(max_abs)):
                discovery_errors.append(f"{role} same-pixel max_abs is unavailable")
            elif float(max_abs) > stage_max_abs_thresholds[role]:
                violations.append(
                    {
                        "role": role,
                        "metric": "same_pixel_max_abs",
                        "value": float(max_abs),
                        "threshold": stage_max_abs_thresholds[role],
                    }
                )

    shadow_edge_release: dict[str, Any] | None = None
    if release_gate:
        assert release_render_mode is not None
        shadow_edge_release = _evaluate_release_shadow_edge_metrics(
            before_manifest,
            after_manifest,
            before_dir,
            after_dir,
            render_mode=release_render_mode,
        )
        comparisons["shadow_edge_metrics"] = shadow_edge_release
        if shadow_edge_release.get("passed") is not True:
            roi_status = str(shadow_edge_release.get("status", "error"))
            if roi_status == "fail":
                stage_statuses = shadow_edge_release.get("stage_statuses", {})
                required_roi_stages = (
                    ("scene_color_hdr", "history_write", "final")
                    if release_render_mode == "taa-on"
                    else ("scene_color_hdr", "final")
                )
                for stage in required_roi_stages:
                    if stage_statuses.get(stage) != "pass":
                        violations.append(
                            {
                                "role": stage,
                                "metric": "shadow_edge_roi_status",
                                "value": stage_statuses.get(stage),
                                "threshold": "pass",
                            }
                        )
            else:
                discovery_errors.extend(
                    f"shadow-edge ROI: {error}"
                    for error in shadow_edge_release.get("errors", [])
                )

    origin = _build_origin_assessment(
        comparisons,
        violations,
        blend_fraction=options.cascade_blend_fraction,
        cascade_nesting=before_projection,
    )
    status = "error" if discovery_errors else "fail" if violations else "pass"
    return {
        "tool": "rdc_csm_motion_compare",
        "tool_version": TOOL_VERSION,
        "status": status,
        "passed": status == "pass",
        "input_mode": (
            "same_current_pose_pair" if same_current_pose else "capture_pair"
        ),
        "pair": pair_name,
        "captures": {
            "before": str(before_capture.resolve()),
            "after": str(after_capture.resolve()),
        },
        "expectation": {
            "same_current_pose": same_current_pose,
            "csm_depth_alignment": (
                "same_texel_unregistered" if same_current_pose else "integer_registration"
            ),
            "csm_registration_role": "diagnostic" if same_current_pose else "alignment",
            "screen_stage_alignment": (
                "same_pixel" if same_current_pose else "integer_registration"
            ),
        },
        "configuration": {
            "max_shift": options.max_shift,
            "release_gate": release_gate,
            "release_render_mode": release_render_mode,
            "max_csm_registration_shift": registration_limit,
            "max_csm_matrix_delta": options.max_csm_matrix_delta,
            "depth_epsilon": options.depth_epsilon,
            "color_epsilon": options.color_epsilon,
            "thresholds": {
                "csm_residual_fraction": options.max_csm_residual_fraction,
                "csm_residual_mae": options.max_csm_residual_mae,
                "csm_active_changed_fraction": options.max_csm_active_changed_fraction,
                "csm_foreground_mismatch_fraction": options.max_csm_foreground_mismatch_fraction,
                "csm_max_abs": options.max_csm_max_abs,
                "min_csm_active_pixels": options.min_csm_active_pixels,
                "min_csm_active_fraction": options.min_csm_active_fraction,
                "csm_matrix_delta": options.max_csm_matrix_delta,
                "csm_center_motion_texels": options.max_csm_center_motion_texels,
                "light_residual_fraction": options.max_light_residual_fraction,
                "taa_residual_fraction": options.max_taa_residual_fraction,
                "final_residual_fraction": options.max_final_residual_fraction,
                "light_max_abs": options.max_light_max_abs,
                "taa_max_abs": options.max_taa_max_abs,
                "final_max_abs": options.max_final_max_abs,
            },
            "power2_tolerance": options.power2_tolerance,
            "grid_phase_tolerance_texels": options.grid_phase_tolerance,
            "cascade_blend_fraction": options.cascade_blend_fraction,
        },
        "session_cleanup": {
            "before": before_manifest.get("session_cleanup"),
            "after": after_manifest.get("session_cleanup"),
        },
        "discovery": {
            "before": _discovery_summary(before_manifest),
            "after": _discovery_summary(after_manifest),
            "errors": discovery_errors,
        },
        "comparisons": comparisons,
        "release_evidence": (
            {
                "render_mode": release_render_mode,
                "canonical_thresholds": dict(RELEASE_CANONICAL_THRESHOLDS),
                "canonical_roi_config": dict(RELEASE_ROI_CONFIG),
                "shadow_edge_metrics": shadow_edge_release,
            }
            if release_gate
            else None
        ),
        "cascade_projection": projection,
        "origin_assessment": origin,
        "violations": violations,
        "elapsed_seconds": time.time() - started,
        "work_dir": str(work_root),
    }


def _run_comparison(
    options: argparse.Namespace,
    work_root: Path,
    *,
    overall_deadline: float,
    runtime_budget: ComparatorRuntimeBudget,
) -> dict[str, Any]:
    _require_deadline(overall_deadline, stage="direct comparison setup")
    before_dir = work_root / "before"
    after_dir = work_root / "after"
    before_dir.mkdir(parents=True, exist_ok=True)
    after_dir.mkdir(parents=True, exist_ok=True)

    before_capture = options.before.resolve()
    after_capture = options.after.resolve()
    before_manifest = _extract_capture(
        options.rdc,
        before_capture,
        before_dir.resolve(),
        timeout=options.timeout,
        tag="before",
        release_gate=False,
        release_render_mode=None,
        overall_deadline=overall_deadline,
        runtime_budget=runtime_budget,
    )
    after_manifest = _extract_capture(
        options.rdc,
        after_capture,
        after_dir.resolve(),
        timeout=options.timeout,
        tag="after",
        release_gate=False,
        release_render_mode=None,
        overall_deadline=overall_deadline,
        runtime_budget=runtime_budget,
    )
    _require_deadline(overall_deadline, stage="direct capture comparison")
    report = _compare_extracted_pair(
        options,
        before_capture=before_capture,
        after_capture=after_capture,
        before_manifest=before_manifest,
        after_manifest=after_manifest,
        before_dir=before_dir,
        after_dir=after_dir,
        work_root=work_root,
        same_current_pose=False,
        release_gate=False,
        release_render_mode=None,
    )
    _require_deadline(overall_deadline, stage="direct comparison completion")
    return report


def _pose_vector(pose: Any, label: str) -> list[float]:
    if not isinstance(pose, dict):
        raise ComparatorError(f"{label} is not an object")
    position = pose.get("position")
    if not isinstance(position, list) or len(position) != 3:
        raise ComparatorError(f"{label}.position must contain three numbers")
    values = [
        *position,
        pose.get("yaw_degrees"),
        pose.get("pitch_degrees"),
    ]
    try:
        converted = [float(value) for value in values]
    except (TypeError, ValueError) as exc:
        raise ComparatorError(f"{label} contains a non-numeric value") from exc
    if not all(math.isfinite(value) for value in converted):
        raise ComparatorError(f"{label} contains a non-finite value")
    return converted


def _pose_max_delta(left: Any, right: Any, label: str) -> float:
    left_values = _pose_vector(left, f"{label}.left")
    right_values = _pose_vector(right, f"{label}.right")
    return max(abs(a - b) for a, b in zip(left_values, right_values))


def _validate_smoke_pose_contract(
    case: dict[str, Any],
    *,
    tolerance: float,
) -> dict[str, Any]:
    checks: list[dict[str, Any]] = []
    errors: list[str] = []
    boundaries = case.get("boundaries")
    if not isinstance(boundaries, dict):
        return {
            "passed": False,
            "tolerance": tolerance,
            "checks": checks,
            "errors": ["case boundaries are missing or invalid"],
        }
    ready: dict[str, dict[str, Any]] = {}
    for boundary in SMOKE_BOUNDARIES:
        entry = boundaries.get(boundary)
        if not isinstance(entry, dict) or not isinstance(entry.get("ready"), dict):
            errors.append(f"{boundary} ready pose is missing")
            continue
        ready[boundary] = entry["ready"]
        for key, expected in (
            ("marker", boundary),
            ("mode", case.get("mode")),
            ("phase", "pre-render"),
        ):
            if ready[boundary].get(key) != expected:
                errors.append(
                    f"{boundary} ready {key}={ready[boundary].get(key)!r}, "
                    f"expected {expected!r}"
                )
    if errors:
        return {
            "passed": False,
            "tolerance": tolerance,
            "checks": checks,
            "errors": errors,
        }

    def equal_check(name: str, left: Any, right: Any) -> None:
        try:
            delta = _pose_max_delta(left, right, name)
        except ComparatorError as exc:
            errors.append(str(exc))
            return
        checks.append(
            {
                "name": name,
                "relation": "equal",
                "max_abs_delta": delta,
                "threshold": tolerance,
                "passed": delta <= tolerance,
            }
        )
        if delta > tolerance:
            errors.append(f"{name} delta {delta:g} exceeds {tolerance:g}")

    last = ready["last-moving"]
    first = ready["first-still"]
    settled = ready["settled"]
    equal_check(
        "last-moving.current_vs_first-still.current",
        last.get("current"),
        first.get("current"),
    )
    equal_check(
        "last-moving.current_vs_first-still.previous",
        last.get("current"),
        first.get("previous"),
    )
    equal_check(
        "first-still.current_vs_settled.current",
        first.get("current"),
        settled.get("current"),
    )
    equal_check(
        "first-still.current_vs_settled.previous",
        first.get("current"),
        settled.get("previous"),
    )
    try:
        motion_delta = _pose_max_delta(
            last.get("current"),
            last.get("previous"),
            "last-moving.current_vs_previous",
        )
        moving_check = {
            "name": "last-moving.current_vs_previous",
            "relation": "different",
            "max_abs_delta": motion_delta,
            "threshold": tolerance,
            "passed": motion_delta > tolerance,
        }
        checks.append(moving_check)
        if not moving_check["passed"]:
            errors.append("last-moving pose did not move relative to its previous pose")
    except ComparatorError as exc:
        errors.append(str(exc))
    return {
        "passed": not errors,
        "tolerance": tolerance,
        "checks": checks,
        "errors": errors,
    }


def _safe_component(value: Any) -> str:
    text = re.sub(r"[^A-Za-z0-9_.-]+", "_", str(value)).strip("._")
    return text[:80] or "case"


def _resolve_manifest_capture(
    manifest_path: Path,
    case: dict[str, Any],
    boundary: str,
    *,
    release_gate: bool,
) -> Path:
    boundary_entry = case.get("boundaries", {}).get(boundary, {})
    candidates: list[Path] = []
    declared_capture: Path | None = None

    def add_candidate(value: Any, *, base: Path) -> None:
        if not value:
            return
        path = Path(str(value))
        if not path.is_absolute():
            path = base / path
        candidates.append(path.resolve())

    declared_value = boundary_entry.get("capture_path")
    if declared_value:
        declared_capture = Path(str(declared_value))
        if not declared_capture.is_absolute():
            declared_capture = manifest_path.parent / declared_capture
        declared_capture = declared_capture.resolve()
    add_candidate(declared_value, base=manifest_path.parent)
    case_directory = case.get("case_directory")
    if case_directory:
        case_dir_path = Path(str(case_directory))
        if not case_dir_path.is_absolute():
            case_dir_path = manifest_path.parent / case_dir_path
        candidates.append((case_dir_path / f"{boundary}.rdc").resolve())
    if case.get("name"):
        candidates.append(
            (manifest_path.parent / str(case["name"]) / f"{boundary}.rdc").resolve()
        )

    unique_candidates: list[Path] = []
    seen: set[str] = set()
    for candidate in candidates:
        key = os.path.normcase(str(candidate))
        if key not in seen:
            seen.add(key)
            unique_candidates.append(candidate)
    existing = [
        candidate
        for candidate in unique_candidates
        if candidate.is_file() and candidate.suffix.lower() == ".rdc"
    ]
    if release_gate:
        if declared_capture is None:
            raise ComparatorError(
                f"{case.get('name', '<unnamed>')} {boundary} capture_path is required"
            )
        if not declared_capture.is_file() or declared_capture.suffix.lower() != ".rdc":
            raise ComparatorError(
                f"{case.get('name', '<unnamed>')} {boundary} declared capture is missing: "
                f"{declared_capture}"
            )
        if any(_path_key(candidate) != _path_key(declared_capture) for candidate in existing):
            raise ComparatorError(
                f"{case.get('name', '<unnamed>')} {boundary} has ambiguous existing capture candidates: "
                + ", ".join(str(candidate) for candidate in existing)
            )
        return declared_capture
    for candidate in existing:
        return candidate
    rendered = ", ".join(str(path) for path in unique_candidates) or "no path"
    raise ComparatorError(
        f"{case.get('name', '<unnamed>')} {boundary} capture not found; tried {rendered}"
    )


def _load_smoke_manifest(path: Path) -> dict[str, Any]:
    try:
        value = json.loads(path.read_text(encoding="utf-8-sig"))
    except (OSError, json.JSONDecodeError) as exc:
        raise ComparatorError(f"cannot read smoke manifest {path}: {exc}") from exc
    if not isinstance(value, dict):
        raise ComparatorError(f"smoke manifest root is not an object: {path}")
    if value.get("schema") != SMOKE_MANIFEST_SCHEMA:
        raise ComparatorError(
            f"unsupported smoke manifest schema {value.get('schema')!r}; "
            f"expected {SMOKE_MANIFEST_SCHEMA!r}"
        )
    if not isinstance(value.get("cases"), list):
        raise ComparatorError("smoke manifest cases must be an array")
    return value


def _validate_full_release_capture_set(
    manifest_path: Path,
    source: dict[str, Any],
) -> dict[str, Any]:
    case_bindings: dict[str, Any] = {}
    resolved_captures: dict[str, dict[str, Path]] = {}
    errors: list[str] = []
    all_paths: list[str] = []
    all_hashes: list[str] = []
    for case in source.get("cases", []):
        if not isinstance(case, dict):
            continue
        case_name = str(case.get("name", ""))
        captures: dict[str, Path] = {}
        for boundary in SMOKE_BOUNDARIES:
            try:
                captures[boundary] = _resolve_manifest_capture(
                    manifest_path,
                    case,
                    boundary,
                    release_gate=True,
                )
            except ComparatorError as exc:
                errors.append(f"{case_name}/{boundary}: {exc}")
        if len(captures) != len(SMOKE_BOUNDARIES):
            continue
        binding = _validate_release_capture_bindings(
            manifest_path,
            source,
            case,
            captures,
        )
        case_bindings[case_name] = binding
        resolved_captures[case_name] = captures
        errors.extend(f"{case_name}: {error}" for error in binding["errors"])
        all_paths.extend(binding.get("paths", {}).values())
        all_hashes.extend(
            identity.get("sha256")
            for identity in binding.get("identities", {}).values()
            if isinstance(identity, dict) and identity.get("sha256")
        )
    if len(all_paths) != 6 or len(set(all_paths)) != 6:
        errors.append("full release validation did not prove six unique canonical RDC paths")
    if len(all_hashes) != 6 or len(set(all_hashes)) != 6:
        errors.append("full release validation did not prove six unique RDC SHA-256 values")
    return {
        "passed": not errors,
        "case_bindings": case_bindings,
        "resolved_captures": {
            case_name: {
                boundary: str(path.resolve()) for boundary, path in captures.items()
            }
            for case_name, captures in resolved_captures.items()
        },
        "unique_path_count": len(set(all_paths)),
        "unique_sha256_count": len(set(all_hashes)),
        "errors": errors,
    }

def _run_manifest_comparison(
    options: argparse.Namespace,
    work_root: Path,
    *,
    overall_deadline: float,
    runtime_toolchain_before: dict[str, Any],
    runtime_budget: ComparatorRuntimeBudget,
) -> dict[str, Any]:
    started = time.time()
    _require_deadline(overall_deadline, stage="manifest comparison setup")
    manifest_path = options.manifest.resolve()
    source = _load_smoke_manifest(manifest_path)
    release_gate, release_gate_reason = _release_gate_enabled(options, source)
    executable_validation: dict[str, Any] | None = None
    toolchain_validation: dict[str, Any] | None = None
    cleanup_validation: dict[str, Any] | None = None
    global_capture_uniqueness: dict[str, Any] | None = None
    full_capture_validation: dict[str, Any] | None = None
    release_profile: dict[str, Any] | None = None
    release_cli_validation: dict[str, Any] | None = None
    capture_set_validation: dict[str, Any] | None = None
    capture_copy_budget_validation: dict[str, Any] | None = None
    release_render_mode: str | None = None
    if release_gate:
        release_cli_validation = _validate_release_cli_contract(options)
        if release_cli_validation.get("passed") is not True:
            raise ComparatorError(
                "release CLI contract failed before replay:\n- "
                + "\n- ".join(release_cli_validation.get("errors", []))
            )
        release_profile = _release_manifest_profile(source)
        if release_profile.get("passed") is not True:
            raise ComparatorError(
                "release profile failed before replay:\n- "
                + "\n- ".join(release_profile.get("errors", []))
            )
        release_render_mode = str(release_profile["render_mode"])
        capture_set_validation = _validate_release_capture_set_evidence(
            source,
            manifest_path,
            render_mode=release_render_mode,
        )
        capture_copy_budget_validation = _validate_release_capture_copy_budget(source)
        toolchain_validation = _validate_manifest_toolchain_evidence(
            source,
            runtime_before=runtime_toolchain_before,
        )
        if toolchain_validation.get("passed") is not True:
            raise ComparatorError(
                "release toolchain evidence failed before replay:\n- "
                + "\n- ".join(toolchain_validation.get("errors", []))
            )
        executable_validation = _validate_release_executable_evidence(source)
        cleanup_errors = _validate_release_cleanup_contract(source)
        cleanup_validation = {
            "schema": RDC_CLEANUP_SCHEMA,
            "passed": not cleanup_errors,
            "errors": cleanup_errors,
            "external_added_daemons": source.get("rdc_session_cleanup", {}).get(
                "external_added_daemons", []
            ),
            "external_added_session_files": source.get(
                "rdc_session_cleanup", {}
            ).get("external_added_session_files", []),
        }
        global_capture_uniqueness = _validate_release_global_capture_uniqueness(
            source,
            manifest_path,
        )
        manifest_errors = _validate_release_manifest_contract(source, manifest_path)
        if manifest_errors:
            raise ComparatorError(
                "release manifest contract failed:\n- " + "\n- ".join(manifest_errors)
            )
        full_capture_validation = _validate_full_release_capture_set(
            manifest_path,
            source,
        )
        if full_capture_validation.get("passed") is not True:
            raise ComparatorError(
                "full release capture binding failed before case filtering:\n- "
                + "\n- ".join(full_capture_validation.get("errors", []))
            )
    requested_cases = set(options.case)
    cases = [
        case
        for case in source["cases"]
        if isinstance(case, dict)
        and (not requested_cases or case.get("name") in requested_cases)
    ]
    found_names = {str(case.get("name")) for case in cases}
    missing_names = sorted(requested_cases - found_names)
    if missing_names:
        raise ComparatorError(f"manifest case(s) not found: {', '.join(missing_names)}")
    if not cases:
        raise ComparatorError("smoke manifest contains no selected cases")

    extraction_cache: dict[str, tuple[dict[str, Any], Path]] = {}
    case_reports: list[dict[str, Any]] = []
    aggregate_errors: list[str] = []
    aggregate_violations: list[dict[str, Any]] = []
    pair_counts = {"pass": 0, "fail": 0, "error": 0}

    for case_index, case in enumerate(cases):
        _require_deadline(
            overall_deadline,
            stage=f"manifest case {case_index} setup",
        )
        case_name = str(case.get("name") or f"case-{case_index}")
        case_errors: list[str] = []
        capture_binding: dict[str, Any] | None = None
        marker_bindings: dict[str, Any] = {}
        resource_semantics: dict[str, Any] | None = None
        taa_ping_pong: dict[str, Any] | None = None
        pose_contract = _validate_smoke_pose_contract(
            case,
            tolerance=options.pose_tolerance,
        )
        if case.get("status") != "passed":
            case_errors.append(
                f"source capture case status is {case.get('status')!r}, expected 'passed'"
            )
        if case.get("pose_validation", {}).get("passed") is not True:
            case_errors.append("source capture case pose_validation.passed is not true")
        if case.get("cleanup", {}).get("passed") is not True:
            case_errors.append("source capture case cleanup.passed is not true")
        case_errors.extend(pose_contract["errors"])

        captures: dict[str, Path] = {}
        if not case_errors:
            for boundary in SMOKE_BOUNDARIES:
                try:
                    captures[boundary] = _resolve_manifest_capture(
                        manifest_path,
                        case,
                        boundary,
                        release_gate=release_gate,
                    )
                except ComparatorError as exc:
                    case_errors.append(str(exc))
        if release_gate and not case_errors:
            capture_binding = _validate_release_capture_bindings(
                manifest_path,
                source,
                case,
                captures,
            )
            case_errors.extend(capture_binding["errors"])

        extracted: dict[str, tuple[dict[str, Any], Path]] = {}
        if not case_errors:
            for boundary in SMOKE_BOUNDARIES:
                _require_deadline(
                    overall_deadline,
                    stage=f"{case_name}/{boundary} extraction",
                )
                capture = captures[boundary]
                cache_key = os.path.normcase(str(capture.resolve()))
                if cache_key not in extraction_cache:
                    output_dir = (
                        work_root
                        / "captures"
                        / f"{len(extraction_cache):03d}_{_safe_component(case_name)}_"
                        f"{_safe_component(boundary)}"
                    )
                    output_dir.mkdir(parents=True, exist_ok=True)
                    try:
                        extraction_cache[cache_key] = (
                            _extract_capture(
                                options.rdc,
                                capture,
                                output_dir.resolve(),
                                timeout=options.timeout,
                                tag=f"m{case_index}_{_safe_component(boundary)}",
                                release_gate=release_gate,
                                release_render_mode=release_render_mode,
                                overall_deadline=overall_deadline,
                                runtime_budget=runtime_budget,
                            ),
                            output_dir,
                        )
                    except Exception as exc:
                        if time.monotonic() >= overall_deadline:
                            raise ComparatorError(
                                f"total comparator deadline exceeded during "
                                f"{case_name}/{boundary} extraction: {exc}"
                            ) from exc
                        case_errors.append(f"{boundary} extraction failed: {exc}")
                        break
                extracted[boundary] = extraction_cache[cache_key]

        if release_gate and not case_errors:
            for boundary in SMOKE_BOUNDARIES:
                extraction_manifest, extraction_dir = extracted[boundary]
                case_errors.extend(
                    _validate_release_extraction_contract(
                        extraction_manifest,
                        extraction_dir,
                        label=boundary,
                        render_mode=release_render_mode,
                    )
                )
                expected_identity = capture_binding["identities"].get(boundary, {})
                replay_identity = extraction_manifest.get("capture_file_identity", {})
                for identity_key in ("path", "size_bytes", "sha256"):
                    if replay_identity.get(identity_key) != expected_identity.get(identity_key):
                        case_errors.append(
                            f"{boundary}: extracted capture identity {identity_key} does not "
                            "match the manifest-bound file"
                        )
                marker_binding = _validate_automation_frame_marker(
                    extraction_manifest,
                    case,
                    boundary,
                )
                marker_bindings[boundary] = marker_binding
                case_errors.extend(
                    f"{boundary}: {error}" for error in marker_binding["errors"]
                )
            assert release_render_mode is not None
            resource_semantics = _validate_three_capture_resource_semantics(
                extracted,
                render_mode=release_render_mode,
            )
            case_errors.extend(resource_semantics["errors"])
            if release_render_mode == "taa-on":
                taa_ping_pong = _validate_taa_history_ping_pong(extracted)
                case_errors.extend(taa_ping_pong["errors"])
            else:
                taa_ping_pong = {
                    "passed": True,
                    "applicable": False,
                    "render_mode": release_render_mode,
                    "errors": [],
                }

        pair_reports: dict[str, Any] = {}
        if not case_errors:
            for before_boundary, after_boundary in SAME_POSE_PAIRS:
                pair_name = f"{before_boundary}_to_{after_boundary}"
                _require_deadline(
                    overall_deadline,
                    stage=f"{case_name}/{pair_name} comparison",
                )
                before_manifest, before_dir = extracted[before_boundary]
                after_manifest, after_dir = extracted[after_boundary]
                pair_report = _compare_extracted_pair(
                    options,
                    before_capture=captures[before_boundary],
                    after_capture=captures[after_boundary],
                    before_manifest=before_manifest,
                    after_manifest=after_manifest,
                    before_dir=before_dir,
                    after_dir=after_dir,
                    work_root=work_root,
                    same_current_pose=True,
                    pair_name=pair_name,
                    release_gate=release_gate,
                    release_render_mode=release_render_mode,
                )
                _require_deadline(
                    overall_deadline,
                    stage=f"{case_name}/{pair_name} comparison completion",
                )
                pair_report["boundaries"] = {
                    "before": before_boundary,
                    "after": after_boundary,
                }
                pair_reports[pair_name] = pair_report
                pair_counts[pair_report["status"]] += 1
                for error in pair_report["discovery"]["errors"]:
                    aggregate_errors.append(f"{case_name} {pair_name}: {error}")
                for violation in pair_report["violations"]:
                    aggregate_violations.append(
                        {
                            "case": case_name,
                            "pair": pair_name,
                            **violation,
                        }
                    )

        if case_errors:
            case_status = "error"
            aggregate_errors.extend(f"{case_name}: {error}" for error in case_errors)
        elif any(report["status"] == "error" for report in pair_reports.values()):
            case_status = "error"
        elif any(report["status"] == "fail" for report in pair_reports.values()):
            case_status = "fail"
        else:
            case_status = "pass"
        case_reports.append(
            {
                "name": case_name,
                "mode": case.get("mode"),
                "render_mode": case.get("render_mode"),
                "gi_mode": case.get("gi_mode"),
                "source_status": case.get("status"),
                "status": case_status,
                "passed": case_status == "pass",
                "pose_contract": pose_contract,
                "release_evidence": {
                    "executable": (
                        executable_validation.get("case_results", {}).get(case_name)
                        if isinstance(executable_validation, dict)
                        else None
                    ),
                    "toolchain": toolchain_validation,
                    "capture_binding": capture_binding,
                    "rdc_frame_markers": marker_bindings,
                    "resource_semantics": resource_semantics,
                    "taa_history_ping_pong": taa_ping_pong,
                    "shadow_edge_metrics": {
                        pair_name: report.get("release_evidence", {}).get(
                            "shadow_edge_metrics"
                        )
                        for pair_name, report in pair_reports.items()
                    },
                } if release_gate else None,
                "captures": {
                    boundary: str(path) for boundary, path in captures.items()
                },
                "comparisons": pair_reports,
                "errors": case_errors,
            }
        )

    if release_gate:
        if len(case_reports) != 2:
            aggregate_errors.append(
                f"release replay produced {len(case_reports)} case reports, expected 2"
            )
        if len(extraction_cache) != 6:
            aggregate_errors.append(
                f"release replay extracted {len(extraction_cache)} unique captures, expected 6"
            )
        if sum(len(case.get("comparisons", {})) for case in case_reports) != 4:
            aggregate_errors.append("release replay did not produce all four same-pose pair reports")
    if aggregate_errors or any(case["status"] == "error" for case in case_reports):
        status = "error"
    elif aggregate_violations or any(case["status"] == "fail" for case in case_reports):
        status = "fail"
    else:
        status = "pass"
    _require_deadline(overall_deadline, stage="manifest comparison completion")
    return {
        "tool": "rdc_csm_motion_compare",
        "tool_version": TOOL_VERSION,
        "status": status,
        "passed": status == "pass",
        "input_mode": "smoke_manifest",
        "source_manifest": str(manifest_path),
        "source_manifest_status": source.get("status"),
        "schema": source.get("schema"),
        "release_evidence": (
            {
                "executable": executable_validation,
                "toolchain": toolchain_validation,
                "cleanup": cleanup_validation,
                "declared_capture_uniqueness": global_capture_uniqueness,
                "full_capture_binding": full_capture_validation,
                "release_profile": release_profile,
                "cli_contract": release_cli_validation,
                "capture_set_validation": capture_set_validation,
                "capture_copy_budget": capture_copy_budget_validation,
                "runtime_budget": runtime_budget.snapshot(),
            }
            if release_gate
            else None
        ),
        "configuration": {
            "selected_cases": options.case,
            "release_gate": release_gate,
            "release_gate_reason": release_gate_reason,
            "release_render_mode": release_render_mode,
            "release_smoke_sequence": (
                dict(RELEASE_SMOKE_SEQUENCE) if release_gate else None
            ),
            "release_boundary_frames": (
                dict(RELEASE_BOUNDARY_FRAMES) if release_gate else None
            ),
            "pose_tolerance": options.pose_tolerance,
            "same_pose_pairs": [
                f"{before}_to_{after}" for before, after in SAME_POSE_PAIRS
            ],
            "unique_capture_extractions": len(extraction_cache),
        },
        "summary": {
            "case_count": len(case_reports),
            "case_status_counts": {
                name: sum(case["status"] == name for case in case_reports)
                for name in ("pass", "fail", "error")
            },
            "pair_status_counts": pair_counts,
            "violation_count": len(aggregate_violations),
            "error_count": len(aggregate_errors),
        },
        "cases": case_reports,
        "violations": aggregate_violations,
        "errors": aggregate_errors,
        "elapsed_seconds": time.time() - started,
        "work_dir": str(work_root),
    }

def _self_test_stable_daemon_identity() -> dict[str, Any]:
    creation_unix_ns = 1_000_000_000
    creation_ticks = (
        WINDOWS_FILETIME_EPOCH_OFFSET_TICKS
        + creation_unix_ns // WINDOWS_FILETIME_TICK_NS
    )
    return {
        "pid": 4242,
        "identity": f"4242@winfiletime:{creation_ticks}",
        "creation_time_key": f"winfiletime:{creation_ticks}",
        "creation_time_unix_seconds": creation_unix_ns / 1_000_000_000.0,
        "creation_time_unix_ns": creation_unix_ns,
        "creation_filetime_ticks": creation_ticks,
        "image_path": "rdc.exe",
        "backend": "windows-process-handle",
        "native_handle_held": True,
        "terminate_access": True,
    }

def _self_test_direct_cleanup_fields() -> dict[str, Any]:
    post_status = {
        "classification": "inactive",
        "inactive": True,
        "basis": "held replay-daemon handle absent plus exact state file absent",
        "subprocess_used": False,
    }
    return {
        "close_subprocess_used": False,
        "status_subprocess_used": False,
        "direct_shutdown": {
            "schema": "mgif-rdc-direct-token-shutdown-v1",
            "subprocess_used": False,
            "pid_only_fallback": False,
            "port_scan_fallback": False,
            "tree_cleanup_requested": False,
            "stable_process_identity": _self_test_stable_daemon_identity(),
            "graceful_requested": True,
            "graceful_accepted": True,
            "same_handle_recovery": {
                "attempted": False,
                "same_native_handle": True,
                "pid_only_fallback": False,
                "tree_cleanup_requested": False,
                "passed": True,
            },
            "owned_daemon_absent": True,
            "state_file_absent": True,
            "post_status": dict(post_status),
            "errors": [],
            "passed": True,
        },
        "post_status": post_status,
        "owned_daemon_absent": True,
        "daemon_process_handle_close": {"closed": True},
    }

def _self_test_case_fixture(
    root: Path,
    *,
    render_mode: str = "taa-on",
) -> tuple[dict[str, Any], dict[str, Any], dict[str, Path]]:
    root.mkdir(parents=True, exist_ok=True)
    started = "2026-07-29T01:00:00+00:00"
    completed = "2026-07-29T01:10:00+00:00"
    expected_frames = dict(RELEASE_BOUNDARY_FRAMES)
    toolchain_before = _toolchain_snapshot(
        {
            "harness": Path(__file__).resolve().with_name(
                "run_csm_shadow_motion_smoke.py"
            ),
            **_runtime_tool_paths(),
        }
    )
    toolchain_after = json.loads(json.dumps(toolchain_before))
    toolchain_comparison = _compare_toolchain_snapshots(
        toolchain_before,
        toolchain_after,
        required_roles=TOOLCHAIN_REQUIRED_ROLES,
    )

    build_dir = root / "out" / "build" / "x64-debug"
    build_dir.mkdir(parents=True)
    source_exe = build_dir / "Demo.exe"
    source_exe.write_bytes(b"self-test-demo-executable-v2")
    source_identity = _file_identity(source_exe)
    launch_dir = root / "run" / "executable"
    launch_dir.mkdir(parents=True)
    launch_exe = launch_dir / f"Demo.{source_identity['sha256']}.exe"
    launch_exe.write_bytes(source_exe.read_bytes())
    launch_identity = _file_identity(launch_exe)

    def comparison(stage: str) -> dict[str, Any]:
        return {"stage": stage, "passed": True, "errors": []}

    lock = {
        "resolved_path": str(launch_exe.resolve()),
        "lock_identity": "self-test-lock",
        "native_handle_held": True,
        "write_share_denied": True,
        "delete_share_denied": True,
    }
    cases: list[dict[str, Any]] = []
    captures_by_case: dict[str, dict[str, Path]] = {}
    release_contract = _release_case_contract(render_mode)
    for case_index, mode in enumerate(release_contract):
        contract = release_contract[mode]
        case_name = f"{mode}__{contract['render_mode']}__{contract['gi_mode']}"
        case_dir = root / case_name
        case_dir.mkdir()
        captures: dict[str, Path] = {}
        boundaries: dict[str, Any] = {}
        for boundary_index, boundary in enumerate(SMOKE_BOUNDARIES):
            path = case_dir / f"{boundary}.rdc"
            path.write_bytes(f"rdc-{mode}-{boundary}".encode("ascii"))
            captures[boundary] = path
            capture_id = case_index * 10 + boundary_index
            capture_digest = _sha256_file(path)
            capture_file_evidence = {
                "schema": "mgif-renderdoc-capture-file-evidence-v1",
                "path": str(path.resolve()),
                "canonical_path": _path_key(path),
                "size_bytes": path.stat().st_size,
                "sha256": capture_digest,
                "read_consistent": True,
            }
            boundaries[boundary] = {
                "capture_path": str(path.resolve()),
                "capture_size_bytes": path.stat().st_size,
                "capture_sha256": capture_digest,
                "capture_file_evidence": dict(capture_file_evidence),
                "capture_rehash_after_all_cases": dict(capture_file_evidence),
                "capture_id": capture_id,
                "renderdoc_capture": {"captureId": capture_id},
                "capture_observation": {"new_capture": True},
                "capture_observed_utc": "2026-07-29T01:02:00+00:00",
                "copy_started_utc": "2026-07-29T01:03:00+00:00",
                "capture_copied_utc": "2026-07-29T01:04:00+00:00",
                "validated_target_frame": True,
                "capture_pose_binding": {"passed": True},
                "ready": {
                    "marker": boundary,
                    "mode": mode,
                    "phase": "pre-render",
                    "frame": expected_frames[boundary],
                    "current": {
                        "position": [0.0, 0.0, 0.0],
                        "yaw_degrees": 0.0,
                        "pitch_degrees": 0.0,
                    },
                    "previous": {
                        "position": [
                            -1.0 if boundary == "last-moving" else 0.0,
                            0.0,
                            0.0,
                        ],
                        "yaw_degrees": 0.0,
                        "pitch_degrees": 0.0,
                    },
                },
            }
        target_pid = 1234 + case_index
        target_creation_ticks = 123456789 + case_index
        target_binding = {
            "pid": target_pid,
            "stable_identity": f"{target_pid}@winfiletime:{target_creation_ticks}",
            "creation_time_key": f"winfiletime:{target_creation_ticks}",
            "creation_filetime_ticks": target_creation_ticks,
            "termination_authority_established": True,
            "duplicated_from_launcher_handle": True,
            "pid_lookup_used": False,
            "target_control_pid": target_pid,
            "target_control_pid_matches_owned_handle": True,
            "observed_image_path": str(launch_exe.resolve()),
            "expected_resolved_path": str(launch_exe.resolve()),
            "expected_size_bytes": launch_identity["size_bytes"],
            "expected_sha256": launch_identity["sha256"],
            "immutable_lock_identity": "self-test-lock",
            "immutable_lock_held_during_binding": True,
            "immutable_lock_write_share_denied": True,
            "immutable_lock_delete_share_denied": True,
            "image_path_matches": True,
            "same_native_handle_reserved_for_cleanup": True,
            "cleanup_verification": {
                "termination_passed": True,
                "same_native_handle": True,
                "original_running_after": False,
                "handle_closed": True,
                "passed": True,
            },
            "passed": True,
        }
        case_executable = {
            "schema": EXECUTABLE_BINDING_SCHEMA,
            "required": True,
            "fail_closed": False,
            "source_before_launch": dict(source_identity),
            "source_baseline_comparison": comparison("source-before-launch"),
            "launch_image_before_launch": dict(launch_identity),
            "launch_image_baseline_comparison": comparison("launch-before-launch"),
            "immutable_lock": dict(lock),
            "target_process_binding": target_binding,
            "passed": True,
        }
        case = {
            "name": case_name,
            "status": "passed",
            "mode": mode,
            "render_mode": contract["render_mode"],
            "gi_mode": contract["gi_mode"],
            "started_utc": started,
            "completed_utc": completed,
            "case_directory": str(case_dir.resolve()),
            "boundary_order": list(SMOKE_BOUNDARIES),
            "expected_automation_frames": expected_frames,
            "boundaries": boundaries,
            "pose_validation": {"passed": True},
            "cleanup": {"passed": True},
            "executable_evidence": case_executable,
            "toolchain_bundle_sha256": toolchain_before["bundle_sha256"],
        }
        cases.append(case)
        captures_by_case[case_name] = captures

    executable_evidence = {
        "schema": EXECUTABLE_BINDING_SCHEMA,
        "required": True,
        "source": {
            "resolved_path": str(source_exe.resolve()),
            "before_copy": dict(source_identity),
            "lock_during_copy": {"native_handle_held": True},
            "lock_close_after_copy": {"closed": True},
            "after_all_cases": dict(source_identity),
            "after_all_cases_comparison": comparison("source-after-all-cases"),
        },
        "launch_image": {
            "resolved_path": str(launch_exe.resolve()),
            "sha_named": True,
            "baseline": dict(launch_identity),
            "source_copy_comparison": comparison("source-copy"),
            "immutable_lock": dict(lock),
            "read_only_mode_set": True,
            "after_all_cases": dict(launch_identity),
            "after_all_cases_comparison": comparison("launch-after-all-cases"),
            "lock_close_after_all_cases": {"closed": True},
        },
        "launch_working_directory": str(build_dir.resolve()),
        "dependency_search": {
            "working_directory_is_source_build_directory": True,
            "source_build_directory_prepended_to_path": True,
        },
        "case_checks": [
            {"case": case["name"], **case["executable_evidence"]} for case in cases
        ],
        "errors": [],
        "passed": True,
    }
    session = {
        "session": "replay-first-still",
        "passed": True,
        **_self_test_direct_cleanup_fields(),
        "state_file": {
            "path": str((root / "replay-first-still.json").resolve()),
            "before": {"exists": False},
            "after_open": {"exists": True, "valid": True},
            "after_cleanup": {"exists": False},
            "absent_after_cleanup": True,
        },
        "daemon_ownership": {
            "established": True,
            "errors": [],
            "state_path_match": True,
            "state_capture_path_match": True,
            "daemon_capture_path_metadata_match": True,
            "stable_image_match": True,
            "state_publication_boundary_exact": True,
            "state_file_volume_verified": True,
            "exact_creation_order_clock_match": True,
            "strict_creation_precedes_publication": True,
            "creation_equals_publication": False,
            "snapshot_evidence_complete": True,
            "process_created_before_state_file": True,
            "stable_process_identity": _self_test_stable_daemon_identity(),

        },
        "owned_daemon_residue": [],
        "errors": [],
    }
    resources = {
        "available": True,
        "errors": [],
        "process_access_denied_count": 0,
        "sessions": {},
        "daemons": {},
    }
    cleanup = {
        "schema": RDC_CLEANUP_SCHEMA,
        "passed": True,
        "closed": True,
        "ownership_model": {
            "hard_gate_scope": "owned additions and owned residue only",
            "external_additions_are_diagnostic": True,
        },
        "named_replay_sessions": [session],
        "run_resource_diff": {
            "available": True,
            "before_errors": [],
            "after_errors": [],
            "before_process_access_denied_count": 0,
            "after_process_access_denied_count": 0,
            "added_daemons": [],
            "added_session_files": [],
            "removed_daemons": [],
            "removed_session_files": [],
            "changed_session_files": [],
        },
        "owned_added_session_files": [],
        "owned_added_daemons": [],
        "owned_session_file_residue": [],
        "owned_daemon_residue": [],
        "external_added_session_files": [],
        "external_added_daemons": [],
        "errors": [],
    }
    capture_set_entries: list[dict[str, Any]] = []
    copy_reservations: list[dict[str, Any]] = []
    reservation_index = 0
    for case in cases:
        for boundary in SMOKE_BOUNDARIES:
            reservation_index += 1
            row = case["boundaries"][boundary]
            capture_set_entries.append(
                {
                    "case": case["name"],
                    "mode": case["mode"],
                    "render_mode": case["render_mode"],
                    "gi_mode": case["gi_mode"],
                    "boundary": boundary,
                    "canonical_path": _path_key(Path(row["capture_path"])),
                    "size_bytes": row["capture_size_bytes"],
                    "sha256": row["capture_sha256"],
                    "checks": {
                        "within_output_directory": True,
                        "direct_path": True,
                        "direct_size": True,
                        "direct_sha256": True,
                        "stored_schema": True,
                        "stored_path": True,
                        "stored_size": True,
                        "stored_sha256": True,
                        "stored_read_consistent": True,
                    },
                    "passed": True,
                }
            )
            copy_reservations.append(
                {
                    "schema": "mgif-renderdoc-capture-copy-budget-v1",
                    "reservation_index": reservation_index,
                    "case": case["name"],
                    "boundary": boundary,
                    "capture_id": row["capture_id"],
                    "declared_bytes": row["capture_size_bytes"],
                    "reserved_bytes": COMPARATOR_MAX_RDC_BYTES,
                    "destination": row["capture_path"],
                    "free_bytes_before_copy": 1 << 50,
                    "required_free_bytes_before_copy": 1,
                    "actual_bytes": row["capture_size_bytes"],
                    "completed": True,
                }
            )
    capture_copy_budget = {
        "schema": "mgif-renderdoc-capture-copy-budget-v1",
        "run_root": str(root.resolve()),
        "case_count": 2,
        "boundary_count": 3,
        "max_candidates_per_boundary": SMOKE_MAX_CANDIDATES_PER_BOUNDARY,
        "max_candidate_count": 12,
        "candidate_count": 6,
        "single_rdc_cap_bytes": COMPARATOR_MAX_RDC_BYTES,
        "max_candidate_bytes": 12 * COMPARATOR_MAX_RDC_BYTES,
        "actual_candidate_bytes": sum(
            reservation["actual_bytes"] for reservation in copy_reservations
        ),
        "replay_scratch_bytes_reserved": 1,
        "safety_margin_bytes": 1,
        "by_boundary": {},
        "reservations": copy_reservations,
        "within_count_budget": True,
        "within_actual_byte_budget": True,
    }
    capture_set_validation = {
        "schema": FINAL_CAPTURE_SET_SCHEMA,
        "passed": True,
        "manifest_path": str((root / "manifest.json").resolve()),
        "expected_capture_count": 6,
        "entry_count": 6,
        "unique_canonical_path_count": 6,
        "unique_sha256_count": 6,
        "single_rdc_cap_bytes": COMPARATOR_MAX_RDC_BYTES,
        "rehash_after_all_cases": True,
        "entries": capture_set_entries,
        "formal_release": {
            "applicable": True,
            "render_mode": render_mode,
            "required_case_count": 2,
            "required_capture_count": 6,
            "required_modes": list(RELEASE_CASE_MODES),
            "required_gi_mode": "no-ddgi",
            "required_sequence": dict(RELEASE_SMOKE_SEQUENCE),
            "errors": [],
            "passed": True,
        },
        "errors": [],
    }
    manifest_path = root / "manifest.json"
    source = {
        "schema": SMOKE_MANIFEST_SCHEMA,
        "status": "passed",
        "started_utc": started,
        "completed_utc": completed,
        "manifest_path": str(manifest_path.resolve()),
        "output_directory": str(root.resolve()),
        "boundary_order": list(SMOKE_BOUNDARIES),
        "options": {
            **RELEASE_SMOKE_SEQUENCE,
            "modes": list(RELEASE_CASE_MODES),
            "render_modes": [render_mode],
            "gi_modes": ["no-ddgi"],
            "capture_control_frame": False,
        },
        "executable": str(source_exe.resolve()),
        "launch_executable": str(launch_exe.resolve()),
        "executable_evidence": executable_evidence,
        "formal_source_executable_contract": {
            "schema": "mgif-formal-source-executable-contract-v1",
            "applicable": True,
            "canonical_source_executable": str(source_exe.resolve()),
            "selected_source_executable": str(source_exe.resolve()),
            "immutable_copy_required": True,
            "checks": {
                "canonical_source_path": True,
                "canonical_filename": True,
                "stale_reactive_test_name_rejected": True,
            },
            "errors": [],
            "passed": True,
        },
        "toolchain_evidence": {
            "schema": TOOLCHAIN_EVIDENCE_SCHEMA,
            "required": True,
            "before_all_cases": toolchain_before,
            "after_all_cases": toolchain_after,
            "comparison": toolchain_comparison,
            "errors": [],
            "passed": True,
        },
        "disk_preflight": {
            "schema": "mgif-csm-smoke-disk-preflight-v1",
            "passed": True,
            "work_directory": str(root.resolve()),
            "case_count": 2,
            "boundary_count": 3,
            "accepted_capture_count": 6,
            "quarantine_capture_count": 6,
            "estimate": {
                "estimated_bytes": 1,
                "safety_margin_bytes": 1,
                "required_free_bytes": 2,
            },
            "free_bytes": 3,
            "free_after_required_bytes": 1,
            "errors": [],
        },
        "rdc_doctor": {"returncode": 0},
        "rdc_status_after": {
            "returncode": 1,
            "stdout": "",
            "stderr": "error: no active session",
        },
        "rdc_resources_before": dict(resources),
        "rdc_resources_after": dict(resources),
        "rdc_session_cleanup": cleanup,
        "capture_copy_budget": capture_copy_budget,
        "capture_set_validation": capture_set_validation,
        "cross_case_pose_validation": {"passed": True},
        "cases": cases,
    }
    first_case = cases[0]
    return source, first_case, captures_by_case[first_case["name"]]
def _self_test_semantic_manifest(
    resource_id: int,
    *,
    automatic_names: bool = False,
) -> dict[str, Any]:
    discovery: dict[str, Any] = {}
    exports: dict[str, Any] = {}
    usage: dict[str, Any] = {}
    automatic_bases = {
        "csm": "2D Depth Attachment",
        "light": "2D Color Attachment",
        "taa": "2D Image",
        "final": "2D Image",
    }
    for role in ROLE_ORDER:
        resource = {
            "resource_id": resource_id,
            "name": (
                f"{automatic_bases[role]} {resource_id}"
                if automatic_names
                else f"stable-{role}"
            ),
            "is_texture": True,
            "width": 64,
            "height": 64,
            "depth": 1,
            "array_size": 2 if role == "csm" else 1,
            "mips": 1,
            "samples": 1,
            "byte_size": 4096,
            "format": "D32_FLOAT" if role == "csm" else "R16G16B16A16_FLOAT",
            "format_type": "Regular",
            "component_type": "Float",
            "component_count": 1 if role == "csm" else 4,
            "component_byte_width": 4 if role == "csm" else 2,
            "bgra": False,
            "srgb": False,
            "creation_flags": 0,
        }
        if not automatic_names:
            resource["autogenerated_name"] = False
        discovery[role] = {
            "resource": resource,
            "marker": {"name": role.upper(), "ancestors": ["Frame"]},
            "passthrough": False,
        }
        exports[role] = (
            [
                {"kind": "npy", "shape": [64, 64], "dtype": "float32", "slice": 0},
                {"kind": "npy", "shape": [64, 64], "dtype": "float32", "slice": 1},
            ]
            if role == "csm"
            else {"kind": "npy", "shape": [64, 64, 4], "dtype": "float32", "slice": 0}
        )
        usage[role] = [{"eid": 1, "usage": "ShaderRead"}]
    return {
        "discovery": discovery,
        "exports": exports,
        "selected_resource_usage": usage,
    }


def _self_test_release_extraction_fixture(
    root: Path,
    *,
    boundary: str,
    history_write_index: int,
    edge_shift_px: float = 0.0,
    receiver_normals: bool = True,
) -> dict[str, Any]:
    import numpy as np

    root.mkdir(parents=True, exist_ok=True)
    shape = (128, 192)
    _, x = np.mgrid[: shape[0], : shape[1]]
    transition = 1.0 / (
        1.0
        + np.exp(
            np.clip(
                -(x.astype(np.float32) - (104.0 + edge_shift_px)) / 2.5,
                -80.0,
                80.0,
            )
        )
    )
    lighting = (1.0 - 0.45 * transition).astype(np.float32)
    depth = np.full(shape, 0.5, dtype=np.float32)
    base_color = np.full((*shape, 4), 0.72, dtype=np.float32)
    base_color[..., 3] = 1.0
    normals = np.zeros((*shape, 3), dtype=np.float32)
    normals[..., 1 if receiver_normals else 0] = 1.0
    color = np.repeat(lighting[..., None], 4, axis=2)
    color[..., 3] = 1.0
    csm = np.ones(shape, dtype=np.float32)
    csm[24:104, 32:160] = 0.5

    def resource(
        resource_id: int,
        name: str,
        fmt: str = "R16G16B16A16_FLOAT",
        *,
        array_size: int = 1,
    ) -> dict[str, Any]:
        upper = fmt.upper()
        if "D32" in upper:
            component_type = "Depth"
            component_count = 1
            component_byte_width = 4
        elif "R16G16_FLOAT" in upper:
            component_type = "Float"
            component_count = 2
            component_byte_width = 2
        elif "R8G8B8A8" in upper:
            component_type = "UNorm"
            component_count = 4
            component_byte_width = 1
        else:
            component_type = "Float"
            component_count = 4
            component_byte_width = 2
        return {
            "resource_id": resource_id,
            "name": name,
            "autogenerated_name": False,
            "is_texture": True,
            "width": shape[1],
            "height": shape[0],
            "depth": 1,
            "array_size": array_size,
            "mips": 1,
            "samples": 1,
            "byte_size": (
                shape[0]
                * shape[1]
                * component_count
                * component_byte_width
                * array_size
            ),
            "format": fmt,
            "format_type": "Regular",
            "component_type": component_type,
            "component_count": component_count,
            "component_byte_width": component_byte_width,
            "bgra": False,
            "srgb": False,
            "creation_flags": 0,
        }

    write_name = f"GPUDrivenSceneColorHistory{history_write_index}"
    read_index = history_write_index ^ 1
    read_name = f"GPUDrivenSceneColorHistory{read_index}"
    csm_resource = resource(90, "CSMDepth", "D32_FLOAT", array_size=4)
    resources = {
        "scene_depth": resource(103, "SceneDepth", "D32_FLOAT"),
        "base_color": resource(100, "SceneColor0"),
        "packed_normal": resource(101, "SceneColor1"),
        "scene_color_hdr": resource(104, "GPUDrivenSceneColorHDR"),
        "velocity": resource(107, "GPUDrivenVelocity", "R16G16_FLOAT"),
        "history_read": _d_history_identity(resource(108 + read_index, read_name)),
        "history_write": _d_history_identity(
            resource(108 + history_write_index, write_name)
        ),
        "final": resource(120, "OutputTexture", "R8G8B8A8_UNORM"),
    }

    def save(
        label: str,
        array: Any,
        *,
        eid: int,
        role: str,
        source_resource: dict[str, Any],
        channel_order: list[str],
        evidence_only: bool = False,
        dimension_class: str = "screen",
        slice_index: int = 0,
    ) -> dict[str, Any]:
        value = np.asarray(array, dtype=np.float32)
        path = root / f"{label}.npy"
        np.save(path, value, allow_pickle=False)
        bytes_per_pixel = int(source_resource["component_count"]) * int(
            source_resource["component_byte_width"]
        )
        source_format = {
            "name": source_resource["format"],
            "format_type": source_resource["format_type"],
            "component_type": source_resource["component_type"],
            "component_count": source_resource["component_count"],
            "component_byte_width": source_resource["component_byte_width"],
            "bgra": source_resource["bgra"],
            "bytes_per_pixel": bytes_per_pixel,
        }
        expected_raw_bytes = shape[0] * shape[1] * bytes_per_pixel
        readback = {
            "schema": "mgif-renderdoc-tight-readback-v1",
            "width": shape[1],
            "height": shape[0],
            "texture_depth": 1,
            "array_size": source_resource["array_size"],
            "samples": 1,
            "mip": 0,
            "slice": slice_index,
            "sample": 0,
            "bytes_per_pixel": bytes_per_pixel,
            "expected_raw_bytes": expected_raw_bytes,
            "raw_byte_length": expected_raw_bytes,
            "raw_byte_length_exact": True,
            "tightly_packed_required": True,
            "packed_decode": (
                "D32_FLOAT" if source_resource["component_type"] == "Depth" else None
            ),
            "decoded_channel_count": len(channel_order),
            "source_format": dict(source_format),
        }
        extent = {"width": shape[1], "height": shape[0]}
        validation = {
            "schema": NPY_EXPORT_EVIDENCE_SCHEMA,
            "passed": True,
            "errors": [],
            "path": str(path.resolve()),
            "role": role,
            "shape": [int(item) for item in value.shape],
            "dtype": str(value.dtype),
            "channel_order": list(channel_order),
            "source_format": dict(source_format),
            "source_resource_id": int(source_resource["resource_id"]),
            "screen_extent": extent,
            "dimension_class": dimension_class,
            "evidence_only": evidence_only,
            "allow_pickle": False,
            "reopened_after_write": True,
        }
        return {
            "kind": "npy",
            "path": path.name,
            "shape": [int(item) for item in value.shape],
            "dtype": str(value.dtype),
            "channel_order": list(channel_order),
            "source_channel_order": (
                ["D"]
                if source_resource["component_type"] == "Depth"
                else ["R", "G", "B", "A"][: source_resource["component_count"]]
            ),
            "source_format": source_format,
            "source_resource_id": int(source_resource["resource_id"]),
            "screen_extent": extent,
            "dimension_class": dimension_class,
            "evidence_only": evidence_only,
            "readback_contract": readback,
            "readback_budget": {
                "role": role,
                "expected_bytes": expected_raw_bytes,
                "actual_bytes": expected_raw_bytes,
                "single_readback_cap_bytes": COMPARATOR_MAX_READBACK_BYTES,
                "completed": True,
            },
            "write_budget": {
                "role": role,
                "kind": "npy",
                "path": str(path.resolve()),
                "payload_bytes": int(value.nbytes),
                "estimated_write_bytes": int(path.stat().st_size) + NPY_HEADER_RESERVE_BYTES,
                "free_bytes_before_write": 1 << 50,
                "required_free_bytes_before_write": 1,
                "actual_file_bytes": int(path.stat().st_size),
                "completed": True,
            },
            "npy_validation": validation,
            "slice": slice_index,
            "mip": 0,
            "sample": 0,
            "eid": eid,
        }

    release_exports = {
        "scene_depth": save(
            "scene_depth",
            depth,
            eid=20,
            role="scene_depth",
            source_resource=resources["scene_depth"],
            channel_order=["D"],
        ),
        "base_color": save(
            "base_color",
            base_color,
            eid=20,
            role="base_color",
            source_resource=resources["base_color"],
            channel_order=["R", "G", "B", "A"],
        ),
        "world_normals": save(
            "world_normals",
            normals,
            eid=20,
            role="world_normals",
            source_resource=resources["packed_normal"],
            channel_order=["X", "Y", "Z"],
        ),
        "scene_color_hdr": save(
            "scene_color_hdr",
            color,
            eid=30,
            role="scene_color_hdr",
            source_resource=resources["scene_color_hdr"],
            channel_order=["R", "G", "B", "A"],
        ),
        "history_read": save(
            "history_read",
            color,
            eid=30,
            role="history_read",
            source_resource=resources["history_read"],
            channel_order=["R", "G", "B", "A"],
            evidence_only=True,
        ),
        "history_write": save(
            "history_write",
            color,
            eid=40,
            role="history_write",
            source_resource=resources["history_write"],
            channel_order=["R", "G", "B", "A"],
        ),
        "final": save(
            "final",
            color,
            eid=40,
            role="final",
            source_resource=resources["final"],
            channel_order=["R", "G", "B", "A"],
        ),
    }
    csm_exports = [
        save(
            f"csm_{slice_index}",
            csm,
            eid=10 + slice_index,
            role=f"csm[{slice_index}]",
            source_resource=csm_resource,
            channel_order=["D"],
            dimension_class="csm",
            slice_index=slice_index,
        )
        for slice_index in range(4)
    ]
    markers = {
        "csm": {"name": "GPUDrivenCSMShadow", "selection": {"ambiguous": False}},
        "light": {"name": "GPUDrivenLightPass", "selection": {"ambiguous": False}},
        "taa": {"name": "GPUDrivenTAAResolve", "selection": {"ambiguous": False}},
        "final": {"name": "GPUDrivenFinalColor", "selection": {"ambiguous": False}},
    }
    discovery = {
        "csm": {
            "resource": csm_resource,
            "selection": {"ambiguous": False},
            "marker": markers["csm"],
        },
        "light": {
            "resource": resources["scene_color_hdr"],
            "eid": 20,
            "selection": {"ambiguous": False},
            "marker": markers["light"],
            "passthrough": False,
            "actual_draw_binding": True,
        },
        "taa": {
            "resource": resources["history_write"],
            "eid": 30,
            "selection": {"ambiguous": False},
            "marker": markers["taa"],
            "passthrough": False,
            "actual_draw_binding": True,
        },
        "final": {
            "resource": resources["final"],
            "eid": 40,
            "selection": {"ambiguous": False},
            "marker": markers["final"],
            "passthrough": False,
            "actual_draw_binding": True,
        },
    }
    descriptor_bindings = {
        "base_color": {"array_element": 0, "resource": resources["base_color"]},
        "packed_normal": {
            "array_element": 1,
            "resource": resources["packed_normal"],
        },
        "scene_depth": {"array_element": 3, "resource": resources["scene_depth"]},
        "scene_color_hdr": {
            "array_element": 4,
            "resource": resources["scene_color_hdr"],
        },
        "velocity": {"array_element": 7, "resource": resources["velocity"]},
        "history_read": {
            "array_element": 8,
            "resource": resources["history_read"],
        },
        "final_history_write": {
            "array_element": 9,
            "resource": resources["history_write"],
        },
    }

    def draw_snapshot(
        eid: int,
        entry: str,
        shader_id: int,
        pipeline_id: int,
        output: dict[str, Any],
    ) -> dict[str, Any]:
        return {
            "eid": eid,
            "action": {
                "name": "Draw(3, 1)",
                "is_draw": True,
                "vertex_count": 3,
                "instance_count_raw": 1,
                "instance_count": 1,
            },
            "shaders": {
                "ps": {"resource_id": shader_id, "entry_point": entry}
            },
            "graphics_pipeline": pipeline_id,
            "outputs": [output],
        }

    light_snapshot = draw_snapshot(
        20,
        "fragmentHdrMain",
        699,
        799,
        resources["scene_color_hdr"],
    )
    taa_snapshot = draw_snapshot(
        30,
        "fragmentTAAResolveMain",
        700,
        800,
        resources["history_write"],
    )
    final_snapshot = draw_snapshot(
        40,
        "fragmentFinalColorMain",
        701,
        801,
        resources["final"],
    )

    def draw_evidence(
        role: str,
        marker_name: str,
        entry: str,
        snapshot: dict[str, Any],
        output: dict[str, Any],
    ) -> dict[str, Any]:
        return {
            "marker_name": marker_name,
            "expected_marker_name": marker_name,
            "draw_eid": snapshot["eid"],
            "draw_count": 1,
            "snapshot_eid": snapshot["eid"],
            "snapshot_matches_last_draw": True,
            "action": snapshot["action"],
            "fragment_shader": snapshot["shaders"]["ps"],
            "expected_fragment_entry": entry,
            "graphics_pipeline": snapshot["graphics_pipeline"],
            "color_output": output,
            "output_written_in_marker": True,
            "role": role,
        }

    camera = {
        "eid": 20,
        "matrix_convention": "column_vectors",
        "renderdoc_value_layout": "logical_rows",
        "framebuffer_y_to_ndc": "top_to_negative_one",
        "depth_range": "zero_to_one",
        "viewport": [0, 0, shape[1], shape[0]],
        "clip_from_world": np.eye(4, dtype=np.float64).tolist(),
        "world_from_clip": np.eye(4, dtype=np.float64).tolist(),
        "view_from_world": np.eye(4, dtype=np.float64).tolist(),
        "world_from_view": np.eye(4, dtype=np.float64).tolist(),
        "source_paths": {},
        "inverse_max_abs_error": 0.0,
        "errors": [],
    }
    release_evidence = {
        "schema": "mgif-csm-release-resource-binding-v1",
        "render_mode": "taa-on",
        "taa_required": True,
        "passed": True,
        "passes": {
            "light": {
                "draw_count": 1,
                "last_draw_eid": 20,
                "snapshot": light_snapshot,
                "errors": [],
            },
            "taa": {
                "draw_count": 1,
                "last_draw_eid": 30,
                "snapshot": taa_snapshot,
                "errors": [],
            },
            "final": {
                "draw_count": 1,
                "last_draw_eid": 40,
                "snapshot": final_snapshot,
                "errors": [],
            },
        },
        "draw_evidence": {
            "light": draw_evidence(
                "light",
                "GPUDrivenLightPass",
                "fragmentHdrMain",
                light_snapshot,
                resources["scene_color_hdr"],
            ),
            "taa": draw_evidence(
                "taa",
                "GPUDrivenTAAResolve",
                "fragmentTAAResolveMain",
                taa_snapshot,
                resources["history_write"],
            ),
            "final": draw_evidence(
                "final",
                "GPUDrivenFinalColor",
                "fragmentFinalColorMain",
                final_snapshot,
                resources["final"],
            ),
        },
        "taa_resolve": {
            "required": True,
            "inner_marker_name": "GPUDrivenTAAResolve",
            "outer_marker_rejected": "GPUDrivenTAAResolvePass",
            "draw_eid": 30,
            "draw_count": 1,
            "snapshot_eid": 30,
            "snapshot_matches_last_draw": True,
            "action": taa_snapshot["action"],
            "fragment_shader": taa_snapshot["shaders"]["ps"],
            "graphics_pipeline": 800,
            "history_write": resources["history_write"],
            "history_read": resources["history_read"],
            "scene_color_hdr": resources["scene_color_hdr"],
            "velocity": resources["velocity"],
            "history_valid": {
                "readable": True,
                "value": 1.0,
                "paths": ["postProcess.params5"],
                "errors": [],
            },
            "descriptor_bindings": descriptor_bindings,
            "passthrough": False,
        },
        "taa_absence": None,
        "no_post": None,
        "resources": resources,
        "camera": camera,
        "errors": [],
    }
    release_validation = {
        "schema": "mgif-shadow-edge-extractor-input-validation-v1",
        "render_mode": "taa-on",
        "passed": True,
        "errors": [],
        "screen_extent": {"width": shape[1], "height": shape[0]},
        "history_read_semantics": "evidence-only",
        "roles": {
            role: {
                "shape": export["shape"],
                "dtype": export["dtype"],
                "channel_order": export["channel_order"],
                "source_format": export["source_format"],
                "evidence_only": export["evidence_only"],
            }
            for role, export in release_exports.items()
        },
    }
    release_inputs = {
        "schema": RELEASE_INPUTS_SCHEMA,
        "render_mode": "taa-on",
        "passed": True,
        "camera": camera,
        "screen_extent": {"width": shape[1], "height": shape[0]},
        "validation": release_validation,
        "resources": {
            role: {
                "resource": resources[
                    "packed_normal" if role == "world_normals" else role
                ],
                "export": export,
                "evidence_only": role == "history_read",
            }
            for role, export in release_exports.items()
        },
        "errors": [],
    }
    frame = RELEASE_BOUNDARY_FRAMES[boundary]
    return {
        "tool_version": TOOL_VERSION,
        "extractor_tool_evidence": _collect_tool_file_evidence(
            Path(__file__),
            role="comparator",
        ),
        "release_gate": True,
        "release_render_mode": "taa-on",
        "export_budget": {
            "schema": "mgif-rdc-daemon-export-budget-v1",
            "export_budget_bytes": COMPARATOR_EXTRACT_BYTES_PER_CAPTURE,
            "single_readback_cap_bytes": COMPARATOR_MAX_READBACK_BYTES,
            "safety_margin_bytes": DISK_SAFETY_MIN_BYTES,
            "free_bytes_at_start": 1 << 50,
            "required_free_bytes_at_start": 1,
            "readback_expected_bytes": sum(
                int(row["readback_budget"]["expected_bytes"])
                for row in [*csm_exports, *release_exports.values()]
            ),
            "readback_actual_bytes": sum(
                int(row["readback_budget"]["actual_bytes"])
                for row in [*csm_exports, *release_exports.values()]
            ),
            "npy_committed_bytes": sum(
                int(row["write_budget"]["actual_file_bytes"])
                for row in [*csm_exports, *release_exports.values()]
            ),
            "readbacks": [
                row["readback_budget"]
                for row in [*csm_exports, *release_exports.values()]
            ],
            "writes": [
                row["write_budget"]
                for row in [*csm_exports, *release_exports.values()]
            ],
        },
        "errors": [],
        "session_cleanup": {
            "schema": "mgif-rdc-comparator-replay-session-cleanup-v4",
            "closed": True,
            "passed": True,
            "errors": [],
            **_self_test_direct_cleanup_fields(),
            "state_file": {"absent_after_cleanup": True},
            "deadline": {"exceeded": False},
            "daemon_ownership": {
                "established": True,
                "errors": [],
                "state_path_match": True,
                "state_capture_path_match": True,
                "daemon_capture_path_metadata_match": True,
                "stable_image_match": True,
                "creation_time_matches_handle": True,
                "command_bound_to_held_identity": True,
                "state_publication_boundary_exact": True,
                "state_file_volume_verified": True,
                "exact_creation_order_clock_match": True,
                "strict_creation_precedes_publication": True,
                "creation_equals_publication": False,
                "process_created_for_open": True,
                "state_pid_matches_handle": True,
                "snapshot_evidence_complete": True,
                "process_created_before_state_file": True,
                "stable_process_identity": _self_test_stable_daemon_identity(),

            },
            "owned_daemon_residue": [],
        },
        "capture_file_identity": {
            "path": str((root / "capture.rdc").resolve()),
            "size_bytes": 1,
            "sha256": "a" * 64,
            "renderdoc_reported_path_match": True,
        },
        "runtime_disk_budget": {
            "passed": True,
            "capture_size_bytes": 1,
            "single_rdc_cap_bytes": COMPARATOR_MAX_RDC_BYTES,
            "free_bytes_before_open": 1 << 50,
            "required_free_bytes_before_open": 1,
        },
        "markers": markers,
        "automation_frame_markers": [
            {
                "mode": "csm-translate-stop",
                "boundary": boundary,
                "frame": frame,
                "nested_in_gpu_driven_csm_shadow": True,
            }
        ],
        "pass_segments": {
            "csm": [{"draw_count": 1, "last_draw_eid": 10}],
            "light": [{"draw_count": 1, "last_draw_eid": 20}],
            "taa": [{"draw_count": 1, "last_draw_eid": 30}],
            "final": [{"draw_count": 1, "last_draw_eid": 40}],
        },
        "pipeline_snapshots": {
            "csm": [{"eid": 10}],
            "light": [light_snapshot],
            "taa": [taa_snapshot],
            "final": [final_snapshot],
        },
        "discovery": discovery,
        "release_resource_evidence": release_evidence,
        "release_inputs": release_inputs,
        "exports": {
            "csm": csm_exports,
            "light": release_exports["scene_color_hdr"],
            "taa": release_exports["history_write"],
            "final": release_exports["final"],
        },
        "selected_resource_usage": {
            "csm": [{"eid": 10, "usage": "DepthStencilTarget"}],
            "light": [{"eid": 20, "usage": "ColorTarget"}],
            "taa": [{"eid": 30, "usage": "ColorTarget"}],
            "final": [{"eid": 40, "usage": "ColorTarget"}],
        },
        "projection_sources": [
            {
                "cascade": cascade,
                "eid": 10 + cascade,
                "matrices": [
                    {
                        "path": "vs.csm.lightViewProj",
                        "rows": 4,
                        "columns": 4,
                        "value": [
                            2.0 ** (-cascade), 0.0, 0.0, 0.0,
                            0.0, 2.0 ** (-cascade), 0.0, 0.0,
                            0.125, 0.375, 1.0, 0.0,
                            0.0, 0.0, 0.0, 1.0,
                        ],
                    }
                ],
            }
            for cascade in range(4)
        ],
    }

def _self_test_no_post_extraction_fixture(
    root: Path,
    *,
    boundary: str,
    edge_shift_px: float = 0.0,
    receiver_normals: bool = True,
) -> dict[str, Any]:
    import copy

    manifest = _self_test_release_extraction_fixture(
        root,
        boundary=boundary,
        history_write_index=RELEASE_BOUNDARY_FRAMES[boundary] & 1,
        edge_shift_px=edge_shift_px,
        receiver_normals=receiver_normals,
    )
    manifest["release_render_mode"] = "no-post"
    manifest["markers"]["taa"] = None
    manifest["pipeline_snapshots"]["taa"] = []
    manifest["pass_segments"]["taa"] = []
    manifest["discovery"]["taa"] = None
    manifest["exports"]["taa"] = None
    manifest["selected_resource_usage"].pop("taa", None)

    evidence = manifest["release_resource_evidence"]
    evidence["render_mode"] = "no-post"
    evidence["taa_required"] = False
    evidence["passes"].pop("taa", None)
    evidence["draw_evidence"].pop("taa", None)
    for role in ("velocity", "history_read", "history_write"):
        evidence["resources"].pop(role, None)
    absence = {
        "required_absent": True,
        "inner_markers": [],
        "resolve_draws": [],
        "inspected_draw_count": 2,
        "errors": [],
        "passed": True,
    }
    evidence["taa_absence"] = absence
    evidence["taa_resolve"] = {
        "required": False,
        "absent": True,
        "absence_evidence": copy.deepcopy(absence),
        "passthrough": None,
    }
    evidence["no_post"] = {
        "required": True,
        "taa_absence": copy.deepcopy(absence),
        "descriptor_bindings": {
            "final_scene_color_hdr": {
                "array_element": 4,
                "resource": copy.deepcopy(evidence["resources"]["scene_color_hdr"]),
            }
        },
        "scene_color_hdr": copy.deepcopy(evidence["resources"]["scene_color_hdr"]),
        "final": copy.deepcopy(evidence["resources"]["final"]),
    }

    release_inputs = manifest["release_inputs"]
    release_inputs["render_mode"] = "no-post"
    for role in ("history_read", "history_write"):
        release_inputs["resources"].pop(role, None)
    validation = release_inputs["validation"]
    validation["render_mode"] = "no-post"
    validation["history_read_semantics"] = "not-applicable-no-post"
    for role in ("history_read", "history_write"):
        validation["roles"].pop(role, None)

    budget = manifest["export_budget"]
    budget["readbacks"] = [
        row
        for row in budget["readbacks"]
        if row.get("role") not in ("history_read", "history_write")
    ]
    budget["writes"] = [
        row
        for row in budget["writes"]
        if row.get("role") not in ("history_read", "history_write")
    ]
    budget["readback_expected_bytes"] = sum(
        int(row["expected_bytes"]) for row in budget["readbacks"]
    )
    budget["readback_actual_bytes"] = sum(
        int(row["actual_bytes"]) for row in budget["readbacks"]
    )
    budget["npy_committed_bytes"] = sum(
        int(row["actual_file_bytes"]) for row in budget["writes"]
    )
    return manifest

def _run_self_tests() -> int:
    tests: list[tuple[str, Any]] = []

    def test(name: str):
        def register(function: Any) -> Any:
            tests.append((name, function))
            return function
        return register

    def structured_cleanup_fixture() -> dict[str, Any]:
        resources = {
            "available": True,
            "errors": [],
            "process_access_denied_count": 0,
            "sessions": {},
            "daemons": {},
        }
        session = {
            "session": "replay-first-still",
            "passed": True,
            **_self_test_direct_cleanup_fields(),
            "state_file": {
                "path": "replay-first-still.json",
                "before": {"exists": False},
                "after_open": {"exists": True, "valid": True},
                "after_cleanup": {"exists": False},
                "absent_after_cleanup": True,
            },
            "daemon_ownership": {
                "established": True,
                "errors": [],
                "state_path_match": True,
                "state_capture_path_match": True,
                "daemon_capture_path_metadata_match": True,
                "stable_image_match": True,
                "state_publication_boundary_exact": True,
                "state_file_volume_verified": True,
                "exact_creation_order_clock_match": True,
                "strict_creation_precedes_publication": True,
                "creation_equals_publication": False,
                "snapshot_evidence_complete": True,
                "process_created_before_state_file": True,
                "stable_process_identity": _self_test_stable_daemon_identity(),

            },
            "owned_daemon_residue": [],
            "errors": [],
        }
        return {
            "rdc_status_after": {
                "returncode": 1,
                "stdout": "",
                "stderr": "error: no active session",
                "classification": "inactive",
            },
            "rdc_resources_before": dict(resources),
            "rdc_resources_after": dict(resources),
            "rdc_session_cleanup": {
                "schema": RDC_CLEANUP_SCHEMA,
                "passed": True,
                "closed": True,
                "ownership_model": {
                    "hard_gate_scope": "owned additions and owned residue only",
                    "external_additions_are_diagnostic": True,
                },
                "named_replay_sessions": [session],
                "run_resource_diff": {
                    "available": True,
                    "before_errors": [],
                    "after_errors": [],
                    "before_process_access_denied_count": 0,
                    "after_process_access_denied_count": 0,
                    "added_daemons": [],
                    "added_session_files": [],
                    "removed_daemons": [],
                    "removed_session_files": [],
                    "changed_session_files": [],
                },
                "owned_added_session_files": [],
                "owned_added_daemons": [],
                "owned_session_file_residue": [],
                "owned_daemon_residue": [],
                "external_added_session_files": [],
                "external_added_daemons": [],
                "errors": [],
            },
        }
    @test("final manifest structure is hard-gated")
    def test_final_manifest_structure() -> None:
        with tempfile.TemporaryDirectory(prefix="rdc_csm_selftest_") as directory:
            root = Path(directory)
            source, _, _ = _self_test_case_fixture(root)
            assert not _validate_release_manifest_contract(source, root / "manifest.json")
            source["status"] = "running"
            assert any(
                "top-level status" in error
                for error in _validate_release_manifest_contract(source, root / "manifest.json")
            )

    @test("cleanup v2 gates owned residue but treats external additions as diagnostic")
    def test_release_cleanup_contract() -> None:
        clean = structured_cleanup_fixture()
        assert not _validate_release_cleanup_contract(clean)

        legacy = structured_cleanup_fixture()
        legacy["rdc_session_cleanup"]["schema"] = "rdc-session-cleanup-v1"
        assert any(
            "schema" in error for error in _validate_release_cleanup_contract(legacy)
        )

        active_named = structured_cleanup_fixture()
        active_named["rdc_session_cleanup"]["named_replay_sessions"][0][
            "post_status"
        ] = {
            "returncode": 0,
            "stdout": "session: replay-first-still",
            "stderr": "",
            "classification": "active",
        }
        assert any(
            "post_status" in error
            for error in _validate_release_cleanup_contract(active_named)
        )

        external = structured_cleanup_fixture()
        external_record = {"identity": "9999@external"}
        external["rdc_session_cleanup"]["run_resource_diff"]["added_daemons"] = [
            external_record
        ]
        external["rdc_session_cleanup"]["external_added_daemons"] = [
            external_record
        ]
        assert not _validate_release_cleanup_contract(external), (
            _validate_release_cleanup_contract(external)
        )

        owned = structured_cleanup_fixture()
        owned["rdc_session_cleanup"]["owned_added_daemons"] = [
            {"identity": "1234@owned"}
        ]
        assert any(
            "owned_added_daemons" in error
            for error in _validate_release_cleanup_contract(owned)
        )

    @test("cleanup accepts direct terminal state-file absence evidence")
    def test_cleanup_accepts_terminal_absence() -> None:
        source = structured_cleanup_fixture()
        cleanup_errors = _validate_release_cleanup_contract(source)
        assert not cleanup_errors, cleanup_errors
        session = source["rdc_session_cleanup"]["named_replay_sessions"][0]
        assert session["state_file"]["after_open"]["exists"] is True
        assert _state_file_absence(session) is True

        after_cleanup_only = structured_cleanup_fixture()
        del after_cleanup_only["rdc_session_cleanup"]["named_replay_sessions"][0][
            "state_file"
        ]["absent_after_cleanup"]
        assert not _validate_release_cleanup_contract(after_cleanup_only)

        absent_flag_only = structured_cleanup_fixture()
        del absent_flag_only["rdc_session_cleanup"]["named_replay_sessions"][0][
            "state_file"
        ]["after_cleanup"]
        assert not _validate_release_cleanup_contract(absent_flag_only)

    @test("cleanup rejects a state file that exists at the terminal snapshot")
    def test_cleanup_rejects_terminal_state_file() -> None:
        source = structured_cleanup_fixture()
        state_file = source["rdc_session_cleanup"]["named_replay_sessions"][0][
            "state_file"
        ]
        state_file["after_cleanup"]["exists"] = True
        state_file["absent_after_cleanup"] = False
        assert any(
            "state file absence" in error
            for error in _validate_release_cleanup_contract(source)
        )

        historical_only = structured_cleanup_fixture()
        historical_state_file = historical_only["rdc_session_cleanup"][
            "named_replay_sessions"
        ][0]["state_file"]
        del historical_state_file["after_cleanup"]
        del historical_state_file["absent_after_cleanup"]
        assert any(
            "state file absence" in error
            for error in _validate_release_cleanup_contract(historical_only)
        )
    @test("cleanup rejects unavailable or errored empty run resource diffs")
    def test_cleanup_rejects_unavailable_resource_diff() -> None:
        unavailable = structured_cleanup_fixture()
        unavailable["rdc_session_cleanup"]["run_resource_diff"][
            "available"
        ] = False
        assert any(
            "run_resource_diff.available is not true" in error
            for error in _validate_release_cleanup_contract(unavailable)
        )

        errored = structured_cleanup_fixture()
        errored["rdc_session_cleanup"]["run_resource_diff"][
            "before_errors"
        ] = ["session scan failed"]
        assert any(
            "run_resource_diff.before_errors is not empty" in error
            for error in _validate_release_cleanup_contract(errored)
        )

    @test("cleanup rejects errored or unavailable resource snapshots")
    def test_cleanup_rejects_snapshot_errors() -> None:
        errored = structured_cleanup_fixture()
        errored["rdc_resources_before"]["errors"] = ["daemon scan failed"]
        assert any(
            "rdc_resources_before.errors is not empty" in error
            for error in _validate_release_cleanup_contract(errored)
        )

        unavailable = structured_cleanup_fixture()
        unavailable["rdc_resources_after"]["available"] = False
        assert any(
            "rdc_resources_after.available is not true" in error
            for error in _validate_release_cleanup_contract(unavailable)
        )

    @test("release manifest locks the 8/24/8 smoke sequence")
    def test_release_smoke_sequence() -> None:
        with tempfile.TemporaryDirectory(prefix="rdc_csm_selftest_") as directory:
            root = Path(directory)
            source, case, _ = _self_test_case_fixture(root)
            source["options"].update(
                {
                    "warmup_frames": 1,
                    "motion_frames": 2,
                    "hold_frames": 3,
                }
            )
            short_frames = {
                "last-moving": 2,
                "first-still": 3,
                "settled": 5,
            }
            case["expected_automation_frames"] = short_frames
            for boundary, frame in short_frames.items():
                case["boundaries"][boundary]["ready"]["frame"] = frame
            errors = _validate_release_manifest_contract(
                source,
                root / "manifest.json",
            )
            assert any("options.warmup_frames=1" in error for error in errors)
            assert any("options.motion_frames=2" in error for error in errors)
            assert any("options.hold_frames=3" in error for error in errors)
            assert any(
                "expected_automation_frames.last-moving" in error
                for error in errors
            )
            assert any(
                "boundaries.settled.ready.frame" in error
                for error in errors
            )

    @test("three boundary files require distinct paths and hashes")
    def test_distinct_boundary_files() -> None:
        with tempfile.TemporaryDirectory(prefix="rdc_csm_selftest_") as directory:
            root = Path(directory)
            source, case, captures = _self_test_case_fixture(root)
            valid = _validate_release_capture_bindings(root / "manifest.json", source, case, captures)
            assert valid["passed"], valid["errors"]
            same_path = {boundary: captures["last-moving"] for boundary in SMOKE_BOUNDARIES}
            for boundary in SMOKE_BOUNDARIES:
                entry = case["boundaries"][boundary]
                entry["capture_path"] = str(captures["last-moving"].resolve())
                entry["capture_size_bytes"] = captures["last-moving"].stat().st_size
                entry["capture_sha256"] = _sha256_file(captures["last-moving"])
            duplicate_path = _validate_release_capture_bindings(
                root / "manifest.json", source, case, same_path
            )
            assert any("different files" in error for error in duplicate_path["errors"])
            source, case, captures = _self_test_case_fixture(root / "second")
            common_bytes = b"identical-rdc"
            for boundary, path in captures.items():
                path.write_bytes(common_bytes)
                case["boundaries"][boundary]["capture_size_bytes"] = len(common_bytes)
                case["boundaries"][boundary]["capture_sha256"] = _sha256_file(path)
            duplicate_hash = _validate_release_capture_bindings(
                root / "second" / "manifest.json", source, case, captures
            )
            assert any("different SHA-256" in error for error in duplicate_hash["errors"])

    @test("captureId zero is valid and invalid integers are rejected")
    def test_capture_id_contract() -> None:
        with tempfile.TemporaryDirectory(prefix="rdc_csm_selftest_") as directory:
            root = Path(directory)
            source, case, captures = _self_test_case_fixture(root)
            valid = _validate_release_capture_bindings(
                root / "manifest.json",
                source,
                case,
                captures,
            )
            assert valid["passed"], valid["errors"]
            assert valid["capture_ids"]["last-moving"] == 0

            source, case, captures = _self_test_case_fixture(root / "negative")
            case["boundaries"]["last-moving"]["capture_id"] = -1
            case["boundaries"]["last-moving"]["renderdoc_capture"][
                "captureId"
            ] = -1
            negative = _validate_release_capture_bindings(
                root / "negative" / "manifest.json",
                source,
                case,
                captures,
            )
            assert any(
                "capture_id must be a non-negative integer" in error
                for error in negative["errors"]
            )
            assert any(
                "renderdoc_capture.captureId must be a non-negative integer"
                in error
                for error in negative["errors"]
            )

            source, case, captures = _self_test_case_fixture(root / "noninteger")
            case["boundaries"]["last-moving"]["capture_id"] = "0"
            case["boundaries"]["last-moving"]["renderdoc_capture"][
                "captureId"
            ] = 0.0
            noninteger = _validate_release_capture_bindings(
                root / "noninteger" / "manifest.json",
                source,
                case,
                captures,
            )
            assert any(
                "capture_id must be a non-negative integer" in error
                for error in noninteger["errors"]
            )
            assert any(
                "renderdoc_capture.captureId must be a non-negative integer"
                in error
                for error in noninteger["errors"]
            )

    @test("RDC frame marker binds mode boundary and frame")
    def test_rdc_frame_marker() -> None:
        with tempfile.TemporaryDirectory(prefix="rdc_csm_selftest_") as directory:
            _, case, _ = _self_test_case_fixture(Path(directory))
            manifest = {
                "automation_frame_markers": [
                    {
                        "mode": case["mode"],
                        "boundary": "first-still",
                        "frame": RELEASE_BOUNDARY_FRAMES["first-still"],
                        "nested_in_gpu_driven_csm_shadow": True,
                    }
                ]
            }
            result = _validate_automation_frame_marker(manifest, case, "first-still")
            assert result["passed"], result["errors"]
            manifest["automation_frame_markers"][0]["frame"] += 1
            assert not _validate_automation_frame_marker(manifest, case, "first-still")["passed"]

    @test("resource semantics normalize automatic IDs but preserve explicit names")
    def test_resource_semantics() -> None:
        stable_explicit = {
            boundary: (_self_test_semantic_manifest(index), Path("."))
            for index, boundary in enumerate(SMOKE_BOUNDARIES)
        }
        result = _validate_three_capture_resource_semantics(
            stable_explicit,
            render_mode="taa-on",
        )
        assert result["passed"], result["errors"]
        stable_explicit["settled"][0]["discovery"]["csm"]["resource"][
            "format"
        ] = "D16_UNORM"
        assert not _validate_three_capture_resource_semantics(
            stable_explicit,
            render_mode="taa-on",
        )[
            "passed"
        ]

        automatic = {
            boundary: (
                _self_test_semantic_manifest(
                    resource_id,
                    automatic_names=True,
                ),
                Path("."),
            )
            for resource_id, boundary in enumerate(
                SMOKE_BOUNDARIES,
                start=350,
            )
        }
        automatic_result = _validate_three_capture_resource_semantics(
            automatic,
            render_mode="taa-on",
        )
        assert automatic_result["passed"], automatic_result["errors"]
        csm_names = {
            row["descriptor"]["resource"]["name"]
            for row in automatic_result["roles"]["csm"].values()
        }
        assert csm_names == {"2D Depth Attachment"}

        explicit_auto_shaped = {
            boundary: (_self_test_semantic_manifest(resource_id), Path("."))
            for resource_id, boundary in enumerate(
                SMOKE_BOUNDARIES,
                start=350,
            )
        }
        for resource_id, boundary in enumerate(SMOKE_BOUNDARIES, start=350):
            resource = explicit_auto_shaped[boundary][0]["discovery"]["csm"][
                "resource"
            ]
            resource["name"] = f"2D Depth Attachment {resource_id}"
            resource["autogenerated_name"] = False
        assert not _validate_three_capture_resource_semantics(
            explicit_auto_shaped,
            render_mode="taa-on",
        )["passed"]

    @test("depth metrics expose active coverage and strict deltas")
    def test_depth_metrics() -> None:
        import numpy as np

        before = np.ones((16, 16), dtype=np.float32)
        after = before.copy()
        before[4:12, 4:12] = 0.5
        after[4:12, 4:12] = 0.5
        after[4, 4] = 0.6
        metrics = _array_diff_metrics(
            before,
            after,
            dx=0,
            dy=0,
            epsilon=1.0e-6,
            depth=True,
        )
        assert metrics["before_active_pixels"] == 64
        assert metrics["after_active_pixels"] == 64
        assert metrics["active_changed_fraction"] > 0.0
        assert metrics["max_abs"] > 0.09

    @test("release metrics hard-fail every non-finite coverage pattern")
    def test_nonfinite_metrics() -> None:
        import numpy as np

        finite_before = np.zeros((2, 2, 4), dtype=np.float32)
        mismatched_after = finite_before.copy()
        mismatched_after[0, 0, 3] = np.nan
        mismatch = _array_diff_metrics(
            finite_before,
            mismatched_after,
            dx=0,
            dy=0,
            epsilon=1.0e-6,
            depth=False,
        )
        assert mismatch["available"]
        assert mismatch["finite_coverage"]["finite_pixels_before"] == 4
        assert mismatch["finite_coverage"]["finite_pixels_after"] == 3
        assert mismatch["nonfinite_counts"]["values_after"] == 1
        assert mismatch["nonfinite_counts"][
            "finite_nonfinite_mismatch_values"
        ] == 1
        assert _release_nonfinite_errors(mismatch, label="mismatch")
        json.dumps(mismatch, allow_nan=False)

        both_before = np.zeros((2, 2), dtype=np.float32)
        both_after = both_before.copy()
        both_before[0, 0] = np.nan
        both_after[0, 0] = np.inf
        both_nonfinite = _array_diff_metrics(
            both_before,
            both_after,
            dx=0,
            dy=0,
            epsilon=1.0e-6,
            depth=False,
        )
        assert both_nonfinite["available"]
        assert both_nonfinite["nonfinite_counts"][
            "finite_nonfinite_mismatch_values"
        ] == 0
        assert both_nonfinite["nonfinite_counts"]["both_nonfinite_values"] == 1
        assert _release_nonfinite_errors(both_nonfinite, label="both")

        finite = _array_diff_metrics(
            finite_before,
            finite_before.copy(),
            dx=0,
            dy=0,
            epsilon=1.0e-6,
            depth=False,
        )
        assert not _release_nonfinite_errors(finite, label="finite")

    @test("release extraction requires real inner TAA draw and exact resource bindings")
    def test_release_extraction_contract() -> None:
        with tempfile.TemporaryDirectory(prefix="rdc_csm_selftest_") as directory:
            root = Path(directory)
            manifest = _self_test_release_extraction_fixture(
                root,
                boundary="last-moving",
                history_write_index=1,
            )
            assert not _validate_release_extraction_contract(
                manifest,
                root,
                label="test",
                render_mode="taa-on",
            ), _validate_release_extraction_contract(
                manifest, root, label="test", render_mode="taa-on"
            )

            manifest["exports"]["final"]["kind"] = "png"
            manifest["discovery"]["taa"]["selection"]["ambiguous"] = True
            errors = _validate_release_extraction_contract(
                manifest, root, label="test", render_mode="taa-on"
            )
            assert any("PNG fallback" in error for error in errors)
            assert any("taa resource selection is ambiguous" in error for error in errors)

        with tempfile.TemporaryDirectory(prefix="rdc_csm_selftest_") as directory:
            root = Path(directory)
            passthrough = _self_test_release_extraction_fixture(
                root,
                boundary="last-moving",
                history_write_index=1,
            )
            passthrough["discovery"]["taa"]["passthrough"] = True
            passthrough["release_resource_evidence"]["taa_resolve"][
                "passthrough"
            ] = True
            assert any(
                "passthrough" in error
                for error in _validate_release_extraction_contract(
                    passthrough,
                    root,
                    label="passthrough",
                    render_mode="taa-on",
                )
            )

        with tempfile.TemporaryDirectory(prefix="rdc_csm_selftest_") as directory:
            root = Path(directory)
            outer_only = _self_test_release_extraction_fixture(
                root,
                boundary="last-moving",
                history_write_index=1,
            )
            outer_only["markers"]["taa"]["name"] = "GPUDrivenTAAResolvePass"
            outer_only["release_resource_evidence"]["taa_resolve"][
                "inner_marker_name"
            ] = "GPUDrivenTAAResolvePass"
            assert any(
                "inner GPUDrivenTAAResolve" in error
                for error in _validate_release_extraction_contract(
                    outer_only,
                    root,
                    label="outer-only",
                    render_mode="taa-on",
                )
            )

        with tempfile.TemporaryDirectory(prefix="rdc_csm_selftest_") as directory:
            root = Path(directory)
            marker_only = _self_test_release_extraction_fixture(
                root,
                boundary="last-moving",
                history_write_index=1,
            )
            marker_only["release_resource_evidence"]["passes"]["taa"][
                "draw_count"
            ] = 0
            marker_only["release_resource_evidence"]["taa_resolve"]["action"] = None
            assert any(
                "draw" in error.lower()
                for error in _validate_release_extraction_contract(
                    marker_only,
                    root,
                    label="marker-only",
                    render_mode="taa-on",
                )
            )

    @test("TAA history resources normalize logically and obey 31/32/39 ping-pong")
    def test_taa_history_ping_pong() -> None:
        with tempfile.TemporaryDirectory(prefix="rdc_csm_selftest_") as directory:
            root = Path(directory)
            extracted: dict[str, tuple[dict[str, Any], Path]] = {}
            for boundary, write_index in zip(SMOKE_BOUNDARIES, (1, 0, 1)):
                capture_root = root / boundary
                manifest = _self_test_release_extraction_fixture(
                    capture_root,
                    boundary=boundary,
                    history_write_index=write_index,
                )
                extracted[boundary] = (manifest, capture_root)
            ping_pong = _validate_taa_history_ping_pong(extracted)
            assert ping_pong["passed"], ping_pong["errors"]
            semantics = _validate_three_capture_resource_semantics(
                extracted,
                render_mode="taa-on",
            )
            assert semantics["passed"], semantics["errors"]
            taa_names = {
                row["descriptor"]["resource"]["name"]
                for row in semantics["roles"]["taa"].values()
            }
            assert taa_names == {"GPUDrivenSceneColorHistory"}
            physical = [
                semantics["roles"]["taa"][boundary]["resource_name_evidence"][
                    "physical_index"
                ]
                for boundary in SMOKE_BOUNDARIES
            ]
            assert physical == [1, 0, 1]

            wrong_root = root / "wrong-settled"
            wrong = _self_test_release_extraction_fixture(
                wrong_root,
                boundary="settled",
                history_write_index=0,
            )
            extracted["settled"] = (wrong, wrong_root)
            assert not _validate_taa_history_ping_pong(extracted)["passed"]

    @test("executable evidence and exact two-case six-capture contract fail closed")
    def test_executable_and_full_case_contract() -> None:
        with tempfile.TemporaryDirectory(prefix="rdc_csm_selftest_") as directory:
            root = Path(directory)
            source, _, _ = _self_test_case_fixture(root)
            executable = _validate_release_executable_evidence(source)
            assert executable["passed"], executable["errors"]
            uniqueness = _validate_release_global_capture_uniqueness(
                source,
                root / "manifest.json",
            )
            assert uniqueness["passed"], uniqueness["errors"]
            assert uniqueness["unique_path_count"] == 6
            assert uniqueness["unique_sha256_count"] == 6

            missing_case = json.loads(json.dumps(source))
            missing_case["cases"] = missing_case["cases"][:1]
            assert any(
                "exactly two target cases" in error
                for error in _validate_release_manifest_contract(
                    missing_case,
                    root / "manifest.json",
                )
            )

            target_mismatch = json.loads(json.dumps(source))
            target_mismatch["cases"][0]["executable_evidence"][
                "target_process_binding"
            ]["observed_image_path"] = str((root / "replacement.exe").resolve())
            target_mismatch["executable_evidence"]["case_checks"][0][
                "target_process_binding"
            ]["observed_image_path"] = str((root / "replacement.exe").resolve())
            assert not _validate_release_executable_evidence(target_mismatch)["passed"]

            duplicate = json.loads(json.dumps(source))
            duplicate_entry = duplicate["cases"][0]["boundaries"]["last-moving"]
            duplicate["cases"][1]["boundaries"]["settled"]["capture_path"] = (
                duplicate_entry["capture_path"]
            )
            duplicate["cases"][1]["boundaries"]["settled"]["capture_sha256"] = (
                duplicate_entry["capture_sha256"]
            )
            duplicate_uniqueness = _validate_release_global_capture_uniqueness(
                duplicate,
                root / "manifest.json",
            )
            assert not duplicate_uniqueness["passed"]

    @test("independent shadow-edge API hard-gates Light HistoryWrite and final")
    def test_shadow_edge_metrics_integration() -> None:
        import copy

        with tempfile.TemporaryDirectory(prefix="rdc_csm_selftest_") as directory:
            root = Path(directory)
            before_root = root / "before"
            after_root = root / "after"
            before = _self_test_release_extraction_fixture(
                before_root,
                boundary="last-moving",
                history_write_index=1,
            )
            after = _self_test_release_extraction_fixture(
                after_root,
                boundary="first-still",
                history_write_index=0,
            )
            stable = _evaluate_release_shadow_edge_metrics(
                before,
                after,
                before_root,
                after_root,
                render_mode="taa-on",
            )
            assert stable["passed"], stable
            assert stable["metrics"]["schema"] == "mgif-shadow-edge-roi-metrics-v1"
            assert set(stable["stage_statuses"]) == {
                "scene_color_hdr",
                "history_write",
                "final",
            }

            missing_camera = copy.deepcopy(after)
            missing_camera["release_inputs"]["camera"]["view_from_world"] = None
            missing_camera["release_inputs"]["camera"]["world_from_view"] = None
            camera_contract_errors = _validate_release_extraction_contract(
                missing_camera,
                after_root,
                label="missing-view-camera",
                render_mode="taa-on",
            )
            assert any(
                "camera.view_from_world" in error
                for error in camera_contract_errors
            ), camera_contract_errors
            assert any(
                "camera.world_from_view" in error
                for error in camera_contract_errors
            ), camera_contract_errors
            missing_camera_metrics = _evaluate_release_shadow_edge_metrics(
                before,
                missing_camera,
                before_root,
                after_root,
                render_mode="taa-on",
            )
            assert missing_camera_metrics["passed"] is False
            assert missing_camera_metrics["status"] == "error"

            shifted_root = root / "shifted"
            shifted = _self_test_release_extraction_fixture(
                shifted_root,
                boundary="first-still",
                history_write_index=0,
                edge_shift_px=1.0,
            )
            moving_edge = _evaluate_release_shadow_edge_metrics(
                before,
                shifted,
                before_root,
                shifted_root,
                render_mode="taa-on",
            )
            assert moving_edge["passed"] is False, moving_edge
            assert moving_edge["status"] == "fail", moving_edge
            assert all(
                moving_edge["stage_statuses"].get(stage) == "fail"
                for stage in ("scene_color_hdr", "history_write", "final")
            ), moving_edge

            no_roi_root = root / "no-roi"
            no_roi = _self_test_release_extraction_fixture(
                no_roi_root,
                boundary="first-still",
                history_write_index=0,
                receiver_normals=False,
            )
            inconclusive = _evaluate_release_shadow_edge_metrics(
                before,
                no_roi,
                before_root,
                no_roi_root,
                render_mode="taa-on",
            )
            assert inconclusive["passed"] is False
            assert inconclusive["status"] in ("inconclusive", "error")
    @test("GetTextureData contract rejects multisample 3D nonzero-mip and non-tight bytes")
    def test_tight_texture_readback_contract() -> None:
        class FakeFormat:
            type = "Regular"
            compType = "UNorm"
            compCount = 4
            compByteWidth = 1

            @staticmethod
            def Name() -> str:
                return "R8G8B8A8_UNORM"

            @staticmethod
            def BGRAOrder() -> bool:
                return False

        class FakeTexture:
            width = 2
            height = 1
            depth = 1
            msSamp = 1
            arraysize = 1
            format = FakeFormat()

        subresource = argparse.Namespace(mip=0, slice=0, sample=0)
        contract = _d_texture_readback_contract(
            FakeTexture(),
            subresource,
            depth=False,
        )
        assert contract["expected_raw_bytes"] == 8
        _d_validate_raw_texture_bytes(bytes(range(8)), contract)
        decoded = _d_decode_regular_texture(
            FakeTexture(),
            bytes(range(8)),
            depth=False,
            contract=contract,
        )
        assert decoded.shape == (1, 2, 4)
        assert str(decoded.dtype) == "float32"
        for payload in (bytes(7), bytes(9)):
            try:
                _d_validate_raw_texture_bytes(payload, dict(contract))
            except RuntimeError:
                pass
            else:
                raise AssertionError("non-tight readback payload unexpectedly passed")
        for field, value in (("msSamp", 2), ("depth", 2)):
            texture = FakeTexture()
            setattr(texture, field, value)
            try:
                _d_texture_readback_contract(texture, subresource, depth=False)
            except RuntimeError:
                pass
            else:
                raise AssertionError(f"invalid texture {field} unexpectedly passed")
        try:
            _d_texture_readback_contract(
                FakeTexture(),
                argparse.Namespace(mip=1, slice=0, sample=0),
                depth=False,
            )
        except RuntimeError:
            pass
        else:
            raise AssertionError("nonzero mip unexpectedly passed")

    @test("NPY release evidence validates shape dtype channels formats extents and HistoryRead semantics")
    def test_npy_release_export_contract() -> None:
        import copy

        with tempfile.TemporaryDirectory(prefix="rdc_csm_npy_selftest_") as directory:
            root = Path(directory)
            manifest = _self_test_release_extraction_fixture(
                root,
                boundary="last-moving",
                history_write_index=1,
            )
            assert not _validate_release_extraction_contract(
                manifest,
                root,
                label="npy-valid",
                render_mode="taa-on",
            )
            wrong_shape = copy.deepcopy(manifest)
            wrong_shape["release_inputs"]["resources"]["final"]["export"][
                "shape"
            ] = [127, 192, 4]
            assert any(
                "actual NPY shape" in error or "screen dimensions" in error
                for error in _validate_release_extraction_contract(
                    wrong_shape,
                    root,
                    label="npy-shape",
                    render_mode="taa-on",
                )
            )
            wrong_channel = copy.deepcopy(manifest)
            wrong_channel["release_inputs"]["resources"]["scene_color_hdr"][
                "export"
            ]["channel_order"] = ["B", "G", "R", "A"]
            assert any(
                "channel order" in error
                for error in _validate_release_extraction_contract(
                    wrong_channel,
                    root,
                    label="npy-channel",
                    render_mode="taa-on",
                )
            )
            wrong_format = copy.deepcopy(manifest)
            wrong_format["release_inputs"]["resources"]["final"]["export"][
                "source_format"
            ]["name"] = "R16G16B16A16_FLOAT"
            assert any(
                "source_format.name" in error or "readback/export source formats" in error
                for error in _validate_release_extraction_contract(
                    wrong_format,
                    root,
                    label="npy-format",
                    render_mode="taa-on",
                )
            )
            history_semantics = copy.deepcopy(manifest)
            history_semantics["release_inputs"]["resources"]["history_read"][
                "evidence_only"
            ] = False
            history_semantics["release_inputs"]["resources"]["history_read"][
                "export"
            ]["evidence_only"] = False
            assert any(
                "evidence_only" in error or "evidence-only" in error
                for error in _validate_release_extraction_contract(
                    history_semantics,
                    root,
                    label="history-semantics",
                    render_mode="taa-on",
                )
            )

    @test("Light and final release draws require exact shader Draw(3,1) and output identity")
    def test_light_final_draw_contract() -> None:
        import copy

        with tempfile.TemporaryDirectory(prefix="rdc_csm_draw_selftest_") as directory:
            root = Path(directory)
            manifest = _self_test_release_extraction_fixture(
                root,
                boundary="last-moving",
                history_write_index=1,
            )
            wrong_light = copy.deepcopy(manifest)
            wrong_light["release_resource_evidence"]["draw_evidence"]["light"][
                "fragment_shader"
            ]["entry_point"] = "fragmentWrongMain"
            assert any(
                "light fragment entry" in error
                for error in _validate_release_extraction_contract(
                    wrong_light,
                    root,
                    label="light-entry",
                    render_mode="taa-on",
                )
            )
            wrong_final = copy.deepcopy(manifest)
            wrong_final_action = wrong_final["release_resource_evidence"][
                "draw_evidence"
            ]["final"]["action"]
            wrong_final_action["instance_count_raw"] = 0
            raw_instance_errors = _validate_release_extraction_contract(
                wrong_final,
                root,
                label="final-raw-instance",
                render_mode="taa-on",
            )
            assert wrong_final_action["instance_count_raw"] == 0
            assert wrong_final_action["instance_count"] == 1
            assert raw_instance_errors
            wrong_output = copy.deepcopy(manifest)
            wrong_output["release_resource_evidence"]["draw_evidence"]["light"][
                "color_output"
            ] = dict(
                wrong_output["release_resource_evidence"]["draw_evidence"]["light"][
                    "color_output"
                ]
            )
            wrong_output["release_resource_evidence"]["draw_evidence"]["light"][
                "color_output"
            ]["resource_id"] = 999999
            assert any(
                "light color output differs" in error
                for error in _validate_release_extraction_contract(
                    wrong_output,
                    root,
                    label="light-output",
                    render_mode="taa-on",
                )
            )

    @test("manifest toolchain binds current harness comparator and ROI hashes")
    def test_toolchain_evidence_contract() -> None:
        import copy

        with tempfile.TemporaryDirectory(prefix="rdc_csm_toolchain_selftest_") as directory:
            source, _, _ = _self_test_case_fixture(Path(directory))
            runtime = _toolchain_snapshot(_runtime_tool_paths())
            valid = _validate_manifest_toolchain_evidence(
                source,
                runtime_before=runtime,
            )
            assert valid["passed"], valid["errors"]
            rdc_package = runtime["tools"]["rdc_cli_package"]
            zero_byte_records = [
                record
                for record in rdc_package["files"]
                if int(record["size_bytes"]) == 0
            ]
            zero_byte_paths = {record["relative_path"] for record in zero_byte_records}
            assert rdc_package["zero_byte_file_count"] == len(zero_byte_records)
            assert {
                "rdc/_skills/__init__.py",
                "rdc/vfs/__init__.py",
                "rdc_cli-0.6.1.dist-info/REQUESTED",
            }.issubset(zero_byte_paths)
            assert all(
                record["sha256"] == hashlib.sha256(b"").hexdigest()
                for record in zero_byte_records
            )
            replaced = copy.deepcopy(source)
            replaced["toolchain_evidence"]["before_all_cases"]["tools"][
                "harness"
            ]["sha256"] = "0" * 64
            replaced["toolchain_evidence"]["after_all_cases"]["tools"][
                "harness"
            ]["sha256"] = "0" * 64
            assert not _validate_manifest_toolchain_evidence(
                replaced,
                runtime_before=runtime,
            )["passed"]
            missing = copy.deepcopy(source)
            del missing["toolchain_evidence"]["before_all_cases"]["tools"]["roi"]
            assert not _validate_manifest_toolchain_evidence(
                missing,
                runtime_before=runtime,
            )["passed"]

    @test("direct replay shutdown uses state token and held handle without PID tree fallback")
    def test_direct_replay_shutdown_contract() -> None:
        class FakeIdentity:
            def __init__(self, *, reuse_before_terminate: bool = False) -> None:
                self.pid = 77
                self.identity = "77@test:direct-token"
                self.running = True
                self.closed = False
                self.terminate_calls = 0
                self.reuse_before_terminate = reuse_before_terminate
                self.replacement_alive = reuse_before_terminate

            def metadata(self) -> dict[str, Any]:
                return {
                    "pid": self.pid,
                    "identity": self.identity,
                    "creation_time_key": "test:direct-token",
                    "creation_time_unix_seconds": 1.0,
                    "image_path": "python.exe",
                    "backend": "self-test-held-handle",
                    "native_handle_held": not self.closed,
                    "terminate_access": True,
                }

            def is_running(self) -> bool:
                if self.closed:
                    raise ComparatorError("fake direct-shutdown handle is closed")
                return self.running

            def terminate(self, *, timeout: float) -> dict[str, Any]:
                assert timeout > 0.0
                self.terminate_calls += 1
                self.running = False
                if self.reuse_before_terminate:
                    return {
                        "passed": True,
                        "same_native_handle": True,
                        "tree_cleanup_requested": False,
                        "termination_requested": False,
                        "original_exited_before_terminate": True,
                        "replacement_untouched": self.replacement_alive,
                        "running_after": False,
                    }
                return {
                    "passed": True,
                    "same_native_handle": True,
                    "tree_cleanup_requested": False,
                    "termination_requested": True,
                    "running_after": False,
                }

        with tempfile.TemporaryDirectory(prefix="rdc_csm_direct_shutdown_") as directory:
            root = Path(directory)
            capture = root / "capture.rdc"
            capture.write_bytes(b"rdc")
            state_path = root / "session.json"
            secret = "comparator-direct-token-secret"

            def write_state() -> dict[str, Any]:
                state_path.write_text(
                    json.dumps(
                        {
                            "capture": str(capture.resolve()),
                            "current_eid": 0,
                            "opened_at": "2026-07-29T00:00:00+00:00",
                            "host": "127.0.0.1",
                            "port": 12345,
                            "token": secret,
                            "pid": 77,
                        }
                    ),
                    encoding="utf-8",
                )
                record = _host_session_state_record(state_path)
                assert record["valid"], record
                return record

            def ownership(identity: FakeIdentity) -> dict[str, Any]:
                return {
                    "established": True,
                    "errors": [],
                    "stable_process_identity": identity.metadata(),
                }

            requested_tokens: list[str] = []

            def request_builder(token: str, request_id: int) -> dict[str, Any]:
                requested_tokens.append(token)
                return {"method": "shutdown", "id": request_id, "params": {"_token": token}}

            graceful_identity = FakeIdentity()
            graceful_state = write_state()

            def graceful_sender(*_: Any, **__: Any) -> dict[str, Any]:
                graceful_identity.running = False
                return {"result": {"ok": True}}

            graceful = _shutdown_owned_replay_session_direct(
                state_path=state_path,
                state_after_open=graceful_state,
                process_identity=graceful_identity,
                ownership=ownership(graceful_identity),
                capture=capture,
                timeout=0.2,
                send_request_fn=graceful_sender,
                shutdown_request_fn=request_builder,
            )
            assert graceful["passed"], graceful
            assert graceful_identity.terminate_calls == 0
            assert graceful["subprocess_used"] is False
            assert graceful["pid_only_fallback"] is False
            assert graceful["port_scan_fallback"] is False
            assert graceful["tree_cleanup_requested"] is False
            assert not state_path.exists()
            assert secret not in json.dumps(graceful)

            def timeout_sender(*_: Any, **__: Any) -> dict[str, Any]:
                raise TimeoutError("injected direct shutdown timeout")

            recovered_identity = FakeIdentity()
            recovered_state = write_state()
            recovered = _shutdown_owned_replay_session_direct(
                state_path=state_path,
                state_after_open=recovered_state,
                process_identity=recovered_identity,
                ownership=ownership(recovered_identity),
                capture=capture,
                timeout=0.1,
                send_request_fn=timeout_sender,
                shutdown_request_fn=request_builder,
            )
            assert recovered["passed"], recovered
            assert recovered_identity.terminate_calls == 1
            assert recovered["same_handle_recovery"]["same_native_handle"] is True
            assert recovered["same_handle_recovery"]["pid_only_fallback"] is False
            assert recovered["same_handle_recovery"]["tree_cleanup_requested"] is False
            assert not state_path.exists()

            reused_identity = FakeIdentity(reuse_before_terminate=True)
            reused_state = write_state()
            reused = _shutdown_owned_replay_session_direct(
                state_path=state_path,
                state_after_open=reused_state,
                process_identity=reused_identity,
                ownership=ownership(reused_identity),
                capture=capture,
                timeout=0.1,
                send_request_fn=timeout_sender,
                shutdown_request_fn=request_builder,
            )
            termination = reused["same_handle_recovery"]["termination"]
            assert reused["passed"], reused
            assert termination["termination_requested"] is False
            assert termination["original_exited_before_terminate"] is True
            assert termination["replacement_untouched"] is True
            assert reused_identity.replacement_alive is True
            assert requested_tokens == [secret, secret, secret]
    @test("disk preflight estimates extraction plus safety margin and fails closed")
    def test_disk_preflight_contract() -> None:
        usage = lambda _: argparse.Namespace(total=0, used=0, free=1 << 50)
        passed = _disk_space_preflight(
            Path.cwd(),
            capture_count=6,
            disk_usage_fn=usage,
        )
        assert passed["passed"]
        estimate = passed["estimate"]
        assert estimate["required_free_bytes"] == (
            estimate["estimated_bytes"] + estimate["safety_margin_bytes"]
        )
        failed = _disk_space_preflight(
            Path.cwd(),
            capture_count=6,
            disk_usage_fn=lambda _: argparse.Namespace(total=0, used=0, free=1),
        )
        assert failed["passed"] is False
        try:
            _require_disk_space(failed)
        except ComparatorError:
            pass
        else:
            raise AssertionError("insufficient disk preflight unexpectedly passed")

    @test("total deadline never turns active or residual sessions into success")
    def test_deadline_cleanup_contract() -> None:
        clean = _session_cleanup_terminal_result(
            direct_shutdown_passed=True,
            owned_daemon_absent=True,
            state_absent=True,
            handle_closed=True,
            errors=[],
            deadline_exceeded=False,
        )
        assert clean["closed"] and clean["passed"]
        expired = _session_cleanup_terminal_result(
            direct_shutdown_passed=True,
            owned_daemon_absent=True,
            state_absent=True,
            handle_closed=True,
            errors=[],
            deadline_exceeded=True,
        )
        assert expired["closed"] and not expired["passed"]
        residual = _session_cleanup_terminal_result(
            direct_shutdown_passed=False,
            owned_daemon_absent=False,
            state_absent=False,
            handle_closed=False,
            errors=[],
            deadline_exceeded=True,
        )
        assert not residual["closed"] and not residual["passed"]
        try:
            _deadline_timeout(
                time.monotonic() - 1.0,
                10.0,
                stage="self-test",
                cleanup_reserve=1.0,
            )
        except ComparatorError:
            pass
        else:
            raise AssertionError("expired total deadline unexpectedly returned a timeout")
    @test("release CLI forbids subsets and locks every threshold including depth epsilon")
    def test_release_cli_and_threshold_lock() -> None:
        import copy

        canonical = _parse_args(["--self-test"])
        evidence = _validate_release_cli_contract(canonical)
        assert evidence["passed"], evidence["errors"]
        depth_bypass = copy.copy(canonical)
        depth_bypass.depth_epsilon = 0.2
        rejected = _validate_release_cli_contract(depth_bypass)
        assert rejected["passed"] is False
        assert rejected["thresholds"]["cli_thresholds"]["depth_epsilon"] == 0.2
        assert any("depth-epsilon" in error for error in rejected["errors"])
        subset = copy.copy(canonical)
        subset.case = ["csm-translate-stop__taa-on__no-ddgi"]
        subset_rejected = _validate_release_cli_contract(subset)
        assert subset_rejected["passed"] is False
        assert any("--case is forbidden" in error for error in subset_rejected["errors"])
        assert evidence["thresholds"]["canonical_roi_thresholds"] == RELEASE_ROI_CONFIG

    @test("TAA release requires readable historyValid one at every boundary")
    def test_taa_history_valid_contract() -> None:
        import copy

        with tempfile.TemporaryDirectory(prefix="rdc_csm_history_valid_") as directory:
            root = Path(directory)
            manifest = _self_test_release_extraction_fixture(
                root,
                boundary="last-moving",
                history_write_index=1,
            )
            zero = copy.deepcopy(manifest)
            zero["release_resource_evidence"]["taa_resolve"]["history_valid"][
                "value"
            ] = 0.0
            zero_errors = _validate_release_extraction_contract(
                zero,
                root,
                label="last-moving-history-zero",
                render_mode="taa-on",
            )
            assert any("historyValid" in error for error in zero_errors)
            unreadable = copy.deepcopy(manifest)
            unreadable["release_resource_evidence"]["taa_resolve"][
                "history_valid"
            ] = {"readable": False, "value": None, "errors": ["unreadable"]}
            unreadable_errors = _validate_release_extraction_contract(
                unreadable,
                root,
                label="last-moving-history-unreadable",
                render_mode="taa-on",
            )
            assert any("historyValid" in error for error in unreadable_errors)

    @test("formal no-post release has two cases six RDCs and proves TAA absence")
    def test_no_post_release_contract() -> None:
        import copy

        with tempfile.TemporaryDirectory(prefix="rdc_csm_no_post_manifest_") as directory:
            root = Path(directory)
            source, _, _ = _self_test_case_fixture(root, render_mode="no-post")
            assert _release_manifest_profile(source)["render_mode"] == "no-post"
            assert not _validate_release_manifest_contract(source, root / "manifest.json")
        with tempfile.TemporaryDirectory(prefix="rdc_csm_no_post_extract_") as directory:
            root = Path(directory)
            extracted: dict[str, tuple[dict[str, Any], Path]] = {}
            for boundary in SMOKE_BOUNDARIES:
                boundary_root = root / boundary
                manifest = _self_test_no_post_extraction_fixture(
                    boundary_root,
                    boundary=boundary,
                )
                assert not _validate_release_extraction_contract(
                    manifest,
                    boundary_root,
                    label=boundary,
                    render_mode="no-post",
                )
                extracted[boundary] = (manifest, boundary_root)
            semantics = _validate_three_capture_resource_semantics(
                extracted,
                render_mode="no-post",
            )
            assert semantics["passed"], semantics["errors"]
            pair_options = _parse_args(["--self-test"])
            no_post_pair = _compare_extracted_pair(
                pair_options,
                before_capture=root / "last-moving.rdc",
                after_capture=root / "first-still.rdc",
                before_manifest=extracted["last-moving"][0],
                after_manifest=extracted["first-still"][0],
                before_dir=extracted["last-moving"][1],
                after_dir=extracted["first-still"][1],
                work_root=root,
                same_current_pose=True,
                pair_name="no-post-last-moving__first-still",
                release_gate=True,
                release_render_mode="no-post",
            )
            assert no_post_pair["passed"], no_post_pair
            assert no_post_pair["comparisons"]["taa"] is None
            assert no_post_pair["release_evidence"]["shadow_edge_metrics"][
                "required_stages"
            ] == {"Light": "scene_color_hdr", "final": "final"}
            bad_marker = copy.deepcopy(extracted["last-moving"][0])
            bad_marker["release_resource_evidence"]["taa_absence"][
                "inner_markers"
            ] = [{"event_id": 30}]
            bad_marker["release_resource_evidence"]["taa_absence"]["passed"] = False
            bad_marker_errors = _validate_release_extraction_contract(
                bad_marker,
                extracted["last-moving"][1],
                label="no-post-inner-marker",
                render_mode="no-post",
            )
            assert any("TAA absence" in error or "TAA marker" in error for error in bad_marker_errors)
            bad_draw = copy.deepcopy(extracted["last-moving"][0])
            bad_draw["release_resource_evidence"]["taa_absence"][
                "resolve_draws"
            ] = [{"event_id": 31, "fragment_entry": "fragmentTAAResolveMain"}]
            bad_draw["release_resource_evidence"]["taa_absence"]["passed"] = False
            bad_draw_errors = _validate_release_extraction_contract(
                bad_draw,
                extracted["last-moving"][1],
                label="no-post-resolve-draw",
                render_mode="no-post",
            )
            assert any("TAA absence" in error or "resolve draw" in error for error in bad_draw_errors)

    @test("release requires four CSM layers exports and projection evidences")
    def test_exact_four_cascade_contract() -> None:
        import copy

        with tempfile.TemporaryDirectory(prefix="rdc_csm_four_layers_") as directory:
            root = Path(directory)
            manifest = _self_test_release_extraction_fixture(
                root,
                boundary="settled",
                history_write_index=1,
            )
            single = copy.deepcopy(manifest)
            single["discovery"]["csm"]["resource"]["array_size"] = 1
            single["exports"]["csm"] = single["exports"]["csm"][:1]
            single["projection_sources"] = single["projection_sources"][:1]
            errors = _validate_release_extraction_contract(
                single,
                root,
                label="single-cascade",
                render_mode="taa-on",
            )
            assert errors
            assert any("exactly four" in error or "four CSM" in error for error in errors)

    @test("harness-shaped final capture-set evidence is exact and cannot be subset")
    def test_harness_capture_set_contract() -> None:
        import copy

        with tempfile.TemporaryDirectory(prefix="rdc_csm_capture_set_") as directory:
            root = Path(directory)
            source, _, _ = _self_test_case_fixture(root)
            valid = _validate_release_capture_set_evidence(
                source,
                root / "manifest.json",
                render_mode="taa-on",
            )
            assert valid["passed"], valid["errors"]
            drift = copy.deepcopy(source)
            drift["capture_set_validation"]["rehash_after_all_cases"] = False
            assert not _validate_release_capture_set_evidence(
                drift,
                root / "manifest.json",
                render_mode="taa-on",
            )["passed"]
            subset = copy.deepcopy(source)
            subset["cases"] = subset["cases"][:1]
            assert _release_manifest_profile(subset)["passed"] is False
            assert _validate_release_manifest_contract(
                subset,
                root / "manifest.json",
            )

    @test("held state-file GUID volume policy is pure fail-closed and exact")
    def test_state_file_handle_policy() -> None:
        publication_ticks = WINDOWS_FILETIME_EPOCH_OFFSET_TICKS + 50_000_000
        final_path = (
            r"\\?\Volume{1082ed61-3991-4ccf-8007-161d89cd277c}"
            r"\rdc\sessions\formal.json"
        )
        volume_root = "\\\\?\\Volume{1082ed61-3991-4ccf-8007-161d89cd277c}\\"
        source = (
            "msvcrt.get_osfhandle + GetFileTime + "
            "GetFinalPathNameByHandleW(VOLUME_NAME_GUID|FILE_NAME_NORMALIZED) + "
            "GetVolumeInformationByHandleW + GetDriveTypeW"
        )

        def snapshot(
            *,
            file_system: str = "NTFS",
            drive_type_code: int = WINDOWS_DRIVE_FIXED,
            ticks: int = publication_ticks,
            native_handle_value: int = 123,
            handle_path: str = final_path,
            include_volume_root: bool = True,
            include_flags: bool = True,
            valid: bool = True,
        ) -> dict[str, Any]:
            value: dict[str, Any] = {
                "schema": STATE_HANDLE_SNAPSHOT_SCHEMA,
                "valid": valid,
                "source": source,
                "path_fallback_used": False,
                "original_path_used": False,
                "native_handle_value": native_handle_value,
                "filetime_ticks": ticks,
                "final_path_guid": handle_path,
                "volume_guid_root": volume_root,
                "volume_serial_number": 42,
                "maximum_component_length": 255,
                "file_system_flags": 0,
                "file_system": file_system,
                "drive_type_code": drive_type_code,
                "drive_type": {
                    0: "unknown",
                    2: "removable",
                    3: "fixed",
                    4: "remote",
                }.get(drive_type_code, "invalid"),
            }
            if include_flags:
                value["final_path_flags"] = {
                    "value": WINDOWS_FINAL_PATH_FLAGS,
                    "volume_name": "VOLUME_NAME_GUID",
                    "file_name": "FILE_NAME_NORMALIZED",
                }
            if not include_volume_root:
                value.pop("volume_guid_root")
            if not valid:
                value["error"] = "injected native API failure"
            return value

        for file_system in ("NTFS", "ReFS"):
            result = _host_evaluate_state_file_handle_policy(
                snapshot(file_system=file_system),
                snapshot(file_system=file_system),
            )
            assert result["verified"] is True, result
            assert result["same_handle_path_identity"] is True
            assert result["raw_filetime_stable"] is True
            assert result["path_fallback_used"] is False

        negative_pairs = (
            ("remote", snapshot(drive_type_code=4), snapshot(drive_type_code=4)),
            ("removable", snapshot(drive_type_code=2), snapshot(drive_type_code=2)),
            ("fat", snapshot(file_system="FAT32"), snapshot(file_system="FAT32")),
            ("unknown", snapshot(file_system="UNKNOWN"), snapshot(file_system="UNKNOWN")),
            ("api-failure", snapshot(valid=False), snapshot(valid=False)),
            ("filetime-changed", snapshot(), snapshot(ticks=publication_ticks + 1)),
            ("handle-changed", snapshot(), snapshot(native_handle_value=124)),
            ("path-changed", snapshot(), snapshot(handle_path=final_path + ".replacement")),
            (
                "missing-volume-identity",
                snapshot(include_volume_root=False),
                snapshot(include_volume_root=False),
            ),
            (
                "missing-final-path-flags",
                snapshot(include_flags=False),
                snapshot(include_flags=False),
            ),
        )
        for label, before, after in negative_pairs:
            result = _host_evaluate_state_file_handle_policy(before, after)
            assert result["verified"] is False, (label, result)
            assert result["errors"], (label, result)
    @test("same-PID replacement after state publication never gains cleanup authority")
    def test_replay_daemon_pid_reuse_after_state_publication() -> None:
        class FakeIdentity:
            def __init__(self, creation_ticks: int) -> None:
                self.creation_ticks = creation_ticks
                self.creation_unix_ns = (
                    creation_ticks - WINDOWS_FILETIME_EPOCH_OFFSET_TICKS
                ) * WINDOWS_FILETIME_TICK_NS
                self.creation_time = self.creation_unix_ns / 1_000_000_000.0
                self.identity = f"77@winfiletime:{creation_ticks}"
                self.closed = False
                self.terminate_calls = 0

            def metadata(self) -> dict[str, Any]:
                return {
                    "pid": 77,
                    "identity": self.identity,
                    "native_handle_held": True,
                    "terminate_access": True,
                    "creation_time_key": f"winfiletime:{self.creation_ticks}",
                    "creation_time_unix_seconds": self.creation_time,
                    "creation_time_unix_ns": self.creation_unix_ns,
                    "creation_filetime_ticks": self.creation_ticks,
                    "image_path": image_path,
                }

            def close(self) -> dict[str, Any]:
                self.closed = True
                return {"closed": True}

            def terminate(self, *, timeout: float) -> dict[str, Any]:
                self.terminate_calls += 1
                return {"passed": True, "same_native_handle": True}

        with tempfile.TemporaryDirectory(prefix="rdc_csm_pid_reuse_") as directory:
            root = Path(directory)
            capture = root / "capture.rdc"
            capture.write_bytes(b"rdc")
            state_path = root / "session.json"
            image_path = str((root / "rdc.exe").resolve())
            state_path.write_text(
                json.dumps(
                    {
                        "capture": str(capture.resolve()),
                        "current_eid": 0,
                        "opened_at": "2026-07-29T00:00:00+00:00",
                        "host": "127.0.0.1",
                        "port": 12345,
                        "token": "self-test-token",
                        "pid": 77,
                    }
                ),
                encoding="utf-8",
            )
            state_record = _host_session_state_record(state_path)
            assert state_record["valid"] is True
            publication = state_record["publication_boundary"]
            publication_ticks = int(publication["filetime_ticks"])
            publication_ns = int(publication["modified_ns"])
            open_started_wall_ns = publication_ns - 1_000_000_000

            def metadata(identity: FakeIdentity) -> dict[str, Any]:
                return {
                    "pid": 77,
                    "identity": identity.identity,
                    "creation_time_key": f"winfiletime:{identity.creation_ticks}",
                    "creation_filetime_ticks": identity.creation_ticks,
                    "creation_time_unix_ns": identity.creation_unix_ns,
                    "create_time": identity.creation_time,
                    "image_path": image_path,
                    "process_name": "rdc.exe",
                    "command": [
                        "python",
                        "-m",
                        "rdc.daemon_server",
                        "--capture",
                        str(capture.resolve()),
                    ],
                    "is_rdc_daemon": True,
                    "capture": str(capture.resolve()),
                    "native_handle_verified_during_metadata_collection": True,
                }

            boundary_before = FakeIdentity(publication_ticks - 1)
            acquired, ownership = _acquire_owned_replay_daemon(
                state_record=state_record,
                expected_state_path=state_path,
                capture=capture,
                open_started_wall_ns=open_started_wall_ns,
                process_identity_factory=lambda pid, require_terminate: boundary_before,
                process_metadata_collector=metadata,
            )
            assert acquired is boundary_before
            assert ownership["established"] is True
            assert ownership["process_created_before_state_file"] is True
            assert ownership["state_publication_boundary_exact"] is True
            acquired.close()

            for label, delta, expected_equal in (
                ("creation-equals-publication", 0, True),
                ("creation-one-tick-after-publication", 1, False),
                (
                    "creation-one-second-after-publication",
                    1_000_000_000 // WINDOWS_FILETIME_TICK_NS,
                    False,
                ),
            ):
                replacement = FakeIdentity(publication_ticks + delta)
                binding = _validate_owned_replay_daemon_binding(
                    state_record=state_record,
                    expected_state_path=state_path,
                    capture=capture,
                    stable_process_identity=replacement.metadata(),
                    process_metadata=metadata(replacement),
                    open_started_wall_ns=open_started_wall_ns,
                )
                assert binding["established"] is False, (label, binding)
                assert binding["strict_creation_precedes_publication"] is False
                assert binding["creation_equals_publication"] is expected_equal
                assert binding["state_file_volume_verified"] is True
                assert binding["errors"]
                try:
                    _acquire_owned_replay_daemon(
                        state_record=state_record,
                        expected_state_path=state_path,
                        capture=capture,
                        open_started_wall_ns=open_started_wall_ns,
                        process_identity_factory=lambda pid, require_terminate, item=replacement: item,
                        process_metadata_collector=metadata,
                    )
                except ComparatorError as exc:
                    if expected_equal:
                        assert "equality is ambiguous" in str(exc)
                    else:
                        assert "after the exact named-session publication boundary" in str(exc)
                else:
                    raise AssertionError(
                        f"{label} unexpectedly established replay daemon ownership"
                    )
                assert replacement.closed is True
                assert replacement.terminate_calls == 0
    @test("runtime count RDC and readback budgets fail before writes")
    def test_runtime_hard_budgets() -> None:
        try:
            _require_single_rdc_size_budget(
                COMPARATOR_MAX_RDC_BYTES + 1,
                label="oversized.rdc",
            )
        except ComparatorError:
            pass
        else:
            raise AssertionError("oversized RDC unexpectedly passed")
        with tempfile.TemporaryDirectory(prefix="rdc_csm_runtime_budget_") as directory:
            root = Path(directory)
            capture = root / "capture.rdc"
            capture.write_bytes(b"rdc")
            preflight = _disk_space_preflight(
                root,
                capture_count=1,
                disk_usage_fn=lambda _: argparse.Namespace(total=0, used=0, free=1 << 50),
            )
            budget = ComparatorRuntimeBudget(
                root,
                capture_count=1,
                disk_preflight=preflight,
                disk_usage_fn=lambda _: argparse.Namespace(total=0, used=0, free=1 << 50),
            )
            budget.before_capture(capture)
            try:
                budget.before_capture(capture)
            except ComparatorError:
                pass
            else:
                raise AssertionError("capture-count budget unexpectedly allowed a second open")
        global _D_EXPORT_BUDGET_STATE
        prior_budget = _D_EXPORT_BUDGET_STATE
        try:
            _D_EXPORT_BUDGET_STATE = {
                "single_readback_cap_bytes": 8,
                "export_budget_bytes": 64,
                "readback_expected_bytes": 0,
                "readback_actual_bytes": 0,
                "npy_committed_bytes": 0,
                "readbacks": [],
                "writes": [],
            }
            try:
                _d_reserve_readback(
                    {"expected_raw_bytes": 9},
                    role="oversized-readback",
                )
            except RuntimeError:
                pass
            else:
                raise AssertionError("oversized readback unexpectedly reserved")
        finally:
            _D_EXPORT_BUDGET_STATE = prior_budget
    @test("final smoke manifests auto-enable release gating")
    def test_auto_release_gate() -> None:
        options = argparse.Namespace(release_gate=None)
        enabled, reason = _release_gate_enabled(
            options,
            {"status": "passed", "completed_utc": "2026-07-28T01:00:00+00:00"},
        )
        assert enabled and reason == "auto_final_smoke_manifest"

    results: list[dict[str, Any]] = []
    failed = 0
    for name, function in tests:
        try:
            function()
            results.append({"name": name, "passed": True})
        except Exception as exc:
            failed += 1
            results.append(
                {
                    "name": name,
                    "passed": False,
                    "error": f"{type(exc).__name__}: {exc}",
                }
            )
    report = {
        "tool": "rdc_csm_motion_compare",
        "tool_version": TOOL_VERSION,
        "self_tests": results,
        "passed": failed == 0,
        "failure_count": failed,
    }
    print(json.dumps(report, indent=2, sort_keys=True))
    return 0 if failed == 0 else 2

def _write_json(path: Path, value: dict[str, Any]) -> None:
    path.parent.mkdir(parents=True, exist_ok=True)
    temporary = path.with_name(f".{path.name}.{os.getpid()}.tmp")
    try:
        temporary.write_text(
            json.dumps(value, indent=2, sort_keys=True, allow_nan=False) + "\n",
            encoding="utf-8",
        )
        os.replace(temporary, path)
    finally:
        if temporary.exists():
            temporary.unlink()


def main(argv: list[str] | None = None) -> int:
    options = _parse_args(argv)
    if options.self_test:
        return _run_self_tests()
    deadline_started: float | None = None
    overall_deadline: float | None = None
    disk_preflight: dict[str, Any] | None = None
    runtime_toolchain_before: dict[str, Any] | None = None
    runtime_toolchain_after: dict[str, Any] | None = None
    runtime_toolchain_comparison: dict[str, Any] | None = None
    runtime_budget: ComparatorRuntimeBudget | None = None
    try:
        _validate_args(options)
        deadline_started = time.monotonic()
        overall_deadline = deadline_started + float(options.total_timeout)
        temporary: tempfile.TemporaryDirectory[str] | None = None
        if options.work_dir:
            work_root = options.work_dir.resolve()
            work_root.mkdir(parents=True, exist_ok=True)
        else:
            temporary = tempfile.TemporaryDirectory(prefix="rdc_csm_compare_")
            work_root = Path(temporary.name)
        try:
            capture_count = _comparison_capture_count(options)
            disk_preflight = _disk_space_preflight(
                work_root,
                capture_count=capture_count,
            )
            _require_disk_space(disk_preflight)
            runtime_budget = ComparatorRuntimeBudget(
                work_root,
                capture_count=capture_count,
                disk_preflight=disk_preflight,
            )
            _require_deadline(overall_deadline, stage="runtime toolchain hashing")
            runtime_toolchain_before = _toolchain_snapshot(_runtime_tool_paths())
            report = (
                _run_manifest_comparison(
                    options,
                    work_root,
                    overall_deadline=overall_deadline,
                    runtime_toolchain_before=runtime_toolchain_before,
                    runtime_budget=runtime_budget,
                )
                if options.manifest is not None
                else _run_comparison(
                    options,
                    work_root,
                    overall_deadline=overall_deadline,
                    runtime_budget=runtime_budget,
                )
            )
            runtime_toolchain_after = _toolchain_snapshot(_runtime_tool_paths())
            runtime_toolchain_comparison = _compare_toolchain_snapshots(
                runtime_toolchain_before,
                runtime_toolchain_after,
                required_roles=("comparator", "roi", "rdc_cli_package", "renderdoc_module"),
            )
            runtime_toolchain = {
                "schema": TOOLCHAIN_EVIDENCE_SCHEMA,
                "before_comparison": runtime_toolchain_before,
                "after_comparison": runtime_toolchain_after,
                "comparison": runtime_toolchain_comparison,
                "passed": runtime_toolchain_comparison.get("passed") is True,
                "errors": list(runtime_toolchain_comparison.get("errors", [])),
            }
            report["toolchain_evidence"] = runtime_toolchain
            report.setdefault("configuration", {})["disk_preflight"] = disk_preflight
            report["configuration"]["runtime_budget"] = runtime_budget.snapshot()
            deadline = _deadline_evidence(
                started_monotonic=deadline_started,
                overall_deadline=overall_deadline,
                configured_seconds=options.total_timeout,
            )
            report["deadline"] = deadline
            report["configuration"]["total_timeout_seconds"] = options.total_timeout
            report["configuration"]["per_operation_timeout_seconds"] = options.timeout
            report["configuration"]["cleanup_reserve_seconds"] = (
                CAPTURE_CLEANUP_RESERVE_SECONDS
            )
            if isinstance(report.get("release_evidence"), dict):
                report["release_evidence"]["toolchain_runtime"] = runtime_toolchain
                report["release_evidence"]["disk_preflight"] = disk_preflight
                report["release_evidence"]["deadline"] = deadline
            terminal_errors: list[str] = []
            if runtime_toolchain["passed"] is not True:
                terminal_errors.append(
                    "comparator/ROI toolchain changed or could not be rebound: "
                    + "; ".join(runtime_toolchain["errors"])
                )
            if deadline["exceeded"] is True:
                terminal_errors.append(
                    "total comparator deadline expired; no result may be published"
                )
            if terminal_errors:
                report.setdefault("errors", []).extend(terminal_errors)
                report["status"] = "error"
                report["passed"] = False
                if isinstance(report.get("summary"), dict):
                    report["summary"]["error_count"] = len(report.get("errors", []))
            if options.output:
                _write_json(options.output.resolve(), report)
            if options.json or not options.output:
                print(json.dumps(report, indent=2, sort_keys=True, allow_nan=False))
            elif report["input_mode"] == "smoke_manifest":
                print(
                    f"{report['status']}: "
                    f"cases={report['summary']['case_count']}, "
                    f"pairs={sum(report['summary']['pair_status_counts'].values())}, "
                    f"violations={report['summary']['violation_count']}, "
                    f"errors={report['summary']['error_count']}, "
                    f"report={options.output.resolve()}"
                )
            else:
                print(
                    f"{report['status']}: "
                    f"origin={report['origin_assessment']['primary']}, "
                    f"violations={len(report['violations'])}, "
                    f"report={options.output.resolve()}"
                )
            return {"pass": 0, "fail": 1}.get(report["status"], 2)
        finally:
            if temporary is not None:
                if options.keep_work_dir:
                    kept = Path.cwd() / f"rdc_csm_compare_{uuid.uuid4().hex[:8]}"
                    shutil.copytree(work_root, kept)
                    print(f"kept extraction files: {kept}", file=sys.stderr)
                temporary.cleanup()
    except KeyboardInterrupt:
        print("interrupted", file=sys.stderr)
        return 130
    except Exception as exc:
        error_report: dict[str, Any] = {
            "tool": "rdc_csm_motion_compare",
            "tool_version": TOOL_VERSION,
            "status": "error",
            "passed": False,
            "error": str(exc),
        }
        if disk_preflight is not None:
            error_report["disk_preflight"] = disk_preflight
        if runtime_toolchain_before is not None:
            error_report["toolchain_before"] = runtime_toolchain_before
        if runtime_toolchain_after is not None:
            error_report["toolchain_after"] = runtime_toolchain_after
        if runtime_toolchain_comparison is not None:
            error_report["toolchain_comparison"] = runtime_toolchain_comparison
        if runtime_budget is not None:
            error_report["runtime_budget"] = runtime_budget.snapshot()
        if deadline_started is not None and overall_deadline is not None:
            error_report["deadline"] = _deadline_evidence(
                started_monotonic=deadline_started,
                overall_deadline=overall_deadline,
                configured_seconds=options.total_timeout,
            )
        if os.environ.get("RDC_CSM_COMPARE_TRACEBACK"):
            error_report["traceback"] = traceback.format_exc()
        if options.output:
            _write_json(options.output.resolve(), error_report)
        stream = sys.stdout if options.json else sys.stderr
        print(
            json.dumps(error_report, indent=2, sort_keys=True, allow_nan=False),
            file=stream,
        )
        return 2

if "controller" in globals() and "state" in globals() and "rd" in globals():
    result = _d_extract_main()
elif __name__ == "__main__":
    raise SystemExit(main())
