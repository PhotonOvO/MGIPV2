#!/usr/bin/env python3
"""Reconcile and transactionally publish the MGIF editor progress ledger."""

from __future__ import annotations

import argparse
import base64
import ctypes
import errno
import hashlib
import importlib.util
import json
import os
import re
import shutil
import stat
import subprocess
import sys
import tempfile
import time
import uuid
from ctypes import wintypes
from datetime import datetime, timezone
from pathlib import Path, PureWindowsPath
from typing import Any, Sequence


def _load_schema_helper() -> Any:
    helper_path = Path(__file__).resolve().with_name("json_schema_subset.py")
    module_name = "_mgif_json_schema_subset_" + hashlib.sha256(os.fsencode(helper_path)).hexdigest()[:16]
    module = sys.modules.get(module_name)
    if module is not None:
        return module
    spec = importlib.util.spec_from_file_location(module_name, helper_path)
    if spec is None or spec.loader is None:
        raise ImportError(f"cannot load progress-ledger schema helper from {helper_path}")
    module = importlib.util.module_from_spec(spec)
    sys.modules[module_name] = module
    try:
        spec.loader.exec_module(module)
    except BaseException:
        sys.modules.pop(module_name, None)
        raise
    return module


_SCHEMA_HELPER = _load_schema_helper()
JsonSchemaError = _SCHEMA_HELPER.JsonSchemaError
JsonSchemaValidator = _SCHEMA_HELPER.JsonSchemaValidator


VERSION = 2
RUN_ID = "run_ede89c4421e9"
STATE_REL = Path("docs/editor-parity/progress-ledger-state.json")
LEDGER_REL = Path("docs/gltf-scene-editor-parity-progress.md")
SPEC_REL = Path("docs/gltf-scene-editor-parity-task.md")
MANIFEST_REL = Path("docs/editor-parity/manifest.json")
LICENSE_REL = Path("docs/editor-parity/LICENSE-SOURCES.md")
SCHEMA_REL = Path("docs/editor-parity/schemas/progress-ledger.schema.json")
VALIDATOR_REL = Path("tools/validate_editor_progress_ledger.py")
STORE_REL = Path("docs/editor-parity/progress-ledger")
AUTHORITY_REL = STORE_REL / "authority.json"
VECTORS_REL = STORE_REL / "protocol-vectors.json"
EVIDENCE_REL = STORE_REL / "evidence/sha256"
BUNDLES_REL = STORE_REL / "bundles/sha256"
POINTER_REL = Path("docs/editor-parity/progress-ledger-current.json")
LOCK_REL = Path("docs/editor-parity/.progress-ledger.lock")
JOURNAL_REL = Path("docs/editor-parity/.progress-ledger-journal.json")
AUTHORITY_SHA256 = "4ae61046c7d5c64eefd361f66df1ecf8eccf8c59a0c8fe20a60473fb751cbbf5"
VECTORS_SHA256 = "7e27003d2cde3ac6df5341575a1422432876178d2c6cb9f10df2258251ac8097"
FIXED_POLICY = {"f42": "BLOCKED", "r24": "BLOCKED", "metal": "Not Verified", "releaseDistribution": "NOT PASS"}
PHASES = [f"P{i}" for i in range(11)]
TASK_STATUSES = {"pending", "ready", "dispatched", "completed", "failed", "blocked"}
DISPATCH_STATUSES = TASK_STATUSES | {"stopped"}
DECISION_OUTCOMES = {"pending", "accepted", "rejected", "superseded"}
MAX_LEDGER_ROWS = 100
DEFERRED = ["L5", "L6", "L7", "L8", "L9", "L10"]
MATERIALIZED = [
    "P5-00", "P5-10", "P5-20", "P5-90", "G5", "O06-BIND", "P6-00", "P6-10", "P7-00",
    "P6-20", "P6-90", "G6", "P7-10", "P7-90", "G7", "P8-00", "P8-10", "P8-11",
    "P8-12", "P8-90", "G8", "P9-00", "P9-10", "P9-11", "P9-12", "P9-90", "G9",
    "P10-00", "P10-10", "P10-11", "P10-90", "P10-EXEC", "G10",
]
MATERIALIZED_ORDER = {name: i for i, name in enumerate(MATERIALIZED)}
GATE_TASKS = {
    "P5": "task_bd6e62faa588", "P6": "task_4af12cfad98a", "P7": "task_3f216d250677",
    "P8": "task_eb85190ed1ac", "P9": "task_e6d16b19af18", "P10": "task_04e52d906959",
}
BOOTSTRAP_TASKS = {
    "task_ba8ade72d3e9", "task_92d30650c179", "task_831f45f22072", "task_8261f1595760",
    "task_958b7d42d0a2", "task_a4832b5ed3e6", "task_c346ebff90f2", "task_d73712d125c5",
}
SHA_RE = re.compile(r"^[0-9a-f]{64}$")
ATTESTATION_PREFIX = "PROGRESS_LEDGER_ATTESTATION="
RECOVERY_SETTLEMENT_PROVENANCE = "coordinator_review_settlement"
RECOVERY_SETTLEMENT_TASK_ID = "task_8c29dd510a99"
RECOVERY_SETTLEMENT_MESSAGE_ID = "msg_341ac0d9d94f"
RECOVERY_SETTLEMENT_DISPATCH_ID = "ctx_3cc9e9bc9f70"
RECOVERY_SETTLEMENT_FINDINGS = [
    "PROBABLE_PROHIBITED_DEMO_TXT_WRITE",
    "P5_LIVE_VULKAN_SCENE_SMOKE_NOT_REACHED",
    "BOUNDED_GLTF_VALIDATOR_REJECTS_SPONZA",
]
RECOVERY_SETTLEMENT_POLICY = {
    "F42": "BLOCKED", "R24": "BLOCKED", "Metal": "Not Verified",
    "releaseDistribution": "NOT PASS",
}
RECOVERY_SETTLEMENT_NOTE = (
    "The verification objective completed and its FAIL verdict is preserved. This settlement only releases "
    "the already-materialized R3 recovery dependency; it does not accept P5 or authorize G5."
)
RECOVERY_SETTLEMENT_KEYS = {
    "provenance", "objectiveCompleted", "acceptanceVerdict", "workerOutcome",
    "originalMessageId", "originalDispatchId", "reportPath", "reportSha256",
    "commandLedgerSha256", "postSourceManifestSha256", "machineReceiptSha256",
    "integrityCheckSha256", "blockingFindings", "fixedPolicy", "note",
}
RECOVERY_SETTLEMENT_HASH_KEYS = {
    "reportSha256", "commandLedgerSha256", "postSourceManifestSha256",
    "machineReceiptSha256", "integrityCheckSha256",
}


class LedgerError(RuntimeError):
    pass


def need(condition: bool, message: str) -> None:
    if not condition:
        raise LedgerError(message)


def payload(value: Any) -> bytes:
    return json.dumps(value, ensure_ascii=True, sort_keys=True, separators=(",", ":")).encode("ascii")


def document(value: Any) -> bytes:
    return json.dumps(value, ensure_ascii=True, sort_keys=True, indent=2).encode("ascii") + b"\n"


def sha(data: bytes) -> str:
    return hashlib.sha256(data).hexdigest()


def file_sha(path: Path) -> str:
    digest = hashlib.sha256()
    with path.open("rb") as stream:
        for chunk in iter(lambda: stream.read(1024 * 1024), b""):
            digest.update(chunk)
    return digest.hexdigest()


def json_bytes(data: bytes, label: str) -> Any:
    try:
        return json.loads(data.decode("utf-8"))
    except (UnicodeError, json.JSONDecodeError) as exc:
        raise LedgerError(f"cannot parse {label}: {exc}") from exc


def load(path: Path, label: str | None = None) -> Any:
    try:
        return json_bytes(pinned(path, label or str(path)), label or str(path))
    except OSError as exc:
        raise LedgerError(f"cannot read {label or path}: {exc}") from exc


def closed(value: Any, keys: set[str], label: str) -> dict[str, Any]:
    need(isinstance(value, dict), f"{label} must be an object")
    actual = set(value)
    need(actual == keys, f"{label} keys differ: missing={sorted(keys-actual)}, extra={sorted(actual-keys)}")
    return value


def utc(value: Any, label: str, nullable: bool = False) -> str | None:
    if value is None and nullable:
        return None
    need(isinstance(value, str) and value.strip(), f"{label} must be a date-time")
    text = value.strip().replace(" ", "T", 1)
    if text.endswith("Z"):
        text = text[:-1] + "+00:00"
    try:
        parsed = datetime.fromisoformat(text)
    except ValueError as exc:
        raise LedgerError(f"{label} is not ISO 8601") from exc
    if parsed.tzinfo is None:
        parsed = parsed.replace(tzinfo=timezone.utc)
    rendered = parsed.astimezone(timezone.utc).isoformat(timespec="milliseconds")
    return rendered[:-6] + "Z"


def now() -> str:
    rendered = datetime.now(timezone.utc).isoformat(timespec="milliseconds")
    return rendered[:-6] + "Z"


def epoch(value: str) -> float:
    return datetime.fromisoformat(value[:-1] + "+00:00").timestamp()


def phase_number(phase: str) -> int:
    return int(phase[1:])


def verdicts(phase: str) -> set[str]:
    if phase == "P0":
        return {"P1 ENGINEERING READINESS: READY"}
    n = phase_number(phase)
    values = {f"GATE P{n} ENGINEERING PASS", f"GATE P{n} FAIL/BLOCKED"}
    if phase == "P10":
        values.add("GATE P10 FINAL RELEASE PASS")
    return values


def passes(verdict: str) -> bool:
    return verdict.endswith("ENGINEERING PASS") or verdict.endswith("READINESS: READY")


def lexical(path: Path) -> Path:
    return Path(os.path.abspath(os.fspath(path)))


class PinnedFile:
    __slots__ = ("path", "data", "size", "sha256", "identity")

    def __init__(self, path: Path, data: bytes, size: int, digest: str, identity_value: tuple[int, int]) -> None:
        self.path = path
        self.data = data
        self.size = size
        self.sha256 = digest
        self.identity = identity_value


_SECURE_READ_TEST_HOOK: Any = None


def _secure_read_hook(stage: str, path: Path) -> None:
    if _SECURE_READ_TEST_HOOK is not None:
        _SECURE_READ_TEST_HOOK(stage, path)


def is_reparse(path: Path) -> bool:
    info = path.lstat()
    if stat.S_ISLNK(info.st_mode):
        return True
    return bool(getattr(info, "st_file_attributes", 0) & getattr(stat, "FILE_ATTRIBUTE_REPARSE_POINT", 0x400))


def no_reparse(path: Path, label: str) -> None:
    cursor = Path(lexical(path).anchor)
    for part in lexical(path).parts[1:]:
        cursor /= part
        need(cursor.exists(), f"{label} path component is missing: {cursor}")
        need(not is_reparse(cursor), f"{label} traverses a reparse point: {cursor}")


class _WinFileTime(ctypes.Structure):
    _fields_ = [("low", wintypes.DWORD), ("high", wintypes.DWORD)]


class _WinFileInfo(ctypes.Structure):
    _fields_ = [
        ("attributes", wintypes.DWORD), ("created", _WinFileTime), ("accessed", _WinFileTime),
        ("written", _WinFileTime), ("volume", wintypes.DWORD), ("sizeHigh", wintypes.DWORD),
        ("sizeLow", wintypes.DWORD), ("links", wintypes.DWORD), ("indexHigh", wintypes.DWORD),
        ("indexLow", wintypes.DWORD),
    ]


class _WinUnicodeString(ctypes.Structure):
    _fields_ = [("length", wintypes.USHORT), ("maximumLength", wintypes.USHORT),
                ("buffer", wintypes.LPWSTR)]


class _WinObjectAttributes(ctypes.Structure):
    _fields_ = [
        ("length", wintypes.ULONG), ("rootDirectory", wintypes.HANDLE),
        ("objectName", ctypes.POINTER(_WinUnicodeString)), ("attributes", wintypes.ULONG),
        ("securityDescriptor", wintypes.LPVOID), ("securityQualityOfService", wintypes.LPVOID),
    ]


class _WinIoStatusValue(ctypes.Union):
    _fields_ = [("status", wintypes.LONG), ("pointer", wintypes.LPVOID)]


class _WinIoStatusBlock(ctypes.Structure):
    _anonymous_ = ("value",)
    _fields_ = [("value", _WinIoStatusValue), ("information", ctypes.c_size_t)]


class _WinApi:
    pass


def _win_api() -> Any:
    kernel32 = ctypes.WinDLL("kernel32", use_last_error=True)
    ntdll = ctypes.WinDLL("ntdll", use_last_error=True)
    kernel32.CreateFileW.argtypes = [wintypes.LPCWSTR, wintypes.DWORD, wintypes.DWORD,
                                     wintypes.LPVOID, wintypes.DWORD, wintypes.DWORD, wintypes.HANDLE]
    kernel32.CreateFileW.restype = wintypes.HANDLE
    kernel32.GetFileInformationByHandle.argtypes = [wintypes.HANDLE, ctypes.POINTER(_WinFileInfo)]
    kernel32.GetFileInformationByHandle.restype = wintypes.BOOL
    kernel32.GetFileType.argtypes = [wintypes.HANDLE]
    kernel32.GetFileType.restype = wintypes.DWORD
    kernel32.GetFinalPathNameByHandleW.argtypes = [wintypes.HANDLE, wintypes.LPWSTR, wintypes.DWORD, wintypes.DWORD]
    kernel32.GetFinalPathNameByHandleW.restype = wintypes.DWORD
    kernel32.ReadFile.argtypes = [wintypes.HANDLE, wintypes.LPVOID, wintypes.DWORD,
                                  ctypes.POINTER(wintypes.DWORD), wintypes.LPVOID]
    kernel32.ReadFile.restype = wintypes.BOOL
    kernel32.CloseHandle.argtypes = [wintypes.HANDLE]
    kernel32.CloseHandle.restype = wintypes.BOOL
    ntdll.NtCreateFile.argtypes = [ctypes.POINTER(wintypes.HANDLE), wintypes.DWORD,
                                   ctypes.POINTER(_WinObjectAttributes), ctypes.POINTER(_WinIoStatusBlock),
                                   wintypes.LPVOID, wintypes.ULONG, wintypes.ULONG, wintypes.ULONG,
                                   wintypes.ULONG, wintypes.LPVOID, wintypes.ULONG]
    ntdll.NtCreateFile.restype = wintypes.LONG
    ntdll.RtlNtStatusToDosError.argtypes = [wintypes.LONG]
    ntdll.RtlNtStatusToDosError.restype = wintypes.ULONG
    api = _WinApi()
    api._kernel32, api._ntdll = kernel32, ntdll
    for name in ("CreateFileW", "GetFileInformationByHandle", "GetFileType",
                 "GetFinalPathNameByHandleW", "ReadFile", "CloseHandle"):
        setattr(api, name, getattr(kernel32, name))
    api.NtCreateFile = ntdll.NtCreateFile
    api.RtlNtStatusToDosError = ntdll.RtlNtStatusToDosError
    return api


def _win_open_relative(api: Any, parent: Any, name: str, expect_directory: bool,
                       label: str) -> Any:
    encoded = name.encode("utf-16-le")
    buffer = ctypes.create_unicode_buffer(name)
    unicode_name = _WinUnicodeString(len(encoded), len(encoded) + 2,
                                     ctypes.cast(buffer, wintypes.LPWSTR))
    attributes = _WinObjectAttributes(
        ctypes.sizeof(_WinObjectAttributes), parent, ctypes.pointer(unicode_name),
        0x00000040 | 0x00001000, None, None,  # CASE_INSENSITIVE | DONT_REPARSE
    )
    io_status = _WinIoStatusBlock()
    handle = wintypes.HANDLE()
    access = 0x00100080 | (0 if expect_directory else 0x00000001)
    options = 0x00200000 | 0x00000020 | (0x00000001 if expect_directory else 0x00000040)
    status = api.NtCreateFile(ctypes.byref(handle), access, ctypes.byref(attributes),
                              ctypes.byref(io_status), None, 0x00000080, 0x00000001,
                              0x00000001, options, None, 0)
    if status < 0 or not handle.value:
        error = int(api.RtlNtStatusToDosError(status)) if status < 0 else 6
        raise LedgerError(f"cannot securely open {label}: Windows error {error}")
    return handle


def _win_identity(info: _WinFileInfo) -> tuple[int, int]:
    return int(info.volume), (int(info.indexHigh) << 32) | int(info.indexLow)


def _win_verify_chain_names(api: Any, handles: Sequence[Any], parts: Sequence[str],
                            infos: Sequence[_WinFileInfo], label: str) -> None:
    for index in range(1, len(handles)):
        expect_directory = bool(infos[index].attributes & 0x00000010)
        verify = _win_open_relative(api, handles[index - 1], parts[index], expect_directory,
                                    f"{label} retained-chain verification")
        try:
            current = _win_info(api, verify, f"{label} retained-chain verification")
            need(not (current.attributes & 0x00000400), f"{label} retained chain became a reparse point")
            need(_win_identity(current) == _win_identity(infos[index]),
                 f"{label} retained chain was replaced")
        finally:
            api.CloseHandle(verify)


def _win_info(api: Any, handle: Any, label: str) -> _WinFileInfo:
    value = _WinFileInfo()
    if not api.GetFileInformationByHandle(handle, ctypes.byref(value)):
        raise LedgerError(f"cannot inspect {label}: Windows error {ctypes.get_last_error()}")
    return value


def _win_final_path(api: Any, handle: Any, label: str) -> Path:
    required = api.GetFinalPathNameByHandleW(handle, None, 0, 0)
    if not required:
        raise LedgerError(f"cannot resolve opened {label}: Windows error {ctypes.get_last_error()}")
    buffer = ctypes.create_unicode_buffer(required + 1)
    written = api.GetFinalPathNameByHandleW(handle, buffer, len(buffer), 0)
    if not written or written >= len(buffer):
        raise LedgerError(f"cannot resolve opened {label}: Windows error {ctypes.get_last_error()}")
    value = buffer.value
    if value.startswith("\\\\?\\UNC\\"):
        value = "\\\\" + value[8:]
    elif value.startswith("\\\\?\\"):
        value = value[4:]
    return Path(os.path.normcase(os.path.abspath(value)))


def _win_open_chain(path: Path, label: str, final_directory: bool) -> tuple[Any, list[Any], _WinFileInfo, Path]:
    api = _win_api()
    absolute = lexical(path)
    parts = absolute.parts
    need(absolute.is_absolute() and parts, f"{label} is not absolute")
    handles: list[Any] = []
    infos: list[_WinFileInfo] = []
    cursor = Path(parts[0])
    components = [cursor]
    for part in parts[1:]:
        cursor /= part
        components.append(cursor)
    invalid = ctypes.c_void_p(-1).value
    try:
        for index, component in enumerate(components):
            last = index == len(components) - 1
            expect_directory = final_directory or not last
            _secure_read_hook("before-component-open", component)
            if index == 0:
                access = 0x00000080 if expect_directory else 0x80000000 | 0x00000080
                flags = 0x00200000 | 0x02000000  # OPEN_REPARSE_POINT | BACKUP_SEMANTICS
                handle = api.CreateFileW(str(component), access, 0x00000001, None, 3, flags, None)
                if handle == invalid:
                    raise LedgerError(f"cannot securely open {label} component {component}: Windows error {ctypes.get_last_error()}")
            else:
                handle = _win_open_relative(api, handles[-1], parts[index], expect_directory,
                                            f"{label} component {component}")
            handles.append(handle)
            _secure_read_hook("after-component-open", component)
            info = _win_info(api, handle, f"{label} component {component}")
            infos.append(info)
            need(api.GetFileType(handle) == 0x0001, f"{label} component is not a disk file: {component}")
            need(not (info.attributes & 0x00000400), f"{label} traverses a reparse point: {component}")
            is_directory = bool(info.attributes & 0x00000010)
            need(is_directory == expect_directory, f"{label} component has the wrong file type: {component}")
            _secure_read_hook("after-component-validation", component)
        _win_verify_chain_names(api, handles, parts, infos, label)
        return api, handles, info, _win_final_path(api, handles[-1], label)
    except BaseException:
        for handle in reversed(handles):
            api.CloseHandle(handle)
        raise


def _win_close(api: Any, handles: Sequence[Any]) -> None:
    for handle in reversed(handles):
        api.CloseHandle(handle)


def _win_read(path: Path, roots: Sequence[Path], label: str, reject_hardlinks: bool) -> PinnedFile:
    root_handles: list[tuple[Any, list[Any]]] = []
    api: Any = None
    handles: list[Any] = []
    try:
        api, handles, before, opened_path = _win_open_chain(path, label, False)
        for root in roots:
            root_api, opened, _, opened_root = _win_open_chain(root, f"{label} root", True)
            root_handles.append((root_api, opened))
            try:
                contained = os.path.commonpath([os.path.normcase(str(opened_path)), os.path.normcase(str(opened_root))]) == os.path.normcase(str(opened_root))
            except ValueError:
                contained = False
            if contained:
                break
        else:
            need(not roots, f"{label} escapes every evidence root")
        need(not reject_hardlinks or before.links == 1, f"{label} is a hard link")
        size = (int(before.sizeHigh) << 32) | int(before.sizeLow)
        identity_value = (int(before.volume), (int(before.indexHigh) << 32) | int(before.indexLow))
        _secure_read_hook("after-open", path)
        chunks: list[bytes] = []
        while True:
            buffer = ctypes.create_string_buffer(1024 * 1024)
            count = wintypes.DWORD()
            if not api.ReadFile(handles[-1], buffer, len(buffer), ctypes.byref(count), None):
                raise LedgerError(f"cannot read {label}: Windows error {ctypes.get_last_error()}")
            if count.value == 0:
                break
            chunks.append(buffer.raw[:count.value])
        _secure_read_hook("after-read", path)
        after = _win_info(api, handles[-1], label)
        after_identity = (int(after.volume), (int(after.indexHigh) << 32) | int(after.indexLow))
        after_size = (int(after.sizeHigh) << 32) | int(after.sizeLow)
        need(after_identity == identity_value and after_size == size and
             (after.written.high, after.written.low) == (before.written.high, before.written.low),
             f"{label} changed while read")
        need(_win_final_path(api, handles[-1], label) == opened_path, f"{label} moved while read")
        data = b"".join(chunks)
        need(len(data) == size, f"{label} size changed while read")
        return PinnedFile(lexical(path), data, size, sha(data), identity_value)
    finally:
        if api is not None:
            _win_close(api, handles)
        for root_api, opened in reversed(root_handles):
            _win_close(root_api, opened)


def _posix_open_directory(path: Path, label: str) -> tuple[list[int], int]:
    need(os.open in os.supports_dir_fd and hasattr(os, "O_NOFOLLOW") and hasattr(os, "O_DIRECTORY"),
         f"{label} cannot be secured: openat/no-follow directory traversal is unavailable")
    flags = os.O_RDONLY | getattr(os, "O_CLOEXEC", 0) | getattr(os, "O_DIRECTORY", 0) | getattr(os, "O_NOFOLLOW", 0)
    absolute = lexical(path)
    fds = [os.open(absolute.anchor, flags)]
    try:
        for part in absolute.parts[1:]:
            _secure_read_hook("before-component-open", absolute)
            fd = os.open(part, flags, dir_fd=fds[-1])
            fds.append(fd)
            _secure_read_hook("after-component-open", absolute)
            need(stat.S_ISDIR(os.fstat(fd).st_mode), f"{label} component is not a directory: {part}")
        return fds, fds[-1]
    except BaseException:
        for fd in reversed(fds):
            os.close(fd)
        raise


def _posix_read(path: Path, roots: Sequence[Path], label: str, reject_hardlinks: bool) -> PinnedFile:
    need(hasattr(os, "O_NOFOLLOW") and hasattr(os, "O_DIRECTORY"),
         f"{label} cannot be secured: no-follow directory traversal is unavailable")
    absolute = lexical(path)
    candidates: list[tuple[Path, Path]] = []
    for root in roots:
        root_abs = lexical(root)
        try:
            relative = absolute.relative_to(root_abs)
        except ValueError:
            continue
        candidates.append((root_abs, relative))
    need(not roots or candidates, f"{label} escapes every evidence root")
    root_path, relative = candidates[0] if candidates else (Path(absolute.anchor), Path(*absolute.parts[1:]))
    fds, parent = _posix_open_directory(root_path, f"{label} root")
    try:
        parts = relative.parts
        need(parts, f"{label} is not a regular file")
        dir_flags = os.O_RDONLY | getattr(os, "O_CLOEXEC", 0) | getattr(os, "O_DIRECTORY", 0) | getattr(os, "O_NOFOLLOW", 0)
        for part in parts[:-1]:
            fd = os.open(part, dir_flags, dir_fd=parent)
            fds.append(fd)
            parent = fd
        file_flags = os.O_RDONLY | getattr(os, "O_CLOEXEC", 0) | getattr(os, "O_NOFOLLOW", 0)
        _secure_read_hook("before-open", absolute)
        fd = os.open(parts[-1], file_flags, dir_fd=parent)
        fds.append(fd)
        before = os.fstat(fd)
        need(stat.S_ISREG(before.st_mode), f"{label} is not regular")
        need(not reject_hardlinks or before.st_nlink == 1, f"{label} is a hard link")
        _secure_read_hook("after-open", absolute)
        chunks: list[bytes] = []
        while chunk := os.read(fd, 1024 * 1024):
            chunks.append(chunk)
        _secure_read_hook("after-read", absolute)
        after = os.fstat(fd)
        need((before.st_dev, before.st_ino, before.st_size, before.st_mtime_ns) ==
             (after.st_dev, after.st_ino, after.st_size, after.st_mtime_ns), f"{label} changed while read")
        verify = os.open(parts[-1], file_flags, dir_fd=parent)
        try:
            current = os.fstat(verify)
            need((current.st_dev, current.st_ino) == (before.st_dev, before.st_ino), f"{label} was replaced while read")
        finally:
            os.close(verify)
        data = b"".join(chunks)
        need(len(data) == before.st_size, f"{label} size changed while read")
        return PinnedFile(absolute, data, before.st_size, sha(data), (before.st_dev, before.st_ino))
    finally:
        for fd in reversed(fds):
            os.close(fd)


def pinned_file(path: Path, label: str, reject_hardlinks: bool = True,
                roots: Sequence[Path] = ()) -> PinnedFile:
    absolute = lexical(path)
    need(absolute.is_absolute() and ".." not in path.parts, f"{label} path is unsafe")
    try:
        return _win_read(absolute, roots, label, reject_hardlinks) if os.name == "nt" else _posix_read(absolute, roots, label, reject_hardlinks)
    except OSError as exc:
        raise LedgerError(f"cannot securely read {label}: {exc}") from exc


def directory(path: Path, label: str) -> Path:
    need(".." not in path.parts, f"{label} contains traversal")
    result = lexical(path)
    if os.name == "nt":
        api, handles, _, _ = _win_open_chain(result, label, True)
        _win_close(api, handles)
    else:
        fds, _ = _posix_open_directory(result, label)
        for fd in reversed(fds):
            os.close(fd)
    return result


def within(path: Path, root: Path) -> bool:
    try:
        # Callers reject every lexical reparse component before containment is
        # checked.  Resolve here only to normalize equivalent Windows names
        # (notably 8.3 aliases versus expanded paths) for the identity test.
        canonical_path = path.resolve(strict=True)
        canonical_root = root.resolve(strict=True)
        return os.path.commonpath([os.path.normcase(canonical_path), os.path.normcase(canonical_root)]) == os.path.normcase(canonical_root)
    except (OSError, ValueError):
        return False


def safe_pinned(value: str | Path, roots: Sequence[Path], label: str,
                reject_hardlinks: bool = True) -> PinnedFile:
    original = Path(value)
    need(original.is_absolute(), f"{label} must be an absolute lexical path")
    need(".." not in original.parts, f"{label} contains traversal")
    result = lexical(original)
    return pinned_file(result, label, reject_hardlinks, roots)


def safe_file(value: str | Path, roots: Sequence[Path], label: str) -> Path:
    return safe_pinned(value, roots, label).path


def pinned(path: Path, label: str, reject_hardlinks: bool = True) -> bytes:
    return pinned_file(path, label, reject_hardlinks).data


def identity(path: Path, relative: Path | None = None, name: str | None = None) -> dict[str, Any]:
    data = pinned(path, f"input {path}", False)
    record: dict[str, Any] = {"path": path.relative_to(relative).as_posix() if relative else str(path), "bytes": len(data), "sha256": sha(data)}
    return {"name": name, **record} if name else record


def protocol(repo: Path) -> tuple[dict[str, Any], dict[str, Any]]:
    authority_data = pinned(repo / AUTHORITY_REL, "authority", False)
    vector_data = pinned(repo / VECTORS_REL, "protocol vectors", False)
    need(sha(authority_data) == AUTHORITY_SHA256 and sha(vector_data) == VECTORS_SHA256, "immutable authority or vector digest changed")
    authority, vectors = json_bytes(authority_data, "authority"), json_bytes(vector_data, "vectors")
    need(document(authority) == authority_data and document(vectors) == vector_data, "authority inputs are not canonical JSON")
    need(authority.get("runId") == RUN_ID and authority.get("schemaVersion") == 2, "authority run/version is wrong")
    need(isinstance(authority.get("runtimeId"), str) and isinstance(authority.get("runGeneration"), int), "authority runtime generation is missing")
    need({row["phase"]: row["taskId"] for row in authority.get("materializedGateTasks", [])} == GATE_TASKS, "materialized Gate authority changed")
    need({row.get("taskId") for row in authority.get("bootstrapEvents", [])} == BOOTSTRAP_TASKS, "bootstrap authority is incomplete")
    return authority, vectors


def derive_policy(repo: Path) -> tuple[list[dict[str, Any]], dict[str, Any]]:
    spec_path, manifest_path, license_path = repo / SPEC_REL, repo / MANIFEST_REL, repo / LICENSE_REL
    spec = pinned(spec_path, "locked specification", False).decode("utf-8")
    manifest = load(manifest_path)
    license_text = pinned(license_path, "license provenance", False).decode("utf-8")
    gate = manifest.get("licensePolicy", {}).get("releaseGate", {})
    need(gate.get("status") == "blocked_pending_owner_root_license_selection" and gate.get("passClaimAllowed") is False, "release policy is no longer blocked")
    need("F42" in gate.get("blockedFeatureIds", []) and "R24" in gate.get("blockedRequirementIds", []), "F42/R24 release blocks changed")
    features = {row.get("id"): row for row in manifest.get("features", []) if isinstance(row, dict)}
    need(features.get("F42", {}).get("mgifEvidence", {}).get("status") == "blocked", "F42 is no longer blocked")
    need("Metal and Android were not run" in features.get("F40", {}).get("mgifEvidence", {}).get("notes", ""), "Metal status changed")
    need("F42 与 R24 必须保持 `BLOCKED`" in spec and "没有 macOS artifact 不得声称 Metal 已验证" in spec and "任何 release/distribution gate 都不得声称 PASS" in spec, "locked policy text changed")
    need("repository root remains unlicensed and rights-reserved" in license_text, "license provenance changed")
    need(not any((repo / n).exists() for n in ("LICENSE", "LICENSE.txt", "LICENSE.md", "COPYING", "COPYING.txt", "NOTICE", "NOTICE.txt")), "root license candidate requires re-audit")
    inputs = [identity(spec_path, repo, "specification"), identity(manifest_path, repo, "manifest"), identity(license_path, repo, "licenseSources")]
    policy = {**FIXED_POLICY, "basis": {"manifestReleaseGateStatus": gate["status"], "manifestPassClaimAllowed": False, "rootLicenseSelected": False, "metalArtifactAccepted": False, "finalAcceptanceComplete": False}}
    return inputs, policy


def parse_orca(stdout: str, label: str) -> dict[str, Any]:
    start = stdout.find("{")
    need(start >= 0, f"{label} returned no JSON")
    try:
        value = json.loads(stdout[start:])
    except json.JSONDecodeError as exc:
        raise LedgerError(f"{label} returned invalid JSON") from exc
    need(isinstance(value, dict) and value.get("ok") is True, f"{label} failed")
    return value


def orca(command: list[str], label: str) -> dict[str, Any]:
    environment = os.environ.copy()
    environment.update({"PYTHONIOENCODING": "utf-8", "PYTHONUTF8": "1"})
    run = subprocess.run(command, text=True, encoding="utf-8", errors="strict", env=environment,
                         capture_output=True, check=False)
    need(run.returncode == 0, f"{label} failed: {(run.stderr or run.stdout).strip()}")
    return parse_orca(run.stdout or "", label)


def normalize_recovery_settlement(value: dict[str, Any], task_id: str, task_completed: Any,
                                  result_payload: bytes) -> dict[str, Any]:
    settlement = closed(value, RECOVERY_SETTLEMENT_KEYS, f"task {task_id} recovery settlement")
    need(task_id == RECOVERY_SETTLEMENT_TASK_ID, f"task {task_id} is not the authorized recovery settlement task")
    need(settlement["provenance"] == RECOVERY_SETTLEMENT_PROVENANCE and
         settlement["objectiveCompleted"] is True and settlement["acceptanceVerdict"] == "FAIL" and
         settlement["workerOutcome"] == "failed", f"task {task_id} recovery settlement verdict is invalid")
    need(settlement["originalMessageId"] == RECOVERY_SETTLEMENT_MESSAGE_ID and
         settlement["originalDispatchId"] == RECOVERY_SETTLEMENT_DISPATCH_ID,
         f"task {task_id} recovery settlement origin is invalid")
    need(settlement["blockingFindings"] == RECOVERY_SETTLEMENT_FINDINGS,
         f"task {task_id} recovery settlement findings differ")
    need(settlement["fixedPolicy"] == RECOVERY_SETTLEMENT_POLICY,
         f"task {task_id} recovery settlement fixed policy differs")
    need(settlement["note"] == RECOVERY_SETTLEMENT_NOTE,
         f"task {task_id} recovery settlement note differs")
    for key in RECOVERY_SETTLEMENT_HASH_KEYS:
        need(isinstance(settlement[key], str) and re.fullmatch(r"[0-9A-Fa-f]{64}", settlement[key]) is not None,
             f"task {task_id} recovery settlement {key} is invalid")
    report = settlement["reportPath"]
    need(isinstance(report, str) and report == report.strip() and "\x00" not in report,
         f"task {task_id} recovery settlement report path is invalid")
    report_path = PureWindowsPath(report)
    try:
        relative_report = report_path.relative_to(PureWindowsPath("C:/Temp"))
    except ValueError as exc:
        raise LedgerError(f"task {task_id} recovery settlement report is outside C:\\Temp") from exc
    need(report_path.is_absolute() and relative_report.parts and ".." not in relative_report.parts and
         report_path.suffix.lower() == ".md" and task_id in report,
         f"task {task_id} recovery settlement report path is invalid")
    settled_at = utc(task_completed, f"task {task_id} recovery settlement completion")
    normalized = {
        **settlement, "taskId": task_id, "settledAtUtc": settled_at,
        **{key: settlement[key].lower() for key in RECOVERY_SETTLEMENT_HASH_KEYS},
    }
    return {
        "messageId": settlement["originalMessageId"], "outcome": "failed",
        "completedAtUtc": settled_at, "reportPath": report,
        "reportedBy": "coordinator-review-settlement",
        "subject": "coordinator-review-settlement:FAIL", "body": settlement["note"],
        "resultPayload": result_payload.decode("ascii"), "resultSha256": sha(result_payload),
        "recoverySettlement": normalized,
    }


def normalize_result(raw: str | None, task_id: str, task_completed: Any) -> dict[str, Any] | None:
    if raw in (None, "", "null"):
        return None
    try:
        value = json.loads(raw)
    except json.JSONDecodeError as exc:
        raise LedgerError(f"task {task_id} result is not JSON") from exc
    need(isinstance(value, dict), f"task {task_id} result is not an object")
    result_payload = payload(value)
    if value.get("provenance") == RECOVERY_SETTLEMENT_PROVENANCE:
        return normalize_recovery_settlement(value, task_id, task_completed, result_payload)
    if value.get("provenance") != "worker_report":
        gate_id = value.get("gateId")
        need(isinstance(gate_id, str) and gate_id and isinstance(value.get("decision"), str), f"task {task_id} non-worker result is not a decision-Gate result")
        return {
            "messageId": f"decision:{gate_id}", "outcome": "succeeded",
            "completedAtUtc": utc(task_completed, f"task {task_id} decision completion"),
            "reportPath": None, "reportedBy": "decision-gate",
            "subject": f"decision-gate:{gate_id}", "body": payload(value).decode("ascii"),
            "resultPayload": result_payload.decode("ascii"), "resultSha256": sha(result_payload),
        }
    need(isinstance(value, dict) and isinstance(value.get("subject"), str) and isinstance(value.get("body"), str), f"task {task_id} result is incomplete")
    return {
        "messageId": value.get("messageId"), "outcome": value.get("outcome"),
        "completedAtUtc": utc(value.get("completedAt"), f"task {task_id} result completion"),
        "reportPath": value.get("reportPath"), "reportedBy": value.get("reportedBy"),
        "subject": value["subject"], "body": value["body"],
        "resultPayload": result_payload.decode("ascii"), "resultSha256": sha(result_payload),
    }


def normalize_task(raw: dict[str, Any], dispatch: dict[str, Any] | None) -> dict[str, Any]:
    task_id = raw["id"]
    try:
        deps = sorted(json.loads(raw.get("deps") or "[]"))
    except json.JSONDecodeError as exc:
        raise LedgerError(f"task {task_id} dependencies are invalid") from exc
    normalized_dispatch = None
    if dispatch is not None:
        normalized_dispatch = {
            "dispatchId": dispatch.get("id"), "status": dispatch.get("status"),
            "dispatchedAtUtc": utc(dispatch.get("dispatched_at") or dispatch.get("created_at"), f"task {task_id} dispatch"),
            "completedAtUtc": utc(dispatch.get("completed_at"), f"task {task_id} dispatch completion", True),
        }
    worker = normalize_result(raw.get("result"), task_id, raw.get("completed_at"))
    task = {
        "taskId": task_id, "displayName": raw.get("display_name") or raw.get("task_title"), "status": raw.get("status"),
        "dependencies": deps, "createdAtUtc": utc(raw.get("created_at"), f"task {task_id} creation"),
        "completedAtUtc": utc(raw.get("completed_at"), f"task {task_id} completion", True),
        "dispatch": normalized_dispatch, "workerResult": worker,
    }
    if worker is not None and "recoverySettlement" in worker:
        task["completionProvenance"] = RECOVERY_SETTLEMENT_PROVENANCE
    return task


def lifecycle(task: dict[str, Any]) -> None:
    task_id, state = task["taskId"], task["status"]
    dispatch, worker, completed = task["dispatch"], task["workerResult"], task["completedAtUtc"]
    settlement = worker.get("recoverySettlement") if worker is not None else None
    provenance = task.get("completionProvenance")
    need((settlement is not None) == (provenance == RECOVERY_SETTLEMENT_PROVENANCE),
         f"task {task_id} recovery settlement marker is inconsistent")
    if state in {"pending", "ready", "blocked"}:
        need(completed is None and worker is None, f"task {task_id} nonterminal lifecycle contradicts result")
        need(dispatch is None or (dispatch["status"] == state and dispatch["completedAtUtc"] is None) or (dispatch["status"] == "stopped" and dispatch["completedAtUtc"] is not None), f"task {task_id} dispatch contradicts status")
    elif state == "dispatched":
        need(completed is None and worker is None and dispatch is not None and dispatch["status"] == "dispatched" and dispatch["completedAtUtc"] is None, f"task {task_id} dispatched lifecycle is invalid")
    else:
        if settlement is not None:
            need(state == "completed" and completed is not None and dispatch is not None and
                 dispatch["status"] == "failed" and dispatch["completedAtUtc"] is not None and
                 dispatch["dispatchId"] == settlement["originalDispatchId"] and
                 worker["messageId"] == settlement["originalMessageId"] and worker["outcome"] == "failed" and
                 worker["reportPath"] == settlement["reportPath"] and
                 settlement["taskId"] == task_id and settlement["settledAtUtc"] == completed,
                 f"task {task_id} recovery settlement lifecycle is invalid")
            need(abs(epoch(worker["completedAtUtc"]) - epoch(completed)) < 1.1 and
                 epoch(completed) >= epoch(dispatch["completedAtUtc"]),
                 f"task {task_id} recovery settlement timestamps disagree")
        else:
            decision_completion = state == "completed" and completed is not None and dispatch is None and worker is not None and worker["reportedBy"] == "decision-gate"
            need(decision_completion or (state in {"completed", "failed"} and completed is not None and dispatch is not None and worker is not None), f"terminal task {task_id} lacks evidence")
            outcome = "succeeded" if state == "completed" else "failed"
            need(worker["outcome"] == outcome, f"terminal task {task_id} worker outcome contradicts status")
            if not decision_completion:
                need(dispatch["status"] == state and dispatch["completedAtUtc"] is not None, f"terminal task {task_id} dispatch contradicts status")
                need(abs(epoch(dispatch["completedAtUtc"]) - epoch(completed)) < 1.1, f"task {task_id} dispatch timestamp disagrees")
            need(abs(epoch(worker["completedAtUtc"]) - epoch(completed)) < 1.1, f"task {task_id} result timestamp disagrees")
    if dispatch:
        need(epoch(dispatch["dispatchedAtUtc"]) >= epoch(task["createdAtUtc"]), f"task {task_id} dispatch predates creation")
        if dispatch["completedAtUtc"]:
            need(epoch(dispatch["completedAtUtc"]) >= epoch(dispatch["dispatchedAtUtc"]), f"task {task_id} dispatch completion predates dispatch")
    if completed:
        need(epoch(completed) >= epoch(task["createdAtUtc"]), f"task {task_id} completion predates creation")


def revision_payload(snapshot: dict[str, Any]) -> dict[str, Any]:
    return {key: snapshot[key] for key in ("runId", "runtimeId", "runGeneration", "runTaskIndex", "tasks", "decisionGates")}


def _keyed(rows: Sequence[dict[str, Any]], key: str, label: str) -> dict[str, dict[str, Any]]:
    result: dict[str, dict[str, Any]] = {}
    for row in rows:
        identity_value = row.get(key)
        need(isinstance(identity_value, str) and identity_value and identity_value not in result,
             f"{label} identities are invalid")
        result[identity_value] = {name: value for name, value in row.items() if name != key}
    return result


_RAW_SCHEMA_VALIDATOR: Any = None


def raw_schema_validate(value: Any, label: str) -> None:
    global _RAW_SCHEMA_VALIDATOR
    if _RAW_SCHEMA_VALIDATOR is None:
        schema_path = Path(__file__).resolve().parents[1] / SCHEMA_REL
        schema = json_bytes(pinned(schema_path, "progress-ledger schema", False), "progress-ledger schema")
        try:
            _RAW_SCHEMA_VALIDATOR = JsonSchemaValidator(schema)
        except JsonSchemaError as exc:
            raise LedgerError(f"progress-ledger schema is invalid at {exc.path}: {exc.message}") from exc
    try:
        _RAW_SCHEMA_VALIDATOR.validate(value)
    except JsonSchemaError as exc:
        raise LedgerError(
            f"{label} fails raw Draft 2020-12 validation at {exc.path}: {exc.message}") from exc


def encode_snapshot(snapshot: dict[str, Any]) -> dict[str, Any]:
    if snapshot.get("schemaVersion") == 4:
        raw_schema_validate(snapshot, "v4 snapshot")
        return snapshot
    if snapshot.get("schemaVersion") == 3:
        snapshot = decode_snapshot(snapshot)
    need(snapshot.get("schemaVersion") == 2, "snapshot wire version is unsupported")
    tasks: dict[str, dict[str, Any]] = {}
    for row in snapshot["tasks"]:
        task_id = row["taskId"]
        need(task_id not in tasks, f"duplicate snapshot task identity {task_id}")
        tasks[task_id] = {key: value for key, value in row.items() if key != "taskId"}
    index_by_id = {row["taskId"]: row for row in snapshot["runTaskIndex"]}
    need(len(index_by_id) == len(snapshot["runTaskIndex"]) and set(tasks).issubset(index_by_id),
         "snapshot index/task identities differ")
    for task_id, task in tasks.items():
        index = index_by_id.get(task_id)
        need(index is not None and all(index[key] == task[key] for key in ("displayName", "status", "createdAtUtc")),
             f"snapshot index differs from task {task_id}")
    run_tasks = {
        task_id: tasks.get(task_id, {key: index[key] for key in ("displayName", "status", "createdAtUtc")})
        for task_id, index in index_by_id.items()
    }
    decision_gates = {}
    for gate in snapshot["decisionGates"]:
        gate_id = gate["gateId"]
        need(gate_id not in decision_gates, "duplicate snapshot decision Gate identity")
        decision_gates[gate_id] = {key: value for key, value in gate.items() if key != "gateId"}
    wire = {
        **{key: value for key, value in snapshot.items() if key not in {"schemaVersion", "runTaskIndex", "tasks", "decisionGates"}},
        "schemaVersion": 4,
        "runTasks": run_tasks, "decisionGates": decision_gates,
    }
    raw_schema_validate(wire, "v4 snapshot")
    return wire


def decode_snapshot(value: Any) -> dict[str, Any]:
    need(isinstance(value, dict), "snapshot must be an object")
    if value.get("schemaVersion") == 2:
        return value
    if value.get("schemaVersion") == 4:
        raw_schema_validate(value, "v4 snapshot")
        keys = {"documentKind", "schemaVersion", "runId", "runtimeId", "runGeneration", "capturedAtUtc",
                "consistencyMode", "revisionSha256", "runTasks", "decisionGates"}
        wire = closed(value, keys, "v4 snapshot")
        need(all(isinstance(wire[key], dict) for key in ("runTasks", "decisionGates")),
             "v4 snapshot identity maps are invalid")
        index = [{"taskId": task_id, **{key: row[key] for key in ("displayName", "status", "createdAtUtc")}}
                 for task_id, row in wire["runTasks"].items()]
        index.sort(key=lambda row: (row.get("createdAtUtc", ""), row["taskId"]))
        tasks = [{"taskId": task_id, **row} for task_id, row in wire["runTasks"].items()
                 if "dependencies" in row]
        tasks.sort(key=lambda row: (row.get("createdAtUtc", ""), row["taskId"]))
        gates = [{"gateId": gate_id, **row} for gate_id, row in wire["decisionGates"].items()]
        gates.sort(key=lambda row: (row.get("createdAtUtc", ""), row["gateId"]))
        return {**{key: item for key, item in wire.items() if key not in {"schemaVersion", "runTasks", "decisionGates"}},
                "schemaVersion": 2, "runTaskIndex": index, "tasks": tasks, "decisionGates": gates}
    keys = {"documentKind", "schemaVersion", "runId", "runtimeId", "runGeneration", "capturedAtUtc",
            "consistencyMode", "revisionSha256", "runTaskOrder", "runTaskIndex", "taskOrder", "tasks",
            "workerResults", "workerMessages", "decisionGateOrder", "decisionGates", "decisionGateTasks"}
    wire = closed(value, keys, "v3 snapshot")
    need(wire["schemaVersion"] == 3 and all(isinstance(wire[key], dict) for key in ("runTaskIndex", "tasks", "workerResults", "decisionGates")),
         "v3 snapshot identity maps are invalid")
    need(isinstance(wire["workerMessages"], dict) and isinstance(wire["decisionGateTasks"], dict), "v3 snapshot secondary identity maps are invalid")
    message_by_task: dict[str, str] = {}
    for message_id, task_id in wire["workerMessages"].items():
        need(isinstance(message_id, str) and isinstance(task_id, str) and task_id not in message_by_task,
             f"duplicate worker message association for task {task_id}")
        message_by_task[task_id] = message_id
    need(set(message_by_task) == set(wire["workerResults"]), "snapshot worker result/message identities differ")
    worker_by_task = {task_id: {"messageId": message_by_task[task_id], **row} for task_id, row in wire["workerResults"].items()}
    for order_name, collection_name in (("runTaskOrder", "runTaskIndex"), ("taskOrder", "tasks"), ("decisionGateOrder", "decisionGates")):
        need(isinstance(wire[order_name], list) and len(wire[order_name]) == len(set(wire[order_name])) and
             set(wire[order_name]) == set(wire[collection_name]), f"v3 snapshot {order_name} differs from {collection_name}")
    index = [{"taskId": task_id, **wire["runTaskIndex"][task_id]} for task_id in wire["runTaskOrder"]]
    tasks = [{"taskId": task_id, **wire["tasks"][task_id], "workerResult": worker_by_task.get(task_id)}
             for task_id in wire["taskOrder"]]
    need(set(worker_by_task).issubset(wire["tasks"]), "snapshot worker result names an unknown task")
    gate_task_by_id = wire["decisionGateTasks"]
    need(set(gate_task_by_id) == set(wire["decisionGates"]) and
         all(isinstance(task_id, str) and task_id for task_id in gate_task_by_id.values()),
         "snapshot decision Gate task identities differ")
    gates = [{"gateId": gate_id, "taskId": gate_task_by_id[gate_id], **wire["decisionGates"][gate_id]} for gate_id in wire["decisionGateOrder"]]
    return {**{key: value for key, value in wire.items() if key not in {"schemaVersion", "runTaskOrder", "runTaskIndex", "taskOrder", "tasks", "workerResults", "workerMessages", "decisionGateOrder", "decisionGates", "decisionGateTasks"}},
            "schemaVersion": 2, "runTaskIndex": index, "tasks": tasks, "decisionGates": gates}


def encode_gate_history(events: Sequence[dict[str, Any]]) -> dict[str, Any]:
    result: dict[str, Any] = {name: {} for name in ("events", "messages", "dispatches", "receipts", "reports", "verdicts", "orcaResults", "resultPayloads", "supersessions", "supersessionTargets")}
    result["order"] = []
    for event in events:
        event_id = event["eventId"]
        need(event_id not in result["events"], f"duplicate Gate event {event_id}")
        result["order"].append(event_id)
        result["events"][event_id] = {key: value for key, value in event.items()
                                      if key not in {"eventId", "dispatchId", "workerMessageId", "receipt", "report", "verdictArtifact", "orcaResult", "supersedesEventId"}}
        dispatch_id = event["dispatchId"]
        need(dispatch_id not in result["dispatches"], f"duplicate Gate dispatch {dispatch_id}")
        result["dispatches"][dispatch_id] = event_id
        message_id = event["workerMessageId"]
        need(message_id not in result["messages"], f"duplicate Gate worker message {message_id}")
        result["messages"][message_id] = event_id
        for source, target in (("receipt", "receipts"), ("report", "reports"), ("verdictArtifact", "verdicts")):
            artifact = event[source]
            artifact_id = artifact["sha256"]
            need(artifact_id not in result[target], f"duplicate Gate {source} identity {artifact_id}")
            result[target][artifact_id] = {"eventId": event_id, "path": artifact["path"], "bytes": artifact["bytes"]}
        orca = event["orcaResult"]
        artifact = orca["artifact"]
        artifact_id = artifact["sha256"]
        need(artifact_id not in result["orcaResults"], f"duplicate Gate Orca result identity {artifact_id}")
        result_id = orca["resultSha256"]
        need(result_id not in result["resultPayloads"], f"duplicate Gate result payload {result_id}")
        result["resultPayloads"][result_id] = event_id
        result["orcaResults"][artifact_id] = {"eventId": event_id, "path": artifact["path"], "bytes": artifact["bytes"],
                                              **{key: value for key, value in orca.items() if key not in {"artifact", "resultSha256"}}}
        previous = event["supersedesEventId"]
        if previous is not None:
            need(previous not in result["supersessions"], f"duplicate Gate supersession identity {previous}")
            result["supersessions"][previous] = event_id
            need(event_id not in result["supersessionTargets"], f"duplicate Gate supersession target {event_id}")
            result["supersessionTargets"][event_id] = previous
    return result


def decode_gate_history(value: Any) -> list[dict[str, Any]]:
    history = closed(value, {"order", "events", "messages", "dispatches", "receipts", "reports", "verdicts", "orcaResults", "resultPayloads", "supersessions", "supersessionTargets"}, "v3 Gate history")
    need(isinstance(history["order"], list) and len(history["order"]) == len(set(history["order"])) and
         all(isinstance(history[key], dict) for key in ("events", "messages", "dispatches", "receipts", "reports", "verdicts", "orcaResults", "resultPayloads", "supersessions", "supersessionTargets")),
         "v3 Gate history identity maps are invalid")

    def associated(collection: str, event_id: str) -> tuple[str, dict[str, Any]]:
        matches = [(identity_value, row) for identity_value, row in history[collection].items()
                   if isinstance(row, dict) and row.get("eventId") == event_id]
        need(len(matches) == 1, f"Gate event {event_id} has invalid {collection} association")
        return matches[0]

    events: list[dict[str, Any]] = []
    need(set(history["order"]) == set(history["events"]), "Gate history order and event identities differ")
    for event_id in history["order"]:
        messages = [message_id for message_id, target in history["messages"].items() if target == event_id]
        dispatches = [dispatch_id for dispatch_id, target in history["dispatches"].items() if target == event_id]
        predecessors = [previous for previous, target in history["supersessions"].items() if target == event_id]
        need(len(messages) == 1 and len(dispatches) == 1 and len(predecessors) <= 1,
             f"Gate event {event_id} message/dispatch/supersession association is invalid")
        need((not predecessors and event_id not in history["supersessionTargets"]) or
             (len(predecessors) == 1 and history["supersessionTargets"].get(event_id) == predecessors[0]),
             f"Gate event {event_id} supersession indexes disagree")
        receipt_id, receipt = associated("receipts", event_id)
        report_id, report = associated("reports", event_id)
        verdict_id, verdict = associated("verdicts", event_id)
        result_id, result = associated("orcaResults", event_id)
        result_payloads = [result_id for result_id, target in history["resultPayloads"].items() if target == event_id]
        need(len(result_payloads) == 1, f"Gate event {event_id} result payload association is invalid")
        event = {"eventId": event_id, **history["events"][event_id], "dispatchId": dispatches[0], "workerMessageId": messages[0],
                 "supersedesEventId": predecessors[0] if predecessors else None,
                 "receipt": {"path": receipt["path"], "bytes": receipt["bytes"], "sha256": receipt_id},
                 "report": {"path": report["path"], "bytes": report["bytes"], "sha256": report_id},
                 "verdictArtifact": {"path": verdict["path"], "bytes": verdict["bytes"], "sha256": verdict_id},
                 "orcaResult": {"artifact": {"path": result["path"], "bytes": result["bytes"], "sha256": result_id},
                                "resultSha256": result_payloads[0],
                                **{key: item for key, item in result.items() if key not in {"eventId", "path", "bytes"}}}}
        events.append(event)
    return events


def encode_decision_history(transitions: Sequence[dict[str, Any]]) -> dict[str, Any]:
    rows, predecessors, successors, order = {}, {}, {}, []
    for transition in transitions:
        transition_id = transition["transitionId"]
        need(transition_id not in rows, f"duplicate decision transition {transition_id}")
        order.append(transition_id)
        rows[transition_id] = {key: value for key, value in transition.items() if key not in {"transitionId", "previousTransitionId"}}
        previous = transition["previousTransitionId"]
        if previous is not None:
            need(previous not in predecessors, f"duplicate decision predecessor {previous}")
            predecessors[previous] = transition_id
            need(transition_id not in successors, f"duplicate decision successor {transition_id}")
            successors[transition_id] = previous
    return {"order": order, "transitions": rows, "predecessors": predecessors, "successors": successors}


def decode_decision_history(value: Any) -> list[dict[str, Any]]:
    history = closed(value, {"order", "transitions", "predecessors", "successors"}, "v3 decision history")
    need(isinstance(history["order"], list) and isinstance(history["transitions"], dict) and isinstance(history["predecessors"], dict) and
         len(history["order"]) == len(set(history["order"])) and set(history["order"]) == set(history["transitions"]),
         "v3 decision history identities are invalid")
    result = []
    need(isinstance(history["successors"], dict), "v3 decision successor identities are invalid")
    for transition_id in history["order"]:
        previous = [identity_value for identity_value, target in history["predecessors"].items() if target == transition_id]
        need(len(previous) <= 1 and ((not previous and transition_id not in history["successors"]) or
             (len(previous) == 1 and history["successors"].get(transition_id) == previous[0])),
             f"decision transition {transition_id} predecessor indexes disagree")
        result.append({"transitionId": transition_id, **history["transitions"][transition_id],
                       "previousTransitionId": previous[0] if previous else None})
    return result


GATE_TABLE_COLUMNS = (
    "rows", "dispatchIds", "messageIds", "receiptSha256s", "receiptMetadata",
    "reportSha256s", "reportMetadata", "verdictSha256s", "verdictMetadata",
    "orcaArtifactSha256s", "orcaResultMetadata", "resultSha256s",
)
GATE_ROW_KEYS = {
    "authorityClass", "inputFingerprintSha256", "phase", "gateTaskId",
    "completedAtUtc", "gateVerdict", "gateScope",
}
DECISION_ROW_KEYS = {
    "gateId", "taskId", "question", "options", "createdAtUtc", "status",
    "outcome", "resolvedAtUtc", "resolution", "observedAtUtc",
}


def _artifact_metadata(value: dict[str, Any]) -> dict[str, Any]:
    return {"path": value["path"], "bytes": value["bytes"]}


def encode_gate_table(events: Sequence[dict[str, Any]]) -> dict[str, Any]:
    need(len(events) <= MAX_LEDGER_ROWS,
         f"Gate history exceeds the v4 wire limit of {MAX_LEDGER_ROWS} rows")
    table: dict[str, list[Any]] = {name: [] for name in GATE_TABLE_COLUMNS}
    previous_by_phase: dict[str, str] = {}
    for event in events:
        phase = event["phase"]
        need(event["eventId"] == event["receipt"]["sha256"],
             f"{phase} Gate receipt identity differs")
        need(event["supersedesEventId"] == previous_by_phase.get(phase),
             f"{phase} Gate history is not a single forward chain")
        previous_by_phase[phase] = event["eventId"]
        table["rows"].append({key: event[key] for key in GATE_ROW_KEYS})
        table["dispatchIds"].append(event["dispatchId"])
        table["messageIds"].append(event["workerMessageId"])
        for source, hash_column, metadata_column in (
            ("receipt", "receiptSha256s", "receiptMetadata"),
            ("report", "reportSha256s", "reportMetadata"),
            ("verdictArtifact", "verdictSha256s", "verdictMetadata"),
        ):
            artifact = event[source]
            table[hash_column].append(artifact["sha256"])
            table[metadata_column].append(_artifact_metadata(artifact))
        orca = event["orcaResult"]
        artifact = orca["artifact"]
        table["orcaArtifactSha256s"].append(artifact["sha256"])
        table["orcaResultMetadata"].append({
            **_artifact_metadata(artifact),
            **{key: orca[key] for key in ("reportedBy", "subjectSha256", "bodySha256")},
        })
        table["resultSha256s"].append(orca["resultSha256"])
    need(len(table["rows"]) == len({payload(row) for row in table["rows"]}),
         "duplicate Gate event row")
    for name in ("dispatchIds", "messageIds", "receiptSha256s", "reportSha256s",
                 "verdictSha256s", "orcaArtifactSha256s", "resultSha256s"):
        need(len(table[name]) == len(set(table[name])), f"duplicate Gate {name} identity")
    return table


def decode_gate_table(value: Any) -> list[dict[str, Any]]:
    table = closed(value, set(GATE_TABLE_COLUMNS), "v4 Gate history table")
    need(all(isinstance(table[name], list) for name in GATE_TABLE_COLUMNS),
         "v4 Gate history columns must be arrays")
    lengths = {len(table[name]) for name in GATE_TABLE_COLUMNS}
    need(len(lengths) == 1 and next(iter(lengths)) <= MAX_LEDGER_ROWS,
         f"v4 Gate history columns differ or exceed {MAX_LEDGER_ROWS} rows")
    need(len(table["rows"]) == len({payload(row) for row in table["rows"] if isinstance(row, dict)}),
         "v4 Gate history has duplicate or non-object rows")
    for name in ("dispatchIds", "messageIds", "receiptSha256s", "reportSha256s",
                 "verdictSha256s", "orcaArtifactSha256s", "resultSha256s"):
        need(all(isinstance(item, str) for item in table[name]) and
             len(table[name]) == len(set(table[name])), f"v4 Gate {name} identities are invalid")
    result: list[dict[str, Any]] = []
    previous_by_phase: dict[str, str] = {}
    for index, item in enumerate(table["rows"]):
        row = closed(item, GATE_ROW_KEYS, f"v4 Gate row {index}")
        phase = row["phase"]
        receipt_id = table["receiptSha256s"][index]
        receipt_meta = closed(table["receiptMetadata"][index], {"path", "bytes"}, f"v4 Gate receipt metadata {index}")
        report_meta = closed(table["reportMetadata"][index], {"path", "bytes"}, f"v4 Gate report metadata {index}")
        verdict_meta = closed(table["verdictMetadata"][index], {"path", "bytes"}, f"v4 Gate verdict metadata {index}")
        orca_meta = closed(table["orcaResultMetadata"][index],
                           {"path", "bytes", "reportedBy", "subjectSha256", "bodySha256"},
                           f"v4 Gate Orca-result metadata {index}")
        event = {
            "eventId": receipt_id, **row,
            "dispatchId": table["dispatchIds"][index],
            "workerMessageId": table["messageIds"][index],
            "supersedesEventId": previous_by_phase.get(phase),
            "receipt": {**receipt_meta, "sha256": receipt_id},
            "report": {**report_meta, "sha256": table["reportSha256s"][index]},
            "verdictArtifact": {**verdict_meta, "sha256": table["verdictSha256s"][index]},
            "orcaResult": {
                "artifact": {"path": orca_meta["path"], "bytes": orca_meta["bytes"],
                             "sha256": table["orcaArtifactSha256s"][index]},
                "resultSha256": table["resultSha256s"][index],
                **{key: orca_meta[key] for key in ("reportedBy", "subjectSha256", "bodySha256")},
            },
        }
        previous_by_phase[phase] = receipt_id
        result.append(event)
    return result


def encode_decision_rows(transitions: Sequence[dict[str, Any]]) -> list[dict[str, Any]]:
    need(0 < len(transitions) <= MAX_LEDGER_ROWS,
         f"decision history must contain 1..{MAX_LEDGER_ROWS} v4 rows")
    previous_by_gate: dict[str, str] = {}
    rows: list[dict[str, Any]] = []
    for transition in transitions:
        gate_id = transition["gateId"]
        need(transition["previousTransitionId"] == previous_by_gate.get(gate_id),
             f"decision Gate {gate_id} history is not a single forward chain")
        row = {key: transition[key] for key in DECISION_ROW_KEYS}
        calculated = sha(payload({**row, "previousTransitionId": previous_by_gate.get(gate_id)}))
        need(calculated == transition["transitionId"],
             f"decision transition {transition['transitionId']} identity differs")
        previous_by_gate[gate_id] = calculated
        rows.append(row)
    need(len(rows) == len({payload(row) for row in rows}), "duplicate v4 decision row")
    return rows


def decode_decision_rows(value: Any) -> list[dict[str, Any]]:
    need(isinstance(value, list) and 0 < len(value) <= MAX_LEDGER_ROWS and
         len(value) == len({payload(row) for row in value if isinstance(row, dict)}),
         f"v4 decision history must contain unique object rows within {MAX_LEDGER_ROWS}")
    previous_by_gate: dict[str, str] = {}
    result: list[dict[str, Any]] = []
    for index, item in enumerate(value):
        row = closed(item, DECISION_ROW_KEYS, f"v4 decision row {index}")
        gate_id = row["gateId"]
        previous_id = previous_by_gate.get(gate_id)
        transition_id = sha(payload({**row, "previousTransitionId": previous_id}))
        result.append({"transitionId": transition_id, **row, "previousTransitionId": previous_id})
        previous_by_gate[gate_id] = transition_id
    return result


def encode_state(state: dict[str, Any]) -> dict[str, Any]:
    if state.get("schemaVersion") == 4:
        raw_schema_validate(state, "v4 state")
        return state
    if state.get("schemaVersion") == 3:
        state = decode_state(state)
    need(state.get("schemaVersion") == 2, "state wire version is unsupported")
    tasks: dict[str, dict[str, Any]] = {}
    for row in state["tasks"]:
        task_id = row["taskId"]
        need(task_id not in tasks, f"duplicate state task {task_id}")
        tasks[task_id] = {key: value for key, value in row.items() if key != "taskId"}
    materialization = state["materialization"]
    phases: dict[str, dict[str, Any]] = {}
    for row in state["phases"]:
        phase = row["phase"]
        need(phase not in phases, f"duplicate state phase {phase}")
        phases[phase] = {key: row[key] for key in ("taskIds", "summary")}
    wire = {
        **{key: value for key, value in state.items() if key not in {"schemaVersion", "generator", "inputs", "materialization", "integrationPrerequisites", "decisionGateHistory", "gateHistory", "phases", "tasks"}},
        "schemaVersion": 4, "generator": {"name": "tools/update_editor_progress_ledger.py", "version": 4},
        "inputs": {row["name"]: {key: value for key, value in row.items() if key != "name"} for row in state["inputs"]},
        "materialization": {**{key: value for key, value in materialization.items() if key != "initialTasks"},
                            "initialTasks": _keyed(materialization["initialTasks"], "taskId", "materialized tasks")},
        "decisionHistory": encode_decision_rows(state["decisionGateHistory"]),
        "gateHistory": encode_gate_table(state["gateHistory"]),
        "phases": phases, "tasks": tasks,
    }
    raw_schema_validate(wire, "v4 state")
    return wire


def decode_state(value: Any) -> dict[str, Any]:
    need(isinstance(value, dict), "state must be an object")
    if value.get("schemaVersion") == 2:
        return value
    if value.get("schemaVersion") == 4:
        raw_schema_validate(value, "v4 state")
        keys = {"documentKind", "schemaVersion", "generator", "runId", "authority", "protocolVectors", "snapshot", "inputs",
                "sourceFingerprintSha256", "policy", "materialization", "decisionHistory", "gateHistory", "phases", "tasks", "publication"}
        wire = closed(value, keys, "v4 state")
        need(wire["generator"] == {"name": "tools/update_editor_progress_ledger.py", "version": 4} and
             all(isinstance(wire[key], dict) for key in ("inputs", "gateHistory", "phases", "tasks")),
             "v4 state identity maps are invalid")
        materialization = wire["materialization"]
        need(isinstance(materialization, dict) and isinstance(materialization.get("initialTasks"), dict),
             "v4 materialization is invalid")
        initial = [{"taskId": task_id, **row} for task_id, row in materialization["initialTasks"].items()]
        initial.sort(key=lambda row: MATERIALIZED_ORDER.get(row.get("displayName"), 1000))
        decision_history = decode_decision_rows(wire["decisionHistory"])
        latest_by_gate: dict[str, dict[str, Any]] = {}
        for transition in decision_history:
            latest_by_gate[transition["gateId"]] = transition
        gates = [
            {key: latest[key] for key in ("gateId", "taskId", "question", "options", "createdAtUtc", "status", "outcome", "resolvedAtUtc", "resolution")} |
            {"latestTransitionId": latest["transitionId"]}
            for latest in latest_by_gate.values()
        ]
        gates.sort(key=lambda row: (row["createdAtUtc"], row["gateId"]))
        events = decode_gate_table(wire["gateHistory"])
        tasks = [{"taskId": task_id, **row} for task_id, row in wire["tasks"].items()]
        tasks.sort(key=task_order)
        derived_phases = derive_phases(tasks, events, gates)
        need(set(wire["phases"]) == set(PHASES), "v4 phase identities are invalid")
        phases = []
        for derived in derived_phases:
            phase = derived["phase"]
            metadata = closed(wire["phases"][phase], {"taskIds", "summary"}, f"v4 phase {phase}")
            need(metadata == {key: derived[key] for key in ("taskIds", "summary")},
                 f"v4 phase {phase} metadata is not derivable")
            phases.append(derived)
        inputs = [{"name": name, **wire["inputs"][name]} for name in ("specification", "manifest", "licenseSources") if name in wire["inputs"]]
        need(len(inputs) == 3 and len(wire["inputs"]) == 3, "v4 state input identities are invalid")
        return {
            **{key: item for key, item in wire.items() if key not in {"schemaVersion", "generator", "inputs", "materialization", "decisionHistory", "gateHistory", "phases", "tasks"}},
            "schemaVersion": 2, "generator": {"name": "tools/update_editor_progress_ledger.py", "version": 2}, "inputs": inputs,
            "materialization": {**{key: item for key, item in materialization.items() if key != "initialTasks"}, "initialTasks": initial},
            "integrationPrerequisites": gates, "decisionGateHistory": decision_history,
            "gateHistory": events, "phases": phases, "tasks": tasks,
        }
    keys = {"documentKind", "schemaVersion", "generator", "runId", "authority", "protocolVectors", "snapshot", "inputs",
            "sourceFingerprintSha256", "policy", "materialization", "integrationPrerequisites", "integrationPrerequisiteTasks", "decisionGateHistory",
            "gateHistory", "phases", "taskOrder", "tasks", "workerMessages", "publication"}
    wire = closed(value, keys, "v3 state")
    need(wire["schemaVersion"] == 3 and wire["generator"] == {"name": "tools/update_editor_progress_ledger.py", "version": 3} and
         all(isinstance(wire[key], dict) for key in ("inputs", "integrationPrerequisites", "integrationPrerequisiteTasks", "phases", "tasks", "workerMessages")),
         "v3 state identity maps are invalid")
    message_by_task: dict[str, str] = {}
    for message_id, task_id in wire["workerMessages"].items():
        need(isinstance(message_id, str) and isinstance(task_id, str) and task_id not in message_by_task,
             f"duplicate state message association for {task_id}")
        message_by_task[task_id] = message_id
    need(isinstance(wire["taskOrder"], list) and len(wire["taskOrder"]) == len(set(wire["taskOrder"])) and
         set(wire["taskOrder"]) == set(wire["tasks"]), "v3 state task order differs from task identities")
    tasks = [{"taskId": task_id, **wire["tasks"][task_id], "workerMessageId": message_by_task.get(task_id)} for task_id in wire["taskOrder"]]
    need(set(message_by_task).issubset(wire["tasks"]), "state message names an unknown task")
    materialization = wire["materialization"]
    need(isinstance(materialization, dict) and isinstance(materialization.get("initialTasks"), dict), "v3 materialization is invalid")
    initial = [{"taskId": task_id, **row} for task_id, row in materialization["initialTasks"].items()]
    initial.sort(key=lambda row: MATERIALIZED_ORDER.get(row.get("displayName"), 1000))
    gate_tasks = wire["integrationPrerequisiteTasks"]
    need(set(gate_tasks) == set(wire["integrationPrerequisites"]) and
         all(isinstance(task_id, str) and task_id for task_id in gate_tasks.values()),
         "v3 integration prerequisite task identities differ")
    gates = [{"gateId": gate_id, "taskId": gate_tasks[gate_id], **row} for gate_id, row in wire["integrationPrerequisites"].items()]
    gates.sort(key=lambda row: (row.get("createdAtUtc", ""), row["gateId"]))
    inputs = [{"name": name, **wire["inputs"][name]} for name in ("specification", "manifest", "licenseSources") if name in wire["inputs"]]
    need(len(inputs) == 3 and len(wire["inputs"]) == 3, "v3 state input identities are invalid")
    phases = [{"phase": phase, **wire["phases"][phase]} for phase in PHASES if phase in wire["phases"]]
    need(len(phases) == len(PHASES) and len(wire["phases"]) == len(PHASES), "v3 phase identities are invalid")
    return {
        **{key: value for key, value in wire.items() if key not in {"schemaVersion", "generator", "inputs", "materialization", "integrationPrerequisites", "integrationPrerequisiteTasks", "decisionGateHistory", "gateHistory", "phases", "taskOrder", "tasks", "workerMessages"}},
        "schemaVersion": 2, "generator": {"name": "tools/update_editor_progress_ledger.py", "version": 2},
        "inputs": inputs,
        "materialization": {**{key: value for key, value in materialization.items() if key != "initialTasks"}, "initialTasks": initial},
        "integrationPrerequisites": gates, "decisionGateHistory": decode_decision_history(wire["decisionGateHistory"]),
        "gateHistory": decode_gate_history(wire["gateHistory"]), "phases": phases, "tasks": tasks,
    }


def validate_snapshot(value: Any, authority: dict[str, Any] | None = None) -> dict[str, Any]:
    value = decode_snapshot(value)
    keys = {"documentKind", "schemaVersion", "runId", "runtimeId", "runGeneration", "capturedAtUtc", "consistencyMode", "revisionSha256", "runTaskIndex", "tasks", "decisionGates"}
    snap = closed(value, keys, "snapshot")
    need(snap["documentKind"] == "orca-progress-snapshot" and snap["schemaVersion"] == 2 and snap["runId"] == RUN_ID, "snapshot kind/version/Run is invalid")
    need(isinstance(snap["runtimeId"], str) and snap["runtimeId"] and isinstance(snap["runGeneration"], int) and snap["runGeneration"] > 0, "snapshot runtime identity is invalid")
    if authority is not None:
        need((snap["runtimeId"], snap["runGeneration"]) == (authority["runtimeId"], authority["runGeneration"]), "snapshot runtime/generation differs from immutable authority")
    need(snap["consistencyMode"] == "stable-two-pass-derived-revision", "snapshot lacks stable two-pass consistency")
    snap["capturedAtUtc"] = utc(snap["capturedAtUtc"], "snapshot capture")
    for key in ("runTaskIndex", "tasks", "decisionGates"):
        need(isinstance(snap[key], list), f"snapshot.{key} is not an array")
    index: dict[str, dict[str, Any]] = {}
    prior: tuple[str, str] | None = None
    times = [snap["capturedAtUtc"]]
    for row in snap["runTaskIndex"]:
        row = closed(row, {"taskId", "displayName", "status", "createdAtUtc"}, "task index row")
        need(row["taskId"] not in index and row["status"] in TASK_STATUSES and isinstance(row["displayName"], str) and row["displayName"], "task index identity is invalid")
        row["createdAtUtc"] = utc(row["createdAtUtc"], "task index creation")
        order = (row["createdAtUtc"], row["taskId"])
        need(prior is None or prior <= order, "task index order is invalid")
        prior, index[row["taskId"]] = order, row
        times.append(row["createdAtUtc"])
    tracked: dict[str, dict[str, Any]] = {}
    for row in snap["tasks"]:
        task_keys = {"taskId", "displayName", "status", "dependencies", "createdAtUtc", "completedAtUtc", "dispatch", "workerResult"}
        if row.get("completionProvenance") == RECOVERY_SETTLEMENT_PROVENANCE:
            task_keys.add("completionProvenance")
        row = closed(row, task_keys, "tracked task")
        task_id = row["taskId"]
        need(task_id not in tracked and task_id in index, f"tracked task {task_id} identity is invalid")
        indexed = index[task_id]
        need((row["displayName"], row["status"], row["createdAtUtc"]) == (indexed["displayName"], indexed["status"], indexed["createdAtUtc"]), f"tracked task {task_id} disagrees with index")
        need(isinstance(row["dependencies"], list) and row["dependencies"] == sorted(row["dependencies"]) and len(row["dependencies"]) == len(set(row["dependencies"])) and task_id not in row["dependencies"] and all(dep in index for dep in row["dependencies"]), f"task {task_id} dependencies are invalid")
        row["completedAtUtc"] = utc(row["completedAtUtc"], f"task {task_id} completion", True)
        if row["dispatch"] is not None:
            dispatch = closed(row["dispatch"], {"dispatchId", "status", "dispatchedAtUtc", "completedAtUtc"}, f"task {task_id} dispatch")
            need(dispatch["status"] in DISPATCH_STATUSES, f"task {task_id} dispatch status is invalid")
            dispatch["dispatchedAtUtc"] = utc(dispatch["dispatchedAtUtc"], "dispatch start")
            dispatch["completedAtUtc"] = utc(dispatch["completedAtUtc"], "dispatch completion", True)
            times.append(dispatch["dispatchedAtUtc"])
            if dispatch["completedAtUtc"]:
                times.append(dispatch["completedAtUtc"])
        if row["workerResult"] is not None:
            worker_keys = {"messageId", "outcome", "completedAtUtc", "reportPath", "reportedBy", "subject", "body", "resultPayload", "resultSha256"}
            if row["workerResult"].get("reportedBy") == "coordinator-review-settlement":
                worker_keys.add("recoverySettlement")
            worker = closed(row["workerResult"], worker_keys, f"task {task_id} result")
            need(worker["outcome"] in {"succeeded", "failed"} and SHA_RE.fullmatch(worker["resultSha256"] or ""), f"task {task_id} result identity is invalid")
            worker["completedAtUtc"] = utc(worker["completedAtUtc"], "worker completion")
            need(all(isinstance(worker[k], str) and worker[k] for k in ("messageId", "reportedBy", "subject", "body", "resultPayload")) and (worker["reportPath"] is None or isinstance(worker["reportPath"], str)), f"task {task_id} result fields are invalid")
            need(normalize_result(worker["resultPayload"], task_id, row["completedAtUtc"]) == worker,
                 f"task {task_id} result payload projection differs")
            times.append(worker["completedAtUtc"])
        lifecycle(row)
        if row["completedAtUtc"]:
            times.append(row["completedAtUtc"])
        tracked[task_id] = row
    gate_ids: set[str] = set()
    gate_prior: tuple[str, str] | None = None
    for gate in snap["decisionGates"]:
        gate = closed(gate, {"gateId", "taskId", "status", "outcome", "question", "options", "createdAtUtc", "resolvedAtUtc", "resolution"}, "decision Gate")
        need(gate["gateId"] not in gate_ids and gate["taskId"] in tracked and gate["status"] in {"pending", "resolved"} and gate["outcome"] in DECISION_OUTCOMES, f"decision Gate {gate['gateId']} is invalid")
        need(isinstance(gate["options"], list) and len(gate["options"]) == len(set(gate["options"])), f"decision Gate {gate['gateId']} options are invalid")
        gate["createdAtUtc"] = utc(gate["createdAtUtc"], "Gate creation")
        gate["resolvedAtUtc"] = utc(gate["resolvedAtUtc"], "Gate resolution", True)
        if gate["status"] == "pending":
            need(gate["outcome"] == "pending" and gate["resolvedAtUtc"] is None and gate["resolution"] is None, f"pending Gate {gate['gateId']} has terminal data")
        else:
            need(gate["outcome"] != "pending" and gate["resolvedAtUtc"] is not None and isinstance(gate["resolution"], str), f"resolved Gate {gate['gateId']} lacks closed outcome")
            times.append(gate["resolvedAtUtc"])
        order = (gate["createdAtUtc"], gate["gateId"])
        need(gate_prior is None or gate_prior <= order, "decision Gate order is invalid")
        gate_prior, gate_ids = order, gate_ids | {gate["gateId"]}
        times.append(gate["createdAtUtc"])
    gates_by_id = {row["gateId"]: row for row in snap["decisionGates"]}
    for task in tracked.values():
        worker = task["workerResult"]
        if worker is None or worker["reportedBy"] != "decision-gate":
            continue
        decision = json_bytes(worker["body"].encode("ascii"), f"decision result {task['taskId']}")
        gate = gates_by_id.get(decision.get("gateId")) if isinstance(decision, dict) else None
        need(gate is not None and gate["taskId"] == task["taskId"] and gate["status"] == "resolved" and decision.get("decision") == gate["resolution"], f"task {task['taskId']} synthetic decision result is not bound to its live Gate")
        need(worker["messageId"] == f"decision:{gate['gateId']}" and worker["resultSha256"] == sha(payload(decision)), f"task {task['taskId']} synthetic decision result identity differs")
    for name in MATERIALIZED:
        rows = [row for row in snap["runTaskIndex"] if row["displayName"] == name]
        need(len(rows) == 1 and rows[0]["taskId"] in tracked, f"materialized task {name} is absent or ambiguous")
    need(all(epoch(snap["capturedAtUtc"]) >= epoch(value) for value in times), "snapshot capture time predates data")
    need(snap["revisionSha256"] == sha(payload(revision_payload(snap))), "snapshot revision does not recompute")
    return snap


def classify_gate(raw: dict[str, Any], policies: dict[str, dict[str, Any]]) -> str:
    if raw.get("status") == "pending":
        return "pending"
    policy = policies.get(raw.get("id"))
    need(policy is not None and policy.get("ownerTaskId") == raw.get("task_id"), f"resolved Gate {raw.get('id')} has no authority policy")
    resolution = raw.get("resolution")
    matches = [outcome for outcome, key in (("accepted", "acceptedResolutions"), ("rejected", "rejectedResolutions"), ("superseded", "supersededResolutions")) if resolution in policy.get(key, [])]
    need(len(matches) == 1, f"resolved Gate {raw.get('id')} outcome is unclassified")
    return matches[0]


def capture_pass(command: Sequence[str], carry: set[str], authority: dict[str, Any]) -> dict[str, Any]:
    status = orca([*command, "status", "--json"], "Orca status")
    runtime = status.get("result", {}).get("runtime", {})
    runtime_id = runtime.get("runtimeId") or status.get("_meta", {}).get("runtimeId")
    need(runtime.get("state") == "ready", "Orca runtime is not ready")
    run_response = orca([*command, "orchestration", "run-show", "--id", RUN_ID, "--json"], "Orca run-show")
    run = run_response.get("result", {}).get("run")
    need(isinstance(run, dict) and run.get("id") == RUN_ID and run_response.get("_meta", {}).get("runtimeId") == runtime_id, "Orca Run/runtime identity changed")
    generation = run.get("consumer_generation")
    need(isinstance(generation, int) and generation > 0, "Orca Run generation is unavailable")
    task_response = orca([*command, "orchestration", "task-list", "--run", RUN_ID, "--brief", "--json"], "Orca task-list")
    gate_response = orca([*command, "orchestration", "gate-list", "--run", RUN_ID, "--json"], "Orca gate-list")
    need(task_response.get("_meta", {}).get("runtimeId") == runtime_id and gate_response.get("_meta", {}).get("runtimeId") == runtime_id, "Orca runtime changed during list capture")
    raw_tasks, raw_gates = task_response.get("result", {}).get("tasks"), gate_response.get("result", {}).get("gates")
    need(isinstance(raw_tasks, list) and isinstance(raw_gates, list), "Orca list capture is incomplete")
    by_id = {row.get("id"): row for row in raw_tasks}
    need(len(by_id) == len(raw_tasks) and all(isinstance(task_id, str) and task_id for task_id in by_id), "Orca task-list contains duplicate or invalid identities")
    materialized = [row for row in raw_tasks if (row.get("display_name") or row.get("task_title")) in MATERIALIZED_ORDER]
    need(len(materialized) == 33, "live Run lacks exactly 33 materialized definitions")
    selected = set(carry) | {row["id"] for row in materialized} | {row["task_id"] for row in raw_gates}
    need(selected.issubset(by_id), f"previously known task disappeared: {sorted(selected-set(by_id))}")
    dispatches: dict[str, dict[str, Any] | None] = {}
    for task_id in sorted(selected):
        response = orca([*command, "orchestration", "dispatch-show", "--task", task_id, "--json"], f"Orca dispatch-show {task_id}")
        need(response.get("_meta", {}).get("runtimeId") == runtime_id, "Orca runtime changed during dispatch capture")
        dispatches[task_id] = response.get("result", {}).get("dispatch")
    tasks = sorted((normalize_task(by_id[task_id], dispatches[task_id]) for task_id in selected), key=lambda row: (row["createdAtUtc"], row["taskId"]))
    index = sorted(({"taskId": row["id"], "displayName": row.get("display_name") or row.get("task_title"), "status": row["status"], "createdAtUtc": utc(row["created_at"], "task creation")} for row in raw_tasks), key=lambda row: (row["createdAtUtc"], row["taskId"]))
    policies = {row["gateId"]: row for row in authority.get("decisionGatePolicies", [])}
    gates = []
    for raw in raw_gates:
        try:
            options = json.loads(raw.get("options") or "[]")
        except json.JSONDecodeError as exc:
            raise LedgerError(f"decision Gate {raw.get('id')} options are invalid") from exc
        gates.append({"gateId": raw["id"], "taskId": raw["task_id"], "status": raw["status"], "outcome": classify_gate(raw, policies), "question": raw["question"], "options": options, "createdAtUtc": utc(raw["created_at"], "Gate creation"), "resolvedAtUtc": utc(raw.get("resolved_at"), "Gate resolution", True), "resolution": raw.get("resolution")})
    gates.sort(key=lambda row: (row["createdAtUtc"], row["gateId"]))
    final = orca([*command, "status", "--json"], "Orca final status")
    need((final.get("result", {}).get("runtime", {}).get("runtimeId") or final.get("_meta", {}).get("runtimeId")) == runtime_id, "Orca runtime changed during capture")
    snapshot = {"documentKind": "orca-progress-snapshot", "schemaVersion": 2, "runId": RUN_ID, "runtimeId": runtime_id, "runGeneration": generation, "capturedAtUtc": now(), "consistencyMode": "stable-two-pass-derived-revision", "revisionSha256": "0"*64, "runTaskIndex": index, "tasks": tasks, "decisionGates": gates}
    snapshot["revisionSha256"] = sha(payload(revision_payload(snapshot)))
    return validate_snapshot(snapshot, authority)


def capture(command: Sequence[str], carry: set[str], authority: dict[str, Any]) -> dict[str, Any]:
    last = None
    for _ in range(3):
        first, second = capture_pass(command, carry, authority), capture_pass(command, carry, authority)
        if (first["runtimeId"], first["runGeneration"], first["revisionSha256"]) == (second["runtimeId"], second["runGeneration"], second["revisionSha256"]):
            return second
        last = (first["revisionSha256"], second["revisionSha256"])
    raise LedgerError(f"Orca state drifted across all stable capture attempts: {last}")


def validate_artifact(record: Any, roots: Sequence[Path], label: str) -> tuple[Path, bytes]:
    record = closed(record, {"path", "bytes", "sha256"}, label)
    need(isinstance(record["bytes"], int) and record["bytes"] > 0 and SHA_RE.fullmatch(record.get("sha256", "")), f"{label} identity is invalid")
    opened = safe_pinned(record["path"], roots, label)
    need(opened.size == record["bytes"] and opened.sha256 == record["sha256"], f"{label} content identity does not match")
    return opened.path, opened.data


def fsync_dir(path: Path) -> None:
    try:
        fd = os.open(path, os.O_RDONLY | getattr(os, "O_DIRECTORY", 0))
    except OSError as exc:
        if os.name == "nt" and exc.errno in {errno.EACCES, errno.EINVAL, errno.EPERM}:
            return
        raise
    try:
        os.fsync(fd)
    except OSError as exc:
        if not (os.name == "nt" and exc.errno in {errno.EACCES, errno.EINVAL, errno.EPERM}):
            raise
    finally:
        os.close(fd)


def stage_atomic_write(path: Path, data: bytes, fault_prefix: str | None = None) -> Path:
    path.parent.mkdir(parents=True, exist_ok=True)
    no_reparse(path.parent, f"write parent for {path}")
    if os.path.lexists(path):
        no_reparse(path, f"write target {path}")
    fd, name = tempfile.mkstemp(prefix=f".{path.name}.", suffix=".tmp", dir=path.parent)
    temporary = Path(name)
    try:
        with os.fdopen(fd, "wb") as stream:
            stream.write(data)
            if fault_prefix:
                fault(f"{fault_prefix}-write")
            stream.flush()
            os.fsync(stream.fileno())
            if fault_prefix:
                fault(f"{fault_prefix}-file-fsync")
        return temporary
    except BaseException:
        temporary.unlink(missing_ok=True)
        raise


def atomic_write(path: Path, data: bytes, fault_prefix: str | None = None) -> None:
    temporary = stage_atomic_write(path, data, fault_prefix)
    try:
        os.replace(temporary, path)
        if fault_prefix:
            fault(f"{fault_prefix}-replace")
        fsync_dir(path.parent)
        if fault_prefix:
            fault(f"{fault_prefix}-parent-fsync")
    finally:
        temporary.unlink(missing_ok=True)


def evidence_record(data: bytes) -> dict[str, Any]:
    digest = sha(data)
    return {"path": (EVIDENCE_REL / digest).as_posix(), "bytes": len(data), "sha256": digest}


def stage_blob(stage: Path, data: bytes) -> dict[str, Any]:
    record = evidence_record(data)
    target = stage / "evidence/sha256" / record["sha256"]
    target.parent.mkdir(parents=True, exist_ok=True)
    if target.exists():
        need(pinned(target, "staged evidence", False) == data, "staged content-address collision")
    else:
        atomic_write(target, data)
    return record


def stage_previous_evidence(repo: Path, previous: dict[str, Any] | None, stage: Path) -> None:
    if not previous or previous.get("schemaVersion") != 2:
        return
    for event in previous.get("gateHistory", []):
        records = [event["receipt"], event["report"], event["verdictArtifact"], event["orcaResult"]["artifact"]]
        for record in records:
            logical = Path(record["path"])
            need(not logical.is_absolute() and ".." not in logical.parts and logical == EVIDENCE_REL / record["sha256"], "previous durable evidence path is unsafe")
            data = pinned(repo / logical, "previous durable evidence")
            need(len(data) == record["bytes"] and sha(data) == record["sha256"], "previous durable evidence identity differs")
            need(stage_blob(stage, data) == record, "previous durable evidence record changed")


def legacy_task(task: dict[str, Any]) -> dict[str, Any]:
    result = {key: value for key, value in task.items() if key != "workerResult"}
    if task["workerResult"] is not None:
        result["workerResult"] = {key: task["workerResult"][key] for key in ("messageId", "outcome", "completedAtUtc", "reportPath")}
    return result


def fingerprint_v1(receipt: dict[str, Any], task: dict[str, Any]) -> str:
    return sha(payload({
        "runId": receipt["runId"], "phase": receipt["phase"], "gateTask": legacy_task(task),
        "gateVerdict": receipt["gateVerdict"], "gateScope": receipt["gateScope"],
        "supersedesGateTaskId": receipt["supersedesGateTaskId"], "report": receipt["report"],
        "verdictArtifact": receipt["verdictArtifact"], "fixedPolicy": receipt["fixedPolicy"],
    }))


def fingerprint_v2(receipt: dict[str, Any], task: dict[str, Any]) -> str:
    return sha(payload({
        "runId": receipt["runId"], "phase": receipt["phase"], "gateTaskId": receipt["gateTaskId"],
        "dispatchId": receipt["dispatchId"], "workerMessageId": receipt["workerMessageId"],
        "workerResultSha256": task["workerResult"]["resultSha256"], "completedAtUtc": receipt["completedAtUtc"],
        "gateVerdict": receipt["gateVerdict"], "gateScope": receipt["gateScope"],
        "supersedesReceiptSha256": receipt["supersedesReceiptSha256"], "report": receipt["report"],
        "verdictArtifact": receipt["verdictArtifact"], "fixedPolicy": receipt["fixedPolicy"],
    }))


def worker_attestation(body: str) -> dict[str, Any]:
    lines = [line[len(ATTESTATION_PREFIX):] for line in body.splitlines() if line.startswith(ATTESTATION_PREFIX)]
    need(len(lines) == 1, "Gate worker result must contain exactly one structured attestation")
    try:
        value = json.loads(lines[0])
    except json.JSONDecodeError as exc:
        raise LedgerError("Gate worker attestation is not valid JSON") from exc
    return closed(value, {"phase", "gateVerdict", "reportSha256", "verdictSha256"}, "Gate worker attestation")


def validate_verdict(value: Any, receipt: dict[str, Any], version: int) -> None:
    supersedes = "supersedesGateTaskId" if version == 1 else "supersedesReceiptSha256"
    keys = {"documentKind", "schemaVersion", "runId", "phase", "gateTaskId", "dispatchId", "gateVerdict", "gateScope", supersedes, "fixedPolicy"}
    value = closed(value, keys, "Gate verdict")
    need(value["documentKind"] == "gate-verdict" and value["schemaVersion"] == version, "Gate verdict kind/version is invalid")
    for key in keys - {"documentKind", "schemaVersion"}:
        need(value[key] == receipt[key], f"Gate verdict field {key} differs from receipt")


def make_event(receipt: dict[str, Any], receipt_data: bytes, report_data: bytes, verdict_data: bytes,
               task: dict[str, Any], authority_class: str, supersedes: str | None, stage: Path) -> dict[str, Any]:
    worker = task["workerResult"]
    return {
        "eventId": sha(receipt_data), "authorityClass": authority_class, "receipt": stage_blob(stage, receipt_data),
        "inputFingerprintSha256": receipt["inputFingerprintSha256"], "phase": receipt["phase"],
        "gateTaskId": receipt["gateTaskId"], "dispatchId": receipt["dispatchId"],
        "workerMessageId": receipt["workerMessageId"], "completedAtUtc": receipt["completedAtUtc"],
        "gateVerdict": receipt["gateVerdict"], "gateScope": receipt["gateScope"],
        "supersedesEventId": supersedes, "report": stage_blob(stage, report_data),
        "verdictArtifact": stage_blob(stage, verdict_data),
        "orcaResult": {
            "artifact": stage_blob(stage, worker["resultPayload"].encode("ascii")),
            "resultSha256": worker["resultSha256"], "reportedBy": worker["reportedBy"],
            "subjectSha256": sha(worker["subject"].encode("utf-8")), "bodySha256": sha(worker["body"].encode("utf-8")),
        },
    }


def receipts(paths: Sequence[Path], snapshot: dict[str, Any], roots: Sequence[Path], authority: dict[str, Any],
             previous: list[dict[str, Any]], stage: Path) -> list[dict[str, Any]]:
    tracked = {row["taskId"]: row for row in snapshot["tasks"]}
    bootstrap = {row["receiptSha256"]: row for row in authority["bootstrapEvents"]}
    existing = {row["eventId"] for row in previous}
    messages = {row["workerMessageId"] for row in previous}
    latest: dict[str, dict[str, Any]] = {}
    for event in previous:
        latest[event["phase"]] = event
    drafts: list[tuple[dict[str, Any], bytes]] = []
    supplied: set[str] = set()
    for argument in paths:
        opened_receipt = safe_pinned(argument, roots, "Gate receipt")
        receipt_path, data = opened_receipt.path, opened_receipt.data
        digest = sha(data)
        need(digest not in supplied, "duplicate Gate receipt input")
        supplied.add(digest)
        if digest not in existing:
            drafts.append((json_bytes(data, "Gate receipt"), data))
    drafts.sort(key=lambda item: (utc(item[0].get("completedAtUtc"), "receipt completion"), item[0].get("phase", ""), sha(item[1])))
    result = list(previous)
    for receipt, receipt_data in drafts:
        receipt_hash = sha(receipt_data)
        fixed = bootstrap.get(receipt_hash)
        version = receipt.get("schemaVersion")
        if fixed:
            keys = {"documentKind", "schemaVersion", "runId", "phase", "gateTaskId", "dispatchId", "workerMessageId", "completedAtUtc", "reviewExecutionOutcome", "gateVerdict", "gateScope", "supersedesGateTaskId", "report", "verdictArtifact", "inputFingerprintSha256", "fixedPolicy"}
            authority_class = "immutable-bootstrap"
        else:
            keys = {"documentKind", "schemaVersion", "runId", "phase", "gateTaskId", "dispatchId", "workerMessageId", "completedAtUtc", "reviewExecutionOutcome", "gateVerdict", "gateScope", "supersedesReceiptSha256", "report", "verdictArtifact", "inputFingerprintSha256", "fixedPolicy"}
            authority_class = "materialized-gate"
        receipt = closed(receipt, keys, "Gate receipt")
        need(receipt["documentKind"] == "gate-receipt" and ((fixed is not None and version == 1) or (fixed is None and version == 2)), "only immutable bootstrap may use receipt v1")
        need(receipt["runId"] == RUN_ID and receipt["phase"] in PHASES and receipt["gateVerdict"] in verdicts(receipt["phase"]), "receipt Run/phase/verdict is invalid")
        need(receipt["reviewExecutionOutcome"] == "succeeded" and receipt["fixedPolicy"] == FIXED_POLICY, "receipt outcome/policy is invalid")
        expected_scope = "final-release" if receipt["gateVerdict"].endswith("FINAL RELEASE PASS") else "engineering"
        need(receipt["gateScope"] == expected_scope and expected_scope != "final-release", "receipt scope or prohibited release PASS is invalid")
        task = tracked.get(receipt["gateTaskId"])
        need(task is not None and task["status"] == "completed" and task["dispatch"] is not None and task["workerResult"] is not None, "receipt Gate task is absent or incomplete")
        dispatch, worker = task["dispatch"], task["workerResult"]
        need(dispatch["dispatchId"] == receipt["dispatchId"] and dispatch["status"] == "completed", "receipt dispatch identity is stale")
        need(worker["outcome"] == "succeeded" and worker["messageId"] == receipt["workerMessageId"], "receipt worker identity is stale")
        receipt["completedAtUtc"] = utc(receipt["completedAtUtc"], "receipt completion")
        need(receipt["completedAtUtc"] == worker["completedAtUtc"] and receipt["workerMessageId"] not in messages, "receipt completion or message replay is invalid")
        messages.add(receipt["workerMessageId"])
        report_path, report_data = validate_artifact(receipt["report"], roots, "Gate report")
        _, verdict_data = validate_artifact(receipt["verdictArtifact"], roots, "Gate verdict")
        need(worker["reportPath"] is not None, "Gate result has no report path")
        worker_report = safe_pinned(worker["reportPath"], roots, "Orca Gate report").path
        need(worker_report == report_path and os.path.samefile(worker_report, report_path), "receipt report is not the exact lexical Orca result report")
        validate_verdict(json_bytes(verdict_data, "Gate verdict"), receipt, version)
        prior = latest.get(receipt["phase"])
        supersedes = prior["eventId"] if prior else None
        need(prior is None or epoch(receipt["completedAtUtc"]) > epoch(prior["completedAtUtc"]), "re-Gate completion must be strictly later than the event it supersedes")
        if fixed:
            mapping = {"phase": "phase", "gateTaskId": "taskId", "dispatchId": "dispatchId", "workerMessageId": "workerMessageId", "completedAtUtc": "completedAtUtc", "gateVerdict": "gateVerdict", "gateScope": "gateScope"}
            need(all(receipt[key] == fixed[value] for key, value in mapping.items()), "bootstrap receipt identity differs from immutable authority")
            need(receipt["report"]["sha256"] == fixed["reportSha256"] and receipt["verdictArtifact"]["sha256"] == fixed["verdictSha256"], "bootstrap evidence differs from authority")
            need(receipt["supersedesGateTaskId"] == fixed["supersedesGateTaskId"] == (prior["gateTaskId"] if prior else None), "bootstrap supersession is invalid")
            need(worker["resultSha256"] == fixed["resultSha256"] and worker["reportedBy"] == fixed["reportedBy"] and sha(worker["subject"].encode()) == fixed["subjectSha256"] and sha(worker["body"].encode()) == fixed["bodySha256"], "bootstrap Orca result differs from authority")
            expected_fingerprint = fingerprint_v1(receipt, task)
        else:
            need(receipt["phase"] in GATE_TASKS and receipt["gateTaskId"] == GATE_TASKS[receipt["phase"]], "ordinary or wrong-phase task cannot issue a Gate receipt")
            need(task["displayName"] == f"G{phase_number(receipt['phase'])}" and receipt["supersedesReceiptSha256"] == supersedes, "Gate identity or immutable re-Gate link is invalid")
            attested = worker_attestation(worker["body"])
            need(attested == {"phase": receipt["phase"], "gateVerdict": receipt["gateVerdict"], "reportSha256": receipt["report"]["sha256"], "verdictSha256": receipt["verdictArtifact"]["sha256"]}, "receipt is not bound to Orca report/verdict attestation")
            need(worker["subject"] == receipt["gateVerdict"], "Gate worker subject is not the verdict")
            expected_fingerprint = fingerprint_v2(receipt, task)
        need(receipt["inputFingerprintSha256"] == expected_fingerprint, "receipt fingerprint is invalid")
        event = make_event(receipt, receipt_data, report_data, verdict_data, task, authority_class, supersedes, stage)
        result.append(event)
        latest[receipt["phase"]] = event
    return result


def label(display_name: str) -> str:
    if display_name in MATERIALIZED_ORDER:
        return display_name
    for pattern, fixed in ((r"^P5-00-R1\b", "P5-00-R1"), (r"^P5-90-R1\b", "P5-90-R1"), (r"^P5-R2-([ABC])\b", None), (r"^P5-90-R2-([DEF])\b", None), (r"^P5-R3\b", "P5-R3")):
        match = re.match(pattern, display_name)
        if match:
            return fixed or f"P5-R2-{match.group(1)}"
    return display_name


def label_phase(value: str) -> str | None:
    match = re.match(r"^[PG](10|[0-9])(?:\b|-)", value)
    return f"P{match.group(1)}" if match else None


def project_task(row: dict[str, Any]) -> dict[str, Any]:
    worker, dispatch = row["workerResult"], row["dispatch"]
    result = {"taskId": row["taskId"], "label": label(row["displayName"]), "displayName": row["displayName"], "status": row["status"], "dependencies": row["dependencies"], "completedAtUtc": row["completedAtUtc"], "dispatchId": dispatch["dispatchId"] if dispatch else None, "workerOutcome": worker["outcome"] if worker else "not_reported", "workerMessageId": worker["messageId"] if worker else None, "reportPath": worker["reportPath"] if worker else None}
    if row.get("completionProvenance") == RECOVERY_SETTLEMENT_PROVENANCE:
        result["completionProvenance"] = RECOVERY_SETTLEMENT_PROVENANCE
    return result


def task_order(row: dict[str, Any]) -> tuple[int, int, str, str]:
    match = re.match(r"^(?:G|P)(\d+)", row["label"])
    return (int(match.group(1)) if match else 99, MATERIALIZED_ORDER.get(row["label"], 1000), row["label"], row["taskId"])


def decision_history(snapshot: dict[str, Any], previous: dict[str, Any] | None) -> tuple[list[dict[str, Any]], list[dict[str, Any]]]:
    history = list(previous.get("decisionGateHistory", [])) if previous and previous.get("schemaVersion") == 2 else []
    prior: dict[str, dict[str, Any]] = {}
    for event in history:
        prior[event["gateId"]] = event
    live = {row["gateId"]: row for row in snapshot["decisionGates"]}
    for gate_id, event in prior.items():
        need(gate_id in live or event["outcome"] in {"accepted", "rejected", "superseded"}, f"unresolved prior Gate {gate_id} disappeared")
    current = []
    for gate in snapshot["decisionGates"]:
        old = prior.get(gate["gateId"])
        immutable = {key: gate[key] for key in ("gateId", "taskId", "question", "options", "createdAtUtc")}
        if old:
            need(all(old[key] == immutable[key] for key in immutable), f"decision Gate {gate['gateId']} identity changed")
            if old["status"] == "resolved":
                need(all(old[key] == gate[key] for key in ("status", "outcome", "resolvedAtUtc", "resolution")), f"resolved decision Gate {gate['gateId']} changed after its terminal transition")
            else:
                need(gate["status"] in {"pending", "resolved"}, f"decision Gate {gate['gateId']} has an invalid transition")
        transition = {**immutable, "status": gate["status"], "outcome": gate["outcome"], "resolvedAtUtc": gate["resolvedAtUtc"], "resolution": gate["resolution"], "previousTransitionId": old["transitionId"] if old else None}
        unchanged = old is not None and all(old[key] == transition[key] for key in ("status", "outcome", "resolvedAtUtc", "resolution"))
        observed = snapshot["capturedAtUtc"]
        event = old if unchanged else {"transitionId": sha(payload({**transition, "observedAtUtc": observed})), **transition, "observedAtUtc": observed}
        if not unchanged:
            history.append(event)
        current.append({**immutable, "status": gate["status"], "outcome": gate["outcome"], "resolvedAtUtc": gate["resolvedAtUtc"], "resolution": gate["resolution"], "latestTransitionId": event["transitionId"]})
    for gate_id, event in prior.items():
        if gate_id not in live:
            current.append({key: event[key] for key in ("gateId", "taskId", "question", "options", "createdAtUtc", "status", "outcome", "resolvedAtUtc", "resolution")} | {"latestTransitionId": event["transitionId"]})
    current.sort(key=lambda row: (row["createdAtUtc"], row["gateId"]))
    return history, current


def derive_phases(tasks: list[dict[str, Any]], events: list[dict[str, Any]], gates: list[dict[str, Any]]) -> list[dict[str, Any]]:
    by_phase = {phase: [] for phase in PHASES}
    task_phases = {phase: [] for phase in PHASES}
    for event in events:
        by_phase[event["phase"]].append(event)
    for task in tasks:
        phase = label_phase(task["label"])
        if phase:
            task_phases[phase].append(task)
    blockers = [row for row in gates if row["outcome"] in {"pending", "rejected", "superseded"}]
    result = []
    for phase in PHASES:
        history, phase_tasks = by_phase[phase], task_phases[phase]
        latest = history[-1] if history else None
        gate = "NOT_RUN" if latest is None else ("PASS" if passes(latest["gateVerdict"]) else "FAIL/BLOCKED")
        if phase == "P5" and gate == "PASS":
            need(not blockers, f"P5 cannot PASS while decision prerequisites block: {[row['gateId'] for row in blockers]}")
        if gate == "PASS":
            execution, summary = "GATED", f"{phase} engineering Gate is accepted; release policy remains blocked."
        elif gate == "FAIL/BLOCKED":
            execution, summary = "BLOCKED", f"{phase} Gate is FAIL/BLOCKED and retained in immutable history."
        elif phase == "P5":
            active = any(re.match(r"^P5-R\d+(?:-|$)", row["label"]) and row["status"] in {"pending", "ready", "dispatched"} for row in phase_tasks)
            execution, summary = ("REPAIR_ACTIVE" if active else "MATERIALIZED_PENDING"), "P5 repair remains active; G5 has no authoritative receipt."
        else:
            execution, summary = "MATERIALIZED_PENDING", f"{phase} is materialized; no authoritative Gate receipt exists."
        task_ids = [row["taskId"] for row in phase_tasks]
        task_ids.extend(row["gateTaskId"] for row in history if row["gateTaskId"] not in task_ids)
        result.append({"phase": phase, "executionStatus": execution, "engineeringGate": gate, "gateTaskId": latest["gateTaskId"] if latest else None, "gateHistoryReceiptSha256s": [row["eventId"] for row in history], "taskIds": task_ids, "summary": summary})
    return result


def previous_state(repo: Path) -> dict[str, Any] | None:
    pointer_path = repo / POINTER_REL
    if os.path.lexists(pointer_path):
        pointer_data = pinned(pointer_path, "authoritative pointer")
        pointer = closed(json_bytes(pointer_data, "authoritative pointer"), {"bundleId", "bundlePath", "documentKind", "markdownSha256", "schemaVersion", "snapshotSha256", "stateSha256"}, "authoritative pointer")
        need(pointer["documentKind"] == "progress-ledger-pointer" and pointer["schemaVersion"] == 2 and document(pointer) == pointer_data and SHA_RE.fullmatch(pointer["bundleId"]), "previous authoritative pointer is invalid")
        bundle_rel = Path(pointer["bundlePath"])
        need(not bundle_rel.is_absolute() and ".." not in bundle_rel.parts and bundle_rel == BUNDLES_REL / pointer["bundleId"], "previous authoritative bundle path is unsafe")
        bundle = repo / bundle_rel
        state_path = bundle / "state.json"
        state_data = pinned(state_path, "previous authoritative state")
        need(sha(state_data) == pointer.get("stateSha256"), "previous authoritative bundle is corrupt")
        return decode_state(json_bytes(state_data, "previous authoritative state"))
    legacy = repo / STATE_REL
    return decode_state(load(legacy, "legacy state")) if legacy.is_file() else None


def state_from(repo: Path, snapshot: dict[str, Any], events: list[dict[str, Any]], previous: dict[str, Any] | None,
               authority_record: dict[str, Any], vectors_record: dict[str, Any], snapshot_hash: str) -> dict[str, Any]:
    inputs, policy = derive_policy(repo)
    tracked = {row["taskId"]: row for row in snapshot["tasks"]}
    by_name = {row["displayName"]: row for row in snapshot["runTaskIndex"] if row["displayName"] in MATERIALIZED_ORDER}
    materialized = [{"taskId": by_name[name]["taskId"], "displayName": name, "status": tracked[by_name[name]["taskId"]]["status"]} for name in MATERIALIZED]
    old_materialized = previous.get("materialization", {}).get("initialTasks") if previous else None
    if old_materialized:
        need([{k: row[k] for k in ("taskId", "displayName")} for row in materialized] == [{k: row[k] for k in ("taskId", "displayName")} for row in old_materialized], "33-task materialization identity changed")
    for phase, task_id in GATE_TASKS.items():
        need(next(row for row in materialized if row["displayName"] == f"G{phase_number(phase)}")["taskId"] == task_id, f"{phase} Gate identity changed")
    transitions, gates = decision_history(snapshot, previous)
    old_ids = {row["taskId"] for row in previous.get("tasks", [])} if previous else set()
    relevant = old_ids | {row["taskId"] for row in materialized} | {row["gateTaskId"] for row in events} | {row["taskId"] for row in gates}
    for row in snapshot["tasks"]:
        task_label = label(row["displayName"])
        if task_label != row["displayName"] or task_label.startswith("P5-R2-"):
            relevant.add(row["taskId"])
    need(relevant.issubset(tracked), "previous relevant task disappeared from tracked snapshot")
    tasks = sorted((project_task(tracked[task_id]) for task_id in relevant), key=task_order)
    phases = derive_phases(tasks, events, gates)
    projection_hash = sha(payload({"runId": RUN_ID, "runtimeId": snapshot["runtimeId"], "runGeneration": snapshot["runGeneration"], "revisionSha256": snapshot["revisionSha256"], "materialization": materialized, "tasks": tasks, "decisionGateHistory": transitions, "gateEventIds": [row["eventId"] for row in events]}))
    source_hash = sha(payload({"schemaVersion": 2, "runId": RUN_ID, "snapshotSha256": snapshot_hash, "relevantProjectionSha256": projection_hash, "inputs": inputs, "authoritySha256": authority_record["sha256"], "protocolVectorsSha256": vectors_record["sha256"], "gateEventIds": [row["eventId"] for row in events]}))
    state = {
        "documentKind": "progress-ledger-state", "schemaVersion": 2,
        "generator": {"name": "tools/update_editor_progress_ledger.py", "version": 2}, "runId": RUN_ID,
        "authority": authority_record, "protocolVectors": vectors_record,
        "snapshot": {"capturedAtUtc": snapshot["capturedAtUtc"], "runtimeId": snapshot["runtimeId"], "runGeneration": snapshot["runGeneration"], "revisionSha256": snapshot["revisionSha256"], "snapshotSha256": snapshot_hash, "relevantProjectionSha256": projection_hash, "runTaskCount": len(snapshot["runTaskIndex"]), "trackedTaskCount": len(snapshot["tasks"])},
        "inputs": inputs, "sourceFingerprintSha256": source_hash, "policy": policy,
        "materialization": {"status": "MATERIALIZED", "initialTaskCount": 33, "initialTasks": materialized, "deferredLedgerDefinitions": DEFERRED},
        "integrationPrerequisites": gates, "decisionGateHistory": transitions, "gateHistory": events,
        "phases": phases, "tasks": tasks,
        "publication": {"ledgerPath": LEDGER_REL.as_posix(), "pointerPath": POINTER_REL.as_posix(), "sourceFingerprintSha256": source_hash, "markdownSha256": "0"*64},
    }
    state["publication"]["markdownSha256"] = sha(render(state))
    return state


def cell(value: Any) -> str:
    return ("" if value is None else str(value)).replace("|", "\\|").replace("\r", " ").replace("\n", " ")


def render(state: dict[str, Any]) -> bytes:
    phases = {row["phase"]: row for row in state["phases"]}
    p5_tasks = [row for row in state["tasks"] if label_phase(row["label"]) == "P5"]
    blockers = [row["gateId"] for row in state["integrationPrerequisites"] if row["outcome"] != "accepted"]
    lines = [
        "# glTF Scene Editor Parity Progress (Authoritative Projection)", "",
        "> Generated from the atomically selected v2 bundle. Do not edit by hand.", "",
        "## Fixed Policy", "", f"- F42: `{state['policy']['f42']}`", f"- R24: `{state['policy']['r24']}`",
        f"- Metal: `{state['policy']['metal']}`", f"- release/distribution: `{state['policy']['releaseDistribution']}`", "",
        "## Phase Gates", "", "| Phase | Execution | Engineering Gate | Gate Task | Summary |", "|---|---|---|---|---|",
    ]
    for phase in PHASES:
        row = phases[phase]
        lines.append(f"| {phase} | {row['executionStatus']} | {row['engineeringGate']} | {cell(row['gateTaskId'])} | {cell(row['summary'])} |")
    lines.extend(["", "## Decision Prerequisites", "", f"Blocking decision Gates: `{', '.join(blockers) if blockers else 'none'}`.", "", "| Gate | Owner Task | Status | Outcome | Resolution |", "|---|---|---|---|---|"])
    for gate in state["integrationPrerequisites"]:
        lines.append(f"| {gate['gateId']} | {gate['taskId']} | {gate['status']} | {gate['outcome']} | {cell(gate['resolution'])} |")
    lines.extend(["", "## P5 Live Task State", "", "| Label | Status | Task | Dependencies | Worker |", "|---|---|---|---|---|"])
    for task in p5_tasks:
        lines.append(f"| {cell(task['label'])} | {task['status']} | {task['taskId']} | {cell(', '.join(task['dependencies']))} | {task['workerOutcome']} |")
    lines.extend(["", "## Immutable Gate History", "", "| Event | Phase | Task | Verdict | Supersedes | Report SHA-256 |", "|---|---|---|---|---|---|"])
    for event in state["gateHistory"]:
        lines.append(f"| `{event['eventId']}` | {event['phase']} | {event['gateTaskId']} | {event['gateVerdict']} | {cell(event['supersedesEventId'])} | `{event['report']['sha256']}` |")
    lines.extend(["", "History is forward-linked only: a re-Gate appends `supersedesEventId` and never rewrites an old event.", "", "## P5-P10 Materialization", "", "- Initial Task definitions: `33/33` materialized.", "- Deferred ledger definitions: `L5`, `L6`, `L7`, `L8`, `L9`, `L10`.", "- Ordinary/non-Gate success never promotes a phase.", "", "## Input Identity", "", "| Input | Bytes | SHA-256 |", "|---|---:|---|"])
    for item in [*state["inputs"], state["authority"], state["protocolVectors"]]:
        lines.append(f"| `{item['path']}` | {item['bytes']} | `{item['sha256']}` |")
    lines.extend(["", f"Common source fingerprint: `{state['sourceFingerprintSha256']}`.", f"Stable Orca revision: `{state['snapshot']['revisionSha256']}` on runtime `{state['snapshot']['runtimeId']}`.", ""])
    return "\n".join(lines).encode("utf-8")


def build(repo: Path, snapshot: dict[str, Any], receipt_paths: Sequence[Path], roots: Sequence[Path],
          previous: dict[str, Any] | None, authority: dict[str, Any], stage: Path) -> tuple[dict[str, Any], bytes, bytes, bytes]:
    snapshot = validate_snapshot(snapshot, authority)
    snapshot_data = document(encode_snapshot(snapshot))
    stage_previous_evidence(repo, previous, stage)
    old_events = list(previous.get("gateHistory", [])) if previous and previous.get("schemaVersion") == 2 else []
    events = receipts(receipt_paths, snapshot, roots, authority, old_events, stage)
    authority_record, vector_record = identity(repo / AUTHORITY_REL, repo), identity(repo / VECTORS_REL, repo)
    state = state_from(repo, snapshot, events, previous, authority_record, vector_record, sha(snapshot_data))
    state_data, markdown = document(encode_state(state)), render(state)
    need(sha(markdown) == state["publication"]["markdownSha256"], "Markdown digest is inconsistent")
    return state, state_data, markdown, snapshot_data


def validator(repo: Path, arguments: list[str]) -> None:
    interpreter = [sys.executable]
    if sys.flags.no_site:
        interpreter.append("-S")
    run = subprocess.run([*interpreter, "-B", str(repo / VALIDATOR_REL), *arguments], text=True, encoding="utf-8", capture_output=True, check=False)
    need(run.returncode == 0, f"independent validation failed: {(run.stderr or run.stdout).strip()}")


def fault(stage: str) -> None:
    crash_stages = {value.strip() for value in os.environ.get("MGIF_LEDGER_CRASH_STAGE", "").split(",") if value.strip()}
    fault_stages = {value.strip() for value in os.environ.get("MGIF_LEDGER_FAULT_STAGE", "").split(",") if value.strip()}
    if stage in crash_stages:
        os._exit(97)
    if stage in fault_stages:
        raise LedgerError(f"injected publication fault at {stage}")


def preimage(path: Path) -> dict[str, Any]:
    if not path.exists():
        return {"exists": False, "sha256": None, "base64": None}
    data = pinned(path, f"preimage {path}", False)
    return {"exists": True, "sha256": sha(data), "base64": base64.b64encode(data).decode("ascii")}


def current_hash(path: Path) -> str | None:
    if not os.path.lexists(path):
        return None
    no_reparse(path, f"publication path {path}")
    need(path.is_file(), f"publication path is not a regular file: {path}")
    return sha(pinned(path, f"publication path {path}"))


def verify_preimage(path: Path, value: dict[str, Any], label_text: str) -> None:
    need(path.is_file() == value["exists"] and current_hash(path) == value["sha256"], f"{label_text} preimage drift")


def restore(path: Path, old: dict[str, Any], expected: str | None, stage: str) -> None:
    current = current_hash(path)
    need(current in {old["sha256"], expected}, f"rollback refuses concurrent edit at {path}")
    if current == old["sha256"]:
        return
    fault(stage)
    if old["exists"]:
        atomic_write(path, base64.b64decode(old["base64"]))
    else:
        path.unlink(missing_ok=True)
        fsync_dir(path.parent)


def alive(pid: int) -> bool:
    if pid <= 0:
        return False
    if pid == os.getpid():
        return True
    if os.name == "nt":
        kernel32 = ctypes.WinDLL("kernel32", use_last_error=True)
        kernel32.OpenProcess.argtypes = [ctypes.c_ulong, ctypes.c_int, ctypes.c_ulong]
        kernel32.OpenProcess.restype = ctypes.c_void_p
        kernel32.GetExitCodeProcess.argtypes = [ctypes.c_void_p, ctypes.POINTER(ctypes.c_ulong)]
        kernel32.GetExitCodeProcess.restype = ctypes.c_int
        kernel32.CloseHandle.argtypes = [ctypes.c_void_p]
        kernel32.CloseHandle.restype = ctypes.c_int
        process = kernel32.OpenProcess(0x1000, False, pid)  # PROCESS_QUERY_LIMITED_INFORMATION
        if not process:
            return ctypes.get_last_error() == 5  # Access denied still proves a live owner.
        try:
            exit_code = ctypes.c_ulong()
            if not kernel32.GetExitCodeProcess(process, ctypes.byref(exit_code)):
                return True  # Fail closed when liveness cannot be established.
            return exit_code.value == 259  # STILL_ACTIVE
        finally:
            kernel32.CloseHandle(process)
    try:
        os.kill(pid, 0)
        return True
    except PermissionError:
        return True
    except OSError:
        return False


def acquire_lock(repo: Path) -> tuple[Path, str]:
    path, token = repo / LOCK_REL, uuid.uuid4().hex
    no_reparse(path.parent, "writer lock parent")
    data = document({"createdAtUtc": now(), "pid": os.getpid(), "token": token})
    for _ in range(2):
        try:
            fd = os.open(path, os.O_CREAT | os.O_EXCL | os.O_WRONLY | getattr(os, "O_BINARY", 0), 0o600)
        except FileExistsError:
            existing_data = pinned(path, "writer lock", False)
            try:
                existing = json_bytes(existing_data, "writer lock")
                created = epoch(utc(existing.get("createdAtUtc"), "lock creation"))
                stale = created <= time.time() + 1 and not alive(int(existing.get("pid", -1)))
            except (LedgerError, ValueError, TypeError):
                stale = False
            need(stale and current_hash(path) == sha(existing_data), "writer lock is active or unverifiable")
            path.unlink()
            fsync_dir(path.parent)
            continue
        with os.fdopen(fd, "wb") as stream:
            stream.write(data); stream.flush(); os.fsync(stream.fileno())
        fsync_dir(path.parent)
        return path, token
    raise LedgerError("could not acquire writer lock")


def release_lock(path: Path, token: str) -> None:
    fault("lock-cleanup")
    value = load(path, "writer lock")
    need(value.get("token") == token and value.get("pid") == os.getpid(), "writer lock ownership changed")
    path.unlink(); fsync_dir(path.parent)


def verify_lock(path: Path, token: str) -> None:
    value = load(path, "writer lock")
    need(value.get("token") == token and value.get("pid") == os.getpid(), "writer lock ownership changed")


def release_owned_lock_after_commit(path: Path, token: str) -> None:
    """Best-effort post-commit cleanup; a dead owner is reclaimable immediately."""
    for _ in range(3):
        try:
            value = load(path, "writer lock")
            if value.get("token") != token or value.get("pid") != os.getpid():
                return
            path.unlink()
            fsync_dir(path.parent)
            return
        except (LedgerError, OSError, ValueError):
            time.sleep(0.02)


def recover(repo: Path) -> None:
    # Recovery runs under the writer lock. Atomic-write remnants from a dead
    # owner are never authoritative and can be removed before journal replay.
    for target in (repo / JOURNAL_REL, repo / POINTER_REL, repo / STATE_REL, repo / LEDGER_REL):
        prefix = f".{target.name}."
        if not target.parent.exists():
            continue
        no_reparse(target.parent, f"atomic recovery parent for {target}")
        for orphan in target.parent.iterdir():
            if orphan.name.startswith(prefix) and orphan.name.endswith(".tmp"):
                no_reparse(orphan, "orphan atomic-write stage")
                need(orphan.is_file(), f"orphan atomic-write stage is not a file: {orphan}")
                orphan.unlink()
        fsync_dir(target.parent)
    bundle_parent = repo / BUNDLES_REL
    if bundle_parent.exists():
        no_reparse(bundle_parent, "immutable bundle store")
        for orphan in bundle_parent.iterdir():
            if orphan.name.startswith(".") and orphan.name.endswith(".tmp"):
                no_reparse(orphan, "orphan bundle stage")
                need(orphan.is_dir(), f"orphan bundle stage is not a directory: {orphan}")
                shutil.rmtree(orphan)
        fsync_dir(bundle_parent)
    journal_path = repo / JOURNAL_REL
    if not journal_path.exists():
        return
    journal = load(journal_path, "publication journal")
    version = journal.get("schemaVersion") if isinstance(journal, dict) else None
    keys = {"documentKind", "schemaVersion", "createdAtUtc", "transactionId", "preimages", "newHashes"}
    if version == 3:
        keys.add("commitPoint")
    journal = closed(journal, keys, "publication journal")
    need(journal["documentKind"] == "progress-ledger-publication-journal" and version in {2, 3} and
         isinstance(journal["transactionId"], str) and journal["transactionId"] and
         utc(journal["createdAtUtc"], "journal creation"), "publication journal is invalid")
    if version == 3:
        need(journal["commitPoint"] == "authority-pointer-replace", "publication journal commit point is invalid")
    paths = {"pointer": repo / POINTER_REL, "state": repo / STATE_REL, "ledger": repo / LEDGER_REL}
    need(set(journal["preimages"]) == set(paths) == set(journal["newHashes"]), "publication journal path set is invalid")
    for name, path in paths.items():
        old = closed(journal["preimages"][name], {"exists", "sha256", "base64"}, f"journal {name} preimage")
        new = journal["newHashes"][name]
        need(isinstance(old["exists"], bool) and SHA_RE.fullmatch(new or ""), f"journal {name} identity is invalid")
        if old["exists"]:
            try:
                old_data = base64.b64decode(old["base64"], validate=True)
            except (ValueError, TypeError) as exc:
                raise LedgerError(f"journal {name} preimage encoding is invalid") from exc
            need(sha(old_data) == old["sha256"], f"journal {name} preimage digest differs")
        else:
            need(old["sha256"] is None and old["base64"] is None, f"journal {name} absent preimage is invalid")
        need(current_hash(path) in {old["sha256"], new}, f"recovery found concurrent edit at {path}")

    # Version 3 defines the successful pointer replacement as the sole commit
    # point. A surviving journal after that point is cleanup debt, not a reason
    # to roll back an independently valid selected bundle.
    committed = version == 3 and current_hash(paths["pointer"]) == journal["newHashes"]["pointer"]
    if committed:
        need(all(current_hash(paths[name]) == journal["newHashes"][name] for name in ("state", "ledger")),
             "committed publication mirrors differ from the journal")
        validator(repo, ["--repo-root", str(repo), "--check"])
        try:
            journal_path.unlink()
        except FileNotFoundError:
            pass
        except OSError:
            # A surviving committed journal must block the next transaction;
            # recovery can retry its removal without questioning the pointer.
            raise
        try:
            fsync_dir(journal_path.parent)
        except OSError:
            # Once unlink succeeded, replay is harmless: a resurrected journal
            # still proves the same already-committed pointer and mirrors.
            pass
        return

    for name, path in paths.items():
        old = journal["preimages"][name]
        if old["exists"]:
            atomic_write(path, base64.b64decode(old["base64"]))
        else:
            path.unlink(missing_ok=True); fsync_dir(path.parent)
    journal_path.unlink(); fsync_dir(journal_path.parent)


def install_evidence(stage: Path, repo: Path) -> None:
    source, target = stage / "evidence/sha256", repo / EVIDENCE_REL
    target.mkdir(parents=True, exist_ok=True)
    no_reparse(target, "durable evidence store")
    for path in source.iterdir():
        data = pinned(path, "staged evidence", False)
        need(path.name == sha(data), "staged evidence name differs from digest")
        destination = target / path.name
        if destination.exists():
            need(pinned(destination, "existing evidence") == data, "content-address collision")
        else:
            atomic_write(destination, data)


def make_bundle(repo: Path, state: bytes, markdown: bytes, snapshot: bytes) -> tuple[str, Path]:
    base = {"documentKind": "progress-ledger-bundle", "schemaVersion": 2, "stateSha256": sha(state), "markdownSha256": sha(markdown), "snapshotSha256": sha(snapshot)}
    bundle_id = sha(payload(base))
    descriptor = document({"bundleId": bundle_id, **base})
    target = repo / BUNDLES_REL / bundle_id
    if os.path.lexists(target):
        no_reparse(target, "existing immutable bundle")
        need(target.is_dir() and pinned(target / "bundle.json", "existing bundle descriptor") == descriptor and pinned(target / "state.json", "existing bundle state") == state and pinned(target / "progress.md", "existing bundle Markdown") == markdown and pinned(target / "snapshot.json", "existing bundle snapshot") == snapshot, "immutable bundle collision")
        return bundle_id, target
    target.parent.mkdir(parents=True, exist_ok=True)
    no_reparse(target.parent, "immutable bundle store")
    temporary = target.parent / f".{bundle_id}.{uuid.uuid4().hex}.tmp"
    temporary.mkdir()
    try:
        atomic_write(temporary / "state.json", state, "bundle-state"); fault("bundle-first-write")
        atomic_write(temporary / "progress.md", markdown, "bundle-markdown"); fault("bundle-second-write")
        atomic_write(temporary / "snapshot.json", snapshot, "bundle-snapshot")
        atomic_write(temporary / "bundle.json", descriptor, "bundle-descriptor")
        fault("bundle-directory-fsync")
        fsync_dir(temporary)
        fault("bundle-directory-post-fsync")
        fault("bundle-directory-replace")
        os.replace(temporary, target)
        fault("bundle-directory-post-replace")
        fsync_dir(target.parent)
        fault("bundle-parent-fsync")
    finally:
        if temporary.exists():
            shutil.rmtree(temporary)
    return bundle_id, target


def publish(repo: Path, stage: Path, state: bytes, markdown: bytes, snapshot: bytes, sources: dict[Path, str]) -> str:
    lock_path, token = acquire_lock(repo)
    committed = False
    bundle_id: str | None = None
    journal_path = repo / JOURNAL_REL
    paths = {"pointer": repo / POINTER_REL, "state": repo / STATE_REL, "ledger": repo / LEDGER_REL}
    old: dict[str, dict[str, Any]] = {}
    new: dict[str, str] = {}
    journal_started = False
    try:
        fault("after-lock")
        recover(repo)
        old = {name: preimage(path) for name, path in paths.items()}
        for path, digest in sources.items():
            need(current_hash(path) == digest, f"source drift before publication: {path}")
        install_evidence(stage, repo)
        bundle_id, bundle_path = make_bundle(repo, state, markdown, snapshot)
        pointer_data = document({"bundleId": bundle_id, "bundlePath": bundle_path.relative_to(repo).as_posix(), "documentKind": "progress-ledger-pointer", "markdownSha256": sha(markdown), "schemaVersion": 2, "snapshotSha256": sha(snapshot), "stateSha256": sha(state)})
        new = {"pointer": sha(pointer_data), "state": sha(state), "ledger": sha(markdown)}
        journal_started = True
        atomic_write(journal_path, document({"documentKind": "progress-ledger-publication-journal", "schemaVersion": 3,
                                            "createdAtUtc": now(), "transactionId": token,
                                            "commitPoint": "authority-pointer-replace",
                                            "preimages": old, "newHashes": new}), "journal")
        fault("after-journal")
        verify_preimage(paths["state"], old["state"], "state mirror")
        atomic_write(paths["state"], state, "state"); fault("post-first-write")
        verify_preimage(paths["ledger"], old["ledger"], "Markdown mirror")
        atomic_write(paths["ledger"], markdown, "ledger"); fault("post-second-write")
        for path, digest in sources.items():
            need(current_hash(path) == digest, f"source drift during publication: {path}")
        fault("final-validation")
        fault("validation-before")
        validator(repo, ["--repo-root", str(repo), "--state", str(paths["state"]), "--ledger", str(paths["ledger"]), "--snapshot", str(bundle_path / "snapshot.json"), "--artifact-base", str(repo), "--candidate-full"])
        fault("validation-after")
        need(current_hash(paths["state"]) == new["state"] and current_hash(paths["ledger"]) == new["ledger"], "published mirror changed after final validation")
        verify_lock(lock_path, token)
        verify_preimage(paths["pointer"], old["pointer"], "authority pointer")
        pointer_stage = stage_atomic_write(paths["pointer"], pointer_data, "pointer")
        try:
            fault("pointer-replace")
            os.replace(pointer_stage, paths["pointer"])
            fault("pointer-replaced-before-bookkeeping")
            committed = True
            fault("pointer-post-replace")
            fsync_dir(paths["pointer"].parent)
            fault("pointer-parent-fsync")
        finally:
            pointer_stage.unlink(missing_ok=True)
        fault("post-commit-diagnostic")
        validator(repo, ["--repo-root", str(repo), "--check"])
        fault("post-pointer-write")
        fault("journal-cleanup")
        journal_path.unlink()
        fault("journal-post-unlink")
        fsync_dir(journal_path.parent)
        fault("journal-cleanup-parent-fsync")
        return bundle_id
    except BaseException:
        if not committed and new.get("pointer") and current_hash(paths["pointer"]) == new["pointer"]:
            need(current_hash(paths["state"]) == new.get("state") and current_hash(paths["ledger"]) == new.get("ledger"),
                 "pointer replacement completed but committed mirrors differ")
            committed = True
        if committed:
            need(bundle_id is not None, "committed publication lacks a bundle identity")
            return bundle_id
        try:
            if journal_started and journal_path.exists():
                restore(paths["pointer"], old["pointer"], new.get("pointer"), "rollback-pointer")
                restore(paths["state"], old["state"], new.get("state"), "rollback-state")
                restore(paths["ledger"], old["ledger"], new.get("ledger"), "rollback-ledger")
                journal_path.unlink(); fsync_dir(journal_path.parent)
        except BaseException as rollback_error:
            raise LedgerError(f"publication failed and durable rollback needs recovery: {rollback_error}")
        raise
    finally:
        try:
            release_lock(lock_path, token)
        except BaseException:
            if not committed:
                raise
            release_owned_lock_after_commit(lock_path, token)


def carry_ids(state: dict[str, Any] | None) -> set[str]:
    if not state:
        return set()
    result = {row["taskId"] for row in state.get("tasks", [])}
    result.update(row["gateTaskId"] for row in state.get("gateHistory", []))
    result.update(row["taskId"] for row in state.get("integrationPrerequisites", []))
    return result


def receipt_source_paths(paths: Sequence[Path], roots: Sequence[Path]) -> list[Path]:
    result = []
    for argument in paths:
        opened_receipt = safe_pinned(argument, roots, "Gate receipt")
        receipt_path = opened_receipt.path
        receipt = json_bytes(opened_receipt.data, "Gate receipt")
        result.extend([receipt_path, safe_pinned(receipt["report"]["path"], roots, "Gate report").path,
                       safe_pinned(receipt["verdictArtifact"]["path"], roots, "Gate verdict").path])
    return result


def arguments() -> argparse.Namespace:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--repo-root", type=Path, default=Path(__file__).resolve().parents[1])
    parser.add_argument("--run-id", default=RUN_ID, help=argparse.SUPPRESS)
    parser.add_argument("--snapshot", type=Path)
    parser.add_argument("--gate-receipt", type=Path, action="append", default=[])
    parser.add_argument("--evidence-root", type=Path, action="append", default=[])
    parser.add_argument("--track-task", action="append", default=[])
    parser.add_argument("--track-gate", action="append", default=[], help=argparse.SUPPRESS)
    parser.add_argument("--orca-command", nargs="+", default=["orca"])
    parser.add_argument("--capture-live", type=Path)
    parser.add_argument("--publish", action="store_true")
    parser.add_argument("--candidate-dir", type=Path)
    parser.add_argument("--check", action="store_true")
    parser.add_argument("--live-check", action="store_true")
    parser.add_argument("--shallow-integrity", action="store_true")
    parser.add_argument("--print-receipt-fingerprint", type=Path)
    return parser.parse_args()


def main() -> int:
    try:
        args = arguments()
        repo = directory(args.repo_root, "repository root")
        need(args.run_id == RUN_ID, "alternate Run IDs are prohibited")
        modes = sum(bool(value) for value in (args.capture_live, args.publish, args.candidate_dir, args.check, args.live_check, args.shallow_integrity, args.print_receipt_fingerprint))
        need(modes == 1, "select exactly one operation")
        authority, _ = protocol(repo)
        if args.check or args.live_check or args.shallow_integrity:
            command = ["--repo-root", str(repo), "--check"]
            if args.shallow_integrity:
                command.append("--shallow-integrity")
            validator(repo, command)
            if args.live_check:
                old = previous_state(repo)
                need(old is not None and old.get("schemaVersion") == 2, "live check requires v2 authority")
                live = capture(args.orca_command, carry_ids(old), authority)
                need((live["runtimeId"], live["runGeneration"], live["revisionSha256"]) == (old["snapshot"]["runtimeId"], old["snapshot"]["runGeneration"], old["snapshot"]["revisionSha256"]), "selected bundle is stale relative to live Orca")
            print("Progress ledger validation passed" + (" (explicit shallow integrity mode)" if args.shallow_integrity else " (full durable evidence mode)"))
            return 0
        old = previous_state(repo)
        roots = [directory(path, "evidence root") for path in args.evidence_root]
        need(not args.gate_receipt or roots, "Gate receipts require evidence roots")
        receipt_tasks: set[str] = set()
        for argument in args.gate_receipt:
            opened_receipt = safe_pinned(argument, roots, "Gate receipt")
            receipt_tasks.add(json_bytes(opened_receipt.data, "Gate receipt").get("gateTaskId"))
        source_hashes: dict[Path, str] = {}
        if args.publish:
            source_paths = [repo / SPEC_REL, repo / MANIFEST_REL, repo / LICENSE_REL, repo / AUTHORITY_REL, repo / VECTORS_REL, *receipt_source_paths(args.gate_receipt, roots)]
            source_hashes = {path: current_hash(path) for path in source_paths}  # type: ignore[assignment]
            need(all(value is not None for value in source_hashes.values()), "publication source is missing")
        tracked = carry_ids(old) | set(args.track_task) | receipt_tasks
        if args.capture_live:
            need(args.snapshot is None, "capture-live does not accept a caller snapshot")
            snapshot = capture(args.orca_command, tracked, authority)
            output = lexical(args.capture_live)
            need(output.parent.is_dir(), "snapshot output parent is missing")
            atomic_write(output, document(encode_snapshot(snapshot)))
            print(f"Captured stable Orca snapshot: revision={snapshot['revisionSha256']}, tasks={len(snapshot['tasks'])}, gates={len(snapshot['decisionGates'])}")
            return 0
        if args.publish:
            need(args.snapshot is None, "publication is live-only; caller snapshots are candidate inputs")
            snapshot = capture(args.orca_command, tracked, authority)
        else:
            need(args.snapshot is not None, "candidate reconstruction requires --snapshot")
            snapshot_path = lexical(args.snapshot)
            snapshot = validate_snapshot(json_bytes(pinned(snapshot_path, "sealed snapshot"), "sealed snapshot"), authority)
        if args.print_receipt_fingerprint:
            opened_draft = safe_pinned(args.print_receipt_fingerprint, roots, "receipt draft")
            draft = json_bytes(opened_draft.data, "receipt draft")
            task = next((row for row in snapshot["tasks"] if row["taskId"] == draft.get("gateTaskId")), None)
            need(task is not None, "receipt draft task is absent")
            print(fingerprint_v2(draft, task) if draft.get("schemaVersion") == 2 else fingerprint_v1(draft, task))
            return 0
        with tempfile.TemporaryDirectory(prefix="mgif-ledger-stage-") as text:
            stage = Path(text)
            state, state_data, markdown, snapshot_data = build(repo, snapshot, args.gate_receipt, roots, old, authority, stage)
            state_path, ledger_path, snapshot_path = stage / "state.json", stage / "progress.md", stage / "snapshot.json"
            atomic_write(state_path, state_data); atomic_write(ledger_path, markdown); atomic_write(snapshot_path, snapshot_data)
            validator(repo, ["--repo-root", str(repo), "--state", str(state_path), "--ledger", str(ledger_path), "--snapshot", str(snapshot_path), "--artifact-base", str(stage), "--candidate-full"])
            if args.candidate_dir:
                target = directory(args.candidate_dir, "candidate directory")
                atomic_write(target / STATE_REL.name, state_data); atomic_write(target / LEDGER_REL.name, markdown); atomic_write(target / "orca-snapshot.json", snapshot_data)
                evidence = target / "evidence/sha256"; evidence.mkdir(parents=True, exist_ok=True)
                for blob in (stage / "evidence/sha256").iterdir():
                    atomic_write(evidence / blob.name, pinned(blob, "candidate evidence", False))
                print(f"Validated deterministic candidate written to {target}")
                return 0
            bundle_id = publish(repo, stage, state_data, markdown, snapshot_data, source_hashes)
            print(f"Published bundle {bundle_id}: state={sha(state_data)}, markdown={sha(markdown)}, source={state['sourceFingerprintSha256']}")
            return 0
    except (LedgerError, OSError, UnicodeError, ValueError) as exc:
        print(f"Editor progress-ledger update failed: {exc}", file=sys.stderr)
        return 1


if __name__ == "__main__":
    raise SystemExit(main())
