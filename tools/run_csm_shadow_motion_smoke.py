#!/usr/bin/env python3
"""Capture exact CSM automation boundary frames with persistent RenderDoc control."""

from __future__ import annotations

import argparse
import ast
import ctypes
import datetime as dt
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
import threading
import time
import uuid
from pathlib import Path
from typing import Any, Callable, Iterable

AUTHORITATIVE_DATE = "2026-07-29"
TOOL_VERSION = "3.6"
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
SMOKE_CAPTURE_BYTES_PER_BOUNDARY = 1024 * 1024 * 1024
SMOKE_QUARANTINE_CANDIDATES_PER_BOUNDARY = 1
SMOKE_MAX_CANDIDATES_PER_BOUNDARY = (
    1 + SMOKE_QUARANTINE_CANDIDATES_PER_BOUNDARY
)
SMOKE_REPLAY_SCRATCH_BYTES_PER_CASE = 512 * 1024 * 1024
DISK_SAFETY_MIN_BYTES = 4 * 1024 * 1024 * 1024
DISK_SAFETY_FRACTION = 0.25
PROTOCOL = "mgif-csm-capture-sync-v1"
BASE_BOUNDARIES = ("last-moving", "first-still", "settled")
CONTROL_BOUNDARY = "control-still"
ARM_LAST_MOVING = "arm-last-moving"
ARM_SETTLED = "arm-settled"
ARM_MARKERS = (ARM_LAST_MOVING, ARM_SETTLED)
APP_ARM_PROTOCOL_REQUIREMENT = (
    "app/MinimalLatestApp.h must emit pre-render arm-last-moving at L-1 and "
    "arm-settled at S-1 before their target-boundary captures"
)
MIN_HOLD_FRAMES = 3
MODES = ("csm-translate-stop", "csm-rotate-stop")
FORMAL_RELEASE_SMOKE_SEQUENCE = {
    "warmup_frames": 8,
    "motion_frames": 24,
    "hold_frames": 8,
}
RENDER_MODES = ("no-post", "taa-on")
GI_MODES = ("no-ddgi", "ddgi-on")
POSITION_TOLERANCE = 1.0e-5
ANGLE_TOLERANCE_DEGREES = 2.0e-5
POSE_TOLERANCE = max(POSITION_TOLERANCE, ANGLE_TOLERANCE_DEGREES)
TARGET_CONTROL_DISCONNECTED = 1
TARGET_CONTROL_NOOP = 3
TARGET_CONTROL_NEW_CAPTURE = 4
TARGET_CONTROL_CAPTURE_COPIED = 5
RDC_NO_ACTIVE_SESSION = "error: no active session"
EXECUTABLE_EVIDENCE_SCHEMA = "mgif-executable-evidence-v1"
EXECUTABLE_BINDING_SCHEMA = "mgif-executable-evidence-binding-v2"
IMMUTABLE_EXECUTABLE_SCHEMA = "mgif-immutable-executable-image-v1"
CAPTURE_FILE_EVIDENCE_SCHEMA = "mgif-renderdoc-capture-file-evidence-v1"
CAPTURE_COPY_BUDGET_SCHEMA = "mgif-renderdoc-capture-copy-budget-v1"
FINAL_CAPTURE_SET_SCHEMA = "mgif-csm-final-capture-set-v1"
TARGET_CONTROL_MESSAGE_NAMES = {
    0: "Unknown",
    TARGET_CONTROL_DISCONNECTED: "Disconnected",
    2: "Busy",
    TARGET_CONTROL_NOOP: "Noop",
    TARGET_CONTROL_NEW_CAPTURE: "NewCapture",
    TARGET_CONTROL_CAPTURE_COPIED: "CaptureCopied",
    6: "RegisterAPI",
    7: "NewChild",
    8: "CaptureProgress",
    9: "CapturableWindowCount",
    10: "RequestShow",
}
POSE_LOG_RE = re.compile(
    r"\[CSM_AUTOMATION\] marker=(last-moving|first-still|settled|control-still) "
    r"mode=(\S+) frame=(\d+) "
    r"current_pos=\(([^)]*)\) current_yaw_pitch=\(([^)]*)\) "
    r"previous_pos=\(([^)]*)\) previous_yaw_pitch=\(([^)]*)\)"
)

AUTOMATION_FRAME_MARKER_RE = re.compile(
    r"^CSM_AUTOMATION_FRAME\s+mode=(?P<mode>[^\s]+)\s+"
    r"boundary=(?P<boundary>last-moving|first-still|settled|control-still)\s+frame=(?P<frame>[0-9]+)$"
)

class SmokeFailure(RuntimeError):
    pass


class ProcessIdentityAccessDenied(SmokeFailure):
    pass


def utc_now() -> str:
    return dt.datetime.now(dt.timezone.utc).isoformat(timespec="milliseconds")


def utc_stamp() -> str:
    return dt.datetime.now(dt.timezone.utc).strftime("%Y%m%dT%H%M%S.%fZ")


def canonical_json_sha256(value: Any) -> str:
    payload = json.dumps(
        value,
        sort_keys=True,
        separators=(",", ":"),
        ensure_ascii=True,
        allow_nan=False,
    ).encode("utf-8")
    return hashlib.sha256(payload).hexdigest()


def python_tool_version(source: str, *, role: str, path: Path) -> str:
    try:
        tree = ast.parse(source, filename=str(path))
    except SyntaxError as exc:
        raise SmokeFailure(f"cannot parse {role} tool source {path}: {exc}") from exc
    if role in ("harness", "comparator"):
        for node in tree.body:
            if not isinstance(node, (ast.Assign, ast.AnnAssign)):
                continue
            targets = node.targets if isinstance(node, ast.Assign) else [node.target]
            if any(
                isinstance(target, ast.Name) and target.id == "TOOL_VERSION"
                for target in targets
            ):
                value = node.value
                if isinstance(value, ast.Constant) and isinstance(value.value, str):
                    return value.value
        raise SmokeFailure(f"{role} tool {path} has no literal TOOL_VERSION")
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
        raise SmokeFailure(
            f"ROI tool {path} has no literal ShadowEdgeMetricsResult.schema"
        )
    raise SmokeFailure(f"unknown tool evidence role {role!r}")


def collect_tool_file_evidence(path: Path, *, role: str) -> dict[str, Any]:
    try:
        resolved = path.expanduser().resolve(strict=True)
    except (OSError, RuntimeError) as exc:
        raise SmokeFailure(
            f"cannot resolve {role} tool path {path}: {type(exc).__name__}: {exc}"
        ) from exc
    if not resolved.is_file() or not resolved.is_absolute():
        raise SmokeFailure(f"{role} tool path is not an absolute file: {resolved}")
    digest = hashlib.sha256()
    payload = bytearray()
    try:
        with resolved.open("rb") as handle:
            stat_before = os.fstat(handle.fileno())
            while True:
                chunk = handle.read(1024 * 1024)
                if not chunk:
                    break
                digest.update(chunk)
                payload.extend(chunk)
            stat_after = os.fstat(handle.fileno())
        path_stat_after = resolved.stat()
    except OSError as exc:
        raise SmokeFailure(
            f"cannot read {role} tool {resolved}: {type(exc).__name__}: {exc}"
        ) from exc
    stable_fields = ("st_dev", "st_ino", "st_size", "st_mtime_ns")
    changed = [
        field
        for field in stable_fields
        if getattr(stat_before, field, None) != getattr(stat_after, field, None)
        or getattr(stat_after, field, None) != getattr(path_stat_after, field, None)
    ]
    if changed:
        raise SmokeFailure(
            f"{role} tool changed while hashing {resolved}: {changed}"
        )
    if int(stat_after.st_size) <= 0 or len(payload) != int(stat_after.st_size):
        raise SmokeFailure(f"{role} tool has an invalid stable size: {resolved}")
    try:
        source = bytes(payload).decode("utf-8-sig")
    except UnicodeDecodeError as exc:
        raise SmokeFailure(f"{role} tool is not UTF-8 source: {resolved}") from exc
    return {
        "schema": TOOL_FILE_EVIDENCE_SCHEMA,
        "role": role,
        "absolute_path": str(resolved),
        "version": python_tool_version(source, role=role, path=resolved),
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
        with resolved.open("rb") as handle:
            stat_before = os.fstat(handle.fileno())
            digest = hashlib.sha256()
            size = 0
            while True:
                chunk = handle.read(1024 * 1024)
                if not chunk:
                    break
                digest.update(chunk)
                size += len(chunk)
            stat_after = os.fstat(handle.fileno())
        path_after = resolved.stat()
    except OSError as exc:
        raise SmokeFailure(
            f"cannot hash {role} dependency file {path}: {type(exc).__name__}: {exc}"
        ) from exc
    if not all(
        stat.S_ISREG(candidate.st_mode)
        for candidate in (stat_before, stat_after, path_after)
    ):
        raise SmokeFailure(
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
        raise SmokeFailure(
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


def _rdc_cli_function(tree: ast.Module, name: str, *, source_path: Path) -> ast.FunctionDef:
    matches = [
        node
        for node in tree.body
        if isinstance(node, ast.FunctionDef) and node.name == name
    ]
    if len(matches) != 1:
        raise SmokeFailure(
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
        raise SmokeFailure(
            f"rdc-cli ordering proof expected one {target} call in "
            f"{source_path}:{function.name}, found {lines!r}"
        )
    return lines[0]


def collect_rdc_cli_session_ordering_evidence(
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
            raise SmokeFailure(
                f"rdc-cli ordering proof is missing hashed source {relative}"
            )
        source_path = Path(str(record.get("absolute_path", ""))).resolve(strict=True)
        try:
            payload = source_path.read_bytes()
        except OSError as exc:
            raise SmokeFailure(
                f"rdc-cli ordering source is unreadable {source_path}: "
                f"{type(exc).__name__}: {exc}"
            ) from exc
        digest = hashlib.sha256(payload).hexdigest()
        if len(payload) != record.get("size_bytes") or digest != record.get("sha256"):
            raise SmokeFailure(
                f"rdc-cli ordering source changed after package hashing: {source_path}"
            )
        try:
            source = payload.decode("utf-8-sig")
            trees[relative] = ast.parse(source, filename=str(source_path))
        except (UnicodeDecodeError, SyntaxError) as exc:
            raise SmokeFailure(
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
        raise SmokeFailure(
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

def collect_rdc_cli_package_evidence() -> dict[str, Any]:
    try:
        import rdc

        distribution = importlib.metadata.distribution("rdc-cli")
    except Exception as exc:
        raise SmokeFailure(
            f"rdc-cli package evidence is unavailable: {type(exc).__name__}: {exc}"
        ) from exc
    package_root = Path(rdc.__file__).resolve(strict=True).parent
    distribution_files = list(distribution.files or ())
    records: list[dict[str, Any]] = []
    for entry in sorted(distribution_files, key=lambda value: str(value).replace("\\", "/")):
        relative = str(entry).replace("\\", "/")
        parts = tuple(part.casefold() for part in Path(relative).parts)
        if "__pycache__" in parts or Path(relative).suffix.casefold() in (".pyc", ".pyo"):
            continue
        resolved = Path(distribution.locate_file(entry))
        if not resolved.is_file():
            raise SmokeFailure(
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
        raise SmokeFailure(
            f"rdc-cli full distribution evidence is incomplete: missing={missing!r}"
        )
    package_version = str(getattr(rdc, "__version__", "") or "")
    distribution_version = str(distribution.version)
    if not package_version or package_version != distribution_version:
        raise SmokeFailure(
            f"rdc-cli package/distribution version mismatch: "
            f"package={package_version!r}, distribution={distribution_version!r}"
        )
    session_ordering = collect_rdc_cli_session_ordering_evidence(
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
        "sha256": canonical_json_sha256(canonical),
        "read_consistent": True,
        "file_count": len(records),
        "zero_byte_file_count": sum(
            int(record["size_bytes"]) == 0 for record in records
        ),
        "files": records,
        "exclusions": ["__pycache__", "*.pyc", "*.pyo"],
        "session_publication_ordering": session_ordering,
    }


def collect_renderdoc_module_evidence() -> dict[str, Any]:
    try:
        from rdc.discover import find_renderdoc

        renderdoc_module = find_renderdoc()
    except Exception as exc:
        raise SmokeFailure(
            f"RenderDoc module discovery failed: {type(exc).__name__}: {exc}"
        ) from exc
    if renderdoc_module is None:
        raise SmokeFailure("RenderDoc module discovery returned None")
    module_file = getattr(renderdoc_module, "__file__", None)
    if not module_file:
        raise SmokeFailure("loaded RenderDoc module has no __file__ identity")
    record = _stable_dependency_file_record(
        Path(str(module_file)),
        relative_path=Path(str(module_file)).name,
        role="renderdoc_module",
    )
    try:
        version = str(renderdoc_module.GetVersionString())
    except Exception as exc:
        raise SmokeFailure(
            f"RenderDoc module version is unavailable: {type(exc).__name__}: {exc}"
        ) from exc
    if not version:
        raise SmokeFailure("RenderDoc module version is empty")
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

def collect_toolchain_snapshot(repo_root: Path) -> dict[str, Any]:
    tool_paths = {
        "harness": Path(__file__).resolve(strict=True),
        "comparator": (repo_root / "tools" / "rdc_csm_motion_compare.py").resolve(
            strict=True
        ),
        "roi": (repo_root / "tools" / "rdc_shadow_edge_metrics.py").resolve(
            strict=True
        ),
    }
    tools = {
        role: collect_tool_file_evidence(path, role=role)
        for role, path in sorted(tool_paths.items())
    }
    tools["rdc_cli_package"] = collect_rdc_cli_package_evidence()
    tools["renderdoc_module"] = collect_renderdoc_module_evidence()
    canonical = {
        role: {
            key: record[key]
            for key in ("role", "absolute_path", "version", "size_bytes", "sha256")
        }
        for role, record in tools.items()
    }
    return {
        "schema": TOOLCHAIN_EVIDENCE_SCHEMA,
        "captured_utc": utc_now(),
        "tools": tools,
        "bundle_sha256": canonical_json_sha256(canonical),
    }


def compare_toolchain_snapshots(
    before: dict[str, Any],
    after: dict[str, Any],
) -> dict[str, Any]:
    required_roles = TOOLCHAIN_REQUIRED_ROLES
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
        errors.append(f"before tool roles are invalid: {sorted(before_tools)}")
    if set(after_tools) != set(required_roles):
        errors.append(f"after tool roles are invalid: {sorted(after_tools)}")
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
        if role == "rdc_cli_package":
            left_ordering = (
                left.get("session_publication_ordering")
                if isinstance(left, dict)
                else None
            )
            right_ordering = (
                right.get("session_publication_ordering")
                if isinstance(right, dict)
                else None
            )
            checks["session_publication_ordering"] = (
                isinstance(left_ordering, dict)
                and isinstance(right_ordering, dict)
                and left_ordering.get("schema") == RDC_CLI_SESSION_ORDERING_SCHEMA
                and left_ordering.get("verified") is True
                and left_ordering.get("daemon_created_before_state_publication") is True
                and left_ordering.get("daemon_ready_before_state_publication") is True
                and left_ordering == right_ordering
            )
        role_errors = [name for name, passed in checks.items() if not passed]
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


def smoke_disk_space_preflight(
    run_root: Path,
    *,
    case_count: int,
    boundary_count: int,
    executable_size_bytes: int,
    disk_usage_fn: Any = shutil.disk_usage,
) -> dict[str, Any]:
    if case_count <= 0 or boundary_count <= 0:
        raise SmokeFailure("disk preflight case/boundary counts must be positive")
    resolved = run_root.resolve()
    try:
        usage = disk_usage_fn(resolved)
        free_bytes = int(usage.free)
    except Exception as exc:
        raise SmokeFailure(
            f"cannot query free space for capture work directory {resolved}: "
            f"{type(exc).__name__}: {exc}"
        ) from exc
    accepted_capture_count = case_count * boundary_count
    quarantine_capture_count = (
        accepted_capture_count * SMOKE_QUARANTINE_CANDIDATES_PER_BOUNDARY
    )
    accepted_capture_bytes = (
        accepted_capture_count * SMOKE_CAPTURE_BYTES_PER_BOUNDARY
    )
    quarantine_capture_bytes = (
        quarantine_capture_count * SMOKE_CAPTURE_BYTES_PER_BOUNDARY
    )
    replay_scratch_bytes = case_count * SMOKE_REPLAY_SCRATCH_BYTES_PER_CASE
    immutable_executable_bytes = max(0, int(executable_size_bytes))
    estimated_bytes = (
        accepted_capture_bytes
        + quarantine_capture_bytes
        + replay_scratch_bytes
        + immutable_executable_bytes
    )
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
        "schema": "mgif-csm-smoke-disk-preflight-v1",
        "passed": not errors,
        "work_directory": str(resolved),
        "case_count": case_count,
        "boundary_count": boundary_count,
        "accepted_capture_count": accepted_capture_count,
        "quarantine_capture_count": quarantine_capture_count,
        "estimate": {
            "capture_bytes_per_boundary": SMOKE_CAPTURE_BYTES_PER_BOUNDARY,
            "accepted_capture_bytes": accepted_capture_bytes,
            "quarantine_capture_bytes": quarantine_capture_bytes,
            "replay_scratch_bytes_per_case": SMOKE_REPLAY_SCRATCH_BYTES_PER_CASE,
            "replay_scratch_bytes": replay_scratch_bytes,
            "immutable_executable_bytes": immutable_executable_bytes,
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


def require_smoke_disk_space(preflight: dict[str, Any]) -> None:
    if preflight.get("passed") is not True:
        raise SmokeFailure(
            "capture work-directory disk preflight failed closed: "
            + "; ".join(str(error) for error in preflight.get("errors", []))
        )

def collect_capture_file_evidence(path: Path) -> dict[str, Any]:
    try:
        resolved = path.expanduser().resolve(strict=True)
    except (OSError, RuntimeError) as exc:
        raise SmokeFailure(
            f"cannot resolve capture evidence path {path}: {type(exc).__name__}: {exc}"
        ) from exc
    if not resolved.is_file() or resolved.suffix.lower() != ".rdc":
        raise SmokeFailure(f"capture evidence path is not an RDC file: {resolved}")
    digest = hashlib.sha256()
    try:
        with resolved.open("rb") as handle:
            stat_before = os.fstat(handle.fileno())
            while True:
                chunk = handle.read(1024 * 1024)
                if not chunk:
                    break
                digest.update(chunk)
            stat_after = os.fstat(handle.fileno())
        path_stat_after = resolved.stat()
    except OSError as exc:
        raise SmokeFailure(
            f"cannot read capture evidence for {resolved}: {type(exc).__name__}: {exc}"
        ) from exc
    stable_fields = ("st_dev", "st_ino", "st_size", "st_mtime_ns")
    changed = [
        field
        for field in stable_fields
        if getattr(stat_before, field, None) != getattr(stat_after, field, None)
        or getattr(stat_after, field, None) != getattr(path_stat_after, field, None)
    ]
    size_bytes = int(stat_after.st_size)
    if changed:
        raise SmokeFailure(f"capture changed while hashing {resolved}: {changed}")
    if size_bytes <= 0:
        raise SmokeFailure(f"capture is empty: {resolved}")
    if size_bytes > SMOKE_CAPTURE_BYTES_PER_BOUNDARY:
        raise SmokeFailure(
            f"capture exceeds the single-RDC cap: size={size_bytes}, "
            f"cap={SMOKE_CAPTURE_BYTES_PER_BOUNDARY}, path={resolved}"
        )
    return {
        "schema": CAPTURE_FILE_EVIDENCE_SCHEMA,
        "path": str(resolved),
        "canonical_path": _normalized_process_image(resolved),
        "size_bytes": size_bytes,
        "sha256": digest.hexdigest(),
        "read_consistent": True,
        "single_rdc_cap_bytes": SMOKE_CAPTURE_BYTES_PER_BOUNDARY,
    }


class CaptureCopyBudget:
    """Hard per-run candidate count/byte/free-space gate for CopyCapture."""

    def __init__(
        self,
        run_root: Path,
        *,
        case_count: int,
        boundary_count: int,
        disk_preflight: dict[str, Any],
        disk_usage_fn: Any = shutil.disk_usage,
    ) -> None:
        if case_count <= 0 or boundary_count <= 0:
            raise SmokeFailure("capture copy budget counts must be positive")
        estimate = disk_preflight.get("estimate")
        if not isinstance(estimate, dict) or disk_preflight.get("passed") is not True:
            raise SmokeFailure("capture copy budget requires a passed disk preflight")
        self._run_root = run_root.resolve()
        self._case_count = int(case_count)
        self._boundary_count = int(boundary_count)
        self._max_candidates = (
            self._case_count
            * self._boundary_count
            * SMOKE_MAX_CANDIDATES_PER_BOUNDARY
        )
        self._max_candidate_bytes = (
            self._max_candidates * SMOKE_CAPTURE_BYTES_PER_BOUNDARY
        )
        self._replay_scratch_bytes = (
            self._case_count * SMOKE_REPLAY_SCRATCH_BYTES_PER_CASE
        )
        self._safety_margin_bytes = int(estimate.get("safety_margin_bytes", 0))
        if self._safety_margin_bytes <= 0:
            raise SmokeFailure("capture copy budget has no positive safety margin")
        self._disk_usage_fn = disk_usage_fn
        self._candidate_count = 0
        self._actual_bytes = 0
        self._by_boundary: dict[str, int] = {}
        self._reservations: list[dict[str, Any]] = []

    def reserve_before_copy(
        self,
        *,
        case_name: str,
        boundary: str,
        capture: dict[str, Any],
        destination: Path,
    ) -> dict[str, Any]:
        try:
            declared_bytes = int(capture.get("byteSize", 0))
        except (TypeError, ValueError) as exc:
            raise SmokeFailure(
                f"NewCapture byteSize is invalid for {case_name}/{boundary}: {capture!r}"
            ) from exc
        if declared_bytes <= 0:
            raise SmokeFailure(
                f"NewCapture byteSize must be positive before CopyCapture for "
                f"{case_name}/{boundary}: {declared_bytes}"
            )
        if declared_bytes > SMOKE_CAPTURE_BYTES_PER_BOUNDARY:
            raise SmokeFailure(
                f"NewCapture exceeds the single-RDC cap before CopyCapture for "
                f"{case_name}/{boundary}: declared={declared_bytes}, "
                f"cap={SMOKE_CAPTURE_BYTES_PER_BOUNDARY}"
            )
        key = f"{case_name}/{boundary}"
        boundary_count = self._by_boundary.get(key, 0)
        if boundary_count >= SMOKE_MAX_CANDIDATES_PER_BOUNDARY:
            raise SmokeFailure(
                f"candidate budget exhausted for {key}: attempted more than "
                f"{SMOKE_MAX_CANDIDATES_PER_BOUNDARY} candidate(s)"
            )
        if self._candidate_count >= self._max_candidates:
            raise SmokeFailure(
                f"run candidate budget exhausted: max={self._max_candidates}"
            )
        resolved_destination = destination.resolve()
        if resolved_destination.exists():
            raise SmokeFailure(
                f"CopyCapture destination already exists before budget reservation: "
                f"{resolved_destination}"
            )
        remaining_candidate_capacity = (
            self._max_candidates - self._candidate_count
        ) * SMOKE_CAPTURE_BYTES_PER_BOUNDARY
        required_free_bytes = (
            remaining_candidate_capacity
            + self._replay_scratch_bytes
            + self._safety_margin_bytes
        )
        try:
            free_bytes = int(self._disk_usage_fn(self._run_root).free)
        except Exception as exc:
            raise SmokeFailure(
                f"cannot recheck free space before CopyCapture for {key}: "
                f"{type(exc).__name__}: {exc}"
            ) from exc
        if free_bytes < required_free_bytes:
            raise SmokeFailure(
                f"insufficient free space before CopyCapture for {key}: "
                f"free={free_bytes}, required={required_free_bytes}"
            )
        self._candidate_count += 1
        self._by_boundary[key] = boundary_count + 1
        reservation = {
            "schema": CAPTURE_COPY_BUDGET_SCHEMA,
            "reservation_index": self._candidate_count,
            "case": case_name,
            "boundary": boundary,
            "capture_id": int(capture.get("captureId", -1)),
            "declared_bytes": declared_bytes,
            "reserved_bytes": SMOKE_CAPTURE_BYTES_PER_BOUNDARY,
            "destination": str(resolved_destination),
            "free_bytes_before_copy": free_bytes,
            "required_free_bytes_before_copy": required_free_bytes,
            "actual_bytes": None,
            "completed": False,
        }
        self._reservations.append(reservation)
        return reservation

    def complete_copy(
        self,
        reservation: dict[str, Any],
        *,
        actual_bytes: int,
    ) -> dict[str, Any]:
        if reservation not in self._reservations or reservation.get("completed") is True:
            raise SmokeFailure("capture copy budget reservation is missing or already complete")
        actual = int(actual_bytes)
        if actual <= 0 or actual > SMOKE_CAPTURE_BYTES_PER_BOUNDARY:
            raise SmokeFailure(
                f"actual RDC size violates the single-capture budget: "
                f"actual={actual}, cap={SMOKE_CAPTURE_BYTES_PER_BOUNDARY}"
            )
        if self._actual_bytes + actual > self._max_candidate_bytes:
            raise SmokeFailure(
                f"actual candidate bytes exceed the run budget: "
                f"next={self._actual_bytes + actual}, max={self._max_candidate_bytes}"
            )
        self._actual_bytes += actual
        reservation["actual_bytes"] = actual
        reservation["completed"] = True
        reservation["completed_utc"] = utc_now()
        return dict(reservation)

    def snapshot(self) -> dict[str, Any]:
        return {
            "schema": CAPTURE_COPY_BUDGET_SCHEMA,
            "run_root": str(self._run_root),
            "case_count": self._case_count,
            "boundary_count": self._boundary_count,
            "max_candidates_per_boundary": SMOKE_MAX_CANDIDATES_PER_BOUNDARY,
            "max_candidate_count": self._max_candidates,
            "candidate_count": self._candidate_count,
            "single_rdc_cap_bytes": SMOKE_CAPTURE_BYTES_PER_BOUNDARY,
            "max_candidate_bytes": self._max_candidate_bytes,
            "actual_candidate_bytes": self._actual_bytes,
            "replay_scratch_bytes_reserved": self._replay_scratch_bytes,
            "safety_margin_bytes": self._safety_margin_bytes,
            "by_boundary": dict(sorted(self._by_boundary.items())),
            "reservations": [dict(record) for record in self._reservations],
            "within_count_budget": self._candidate_count <= self._max_candidates,
            "within_actual_byte_budget": self._actual_bytes <= self._max_candidate_bytes,
        }


def collect_executable_evidence(path: Path) -> dict[str, Any]:
    try:
        resolved = path.expanduser().resolve(strict=True)
    except (OSError, RuntimeError) as exc:
        raise SmokeFailure(
            f"cannot resolve executable evidence path {path}: {type(exc).__name__}: {exc}"
        ) from exc
    if not resolved.is_file():
        raise SmokeFailure(f"executable evidence path is not a file: {resolved}")

    digest = hashlib.sha256()
    try:
        with resolved.open("rb") as handle:
            stat_before = os.fstat(handle.fileno())
            while True:
                chunk = handle.read(1024 * 1024)
                if not chunk:
                    break
                digest.update(chunk)
            stat_after = os.fstat(handle.fileno())
        path_stat_after = resolved.stat()
    except OSError as exc:
        raise SmokeFailure(
            f"cannot read executable evidence for {resolved}: {type(exc).__name__}: {exc}"
        ) from exc

    stable_fields = ("st_dev", "st_ino", "st_size", "st_mtime_ns")
    changed_during_read = [
        field
        for field in stable_fields
        if getattr(stat_before, field, None) != getattr(stat_after, field, None)
        or getattr(stat_after, field, None) != getattr(path_stat_after, field, None)
    ]
    if changed_during_read:
        raise SmokeFailure(
            f"executable changed while collecting evidence for {resolved}: "
            f"{changed_during_read}"
        )
    if stat_after.st_size <= 0:
        raise SmokeFailure(f"executable evidence file is empty: {resolved}")

    return {
        "schema": EXECUTABLE_EVIDENCE_SCHEMA,
        "captured_utc": utc_now(),
        "resolved_path": str(resolved),
        "normalized_resolved_path": _normalized_process_image(resolved),
        "size_bytes": int(stat_after.st_size),
        "sha256": digest.hexdigest(),
        "stat": {
            "device": int(stat_after.st_dev),
            "inode": int(stat_after.st_ino),
            "modified_ns": int(stat_after.st_mtime_ns),
        },
        "read_consistent": True,
    }


def compare_executable_evidence(
    expected: dict[str, Any],
    observed: dict[str, Any],
    *,
    stage: str,
) -> dict[str, Any]:
    checks = {
        "schema": observed.get("schema") == expected.get("schema"),
        "resolved_path": observed.get("normalized_resolved_path")
        == expected.get("normalized_resolved_path"),
        "size_bytes": observed.get("size_bytes") == expected.get("size_bytes"),
        "sha256": observed.get("sha256") == expected.get("sha256"),
        "read_consistent": observed.get("read_consistent") is True,
    }
    errors = [name for name, passed in checks.items() if not passed]
    return {
        "stage": stage,
        "passed": not errors,
        "checks": checks,
        "errors": errors,
        "expected": {
            "resolved_path": expected.get("resolved_path"),
            "size_bytes": expected.get("size_bytes"),
            "sha256": expected.get("sha256"),
        },
        "observed": {
            "resolved_path": observed.get("resolved_path"),
            "size_bytes": observed.get("size_bytes"),
            "sha256": observed.get("sha256"),
        },
    }


def dedupe(values: Iterable[str]) -> list[str]:
    return list(dict.fromkeys(values))


def command_text(command: list[str]) -> str:
    return subprocess.list2cmdline(command) if os.name == "nt" else " ".join(command)


def atomic_write_json(path: Path, value: Any) -> None:
    path.parent.mkdir(parents=True, exist_ok=True)
    temporary = path.with_name(f".{path.name}.{os.getpid()}.tmp")
    with temporary.open("w", encoding="utf-8", newline="\n") as handle:
        json.dump(value, handle, indent=2, sort_keys=True)
        handle.write("\n")
        handle.flush()
        os.fsync(handle.fileno())
    os.replace(temporary, path)


def run_command(
    command: list[str], *, cwd: Path, timeout: float, check: bool = True
) -> subprocess.CompletedProcess[str]:
    try:
        completed = subprocess.run(
            command,
            cwd=cwd,
            capture_output=True,
            text=True,
            encoding="utf-8",
            errors="replace",
            timeout=timeout,
            check=False,
        )
    except subprocess.TimeoutExpired as exc:
        raise SmokeFailure(f"command timed out after {timeout:.1f}s: {command_text(command)}") from exc
    if check and completed.returncode != 0:
        output = "\n".join(part for part in (completed.stdout.strip(), completed.stderr.strip()) if part)
        raise SmokeFailure(
            f"command failed with exit {completed.returncode}: {command_text(command)}"
            + (f"\n{output}" if output else "")
        )
    return completed


def run_command_pumped(
    command: list[str],
    *,
    cwd: Path,
    timeout: float,
    pump: Callable[[], Any],
    check: bool = True,
) -> subprocess.CompletedProcess[str]:
    try:
        process = subprocess.Popen(
            command,
            cwd=cwd,
            stdout=subprocess.PIPE,
            stderr=subprocess.PIPE,
            text=True,
            encoding="utf-8",
            errors="replace",
        )
    except OSError as exc:
        raise SmokeFailure(f"failed to start command: {command_text(command)}: {exc}") from exc
    deadline = time.monotonic() + timeout
    stdout = ""
    stderr = ""
    try:
        while True:
            pump()
            remaining = deadline - time.monotonic()
            if remaining <= 0.0:
                process.kill()
                stdout, stderr = process.communicate()
                raise SmokeFailure(
                    f"command timed out after {timeout:.1f}s: {command_text(command)}"
                    + (f"\n{stdout[-4000:]}" if stdout else "")
                    + (f"\n{stderr[-4000:]}" if stderr else "")
                )
            try:
                stdout, stderr = process.communicate(timeout=min(0.1, remaining))
                break
            except subprocess.TimeoutExpired:
                continue
    except BaseException:
        if process.poll() is None:
            process.kill()
            process.communicate()
        raise
    completed = subprocess.CompletedProcess(
        command,
        process.returncode,
        stdout,
        stderr,
    )
    if check and completed.returncode != 0:
        output = "\n".join(
            part for part in (completed.stdout.strip(), completed.stderr.strip()) if part
        )
        raise SmokeFailure(
            f"command failed with exit {completed.returncode}: {command_text(command)}"
            + (f"\n{output}" if output else "")
        )
    return completed


class OutputCollector:
    def __init__(self, stream: Any, log_path: Path) -> None:
        self._stream = stream
        self._log_path = log_path
        self._lines: list[str] = []
        self._condition = threading.Condition()
        self._thread = threading.Thread(target=self._read, name="csm-smoke-output", daemon=True)
        self._thread.start()

    def _read(self) -> None:
        with self._log_path.open("w", encoding="utf-8", newline="\n") as log_file:
            try:
                for raw_line in self._stream:
                    line = raw_line.rstrip("\r\n")
                    log_file.write(line + "\n")
                    log_file.flush()
                    if "[CSM_AUTOMATION]" in line or line.lstrip().startswith("{"):
                        print(f"[target] {line}", flush=True)
                    with self._condition:
                        self._lines.append(line)
                        self._condition.notify_all()
            finally:
                with self._condition:
                    self._condition.notify_all()

    def wait_for_value(
        self,
        parser: Callable[[str], Any | None],
        *,
        timeout: float,
        description: str,
        process_identity: Any | None = None,
        pump: Callable[[], Any] | None = None,
    ) -> Any:
        deadline = time.monotonic() + timeout
        cursor = 0
        while True:
            with self._condition:
                current_length = len(self._lines)
                pending = self._lines[cursor:current_length]
                cursor = current_length
            for line in pending:
                value = parser(line)
                if value is not None:
                    return value
            if pump is not None:
                pump()
            if process_identity is not None and not process_identity.is_running():
                raise SmokeFailure(
                    f"target process {process_identity.identity} exited while waiting for "
                    f"{description}\n{self.tail()}"
                )
            remaining = deadline - time.monotonic()
            if remaining <= 0.0:
                raise SmokeFailure(f"timed out waiting for {description}\n{self.tail()}")
            with self._condition:
                self._condition.wait(timeout=min(0.1, remaining))

    def tail(self, count: int = 30) -> str:
        with self._condition:
            return "\n".join(self._lines[-count:])

    def join(self, timeout: float) -> bool:
        self._thread.join(timeout)
        return not self._thread.is_alive()


if os.name == "nt":
    from ctypes import wintypes

    class _FILETIME(ctypes.Structure):
        _fields_ = [
            ("dwLowDateTime", wintypes.DWORD),
            ("dwHighDateTime", wintypes.DWORD),
        ]

    _KERNEL32 = ctypes.WinDLL("kernel32", use_last_error=True)
    _KERNEL32.CreateFileW.argtypes = [
        wintypes.LPCWSTR,
        wintypes.DWORD,
        wintypes.DWORD,
        wintypes.LPVOID,
        wintypes.DWORD,
        wintypes.DWORD,
        wintypes.HANDLE,
    ]
    _KERNEL32.CreateFileW.restype = wintypes.HANDLE
    _KERNEL32.GetFinalPathNameByHandleW.argtypes = [
        wintypes.HANDLE,
        wintypes.LPWSTR,
        wintypes.DWORD,
        wintypes.DWORD,
    ]
    _KERNEL32.GetFinalPathNameByHandleW.restype = wintypes.DWORD
    _KERNEL32.OpenProcess.argtypes = [wintypes.DWORD, wintypes.BOOL, wintypes.DWORD]
    _KERNEL32.OpenProcess.restype = wintypes.HANDLE
    _KERNEL32.GetProcessId.argtypes = [wintypes.HANDLE]
    _KERNEL32.GetProcessId.restype = wintypes.DWORD
    _KERNEL32.GetProcessTimes.argtypes = [
        wintypes.HANDLE,
        ctypes.POINTER(_FILETIME),
        ctypes.POINTER(_FILETIME),
        ctypes.POINTER(_FILETIME),
        ctypes.POINTER(_FILETIME),
    ]
    _KERNEL32.GetProcessTimes.restype = wintypes.BOOL
    _KERNEL32.GetFileTime.argtypes = [
        wintypes.HANDLE,
        ctypes.POINTER(_FILETIME),
        ctypes.POINTER(_FILETIME),
        ctypes.POINTER(_FILETIME),
    ]
    _KERNEL32.GetFileTime.restype = wintypes.BOOL
    _KERNEL32.GetDriveTypeW.argtypes = [wintypes.LPCWSTR]
    _KERNEL32.GetDriveTypeW.restype = wintypes.UINT
    _KERNEL32.GetVolumeInformationByHandleW.argtypes = [
        wintypes.HANDLE,
        wintypes.LPWSTR,
        wintypes.DWORD,
        ctypes.POINTER(wintypes.DWORD),
        ctypes.POINTER(wintypes.DWORD),
        ctypes.POINTER(wintypes.DWORD),
        wintypes.LPWSTR,
        wintypes.DWORD,
    ]
    _KERNEL32.GetVolumeInformationByHandleW.restype = wintypes.BOOL
    _KERNEL32.QueryFullProcessImageNameW.argtypes = [
        wintypes.HANDLE,
        wintypes.DWORD,
        wintypes.LPWSTR,
        ctypes.POINTER(wintypes.DWORD),
    ]
    _KERNEL32.QueryFullProcessImageNameW.restype = wintypes.BOOL
    _KERNEL32.WaitForSingleObject.argtypes = [wintypes.HANDLE, wintypes.DWORD]
    _KERNEL32.WaitForSingleObject.restype = wintypes.DWORD
    _KERNEL32.TerminateProcess.argtypes = [wintypes.HANDLE, wintypes.UINT]
    _KERNEL32.TerminateProcess.restype = wintypes.BOOL
    _KERNEL32.CloseHandle.argtypes = [wintypes.HANDLE]
    _KERNEL32.CloseHandle.restype = wintypes.BOOL
    _KERNEL32.GetCurrentProcess.argtypes = []
    _KERNEL32.GetCurrentProcess.restype = wintypes.HANDLE
    _KERNEL32.DuplicateHandle.argtypes = [
        wintypes.HANDLE,
        wintypes.HANDLE,
        wintypes.HANDLE,
        ctypes.POINTER(wintypes.HANDLE),
        wintypes.DWORD,
        wintypes.BOOL,
        wintypes.DWORD,
    ]
    _KERNEL32.DuplicateHandle.restype = wintypes.BOOL
    _NTDLL = ctypes.WinDLL("ntdll", use_last_error=True)
    _NTDLL.NtResumeProcess.argtypes = [wintypes.HANDLE]
    _NTDLL.NtResumeProcess.restype = wintypes.LONG

    _GENERIC_READ = 0x80000000
    _FILE_SHARE_READ = 0x00000001
    _OPEN_EXISTING = 3
    _FILE_ATTRIBUTE_NORMAL = 0x00000080
    _INVALID_HANDLE_VALUE = ctypes.c_void_p(-1).value
    _ERROR_ACCESS_DENIED = 5
    _PROCESS_TERMINATE = 0x0001
    _PROCESS_QUERY_LIMITED_INFORMATION = 0x1000
    _SYNCHRONIZE = 0x00100000
    _DUPLICATE_SAME_ACCESS = 0x00000002
    _CREATE_SUSPENDED = 0x00000004
    _WAIT_OBJECT_0 = 0x00000000
    _WAIT_TIMEOUT = 0x00000102
    _WAIT_FAILED = 0xFFFFFFFF
    _WINDOWS_EPOCH_OFFSET_SECONDS = 11644473600
    _WINDOWS_TICKS_PER_SECOND = 10_000_000


def _normalized_process_image(value: str | Path) -> str:
    return os.path.normcase(os.path.abspath(str(value)))


def _process_images_match(expected: str | Path, observed: str | Path) -> bool:
    try:
        return os.path.samefile(expected, observed)
    except (OSError, ValueError):
        return _normalized_process_image(expected) == _normalized_process_image(observed)


def _windows_handle_path(handle: int) -> str:
    capacity = 32768
    buffer = ctypes.create_unicode_buffer(capacity)
    length = int(
        _KERNEL32.GetFinalPathNameByHandleW(handle, buffer, capacity, 0)
    )
    if length <= 0 or length >= capacity:
        error_code = ctypes.get_last_error()
        raise SmokeFailure(
            "GetFinalPathNameByHandleW failed for executable lock with error "
            f"{error_code}: {ctypes.FormatError(error_code).strip()}"
        )
    value = buffer.value
    if value.startswith("\\\\?\\UNC\\"):
        return "\\\\" + value[8:]
    if value.startswith("\\\\?\\"):
        return value[4:]
    return value


class LockedExecutableImage:
    """A held file identity that denies write/delete sharing on Windows."""

    def __init__(
        self,
        *,
        path: Path,
        native_handle: int,
        backend: str,
        write_share_denied: bool,
        delete_share_denied: bool,
    ) -> None:
        self.path = path
        self._native_handle = native_handle
        self._backend = backend
        self._write_share_denied = bool(write_share_denied)
        self._delete_share_denied = bool(delete_share_denied)
        self._closed = False
        self._identity = uuid.uuid4().hex
    @classmethod
    def acquire(cls, path: Path) -> "LockedExecutableImage":
        try:
            resolved = path.expanduser().resolve(strict=True)
        except (OSError, RuntimeError) as exc:
            raise SmokeFailure(
                f"cannot resolve executable lock path {path}: {type(exc).__name__}: {exc}"
            ) from exc
        if not resolved.is_file():
            raise SmokeFailure(f"executable lock path is not a file: {resolved}")
        if os.name != "nt":
            raise SmokeFailure(
                "immutable executable binding requires Windows deny-write/delete file sharing"
            )
        handle = _KERNEL32.CreateFileW(
            str(resolved),
            _GENERIC_READ,
            _FILE_SHARE_READ,
            None,
            _OPEN_EXISTING,
            _FILE_ATTRIBUTE_NORMAL,
            None,
        )
        if not handle or int(handle) == int(_INVALID_HANDLE_VALUE):
            error_code = ctypes.get_last_error()
            raise SmokeFailure(
                f"CreateFileW immutable lock failed for {resolved} with error "
                f"{error_code}: {ctypes.FormatError(error_code).strip()}"
            )
        try:
            handle_path = Path(_windows_handle_path(int(handle)))
            if not _process_images_match(resolved, handle_path):
                raise SmokeFailure(
                    "immutable executable handle path mismatch: "
                    f"requested={resolved}, handle={handle_path}"
                )
            return cls(
                path=resolved,
                native_handle=int(handle),
                backend="windows-createfile-deny-write-delete",
                write_share_denied=True,
                delete_share_denied=True,
            )
        except Exception:
            _KERNEL32.CloseHandle(handle)
            raise

    def metadata(self) -> dict[str, Any]:
        return {
            "schema": IMMUTABLE_EXECUTABLE_SCHEMA,
            "lock_identity": self._identity,
            "resolved_path": str(self.path),
            "normalized_resolved_path": _normalized_process_image(self.path),
            "backend": self._backend,
            "native_handle_held": not self._closed,
            "write_share_denied": self._write_share_denied,
            "delete_share_denied": self._delete_share_denied,
        }

    def collect_evidence(self) -> dict[str, Any]:
        if self._closed:
            raise SmokeFailure(
                f"immutable executable lock is closed for {self.path}"
            )
        evidence = collect_executable_evidence(self.path)
        evidence["immutable_lock"] = self.metadata()
        return evidence

    def close(self) -> dict[str, Any]:
        result = {
            "lock_identity": self._identity,
            "resolved_path": str(self.path),
            "closed_before": self._closed,
            "closed": False,
        }
        if self._closed:
            result["closed"] = True
            return result
        if not _KERNEL32.CloseHandle(self._native_handle):
            error_code = ctypes.get_last_error()
            result["error"] = (
                f"CloseHandle failed with {error_code}: "
                f"{ctypes.FormatError(error_code).strip()}"
            )
            return result
        self._closed = True
        result["closed"] = True
        return result


def compare_executable_content_evidence(
    source: dict[str, Any],
    copied: dict[str, Any],
    *,
    stage: str,
) -> dict[str, Any]:
    checks = {
        "schema": copied.get("schema") == source.get("schema"),
        "size_bytes": copied.get("size_bytes") == source.get("size_bytes"),
        "sha256": copied.get("sha256") == source.get("sha256"),
        "source_read_consistent": source.get("read_consistent") is True,
        "copy_read_consistent": copied.get("read_consistent") is True,
    }
    errors = [name for name, passed in checks.items() if not passed]
    return {
        "stage": stage,
        "passed": not errors,
        "checks": checks,
        "errors": errors,
        "source": {
            "resolved_path": source.get("resolved_path"),
            "size_bytes": source.get("size_bytes"),
            "sha256": source.get("sha256"),
        },
        "copy": {
            "resolved_path": copied.get("resolved_path"),
            "size_bytes": copied.get("size_bytes"),
            "sha256": copied.get("sha256"),
        },
    }


def prepare_immutable_executable_binding(
    source_executable: Path,
    run_root: Path,
) -> tuple[dict[str, Any], LockedExecutableImage]:
    source_lock: LockedExecutableImage | None = None
    launch_lock: LockedExecutableImage | None = None
    try:
        source_lock = LockedExecutableImage.acquire(source_executable)
        source_lock_metadata = source_lock.metadata()
        source_before = source_lock.collect_evidence()
        launch_directory = run_root / "executable"
        launch_directory.mkdir(parents=True, exist_ok=True)
        launch_path = launch_directory / (
            f"{source_executable.stem}.{source_before['sha256']}{source_executable.suffix}"
        )
        if launch_path.exists():
            raise SmokeFailure(
                f"immutable launch image already exists; refusing overwrite: {launch_path}"
            )
        try:
            with source_lock.path.open("rb") as source_handle, launch_path.open("xb") as copy_handle:
                shutil.copyfileobj(source_handle, copy_handle, length=1024 * 1024)
                copy_handle.flush()
                os.fsync(copy_handle.fileno())
            os.chmod(launch_path, stat.S_IREAD | stat.S_IEXEC)
        except Exception:
            if launch_path.exists():
                try:
                    os.chmod(launch_path, stat.S_IWRITE | stat.S_IREAD)
                    launch_path.unlink()
                except OSError:
                    pass
            raise

        launch_lock = LockedExecutableImage.acquire(launch_path)
        launch_baseline = launch_lock.collect_evidence()
        content_comparison = compare_executable_content_evidence(
            source_before,
            launch_baseline,
            stage="source-to-immutable-launch-copy",
        )
        if content_comparison["passed"] is not True:
            raise SmokeFailure(
                "immutable launch copy content does not match source executable: "
                f"{content_comparison['errors']}"
            )
        source_close = source_lock.close()
        source_lock = None
        if source_close.get("closed") is not True:
            raise SmokeFailure(
                f"source executable lock did not close after copy: {source_close!r}"
            )
        binding = {
            "schema": EXECUTABLE_BINDING_SCHEMA,
            "required": True,
            "source": {
                "resolved_path": str(source_executable.resolve()),
                "before_copy": source_before,
                "lock_during_copy": source_lock_metadata,
                "lock_close_after_copy": source_close,
                "after_all_cases": None,
                "after_all_cases_comparison": None,
            },
            "launch_image": {
                "resolved_path": str(launch_path.resolve()),
                "sha_named": launch_path.name
                == f"{source_executable.stem}.{source_before['sha256']}{source_executable.suffix}",
                "baseline": launch_baseline,
                "source_copy_comparison": content_comparison,
                "immutable_lock": launch_lock.metadata(),
                "read_only_mode_set": not bool(launch_path.stat().st_mode & stat.S_IWRITE),
                "after_all_cases": None,
                "after_all_cases_comparison": None,
                "lock_close_after_all_cases": None,
            },
            "launch_working_directory": str(source_executable.resolve().parent),
            "dependency_search": {
                "working_directory_is_source_build_directory": True,
                "source_build_directory_prepended_to_path": True,
            },
            "case_checks": [],
            "errors": [],
            "passed": False,
        }
        return binding, launch_lock
    except Exception:
        if source_lock is not None:
            source_lock.close()
        if launch_lock is not None:
            launch_lock.close()
        raise


class StableProcessIdentity:
    """A native process handle that remains bound to one process lifetime."""

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
        ownership_source: str = "pid-acquired-stable-handle",
        duplicated_from_launcher_handle: bool = False,
        pid_lookup_used: bool = True,
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
        self._ownership_source = ownership_source
        self._duplicated_from_launcher_handle = bool(duplicated_from_launcher_handle)
        self._pid_lookup_used = bool(pid_lookup_used)
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
    def from_launch_owned_process(
        cls,
        process: subprocess.Popen[str],
        *,
        expected_image: Path,
    ) -> "StableProcessIdentity":
        """Duplicate the exact Windows process handle returned by CreateProcess."""
        if os.name != "nt":
            raise SmokeFailure(
                "launch-owned Demo identity requires a duplicated Windows process handle"
            )
        source_handle = getattr(process, "_handle", None)
        if source_handle in (None, 0):
            raise SmokeFailure(
                "subprocess launcher did not expose its CreateProcess-owned process handle"
            )
        duplicated = wintypes.HANDLE()
        current_process = _KERNEL32.GetCurrentProcess()
        if not _KERNEL32.DuplicateHandle(
            current_process,
            wintypes.HANDLE(int(source_handle)),
            current_process,
            ctypes.byref(duplicated),
            0,
            False,
            _DUPLICATE_SAME_ACCESS,
        ):
            error_code = ctypes.get_last_error()
            raise SmokeFailure(
                "DuplicateHandle failed for the launcher-owned Demo process handle with "
                f"error {error_code}: {ctypes.FormatError(error_code).strip()}"
            )
        handle = int(duplicated.value or 0)
        if handle <= 0:
            raise SmokeFailure("DuplicateHandle returned an invalid Demo process handle")
        try:
            observed_pid = int(_KERNEL32.GetProcessId(handle))
            launcher_pid = int(process.pid)
            if launcher_pid <= 0 or observed_pid <= 0 or observed_pid != launcher_pid:
                raise SmokeFailure(
                    "launcher-owned process handle PID mismatch: "
                    f"Popen={launcher_pid}, handle={observed_pid}"
                )
            creation = _FILETIME()
            exit_time = _FILETIME()
            kernel_time = _FILETIME()
            user_time = _FILETIME()
            if not _KERNEL32.GetProcessTimes(
                handle,
                ctypes.byref(creation),
                ctypes.byref(exit_time),
                ctypes.byref(kernel_time),
                ctypes.byref(user_time),
            ):
                error_code = ctypes.get_last_error()
                raise SmokeFailure(
                    "GetProcessTimes failed for the launcher-owned Demo handle with "
                    f"error {error_code}: {ctypes.FormatError(error_code).strip()}"
                )
            creation_ticks = (
                int(creation.dwHighDateTime) << 32
            ) | int(creation.dwLowDateTime)
            if creation_ticks <= 0:
                raise SmokeFailure(
                    "launcher-owned Demo handle returned an invalid creation FILETIME"
                )
            image_capacity = wintypes.DWORD(32768)
            image_buffer = ctypes.create_unicode_buffer(image_capacity.value)
            if not _KERNEL32.QueryFullProcessImageNameW(
                handle,
                0,
                image_buffer,
                ctypes.byref(image_capacity),
            ):
                error_code = ctypes.get_last_error()
                raise SmokeFailure(
                    "QueryFullProcessImageNameW failed for the launcher-owned Demo handle "
                    f"with error {error_code}: {ctypes.FormatError(error_code).strip()}"
                )
            image_path = image_buffer.value
            if not _process_images_match(expected_image, image_path):
                raise SmokeFailure(
                    "launcher-owned Demo image mismatch: "
                    f"observed={image_path!r}, expected={str(expected_image)!r}"
                )
            wait_result = int(_KERNEL32.WaitForSingleObject(handle, 0))
            if wait_result != _WAIT_TIMEOUT:
                if wait_result == _WAIT_OBJECT_0:
                    raise SmokeFailure(
                        "Demo exited before its launch-owned stable identity was established"
                    )
                error_code = ctypes.get_last_error()
                raise SmokeFailure(
                    "WaitForSingleObject failed while binding the launcher-owned Demo "
                    f"handle: result={wait_result}, error={error_code}: "
                    f"{ctypes.FormatError(error_code).strip()}"
                )
            creation_key = f"winfiletime:{creation_ticks}"
            return cls(
                pid=observed_pid,
                identity=f"{observed_pid}@{creation_key}",
                creation_time_key=creation_key,
                creation_time_unix_seconds=(
                    creation_ticks / _WINDOWS_TICKS_PER_SECOND
                    - _WINDOWS_EPOCH_OFFSET_SECONDS
                ),
                image_path=image_path,
                native_handle=handle,
                terminate_access=True,
                backend="windows-launch-owned-duplicated-process-handle",
                ownership_source="launcher-owned-duplicated-process-handle",
                duplicated_from_launcher_handle=True,
                pid_lookup_used=False,
                creation_filetime_ticks=creation_ticks,
            )
        except Exception:
            _KERNEL32.CloseHandle(handle)
            raise
    @classmethod
    def acquire(
        cls,
        pid: int,
        *,
        expected_image: Path | None = None,
        require_terminate: bool = True,
    ) -> "StableProcessIdentity":
        if pid <= 0:
            raise SmokeFailure(f"cannot acquire stable identity for invalid pid {pid}")
        if os.name == "nt":
            access = _PROCESS_QUERY_LIMITED_INFORMATION | _SYNCHRONIZE
            if require_terminate:
                access |= _PROCESS_TERMINATE
            handle = _KERNEL32.OpenProcess(access, False, pid)
            if not handle:
                error_code = ctypes.get_last_error()
                error_type = (
                    ProcessIdentityAccessDenied
                    if error_code == _ERROR_ACCESS_DENIED
                    else SmokeFailure
                )
                raise error_type(
                    f"OpenProcess failed for pid {pid} with error {error_code}: "
                    f"{ctypes.FormatError(error_code).strip()}"
                )
            try:
                observed_pid = int(_KERNEL32.GetProcessId(handle))
                if observed_pid != pid:
                    raise SmokeFailure(
                        f"stable process handle pid mismatch: requested={pid}, observed={observed_pid}"
                    )
                creation = _FILETIME()
                exit_time = _FILETIME()
                kernel_time = _FILETIME()
                user_time = _FILETIME()
                if not _KERNEL32.GetProcessTimes(
                    handle,
                    ctypes.byref(creation),
                    ctypes.byref(exit_time),
                    ctypes.byref(kernel_time),
                    ctypes.byref(user_time),
                ):
                    error_code = ctypes.get_last_error()
                    raise SmokeFailure(
                        f"GetProcessTimes failed for pid {pid} with error {error_code}: "
                        f"{ctypes.FormatError(error_code).strip()}"
                    )
                creation_ticks = (
                    int(creation.dwHighDateTime) << 32
                ) | int(creation.dwLowDateTime)
                if creation_ticks <= 0:
                    raise SmokeFailure(
                        f"GetProcessTimes returned an invalid creation time for pid {pid}"
                    )
                image_capacity = wintypes.DWORD(32768)
                image_buffer = ctypes.create_unicode_buffer(image_capacity.value)
                if not _KERNEL32.QueryFullProcessImageNameW(
                    handle,
                    0,
                    image_buffer,
                    ctypes.byref(image_capacity),
                ):
                    error_code = ctypes.get_last_error()
                    raise SmokeFailure(
                        f"QueryFullProcessImageNameW failed for pid {pid} with error "
                        f"{error_code}: {ctypes.FormatError(error_code).strip()}"
                    )
                image_path = image_buffer.value
                if expected_image is not None and not _process_images_match(
                    expected_image,
                    image_path,
                ):
                    raise SmokeFailure(
                        f"stable process image mismatch for pid {pid}: "
                        f"observed={image_path!r}, expected={str(expected_image)!r}"
                    )
                creation_key = f"winfiletime:{creation_ticks}"
                identity = f"{pid}@{creation_key}"
                creation_unix_seconds = (
                    creation_ticks / _WINDOWS_TICKS_PER_SECOND
                    - _WINDOWS_EPOCH_OFFSET_SECONDS
                )
                return cls(
                    pid=pid,
                    identity=identity,
                    creation_time_key=creation_key,
                    creation_time_unix_seconds=creation_unix_seconds,
                    image_path=image_path,
                    native_handle=int(handle),
                    terminate_access=require_terminate,
                    backend="windows-process-handle",
                    creation_filetime_ticks=creation_ticks,
                )
            except Exception:
                _KERNEL32.CloseHandle(handle)
                raise

        if not hasattr(os, "pidfd_open"):
            raise SmokeFailure(
                "stable process identity requires pidfd_open on this non-Windows platform"
            )
        try:
            import psutil

            pidfd = os.pidfd_open(pid, 0)
            try:
                process = psutil.Process(pid)
                creation_time = float(process.create_time())
                image_path = str(process.exe())
                if expected_image is not None and not _process_images_match(
                    expected_image,
                    image_path,
                ):
                    raise SmokeFailure(
                        f"stable process image mismatch for pid {pid}: "
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
            raise SmokeFailure(
                f"failed to acquire stable pidfd identity for pid {pid}: {exc}"
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
            "ownership_source": self._ownership_source,
            "duplicated_from_launcher_handle": self._duplicated_from_launcher_handle,
            "pid_lookup_used": self._pid_lookup_used,
            "creation_filetime_ticks": self._creation_filetime_ticks,
            "creation_time_unix_ns": self._creation_time_unix_ns,
        }

    def creation_time_matches(self, create_time: float) -> bool:
        if os.name != "nt":
            return float(create_time) == self.creation_time_unix_seconds
        expected_ticks = round(
            (float(create_time) + _WINDOWS_EPOCH_OFFSET_SECONDS)
            * _WINDOWS_TICKS_PER_SECOND
        )
        observed_ticks = int(self.creation_time_key.split(":", 1)[1])
        return abs(expected_ticks - observed_ticks) <= 32

    def is_running(self) -> bool:
        if self._closed:
            raise SmokeFailure(
                f"stable process handle is closed for identity {self.identity}"
            )
        if os.name == "nt":
            wait_result = int(_KERNEL32.WaitForSingleObject(self._native_handle, 0))
            if wait_result == _WAIT_TIMEOUT:
                return True
            if wait_result == _WAIT_OBJECT_0:
                return False
            error_code = ctypes.get_last_error()
            raise SmokeFailure(
                f"WaitForSingleObject failed for {self.identity} with result "
                f"{wait_result} and error {error_code}: "
                f"{ctypes.FormatError(error_code).strip()}"
            )

        import select

        poller = select.poll()
        poller.register(self._native_handle, select.POLLIN)
        return not bool(poller.poll(0))

    def wait_for_exit(
        self,
        *,
        timeout: float,
        pump: Callable[[], Any] | None = None,
    ) -> dict[str, Any]:
        result: dict[str, Any] = {
            **self.metadata(),
            "same_native_handle": True,
            "exited": False,
            "passed": False,
        }
        if not math.isfinite(timeout) or timeout <= 0.0:
            result["error"] = f"invalid stable process wait timeout: {timeout!r}"
            return result
        deadline = time.monotonic() + timeout
        while True:
            try:
                if not self.is_running():
                    result.update({"exited": True, "running_after": False, "passed": True})
                    return result
            except Exception as exc:
                result["error"] = (
                    "stable handle wait failed closed: "
                    f"{type(exc).__name__}: {exc}"
                )
                return result
            if pump is not None:
                try:
                    pump()
                except Exception as exc:
                    result["error"] = (
                        "pump failed while waiting on the stable process handle: "
                        f"{type(exc).__name__}: {exc}"
                    )
                    return result
            remaining = deadline - time.monotonic()
            if remaining <= 0.0:
                result.update(
                    {
                        "timed_out": True,
                        "running_after": True,
                        "error": "timed out waiting on the held process handle",
                    }
                )
                return result
            time.sleep(min(0.02, remaining))
    def terminate(self, *, timeout: float) -> dict[str, Any]:
        result: dict[str, Any] = {
            **self.metadata(),
            "same_native_handle": True,
            "tree_cleanup_requested": False,
            "termination_requested": False,
            "passed": False,
        }
        if not math.isfinite(timeout) or timeout <= 0.0:
            result["error"] = f"invalid stable process termination timeout: {timeout!r}"
            return result
        try:
            running_before = self.is_running()
        except Exception as exc:
            result["error"] = (
                f"stable handle liveness check failed closed: {type(exc).__name__}: {exc}"
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
            result["error"] = "stable handle has no terminate access; refusing pid-based fallback"
            return result

        result["termination_requested"] = True
        if os.name == "nt":
            if not _KERNEL32.TerminateProcess(self._native_handle, 1):
                error_code = ctypes.get_last_error()
                result["terminate_error_code"] = error_code
                result["terminate_error"] = ctypes.FormatError(error_code).strip()
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
                    "TerminateProcess failed on the held process handle; "
                    "refusing any pid-based fallback"
                )
                return result
            timeout_ms = min(0xFFFFFFFE, max(1, math.ceil(timeout * 1000.0)))
            wait_result = int(
                _KERNEL32.WaitForSingleObject(self._native_handle, timeout_ms)
            )
            result["wait_result"] = wait_result
            if wait_result == _WAIT_OBJECT_0:
                result.update({"running_after": False, "passed": True})
                return result
            if wait_result == _WAIT_TIMEOUT:
                result.update(
                    {
                        "running_after": True,
                        "error": "timed out waiting on the held process handle",
                    }
                )
                return result
            error_code = ctypes.get_last_error()
            result.update(
                {
                    "running_after": None,
                    "error": (
                        f"WaitForSingleObject after TerminateProcess failed with "
                        f"{error_code}: {ctypes.FormatError(error_code).strip()}"
                    ),
                }
            )
            return result

        if not hasattr(signal, "pidfd_send_signal"):
            result["error"] = (
                "pidfd_send_signal is unavailable; refusing pid-based fallback"
            )
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
                "error": "timed out waiting on the held pidfd after SIGKILL",
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
                if not _KERNEL32.CloseHandle(self._native_handle):
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


def acquire_stable_process_identity(
    pid: int,
    *,
    expected_image: Path | None = None,
    require_terminate: bool = True,
) -> StableProcessIdentity:
    return StableProcessIdentity.acquire(
        pid,
        expected_image=expected_image,
        require_terminate=require_terminate,
    )


def finalize_process(process: subprocess.Popen[str], *, cwd: Path) -> dict[str, Any]:
    result: dict[str, Any] = {
        "pid": process.pid,
        "returncode_before": process.poll(),
        "stable_reference": (
            "subprocess.Popen-held Windows process handle"
            if os.name == "nt"
            else "subprocess child identity"
        ),
        "tree_cleanup_requested": False,
    }
    if process.poll() is None:
        result["terminate_called"] = True
        try:
            process.terminate()
        except OSError as exc:
            result["terminate_error"] = str(exc)
    try:
        process.wait(timeout=3.0)
    except subprocess.TimeoutExpired:
        result["kill_called"] = True
        try:
            process.kill()
        except OSError as exc:
            result["kill_error"] = str(exc)
        try:
            process.wait(timeout=2.0)
        except subprocess.TimeoutExpired:
            result["wait_error"] = "process did not exit after terminate/kill"
    result["returncode_after"] = process.poll()
    result["running_after"] = process.poll() is None
    result["passed"] = result["running_after"] is False
    return result
def load_renderdoc_module() -> Any:
    try:
        from rdc.discover import find_renderdoc
    except ImportError as exc:
        raise SmokeFailure("rdc-cli Python package is not available") from exc
    renderdoc_module = find_renderdoc()
    if renderdoc_module is None:
        raise SmokeFailure(
            "RenderDoc Python module was not found; run 'rdc doctor' and check RENDERDOC_PYTHON_PATH"
        )
    return renderdoc_module


def capture_notification_payload(new_capture: Any) -> dict[str, Any]:
    capture = {
        "captureId": int(new_capture.captureId),
        "path": str(new_capture.path),
        "frameNumber": int(new_capture.frameNumber),
        "byteSize": int(new_capture.byteSize),
        "api": str(new_capture.api),
        "local": bool(new_capture.local),
    }
    if capture["captureId"] < 0:
        raise SmokeFailure(f"RenderDoc returned an invalid capture notification: {capture!r}")
    return capture


class PersistentTargetControl:
    """One RenderDoc TargetControl connection kept alive for an entire case."""

    def __init__(self, ident: int, *, renderdoc_module: Any | None = None) -> None:
        self._ident = ident
        self._owner_thread_id = threading.get_ident()
        self._rd = renderdoc_module if renderdoc_module is not None else load_renderdoc_module()
        self._tc: Any | None = None
        self._pending_captures: list[dict[str, Any]] = []
        self._cached_messages: list[dict[str, Any]] = []
        self._capture_copied_count = 0
        self._capture_notification_sequence = 0
        self._seen_capture_ids: set[int] = set()
        self._batch_epoch = 0
        self._capture_batches: list[dict[str, Any]] = []
        self._active_batch: dict[str, Any] | None = None
        self._receive_count = 0
        self._message_counts: dict[int, int] = {}
        self._target_pid = 0
        self._target_name = ""
        self._api = ""

    def _assert_owner_thread(self) -> None:
        current_thread_id = threading.get_ident()
        if current_thread_id != self._owner_thread_id:
            raise SmokeFailure(
                "RenderDoc TargetControl must only be used by its owning thread: "
                f"owner={self._owner_thread_id}, current={current_thread_id}"
            )

    @property
    def metadata(self) -> dict[str, Any]:
        self._assert_owner_thread()
        get_version = getattr(self._rd, "GetVersionString", None)
        version = str(get_version()) if callable(get_version) else "unknown"
        return {
            "backend": "renderdoc-python-target-control",
            "ident": self._ident,
            "owner_thread_id": self._owner_thread_id,
            "connection_lifetime": "one persistent TargetControl connection per case",
            "persistent_connection_opened": self._tc is not None,
            "target_pid": self._target_pid,
            "target_name": self._target_name,
            "api": self._api,
            "renderdoc_version": version,
            "receive_count": self._receive_count,
            "message_counts": self._message_count_snapshot(),
            "cached_non_noop_messages": [dict(message) for message in self._cached_messages],
            "capture_batches": [self._batch_snapshot(batch) for batch in self._capture_batches],
        }

    @staticmethod
    def _batch_snapshot(batch: dict[str, Any]) -> dict[str, Any]:
        return json.loads(json.dumps(batch))

    def _message_count_snapshot(self) -> dict[str, int]:
        return {
            TARGET_CONTROL_MESSAGE_NAMES.get(message_type, str(message_type)): count
            for message_type, count in sorted(self._message_counts.items())
        }

    def _connected(self) -> bool:
        self._assert_owner_thread()
        if self._tc is None:
            return False
        try:
            return bool(self._tc.Connected())
        except Exception:
            return False

    def _require_connected(self) -> Any:
        if not self._connected():
            raise SmokeFailure(f"RenderDoc TargetControl ident={self._ident} is disconnected")
        return self._tc

    @staticmethod
    def _shutdown_target_control(target_control: Any) -> None:
        try:
            target_control.Shutdown()
        except Exception:
            pass

    def _receive_message(self) -> dict[str, Any]:
        target_control = self._require_connected()
        try:
            message = target_control.ReceiveMessage(None)
            message_type = int(message.type)
        except Exception as exc:
            raise SmokeFailure(
                f"RenderDoc TargetControl ident={self._ident} failed receiving a message: {exc}"
            ) from exc
        self._receive_count += 1
        self._message_counts[message_type] = self._message_counts.get(message_type, 0) + 1
        result: dict[str, Any] = {
            "type": message_type,
            "type_name": TARGET_CONTROL_MESSAGE_NAMES.get(message_type, f"type-{message_type}"),
        }
        if message_type == TARGET_CONTROL_NEW_CAPTURE:
            if message.newCapture is None:
                raise SmokeFailure("RenderDoc sent NewCapture without capture metadata")
            capture = capture_notification_payload(message.newCapture)
            self._capture_notification_sequence += 1
            capture["notification_sequence"] = self._capture_notification_sequence
            capture["received_utc"] = utc_now()
            capture["observed_batch_epoch"] = (
                int(self._active_batch["epoch"]) if self._active_batch is not None else None
            )
            self._seen_capture_ids.add(int(capture["captureId"]))
            self._pending_captures.append(capture)
            result["capture"] = capture
        elif message_type == TARGET_CONTROL_CAPTURE_COPIED:
            self._capture_copied_count += 1
        if message_type != TARGET_CONTROL_NOOP:
            self._cached_messages.append(dict(result))
        if message_type == TARGET_CONTROL_DISCONNECTED:
            raise SmokeFailure(f"RenderDoc TargetControl ident={self._ident} disconnected")
        return result

    def _drain_pending_messages(self, *, timeout: float) -> dict[str, Any]:
        deadline = time.monotonic() + timeout
        messages: list[dict[str, Any]] = []
        while time.monotonic() < deadline and len(messages) < 256:
            message = self._receive_message()
            messages.append(message)
            if message["type"] == TARGET_CONTROL_NOOP:
                return {
                    "drained": True,
                    "message_count": len(messages),
                    "message_types": [entry["type_name"] for entry in messages],
                }
            time.sleep(0.005)
        raise SmokeFailure(
            f"timed out draining RenderDoc TargetControl ident={self._ident} before trigger"
        )

    def connect(
        self,
        *,
        timeout: float,
        process_identity: Any,
    ) -> tuple[list[dict[str, Any]], dict[str, Any]]:
        self._assert_owner_thread()
        if self._tc is not None:
            raise SmokeFailure("RenderDoc TargetControl connect called more than once")
        deadline = time.monotonic() + timeout
        attempts = 0
        last_error = ""
        while time.monotonic() < deadline:
            if not process_identity.is_running():
                raise SmokeFailure(
                    f"target process {process_identity.identity} exited before TargetControl connected"
                )
            attempts += 1
            candidate = None
            try:
                candidate = self._rd.CreateTargetControl(
                    "", self._ident, "mgif-csm-shadow-motion-smoke", True
                )
                if candidate is None or not candidate.Connected():
                    last_error = "CreateTargetControl returned no connected target"
                else:
                    observed_pid = int(candidate.GetPID())
                    if observed_pid <= 0:
                        raise SmokeFailure(
                            f"TargetControl ident={self._ident} returned invalid pid={observed_pid}; "
                            f"expected exact launch-owned identity {process_identity.identity}"
                        )
                    if observed_pid != process_identity.pid:
                        raise SmokeFailure(
                            f"TargetControl ident={self._ident} reports pid={observed_pid}, "
                            f"expected stable identity {process_identity.identity}"
                        )
                    if not process_identity.is_running():
                        raise SmokeFailure(
                            "target process exited while TargetControl connection was being verified: "
                            f"{process_identity.identity}"
                        )
                    self._tc = candidate
                    self._target_pid = observed_pid
                    self._target_name = str(candidate.GetTarget())
                    self._api = str(candidate.GetAPI())
                    drain_timeout = min(1.0, max(0.1, deadline - time.monotonic()))
                    drain = self._drain_pending_messages(timeout=drain_timeout)
                    initial_captures = list(self._pending_captures)
                    return initial_captures, {
                        "attempts": attempts,
                        "drain": drain,
                        "target_pid": observed_pid,
                        "target_name": self._target_name,
                        "api": self._api,
                    }
            except SmokeFailure:
                if self._tc is candidate:
                    self._tc = None
                if candidate is not None:
                    self._shutdown_target_control(candidate)
                raise
            except Exception as exc:
                last_error = str(exc)
            if candidate is not None:
                self._shutdown_target_control(candidate)
            time.sleep(min(0.05, max(0.0, deadline - time.monotonic())))
        raise SmokeFailure(
            f"timed out connecting RenderDoc TargetControl ident={self._ident}: {last_error}"
        )

    def trigger(
        self,
        *,
        num_frames: int,
        expected_boundaries: Iterable[str],
        timeout: float,
    ) -> dict[str, Any]:
        self._assert_owner_thread()
        if num_frames <= 0:
            raise SmokeFailure(f"RenderDoc TriggerCapture frame count must be positive: {num_frames}")
        boundaries = [str(boundary) for boundary in expected_boundaries]
        if len(boundaries) != num_frames or any(not boundary for boundary in boundaries):
            raise SmokeFailure(
                "RenderDoc capture batch boundaries must match TriggerCapture count: "
                f"num_frames={num_frames}, boundaries={boundaries!r}"
            )
        if (
            self._active_batch is not None
            and self._active_batch.get("status") not in ("completed", "trigger-failed")
        ):
            raise SmokeFailure(
                "cannot arm a new RenderDoc capture batch before the previous batch completes: "
                f"{self._batch_snapshot(self._active_batch)!r}"
            )
        drain = self._drain_pending_messages(timeout=min(1.0, max(0.1, timeout * 0.25)))
        baseline_captures = [dict(capture) for capture in self._pending_captures]
        self._pending_captures.clear()
        self._batch_epoch += 1
        batch: dict[str, Any] = {
            "epoch": self._batch_epoch,
            "status": "arming",
            "expected_boundaries": boundaries,
            "expected_count": num_frames,
            "next_ordinal": 1,
            "baseline_notification_sequence": self._capture_notification_sequence,
            "baseline_capture_ids": sorted(self._seen_capture_ids),
            "pre_trigger_captures": baseline_captures,
            "filtered_notifications": [],
            "candidate_count": 0,
            "accepted_candidates": [],
            "rejected_candidates": [],
            "trigger_started_utc": utc_now(),
        }
        self._capture_batches.append(batch)
        self._active_batch = batch
        target_control = self._require_connected()
        started = time.monotonic()
        try:
            target_control.TriggerCapture(num_frames)
        except Exception as exc:
            batch["status"] = "trigger-failed"
            batch["trigger_error"] = str(exc)
            raise SmokeFailure(f"RenderDoc TriggerCapture failed: {exc}") from exc
        elapsed = time.monotonic() - started
        if elapsed > timeout:
            batch["status"] = "trigger-failed"
            raise SmokeFailure(
                f"RenderDoc TriggerCapture exceeded timeout ({elapsed:.3f}s > {timeout:.3f}s)"
            )
        batch.update(
            {
                "status": "armed",
                "trigger_completed_utc": utc_now(),
                "trigger_elapsed_seconds": elapsed,
                "pre_trigger_drain": drain,
            }
        )
        return {
            "backend": "renderdoc.TargetControl.TriggerCapture",
            "ident": self._ident,
            "num_frames": num_frames,
            "elapsed_seconds": elapsed,
            "pre_trigger_drain": drain,
            "persistent_connection": True,
            "batch": self._batch_snapshot(batch),
            "epoch": int(batch["epoch"]),
        }

    def pump(self, *, max_messages: int = 64) -> dict[str, Any]:
        self._assert_owner_thread()
        if max_messages <= 0:
            raise SmokeFailure(f"TargetControl pump max_messages must be positive: {max_messages}")
        receive_count_before = self._receive_count
        pending_before = len(self._pending_captures)
        message_types: list[str] = []
        reached_noop = False
        for _ in range(max_messages):
            message = self._receive_message()
            message_types.append(str(message["type_name"]))
            if message["type"] == TARGET_CONTROL_NOOP:
                reached_noop = True
                break
        return {
            "backend": "renderdoc.TargetControl.ReceiveMessage",
            "messages_received": self._receive_count - receive_count_before,
            "message_types": message_types,
            "reached_noop": reached_noop,
            "captures_buffered": len(self._pending_captures) - pending_before,
            "pending_capture_count": len(self._pending_captures),
            "cached_non_noop_message_count": len(self._cached_messages),
            "persistent_connection": True,
        }

    def _capture_batch_by_epoch(self, epoch: int) -> dict[str, Any]:
        self._assert_owner_thread()
        for batch in self._capture_batches:
            if int(batch.get("epoch", -1)) == epoch:
                return batch
        raise SmokeFailure(f"RenderDoc capture batch epoch {epoch} does not exist")

    def _require_active_capture_batch(self, epoch: int) -> dict[str, Any]:
        batch = self._capture_batch_by_epoch(epoch)
        if self._active_batch is not batch:
            raise SmokeFailure(f"RenderDoc capture batch epoch {epoch} is not active")
        return batch

    def _take_batch_candidate(self, batch: dict[str, Any]) -> dict[str, Any] | None:
        baseline_sequence = int(batch["baseline_notification_sequence"])
        baseline_capture_ids = {
            int(capture_id) for capture_id in batch.get("baseline_capture_ids", [])
        }
        disposed_capture_ids = {
            int(record["capture"]["captureId"])
            for key in ("accepted_candidates", "rejected_candidates")
            for record in batch.get(key, [])
        }
        while self._pending_captures:
            capture = self._pending_captures.pop(0)
            sequence = int(capture.get("notification_sequence", -1))
            capture_id = int(capture["captureId"])
            if sequence <= baseline_sequence or capture_id in baseline_capture_ids:
                reason = "notification belongs to the pre-trigger baseline"
            elif capture_id in disposed_capture_ids:
                reason = "duplicate notification for an already-disposed capture id"
            else:
                return capture
            batch["filtered_notifications"].append(
                {
                    "capture": dict(capture),
                    "reason": reason,
                }
            )
        return None

    def wait_for_batch_candidate(
        self,
        *,
        batch_epoch: int,
        expected_boundary: str,
        expected_ordinal: int,
        timeout: float,
        process_identity: Any,
    ) -> tuple[dict[str, Any], dict[str, Any]]:
        self._assert_owner_thread()
        batch = self._require_active_capture_batch(batch_epoch)
        next_ordinal = int(batch["next_ordinal"])
        if expected_ordinal != next_ordinal:
            raise SmokeFailure(
                f"capture batch epoch {batch_epoch} ordinal mismatch: "
                f"requested={expected_ordinal}, next={next_ordinal}"
            )
        expected_boundaries = list(batch["expected_boundaries"])
        if expected_ordinal < 1 or expected_ordinal > len(expected_boundaries):
            raise SmokeFailure(
                f"capture batch epoch {batch_epoch} ordinal is out of range: {expected_ordinal}"
            )
        batch_boundary = str(expected_boundaries[expected_ordinal - 1])
        if expected_boundary != batch_boundary:
            raise SmokeFailure(
                f"capture batch epoch {batch_epoch} boundary mismatch at ordinal "
                f"{expected_ordinal}: requested={expected_boundary!r}, "
                f"expected={batch_boundary!r}"
            )
        deadline = time.monotonic() + timeout
        receive_count_before = self._receive_count
        capture = self._take_batch_candidate(batch)
        while capture is None and time.monotonic() < deadline:
            message = self._receive_message()
            capture = self._take_batch_candidate(batch)
            if capture is None and message["type"] == TARGET_CONTROL_NOOP:
                if not process_identity.is_running():
                    raise SmokeFailure(
                        f"target process {process_identity.identity} exited before NewCapture arrived"
                    )
                time.sleep(0.01)
        if capture is None:
            raise SmokeFailure(
                f"timed out waiting for RenderDoc NewCapture for batch epoch "
                f"{batch_epoch} ordinal {expected_ordinal}"
            )
        batch["candidate_count"] = int(batch["candidate_count"]) + 1
        candidate = dict(capture)
        candidate.update(
            {
                "batch_epoch": batch_epoch,
                "expected_boundary": expected_boundary,
                "expected_ordinal": expected_ordinal,
                "candidate_index": int(batch["candidate_count"]),
            }
        )
        return candidate, {
            "backend": "renderdoc.TargetControl.ReceiveMessage",
            "messages_received": self._receive_count - receive_count_before,
            "persistent_connection": True,
            "message_counts": self._message_count_snapshot(),
            "batch_epoch": batch_epoch,
            "expected_boundary": expected_boundary,
            "expected_ordinal": expected_ordinal,
            "candidate_index": int(batch["candidate_count"]),
            "notification_sequence": int(capture["notification_sequence"]),
        }

    def reject_batch_candidate(
        self,
        *,
        batch_epoch: int,
        expected_boundary: str,
        expected_ordinal: int,
        capture: dict[str, Any],
        reason: str,
        evidence: dict[str, Any],
    ) -> dict[str, Any]:
        batch = self._require_active_capture_batch(batch_epoch)
        if int(batch["next_ordinal"]) != expected_ordinal:
            raise SmokeFailure(
                f"cannot reject capture for stale batch ordinal {expected_ordinal}"
            )
        record = {
            "expected_boundary": expected_boundary,
            "expected_ordinal": expected_ordinal,
            "capture": dict(capture),
            "reason": reason,
            "evidence": evidence,
            "rejected_utc": utc_now(),
        }
        batch["rejected_candidates"].append(record)
        return self._batch_snapshot(batch)

    def accept_batch_candidate(
        self,
        *,
        batch_epoch: int,
        expected_boundary: str,
        expected_ordinal: int,
        capture: dict[str, Any],
        evidence: dict[str, Any],
    ) -> dict[str, Any]:
        batch = self._require_active_capture_batch(batch_epoch)
        if int(batch["next_ordinal"]) != expected_ordinal:
            raise SmokeFailure(
                f"cannot accept capture for stale batch ordinal {expected_ordinal}"
            )
        expected = str(batch["expected_boundaries"][expected_ordinal - 1])
        if expected != expected_boundary:
            raise SmokeFailure(
                f"cannot accept {expected_boundary!r} for batch boundary {expected!r}"
            )
        record = {
            "expected_boundary": expected_boundary,
            "expected_ordinal": expected_ordinal,
            "capture": dict(capture),
            "evidence": evidence,
            "accepted_utc": utc_now(),
        }
        batch["accepted_candidates"].append(record)
        batch["next_ordinal"] = expected_ordinal + 1
        if int(batch["next_ordinal"]) > int(batch["expected_count"]):
            batch["status"] = "completed"
            batch["completed_utc"] = utc_now()
        return self._batch_snapshot(batch)

    def capture_batch_snapshot(self, batch_epoch: int) -> dict[str, Any]:
        return self._batch_snapshot(self._capture_batch_by_epoch(batch_epoch))
    def copy_capture(self, capture_id: int, destination: Path, *, timeout: float) -> dict[str, Any]:
        self._assert_owner_thread()
        destination.parent.mkdir(parents=True, exist_ok=True)
        target_control = self._require_connected()
        acknowledgement_before = self._capture_copied_count
        started = time.monotonic()
        try:
            target_control.CopyCapture(capture_id, str(destination))
        except Exception as exc:
            raise SmokeFailure(f"RenderDoc CopyCapture failed for capture {capture_id}: {exc}") from exc
        deadline = started + timeout
        receive_count_before = self._receive_count
        while self._capture_copied_count == acknowledgement_before and time.monotonic() < deadline:
            message = self._receive_message()
            if message["type"] == TARGET_CONTROL_NOOP:
                time.sleep(0.01)
        if self._capture_copied_count == acknowledgement_before:
            raise SmokeFailure(f"timed out waiting for CaptureCopied for capture {capture_id}")
        return {
            "backend": "renderdoc.TargetControl.CopyCapture",
            "capture_id": capture_id,
            "destination": str(destination),
            "messages_received": self._receive_count - receive_count_before,
            "elapsed_seconds": time.monotonic() - started,
            "persistent_connection": True,
        }

    def shutdown(self) -> dict[str, Any]:
        self._assert_owner_thread()
        target_control = self._tc
        result: dict[str, Any] = {
            "opened": target_control is not None,
            "connected_before": self._connected(),
            "shutdown_called": False,
            "closed": True,
            "message_counts": self._message_count_snapshot(),
            "cached_non_noop_messages": [dict(message) for message in self._cached_messages],
        }
        self._tc = None
        if target_control is not None:
            try:
                target_control.Shutdown()
                result["shutdown_called"] = True
                try:
                    result["connected_after"] = bool(target_control.Connected())
                except Exception:
                    result["connected_after"] = False
                result["closed"] = not result["connected_after"]
            except Exception as exc:
                result["closed"] = False
                result["shutdown_error"] = str(exc)
        return result


def wait_for_ready_marker(
    path: Path,
    *,
    timeout: float,
    process_identity: Any,
    unexpected_ready_paths: dict[str, Path] | None = None,
    pump: Callable[[], Any] | None = None,
) -> dict[str, Any]:
    deadline = time.monotonic() + timeout
    last_error = ""
    unexpected_ready_paths = unexpected_ready_paths or {}
    while time.monotonic() < deadline:
        if path.is_file():
            try:
                payload = json.loads(path.read_text(encoding="utf-8"))
            except (OSError, json.JSONDecodeError) as exc:
                last_error = str(exc)
            else:
                if isinstance(payload, dict):
                    return payload
                last_error = f"ready marker is not an object: {payload!r}"
        for unexpected_marker, unexpected_path in unexpected_ready_paths.items():
            if unexpected_path.is_file():
                required_marker = path.name.removesuffix(".ready.json")
                raise SmokeFailure(
                    f"Demo published {unexpected_marker!r} before required {required_marker!r}; "
                    f"{APP_ARM_PROTOCOL_REQUIREMENT}"
                )
        if pump is not None:
            pump()
        if not process_identity.is_running():
            raise SmokeFailure(
                f"target process {process_identity.identity} exited before ready marker "
                f"{path}: {last_error}"
            )
        time.sleep(0.02)
    raise SmokeFailure(f"timed out waiting for ready marker {path}: {last_error}")


def write_continue_marker(path: Path, *, marker: str, ident: int, action: str) -> None:
    temporary = path.with_name(f".{path.name}.{os.getpid()}.tmp")
    payload = {
        "protocol": PROTOCOL,
        "marker": marker,
        "ident": ident,
        "release_action": action,
        "released_utc": utc_now(),
    }
    with temporary.open("w", encoding="utf-8", newline="\n") as handle:
        json.dump(payload, handle, sort_keys=True)
        handle.write("\n")
        handle.flush()
        os.fsync(handle.fileno())
    os.replace(temporary, path)


def replay_action_name(action: Any, structured_file: Any) -> str:
    custom_name = str(getattr(action, "customName", "") or "")
    if custom_name:
        return custom_name
    try:
        return str(action.GetName(structured_file))
    except Exception:
        return f"EID {int(getattr(action, 'eventId', 0))}"


def walk_replay_actions(
    actions: Iterable[Any],
    structured_file: Any,
    ancestors: tuple[str, ...] = (),
    path: tuple[int, ...] = (),
) -> Iterable[tuple[Any, str, tuple[str, ...], tuple[int, ...]]]:
    for index, action in enumerate(actions):
        name = replay_action_name(action, structured_file)
        current_path = path + (index,)
        yield action, name, ancestors, current_path
        yield from walk_replay_actions(
            getattr(action, "children", []),
            structured_file,
            ancestors + (name,),
            current_path,
        )


def collect_automation_frame_markers(
    actions: Iterable[Any],
    structured_file: Any,
) -> list[dict[str, Any]]:
    markers: list[dict[str, Any]] = []
    for action, name, ancestors, path in walk_replay_actions(actions, structured_file):
        stripped_name = name.strip()
        if not stripped_name.startswith("CSM_AUTOMATION_FRAME"):
            continue
        marker: dict[str, Any] = {
            "name": stripped_name,
            "path": list(path),
            "ancestors": list(ancestors),
            "event_id": int(getattr(action, "eventId", 0)),
            "nested_in_gpu_driven_csm_shadow": any(
                ancestor.strip() == "GPUDrivenCSMShadow" for ancestor in ancestors
            ),
        }
        match = AUTOMATION_FRAME_MARKER_RE.fullmatch(stripped_name)
        if match is None:
            marker["parse_error"] = "marker does not match the strict CSM_AUTOMATION_FRAME format"
        else:
            marker.update(
                {
                    "mode": match.group("mode"),
                    "boundary": match.group("boundary"),
                    "frame": int(match.group("frame")),
                }
            )
        markers.append(marker)
    return markers


def validate_automation_frame_markers(
    markers: list[dict[str, Any]],
    *,
    mode: str,
    boundary: str,
    frame: int,
) -> dict[str, Any]:
    expected_name = f"CSM_AUTOMATION_FRAME mode={mode} boundary={boundary} frame={frame}"
    expected = {
        "name": expected_name,
        "mode": mode,
        "boundary": boundary,
        "frame": frame,
        "nested_in_gpu_driven_csm_shadow": True,
    }
    errors: list[str] = []
    if len(markers) != 1:
        errors.append(f"expected exactly one CSM_AUTOMATION_FRAME marker, found {len(markers)}")
    marker = markers[0] if len(markers) == 1 else None
    if marker is not None:
        if marker.get("parse_error"):
            errors.append(str(marker["parse_error"]))
        for key, expected_value in expected.items():
            if marker.get(key) != expected_value:
                errors.append(
                    f"RDC marker {key}={marker.get(key)!r}, expected {expected_value!r}"
                )
    return {
        "passed": not errors,
        "expected": expected,
        "observed": markers,
        "errors": errors,
    }


def replay_action_is_draw(action: Any, name: str) -> bool:
    rd_api = globals().get("rd")
    draw_flag = getattr(getattr(rd_api, "ActionFlags", object()), "Drawcall", None)
    if draw_flag is not None:
        try:
            if bool(action.flags & draw_flag):
                return True
        except Exception:
            pass
    if getattr(action, "is_draw", False) is True:
        return True
    lower = name.lower()
    return "draw" in lower and "begin" not in lower and "end" not in lower


def replay_resource_name(replay_state: Any, resource_id: int) -> str:
    return str(getattr(replay_state, "res_names", {}).get(int(resource_id), "") or "")


def replay_resource_record(replay_state: Any, resource_id: int) -> dict[str, Any]:
    texture = getattr(replay_state, "tex_map", {}).get(int(resource_id))
    format_name = ""
    if texture is not None:
        fmt = getattr(texture, "format", None)
        if fmt is not None:
            try:
                format_name = str(fmt.Name())
            except Exception:
                format_name = str(fmt)
    return {
        "resource_id": int(resource_id),
        "name": replay_resource_name(replay_state, resource_id),
        "format": format_name,
        "is_texture": texture is not None,
    }


def replay_is_rgba16f(format_name: str) -> bool:
    compact = re.sub(r"[^A-Z0-9]+", "", str(format_name).upper())
    return "R16G16B16A16" in compact and (
        "SFLOAT" in compact or "FLOAT" in compact
    )


def replay_descriptor_binding_name(
    reflections: dict[int, list[Any]],
    access: Any,
) -> str:
    resources = reflections.get(int(access.stage), [])
    index = int(access.index)
    if 0 <= index < len(resources):
        return str(getattr(resources[index], "name", "") or "")
    return ""


def replay_history_valid_value(
    replay_controller: Any,
    pipe_state: Any,
    fragment_stage: Any,
) -> dict[str, Any]:
    evidence: dict[str, Any] = {
        "readable": False,
        "value": None,
        "paths": [],
        "errors": [],
    }
    reflection = pipe_state.GetShaderReflection(fragment_stage)
    shader_id = pipe_state.GetShader(fragment_stage)
    if reflection is None or int(shader_id) == 0:
        evidence["errors"].append("fragment reflection or shader is unavailable")
        return evidence
    try:
        from rdc.handlers._helpers import get_pipeline_for_stage

        pipeline = get_pipeline_for_stage(pipe_state, fragment_stage)
    except Exception as exc:
        evidence["errors"].append(
            f"pipeline reflection helper unavailable: {type(exc).__name__}: {exc}"
        )
        return evidence

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
            variables = replay_controller.GetCBufferVariableContents(
                pipeline,
                shader_id,
                fragment_stage,
                pipe_state.GetShaderEntryPoint(fragment_stage),
                block_index,
                descriptor.resource,
                descriptor.byteOffset,
                descriptor.byteSize,
            )
        except Exception as exc:
            evidence["errors"].append(
                f"constant block {block_index} unreadable: {type(exc).__name__}: {exc}"
            )
            continue
        block_name = str(getattr(block_definition, "name", "") or "")
        for variable in variables:
            for path, values in flatten(variable, block_name):
                if path.rsplit(".", 1)[-1].casefold() == "params5" and len(values) >= 2:
                    candidates.append((path, float(values[1])))
    evidence["paths"] = [path for path, _ in candidates]
    if len(candidates) == 1:
        evidence["readable"] = True
        evidence["value"] = candidates[0][1]
    elif len(candidates) > 1:
        evidence["errors"].append(
            f"historyValid params5.y is ambiguous across {len(candidates)} paths"
        )
    return evidence


def validate_taa_resolve_capture(
    actions: Iterable[Any],
    structured_file: Any,
    replay_controller: Any,
    replay_state: Any,
    *,
    boundary: str,
    required: bool,
) -> dict[str, Any]:
    result: dict[str, Any] = {
        "required": required,
        "passed": not required,
        "inner_marker_name": "GPUDrivenTAAResolve",
        "outer_marker_name_rejected": "GPUDrivenTAAResolvePass",
        "inner_markers": [],
        "outer_marker_count": 0,
        "draw": None,
        "pipeline": None,
        "color_output": None,
        "descriptor_bindings": {},
        "history_valid": None,
        "errors": [],
    }
    if not required:
        return result
    marker_records: list[tuple[Any, str, tuple[str, ...], tuple[int, ...]]] = []
    for action, name, ancestors, path in walk_replay_actions(
        actions,
        structured_file,
    ):
        stripped = name.strip()
        if stripped == "GPUDrivenTAAResolvePass":
            result["outer_marker_count"] += 1
        if stripped == "GPUDrivenTAAResolve":
            marker_records.append((action, stripped, ancestors, path))
            result["inner_markers"].append(
                {
                    "event_id": int(getattr(action, "eventId", 0)),
                    "path": list(path),
                    "ancestors": list(ancestors),
                }
            )
    if len(marker_records) != 1:
        result["errors"].append(
            "expected exactly one inner GPUDrivenTAAResolve marker, found "
            f"{len(marker_records)}; GPUDrivenTAAResolvePass is not accepted"
        )
        return result

    marker_action = marker_records[0][0]
    draw_records: list[dict[str, Any]] = []
    for action, name, ancestors, path in walk_replay_actions(
        getattr(marker_action, "children", []),
        structured_file,
        ancestors=("GPUDrivenTAAResolve",),
    ):
        if not replay_action_is_draw(action, name):
            continue
        vertex_count = int(getattr(action, "numIndices", 0) or 0)
        instance_count_raw = int(getattr(action, "numInstances", 0) or 0)
        draw_records.append(
            {
                "action": action,
                "name": name,
                "event_id": int(getattr(action, "eventId", 0)),
                "vertex_count": vertex_count,
                "instance_count_raw": instance_count_raw,
                "instance_count": instance_count_raw,
                "path": list(path),
                "ancestors": list(ancestors),
            }
        )
    matching_draws = [
        row
        for row in draw_records
        if row["vertex_count"] == 3 and row["instance_count"] == 1
    ]
    if len(matching_draws) != 1:
        result["errors"].append(
            "inner GPUDrivenTAAResolve must contain exactly one Draw(3,1); "
            f"observed={len(matching_draws)} matching of {len(draw_records)} draw(s)"
        )
        return result
    draw = matching_draws[0]
    draw_action = draw.pop("action")
    result["draw"] = draw
    draw_eid = int(draw["event_id"])
    if draw_eid <= 0:
        result["errors"].append("TAA resolve Draw(3,1) has no positive event ID")
        return result

    rd_api = globals().get("rd")
    fragment_stage = getattr(getattr(rd_api, "ShaderStage", object()), "Fragment", 4)
    try:
        replay_controller.SetFrameEvent(draw_eid, True)
        pipe_state = replay_controller.GetPipelineState()
        fragment_entry = str(pipe_state.GetShaderEntryPoint(fragment_stage) or "")
        fragment_shader = int(pipe_state.GetShader(fragment_stage))
        graphics_pipeline = int(pipe_state.GetGraphicsPipelineObject())
    except Exception as exc:
        result["errors"].append(
            f"TAA resolve pipeline snapshot failed: {type(exc).__name__}: {exc}"
        )
        return result
    result["pipeline"] = {
        "snapshot_eid": draw_eid,
        "draw_eid_match": True,
        "fragment_entry": fragment_entry,
        "fragment_shader": fragment_shader,
        "graphics_pipeline": graphics_pipeline,
    }
    if fragment_entry != "fragmentTAAResolveMain":
        result["errors"].append(
            f"TAA fragment entry is {fragment_entry!r}, expected 'fragmentTAAResolveMain'"
        )
    if fragment_shader == 0 or graphics_pipeline == 0:
        result["errors"].append(
            "TAA resolve draw has no bound fragment shader or graphics pipeline"
        )

    output_rows: list[dict[str, Any]] = []
    try:
        for target_index, target in enumerate(pipe_state.GetOutputTargets()):
            resource_id = int(getattr(target, "resource", 0))
            if resource_id == 0:
                continue
            row = replay_resource_record(replay_state, resource_id)
            row["target_index"] = target_index
            output_rows.append(row)
    except Exception as exc:
        result["errors"].append(
            f"TAA color outputs could not be inspected: {type(exc).__name__}: {exc}"
        )
    history_outputs = [
        row
        for row in output_rows
        if re.fullmatch(r"GPUDrivenSceneColorHistory([01])", row["name"])
        and replay_is_rgba16f(row["format"])
    ]
    if len(history_outputs) != 1:
        result["errors"].append(
            "TAA resolve Draw(3,1) must bind exactly one RGBA16F "
            "GPUDrivenSceneColorHistory0/1 color attachment"
        )
        history_write = None
    else:
        history_write = history_outputs[0]
        history_write["physical_index"] = int(history_write["name"][-1])
        result["color_output"] = history_write

    reflections: dict[int, list[Any]] = {}
    reflection = pipe_state.GetShaderReflection(fragment_stage)
    if reflection is not None:
        reflections[int(fragment_stage)] = list(
            getattr(reflection, "readOnlyResources", [])
        )
    descriptor_rows: list[dict[str, Any]] = []
    try:
        for used in pipe_state.GetAllUsedDescriptors(True):
            access = used.access
            if int(access.stage) != int(fragment_stage):
                continue
            resource_id = int(getattr(used.descriptor, "resource", 0))
            if resource_id == 0:
                continue
            row = replay_resource_record(replay_state, resource_id)
            row.update(
                {
                    "array_element": int(getattr(access, "arrayElement", -1)),
                    "reflection_index": int(getattr(access, "index", -1)),
                    "binding": replay_descriptor_binding_name(reflections, access),
                }
            )
            descriptor_rows.append(row)
    except Exception as exc:
        result["errors"].append(
            f"TAA descriptors could not be inspected: {type(exc).__name__}: {exc}"
        )
    expected_bindings = {
        4: ("scene_color_hdr", re.compile(r"GPUDrivenSceneColorHDR")),
        7: ("velocity", re.compile(r"GPUDrivenVelocity")),
        8: ("history_read", re.compile(r"GPUDrivenSceneColorHistory([01])")),
    }
    selected_bindings: dict[str, dict[str, Any]] = {}
    for array_element, (role, name_pattern) in expected_bindings.items():
        candidates = [
            row
            for row in descriptor_rows
            if row["array_element"] == array_element
            and name_pattern.fullmatch(row["name"])
        ]
        if len(candidates) != 1:
            result["errors"].append(
                f"TAA descriptor array element {array_element} ({role}) matched "
                f"{len(candidates)} expected resources"
            )
            continue
        selected_bindings[role] = candidates[0]
    result["descriptor_bindings"] = selected_bindings

    history_read = selected_bindings.get("history_read")
    if history_read is not None:
        history_read["physical_index"] = int(history_read["name"][-1])
    if history_write is not None and history_read is not None:
        if history_read["resource_id"] == history_write["resource_id"]:
            result["errors"].append("TAA historyRead and historyWrite are the same resource")
        if history_read["physical_index"] == history_write["physical_index"]:
            result["errors"].append(
                "TAA historyRead and historyWrite have the same physical ping-pong index"
            )

    history_valid = replay_history_valid_value(
        replay_controller,
        pipe_state,
        fragment_stage,
    )
    result["history_valid"] = history_valid
    if (
        boundary in ("first-still", "settled", "control-still")
        and history_valid.get("readable") is True
        and abs(float(history_valid.get("value", 0.0)) - 1.0) > 1.0e-6
    ):
        result["errors"].append(
            f"stable boundary {boundary} has readable historyValid="
            f"{history_valid.get('value')!r}, expected 1"
        )

    result["passed"] = not result["errors"]
    return result

def rdc_marker_replay_main() -> dict[str, Any]:
    try:
        script_args = globals().get("args", {})
        mode = str(script_args["mode"])
        boundary = str(script_args["boundary"])
        frame = int(script_args["frame"])
        render_mode = str(script_args.get("render_mode", "no-post"))
        replay_controller = globals()["controller"]
        replay_state = globals()["state"]
        root_actions = replay_controller.GetRootActions()
        structured_file = getattr(replay_state, "structured_file", None)
        markers = collect_automation_frame_markers(
            root_actions,
            structured_file,
        )
        validation = validate_automation_frame_markers(
            markers,
            mode=mode,
            boundary=boundary,
            frame=frame,
        )
        taa_validation = validate_taa_resolve_capture(
            root_actions,
            structured_file,
            replay_controller,
            replay_state,
            boundary=boundary,
            required=render_mode == "taa-on",
        )
        validation["render_mode"] = render_mode
        validation["taa_resolve"] = taa_validation
        if taa_validation.get("passed") is not True:
            validation["errors"].extend(
                f"TAA resolve validation: {error}"
                for error in taa_validation.get("errors", [])
            )
        validation["passed"] = not validation["errors"]
        validation["capture"] = str(getattr(replay_state, "capture", ""))
        return validation
    except Exception as exc:
        return {
            "passed": False,
            "expected": None,
            "observed": [],
            "errors": [f"marker replay script failed: {type(exc).__name__}: {exc}"],
        }


def command_result_record(
    command: list[str], completed: subprocess.CompletedProcess[str]
) -> dict[str, Any]:
    return {
        "command": command,
        "command_text": command_text(command),
        "returncode": completed.returncode,
        "stdout": completed.stdout.strip(),
        "stderr": completed.stderr.strip(),
    }


def verify_capture_marker_replay(
    rdc: str,
    capture: Path,
    *,
    mode: str,
    render_mode: str,
    boundary: str,
    frame: int,
    cwd: Path,
    timeout: float,
    pump: Callable[[], Any],
    command_runner: Callable[..., subprocess.CompletedProcess[str]] = run_command_pumped,
    resource_snapshotter: Callable[[], dict[str, Any]] | None = None,
    session_allocator: Callable[[], tuple[str, Path]] | None = None,
    process_identity_factory: Callable[..., Any] | None = None,
    direct_shutdown_runner: Callable[..., dict[str, Any]] | None = None,
) -> dict[str, Any]:
    capture = capture.resolve()
    if not math.isfinite(timeout) or timeout <= 0.0:
        raise SmokeFailure(f"invalid replay timeout: {timeout!r}")
    if resource_snapshotter is None:
        resource_snapshotter = snapshot_rdc_resources
    if session_allocator is None:
        session_allocator = allocate_replay_session_name
    if process_identity_factory is None:
        process_identity_factory = acquire_stable_process_identity
    if direct_shutdown_runner is None:
        direct_shutdown_runner = shutdown_owned_rdc_session_direct
    session, state_path = session_allocator()
    base = [rdc, "--session", session]
    operation_started = time.monotonic()
    overall_deadline = operation_started + timeout
    cleanup_reserve = min(15.0, max(0.25, timeout * 0.25))
    validation_deadline = max(operation_started + min(0.05, timeout * 0.25), overall_deadline - cleanup_reserve)

    def remaining(deadline: float, stage: str) -> float:
        seconds = deadline - time.monotonic()
        if seconds <= 0.0:
            raise SmokeFailure(
                f"RDC marker replay total timeout expired before {stage}: {timeout:.3f}s"
            )
        return max(0.01, seconds)

    def take_resource_snapshot(stage: str) -> dict[str, Any]:
        try:
            snapshot = resource_snapshotter()
        except Exception as exc:
            return {
                "captured_utc": utc_now(),
                "sessions": {},
                "daemons": {},
                "errors": [f"{stage} resource snapshot raised: {type(exc).__name__}: {exc}"],
                "available": False,
            }
        if not isinstance(snapshot, dict):
            return {
                "sessions": {},
                "daemons": {},
                "errors": [f"{stage} snapshot is not an object"],
                "available": False,
            }
        return snapshot

    resources_before = take_resource_snapshot("before-open")
    state_before = rdc_session_state_record(state_path)
    result: dict[str, Any] = {
        "capture": str(capture),
        "session": session,
        "session_state_path": str(state_path.resolve()),
        "expected": {
            "mode": mode,
            "render_mode": render_mode,
            "boundary": boundary,
            "frame": frame,
        },
        "total_timeout_seconds": timeout,
        "validation": None,
        "errors": [],
        "passed": False,
    }
    errors: list[str] = []
    resources_after_open: dict[str, Any] | None = None
    state_after_open: dict[str, Any] | None = None
    daemon_process_identity: Any | None = None
    ownership: dict[str, Any] | None = None
    cleanup: dict[str, Any] = {
        "attempted": True,
        "closed": False,
        "passed": False,
        "shutdown_requested": True,
        "session": session,
        "state_file": {
            "path": str(state_path.resolve()),
            "before": state_before,
        },
        "errors": [],
    }
    try:
        if state_before.get("exists"):
            raise SmokeFailure(f"allocated replay session state already exists: {state_path}")
        if not capture.is_file() or capture.stat().st_size <= 0:
            raise SmokeFailure(f"cannot replay missing or empty capture: {capture}")
        open_command = base + ["open", str(capture)]
        opened = command_runner(
            open_command,
            cwd=cwd,
            timeout=remaining(validation_deadline, "session open"),
            pump=pump,
            check=False,
        )
        result["open"] = command_result_record(open_command, opened)
        state_after_open = rdc_session_state_record(state_path)
        result["session_state_after_open"] = state_after_open
        if opened.returncode != 0:
            raise SmokeFailure(
                f"rdc replay open failed with {opened.returncode}: "
                f"{opened.stderr.strip() or opened.stdout.strip()}"
            )
        if state_after_open.get("exists") is not True or state_after_open.get("valid") is not True:
            raise SmokeFailure(
                f"rdc replay open succeeded without a valid named-session state file: "
                f"{state_after_open!r}"
            )
        daemon_pid = int(state_after_open["state"]["pid"])
        daemon_process_identity = process_identity_factory(
            daemon_pid,
            require_terminate=True,
        )
        daemon_identity_metadata = daemon_process_identity.metadata()
        if daemon_identity_metadata.get("native_handle_held") is not True:
            raise SmokeFailure(
                "named replay daemon identity factory returned no held native process handle"
            )
        result["daemon_stable_process_identity"] = daemon_identity_metadata
        resources_after_open = take_resource_snapshot("after-open")
        result["open_resource_diff"] = diff_rdc_resources(
            resources_before,
            resources_after_open,
        )
        ownership = determine_named_session_daemon_ownership(
            state_after_open,
            resources_before,
            resources_after_open,
            state_path,
            capture,
            daemon_identity_metadata,
        )
        script_command = base + [
            "script",
            str(Path(__file__).resolve()),
            "--arg",
            f"mode={mode}",
            "--arg",
            f"render_mode={render_mode}",
            "--arg",
            f"boundary={boundary}",
            "--arg",
            f"frame={frame}",
            "--json",
        ]
        scripted = command_runner(
            script_command,
            cwd=cwd,
            timeout=remaining(validation_deadline, "marker script"),
            pump=pump,
            check=False,
        )
        result["script"] = command_result_record(script_command, scripted)
        if scripted.returncode != 0:
            raise SmokeFailure(
                f"rdc marker script failed with {scripted.returncode}: "
                f"{scripted.stderr.strip() or scripted.stdout.strip()}"
            )
        try:
            envelope = json.loads(scripted.stdout)
        except json.JSONDecodeError as exc:
            raise SmokeFailure(
                f"rdc marker script returned invalid JSON: {scripted.stdout[-4000:]}"
            ) from exc
        if not isinstance(envelope, dict):
            raise SmokeFailure(f"rdc marker script envelope is invalid: {envelope!r}")
        if str(envelope.get("stderr", "")).strip():
            raise SmokeFailure(f"rdc marker script stderr: {envelope['stderr']}")
        validation = envelope.get("return_value")
        if not isinstance(validation, dict):
            raise SmokeFailure(f"rdc marker script return value is invalid: {validation!r}")
        result["validation"] = validation
        if validation.get("passed") is not True:
            validation_errors = validation.get("errors")
            if isinstance(validation_errors, list) and validation_errors:
                errors.extend(str(error) for error in validation_errors)
            else:
                errors.append(f"RDC marker validation failed: {validation!r}")
    except Exception as exc:
        errors.append(str(exc))
    finally:
        if resources_after_open is None:
            resources_after_open = take_resource_snapshot("after-open-exception")
        if state_after_open is None:
            state_after_open = rdc_session_state_record(state_path)
        stable_daemon_metadata: dict[str, Any] | None = None
        if daemon_process_identity is not None:
            try:
                stable_daemon_metadata = daemon_process_identity.metadata()
            except Exception as exc:
                cleanup["errors"].append(
                    f"stable daemon identity metadata failed: {type(exc).__name__}: {exc}"
                )
        if ownership is None:
            ownership = determine_named_session_daemon_ownership(
                state_after_open,
                resources_before,
                resources_after_open,
                state_path,
                capture,
                stable_daemon_metadata,
            )
        cleanup["stable_daemon_process_identity"] = stable_daemon_metadata
        cleanup["daemon_ownership"] = ownership
        cleanup["owned_daemon_identities"] = (
            [str(ownership["identity"])]
            if ownership.get("established") is True and not ownership.get("errors")
            else []
        )
        cleanup["owned_daemons_observed_after_open"] = (
            [ownership["daemon"]]
            if ownership.get("established") is True and not ownership.get("errors")
            else []
        )
        cleanup["errors"].extend(
            f"daemon ownership: {message}" for message in ownership.get("errors", [])
        )

        cleanup_pump_errors: list[str] = []

        def cleanup_pump() -> None:
            try:
                pump()
            except Exception as exc:
                if not cleanup_pump_errors:
                    cleanup_pump_errors.append(f"{type(exc).__name__}: {exc}")

        cleanup["close_subprocess_used"] = False
        cleanup["status_subprocess_used"] = False
        if daemon_process_identity is None:
            direct_shutdown = {
                "schema": "mgif-rdc-direct-token-shutdown-v1",
                "subprocess_used": False,
                "pid_only_fallback": False,
                "port_scan_fallback": False,
                "tree_cleanup_requested": False,
                "passed": False,
                "errors": [
                    "no held owned replay-daemon handle exists; refusing PID-only cleanup"
                ],
                "post_status": {
                    "classification": "error",
                    "inactive": False,
                    "subprocess_used": False,
                },
            }
        else:
            try:
                direct_shutdown = direct_shutdown_runner(
                    state_path=state_path,
                    state_after_open=state_after_open,
                    process_identity=daemon_process_identity,
                    ownership=ownership,
                    timeout=remaining(overall_deadline, "direct token shutdown"),
                    pump=cleanup_pump,
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
        cleanup["post_status"] = direct_shutdown.get(
            "post_status",
            {
                "classification": "error",
                "inactive": False,
                "subprocess_used": False,
            },
        )
        if direct_shutdown.get("passed") is not True:
            cleanup["errors"].extend(
                f"direct token shutdown: {message}"
                for message in direct_shutdown.get("errors", [])
            )

        state_after_close = rdc_session_state_record(state_path)
        resources_after_close = take_resource_snapshot("after-direct-shutdown")
        cleanup["state_file"]["after_open"] = state_after_open
        cleanup["state_file"]["after_close_attempt"] = state_after_close
        cleanup["after_close_resource_diff"] = diff_rdc_resources(
            resources_before,
            resources_after_close,
        )
        cleanup["target_control_pump_errors"] = cleanup_pump_errors
        if cleanup_pump_errors:
            cleanup["errors"].append(
                f"TargetControl pump failed during replay cleanup: {cleanup_pump_errors[0]}"
            )
        state_after_cleanup = rdc_session_state_record(state_path)
        cleanup["state_file"]["after_cleanup"] = state_after_cleanup
        cleanup["state_file"]["absent_after_cleanup"] = not state_after_cleanup.get(
            "exists", False
        )
        if not cleanup["state_file"]["absent_after_cleanup"]:
            cleanup["errors"].append(f"named session state file remains: {state_path}")

        resources_after = take_resource_snapshot("after-cleanup")
        resource_diff = diff_rdc_resources(resources_before, resources_after)
        cleanup["resource_diff"] = resource_diff
        cleanup["resource_snapshots_available"] = all(
            snapshot.get("available") is True
            for snapshot in (
                resources_before,
                resources_after_open,
                resources_after_close,
                resources_after,
            )
        )
        if not cleanup["resource_snapshots_available"]:
            cleanup["errors"].append("rdc session/daemon resource snapshots were incomplete")
        after_daemons = resources_after.get("daemons", {})
        owned_daemon_ids = list(cleanup["owned_daemon_identities"])
        cleanup["owned_daemon_residue"] = [
            after_daemons[identity] for identity in owned_daemon_ids if identity in after_daemons
        ]
        if cleanup["owned_daemon_residue"]:
            cleanup["errors"].append(
                f"script-created rdc daemon remains: {cleanup['owned_daemon_residue']!r}"
            )
        cleanup["owned_session_file_residue"] = state_after_cleanup.get("exists", False)
        if daemon_process_identity is not None:
            try:
                handle_close = daemon_process_identity.close()
            except Exception as exc:
                handle_close = {
                    "closed": False,
                    "error": f"{type(exc).__name__}: {exc}",
                }
            cleanup["daemon_process_handle_close"] = handle_close
            if handle_close.get("closed") is not True:
                cleanup["errors"].append(
                    f"stable daemon process handle did not close: {handle_close!r}"
                )
        else:
            cleanup["daemon_process_handle_close"] = {
                "opened": False,
                "closed": True,
            }
        cleanup["elapsed_seconds"] = time.monotonic() - operation_started
        cleanup["passed"] = not cleanup["errors"]
        cleanup["closed"] = cleanup["passed"]
        errors.extend(f"replay cleanup: {error}" for error in cleanup["errors"])
    result["session_cleanup"] = cleanup
    result["elapsed_seconds"] = time.monotonic() - operation_started
    result["errors"] = errors
    result["passed"] = not errors and cleanup.get("passed") is True
    return result

def acquire_validated_batch_capture(
    controller: PersistentTargetControl,
    *,
    batch_epoch: int,
    expected_boundary: str,
    expected_ordinal: int,
    mode: str,
    render_mode: str,
    case_name: str,
    copy_budget: CaptureCopyBudget,
    frame: int,
    destination: Path,
    quarantine_directory: Path,
    rdc: str,
    cwd: Path,
    process_identity: Any,
    total_timeout: float,
    copy_timeout: float,
    replay_timeout: float,
    replay_verifier: Callable[..., dict[str, Any]] = verify_capture_marker_replay,
    attempt_log: list[dict[str, Any]] | None = None,
) -> dict[str, Any]:
    if not math.isfinite(total_timeout) or total_timeout <= 0.0:
        raise SmokeFailure(f"invalid capture candidate total timeout: {total_timeout!r}")
    started = time.monotonic()
    deadline = started + total_timeout
    attempts = attempt_log if attempt_log is not None else []
    quarantine_directory.mkdir(parents=True, exist_ok=True)
    destination = destination.resolve()

    def remaining(stage: str) -> float:
        seconds = deadline - time.monotonic()
        if seconds <= 0.0:
            rejected_ids = [
                attempt.get("capture", {}).get("captureId") for attempt in attempts
            ]
            raise SmokeFailure(
                f"capture candidate total timeout expired for {expected_boundary} "
                f"after {time.monotonic() - started:.3f}s; rejected_capture_ids={rejected_ids!r}; "
                f"stage={stage}"
            )
        return max(0.01, seconds)

    while True:
        try:
            candidate, observation = controller.wait_for_batch_candidate(
                batch_epoch=batch_epoch,
                expected_boundary=expected_boundary,
                expected_ordinal=expected_ordinal,
                timeout=remaining("NewCapture wait"),
                process_identity=process_identity,
            )
        except SmokeFailure as exc:
            rejected_ids = [
                attempt.get("capture", {}).get("captureId") for attempt in attempts
            ]
            raise SmokeFailure(
                f"failed to acquire validated capture for {expected_boundary}; "
                f"rejected_capture_ids={rejected_ids!r}: {exc}"
            ) from exc
        capture_id = int(candidate["captureId"])
        candidate_index = int(candidate["candidate_index"])
        candidate_path = (
            quarantine_directory
            / (
                f"epoch-{batch_epoch:03d}-ordinal-{expected_ordinal:02d}-"
                f"candidate-{candidate_index:03d}-capture-{capture_id}.rdc"
            )
        ).resolve()
        copy_budget_reservation = copy_budget.reserve_before_copy(
            case_name=case_name,
            boundary=expected_boundary,
            capture=candidate,
            destination=candidate_path,
        )
        copy_started_utc = utc_now()
        copy_result = controller.copy_capture(
            capture_id,
            candidate_path,
            timeout=min(copy_timeout, remaining("candidate CopyCapture")),
        )
        capture_copied_utc = utc_now()
        if not candidate_path.is_file() or candidate_path.stat().st_size <= 0:
            raise SmokeFailure(
                f"RenderDoc CopyCapture did not create a non-empty candidate file: "
                f"{candidate_path}"
            )
        copy_budget_record = copy_budget.complete_copy(
            copy_budget_reservation,
            actual_bytes=candidate_path.stat().st_size,
        )

        marker_replay_started_utc = utc_now()
        replay = replay_verifier(
            rdc,
            candidate_path,
            mode=mode,
            render_mode=render_mode,
            boundary=expected_boundary,
            frame=frame,
            cwd=cwd,
            timeout=min(replay_timeout, remaining("candidate marker replay")),
            pump=controller.pump,
        )
        marker_replay_completed_utc = utc_now()
        attempt = {
            "capture": dict(candidate),
            "capture_observation": observation,
            "candidate_path": str(candidate_path),
            "copy_started_utc": copy_started_utc,
            "copy_result": copy_result,
            "copy_budget": copy_budget_record,
            "capture_copied_utc": capture_copied_utc,
            "marker_replay_started_utc": marker_replay_started_utc,
            "marker_replay_completed_utc": marker_replay_completed_utc,
            "rdc_marker_replay": replay,
        }
        attempts.append(attempt)
        replay_cleanup = replay.get("session_cleanup")
        try:
            remaining("candidate replay completion")
        except SmokeFailure as exc:
            timeout_reason = str(exc)
            attempt["disposition"] = "rejected"
            attempt["rejection_reason"] = timeout_reason
            controller.reject_batch_candidate(
                batch_epoch=batch_epoch,
                expected_boundary=expected_boundary,
                expected_ordinal=expected_ordinal,
                capture=candidate,
                reason=timeout_reason,
                evidence={
                    "candidate_path": str(candidate_path),
                    "validation": replay.get("validation"),
                    "errors": replay.get("errors", []),
                    "session_cleanup_passed": (
                        replay_cleanup.get("passed")
                        if isinstance(replay_cleanup, dict)
                        else None
                    ),
                },
            )
            raise
        if replay.get("passed") is not True:
            replay_errors = replay.get("errors", [])
            rejection_reason = (
                "; ".join(str(error) for error in replay_errors)
                if isinstance(replay_errors, list) and replay_errors
                else "candidate RDC did not contain the expected automation marker"
            )
            attempt["disposition"] = "rejected"
            attempt["rejection_reason"] = rejection_reason
            controller.reject_batch_candidate(
                batch_epoch=batch_epoch,
                expected_boundary=expected_boundary,
                expected_ordinal=expected_ordinal,
                capture=candidate,
                reason=rejection_reason,
                evidence={
                    "candidate_path": str(candidate_path),
                    "validation": replay.get("validation"),
                    "errors": replay_errors,
                    "session_cleanup_passed": (
                        replay_cleanup.get("passed")
                        if isinstance(replay_cleanup, dict)
                        else None
                    ),
                },
            )
            if not isinstance(replay_cleanup, dict) or replay_cleanup.get("passed") is not True:
                raise SmokeFailure(
                    f"candidate {capture_id} for {expected_boundary} was rejected and its "
                    f"replay session did not cleanly close: {replay!r}"
                )
            remaining("next capture candidate")
            continue

        if destination.exists():
            raise SmokeFailure(f"validated capture destination already exists: {destination}")
        os.replace(candidate_path, destination)
        capture_file_evidence = collect_capture_file_evidence(destination)
        replayed_candidate_path = str(candidate_path)
        replay["replayed_candidate_path"] = replayed_candidate_path
        replay["accepted_capture_path"] = str(destination)
        replay["capture"] = str(destination)
        validation = replay.get("validation")
        if isinstance(validation, dict):
            validation["replayed_candidate_path"] = validation.get("capture")
            validation["capture"] = str(destination)
        attempt["disposition"] = "accepted"
        attempt["accepted_capture_path"] = str(destination)
        attempt["capture_file_evidence"] = capture_file_evidence
        batch_after_accept = controller.accept_batch_candidate(
            batch_epoch=batch_epoch,
            expected_boundary=expected_boundary,
            expected_ordinal=expected_ordinal,
            capture=candidate,
            evidence={
                "replayed_candidate_path": replayed_candidate_path,
                "accepted_capture_path": str(destination),
                "marker_validation_passed": True,
                "capture_file_evidence": capture_file_evidence,
            },
        )
        return {
            "capture": candidate,
            "capture_observation": observation,
            "copy_started_utc": copy_started_utc,
            "copy_result": copy_result,
            "copy_budget": copy_budget_record,
            "capture_copied_utc": capture_copied_utc,
            "capture_path": str(destination),
            "capture_size_bytes": capture_file_evidence["size_bytes"],
            "capture_sha256": capture_file_evidence["sha256"],
            "capture_file_evidence": capture_file_evidence,
            "marker_replay_started_utc": marker_replay_started_utc,
            "marker_replay_completed_utc": marker_replay_completed_utc,
            "rdc_marker_replay": replay,
            "candidate_attempts": attempts,
            "rejected_candidate_count": len(attempts) - 1,
            "batch_after_accept": batch_after_accept,
            "elapsed_seconds": time.monotonic() - started,
            "total_timeout_seconds": total_timeout,
        }

def parse_float_tuple(text: str, expected: int) -> tuple[float, ...]:
    values = tuple(float(part.strip()) for part in text.split(","))
    if len(values) != expected:
        raise SmokeFailure(f"expected {expected} floats, got {values!r}")
    return values


def parse_pose_log(line: str) -> dict[str, Any] | None:
    match = POSE_LOG_RE.search(line)
    if not match:
        return None
    current_position = parse_float_tuple(match.group(4), 3)
    current_angles = parse_float_tuple(match.group(5), 2)
    previous_position = parse_float_tuple(match.group(6), 3)
    previous_angles = parse_float_tuple(match.group(7), 2)
    return {
        "marker": match.group(1),
        "mode": match.group(2),
        "frame": int(match.group(3)),
        "current": {
            "position": list(current_position),
            "yaw_degrees": current_angles[0],
            "pitch_degrees": current_angles[1],
        },
        "previous": {
            "position": list(previous_position),
            "yaw_degrees": previous_angles[0],
            "pitch_degrees": previous_angles[1],
        },
        "line": line,
    }


def pose_values(pose: dict[str, Any]) -> tuple[float, float, float, float, float]:
    position = pose.get("position")
    if not isinstance(position, list) or len(position) != 3:
        raise SmokeFailure(f"invalid pose position: {pose!r}")
    return (
        float(position[0]),
        float(position[1]),
        float(position[2]),
        float(pose["yaw_degrees"]),
        float(pose["pitch_degrees"]),
    )


def assert_pose_close(actual: dict[str, Any], expected: dict[str, Any], description: str) -> None:
    actual_values = pose_values(actual)
    expected_values = pose_values(expected)
    deltas = [abs(left - right) for left, right in zip(actual_values, expected_values)]
    tolerances = [POSITION_TOLERANCE] * 3 + [ANGLE_TOLERANCE_DEGREES] * 2
    if any(delta > tolerance for delta, tolerance in zip(deltas, tolerances)):
        raise SmokeFailure(
            f"pose mismatch for {description}: actual={actual_values}, expected={expected_values}, "
            f"deltas={deltas}, tolerances={tolerances}"
        )


def automation_pose(mode: str, frame: int, *, warmup: int, motion: int) -> dict[str, Any]:
    start = [8.0, 1.5, 0.0, 180.0, 0.0]
    finish = list(start)
    if mode == "csm-translate-stop":
        finish[2] += 1.0
    elif mode == "csm-rotate-stop":
        finish[3] += 8.0
    else:
        raise SmokeFailure(f"unsupported automation mode: {mode}")
    motion_start = warmup
    motion_end = warmup + motion
    if frame < motion_start:
        values = start
    elif frame >= motion_end:
        values = finish
    else:
        progress = float(frame - motion_start + 1) / float(motion)
        values = [left + (right - left) * progress for left, right in zip(start, finish)]
    return {
        "position": values[:3],
        "yaw_degrees": values[3],
        "pitch_degrees": values[4],
    }


def selected_boundaries(capture_control_frame: bool) -> tuple[str, ...]:
    return BASE_BOUNDARIES + ((CONTROL_BOUNDARY,) if capture_control_frame else ())


def app_capture_sync_timeout_seconds(
    *,
    handshake_timeout: float,
    boundary_timeout: float,
    capture_timeout: float,
) -> float:
    # After releasing boundary N, Demo can reach and wait at boundary N+1 while
    # the driver is still observing, validating, and copying capture N.
    return (2.0 * capture_timeout) + boundary_timeout + handshake_timeout + 15.0


def validate_hold_frames(hold: int) -> None:
    if hold < MIN_HOLD_FRAMES:
        raise SmokeFailure(
            f"--hold-frames must be at least {MIN_HOLD_FRAMES}: with a single app marker per "
            "frame, arm-settled (S-1) must occur after first-still"
        )


def expected_boundary_frames(
    warmup: int,
    motion: int,
    hold: int,
    *,
    capture_control_frame: bool,
) -> dict[str, int]:
    validate_hold_frames(hold)
    last_moving = warmup + motion - 1
    settled = warmup + motion + hold - 1
    frames = {
        "last-moving": last_moving,
        "first-still": last_moving + 1,
        "settled": settled,
    }
    if capture_control_frame:
        frames[CONTROL_BOUNDARY] = settled + 1
    return frames


def expected_arm_frames(warmup: int, motion: int, hold: int) -> dict[str, int]:
    boundaries = expected_boundary_frames(
        warmup,
        motion,
        hold,
        capture_control_frame=False,
    )
    frames = {
        ARM_LAST_MOVING: boundaries["last-moving"] - 1,
        ARM_SETTLED: boundaries["settled"] - 1,
    }
    if min(frames.values()) < 0:
        raise SmokeFailure(f"capture arm frame precedes automation frame zero: {frames!r}")
    return frames


def capture_protocol_steps(
    warmup: int,
    motion: int,
    hold: int,
    *,
    capture_control_frame: bool,
) -> list[dict[str, Any]]:
    boundary_frames = expected_boundary_frames(
        warmup,
        motion,
        hold,
        capture_control_frame=capture_control_frame,
    )
    arm_frames = expected_arm_frames(warmup, motion, hold)
    settled_captures = ["settled"]
    if capture_control_frame:
        settled_captures.append(CONTROL_BOUNDARY)
    steps: list[dict[str, Any]] = [
        {
            "kind": "arm",
            "marker": ARM_LAST_MOVING,
            "frame": arm_frames[ARM_LAST_MOVING],
            "num_frames": 2,
            "captures": ["last-moving", "first-still"],
        },
        {
            "kind": "arm",
            "marker": ARM_SETTLED,
            "frame": arm_frames[ARM_SETTLED],
            "num_frames": len(settled_captures),
            "captures": settled_captures,
        },
    ]
    steps.extend(
        {"kind": "capture", "marker": marker, "frame": frame}
        for marker, frame in boundary_frames.items()
    )
    marker_order = {marker: index for index, marker in enumerate((*ARM_MARKERS, *selected_boundaries(capture_control_frame)))}
    steps.sort(
        key=lambda step: (
            int(step["frame"]),
            0 if step["kind"] == "arm" else 1,
            marker_order[str(step["marker"])],
        )
    )
    return steps


def validate_ready_marker(
    ready: dict[str, Any],
    *,
    marker: str,
    mode: str,
    render_mode: str,
    gi_mode: str,
    capture_control_frame: bool,
    expected_frame: int,
    warmup: int,
    motion: int,
) -> dict[str, Any]:
    expected_fields = {
        "protocol": PROTOCOL,
        "phase": "pre-render",
        "marker": marker,
        "mode": mode,
        "frame": expected_frame,
        "no_post": render_mode == "no-post",
        "taa": render_mode == "taa-on",
        "no_ddgi": gi_mode == "no-ddgi",
        "capture_control": capture_control_frame,
    }
    for key, expected in expected_fields.items():
        if ready.get(key) != expected:
            raise SmokeFailure(f"ready marker {marker} field {key!r}: {ready.get(key)!r} != {expected!r}")
    current = ready.get("current")
    previous = ready.get("previous")
    if not isinstance(current, dict) or not isinstance(previous, dict):
        raise SmokeFailure(f"ready marker {marker} has invalid poses: {ready!r}")
    expected_current = automation_pose(mode, expected_frame, warmup=warmup, motion=motion)
    expected_previous = automation_pose(mode, expected_frame - 1, warmup=warmup, motion=motion)
    assert_pose_close(current, expected_current, f"{marker} ready current")
    assert_pose_close(previous, expected_previous, f"{marker} ready previous")
    return {
        "passed": True,
        "expected_current": expected_current,
        "expected_previous": expected_previous,
        "ready_current_matches_expected": True,
        "ready_previous_matches_expected": True,
    }


def validate_log_against_ready(
    log_pose: dict[str, Any], ready: dict[str, Any], marker: str
) -> dict[str, Any]:
    for key in ("marker", "mode", "frame"):
        if log_pose[key] != ready[key]:
            raise SmokeFailure(f"{marker} log {key}={log_pose[key]!r} does not match ready={ready[key]!r}")
    assert_pose_close(log_pose["current"], ready["current"], f"{marker} post-render log current")
    assert_pose_close(log_pose["previous"], ready["previous"], f"{marker} post-render log previous")
    return {
        "passed": True,
        "post_render_current_matches_ready": True,
        "post_render_previous_matches_ready": True,
    }


def validate_boundary_relationships(boundaries: dict[str, dict[str, Any]]) -> dict[str, Any]:
    last = boundaries["last-moving"]["ready"]
    first = boundaries["first-still"]["ready"]
    settled = boundaries["settled"]["ready"]
    if int(first["frame"]) != int(last["frame"]) + 1:
        raise SmokeFailure("first-still is not the frame immediately after last-moving")
    assert_pose_close(first["current"], last["current"], "first-still current vs last-moving current")
    assert_pose_close(first["previous"], last["current"], "first-still previous vs last-moving current")
    assert_pose_close(settled["current"], first["current"], "settled current vs first-still current")
    assert_pose_close(settled["previous"], first["current"], "settled previous vs first-still current")
    last_delta = max(
        abs(left - right)
        for left, right in zip(pose_values(last["current"]), pose_values(last["previous"]))
    )
    if last_delta <= POSE_TOLERANCE:
        raise SmokeFailure("last-moving pose did not move relative to its previous frame")
    result: dict[str, Any] = {
        "passed": True,
        "tolerance": POSE_TOLERANCE,
        "position_tolerance": POSITION_TOLERANCE,
        "angle_tolerance_degrees": ANGLE_TOLERANCE_DEGREES,
        "last_moving_delta_from_previous": last_delta,
        "first_still_matches_last_moving": True,
        "first_still_previous_equals_current": True,
        "settled_matches_first_still": True,
        "control_still_enabled": CONTROL_BOUNDARY in boundaries,
    }
    if CONTROL_BOUNDARY in boundaries:
        control = boundaries[CONTROL_BOUNDARY]["ready"]
        if int(control["frame"]) != int(settled["frame"]) + 1:
            raise SmokeFailure("control-still is not the frame immediately after settled")
        assert_pose_close(control["current"], settled["current"], "control-still current vs settled current")
        assert_pose_close(control["previous"], settled["current"], "control-still previous vs settled current")
        result.update(
            {
                "control_still_is_consecutive": True,
                "control_still_matches_settled": True,
                "control_still_previous_equals_current": True,
            }
        )
    return result


def validate_config_line(
    line: str,
    render_mode: str,
    gi_mode: str,
    *,
    capture_control_frame: bool,
) -> None:
    required = [
        "marker=config",
        "capture_sync=1",
        f"capture_control={1 if capture_control_frame else 0}",
    ]
    if render_mode == "no-post":
        required.extend(["no_post=1", "taa=0"])
    else:
        required.extend(["no_post=0", "taa=1"])
    required.append("no_ddgi=1" if gi_mode == "no-ddgi" else "no_ddgi=0")
    missing = [token for token in required if token not in line]
    if missing:
        raise SmokeFailure(f"config log is missing {missing}: {line}")


def run_case(
    args: argparse.Namespace, mode: str, render_mode: str, gi_mode: str, run_root: Path
) -> dict[str, Any]:
    case_name = f"{mode}__{render_mode}__{gi_mode}"
    case_dir = run_root / case_name
    case_dir.mkdir(parents=True, exist_ok=False)
    sync_dir = case_dir / "sync"
    sync_dir.mkdir()
    log_path = case_dir / "target.log"
    boundary_order = selected_boundaries(args.capture_control_frame)
    expected_frames = expected_boundary_frames(
        args.warmup_frames,
        args.motion_frames,
        args.hold_frames,
        capture_control_frame=args.capture_control_frame,
    )
    expected_arms = expected_arm_frames(
        args.warmup_frames,
        args.motion_frames,
        args.hold_frames,
    )
    protocol_steps = capture_protocol_steps(
        args.warmup_frames,
        args.motion_frames,
        args.hold_frames,
        capture_control_frame=args.capture_control_frame,
    )
    capture_bindings: dict[str, dict[str, Any]] = {}
    for step in protocol_steps:
        if step["kind"] != "arm":
            continue
        for ordinal, boundary in enumerate(step["captures"], start=1):
            capture_bindings[str(boundary)] = {
                "arm_marker": step["marker"],
                "arm_frame": step["frame"],
                "arm_num_frames": step["num_frames"],
                "capture_ordinal_in_arm": ordinal,
            }
    app_sync_timeout = app_capture_sync_timeout_seconds(
        handshake_timeout=args.handshake_timeout,
        boundary_timeout=args.boundary_timeout,
        capture_timeout=args.capture_timeout,
    )

    app_args = [
        f"--automation={mode}",
        f"--fixed-dt={args.fixed_dt:.9g}",
        f"--warmup-frames={args.warmup_frames}",
        f"--motion-frames={args.motion_frames}",
        f"--hold-frames={args.hold_frames}",
        "--no-ui",
        f"--capture-sync-dir={sync_dir}",
        f"--capture-sync-timeout-ms={int(app_sync_timeout * 1000.0)}",
    ]
    if args.capture_control_frame:
        app_args.append("--capture-control-frame")
    app_args.append("--no-post" if render_mode == "no-post" else "--taa")
    if gi_mode == "no-ddgi":
        app_args.append("--no-ddgi")
    app_args.extend(args.extra_app_arg)
    launch_command = [str(args.launch_exe), *app_args]

    case: dict[str, Any] = {
        "name": case_name,
        "status": "running",
        "mode": mode,
        "render_mode": render_mode,
        "gi_mode": gi_mode,
        "capture_control_frame": args.capture_control_frame,
        "boundary_order": list(boundary_order),
        "started_utc": utc_now(),
        "case_directory": str(case_dir.resolve()),
        "sync_directory": str(sync_dir.resolve()),
        "log_path": str(log_path.resolve()),
        "launch_command": launch_command,
        "launch_command_text": command_text(launch_command),
        "app_args": app_args,
        "app_capture_sync_timeout_seconds": app_sync_timeout,
        "expected_automation_frames": expected_frames,
        "expected_arm_frames": expected_arms,
        "arm_order": [step["marker"] for step in protocol_steps if step["kind"] == "arm"],
        "capture_protocol_steps": protocol_steps,
        "arm_protocol_requirement": APP_ARM_PROTOCOL_REQUIREMENT,
        "executable_evidence": {
            "schema": EXECUTABLE_BINDING_SCHEMA,
            "required": True,
            "fail_closed": False,
            "source_before_launch": None,
            "source_baseline_comparison": None,
            "launch_image_before_launch": None,
            "launch_image_baseline_comparison": None,
            "immutable_lock": None,
            "target_process_binding": None,
            "passed": False,
        },
        "arms": {},
        "boundaries": {},
    }

    launcher: subprocess.Popen[str] | None = None
    collector: OutputCollector | None = None
    controller: PersistentTargetControl | None = None
    target_pid: int | None = None
    target_process_identity: StableProcessIdentity | None = None
    ident: int | None = None

    print(f"\n=== {case_name} ===", flush=True)
    print(f"launch: {case['launch_command_text']}", flush=True)

    try:
        try:
            source_before_launch = collect_executable_evidence(args.source_exe)
            launch_before_launch = args.immutable_executable_lock.collect_evidence()
            immutable_lock_metadata = args.immutable_executable_lock.metadata()
        except Exception as exc:
            case["executable_evidence"].update(
                {
                    "fail_closed": True,
                    "passed": False,
                    "error": (
                        "pre-launch executable evidence collection failed closed: "
                        f"{type(exc).__name__}: {exc}"
                    ),
                }
            )
            raise SmokeFailure(case["executable_evidence"]["error"]) from exc
        source_comparison = compare_executable_evidence(
            args.source_executable_evidence_baseline,
            source_before_launch,
            stage=f"source-before-launch:{case_name}",
        )
        launch_comparison = compare_executable_evidence(
            args.launch_executable_evidence_baseline,
            launch_before_launch,
            stage=f"immutable-launch-before-launch:{case_name}",
        )
        immutable_lock_passed = (
            immutable_lock_metadata.get("native_handle_held") is True
            and immutable_lock_metadata.get("write_share_denied") is True
            and immutable_lock_metadata.get("delete_share_denied") is True
            and immutable_lock_metadata.get("lock_identity")
            == args.immutable_executable_lock_identity
            and _process_images_match(
                immutable_lock_metadata.get("resolved_path", ""),
                args.launch_exe,
            )
        )
        case["executable_evidence"].update(
            {
                "source_before_launch": source_before_launch,
                "source_baseline_comparison": source_comparison,
                "launch_image_before_launch": launch_before_launch,
                "launch_image_baseline_comparison": launch_comparison,
                "immutable_lock": immutable_lock_metadata,
                "launch_working_directory": str(args.launch_cwd),
                "passed": (
                    source_comparison["passed"]
                    and launch_comparison["passed"]
                    and immutable_lock_passed
                ),
                "fail_closed": not (
                    source_comparison["passed"]
                    and launch_comparison["passed"]
                    and immutable_lock_passed
                ),
            }
        )
        if source_comparison["passed"] is not True:
            raise SmokeFailure(
                "pre-launch source executable evidence differs from the run baseline: "
                f"{source_comparison['errors']}"
            )
        if launch_comparison["passed"] is not True:
            raise SmokeFailure(
                "pre-launch immutable launch image differs from the run baseline: "
                f"{launch_comparison['errors']}"
            )
        if immutable_lock_passed is not True:
            raise SmokeFailure(
                "immutable launch image lock is not held with deny-write/delete semantics"
            )
        if os.name != "nt":
            raise SmokeFailure(
                "exact launch-owned Demo identity currently requires Windows process handles"
            )
        renderdoc_module = load_renderdoc_module()
        launch_environment = os.environ.copy()
        launch_environment["PATH"] = str(args.launch_cwd) + os.pathsep + launch_environment.get(
            "PATH",
            "",
        )
        renderdoc_module_path = getattr(renderdoc_module, "__file__", None)
        renderdoc_module_directory: Path | None = None
        if renderdoc_module_path:
            renderdoc_module_directory = Path(str(renderdoc_module_path)).resolve().parent
            if (renderdoc_module_directory / "renderdoc.json").is_file():
                launch_environment["ENABLE_VULKAN_RENDERDOC_CAPTURE"] = "1"
                launch_environment["VK_IMPLICIT_LAYER_PATH"] = str(
                    renderdoc_module_directory
                )
        creation_flags = _CREATE_SUSPENDED | getattr(
            subprocess,
            "CREATE_NEW_PROCESS_GROUP",
            0,
        )
        launcher = subprocess.Popen(
            launch_command,
            cwd=args.launch_cwd,
            env=launch_environment,
            stdout=subprocess.PIPE,
            stderr=subprocess.STDOUT,
            text=True,
            encoding="utf-8",
            errors="replace",
            bufsize=1,
            creationflags=creation_flags,
        )
        target_pid = int(launcher.pid)
        target_pid_observed_utc = utc_now()
        case["demo_launcher_pid"] = target_pid
        case["launch_ownership_acquire_started_utc"] = target_pid_observed_utc
        try:
            target_process_identity = StableProcessIdentity.from_launch_owned_process(
                launcher,
                expected_image=args.launch_exe,
            )
            target_identity_acquired_utc = utc_now()
        except Exception as exc:
            case["executable_evidence"].update(
                {
                    "fail_closed": True,
                    "passed": False,
                    "target_process_binding": {
                        "pid": target_pid,
                        "expected_resolved_path": launch_before_launch["resolved_path"],
                        "expected_size_bytes": launch_before_launch["size_bytes"],
                        "expected_sha256": launch_before_launch["sha256"],
                        "immutable_lock_identity": args.immutable_executable_lock_identity,
                        "termination_authority_established": False,
                        "termination_calls": 0,
                        "passed": False,
                        "error": (
                            "could not duplicate and bind the exact CreateProcess-owned Demo "
                            "handle/creation FILETIME; refusing PID or same-image fallback: "
                            f"{type(exc).__name__}: {exc}"
                        ),
                    },
                }
            )
            raise SmokeFailure(
                case["executable_evidence"]["target_process_binding"]["error"]
            ) from exc
        if target_process_identity.pid != target_pid:
            raise SmokeFailure(
                "duplicated launch-owned Demo handle PID differs from subprocess PID"
            )
        if not target_process_identity.is_running():
            raise SmokeFailure(
                "Demo exited before RenderDoc injection through its launch-owned handle: "
                f"{target_process_identity.identity}"
            )

        capture_options = renderdoc_module.GetDefaultCaptureOptions()
        injection = renderdoc_module.InjectIntoProcess(
            target_process_identity.pid,
            [],
            "",
            capture_options,
            False,
        )
        injection_details = injection.result
        if hasattr(injection_details, "code") and hasattr(injection_details, "OK"):
            injection_result = int(injection_details.code)
            injection_succeeded = bool(injection_details.OK())
            injection_message = str(injection_details.Message())
        else:
            injection_result = int(injection_details)
            injection_succeeded = injection_result == 0
            injection_message = ""
        ident = int(injection.ident)
        case["renderdoc_injection"] = {
            "backend": "renderdoc.InjectIntoProcess",
            "result": injection_result,
            "message": injection_message,
            "ident": ident,
            "target_pid": target_process_identity.pid,
            "target_identity": target_process_identity.identity,
            "process_was_suspended": True,
            "renderdoc_module_path": (
                str(Path(str(renderdoc_module_path)).resolve())
                if renderdoc_module_path
                else None
            ),
            "renderdoc_environment_directory": (
                str(renderdoc_module_directory)
                if renderdoc_module_directory is not None
                else None
            ),
        }
        if not injection_succeeded or ident <= 0:
            raise SmokeFailure(
                "RenderDoc InjectIntoProcess failed or returned no exact target ident: "
                f"result={injection_result}, ident={ident}, message={injection_message}"
            )
        resume_status = int(
            _NTDLL.NtResumeProcess(wintypes.HANDLE(int(getattr(launcher, "_handle", 0))))
        )
        case["renderdoc_injection"]["nt_resume_process_status"] = resume_status
        case["renderdoc_injection"]["process_resumed"] = resume_status == 0
        if resume_status != 0:
            raise SmokeFailure(
                f"NtResumeProcess failed for the launch-owned Demo handle: status={resume_status}"
            )
        if launcher.stdout is None:
            raise SmokeFailure("failed to capture direct Demo launch output")
        collector = OutputCollector(launcher.stdout, log_path)
        case["ident"] = ident
        case["pid"] = target_pid
        case["target_stable_process_identity"] = target_process_identity.metadata()
        case["executable_evidence"]["target_process_binding"] = {
            "pid": target_pid,
            "pid_source": "subprocess CreateProcess result",
            "pid_observed_utc": target_pid_observed_utc,
            "stable_handle_acquired_utc": target_identity_acquired_utc,
            "ownership_basis": (
                "launcher-owned CreateProcess handle duplicated before injection/resume; "
                "PID, exact creation FILETIME, and image were read from that duplicate"
            ),
            "termination_authority": "same duplicated launch-owned process handle only",
            "termination_authority_established": True,
            "stable_identity": target_process_identity.identity,
            "creation_time_key": target_process_identity.creation_time_key,
            "creation_filetime_ticks": target_process_identity.metadata().get(
                "creation_filetime_ticks"
            ),
            "duplicated_from_launcher_handle": target_process_identity.metadata().get(
                "duplicated_from_launcher_handle"
            ),
            "pid_lookup_used": target_process_identity.metadata().get("pid_lookup_used"),
            "observed_image_path": target_process_identity.image_path,
            "expected_resolved_path": launch_before_launch["resolved_path"],
            "expected_size_bytes": launch_before_launch["size_bytes"],
            "expected_sha256": launch_before_launch["sha256"],
            "immutable_lock_identity": args.immutable_executable_lock_identity,
            "immutable_lock_held_during_binding": immutable_lock_metadata.get(
                "native_handle_held"
            ) is True,
            "immutable_lock_write_share_denied": immutable_lock_metadata.get(
                "write_share_denied"
            ) is True,
            "immutable_lock_delete_share_denied": immutable_lock_metadata.get(
                "delete_share_denied"
            ) is True,
            "image_path_matches": _process_images_match(
                launch_before_launch["resolved_path"],
                target_process_identity.image_path,
            ),
            "same_native_handle_reserved_for_cleanup": True,
            "passed": True,
        }
        if case["executable_evidence"]["target_process_binding"]["image_path_matches"] is not True:
            case["executable_evidence"].update({"fail_closed": True, "passed": False})
            raise SmokeFailure(
                "stable Demo target process image does not match the executable evidence path"
            )

        controller = PersistentTargetControl(
            ident,
            renderdoc_module=renderdoc_module,
        )
        initial_captures, target_probe = controller.connect(
            timeout=args.launch_timeout,
            process_identity=target_process_identity,
        )
        target_control_pid_matches = (
            int(target_probe.get("target_pid", 0)) == target_process_identity.pid
        )
        case["executable_evidence"]["target_process_binding"].update(
            {
                "target_control_pid": target_probe.get("target_pid"),
                "target_control_pid_matches_owned_handle": target_control_pid_matches,
            }
        )
        if not target_control_pid_matches:
            raise SmokeFailure(
                "TargetControl PID does not equal the launch-owned stable process handle PID"
            )
        case["capture_control"] = {
            **controller.metadata,
            "target_probe": target_probe,
            "initial_captures": initial_captures,
        }

        startup_wait_started = time.monotonic()
        startup_wait_deadline = startup_wait_started + args.startup_timeout
        case["startup_wait"] = {
            "budget_seconds": args.startup_timeout,
            "started_utc": utc_now(),
            "controller_pump": "same owning thread",
        }
        config_wait_timeout = startup_wait_deadline - time.monotonic()
        if config_wait_timeout <= 0.0:
            raise SmokeFailure("startup timeout expired before automation config wait")
        config_line = collector.wait_for_value(
            lambda line: line if "[CSM_AUTOMATION] marker=config" in line else None,
            timeout=config_wait_timeout,
            description="automation config log",
            process_identity=target_process_identity,
            pump=controller.pump,
        )
        validate_config_line(
            config_line,
            render_mode,
            gi_mode,
            capture_control_frame=args.capture_control_frame,
        )
        case["config_log"] = config_line
        case["startup_wait"]["config_observed_utc"] = utc_now()
        case["startup_wait"]["config_elapsed_seconds"] = time.monotonic() - startup_wait_started

        for step_index, step in enumerate(protocol_steps):
            marker = str(step["marker"])
            step_kind = str(step["kind"])
            expected_frame = int(step["frame"])
            ready_path = sync_dir / f"{marker}.ready.json"
            continue_path = sync_dir / f"{marker}.continue"
            if step_index == 0:
                ready_timeout = startup_wait_deadline - time.monotonic()
                if ready_timeout <= 0.0:
                    raise SmokeFailure("startup timeout expired before first ready-marker wait")
            else:
                ready_timeout = args.boundary_timeout
            unexpected_ready_paths: dict[str, Path] = {}
            if step_kind == "arm":
                for future_step in protocol_steps[step_index + 1 :]:
                    if future_step["kind"] == "capture":
                        future_marker = str(future_step["marker"])
                        unexpected_ready_paths[future_marker] = (
                            sync_dir / f"{future_marker}.ready.json"
                        )
            print(f"[{case_name}] waiting for {marker} pre-render marker", flush=True)
            ready = wait_for_ready_marker(
                ready_path,
                timeout=ready_timeout,
                process_identity=target_process_identity,
                unexpected_ready_paths=unexpected_ready_paths,
                pump=controller.pump,
            )
            if step_index == 0:
                case["startup_wait"]["first_ready_observed_utc"] = utc_now()
                case["startup_wait"]["elapsed_seconds"] = time.monotonic() - startup_wait_started
                case["startup_wait"]["completed_within_budget"] = True
            ready_pose_validation = validate_ready_marker(
                ready,
                marker=marker,
                mode=mode,
                render_mode=render_mode,
                gi_mode=gi_mode,
                capture_control_frame=args.capture_control_frame,
                expected_frame=expected_frame,
                warmup=args.warmup_frames,
                motion=args.motion_frames,
            )

            handshake_started = time.monotonic()
            handshake_deadline = handshake_started + args.handshake_timeout

            if step_kind == "arm":
                arm_result: dict[str, Any] = {
                    "marker": marker,
                    "automation_frame": ready["frame"],
                    "ready_observed_utc": utc_now(),
                    "ready_path": str(ready_path.resolve()),
                    "ready": ready,
                    "pose_validation": {"pre_render": ready_pose_validation},
                    "capture_targets": list(step["captures"]),
                    "num_frames": int(step["num_frames"]),
                }
                case["arms"][marker] = arm_result
                trigger_budget = min(
                    args.trigger_timeout,
                    handshake_deadline - time.monotonic(),
                )
                if trigger_budget <= 0.0:
                    raise SmokeFailure(f"{marker} handshake expired before TriggerCapture")
                trigger_result = controller.trigger(
                    num_frames=int(step["num_frames"]),
                    expected_boundaries=step["captures"],
                    timeout=trigger_budget,
                )
                batch_epoch = int(trigger_result["epoch"])
                for target_boundary in step["captures"]:
                    capture_bindings[str(target_boundary)]["batch_epoch"] = batch_epoch
                arm_result["batch_epoch"] = batch_epoch
                arm_result["trigger"] = trigger_result
                arm_result["trigger_completed_utc"] = utc_now()
                if time.monotonic() >= handshake_deadline:
                    raise SmokeFailure(f"{marker} TriggerCapture exceeded handshake timeout")
                write_continue_marker(
                    continue_path,
                    marker=marker,
                    ident=ident,
                    action="triggered-and-armed",
                )
                arm_result["continue_path"] = str(continue_path.resolve())
                arm_result["continue_written_utc"] = utc_now()
                arm_result["handshake_elapsed_seconds"] = time.monotonic() - handshake_started
                print(
                    f"[{case_name}] {marker}: armed {int(step['num_frames'])} capture(s)",
                    flush=True,
                )
                continue

            boundary = marker
            binding = capture_bindings[boundary]
            boundary_result: dict[str, Any] = {
                "marker": boundary,
                "automation_frame": ready["frame"],
                "ready_observed_utc": utc_now(),
                "ready_path": str(ready_path.resolve()),
                "ready": ready,
                "pose_validation": {"pre_render": ready_pose_validation},
                "trigger_at_boundary": False,
                "arm_binding": binding,
            }
            case["boundaries"][boundary] = boundary_result

            if time.monotonic() >= handshake_deadline:
                raise SmokeFailure(f"{boundary} pose validation exceeded handshake timeout")
            write_continue_marker(
                continue_path,
                marker=boundary,
                ident=ident,
                action="pose-validated-target-release",
            )
            boundary_result["continue_path"] = str(continue_path.resolve())
            boundary_result["continue_written_utc"] = utc_now()
            boundary_result["handshake_elapsed_seconds"] = time.monotonic() - handshake_started

            capture_path = case_dir / f"{boundary}.rdc"
            quarantine_directory = (
                case_dir / "candidate-quarantine" / f"batch-{int(binding['batch_epoch']):03d}"
            )
            boundary_result["validated_target_frame"] = False
            candidate_attempts: list[dict[str, Any]] = []
            boundary_result["capture_candidate_attempts"] = candidate_attempts
            candidate_result = acquire_validated_batch_capture(
                controller,
                batch_epoch=int(binding["batch_epoch"]),
                expected_boundary=boundary,
                expected_ordinal=int(binding["capture_ordinal_in_arm"]),
                mode=mode,
                render_mode=render_mode,
                case_name=case_name,
                copy_budget=args.capture_copy_budget,
                frame=int(ready["frame"]),
                destination=capture_path,
                quarantine_directory=quarantine_directory,
                rdc=args.rdc,
                cwd=args.repo_root,
                process_identity=target_process_identity,
                total_timeout=args.capture_timeout + args.replay_timeout,
                copy_timeout=args.capture_timeout,
                replay_timeout=args.replay_timeout,
                attempt_log=candidate_attempts,
            )
            capture = candidate_result["capture"]
            capture_id = int(capture["captureId"])
            boundary_result.update(
                {
                    "renderdoc_capture": capture,
                    "renderdoc_frame_number_informational_only": capture.get("frameNumber"),
                    "capture_id": capture_id,
                    "capture_observed_utc": capture.get("received_utc", utc_now()),
                    "capture_observation": candidate_result["capture_observation"],
                    "copy_backend": "renderdoc.TargetControl.CopyCapture",
                    "copy_result": candidate_result["copy_result"],
                    "copy_started_utc": candidate_result["copy_started_utc"],
                    "capture_path": candidate_result["capture_path"],
                    "capture_size_bytes": candidate_result["capture_size_bytes"],
                    "capture_sha256": candidate_result["capture_sha256"],
                    "capture_file_evidence": candidate_result[
                        "capture_file_evidence"
                    ],
                    "copy_budget": candidate_result["copy_budget"],
                    "capture_copied_utc": candidate_result["capture_copied_utc"],
                    "rdc_marker_replay": candidate_result["rdc_marker_replay"],
                    "marker_replay_started_utc": candidate_result[
                        "marker_replay_started_utc"
                    ],
                    "marker_replay_completed_utc": candidate_result[
                        "marker_replay_completed_utc"
                    ],
                    "capture_candidate_validation": {
                        "batch_epoch": int(binding["batch_epoch"]),
                        "expected_ordinal": int(binding["capture_ordinal_in_arm"]),
                        "candidate_attempts": candidate_result["candidate_attempts"],
                        "rejected_candidate_count": candidate_result[
                            "rejected_candidate_count"
                        ],
                        "elapsed_seconds": candidate_result["elapsed_seconds"],
                        "total_timeout_seconds": candidate_result[
                            "total_timeout_seconds"
                        ],
                        "quarantine_directory": str(quarantine_directory.resolve()),
                        "batch_after_accept": candidate_result["batch_after_accept"],
                    },
                }
            )
            case["arms"][binding["arm_marker"]]["batch_latest"] = candidate_result[
                "batch_after_accept"
            ]

            log_pose = collector.wait_for_value(
                lambda line, wanted=boundary: (
                    parsed
                    if (parsed := parse_pose_log(line)) is not None and parsed["marker"] == wanted
                    else None
                ),
                timeout=args.boundary_timeout,
                description=f"{boundary} post-render pose log",
                process_identity=target_process_identity,
                pump=controller.pump,
            )
            post_render_validation = validate_log_against_ready(log_pose, ready, boundary)
            boundary_result["post_render_log"] = log_pose
            boundary_result["pose_validation"]["post_render"] = post_render_validation
            boundary_result.update(
                {
                    "validated_target_frame": True,
                    "capture_pose_binding": {
                        "passed": True,
                        "basis": (
                            f"explicit {binding['arm_marker']} pre-render arm at automation frame "
                            f"{binding['arm_frame']} created capture batch epoch "
                            f"{binding['batch_epoch']} with {binding['arm_num_frames']} ordinal(s); "
                            f"{boundary} is ordinal {binding['capture_ordinal_in_arm']}. Every "
                            "post-baseline NewCapture candidate was copied and replayed; mismatched "
                            "markers were quarantined without advancing the ordinal, and this RDC "
                            "contained exactly the expected CSM_AUTOMATION_FRAME marker nested in "
                            "GPUDrivenCSMShadow."
                        ),
                        "same_frame_trigger_assumed": False,
                        "fifo_only_binding": False,
                        "replay_marker_validated": True,
                        "arm_marker": binding["arm_marker"],
                        "arm_frame": binding["arm_frame"],
                        "arm_num_frames": binding["arm_num_frames"],
                        "batch_epoch": binding["batch_epoch"],
                        "capture_ordinal_in_arm": binding["capture_ordinal_in_arm"],
                        "rejected_candidate_count": candidate_result[
                            "rejected_candidate_count"
                        ],
                        "ready_pose_validated": True,
                        "post_render_pose_matches_ready": True,
                    },
                }
            )
            print(f"[{case_name}] {boundary}: {capture_path.resolve()}", flush=True)

        case["pose_validation"] = validate_boundary_relationships(case["boundaries"])
        case["status"] = "passed"
    except Exception as exc:
        case["status"] = "failed"
        case["error"] = str(exc)
    finally:
        cleanup: dict[str, Any] = {
            "started_utc": utc_now(),
        }
        if controller is not None:
            cleanup["target_control"] = controller.shutdown()
        else:
            cleanup["target_control"] = {"opened": False, "closed": True}
        if target_process_identity is not None:
            try:
                cleanup["target_process"] = target_process_identity.terminate(timeout=10.0)
            except Exception as exc:
                cleanup["target_process"] = {
                    "identity": target_process_identity.identity,
                    "passed": False,
                    "running_after": None,
                    "error": f"same-handle target termination raised: {type(exc).__name__}: {exc}",
                }
            try:
                cleanup["target_process_handle_close"] = target_process_identity.close()
            except Exception as exc:
                cleanup["target_process_handle_close"] = {
                    "closed": False,
                    "error": f"{type(exc).__name__}: {exc}",
                }
        elif target_pid is not None:
            cleanup["target_process"] = {
                "pid": target_pid,
                "passed": False,
                "running_after": None,
                "error": (
                    "target pid was observed but no stable process handle was established; "
                    "refusing pid-based cleanup"
                ),
            }
            cleanup["target_process_handle_close"] = {
                "opened": False,
                "closed": False,
                "error": "target PID existed but no stable process handle was acquired",
            }
        else:
            cleanup["target_process"] = {
                "pid": None,
                "passed": False,
                "running_after": None,
                "error": (
                    "no Demo target PID/stable identity was established; cleanup ownership "
                    "cannot be claimed"
                ),
            }
            cleanup["target_process_handle_close"] = {
                "opened": False,
                "closed": False,
                "error": "no launch-owned target process handle existed",
            }
        if launcher is not None:
            launcher_record: dict[str, Any] = {
                "pid": int(launcher.pid),
                "returncode_before_wait": launcher.poll(),
                "popen_terminate_called": False,
                "popen_kill_called": False,
                "pid_only_fallback": False,
                "tree_cleanup_requested": False,
                "authority": "duplicated launch-owned process handle only",
            }
            if launcher.poll() is None and target_process_identity is not None:
                try:
                    launcher.wait(timeout=2.0)
                except subprocess.TimeoutExpired:
                    launcher_record["wait_error"] = (
                        "Demo remained live after same-handle cleanup; no Popen/PID fallback used"
                    )
            elif launcher.poll() is not None:
                try:
                    launcher.wait(timeout=0.1)
                except subprocess.TimeoutExpired:
                    pass
            launcher_record["returncode_after_wait"] = launcher.poll()
            launcher_record["running_after"] = launcher.poll() is None
            launcher_record["passed"] = launcher_record["running_after"] is False
            cleanup["demo_launcher"] = launcher_record
        if collector is not None:
            cleanup["output_reader_closed"] = collector.join(timeout=5.0)
            if not cleanup["output_reader_closed"]:
                cleanup["output_tail"] = collector.tail()
        cleanup["completed_utc"] = utc_now()
        case["cleanup"] = cleanup
        case["completed_utc"] = utc_now()

        cleanup_ok = True
        target_cleanup = cleanup.get("target_process", {})
        if target_cleanup.get("passed") is not True:
            cleanup_ok = False
        target_handle_close = cleanup.get("target_process_handle_close", {})
        if target_handle_close.get("closed") is not True:
            cleanup_ok = False
        launcher_cleanup = cleanup.get("demo_launcher", {})
        if launcher is not None and launcher_cleanup.get("passed") is not True:
            cleanup_ok = False
        target_control_cleanup = cleanup.get("target_control", {})
        if target_control_cleanup.get("closed") is False:
            cleanup_ok = False
        if not cleanup.get("output_reader_closed", True):
            cleanup_ok = False
        cleanup["passed"] = cleanup_ok
        target_binding = case.get("executable_evidence", {}).get("target_process_binding")
        if isinstance(target_binding, dict):
            binding_cleanup_passed = (
                target_cleanup.get("passed") is True
                and target_cleanup.get("same_native_handle") is True
                and target_handle_close.get("closed") is True
            )
            target_binding["cleanup_verification"] = {
                "termination_passed": target_cleanup.get("passed") is True,
                "same_native_handle": target_cleanup.get("same_native_handle") is True,
                "original_running_after": target_cleanup.get("running_after"),
                "handle_closed": target_handle_close.get("closed") is True,
                "passed": binding_cleanup_passed,
            }
            target_binding["passed"] = (
                target_binding.get("image_path_matches") is True
                and target_binding.get("immutable_lock_held_during_binding") is True
                and target_binding.get("immutable_lock_write_share_denied") is True
                and target_binding.get("immutable_lock_delete_share_denied") is True
                and target_binding.get("termination_authority_established") is True
                and target_binding.get("duplicated_from_launcher_handle") is True
                and target_binding.get("pid_lookup_used") is False
                and isinstance(target_binding.get("creation_filetime_ticks"), int)
                and int(target_binding["creation_filetime_ticks"]) > 0
                and target_binding.get("target_control_pid_matches_owned_handle") is True
                and binding_cleanup_passed
            )
            case["executable_evidence"]["passed"] = (
                case["executable_evidence"].get(
                    "source_baseline_comparison", {}
                ).get("passed") is True
                and case["executable_evidence"].get(
                    "launch_image_baseline_comparison", {}
                ).get("passed") is True
                and case["executable_evidence"].get("immutable_lock", {}).get(
                    "native_handle_held"
                ) is True
                and target_binding["passed"] is True
            )
            if target_binding["passed"] is not True:
                case["executable_evidence"]["fail_closed"] = True
        if not cleanup_ok and case["status"] == "passed":
            case["status"] = "failed"
            case["error"] = "capture completed, but process/target-control cleanup did not complete"

    if case.get("status") == "failed":
        print(f"[{case_name}] FAILED: {case.get('error', 'unknown failure')}", file=sys.stderr, flush=True)
    return case


def validate_final_capture_set(
    manifest: dict[str, Any],
    manifest_path: Path,
) -> dict[str, Any]:
    errors: list[str] = []
    entries: list[dict[str, Any]] = []
    paths: list[str] = []
    hashes: list[str] = []
    output_root = manifest_path.resolve().parent
    cases = manifest.get("cases")
    if not isinstance(cases, list):
        cases = []
        errors.append("manifest cases are unavailable for final capture rehash")
    expected_capture_count = 0
    for case_index, case in enumerate(cases):
        if not isinstance(case, dict):
            errors.append(f"cases[{case_index}] is not an object")
            continue
        case_name = str(case.get("name", f"case-{case_index}"))
        boundary_order = case.get("boundary_order")
        if not isinstance(boundary_order, list) or not boundary_order:
            errors.append(f"{case_name}: boundary_order is missing")
            continue
        expected_capture_count += len(boundary_order)
        boundaries = case.get("boundaries")
        if not isinstance(boundaries, dict):
            errors.append(f"{case_name}: boundaries map is missing")
            continue
        for boundary in boundary_order:
            label = f"{case_name}/{boundary}"
            row = boundaries.get(boundary)
            if not isinstance(row, dict):
                errors.append(f"{label}: boundary evidence is missing")
                continue
            capture_path_value = row.get("capture_path")
            if not isinstance(capture_path_value, str) or not capture_path_value.strip():
                errors.append(f"{label}: capture_path is missing")
                continue
            capture_path = Path(capture_path_value)
            if not capture_path.is_absolute():
                capture_path = manifest_path.parent / capture_path
            try:
                current = collect_capture_file_evidence(capture_path)
            except Exception as exc:
                errors.append(
                    f"{label}: final capture rehash failed: {type(exc).__name__}: {exc}"
                )
                continue
            row["capture_rehash_after_all_cases"] = current
            canonical_path = str(current["canonical_path"])
            try:
                Path(current["path"]).resolve().relative_to(output_root)
                within_output = True
            except ValueError:
                within_output = False
            stored = row.get("capture_file_evidence")
            checks = {
                "within_output_directory": within_output,
                "direct_path": _normalized_process_image(capture_path)
                == canonical_path,
                "direct_size": row.get("capture_size_bytes")
                == current["size_bytes"],
                "direct_sha256": str(row.get("capture_sha256", "")).lower()
                == current["sha256"],
                "stored_schema": isinstance(stored, dict)
                and stored.get("schema") == CAPTURE_FILE_EVIDENCE_SCHEMA,
                "stored_path": isinstance(stored, dict)
                and stored.get("canonical_path") == canonical_path,
                "stored_size": isinstance(stored, dict)
                and stored.get("size_bytes") == current["size_bytes"],
                "stored_sha256": isinstance(stored, dict)
                and stored.get("sha256") == current["sha256"],
                "stored_read_consistent": isinstance(stored, dict)
                and stored.get("read_consistent") is True,
            }
            failed_checks = [name for name, passed in checks.items() if not passed]
            if failed_checks:
                errors.append(f"{label}: final capture evidence failed {failed_checks}")
            entry = {
                "case": case_name,
                "mode": case.get("mode"),
                "render_mode": case.get("render_mode"),
                "gi_mode": case.get("gi_mode"),
                "boundary": boundary,
                "canonical_path": canonical_path,
                "size_bytes": current["size_bytes"],
                "sha256": current["sha256"],
                "checks": checks,
                "passed": not failed_checks,
            }
            entries.append(entry)
            paths.append(canonical_path)
            hashes.append(str(current["sha256"]))

    if len(entries) != expected_capture_count:
        errors.append(
            f"final capture rehash found {len(entries)} of "
            f"{expected_capture_count} expected capture(s)"
        )
    if len(paths) != len(set(paths)):
        errors.append("final capture canonical paths are not unique")
    if len(hashes) != len(set(hashes)):
        errors.append("final capture SHA-256 values are not unique")

    options = manifest.get("options")
    selected_render_mode: str | None = None
    formal_release_selected = False
    if isinstance(options, dict):
        render_modes = options.get("render_modes")
        selected_render_mode = (
            str(render_modes[0])
            if isinstance(render_modes, list) and len(render_modes) == 1
            else None
        )
        formal_release_selected = (
            set(options.get("modes", [])) == set(MODES)
            and len(options.get("modes", [])) == len(MODES)
            and selected_render_mode in RENDER_MODES
            and options.get("gi_modes") == ["no-ddgi"]
            and options.get("capture_control_frame") is False
            and all(
                options.get(field) == expected
                for field, expected in FORMAL_RELEASE_SMOKE_SEQUENCE.items()
            )
        )
    formal_errors: list[str] = []
    if formal_release_selected:
        expected_names = {
            f"{mode}__{selected_render_mode}__no-ddgi" for mode in MODES
        }
        observed_names = {
            str(case.get("name")) for case in cases if isinstance(case, dict)
        }
        observed_modes = {
            str(case.get("mode")) for case in cases if isinstance(case, dict)
        }
        if len(cases) != 2:
            formal_errors.append(f"formal release requires exactly two cases, found {len(cases)}")
        if observed_modes != set(MODES):
            formal_errors.append(
                f"formal release modes are {sorted(observed_modes)!r}, expected {list(MODES)!r}"
            )
        if observed_names != expected_names:
            formal_errors.append(
                f"formal release case names are {sorted(observed_names)!r}, "
                f"expected {sorted(expected_names)!r}"
            )
        if len(entries) != 6:
            formal_errors.append(
                f"formal release requires exactly six rehashed RDCs, found {len(entries)}"
            )
        if len(set(paths)) != 6:
            formal_errors.append(
                f"formal release requires six unique canonical RDC paths, found {len(set(paths))}"
            )
        if len(set(hashes)) != 6:
            formal_errors.append(
                f"formal release requires six unique RDC SHA-256 values, found {len(set(hashes))}"
            )
        for entry in entries:
            if entry.get("render_mode") != selected_render_mode:
                formal_errors.append(
                    f"{entry['case']}/{entry['boundary']}: render mode differs from "
                    f"formal profile {selected_render_mode!r}"
                )
            if entry.get("gi_mode") != "no-ddgi":
                formal_errors.append(
                    f"{entry['case']}/{entry['boundary']}: formal release GI mode is not no-ddgi"
                )
    errors.extend(formal_errors)
    return {
        "schema": FINAL_CAPTURE_SET_SCHEMA,
        "passed": not errors,
        "manifest_path": str(manifest_path.resolve()),
        "expected_capture_count": expected_capture_count,
        "entry_count": len(entries),
        "unique_canonical_path_count": len(set(paths)),
        "unique_sha256_count": len(set(hashes)),
        "single_rdc_cap_bytes": SMOKE_CAPTURE_BYTES_PER_BOUNDARY,
        "rehash_after_all_cases": True,
        "entries": entries,
        "formal_release": {
            "applicable": formal_release_selected,
            "render_mode": selected_render_mode if formal_release_selected else None,
            "required_case_count": 2,
            "required_capture_count": 6,
            "required_modes": list(MODES),
            "required_gi_mode": "no-ddgi",
            "required_sequence": dict(FORMAL_RELEASE_SMOKE_SEQUENCE),
            "errors": formal_errors,
            "passed": formal_release_selected and not formal_errors,
        },
        "errors": errors,
    }


def validate_cross_case_poses(cases: list[dict[str, Any]]) -> dict[str, Any]:
    comparisons: list[dict[str, Any]] = []
    by_mode: dict[str, list[dict[str, Any]]] = {}
    for case in cases:
        if case.get("status") == "passed":
            by_mode.setdefault(case["mode"], []).append(case)
    for mode, mode_cases in by_mode.items():
        if len(mode_cases) < 2:
            continue
        reference = mode_cases[0]
        reference_order = tuple(reference["boundary_order"])
        for candidate in mode_cases[1:]:
            candidate_order = tuple(candidate["boundary_order"])
            if candidate_order != reference_order:
                raise SmokeFailure(
                    f"{mode} boundary order differs across cases: {reference_order} != {candidate_order}"
                )
            for boundary in reference_order:
                reference_ready = reference["boundaries"][boundary]["ready"]
                candidate_ready = candidate["boundaries"][boundary]["ready"]
                assert_pose_close(
                    candidate_ready["current"],
                    reference_ready["current"],
                    f"{mode} {boundary} current across cases",
                )
                assert_pose_close(
                    candidate_ready["previous"],
                    reference_ready["previous"],
                    f"{mode} {boundary} previous across cases",
                )
            comparisons.append(
                {
                    "mode": mode,
                    "reference": reference["name"],
                    "candidate": candidate["name"],
                    "boundaries": list(reference_order),
                    "passed": True,
                }
            )
    return {
        "passed": True,
        "comparisons": comparisons,
        "tolerance": POSE_TOLERANCE,
        "position_tolerance": POSITION_TOLERANCE,
        "angle_tolerance_degrees": ANGLE_TOLERANCE_DEGREES,
    }


def run_executable_evidence_self_test() -> None:
    with tempfile.TemporaryDirectory(prefix="mgif-executable-evidence-self-test-") as temporary:
        executable = Path(temporary) / "Demo.exe"
        first_payload = b"MGIF-Demo-build-A-evidence"
        second_payload = b"MGIF-Demo-build-B-evidence"
        if len(first_payload) != len(second_payload):
            raise SmokeFailure("executable evidence self-test payload sizes differ")
        executable.write_bytes(first_payload)

        baseline = collect_executable_evidence(executable)
        if (
            baseline.get("schema") != EXECUTABLE_EVIDENCE_SCHEMA
            or Path(str(baseline.get("resolved_path"))) != executable.resolve()
            or baseline.get("size_bytes") != len(first_payload)
            or baseline.get("sha256") != hashlib.sha256(first_payload).hexdigest()
            or baseline.get("read_consistent") is not True
        ):
            raise SmokeFailure(
                f"executable evidence baseline self-test failed: {baseline!r}"
            )

        unchanged = collect_executable_evidence(executable)
        unchanged_comparison = compare_executable_evidence(
            baseline,
            unchanged,
            stage="self-test-unchanged",
        )
        if unchanged_comparison.get("passed") is not True:
            raise SmokeFailure(
                "unchanged executable evidence unexpectedly failed: "
                f"{unchanged_comparison!r}"
            )

        executable.write_bytes(second_payload)
        changed = collect_executable_evidence(executable)
        changed_comparison = compare_executable_evidence(
            baseline,
            changed,
            stage="self-test-same-size-content-change",
        )
        if (
            changed_comparison.get("passed") is not False
            or changed_comparison.get("checks", {}).get("size_bytes") is not True
            or changed_comparison.get("checks", {}).get("sha256") is not False
            or "sha256" not in changed_comparison.get("errors", [])
        ):
            raise SmokeFailure(
                "same-size executable mutation was not rejected by SHA-256: "
                f"{changed_comparison!r}"
            )

        executable.unlink()
        try:
            collect_executable_evidence(executable)
        except SmokeFailure:
            pass
        else:
            raise SmokeFailure("missing executable evidence self-test unexpectedly passed")

    if os.name == "nt":
        with tempfile.TemporaryDirectory(
            prefix="mgif-immutable-executable-self-test-"
        ) as immutable_temporary:
            immutable_root = Path(immutable_temporary)
            system_command = Path(
                os.environ.get("ComSpec", r"C:\Windows\System32\cmd.exe")
            ).resolve(strict=True)
            source_copy = immutable_root / "Demo.exe"
            shutil.copyfile(system_command, source_copy)
            binding, launch_lock = prepare_immutable_executable_binding(
                source_copy,
                immutable_root / "run",
            )
            launch_path = Path(binding["launch_image"]["resolved_path"])
            try:
                lock_metadata = launch_lock.metadata()
                if (
                    binding.get("schema") != EXECUTABLE_BINDING_SCHEMA
                    or binding["launch_image"]["source_copy_comparison"].get("passed")
                    is not True
                    or lock_metadata.get("native_handle_held") is not True
                    or lock_metadata.get("write_share_denied") is not True
                    or lock_metadata.get("delete_share_denied") is not True
                    or binding["launch_image"].get("sha_named") is not True
                ):
                    raise SmokeFailure(
                        f"immutable executable binding self-test failed: {binding!r}"
                    )
                write_blocked = False
                try:
                    with launch_path.open("r+b") as writable:
                        writable.write(b"X")
                except OSError:
                    write_blocked = True
                if not write_blocked:
                    raise SmokeFailure(
                        "immutable executable lock allowed a second writable handle"
                    )
                launch_options: dict[str, Any] = {}
                if os.name == "nt":
                    launch_options["creationflags"] = getattr(
                        subprocess,
                        "CREATE_NO_WINDOW",
                        0,
                    )
                launched = subprocess.run(
                    [str(launch_path), "/d", "/c", "exit", "0"],
                    cwd=source_copy.parent,
                    text=True,
                    encoding="utf-8",
                    errors="replace",
                    capture_output=True,
                    timeout=10.0,
                    check=False,
                    **launch_options,
                )
                if launched.returncode != 0:
                    raise SmokeFailure(
                        "CreateProcess could not execute the held immutable launch image: "
                        f"rc={launched.returncode} stderr={launched.stderr!r}"
                    )
                launch_after = launch_lock.collect_evidence()
                launch_comparison = compare_executable_evidence(
                    binding["launch_image"]["baseline"],
                    launch_after,
                    stage="immutable-self-test-after-launch",
                )
                if launch_comparison.get("passed") is not True:
                    raise SmokeFailure(
                        "immutable launch image changed while its lock was held: "
                        f"{launch_comparison!r}"
                    )
            finally:
                close_result = launch_lock.close()
                if close_result.get("closed") is not True:
                    raise SmokeFailure(
                        f"immutable executable lock self-test did not close: {close_result!r}"
                    )
                if launch_path.exists():
                    os.chmod(launch_path, stat.S_IWRITE | stat.S_IREAD)


def run_capture_budget_and_final_set_self_test() -> None:
    with tempfile.TemporaryDirectory(
        prefix="mgif-capture-budget-set-self-test-"
    ) as temporary:
        root = Path(temporary)
        preflight = {
            "passed": True,
            "estimate": {"safety_margin_bytes": 1},
        }
        abundant_usage = lambda _: argparse.Namespace(free=1 << 62)
        budget = CaptureCopyBudget(
            root,
            case_count=1,
            boundary_count=1,
            disk_preflight=preflight,
            disk_usage_fn=abundant_usage,
        )
        for index in range(SMOKE_MAX_CANDIDATES_PER_BOUNDARY):
            capture = {
                "captureId": index,
                "byteSize": 4096,
            }
            destination = root / f"candidate-{index}.rdc"
            reservation = budget.reserve_before_copy(
                case_name="budget-case",
                boundary="last-moving",
                capture=capture,
                destination=destination,
            )
            destination.write_bytes(f"candidate-{index}".encode("ascii"))
            budget.complete_copy(
                reservation,
                actual_bytes=destination.stat().st_size,
            )
        try:
            budget.reserve_before_copy(
                case_name="budget-case",
                boundary="last-moving",
                capture={"captureId": 99, "byteSize": 4096},
                destination=root / "candidate-over-count.rdc",
            )
        except SmokeFailure as exc:
            if "candidate budget exhausted" not in str(exc):
                raise
        else:
            raise SmokeFailure("candidate count budget unexpectedly allowed a third copy")
        oversized = CaptureCopyBudget(
            root,
            case_count=1,
            boundary_count=1,
            disk_preflight=preflight,
            disk_usage_fn=abundant_usage,
        )
        try:
            oversized.reserve_before_copy(
                case_name="budget-case",
                boundary="settled",
                capture={
                    "captureId": 100,
                    "byteSize": SMOKE_CAPTURE_BYTES_PER_BOUNDARY + 1,
                },
                destination=root / "oversized.rdc",
            )
        except SmokeFailure as exc:
            if "single-RDC cap" not in str(exc):
                raise
        else:
            raise SmokeFailure("oversized NewCapture was not rejected before CopyCapture")
        low_space = CaptureCopyBudget(
            root,
            case_count=1,
            boundary_count=1,
            disk_preflight=preflight,
            disk_usage_fn=lambda _: argparse.Namespace(free=1),
        )
        try:
            low_space.reserve_before_copy(
                case_name="budget-case",
                boundary="settled",
                capture={"captureId": 101, "byteSize": 4096},
                destination=root / "low-space.rdc",
            )
        except SmokeFailure as exc:
            if "insufficient free space before CopyCapture" not in str(exc):
                raise
        else:
            raise SmokeFailure("CopyCapture free-space recheck unexpectedly passed")

        cases: list[dict[str, Any]] = []
        first_capture: Path | None = None
        for mode_index, mode in enumerate(MODES):
            case_name = f"{mode}__taa-on__no-ddgi"
            case_dir = root / case_name
            case_dir.mkdir()
            boundaries: dict[str, Any] = {}
            for boundary_index, boundary in enumerate(BASE_BOUNDARIES):
                path = case_dir / f"{boundary}.rdc"
                path.write_bytes(
                    f"rdc-{mode_index}-{boundary_index}-{boundary}".encode("ascii")
                )
                if first_capture is None:
                    first_capture = path
                evidence = collect_capture_file_evidence(path)
                boundaries[boundary] = {
                    "capture_path": str(path.resolve()),
                    "capture_size_bytes": evidence["size_bytes"],
                    "capture_sha256": evidence["sha256"],
                    "capture_file_evidence": evidence,
                }
            cases.append(
                {
                    "name": case_name,
                    "mode": mode,
                    "render_mode": "taa-on",
                    "gi_mode": "no-ddgi",
                    "boundary_order": list(BASE_BOUNDARIES),
                    "boundaries": boundaries,
                }
            )
        manifest_path = root / "manifest.json"
        manifest = {
            "options": {
                "modes": list(MODES),
                "render_modes": ["taa-on"],
                "gi_modes": ["no-ddgi"],
                "capture_control_frame": False,
                **FORMAL_RELEASE_SMOKE_SEQUENCE,
            },
            "cases": cases,
        }
        valid = validate_final_capture_set(manifest, manifest_path)
        if (
            valid.get("passed") is not True
            or valid.get("entry_count") != 6
            or valid.get("unique_canonical_path_count") != 6
            or valid.get("unique_sha256_count") != 6
            or valid.get("formal_release", {}).get("passed") is not True
        ):
            raise SmokeFailure(f"formal six-capture rehash self-test failed: {valid!r}")
        assert first_capture is not None
        with first_capture.open("ab") as handle:
            handle.write(b"mutation")
        mutated = validate_final_capture_set(manifest, manifest_path)
        if mutated.get("passed") is not False or not any(
            "final capture evidence failed" in str(error)
            for error in mutated.get("errors", [])
        ):
            raise SmokeFailure(
                f"post-capture mutation was not rejected by final rehash: {mutated!r}"
            )


def run_stable_process_identity_self_test() -> None:
    current_identity = acquire_stable_process_identity(
        os.getpid(),
        expected_image=Path(sys.executable),
        require_terminate=False,
    )
    try:
        current_metadata = current_identity.metadata()
        if (
            current_metadata.get("native_handle_held") is not True
            or not current_metadata.get("creation_time_key")
            or current_identity.is_running() is not True
        ):
            raise SmokeFailure(
                f"current-process stable identity self-test failed: {current_metadata!r}"
            )
    finally:
        current_close = current_identity.close()
    if current_close.get("closed") is not True:
        raise SmokeFailure(
            f"current-process stable identity handle did not close: {current_close!r}"
        )

    sleeper_options: dict[str, Any] = {}
    if os.name == "nt":
        sleeper_options["creationflags"] = getattr(
            subprocess,
            "CREATE_NEW_PROCESS_GROUP",
            0,
        )
    else:
        sleeper_options["start_new_session"] = True
    sleeper = subprocess.Popen(
        [sys.executable, "-c", "import time; time.sleep(30)"],
        stdout=subprocess.DEVNULL,
        stderr=subprocess.DEVNULL,
        text=True,
        **sleeper_options,
    )
    sleeper_identity: StableProcessIdentity | None = None
    try:
        sleeper_identity = (
            StableProcessIdentity.from_launch_owned_process(
                sleeper,
                expected_image=Path(sys.executable),
            )
            if os.name == "nt"
            else acquire_stable_process_identity(
                sleeper.pid,
                expected_image=Path(sys.executable),
                require_terminate=True,
            )
        )
        sleeper_metadata = sleeper_identity.metadata()
        if os.name == "nt" and (
            sleeper_metadata.get("ownership_source")
            != "launcher-owned-duplicated-process-handle"
            or sleeper_metadata.get("duplicated_from_launcher_handle") is not True
            or sleeper_metadata.get("pid_lookup_used") is not False
            or not isinstance(sleeper_metadata.get("creation_filetime_ticks"), int)
            or int(sleeper_metadata["creation_filetime_ticks"]) <= 0
        ):
            raise SmokeFailure(
                "launch-owned duplicated-handle/creation-FILETIME evidence is incomplete: "
                f"{sleeper_metadata!r}"
            )
        termination = sleeper_identity.terminate(timeout=3.0)
        if (
            termination.get("passed") is not True
            or termination.get("same_native_handle") is not True
            or termination.get("tree_cleanup_requested") is not False
            or termination.get("running_after") is not False
        ):
            raise SmokeFailure(
                f"same-handle process termination self-test failed: {termination!r}"
            )
        sleeper.wait(timeout=3.0)
    finally:
        if sleeper_identity is not None:
            close_result = sleeper_identity.close()
            if close_result.get("closed") is not True:
                raise SmokeFailure(
                    f"sleeper stable process handle did not close: {close_result!r}"
                )
        if sleeper.poll() is None:
            fallback = finalize_process(sleeper, cwd=Path.cwd())
            if fallback.get("passed") is not True:
                raise SmokeFailure(
                    f"self-test sleeper fallback cleanup failed: {fallback!r}"
                )

    if os.name == "nt":
        first = subprocess.Popen(
            [sys.executable, "-c", "import time; time.sleep(30)"],
            stdout=subprocess.DEVNULL,
            stderr=subprocess.DEVNULL,
            text=True,
            creationflags=getattr(subprocess, "CREATE_NEW_PROCESS_GROUP", 0),
        )
        replacement = subprocess.Popen(
            [sys.executable, "-c", "import time; time.sleep(30)"],
            stdout=subprocess.DEVNULL,
            stderr=subprocess.DEVNULL,
            text=True,
            creationflags=getattr(subprocess, "CREATE_NEW_PROCESS_GROUP", 0),
        )
        first_identity: StableProcessIdentity | None = None
        replacement_identity: StableProcessIdentity | None = None
        try:
            first_identity = StableProcessIdentity.from_launch_owned_process(
                first,
                expected_image=Path(sys.executable),
            )
            replacement_identity = StableProcessIdentity.from_launch_owned_process(
                replacement,
                expected_image=Path(sys.executable),
            )
            reused_pid_view = argparse.Namespace(
                pid=replacement.pid,
                _handle=getattr(first, "_handle"),
            )
            try:
                StableProcessIdentity.from_launch_owned_process(
                    reused_pid_view,
                    expected_image=Path(sys.executable),
                )
            except SmokeFailure as exc:
                if "handle PID mismatch" not in str(exc):
                    raise
            else:
                raise SmokeFailure(
                    "same-image PID reuse before launch-owned identity acquisition passed"
                )
            if first.poll() is not None or replacement.poll() is not None:
                raise SmokeFailure(
                    "same-image PID mismatch path terminated the original or replacement process"
                )
        finally:
            for identity in (first_identity, replacement_identity):
                if identity is not None:
                    identity.terminate(timeout=3.0)
                    identity.close()
            for process in (first, replacement):
                try:
                    process.wait(timeout=3.0)
                except subprocess.TimeoutExpired:
                    raise SmokeFailure(
                        "launch-owned PID-reuse self-test cleanup did not exit through held handles"
                    )
def run_direct_rdc_shutdown_self_test() -> None:
    class FakeOwnedIdentity:
        def __init__(self, *, reuse_before_terminate: bool = False) -> None:
            self.pid = 9001
            self.identity = "9001@test:direct-token"
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
                raise SmokeFailure("direct-shutdown fake handle is closed")
            return self.running

        def terminate(self, *, timeout: float) -> dict[str, Any]:
            if timeout <= 0.0:
                raise SmokeFailure("direct-shutdown fake received invalid timeout")
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

    with tempfile.TemporaryDirectory(prefix="mgif-direct-shutdown-self-test-") as temporary:
        root = Path(temporary)
        state_path = root / "session.json"
        capture = root / "capture.rdc"
        capture.write_bytes(b"rdc")
        secret = "direct-token-secret-must-not-leak"

        def write_state() -> dict[str, Any]:
            state_path.write_text(
                json.dumps(
                    {
                        "capture": str(capture.resolve()),
                        "current_eid": 0,
                        "opened_at": utc_now(),
                        "host": "127.0.0.1",
                        "port": 12345,
                        "token": secret,
                        "pid": 9001,
                    }
                ),
                encoding="utf-8",
            )
            record = rdc_session_state_record(state_path)
            if record.get("valid") is not True:
                raise SmokeFailure(f"direct shutdown state fixture is invalid: {record!r}")
            return record

        def ownership(identity: FakeOwnedIdentity) -> dict[str, Any]:
            return {
                "established": True,
                "identity": identity.identity,
                "errors": [],
            }

        request_tokens: list[str] = []

        def request_builder(token: str, request_id: int) -> dict[str, Any]:
            request_tokens.append(token)
            return {"method": "shutdown", "id": request_id, "params": {"_token": token}}

        graceful_identity = FakeOwnedIdentity()
        graceful_state = write_state()

        def graceful_sender(
            host: str,
            port: int,
            payload: dict[str, Any],
            *,
            timeout: float,
        ) -> dict[str, Any]:
            if host != "127.0.0.1" or port != 12345 or timeout <= 0.0:
                raise SmokeFailure("direct shutdown sender received invalid endpoint/timeout")
            if payload.get("method") != "shutdown":
                raise SmokeFailure("direct shutdown sender received a non-shutdown request")
            graceful_identity.running = False
            return {"result": {"ok": True}}

        graceful = shutdown_owned_rdc_session_direct(
            state_path=state_path,
            state_after_open=graceful_state,
            process_identity=graceful_identity,
            ownership=ownership(graceful_identity),
            timeout=0.2,
            send_request_fn=graceful_sender,
            shutdown_request_fn=request_builder,
        )
        if (
            graceful.get("passed") is not True
            or graceful_identity.terminate_calls != 0
            or graceful.get("subprocess_used") is not False
            or graceful.get("pid_only_fallback") is not False
            or graceful.get("port_scan_fallback") is not False
            or graceful.get("tree_cleanup_requested") is not False
            or state_path.exists()
            or secret in json.dumps(graceful)
        ):
            raise SmokeFailure(f"direct graceful token shutdown self-test failed: {graceful!r}")

        failed_identity = FakeOwnedIdentity()
        failed_state = write_state()

        def failing_sender(*_: Any, **__: Any) -> dict[str, Any]:
            raise TimeoutError("injected direct shutdown timeout")

        recovered = shutdown_owned_rdc_session_direct(
            state_path=state_path,
            state_after_open=failed_state,
            process_identity=failed_identity,
            ownership=ownership(failed_identity),
            timeout=0.1,
            send_request_fn=failing_sender,
            shutdown_request_fn=request_builder,
        )
        recovery = recovered.get("same_handle_recovery", {})
        if (
            recovered.get("passed") is not True
            or failed_identity.terminate_calls != 1
            or recovery.get("same_native_handle") is not True
            or recovery.get("pid_only_fallback") is not False
            or recovery.get("tree_cleanup_requested") is not False
            or recovery.get("passed") is not True
            or state_path.exists()
        ):
            raise SmokeFailure(f"direct timeout same-handle recovery failed: {recovered!r}")

        reused_identity = FakeOwnedIdentity(reuse_before_terminate=True)
        reused_state = write_state()
        reused = shutdown_owned_rdc_session_direct(
            state_path=state_path,
            state_after_open=reused_state,
            process_identity=reused_identity,
            ownership=ownership(reused_identity),
            timeout=0.1,
            send_request_fn=failing_sender,
            shutdown_request_fn=request_builder,
        )
        reused_termination = reused.get("same_handle_recovery", {}).get(
            "termination",
            {},
        )
        if (
            reused.get("passed") is not True
            or reused_identity.terminate_calls != 1
            or reused_termination.get("termination_requested") is not False
            or reused_termination.get("original_exited_before_terminate") is not True
            or reused_termination.get("replacement_untouched") is not True
            or reused_identity.replacement_alive is not True
        ):
            raise SmokeFailure(
                "direct-shutdown PID-reuse TOCTOU touched a replacement process: "
                f"{reused!r}"
            )
        if request_tokens != [secret, secret, secret]:
            raise SmokeFailure("direct shutdown did not use the exact state-file token")

def run_persistent_target_control_self_test() -> None:
    class FakeTargetControl:
        def __init__(self, pid: int) -> None:
            self.pid = pid
            self.connected = True
            self.trigger_calls: list[int] = []
            self.copy_calls: list[int] = []
            self.call_threads: set[int] = set()
            self.messages: list[Any] = [
                argparse.Namespace(type=6, newCapture=None),
                argparse.Namespace(type=9, newCapture=None),
                argparse.Namespace(type=TARGET_CONTROL_NOOP, newCapture=None),
            ]

        def record_thread(self) -> None:
            self.call_threads.add(threading.get_ident())

        def Connected(self) -> bool:
            self.record_thread()
            return self.connected

        def GetPID(self) -> int:
            self.record_thread()
            return self.pid

        def GetTarget(self) -> str:
            self.record_thread()
            return "fake-demo"

        def GetAPI(self) -> str:
            self.record_thread()
            return "Vulkan"

        def ReceiveMessage(self, _: Any) -> Any:
            self.record_thread()
            if self.messages:
                return self.messages.pop(0)
            return argparse.Namespace(type=TARGET_CONTROL_NOOP, newCapture=None)

        def TriggerCapture(self, num_frames: int) -> None:
            self.record_thread()
            self.trigger_calls.append(int(num_frames))

        def CopyCapture(self, capture_id: int, destination: str) -> None:
            self.record_thread()
            self.copy_calls.append(int(capture_id))
            Path(destination).write_bytes(f"fake-renderdoc-capture-{capture_id}".encode("ascii"))
            self.messages.append(
                argparse.Namespace(type=TARGET_CONTROL_CAPTURE_COPIED, newCapture=None)
            )

        def Shutdown(self) -> None:
            self.record_thread()
            self.connected = False

        def queue_capture(self, capture_id: int, frame_number: int) -> None:
            self.messages.append(
                argparse.Namespace(
                    type=TARGET_CONTROL_NEW_CAPTURE,
                    newCapture=argparse.Namespace(
                        captureId=capture_id,
                        path=f"capture-{capture_id}.rdc",
                        frameNumber=frame_number,
                        byteSize=4096,
                        api="Vulkan",
                        local=True,
                    ),
                )
            )

    class FakeRenderDoc:
        def __init__(self, target_control: FakeTargetControl) -> None:
            self.target_control = target_control
            self.create_calls = 0

        def CreateTargetControl(
            self,
            host: str,
            ident: int,
            client_name: str,
            force_connection: bool,
        ) -> FakeTargetControl:
            if host != "" or ident != 4242 or not client_name or not force_connection:
                raise SmokeFailure(
                    "persistent TargetControl self-test received invalid connect arguments"
                )
            self.create_calls += 1
            return self.target_control

        @staticmethod
        def GetVersionString() -> str:
            return "self-test"

    target_pid = os.getpid()
    target_process_identity = argparse.Namespace(
        pid=target_pid,
        identity=f"self-test-target@{target_pid}",
        is_running=lambda: True,
    )
    invalid_pid_target_control = FakeTargetControl(0)
    invalid_pid_controller = PersistentTargetControl(
        4242,
        renderdoc_module=FakeRenderDoc(invalid_pid_target_control),
    )
    try:
        invalid_pid_controller.connect(
            timeout=1.0,
            process_identity=target_process_identity,
        )
    except SmokeFailure as exc:
        if "invalid pid=0" not in str(exc):
            raise
    else:
        raise SmokeFailure("TargetControl GetPID()==0 unexpectedly established ownership")

    fake_target_control = FakeTargetControl(target_pid)
    fake_renderdoc = FakeRenderDoc(fake_target_control)
    controller = PersistentTargetControl(4242, renderdoc_module=fake_renderdoc)
    initial_captures, probe = controller.connect(
        timeout=1.0,
        process_identity=target_process_identity,
    )
    if initial_captures:
        raise SmokeFailure(
            f"persistent TargetControl self-test found initial captures: {initial_captures!r}"
        )
    if probe["drain"]["message_types"] != ["RegisterAPI", "CapturableWindowCount", "Noop"]:
        raise SmokeFailure(f"persistent TargetControl initial drain failed: {probe!r}")
    initial_cached_types = [
        message["type_name"] for message in controller.metadata["cached_non_noop_messages"]
    ]
    if initial_cached_types != ["RegisterAPI", "CapturableWindowCount"]:
        raise SmokeFailure(
            f"TargetControl did not cache non-Noop messages: {initial_cached_types!r}"
        )

    wrong_thread_errors: list[str] = []

    def pump_from_wrong_thread() -> None:
        try:
            controller.pump()
        except SmokeFailure as exc:
            wrong_thread_errors.append(str(exc))

    wrong_thread = threading.Thread(
        target=pump_from_wrong_thread,
        name="csm-smoke-wrong-tc-thread",
    )
    wrong_thread.start()
    wrong_thread.join(timeout=1.0)
    if (
        wrong_thread.is_alive()
        or len(wrong_thread_errors) != 1
        or "owning thread" not in wrong_thread_errors[0]
    ):
        raise SmokeFailure(
            f"TargetControl owning-thread guard self-test failed: {wrong_thread_errors!r}"
        )

    marker_by_capture_id = {
        7: "settled",
        0: "last-moving",
        1: "first-still",
        2: "settled",
    }

    def fake_replay_verifier(
        _: str,
        capture: Path,
        *,
        boundary: str,
        **__: Any,
    ) -> dict[str, Any]:
        match = re.search(r"capture-(-?[0-9]+)\.rdc$", capture.name)
        if match is None:
            raise SmokeFailure(f"candidate filename does not expose capture id: {capture}")
        capture_id = int(match.group(1))
        observed_boundary = marker_by_capture_id.get(capture_id)
        passed = observed_boundary == boundary
        return {
            "capture": str(capture.resolve()),
            "validation": {
                "passed": passed,
                "observed_boundary": observed_boundary,
                "expected_boundary": boundary,
                "capture": str(capture.resolve()),
            },
            "session_cleanup": {"passed": True, "closed": True, "errors": []},
            "errors": [] if passed else [
                f"marker boundary {observed_boundary!r} does not match {boundary!r}"
            ],
            "passed": passed,
        }

    observed_capture_ids: list[int] = []
    with tempfile.TemporaryDirectory(prefix="mgif-target-control-self-test-") as temporary:
        capture_dir = Path(temporary)
        quarantine_dir = capture_dir / "quarantine"
        self_test_copy_budget = CaptureCopyBudget(
            capture_dir,
            case_count=1,
            boundary_count=4,
            disk_preflight={
                "passed": True,
                "estimate": {"safety_margin_bytes": 1},
            },
            disk_usage_fn=lambda _: argparse.Namespace(free=1 << 62),
        )

        fake_target_control.queue_capture(90, 1)
        controller.pump()
        first_trigger = controller.trigger(
            num_frames=2,
            expected_boundaries=("last-moving", "first-still"),
            timeout=1.0,
        )
        first_epoch = int(first_trigger["epoch"])
        baseline_ids = [
            int(capture["captureId"])
            for capture in first_trigger["batch"]["pre_trigger_captures"]
        ]
        if baseline_ids != [90]:
            raise SmokeFailure(
                f"pre-trigger capture baseline was not isolated: {first_trigger!r}"
            )
        try:
            controller.wait_for_batch_candidate(
                batch_epoch=first_epoch,
                expected_boundary="first-still",
                expected_ordinal=2,
                timeout=0.1,
                process_identity=target_process_identity,
            )
        except SmokeFailure as exc:
            if "ordinal mismatch" not in str(exc):
                raise
        else:
            raise SmokeFailure("two-frame capture batch allowed ordinal 2 before ordinal 1")

        fake_target_control.queue_capture(7, 99)
        fake_target_control.queue_capture(0, 2)
        fake_target_control.queue_capture(1, 3)
        last_result = acquire_validated_batch_capture(
            controller,
            batch_epoch=first_epoch,
            expected_boundary="last-moving",
            expected_ordinal=1,
            mode="csm-translate-stop",
            render_mode="no-post",
            case_name="self-test-case",
            copy_budget=self_test_copy_budget,
            frame=2,
            destination=capture_dir / "last-moving.rdc",
            quarantine_directory=quarantine_dir,
            rdc="rdc",
            cwd=Path.cwd(),
            process_identity=target_process_identity,
            total_timeout=1.0,
            copy_timeout=1.0,
            replay_timeout=1.0,
            replay_verifier=fake_replay_verifier,
        )
        observed_capture_ids.append(int(last_result["capture"]["captureId"]))
        if (
            int(last_result["capture"]["captureId"]) != 0
            or last_result["rejected_candidate_count"] != 1
            or int(last_result["candidate_attempts"][0]["capture"]["captureId"]) != 7
            or last_result["candidate_attempts"][0]["disposition"] != "rejected"
            or not Path(last_result["candidate_attempts"][0]["candidate_path"]).is_file()
        ):
            raise SmokeFailure(
                f"extra NewCapture was not quarantined before captureId=0: {last_result!r}"
            )

        first_still_result = acquire_validated_batch_capture(
            controller,
            batch_epoch=first_epoch,
            expected_boundary="first-still",
            expected_ordinal=2,
            mode="csm-translate-stop",
            render_mode="no-post",
            case_name="self-test-case",
            copy_budget=self_test_copy_budget,
            frame=3,
            destination=capture_dir / "first-still.rdc",
            quarantine_directory=quarantine_dir,
            rdc="rdc",
            cwd=Path.cwd(),
            process_identity=target_process_identity,
            total_timeout=1.0,
            copy_timeout=1.0,
            replay_timeout=1.0,
            replay_verifier=fake_replay_verifier,
        )
        observed_capture_ids.append(int(first_still_result["capture"]["captureId"]))
        first_batch = first_still_result["batch_after_accept"]
        accepted_ordinals = [
            int(record["expected_ordinal"])
            for record in first_batch["accepted_candidates"]
        ]
        if first_batch["status"] != "completed" or accepted_ordinals != [1, 2]:
            raise SmokeFailure(f"two-frame batch ordinals were not preserved: {first_batch!r}")

        second_trigger = controller.trigger(
            num_frames=1,
            expected_boundaries=("settled",),
            timeout=1.0,
        )
        fake_target_control.queue_capture(2, 5)
        settled_result = acquire_validated_batch_capture(
            controller,
            batch_epoch=int(second_trigger["epoch"]),
            expected_boundary="settled",
            expected_ordinal=1,
            mode="csm-translate-stop",
            render_mode="no-post",
            case_name="self-test-case",
            copy_budget=self_test_copy_budget,
            frame=5,
            destination=capture_dir / "settled.rdc",
            quarantine_directory=quarantine_dir,
            rdc="rdc",
            cwd=Path.cwd(),
            process_identity=target_process_identity,
            total_timeout=1.0,
            copy_timeout=1.0,
            replay_timeout=1.0,
            replay_verifier=fake_replay_verifier,
        )
        observed_capture_ids.append(int(settled_result["capture"]["captureId"]))

        timeout_trigger = controller.trigger(
            num_frames=1,
            expected_boundaries=("timeout-boundary",),
            timeout=1.0,
        )
        timeout_started = time.monotonic()
        try:
            acquire_validated_batch_capture(
                controller,
                batch_epoch=int(timeout_trigger["epoch"]),
                expected_boundary="timeout-boundary",
                expected_ordinal=1,
                mode="csm-translate-stop",
                render_mode="no-post",
                case_name="self-test-case",
                copy_budget=self_test_copy_budget,
                frame=6,
                destination=capture_dir / "timeout.rdc",
                quarantine_directory=quarantine_dir,
                rdc="rdc",
                cwd=Path.cwd(),
                process_identity=target_process_identity,
                total_timeout=0.04,
                copy_timeout=1.0,
                replay_timeout=1.0,
                replay_verifier=fake_replay_verifier,
            )
        except SmokeFailure as exc:
            if "timed out" not in str(exc) and "total timeout" not in str(exc):
                raise
        else:
            raise SmokeFailure("capture candidate total-timeout self-test unexpectedly passed")
        if time.monotonic() - timeout_started > 0.5:
            raise SmokeFailure("capture candidate total timeout was not bounded")

        output_pump_calls = 0

        def output_wait_pump() -> None:
            nonlocal output_pump_calls
            output_pump_calls += 1
            controller.pump()

        output_collector = OutputCollector(iter(()), capture_dir / "pump-wait.log")
        try:
            output_collector.wait_for_value(
                lambda _: None,
                timeout=0.03,
                description="pump injection self-test",
                process_identity=target_process_identity,
                pump=output_wait_pump,
            )
        except SmokeFailure as exc:
            if "timed out" not in str(exc):
                raise
        else:
            raise SmokeFailure("OutputCollector pump self-test unexpectedly returned a value")
        if not output_collector.join(timeout=1.0) or output_pump_calls <= 0:
            raise SmokeFailure("OutputCollector did not pump TargetControl while waiting")

        ready_pump_calls = 0
        ready_path = capture_dir / "pump.ready.json"

        def ready_wait_pump() -> None:
            nonlocal ready_pump_calls
            ready_pump_calls += 1
            controller.pump()
            if ready_pump_calls == 1:
                atomic_write_json(ready_path, {"ready": True})

        ready_payload = wait_for_ready_marker(
            ready_path,
            timeout=1.0,
            process_identity=target_process_identity,
            pump=ready_wait_pump,
        )
        if ready_payload != {"ready": True} or ready_pump_calls <= 0:
            raise SmokeFailure("ready-marker wait did not pump TargetControl")

    shutdown = controller.shutdown()
    if fake_renderdoc.create_calls != 1:
        raise SmokeFailure(
            f"TargetControl was not created exactly once: {fake_renderdoc.create_calls}"
        )
    if fake_target_control.trigger_calls != [2, 1, 1]:
        raise SmokeFailure(f"unexpected TriggerCapture calls: {fake_target_control.trigger_calls!r}")
    if fake_target_control.copy_calls != [7, 0, 1, 2] or observed_capture_ids != [0, 1, 2]:
        raise SmokeFailure(
            f"batch candidate/captureId=0 handling failed: copies={fake_target_control.copy_calls!r}, "
            f"accepted={observed_capture_ids!r}"
        )
    if 90 in fake_target_control.copy_calls:
        raise SmokeFailure("pre-trigger baseline capture was incorrectly copied")
    if fake_target_control.call_threads != {threading.get_ident()}:
        raise SmokeFailure(
            f"TargetControl was accessed from multiple threads: {fake_target_control.call_threads!r}"
        )
    cached_types = [message["type_name"] for message in shutdown["cached_non_noop_messages"]]
    if cached_types.count("NewCapture") != 5 or cached_types.count("CaptureCopied") != 4:
        raise SmokeFailure(
            "TargetControl did not retain capture/other message records: "
            f"{cached_types!r}"
        )
    if not shutdown["closed"] or fake_target_control.connected:
        raise SmokeFailure(f"persistent TargetControl shutdown failed: {shutdown!r}")

def run_marker_replay_self_test() -> None:
    class FakeAction:
        def __init__(
            self,
            name: str,
            *,
            event_id: int,
            children: Iterable[Any] = (),
        ) -> None:
            self.customName = name
            self.eventId = event_id
            self.children = list(children)

        def GetName(self, _: Any) -> str:
            return self.customName

    mode = "csm-rotate-stop"
    boundary = "first-still"
    frame = 3
    marker_name = f"CSM_AUTOMATION_FRAME mode={mode} boundary={boundary} frame={frame}"
    marker_action = FakeAction(marker_name, event_id=12)
    roots = [
        FakeAction(
            "GPUDrivenCSMShadow",
            event_id=10,
            children=[marker_action],
        )
    ]
    markers = collect_automation_frame_markers(roots, None)
    validation = validate_automation_frame_markers(
        markers,
        mode=mode,
        boundary=boundary,
        frame=frame,
    )
    if validation["passed"] is not True:
        raise SmokeFailure(f"strict marker hierarchy self-test failed: {validation!r}")

    class FakeMarkerReplayController:
        def GetRootActions(self) -> list[Any]:
            return roots

    sentinel = object()
    previous_globals = {
        name: globals().get(name, sentinel) for name in ("args", "controller", "state")
    }
    try:
        globals()["args"] = {
            "mode": mode,
            "boundary": boundary,
            "frame": frame,
            "render_mode": "no-post",
        }
        globals()["controller"] = FakeMarkerReplayController()
        globals()["state"] = argparse.Namespace(
            structured_file=None,
            capture="real-call-shape-self-test.rdc",
        )
        real_call_shape = rdc_marker_replay_main()
    finally:
        for name, previous in previous_globals.items():
            if previous is sentinel:
                globals().pop(name, None)
            else:
                globals()[name] = previous
    if real_call_shape.get("passed") is not True:
        raise SmokeFailure(
            "rdc_marker_replay_main real call-shape regression failed: "
            f"{real_call_shape!r}"
        )

    wrong_parent_markers = collect_automation_frame_markers([marker_action], None)
    wrong_parent = validate_automation_frame_markers(
        wrong_parent_markers,
        mode=mode,
        boundary=boundary,
        frame=frame,
    )
    if wrong_parent["passed"] is not False:
        raise SmokeFailure("marker nesting self-test unexpectedly passed")

    duplicate = validate_automation_frame_markers(
        markers + markers,
        mode=mode,
        boundary=boundary,
        frame=frame,
    )
    if duplicate["passed"] is not False:
        raise SmokeFailure("duplicate marker self-test unexpectedly passed")



    similar_parent_markers = collect_automation_frame_markers(
        [FakeAction("GPU Driven CSM Shadow", event_id=9, children=[marker_action])],
        None,
    )
    similar_parent = validate_automation_frame_markers(
        similar_parent_markers,
        mode=mode,
        boundary=boundary,
        frame=frame,
    )
    if similar_parent["passed"] is not False:
        raise SmokeFailure("non-exact GPUDrivenCSMShadow ancestor unexpectedly passed")

    class FakeFormat:
        def Name(self) -> str:
            return "R16G16B16A16_SFLOAT"

    class FakeTexture:
        def __init__(self) -> None:
            self.format = FakeFormat()

    class FakeView:
        def __init__(self, resource: int) -> None:
            self.resource = resource

    class FakeReflectionResource:
        name = "inTexture"

    class FakeReflection:
        readOnlyResources = [FakeReflectionResource()]
        constantBlocks: list[Any] = []

    class FakeAccess:
        def __init__(self, array_element: int) -> None:
            self.stage = 4
            self.index = 0
            self.arrayElement = array_element

    class FakeDescriptor:
        def __init__(self, resource: int) -> None:
            self.resource = resource

    class FakeUsedDescriptor:
        def __init__(self, array_element: int, resource: int) -> None:
            self.access = FakeAccess(array_element)
            self.descriptor = FakeDescriptor(resource)

    class FakeTaaPipe:
        def GetShaderEntryPoint(self, _: Any) -> str:
            return "fragmentTAAResolveMain"

        def GetShader(self, _: Any) -> int:
            return 101

        def GetGraphicsPipelineObject(self) -> int:
            return 202

        def GetOutputTargets(self) -> list[Any]:
            return [FakeView(200)]

        def GetShaderReflection(self, _: Any) -> Any:
            return FakeReflection()

        def GetAllUsedDescriptors(self, _: bool) -> list[Any]:
            return [
                FakeUsedDescriptor(4, 100),
                FakeUsedDescriptor(7, 102),
                FakeUsedDescriptor(8, 101),
            ]

    class FakeTaaController:
        def __init__(self) -> None:
            self.eids: list[int] = []

        def SetFrameEvent(self, eid: int, force: bool) -> None:
            if force is not True:
                raise SmokeFailure("TAA self-test expected forced frame event")
            self.eids.append(eid)

        def GetPipelineState(self) -> Any:
            return FakeTaaPipe()

    class FakeTaaState:
        structured_file = None
        res_names = {
            100: "GPUDrivenSceneColorHDR",
            101: "GPUDrivenSceneColorHistory1",
            102: "GPUDrivenVelocity",
            200: "GPUDrivenSceneColorHistory0",
        }
        tex_map = {resource_id: FakeTexture() for resource_id in res_names}

    taa_draw = FakeAction("Draw(3)", event_id=32)
    taa_draw.is_draw = True
    taa_draw.numIndices = 3
    taa_draw.numInstances = 1
    taa_inner = FakeAction(
        "GPUDrivenTAAResolve",
        event_id=30,
        children=[taa_draw],
    )
    taa_outer = FakeAction(
        "GPUDrivenTAAResolvePass",
        event_id=29,
        children=[taa_inner],
    )
    taa_controller = FakeTaaController()
    taa_positive = validate_taa_resolve_capture(
        [taa_outer],
        None,
        taa_controller,
        FakeTaaState(),
        boundary="first-still",
        required=True,
    )
    if taa_positive["passed"] is not True or taa_controller.eids != [32]:
        raise SmokeFailure(f"real TAA resolve self-test failed: {taa_positive!r}")

    zero_instance_draw = FakeAction("Draw(3,0)", event_id=33)
    zero_instance_draw.is_draw = True
    zero_instance_draw.numIndices = 3
    zero_instance_draw.numInstances = 0
    zero_instance_taa = validate_taa_resolve_capture(
        [
            FakeAction(
                "GPUDrivenTAAResolve",
                event_id=31,
                children=[zero_instance_draw],
            )
        ],
        None,
        FakeTaaController(),
        FakeTaaState(),
        boundary="first-still",
        required=True,
    )
    if zero_instance_taa["passed"] is not False or not any(
        "Draw(3,1)" in str(error) for error in zero_instance_taa.get("errors", [])
    ):
        raise SmokeFailure(
            "raw RenderDoc numInstances=0 incorrectly normalized to one: "
            f"{zero_instance_taa!r}"
        )

    marker_only_taa = validate_taa_resolve_capture(
        roots,
        None,
        FakeTaaController(),
        FakeTaaState(),
        boundary="first-still",
        required=True,
    )
    if marker_only_taa["passed"] is not False:
        raise SmokeFailure(
            "CSM marker-only capture incorrectly satisfied TAA resolve validation"
        )

    outer_only_taa = validate_taa_resolve_capture(
        [FakeAction("GPUDrivenTAAResolvePass", event_id=29, children=[taa_draw])],
        None,
        FakeTaaController(),
        FakeTaaState(),
        boundary="first-still",
        required=True,
    )
    if outer_only_taa["passed"] is not False:
        raise SmokeFailure(
            "outer GPUDrivenTAAResolvePass event incorrectly substituted for inner resolve"
        )

    if rdc_daemon_identity(42, 1000.1234564) == rdc_daemon_identity(42, 1000.1234565):
        raise SmokeFailure("daemon identity rounded distinct create-times together")

    inactive = classify_rdc_session_status(
        subprocess.CompletedProcess(["rdc", "status"], 1, "", RDC_NO_ACTIVE_SESSION + "\n")
    )
    active = classify_rdc_session_status(
        subprocess.CompletedProcess(["rdc", "status"], 0, "session: fake\n", "")
    )
    status_error = classify_rdc_session_status(
        subprocess.CompletedProcess(["rdc", "status"], 1, "", "error: stale session")
    )
    nonempty_stdout = classify_rdc_session_status(
        subprocess.CompletedProcess(["rdc", "status"], 1, "unexpected", RDC_NO_ACTIVE_SESSION)
    )
    classifications = [
        inactive["classification"],
        active["classification"],
        status_error["classification"],
        nonempty_stdout["classification"],
    ]
    if classifications != ["inactive", "active", "error", "error"]:
        raise SmokeFailure(
            "rdc status classification self-test failed: "
            f"inactive={inactive!r}, active={active!r}, "
            f"status_error={status_error!r}, nonempty_stdout={nonempty_stdout!r}"
        )

    original_candidates = globals()["_python_process_candidates"]
    original_inspector = globals()["inspect_process_identity"]
    globals()["_python_process_candidates"] = lambda: [(7777, "python.exe")]

    def denied_inspector(_: int) -> dict[str, Any] | None:
        raise ProcessIdentityAccessDenied("self-test injected access denied")

    globals()["inspect_process_identity"] = denied_inspector
    try:
        denied_snapshot = snapshot_rdc_resources()
    finally:
        globals()["_python_process_candidates"] = original_candidates
        globals()["inspect_process_identity"] = original_inspector
    if (
        denied_snapshot.get("available") is not False
        or denied_snapshot.get("process_access_denied_count") != 1
        or not denied_snapshot.get("errors")
    ):
        raise SmokeFailure(
            f"access-denied daemon snapshot did not fail closed: {denied_snapshot!r}"
        )

    commands: list[list[str]] = []
    pump_calls = 0
    validation_payload: dict[str, Any] = validation

    def fake_pump() -> None:
        nonlocal pump_calls
        pump_calls += 1

    with tempfile.TemporaryDirectory(prefix="mgif-marker-replay-self-test-") as temporary:
        temporary_path = Path(temporary)
        capture = temporary_path / "marker.rdc"
        capture.write_bytes(b"fake-rdc")
        session_name = "csm_marker_self_test"
        state_path = temporary_path / "sessions" / f"{session_name}.json"
        def fake_filetime_ticks(unix_ns: int) -> int:
            if unix_ns <= 0 or unix_ns % WINDOWS_FILETIME_TICK_NS != 0:
                raise SmokeFailure(f"invalid self-test creation nanoseconds: {unix_ns}")
            return (
                WINDOWS_FILETIME_EPOCH_OFFSET_TICKS
                + unix_ns // WINDOWS_FILETIME_TICK_NS
            )

        daemon_creation_unix_ns = 1_000_000_000
        daemon_creation_ticks = fake_filetime_ticks(daemon_creation_unix_ns)
        daemon_creation_key = f"winfiletime:{daemon_creation_ticks}"
        daemon_identity = f"9001@{daemon_creation_key}"
        replacement_creation_unix_ns = 2_000_000_000
        replacement_creation_ticks = fake_filetime_ticks(replacement_creation_unix_ns)
        replacement_creation_key = f"winfiletime:{replacement_creation_ticks}"
        replacement_identity = f"9001@{replacement_creation_key}"
        resource_phase = "before"
        close_mode = "success"
        factory_identity = daemon_identity
        factory_behavior = "normal"
        factory_creation_unix_ns = daemon_creation_unix_ns
        factory_creation_time = factory_creation_unix_ns / 1_000_000_000.0
        active_process_identity: Any | None = None
        termination_records: list[dict[str, Any]] = []

        def fake_snapshot() -> dict[str, Any]:
            sessions: dict[str, Any] = {}
            daemons: dict[str, Any] = {}
            if resource_phase == "open":
                state_record = rdc_session_state_record(state_path)
                sessions[state_record["path"]] = state_record
                daemons[daemon_identity] = {
                    "identity": daemon_identity,
                    "pid": 9001,
                    "creation_time_key": daemon_creation_key,
                    "creation_filetime_ticks": daemon_creation_ticks,
                    "creation_time_unix_ns": daemon_creation_unix_ns,
                    "create_time": daemon_creation_unix_ns / 1_000_000_000.0,
                    "image_path": "python.exe",
                    "name": "python.exe",
                    "is_rdc_daemon": True,
                    "capture": str(capture.resolve()),
                    "command": ["python", "-m", "rdc.daemon_server", "--token", "<redacted>"],
                    "native_handle_verified_during_inspection": True,
                }
            elif resource_phase in ("replacement", "replacement-after-state"):
                if resource_phase == "replacement-after-state":
                    state_record = rdc_session_state_record(state_path)
                    sessions[state_record["path"]] = state_record
                daemons[replacement_identity] = {
                    "identity": replacement_identity,
                    "pid": 9001,
                    "creation_time_key": replacement_creation_key,
                    "creation_filetime_ticks": replacement_creation_ticks,
                    "creation_time_unix_ns": replacement_creation_unix_ns,
                    "create_time": factory_creation_time,
                    "image_path": "python.exe",
                    "name": "python.exe",
                    "is_rdc_daemon": True,
                    "capture": (
                        str(capture.resolve())
                        if resource_phase == "replacement-after-state"
                        else str(temporary_path / "external-replacement.rdc")
                    ),
                    "command": ["python", "-m", "rdc.daemon_server"],
                    "native_handle_verified_during_inspection": True,
                }
            return {
                "captured_utc": utc_now(),
                "session_directory": str(state_path.parent),
                "sessions": sessions,
                "daemons": daemons,
                "errors": [],
                "process_access_denied_count": 0,
                "available": True,
            }

        def fake_allocator() -> tuple[str, Path]:
            return session_name, state_path

        def fake_runner(
            command: list[str],
            **kwargs: Any,
        ) -> subprocess.CompletedProcess[str]:
            nonlocal resource_phase
            commands.append(command)
            kwargs["pump"]()
            action = command[3]
            if action == "open":
                state_path.parent.mkdir(parents=True, exist_ok=True)
                state_path.write_text(
                    json.dumps(
                        {
                            "capture": str(capture.resolve()),
                            "current_eid": 0,
                            "opened_at": utc_now(),
                            "host": "127.0.0.1",
                            "port": 12345,
                            "token": "self-test-secret-must-not-leak",
                            "pid": 9001,
                        }
                    ),
                    encoding="utf-8",
                )
                resource_phase = (
                    "replacement-after-state"
                    if factory_behavior == "replacement-after-state"
                    else "open"
                )
                return subprocess.CompletedProcess(command, 0, "opened\n", "")
            if action == "script":
                return subprocess.CompletedProcess(
                    command,
                    0,
                    json.dumps(
                        {
                            "stdout": "",
                            "stderr": "",
                            "elapsed_ms": 1,
                            "return_value": validation_payload,
                        }
                    ),
                    "",
                )

            raise SmokeFailure(f"unexpected fake rdc command: {command!r}")

        class FakeStableProcessIdentity:
            def __init__(
                self,
                identity: str,
                behavior: str,
                creation_unix_ns: int,
            ) -> None:
                self.pid = 9001
                self.identity = identity
                self.creation_time_key = identity.split("@", 1)[1]
                self.behavior = behavior
                self.creation_unix_ns = creation_unix_ns
                self.creation_ticks = fake_filetime_ticks(creation_unix_ns)
                self.creation_time = creation_unix_ns / 1_000_000_000.0
                self.running = True
                self.closed = False

            def metadata(self) -> dict[str, Any]:
                return {
                    "pid": self.pid,
                    "identity": self.identity,
                    "creation_time_key": self.creation_time_key,
                    "creation_time_unix_seconds": self.creation_time,
                    "creation_time_unix_ns": self.creation_unix_ns,
                    "creation_filetime_ticks": self.creation_ticks,
                    "image_path": "python.exe",
                    "backend": "self-test-stable-handle",
                    "native_handle_held": not self.closed,
                    "terminate_access": True,
                }

            def is_running(self) -> bool:
                if self.closed:
                    raise SmokeFailure("self-test stable handle was already closed")
                return self.running

            def terminate(self, *, timeout: float) -> dict[str, Any]:
                nonlocal resource_phase
                if timeout <= 0.0:
                    raise SmokeFailure("self-test received invalid termination timeout")
                record = {
                    "identity": self.identity,
                    "behavior": self.behavior,
                    "same_native_handle": True,
                    "tree_cleanup_requested": False,
                }
                termination_records.append(record)
                if self.behavior == "reuse-before-terminate":
                    self.running = False
                    state_path.unlink(missing_ok=True)
                    resource_phase = "replacement"
                    return {
                        **record,
                        "termination_requested": False,
                        "already_exited": True,
                        "running_after": False,
                        "replacement_untouched": True,
                        "passed": True,
                    }
                self.running = False
                state_path.unlink(missing_ok=True)
                resource_phase = "closed"
                return {
                    **record,
                    "termination_requested": True,
                    "running_after": False,
                    "passed": True,
                }

            def close(self) -> dict[str, Any]:
                self.closed = True
                return {"identity": self.identity, "closed": True}

        def fake_process_identity_factory(
            pid: int,
            *,
            require_terminate: bool,
            expected_image: Path | None = None,
        ) -> FakeStableProcessIdentity:
            nonlocal active_process_identity, factory_creation_time
            nonlocal factory_creation_unix_ns, factory_identity
            nonlocal replacement_creation_unix_ns, replacement_creation_ticks
            nonlocal replacement_creation_key, replacement_identity
            if pid != 9001 or require_terminate is not True or expected_image is not None:
                raise SmokeFailure(
                    "fake stable process identity factory received invalid arguments"
                )
            if factory_behavior == "replacement-after-state":
                state_record = rdc_session_state_record(state_path)
                replacement_creation_unix_ns = (
                    int(state_record.get("modified_ns", 0)) + 500_000
                )
                replacement_creation_ticks = fake_filetime_ticks(
                    replacement_creation_unix_ns
                )
                replacement_creation_key = (
                    f"winfiletime:{replacement_creation_ticks}"
                )
                replacement_identity = f"9001@{replacement_creation_key}"
                factory_creation_unix_ns = replacement_creation_unix_ns
                factory_identity = replacement_identity
            else:
                factory_creation_unix_ns = daemon_creation_unix_ns
            factory_creation_time = factory_creation_unix_ns / 1_000_000_000.0
            active_process_identity = FakeStableProcessIdentity(
                factory_identity,
                factory_behavior,
                factory_creation_unix_ns,
            )
            return active_process_identity

        direct_shutdown_calls: list[dict[str, Any]] = []

        def fake_direct_shutdown(**kwargs: Any) -> dict[str, Any]:
            nonlocal resource_phase
            identity = kwargs["process_identity"]
            ownership = kwargs["ownership"]
            direct_shutdown_calls.append(
                {
                    "identity": identity.identity,
                    "ownership_established": ownership.get("established"),
                    "mode": close_mode,
                    "pid_only_fallback": False,
                    "tree_cleanup_requested": False,
                }
            )
            if ownership.get("established") is not True or ownership.get("errors"):
                return {
                    "schema": "mgif-rdc-direct-token-shutdown-v1",
                    "subprocess_used": False,
                    "pid_only_fallback": False,
                    "port_scan_fallback": False,
                    "tree_cleanup_requested": False,
                    "passed": False,
                    "errors": ["ownership not established"],
                    "post_status": {
                        "classification": "error",
                        "inactive": False,
                        "subprocess_used": False,
                    },
                    "owned_daemon_absent": False,
                }
            if close_mode == "success":
                identity.running = False
                state_path.unlink(missing_ok=True)
                resource_phase = "closed"
                recovery = {
                    "attempted": False,
                    "same_native_handle": True,
                    "pid_only_fallback": False,
                    "tree_cleanup_requested": False,
                    "passed": True,
                }
                graceful_accepted = True
            else:
                termination = identity.terminate(timeout=max(0.05, float(kwargs["timeout"])))
                recovery = {
                    "attempted": True,
                    "reason": f"direct shutdown {close_mode}",
                    "same_native_handle": True,
                    "pid_only_fallback": False,
                    "tree_cleanup_requested": False,
                    "termination": termination,
                    "passed": termination.get("passed") is True,
                    "original_exited_before_terminate": bool(
                        termination.get("already_exited")
                        or termination.get("original_exited_before_terminate")
                    ),
                }
                graceful_accepted = False
            inactive = identity.is_running() is False and not state_path.exists()
            return {
                "schema": "mgif-rdc-direct-token-shutdown-v1",
                "subprocess_used": False,
                "pid_only_fallback": False,
                "port_scan_fallback": False,
                "tree_cleanup_requested": False,
                "graceful_requested": True,
                "graceful_accepted": graceful_accepted,
                "same_handle_recovery": recovery,
                "owned_daemon_absent": identity.is_running() is False,
                "state_file_absent": not state_path.exists(),
                "post_status": {
                    "classification": "inactive" if inactive else "error",
                    "inactive": inactive,
                    "subprocess_used": False,
                },
                "errors": [] if inactive and recovery.get("passed") is True else ["cleanup failed"],
                "passed": inactive and recovery.get("passed") is True,
            }
        replay = verify_capture_marker_replay(
            "rdc",
            capture,
            mode=mode,
            render_mode="no-post",
            boundary=boundary,
            frame=frame,
            cwd=Path.cwd(),
            timeout=1.0,
            pump=fake_pump,
            command_runner=fake_runner,
            resource_snapshotter=fake_snapshot,
            session_allocator=fake_allocator,
            process_identity_factory=fake_process_identity_factory,
            direct_shutdown_runner=fake_direct_shutdown,
        )
        if [command[3] for command in commands] != ["open", "script"]:
            raise SmokeFailure(f"marker replay session lifecycle is invalid: {commands!r}")
        cleanup = replay["session_cleanup"]
        if (
            replay["passed"] is not True
            or cleanup["passed"] is not True
            or cleanup["post_status"]["classification"] != "inactive"
            or cleanup["state_file"]["absent_after_cleanup"] is not True
            or cleanup["owned_daemon_residue"]
        ):
            raise SmokeFailure(f"marker replay/cleanup self-test failed: {replay!r}")
        if "self-test-secret-must-not-leak" in json.dumps(replay):
            raise SmokeFailure("rdc session token leaked into replay result")

        validation_payload = {"passed": False, "errors": []}
        resource_phase = "before"
        failed_replay = verify_capture_marker_replay(
            "rdc",
            capture,
            mode=mode,
            render_mode="no-post",
            boundary=boundary,
            frame=frame,
            cwd=Path.cwd(),
            timeout=1.0,
            pump=fake_pump,
            command_runner=fake_runner,
            resource_snapshotter=fake_snapshot,
            session_allocator=fake_allocator,
            process_identity_factory=fake_process_identity_factory,
            direct_shutdown_runner=fake_direct_shutdown,
        )
        if failed_replay["passed"] is not False or not failed_replay["errors"]:
            raise SmokeFailure(f"failed marker validation unexpectedly passed: {failed_replay!r}")
        validation_payload = validation
        close_mode = "fail"
        resource_phase = "before"
        factory_identity = daemon_identity
        factory_behavior = "normal"
        termination_records.clear()
        commands.clear()
        recovered_replay = verify_capture_marker_replay(
            "rdc",
            capture,
            mode=mode,
            render_mode="no-post",
            boundary=boundary,
            frame=frame,
            cwd=Path.cwd(),
            timeout=1.0,
            pump=fake_pump,
            command_runner=fake_runner,
            resource_snapshotter=fake_snapshot,
            session_allocator=fake_allocator,
            process_identity_factory=fake_process_identity_factory,
            direct_shutdown_runner=fake_direct_shutdown,
        )
        recovered_cleanup = recovered_replay["session_cleanup"]
        recovery = recovered_cleanup["verified_daemon_recovery"]
        if (
            recovered_replay["passed"] is not True
            or recovered_cleanup.get("close_subprocess_used") is not False
            or recovered_cleanup.get("status_subprocess_used") is not False
            or recovered_cleanup.get("direct_shutdown", {}).get("passed") is not True
            or recovery.get("attempted") is not True
            or recovery.get("same_native_handle") is not True
            or recovery.get("pid_only_fallback") is not False
            or recovery.get("tree_cleanup_requested") is not False
            or recovery.get("passed") is not True
            or len(termination_records) != 1
            or termination_records[0].get("identity") != daemon_identity
            or termination_records[0].get("same_native_handle") is not True
            or termination_records[0].get("tree_cleanup_requested") is not False
            or [command[3] for command in commands] != ["open", "script"]
            or recovered_cleanup["post_status"]["classification"] != "inactive"
            or recovered_cleanup["state_file"]["absent_after_cleanup"] is not True
            or recovered_cleanup["owned_daemon_residue"]
        ):
            raise SmokeFailure(
                f"verified daemon recovery self-test failed: {recovered_replay!r}"
            )

        close_mode = "timeout"
        resource_phase = "before"
        factory_identity = daemon_identity
        factory_behavior = "normal"
        termination_records.clear()
        commands.clear()
        timed_out_replay = verify_capture_marker_replay(
            "rdc",
            capture,
            mode=mode,
            render_mode="no-post",
            boundary=boundary,
            frame=frame,
            cwd=Path.cwd(),
            timeout=1.0,
            pump=fake_pump,
            command_runner=fake_runner,
            resource_snapshotter=fake_snapshot,
            session_allocator=fake_allocator,
            process_identity_factory=fake_process_identity_factory,
            direct_shutdown_runner=fake_direct_shutdown,
        )
        timed_out_cleanup = timed_out_replay["session_cleanup"]
        timed_out_recovery = timed_out_cleanup["verified_daemon_recovery"]
        if (
            timed_out_replay["passed"] is not True
            or timed_out_cleanup.get("direct_shutdown", {}).get("passed") is not True
            or timed_out_recovery.get("attempted") is not True
            or timed_out_recovery.get("same_native_handle") is not True
            or timed_out_recovery.get("pid_only_fallback") is not False
            or timed_out_recovery.get("tree_cleanup_requested") is not False
            or timed_out_recovery.get("passed") is not True
            or len(termination_records) != 1
            or termination_records[0].get("identity") != daemon_identity
            or termination_records[0].get("same_native_handle") is not True
            or termination_records[0].get("tree_cleanup_requested") is not False
            or "timeout" not in str(timed_out_recovery.get("reason", ""))
            or [command[3] for command in commands] != ["open", "script"]
        ):
            raise SmokeFailure(
                f"timed-out close exact-daemon recovery self-test failed: {timed_out_replay!r}"
            )

        close_mode = "fail"
        resource_phase = "before"
        factory_identity = replacement_identity
        factory_behavior = "normal"
        termination_records.clear()
        commands.clear()
        mismatched_replay = verify_capture_marker_replay(
            "rdc",
            capture,
            mode=mode,
            render_mode="no-post",
            boundary=boundary,
            frame=frame,
            cwd=Path.cwd(),
            timeout=1.0,
            pump=fake_pump,
            command_runner=fake_runner,
            resource_snapshotter=fake_snapshot,
            session_allocator=fake_allocator,
            process_identity_factory=fake_process_identity_factory,
            direct_shutdown_runner=fake_direct_shutdown,
        )
        mismatched_recovery = mismatched_replay["session_cleanup"][
            "verified_daemon_recovery"
        ]
        if (
            mismatched_replay["passed"] is not False
            or termination_records
            or mismatched_recovery.get("passed") is not False
            or mismatched_recovery.get("attempted") is not False
            or mismatched_replay["session_cleanup"]["daemon_ownership"].get("established") is not False
        ):
            raise SmokeFailure(
                "pid reuse/mismatched create-time was not refused: "
                f"{mismatched_replay!r}"
            )
        state_path.unlink(missing_ok=True)
        resource_phase = "closed"

        close_mode = "fail"
        resource_phase = "before"
        factory_identity = replacement_identity
        factory_behavior = "replacement-after-state"
        termination_records.clear()
        direct_shutdown_calls.clear()
        commands.clear()
        pre_acquire_reuse_replay = verify_capture_marker_replay(
            "rdc",
            capture,
            mode=mode,
            render_mode="no-post",
            boundary=boundary,
            frame=frame,
            cwd=Path.cwd(),
            timeout=1.0,
            pump=fake_pump,
            command_runner=fake_runner,
            resource_snapshotter=fake_snapshot,
            session_allocator=fake_allocator,
            process_identity_factory=fake_process_identity_factory,
            direct_shutdown_runner=fake_direct_shutdown,
        )
        pre_acquire_cleanup = pre_acquire_reuse_replay["session_cleanup"]
        pre_acquire_ownership = pre_acquire_cleanup["daemon_ownership"]
        pre_acquire_recovery = pre_acquire_cleanup["verified_daemon_recovery"]
        if (
            pre_acquire_reuse_replay["passed"] is not False
            or pre_acquire_ownership.get("established") is not False
            or pre_acquire_ownership.get("process_created_before_state_file") is not False
            or not any(
                "reused before stable identity acquisition" in str(error)
                for error in pre_acquire_ownership.get("errors", [])
            )
            or pre_acquire_recovery.get("attempted") is not False
            or termination_records
            or len(direct_shutdown_calls) != 1
            or direct_shutdown_calls[0].get("ownership_established") is not False
            or active_process_identity is None
            or active_process_identity.identity != replacement_identity
            or active_process_identity.running is not True
            or replacement_creation_unix_ns
            != int(pre_acquire_cleanup["state_file"]["after_open"]["modified_ns"])
            + 500_000
        ):
            raise SmokeFailure(
                "same-PID replacement created 0.5ms after state publication was not "
                "refused without shutdown or termination authority: "
                f"{pre_acquire_reuse_replay!r}"
            )
        state_path.unlink(missing_ok=True)
        resource_phase = "closed"

        close_mode = "fail"
        resource_phase = "before"
        factory_identity = daemon_identity
        factory_behavior = "reuse-before-terminate"
        termination_records.clear()
        commands.clear()
        reused_pid_replay = verify_capture_marker_replay(
            "rdc",
            capture,
            mode=mode,
            render_mode="no-post",
            boundary=boundary,
            frame=frame,
            cwd=Path.cwd(),
            timeout=1.0,
            pump=fake_pump,
            command_runner=fake_runner,
            resource_snapshotter=fake_snapshot,
            session_allocator=fake_allocator,
            process_identity_factory=fake_process_identity_factory,
            direct_shutdown_runner=fake_direct_shutdown,
        )
        reused_pid_cleanup = reused_pid_replay["session_cleanup"]
        reused_pid_recovery = reused_pid_cleanup["verified_daemon_recovery"]
        reused_pid_termination = reused_pid_recovery.get("termination", {})
        replacement_additions = reused_pid_cleanup["resource_diff"].get(
            "added_daemons", []
        )
        if (
            reused_pid_replay["passed"] is not True
            or reused_pid_cleanup.get("direct_shutdown", {}).get("passed") is not True
            or reused_pid_recovery.get("same_native_handle") is not True
            or reused_pid_recovery.get("pid_only_fallback") is not False
            or reused_pid_recovery.get("tree_cleanup_requested") is not False
            or reused_pid_recovery.get("original_exited_before_terminate") is not True
            or reused_pid_termination.get("same_native_handle") is not True
            or reused_pid_termination.get("termination_requested") is not False
            or reused_pid_termination.get("replacement_untouched") is not True
            or len(termination_records) != 1
            or termination_records[0].get("behavior") != "reuse-before-terminate"
            or reused_pid_cleanup["owned_daemon_residue"]
            or len(replacement_additions) != 1
            or replacement_additions[0].get("identity") != replacement_identity
        ):
            raise SmokeFailure(
                "same-handle PID-reuse TOCTOU self-test touched or misclassified the "
                f"replacement process: {reused_pid_replay!r}"
            )
        state_path.unlink(missing_ok=True)
        resource_phase = "closed"
        close_mode = "success"
        factory_identity = daemon_identity
        factory_behavior = "normal"

        state_owner_record = recovered_cleanup["state_file"]["after_open"]
        publication_ticks = int(
            state_owner_record["publication_boundary"]["filetime_ticks"]
        )
        boundary_creation_ticks = publication_ticks - 1
        boundary_creation_unix_ns = (
            boundary_creation_ticks - WINDOWS_FILETIME_EPOCH_OFFSET_TICKS
        ) * WINDOWS_FILETIME_TICK_NS
        boundary_creation_key = f"winfiletime:{boundary_creation_ticks}"
        boundary_identity = f"9001@{boundary_creation_key}"
        boundary_stable_metadata = {
            "pid": 9001,
            "identity": boundary_identity,
            "creation_time_key": boundary_creation_key,
            "creation_time_unix_seconds": boundary_creation_unix_ns / 1_000_000_000.0,
            "creation_time_unix_ns": boundary_creation_unix_ns,
            "creation_filetime_ticks": boundary_creation_ticks,
            "image_path": "python.exe",
            "native_handle_held": True,
            "terminate_access": True,
        }
        boundary_snapshot = {
            "available": True,
            "errors": [],
            "process_access_denied_count": 0,
            "sessions": {str(state_path.resolve()): state_owner_record},
            "daemons": {
                boundary_identity: {
                    "identity": boundary_identity,
                    "pid": 9001,
                    "creation_time_key": boundary_creation_key,
                    "creation_filetime_ticks": boundary_creation_ticks,
                    "creation_time_unix_ns": boundary_creation_unix_ns,
                    "create_time": boundary_creation_unix_ns / 1_000_000_000.0,
                    "image_path": "python.exe",
                    "is_rdc_daemon": True,
                    "capture": str(capture.resolve()),
                    "command": ["python", "-m", "rdc.daemon_server"],
                    "native_handle_verified_during_inspection": True,
                }
            },
        }
        boundary_ownership = determine_named_session_daemon_ownership(
            state_owner_record,
            {
                "available": True,
                "errors": [],
                "process_access_denied_count": 0,
                "sessions": {},
                "daemons": {},
            },
            boundary_snapshot,
            state_path,
            capture,
            boundary_stable_metadata,
        )
        if (
            boundary_ownership.get("established") is not True
            or boundary_ownership.get("process_created_before_state_file") is not True
            or boundary_ownership.get("state_publication_boundary_exact") is not True
            or boundary_ownership.get("exact_creation_order_clock_match") is not True
        ):
            raise SmokeFailure(
                "daemon created exactly one FILETIME tick before state publication "
                f"was not accepted: {boundary_ownership!r}"
            )

        for label, delta, expected_equal in (
            ("creation-equals-publication", 0, True),
            ("creation-one-tick-after-publication", 1, False),
        ):
            rejected_ticks = publication_ticks + delta
            rejected_unix_ns = (
                rejected_ticks - WINDOWS_FILETIME_EPOCH_OFFSET_TICKS
            ) * WINDOWS_FILETIME_TICK_NS
            rejected_key = f"winfiletime:{rejected_ticks}"
            rejected_identity = f"9001@{rejected_key}"
            rejected_metadata = {
                **boundary_stable_metadata,
                "identity": rejected_identity,
                "creation_time_key": rejected_key,
                "creation_time_unix_seconds": rejected_unix_ns / 1_000_000_000.0,
                "creation_time_unix_ns": rejected_unix_ns,
                "creation_filetime_ticks": rejected_ticks,
            }
            rejected_snapshot = {
                **boundary_snapshot,
                "daemons": {
                    rejected_identity: {
                        **next(iter(boundary_snapshot["daemons"].values())),
                        "identity": rejected_identity,
                        "creation_time_key": rejected_key,
                        "creation_filetime_ticks": rejected_ticks,
                        "creation_time_unix_ns": rejected_unix_ns,
                        "create_time": rejected_unix_ns / 1_000_000_000.0,
                    }
                },
            }
            rejected_ownership = determine_named_session_daemon_ownership(
                state_owner_record,
                {
                    "available": True,
                    "errors": [],
                    "process_access_denied_count": 0,
                    "sessions": {},
                    "daemons": {},
                },
                rejected_snapshot,
                state_path,
                capture,
                rejected_metadata,
            )
            if (
                rejected_ownership.get("established") is not False
                or rejected_ownership.get("strict_creation_precedes_publication") is not False
                or rejected_ownership.get("creation_equals_publication") is not expected_equal
                or rejected_ownership.get("state_file_volume_verified") is not True
                or not rejected_ownership.get("errors")
            ):
                raise SmokeFailure(
                    f"{label} did not reject ownership without tolerance: "
                    f"{rejected_ownership!r}"
                )

        stable_owner_metadata = {
            "pid": 9001,
            "identity": daemon_identity,
            "creation_time_key": daemon_creation_key,
            "creation_time_unix_seconds": daemon_creation_unix_ns / 1_000_000_000.0,
            "creation_time_unix_ns": daemon_creation_unix_ns,
            "creation_filetime_ticks": daemon_creation_ticks,
            "image_path": "python.exe",
            "native_handle_held": True,
            "terminate_access": True,
        }
        same_path_external = {
            "available": True,
            "errors": [],
            "process_access_denied_count": 0,
            "sessions": {str(state_path.resolve()): state_owner_record},
            "daemons": {
                "9002@test:external": {
                    "identity": "9002@test:external",
                    "pid": 9002,
                    "creation_time_key": "test:external",
                    "create_time": 2.0,
                    "is_rdc_daemon": True,
                    "capture": str(capture.resolve()),
                }
            },
        }
        path_only_ownership = determine_named_session_daemon_ownership(
            state_owner_record,
            {
                "available": True,
                "errors": [],
                "process_access_denied_count": 0,
                "sessions": {},
                "daemons": {},
            },
            same_path_external,
            state_path,
            capture,
            stable_owner_metadata,
        )
        if path_only_ownership["established"] is not False:
            raise SmokeFailure(
                "capture path incorrectly established daemon ownership: "
                f"{path_only_ownership!r}"
            )
        mismatched_path_owner_snapshot = {
            **same_path_external,
            "daemons": {
                daemon_identity: {
                    "identity": daemon_identity,
                    "pid": 9001,
                    "creation_time_key": daemon_creation_key,
                    "creation_filetime_ticks": daemon_creation_ticks,
                    "creation_time_unix_ns": daemon_creation_unix_ns,
                    "create_time": daemon_creation_unix_ns / 1_000_000_000.0,
                    "image_path": "python.exe",
                    "is_rdc_daemon": True,
                    "capture": str(temporary_path / "different-capture.rdc"),
                    "command": ["python", "-m", "rdc.daemon_server"],
                    "native_handle_verified_during_inspection": True,
                }
            },
        }
        pid_time_ownership = determine_named_session_daemon_ownership(
            state_owner_record,
            {
                "available": True,
                "errors": [],
                "process_access_denied_count": 0,
                "sessions": {},
                "daemons": {},
            },
            mismatched_path_owner_snapshot,
            state_path,
            capture,
            stable_owner_metadata,
        )
        if (
            pid_time_ownership["established"] is not False
            or pid_time_ownership["daemon_capture_path_metadata_match"] is not False
            or not pid_time_ownership["errors"]
        ):
            raise SmokeFailure(
                "capture-path mismatch did not fail closed before daemon ownership: "
                f"{pid_time_ownership!r}"
            )

        missing_state_capture_record = json.loads(json.dumps(state_owner_record))
        missing_state_capture_record["state"]["capture"] = None
        missing_state_capture = determine_named_session_daemon_ownership(
            missing_state_capture_record,
            {
                "available": True,
                "errors": [],
                "process_access_denied_count": 0,
                "sessions": {},
                "daemons": {},
            },
            same_path_external,
            state_path,
            capture,
            stable_owner_metadata,
        )
        if missing_state_capture["established"] is not False or not any(
            "state file has no capture path" in str(error)
            for error in missing_state_capture["errors"]
        ):
            raise SmokeFailure(
                "missing named-session capture path did not fail closed: "
                f"{missing_state_capture!r}"
            )

        wrong_state_path_record = json.loads(json.dumps(state_owner_record))
        wrong_state_path_record["path"] = str(
            temporary_path / "sessions" / "other-session.json"
        )
        wrong_state_path = determine_named_session_daemon_ownership(
            wrong_state_path_record,
            {
                "available": True,
                "errors": [],
                "process_access_denied_count": 0,
                "sessions": {},
                "daemons": {},
            },
            same_path_external,
            state_path,
            capture,
            stable_owner_metadata,
        )
        if wrong_state_path["established"] is not False or not any(
            "does not match the allocated session path" in str(error)
            for error in wrong_state_path["errors"]
        ):
            raise SmokeFailure(
                f"mismatched named-session path did not fail closed: {wrong_state_path!r}"
            )

        clean_snapshot = fake_snapshot()
        cases = [
            {
                "name": "self-test-case",
                "boundaries": {boundary: {"rdc_marker_replay": replay}},
            }
        ]
        aggregate = aggregate_rdc_session_cleanup(cases, clean_snapshot, clean_snapshot)
        if (
            aggregate["passed"] is not True
            or aggregate["closed"] is not True
            or aggregate["session_count"] != 1
            or aggregate["schema"] != "rdc-session-cleanup-v2"
            or len(aggregate["named_replay_sessions"]) != 1
            or aggregate["named_replay_sessions"][0].get("session") != session_name
            or aggregate["run_resource_diff"]["added_daemons"]
            or aggregate["run_resource_diff"]["added_session_files"]
        ):
            raise SmokeFailure(f"top-level cleanup aggregation self-test failed: {aggregate!r}")
        preexisting_identity = rdc_daemon_identity(7000, 1.0)
        preexisting_daemon = {
            "identity": preexisting_identity,
            "pid": 7000,
            "capture": str(temporary_path / "user-preexisting.rdc"),
        }
        preexisting_snapshot = {
            **clean_snapshot,
            "daemons": {preexisting_identity: preexisting_daemon},
        }
        preexisting_aggregate = aggregate_rdc_session_cleanup(cases, preexisting_snapshot, preexisting_snapshot)
        if (
            preexisting_aggregate["passed"] is not True
            or preexisting_aggregate["external_added_daemons"]
            or preexisting_aggregate["owned_added_daemons"]
        ):
            raise SmokeFailure(
                "pre-existing user daemon was not preserved as baseline state: "
                f"{preexisting_aggregate!r}"
            )
        external_after = {
            **clean_snapshot,
            "daemons": {
                "7001@2.0": {
                    "identity": "7001@2.0",
                    "pid": 7001,
                    "capture": str(capture.resolve()),
                }
            },
        }
        external_aggregate = aggregate_rdc_session_cleanup(cases, clean_snapshot, external_after)
        if (
            external_aggregate["passed"] is not True
            or external_aggregate["closed"] is not True
            or len(external_aggregate["external_added_daemons"]) != 1
            or external_aggregate["owned_added_daemons"]
            or external_aggregate["owned_daemon_residue"]
        ):
            raise SmokeFailure(
                "external daemon diff was gated or misattributed to the harness: "
                f"{external_aggregate!r}"
            )
        leaked_after = {
            **clean_snapshot,
            "daemons": {
                daemon_identity: {
                    "identity": daemon_identity,
                    "pid": 9001,
                    "capture": str(capture.resolve()),
                }
            },
        }
        leaked_aggregate = aggregate_rdc_session_cleanup(cases, clean_snapshot, leaked_after)
        if leaked_aggregate["passed"] is not False or not leaked_aggregate["owned_daemon_residue"]:
            raise SmokeFailure(f"owned daemon residue self-test unexpectedly passed: {leaked_aggregate!r}")
        leaked_session_record = {**cleanup["state_file"]["after_open"], "exists": True}
        leaked_session_after = {
            **clean_snapshot,
            "sessions": {str(state_path.resolve()): leaked_session_record},
        }
        leaked_session_aggregate = aggregate_rdc_session_cleanup(cases, clean_snapshot, leaked_session_after)
        if leaked_session_aggregate["passed"] is not False or not leaked_session_aggregate["owned_session_file_residue"]:
            raise SmokeFailure(f"owned session-file residue self-test unexpectedly passed: {leaked_session_aggregate!r}")

    if pump_calls < 14:
        raise SmokeFailure(f"marker replay subprocess waits did not pump TargetControl: {pump_calls}")


def run_state_file_handle_policy_self_test() -> None:
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
        result = _evaluate_state_file_handle_policy(
            snapshot(file_system=file_system),
            snapshot(file_system=file_system),
        )
        if (
            result.get("verified") is not True
            or result.get("same_handle_path_identity") is not True
            or result.get("raw_filetime_stable") is not True
            or result.get("path_fallback_used") is not False
        ):
            raise SmokeFailure(
                f"fixed {file_system} held-handle policy unexpectedly failed: {result!r}"
            )

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
        result = _evaluate_state_file_handle_policy(before, after)
        if result.get("verified") is not False or not result.get("errors"):
            raise SmokeFailure(
                f"{label} held-handle policy did not fail closed: {result!r}"
            )
def run_self_tests() -> int:
    checks: list[str] = []
    run_executable_evidence_self_test()
    checks.append("executable resolved path/size/SHA-256 binding and mutation fail-closed")

    run_state_file_handle_policy_self_test()
    checks.append(
        "same-handle GUID/FILETIME fixed NTFS/ReFS policy and fail-closed volume matrix"
    )

    repo_root = Path(__file__).resolve().parent.parent
    toolchain_before = collect_toolchain_snapshot(repo_root)
    rdc_package = toolchain_before["tools"]["rdc_cli_package"]
    zero_byte_paths = {
        record["relative_path"]
        for record in rdc_package["files"]
        if int(record["size_bytes"]) == 0
    }
    expected_zero_byte_paths = {
        "rdc/_skills/__init__.py",
        "rdc/vfs/__init__.py",
        "rdc_cli-0.6.1.dist-info/REQUESTED",
    }
    if (
        rdc_package.get("zero_byte_file_count") != len(zero_byte_paths)
        or not expected_zero_byte_paths.issubset(zero_byte_paths)
        or any(
            record["sha256"] != hashlib.sha256(b"").hexdigest()
            for record in rdc_package["files"]
            if int(record["size_bytes"]) == 0
        )
    ):
        raise SmokeFailure(
            "rdc-cli legitimate zero-byte distribution files were not stably evidenced: "
            f"{sorted(zero_byte_paths)!r}"
        )
    toolchain_after = json.loads(json.dumps(toolchain_before))
    toolchain_comparison = compare_toolchain_snapshots(
        toolchain_before,
        toolchain_after,
    )
    if toolchain_comparison["passed"] is not True:
        raise SmokeFailure(
            f"toolchain evidence baseline self-test failed: {toolchain_comparison!r}"
        )
    toolchain_after["tools"]["roi"]["sha256"] = "0" * 64
    mutated_toolchain = compare_toolchain_snapshots(
        toolchain_before,
        toolchain_after,
    )
    if mutated_toolchain["passed"] is not False:
        raise SmokeFailure("mutated ROI toolchain evidence unexpectedly passed")
    checks.append("harness/comparator/ROI/rdc-cli/RenderDoc binding including zero-byte files")

    with tempfile.TemporaryDirectory(prefix="mgif-disk-preflight-self-test-") as temporary:
        work_root = Path(temporary)
        enough = smoke_disk_space_preflight(
            work_root,
            case_count=2,
            boundary_count=3,
            executable_size_bytes=1024,
            disk_usage_fn=lambda _: argparse.Namespace(
                total=0,
                used=0,
                free=1 << 50,
            ),
        )
        if enough["passed"] is not True:
            raise SmokeFailure(f"disk preflight positive self-test failed: {enough!r}")
        estimate = enough["estimate"]
        if estimate["required_free_bytes"] != (
            estimate["estimated_bytes"] + estimate["safety_margin_bytes"]
        ):
            raise SmokeFailure("disk preflight estimate/safety arithmetic is invalid")
        insufficient = smoke_disk_space_preflight(
            work_root,
            case_count=2,
            boundary_count=3,
            executable_size_bytes=1024,
            disk_usage_fn=lambda _: argparse.Namespace(total=0, used=0, free=1),
        )
        try:
            require_smoke_disk_space(insufficient)
        except SmokeFailure:
            pass
        else:
            raise SmokeFailure("insufficient disk preflight unexpectedly passed")
    checks.append("capture/quarantine/replay disk estimate plus safety-margin fail-closed")
    run_capture_budget_and_final_set_self_test()
    checks.append(
        "hard CopyCapture candidate/byte/free-space budget and final six-RDC rehash"
    )

    run_stable_process_identity_self_test()
    checks.append("launch-owned duplicated process handle/FILETIME and same-handle termination")
    run_direct_rdc_shutdown_self_test()
    checks.append("direct session-token shutdown with no PID/tree fallback and PID-reuse safety")
    run_persistent_target_control_self_test()
    checks.append(
        "single-thread persistent TargetControl pump/trigger/receive/copy lifecycle and captureId=0"
    )
    run_marker_replay_self_test()
    checks.append("strict RDC marker hierarchy replay and session cleanup")

    invalid_capture = argparse.Namespace(
        captureId=-1,
        path="invalid.rdc",
        frameNumber=0,
        byteSize=0,
        api="Vulkan",
        local=True,
    )
    try:
        capture_notification_payload(invalid_capture)
    except SmokeFailure:
        pass
    else:
        raise SmokeFailure("negative captureId self-test unexpectedly passed")
    checks.append("negative captureId rejection")

    try:
        capture_protocol_steps(1, 2, 2, capture_control_frame=False)
    except SmokeFailure as exc:
        if "at least 3" not in str(exc):
            raise
    else:
        raise SmokeFailure("hold_frames=2 protocol self-test unexpectedly passed")
    checks.append("hold_frames=2 rejection for single-marker app protocol")

    try:
        run_command(
            [sys.executable, "-c", "import time; time.sleep(5)"],
            cwd=Path.cwd(),
            timeout=0.1,
        )
    except SmokeFailure as exc:
        if "timed out" not in str(exc):
            raise
    else:
        raise SmokeFailure("run_command timeout self-test unexpectedly completed")
    checks.append("subprocess hard timeout")

    sleeper_options: dict[str, Any] = {}
    if os.name == "nt":
        sleeper_options["creationflags"] = getattr(subprocess, "CREATE_NEW_PROCESS_GROUP", 0)
    else:
        sleeper_options["start_new_session"] = True
    sleeper = subprocess.Popen(
        [sys.executable, "-c", "import time; time.sleep(5)"],
        stdout=subprocess.DEVNULL,
        stderr=subprocess.DEVNULL,
        text=True,
        **sleeper_options,
    )
    sleeper_cleanup = finalize_process(sleeper, cwd=Path.cwd())
    if sleeper_cleanup.get("running_after"):
        raise SmokeFailure(f"process cleanup self-test failed: {sleeper_cleanup!r}")
    checks.append("abnormal process cleanup")

    app_timeout = app_capture_sync_timeout_seconds(
        handshake_timeout=30.0,
        boundary_timeout=60.0,
        capture_timeout=120.0,
    )
    if app_timeout != 345.0:
        raise SmokeFailure(f"app capture sync timeout budget self-test failed: {app_timeout}")
    checks.append("next-boundary app timeout budget")

    for capture_control_frame in (False, True):
        boundary_order = selected_boundaries(capture_control_frame)
        frames = expected_boundary_frames(
            1,
            2,
            3,
            capture_control_frame=capture_control_frame,
        )
        if tuple(frames) != boundary_order:
            raise SmokeFailure(f"boundary order self-test failed: {frames!r} vs {boundary_order!r}")
        arm_frames = expected_arm_frames(1, 2, 3)
        if arm_frames != {ARM_LAST_MOVING: 1, ARM_SETTLED: 4}:
            raise SmokeFailure(f"arm frame self-test failed: {arm_frames!r}")
        protocol_steps = capture_protocol_steps(
            1,
            2,
            3,
            capture_control_frame=capture_control_frame,
        )
        protocol_signature = [
            (
                step["kind"],
                step["marker"],
                step["frame"],
                step.get("num_frames"),
            )
            for step in protocol_steps
        ]
        expected_signature = [
            ("arm", ARM_LAST_MOVING, 1, 2),
            ("capture", "last-moving", 2, None),
            ("capture", "first-still", 3, None),
            ("arm", ARM_SETTLED, 4, 2 if capture_control_frame else 1),
            ("capture", "settled", 5, None),
        ]
        if capture_control_frame:
            expected_signature.append(("capture", CONTROL_BOUNDARY, 6, None))
        if protocol_signature != expected_signature:
            raise SmokeFailure(
                f"capture arm protocol order self-test failed: {protocol_signature!r} "
                f"!= {expected_signature!r}"
            )
        for mode in MODES:
            boundaries: dict[str, dict[str, Any]] = {}
            for arm_marker, arm_frame in arm_frames.items():
                arm_ready = {
                    "protocol": PROTOCOL,
                    "phase": "pre-render",
                    "marker": arm_marker,
                    "mode": mode,
                    "frame": arm_frame,
                    "no_post": True,
                    "taa": False,
                    "no_ddgi": True,
                    "capture_control": capture_control_frame,
                    "current": automation_pose(mode, arm_frame, warmup=1, motion=2),
                    "previous": automation_pose(mode, arm_frame - 1, warmup=1, motion=2),
                }
                validate_ready_marker(
                    arm_ready,
                    marker=arm_marker,
                    mode=mode,
                    render_mode="no-post",
                    gi_mode="no-ddgi",
                    capture_control_frame=capture_control_frame,
                    expected_frame=arm_frame,
                    warmup=1,
                    motion=2,
                )
            for marker in boundary_order:
                frame = frames[marker]
                ready = {
                    "protocol": PROTOCOL,
                    "phase": "pre-render",
                    "marker": marker,
                    "mode": mode,
                    "frame": frame,
                    "no_post": True,
                    "taa": False,
                    "no_ddgi": True,
                    "capture_control": capture_control_frame,
                    "current": automation_pose(mode, frame, warmup=1, motion=2),
                    "previous": automation_pose(mode, frame - 1, warmup=1, motion=2),
                }
                pre_validation = validate_ready_marker(
                    ready,
                    marker=marker,
                    mode=mode,
                    render_mode="no-post",
                    gi_mode="no-ddgi",
                    capture_control_frame=capture_control_frame,
                    expected_frame=frame,
                    warmup=1,
                    motion=2,
                )
                log_pose = {
                    "marker": marker,
                    "mode": mode,
                    "frame": frame,
                    "current": ready["current"],
                    "previous": ready["previous"],
                }
                post_validation = validate_log_against_ready(log_pose, ready, marker)
                boundaries[marker] = {
                    "ready": ready,
                    "pose_validation": {
                        "pre_render": pre_validation,
                        "post_render": post_validation,
                    },
                }
            relationship_validation = validate_boundary_relationships(boundaries)
            if relationship_validation["control_still_enabled"] != capture_control_frame:
                raise SmokeFailure(f"control-still relationship self-test failed: {relationship_validation!r}")
        checks.append(
            "armed three-frame pose protocol"
            if not capture_control_frame
            else "armed optional consecutive control-still pose protocol"
        )

    reference_pose = {
        "position": [8.0, 1.5, 1.0],
        "yaw_degrees": 188.0,
        "pitch_degrees": 0.0,
    }
    within_rotation_tolerance = {
        **reference_pose,
        "position": list(reference_pose["position"]),
        "yaw_degrees": reference_pose["yaw_degrees"] + 1.017e-5,
    }
    assert_pose_close(
        within_rotation_tolerance,
        reference_pose,
        "rotation tolerance regression",
    )
    outside_rotation_tolerance = {
        **reference_pose,
        "position": list(reference_pose["position"]),
        "yaw_degrees": reference_pose["yaw_degrees"] + 2.1e-5,
    }
    try:
        assert_pose_close(
            outside_rotation_tolerance,
            reference_pose,
            "rotation tolerance upper bound",
        )
    except SmokeFailure:
        pass
    else:
        raise SmokeFailure("rotation tolerance upper-bound self-test unexpectedly passed")
    checks.append("separate position/rotation pose tolerances")

    with tempfile.TemporaryDirectory(prefix="mgif-arm-marker-self-test-") as temporary:
        sync_dir = Path(temporary)
        unexpected_path = sync_dir / "last-moving.ready.json"
        unexpected_path.write_text("{}\n", encoding="utf-8")
        try:
            wait_for_ready_marker(
                sync_dir / f"{ARM_LAST_MOVING}.ready.json",
                timeout=0.1,
                process_identity=argparse.Namespace(
                    pid=os.getpid(),
                    identity="self-test-current-process",
                    is_running=lambda: True,
                ),
                unexpected_ready_paths={"last-moving": unexpected_path},
            )
        except SmokeFailure as exc:
            if APP_ARM_PROTOCOL_REQUIREMENT not in str(exc):
                raise
        else:
            raise SmokeFailure("missing arm marker protocol self-test unexpectedly passed")
    checks.append("missing app arm marker fails explicitly")

    validate_config_line(
        "marker=config capture_sync=1 capture_control=0 no_post=1 taa=0 no_ddgi=1",
        "no-post",
        "no-ddgi",
        capture_control_frame=False,
    )
    validate_config_line(
        "marker=config capture_sync=1 capture_control=1 no_post=0 taa=1 no_ddgi=0",
        "taa-on",
        "ddgi-on",
        capture_control_frame=True,
    )
    checks.append("render/DDGI mode config validation")
    control_log = parse_pose_log(
        "[CSM_AUTOMATION] marker=control-still mode=csm-translate-stop frame=6 "
        "current_pos=(8.000000,1.500000,1.000000) current_yaw_pitch=(180.000000,0.000000) "
        "previous_pos=(8.000000,1.500000,1.000000) previous_yaw_pitch=(180.000000,0.000000)"
    )
    if control_log is None or control_log["marker"] != CONTROL_BOUNDARY:
        raise SmokeFailure(f"control-still pose log parser self-test failed: {control_log!r}")
    checks.append("control-still pose log parsing")

    canonical_repo = Path("C:/formal-source-self-test").resolve()
    canonical_args = argparse.Namespace(
        repo_root=canonical_repo,
        exe=(canonical_repo / "out/build/x64-debug/Demo.exe"),
        mode=list(MODES),
        render_mode=["taa-on"],
        gi_mode=["no-ddgi"],
        capture_control_frame=False,
        **FORMAL_RELEASE_SMOKE_SEQUENCE,
    )
    canonical_contract = validate_formal_source_executable_contract(canonical_args)
    if canonical_contract.get("applicable") is not True or canonical_contract.get("passed") is not True:
        raise SmokeFailure(
            f"canonical formal Demo.exe source contract failed: {canonical_contract!r}"
        )
    stale_args = argparse.Namespace(
        **{
            **vars(canonical_args),
            "exe": canonical_repo / "out/build/x64-debug/Demo.csm_shadow_reactive_test.exe",
        }
    )
    stale_contract = validate_formal_source_executable_contract(stale_args)
    if stale_contract.get("passed") is not False or not stale_contract.get("errors"):
        raise SmokeFailure(
            f"stale formal executable source unexpectedly passed: {stale_contract!r}"
        )
    checks.append("formal 8/24/8 source is canonical rebuilt Demo.exe before immutable SHA copy")
    print(f"self-test: passed ({len(checks)} checks)")
    for check in checks:
        print(f"  - {check}")
    return 0


def validate_formal_source_executable_contract(
    args: argparse.Namespace,
) -> dict[str, Any]:
    canonical = (args.repo_root / "out" / "build" / "x64-debug" / "Demo.exe").resolve()
    requested = (
        set(args.mode) == set(MODES)
        and len(args.mode) == len(MODES)
        and len(args.render_mode) == 1
        and args.render_mode[0] in RENDER_MODES
        and args.gi_mode == ["no-ddgi"]
        and args.capture_control_frame is False
        and all(
            int(getattr(args, field)) == expected
            for field, expected in FORMAL_RELEASE_SMOKE_SEQUENCE.items()
        )
    )
    selected = args.exe.resolve()
    checks = {
        "canonical_source_path": _process_images_match(selected, canonical),
        "canonical_filename": selected.name == "Demo.exe",
        "stale_reactive_test_name_rejected": (
            selected.name.casefold() != "demo.csm_shadow_reactive_test.exe"
        ),
    }
    errors: list[str] = []
    if requested:
        if checks["canonical_source_path"] is not True:
            errors.append(
                "formal 8/24/8 release must source the rebuilt "
                f"{canonical}; selected {selected}"
            )
        if checks["canonical_filename"] is not True:
            errors.append("formal release source executable must be named Demo.exe")
        if checks["stale_reactive_test_name_rejected"] is not True:
            errors.append(
                "stale Demo.csm_shadow_reactive_test.exe is forbidden for formal release"
            )
    return {
        "schema": "mgif-formal-source-executable-contract-v1",
        "applicable": requested,
        "canonical_source_executable": str(canonical),
        "selected_source_executable": str(selected),
        "immutable_copy_required": requested,
        "checks": checks,
        "errors": errors,
        "passed": not errors,
    }

def parse_args() -> argparse.Namespace:
    default_repo_root = Path(__file__).resolve().parents[1]
    parser = argparse.ArgumentParser(
        description=(
            "Launch Demo suspended, bind its CreateProcess handle, inject RenderDoc, and keep one TargetControl "
            "connection alive, arm captures before target frames, and capture the exact "
            "last-moving, first-still, and settled CSM automation boundaries."
        ),
        formatter_class=argparse.ArgumentDefaultsHelpFormatter,
    )
    parser.add_argument("--repo-root", type=Path, default=default_repo_root)
    parser.add_argument("--exe", type=Path, default=Path("out/build/x64-debug/Demo.exe"))
    parser.add_argument("--rdc", default="rdc")
    parser.add_argument("--output-dir", type=Path)
    parser.add_argument("--mode", choices=MODES, action="append")
    parser.add_argument("--render-mode", choices=RENDER_MODES, action="append")
    parser.add_argument("--gi-mode", choices=GI_MODES, action="append")
    parser.add_argument("--fixed-dt", type=float, default=1.0 / 60.0)
    parser.add_argument("--warmup-frames", type=int, default=1)
    parser.add_argument("--motion-frames", type=int, default=2)
    parser.add_argument("--hold-frames", type=int, default=MIN_HOLD_FRAMES)
    parser.add_argument(
        "--capture-control-frame",
        action="store_true",
        help="Capture an optional fourth, consecutive still frame immediately after settled.",
    )
    parser.add_argument("--launch-timeout", type=float, default=30.0)
    parser.add_argument("--startup-timeout", type=float, default=180.0)
    parser.add_argument("--boundary-timeout", type=float, default=60.0)
    parser.add_argument("--handshake-timeout", type=float, default=30.0)
    parser.add_argument("--trigger-timeout", type=float, default=15.0)
    parser.add_argument("--capture-timeout", type=float, default=120.0)
    parser.add_argument("--replay-timeout", type=float, default=120.0)
    parser.add_argument("--extra-app-arg", action="append", default=[])
    parser.add_argument(
        "--self-test",
        action="store_true",
        help="Run deterministic protocol/controller tests without launching Demo or RenderDoc.",
    )
    args = parser.parse_args()

    args.repo_root = args.repo_root.resolve()
    if not args.exe.is_absolute():
        args.exe = (args.repo_root / args.exe).resolve()
    else:
        args.exe = args.exe.resolve()
    args.mode = dedupe(args.mode or ["csm-translate-stop"])
    args.render_mode = dedupe(args.render_mode or ["no-post"])
    args.gi_mode = dedupe(args.gi_mode or ["no-ddgi"])

    for name in ("warmup_frames", "motion_frames"):
        if getattr(args, name) <= 0:
            parser.error(f"--{name.replace('_', '-')} must be greater than zero")
    try:
        validate_hold_frames(args.hold_frames)
    except SmokeFailure as exc:
        parser.error(str(exc))
    if not math.isfinite(args.fixed_dt) or args.fixed_dt <= 0.0:
        parser.error("--fixed-dt must be a positive finite number")
    for name in (
        "launch_timeout",
        "startup_timeout",
        "boundary_timeout",
        "handshake_timeout",
        "trigger_timeout",
        "capture_timeout",
        "replay_timeout",
    ):
        if not math.isfinite(getattr(args, name)) or getattr(args, name) <= 0.0:
            parser.error(f"--{name.replace('_', '-')} must be a positive finite number")
    if args.trigger_timeout > args.handshake_timeout:
        parser.error("--trigger-timeout must not exceed --handshake-timeout")
    app_sync_timeout = app_capture_sync_timeout_seconds(
        handshake_timeout=args.handshake_timeout,
        boundary_timeout=args.boundary_timeout,
        capture_timeout=args.capture_timeout,
    )
    if app_sync_timeout * 1000.0 > 4294967295.0:
        parser.error("timeout budgets are too large for Demo's millisecond timeout option")
    return args


def main() -> int:
    args = parse_args()
    if args.self_test:
        return run_self_tests()
    if not args.repo_root.is_dir():
        raise SmokeFailure(f"repository root does not exist: {args.repo_root}")
    formal_source_contract = validate_formal_source_executable_contract(args)
    if formal_source_contract.get("passed") is not True:
        raise SmokeFailure("; ".join(formal_source_contract["errors"]))
    if not args.exe.is_file():
        raise SmokeFailure(f"Demo executable does not exist: {args.exe}")
    if shutil.which(args.rdc) is None and not Path(args.rdc).is_file():
        raise SmokeFailure(f"rdc executable was not found: {args.rdc}")

    if args.output_dir is None:
        run_root = Path(tempfile.gettempdir()) / "mgif-csm-shadow-motion" / utc_stamp()
    else:
        run_root = args.output_dir.resolve()
    run_root.mkdir(parents=True, exist_ok=True)
    manifest_path = run_root / "manifest.json"
    source_executable = args.exe
    boundary_order = selected_boundaries(args.capture_control_frame)
    case_count = len(args.mode) * len(args.render_mode) * len(args.gi_mode)
    disk_preflight = smoke_disk_space_preflight(
        run_root,
        case_count=case_count,
        boundary_count=len(boundary_order),
        executable_size_bytes=source_executable.stat().st_size,
    )
    require_smoke_disk_space(disk_preflight)
    capture_copy_budget = CaptureCopyBudget(
        run_root,
        case_count=case_count,
        boundary_count=len(boundary_order),
        disk_preflight=disk_preflight,
    )
    args.capture_copy_budget = capture_copy_budget
    toolchain_before = collect_toolchain_snapshot(args.repo_root)
    executable_binding, immutable_executable_lock = prepare_immutable_executable_binding(
        source_executable,
        run_root,
    )
    args.source_exe = source_executable
    args.launch_exe = Path(executable_binding["launch_image"]["resolved_path"])
    args.launch_cwd = source_executable.parent
    args.immutable_executable_lock = immutable_executable_lock
    args.immutable_executable_lock_identity = immutable_executable_lock.metadata()[
        "lock_identity"
    ]
    args.source_executable_evidence_baseline = executable_binding["source"][
        "before_copy"
    ]
    args.launch_executable_evidence_baseline = executable_binding["launch_image"][
        "baseline"
    ]
    protocol_steps = capture_protocol_steps(
        args.warmup_frames,
        args.motion_frames,
        args.hold_frames,
        capture_control_frame=args.capture_control_frame,
    )

    rdc_resources_before = snapshot_rdc_resources()
    manifest: dict[str, Any] = {
        "schema": "mgif-csm-shadow-motion-smoke-v1",
        "tool_version": TOOL_VERSION,
        "authoritative_date": AUTHORITATIVE_DATE,
        "started_utc": utc_now(),
        "status": "running",
        "repo_root": str(args.repo_root),
        "executable": str(args.source_exe),
        "launch_executable": str(args.launch_exe),
        "executable_evidence": executable_binding,
        "formal_source_executable_contract": formal_source_contract,
        "toolchain_evidence": {
            "schema": TOOLCHAIN_EVIDENCE_SCHEMA,
            "required": True,
            "before_all_cases": toolchain_before,
            "after_all_cases": None,
            "comparison": None,
            "errors": [],
            "passed": False,
        },
        "disk_preflight": disk_preflight,
        "capture_copy_budget": capture_copy_budget.snapshot(),
        "rdc": args.rdc,
        "output_directory": str(run_root),
        "manifest_path": str(manifest_path),
        "boundary_order": list(boundary_order),
        "arm_order": [step["marker"] for step in protocol_steps if step["kind"] == "arm"],
        "capture_protocol_steps": protocol_steps,
        "app_protocol_requirement": APP_ARM_PROTOCOL_REQUIREMENT,
        "rdc_resources_before": rdc_resources_before,
        "capture_control": {
            "launch": "CreateProcess(CREATE_SUSPENDED) + launch-owned duplicated handle + renderdoc.InjectIntoProcess",
            "module_discovery": "rdc.discover.find_renderdoc()",
            "connection": "renderdoc.CreateTargetControl('', ident, client_name, True)",
            "connection_lifetime": "one persistent TargetControl connection per case",
            "trigger": (
                "TargetControl.TriggerCapture(2) at arm-last-moving; "
                "TargetControl.TriggerCapture(1) at arm-settled, or 2 when control-still is enabled"
            ),
            "notification": "same TargetControl.ReceiveMessage(NewCapture)",
            "copy": "same TargetControl.CopyCapture followed by ReceiveMessage(CaptureCopied)",
            "batch_binding": (
                "each TriggerCapture creates an epoch with a frozen pre-trigger notification/id "
                "baseline and an explicit expected-boundary ordinal sequence"
            ),
            "candidate_isolation": (
                "every post-baseline NewCapture candidate is copied to quarantine and replayed; "
                "a mismatched marker is recorded and cannot advance or steal the current ordinal"
            ),
            "wait_pump": (
                "OutputCollector.wait_for_value, ready-marker waits, and replay subprocess waits "
                "call TargetControl.ReceiveMessage on the owning thread"
            ),
            "frame_validation": (
                "isolated rdc open + marker-only replay requires one strict CSM_AUTOMATION_FRAME "
                "nested in GPUDrivenCSMShadow before validated_target_frame becomes true"
            ),
            "replay_cleanup": (
                "direct state-token shutdown followed by held-handle daemon absence, exact state-file "
                "absence, and before/after owned daemon/session diff for every candidate replay"
            ),
            "daemon_ownership": (
                "only the named session state-file PID plus one continuously held native process "
                "handle with exact creation-time identity; that identity must be absent before open"
            ),
            "capture_path_role": (
                "state/session and daemon capture paths must both match before ownership is "
                "established; path equality alone never establishes identity or authorizes kill"
            ),
            "close_failure_recovery": (
                "only the originally held daemon handle may be terminated and waited; an exited "
                "original is accepted even if its PID was reused, with no PID/tree fallback"
            ),
            "target_cleanup": (
                "the Demo PID is immediately converted to a held native process identity whose "
                "image matches executable evidence; cleanup terminates and waits on that same handle"
            ),
            "executable_binding": (
                "the source Demo is copied while deny-write/delete locked into a run-local "
                "SHA-named image; that image remains locked through every CreateProcess and "
                "target-image binding, while source and launch hashes are rechecked after all cases"
            ),
            "persistent_connection_per_case": True,
            "same_frame_trigger_assumed": False,
            "fifo_only_binding": False,
            "capture_id_rule": "captureId is valid when >= 0",
            "minimum_hold_frames": MIN_HOLD_FRAMES,
        },
        "options": {
            "modes": args.mode,
            "render_modes": args.render_mode,
            "gi_modes": args.gi_mode,
            "fixed_dt": args.fixed_dt,
            "warmup_frames": args.warmup_frames,
            "motion_frames": args.motion_frames,
            "hold_frames": args.hold_frames,
            "capture_control_frame": args.capture_control_frame,
            "launch_timeout": args.launch_timeout,
            "startup_timeout": args.startup_timeout,
            "boundary_timeout": args.boundary_timeout,
            "handshake_timeout": args.handshake_timeout,
            "trigger_timeout": args.trigger_timeout,
            "capture_timeout": args.capture_timeout,
            "replay_timeout": args.replay_timeout,
            "candidate_total_timeout_seconds": args.capture_timeout + args.replay_timeout,
            "app_capture_sync_timeout_seconds": app_capture_sync_timeout_seconds(
                handshake_timeout=args.handshake_timeout,
                boundary_timeout=args.boundary_timeout,
                capture_timeout=args.capture_timeout,
            ),
            "extra_app_args": args.extra_app_arg,
        },
        "cases": [],
    }
    atomic_write_json(manifest_path, manifest)

    try:
        doctor = run_command([args.rdc, "doctor"], cwd=args.repo_root, timeout=30.0)
        manifest["rdc_doctor"] = {
            "returncode": doctor.returncode,
            "stdout": doctor.stdout.strip(),
            "stderr": doctor.stderr.strip(),
        }
        status_before = run_command([args.rdc, "status"], cwd=args.repo_root, timeout=10.0, check=False)
        manifest["rdc_status_before"] = {
            "returncode": status_before.returncode,
            "stdout": status_before.stdout.strip(),
            "stderr": status_before.stderr.strip(),
        }
        atomic_write_json(manifest_path, manifest)

        for mode in args.mode:
            for render_mode in args.render_mode:
                for gi_mode in args.gi_mode:
                    case = run_case(args, mode, render_mode, gi_mode, run_root)
                    case["toolchain_bundle_sha256"] = toolchain_before[
                        "bundle_sha256"
                    ]
                    manifest["cases"].append(case)
                    manifest["capture_copy_budget"] = capture_copy_budget.snapshot()
                    manifest["executable_evidence"]["case_checks"].append(
                        {
                            "case": case["name"],
                            **case.get("executable_evidence", {}),
                        }
                    )
                    atomic_write_json(manifest_path, manifest)
                    if case.get("executable_evidence", {}).get("fail_closed") is True:
                        raise SmokeFailure(
                            f"executable evidence failed closed for case {case['name']}: "
                            f"{case.get('error', case['executable_evidence'])}"
                        )

        manifest["cross_case_pose_validation"] = validate_cross_case_poses(manifest["cases"])
        status_after = run_command([args.rdc, "status"], cwd=args.repo_root, timeout=10.0, check=False)
        manifest["rdc_status_after"] = {
            "returncode": status_after.returncode,
            "stdout": status_after.stdout.strip(),
            "stderr": status_after.stderr.strip(),
        }
        manifest["status"] = (
            "passed"
            if all(case.get("status") == "passed" for case in manifest["cases"])
            else "failed"
        )
    except Exception as exc:
        manifest["status"] = "failed"
        manifest["error"] = str(exc)
    finally:
        toolchain_record = manifest["toolchain_evidence"]
        try:
            toolchain_after = collect_toolchain_snapshot(args.repo_root)
            toolchain_comparison = compare_toolchain_snapshots(
                toolchain_before,
                toolchain_after,
            )
            toolchain_record["after_all_cases"] = toolchain_after
            toolchain_record["comparison"] = toolchain_comparison
            toolchain_record["errors"] = list(toolchain_comparison.get("errors", []))
            toolchain_record["passed"] = toolchain_comparison.get("passed") is True
        except Exception as toolchain_exc:
            toolchain_record["errors"].append(
                "post-run toolchain evidence collection failed closed: "
                f"{type(toolchain_exc).__name__}: {toolchain_exc}"
            )
            toolchain_record["passed"] = False
        if toolchain_record["passed"] is not True:
            manifest["status"] = "failed"
            manifest["toolchain_evidence_error"] = "; ".join(
                toolchain_record["errors"]
                or ["harness/comparator/ROI toolchain binding failed"]
            )

        executable_binding = manifest["executable_evidence"]
        source_binding = executable_binding["source"]
        launch_binding = executable_binding["launch_image"]
        try:
            source_after = collect_executable_evidence(args.source_exe)
            source_after_comparison = compare_executable_evidence(
                args.source_executable_evidence_baseline,
                source_after,
                stage="source-after-all-cases",
            )
            source_binding["after_all_cases"] = source_after
            source_binding["after_all_cases_comparison"] = source_after_comparison
            if source_after_comparison["passed"] is not True:
                executable_binding["errors"].append(
                    "post-run source executable evidence differs from the run baseline: "
                    f"{source_after_comparison['errors']}"
                )

            launch_after = immutable_executable_lock.collect_evidence()
            launch_after_comparison = compare_executable_evidence(
                args.launch_executable_evidence_baseline,
                launch_after,
                stage="immutable-launch-after-all-cases",
            )
            launch_binding["after_all_cases"] = launch_after
            launch_binding["after_all_cases_comparison"] = launch_after_comparison
            launch_binding["immutable_lock_after_all_cases"] = (
                immutable_executable_lock.metadata()
            )
            if launch_after_comparison["passed"] is not True:
                executable_binding["errors"].append(
                    "post-run immutable launch image differs from the run baseline: "
                    f"{launch_after_comparison['errors']}"
                )
        except Exception as evidence_exc:
            executable_binding["errors"].append(
                "post-run executable evidence collection failed closed: "
                f"{type(evidence_exc).__name__}: {evidence_exc}"
            )
        finally:
            try:
                launch_lock_close = immutable_executable_lock.close()
            except Exception as lock_close_exc:
                launch_lock_close = {
                    "closed": False,
                    "error": f"{type(lock_close_exc).__name__}: {lock_close_exc}",
                }
            launch_binding["lock_close_after_all_cases"] = launch_lock_close
            if launch_lock_close.get("closed") is not True:
                executable_binding["errors"].append(
                    f"immutable launch image lock did not close: {launch_lock_close!r}"
                )
        executable_binding["passed"] = (
            not executable_binding["errors"]
            and source_binding.get("after_all_cases_comparison", {}).get("passed") is True
            and launch_binding.get("after_all_cases_comparison", {}).get("passed") is True
            and launch_binding.get("source_copy_comparison", {}).get("passed") is True
            and launch_binding.get("immutable_lock", {}).get("native_handle_held") is True
            and launch_binding.get("immutable_lock", {}).get("write_share_denied") is True
            and launch_binding.get("immutable_lock", {}).get("delete_share_denied") is True
            and launch_binding.get("lock_close_after_all_cases", {}).get("closed") is True
            and all(
                check.get("passed") is True and check.get("fail_closed") is not True
                for check in executable_binding["case_checks"]
            )
        )
        if executable_binding["passed"] is not True:
            manifest["status"] = "failed"
            manifest["executable_evidence_error"] = "; ".join(
                executable_binding["errors"]
                or ["one or more immutable executable binding checks failed"]
            )

        try:
            rdc_resources_after = snapshot_rdc_resources()
            manifest["rdc_resources_after"] = rdc_resources_after
            manifest["rdc_session_cleanup"] = aggregate_rdc_session_cleanup(
                manifest["cases"],
                rdc_resources_before,
                rdc_resources_after,
            )
        except Exception as cleanup_exc:
            manifest["rdc_session_cleanup"] = {
                "opened_by_script": None,
                "closed": False,
                "passed": False,
                "errors": [f"cleanup aggregation failed: {cleanup_exc}"],
            }
        if manifest["rdc_session_cleanup"].get("passed") is not True:
            manifest["status"] = "failed"
            manifest["cleanup_error"] = "; ".join(
                manifest["rdc_session_cleanup"].get("errors", [])
            )
        manifest["capture_copy_budget"] = capture_copy_budget.snapshot()
        if (
            manifest["capture_copy_budget"].get("within_count_budget") is not True
            or manifest["capture_copy_budget"].get("within_actual_byte_budget") is not True
        ):
            manifest["status"] = "failed"
            manifest["capture_copy_budget_error"] = (
                "capture candidate count/byte budget was exceeded"
            )
        try:
            capture_set_validation = validate_final_capture_set(
                manifest,
                manifest_path,
            )
        except Exception as capture_set_exc:
            capture_set_validation = {
                "schema": FINAL_CAPTURE_SET_SCHEMA,
                "passed": False,
                "errors": [
                    "final capture-set validation raised: "
                    f"{type(capture_set_exc).__name__}: {capture_set_exc}"
                ],
            }
        manifest["capture_set_validation"] = capture_set_validation
        if capture_set_validation.get("passed") is not True:
            manifest["status"] = "failed"
            manifest["capture_set_error"] = "; ".join(
                str(error) for error in capture_set_validation.get("errors", [])
            )
        manifest["completed_utc"] = utc_now()
        atomic_write_json(manifest_path, manifest)

    print(f"\nmanifest: {manifest_path.resolve()}")
    for case in manifest["cases"]:
        for boundary in case.get("boundary_order", boundary_order):
            capture_path = case.get("boundaries", {}).get(boundary, {}).get("capture_path")
            if capture_path:
                print(f"capture {case['name']} {boundary}: {capture_path}")
    print(f"status: {manifest['status']}")
    return 0 if manifest["status"] == "passed" else 1


def rdc_data_directory() -> Path:
    override = os.environ.get("RDC_DATA_DIR")
    if override:
        return Path(override).expanduser().resolve()
    if os.name == "nt":
        base = os.environ.get("LOCALAPPDATA", str(Path.home()))
        return (Path(base) / "rdc").resolve()
    return (Path.home() / ".rdc").resolve()


def rdc_session_state_path(session: str) -> Path:
    if re.fullmatch(r"[A-Za-z0-9_-]{1,64}", session) is None:
        raise SmokeFailure(f"invalid rdc session name: {session!r}")
    return rdc_data_directory() / "sessions" / f"{session}.json"


def _filetime_ticks(value: Any) -> int:
    return (int(value.dwHighDateTime) << 32) | int(value.dwLowDateTime)


def _state_file_handle_snapshot(handle: Any) -> dict[str, Any]:
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

    last_write = _FILETIME()
    ctypes.set_last_error(0)
    if not _KERNEL32.GetFileTime(
        native_handle, None, None, ctypes.byref(last_write)
    ):
        result["error"] = (
            "GetFileTime failed for held state-file handle: "
            f"winerror={ctypes.get_last_error()}"
        )
        return result
    filetime_ticks = _filetime_ticks(last_write)
    if filetime_ticks <= WINDOWS_FILETIME_EPOCH_OFFSET_TICKS:
        result["error"] = "held state-file last-write FILETIME is invalid"
        return result

    ctypes.set_last_error(0)
    required_chars = int(
        _KERNEL32.GetFinalPathNameByHandleW(
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
        _KERNEL32.GetFinalPathNameByHandleW(
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
    if not _KERNEL32.GetVolumeInformationByHandleW(
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
    drive_type_code = int(_KERNEL32.GetDriveTypeW(volume_guid_root))
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


def _evaluate_state_file_handle_policy(
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


def _state_publication_boundary_evidence(
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
    policy = _evaluate_state_file_handle_policy(before_snapshot, after_snapshot)
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

def _read_rdc_session_state(
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
            handle_snapshot_before = _state_file_handle_snapshot(handle)
            payload_bytes = handle.read()
            stat_after = os.fstat(handle.fileno())
            handle_snapshot_after = _state_file_handle_snapshot(handle)
            publication_boundary = _state_publication_boundary_evidence(
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
            raise SmokeFailure(
                f"named session state changed while reading {path}: {changed}"
            )
        record["publication_boundary"] = publication_boundary
        if publication_boundary.get("exact") is not True:
            raise SmokeFailure(
                "named session state has no exact publication boundary: "
                f"{publication_boundary.get('error', 'unknown error')}"
            )
        decoded = json.loads(payload_bytes.decode("utf-8-sig"))
        if not isinstance(decoded, dict):
            raise SmokeFailure("session state is not a JSON object")
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


def rdc_session_state_record(path: Path) -> dict[str, Any]:
    record, _ = _read_rdc_session_state(path)
    return record


def _rdc_state_fingerprint(record: dict[str, Any]) -> dict[str, Any] | None:
    fingerprint = record.get("fingerprint") if isinstance(record, dict) else None
    if not isinstance(fingerprint, dict):
        return None
    required = ("device", "inode", "size_bytes", "modified_ns", "sha256")
    if any(key not in fingerprint for key in required):
        return None
    return {key: fingerprint[key] for key in required}


def load_exact_rdc_shutdown_credentials(
    path: Path,
    *,
    expected_record: dict[str, Any],
) -> dict[str, Any]:
    observed, payload = _read_rdc_session_state(path)
    expected_fingerprint = _rdc_state_fingerprint(expected_record)
    observed_fingerprint = _rdc_state_fingerprint(observed)
    if (
        expected_record.get("exists") is not True
        or expected_record.get("valid") is not True
        or expected_record.get("read_consistent") is not True
        or expected_fingerprint is None
    ):
        raise SmokeFailure("expected named-session state evidence is incomplete")
    if (
        observed.get("exists") is not True
        or observed.get("valid") is not True
        or observed.get("read_consistent") is not True
        or observed_fingerprint != expected_fingerprint
        or not isinstance(payload, dict)
    ):
        raise SmokeFailure(
            "named-session state identity changed before direct token shutdown"
        )
    token = payload.get("token")
    state = observed.get("state")
    if not isinstance(token, str) or not token or not isinstance(state, dict):
        raise SmokeFailure("named-session shutdown token/state is unavailable")
    return {
        "host": str(state["host"]),
        "port": int(state["port"]),
        "pid": int(state["pid"]),
        "capture": state.get("capture"),
        "token": token,
        "token_sha256": observed.get("token_sha256"),
        "fingerprint": observed_fingerprint,
    }


def remove_exact_rdc_session_state(
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
    observed = rdc_session_state_record(path)
    if observed.get("exists") is not True:
        result.update({"already_absent": True, "passed": True})
        return result
    if _rdc_state_fingerprint(observed) != _rdc_state_fingerprint(expected_record):
        result["error"] = (
            "named-session state identity changed; refusing to remove a replacement file"
        )
        return result
    try:
        path.unlink()
    except Exception as exc:
        result["error"] = f"exact state-file removal failed: {type(exc).__name__}: {exc}"
        return result
    after = rdc_session_state_record(path)
    result["after"] = after
    result["removed"] = after.get("exists") is False
    result["passed"] = result["removed"]
    if not result["passed"]:
        result["error"] = "exact named-session state file still exists after unlink"
    return result


def wait_for_owned_process_exit(
    process_identity: Any,
    *,
    timeout: float,
    pump: Callable[[], Any] | None = None,
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
        if pump is not None:
            try:
                pump()
            except Exception as exc:
                result["error"] = f"cleanup pump failed: {type(exc).__name__}: {exc}"
                return result
        remaining = deadline - time.monotonic()
        if remaining <= 0.0:
            result.update(
                {
                    "timed_out": True,
                    "running_after": True,
                    "error": "timed out waiting on the held process handle",
                }
            )
            return result
        time.sleep(min(0.02, remaining))


def shutdown_owned_rdc_session_direct(
    *,
    state_path: Path,
    state_after_open: dict[str, Any],
    process_identity: Any,
    ownership: dict[str, Any],
    timeout: float,
    pump: Callable[[], Any] | None = None,
    send_request_fn: Callable[..., dict[str, Any]] | None = None,
    shutdown_request_fn: Callable[..., dict[str, Any]] | None = None,
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
        errors.append("daemon ownership is not established without errors")
        return result
    try:
        metadata = process_identity.metadata()
    except Exception as exc:
        errors.append(f"stable daemon metadata failed: {type(exc).__name__}: {exc}")
        return result
    result["stable_process_identity"] = metadata
    state = state_after_open.get("state")
    if (
        metadata.get("native_handle_held") is not True
        or metadata.get("terminate_access") is not True
        or not isinstance(state, dict)
        or int(state.get("pid", 0) or 0) != int(metadata.get("pid", 0) or 0)
        or str(ownership.get("identity", "")) != str(metadata.get("identity", ""))
    ):
        errors.append(
            "direct shutdown lacks one exact owned daemon handle/state identity binding"
        )
        return result
    try:
        credentials = load_exact_rdc_shutdown_credentials(
            state_path,
            expected_record=state_after_open,
        )
    except Exception as exc:
        errors.append(f"shutdown credential binding failed: {type(exc).__name__}: {exc}")
        return result
    if credentials["pid"] != int(metadata["pid"]):
        errors.append("shutdown credential PID differs from the held daemon handle")
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
            shutdown_request_fn(credentials["token"], request_id=91),
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
    graceful_wait = wait_for_owned_process_exit(
        process_identity,
        timeout=min(max(0.01, remaining), max(0.05, timeout * 0.30)),
        pump=pump,
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
                "owned daemon did not exit through direct token shutdown or its held handle"
            )
    result["same_handle_recovery"] = recovery
    try:
        result["owned_daemon_absent"] = process_identity.is_running() is False
    except Exception as exc:
        errors.append(f"final held-handle liveness check failed: {type(exc).__name__}: {exc}")
    if result["owned_daemon_absent"] is not True:
        errors.append("owned daemon remains present on its held process handle")
    if result["owned_daemon_absent"] is True:
        state_removal = remove_exact_rdc_session_state(
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
    final_state = rdc_session_state_record(state_path)
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
        "basis": "held daemon handle absent plus exact state file absent",
        "subprocess_used": False,
    }
    if graceful_error is not None and recovery.get("passed") is not True:
        errors.append(graceful_error)
    result["passed"] = not errors and result["post_status"]["inactive"] is True
    return result

def _redact_rdc_daemon_command(command: list[str]) -> list[str]:
    redacted = list(command)
    for index, token in enumerate(redacted):
        if token == "--token" and index + 1 < len(redacted):
            redacted[index + 1] = "<redacted>"
        elif token.startswith("--token="):
            redacted[index] = "--token=<redacted>"
    return redacted


def _rdc_daemon_capture(command: list[str]) -> str | None:
    for index, token in enumerate(command):
        if token == "--capture" and index + 1 < len(command):
            return command[index + 1]
        if token.startswith("--capture="):
            return token.split("=", 1)[1]
    return None


def rdc_daemon_identity(pid: int, create_time: float) -> str:
    return f"{int(pid)}@unix:{float(create_time)!r}"


def inspect_process_identity(pid: int) -> dict[str, Any] | None:
    if pid <= 0:
        return None
    try:
        import psutil
    except ImportError as exc:
        raise SmokeFailure("psutil is required for rdc daemon metadata inspection") from exc

    stable_process: StableProcessIdentity | None = None
    try:
        stable_process = acquire_stable_process_identity(
            pid,
            require_terminate=False,
        )
        process = psutil.Process(pid)
        create_time_before = float(process.create_time())
        command = [str(token) for token in process.cmdline()]
        name = str(process.name() or "")
        create_time_after = float(process.create_time())
        if create_time_before != create_time_after:
            raise SmokeFailure(
                f"process {pid} creation time changed while collecting metadata"
            )
        if not stable_process.creation_time_matches(create_time_before):
            raise SmokeFailure(
                f"process {pid} metadata does not match its stable native-handle creation time"
            )
        if not stable_process.is_running():
            return None
        record = stable_process.metadata()
        record.update(
            {
                "create_time": create_time_before,
                "name": name,
                "is_rdc_daemon": "rdc.daemon_server" in command,
                "capture": _rdc_daemon_capture(command),
                "command": _redact_rdc_daemon_command(command),
                "identity_source": "transient stable native handle",
                "native_handle_verified_during_inspection": True,
                "native_handle_held": False,
            }
        )
        return record
    except (psutil.NoSuchProcess, psutil.ZombieProcess):
        return None
    finally:
        if stable_process is not None:
            close_result = stable_process.close()
            if close_result.get("closed") is not True and sys.exc_info()[0] is None:
                raise SmokeFailure(
                    f"failed to close transient process identity handle: {close_result!r}"
                )

def determine_named_session_daemon_ownership(
    state_after_open: dict[str, Any],
    resources_before: dict[str, Any],
    resources_after_open: dict[str, Any],
    expected_state_path: Path,
    capture: Path,
    stable_process_identity: dict[str, Any] | None,
) -> dict[str, Any]:
    result: dict[str, Any] = {
        "established": False,
        "basis": (
            "exact named-session state path/capture plus state PID, one held native "
            "process handle, exact creation FILETIME strictly earlier than the exact state "
            "publication FILETIME with zero positive tolerance, and matching image/cmdline"
        ),
        "state_pid": None,
        "identity": None,
        "daemon": None,
        "stable_process_identity": stable_process_identity,
        "state_path_match": False,
        "state_capture_path_match": False,
        "daemon_capture_path_metadata_match": False,
        "stable_image_match": False,
        "state_publication_boundary_exact": False,
        "state_file_volume_verified": False,
        "exact_creation_order_clock_match": False,
        "strict_creation_precedes_publication": False,
        "creation_equals_publication": False,
        "process_created_before_state_file": False,
        "snapshot_evidence_complete": False,
        "errors": [],
    }
    errors = result["errors"]
    for label, snapshot in (
        ("before-open", resources_before),
        ("after-open", resources_after_open),
    ):
        if not isinstance(snapshot, dict) or snapshot.get("available") is not True:
            errors.append(f"{label} daemon/session snapshot is unavailable")
            continue
        snapshot_errors = snapshot.get("errors")
        if not isinstance(snapshot_errors, list) or snapshot_errors:
            errors.append(f"{label} daemon/session snapshot has errors: {snapshot_errors!r}")
        access_denied = snapshot.get("process_access_denied_count")
        if not isinstance(access_denied, int) or isinstance(access_denied, bool):
            errors.append(f"{label} snapshot has no integer process_access_denied_count")
        elif access_denied != 0:
            errors.append(
                f"{label} snapshot encountered {access_denied} access-denied process(es)"
            )
    if errors:
        return result
    result["snapshot_evidence_complete"] = True

    if state_after_open.get("exists") is not True or state_after_open.get("valid") is not True:
        errors.append("named session state file is missing or invalid after open")
        return result
    observed_state_path = state_after_open.get("path")
    if not isinstance(observed_state_path, str) or not observed_state_path.strip():
        errors.append("named session state record has no canonical path")
        return result
    try:
        result["state_path_match"] = (
            normalized_absolute_path(observed_state_path)
            == normalized_absolute_path(expected_state_path)
        )
    except (OSError, RuntimeError, ValueError) as exc:
        errors.append(f"named session state path could not be normalized: {exc}")
        return result
    if result["state_path_match"] is not True:
        errors.append(
            "named session state path does not match the allocated session path"
        )
        return result

    matching_session_records = []
    for record in resources_after_open.get("sessions", {}).values():
        if not isinstance(record, dict):
            continue
        record_path = record.get("path")
        if not isinstance(record_path, str) or not record_path.strip():
            continue
        try:
            if normalized_absolute_path(record_path) == normalized_absolute_path(
                expected_state_path
            ):
                matching_session_records.append(record)
        except (OSError, RuntimeError, ValueError):
            continue
    if len(matching_session_records) != 1:
        errors.append(
            "after-open resource snapshot did not contain exactly one allocated "
            f"session state record (found {len(matching_session_records)})"
        )
        return result

    state = state_after_open.get("state")
    if not isinstance(state, dict):
        errors.append("named session state file has no parsed state object")
        return result
    try:
        state_pid = int(state.get("pid", 0))
    except (TypeError, ValueError):
        state_pid = 0
    result["state_pid"] = state_pid
    if state_pid <= 0:
        errors.append("named session state file has no positive daemon pid")
        return result

    state_capture = state.get("capture")
    if not isinstance(state_capture, str) or not state_capture.strip():
        errors.append("named session state file has no capture path")
        return result
    try:
        result["state_capture_path_match"] = (
            normalized_absolute_path(state_capture) == normalized_absolute_path(capture)
        )
    except (OSError, RuntimeError, ValueError) as exc:
        errors.append(f"named session capture path could not be normalized: {exc}")
        return result
    if result["state_capture_path_match"] is not True:
        errors.append("named session state capture path does not match replay capture")
        return result

    if not isinstance(stable_process_identity, dict):
        errors.append("named session daemon has no held stable native process identity")
        return result
    if stable_process_identity.get("native_handle_held") is not True:
        errors.append(
            "named session daemon stable process handle was not held during ownership validation"
        )
        return result
    if stable_process_identity.get("terminate_access") is not True:
        errors.append("named session daemon stable process handle lacks terminate access")
        return result
    try:
        stable_pid = int(stable_process_identity.get("pid", 0))
    except (TypeError, ValueError):
        stable_pid = 0
    if stable_pid != state_pid:
        errors.append(
            f"named session state pid {state_pid} does not match held handle pid {stable_pid}"
        )
        return result
    identity = str(stable_process_identity.get("identity", ""))
    if not identity or not stable_process_identity.get("creation_time_key"):
        errors.append("held daemon process identity has no exact creation-time key")
        return result
    creation_ticks = stable_process_identity.get("creation_filetime_ticks")
    creation_unix_ns = stable_process_identity.get("creation_time_unix_ns")
    publication_boundary = state_after_open.get("publication_boundary")
    publication_volume = (
        publication_boundary.get("volume")
        if isinstance(publication_boundary, dict)
        else None
    )
    if (
        isinstance(creation_ticks, bool)
        or not isinstance(creation_ticks, int)
        or creation_ticks <= WINDOWS_FILETIME_EPOCH_OFFSET_TICKS
        or stable_process_identity.get("creation_time_key")
        != f"winfiletime:{creation_ticks}"
    ):
        errors.append("held daemon has no exact Windows creation FILETIME identity")
        return result
    expected_creation_unix_ns = (
        creation_ticks - WINDOWS_FILETIME_EPOCH_OFFSET_TICKS
    ) * WINDOWS_FILETIME_TICK_NS
    if creation_unix_ns != expected_creation_unix_ns:
        errors.append("held daemon creation nanoseconds do not match its exact FILETIME")
        return result
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
        or publication_boundary.get("positive_post_state_tolerance_ticks") != 0
        or publication_boundary.get("modified_ns")
        != state_after_open.get("modified_ns")
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
    ):
        errors.append(
            "named-session state publication boundary is missing, inexact, or mismatched"
        )
        return result
    publication_ticks = publication_boundary.get("filetime_ticks")
    if (
        isinstance(publication_ticks, bool)
        or not isinstance(publication_ticks, int)
        or publication_ticks <= WINDOWS_FILETIME_EPOCH_OFFSET_TICKS
    ):
        errors.append("named-session state publication FILETIME is invalid")
        return result
    result["state_publication_boundary"] = publication_boundary
    result["state_publication_boundary_exact"] = True
    result["state_file_volume_verified"] = True
    result["exact_creation_order_clock_match"] = True
    result["creation_equals_publication"] = creation_ticks == publication_ticks
    result["strict_creation_precedes_publication"] = creation_ticks < publication_ticks
    result["process_created_before_state_file"] = result[
        "strict_creation_precedes_publication"
    ]
    if result["creation_equals_publication"] is True:
        errors.append(
            "held process creation FILETIME equals the exact named-session publication "
            "FILETIME; equality is ambiguous and grants no ownership"
        )
        return result
    if result["process_created_before_state_file"] is not True:
        errors.append(
            "held process was created after the exact named-session publication boundary; "
            "state PID was reused before stable identity acquisition"
        )
        return result

    after_daemons = resources_after_open.get("daemons", {})
    matches = [
        daemon
        for daemon in after_daemons.values()
        if isinstance(daemon, dict)
        and int(daemon.get("pid", 0)) == state_pid
        and str(daemon.get("identity", "")) == identity
    ]
    if len(matches) != 1:
        errors.append(
            f"named session held identity {identity} matched {len(matches)} daemon records"
        )
        return result
    daemon = dict(matches[0])
    result["identity"] = identity
    result["daemon"] = daemon
    if daemon.get("is_rdc_daemon") is not True:
        errors.append(
            f"named session held identity {identity} is not an rdc.daemon_server process"
        )
        return result
    if str(daemon.get("creation_time_key", "")) != str(
        stable_process_identity.get("creation_time_key", "")
    ):
        errors.append(
            "daemon snapshot creation-time key disagrees with the held process handle"
        )
        return result
    if daemon.get("creation_filetime_ticks") != creation_ticks:
        errors.append(
            "daemon snapshot creation FILETIME disagrees with the held process handle"
        )
        return result
    if daemon.get("creation_time_unix_ns") != expected_creation_unix_ns:
        errors.append(
            "daemon snapshot creation nanoseconds disagree with the held process handle"
        )
        return result
    if daemon.get("native_handle_verified_during_inspection") is not True:
        errors.append("daemon command/image metadata was not bound through a stable handle")
        return result
    daemon_image = daemon.get("image_path")
    stable_image = stable_process_identity.get("image_path")
    if not isinstance(daemon_image, str) or not isinstance(stable_image, str):
        errors.append("daemon or held-handle image metadata is missing")
        return result
    result["stable_image_match"] = _process_images_match(stable_image, daemon_image)
    if result["stable_image_match"] is not True:
        errors.append("daemon snapshot image disagrees with the held process handle")
        return result
    daemon_command = daemon.get("command")
    if not isinstance(daemon_command, list) or not any(
        "rdc.daemon_server" in str(token) for token in daemon_command
    ):
        errors.append("daemon command line is missing or is not rdc.daemon_server")
        return result
    if identity in resources_before.get("daemons", {}):
        errors.append(
            f"named session native-handle identity was already present before open: {identity}"
        )
        return result

    daemon_capture = daemon.get("capture")
    if not isinstance(daemon_capture, str) or not daemon_capture.strip():
        errors.append("owned daemon command metadata has no replay capture path")
        return result
    try:
        result["daemon_capture_path_metadata_match"] = (
            normalized_absolute_path(daemon_capture) == normalized_absolute_path(capture)
        )
    except (OSError, RuntimeError, ValueError) as exc:
        errors.append(f"owned daemon capture path could not be normalized: {exc}")
        return result
    if result["daemon_capture_path_metadata_match"] is not True:
        errors.append(
            "owned daemon capture-path metadata does not match the replay capture"
        )
        return result

    result["established"] = not errors
    return result

def terminate_verified_rdc_daemon(
    expected_identity: str,
    *,
    process_identity: Any,
    timeout: float,
) -> dict[str, Any]:
    result: dict[str, Any] = {
        "expected_identity": expected_identity,
        "owned_residue_detected": False,
        "identity_revalidated": False,
        "termination_requested": False,
        "same_native_handle": True,
        "tree_cleanup_requested": False,
        "passed": False,
    }
    try:
        metadata = process_identity.metadata()
    except Exception as exc:
        result["error"] = (
            f"stable daemon identity metadata failed closed: {type(exc).__name__}: {exc}"
        )
        return result
    result["held_process_identity"] = metadata
    if metadata.get("native_handle_held") is not True:
        result["error"] = "stable daemon process handle is not held; refusing pid fallback"
        return result
    if metadata.get("terminate_access") is not True:
        result["error"] = "stable daemon handle lacks terminate access; refusing pid fallback"
        return result
    if str(metadata.get("identity", "")) != expected_identity:
        result["error"] = (
            "held daemon identity changed or does not match ownership record: "
            f"observed={metadata.get('identity')!r}"
        )
        return result
    result["identity_revalidated"] = True
    try:
        running_before = bool(process_identity.is_running())
    except Exception as exc:
        result["error"] = (
            f"stable daemon handle liveness check failed closed: {type(exc).__name__}: {exc}"
        )
        return result
    result["running_before"] = running_before
    if not running_before:
        result.update(
            {
                "already_gone": True,
                "running_after": False,
                "passed": True,
            }
        )
        return result

    result["owned_residue_detected"] = True
    result["termination_requested"] = True
    try:
        termination = process_identity.terminate(timeout=timeout)
    except Exception as exc:
        result["error"] = (
            f"same-handle daemon termination raised: {type(exc).__name__}: {exc}"
        )
        return result
    result["termination"] = termination
    result["running_after"] = termination.get("running_after")
    result["original_exited_before_terminate"] = bool(
        termination.get("already_exited")
        or termination.get("original_exited_before_terminate")
    )
    result["passed"] = (
        termination.get("passed") is True
        and termination.get("running_after") is False
        and termination.get("same_native_handle") is True
    )
    if not result["passed"]:
        result["error"] = (
            "owned rdc daemon did not terminate through its original stable native handle"
        )
    return result

def _python_process_candidates() -> list[tuple[int, str]]:
    if os.name != "nt":
        import psutil

        candidates: list[tuple[int, str]] = []
        for process in psutil.process_iter(["pid", "name"]):
            name = str(process.info.get("name") or "")
            if "python" in name.casefold():
                candidates.append((int(process.info["pid"]), name))
        return candidates

    from ctypes import wintypes

    class ProcessEntry32W(ctypes.Structure):
        _fields_ = [
            ("dwSize", wintypes.DWORD),
            ("cntUsage", wintypes.DWORD),
            ("th32ProcessID", wintypes.DWORD),
            ("th32DefaultHeapID", ctypes.c_size_t),
            ("th32ModuleID", wintypes.DWORD),
            ("cntThreads", wintypes.DWORD),
            ("th32ParentProcessID", wintypes.DWORD),
            ("pcPriClassBase", wintypes.LONG),
            ("dwFlags", wintypes.DWORD),
            ("szExeFile", wintypes.WCHAR * 260),
        ]

    kernel32 = ctypes.WinDLL("kernel32", use_last_error=True)
    kernel32.CreateToolhelp32Snapshot.argtypes = [wintypes.DWORD, wintypes.DWORD]
    kernel32.CreateToolhelp32Snapshot.restype = wintypes.HANDLE
    kernel32.Process32FirstW.argtypes = [wintypes.HANDLE, ctypes.POINTER(ProcessEntry32W)]
    kernel32.Process32FirstW.restype = wintypes.BOOL
    kernel32.Process32NextW.argtypes = [wintypes.HANDLE, ctypes.POINTER(ProcessEntry32W)]
    kernel32.Process32NextW.restype = wintypes.BOOL
    kernel32.CloseHandle.argtypes = [wintypes.HANDLE]
    kernel32.CloseHandle.restype = wintypes.BOOL

    snapshot = kernel32.CreateToolhelp32Snapshot(0x00000002, 0)
    invalid_handle = ctypes.c_void_p(-1).value
    if snapshot == invalid_handle:
        raise OSError(ctypes.get_last_error(), "CreateToolhelp32Snapshot failed")
    candidates: list[tuple[int, str]] = []
    try:
        entry = ProcessEntry32W()
        entry.dwSize = ctypes.sizeof(ProcessEntry32W)
        has_entry = bool(kernel32.Process32FirstW(snapshot, ctypes.byref(entry)))
        while has_entry:
            name = str(entry.szExeFile)
            if "python" in name.casefold():
                candidates.append((int(entry.th32ProcessID), name))
            has_entry = bool(kernel32.Process32NextW(snapshot, ctypes.byref(entry)))
    finally:
        kernel32.CloseHandle(snapshot)
    return candidates


def snapshot_rdc_resources() -> dict[str, Any]:
    session_directory = rdc_data_directory() / "sessions"
    snapshot: dict[str, Any] = {
        "captured_utc": utc_now(),
        "session_directory": str(session_directory.resolve()),
        "sessions": {},
        "daemons": {},
        "errors": [],
    }
    try:
        if session_directory.is_dir():
            for path in sorted(session_directory.glob("*.json")):
                record = rdc_session_state_record(path)
                snapshot["sessions"][record["path"]] = record
    except OSError as exc:
        snapshot["errors"].append(f"session scan failed: {type(exc).__name__}: {exc}")

    access_denied = 0
    try:
        import psutil

        for pid, _ in _python_process_candidates():
            try:
                process_record = inspect_process_identity(pid)
                if process_record is None or process_record.get("is_rdc_daemon") is not True:
                    continue
                identity = str(process_record["identity"])
                snapshot["daemons"][identity] = process_record
            except (psutil.NoSuchProcess, psutil.ZombieProcess):
                continue
            except (psutil.AccessDenied, ProcessIdentityAccessDenied) as exc:
                access_denied += 1
                snapshot["errors"].append(
                    f"daemon metadata access denied for pid {pid}: "
                    f"{type(exc).__name__}: {exc}"
                )
    except (psutil.AccessDenied, ProcessIdentityAccessDenied) as exc:
        access_denied += 1
        snapshot["errors"].append(
            f"daemon scan access denied: {type(exc).__name__}: {exc}"
        )
    except Exception as exc:
        snapshot["errors"].append(f"daemon scan failed: {type(exc).__name__}: {exc}")
    snapshot["process_access_denied_count"] = access_denied
    snapshot["available"] = not snapshot["errors"] and access_denied == 0
    return snapshot


def diff_rdc_resources(before: dict[str, Any], after: dict[str, Any]) -> dict[str, Any]:
    before_sessions = before.get("sessions", {})
    after_sessions = after.get("sessions", {})
    before_daemons = before.get("daemons", {})
    after_daemons = after.get("daemons", {})
    changed_sessions = [
        path
        for path in sorted(set(before_sessions) & set(after_sessions))
        if before_sessions[path] != after_sessions[path]
    ]
    return {
        "available": before.get("available") is True and after.get("available") is True,
        "before_errors": list(before.get("errors", [])),
        "after_errors": list(after.get("errors", [])),
        "before_process_access_denied_count": before.get(
            "process_access_denied_count"
        ),
        "after_process_access_denied_count": after.get(
            "process_access_denied_count"
        ),
        "added_session_files": [
            after_sessions[path] for path in sorted(set(after_sessions) - set(before_sessions))
        ],
        "removed_session_files": [
            before_sessions[path] for path in sorted(set(before_sessions) - set(after_sessions))
        ],
        "changed_session_files": [
            {"before": before_sessions[path], "after": after_sessions[path]}
            for path in changed_sessions
        ],
        "added_daemons": [
            after_daemons[identity]
            for identity in sorted(set(after_daemons) - set(before_daemons))
        ],
        "removed_daemons": [
            before_daemons[identity]
            for identity in sorted(set(before_daemons) - set(after_daemons))
        ],
    }


def classify_rdc_session_status(
    completed: subprocess.CompletedProcess[str],
) -> dict[str, Any]:
    stderr = completed.stderr.strip()
    stdout = completed.stdout.strip()
    if completed.returncode == 1 and stdout == "" and stderr == RDC_NO_ACTIVE_SESSION:
        classification = "inactive"
    elif completed.returncode == 0:
        classification = "active"
    else:
        classification = "error"
    return {
        "classification": classification,
        "inactive": classification == "inactive",
        "returncode": completed.returncode,
        "stdout": stdout,
        "stderr": stderr,
    }


def normalized_absolute_path(value: str | Path) -> str:
    return os.path.normcase(str(Path(value).resolve()))


def allocate_replay_session_name() -> tuple[str, Path]:
    for _ in range(32):
        session = f"csm_marker_{os.getpid()}_{uuid.uuid4().hex[:10]}"
        state_path = rdc_session_state_path(session)
        if not state_path.exists():
            return session, state_path
    raise SmokeFailure("could not allocate a collision-free RenderDoc replay session name")

def aggregate_rdc_session_cleanup(
    cases: list[dict[str, Any]],
    resources_before: dict[str, Any],
    resources_after: dict[str, Any],
) -> dict[str, Any]:
    sessions: list[dict[str, Any]] = []
    owned_session_paths: set[str] = set()
    owned_daemon_ids: set[str] = set()
    errors: list[str] = []
    for case in cases:
        case_name = case.get("name")
        for boundary, boundary_result in case.get("boundaries", {}).items():
            replay_rows: list[tuple[str, dict[str, Any], dict[str, Any] | None]] = []
            attempt_records = boundary_result.get("capture_candidate_attempts")
            if isinstance(attempt_records, list) and attempt_records:
                for attempt_index, attempt in enumerate(attempt_records, start=1):
                    if not isinstance(attempt, dict):
                        continue
                    replay = attempt.get("rdc_marker_replay")
                    if isinstance(replay, dict):
                        replay_rows.append(
                            (f"candidate-{attempt_index}", replay, attempt)
                        )
            else:
                replay = boundary_result.get("rdc_marker_replay")
                if isinstance(replay, dict):
                    replay_rows.append(("accepted", replay, None))

            for replay_label, replay, attempt in replay_rows:
                cleanup = replay.get("session_cleanup")
                label = f"{case_name}/{boundary}/{replay_label}"
                if not isinstance(cleanup, dict):
                    errors.append(f"{label}: missing replay session cleanup")
                    continue
                state_file = cleanup.get("state_file", {})
                state_path = str(state_file.get("path", ""))
                if state_path:
                    owned_session_paths.add(state_path)
                daemon_ownership = cleanup.get("daemon_ownership")
                if (
                    isinstance(daemon_ownership, dict)
                    and daemon_ownership.get("established") is True
                    and not daemon_ownership.get("errors")
                    and daemon_ownership.get("identity")
                ):
                    owned_daemon_ids.add(str(daemon_ownership["identity"]))
                capture_metadata = attempt.get("capture", {}) if attempt else {}
                session_entry = {
                    "case": case_name,
                    "boundary": boundary,
                    "replay_label": replay_label,
                    "candidate_disposition": (
                        attempt.get("disposition") if attempt else "accepted"
                    ),
                    "candidate_capture_id": capture_metadata.get("captureId"),
                    "session": cleanup.get("session", replay.get("session")),
                    "passed": cleanup.get("passed") is True,
                    "close": cleanup.get("close"),
                    "close_subprocess_used": cleanup.get("close_subprocess_used"),
                    "status_subprocess_used": cleanup.get("status_subprocess_used"),
                    "direct_shutdown": cleanup.get("direct_shutdown"),
                    "verified_daemon_recovery": cleanup.get(
                        "verified_daemon_recovery"
                    ),
                    "post_status": cleanup.get("post_status"),
                    "state_file": state_file,
                    "daemon_ownership": cleanup.get("daemon_ownership"),
                    "owned_daemon_identities": cleanup.get(
                        "owned_daemon_identities", []
                    ),
                    "owned_daemon_absent": cleanup.get(
                        "direct_shutdown", {}
                    ).get("owned_daemon_absent"),
                    "owned_daemon_residue": cleanup.get(
                        "owned_daemon_residue", []
                    ),
                    "daemon_process_handle_close": cleanup.get(
                        "daemon_process_handle_close"
                    ),
                    "errors": list(cleanup.get("errors", [])),
                }
                sessions.append(session_entry)
                if session_entry["passed"] is not True:
                    errors.append(f"{label}: replay session cleanup did not pass")
    run_diff = diff_rdc_resources(resources_before, resources_after)
    after_daemons = resources_after.get("daemons", {})
    after_sessions = resources_after.get("sessions", {})
    owned_session_residue = [
        after_sessions[path] for path in sorted(owned_session_paths) if path in after_sessions
    ]
    owned_daemon_residue = [
        after_daemons[identity]
        for identity in sorted(owned_daemon_ids)
        if identity in after_daemons
    ]
    owned_added_session_files = [
        record
        for record in run_diff.get("added_session_files", [])
        if str(record.get("path", "")) in owned_session_paths
    ]
    owned_added_daemons = [
        record
        for record in run_diff.get("added_daemons", [])
        if str(record.get("identity", "")) in owned_daemon_ids
    ]
    if resources_before.get("available") is not True or resources_after.get("available") is not True:
        errors.append("top-level rdc session/daemon snapshots were incomplete")
    if run_diff.get("available") is not True:
        errors.append("top-level rdc session/daemon resource diff was incomplete")
    if run_diff.get("before_errors"):
        errors.append(
            f"top-level rdc resource snapshot-before errors: {run_diff['before_errors']!r}"
        )
    if run_diff.get("after_errors"):
        errors.append(
            f"top-level rdc resource snapshot-after errors: {run_diff['after_errors']!r}"
        )
    for label, snapshot in (
        ("before", resources_before),
        ("after", resources_after),
    ):
        access_denied = snapshot.get("process_access_denied_count")
        if not isinstance(access_denied, int) or isinstance(access_denied, bool):
            errors.append(
                f"top-level rdc resource snapshot-{label} has no integer "
                "process_access_denied_count"
            )
        elif access_denied != 0:
            errors.append(
                f"top-level rdc resource snapshot-{label} encountered "
                f"{access_denied} access-denied process(es)"
            )
    if owned_added_session_files:
        errors.append(
            "script-created RenderDoc session state was added and remains after this run: "
            f"{owned_added_session_files!r}"
        )
    if owned_added_daemons:
        errors.append(
            "script-created RenderDoc daemon processes were added and remain after this run: "
            f"{owned_added_daemons!r}"
        )
    if owned_session_residue:
        errors.append(f"script-created session files remain: {owned_session_residue!r}")
    if owned_daemon_residue:
        errors.append(f"script-created daemon processes remain: {owned_daemon_residue!r}")
    result = {
        "schema": "rdc-session-cleanup-v2",
        "ownership_model": {
            "hard_gate_scope": "owned additions and owned residue only",
            "external_additions_are_diagnostic": True,
            "daemon_identity": (
                "allocated session path/capture path plus state PID, held native handle, "
                "and exact creation-time identity"
            ),
        },
        "opened_by_script": bool(sessions),
        "session_count": len(sessions),
        "named_replay_sessions": sessions,
        "sessions": sessions,
        "run_resource_diff": run_diff,
        "owned_session_paths": sorted(owned_session_paths),
        "owned_daemon_identities": sorted(owned_daemon_ids),
        "owned_added_session_files": owned_added_session_files,
        "owned_added_daemons": owned_added_daemons,
        "owned_session_file_residue": owned_session_residue,
        "owned_daemon_residue": owned_daemon_residue,
        "external_added_session_files": [
            record
            for record in run_diff.get("added_session_files", [])
            if str(record.get("path", "")) not in owned_session_paths
        ],
        "external_added_daemons": [
            record
            for record in run_diff.get("added_daemons", [])
            if str(record.get("identity", "")) not in owned_daemon_ids
        ],
        "errors": errors,
        "passed": not errors,
    }
    result["closed"] = result["passed"]
    return result

if "controller" in globals() and "state" in globals() and "rd" in globals():
    result = rdc_marker_replay_main()
elif __name__ == "__main__":
    try:
        raise SystemExit(main())
    except SmokeFailure as exc:
        print(f"error: {exc}", file=sys.stderr)
        raise SystemExit(1)
