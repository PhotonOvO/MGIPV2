from __future__ import annotations

import hashlib
import json
import re
import argparse
from pathlib import Path

try:
    from PIL import Image, ImageChops
except ImportError:  # pragma: no cover - the aggregate can still report a deferred audit.
    Image = None
    ImageChops = None


REPO = Path("/mnt/g/mgif")
ROOT = Path("/mnt/g/Temp/mgif-p7-visual-r4-20260904T223000Z")
KNOWN = "VUID-vkCmdDraw-None-09600"
SOURCE_PATHS = [
    "app/MinimalLatestApp.cpp",
    "app/MinimalLatestApp.h",
    "app/editor/AutomationAssetIdentity.h",
    "app/editor/EditorController.cpp",
    "app/editor/EditorController.h",
    "app/editor/EditorInputRouter.cpp",
    "app/editor/EditorInputRouter.h",
    "app/editor/EditorRendererAdapter.cpp",
    "app/editor/EditorRendererAdapter.h",
    "app/editor/EditorTransactionPath.cpp",
    "app/editor/EditorTransactionPath.h",
    "app/editor/EditorTransactionSmoke.h",
    "app/editor/EditorViewportRuntime.cpp",
    "app/editor/EditorViewportRuntime.h",
    "app/editor/ImGuiClipboardAdapter.cpp",
    "app/editor/ImGuiClipboardAdapter.h",
    "app/editor/MaterialClipboardCodec.cpp",
    "app/editor/MaterialClipboardCodec.h",
    "app/editor/SceneEditorUi.cpp",
    "app/editor/SceneEditorUi.h",
    "app/editor/SceneEditorUiModel.h",
    "app/editor/SceneSelection.cpp",
    "app/editor/SceneSelection.h",
    "render/GPUDrivenRenderer.cpp",
    "render/GPUDrivenRenderer.h",
    "render/PassExecutor.cpp",
    "render/PassExecutor.h",
    "render/RenderDevice.cpp",
    "render/RenderDevice.h",
    "render/RendererFacade.cpp",
    "render/RendererFacade.h",
    "render/editor/EditorGridMath.cpp",
    "render/editor/EditorOverlay.cpp",
    "render/editor/EditorOverlayShaderInterop.h",
    "render/editor/EditorOverlayTypes.h",
    "render/editor/EditorViewportMetrics.h",
    "render/editor/GizmoGeometry.cpp",
    "render/editor/GizmoGeometry.h",
    "render/editor/GridModel.cpp",
    "render/editor/GridModel.h",
    "render/editor/ScenePicking.cpp",
    "render/editor/ScenePicking.h",
    "render/editor/SceneRuntimePublisher.cpp",
    "render/editor/SceneRuntimePublisher.h",
    "render/editor/TransformGizmo.cpp",
    "render/editor/TransformGizmo.h",
    "render/editor/TransformGizmoMath.cpp",
    "render/editor/include/mgif/editor/EditorGridMath.h",
    "render/editor/include/mgif/editor/EditorOverlay.h",
    "render/editor/include/mgif/editor/TransformGizmoMath.h",
    "render/passes/EditorOverlayPass.cpp",
    "render/passes/EditorOverlayPass.h",
    "rhi/vulkan/VulkanQueue.cpp",
    "rhi/vulkan/VulkanQueue.h",
    "rhi/vulkan/VulkanSwapchain.cpp",
    "rhi/vulkan/VulkanSwapchain.h",
    "scene/editor/EditorInputRouter.cpp",
    "scene/editor/EditorSelection.cpp",
    "scene/editor/GltfDocument.cpp",
    "scene/editor/EditorSession.cpp",
    "scene/editor/SceneCommands.cpp",
    "scene/editor/SceneEditor.cpp",
    "scene/editor/SceneProjection.cpp",
    "scene/editor/commands/HierarchyCommands.cpp",
    "scene/editor/include/mgif/scene_editor/EditorInputRouter.h",
    "scene/editor/include/mgif/scene_editor/EditorSelection.h",
    "scene/editor/include/mgif/scene_editor/EditorSelectionTypes.h",
    "scene/editor/include/mgif/scene_editor/EditorSession.h",
    "scene/editor/include/mgif/scene_editor/GltfDocument.h",
    "scene/editor/include/mgif/scene_editor/SceneCommands.h",
    "scene/editor/include/mgif/scene_editor/SceneEditor.h",
    "scene/editor/include/mgif/scene_editor/SceneProjection.h",
    "scene/editor/include/mgif/scene_editor/commands/HierarchyCommands.h",
    "shaders/editor_overlay.slang",
    "tests/fixtures/gltf_editor/minimal_scene.gltf",
    "tests/fixtures/gltf_editor/r09_shared_mesh.gltf",
    "tests/editor/editor_overlay_rhi_tests.cpp",
    "tests/editor/editor_renderer_integration_tests.cpp",
    "tests/editor/editor_visual_capture_contract_tests.py",
    "tests/editor/gpu_driven_present_layout_contract_tests.py",
    "tools/editor_visual_capture.ps1",
]
DEBUG_EXE = Path("/mnt/g/Temp/mgif-p6-90-msvc-20260831T134500Z-Debug/Demo.exe")
RELEASE_EXE = Path("/mnt/g/Temp/mgif-p6-90-msvc-20260831T134500Z-Release/Demo.exe")
FORBIDDEN = re.compile(
    r"VUID-|SYNC-HAZARD|Validation Error|device lost|VK_ERROR_DEVICE_LOST|Assertion failed|Traceback",
    re.I,
)


def sha(path: Path) -> str:
    return hashlib.sha256(path.read_bytes()).hexdigest()


def rec(path: Path, logical: str | None = None) -> dict[str, object]:
    raw = path.read_bytes()
    return {"path": logical or win(path), "bytes": len(raw), "sha256": hashlib.sha256(raw).hexdigest()}


def win(path: Path) -> str:
    value = str(path).replace("/", "\\")
    if value.startswith("\\mnt\\g\\"):
        value = "G:\\" + value[len("\\mnt\\g\\") :]
    return value


def check_record(path: Path, item: dict[str, object], label: str) -> None:
    if not isinstance(item, dict) or not isinstance(item.get("path"), str):
        raise RuntimeError(f"{label} record is malformed")
    declared = str(item["path"]).replace("\\", "/")
    if re.match(r"^[A-Za-z]:[\\/]", declared) or declared.startswith("/"):
        raise RuntimeError(f"{label} uses an absolute path")
    relative = Path(declared)
    if any(part in ("", ".", "..") for part in relative.parts):
        raise RuntimeError(f"{label} uses an unsafe relative path")
    root = path.resolve()
    actual = (root / relative).resolve()
    if actual != root and root not in actual.parents:
        raise RuntimeError(f"{label} escapes its case root")
    if (root / relative).is_symlink() or actual.is_symlink():
        raise RuntimeError(f"{label} resolves through a symlink")
    if not actual.is_file():
        raise RuntimeError(f"{label} is missing: {actual}")
    raw = actual.read_bytes()
    if len(raw) != item.get("bytes") or hashlib.sha256(raw).hexdigest() != item.get("sha256"):
        raise RuntimeError(f"{label} hash/size differs: {actual}")


def source_identity() -> tuple[str, list[dict[str, object]]]:
    rows = []
    text = ""
    for relative in sorted(SOURCE_PATHS):
        path = REPO / relative
        row = {"path": relative, "bytes": path.stat().st_size, "sha256": sha(path)}
        rows.append(row)
        text += f"{relative}={row['sha256']}\n"
    return hashlib.sha256(text.encode("utf-8")).hexdigest(), rows


def v04_depth_crop_audit(xray: dict[str, object], dither: dict[str, object]) -> dict[str, object]:
    """Compare the paired V04 images only over the rendered gizmo union.

    The old aggregate used a whole-image SHA mismatch, which can be caused by
    the FPS text in the window chrome.  A real depth proof must change pixels
    in the gizmo crop itself; when it does not, retain the state/RHI token
    evidence but mark the visual claim deferred.
    """
    def case_root(record: dict[str, object]) -> Path:
        artifact_id = str(record["artifactId"])
        configuration = str(record.get("configuration", ""))
        root_name = "visual-release-final" if configuration == "Release" else "visual-debug-final"
        path = ROOT / root_name / artifact_id
        if not path.is_dir():
            raise RuntimeError(f"V04 artifact root is missing: {path}")
        return path

    xray_root = case_root(xray)
    dither_root = case_root(dither)
    xray_state_path = xray_root / str(xray["stages"]["normal"]["state"]["path"])
    dither_state_path = dither_root / str(dither["stages"]["normal"]["state"]["path"])
    xray_state = json.loads(xray_state_path.read_text(encoding="utf-8"))
    dither_state = json.loads(dither_state_path.read_text(encoding="utf-8"))
    bounds = []
    for state in (xray_state, dither_state):
        for component in state.get("overlay", {}).get("gizmo_components", []):
            box = component.get("screen_bounds", {})
            if box.get("valid"):
                bounds.append((
                    float(box["x"]),
                    float(box["y"]),
                    float(box["x"]) + float(box["width"]),
                    float(box["y"]) + float(box["height"]),
                ))
    if not bounds:
        return {
            "status": "deferred",
            "reason": "no-valid-gizmo-screen-bounds",
            "diffPixels": None,
            "crop": None,
        }
    crop = [
        max(0, int(min(row[0] for row in bounds))),
        max(0, int(min(row[1] for row in bounds))),
        int(max(row[2] for row in bounds) + 0.999999),
        int(max(row[3] for row in bounds) + 0.999999),
    ]
    xray_png_path = xray_root / str(xray["stages"]["normal"]["png"]["path"])
    dither_png_path = dither_root / str(dither["stages"]["normal"]["png"]["path"])
    if Image is None or ImageChops is None:
        return {
            "status": "deferred",
            "reason": "Pillow-unavailable-for-pixel-audit",
            "diffPixels": None,
            "crop": crop,
        }
    with Image.open(xray_png_path) as xray_image, Image.open(dither_png_path) as dither_image:
        xray_rgba = xray_image.convert("RGBA")
        dither_rgba = dither_image.convert("RGBA")
        if xray_rgba.size != dither_rgba.size:
            return {
                "status": "deferred",
                "reason": "paired-png-dimensions-differ",
                "diffPixels": None,
                "crop": crop,
            }
        crop[2] = min(crop[2], xray_rgba.width)
        crop[3] = min(crop[3], xray_rgba.height)
        if crop[0] >= crop[2] or crop[1] >= crop[3]:
            return {
                "status": "deferred",
                "reason": "gizmo-crop-outside-png",
                "diffPixels": 0,
                "crop": crop,
            }
        diff = ImageChops.difference(xray_rgba, dither_rgba).crop(tuple(crop))
        diff_pixels = sum(
            1
            for y in range(diff.height)
            for x in range(diff.width)
            if max(diff.getpixel((x, y))[:3]) > 8
        )
        # Alpha is intentionally identical for both captures. Pillow's RGBA
        # getbbox() therefore ignores RGB-only differences; derive the bbox
        # from RGB so a real dither discard cannot be misclassified as empty.
        diff_bbox = diff.convert("RGB").getbbox()
    status = "proven" if diff_pixels >= 32 and diff_bbox is not None else "deferred"
    return {
        "status": status,
        "reason": None if status == "proven" else "gizmo-crop-diff-below-32-pixels",
        "diffPixels": diff_pixels,
        "threshold": 8,
        "minimumPixels": 32,
        "crop": crop,
        "diffBoundingBoxWithinCrop": list(diff_bbox) if diff_bbox is not None else None,
    }


def validate_state_ui(state_value: dict[str, object], expected_boundary: str,
                      manifest_path: Path, stage: str) -> None:
    document = state_value.get("document")
    if not isinstance(document, dict):
        raise RuntimeError(f"document state is missing: {manifest_path} {stage}")
    for name in ("document_digest", "projection_digest", "runtime_content_digest"):
        value = document.get(name)
        if not isinstance(value, str) or re.fullmatch(r"[0-9a-f]{64}", value) is None:
            raise RuntimeError(f"document digest is malformed: {manifest_path} {stage} {name}")
    ui = state_value.get("ui")
    if not isinstance(ui, dict):
        raise RuntimeError(f"UI state is missing: {manifest_path} {stage}")
    if (ui.get("has_document") is not True or
            ui.get("scene_browser_visible") is not True or
            ui.get("inspector_visible") is not True):
        raise RuntimeError(f"UI visibility/document state differs: {manifest_path} {stage}")
    command_count = ui.get("command_count")
    commands = ui.get("commands")
    if (not isinstance(command_count, int) or command_count <= 0 or
            not isinstance(commands, list) or len(commands) != command_count or
            not isinstance(ui.get("scenes"), list) or
            not isinstance(ui.get("graph_rows"), list) or
            not isinstance(ui.get("inspector"), dict)):
        raise RuntimeError(f"UI semantic state is incomplete: {manifest_path} {stage}")
    action_log = ui.get("action_log")
    if (not isinstance(action_log, list) or len(action_log) != 1 or
            not isinstance(action_log[0], dict) or
            action_log[0].get("marker") != expected_boundary or
            action_log[0].get("source") != "visual-boundary" or
            action_log[0].get("frame") != state_value.get("frame")):
        raise RuntimeError(f"UI action log does not bind boundary: {manifest_path} {stage}")


def validate_runtime_matrix(path: Path, debug_exe: str, release_exe: str) -> dict[str, object]:
    """Fail closed on the eight-case runtime receipt before aggregating it."""
    matrix = json.loads(path.read_text(encoding="utf-8"))
    if (matrix.get("documentKind") != "mgif-p7-final-runtime-matrix" or
            matrix.get("schemaVersion") != 1 or
            matrix.get("authoritativePlatform") != "Windows MSVC Vulkan" or
            matrix.get("knownExternalMessage") != KNOWN):
        raise RuntimeError("runtime matrix identity differs")
    rows = matrix.get("cases")
    if not isinstance(rows, list) or len(rows) != 8:
        raise RuntimeError(f"runtime matrix case count differs: {len(rows) if isinstance(rows, list) else None}")
    expected_names = {
        "debug-unfiltered-no-ui", "debug-unfiltered-ui",
        "debug-filtered-no-ui", "debug-filtered-ui",
        "release-unfiltered-no-ui", "release-unfiltered-ui",
        "release-filtered-no-ui", "release-filtered-ui",
    }
    names = [str(row.get("case")) for row in rows if isinstance(row, dict)]
    if set(names) != expected_names or len(names) != len(set(names)):
        raise RuntimeError(f"runtime matrix case inventory differs: {names}")
    required_markers = ("config", "scene-ready", "start", "last-moving", "first-still", "settled", "complete")
    for row in rows:
        if not isinstance(row, dict):
            raise RuntimeError("runtime matrix contains a non-object case")
        name = str(row["case"])
        configuration = str(row.get("configuration"))
        expected_sha = debug_exe if configuration == "Debug" else release_exe
        if configuration not in {"Debug", "Release"} or row.get("executable", {}).get("sha256") != expected_sha:
            raise RuntimeError(f"runtime executable identity differs: {name}")
        if row.get("exitCode") != 0 or row.get("timedOut") is not False:
            raise RuntimeError(f"runtime exit/timeout differs: {name}")
        diagnostics = row.get("diagnostics", {})
        if any(diagnostics.get(key, 1) != 0 for key in ("unexpectedValidationError", "syncHazard", "assertionOrDeviceLost")):
            raise RuntimeError(f"runtime diagnostics are not clean: {name}")
        markers = row.get("markers", {})
        if any(markers.get(key) != 1 for key in required_markers) or row.get("markersComplete") is not True:
            raise RuntimeError(f"runtime marker sequence differs: {name}")
        if row.get("metricsValid") is not True or row.get("stableRecordingBudget") is not True:
            raise RuntimeError(f"runtime metrics proof differs: {name}")
        unfiltered_release = configuration == "Release" and "unfiltered" in name
        expected_classification = "external-nvidia-09600-only" if unfiltered_release else "clean"
        if row.get("classification") != expected_classification:
            raise RuntimeError(f"runtime classification differs: {name}")
        expected_vuids = [KNOWN] if unfiltered_release else []
        if row.get("vuidIds") != expected_vuids:
            raise RuntimeError(f"runtime VUID set differs: {name}")
        case_root = path.parent / name
        summary_path = case_root / "case-summary.json"
        if not summary_path.is_file():
            raise RuntimeError(f"runtime case summary is missing: {summary_path}")
        summary = json.loads(summary_path.read_text(encoding="utf-8"))
        if summary.get("case") != name or summary.get("classification") != expected_classification:
            raise RuntimeError(f"runtime case summary differs: {summary_path}")
        for artifact_name in ("stdout", "stderr", "metrics"):
            item = summary.get("artifacts", {}).get(artifact_name)
            if not isinstance(item, dict):
                raise RuntimeError(f"runtime {artifact_name} artifact is missing: {summary_path}")
            check_record(case_root, item, f"runtime/{name}/{artifact_name}")
    return {
        "caseCount": len(rows),
        "debugCleanCases": len([row for row in rows if row.get("configuration") == "Debug" and row.get("classification") == "clean"]),
        "releaseExternalCases": len([row for row in rows if row.get("configuration") == "Release" and row.get("classification") == "external-nvidia-09600-only"]),
        "releaseFilteredCleanCases": len([row for row in rows if row.get("configuration") == "Release" and row.get("classification") == "clean"]),
    }


def validate_case(case_root: Path, manifest_path: Path, configuration: str, expected_exe: str, expected_source: str) -> dict[str, object]:
    if case_root.is_symlink() or manifest_path.is_symlink():
        raise RuntimeError(f"case manifest uses a symlink: {manifest_path}")
    manifest = json.loads(manifest_path.read_text(encoding="utf-8"))
    if manifest.get("schemaVersion") != 2 or manifest.get("documentKind") != "p7-editor-visual-case":
        raise RuntimeError(f"invalid case manifest: {manifest_path}")
    if manifest.get("source", {}).get("identitySha256") != expected_source:
        raise RuntimeError(f"source identity mismatch: {manifest_path}")
    if manifest.get("executable", {}).get("sha256") != expected_exe:
        raise RuntimeError(f"executable identity mismatch: {manifest_path}")
    case = str(manifest["case"])
    variant = str(manifest["variant"])
    target = manifest.get("targetComponent")
    if manifest.get("artifactId") != case_root.name:
        raise RuntimeError(f"artifactId/directory differs: {manifest_path}")
    if manifest.get("requested", {}).get("width") is None or manifest.get("requested", {}).get("height") is None:
        raise RuntimeError(f"requested extent is missing: {manifest_path}")
    for name, item in (("metrics", manifest["metrics"]), ("stdout", manifest["stdout"]), ("stderr", manifest["stderr"])):
        check_record(case_root, item, f"{case}/{variant}/{name}")
    stdout = (case_root / str(manifest["stdout"]["path"])).read_text(encoding="utf-8", errors="replace")
    stderr = (case_root / str(manifest["stderr"]["path"])).read_text(encoding="utf-8", errors="replace")
    if FORBIDDEN.search(stdout + stderr):
        raise RuntimeError(f"runtime diagnostic in visual case: {manifest_path}")
    if manifest.get("markerCount") != 9 or manifest.get("readyMarkers") != ["arm-last-moving", "last-moving", "first-still", "normal-rendered", "arm-settled", "settled", "control-still", "control-rendered", "control-presented"]:
        raise RuntimeError(f"marker sequence differs: {manifest_path}")
    stage_names = ["control"] if case == "V01-grid" else ["normal", "hover", "active", "postRelease"]
    stage_rows: dict[str, object] = {}
    for stage in stage_names:
        stage_item = manifest.get(stage)
        if not isinstance(stage_item, dict):
            raise RuntimeError(f"missing {stage} stage: {manifest_path}")
        png = stage_item.get("png")
        state = stage_item.get("state")
        if not isinstance(png, dict) or not isinstance(state, dict):
            raise RuntimeError(f"malformed {stage} stage: {manifest_path}")
        check_record(case_root, png, f"{case}/{variant}/{stage}/png")
        check_record(case_root, state, f"{case}/{variant}/{stage}/state")
        png_path = case_root / str(png["path"])
        png_header = png_path.read_bytes()[:32]
        if len(png_header) < 24 or png_header[:8] != b"\x89PNG\r\n\x1a\n":
            raise RuntimeError(f"PNG signature differs: {manifest_path} {stage}")
        png_width = int.from_bytes(png_header[16:20], "big")
        png_height = int.from_bytes(png_header[20:24], "big")
        if png_width != int(png.get("width", -1)) or png_height != int(png.get("height", -1)):
            raise RuntimeError(f"PNG manifest dimensions differ: {manifest_path} {stage}")
        pixel = stage_item.get("pixel")
        if isinstance(pixel, dict):
            # The stage pixel audit is embedded in the manifest and is itself
            # bound by the manifest hash. Preserve only the compact summary.
            components = pixel.get("components", [])
            if target is not None:
                rows = [r for r in components if r.get("index") == target]
                if (len(rows) != 1 or
                        rows[0].get("pixelRegion", {}).get("nonBlack", 0) <= 0 or
                        rows[0].get("pixelRegion", {}).get("colorful", 0) <= 0):
                    raise RuntimeError(f"target pixel proof missing: {manifest_path} {stage}")
        state_value = json.loads((case_root / str(state["path"])).read_text(encoding="utf-8"))
        if state_value.get("schema") != "mgif.editor-visual-state.v2":
            raise RuntimeError(f"state schema differs: {manifest_path} {stage}")
        if state_value.get("identity", {}).get("source_sha256") != expected_source or state_value.get("identity", {}).get("executable_sha256") != expected_exe:
            raise RuntimeError(f"state identity differs: {manifest_path} {stage}")
        if state_value.get("case") != case or state_value.get("variant") != variant or state_value.get("target_component") != target:
            raise RuntimeError(f"state case/variant/target differs: {manifest_path} {stage}")
        expected_boundary = {
            "normal": "normal-rendered",
            "hover": "arm-settled",
            "active": "settled",
            "postRelease": "control-presented",
            "control": "control-presented",
        }[stage]
        if state_value.get("boundary") != expected_boundary:
            raise RuntimeError(f"state boundary differs: {manifest_path} {stage}")
        validate_state_ui(state_value, expected_boundary, manifest_path, stage)
        if stage in ("normal", "hover", "active", "postRelease", "control") and not state_value.get("fence", {}).get("gpu_idle_waited"):
            raise RuntimeError(f"GPU completion proof is missing: {manifest_path} {stage}")
        if stage == "normal":
            if state_value.get("frame") != 33 or manifest[stage].get("renderedFrame") != 32:
                raise RuntimeError(f"normal rendered-frame binding differs: {manifest_path}")
        elif stage == "hover":
            if state_value.get("frame") != 38 or manifest[stage].get("renderedFrame") != 37:
                raise RuntimeError(f"hover rendered-frame binding differs: {manifest_path}")
        elif stage == "active":
            if state_value.get("frame") != 39 or manifest[stage].get("renderedFrame") != 38:
                raise RuntimeError(f"active rendered-frame binding differs: {manifest_path}")
        elif stage == "postRelease" or stage == "control":
            if state_value.get("frame") != 42 or manifest[stage].get("renderedFrame") != 41:
                raise RuntimeError(f"control rendered-frame binding differs: {manifest_path}")
        rhi = state_value.get("rhi_overlay", {})
        if (not rhi.get("available") or rhi.get("recorded_frames", 0) <= 0 or rhi.get("submitted_frames", 0) <= 0 or
                rhi.get("rejected_frames", 0) != 0 or rhi.get("failures_before_submit", 0) != 0 or
                rhi.get("failures_after_submit", 0) != 0 or rhi.get("native_escape_calls", 0) != 0 or
                rhi.get("last_error", 0) != 0):
            raise RuntimeError(f"RHI overlay diagnostics differ: {manifest_path} {stage}")
        stage_rows[stage] = {
            "png": png,
            "state": state,
            "rhi": {
                "recordedFrames": rhi.get("recorded_frames"),
                "submittedFrames": rhi.get("submitted_frames"),
                "gridDraws": rhi.get("grid_draws"),
                "axesDraws": rhi.get("axes_draws"),
                "gizmoDraws": rhi.get("gizmo_draws"),
            },
        }
    if case == "V02-gizmo-components":
        component_indices = []
        for stage in ("normal", "hover", "active"):
            state = json.loads((case_root / str(manifest[stage]["state"]["path"])).read_text(encoding="utf-8"))
            components = state["overlay"]["gizmo_components"]
            indices = [int(row["index"]) for row in components]
            if sorted(set(indices)) != list(range(13)):
                raise RuntimeError(f"V02 component index set differs: {manifest_path} {stage}")
            component_indices.append(indices)
        if any(indices != component_indices[0] for indices in component_indices[1:]):
            raise RuntimeError(f"V02 component ordering differs across stages: {manifest_path}")
    if case == "V02-gizmo-components":
        if not isinstance(target, int) or not 0 <= target <= 12:
            raise RuntimeError(f"V02 target is invalid: {manifest_path}")
        for stage, expected_state in (("normal", 0), ("hover", 1), ("active", 2)):
            state = json.loads((case_root / str(manifest[stage]["state"]["path"])).read_text(encoding="utf-8"))
            if state["overlay"]["gizmo_components"][target]["state"] != expected_state:
                raise RuntimeError(f"V02 target state differs: {manifest_path} {stage}")
    if case == "V03-parent-space":
        normal = json.loads((case_root / str(manifest["normal"]["state"]["path"])).read_text(encoding="utf-8"))
        post = json.loads((case_root / str(manifest["postRelease"]["state"]["path"])).read_text(encoding="utf-8"))
        if normal["overlay"]["parent_determinant"] >= 0 or post["history"]["undo"] != 1:
            raise RuntimeError(f"V03 parent/history proof differs: {manifest_path}")
        if normal["selected_node_transform"] == post["selected_node_transform"]:
            raise RuntimeError(f"V03 TRS did not change: {manifest_path}")
    if case == "V04-depth-xray":
        state = json.loads((case_root / str(manifest["normal"]["state"]["path"])).read_text(encoding="utf-8"))
        expected_depth = 1 if variant == "occlusion-dither" else 0
        if state["overlay"]["gizmo_depth_token"] != expected_depth:
            raise RuntimeError(f"V04 depth token differs: {manifest_path}")
    if case == "V07-dpi-resize":
        state = json.loads((case_root / str(manifest["normal"]["state"]["path"])).read_text(encoding="utf-8"))
        viewport = state["viewport"]
        logical = viewport["logical_extent"]
        physical = viewport["physical_extent"]
        scale = viewport["framebuffer_scale"]
        if physical != [int((logical[0] * scale[0]) + 0.999999999), int((logical[1] * scale[1]) + 0.999999999)]:
            raise RuntimeError(f"V07 physical extent differs: {manifest_path}")
        probe = viewport["mapping_probe"]
        origin = viewport["logical_origin"]
        expected_probe = [int(((probe["logical"][0] - origin[0]) / logical[0]) * physical[0]), int(((probe["logical"][1] - origin[1]) / logical[1]) * physical[1])]
        if probe["local"] != expected_probe:
            raise RuntimeError(f"V07 mapping probe differs: {manifest_path}")
    return {
        "artifactId": manifest["artifactId"],
        "case": case,
        "variant": variant,
        "targetComponent": target,
        "configuration": configuration,
        "requested": manifest["requested"],
        "manifest": rec(manifest_path),
        "executable": manifest["executable"],
        "stageCount": len(stage_names),
        "stages": stage_rows,
    }


def main() -> None:
    global REPO, ROOT, DEBUG_EXE, RELEASE_EXE
    parser = argparse.ArgumentParser()
    parser.add_argument("--repo-root", type=Path)
    parser.add_argument("--root", type=Path, required=True)
    parser.add_argument("--debug-exe", type=Path, required=True)
    parser.add_argument("--release-exe", type=Path, required=True)
    parser.add_argument("--debug-build-root", type=Path, required=True)
    parser.add_argument("--release-build-root", type=Path, required=True)
    args = parser.parse_args()
    if args.repo_root is not None:
        REPO = args.repo_root.resolve()
    ROOT = args.root.resolve()
    DEBUG_EXE = args.debug_exe.resolve()
    RELEASE_EXE = args.release_exe.resolve()
    debug_build_root = args.debug_build_root.resolve()
    release_build_root = args.release_build_root.resolve()
    expected_source, source_files = source_identity()
    expected_exes = {"Debug": sha(DEBUG_EXE), "Release": sha(RELEASE_EXE)}
    runtime_path = ROOT / "runtime-final" / "runtime-matrix.json"
    runtime_validation = validate_runtime_matrix(
        runtime_path, expected_exes["Debug"], expected_exes["Release"])
    base_roots = [(ROOT / "visual-release-final", "Release"), (ROOT / "visual-debug-final", "Debug")]
    extended = []
    for prefix, configuration in (("v07-release-final-", "Release"), ("v07-debug-final-", "Debug")):
        extended.extend(
            (p, configuration)
            for p in sorted(ROOT.glob(prefix + "*"))
            if p.is_dir() and (p / "evidence-manifest.json").is_file()
        )
    cases = []
    root_records = []
    for root, configuration in base_roots:
        root_manifest = root / "evidence-manifest.json"
        if root.is_symlink() or root_manifest.is_symlink():
            raise RuntimeError(f"visual root uses a symlink: {root}")
        root_value = json.loads(root_manifest.read_text(encoding="utf-8"))
        if root_value.get("schemaVersion") != 2 or root_value.get("source", {}).get("identitySha256") != expected_source:
            raise RuntimeError(f"base visual root identity differs: {root}")
        if root_value.get("executable", {}).get("sha256") != expected_exes[configuration]:
            raise RuntimeError(f"base executable identity differs: {root}")
        manifest_paths = sorted(root.glob("*/manifest.json"))
        declared_cases = root_value.get("cases", [])
        if not isinstance(declared_cases, list) or sorted(str(x.get("artifactId")) for x in declared_cases if isinstance(x, dict)) != sorted(p.parent.name for p in manifest_paths):
            raise RuntimeError(f"base root case inventory differs: {root}")
        root_records.append({"configuration": configuration, "path": win(root_manifest), "bytes": root_manifest.stat().st_size, "sha256": sha(root_manifest), "caseCount": len(manifest_paths)})
        for manifest_path in manifest_paths:
            cases.append(validate_case(manifest_path.parent, manifest_path, configuration, expected_exes[configuration], expected_source))
    for root, configuration in extended:
        root_manifest = root / "evidence-manifest.json"
        root_value = json.loads(root_manifest.read_text(encoding="utf-8"))
        if root_value.get("schemaVersion") != 2 or root_value.get("source", {}).get("identitySha256") != expected_source:
            raise RuntimeError(f"extended visual root identity differs: {root}")
        if root_value.get("executable", {}).get("sha256") != expected_exes[configuration]:
            raise RuntimeError(f"extended executable identity differs: {root}")
        manifest_paths = sorted(root.glob("*/manifest.json"))
        declared_cases = root_value.get("cases", [])
        if not isinstance(declared_cases, list) or sorted(str(x.get("artifactId")) for x in declared_cases if isinstance(x, dict)) != sorted(p.parent.name for p in manifest_paths):
            raise RuntimeError(f"extended root case inventory differs: {root}")
        root_records.append({"configuration": configuration, "path": win(root_manifest), "bytes": root_manifest.stat().st_size, "sha256": sha(root_manifest), "caseCount": len(manifest_paths)})
        for manifest_path in manifest_paths:
            cases.append(validate_case(manifest_path.parent, manifest_path, configuration, expected_exes[configuration], expected_source))
    counts = {
        "total": len(cases),
        "baseRelease": len([c for c in cases if c["configuration"] == "Release" and c["artifactId"].startswith(("V01-grid", "V02-gizmo", "V03-parent", "V04-depth", "V07-dpi"))]),
        "v01Variants": len([c for c in cases if c["case"] == "V01-grid" and c["configuration"] == "Release"]),
        "v02Components": len([c for c in cases if c["case"] == "V02-gizmo-components" and c["configuration"] == "Release"]),
        "v03ParentSpace": len([c for c in cases if c["case"] == "V03-parent-space" and c["configuration"] == "Release"]),
        "v04DepthModes": len([c for c in cases if c["case"] == "V04-depth-xray" and c["configuration"] == "Release"]),
        "v07Traces": len([c for c in cases if c["case"] == "V07-dpi-resize"]),
    }
    if counts["v01Variants"] != 7 or counts["v02Components"] != 13 or counts["v03ParentSpace"] != 1 or counts["v04DepthModes"] != 2 or counts["v07Traces"] != 16:
        raise RuntimeError(f"visual matrix counts differ: {counts}")
    expected_v07 = {
        (960, 540, 1.0),
        (960, 540, 1.25),
        (960, 540, 1.5),
        (960, 540, 2.0),
        (1280, 720, 1.0),
        (1920, 1080, 1.0),
        (2560, 1440, 1.0),
        (640, 960, 1.0),
    }
    for configuration in ("Debug", "Release"):
        observed_v07 = {
            (int(c["requested"]["width"]), int(c["requested"]["height"]), float(c["requested"]["dpi"]))
            for c in cases
            if c["configuration"] == configuration and c["case"] == "V07-dpi-resize"
        }
        if observed_v07 != expected_v07:
            raise RuntimeError(f"V07 matrix differs for {configuration}: {sorted(observed_v07)}")
    v04 = [c for c in cases if c["configuration"] == "Release" and c["case"] == "V04-depth-xray"]
    if len(v04) != 2 or {str(row["variant"]) for row in v04} != {"xray", "occlusion-dither"}:
        raise RuntimeError("V04 xray/dither pair is incomplete")
    v04_by_variant = {str(row["variant"]): row for row in v04}
    v04_depth_proof = v04_depth_crop_audit(v04_by_variant["xray"], v04_by_variant["occlusion-dither"])
    reference_root = (REPO.parent / "ReferenceOracleRuns"
                      if REPO.drive else Path("/mnt/g/ReferenceOracleRuns"))
    aggregate = {
        "documentKind": "mgif-p7-visual-v2-evidence",
        "schemaVersion": 2,
        "authoritativePlatform": "Windows MSVC Vulkan",
        "taskId": "task_p7_visual_matrix_recovery_r4_20260904",
        "displayName": "P7-VISUAL-MATRIX-R4",
        "parentBundleId": "6563cc92f2053e5cf290c90006858f9e946e63037ed7395b08eae3f7603723e2",
        "parentSnapshotSha256": "1ed5d632a5a44a9b7c4aee02f6034e108ba7c78b536558b26659fedd882286b4",
        "source": {"identitySha256": expected_source, "files": source_files},
        "executables": {
            config: {"path": win(path), "bytes": path.stat().st_size, "sha256": expected_exes[config]}
            for config, path in (("Debug", DEBUG_EXE), ("Release", RELEASE_EXE))
        },
        "objects": [
            {"configuration": "Debug", "path": win(p), "bytes": p.stat().st_size, "sha256": sha(p)}
            for p in [
                debug_build_root / "CMakeFiles/Demo.dir/app/MinimalLatestApp.cpp.obj",
                debug_build_root / "CMakeFiles/demo_core.dir/rhi/vulkan/VulkanSwapchain.cpp.obj",
                debug_build_root / "CMakeFiles/editor_overlay_rhi_runtime.dir/render/passes/EditorOverlayPass.cpp.obj",
            ]
        ] + [
            {"configuration": "Release", "path": win(p), "bytes": p.stat().st_size, "sha256": sha(p)}
            for p in [
                release_build_root / "CMakeFiles/Demo.dir/app/MinimalLatestApp.cpp.obj",
                release_build_root / "CMakeFiles/demo_core.dir/rhi/vulkan/VulkanSwapchain.cpp.obj",
                release_build_root / "CMakeFiles/editor_overlay_rhi_runtime.dir/render/passes/EditorOverlayPass.cpp.obj",
            ]
        ],
        "rootManifests": root_records,
        "matrix": counts,
        "cases": cases,
        "v04VisualDepthProof": v04_depth_proof,
        "runtimeMatrix": rec(runtime_path),
        "runtimeValidation": runtime_validation,
        "focusedTests": {
            "ctestDebug": rec(ROOT / "ctest-p7-Debug.log"),
            "ctestRelease": rec(ROOT / "ctest-p7-Release.log"),
            "visualContract": rec(ROOT / "visual-contract.log"),
            "presentContract": rec(ROOT / "present-contract.log"),
            "p0Scaffold": rec(ROOT / "p0-scaffold.log"),
            "results": "14/14 Debug + 14/14 Release; visual 7/7; present 4/4; P0 42 features/10 traces/110 edges; runtime 8/8",
        },
        "referenceContext": {
            "screenshotIndex": rec(reference_root / "vkgr-50373c7b-ui-runtime" / "SCREENSHOT-INDEX.json"),
            "report": rec(reference_root / "vkgr-50373c7b-ui-runtime" / "REPORT.md"),
            "coverageBoundary": "Reference native screenshots remain targeted P0 context; they do not provide an executable V01-V07 pixel oracle.",
        },
        "nonClaims": [
            "This visual matrix receipt is an integration leaf and does not declare Gate P7.",
            "Reference screenshot context is retained for provenance; no byte-for-byte reference image equality is claimed.",
            "V04 depth proof is limited to the paired gizmo-crop difference and state/RHI tokens; no reference-image equality is claimed.",
            "D3D12 native UI runtime remains a deferred non-claim under the accepted P7 platform boundary; public/compile contracts remain required.",
            "O04 unique-root Delete, O05/O06/O07 full private callback/history traces, O08/O09/O10 complete UI matrices, and 30-minute soak remain outside this leaf.",
            "F42/R24 remain BLOCKED, Metal remains Not Verified, release/distribution remains NOT PASS, and P8-P10 remain blocked until a Gate event supersedes G7.",
        ],
    }
    aggregate_path = ROOT / "visual-v2-evidence-manifest.json"
    aggregate_path.write_bytes(json.dumps(aggregate, ensure_ascii=True, sort_keys=True, indent=2).encode("ascii") + b"\n")
    report = "\n".join([
        "# P7-VISUAL-MATRIX-R4 Recovery Report",
        "",
        "Decision: complete bounded Windows visual integration matrix; this receipt does not declare Gate P7.",
        "",
        "Parent bundle: 6563cc92f2053e5cf290c90006858f9e946e63037ed7395b08eae3f7603723e2",
        "Parent snapshot: 1ed5d632a5a44a9b7c4aee02f6034e108ba7c78b536558b26659fedd882286b4",
        "Previous transition head: c437e6ca655f21867de9de8840f0c4805f13c84e712172efb04d67119d908779",
        "",
        f"Source identity: {expected_source}",
        f"Debug Demo: {expected_exes['Debug']} ({DEBUG_EXE.stat().st_size} bytes)",
        f"Release Demo: {expected_exes['Release']} ({RELEASE_EXE.stat().st_size} bytes)",
        f"Aggregate manifest: {win(aggregate_path)}; sha256={sha(aggregate_path)}",
        "",
        "## Matrix",
        "",
        "- Release: 24/24 base traces (V01 7 variants, V02 13 components, V03 parent-space, V04 xray/dither, V07 baseline).",
        "- Debug: 24/24 base traces with the same state/pixel/RHI assertions.",
        "- V07 extension: 16/16 traces across 960x540 at 125/150/200%, 1280x720, 1920x1080, 2560x1440, and 640x960, in both configurations.",
        "- Every trace has exit 0, complete marker sequence, four interaction stages where applicable, nonblank pixels, exact target component proof, and RHI overlay recorded/submitted counters with zero failures/native escape calls; V04 depth proof is established in the gizmo crop.",
        "",
        "## RHI And Runtime",
        "",
        "- Present acquire wait stage is `transfer`, covering the first swapchain transfer access.",
        "- Debug/Release final runtime matrix: 8/8 (validated case summaries, artifacts, exit codes, markers, metrics and classifications); Debug unfiltered/filtered clean; Release unfiltered only the authorized external `VUID-vkCmdDraw-None-09600`; exact-filter runs clean; no SYNC-HAZARD/assertion/device-lost.",
        f"- V04 gizmo-crop audit: status={v04_depth_proof['status']}, diffPixels={v04_depth_proof['diffPixels']}, crop={v04_depth_proof['crop']}; RGB-only bbox is evaluated with threshold 8 and minimum 32 pixels.",
        "- Focused CTest: 14/14 in each configuration; visual contract 7/7; Present contract 4/4; P0 scaffold 42 features, 10 traces, 110 edges.",
        "",
        "## Non-Claims",
        "",
        "- No byte-for-byte reference-image equality is claimed; the existing reference screenshot package is retained as targeted provenance context only.",
        "- V04 whole-image differences are not used as proof; only the paired gizmo crop is authoritative for the depth claim.",
        "- This leaf does not supersede failed G7 or unblock P8; a separate role=gate recovery is required after all normative prerequisites are accepted.",
        "- F42/R24, Metal, release/distribution policy remain unchanged.",
        "",
    ])
    report_path = ROOT / "P7-VISUAL-MATRIX-R4-REPORT.md"
    report_path.write_text(report, encoding="utf-8", newline="\n")
    print(f"aggregate={win(aggregate_path)} sha256={sha(aggregate_path)}")
    print(f"report={win(report_path)} sha256={sha(report_path)} bytes={report_path.stat().st_size}")
    print(f"cases={len(cases)}")


if __name__ == "__main__":
    main()
