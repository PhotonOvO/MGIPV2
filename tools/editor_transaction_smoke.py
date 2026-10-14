#!/usr/bin/env python3
"""Independent validation for the P5 editor-transaction smoke evidence.

The production executable is deliberately outside this module's contract.  A
runner supplies a case record containing its command result, captured output,
identity hashes, and saved-artifact path; this module parses and validates the
evidence without constructing or interpreting production CLI flags.

The public entry points are :func:`validate_case`, :func:`validate_summary`,
:func:`validate_case_file`, and :func:`validate_summary_file`.  They return a
JSON-serialisable, derived record or raise :class:`ValidationError`.
"""

from __future__ import annotations

import argparse
import hashlib
import json
import os
import re
import stat
import struct
import sys
from dataclasses import dataclass
from pathlib import Path, PurePosixPath
from typing import Any, Iterable, Mapping, Sequence
from urllib.parse import unquote, urlsplit


MODEL_IDENTITY = "resources/GLTF_Sponza/sponza.gltf"
VALID_MODES = frozenset({"csm-translate-stop", "csm-rotate-stop"})
CASE_RESULT_SCHEMA = "mgif.p5.editor-transaction.case.v1"
SUMMARY_RESULT_SCHEMA = "mgif.p5.editor-transaction.summary.v1"
U64_MAX = (1 << 64) - 1
SHA256_RE = re.compile(r"[0-9a-fA-F]{64}\Z")
_U64_TEXT = r"(?:0|[1-9][0-9]*)"
_POSITIVE_U64_TEXT = r"[1-9][0-9]*"
_MODE_TEXT = r"(?:csm-translate-stop|csm-rotate-stop)"
_BIT_TEXT = r"[01]"
_FLOAT6_TEXT = r"[-+]?(?:[0-9]+\.[0-9]{6})"

# ``utils::Logger`` prefixes every production line with ``[HH:MM:SS.mmm]``
# when ``eSHOW_TIME`` is enabled.  Unit fixtures may use the payload without a
# prefix, but a present prefix must be a real logger timestamp rather than an
# arbitrary string that happens to contain a marker.
_LOGGER_TIMESTAMP_PREFIX = re.compile(
    r"^\s*\[(?:[0-9]{2}:[0-9]{2}:[0-9]{2}\.[0-9]{3,6}|"
    r"[0-9]{4}-[0-9]{2}-[0-9]{2}(?:[ T][0-9]{2}:[0-9]{2}:[0-9]{2}(?:\.[0-9]{3,6})?)?)\]\s*$"
)
_CSM_MARKER_START = "[CSM_AUTOMATION] marker="
_OLD_TRANSACTION_MARKER_START = "[EDITOR_TRANSACTION] marker="
_FRAME_40_EVIDENCE = re.compile(
    r"(?<![A-Za-z0-9_])(?:(?:rendered|automation)_)?frame(?:_index)?[\"']?"
    r"(?:\s*(?:=|:)\s*|\s+)[\"']?40(?![0-9])",
    re.IGNORECASE,
)

AUTHORITATIVE_MARKERS = (
    "scene-ready",
    "automation-start",
    "undo-observed",
    "redo-observed",
    "save-complete",
    "frame-budget-complete",
    "automation-complete",
)

SUPPLEMENTARY_MARKERS = (
    "config",
    "scene-ready",
    "start",
    "last-moving",
    "first-still",
    "settled",
    "complete",
)

FAILURE_PATTERN = re.compile(
    r"(?:"
    r"VUID-|SYNC-HAZARD|Vulkan validation (?:ERROR|WARNING)\b|"
    r"Vulkan error:|VK_ERROR(?:_[A-Z0-9_]+)?|device lost|"
    r"Frame failed during phase|Failed to create Vulkan instance|"
    r"Could not initialize GLFW|GLFW: Vulkan not supported|"
    r"Failed to load model:|Failed to load DDGI mesh SDF:|"
    r"Scene load failed with exception|Failed to retrieve loaded scene|"
    r"Out of memory while (?:loading|retrieving|preparing)|"
    r"Unknown command-line option:|Unknown --automation mode:|"
    r"marker=automation-failed|Editor transaction automation failed|"
    r"Save As verification failed|Assertion failed|Traceback"
    r")",
    re.IGNORECASE,
)

DDGI_SDF_PATTERN = re.compile(
    r"Loaded DDGI mesh SDF:\s+.*sponza_(?:sdf|SDF)\.bin"
)


class ValidationError(RuntimeError):
    """Raised when smoke evidence does not satisfy the independent contract."""


def require(condition: bool, message: str) -> None:
    """Raise a stable validation error when ``condition`` is false."""

    if not condition:
        raise ValidationError(message)


@dataclass(frozen=True)
class CaseSpec:
    """The fixed five-case matrix used by the aggregate driver."""

    name: str
    mode: str
    no_post: bool
    no_ddgi: bool
    taa: bool
    require_sdf: bool


CASE_SPECS: tuple[CaseSpec, ...] = (
    CaseSpec("01-translate-no-post-no-ddgi", "csm-translate-stop", True, True, False, False),
    CaseSpec("02-rotate-no-post-no-ddgi", "csm-rotate-stop", True, True, False, False),
    CaseSpec("03-translate-taa-no-ddgi", "csm-translate-stop", False, True, True, False),
    CaseSpec("04-rotate-taa-no-ddgi", "csm-rotate-stop", False, True, True, False),
    CaseSpec("05-rotate-taa-ddgi", "csm-rotate-stop", False, False, True, True),
)
CASE_DEFINITIONS = {spec.name: spec for spec in CASE_SPECS}


_MISSING = object()


def _first(mapping: Mapping[str, Any], *keys: str, default: Any = _MISSING) -> Any:
    for key in keys:
        if key in mapping:
            return mapping[key]
    return default


def _lookup(mapping: Mapping[str, Any], *keys: str) -> Any:
    """Look up a field on a case and its common execution subobjects."""

    value = _first(mapping, *keys, default=_MISSING)
    if value is not _MISSING:
        return value
    for parent_name in ("process", "execution", "result", "observations"):
        parent = mapping.get(parent_name)
        if isinstance(parent, Mapping):
            value = _first(parent, *keys, default=_MISSING)
            if value is not _MISSING:
                return value
    return _MISSING


def _load_json(value: Any, *, label: str) -> Any:
    if isinstance(value, Mapping):
        return dict(value)
    require(isinstance(value, (str, Path)), f"{label} must be an object or JSON path")
    path = Path(value)
    try:
        text = path.read_text(encoding="utf-8")
        return json.loads(text)
    except (OSError, UnicodeError, json.JSONDecodeError) as exc:
        raise ValidationError(f"cannot parse {label} {path}: {exc}") from exc


def _as_bool(value: Any, field: str) -> bool:
    require(type(value) is bool, f"{field} must be a boolean")
    return bool(value)


def _as_int(value: Any, field: str, *, minimum: int | None = None, maximum: int | None = None) -> int:
    require(type(value) is int, f"{field} must be an integer")
    if minimum is not None:
        require(value >= minimum, f"{field} must be >= {minimum}")
    if maximum is not None:
        require(value <= maximum, f"{field} must be <= {maximum}")
    return value


def _sha256(path: Path) -> str:
    digest = hashlib.sha256()
    try:
        with path.open("rb") as stream:
            for chunk in iter(lambda: stream.read(1024 * 1024), b""):
                digest.update(chunk)
    except OSError as exc:
        raise ValidationError(f"cannot hash {path}: {exc}") from exc
    return digest.hexdigest()


def _valid_sha(value: Any, field: str) -> str:
    require(isinstance(value, str) and SHA256_RE.fullmatch(value) is not None, f"{field} must be a SHA-256 hex string")
    return value.lower()


def _resolve_path(value: Any, *, base: Path, field: str) -> Path:
    require(isinstance(value, (str, Path)) and str(value), f"{field} must be a non-empty path")
    path = Path(value)
    if not path.is_absolute():
        path = base / path
    try:
        return path.resolve(strict=False)
    except OSError as exc:
        raise ValidationError(f"cannot resolve {field}: {value!r}: {exc}") from exc


def _lexical_path(value: Any, *, base: Path, field: str) -> Path:
    """Return an absolute normalized path without following its final alias."""

    require(isinstance(value, (str, Path)) and str(value), f"{field} must be a non-empty path")
    path = Path(value)
    if not path.is_absolute():
        path = base / path
    try:
        return Path(os.path.abspath(path))
    except OSError as exc:
        raise ValidationError(f"cannot normalize {field}: {value!r}: {exc}") from exc


def _is_within(path: Path, root: Path) -> bool:
    try:
        path.relative_to(root)
        return True
    except ValueError:
        return False


def _require_within(path: Path, root: Path, field: str) -> None:
    require(_is_within(path, root), f"{field} escapes its evidence root")


def _read_capture(value: Any, *, base: Path, field: str) -> str:
    if isinstance(value, bytes):
        return value.decode("utf-8", errors="replace")
    require(isinstance(value, str), f"{field} must be captured text")
    return value


def _read_capture_path(value: Any, *, base: Path, field: str) -> str:
    path = _resolve_path(value, base=base, field=field)
    _require_within(path, base, field)
    require(path.is_file(), f"{field} does not exist: {path}")
    try:
        return path.read_text(encoding="utf-8", errors="replace")
    except OSError as exc:
        raise ValidationError(f"cannot read {field} {path}: {exc}") from exc


def _case_name(case: Mapping[str, Any]) -> str:
    value = _first(case, "case", "name", "case_name", "id", default=_MISSING)
    require(isinstance(value, str) and value, "case name is required")
    return value


def _case_spec(case_name: str, expected: CaseSpec | None) -> CaseSpec | None:
    if expected is not None:
        require(case_name == expected.name, f"case name {case_name!r} does not match expected {expected.name!r}")
        return expected
    return CASE_DEFINITIONS.get(case_name)


def _validate_case_options(case: Mapping[str, Any], spec: CaseSpec | None) -> None:
    mode = _lookup(case, "mode", "automation_mode")
    if mode is not _MISSING:
        require(mode in VALID_MODES, f"case mode is invalid: {mode!r}")
    if spec is None:
        return
    if mode is not _MISSING:
        require(mode == spec.mode, f"case mode must be {spec.mode!r}")
    for field, expected in (
        ("no_post", spec.no_post),
        ("no_ddgi", spec.no_ddgi),
        ("taa", spec.taa),
    ):
        value = _lookup(case, field, "noPost" if field == "no_post" else "noDdgi" if field == "no_ddgi" else "use_taa")
        if value is not _MISSING:
            require(type(value) is bool, f"case {field} must be a boolean")
            require(value == expected, f"case {field} does not match the fixed case matrix")


def _combined_output(case: Mapping[str, Any], base: Path) -> str:
    direct = _first(case, "combined_output", "combinedOutput", "output", default=_MISSING)
    if direct is not _MISSING:
        # A caller cannot provide a pre-merged stream and either independent
        # stream: accepting both would make the record ambiguous and could let
        # the merged value hide diagnostics present in the real output.
        explicit_stdout = _lookup(case, "stdout", "stdout_path", "stdoutPath") is not _MISSING
        explicit_stderr = _lookup(case, "stderr", "stderr_path", "stderrPath") is not _MISSING
        require(
            not explicit_stdout and not explicit_stderr,
            "combined_output cannot be combined with explicit stdout/stdout_path or stderr/stderr_path",
        )
        if isinstance(direct, (str, bytes)):
            return _read_capture(direct, base=base, field="combined_output")
        raise ValidationError("combined_output must be captured text")

    stdout = _first(case, "stdout", default=_MISSING)
    stderr = _first(case, "stderr", default=_MISSING)
    stdout_path = _first(case, "stdout_path", "stdoutPath", default=_MISSING)
    stderr_path = _first(case, "stderr_path", "stderrPath", default=_MISSING)
    if stdout is _MISSING and stdout_path is not _MISSING:
        stdout = _read_capture_path(stdout_path, base=base, field="stdout_path")
    elif stdout is not _MISSING:
        stdout = _read_capture(stdout, base=base, field="stdout")
    if stderr is _MISSING and stderr_path is not _MISSING:
        stderr = _read_capture_path(stderr_path, base=base, field="stderr_path")
    elif stderr is not _MISSING:
        stderr = _read_capture(stderr, base=base, field="stderr")
    require(stdout is not _MISSING or stderr is not _MISSING, "captured stdout/stderr is required")
    return ("" if stdout is _MISSING else str(stdout)) + ("\n" if stdout is not _MISSING and stderr is not _MISSING else "") + ("" if stderr is _MISSING else str(stderr))


def _line_entries(text: str) -> list[tuple[int, str]]:
    entries: list[tuple[int, str]] = []
    byte_offset = 0
    for raw in text.splitlines(keepends=True):
        line = raw
        if line.endswith("\r\n"):
            line = line[:-2]
        elif line.endswith(("\r", "\n")):
            line = line[:-1]
        entries.append((byte_offset, line))
        byte_offset += len(raw.encode("utf-8", errors="replace"))
    if not text or (text and not text.endswith(("\n", "\r"))):
        if not entries:
            entries.append((0, text))
    return entries


def _marker_patterns() -> dict[str, re.Pattern[str]]:
    return {
        "scene-ready": re.compile(
            rf"\[CSM_AUTOMATION\] marker=scene-ready mode=(?P<mode>{_MODE_TEXT}) model=(?P<model>{re.escape(MODEL_IDENTITY)})"
        ),
        "automation-start": re.compile(
            rf"\[CSM_AUTOMATION\] marker=automation-start mode=(?P<mode>{_MODE_TEXT}) transaction=rename-undo-redo-save frame_budget=40"
        ),
        "undo-observed": re.compile(
            rf"\[CSM_AUTOMATION\] marker=undo-observed mode=(?P<mode>{_MODE_TEXT}) revision=(?P<revision>{_U64_TEXT}) undo=0 redo=1 digest=(?P<digest>[0-9a-f]{{64}}) renderer_revision=(?P<renderer_revision>{_U64_TEXT})"
        ),
        "redo-observed": re.compile(
            rf"\[CSM_AUTOMATION\] marker=redo-observed mode=(?P<mode>{_MODE_TEXT}) revision=(?P<revision>{_U64_TEXT}) undo=1 redo=0 digest=(?P<digest>[0-9a-f]{{64}}) renderer_revision=(?P<renderer_revision>{_U64_TEXT})"
        ),
        "save-complete": re.compile(
            rf"\[CSM_AUTOMATION\] marker=save-complete mode=(?P<mode>{_MODE_TEXT}) revision=(?P<revision>{_U64_TEXT}) clean_revision=(?P<clean_revision>{_U64_TEXT}) bytes=(?P<bytes>{_POSITIVE_U64_TEXT}) reload_valid=1 semantic_equal=1"
        ),
        "frame-budget-complete": re.compile(
            rf"\[CSM_AUTOMATION\] marker=frame-budget-complete mode=(?P<mode>{_MODE_TEXT}) frames=40"
        ),
        "automation-complete": re.compile(
            rf"\[CSM_AUTOMATION\] marker=automation-complete mode=(?P<mode>{_MODE_TEXT}) frames=40 auto_exit=1"
        ),
    }


def _supplementary_patterns(spec: CaseSpec | None) -> dict[str, re.Pattern[str]]:
    # Unknown one-case records may still be checked against the supplementary
    # gate; the option fields are then accepted as either 0 or 1 and are still
    # exact.  Keep this grammar byte-for-byte compatible with the production
    # logger, including capture-sync fields and pose fields.
    mode_text = spec.mode if spec is not None else _MODE_TEXT
    no_post = int(spec.no_post) if spec is not None else r"[01]"
    no_ddgi = int(spec.no_ddgi) if spec is not None else r"[01]"
    taa = int(spec.taa) if spec is not None else r"[01]"
    return {
        "config": re.compile(
            rf"\[CSM_AUTOMATION\] marker=config mode={mode_text} fixed_dt=0\.01666666[0-9] warmup=8 motion=24 hold=8 no_ui=1 no_post={no_post} no_ddgi={no_ddgi} taa={taa} auto_exit=1 capture_sync=(?P<capture_sync>{_BIT_TEXT}) capture_control=(?P<capture_control>{_BIT_TEXT}) capture_sync_timeout_ms=(?P<capture_sync_timeout_ms>{_U64_TEXT}) capture_sync_dir=(?P<capture_sync_dir>[^\r\n]*)"
        ),
        "scene-ready": re.compile(
            rf"\[CSM_AUTOMATION\] marker=scene-ready mode={mode_text} model={re.escape(MODEL_IDENTITY)}"
        ),
        "start": re.compile(rf"\[CSM_AUTOMATION\] marker=start mode={mode_text} frame=0"),
        "last-moving": re.compile(
            rf"\[CSM_AUTOMATION\] marker=last-moving mode={mode_text} frame=31 current_pos=\({_FLOAT6_TEXT},{_FLOAT6_TEXT},{_FLOAT6_TEXT}\) current_yaw_pitch=\({_FLOAT6_TEXT},{_FLOAT6_TEXT}\) previous_pos=\({_FLOAT6_TEXT},{_FLOAT6_TEXT},{_FLOAT6_TEXT}\) previous_yaw_pitch=\({_FLOAT6_TEXT},{_FLOAT6_TEXT}\)"
        ),
        "first-still": re.compile(
            rf"\[CSM_AUTOMATION\] marker=first-still mode={mode_text} frame=32 current_pos=\({_FLOAT6_TEXT},{_FLOAT6_TEXT},{_FLOAT6_TEXT}\) current_yaw_pitch=\({_FLOAT6_TEXT},{_FLOAT6_TEXT}\) previous_pos=\({_FLOAT6_TEXT},{_FLOAT6_TEXT},{_FLOAT6_TEXT}\) previous_yaw_pitch=\({_FLOAT6_TEXT},{_FLOAT6_TEXT}\)"
        ),
        "settled": re.compile(
            rf"\[CSM_AUTOMATION\] marker=settled mode={mode_text} frame=39 current_pos=\({_FLOAT6_TEXT},{_FLOAT6_TEXT},{_FLOAT6_TEXT}\) current_yaw_pitch=\({_FLOAT6_TEXT},{_FLOAT6_TEXT}\) previous_pos=\({_FLOAT6_TEXT},{_FLOAT6_TEXT},{_FLOAT6_TEXT}\) previous_yaw_pitch=\({_FLOAT6_TEXT},{_FLOAT6_TEXT}\)"
        ),
        "complete": re.compile(rf"\[CSM_AUTOMATION\] marker=complete mode={mode_text} frames=40"),
    }


def _marker_payload(line: str) -> str | None:
    """Return the exact CSM payload after an optional logger timestamp.

    The production logger currently emits ``[HH:MM:SS.mmm] ``.  Supporting an
    ISO-style timestamp as well keeps the parser compatible with file logger
    builds while still rejecting arbitrary prefixes.  A marker token without a
    valid prefix is returned as a payload so the caller can report it as a
    malformed protocol line.
    """

    marker_offset = line.find(_CSM_MARKER_START)
    old_offset = line.find(_OLD_TRANSACTION_MARKER_START)
    if marker_offset < 0:
        if old_offset >= 0:
            return line[old_offset:]
        return None
    prefix = line[:marker_offset]
    if prefix and _LOGGER_TIMESTAMP_PREFIX.fullmatch(prefix) is None:
        raise ValidationError("CSM marker line has an invalid logger timestamp prefix")
    return line[marker_offset:]


def _marker_token(line: str) -> str | None:
    payload = _marker_payload(line)
    if payload is None:
        return None
    if payload.startswith(_OLD_TRANSACTION_MARKER_START):
        remainder = payload[len(_OLD_TRANSACTION_MARKER_START) :]
    elif payload.startswith(_CSM_MARKER_START):
        remainder = payload[len(_CSM_MARKER_START) :]
    else:
        return None
    return remainder.split(" ", 1)[0]


def _validate_marker_lines(text: str, spec: CaseSpec | None) -> dict[str, Any]:
    authoritative_patterns = _marker_patterns()
    supplementary_patterns = _supplementary_patterns(spec)
    entries = _line_entries(text)
    authoritative: dict[str, list[tuple[int, dict[str, str]]]] = {name: [] for name in AUTHORITATIVE_MARKERS}
    supplementary: dict[str, list[int]] = {name: [] for name in SUPPLEMENTARY_MARKERS}
    combined: list[tuple[int, str]] = []

    for offset, line in entries:
        payload = _marker_payload(line)
        if payload is not None:
            token = _marker_token(payload)
            if token is not None:
                combined.append((offset, token))
        for name, pattern in authoritative_patterns.items():
            match = pattern.fullmatch(payload) if payload is not None else None
            if match is not None:
                authoritative[name].append((offset, match.groupdict()))
        for name, pattern in supplementary_patterns.items():
            if payload is not None and pattern.fullmatch(payload) is not None:
                supplementary[name].append(offset)

    # A line carrying a known marker token but malformed fields is evidence of a
    # protocol violation, even when a later correct line happens to exist.
    known_tokens = set(AUTHORITATIVE_MARKERS) | set(SUPPLEMENTARY_MARKERS)
    exact_offsets: dict[str, set[int]] = {
        name: {offset for offset, _groups in rows} for name, rows in authoritative.items()
    }
    exact_offsets.update({name: set(offsets) for name, offsets in supplementary.items()})
    for offset, line in entries:
        payload = _marker_payload(line)
        token = _marker_token(payload) if payload is not None else None
        if token is None:
            continue
        if payload is not None and payload.startswith(_OLD_TRANSACTION_MARKER_START):
            if token in AUTHORITATIVE_MARKERS or token in SUPPLEMENTARY_MARKERS:
                raise ValidationError(
                    f"legacy [EDITOR_TRANSACTION] marker is not accepted: {token!r}"
                )
            continue
        if token in known_tokens:
            exact = offset in exact_offsets.get(token, set())
            # The scene-ready line is represented in both maps, so either exact
            # match is enough to classify it as valid.
            if not exact:
                raise ValidationError(f"malformed {token!r} marker line")
        elif any(token.startswith(name) or name.startswith(token) for name in AUTHORITATIVE_MARKERS):
            raise ValidationError(f"marker alias/subString is not authoritative: {token!r}")

    # The two marker ledgers are one runtime timeline.  ``scene-ready`` is
    # intentionally shared by both ledgers and therefore contributes one event.
    expected_combined = (
        "config",
        "scene-ready",
        "start",
        "automation-start",
        "undo-observed",
        "redo-observed",
        "save-complete",
        "last-moving",
        "first-still",
        "settled",
        "complete",
        "frame-budget-complete",
        "automation-complete",
    )
    combined_known = [token for _offset, token in combined if token in set(expected_combined)]
    require(
        combined_known == list(expected_combined),
        "combined marker chronology is invalid",
    )

    for name in AUTHORITATIVE_MARKERS:
        count = len(authoritative[name])
        require(count == 1, f"authoritative marker {name!r} must occur exactly once (found {count})")
    for name in SUPPLEMENTARY_MARKERS:
        count = len(supplementary[name])
        require(count == 1, f"supplementary marker {name!r} must occur exactly once (found {count})")

    authoritative_offsets = [authoritative[name][0][0] for name in AUTHORITATIVE_MARKERS]
    require(authoritative_offsets == sorted(authoritative_offsets) and len(set(authoritative_offsets)) == len(authoritative_offsets), "authoritative marker order is invalid")
    supplementary_offsets = [supplementary[name][0] for name in SUPPLEMENTARY_MARKERS]
    require(supplementary_offsets == sorted(supplementary_offsets) and len(set(supplementary_offsets)) == len(supplementary_offsets), "supplementary marker order is invalid")

    config_offset = supplementary["config"][0]
    config_line = next(line for offset, line in entries if offset == config_offset)
    config_payload = _marker_payload(config_line)
    config_match = (
        supplementary_patterns["config"].fullmatch(config_payload)
        if config_payload is not None
        else None
    )
    require(config_match is not None, "config marker did not match production grammar")
    config = config_match.groupdict()
    require(config["capture_control"] == "0", "P5 config must not enable the control frame")
    capture_timeout = _parse_u64_text(
        config["capture_sync_timeout_ms"], "config capture_sync_timeout_ms"
    )
    require(capture_timeout > 0, "config capture_sync_timeout_ms must be positive")
    require(
        (config["capture_sync"] == "1") == bool(config["capture_sync_dir"]),
        "config capture_sync and capture_sync_dir disagree",
    )

    records = {name: authoritative[name][0][1] for name in AUTHORITATIVE_MARKERS}
    mode_values = [record.get("mode") for record in records.values()]
    require(len(set(mode_values)) == 1 and mode_values[0] in VALID_MODES, "authoritative markers must use one valid mode")
    if spec is not None:
        require(mode_values[0] == spec.mode, f"markers use the wrong mode for {spec.name}")
    require(records["scene-ready"].get("model") == MODEL_IDENTITY, "scene-ready model identity is not the strict Sponza literal")

    undo = records["undo-observed"]
    redo = records["redo-observed"]
    save = records["save-complete"]
    undo_revision = _parse_u64_text(undo["revision"], "undo revision")
    undo_renderer_revision = _parse_u64_text(undo["renderer_revision"], "undo renderer revision")
    redo_revision = _parse_u64_text(redo["revision"], "redo revision")
    redo_renderer_revision = _parse_u64_text(redo["renderer_revision"], "redo renderer revision")
    save_revision = _parse_u64_text(save["revision"], "save revision")
    clean_revision = _parse_u64_text(save["clean_revision"], "save clean revision")
    save_bytes = _parse_u64_text(save["bytes"], "save bytes")
    require(undo_revision == undo_renderer_revision, "undo renderer revision does not match undo revision")
    require(redo_revision == redo_renderer_revision, "redo renderer revision does not match redo revision")
    require(redo_revision == undo_revision + 1, "undo/redo revisions are not consecutive")
    require(save_revision == redo_revision and clean_revision == save_revision, "save revisions are inconsistent")
    require(undo["digest"] != redo["digest"], "Undo and Redo digests must prove a changed transaction")
    require(save_bytes > 0, "save marker bytes must be positive")

    return {
        "authoritative": records,
        "authoritative_offsets": dict(zip(AUTHORITATIVE_MARKERS, authoritative_offsets)),
        "supplementary_offsets": dict(zip(SUPPLEMENTARY_MARKERS, supplementary_offsets)),
        "mode": mode_values[0],
        "undo_revision": undo_revision,
        "redo_revision": redo_revision,
        "save_revision": save_revision,
        "clean_revision": clean_revision,
        "save_bytes": save_bytes,
        "undo_digest": undo["digest"],
        "redo_digest": redo["digest"],
        "config": config,
        "combined_offsets": {
            token: next(offset for offset, observed in combined if observed == token)
            for token in expected_combined
        },
    }


def _parse_u64_text(value: Any, field: str) -> int:
    require(isinstance(value, str) and re.fullmatch(_U64_TEXT, value) is not None, f"{field} must be an unsigned 64-bit decimal")
    parsed = int(value, 10)
    require(0 <= parsed <= U64_MAX, f"{field} is outside uint64")
    return parsed


def _validate_execution(case: Mapping[str, Any], output: str) -> dict[str, Any]:
    timed_out = _lookup(case, "timed_out", "timedOut", "timeout")
    require(timed_out is not _MISSING, "timed_out is required")
    require(type(timed_out) is bool, "timed_out must be a boolean")
    require(not timed_out, "case timed out")
    exit_code = _lookup(case, "exit_code", "exitCode", "returncode")
    require(exit_code is not _MISSING, "exit_code is required")
    _as_int(exit_code, "exit_code")
    require(exit_code == 0, f"process exit code must be 0 (got {exit_code})")

    matches = [match.group(0) for match in FAILURE_PATTERN.finditer(output)]
    supplied_matches = _lookup(case, "failure_regex_matches", "failureRegexMatches")
    if supplied_matches is not _MISSING:
        require(isinstance(supplied_matches, list), "failure_regex_matches must be an array")
        require(not supplied_matches, "runner reported failure-regex matches")
    for field in (
        "failure_regex_match_count",
        "failureRegexMatchCount",
        "validation_error_or_failure_match_count",
        "validationErrorOrFailureMatchCount",
        "vuid_match_count",
        "sync_hazard_match_count",
    ):
        value = _lookup(case, field)
        if value is not _MISSING:
            _as_int(value, field, minimum=0)
            require(value == 0, f"{field} must be zero")
    if matches:
        raise ValidationError(f"failure regex matched: {matches[0]!r}")
    # ``frames=40`` on the terminal markers is the budget summary; a singular
    # counted ``frame=40`` (including CSM telemetry or JSON-style ``frame: 40``)
    # proves that an illegal forty-first frame was observed.
    frame_match = _FRAME_40_EVIDENCE.search(output)
    require(frame_match is None, "captured evidence contains forbidden frame=40")
    for field in (
        "frame_telemetry",
        "frameTelemetry",
        "frame_events",
        "frameEvents",
        "rendered_frame_records",
        "renderedFrameRecords",
        "telemetry",
    ):
        value = _lookup(case, field)
        if value is _MISSING:
            continue
        try:
            encoded = json.dumps(value, sort_keys=True, separators=(",", ":"))
        except (TypeError, ValueError) as exc:
            raise ValidationError(f"{field} cannot be serialized for frame validation") from exc
        require(
            _FRAME_40_EVIDENCE.search(encoded) is None,
            f"{field} contains forbidden frame=40",
        )
    return {"timed_out": False, "exit_code": 0, "failure_matches": matches}


def _identity_sources(case: Mapping[str, Any]) -> tuple[Mapping[str, Any], Mapping[str, Any] | None]:
    identity = case.get("identity")
    if not isinstance(identity, Mapping):
        identity = {}
    executable = case.get("executable")
    if not isinstance(executable, Mapping):
        executable = None
    return identity, executable


def _validate_executable(
    case: Mapping[str, Any],
    *,
    root: Path,
    expected_path: Path | None = None,
    expected_sha256: str | None = None,
) -> dict[str, Any]:
    identity, executable = _identity_sources(case)
    path_value = _lookup(case, "demo_path", "executable_path", "executablePath")
    if path_value is _MISSING:
        path_value = _first(identity, "demo_path", "executable_path", "path", default=_MISSING)
    if path_value is _MISSING and executable is not None:
        path_value = _first(executable, "path", default=_MISSING)
    path = expected_path if path_value is _MISSING and expected_path is not None else _resolve_path(path_value, base=root, field="demo_path")
    _require_within(path, root, "demo_path")
    require(path.is_file(), f"executable does not exist: {path}")
    actual = _sha256(path)

    expected_value = _lookup(case, "expected_demo_sha256", "expected_executable_sha256")
    if expected_value is _MISSING:
        expected_value = _first(identity, "demo_sha256", "sha256", default=_MISSING)
    if expected_value is _MISSING and executable is not None:
        expected_value = _first(executable, "sha256", default=_MISSING)
    if expected_value is _MISSING:
        expected_value = expected_sha256
    require(expected_value is not _MISSING and expected_value is not None, "expected executable SHA-256 is required")
    expected = _valid_sha(expected_value, "expected executable SHA-256")
    require(actual == expected, "executable SHA-256 does not match the expected identity")

    observed = _lookup(case, "demo_sha256", "executable_sha256", "observed_executable_sha256")
    if observed is not _MISSING:
        require(_valid_sha(observed, "observed executable SHA-256") == actual, "observed executable SHA-256 does not match")
    before = _lookup(case, "demo_sha256_before", "executable_sha256_before", "hash_before")
    after = _lookup(case, "demo_sha256_after", "executable_sha256_after", "hash_after")
    require(before is not _MISSING and after is not _MISSING, "both before and after executable hashes are required")
    require(_valid_sha(before, "demo_sha256_before") == actual, "executable hash drifted before the case")
    require(_valid_sha(after, "demo_sha256_after") == actual, "executable hash drifted after the case")
    return {"path": str(path), "sha256": actual}


def _artifact_fields(case: Mapping[str, Any]) -> tuple[Any, Any, Any, Any]:
    artifact = case.get("artifact")
    artifact_map = artifact if isinstance(artifact, Mapping) else {}
    artifact_path = _first(
        case,
        "artifact_path",
        "saved_artifact_path",
        "output_path",
        "save_path",
        "save_target",
        default=_MISSING,
    )
    if artifact_path is _MISSING:
        artifact_path = _first(artifact_map, "path", "artifact_path", default=_MISSING)
    save_target = _first(case, "save_target", "save_path", "saved_path", default=_MISSING)
    if save_target is _MISSING:
        save_target = _first(artifact_map, "target", "path", default=_MISSING)
    artifact_hash = _first(case, "artifact_sha256", "saved_artifact_sha256", default=_MISSING)
    if artifact_hash is _MISSING:
        artifact_hash = _first(artifact_map, "sha256", "hash", default=_MISSING)
    artifact_size = _first(case, "artifact_size_bytes", "saved_artifact_size_bytes", default=_MISSING)
    if artifact_size is _MISSING:
        artifact_size = _first(artifact_map, "size_bytes", "bytes", "size", default=_MISSING)
    return artifact_path, save_target, artifact_hash, artifact_size


def _is_regular_non_alias(path: Path) -> bool:
    """Return true only for a regular file that is not a link/reparse alias."""

    try:
        info = path.lstat()
    except OSError:
        return False
    if not stat.S_ISREG(info.st_mode):
        return False
    if stat.S_ISLNK(info.st_mode) or path.is_symlink():
        return False
    # Windows exposes the reparse bit through st_file_attributes.  Keep the
    # numeric fallback so this remains harmless on POSIX test hosts.
    attributes = int(getattr(info, "st_file_attributes", 0))
    return (attributes & 0x400) == 0


def _is_directory_non_alias(path: Path) -> bool:
    """Return true only for a directory that is not a link/reparse alias."""

    try:
        info = path.lstat()
    except OSError:
        return False
    if not stat.S_ISDIR(info.st_mode):
        return False
    if stat.S_ISLNK(info.st_mode) or path.is_symlink():
        return False
    attributes = int(getattr(info, "st_file_attributes", 0))
    return (attributes & 0x400) == 0


def _require_no_alias_ancestors(path: Path, field: str) -> None:
    """Reject a path whose existing ancestor is a symlink or reparse point."""

    current = path
    while True:
        try:
            info = current.lstat()
        except OSError as exc:
            raise ValidationError(f"cannot inspect {field} ancestor: {current}") from exc
        attributes = int(getattr(info, "st_file_attributes", 0))
        require(
            not stat.S_ISLNK(info.st_mode)
            and not current.is_symlink()
            and (attributes & 0x400) == 0,
            f"{field} traverses a symlink/reparse alias: {current}",
        )
        parent = current.parent
        if parent == current:
            return
        current = parent


def _require_manifest_path_non_alias(root: Path, relative: PurePosixPath, field: str) -> Path:
    """Reject aliases in every source-manifest path component."""

    candidate = root
    for index, part in enumerate(relative.parts):
        candidate /= part
        is_final = index + 1 == len(relative.parts)
        require(
            _is_regular_non_alias(candidate)
            if is_final
            else _is_directory_non_alias(candidate),
            f"{field} traverses a symlink/reparse alias or non-directory component",
        )
    return candidate


def _closed_source_tree_paths(root: Path) -> set[str]:
    """Return every regular file in the strict Sponza runtime tree."""

    tree_relative = PurePosixPath(MODEL_IDENTITY).parent
    tree_root = root
    for part in tree_relative.parts:
        tree_root /= part
        require(
            _is_directory_non_alias(tree_root),
            "strict source tree traverses a symlink/reparse alias or missing directory",
        )

    paths: set[str] = set()

    def walk_error(error: OSError) -> None:
        raise ValidationError(f"cannot enumerate strict source tree: {error}")

    for current_value, directory_names, file_names in os.walk(
        tree_root,
        topdown=True,
        onerror=walk_error,
        followlinks=False,
    ):
        current = Path(current_value)
        directory_names.sort()
        file_names.sort()
        for name in directory_names:
            require(
                _is_directory_non_alias(current / name),
                "strict source tree contains a symlink/reparse directory",
            )
        for name in file_names:
            path = current / name
            require(
                _is_regular_non_alias(path),
                "strict source tree contains a non-regular or aliased file",
            )
            paths.add(path.relative_to(root).as_posix())
    require(paths, "strict source tree contains no regular files")
    return paths


def _manifest_value(case: Mapping[str, Any], *names: str) -> Any:
    value = _first(case, *names, default=_MISSING)
    if value is not _MISSING:
        return value
    for parent_name in ("identity", "source", "sources", "manifest", "observations"):
        parent = case.get(parent_name)
        if isinstance(parent, Mapping):
            value = _first(parent, *names, default=_MISSING)
            if value is not _MISSING:
                return value
    return _MISSING


def _manifest_records(value: Any, *, field: str) -> list[Mapping[str, Any]]:
    if isinstance(value, Mapping):
        nested = _first(value, "files", "entries", "records", "items", "dependencies", default=_MISSING)
        require(nested is not _MISSING, f"{field} must contain files/entries")
        # Some runners keep the scene file in ``files`` and dependencies in a
        # sibling array.  Merge both while retaining one canonical list.
        values: list[Any] = []
        if isinstance(nested, list):
            values.extend(nested)
        else:
            values.append(nested)
        sibling = value.get("dependencies")
        if sibling is not None and sibling is not nested:
            require(isinstance(sibling, list), f"{field}.dependencies must be an array")
            values.extend(sibling)
    else:
        values = value if isinstance(value, list) else []
    require(isinstance(values, list) and values, f"{field} must contain at least one file record")
    records: list[Mapping[str, Any]] = []
    for index, record in enumerate(values):
        require(isinstance(record, Mapping), f"{field}[{index}] must be an object")
        records.append(record)
    return records


def _normalize_manifest_snapshot(
    value: Any,
    *,
    root: Path,
    field: str,
) -> tuple[list[dict[str, Any]], set[str]]:
    records = _manifest_records(value, field=field)
    normalized: list[dict[str, Any]] = []
    seen: set[str] = set()
    seen_folded: set[str] = set()
    for index, record in enumerate(records):
        label = f"{field}[{index}]"
        relative_value = _first(record, "relative_path", "relativePath", "path", default=_MISSING)
        require(isinstance(relative_value, str) and relative_value, f"{label}.relative_path is required")
        relative = relative_value.replace("\\", "/")
        require(relative == relative_value, f"{label}.relative_path must use forward slashes")
        pure = PurePosixPath(relative)
        require(
            not pure.is_absolute()
            and relative != "."
            and ".." not in pure.parts
            and not re.match(r"^(?:[A-Za-z]:|//)", relative),
            f"{label}.relative_path escapes the source root",
        )
        require(str(pure) == relative, f"{label}.relative_path is not normalized")
        folded = relative.casefold()
        require(relative not in seen, f"{label}.relative_path is duplicated")
        require(folded not in seen_folded, f"{label}.relative_path case-collides")
        seen.add(relative)
        seen_folded.add(folded)

        size_value = _first(record, "size_bytes", "sizeBytes", "bytes", "size", default=_MISSING)
        require(size_value is not _MISSING, f"{label}.size_bytes is required")
        size = _as_int(size_value, f"{label}.size_bytes", minimum=0)
        digest_value = _first(record, "sha256", "hash", "digest", default=_MISSING)
        digest = _valid_sha(digest_value, f"{label}.sha256")

        candidate = _require_manifest_path_non_alias(root, pure, f"{label}.relative_path")
        try:
            resolved = candidate.resolve(strict=False)
        except OSError as exc:
            raise ValidationError(f"cannot resolve {label}.relative_path") from exc
        _require_within(resolved, root, f"{label}.relative_path")
        actual_size = candidate.stat().st_size
        actual_digest = _sha256(candidate)
        require(actual_size == size, f"{label}.size_bytes does not match the source file")
        require(actual_digest == digest, f"{label}.sha256 does not match the source file")

        role_value = _first(record, "role", "kind", default="dependency")
        require(isinstance(role_value, str) and role_value, f"{label}.role must be non-empty")
        normalized.append(
            {
                "relative_path": relative,
                "size_bytes": size,
                "sha256": digest,
                "role": role_value,
            }
        )
    normalized.sort(key=lambda item: item["relative_path"])
    return normalized, seen


def _validate_source_manifest(
    case: Mapping[str, Any],
    *,
    evidence_root: Path,
    spec: CaseSpec | None,
) -> dict[str, Any]:
    """Validate closed before/after source and runtime dependency evidence."""

    manifest = _manifest_value(
        case,
        "source_manifest",
        "sourceManifest",
        "source_dependency_manifest",
        "sourceDependencyManifest",
    )
    if manifest is _MISSING:
        before_value = _manifest_value(
            case,
            "source_manifest_before",
            "sourceManifestBefore",
            "source_dependencies_before",
            "sourceDependenciesBefore",
        )
        after_value = _manifest_value(
            case,
            "source_manifest_after",
            "sourceManifestAfter",
            "source_dependencies_after",
            "sourceDependenciesAfter",
        )
        require(before_value is not _MISSING and after_value is not _MISSING, "source before/after manifest is required")
        manifest = {"before": before_value, "after": after_value}
    require(isinstance(manifest, Mapping), "source_manifest must be an object")

    root_value = _first(
        manifest,
        "source_root",
        "sourceRoot",
        "root",
        "root_path",
        "rootPath",
        default=_MISSING,
    )
    if root_value is _MISSING:
        root_value = _manifest_value(case, "source_root", "sourceRoot", "source_root_path")
    require(root_value is not _MISSING, "source manifest source_root is required")
    source_root_lexical = _lexical_path(
        root_value,
        base=evidence_root,
        field="source manifest source_root",
    )
    _require_no_alias_ancestors(source_root_lexical, "source manifest root")
    require(
        _is_directory_non_alias(source_root_lexical),
        "source manifest root must be a real non-symlink/non-reparse directory",
    )
    source_root = source_root_lexical.resolve(strict=False)
    require(source_root.is_dir(), f"source manifest root does not exist: {source_root}")

    before_value = _first(manifest, "before", "pre", "before_run", "beforeRun", default=_MISSING)
    after_value = _first(manifest, "after", "post", "after_run", "afterRun", default=_MISSING)
    require(before_value is not _MISSING and after_value is not _MISSING, "source manifest before and after snapshots are required")
    before, before_paths = _normalize_manifest_snapshot(before_value, root=source_root, field="source_manifest.before")
    after, after_paths = _normalize_manifest_snapshot(after_value, root=source_root, field="source_manifest.after")
    require(before == after, "source/dependency manifest changed between before and after")
    require(before_paths == after_paths, "source/dependency paths were added or removed")
    final_tree_paths = _closed_source_tree_paths(source_root)
    require(
        after_paths == final_tree_paths,
        "source manifest must exactly cover the final strict Sponza runtime tree",
    )

    source_entry = next((entry for entry in after if entry["relative_path"] == MODEL_IDENTITY), None)
    require(source_entry is not None, f"source manifest must include strict model {MODEL_IDENTITY}")
    source_model = source_root.joinpath(*PurePosixPath(MODEL_IDENTITY).parts)
    try:
        source_bytes = source_model.read_bytes()
    except OSError as exc:
        raise ValidationError(f"cannot read strict source model: {source_model}") from exc
    _parse_gltf(source_model, source_bytes, case_dir=source_root)
    try:
        source_document = json.loads(source_bytes.decode("utf-8"))
    except (UnicodeDecodeError, json.JSONDecodeError) as exc:
        raise ValidationError(f"strict source model is not valid UTF-8 JSON: {source_model}") from exc
    for field, uri in _iter_uris(source_document):
        dependency = _validate_external_uri(
            uri,
            artifact_parent=source_model.parent,
            case_dir=source_root,
            field=field,
        )
        if dependency is None:
            continue
        relative = dependency.resolve(strict=False).relative_to(source_root).as_posix()
        require(
            relative in after_paths,
            f"source manifest omits runtime dependency: {relative}",
        )
    if spec is not None and spec.require_sdf:
        require(
            "resources/GLTF_Sponza/sponza_sdf.bin" in after_paths,
            "DDGI case source manifest omits sponza_sdf.bin",
        )
    return {
        "source_root": str(source_root),
        "before": before,
        "after": after,
    }


def _validate_transaction_progression(
    case: Mapping[str, Any], marker_data: Mapping[str, Any]
) -> dict[str, Any]:
    evidence = _lookup(
        case,
        "transaction_progression",
        "transactionProgression",
        "transaction_evidence",
        "transactionEvidence",
    )
    if evidence is _MISSING:
        evidence = case.get("transaction")
    if evidence is _MISSING or evidence is None:
        return {}
    require(isinstance(evidence, Mapping), "structured transaction progression must be an object")

    def sequence(field: str, aliases: tuple[str, ...]) -> dict[str, int]:
        value = _first(evidence, field, *aliases, default=_MISSING)
        require(isinstance(value, Mapping), f"transaction.{field} must be an object")
        parsed: dict[str, int] = {}
        for name in ("baseline", "mutation", "undo", "redo"):
            raw = _first(value, name, default=_MISSING)
            require(raw is not _MISSING, f"transaction.{field}.{name} is required")
            parsed[name] = _as_int(
                raw,
                f"transaction.{field}.{name}",
                minimum=0,
                maximum=U64_MAX,
            )
        for previous, current in (
            ("baseline", "mutation"),
            ("mutation", "undo"),
            ("undo", "redo"),
        ):
            require(
                parsed[previous] < U64_MAX
                and parsed[current] == parsed[previous] + 1,
                f"transaction.{field} must advance exactly one step from {previous} to {current}",
            )
        return parsed

    revisions = sequence("revisions", ("document_revisions", "documentRevisions"))
    mapping_epochs = sequence("mapping_epochs", ("mappingEpochs", "epochs"))
    require(
        revisions["undo"] == marker_data["undo_revision"],
        "structured undo revision disagrees with the undo marker",
    )
    require(
        revisions["redo"] == marker_data["redo_revision"],
        "structured redo revision disagrees with the redo marker",
    )
    return {"revisions": revisions, "mapping_epochs": mapping_epochs}


def _validate_artifact_output(case: Mapping[str, Any], *, root: Path, marker_data: Mapping[str, Any]) -> dict[str, Any]:
    case_dir_value = _first(case, "case_dir", "caseDir", "output_dir", default=_MISSING)
    artifact_path_value, save_target_value, artifact_hash_value, artifact_size_value = _artifact_fields(case)
    require(artifact_path_value is not _MISSING, "saved artifact path is required")
    require(save_target_value is not _MISSING, "save_target is required to bind the output path")
    if case_dir_value is _MISSING:
        artifact_lexical = _lexical_path(artifact_path_value, base=root, field="artifact_path")
        require(_is_regular_non_alias(artifact_lexical), f"saved artifact must be a regular non-symlink file: {artifact_lexical}")
        artifact_path = artifact_lexical.resolve(strict=False)
        case_dir = artifact_path.parent
    else:
        case_dir_lexical = _lexical_path(case_dir_value, base=root, field="case_dir")
        require(not case_dir_lexical.is_symlink(), "case_dir must not be a symlink")
        case_dir = case_dir_lexical.resolve(strict=False)
        artifact_lexical = _lexical_path(artifact_path_value, base=case_dir_lexical, field="artifact_path")
        require(_is_regular_non_alias(artifact_lexical), f"saved artifact must be a regular non-symlink file: {artifact_lexical}")
        artifact_path = artifact_lexical.resolve(strict=False)
    _require_within(case_dir, root, "case_dir")
    _require_within(artifact_path, case_dir, "artifact_path")
    require(case_dir.is_dir(), f"case_dir does not exist: {case_dir}")
    require(_is_regular_non_alias(artifact_path), f"saved artifact must be a regular non-symlink file: {artifact_path}")
    require(artifact_path.suffix.lower() == ".glb", "P5 saved artifact must use the .glb extension")
    require(artifact_path.stat().st_size > 12, "saved artifact must be larger than the GLB header")

    save_target_lexical = _lexical_path(save_target_value, base=case_dir, field="save_target")
    require(_is_regular_non_alias(save_target_lexical), "save_target must identify a regular non-symlink file")
    save_target = save_target_lexical.resolve(strict=False)
    require(save_target == artifact_path, "save_target does not identify the saved artifact path")

    require(artifact_hash_value is not _MISSING, "artifact SHA-256 is required")
    expected_hash = _valid_sha(artifact_hash_value, "artifact SHA-256")
    require(artifact_size_value is not _MISSING, "artifact size is required")
    expected_size = _as_int(artifact_size_value, "artifact_size_bytes", minimum=1)

    artifact = validate_saved_artifact(artifact_path, case_dir=case_dir, require_glb=True)
    require(artifact["sha256"] == expected_hash, "artifact SHA-256 does not match")
    require(artifact["size_bytes"] == expected_size, "artifact size does not match")
    require(marker_data["save_bytes"] == expected_size, "save marker bytes do not match the artifact")
    return {**artifact, "path": str(artifact_path), "case_dir": str(case_dir)}


def _iter_uris(value: Any, path: str = "") -> Iterable[tuple[str, str]]:
    if isinstance(value, Mapping):
        for key, child in value.items():
            child_path = f"{path}.{key}" if path else str(key)
            if key == "uri":
                require(isinstance(child, str), f"{child_path} must be a string")
                yield child_path, child
            else:
                yield from _iter_uris(child, child_path)
    elif isinstance(value, list):
        for index, child in enumerate(value):
            yield from _iter_uris(child, f"{path}[{index}]")


def _validate_external_uri(
    uri: str, *, artifact_parent: Path, case_dir: Path, field: str
) -> Path | None:
    require(uri, f"{field} URI must not be empty")
    lowered = uri.lower()
    if lowered.startswith("data:"):
        return None
    try:
        parts = urlsplit(uri)
    except ValueError as exc:
        raise ValidationError(f"{field} URI is malformed") from exc
    require(not parts.scheme, f"{field} uses an unsupported URI scheme")
    require(not parts.netloc, f"{field} must not use a network URI without http/https")
    require(not parts.query and not parts.fragment, f"{field} must not contain a query or fragment")
    decoded = parts.path
    for _ in range(8):
        next_value = unquote(decoded)
        if next_value == decoded:
            break
        decoded = next_value
    else:
        raise ValidationError(f"{field} URI is excessively encoded")
    require(decoded and "\x00" not in decoded, f"{field} URI is malformed")
    require("\\" not in decoded, f"{field} URI must not contain backslashes")
    require(not re.match(r"^(?:[A-Za-z]:|/|//)", decoded), f"{field} URI must be relative")
    pure = PurePosixPath(decoded)
    require(not pure.is_absolute() and ".." not in pure.parts, f"{field} URI escapes the case directory")
    lexical_candidate = artifact_parent.joinpath(*pure.parts)
    _require_manifest_path_non_alias(artifact_parent, pure, field)
    candidate = lexical_candidate.resolve(strict=False)
    _require_within(candidate, case_dir.resolve(strict=False), f"{field} URI")
    require(candidate.is_file(), f"{field} dependency is missing: {uri!r}")
    return candidate


def _validate_gltf_json(document: Any, *, artifact_parent: Path, case_dir: Path) -> None:
    require(isinstance(document, Mapping), "glTF JSON root must be an object")
    asset = document.get("asset")
    require(isinstance(asset, Mapping), "glTF asset object is required")
    version = asset.get("version")
    require(isinstance(version, str) and re.fullmatch(r"2(?:\.\d+)?", version) is not None, "glTF asset.version must be glTF 2")
    for field, uri in _iter_uris(document):
        _validate_external_uri(uri, artifact_parent=artifact_parent, case_dir=case_dir, field=field)


def _parse_gltf(path: Path, data: bytes, *, case_dir: Path) -> None:
    try:
        text = data.decode("utf-8")
    except UnicodeDecodeError as exc:
        raise ValidationError(f".gltf is not valid UTF-8: {path}") from exc
    try:
        document = json.loads(text)
    except json.JSONDecodeError as exc:
        raise ValidationError(f".gltf is not valid JSON: {path}: {exc}") from exc
    _validate_gltf_json(document, artifact_parent=path.parent, case_dir=case_dir)


def _parse_glb(path: Path, data: bytes, *, case_dir: Path) -> None:
    require(len(data) >= 12, ".glb is shorter than its 12-byte header")
    magic, version, declared_length = struct.unpack_from("<4sII", data, 0)
    require(magic == b"glTF", ".glb magic is invalid")
    require(version == 2, ".glb version must be 2")
    require(declared_length == len(data), ".glb header length does not match the file")
    offset = 12
    json_document: Any = None
    chunk_index = 0
    while offset < declared_length:
        require(offset + 8 <= declared_length, ".glb chunk header is truncated")
        chunk_length, chunk_type = struct.unpack_from("<II", data, offset)
        offset += 8
        require(chunk_length % 4 == 0, ".glb chunk length must be 4-byte aligned")
        require(chunk_length <= declared_length - offset, ".glb chunk exceeds the declared file length")
        chunk = data[offset : offset + chunk_length]
        offset += chunk_length
        if chunk_index == 0:
            require(chunk_type == 0x4E4F534A, "the first .glb chunk must be JSON")
            try:
                json_text = chunk.rstrip(b" \t\r\n\x00").decode("utf-8")
                json_document = json.loads(json_text)
            except (UnicodeDecodeError, json.JSONDecodeError) as exc:
                raise ValidationError(f".glb JSON chunk is invalid: {path}") from exc
        chunk_index += 1
    require(offset == declared_length and json_document is not None, ".glb has no JSON chunk")
    _validate_gltf_json(json_document, artifact_parent=path.parent, case_dir=case_dir)


def validate_saved_artifact(
    path: str | Path,
    *,
    case_dir: str | Path | None = None,
    require_glb: bool = False,
) -> dict[str, Any]:
    """Validate a saved `.gltf` or `.glb` and return its derived identity.

    External glTF URIs are accepted only when they are embedded ``data:`` URIs
    or regular local files contained by ``case_dir``.  Network schemes are
    rejected, and the function never follows a dependency through a symlink.
    """

    lexical_artifact = _lexical_path(path, base=Path.cwd(), field="saved artifact")
    require(_is_regular_non_alias(lexical_artifact), f"saved artifact must be a regular non-symlink file: {lexical_artifact}")
    artifact_path = lexical_artifact.resolve(strict=False)
    size = artifact_path.stat().st_size
    require(size > 0, "saved artifact must be non-empty")
    root_lexical = artifact_path.parent if case_dir is None else _lexical_path(case_dir, base=Path.cwd(), field="artifact case directory")
    root = root_lexical.resolve(strict=False)
    require(root.is_dir(), f"artifact case directory does not exist: {root}")
    require(not root_lexical.is_symlink(), "artifact case directory must not be a symlink")
    _require_within(artifact_path, root, "saved artifact")
    try:
        data = artifact_path.read_bytes()
    except OSError as exc:
        raise ValidationError(f"cannot read saved artifact {artifact_path}: {exc}") from exc
    suffix = artifact_path.suffix.lower()
    if suffix == ".gltf":
        require(not require_glb, "P5 saved artifact must be a GLB")
        _parse_gltf(artifact_path, data, case_dir=root)
        artifact_format = "gltf"
    elif suffix == ".glb":
        _parse_glb(artifact_path, data, case_dir=root)
        artifact_format = "glb"
    else:
        raise ValidationError("saved artifact must use a .gltf or .glb extension")
    return {"format": artifact_format, "size_bytes": size, "sha256": hashlib.sha256(data).hexdigest()}


def _validate_case_metadata(case: Mapping[str, Any], marker_data: Mapping[str, Any], *, spec: CaseSpec | None, sdf_count: int) -> None:
    if _lookup(case, "strict_scene_ready_count", "strictSceneReadyCount") is not _MISSING:
        value = _lookup(case, "strict_scene_ready_count", "strictSceneReadyCount")
        require(_as_int(value, "strict_scene_ready_count", minimum=0) == 1, "strict scene-ready count must be 1")
    for field, aliases in (
        ("rendered_frames", ("rendered_frames", "renderedFrames", "frames")),
        ("frame_count", ("frame_count", "frameCount")),
    ):
        value = _lookup(case, *aliases)
        if value is not _MISSING:
            require(_as_int(value, field, minimum=0) == 40, f"{field} must be 40")
    for field, aliases in (
        ("auto_exit_successful", ("auto_exit_successful", "autoExitSuccessful")),
        ("auto_exit", ("auto_exit", "autoExit")),
    ):
        value = _lookup(case, *aliases)
        if value is not _MISSING:
            require(_as_bool(value, field), f"{field} must be true")
    for field in ("literal_marker_order_valid", "documented_marker_contract_passed"):
        value = _lookup(case, field, "literalMarkerOrderValid" if field == "literal_marker_order_valid" else "documentedMarkerContractPassed")
        if value is not _MISSING:
            require(_as_bool(value, field), f"{field} must be true")
    counts = _lookup(case, "literal_marker_counts", "literalMarkerCounts")
    if counts is not _MISSING:
        require(isinstance(counts, Mapping), "literal_marker_counts must be an object")
        key_aliases = {
            "scene-ready": ("scene-ready", "sceneReady"),
            "automation-start": ("automation-start", "automationStart"),
            "undo-observed": ("undo-observed", "undoObserved"),
            "redo-observed": ("redo-observed", "redoObserved"),
            "save-complete": ("save-complete", "saveComplete"),
            "frame-budget-complete": ("frame-budget-complete", "frameBudgetComplete"),
            "automation-complete": ("automation-complete", "automationComplete"),
        }
        for marker, aliases in key_aliases.items():
            value = _first(counts, *aliases, default=_MISSING)
            require(value is not _MISSING and _as_int(value, f"literal_marker_counts.{marker}", minimum=0) == 1, f"literal marker count for {marker} must be 1")
    sdf_value = _lookup(case, "ddgi_sdf_marker_count", "ddgiSdfMarkerCount")
    if sdf_value is not _MISSING:
        require(_as_int(sdf_value, "ddgi_sdf_marker_count", minimum=0) == sdf_count, "DDGI SDF marker count disagrees with captured output")
    if spec is not None:
        require((sdf_count > 0) == spec.require_sdf, "DDGI SDF marker rule does not match the fixed case")


def validate_case(
    case: Mapping[str, Any] | str | Path,
    *,
    evidence_root: str | Path | None = None,
    expected_case: CaseSpec | None = None,
    expected_executable_path: str | Path | None = None,
    expected_executable_sha256: str | None = None,
) -> dict[str, Any]:
    """Validate one smoke case and return a derived, JSON-safe result.

    ``case`` may be an in-memory mapping or a path to a runner result JSON.  The
    record schema is intentionally field-oriented and accepts common snake/camel
    aliases so a future runner can evolve without changing the protocol parser.
    No production command-line option names are consumed here.
    """

    raw = _load_json(case, label="case")
    require(isinstance(raw, Mapping), "case JSON root must be an object")
    claimed_result: dict[str, Any] | None = None
    if raw.get("schema") == CASE_RESULT_SCHEMA:
        claimed_result = dict(raw)
        evidence = raw.get("evidence")
        require(isinstance(evidence, Mapping), "derived case result must contain an evidence object")
        case_map = dict(evidence)
    else:
        case_map = dict(raw)
    name = _case_name(case_map)
    spec = _case_spec(name, expected_case)
    _validate_case_options(case_map, spec)

    if evidence_root is None:
        root_value = _first(case_map, "evidence_root", "evidenceRoot", default=_MISSING)
        if root_value is not _MISSING:
            root = Path(root_value).resolve(strict=False)
        else:
            case_dir_value = _first(case_map, "case_dir", "caseDir", "output_dir", default=_MISSING)
            if case_dir_value is _MISSING:
                artifact_value, _, _, _ = _artifact_fields(case_map)
                require(artifact_value is not _MISSING, "evidence_root is required when artifact path is not inferable")
                root = Path(artifact_value).resolve(strict=False).parent
            else:
                root = Path(case_dir_value).resolve(strict=False).parent
    else:
        root = Path(evidence_root).resolve(strict=False)
    require(root.is_dir(), f"evidence root does not exist: {root}")

    output = _combined_output(case_map, root)
    execution = _validate_execution(case_map, output)
    marker_data = _validate_marker_lines(output, spec)
    transaction = _validate_transaction_progression(case_map, marker_data)
    source_manifest = _validate_source_manifest(case_map, evidence_root=root, spec=spec)
    sdf_count = sum(1 for _, line in _line_entries(output) if DDGI_SDF_PATTERN.search(line) is not None)
    _validate_case_metadata(case_map, marker_data, spec=spec, sdf_count=sdf_count)

    expected_path = None if expected_executable_path is None else _resolve_path(expected_executable_path, base=root, field="expected executable path")
    executable = _validate_executable(case_map, root=root, expected_path=expected_path, expected_sha256=expected_executable_sha256)
    artifact = _validate_artifact_output(case_map, root=root, marker_data=marker_data)
    source_root = Path(source_manifest["source_root"]).resolve(strict=False)
    artifact_path = Path(artifact["path"]).resolve(strict=False)
    require(
        not _is_within(artifact_path, source_root),
        "saved artifact must not overwrite or reside inside the source root",
    )

    # Optional structured observations are cross-checked when present.  Marker
    # fields remain authoritative; these checks prevent a runner from recording a
    # contradictory success result alongside a valid-looking log.
    for field, expected in (
        ("undo_count", 0),
        ("redo_count", 1),
    ):
        value = _lookup(case_map, field, "undoCount" if field == "undo_count" else "redoCount")
        if value is not _MISSING:
            require(_as_int(value, field, minimum=0) == expected, f"{field} does not match undo marker")
    for field, expected in (("final_undo_count", 1), ("final_redo_count", 0)):
        value = _lookup(case_map, field, "finalUndoCount" if field == "final_undo_count" else "finalRedoCount")
        if value is not _MISSING:
            require(_as_int(value, field, minimum=0) == expected, f"{field} does not match redo marker")
    for field, expected in (("reload_valid", 1), ("semantic_equal", 1)):
        value = _lookup(case_map, field, "reloadValid" if field == "reload_valid" else "semanticEqual")
        if value is not _MISSING:
            require(_as_int(value, field, minimum=0) == expected, f"{field} must be {expected}")

    claimed_pass = _first(case_map, "passed", "pass", default=_MISSING)
    if claimed_pass is not _MISSING:
        require(type(claimed_pass) is bool and claimed_pass, "case result cannot claim pass when validation is incomplete")

    normalized_evidence = {
        "case": name,
        "mode": marker_data["mode"],
        "timed_out": False,
        "exit_code": 0,
        "combined_output": output,
        "case_dir": artifact["case_dir"],
        "artifact_path": artifact["path"],
        "save_target": artifact["path"],
        "artifact_sha256": artifact["sha256"],
        "artifact_size_bytes": artifact["size_bytes"],
        "demo_path": executable["path"],
        "expected_demo_sha256": executable["sha256"],
        "demo_sha256": executable["sha256"],
        "demo_sha256_before": executable["sha256"],
        "demo_sha256_after": executable["sha256"],
        "ddgi_sdf_marker_count": sdf_count,
        "transaction_progression": transaction,
        "source_manifest": source_manifest,
        "rendered_frames": 40,
        "auto_exit_successful": True,
        "passed": True,
    }
    result = {
        "schema": CASE_RESULT_SCHEMA,
        "case": name,
        "mode": marker_data["mode"],
        "passed": True,
        "timed_out": execution["timed_out"],
        "exit_code": execution["exit_code"],
        "failure_matches": execution["failure_matches"],
        "authoritative_markers": {
            marker: {"count": 1, "byte_offset": marker_data["authoritative_offsets"][marker]}
            for marker in AUTHORITATIVE_MARKERS
        },
        "supplementary_markers": {
            marker: {"count": 1, "byte_offset": marker_data["supplementary_offsets"][marker]}
            for marker in SUPPLEMENTARY_MARKERS
        },
        "ddgi_sdf_marker_count": sdf_count,
        "executable": executable,
        "artifact": artifact,
        "transaction_progression": transaction,
        "source_manifest": source_manifest,
        "frames": 40,
        "auto_exit": True,
        "revisions": {
            "undo": marker_data["undo_revision"],
            "redo": marker_data["redo_revision"],
            "save": marker_data["save_revision"],
            "clean": marker_data["clean_revision"],
        },
        "evidence": normalized_evidence,
    }
    if claimed_result is not None:
        require(set(claimed_result) == set(result), "derived case result contains unknown or missing fields")
        require(claimed_result == result, "derived case result does not match independently revalidated evidence")
    return result


def _summary_records(raw: Mapping[str, Any], *, root: Path) -> list[Mapping[str, Any]]:
    records = _first(raw, "cases", "case_results", "caseResults", default=_MISSING)
    require(isinstance(records, list), "summary cases must be an array")
    loaded: list[Mapping[str, Any]] = []
    for index, record in enumerate(records):
        if isinstance(record, (str, Path)):
            path = _resolve_path(record, base=root, field=f"cases[{index}]")
            _require_within(path, root, f"cases[{index}]")
            value = _load_json(path, label=f"cases[{index}]")
            require(isinstance(value, Mapping), f"cases[{index}] JSON root must be an object")
            loaded.append(dict(value))
            continue
        require(isinstance(record, Mapping), f"cases[{index}] must be an object or result path")
        result_path = _first(record, "result_path", "resultPath", default=_MISSING)
        if result_path is not _MISSING:
            path = _resolve_path(result_path, base=root, field=f"cases[{index}].result_path")
            _require_within(path, root, f"cases[{index}].result_path")
            value = _load_json(path, label=f"cases[{index}].result_path")
            require(isinstance(value, Mapping), f"cases[{index}].result_path JSON root must be an object")
            merged = dict(value)
            merged.update({key: value for key, value in record.items() if key != "result_path"})
            loaded.append(merged)
        else:
            loaded.append(dict(record))
    return loaded


def _load_summary(value: Mapping[str, Any] | str | Path) -> tuple[dict[str, Any], Path]:
    if isinstance(value, (str, Path)):
        input_path = Path(value)
        if input_path.is_dir():
            summary_path = input_path / "summary.json"
            root = input_path.resolve(strict=False)
        else:
            summary_path = input_path
            root = input_path.parent.resolve(strict=False)
        raw = _load_json(summary_path, label="summary")
        require(isinstance(raw, Mapping), "summary JSON root must be an object")
        return dict(raw), root
    require(isinstance(value, Mapping), "summary must be an object or JSON path")
    raw = dict(value)
    root_value = _first(raw, "evidence_root", "evidenceRoot", default=_MISSING)
    root = Path(root_value).resolve(strict=False) if root_value is not _MISSING else Path.cwd().resolve(strict=False)
    return raw, root


def _derive_summary_identity(record: Mapping[str, Any]) -> dict[str, Any]:
    executable = record.get("executable")
    if isinstance(executable, Mapping):
        path = _first(executable, "path", "demo_path", "executable_path", default=_MISSING)
        digest = _first(executable, "sha256", "demo_sha256", default=_MISSING)
        if path is not _MISSING and digest is not _MISSING:
            return {"path": path, "sha256": digest}
    identity = record.get("identity")
    if isinstance(identity, Mapping):
        path = _first(identity, "path", "demo_path", "executable_path", default=_MISSING)
        digest = _first(identity, "sha256", "demo_sha256", default=_MISSING)
        if path is not _MISSING and digest is not _MISSING:
            return {"path": path, "sha256": digest}
    path = _lookup(record, "demo_path", "executable_path", "executablePath")
    digest = _lookup(record, "expected_demo_sha256", "expected_executable_sha256", "demo_sha256")
    require(path is not _MISSING and digest is not _MISSING, "summary identity cannot be derived from its first case")
    return {"path": path, "sha256": digest}


def validate_summary(
    summary: Mapping[str, Any] | Sequence[Mapping[str, Any]] | str | Path,
    *,
    evidence_root: str | Path | None = None,
    repo_root: str | Path | None = None,
) -> dict[str, Any]:
    """Validate the complete five-case matrix and derive a closed summary.

    A directory or JSON path is reloaded from disk, including each referenced
    case result.  Passing a sequence is convenient for a runner before it writes
    its summary, while still applying exactly the same checks.
    """

    claimed_derived_summary: dict[str, Any] | None = None
    if isinstance(summary, Sequence) and not isinstance(summary, (str, bytes, Path, Mapping)):
        raw: dict[str, Any] = {"cases": list(summary)}
        root = Path(evidence_root).resolve(strict=False) if evidence_root is not None else Path.cwd().resolve(strict=False)
    else:
        raw, inferred_root = _load_summary(summary)  # type: ignore[arg-type]
        root = Path(evidence_root).resolve(strict=False) if evidence_root is not None else inferred_root
        if raw.get("schema") == SUMMARY_RESULT_SCHEMA and "artifact_paths" in raw:
            claimed_derived_summary = dict(raw)
    require(root.is_dir(), f"evidence root does not exist: {root}")
    if repo_root is not None:
        repository = Path(repo_root).resolve(strict=False)
        require(not _is_within(root, repository), "evidence root must be outside the repository")

    records = _summary_records(raw, root=root)
    require(records, "summary must contain case records")
    identity = raw.get("identity")
    if not isinstance(identity, Mapping):
        identity = raw.get("executable")
    if not isinstance(identity, Mapping):
        identity = _derive_summary_identity(records[0])
    identity_path_value = _first(identity, "demo_path", "executable_path", "path", default=_MISSING)
    if identity_path_value is _MISSING:
        identity_path_value = _first(raw, "demo_path", "executable_path", default=_MISSING)
    require(identity_path_value is not _MISSING, "summary executable path is required")
    identity_path = _resolve_path(identity_path_value, base=root, field="summary executable path")
    _require_within(identity_path, root, "summary executable path")
    require(identity_path.is_file(), f"summary executable does not exist: {identity_path}")
    identity_hash_value = _first(identity, "demo_sha256", "sha256", default=_MISSING)
    if identity_hash_value is _MISSING:
        identity_hash_value = _first(raw, "demo_sha256", "expected_demo_sha256", default=_MISSING)
    require(identity_hash_value is not _MISSING, "summary executable SHA-256 is required")
    identity_hash = _valid_sha(identity_hash_value, "summary executable SHA-256")
    actual_identity_hash = _sha256(identity_path)
    require(identity_hash == actual_identity_hash, "summary executable identity hash does not match")
    for key in ("demo_sha256_before", "before_sha256", "demo_sha256_after", "after_sha256"):
        value = _first(identity, key, default=_MISSING)
        if value is not _MISSING:
            require(_valid_sha(value, f"summary identity {key}") == actual_identity_hash, f"summary identity {key} drifted")
    stable = _first(raw, "demo_sha256_stable", "demoSha256Stable", default=_MISSING)
    if stable is not _MISSING:
        require(_as_bool(stable, "demo_sha256_stable"), "summary executable hash is not stable")

    require(len(records) == len(CASE_SPECS) == 5, "summary must contain exactly five case records")
    names = [_case_name(record) for record in records]
    expected_names = [spec.name for spec in CASE_SPECS]
    require(names == expected_names, "summary cases must use the canonical five-case order exactly once")

    validated: list[dict[str, Any]] = []
    case_dirs: set[Path] = set()
    artifact_paths: set[Path] = set()
    for index, record in enumerate(records):
        result = validate_case(
            record,
            evidence_root=root,
            expected_case=CASE_SPECS[index],
            expected_executable_path=identity_path,
            expected_executable_sha256=identity_hash,
        )
        case_dir = Path(result["artifact"]["case_dir"]).resolve(strict=False)
        artifact_path = Path(result["artifact"]["path"]).resolve(strict=False)
        require(case_dir not in case_dirs, "per-case output directories must be distinct")
        require(artifact_path not in artifact_paths, "per-case saved artifact paths must be distinct")
        require(
            all(not _is_within(case_dir, prior) and not _is_within(prior, case_dir) for prior in case_dirs),
            "per-case output directories must not overlap",
        )
        case_dirs.add(case_dir)
        artifact_paths.add(artifact_path)
        validated.append(result)

    summary_object = raw.get("summary") if isinstance(raw.get("summary"), Mapping) else raw
    claimed_status = _first(summary_object, "status", "verdict", default=_MISSING)
    if claimed_status is not _MISSING:
        require(claimed_status == "pass", "aggregate status must be pass only after all five cases validate")
    claimed_passed = _first(summary_object, "passed", "passed_cases", "passedCases", default=_MISSING)
    if claimed_passed is not _MISSING:
        require(_as_int(claimed_passed, "summary.passed", minimum=0) == 5, "summary.passed must be 5")
    claimed_failed = _first(summary_object, "failed", "failed_cases", "failedCases", default=_MISSING)
    if claimed_failed is not _MISSING:
        require(_as_int(claimed_failed, "summary.failed", minimum=0) == 0, "summary.failed must be 0")
    claimed_total = _first(summary_object, "total", "case_count", "caseCount", default=_MISSING)
    if claimed_total is not _MISSING:
        require(_as_int(claimed_total, "summary.total", minimum=0) == 5, "summary.total must be 5")
    claimed_pass = _first(raw, "pass", default=_MISSING)
    if claimed_pass is not _MISSING:
        require(type(claimed_pass) is bool and claimed_pass, "summary cannot claim pass independently of case validation")

    result = {
        "schema": SUMMARY_RESULT_SCHEMA,
        "status": "pass",
        "passed": 5,
        "failed": 0,
        "total": 5,
        "identity": {"path": str(identity_path), "sha256": actual_identity_hash},
        "cases": validated,
        "artifact_paths": [str(path) for path in sorted(artifact_paths, key=lambda item: str(item))],
    }
    if claimed_derived_summary is not None:
        require(set(claimed_derived_summary) == set(result), "derived summary contains unknown or missing fields")
        require(claimed_derived_summary == result, "derived summary does not match independently revalidated case evidence")
    return result


def validate_case_file(path: str | Path, *, evidence_root: str | Path | None = None, **kwargs: Any) -> dict[str, Any]:
    case_path = Path(path)
    if evidence_root is not None and not case_path.is_absolute():
        case_path = Path(evidence_root) / case_path
    return validate_case(case_path, evidence_root=evidence_root, **kwargs)


def validate_summary_file(path: str | Path, *, evidence_root: str | Path | None = None, repo_root: str | Path | None = None) -> dict[str, Any]:
    summary_path = Path(path)
    if evidence_root is not None and not summary_path.is_absolute():
        summary_path = Path(evidence_root) / summary_path
    return validate_summary(summary_path, evidence_root=evidence_root, repo_root=repo_root)


# Explicit aliases keep the API discoverable for runners that prefer a noun-like
# name; all aliases point to the same deterministic implementation.
validate_case_evidence = validate_case
validate_five_case_summary = validate_summary
validate_aggregate = validate_summary
validate_saved_output = validate_saved_artifact


def _write_json(path: Path, value: Mapping[str, Any]) -> None:
    path.parent.mkdir(parents=True, exist_ok=True)
    path.write_text(json.dumps(value, indent=2, sort_keys=True) + "\n", encoding="utf-8")


def write_case_result(path: str | Path, case: Mapping[str, Any], **kwargs: Any) -> dict[str, Any]:
    result = validate_case(case, **kwargs)
    output_path = Path(path)
    _write_json(output_path, result)
    return validate_case_file(output_path, **kwargs)


def write_summary(path: str | Path, summary: Mapping[str, Any] | Sequence[Mapping[str, Any]], **kwargs: Any) -> dict[str, Any]:
    result = validate_summary(summary, **kwargs)
    output_path = Path(path)
    _write_json(output_path, result)
    return validate_summary_file(output_path, **kwargs)


def _cli() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    group = parser.add_mutually_exclusive_group(required=True)
    group.add_argument("--case", type=Path, help="one case result JSON")
    group.add_argument("--summary", type=Path, help="summary JSON or evidence directory")
    parser.add_argument("--evidence-root", type=Path)
    parser.add_argument("--repo-root", type=Path)
    parser.add_argument("--output", type=Path, help="write the derived validation JSON")
    args = parser.parse_args()
    try:
        if args.case is not None:
            result = validate_case_file(args.case, evidence_root=args.evidence_root)
        else:
            result = validate_summary_file(args.summary, evidence_root=args.evidence_root, repo_root=args.repo_root)
    except ValidationError as exc:
        print(f"Validation failed: {exc}", file=sys.stderr)
        return 1
    rendered = json.dumps(result, indent=2, sort_keys=True)
    if args.output is None:
        print(rendered)
    else:
        args.output.parent.mkdir(parents=True, exist_ok=True)
        args.output.write_text(rendered + "\n", encoding="utf-8")
    return 0


if __name__ == "__main__":
    raise SystemExit(_cli())
