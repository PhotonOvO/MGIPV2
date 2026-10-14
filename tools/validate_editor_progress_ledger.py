#!/usr/bin/env python3
"""Independent full validator for the MGIF editor progress-ledger protocol."""

from __future__ import annotations

import argparse
import ctypes
import hashlib
import importlib.util
import json
import os
import re
import stat
import sys
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
SCHEMA_REL = Path("docs/editor-parity/schemas/progress-ledger.schema.json")
SPEC_REL = Path("docs/gltf-scene-editor-parity-task.md")
MANIFEST_REL = Path("docs/editor-parity/manifest.json")
LICENSE_REL = Path("docs/editor-parity/LICENSE-SOURCES.md")
STORE_REL = Path("docs/editor-parity/progress-ledger")
AUTHORITY_REL = STORE_REL / "authority.json"
VECTORS_REL = STORE_REL / "protocol-vectors.json"
POINTER_REL = Path("docs/editor-parity/progress-ledger-current.json")
AUTHORITY_SHA256 = "4ae61046c7d5c64eefd361f66df1ecf8eccf8c59a0c8fe20a60473fb751cbbf5"
VECTORS_SHA256 = "7e27003d2cde3ac6df5341575a1422432876178d2c6cb9f10df2258251ac8097"
FIXED_POLICY = {"f42": "BLOCKED", "r24": "BLOCKED", "metal": "Not Verified", "releaseDistribution": "NOT PASS"}
PHASES = [f"P{i}" for i in range(11)]
TASK_STATES = {"pending", "ready", "dispatched", "completed", "failed", "blocked"}
DISPATCH_STATES = TASK_STATES | {"stopped"}
OUTCOMES = {"pending", "accepted", "rejected", "superseded"}
MAX_LEDGER_ROWS = 100
MATERIALIZED = [
    "P5-00", "P5-10", "P5-20", "P5-90", "G5", "O06-BIND", "P6-00", "P6-10", "P7-00",
    "P6-20", "P6-90", "G6", "P7-10", "P7-90", "G7", "P8-00", "P8-10", "P8-11",
    "P8-12", "P8-90", "G8", "P9-00", "P9-10", "P9-11", "P9-12", "P9-90", "G9",
    "P10-00", "P10-10", "P10-11", "P10-90", "P10-EXEC", "G10",
]
DEFERRED = ["L5", "L6", "L7", "L8", "L9", "L10"]
ORDER = {name: i for i, name in enumerate(MATERIALIZED)}
GATE_TASKS = {
    "P5": "task_bd6e62faa588", "P6": "task_4af12cfad98a", "P7": "task_3f216d250677",
    "P8": "task_eb85190ed1ac", "P9": "task_e6d16b19af18", "P10": "task_04e52d906959",
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


class ValidationError(RuntimeError):
    pass


def insist(condition: bool, message: str) -> None:
    if not condition:
        raise ValidationError(message)


def canonical_payload(value: Any) -> bytes:
    return json.dumps(value, ensure_ascii=True, sort_keys=True, separators=(",", ":")).encode("ascii")


def canonical_document(value: Any) -> bytes:
    return json.dumps(value, ensure_ascii=True, sort_keys=True, indent=2).encode("ascii") + b"\n"


def digest(data: bytes) -> str:
    return hashlib.sha256(data).hexdigest()


def digest_path(path: Path) -> str:
    return digest(pinned_read(path, str(path)))


def parse_bytes(data: bytes, label: str) -> Any:
    try:
        return json.loads(data.decode("utf-8"))
    except (UnicodeError, json.JSONDecodeError) as exc:
        raise ValidationError(f"cannot parse {label}: {exc}") from exc


def read_json(path: Path, label: str) -> tuple[Any, bytes]:
    try:
        data = pinned_read(path, label)
    except OSError as exc:
        raise ValidationError(f"cannot read {label}: {exc}") from exc
    return parse_bytes(data, label), data


def exact(value: Any, keys: set[str], label: str) -> dict[str, Any]:
    insist(isinstance(value, dict), f"{label} must be an object")
    actual = set(value)
    insist(actual == keys, f"{label} keys differ: missing={sorted(keys-actual)}, extra={sorted(actual-keys)}")
    return value


def normalized_time(value: Any, label: str, nullable: bool = False) -> str | None:
    if value is None and nullable:
        return None
    insist(isinstance(value, str) and value.strip(), f"{label} must be a date-time")
    text = value.strip().replace(" ", "T", 1)
    if text.endswith("Z"):
        text = text[:-1] + "+00:00"
    try:
        parsed = datetime.fromisoformat(text)
    except ValueError as exc:
        raise ValidationError(f"{label} is not ISO 8601") from exc
    if parsed.tzinfo is None:
        parsed = parsed.replace(tzinfo=timezone.utc)
    rendered = parsed.astimezone(timezone.utc).isoformat(timespec="milliseconds")
    return rendered[:-6] + "Z"


def seconds(value: str) -> float:
    return datetime.fromisoformat(value[:-1] + "+00:00").timestamp()


def phase_index(phase: str) -> int:
    return int(phase[1:])


def legal_verdicts(phase: str) -> set[str]:
    if phase == "P0":
        return {"P1 ENGINEERING READINESS: READY"}
    number = phase_index(phase)
    values = {f"GATE P{number} ENGINEERING PASS", f"GATE P{number} FAIL/BLOCKED"}
    if phase == "P10":
        values.add("GATE P10 FINAL RELEASE PASS")
    return values


def verdict_is_pass(value: str) -> bool:
    return value.endswith("ENGINEERING PASS") or value.endswith("READINESS: READY")


def lexical(path: Path) -> Path:
    return Path(os.path.abspath(os.fspath(path)))


class OpenedFile:
    __slots__ = ("path", "data", "size", "sha256", "identity")

    def __init__(self, path: Path, data: bytes, size: int, digest_value: str,
                 identity_value: tuple[int, int]) -> None:
        self.path = path
        self.data = data
        self.size = size
        self.sha256 = digest_value
        self.identity = identity_value


_SECURE_READ_TEST_HOOK: Any = None


def _secure_read_hook(stage: str, path: Path) -> None:
    if _SECURE_READ_TEST_HOOK is not None:
        _SECURE_READ_TEST_HOOK(stage, path)


class _FileTime(ctypes.Structure):
    _fields_ = [("low", wintypes.DWORD), ("high", wintypes.DWORD)]


class _FileInfo(ctypes.Structure):
    _fields_ = [
        ("attributes", wintypes.DWORD), ("created", _FileTime), ("accessed", _FileTime),
        ("written", _FileTime), ("volume", wintypes.DWORD), ("sizeHigh", wintypes.DWORD),
        ("sizeLow", wintypes.DWORD), ("links", wintypes.DWORD), ("indexHigh", wintypes.DWORD),
        ("indexLow", wintypes.DWORD),
    ]


class _WindowsUnicodeString(ctypes.Structure):
    _fields_ = [("length", wintypes.USHORT), ("maximumLength", wintypes.USHORT),
                ("buffer", wintypes.LPWSTR)]


class _WindowsObjectAttributes(ctypes.Structure):
    _fields_ = [
        ("length", wintypes.ULONG), ("rootDirectory", wintypes.HANDLE),
        ("objectName", ctypes.POINTER(_WindowsUnicodeString)), ("attributes", wintypes.ULONG),
        ("securityDescriptor", wintypes.LPVOID), ("securityQualityOfService", wintypes.LPVOID),
    ]


class _WindowsIoStatusValue(ctypes.Union):
    _fields_ = [("status", wintypes.LONG), ("pointer", wintypes.LPVOID)]


class _WindowsIoStatusBlock(ctypes.Structure):
    _anonymous_ = ("value",)
    _fields_ = [("value", _WindowsIoStatusValue), ("information", ctypes.c_size_t)]


class _WindowsApi:
    pass


def _windows_api() -> Any:
    kernel32 = ctypes.WinDLL("kernel32", use_last_error=True)
    ntdll = ctypes.WinDLL("ntdll", use_last_error=True)
    kernel32.CreateFileW.argtypes = [wintypes.LPCWSTR, wintypes.DWORD, wintypes.DWORD,
                                     wintypes.LPVOID, wintypes.DWORD, wintypes.DWORD, wintypes.HANDLE]
    kernel32.CreateFileW.restype = wintypes.HANDLE
    kernel32.GetFileInformationByHandle.argtypes = [wintypes.HANDLE, ctypes.POINTER(_FileInfo)]
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
                                   ctypes.POINTER(_WindowsObjectAttributes), ctypes.POINTER(_WindowsIoStatusBlock),
                                   wintypes.LPVOID, wintypes.ULONG, wintypes.ULONG, wintypes.ULONG,
                                   wintypes.ULONG, wintypes.LPVOID, wintypes.ULONG]
    ntdll.NtCreateFile.restype = wintypes.LONG
    ntdll.RtlNtStatusToDosError.argtypes = [wintypes.LONG]
    ntdll.RtlNtStatusToDosError.restype = wintypes.ULONG
    api = _WindowsApi()
    api._kernel32, api._ntdll = kernel32, ntdll
    for name in ("CreateFileW", "GetFileInformationByHandle", "GetFileType",
                 "GetFinalPathNameByHandleW", "ReadFile", "CloseHandle"):
        setattr(api, name, getattr(kernel32, name))
    api.NtCreateFile = ntdll.NtCreateFile
    api.RtlNtStatusToDosError = ntdll.RtlNtStatusToDosError
    return api


def _windows_open_relative(api: Any, parent: Any, name: str, expect_directory: bool,
                           label: str) -> Any:
    encoded = name.encode("utf-16-le")
    buffer = ctypes.create_unicode_buffer(name)
    unicode_name = _WindowsUnicodeString(len(encoded), len(encoded) + 2,
                                         ctypes.cast(buffer, wintypes.LPWSTR))
    attributes = _WindowsObjectAttributes(
        ctypes.sizeof(_WindowsObjectAttributes), parent, ctypes.pointer(unicode_name),
        0x00000040 | 0x00001000, None, None,  # CASE_INSENSITIVE | DONT_REPARSE
    )
    io_status = _WindowsIoStatusBlock()
    handle = wintypes.HANDLE()
    access = 0x00100080 | (0 if expect_directory else 0x00000001)
    options = 0x00200000 | 0x00000020 | (0x00000001 if expect_directory else 0x00000040)
    status = api.NtCreateFile(ctypes.byref(handle), access, ctypes.byref(attributes),
                              ctypes.byref(io_status), None, 0x00000080, 0x00000001,
                              0x00000001, options, None, 0)
    if status < 0 or not handle.value:
        error = int(api.RtlNtStatusToDosError(status)) if status < 0 else 6
        raise ValidationError(f"cannot securely open {label}: Windows error {error}")
    return handle


def _windows_identity(info: _FileInfo) -> tuple[int, int]:
    return int(info.volume), (int(info.indexHigh) << 32) | int(info.indexLow)


def _windows_verify_chain_names(api: Any, handles: Sequence[Any], parts: Sequence[str],
                                infos: Sequence[_FileInfo], label: str) -> None:
    for index in range(1, len(handles)):
        expect_directory = bool(infos[index].attributes & 0x00000010)
        verify = _windows_open_relative(api, handles[index - 1], parts[index], expect_directory,
                                        f"{label} retained-chain verification")
        try:
            current = _windows_info(api, verify, f"{label} retained-chain verification")
            insist(not (current.attributes & 0x00000400), f"{label} retained chain became a reparse point")
            insist(_windows_identity(current) == _windows_identity(infos[index]),
                   f"{label} retained chain was replaced")
        finally:
            api.CloseHandle(verify)


def _windows_info(api: Any, handle: Any, label: str) -> _FileInfo:
    value = _FileInfo()
    if not api.GetFileInformationByHandle(handle, ctypes.byref(value)):
        raise ValidationError(f"cannot inspect {label}: Windows error {ctypes.get_last_error()}")
    return value


def _windows_final_path(api: Any, handle: Any, label: str) -> Path:
    required = api.GetFinalPathNameByHandleW(handle, None, 0, 0)
    if not required:
        raise ValidationError(f"cannot resolve opened {label}: Windows error {ctypes.get_last_error()}")
    buffer = ctypes.create_unicode_buffer(required + 1)
    written = api.GetFinalPathNameByHandleW(handle, buffer, len(buffer), 0)
    if not written or written >= len(buffer):
        raise ValidationError(f"cannot resolve opened {label}: Windows error {ctypes.get_last_error()}")
    value = buffer.value
    if value.startswith("\\\\?\\UNC\\"):
        value = "\\\\" + value[8:]
    elif value.startswith("\\\\?\\"):
        value = value[4:]
    return Path(os.path.normcase(os.path.abspath(value)))


def _windows_open_chain(path: Path, label: str, final_directory: bool) -> tuple[Any, list[Any], _FileInfo, Path]:
    api = _windows_api()
    absolute = lexical(path)
    parts = absolute.parts
    insist(absolute.is_absolute() and parts, f"{label} is not absolute")
    cursor = Path(parts[0])
    components = [cursor]
    for part in parts[1:]:
        cursor /= part
        components.append(cursor)
    handles: list[Any] = []
    infos: list[_FileInfo] = []
    invalid = ctypes.c_void_p(-1).value
    try:
        for index, component in enumerate(components):
            final = index == len(components) - 1
            expect_directory = final_directory or not final
            _secure_read_hook("before-component-open", component)
            if index == 0:
                access = 0x00000080 if expect_directory else 0x80000000 | 0x00000080
                handle = api.CreateFileW(str(component), access, 0x00000001, None, 3,
                                         0x00200000 | 0x02000000, None)
                if handle == invalid:
                    raise ValidationError(f"cannot securely open {label} component {component}: Windows error {ctypes.get_last_error()}")
            else:
                handle = _windows_open_relative(api, handles[-1], parts[index], expect_directory,
                                                f"{label} component {component}")
            handles.append(handle)
            _secure_read_hook("after-component-open", component)
            info = _windows_info(api, handle, f"{label} component {component}")
            infos.append(info)
            insist(api.GetFileType(handle) == 0x0001, f"{label} component is not a disk file: {component}")
            insist(not (info.attributes & 0x00000400), f"{label} traverses a reparse point: {component}")
            insist(bool(info.attributes & 0x00000010) == expect_directory,
                    f"{label} component has the wrong file type: {component}")
            _secure_read_hook("after-component-validation", component)
        _windows_verify_chain_names(api, handles, parts, infos, label)
        return api, handles, info, _windows_final_path(api, handles[-1], label)
    except BaseException:
        for handle in reversed(handles):
            api.CloseHandle(handle)
        raise


def _windows_close(api: Any, handles: Sequence[Any]) -> None:
    for handle in reversed(handles):
        api.CloseHandle(handle)


def _windows_read(path: Path, roots: Sequence[Path], label: str, reject_hardlink: bool) -> OpenedFile:
    root_handles: list[tuple[Any, list[Any]]] = []
    api: Any = None
    handles: list[Any] = []
    try:
        api, handles, before, opened_path = _windows_open_chain(path, label, False)
        for root in roots:
            root_api, opened, _, opened_root = _windows_open_chain(root, f"{label} root", True)
            root_handles.append((root_api, opened))
            try:
                contained = os.path.commonpath([str(opened_path), str(opened_root)]) == str(opened_root)
            except ValueError:
                contained = False
            if contained:
                break
        else:
            insist(not roots, f"{label} escapes every allowed root")
        insist(not reject_hardlink or before.links == 1, f"{label} is a hard link")
        size = (int(before.sizeHigh) << 32) | int(before.sizeLow)
        identity_value = (int(before.volume), (int(before.indexHigh) << 32) | int(before.indexLow))
        _secure_read_hook("after-open", path)
        chunks: list[bytes] = []
        while True:
            buffer = ctypes.create_string_buffer(1024 * 1024)
            count = wintypes.DWORD()
            if not api.ReadFile(handles[-1], buffer, len(buffer), ctypes.byref(count), None):
                raise ValidationError(f"cannot read {label}: Windows error {ctypes.get_last_error()}")
            if count.value == 0:
                break
            chunks.append(buffer.raw[:count.value])
        _secure_read_hook("after-read", path)
        after = _windows_info(api, handles[-1], label)
        after_identity = (int(after.volume), (int(after.indexHigh) << 32) | int(after.indexLow))
        after_size = (int(after.sizeHigh) << 32) | int(after.sizeLow)
        insist(after_identity == identity_value and after_size == size and
                (after.written.high, after.written.low) == (before.written.high, before.written.low),
                f"{label} changed while read")
        insist(_windows_final_path(api, handles[-1], label) == opened_path, f"{label} moved while read")
        data = b"".join(chunks)
        insist(len(data) == size, f"{label} size changed while read")
        return OpenedFile(lexical(path), data, size, digest(data), identity_value)
    finally:
        if api is not None:
            _windows_close(api, handles)
        for root_api, opened in reversed(root_handles):
            _windows_close(root_api, opened)


def _posix_open_directory(path: Path, label: str) -> tuple[list[int], int]:
    insist(os.open in os.supports_dir_fd and hasattr(os, "O_NOFOLLOW") and hasattr(os, "O_DIRECTORY"),
           f"{label} cannot be secured: openat/no-follow directory traversal is unavailable")
    flags = os.O_RDONLY | getattr(os, "O_CLOEXEC", 0) | getattr(os, "O_DIRECTORY", 0) | getattr(os, "O_NOFOLLOW", 0)
    absolute = lexical(path)
    fds = [os.open(absolute.anchor, flags)]
    try:
        for part in absolute.parts[1:]:
            fd = os.open(part, flags, dir_fd=fds[-1])
            fds.append(fd)
            insist(stat.S_ISDIR(os.fstat(fd).st_mode), f"{label} component is not a directory: {part}")
        return fds, fds[-1]
    except BaseException:
        for fd in reversed(fds):
            os.close(fd)
        raise


def _posix_read(path: Path, roots: Sequence[Path], label: str, reject_hardlink: bool) -> OpenedFile:
    insist(hasattr(os, "O_NOFOLLOW") and hasattr(os, "O_DIRECTORY"),
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
    insist(not roots or candidates, f"{label} escapes every allowed root")
    root_path, relative = candidates[0] if candidates else (Path(absolute.anchor), Path(*absolute.parts[1:]))
    fds, parent = _posix_open_directory(root_path, f"{label} root")
    try:
        parts = relative.parts
        insist(parts, f"{label} is not a regular file")
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
        insist(stat.S_ISREG(before.st_mode), f"{label} is not regular")
        insist(not reject_hardlink or before.st_nlink == 1, f"{label} is a hard link")
        _secure_read_hook("after-open", absolute)
        chunks: list[bytes] = []
        while chunk := os.read(fd, 1024 * 1024):
            chunks.append(chunk)
        _secure_read_hook("after-read", absolute)
        after = os.fstat(fd)
        insist((before.st_dev, before.st_ino, before.st_size, before.st_mtime_ns) ==
               (after.st_dev, after.st_ino, after.st_size, after.st_mtime_ns), f"{label} changed while read")
        verify = os.open(parts[-1], file_flags, dir_fd=parent)
        try:
            current = os.fstat(verify)
            insist((current.st_dev, current.st_ino) == (before.st_dev, before.st_ino), f"{label} was replaced while read")
        finally:
            os.close(verify)
        data = b"".join(chunks)
        insist(len(data) == before.st_size, f"{label} size changed while read")
        return OpenedFile(absolute, data, before.st_size, digest(data), (before.st_dev, before.st_ino))
    finally:
        for fd in reversed(fds):
            os.close(fd)


def opened_file(path: Path, label: str, reject_hardlink: bool = True,
                roots: Sequence[Path] = ()) -> OpenedFile:
    absolute = lexical(path)
    insist(absolute.is_absolute() and ".." not in path.parts, f"{label} path is unsafe")
    try:
        return _windows_read(absolute, roots, label, reject_hardlink) if os.name == "nt" else _posix_read(absolute, roots, label, reject_hardlink)
    except OSError as exc:
        raise ValidationError(f"cannot securely read {label}: {exc}") from exc


def checked_directory(path: Path, label: str) -> Path:
    insist(".." not in path.parts, f"{label} contains traversal")
    result = lexical(path)
    if os.name == "nt":
        api, handles, _, _ = _windows_open_chain(result, label, True)
        _windows_close(api, handles)
    else:
        fds, _ = _posix_open_directory(result, label)
        for fd in reversed(fds):
            os.close(fd)
    return result


def pinned_read(path: Path, label: str, reject_hardlink: bool = True,
                roots: Sequence[Path] = ()) -> bytes:
    return opened_file(path, label, reject_hardlink, roots).data


def repository_identity(path: Path, repo: Path, name: str | None = None) -> dict[str, Any]:
    data = pinned_read(path, f"repository input {path}", False)
    row: dict[str, Any] = {"path": path.relative_to(repo).as_posix(), "bytes": len(data), "sha256": digest(data)}
    return {"name": name, **row} if name else row


def load_authority(repo: Path) -> tuple[dict[str, Any], dict[str, Any]]:
    authority_data = pinned_read(repo / AUTHORITY_REL, "authority", False)
    vector_data = pinned_read(repo / VECTORS_REL, "protocol vectors", False)
    insist(digest(authority_data) == AUTHORITY_SHA256 and digest(vector_data) == VECTORS_SHA256, "locked authority/vector digest changed")
    authority, vectors = parse_bytes(authority_data, "authority"), parse_bytes(vector_data, "vectors")
    insist(canonical_document(authority) == authority_data and canonical_document(vectors) == vector_data, "authority/vector JSON is not canonical")
    insist(authority.get("runId") == RUN_ID and authority.get("schemaVersion") == 2, "authority Run/version is invalid")
    insist(isinstance(authority.get("runtimeId"), str) and isinstance(authority.get("runGeneration"), int), "authority runtime generation is missing")
    insist({row["phase"]: row["taskId"] for row in authority.get("materializedGateTasks", [])} == GATE_TASKS, "materialized Gate authority mapping changed")
    return authority, vectors


def derive_policy(repo: Path) -> tuple[list[dict[str, Any]], dict[str, Any]]:
    spec_path, manifest_path, license_path = repo / SPEC_REL, repo / MANIFEST_REL, repo / LICENSE_REL
    spec = pinned_read(spec_path, "locked specification", False).decode("utf-8")
    manifest, _ = read_json(manifest_path, "manifest")
    license_text = pinned_read(license_path, "license provenance", False).decode("utf-8")
    gate = manifest.get("licensePolicy", {}).get("releaseGate", {})
    insist(gate.get("status") == "blocked_pending_owner_root_license_selection" and gate.get("passClaimAllowed") is False, "release policy is not blocked")
    insist("F42" in gate.get("blockedFeatureIds", []) and "R24" in gate.get("blockedRequirementIds", []), "F42/R24 policy changed")
    features = {row.get("id"): row for row in manifest.get("features", []) if isinstance(row, dict)}
    insist(features.get("F42", {}).get("mgifEvidence", {}).get("status") == "blocked", "F42 is not blocked")
    insist("Metal and Android were not run" in features.get("F40", {}).get("mgifEvidence", {}).get("notes", ""), "Metal fact changed")
    insist("F42 与 R24 必须保持 `BLOCKED`" in spec and "没有 macOS artifact 不得声称 Metal 已验证" in spec and "任何 release/distribution gate 都不得声称 PASS" in spec, "locked policy text changed")
    insist("repository root remains unlicensed and rights-reserved" in license_text, "license provenance changed")
    insist(not any((repo / name).exists() for name in ("LICENSE", "LICENSE.txt", "LICENSE.md", "COPYING", "COPYING.txt", "NOTICE", "NOTICE.txt")), "root license candidate requires policy re-audit")
    inputs = [repository_identity(spec_path, repo, "specification"), repository_identity(manifest_path, repo, "manifest"), repository_identity(license_path, repo, "licenseSources")]
    policy = {**FIXED_POLICY, "basis": {"manifestReleaseGateStatus": gate["status"], "manifestPassClaimAllowed": False, "rootLicenseSelected": False, "metalArtifactAccepted": False, "finalAcceptanceComplete": False}}
    return inputs, policy


def lifecycle_ok(task: dict[str, Any]) -> bool:
    status_value, completed = task["status"], task["completedAtUtc"]
    dispatch, worker = task["dispatch"], task["workerResult"]
    settlement = worker.get("recoverySettlement") if worker is not None else None
    if (settlement is not None) != (task.get("completionProvenance") == RECOVERY_SETTLEMENT_PROVENANCE):
        return False
    if status_value in {"pending", "ready", "blocked"}:
        return completed is None and worker is None and (dispatch is None or (dispatch["status"] == status_value and dispatch["completedAtUtc"] is None) or (dispatch["status"] == "stopped" and dispatch["completedAtUtc"] is not None))
    if status_value == "dispatched":
        return completed is None and worker is None and dispatch is not None and dispatch["status"] == "dispatched" and dispatch["completedAtUtc"] is None
    if settlement is not None:
        return (status_value == "completed" and completed is not None and dispatch is not None and
                dispatch["status"] == "failed" and dispatch["completedAtUtc"] is not None and
                dispatch["dispatchId"] == settlement["originalDispatchId"] and
                worker["messageId"] == settlement["originalMessageId"] and worker["outcome"] == "failed" and
                worker["reportPath"] == settlement["reportPath"] and settlement["taskId"] == task["taskId"] and
                settlement["settledAtUtc"] == completed and
                abs(seconds(completed)-seconds(worker["completedAtUtc"])) < 1.1 and
                seconds(completed) >= seconds(dispatch["completedAtUtc"]))
    decision_completion = status_value == "completed" and completed is not None and dispatch is None and worker is not None and worker["reportedBy"] == "decision-gate"
    if not decision_completion and (status_value not in {"completed", "failed"} or completed is None or dispatch is None or worker is None):
        return False
    expected = "succeeded" if status_value == "completed" else "failed"
    if decision_completion:
        return worker["outcome"] == expected and abs(seconds(completed)-seconds(worker["completedAtUtc"])) < 1.1
    return worker["outcome"] == expected and dispatch["status"] == status_value and dispatch["completedAtUtc"] is not None and abs(seconds(completed)-seconds(worker["completedAtUtc"])) < 1.1 and abs(seconds(completed)-seconds(dispatch["completedAtUtc"])) < 1.1


def normalize_recovery_settlement(value: dict[str, Any], task_id: str, task_completed: Any,
                                  result_payload: bytes) -> dict[str, Any]:
    settlement = exact(value, RECOVERY_SETTLEMENT_KEYS, f"task {task_id} recovery settlement")
    insist(task_id == RECOVERY_SETTLEMENT_TASK_ID, f"task {task_id} is not the authorized recovery settlement task")
    insist(settlement["provenance"] == RECOVERY_SETTLEMENT_PROVENANCE and
           settlement["objectiveCompleted"] is True and settlement["acceptanceVerdict"] == "FAIL" and
           settlement["workerOutcome"] == "failed", f"task {task_id} recovery settlement verdict is invalid")
    insist(settlement["originalMessageId"] == RECOVERY_SETTLEMENT_MESSAGE_ID and
           settlement["originalDispatchId"] == RECOVERY_SETTLEMENT_DISPATCH_ID,
           f"task {task_id} recovery settlement origin is invalid")
    insist(settlement["blockingFindings"] == RECOVERY_SETTLEMENT_FINDINGS,
           f"task {task_id} recovery settlement findings differ")
    insist(settlement["fixedPolicy"] == RECOVERY_SETTLEMENT_POLICY,
           f"task {task_id} recovery settlement fixed policy differs")
    insist(settlement["note"] == RECOVERY_SETTLEMENT_NOTE,
           f"task {task_id} recovery settlement note differs")
    for key in RECOVERY_SETTLEMENT_HASH_KEYS:
        insist(isinstance(settlement[key], str) and re.fullmatch(r"[0-9A-Fa-f]{64}", settlement[key]) is not None,
               f"task {task_id} recovery settlement {key} is invalid")
    report = settlement["reportPath"]
    insist(isinstance(report, str) and report == report.strip() and "\x00" not in report,
           f"task {task_id} recovery settlement report path is invalid")
    report_path = PureWindowsPath(report)
    try:
        relative_report = report_path.relative_to(PureWindowsPath("C:/Temp"))
    except ValueError as exc:
        raise ValidationError(f"task {task_id} recovery settlement report is outside C:\\Temp") from exc
    insist(report_path.is_absolute() and relative_report.parts and ".." not in relative_report.parts and
           report_path.suffix.lower() == ".md" and task_id in report,
           f"task {task_id} recovery settlement report path is invalid")
    settled_at = normalized_time(task_completed, f"task {task_id} recovery settlement completion")
    normalized = {
        **settlement, "taskId": task_id, "settledAtUtc": settled_at,
        **{key: settlement[key].lower() for key in RECOVERY_SETTLEMENT_HASH_KEYS},
    }
    return {
        "messageId": settlement["originalMessageId"], "outcome": "failed",
        "completedAtUtc": settled_at, "reportPath": report,
        "reportedBy": "coordinator-review-settlement",
        "subject": "coordinator-review-settlement:FAIL", "body": settlement["note"],
        "resultPayload": result_payload.decode("ascii"), "resultSha256": digest(result_payload),
        "recoverySettlement": normalized,
    }


def normalize_result_payload(data: bytes, task_id: str, task_completed: Any) -> dict[str, Any]:
    value = parse_bytes(data, f"task {task_id} result payload")
    insist(isinstance(value, dict) and canonical_payload(value) == data, f"task {task_id} result payload is not canonical")
    result_sha = digest(data)
    if value.get("provenance") == RECOVERY_SETTLEMENT_PROVENANCE:
        return normalize_recovery_settlement(value, task_id, task_completed, data)
    if value.get("provenance") != "worker_report":
        gate_id = value.get("gateId")
        insist(isinstance(gate_id, str) and gate_id and isinstance(value.get("decision"), str), f"task {task_id} non-worker result is not a decision-Gate result")
        return {"messageId": f"decision:{gate_id}", "outcome": "succeeded", "completedAtUtc": normalized_time(task_completed, f"task {task_id} decision completion"), "reportPath": None, "reportedBy": "decision-gate", "subject": f"decision-gate:{gate_id}", "body": data.decode("ascii"), "resultPayload": data.decode("ascii"), "resultSha256": result_sha}
    insist(isinstance(value.get("subject"), str) and isinstance(value.get("body"), str), f"task {task_id} worker result payload is incomplete")
    return {"messageId": value.get("messageId"), "outcome": value.get("outcome"), "completedAtUtc": normalized_time(value.get("completedAt"), f"task {task_id} result completion"), "reportPath": value.get("reportPath"), "reportedBy": value.get("reportedBy"), "subject": value["subject"], "body": value["body"], "resultPayload": data.decode("ascii"), "resultSha256": result_sha}


def snapshot_revision(value: dict[str, Any]) -> str:
    projection = {key: value[key] for key in ("runId", "runtimeId", "runGeneration", "runTaskIndex", "tasks", "decisionGates")}
    return digest(canonical_payload(projection))


_RAW_SCHEMA_VALIDATOR: Any = None


def raw_schema_validate(value: Any, label: str) -> None:
    global _RAW_SCHEMA_VALIDATOR
    if _RAW_SCHEMA_VALIDATOR is None:
        schema_path = Path(__file__).resolve().parents[1] / SCHEMA_REL
        schema = parse_bytes(pinned_read(schema_path, "progress-ledger schema", False), "progress-ledger schema")
        try:
            _RAW_SCHEMA_VALIDATOR = JsonSchemaValidator(schema)
        except JsonSchemaError as exc:
            raise ValidationError(f"progress-ledger schema is invalid at {exc.path}: {exc.message}") from exc
    try:
        _RAW_SCHEMA_VALIDATOR.validate(value)
    except JsonSchemaError as exc:
        raise ValidationError(
            f"{label} fails raw Draft 2020-12 validation at {exc.path}: {exc.message}") from exc


def decode_snapshot_wire(value: Any) -> dict[str, Any]:
    insist(isinstance(value, dict), "snapshot must be an object")
    if value.get("schemaVersion") == 2:
        return value
    if value.get("schemaVersion") == 4:
        raw_schema_validate(value, "v4 snapshot")
        keys = {"documentKind", "schemaVersion", "runId", "runtimeId", "runGeneration", "capturedAtUtc",
                "consistencyMode", "revisionSha256", "runTasks", "decisionGates"}
        wire = exact(value, keys, "v4 snapshot")
        insist(all(isinstance(wire[key], dict) for key in ("runTasks", "decisionGates")),
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
    wire = exact(value, keys, "v3 snapshot")
    insist(wire["schemaVersion"] == 3 and all(isinstance(wire[key], dict) for key in ("runTaskIndex", "tasks", "workerResults", "decisionGates")),
           "v3 snapshot identity maps are invalid")
    insist(isinstance(wire["workerMessages"], dict) and isinstance(wire["decisionGateTasks"], dict), "v3 snapshot secondary identity maps are invalid")
    message_by_task: dict[str, str] = {}
    for message_id, task_id in wire["workerMessages"].items():
        insist(isinstance(message_id, str) and isinstance(task_id, str) and task_id not in message_by_task,
               f"duplicate worker message association for task {task_id}")
        message_by_task[task_id] = message_id
    insist(set(message_by_task) == set(wire["workerResults"]), "snapshot worker result/message identities differ")
    worker_by_task = {task_id: {"messageId": message_by_task[task_id], **row} for task_id, row in wire["workerResults"].items()}
    for order_name, collection_name in (("runTaskOrder", "runTaskIndex"), ("taskOrder", "tasks"), ("decisionGateOrder", "decisionGates")):
        insist(isinstance(wire[order_name], list) and len(wire[order_name]) == len(set(wire[order_name])) and
               set(wire[order_name]) == set(wire[collection_name]), f"v3 snapshot {order_name} differs from {collection_name}")
    index = [{"taskId": task_id, **wire["runTaskIndex"][task_id]} for task_id in wire["runTaskOrder"]]
    tasks = [{"taskId": task_id, **wire["tasks"][task_id], "workerResult": worker_by_task.get(task_id)}
             for task_id in wire["taskOrder"]]
    insist(set(worker_by_task).issubset(wire["tasks"]), "snapshot worker result names an unknown task")
    gate_task_by_id = wire["decisionGateTasks"]
    insist(set(gate_task_by_id) == set(wire["decisionGates"]) and
           all(isinstance(task_id, str) and task_id for task_id in gate_task_by_id.values()),
           "snapshot decision Gate task identities differ")
    gates = [{"gateId": gate_id, "taskId": gate_task_by_id[gate_id], **wire["decisionGates"][gate_id]} for gate_id in wire["decisionGateOrder"]]
    return {**{key: item for key, item in wire.items() if key not in {"schemaVersion", "runTaskOrder", "runTaskIndex", "taskOrder", "tasks", "workerResults", "workerMessages", "decisionGateOrder", "decisionGates", "decisionGateTasks"}},
            "schemaVersion": 2, "runTaskIndex": index, "tasks": tasks, "decisionGates": gates}


def decode_gate_history_wire(value: Any) -> list[dict[str, Any]]:
    history = exact(value, {"order", "events", "messages", "dispatches", "receipts", "reports", "verdicts", "orcaResults", "resultPayloads", "supersessions", "supersessionTargets"}, "v3 Gate history")
    insist(isinstance(history["order"], list) and len(history["order"]) == len(set(history["order"])) and
           all(isinstance(history[key], dict) for key in ("events", "messages", "dispatches", "receipts", "reports", "verdicts", "orcaResults", "resultPayloads", "supersessions", "supersessionTargets")),
           "v3 Gate history identity maps are invalid")

    def association(collection: str, event_id: str) -> tuple[str, dict[str, Any]]:
        matches = [(identity_value, row) for identity_value, row in history[collection].items()
                   if isinstance(row, dict) and row.get("eventId") == event_id]
        insist(len(matches) == 1, f"Gate event {event_id} has invalid {collection} association")
        return matches[0]

    insist(set(history["order"]) == set(history["events"]), "Gate history order and event identities differ")
    events = []
    for event_id in history["order"]:
        messages = [message_id for message_id, target in history["messages"].items() if target == event_id]
        dispatches = [dispatch_id for dispatch_id, target in history["dispatches"].items() if target == event_id]
        predecessors = [previous for previous, target in history["supersessions"].items() if target == event_id]
        insist(len(messages) == 1 and len(dispatches) == 1 and len(predecessors) <= 1,
               f"Gate event {event_id} message/dispatch/supersession association is invalid")
        insist((not predecessors and event_id not in history["supersessionTargets"]) or
               (len(predecessors) == 1 and history["supersessionTargets"].get(event_id) == predecessors[0]),
               f"Gate event {event_id} supersession indexes disagree")
        receipt_id, receipt = association("receipts", event_id)
        report_id, report = association("reports", event_id)
        verdict_id, verdict = association("verdicts", event_id)
        result_id, result = association("orcaResults", event_id)
        result_payloads = [result_id for result_id, target in history["resultPayloads"].items() if target == event_id]
        insist(len(result_payloads) == 1, f"Gate event {event_id} result payload association is invalid")
        events.append({"eventId": event_id, **history["events"][event_id], "dispatchId": dispatches[0], "workerMessageId": messages[0],
                       "supersedesEventId": predecessors[0] if predecessors else None,
                       "receipt": {"path": receipt["path"], "bytes": receipt["bytes"], "sha256": receipt_id},
                       "report": {"path": report["path"], "bytes": report["bytes"], "sha256": report_id},
                       "verdictArtifact": {"path": verdict["path"], "bytes": verdict["bytes"], "sha256": verdict_id},
                       "orcaResult": {"artifact": {"path": result["path"], "bytes": result["bytes"], "sha256": result_id},
                                      "resultSha256": result_payloads[0],
                                      **{key: item for key, item in result.items() if key not in {"eventId", "path", "bytes"}}}})
    return events


def decode_decision_history_wire(value: Any) -> list[dict[str, Any]]:
    history = exact(value, {"order", "transitions", "predecessors", "successors"}, "v3 decision history")
    insist(isinstance(history["order"], list) and isinstance(history["transitions"], dict) and isinstance(history["predecessors"], dict) and
           len(history["order"]) == len(set(history["order"])) and set(history["order"]) == set(history["transitions"]),
           "v3 decision history identities are invalid")
    insist(isinstance(history["successors"], dict), "v3 decision successor identities are invalid")
    result = []
    for transition_id in history["order"]:
        previous = [identity_value for identity_value, target in history["predecessors"].items() if target == transition_id]
        insist(len(previous) <= 1 and ((not previous and transition_id not in history["successors"]) or
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


def decode_gate_table_wire(value: Any) -> list[dict[str, Any]]:
    table = exact(value, set(GATE_TABLE_COLUMNS), "v4 Gate history table")
    insist(all(isinstance(table[name], list) for name in GATE_TABLE_COLUMNS),
           "v4 Gate history columns must be arrays")
    lengths = {len(table[name]) for name in GATE_TABLE_COLUMNS}
    insist(len(lengths) == 1 and next(iter(lengths)) <= MAX_LEDGER_ROWS,
           f"v4 Gate history columns differ or exceed {MAX_LEDGER_ROWS} rows")
    insist(len(table["rows"]) == len({canonical_payload(row) for row in table["rows"] if isinstance(row, dict)}),
           "v4 Gate history has duplicate or non-object rows")
    for name in ("dispatchIds", "messageIds", "receiptSha256s", "reportSha256s",
                 "verdictSha256s", "orcaArtifactSha256s", "resultSha256s"):
        insist(all(isinstance(item, str) for item in table[name]) and
               len(table[name]) == len(set(table[name])), f"v4 Gate {name} identities are invalid")
    result: list[dict[str, Any]] = []
    previous_by_phase: dict[str, str] = {}
    for index, item in enumerate(table["rows"]):
        row = exact(item, GATE_ROW_KEYS, f"v4 Gate row {index}")
        phase = row["phase"]
        receipt_id = table["receiptSha256s"][index]
        receipt_meta = exact(table["receiptMetadata"][index], {"path", "bytes"}, f"v4 Gate receipt metadata {index}")
        report_meta = exact(table["reportMetadata"][index], {"path", "bytes"}, f"v4 Gate report metadata {index}")
        verdict_meta = exact(table["verdictMetadata"][index], {"path", "bytes"}, f"v4 Gate verdict metadata {index}")
        orca_meta = exact(table["orcaResultMetadata"][index],
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


def decode_decision_rows_wire(value: Any) -> list[dict[str, Any]]:
    insist(isinstance(value, list) and 0 < len(value) <= MAX_LEDGER_ROWS and
           len(value) == len({canonical_payload(row) for row in value if isinstance(row, dict)}),
           f"v4 decision history must contain unique object rows within {MAX_LEDGER_ROWS}")
    previous_by_gate: dict[str, str] = {}
    result: list[dict[str, Any]] = []
    for index, item in enumerate(value):
        row = exact(item, DECISION_ROW_KEYS, f"v4 decision row {index}")
        gate_id = row["gateId"]
        previous_id = previous_by_gate.get(gate_id)
        transition_id = digest(canonical_payload({**row, "previousTransitionId": previous_id}))
        result.append({"transitionId": transition_id, **row, "previousTransitionId": previous_id})
        previous_by_gate[gate_id] = transition_id
    return result


def decode_state_wire(value: Any) -> dict[str, Any]:
    insist(isinstance(value, dict), "state must be an object")
    if value.get("schemaVersion") == 2:
        return value
    if value.get("schemaVersion") == 4:
        raw_schema_validate(value, "v4 state")
        keys = {"documentKind", "schemaVersion", "generator", "runId", "authority", "protocolVectors", "snapshot", "inputs",
                "sourceFingerprintSha256", "policy", "materialization", "decisionHistory", "gateHistory", "phases", "tasks", "publication"}
        wire = exact(value, keys, "v4 state")
        insist(wire["generator"] == {"name": "tools/update_editor_progress_ledger.py", "version": 4} and
               all(isinstance(wire[key], dict) for key in ("inputs", "gateHistory", "phases", "tasks")),
               "v4 state identity maps are invalid")
        materialization = wire["materialization"]
        insist(isinstance(materialization, dict) and isinstance(materialization.get("initialTasks"), dict),
               "v4 materialization is invalid")
        initial = [{"taskId": task_id, **row} for task_id, row in materialization["initialTasks"].items()]
        initial.sort(key=lambda row: ORDER.get(row.get("displayName"), 1000))
        decision_history = decode_decision_rows_wire(wire["decisionHistory"])
        latest_by_gate: dict[str, dict[str, Any]] = {}
        for transition in decision_history:
            latest_by_gate[transition["gateId"]] = transition
        gates = [
            {key: latest[key] for key in ("gateId", "taskId", "question", "options", "createdAtUtc", "status", "outcome", "resolvedAtUtc", "resolution")} |
            {"latestTransitionId": latest["transitionId"]}
            for latest in latest_by_gate.values()
        ]
        gates.sort(key=lambda row: (row["createdAtUtc"], row["gateId"]))
        events = decode_gate_table_wire(wire["gateHistory"])
        tasks = [{"taskId": task_id, **row} for task_id, row in wire["tasks"].items()]
        tasks.sort(key=task_key)
        derived_phases = independently_derive_phases(tasks, events, gates)
        insist(set(wire["phases"]) == set(PHASES), "v4 phase identities are invalid")
        phases = []
        for derived in derived_phases:
            phase = derived["phase"]
            metadata = exact(wire["phases"][phase], {"taskIds", "summary"}, f"v4 phase {phase}")
            insist(metadata == {key: derived[key] for key in ("taskIds", "summary")},
                   f"v4 phase {phase} metadata is not derivable")
            phases.append(derived)
        inputs = [{"name": name, **wire["inputs"][name]} for name in ("specification", "manifest", "licenseSources") if name in wire["inputs"]]
        insist(len(inputs) == 3 and len(wire["inputs"]) == 3, "v4 state input identities are invalid")
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
    wire = exact(value, keys, "v3 state")
    insist(wire["schemaVersion"] == 3 and wire["generator"] == {"name": "tools/update_editor_progress_ledger.py", "version": 3} and
           all(isinstance(wire[key], dict) for key in ("inputs", "integrationPrerequisites", "integrationPrerequisiteTasks", "phases", "tasks", "workerMessages")),
           "v3 state identity maps are invalid")
    message_by_task: dict[str, str] = {}
    for message_id, task_id in wire["workerMessages"].items():
        insist(isinstance(message_id, str) and isinstance(task_id, str) and task_id not in message_by_task,
               f"duplicate state message association for {task_id}")
        message_by_task[task_id] = message_id
    insist(isinstance(wire["taskOrder"], list) and len(wire["taskOrder"]) == len(set(wire["taskOrder"])) and
           set(wire["taskOrder"]) == set(wire["tasks"]), "v3 state task order differs from task identities")
    tasks = [{"taskId": task_id, **wire["tasks"][task_id], "workerMessageId": message_by_task.get(task_id)} for task_id in wire["taskOrder"]]
    insist(set(message_by_task).issubset(wire["tasks"]), "state message names an unknown task")
    materialization = wire["materialization"]
    insist(isinstance(materialization, dict) and isinstance(materialization.get("initialTasks"), dict), "v3 materialization is invalid")
    initial = [{"taskId": task_id, **row} for task_id, row in materialization["initialTasks"].items()]
    initial.sort(key=lambda row: ORDER.get(row.get("displayName"), 1000))
    gate_tasks = wire["integrationPrerequisiteTasks"]
    insist(set(gate_tasks) == set(wire["integrationPrerequisites"]) and
           all(isinstance(task_id, str) and task_id for task_id in gate_tasks.values()),
           "v3 integration prerequisite task identities differ")
    gates = [{"gateId": gate_id, "taskId": gate_tasks[gate_id], **row} for gate_id, row in wire["integrationPrerequisites"].items()]
    gates.sort(key=lambda row: (row.get("createdAtUtc", ""), row["gateId"]))
    inputs = [{"name": name, **wire["inputs"][name]} for name in ("specification", "manifest", "licenseSources") if name in wire["inputs"]]
    insist(len(inputs) == 3 and len(wire["inputs"]) == 3, "v3 state input identities are invalid")
    phases = [{"phase": phase, **wire["phases"][phase]} for phase in PHASES if phase in wire["phases"]]
    insist(len(phases) == len(PHASES) and len(wire["phases"]) == len(PHASES), "v3 phase identities are invalid")
    return {
        **{key: item for key, item in wire.items() if key not in {"schemaVersion", "generator", "inputs", "materialization", "integrationPrerequisites", "integrationPrerequisiteTasks", "decisionGateHistory", "gateHistory", "phases", "taskOrder", "tasks", "workerMessages"}},
        "schemaVersion": 2, "generator": {"name": "tools/update_editor_progress_ledger.py", "version": 2}, "inputs": inputs,
        "materialization": {**{key: item for key, item in materialization.items() if key != "initialTasks"}, "initialTasks": initial},
        "integrationPrerequisites": gates, "decisionGateHistory": decode_decision_history_wire(wire["decisionGateHistory"]),
        "gateHistory": decode_gate_history_wire(wire["gateHistory"]), "phases": phases, "tasks": tasks,
    }


def validate_snapshot(value: Any, authority: dict[str, Any] | None = None) -> dict[str, Any]:
    value = decode_snapshot_wire(value)
    keys = {"documentKind", "schemaVersion", "runId", "runtimeId", "runGeneration", "capturedAtUtc", "consistencyMode", "revisionSha256", "runTaskIndex", "tasks", "decisionGates"}
    snapshot = exact(value, keys, "snapshot")
    insist(snapshot["documentKind"] == "orca-progress-snapshot" and snapshot["schemaVersion"] == 2 and snapshot["runId"] == RUN_ID, "snapshot kind/version/Run is invalid")
    insist(isinstance(snapshot["runtimeId"], str) and snapshot["runtimeId"] and isinstance(snapshot["runGeneration"], int) and snapshot["runGeneration"] > 0, "snapshot runtime identity is invalid")
    if authority is not None:
        insist((snapshot["runtimeId"], snapshot["runGeneration"]) == (authority["runtimeId"], authority["runGeneration"]), "snapshot runtime/generation differs from immutable authority")
    insist(snapshot["consistencyMode"] == "stable-two-pass-derived-revision", "snapshot consistency mode is not stable two-pass")
    snapshot["capturedAtUtc"] = normalized_time(snapshot["capturedAtUtc"], "snapshot capture")
    for name in ("runTaskIndex", "tasks", "decisionGates"):
        insist(isinstance(snapshot[name], list), f"snapshot.{name} is not an array")
    index: dict[str, dict[str, Any]] = {}
    order: tuple[str, str] | None = None
    observed = [snapshot["capturedAtUtc"]]
    for row in snapshot["runTaskIndex"]:
        row = exact(row, {"taskId", "displayName", "status", "createdAtUtc"}, "task index row")
        insist(row["taskId"] not in index and row["status"] in TASK_STATES and isinstance(row["displayName"], str) and row["displayName"], "task index row is invalid")
        row["createdAtUtc"] = normalized_time(row["createdAtUtc"], "index creation")
        key = (row["createdAtUtc"], row["taskId"])
        insist(order is None or order <= key, "task index order is invalid")
        order, index[row["taskId"]] = key, row
        observed.append(row["createdAtUtc"])
    tracked: dict[str, dict[str, Any]] = {}
    for row in snapshot["tasks"]:
        task_keys = {"taskId", "displayName", "status", "dependencies", "createdAtUtc", "completedAtUtc", "dispatch", "workerResult"}
        if row.get("completionProvenance") == RECOVERY_SETTLEMENT_PROVENANCE:
            task_keys.add("completionProvenance")
        row = exact(row, task_keys, "tracked task")
        task_id = row["taskId"]
        insist(task_id not in tracked and task_id in index, f"tracked task {task_id} identity is invalid")
        indexed = index[task_id]
        insist((row["displayName"], row["status"], row["createdAtUtc"]) == (indexed["displayName"], indexed["status"], indexed["createdAtUtc"]), f"tracked task {task_id} contradicts index")
        insist(isinstance(row["dependencies"], list) and row["dependencies"] == sorted(row["dependencies"]) and len(row["dependencies"]) == len(set(row["dependencies"])) and task_id not in row["dependencies"] and all(dep in index for dep in row["dependencies"]), f"task {task_id} dependencies are invalid")
        row["completedAtUtc"] = normalized_time(row["completedAtUtc"], "task completion", True)
        if row["dispatch"] is not None:
            dispatch = exact(row["dispatch"], {"dispatchId", "status", "dispatchedAtUtc", "completedAtUtc"}, "dispatch")
            insist(dispatch["status"] in DISPATCH_STATES, "dispatch status is invalid")
            dispatch["dispatchedAtUtc"] = normalized_time(dispatch["dispatchedAtUtc"], "dispatch start")
            dispatch["completedAtUtc"] = normalized_time(dispatch["completedAtUtc"], "dispatch completion", True)
            insist(seconds(dispatch["dispatchedAtUtc"]) >= seconds(row["createdAtUtc"]), f"task {task_id} dispatch predates task")
            observed.append(dispatch["dispatchedAtUtc"])
            if dispatch["completedAtUtc"]:
                observed.append(dispatch["completedAtUtc"])
        if row["workerResult"] is not None:
            worker_keys = {"messageId", "outcome", "completedAtUtc", "reportPath", "reportedBy", "subject", "body", "resultPayload", "resultSha256"}
            if row["workerResult"].get("reportedBy") == "coordinator-review-settlement":
                worker_keys.add("recoverySettlement")
            worker = exact(row["workerResult"], worker_keys, "worker result")
            insist(worker["outcome"] in {"succeeded", "failed"} and SHA_RE.fullmatch(worker.get("resultSha256", "")), "worker result identity is invalid")
            worker["completedAtUtc"] = normalized_time(worker["completedAtUtc"], "worker completion")
            insist(all(isinstance(worker[key], str) and worker[key] for key in ("messageId", "reportedBy", "subject", "body", "resultPayload")) and (worker["reportPath"] is None or isinstance(worker["reportPath"], str)), "worker result fields are invalid")
            insist(normalize_result_payload(worker["resultPayload"].encode("ascii"), task_id, row["completedAtUtc"]) == worker, f"task {task_id} result payload projection differs")
            observed.append(worker["completedAtUtc"])
        insist(lifecycle_ok(row), f"task {task_id} lifecycle is contradictory")
        if row["completedAtUtc"]:
            insist(seconds(row["completedAtUtc"]) >= seconds(row["createdAtUtc"]), f"task {task_id} completion predates creation")
            observed.append(row["completedAtUtc"])
        if row["dispatch"] and row["dispatch"]["completedAtUtc"]:
            insist(seconds(row["dispatch"]["completedAtUtc"]) >= seconds(row["dispatch"]["dispatchedAtUtc"]), f"task {task_id} dispatch completion predates dispatch")
        tracked[task_id] = row
    gates: set[str] = set()
    gate_order: tuple[str, str] | None = None
    for gate in snapshot["decisionGates"]:
        gate = exact(gate, {"gateId", "taskId", "status", "outcome", "question", "options", "createdAtUtc", "resolvedAtUtc", "resolution"}, "decision Gate")
        insist(gate["gateId"] not in gates and gate["taskId"] in tracked and gate["status"] in {"pending", "resolved"} and gate["outcome"] in OUTCOMES, f"decision Gate {gate['gateId']} identity/state is invalid")
        insist(isinstance(gate["options"], list) and len(gate["options"]) == len(set(gate["options"])), f"decision Gate {gate['gateId']} options are invalid")
        gate["createdAtUtc"] = normalized_time(gate["createdAtUtc"], "Gate creation")
        gate["resolvedAtUtc"] = normalized_time(gate["resolvedAtUtc"], "Gate resolution", True)
        if gate["status"] == "pending":
            insist(gate["outcome"] == "pending" and gate["resolvedAtUtc"] is None and gate["resolution"] is None, f"pending Gate {gate['gateId']} has terminal fields")
        else:
            insist(gate["outcome"] != "pending" and gate["resolvedAtUtc"] is not None and isinstance(gate["resolution"], str), f"resolved Gate {gate['gateId']} lacks a closed outcome")
            observed.append(gate["resolvedAtUtc"])
        key = (gate["createdAtUtc"], gate["gateId"])
        insist(gate_order is None or gate_order <= key, "decision Gate order is invalid")
        gate_order, gates = key, gates | {gate["gateId"]}
        observed.append(gate["createdAtUtc"])
    gates_by_id = {row["gateId"]: row for row in snapshot["decisionGates"]}
    for task in tracked.values():
        worker = task["workerResult"]
        if worker is None or worker["reportedBy"] != "decision-gate":
            continue
        decision = parse_bytes(worker["body"].encode("ascii"), f"decision result {task['taskId']}")
        gate = gates_by_id.get(decision.get("gateId")) if isinstance(decision, dict) else None
        insist(gate is not None and gate["taskId"] == task["taskId"] and gate["status"] == "resolved" and decision.get("decision") == gate["resolution"], f"task {task['taskId']} synthetic decision result is not bound to its live Gate")
        insist(worker["messageId"] == f"decision:{gate['gateId']}" and worker["resultSha256"] == digest(canonical_payload(decision)), f"task {task['taskId']} synthetic decision result identity differs")
    for name in MATERIALIZED:
        rows = [row for row in snapshot["runTaskIndex"] if row["displayName"] == name]
        insist(len(rows) == 1 and rows[0]["taskId"] in tracked, f"materialized task {name} is absent or ambiguous")
    insist(all(seconds(snapshot["capturedAtUtc"]) >= seconds(value) for value in observed), "snapshot capture timestamp predates content")
    insist(snapshot["revisionSha256"] == snapshot_revision(snapshot), "snapshot revision does not recompute")
    return snapshot


def evaluate_vectors(authority: dict[str, Any], vectors: dict[str, Any]) -> None:
    bootstrap = {(row["phase"], row["taskId"]) for row in authority["bootstrapEvents"]}
    gates = {(row["phase"], row["taskId"]) for row in authority["materializedGateTasks"]}
    policies = {row["gateId"]: row for row in authority["decisionGatePolicies"]}
    ids: set[str] = set()
    for expected, collection in ((True, vectors.get("accepted", [])), (False, vectors.get("rejected", []))):
        for row in collection:
            case_id, kind = row.get("caseId"), row.get("kind")
            insist(isinstance(case_id, str) and case_id not in ids, "protocol vector case IDs are invalid")
            insist(row.get("expected") == ("accepted" if expected else "rejected"), f"protocol vector {case_id} has a contradictory declared outcome")
            ids.add(case_id)
            if kind == "gateAuthority":
                actual = (row.get("phase"), row.get("taskId")) in bootstrap | gates
            elif kind == "phaseVerdict":
                phase, verdict, scope = row.get("phase"), row.get("gateVerdict"), row.get("gateScope")
                actual = phase in PHASES and verdict in legal_verdicts(phase) and scope == ("final-release" if str(verdict).endswith("FINAL RELEASE PASS") else "engineering") and not str(verdict).endswith("FINAL RELEASE PASS")
            elif kind == "decisionOutcome":
                policy = policies.get(row.get("gateId"))
                actual = policy is not None and row.get("resolution") in policy.get("acceptedResolutions", [])
            elif kind == "runIdentity":
                actual = row.get("runId") == RUN_ID
            elif kind == "lifecycle":
                state_value = row.get("taskStatus")
                if state_value in {"pending", "ready", "blocked"}:
                    actual = not row.get("hasCompletion") and not row.get("hasWorkerResult") and (not row.get("hasDispatch") or row.get("dispatchStatus") in {"pending", "ready", "blocked", "stopped"})
                elif state_value == "dispatched":
                    actual = not row.get("hasCompletion") and not row.get("hasWorkerResult") and row.get("hasDispatch") and row.get("dispatchStatus") == "dispatched"
                else:
                    outcome = "succeeded" if state_value == "completed" else "failed"
                    actual = state_value in {"completed", "failed"} and row.get("hasCompletion") and row.get("hasWorkerResult") and row.get("hasDispatch") and row.get("dispatchStatus") == state_value and row.get("workerOutcome") == outcome
            else:
                raise ValidationError(f"unknown protocol vector kind {kind}")
            insist(actual is expected, f"protocol vector {case_id} produced the wrong trust decision")


def phase_label(display_name: str) -> str:
    if display_name in ORDER:
        return display_name
    for pattern, fixed in ((r"^P5-00-R1\b", "P5-00-R1"), (r"^P5-90-R1\b", "P5-90-R1"), (r"^P5-R2-([ABC])\b", None), (r"^P5-90-R2-([DEF])\b", None), (r"^P5-R3\b", "P5-R3")):
        match = re.match(pattern, display_name)
        if match:
            return fixed or f"P5-R2-{match.group(1)}"
    return display_name


def phase_of(label: str) -> str | None:
    match = re.match(r"^[PG](10|[0-9])(?:\b|-)", label)
    return f"P{match.group(1)}" if match else None


def projected_task(row: dict[str, Any]) -> dict[str, Any]:
    dispatch, worker = row["dispatch"], row["workerResult"]
    result = {"taskId": row["taskId"], "label": phase_label(row["displayName"]), "displayName": row["displayName"], "status": row["status"], "dependencies": row["dependencies"], "completedAtUtc": row["completedAtUtc"], "dispatchId": dispatch["dispatchId"] if dispatch else None, "workerOutcome": worker["outcome"] if worker else "not_reported", "workerMessageId": worker["messageId"] if worker else None, "reportPath": worker["reportPath"] if worker else None}
    if row.get("completionProvenance") == RECOVERY_SETTLEMENT_PROVENANCE:
        result["completionProvenance"] = RECOVERY_SETTLEMENT_PROVENANCE
    return result


def task_key(row: dict[str, Any]) -> tuple[int, int, str, str]:
    match = re.match(r"^(?:G|P)(\d+)", row["label"])
    return (int(match.group(1)) if match else 99, ORDER.get(row["label"], 1000), row["label"], row["taskId"])


def artifact_path(record: Any, repo: Path, artifact_base: Path, label: str) -> tuple[dict[str, Any], bytes]:
    record = exact(record, {"path", "bytes", "sha256"}, label)
    insist(isinstance(record["bytes"], int) and record["bytes"] > 0 and SHA_RE.fullmatch(record.get("sha256", "")), f"{label} identity is invalid")
    logical = Path(record["path"])
    insist(not logical.is_absolute() and ".." not in logical.parts, f"{label} logical path is unsafe")
    if artifact_base == repo:
        path = repo / logical
    else:
        try:
            suffix = logical.relative_to(STORE_REL)
        except ValueError as exc:
            raise ValidationError(f"{label} is outside the protocol store") from exc
        path = artifact_base / suffix
    opened = opened_file(path, label, roots=(artifact_base,))
    data = opened.data
    insist(opened.size == record["bytes"] and opened.sha256 == record["sha256"], f"{label} durable content identity differs")
    insist(path.name == record["sha256"] if "evidence/sha256" in logical.as_posix() else True, f"{label} content-addressed name differs")
    return record, data


def legacy_snapshot_task(task: dict[str, Any]) -> dict[str, Any]:
    value = {key: item for key, item in task.items() if key != "workerResult"}
    if task["workerResult"] is not None:
        value["workerResult"] = {key: task["workerResult"][key] for key in ("messageId", "outcome", "completedAtUtc", "reportPath")}
    return value


def fingerprint_v1(receipt: dict[str, Any], task: dict[str, Any]) -> str:
    return digest(canonical_payload({
        "runId": receipt["runId"], "phase": receipt["phase"], "gateTask": legacy_snapshot_task(task),
        "gateVerdict": receipt["gateVerdict"], "gateScope": receipt["gateScope"],
        "supersedesGateTaskId": receipt["supersedesGateTaskId"], "report": receipt["report"],
        "verdictArtifact": receipt["verdictArtifact"], "fixedPolicy": receipt["fixedPolicy"],
    }))


def fingerprint_v2(receipt: dict[str, Any], result_hash: str) -> str:
    return digest(canonical_payload({
        "runId": receipt["runId"], "phase": receipt["phase"], "gateTaskId": receipt["gateTaskId"],
        "dispatchId": receipt["dispatchId"], "workerMessageId": receipt["workerMessageId"],
        "workerResultSha256": result_hash, "completedAtUtc": receipt["completedAtUtc"],
        "gateVerdict": receipt["gateVerdict"], "gateScope": receipt["gateScope"],
        "supersedesReceiptSha256": receipt["supersedesReceiptSha256"], "report": receipt["report"],
        "verdictArtifact": receipt["verdictArtifact"], "fixedPolicy": receipt["fixedPolicy"],
    }))


def validate_verdict(data: bytes, receipt: dict[str, Any], version: int) -> None:
    value = parse_bytes(data, "durable Gate verdict")
    supersedes = "supersedesGateTaskId" if version == 1 else "supersedesReceiptSha256"
    keys = {"documentKind", "schemaVersion", "runId", "phase", "gateTaskId", "dispatchId", "gateVerdict", "gateScope", supersedes, "fixedPolicy"}
    value = exact(value, keys, "durable Gate verdict")
    insist(value["documentKind"] == "gate-verdict" and value["schemaVersion"] == version, "Gate verdict kind/version is invalid")
    for key in keys - {"documentKind", "schemaVersion"}:
        insist(value[key] == receipt[key], f"Gate verdict field {key} differs from receipt")


def worker_attestation(body: str) -> dict[str, Any]:
    lines = [line[len(ATTESTATION_PREFIX):] for line in body.splitlines() if line.startswith(ATTESTATION_PREFIX)]
    insist(len(lines) == 1, "durable Gate result must contain exactly one structured attestation")
    try:
        value = json.loads(lines[0])
    except json.JSONDecodeError as exc:
        raise ValidationError("durable Gate worker attestation is not valid JSON") from exc
    return exact(value, {"phase", "gateVerdict", "reportSha256", "verdictSha256"}, "durable Gate worker attestation")


def validate_gate_history(state: dict[str, Any], snapshot: dict[str, Any], authority: dict[str, Any], repo: Path, artifact_base: Path) -> None:
    history = state["gateHistory"]
    insist(isinstance(history, list), "gateHistory must be an array")
    tracked = {row["taskId"]: row for row in snapshot["tasks"]}
    bootstrap = {row["receiptSha256"]: row for row in authority["bootstrapEvents"]}
    latest: dict[str, dict[str, Any]] = {}
    ids: set[str] = set()
    messages: set[str] = set()
    for event in history:
        event = exact(event, {"eventId", "authorityClass", "receipt", "inputFingerprintSha256", "phase", "gateTaskId", "dispatchId", "workerMessageId", "completedAtUtc", "gateVerdict", "gateScope", "supersedesEventId", "report", "verdictArtifact", "orcaResult"}, "Gate history event")
        insist(event["eventId"] not in ids and event["workerMessageId"] not in messages and event["phase"] in PHASES, "Gate history event is duplicated or has bad phase")
        ids.add(event["eventId"]); messages.add(event["workerMessageId"])
        _, receipt_data = artifact_path(event["receipt"], repo, artifact_base, "durable Gate receipt")
        _, report_data = artifact_path(event["report"], repo, artifact_base, "durable Gate report")
        _, verdict_data = artifact_path(event["verdictArtifact"], repo, artifact_base, "durable Gate verdict")
        insist(digest(receipt_data) == event["eventId"] and SHA_RE.fullmatch(event.get("inputFingerprintSha256", "")), "Gate event/receipt identity is invalid")
        receipt = parse_bytes(receipt_data, "durable Gate receipt")
        fixed = bootstrap.get(event["eventId"])
        version = receipt.get("schemaVersion") if isinstance(receipt, dict) else None
        if fixed:
            keys = {"documentKind", "schemaVersion", "runId", "phase", "gateTaskId", "dispatchId", "workerMessageId", "completedAtUtc", "reviewExecutionOutcome", "gateVerdict", "gateScope", "supersedesGateTaskId", "report", "verdictArtifact", "inputFingerprintSha256", "fixedPolicy"}
            insist(event["authorityClass"] == "immutable-bootstrap" and version == 1, "bootstrap event authority class/version is wrong")
        else:
            keys = {"documentKind", "schemaVersion", "runId", "phase", "gateTaskId", "dispatchId", "workerMessageId", "completedAtUtc", "reviewExecutionOutcome", "gateVerdict", "gateScope", "supersedesReceiptSha256", "report", "verdictArtifact", "inputFingerprintSha256", "fixedPolicy"}
            insist(event["authorityClass"] == "materialized-gate" and version == 2, "non-bootstrap event authority class/version is wrong")
        receipt = exact(receipt, keys, "durable Gate receipt")
        insist(receipt["documentKind"] == "gate-receipt" and receipt["runId"] == RUN_ID and receipt["phase"] in PHASES, "Gate receipt kind/Run/phase is wrong")
        insist(receipt["gateVerdict"] in legal_verdicts(receipt["phase"]) and receipt["reviewExecutionOutcome"] == "succeeded" and receipt["fixedPolicy"] == FIXED_POLICY, "Gate receipt verdict/outcome/policy is wrong")
        expected_scope = "final-release" if receipt["gateVerdict"].endswith("FINAL RELEASE PASS") else "engineering"
        insist(receipt["gateScope"] == expected_scope and expected_scope != "final-release", "Gate scope or release PASS is invalid")
        for key in ("phase", "gateTaskId", "dispatchId", "workerMessageId", "completedAtUtc", "gateVerdict", "gateScope", "inputFingerprintSha256"):
            insist(event[key] == receipt[key], f"Gate event field {key} differs from receipt")
        insist(receipt["report"]["bytes"] == len(report_data) and receipt["report"]["sha256"] == digest(report_data), "durable report differs from receipt")
        insist(receipt["verdictArtifact"]["bytes"] == len(verdict_data) and receipt["verdictArtifact"]["sha256"] == digest(verdict_data), "durable verdict differs from receipt")
        validate_verdict(verdict_data, receipt, version)
        prior = latest.get(event["phase"])
        insist(event["supersedesEventId"] == (prior["eventId"] if prior else None), "Gate history is not one immutable forward-linked chain per phase")
        result_identity = exact(event["orcaResult"], {"artifact", "resultSha256", "reportedBy", "subjectSha256", "bodySha256"}, "Gate Orca result identity")
        insist(all(SHA_RE.fullmatch(result_identity[key]) for key in ("resultSha256", "subjectSha256", "bodySha256")) and isinstance(result_identity["reportedBy"], str), "Gate Orca result identity is invalid")
        result_record, result_data = artifact_path(result_identity["artifact"], repo, artifact_base, "durable Orca Gate result")
        insist(result_record["sha256"] == result_identity["resultSha256"], "durable Orca result artifact digest differs from result identity")
        durable_result = normalize_result_payload(result_data, event["gateTaskId"], event["completedAtUtc"])
        insist(durable_result["messageId"] == receipt["workerMessageId"] and durable_result["outcome"] == "succeeded" and durable_result["completedAtUtc"] == receipt["completedAtUtc"], "durable Orca result message/outcome/time differs")
        insist(durable_result["resultSha256"] == result_identity["resultSha256"] and durable_result["reportedBy"] == result_identity["reportedBy"] and digest(durable_result["subject"].encode()) == result_identity["subjectSha256"] and digest(durable_result["body"].encode()) == result_identity["bodySha256"], "durable Orca result content identity differs")
        raw_result_path = durable_result["reportPath"] or ""
        raw_receipt_path = receipt["report"]["path"]
        if re.match(r"^[A-Za-z]:[\\/]", raw_result_path) or re.match(r"^[A-Za-z]:[\\/]", raw_receipt_path):
            result_path = PureWindowsPath(raw_result_path)
            receipt_path = PureWindowsPath(raw_receipt_path)
            insist(result_path.is_absolute() and receipt_path.is_absolute() and
                    ".." not in result_path.parts and ".." not in receipt_path.parts and
                    str(result_path).casefold() == str(receipt_path).casefold(),
                    "durable Orca result report path differs from receipt")
        else:
            result_path = Path(raw_result_path)
            receipt_path = Path(raw_receipt_path)
            insist(result_path.is_absolute() and receipt_path.is_absolute() and
                    ".." not in result_path.parts and ".." not in receipt_path.parts and
                    lexical(result_path) == lexical(receipt_path),
                    "durable Orca result report path differs from receipt")
        task = tracked.get(event["gateTaskId"])
        if fixed:
            insist(task is not None and task["workerResult"] is not None, "bootstrap task is absent from durable snapshot")
            worker = task["workerResult"]
            mapping = {"phase": "phase", "gateTaskId": "taskId", "dispatchId": "dispatchId", "workerMessageId": "workerMessageId", "completedAtUtc": "completedAtUtc", "gateVerdict": "gateVerdict", "gateScope": "gateScope"}
            insist(all(receipt[key] == fixed[value] for key, value in mapping.items()), "bootstrap receipt differs from immutable authority")
            insist(receipt["report"]["sha256"] == fixed["reportSha256"] and receipt["verdictArtifact"]["sha256"] == fixed["verdictSha256"], "bootstrap evidence digest differs")
            insist(receipt["supersedesGateTaskId"] == fixed["supersedesGateTaskId"] == (prior["gateTaskId"] if prior else None), "bootstrap task supersession differs")
            insist(worker["messageId"] == receipt["workerMessageId"] and worker["resultSha256"] == fixed["resultSha256"] and worker["reportedBy"] == fixed["reportedBy"], "bootstrap worker identity differs")
            insist(digest(worker["subject"].encode()) == fixed["subjectSha256"] and digest(worker["body"].encode()) == fixed["bodySha256"], "bootstrap worker subject/body differs")
            insist(worker == durable_result, "bootstrap live Orca result differs from its durable result artifact")
            insist(receipt["inputFingerprintSha256"] == fingerprint_v1(receipt, task), "bootstrap receipt fingerprint does not recompute")
        else:
            insist(receipt["phase"] in GATE_TASKS and receipt["gateTaskId"] == GATE_TASKS[receipt["phase"]], "ordinary or wrong-phase task issued Gate receipt")
            insist(receipt["supersedesReceiptSha256"] == event["supersedesEventId"], "v2 receipt supersession differs from event")
            insist(receipt["inputFingerprintSha256"] == fingerprint_v2(receipt, result_identity["resultSha256"]), "v2 receipt fingerprint does not recompute")
            insist(durable_result["subject"] == receipt["gateVerdict"] and worker_attestation(durable_result["body"]) == {"phase": receipt["phase"], "gateVerdict": receipt["gateVerdict"], "reportSha256": receipt["report"]["sha256"], "verdictSha256": receipt["verdictArtifact"]["sha256"]}, "durable Orca result does not attest receipt report/verdict")
            if task and task["workerResult"] and task["workerResult"]["messageId"] == receipt["workerMessageId"]:
                worker = task["workerResult"]
                insist(worker == durable_result, "latest Orca result differs from durable event")
        if prior:
            insist(seconds(event["completedAtUtc"]) > seconds(prior["completedAtUtc"]), "re-Gate completion is not strictly later than its predecessor")
        latest[event["phase"]] = event


def validate_decisions(state: dict[str, Any], snapshot: dict[str, Any], authority: dict[str, Any]) -> None:
    history, current = state["decisionGateHistory"], state["integrationPrerequisites"]
    insist(isinstance(history, list) and isinstance(current, list), "decision Gate collections are invalid")
    policies = {row["gateId"]: row for row in authority["decisionGatePolicies"]}
    latest: dict[str, dict[str, Any]] = {}
    ids: set[str] = set()
    for event in history:
        keys = {"transitionId", "gateId", "taskId", "question", "options", "createdAtUtc", "status", "outcome", "resolvedAtUtc", "resolution", "previousTransitionId", "observedAtUtc"}
        event = exact(event, keys, "decision transition")
        insist(event["transitionId"] not in ids and event["outcome"] in OUTCOMES, "decision transition ID/outcome is invalid")
        prior = latest.get(event["gateId"])
        insist(event["previousTransitionId"] == (prior["transitionId"] if prior else None), "decision transitions do not form an append-only chain")
        if prior:
            insist(all(event[key] == prior[key] for key in ("gateId", "taskId", "question", "options", "createdAtUtc")), "decision Gate immutable identity changed")
        transition = {key: event[key] for key in ("gateId", "taskId", "question", "options", "createdAtUtc", "status", "outcome", "resolvedAtUtc", "resolution", "previousTransitionId")}
        insist(event["transitionId"] == digest(canonical_payload({**transition, "observedAtUtc": event["observedAtUtc"]})), "decision transition digest does not recompute")
        if prior:
            insist(prior["status"] == "pending" and event["status"] == "resolved", "decision Gate history contains a duplicate or post-terminal transition")
            insist(seconds(event["observedAtUtc"]) >= seconds(prior["observedAtUtc"]), "decision transition observation order regressed")
        if event["status"] == "pending":
            insist(event["outcome"] == "pending" and event["resolvedAtUtc"] is None and event["resolution"] is None, "pending decision transition has terminal data")
        else:
            policy = policies.get(event["gateId"])
            insist(policy is not None and policy["ownerTaskId"] == event["taskId"], "resolved decision Gate has no authority policy")
            mapping = {resolution: outcome for outcome, field in (("accepted", "acceptedResolutions"), ("rejected", "rejectedResolutions"), ("superseded", "supersededResolutions")) for resolution in policy[field]}
            insist(mapping.get(event["resolution"]) == event["outcome"] and event["resolvedAtUtc"] is not None, "decision Gate resolution classification is wrong")
        ids.add(event["transitionId"]); latest[event["gateId"]] = event
    current_by_id = {row["gateId"]: row for row in current}
    insist(len(current_by_id) == len(current) == len(latest), "decision Gate latest projection is incomplete or duplicated")
    for gate_id, event in latest.items():
        row = current_by_id[gate_id]
        expected = {key: event[key] for key in ("gateId", "taskId", "question", "options", "createdAtUtc", "status", "outcome", "resolvedAtUtc", "resolution")} | {"latestTransitionId": event["transitionId"]}
        insist(row == expected, f"decision Gate {gate_id} latest projection differs")
    live = {row["gateId"]: row for row in snapshot["decisionGates"]}
    for gate_id, gate in live.items():
        insist(gate_id in current_by_id, f"live decision Gate {gate_id} was omitted")
        for key in ("gateId", "taskId", "question", "options", "createdAtUtc", "status", "outcome", "resolvedAtUtc", "resolution"):
            insist(current_by_id[gate_id][key] == gate[key], f"live decision Gate {gate_id} projection differs")
    for gate_id, row in current_by_id.items():
        insist(gate_id in live or row["outcome"] in {"accepted", "rejected", "superseded"}, f"unresolved prior decision Gate {gate_id} disappeared")


def independently_derive_phases(tasks: list[dict[str, Any]], events: list[dict[str, Any]], gates: list[dict[str, Any]]) -> list[dict[str, Any]]:
    histories = {phase: [] for phase in PHASES}
    phase_tasks = {phase: [] for phase in PHASES}
    for event in events:
        histories[event["phase"]].append(event)
    for task in tasks:
        phase = phase_of(task["label"])
        if phase:
            phase_tasks[phase].append(task)
    blockers = [row for row in gates if row["outcome"] in {"pending", "rejected", "superseded"}]
    result = []
    for phase in PHASES:
        history, tasks_for_phase = histories[phase], phase_tasks[phase]
        latest = history[-1] if history else None
        gate = "NOT_RUN" if latest is None else ("PASS" if verdict_is_pass(latest["gateVerdict"]) else "FAIL/BLOCKED")
        if phase == "P5" and gate == "PASS":
            insist(not blockers, "P5 PASS has a pending/negative/superseded prerequisite")
        if gate == "PASS":
            execution, summary = "GATED", f"{phase} engineering Gate is accepted; release policy remains blocked."
        elif gate == "FAIL/BLOCKED":
            execution, summary = "BLOCKED", f"{phase} Gate is FAIL/BLOCKED and retained in immutable history."
        elif phase == "P5":
            active = any(re.match(r"^P5-R\d+(?:-|$)", row["label"]) and row["status"] in {"pending", "ready", "dispatched"} for row in tasks_for_phase)
            execution, summary = ("REPAIR_ACTIVE" if active else "MATERIALIZED_PENDING"), "P5 repair remains active; G5 has no authoritative receipt."
        else:
            execution, summary = "MATERIALIZED_PENDING", f"{phase} is materialized; no authoritative Gate receipt exists."
        task_ids = [row["taskId"] for row in tasks_for_phase]
        task_ids.extend(row["gateTaskId"] for row in history if row["gateTaskId"] not in task_ids)
        result.append({"phase": phase, "executionStatus": execution, "engineeringGate": gate, "gateTaskId": latest["gateTaskId"] if latest else None, "gateHistoryReceiptSha256s": [row["eventId"] for row in history], "taskIds": task_ids, "summary": summary})
    return result


def validate_state(state: Any, snapshot: dict[str, Any], repo: Path, artifact_base: Path, authority: dict[str, Any], vectors: dict[str, Any]) -> dict[str, Any]:
    state = decode_state_wire(state)
    keys = {"documentKind", "schemaVersion", "generator", "runId", "authority", "protocolVectors", "snapshot", "inputs", "sourceFingerprintSha256", "policy", "materialization", "integrationPrerequisites", "decisionGateHistory", "gateHistory", "phases", "tasks", "publication"}
    state = exact(state, keys, "progress state")
    insist(state["documentKind"] == "progress-ledger-state" and state["schemaVersion"] == 2 and state["runId"] == RUN_ID, "state kind/version/Run is invalid")
    insist(state["generator"] == {"name": "tools/update_editor_progress_ledger.py", "version": 2}, "state generator identity is invalid")
    authority_record = repository_identity(repo / AUTHORITY_REL, repo)
    vectors_record = repository_identity(repo / VECTORS_REL, repo)
    insist(state["authority"] == authority_record and state["protocolVectors"] == vectors_record, "state authority/vector identity is stale")
    inputs, policy = derive_policy(repo)
    insist(state["inputs"] == inputs and state["policy"] == policy, "state policy or policy inputs are stale")
    snapshot_identity = exact(state["snapshot"], {"capturedAtUtc", "runtimeId", "runGeneration", "revisionSha256", "snapshotSha256", "relevantProjectionSha256", "runTaskCount", "trackedTaskCount"}, "state snapshot identity")
    insist(snapshot_identity["capturedAtUtc"] == snapshot["capturedAtUtc"] and snapshot_identity["runtimeId"] == snapshot["runtimeId"] and snapshot_identity["runGeneration"] == snapshot["runGeneration"] and snapshot_identity["revisionSha256"] == snapshot["revisionSha256"], "state snapshot identity differs")
    insist(snapshot_identity["runTaskCount"] == len(snapshot["runTaskIndex"]) and snapshot_identity["trackedTaskCount"] == len(snapshot["tasks"]), "state snapshot counts differ")
    validate_gate_history(state, snapshot, authority, repo, artifact_base)
    validate_decisions(state, snapshot, authority)
    tracked = {row["taskId"]: row for row in snapshot["tasks"]}
    materialized = state["materialization"]
    insist(materialized.get("status") == "MATERIALIZED" and materialized.get("initialTaskCount") == 33 and materialized.get("deferredLedgerDefinitions") == DEFERRED, "materialization envelope is invalid")
    initial = materialized.get("initialTasks")
    insist(isinstance(initial, list) and len(initial) == 33 and [row["displayName"] for row in initial] == MATERIALIZED, "materialized task list is invalid")
    index_by_name = {row["displayName"]: row for row in snapshot["runTaskIndex"] if row["displayName"] in ORDER}
    expected_initial = [{"taskId": index_by_name[name]["taskId"], "displayName": name, "status": tracked[index_by_name[name]["taskId"]]["status"]} for name in MATERIALIZED]
    insist(initial == expected_initial, "materialized identities/statuses differ from snapshot")
    for phase, task_id in GATE_TASKS.items():
        insist(next(row for row in initial if row["displayName"] == f"G{phase_index(phase)}")["taskId"] == task_id, f"authoritative {phase} Gate task differs")
    gate_task_ids = {row["gateTaskId"] for row in state["gateHistory"]}
    expected_ids = {row["taskId"] for row in initial} | gate_task_ids | {row["taskId"] for row in state["integrationPrerequisites"]}
    for row in snapshot["tasks"]:
        task_label = phase_label(row["displayName"])
        if task_label != row["displayName"] or task_label.startswith("P5-R2-"):
            expected_ids.add(row["taskId"])
    state_ids = [row["taskId"] for row in state["tasks"]]
    insist(len(state_ids) == len(set(state_ids)) and set(state_ids) == expected_ids, f"state task projection omits/adds relevant task identity: gateTasks={sorted(gate_task_ids)}, missing={sorted(expected_ids-set(state_ids))}, extra={sorted(set(state_ids)-expected_ids)}")
    expected_tasks = sorted((projected_task(tracked[task_id]) for task_id in expected_ids), key=task_key)
    insist(state["tasks"] == expected_tasks, "state task projection differs from snapshot")
    expected_phases = independently_derive_phases(expected_tasks, state["gateHistory"], state["integrationPrerequisites"])
    insist(state["phases"] == expected_phases, "state phase projection is not independently derivable")
    projection = {"runId": RUN_ID, "runtimeId": snapshot["runtimeId"], "runGeneration": snapshot["runGeneration"], "revisionSha256": snapshot["revisionSha256"], "materialization": initial, "tasks": expected_tasks, "decisionGateHistory": state["decisionGateHistory"], "gateEventIds": [row["eventId"] for row in state["gateHistory"]]}
    projection_hash = digest(canonical_payload(projection))
    insist(snapshot_identity["relevantProjectionSha256"] == projection_hash, "relevant projection digest does not recompute")
    source_hash = digest(canonical_payload({"schemaVersion": 2, "runId": RUN_ID, "snapshotSha256": snapshot_identity["snapshotSha256"], "relevantProjectionSha256": projection_hash, "inputs": inputs, "authoritySha256": authority_record["sha256"], "protocolVectorsSha256": vectors_record["sha256"], "gateEventIds": [row["eventId"] for row in state["gateHistory"]]}))
    insist(state["sourceFingerprintSha256"] == source_hash, "state source fingerprint does not recompute")
    publication = state["publication"]
    insist(publication == {"ledgerPath": LEDGER_REL.as_posix(), "pointerPath": POINTER_REL.as_posix(), "sourceFingerprintSha256": source_hash, "markdownSha256": publication.get("markdownSha256")}, "publication envelope is invalid")
    insist(SHA_RE.fullmatch(publication.get("markdownSha256", "")), "publication Markdown digest is invalid")
    evaluate_vectors(authority, vectors)
    return state


def markdown_cell(value: Any) -> str:
    return ("" if value is None else str(value)).replace("|", "\\|").replace("\r", " ").replace("\n", " ")


def render_markdown(state: dict[str, Any]) -> bytes:
    phases = {row["phase"]: row for row in state["phases"]}
    p5_tasks = [row for row in state["tasks"] if phase_of(row["label"]) == "P5"]
    blockers = [row["gateId"] for row in state["integrationPrerequisites"] if row["outcome"] != "accepted"]
    lines = ["# glTF Scene Editor Parity Progress (Authoritative Projection)", "", "> Generated from the atomically selected v2 bundle. Do not edit by hand.", "", "## Fixed Policy", "", f"- F42: `{state['policy']['f42']}`", f"- R24: `{state['policy']['r24']}`", f"- Metal: `{state['policy']['metal']}`", f"- release/distribution: `{state['policy']['releaseDistribution']}`", "", "## Phase Gates", "", "| Phase | Execution | Engineering Gate | Gate Task | Summary |", "|---|---|---|---|---|"]
    for phase in PHASES:
        row = phases[phase]
        lines.append(f"| {phase} | {row['executionStatus']} | {row['engineeringGate']} | {markdown_cell(row['gateTaskId'])} | {markdown_cell(row['summary'])} |")
    lines.extend(["", "## Decision Prerequisites", "", f"Blocking decision Gates: `{', '.join(blockers) if blockers else 'none'}`.", "", "| Gate | Owner Task | Status | Outcome | Resolution |", "|---|---|---|---|---|"])
    for gate in state["integrationPrerequisites"]:
        lines.append(f"| {gate['gateId']} | {gate['taskId']} | {gate['status']} | {gate['outcome']} | {markdown_cell(gate['resolution'])} |")
    lines.extend(["", "## P5 Live Task State", "", "| Label | Status | Task | Dependencies | Worker |", "|---|---|---|---|---|"])
    for task in p5_tasks:
        lines.append(f"| {markdown_cell(task['label'])} | {task['status']} | {task['taskId']} | {markdown_cell(', '.join(task['dependencies']))} | {task['workerOutcome']} |")
    lines.extend(["", "## Immutable Gate History", "", "| Event | Phase | Task | Verdict | Supersedes | Report SHA-256 |", "|---|---|---|---|---|---|"])
    for event in state["gateHistory"]:
        lines.append(f"| `{event['eventId']}` | {event['phase']} | {event['gateTaskId']} | {event['gateVerdict']} | {markdown_cell(event['supersedesEventId'])} | `{event['report']['sha256']}` |")
    lines.extend(["", "History is forward-linked only: a re-Gate appends `supersedesEventId` and never rewrites an old event.", "", "## P5-P10 Materialization", "", "- Initial Task definitions: `33/33` materialized.", "- Deferred ledger definitions: `L5`, `L6`, `L7`, `L8`, `L9`, `L10`.", "- Ordinary/non-Gate success never promotes a phase.", "", "## Input Identity", "", "| Input | Bytes | SHA-256 |", "|---|---:|---|"])
    for item in [*state["inputs"], state["authority"], state["protocolVectors"]]:
        lines.append(f"| `{item['path']}` | {item['bytes']} | `{item['sha256']}` |")
    lines.extend(["", f"Common source fingerprint: `{state['sourceFingerprintSha256']}`.", f"Stable Orca revision: `{state['snapshot']['revisionSha256']}` on runtime `{state['snapshot']['runtimeId']}`.", ""])
    return "\n".join(lines).encode("utf-8")


def shallow(state_path: Path, ledger_path: Path, repo: Path) -> tuple[dict[str, Any], bytes, bytes]:
    wire_state, state_data = read_json(state_path, "progress state")
    insist(canonical_document(wire_state) == state_data, "state is not canonical JSON")
    state = decode_state_wire(wire_state)
    insist(isinstance(state, dict) and state.get("documentKind") == "progress-ledger-state" and state.get("schemaVersion") == 2 and state.get("runId") == RUN_ID, "shallow state envelope is invalid")
    inputs, policy = derive_policy(repo)
    insist(state.get("inputs") == inputs and state.get("policy") == policy, "shallow policy inputs are stale")
    expected = render_markdown(state)
    actual = pinned_read(ledger_path, "progress Markdown", False)
    insist(actual == expected and digest(actual) == state.get("publication", {}).get("markdownSha256"), "shallow deterministic Markdown differs")
    return state, state_data, actual


def append_only(previous_path: Path, state: dict[str, Any]) -> None:
    previous, _ = read_json(previous_path, "previous state")
    previous = decode_state_wire(previous)
    insist(previous.get("schemaVersion") == 2, "append-only comparison requires a v2 previous state")
    old_gates, old_decisions = previous.get("gateHistory", []), previous.get("decisionGateHistory", [])
    insist(state["gateHistory"][:len(old_gates)] == old_gates, "Gate history is not append-only")
    insist(state["decisionGateHistory"][:len(old_decisions)] == old_decisions, "decision Gate history is not append-only")
    old_materialized = [{key: row[key] for key in ("taskId", "displayName")} for row in previous["materialization"]["initialTasks"]]
    new_materialized = [{key: row[key] for key in ("taskId", "displayName")} for row in state["materialization"]["initialTasks"]]
    insist(old_materialized == new_materialized, "materialization identity changed")


def full_files(repo: Path, state_path: Path, ledger_path: Path, snapshot_path: Path, artifact_base: Path,
               previous_path: Path | None = None) -> tuple[dict[str, Any], bytes, bytes]:
    authority, vectors = load_authority(repo)
    snapshot_wire, snapshot_data = read_json(snapshot_path, "sealed snapshot")
    insist(canonical_document(snapshot_wire) == snapshot_data, "snapshot is not canonical deterministic JSON")
    snapshot = validate_snapshot(snapshot_wire, authority)
    state_wire, state_data = read_json(state_path, "progress state")
    insist(canonical_document(state_wire) == state_data, "state is not canonical deterministic JSON")
    state = validate_state(state_wire, snapshot, repo, artifact_base, authority, vectors)
    insist(state["snapshot"]["snapshotSha256"] == digest(snapshot_data), "state snapshot digest differs from sealed snapshot")
    expected_markdown = render_markdown(state)
    actual_markdown = pinned_read(ledger_path, "progress Markdown", False)
    insist(actual_markdown == expected_markdown, "progress Markdown differs from deterministic independent rendering")
    insist(digest(actual_markdown) == state["publication"]["markdownSha256"], "publication Markdown digest differs")
    if previous_path:
        append_only(previous_path, state)
    return state, state_data, actual_markdown


def validate_pointer(repo: Path, shallow_mode: bool) -> tuple[dict[str, Any], bytes, bytes]:
    pointer_path = repo / POINTER_REL
    pointer, pointer_data = read_json(pointer_path, "authoritative pointer")
    pointer = exact(pointer, {"bundleId", "bundlePath", "documentKind", "markdownSha256", "schemaVersion", "snapshotSha256", "stateSha256"}, "authoritative pointer")
    insist(pointer["documentKind"] == "progress-ledger-pointer" and pointer["schemaVersion"] == 2 and canonical_document(pointer) == pointer_data, "pointer kind/version/canonical form is invalid")
    bundle_rel = Path(pointer["bundlePath"])
    insist(not bundle_rel.is_absolute() and ".." not in bundle_rel.parts and bundle_rel == Path("docs/editor-parity/progress-ledger/bundles/sha256") / pointer["bundleId"], "pointer bundle path is unsafe or noncanonical")
    bundle = repo / bundle_rel
    descriptor, descriptor_data = read_json(bundle / "bundle.json", "bundle descriptor")
    descriptor = exact(descriptor, {"bundleId", "documentKind", "markdownSha256", "schemaVersion", "snapshotSha256", "stateSha256"}, "bundle descriptor")
    insist(descriptor["documentKind"] == "progress-ledger-bundle" and descriptor["schemaVersion"] == 2 and descriptor["bundleId"] == pointer["bundleId"], "bundle descriptor identity is invalid")
    base = {key: descriptor[key] for key in ("documentKind", "schemaVersion", "stateSha256", "markdownSha256", "snapshotSha256")}
    insist(descriptor["bundleId"] == digest(canonical_payload(base)) and canonical_document(descriptor) == descriptor_data, "bundle ID/canonical form does not recompute")
    for key, name in (("stateSha256", "state.json"), ("markdownSha256", "progress.md"), ("snapshotSha256", "snapshot.json")):
        insist(descriptor[key] == pointer[key] and digest_path(bundle / name) == pointer[key], f"bundle {name} identity differs from pointer")
    state_mirror, ledger_mirror = repo / STATE_REL, repo / LEDGER_REL
    insist(digest_path(state_mirror) == pointer["stateSha256"] and digest_path(ledger_mirror) == pointer["markdownSha256"], "checked-in mirrors differ from authoritative pointer")
    if shallow_mode:
        return shallow(state_mirror, ledger_mirror, repo)
    state, state_data, markdown_data = full_files(repo, bundle / "state.json", bundle / "progress.md", bundle / "snapshot.json", repo)
    insist(pinned_read(state_mirror, "state mirror") == state_data and pinned_read(ledger_mirror, "Markdown mirror") == markdown_data, "mirrors differ byte-for-byte from selected bundle")
    return state, state_data, markdown_data


def parse_args() -> argparse.Namespace:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--repo-root", type=Path, default=Path(__file__).resolve().parents[1])
    parser.add_argument("--state", type=Path)
    parser.add_argument("--ledger", type=Path)
    parser.add_argument("--snapshot", type=Path)
    parser.add_argument("--artifact-base", type=Path)
    parser.add_argument("--previous-state", type=Path)
    parser.add_argument("--gate-receipt", type=Path, action="append", default=[], help=argparse.SUPPRESS)
    parser.add_argument("--evidence-root", type=Path, action="append", default=[], help=argparse.SUPPRESS)
    parser.add_argument("--candidate-full", action="store_true")
    parser.add_argument("--check", action="store_true")
    parser.add_argument("--shallow-integrity", action="store_true")
    return parser.parse_args()


def main() -> int:
    try:
        args = parse_args()
        repo = checked_directory(args.repo_root, "repository root")
        insist(not args.gate_receipt, "external Gate receipts are updater inputs; full validator reads durable content-addressed evidence")
        if args.check:
            state, state_data, markdown_data = validate_pointer(repo, args.shallow_integrity)
            mode = "SHALLOW INTEGRITY ONLY" if args.shallow_integrity else "FULL DURABLE EVIDENCE"
        else:
            insist(args.state is not None and args.ledger is not None and args.snapshot is not None and args.candidate_full, "candidate validation requires --state, --ledger, --snapshot, and --candidate-full")
            state_path, ledger_path, snapshot_path = lexical(args.state), lexical(args.ledger), lexical(args.snapshot)
            artifact_base = checked_directory(args.artifact_base or repo, "artifact base")
            state, state_data, markdown_data = full_files(repo, state_path, ledger_path, snapshot_path, artifact_base, args.previous_state)
            mode = "FULL CANDIDATE EVIDENCE"
        print(f"Editor progress ledger validated ({mode}): state={digest(state_data)}, markdown={digest(markdown_data)}, source={state['sourceFingerprintSha256']}")
        return 0
    except (ValidationError, OSError, UnicodeError, ValueError) as exc:
        print(f"Editor progress-ledger validation failed: {exc}", file=sys.stderr)
        return 1


if __name__ == "__main__":
    raise SystemExit(main())
