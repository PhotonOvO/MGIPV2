#!/usr/bin/env python3
"""Publish the post-Orca editor progress overlay from local sealed receipts.

The checked-in progress ledger remains a frozen legacy origin.  This module is
deliberately independent of the Orca CLI and never launches a subprocess.
"""

from __future__ import annotations

import argparse
import base64
import binascii
import hashlib
import json
import os
import re
import shutil
import socket
import stat
import subprocess
import sys
import tempfile
import time
import uuid
from datetime import datetime, timezone
from pathlib import Path, PureWindowsPath
from typing import Any, Iterable


RUN_ID = "run_ede89c4421e9"
LEDGER_ID = "mgif-editor-parity"
LEGACY_POINTER_REL = Path("docs/editor-parity/progress-ledger-current.json")
LEGACY_STORE_REL = Path("docs/editor-parity/progress-ledger/bundles/sha256")
LOCAL_POINTER_REL = Path("docs/editor-parity/progress-ledger-local-current.json")
LOCAL_STORE_REL = Path("docs/editor-parity/progress-ledger/local-bundles/sha256")
LOCAL_STATE_REL = Path("docs/editor-parity/progress-ledger-local-state.json")
LOCAL_MARKDOWN_REL = Path("docs/gltf-scene-editor-parity-local-progress.md")
LOCAL_VALIDATOR_REL = Path("tools/validate_editor_local_progress.py")
LEGACY_VALIDATOR_REL = Path("tools/validate_editor_progress_ledger.py")
EVIDENCE_REL = Path("docs/editor-parity/progress-ledger/evidence/sha256")
LOCK_REL = Path("docs/editor-parity/.progress-ledger-local.lock")
GUARD_REL = Path("docs/editor-parity/.progress-ledger-local.guard")
JOURNAL_REL = Path("docs/editor-parity/.progress-ledger-local-journal.json")
POLICY = {
    "f42": "BLOCKED",
    "r24": "BLOCKED",
    "metal": "Not Verified",
    "releaseDistribution": "NOT PASS",
}
TASK_STATES = {"pending", "ready", "dispatched", "completed", "failed", "blocked"}
TERMINAL_STATES = {"completed", "failed"}
RECOVERY_ROLES = {"repair", "integration", "gate", "reconciliation"}
GATE_PASS = "ENGINEERING PASS"
GATE_FAIL = "FAIL/BLOCKED"
SHA_RE = re.compile(r"^[0-9a-f]{64}$")
WINDOWS_ABS_RE = re.compile(r"^[A-Za-z]:[\\/]|^\\\\")
BANNED_LOCAL_KEYS = {
    "runtimeId", "runGeneration", "dispatch", "dispatchId", "workerResult",
    "workerMessageId", "workerMessages", "orcaResult", "orcaResults",
}


class LocalLedgerError(RuntimeError):
    pass


def need(condition: bool, message: str) -> None:
    if not condition:
        raise LocalLedgerError(message)


def payload(value: Any) -> bytes:
    return json.dumps(value, ensure_ascii=True, sort_keys=True, separators=(",", ":")).encode("ascii")


def document(value: Any) -> bytes:
    return json.dumps(value, ensure_ascii=True, sort_keys=True, indent=2).encode("ascii") + b"\n"


def digest(data: bytes) -> str:
    return hashlib.sha256(data).hexdigest()


def file_digest(path: Path) -> str:
    h = hashlib.sha256()
    with path.open("rb") as stream:
        for chunk in iter(lambda: stream.read(1024 * 1024), b""):
            h.update(chunk)
    return h.hexdigest()


def parse_json(data: bytes, label: str) -> Any:
    try:
        return json.loads(data.decode("utf-8"))
    except (UnicodeDecodeError, json.JSONDecodeError) as exc:
        raise LocalLedgerError(f"{label} is not valid UTF-8 JSON: {exc}") from exc


def read_json(path: Path, label: str) -> tuple[Any, bytes]:
    data = read_file(path, label)
    value = parse_json(data, label)
    need(document(value) == data, f"{label} is not canonical JSON")
    return value, data


def read_file(path: Path, label: str) -> bytes:
    no_reparse(path, label)
    need(path.is_file(), f"{label} is not a regular file: {path}")
    try:
        return path.read_bytes()
    except OSError as exc:
        raise LocalLedgerError(f"cannot read {label}: {exc}") from exc


def no_reparse(path: Path, label: str) -> None:
    absolute = Path(os.path.abspath(os.fspath(path)))
    cursor = Path(absolute.anchor)
    for part in absolute.parts[1:]:
        cursor /= part
        if not os.path.lexists(cursor):
            continue
        info = os.lstat(cursor)
        reparse = stat.S_ISLNK(info.st_mode) or bool(
            getattr(info, "st_file_attributes", 0) & 0x400
        )
        need(not reparse, f"{label} traverses a reparse point: {cursor}")


def utc(value: Any, label: str) -> str:
    need(isinstance(value, str) and value.strip(), f"{label} must be an ISO-8601 timestamp")
    text = value.strip().replace(" ", "T", 1)
    if text.endswith("Z"):
        text = text[:-1] + "+00:00"
    try:
        parsed = datetime.fromisoformat(text)
    except ValueError as exc:
        raise LocalLedgerError(f"{label} is not an ISO-8601 timestamp") from exc
    if parsed.tzinfo is None:
        parsed = parsed.replace(tzinfo=timezone.utc)
    rendered = parsed.astimezone(timezone.utc).isoformat(timespec="milliseconds")
    return rendered[:-6] + "Z"


def epoch(value: str) -> float:
    return datetime.fromisoformat(value[:-1] + "+00:00").timestamp()


def closed(value: Any, keys: set[str], label: str) -> dict[str, Any]:
    need(isinstance(value, dict), f"{label} must be an object")
    actual = set(value)
    need(actual == keys, f"{label} keys differ: missing={sorted(keys - actual)}, extra={sorted(actual - keys)}")
    return value


def map_windows_path(raw: str) -> Path:
    """Map a Windows absolute path when this process is running under POSIX."""
    if os.name == "nt":
        return Path(raw)
    if re.match(r"^[A-Za-z]:[\\/]", raw):
        drive = raw[0].lower()
        remainder = raw[2:].replace("\\", "/").lstrip("/")
        return Path("/mnt") / drive / remainder
    if raw.startswith("\\\\"):
        return Path("/") / raw.replace("\\", "/").lstrip("/")
    return Path(raw)


def lexical(path: Path) -> Path:
    return Path(os.path.abspath(os.fspath(path)))


def contained(path: Path, root: Path) -> bool:
    try:
        lexical(path).relative_to(lexical(root))
        return True
    except ValueError:
        return False


def resolve_external(raw: str, roots: Iterable[Path], label: str) -> Path:
    need(isinstance(raw, str) and raw.strip() and "\x00" not in raw, f"{label} path is invalid")
    path = map_windows_path(raw) if WINDOWS_ABS_RE.match(raw) else Path(raw)
    path = lexical(path)
    matched = False
    for root in roots:
        root = lexical(root)
        if contained(path, root):
            matched = True
            break
    need(matched, f"{label} escapes every evidence root: {raw}")
    no_reparse(path, label)
    need(path.is_file(), f"{label} is not a regular file: {raw}")
    return path


def relative_cas(path: str, label: str) -> str:
    value = Path(path.replace("\\", "/"))
    expected_parent = EVIDENCE_REL.as_posix().split("/")
    need(not value.is_absolute() and ".." not in value.parts, f"{label} path is unsafe")
    need(value.parts[:-1] == tuple(expected_parent) and SHA_RE.fullmatch(value.name or "") is not None,
         f"{label} must be a content-addressed repository path")
    return value.as_posix()


def artifact_record(path: Path, logical_path: str) -> dict[str, Any]:
    data = read_file(path, "artifact")
    return {"path": logical_path, "bytes": len(data), "sha256": digest(data)}


def verify_record(record: Any, repo: Path, label: str) -> tuple[dict[str, Any], bytes]:
    record = closed(record, {"path", "bytes", "sha256"}, label)
    need(isinstance(record["bytes"], int) and record["bytes"] > 0 and SHA_RE.fullmatch(record["sha256"] or ""),
         f"{label} identity is invalid")
    logical = Path(str(record["path"]).replace("\\", "/"))
    relative_cas(logical.as_posix(), label)
    path = repo / logical
    data = read_file(path, label)
    need(len(data) == record["bytes"] and digest(data) == record["sha256"], f"{label} bytes/hash differ")
    return record, data


def write_atomic(path: Path, data: bytes, label: str) -> None:
    path.parent.mkdir(parents=True, exist_ok=True)
    no_reparse(path.parent, f"{label} parent")
    temporary = path.parent / f".{path.name}.{uuid.uuid4().hex}.tmp"
    try:
        with temporary.open("xb") as stream:
            stream.write(data)
            stream.flush()
            os.fsync(stream.fileno())
        os.replace(temporary, path)
        try:
            fd = os.open(path.parent, os.O_RDONLY)
            try:
                os.fsync(fd)
            finally:
                os.close(fd)
        except OSError:
            pass
    finally:
        temporary.unlink(missing_ok=True)


def unlink_durable(path: Path) -> None:
    path.unlink(missing_ok=True)
    try:
        fd = os.open(path.parent, os.O_RDONLY)
        try:
            os.fsync(fd)
        finally:
            os.close(fd)
    except OSError:
        pass


def preimage(path: Path) -> dict[str, Any]:
    if not os.path.lexists(path):
        return {"exists": False, "sha256": None, "base64": None}
    data = read_file(path, f"preimage {path}")
    return {"exists": True, "sha256": digest(data), "base64": base64.b64encode(data).decode("ascii")}


def current_hash(path: Path) -> str | None:
    if not os.path.lexists(path):
        return None
    return digest(read_file(path, f"current {path}"))


def publication_paths(repo: Path) -> dict[str, Path]:
    return {
        "pointer": repo / LOCAL_POINTER_REL,
        "state": repo / LOCAL_STATE_REL,
        "ledger": repo / LOCAL_MARKDOWN_REL,
    }


def restore_preimages(paths: dict[str, Path], images: dict[str, Any]) -> None:
    closed(images, set(paths), "local publication preimages")
    for name, path in paths.items():
        image = closed(images[name], {"exists", "sha256", "base64"},
                       f"local {name} preimage")
        need(isinstance(image["exists"], bool),
             f"local {name} preimage existence is invalid")
        if image["exists"]:
            need(isinstance(image["sha256"], str)
                 and SHA_RE.fullmatch(image["sha256"]),
                 f"local {name} preimage hash is invalid")
            need(isinstance(image["base64"], str),
                 f"local {name} preimage bytes are invalid")
            try:
                data = base64.b64decode(image["base64"], validate=True)
            except (ValueError, binascii.Error) as exc:
                raise LocalLedgerError(
                    f"local {name} preimage bytes are invalid") from exc
            need(digest(data) == image["sha256"],
                 f"local {name} preimage hash differs")
            write_atomic(path, data, f"restore local {name}")
        else:
            need(image["sha256"] is None and image["base64"] is None,
                 f"absent local {name} preimage carries bytes")
            path.unlink(missing_ok=True)


def preimage_hashes(images: dict[str, Any], names: set[str]) -> dict[str, str | None]:
    closed(images, names, "local publication preimages")
    result: dict[str, str | None] = {}
    for name in names:
        image = closed(images[name], {"exists", "sha256", "base64"},
                       f"local {name} preimage")
        need(isinstance(image["exists"], bool),
             f"local {name} preimage existence is invalid")
        if image["exists"]:
            need(isinstance(image["sha256"], str)
                 and SHA_RE.fullmatch(image["sha256"]),
                 f"local {name} preimage hash is invalid")
            result[name] = image["sha256"]
        else:
            need(image["sha256"] is None and image["base64"] is None,
                 f"absent local {name} preimage carries bytes")
            result[name] = None
    return result


def require_publication_hashes(paths: dict[str, Path],
                               expected: dict[str, str | None],
                               label: str) -> None:
    closed(expected, set(paths), f"{label} expected hashes")
    for name, path in paths.items():
        need(current_hash(path) == expected[name],
             f"local {name} changed outside the publication during {label}")


def restore_preimages_guarded(paths: dict[str, Path], images: dict[str, Any],
                              new_hashes: dict[str, str] | None) -> None:
    old_hashes = preimage_hashes(images, set(paths))
    if new_hashes is not None:
        closed(new_hashes, set(paths), "local publication new hashes")
    for name, path in paths.items():
        allowed = {old_hashes[name]}
        if new_hashes is not None:
            new_hash = new_hashes[name]
            need(isinstance(new_hash, str) and SHA_RE.fullmatch(new_hash),
                 f"local {name} publication target hash is invalid")
            allowed.add(new_hash)
        need(current_hash(path) in allowed,
             f"local {name} changed outside the publication; rollback refused")
        image = images[name]
        if image["exists"]:
            try:
                data = base64.b64decode(image["base64"], validate=True)
            except (ValueError, binascii.Error) as exc:
                raise LocalLedgerError(
                    f"local {name} preimage bytes are invalid") from exc
            need(digest(data) == image["sha256"],
                 f"local {name} preimage hash differs")
            write_atomic(path, data, f"restore local {name}")
        else:
            path.unlink(missing_ok=True)


def guard_path(repo: Path) -> Path:
    return repo / GUARD_REL


def acquire_advisory_guard(repo: Path) -> Any:
    path = guard_path(repo)
    path.parent.mkdir(parents=True, exist_ok=True)
    stream = path.open("a+b")
    try:
        stream.seek(0, os.SEEK_END)
        if stream.tell() == 0:
            stream.write(b"\0")
            stream.flush()
            os.fsync(stream.fileno())
        stream.seek(0)
        if os.name == "nt":
            import msvcrt

            msvcrt.locking(stream.fileno(), msvcrt.LK_NBLCK, 1)
        else:
            import fcntl

            fcntl.flock(stream.fileno(), fcntl.LOCK_EX | fcntl.LOCK_NB)
        return stream
    except (OSError, BlockingIOError) as exc:
        stream.close()
        raise LocalLedgerError(
            f"local ledger writer guard is already active: {path}") from exc


def release_advisory_guard(stream: Any) -> None:
    try:
        stream.seek(0)
        if os.name == "nt":
            import msvcrt

            msvcrt.locking(stream.fileno(), msvcrt.LK_UNLCK, 1)
        else:
            import fcntl

            fcntl.flock(stream.fileno(), fcntl.LOCK_UN)
    finally:
        stream.close()


def acquire_lock(repo: Path, *, reclaim_stale: bool = False) -> tuple[Path, str, str | None, Any]:
    guard = acquire_advisory_guard(repo)
    path = repo / LOCK_REL
    path.parent.mkdir(parents=True, exist_ok=True)
    token = uuid.uuid4().hex
    reclaimed_token = None
    try:
        if os.path.lexists(path):
            if not reclaim_stale:
                raise LocalLedgerError(
                    f"local ledger writer lock is already active: {path}")
            owner, owner_data = read_json(path, "existing local writer lock")
            owner = closed(
                owner,
                {"documentKind", "schemaVersion", "pid", "platform", "host",
                 "token"},
                "existing local writer lock",
            )
            need(owner["documentKind"] == "local-ledger-lock"
                 and owner["schemaVersion"] == 1
                 and isinstance(owner["pid"], int)
                 and isinstance(owner["platform"], str)
                 and isinstance(owner["host"], str)
                 and isinstance(owner["token"], str)
                 and bool(owner["token"]),
                 "existing local writer lock identity is invalid")
            need(owner["platform"] == sys.platform
                 and owner["host"] == socket.gethostname(),
                 "local ledger writer lock belongs to another host or platform")
            journal_path = repo / JOURNAL_REL
            if os.path.lexists(journal_path):
                journal, _ = read_json(journal_path,
                                       "interrupted local publication journal")
                need(isinstance(journal, dict)
                     and journal.get("transactionId") == owner["token"],
                     "stale writer lock does not own the interrupted journal")
            need(current_hash(path) == digest(owner_data),
                 "local ledger writer lock changed during stale-owner recovery")
            reclaimed_token = owner["token"]
            path.unlink()
        fd = os.open(path, os.O_CREAT | os.O_EXCL | os.O_WRONLY, 0o600)
        with os.fdopen(fd, "wb") as stream:
            stream.write(document({
                "documentKind": "local-ledger-lock",
                "schemaVersion": 1,
                "pid": os.getpid(),
                "platform": sys.platform,
                "host": socket.gethostname(),
                "token": token,
            }))
            stream.flush()
            os.fsync(stream.fileno())
        return path, token, reclaimed_token, guard
    except BaseException:
        release_advisory_guard(guard)
        raise


def release_lock(path: Path, token: str, guard: Any) -> None:
    try:
        value, _ = read_json(path, "local writer lock")
        need(value.get("token") == token and value.get("pid") == os.getpid(), "local writer lock ownership changed")
        path.unlink()
    except FileNotFoundError:
        pass
    finally:
        release_advisory_guard(guard)


def recover_local_publication(repo: Path) -> bool:
    journal_path = repo / JOURNAL_REL
    lock, token, reclaimed_token, guard = acquire_lock(repo, reclaim_stale=True)
    try:
        if not os.path.lexists(journal_path):
            return False
        journal, _ = read_json(journal_path, "local publication journal")
        journal = closed(
            journal,
            {"documentKind", "schemaVersion", "transactionId", "commitPoint",
             "preimages", "newHashes"},
            "local publication journal",
        )
        need(journal["documentKind"]
             == "local-progress-ledger-publication-journal"
             and journal["schemaVersion"] == 1
             and journal["commitPoint"] == "pointer-replacement"
             and isinstance(journal["transactionId"], str)
             and bool(journal["transactionId"]),
             "local publication journal identity is invalid")
        if reclaimed_token is not None:
            need(journal["transactionId"] == reclaimed_token,
                 "stale writer lock does not own the interrupted journal")
        paths = publication_paths(repo)
        images = closed(journal["preimages"], set(paths),
                        "local publication preimages")
        new_hashes = closed(journal["newHashes"], set(paths),
                            "local publication new hashes")
        current_pointer = current_hash(paths["pointer"])
        old_hashes = preimage_hashes(images, set(paths))
        if current_pointer == new_hashes["pointer"]:
            require_publication_hashes(paths, new_hashes,
                                       "committed journal recovery")
            run_independent_validator(repo)
        else:
            need(current_pointer == old_hashes["pointer"],
                 "local pointer changed outside the interrupted publication")
            restore_preimages_guarded(paths, images, new_hashes)
        unlink_durable(journal_path)
        return True
    finally:
        release_lock(lock, token, guard)


def legacy_origin(repo: Path) -> dict[str, Any]:
    pointer_path = repo / LEGACY_POINTER_REL
    pointer, pointer_data = read_json(pointer_path, "legacy progress pointer")
    closed(pointer, {"bundleId", "bundlePath", "documentKind", "markdownSha256", "schemaVersion", "snapshotSha256", "stateSha256"}, "legacy progress pointer")
    need(pointer["documentKind"] == "progress-ledger-pointer" and pointer["schemaVersion"] == 2, "legacy pointer kind/version changed")
    need(pointer["bundlePath"] == (LEGACY_STORE_REL / pointer["bundleId"]).as_posix(), "legacy bundle path is not canonical")
    pointer_sha = digest(pointer_data)
    bundle = repo / pointer["bundlePath"]
    no_reparse(bundle, "legacy bundle")
    descriptor, descriptor_data = read_json(bundle / "bundle.json", "legacy bundle descriptor")
    closed(descriptor, {"bundleId", "documentKind", "markdownSha256", "schemaVersion", "snapshotSha256", "stateSha256"}, "legacy bundle descriptor")
    base = {key: descriptor[key] for key in ("documentKind", "schemaVersion", "stateSha256", "markdownSha256", "snapshotSha256")}
    need(descriptor["bundleId"] == pointer["bundleId"] and digest(payload(base)) == descriptor["bundleId"], "legacy bundle ID does not recompute")
    need(document(descriptor) == descriptor_data, "legacy bundle descriptor is not canonical")
    state_data = read_file(bundle / "state.json", "legacy state")
    snapshot_data = read_file(bundle / "snapshot.json", "legacy snapshot")
    markdown_data = read_file(bundle / "progress.md", "legacy progress Markdown")
    need(digest(state_data) == pointer["stateSha256"] == descriptor["stateSha256"], "legacy state hash differs")
    need(digest(snapshot_data) == pointer["snapshotSha256"] == descriptor["snapshotSha256"], "legacy snapshot hash differs")
    need(digest(markdown_data) == pointer["markdownSha256"] == descriptor["markdownSha256"], "legacy Markdown hash differs")
    snapshot = parse_json(snapshot_data, "legacy snapshot")
    state = parse_json(state_data, "legacy state")
    need(snapshot.get("documentKind") == "orca-progress-snapshot" and snapshot.get("schemaVersion") == 4, "legacy snapshot is not v4")
    need(state.get("documentKind") == "progress-ledger-state" and state.get("schemaVersion") == 4, "legacy state is not v4")
    tasks = snapshot.get("runTasks")
    need(isinstance(tasks, dict) and len(tasks) >= 33, "legacy task map is incomplete")
    state_tasks = state.get("tasks", {})
    initial_tasks = state.get("materialization", {}).get("initialTasks", {})
    need(isinstance(initial_tasks, dict) and len(initial_tasks) == 33, "legacy materialized task identity map differs")
    normalized_tasks: dict[str, dict[str, Any]] = {}
    for task_id in initial_tasks:
        task = tasks.get(task_id)
        need(isinstance(task, dict), f"legacy task {task_id} is not an object")
        # v4 snapshots retain an index-only row for untracked tasks.  Merge
        # lifecycle fields from the state projection when it has the task.
        source = {**task}
        projected = state_tasks.get(task_id) if isinstance(state_tasks, dict) else None
        if isinstance(projected, dict):
            for key in ("dependencies", "completedAtUtc"):
                if key not in source and key in projected:
                    source[key] = projected[key]
        source.setdefault("dependencies", [])
        source.setdefault("completedAtUtc", None)
        source.setdefault("dispatch", None)
        source.setdefault("workerResult", None)
        task_keys = {"displayName", "status", "dependencies", "createdAtUtc", "completedAtUtc", "dispatch", "workerResult"}
        if "completionProvenance" in source:
            task_keys.add("completionProvenance")
        closed(source, task_keys, f"legacy task {task_id}")
        need(isinstance(source["dependencies"], list) and source["status"] in TASK_STATES, f"legacy task {task_id} lifecycle is invalid")
        normalized_tasks[task_id] = source
    return {
        "pointer": pointer,
        "pointerData": pointer_data,
        "pointerSha256": pointer_sha,
        "bundleId": pointer["bundleId"],
        "stateSha256": pointer["stateSha256"],
        "snapshotSha256": pointer["snapshotSha256"],
        "snapshot": snapshot,
        "tasks": normalized_tasks,
        "state": state,
    }


def legacy_task_view(task_id: str, task: dict[str, Any]) -> dict[str, Any]:
    evidence = None
    if task["status"] in TERMINAL_STATES:
        evidence = {"sourceKind": "legacy-orca", "legacyTaskSha256": digest(payload(task))}
    return {
        "taskId": task_id,
        "displayName": task["displayName"],
        "status": task["status"],
        "dependencies": sorted(task["dependencies"]),
        "createdAtUtc": utc(task["createdAtUtc"], f"legacy task {task_id} creation"),
        "completedAtUtc": utc(task["completedAtUtc"], f"legacy task {task_id} completion") if task["completedAtUtc"] else None,
        "completionEvidence": evidence,
    }


def task_hash(task: dict[str, Any]) -> str:
    return digest(payload({key: task[key] for key in ("taskId", "displayName", "status", "dependencies", "createdAtUtc", "completedAtUtc", "completionEvidence")}))


def gate_phase(display_name: str) -> str | None:
    match = re.fullmatch(r"G(10|[0-9])(?:-[A-Za-z0-9._:-]+)?", display_name)
    return f"P{match.group(1)}" if match else None


def recovery_task_view(definition: dict[str, Any]) -> dict[str, Any]:
    return {
        "taskId": definition["taskId"],
        "displayName": definition["displayName"],
        "status": "pending",
        "dependencies": sorted(definition["dependencies"]),
        "createdAtUtc": utc(
            definition["createdAtUtc"],
            f"recovery task {definition['taskId']} creation",
        ),
        "completedAtUtc": None,
        "completionEvidence": None,
    }


def latest_gate_events(events: list[dict[str, Any]]) -> dict[str, dict[str, Any]]:
    latest: dict[str, dict[str, Any]] = {}
    known: dict[str, dict[str, Any]] = {}
    for event in events:
        event_id = event["eventId"]
        need(event_id not in known, f"duplicate local Gate event {event_id}")
        prior = event["supersedesEventId"]
        if prior is not None:
            need(prior in known, "local Gate event supersedes an unknown event")
            need(known[prior]["phase"] == event["phase"],
                 "local Gate event supersedes a different phase")
            need(latest.get(event["phase"], {}).get("eventId") == prior,
                 "local Gate event does not supersede the latest event")
        else:
            need(event["phase"] not in latest,
                 "local Gate event omits required supersession")
        known[event_id] = event
        latest[event["phase"]] = event
    return latest


def check_task_graph(tasks: dict[str, dict[str, Any]]) -> None:
    for task_id, task in tasks.items():
        need(len(task["dependencies"]) == len(set(task["dependencies"])),
             f"task {task_id} has duplicate dependencies")
    visiting: set[str] = set()
    visited: set[str] = set()

    def visit(task_id: str) -> None:
        need(task_id not in visiting, f"local task graph contains a cycle at {task_id}")
        if task_id in visited:
            return
        visiting.add(task_id)
        for dependency in tasks[task_id]["dependencies"]:
            if dependency in tasks:
                visit(dependency)
        visiting.remove(task_id)
        visited.add(task_id)

    for task_id in tasks:
        visit(task_id)


def active_dependency_replacements(
    events: list[dict[str, Any]],
    replacements: list[dict[str, Any]],
) -> dict[tuple[str, str], dict[str, Any]]:
    latest = latest_gate_events(events)
    active: dict[tuple[str, str], dict[str, Any]] = {}
    for replacement in replacements:
        event = latest.get(replacement["acceptedGate"]["phase"])
        if (event is not None
                and event["eventId"] == replacement["eventId"]
                and event["gateVerdict"] == GATE_PASS):
            active[(replacement["taskId"],
                    replacement["replacedDependencyTaskId"])] = replacement
    return active


RECOVERY_TASK_KEYS = {
    "taskId", "displayName", "dependencies", "createdAtUtc", "phase",
    "role", "supersedesTaskId",
}


def validate_recovery_task_definition(
    value: Any, label: str
) -> dict[str, Any]:
    definition = closed(value, RECOVERY_TASK_KEYS, label)
    task_id = definition["taskId"]
    need(isinstance(task_id, str)
         and re.fullmatch(r"task_[A-Za-z0-9._:-]+", task_id) is not None,
         f"{label} task ID is invalid")
    need(isinstance(definition["displayName"], str)
         and definition["displayName"].strip() == definition["displayName"]
         and definition["displayName"], f"{label} display name is invalid")
    dependencies = definition["dependencies"]
    need(isinstance(dependencies, list)
         and bool(dependencies)
         and dependencies == sorted(dependencies)
         and len(dependencies) == len(set(dependencies))
         and all(isinstance(item, str) and item for item in dependencies),
         f"{label} dependencies are invalid")
    need(task_id not in dependencies, f"{label} contains a self cycle")
    phase = definition["phase"]
    need(isinstance(phase, str) and re.fullmatch(r"P(?:10|[0-9])", phase),
         f"{label} phase is invalid")
    need(definition["role"] in RECOVERY_ROLES, f"{label} role is invalid")
    created = utc(definition["createdAtUtc"], f"{label} creation")
    need(created == definition["createdAtUtc"],
         f"{label} creation time is not canonical")
    supersedes = definition["supersedesTaskId"]
    need(supersedes is None or (isinstance(supersedes, str) and supersedes),
         f"{label} superseded task is invalid")
    if definition["role"] == "gate":
        need(gate_phase(definition["displayName"]) == phase,
             f"{label} Gate display name and phase differ")
        need(supersedes is not None,
             f"{label} recovery Gate must supersede a prior task")
    return definition


def validate_gate_event(value: Any, definition: dict[str, Any], label: str) -> dict[str, Any] | None:
    if definition["role"] != "gate":
        need(value is None, f"{label} is only valid for a Gate recovery task")
        return None
    event = closed(value, {"phase", "gateVerdict", "supersedesEventId"}, label)
    need(event["phase"] == definition["phase"],
         f"{label} phase differs from its task")
    need(event["gateVerdict"] in {GATE_PASS, GATE_FAIL},
         f"{label} verdict is invalid")
    need(isinstance(event["supersedesEventId"], str)
         and SHA_RE.fullmatch(event["supersedesEventId"]),
         f"{label} superseded event is invalid")
    return event


def validate_dependency_replacement_drafts(
    values: Any,
    definition: dict[str, Any],
    gate_event: dict[str, Any] | None,
    tasks: dict[str, dict[str, Any]],
    prior_events: list[dict[str, Any]],
    label: str,
) -> list[dict[str, Any]]:
    need(isinstance(values, list), f"{label} must be an array")
    if definition["role"] != "gate":
        need(not values, f"{label} is only valid for a Gate recovery task")
        return []
    assert gate_event is not None
    if gate_event["gateVerdict"] != GATE_PASS:
        need(not values,
             f"{label} failed Gate cannot declare dependency replacements")
    known_events = {event["eventId"]: event for event in prior_events}
    superseded = known_events.get(gate_event["supersedesEventId"])
    need(superseded is not None, f"{label} Gate supersedes an unknown event")
    need(superseded["phase"] == gate_event["phase"],
         f"{label} Gate supersedes a different phase")
    need(superseded["taskId"] == definition["supersedesTaskId"],
         f"{label} Gate event and superseded task differ")
    latest = latest_gate_events(prior_events)
    need(latest.get(gate_event["phase"], {}).get("eventId")
         == gate_event["supersedesEventId"],
         f"{label} Gate does not supersede the latest event")
    result: list[dict[str, Any]] = []
    pairs: set[tuple[str, str]] = set()
    for index, raw in enumerate(values):
        item = closed(
            raw,
            {"taskId", "replacedDependencyTaskId", "acceptedGate"},
            f"{label} {index}",
        )
        accepted = closed(
            item["acceptedGate"], {"phase", "gateVerdict"},
            f"{label} {index} accepted Gate")
        need(accepted == {"phase": gate_event["phase"],
                          "gateVerdict": GATE_PASS},
             f"{label} {index} accepted Gate predicate differs")
        target = tasks.get(item["taskId"])
        need(target is not None, f"{label} {index} names an unknown task")
        need(target["status"] not in TERMINAL_STATES,
             f"{label} {index} targets a terminal task")
        need(item["replacedDependencyTaskId"] in target["dependencies"],
             f"{label} {index} does not replace an actual dependency")
        need(item["replacedDependencyTaskId"] == superseded["taskId"],
             f"{label} {index} does not replace the superseded Gate task")
        pair = (item["taskId"], item["replacedDependencyTaskId"])
        need(pair not in pairs, f"{label} contains a duplicate replacement")
        pairs.add(pair)
        result.append(item)
    return result


def implicit_v1_gate_event(
    receipt: dict[str, Any], receipt_sha: str
) -> dict[str, Any] | None:
    phase = gate_phase(receipt["displayName"])
    if phase is None:
        return None
    return {
        "eventId": receipt_sha,
        "phase": phase,
        "taskId": receipt["taskId"],
        "gateVerdict": GATE_PASS
        if receipt["toStatus"] == "completed" else GATE_FAIL,
        "supersedesEventId": None,
    }


# Schema-2 ordinary transitions deliberately have no Gate event envelope.  A
# historical Gate can nevertheless have been recorded that way (the original
# G7 receipt is one such transition).  Keep those snapshots backward
# compatible and only materialize a virtual predecessor when a later, explicit
# recovery Gate names the exact receipt hash.  This is intentionally narrow:
# successful ordinary Gate transitions and non-materialized/renamed tasks are
# never promoted implicitly.
_BRIDGE_RECOVERY_FIELDS = frozenset({
    "taskDefinition", "authorityArtifact", "gateEvent",
    "dependencyReplacements",
})


def ordinary_schema2_failed_gate_event(
    receipt: Any,
    receipt_sha: str,
    origin_tasks: dict[str, dict[str, Any]],
) -> dict[str, Any] | None:
    """Return a virtual event for the one safe legacy Gate shape.

    The caller has already verified the receipt's content-addressed record and
    transition-chain fields.  The checks here bind the bridge to the frozen
    materialized Gate task and reject any recovery/ordinary look-alike.
    """
    if not isinstance(receipt, dict) or receipt.get("schemaVersion") != 2:
        return None
    if set(receipt) & _BRIDGE_RECOVERY_FIELDS:
        return None
    if (receipt.get("documentKind") != "local-task-transition-receipt"
            or receipt.get("ledgerId") != LEDGER_ID
            or receipt.get("fromStatus") != "ready"
            or receipt.get("toStatus") != "failed"
            or receipt.get("outcome") != "failed"
            or receipt.get("reviewAuthority") != "local-automatic-review"
            or receipt.get("reviewExecutionOutcome") != "succeeded"
            or receipt.get("fixedPolicy") != POLICY):
        return None
    if not isinstance(receipt_sha, str) or SHA_RE.fullmatch(receipt_sha) is None:
        return None
    try:
        if digest(document(receipt)) != receipt_sha:
            return None
    except (TypeError, ValueError, UnicodeError):
        return None
    display_name = receipt.get("displayName")
    if not isinstance(display_name, str):
        return None
    phase = gate_phase(display_name)
    if phase is None:
        return None
    expected_display = f"G{phase[1:]}"
    matches = [
        (task_id, task)
        for task_id, task in origin_tasks.items()
        if isinstance(task, dict) and task.get("displayName") == expected_display
    ]
    if len(matches) != 1:
        return None
    task_id, task = matches[0]
    if (receipt.get("taskId") != task_id
            or display_name != task.get("displayName")):
        return None
    # A first transition has no predecessor.  Requiring one here prevents a
    # forged standalone ordinary receipt from becoming a Gate anchor.
    predecessor = receipt.get("previousTransitionSha256")
    if not isinstance(predecessor, str) or SHA_RE.fullmatch(predecessor) is None:
        return None
    return {
        "eventId": receipt_sha,
        "phase": phase,
        "taskId": task_id,
        "gateVerdict": GATE_FAIL,
        "supersedesEventId": None,
    }


def collect_ordinary_gate_bridge_events(
    records: list[dict[str, Any]],
    repo: Path,
    origin_tasks: dict[str, dict[str, Any]],
) -> dict[str, dict[str, Any]]:
    """Index bridgeable ordinary Gate receipts in an already sealed prefix."""
    result: dict[str, dict[str, Any]] = {}
    for index, record in enumerate(records):
        _, receipt_data = verify_record(
            record, repo, f"local transition receipt {index}")
        receipt = parse_json(receipt_data,
                             f"local transition receipt {index}")
        event = ordinary_schema2_failed_gate_event(
            receipt, record["sha256"], origin_tasks)
        if event is not None:
            result[record["sha256"]] = event
    return result


def maybe_bridge_gate_predecessor(
    gate_event: dict[str, Any],
    definition: dict[str, Any],
    prior_events: list[dict[str, Any]],
    candidates: dict[str, dict[str, Any]],
    label: str,
) -> None:
    """Resolve an explicitly named ordinary Gate receipt, if present."""
    reference = gate_event["supersedesEventId"]
    if any(event["eventId"] == reference for event in prior_events):
        return
    candidate = candidates.get(reference)
    if candidate is None:
        # Preserve the existing unknown-event diagnostic in the regular
        # dependency-replacement validator.
        return
    need(candidate["phase"] == gate_event["phase"],
         f"{label} ordinary Gate bridge phase differs")
    need(candidate["taskId"] == definition["supersedesTaskId"],
         f"{label} ordinary Gate bridge task differs")
    need(not any(event["phase"] == candidate["phase"]
                 for event in prior_events),
         f"{label} ordinary Gate bridge would duplicate a phase")
    prior_events.append(candidate)
    latest_gate_events(prior_events)


def load_local(repo: Path, origin: dict[str, Any]) -> dict[str, Any] | None:
    pointer_path = repo / LOCAL_POINTER_REL
    if not os.path.lexists(pointer_path):
        return None
    pointer, pointer_data = read_json(pointer_path, "local progress pointer")
    closed(pointer, {"documentKind", "schemaVersion", "ledgerId", "basePointerSha256", "baseBundleId", "baseStateSha256", "baseSnapshotSha256", "bundleId", "bundlePath", "stateSha256", "snapshotSha256", "markdownSha256"}, "local progress pointer")
    need(pointer["documentKind"] == "local-progress-ledger-pointer" and pointer["schemaVersion"] == 1 and pointer["ledgerId"] == LEDGER_ID, "local pointer identity is invalid")
    need(pointer["basePointerSha256"] == origin["pointerSha256"] and pointer["baseBundleId"] == origin["bundleId"] and pointer["baseStateSha256"] == origin["stateSha256"] and pointer["baseSnapshotSha256"] == origin["snapshotSha256"], "local pointer legacy base changed")
    need(pointer["bundlePath"] == (LOCAL_STORE_REL / pointer["bundleId"]).as_posix(), "local bundle path is not canonical")
    bundle = repo / pointer["bundlePath"]
    descriptor, descriptor_data = read_json(bundle / "bundle.json", "local bundle descriptor")
    closed(descriptor, {"bundleId", "documentKind", "markdownSha256", "schemaVersion", "snapshotSha256", "stateSha256"}, "local bundle descriptor")
    base = {key: descriptor[key] for key in ("documentKind", "schemaVersion", "stateSha256", "markdownSha256", "snapshotSha256")}
    need(descriptor["documentKind"] == "local-progress-ledger-bundle" and descriptor["schemaVersion"] == 1 and digest(payload(base)) == descriptor["bundleId"] == pointer["bundleId"], "local bundle ID does not recompute")
    need(document(descriptor) == descriptor_data, "local bundle descriptor is not canonical")
    state, state_data = read_json(bundle / "state.json", "local state")
    snapshot, snapshot_data = read_json(bundle / "snapshot.json", "local snapshot")
    markdown_data = read_file(bundle / "progress.md", "local progress Markdown")
    need(digest(state_data) == pointer["stateSha256"] == descriptor["stateSha256"], "local state hash differs")
    need(digest(snapshot_data) == pointer["snapshotSha256"] == descriptor["snapshotSha256"], "local snapshot hash differs")
    need(digest(markdown_data) == pointer["markdownSha256"] == descriptor["markdownSha256"], "local Markdown hash differs")
    validate_local_snapshot(snapshot, origin, repo)
    transitions = snapshot["transitionReceipts"]
    overrides = snapshot["taskOverrides"]
    definitions = snapshot.get("recoveryTaskDefinitions", {})
    gate_events = snapshot.get("gateEvents")
    if gate_events is None:
        gate_events = []
        for index, record in enumerate(transitions):
            _, receipt_data = verify_record(
                record, repo, f"local transition receipt {index}")
            receipt = parse_json(receipt_data,
                                 f"local transition receipt {index}")
            event = implicit_v1_gate_event(receipt, record["sha256"])
            if event is not None:
                gate_events.append(event)
    replacements = snapshot.get("dependencyReplacements", [])
    tasks = effective_tasks(
        origin, overrides, repo, definitions, gate_events, replacements)
    validate_local_state(
        state, snapshot, origin, repo, pointer, tasks, gate_events, definitions)
    return {"pointer": pointer, "pointerData": pointer_data,
            "pointerSha256": digest(pointer_data),
            "bundleId": pointer["bundleId"], "snapshot": snapshot,
            "state": state, "tasks": tasks, "transitions": transitions,
            "overrides": overrides, "recoveryDefinitions": definitions,
            "gateEvents": gate_events,
            "dependencyReplacements": replacements}


def validate_local_snapshot(snapshot: dict[str, Any], origin: dict[str, Any], repo: Path) -> None:
    version = snapshot.get("schemaVersion")
    need(version in {1, 2}, "local snapshot version is invalid")
    keys = {"documentKind", "schemaVersion", "ledgerId", "base", "parent", "sealedAtUtc", "consistencyMode", "transitionReceipts", "transitionHeadSha256", "taskOverrides", "revisionSha256"}
    if version == 2:
        keys |= {"recoveryTaskDefinitions", "gateEvents",
                 "dependencyReplacements"}
    closed(snapshot, keys, "local snapshot")
    need(snapshot["documentKind"] == "local-progress-snapshot"
         and snapshot["ledgerId"] == LEDGER_ID,
         "local snapshot identity is invalid")
    base = closed(snapshot["base"], {"pointerSha256", "bundleId", "stateSha256", "snapshotSha256", "revisionSha256"}, "local snapshot base")
    need(base == {"pointerSha256": origin["pointerSha256"], "bundleId": origin["bundleId"], "stateSha256": origin["stateSha256"], "snapshotSha256": origin["snapshotSha256"], "revisionSha256": origin["snapshot"].get("revisionSha256")}, "local snapshot base differs from frozen origin")
    parent = snapshot["parent"]
    if parent is not None:
        closed(parent, {"bundleId", "snapshotSha256", "transitionHeadSha256"}, "local snapshot parent")
    need(snapshot["consistencyMode"] == "frozen-origin-plus-receipt-chain", "local snapshot consistency mode is invalid")
    need(isinstance(snapshot["transitionReceipts"], list) and len(snapshot["transitionReceipts"]) <= 100, "local transition list is invalid")
    previous = None
    previous_completion = None
    saw_v2 = False
    expected_definitions: dict[str, dict[str, Any]] = {}
    expected_events: list[dict[str, Any]] = []
    expected_replacements: list[dict[str, Any]] = []
    ordinary_gate_candidates: dict[str, dict[str, Any]] = {}
    for index, record in enumerate(snapshot["transitionReceipts"]):
        record, receipt_data = verify_record(
            record, repo, f"local transition receipt {index}")
        receipt = parse_json(receipt_data,
                             f"local transition receipt {index}")
        need(document(receipt) == receipt_data,
             f"local transition receipt {index} is not canonical JSON")
        receipt_version = receipt.get("schemaVersion")
        need(receipt_version in {1, 2},
             f"local transition receipt {index} version is invalid")
        if saw_v2:
            need(receipt_version == 2,
                 "local receipt chain cannot downgrade from v2")
        saw_v2 = saw_v2 or receipt_version == 2
        is_recovery = receipt_version == 2 and "taskDefinition" in receipt
        recovery_fields = {
            "taskDefinition", "authorityArtifact", "gateEvent",
            "dependencyReplacements",
        }
        need(not (set(receipt) & recovery_fields)
             or recovery_fields <= set(receipt),
             f"local transition receipt {index} recovery fields are partial")
        if receipt_version == 1:
            event = implicit_v1_gate_event(receipt, record["sha256"])
            if event is not None:
                expected_events.append(event)
        elif is_recovery:
            definition = validate_recovery_task_definition(
                receipt.get("taskDefinition"),
                f"local transition receipt {index} task definition")
            need(receipt.get("taskId") == definition["taskId"]
                 and receipt.get("displayName") == definition["displayName"],
                 f"local transition receipt {index} task definition differs")
            need(definition["taskId"] not in origin["tasks"]
                 and definition["taskId"] not in expected_definitions,
                 f"local transition receipt {index} recovery task is not new")
            authority_record, authority_data = verify_record(
                receipt.get("authorityArtifact"), repo,
                f"local transition receipt {index} authority")
            authority = parse_json(
                authority_data,
                f"local transition receipt {index} authority")
            closed(authority, {
                "documentKind", "schemaVersion", "ledgerId",
                "parentBundleId", "parentSnapshotSha256",
                "taskDefinitionSha256", "authorizedAtUtc", "fixedPolicy",
            }, f"local transition receipt {index} authority")
            need(authority["documentKind"] == "local-recovery-task-authority"
                 and authority["schemaVersion"] == 1
                 and authority["ledgerId"] == LEDGER_ID
                 and authority["parentBundleId"] == receipt.get("parentBundleId")
                 and authority["parentSnapshotSha256"] == receipt.get("parentSnapshotSha256")
                 and authority["taskDefinitionSha256"] == digest(payload(definition))
                 and authority["authorizedAtUtc"] == definition["createdAtUtc"]
                 and authority["fixedPolicy"] == POLICY
                 and document(authority) == authority_data,
                 f"local transition receipt {index} authority is not bound")
            stored = {**definition, "authorityArtifact": authority_record}
            expected_definitions[definition["taskId"]] = stored
            event = validate_gate_event(
                receipt.get("gateEvent"), definition,
                f"local transition receipt {index} Gate event")
            if event is not None:
                need(event["gateVerdict"]
                     == (GATE_PASS if receipt.get("toStatus") == "completed"
                         else GATE_FAIL),
                     f"local transition receipt {index} Gate verdict differs")
                maybe_bridge_gate_predecessor(
                    event, definition, expected_events,
                    ordinary_gate_candidates,
                    f"local transition receipt {index}")
                expected_events.append({
                    "eventId": record["sha256"], "taskId": definition["taskId"],
                    **event,
                })
            replacements = receipt.get("dependencyReplacements")
            need(isinstance(replacements, list),
                 f"local transition receipt {index} replacements are invalid")
            if event is not None and event["gateVerdict"] != GATE_PASS:
                need(not replacements,
                     f"local transition receipt {index} failed Gate cannot declare dependency replacements")
            for replacement in replacements:
                expected_replacements.append({
                    "eventId": record["sha256"], **replacement})
        else:
            event = ordinary_schema2_failed_gate_event(
                receipt, record["sha256"], origin["tasks"])
            if event is not None:
                ordinary_gate_candidates[record["sha256"]] = event
        completion = utc(receipt.get("completedAtUtc"),
                         f"local transition receipt {index} completion")
        if previous_completion is not None:
            need(epoch(completion) > epoch(previous_completion),
                 "local transition completion time is not strictly increasing")
        if previous is not None:
            need(record["sha256"] != previous, "local transition receipt is duplicated")
        previous = record["sha256"]
        previous_completion = completion
    need(snapshot["transitionHeadSha256"] == previous, "local transition head does not match list")
    sealed_at = utc(snapshot["sealedAtUtc"], "local snapshot seal time")
    need(previous_completion is not None and sealed_at == previous_completion,
         "local snapshot seal time does not match its final receipt")
    need(isinstance(snapshot["taskOverrides"], dict), "local task overrides are invalid")
    for task_id, override in snapshot["taskOverrides"].items():
        closed(override, {"status", "completedAtUtc", "completionEvidence"}, f"local override {task_id}")
        need(override["status"] in TERMINAL_STATES and override["completionEvidence"]["sourceKind"] == "local-automatic-review", f"local override {task_id} is not a terminal local transition")
    if version == 1:
        need(not saw_v2, "v1 snapshot contains a v2 receipt")
    else:
        need(saw_v2, "v2 snapshot contains no v2 receipt")
        need(snapshot["recoveryTaskDefinitions"] == expected_definitions,
             "local recovery task definitions differ from receipt replay")
        need(snapshot["gateEvents"] == expected_events,
             "local Gate events differ from receipt replay")
        need(snapshot["dependencyReplacements"] == expected_replacements,
             "local dependency replacements differ from receipt replay")
        latest_gate_events(expected_events)
        effective_tasks(origin, snapshot["taskOverrides"], repo,
                        expected_definitions, expected_events,
                        expected_replacements)
    body = {key: snapshot[key] for key in keys if key != "revisionSha256"}
    need(snapshot["revisionSha256"] == digest(payload(body)), "local snapshot revision does not recompute")


def validate_local_state(
    state: dict[str, Any], snapshot: dict[str, Any], origin: dict[str, Any],
    repo: Path, pointer: dict[str, Any], tasks: list[dict[str, Any]],
    gate_events: list[dict[str, Any]],
    recovery_definitions: dict[str, dict[str, Any]],
) -> None:
    keys = {"documentKind", "schemaVersion", "ledgerId", "sourceKind", "basePointerSha256", "baseBundleId", "baseStateSha256", "baseSnapshotSha256", "snapshotSha256", "transitionHeadSha256", "legacyOrigin", "policy", "tasks", "phases", "nextRunnableTaskIds", "publication"}
    closed(state, keys, "local state")
    need(state["documentKind"] == "local-progress-ledger-state"
         and state["schemaVersion"] == snapshot["schemaVersion"]
         and state["ledgerId"] == LEDGER_ID
         and state["sourceKind"] == "local-auto-review",
         "local state identity is invalid")
    need(state["basePointerSha256"] == origin["pointerSha256"] and state["baseBundleId"] == origin["bundleId"] and state["baseStateSha256"] == origin["stateSha256"] and state["baseSnapshotSha256"] == origin["snapshotSha256"], "local state base differs")
    need(state["snapshotSha256"] == pointer["snapshotSha256"] and state["transitionHeadSha256"] == snapshot["transitionHeadSha256"], "local state snapshot identity differs")
    need(state["legacyOrigin"] == {"sourceKind": "legacy-orca", "pointerSha256": origin["pointerSha256"], "bundleId": origin["bundleId"], "stateSha256": origin["stateSha256"], "snapshotSha256": origin["snapshotSha256"]}, "legacy origin record differs")
    need(state["policy"] == POLICY, "local fixed policy changed")
    for task in state["tasks"]:
        closed(task, {"taskId", "displayName", "status", "dependencies", "createdAtUtc", "completedAtUtc", "completionEvidence"}, "local state task")
        assert_no_banned_keys(task, "local state task")
    need(state["nextRunnableTaskIds"] == sorted(state["nextRunnableTaskIds"]), "local runnable task order is not canonical")
    expected = sorted(task["taskId"] for task in state["tasks"] if task["status"] == "ready")
    need(state["nextRunnableTaskIds"] == expected, "local runnable task projection differs")
    need(state["tasks"] == tasks, "local task projection differs")
    expected_phases = derive_phases(
        origin, tasks, gate_events if snapshot["schemaVersion"] == 2 else [],
        recovery_definitions)
    need(state["phases"] == expected_phases, "local phase projection differs")


def assert_no_banned_keys(value: Any, label: str) -> None:
    if isinstance(value, dict):
        for key, item in value.items():
            need(key not in BANNED_LOCAL_KEYS, f"{label} contains forbidden legacy key {key}")
            assert_no_banned_keys(item, label)
    elif isinstance(value, list):
        for item in value:
            assert_no_banned_keys(item, label)


def effective_tasks(
    origin: dict[str, Any],
    overrides: dict[str, Any],
    repo: Path,
    recovery_definitions: dict[str, dict[str, Any]] | None = None,
    gate_events: list[dict[str, Any]] | None = None,
    dependency_replacements: list[dict[str, Any]] | None = None,
) -> list[dict[str, Any]]:
    del repo
    tasks = {task_id: legacy_task_view(task_id, task)
             for task_id, task in origin["tasks"].items()}
    definitions = recovery_definitions or {}
    for task_id, definition in definitions.items():
        need(task_id == definition["taskId"],
             f"recovery task definition key differs for {task_id}")
        need(task_id not in tasks, f"recovery task {task_id} is not new")
        tasks[task_id] = recovery_task_view(definition)
    check_task_graph(tasks)
    for task_id, override in overrides.items():
        need(task_id in tasks, f"local override names unknown task {task_id}")
        evidence = override["completionEvidence"]
        tasks[task_id] = {**tasks[task_id], "status": override["status"], "completedAtUtc": utc(override["completedAtUtc"], f"local task {task_id} completion"), "completionEvidence": evidence}
    active_replacements = active_dependency_replacements(
        gate_events or [], dependency_replacements or [])
    # A successful recovery integration is an explicit retry of one failed
    # materialized task.  Keep the failed historical row immutable, but allow
    # descendants to depend on the completed replacement task.  Only the
    # integration role may use this bridge, and only for a failed target; a
    # successful task can never be silently superseded.
    recovery_replacements: dict[str, str] = {}
    for recovery_id, definition in definitions.items():
        superseded_id = definition.get("supersedesTaskId")
        if (definition.get("role") == "integration"
                and definition.get("phase") == "P8"
                and isinstance(definition.get("displayName"), str)
                and definition["displayName"].endswith("-R1")
                and isinstance(superseded_id, str)
                and tasks.get(recovery_id, {}).get("status") == "completed"
                and tasks.get(superseded_id, {}).get("status") == "failed"):
            need(superseded_id not in recovery_replacements,
                 f"multiple recovery integrations supersede {superseded_id}")
            recovery_replacements[superseded_id] = recovery_id
    changed = True
    while changed:
        changed = False
        for task in tasks.values():
            if task["status"] != "pending":
                continue
            dependency_states = [
                "completed"
                if (task["taskId"], dependency) in active_replacements
                or dependency in recovery_replacements
                else tasks[dependency]["status"]
                for dependency in task["dependencies"]
                if dependency in tasks
            ]
            if dependency_states and all(value == "completed" for value in dependency_states):
                task["status"] = "ready"
                changed = True
            elif any(value in {"failed", "blocked"} for value in dependency_states):
                task["status"] = "blocked"
                changed = True
    return sorted(tasks.values(), key=lambda row: (row["createdAtUtc"], row["taskId"]))


def stage_receipt_artifact(
    value: Any, name: str, roots: list[Path], repo: Path, stage: Path,
    expected_suffix: str,
) -> tuple[dict[str, Any], bytes]:
    record = closed(value, {"path", "bytes", "sha256"},
                    f"local receipt {name}")
    external = resolve_external(record["path"], roots + [repo],
                                f"local receipt {name}")
    data = read_file(external, f"local receipt {name}")
    need(len(data) == record["bytes"] and digest(data) == record["sha256"],
         f"local receipt {name} bytes/hash differ")
    need(external.suffix.lower() == expected_suffix,
         f"local receipt {name} must be {expected_suffix}")
    cas = digest(data)
    destination = stage / "evidence" / cas
    destination.parent.mkdir(parents=True, exist_ok=True)
    if destination.exists():
        need(read_file(destination, "staged local artifact") == data,
             "staged local artifact collision")
    else:
        destination.write_bytes(data)
    return ({"path": (EVIDENCE_REL / cas).as_posix(),
             "bytes": len(data), "sha256": cas}, data)


def validate_receipt_draft(value: Any, origin: dict[str, Any], parent: dict[str, Any] | None, repo: Path, roots: list[Path], stage: Path) -> tuple[dict[str, Any], str]:
    version = value.get("schemaVersion") if isinstance(value, dict) else None
    need(version in {1, 2}, "local receipt version is invalid")
    keys = {"documentKind", "schemaVersion", "ledgerId", "parentBundleId", "parentSnapshotSha256", "previousTransitionSha256", "taskId", "displayName", "beforeTaskSha256", "fromStatus", "toStatus", "outcome", "completedAtUtc", "reviewAuthority", "reviewExecutionOutcome", "report", "verdictArtifact", "fixedPolicy"}
    recovery_fields = {"taskDefinition", "authorityArtifact", "gateEvent",
                       "dependencyReplacements"}
    present_recovery_fields = set(value) & recovery_fields
    need(not present_recovery_fields
         or (version == 2 and present_recovery_fields == recovery_fields),
         "local receipt recovery fields are partial or invalid")
    is_recovery = bool(present_recovery_fields)
    if is_recovery:
        keys |= recovery_fields
    value = closed(value, keys, "local task receipt draft")
    need(value["documentKind"] == "local-task-transition-receipt"
         and value["ledgerId"] == LEDGER_ID,
         "local receipt identity is invalid")
    if parent and parent["snapshot"]["schemaVersion"] == 2:
        need(version == 2, "local receipt chain cannot downgrade from v2")
    parent_bundle = parent["bundleId"] if parent else origin["bundleId"]
    need(value["parentBundleId"] == parent_bundle and value["parentSnapshotSha256"] == (parent["pointer"]["snapshotSha256"] if parent else origin["snapshotSha256"]), "local receipt parent differs")
    previous_head = parent["snapshot"]["transitionHeadSha256"] if parent else None
    need(value["previousTransitionSha256"] == previous_head, "local receipt predecessor differs")
    need(value["toStatus"] in TERMINAL_STATES and value["fromStatus"] == "ready" and value["outcome"] == ("succeeded" if value["toStatus"] == "completed" else "failed"), "local receipt state transition is invalid")
    need(value["reviewAuthority"] == "local-automatic-review" and value["reviewExecutionOutcome"] == "succeeded" and value["fixedPolicy"] == POLICY, "local receipt authority/policy is invalid")
    completed = utc(value["completedAtUtc"], "local receipt completion")
    parent_completed = utc(
        parent["snapshot"]["sealedAtUtc"]
        if parent else origin["snapshot"]["capturedAtUtc"],
        "selected parent snapshot time",
    )
    need(epoch(completed) > epoch(parent_completed),
         "local receipt completion is not newer than its parent snapshot")
    prior_definitions = dict(parent["recoveryDefinitions"]) if parent else {}
    prior_events = list(parent["gateEvents"]) if parent else []
    prior_replacements = list(parent["dependencyReplacements"]) if parent else []
    ordinary_gate_candidates = collect_ordinary_gate_bridge_events(
        list(parent.get("transitions", [])) if parent else [], repo,
        origin["tasks"])
    definitions = dict(prior_definitions)
    definition = None
    gate_event = None
    replacements: list[dict[str, Any]] = []
    sealed_authority = None
    if is_recovery:
        need(parent is not None,
             "a recovery task requires an existing local parent")
        definition = validate_recovery_task_definition(
            value["taskDefinition"], "local recovery task definition")
        need(definition["taskId"] == value["taskId"]
             and definition["displayName"] == value["displayName"],
             "local recovery task definition differs from receipt")
        need(definition["taskId"] not in origin["tasks"]
             and definition["taskId"] not in definitions,
             "local recovery task ID is not new")
        need(epoch(definition["createdAtUtc"]) > epoch(parent_completed),
             "local recovery task creation is not newer than its parent")
        need(epoch(completed) > epoch(definition["createdAtUtc"]),
             "local recovery task completion is not newer than creation")
        prior_tasks = {row["taskId"]: row for row in effective_tasks(
            origin, parent["overrides"], repo, prior_definitions,
            prior_events, prior_replacements)}
        need(all(dependency in prior_tasks
                 for dependency in definition["dependencies"]),
             "local recovery task names an unknown dependency")
        superseded = prior_tasks.get(definition["supersedesTaskId"])
        if definition["supersedesTaskId"] is not None:
            need(superseded is not None
                 and superseded["status"] in TERMINAL_STATES,
                 "local recovery task does not supersede a terminal task")
            if (definition["role"] == "integration"
                    and definition["phase"] == "P8"
                    and definition["displayName"].endswith("-R1")):
                need(superseded["status"] == "failed",
                     "local recovery integration must supersede a failed task")
                need(definition["supersedesTaskId"] not in definition["dependencies"],
                     "local recovery integration cannot depend on its superseded task")
        sealed_authority, authority_data = stage_receipt_artifact(
            value["authorityArtifact"], "authorityArtifact", roots, repo,
            stage, ".json")
        authority = parse_json(authority_data, "local recovery authority")
        closed(authority, {
            "documentKind", "schemaVersion", "ledgerId", "parentBundleId",
            "parentSnapshotSha256", "taskDefinitionSha256",
            "authorizedAtUtc", "fixedPolicy",
        }, "local recovery authority")
        need(authority["documentKind"] == "local-recovery-task-authority"
             and authority["schemaVersion"] == 1
             and authority["ledgerId"] == LEDGER_ID
             and authority["parentBundleId"] == value["parentBundleId"]
             and authority["parentSnapshotSha256"] == value["parentSnapshotSha256"]
             and authority["taskDefinitionSha256"] == digest(payload(definition))
             and authority["authorizedAtUtc"] == definition["createdAtUtc"]
             and authority["fixedPolicy"] == POLICY
             and document(authority) == authority_data,
             "local recovery authority is not bound")
        definitions[definition["taskId"]] = {
            **definition, "authorityArtifact": sealed_authority}
        gate_event = validate_gate_event(
            value["gateEvent"], definition, "local recovery Gate event")
        if gate_event is not None:
            need(gate_event["gateVerdict"]
                 == (GATE_PASS if value["toStatus"] == "completed"
                     else GATE_FAIL),
                 "local recovery Gate verdict differs from task outcome")
            maybe_bridge_gate_predecessor(
                gate_event, definition, prior_events,
                ordinary_gate_candidates,
                "local recovery Gate event")
        replacements = validate_dependency_replacement_drafts(
            value["dependencyReplacements"], definition, gate_event,
            prior_tasks, prior_events, "local dependency replacements")
    tasks = effective_tasks(
        origin, parent["overrides"] if parent else {}, repo, definitions,
        prior_events, prior_replacements)
    task = next((row for row in tasks if row["taskId"] == value["taskId"]), None)
    need(task is not None and task["displayName"] == value["displayName"] and task["status"] == value["fromStatus"], "local receipt task is unknown, stale, or not ready")
    need(value["beforeTaskSha256"] == task_hash(task), "local receipt before-task hash differs")
    if task["completedAtUtc"]:
        need(epoch(completed) > epoch(task["completedAtUtc"]), "local receipt completion is not newer than task state")
    sealed_report, _ = stage_receipt_artifact(
        value["report"], "report", roots, repo, stage, ".md")
    sealed_verdict, verdict_data = stage_receipt_artifact(
        value["verdictArtifact"], "verdictArtifact", roots, repo, stage,
        ".json")
    verdict = parse_json(verdict_data, "local verdict")
    verdict_keys = {"documentKind", "schemaVersion", "ledgerId", "parentBundleId", "parentSnapshotSha256", "taskId", "displayName", "beforeTaskSha256", "decision", "outcome", "reportSha256", "fixedPolicy", "reviewAuthority", "reviewExecutionOutcome"}
    if is_recovery:
        verdict_keys |= {"taskDefinitionSha256", "authoritySha256",
                         "gateEvent", "dependencyReplacementsSha256"}
    verdict = closed(verdict, verdict_keys, "local task verdict")
    need(verdict["documentKind"] == "local-task-verdict"
         and verdict["schemaVersion"] == version
         and verdict["ledgerId"] == LEDGER_ID,
         "local verdict identity is invalid")
    need(verdict["parentBundleId"] == value["parentBundleId"] and verdict["parentSnapshotSha256"] == value["parentSnapshotSha256"] and verdict["taskId"] == value["taskId"] and verdict["displayName"] == value["displayName"] and verdict["beforeTaskSha256"] == value["beforeTaskSha256"] and verdict["decision"] == value["toStatus"] and verdict["outcome"] == value["outcome"] and verdict["reportSha256"] == sealed_report["sha256"] and verdict["fixedPolicy"] == POLICY and verdict["reviewAuthority"] == "local-automatic-review" and verdict["reviewExecutionOutcome"] == "succeeded", "local verdict is not bound to receipt")
    if is_recovery:
        assert definition is not None and sealed_authority is not None
        need(verdict["taskDefinitionSha256"] == digest(payload(definition))
             and verdict["authoritySha256"] == sealed_authority["sha256"]
             and verdict["gateEvent"] == gate_event
             and verdict["dependencyReplacementsSha256"]
             == digest(payload(replacements)),
             "local recovery verdict is not bound to receipt")
    sealed = {**value, "completedAtUtc": completed,
              "report": sealed_report, "verdictArtifact": sealed_verdict}
    if is_recovery:
        sealed["authorityArtifact"] = sealed_authority
    sealed_data = document(sealed)
    receipt_sha = digest(sealed_data)
    receipt_path = stage / "evidence" / receipt_sha
    receipt_path.write_bytes(sealed_data)
    return sealed, receipt_sha


def phase_from_label(label: str) -> str | None:
    match = re.match(r"^[PG](10|[0-9])(?:\b|-)", label)
    return f"P{match.group(1)}" if match else None


def derive_phases(
    origin: dict[str, Any], tasks: list[dict[str, Any]],
    local_gate_events: list[dict[str, Any]] | None = None,
    recovery_definitions: dict[str, dict[str, Any]] | None = None,
) -> list[dict[str, Any]]:
    latest: dict[str, dict[str, Any]] = {}
    rows = origin["state"].get("gateHistory", {}).get("rows", [])
    receipt_hashes = origin["state"].get("gateHistory", {}).get("receiptSha256s", [])
    need(len(rows) == len(receipt_hashes), "legacy Gate row/receipt columns differ")
    for index, row in enumerate(rows):
        latest[row["phase"]] = {**row, "eventId": receipt_hashes[index]}
    local_events = local_gate_events or []
    for phase, event in latest_gate_events(local_events).items():
        latest[phase] = {
            "phase": phase,
            "gateVerdict": event["gateVerdict"],
            "gateTaskId": event["taskId"],
            "eventId": event["eventId"],
        }
    by_phase = {f"P{i}": [] for i in range(11)}
    definition_phases = {
        task_id: definition["phase"]
        for task_id, definition in (recovery_definitions or {}).items()
    }
    for task in tasks:
        phase = definition_phases.get(
            task["taskId"], phase_from_label(task["displayName"]))
        if phase:
            by_phase[phase].append(task["taskId"])
    result = []
    for index in range(11):
        phase = f"P{index}"
        gate_row = latest.get(phase)
        verdict = gate_row.get("gateVerdict") if gate_row else None
        if verdict and (verdict.endswith("ENGINEERING PASS") or verdict.endswith("READINESS: READY")):
            engineering, execution = "PASS", "GATED"
            summary = f"{phase} engineering Gate is accepted; release policy remains blocked."
        elif verdict:
            engineering, execution = "FAIL/BLOCKED", "BLOCKED"
            summary = f"{phase} Gate is FAIL/BLOCKED and retained in immutable Gate history."
        else:
            engineering, execution = "NOT_RUN", "MATERIALIZED_PENDING"
            summary = f"{phase} is materialized; no local Gate receipt exists."
        history = [receipt_hashes[i] for i, row in enumerate(rows)
                   if row.get("phase") == phase]
        history.extend(event["eventId"] for event in local_events
                       if event["phase"] == phase)
        result.append({"phase": phase, "executionStatus": execution,
                       "engineeringGate": engineering,
                       "gateTaskId": gate_row.get("gateTaskId")
                       if gate_row else None,
                       "gateHistoryReceiptSha256s": history,
                       "taskIds": sorted(by_phase[phase]), "summary": summary})
    return result


def build_local(origin: dict[str, Any], parent: dict[str, Any] | None, sealed: dict[str, Any], receipt_sha: str, receipt_record: dict[str, Any], repo: Path) -> tuple[dict[str, Any], dict[str, Any], bytes, bytes, bytes]:
    parent_snapshot = parent["snapshot"] if parent else None
    prior_receipts = list(parent_snapshot["transitionReceipts"]) if parent_snapshot else []
    prior_overrides = dict(parent_snapshot["taskOverrides"]) if parent_snapshot else {}
    need(not prior_receipts or prior_receipts[-1]["sha256"] == sealed["previousTransitionSha256"], "local receipt chain predecessor is not the selected parent")
    transition_record = {"path": (EVIDENCE_REL / receipt_sha).as_posix(), "bytes": receipt_record["bytes"], "sha256": receipt_sha}
    overrides = dict(prior_overrides)
    overrides[sealed["taskId"]] = {"status": sealed["toStatus"], "completedAtUtc": sealed["completedAtUtc"], "completionEvidence": {"sourceKind": "local-automatic-review", "receiptSha256": receipt_sha, "outcome": sealed["outcome"], "reportSha256": sealed["report"]["sha256"], "verdictSha256": sealed["verdictArtifact"]["sha256"]}}
    version = sealed["schemaVersion"]
    definitions = dict(parent["recoveryDefinitions"]) if parent else {}
    gate_events = list(parent["gateEvents"]) if parent else []
    replacements = list(parent["dependencyReplacements"]) if parent else []
    ordinary_gate_candidates = collect_ordinary_gate_bridge_events(
        list(parent.get("transitions", [])) if parent else [], repo,
        origin["tasks"])
    if version == 2 and "taskDefinition" in sealed:
        definition = sealed["taskDefinition"]
        definitions[definition["taskId"]] = {
            **definition, "authorityArtifact": sealed["authorityArtifact"]}
        if sealed["gateEvent"] is not None:
            maybe_bridge_gate_predecessor(
                sealed["gateEvent"], definition, gate_events,
                ordinary_gate_candidates,
                "local recovery Gate event")
            gate_events.append({
                "eventId": receipt_sha, "taskId": sealed["taskId"],
                **sealed["gateEvent"],
            })
        replacements.extend(
            {"eventId": receipt_sha, **replacement}
            for replacement in sealed["dependencyReplacements"])
        latest_gate_events(gate_events)
    body = {
        "documentKind": "local-progress-snapshot", "schemaVersion": version, "ledgerId": LEDGER_ID,
        "base": {"pointerSha256": origin["pointerSha256"], "bundleId": origin["bundleId"], "stateSha256": origin["stateSha256"], "snapshotSha256": origin["snapshotSha256"], "revisionSha256": origin["snapshot"].get("revisionSha256")},
        "parent": {"bundleId": parent["bundleId"], "snapshotSha256": parent["pointer"]["snapshotSha256"], "transitionHeadSha256": parent_snapshot["transitionHeadSha256"]} if parent else None,
        "sealedAtUtc": sealed["completedAtUtc"], "consistencyMode": "frozen-origin-plus-receipt-chain",
        "transitionReceipts": prior_receipts + [transition_record], "transitionHeadSha256": receipt_sha, "taskOverrides": overrides,
    }
    if version == 2:
        body.update({
            "recoveryTaskDefinitions": definitions,
            "gateEvents": gate_events,
            "dependencyReplacements": replacements,
        })
    snapshot = {**body, "revisionSha256": digest(payload(body))}
    snapshot_data = document(snapshot)
    tasks = effective_tasks(
        origin, overrides, repo, definitions, gate_events, replacements)
    phases = derive_phases(
        origin, tasks, gate_events if version == 2 else [], definitions)
    state = {
        "documentKind": "local-progress-ledger-state", "schemaVersion": version, "ledgerId": LEDGER_ID, "sourceKind": "local-auto-review",
        "basePointerSha256": origin["pointerSha256"], "baseBundleId": origin["bundleId"], "baseStateSha256": origin["stateSha256"], "baseSnapshotSha256": origin["snapshotSha256"],
        "snapshotSha256": digest(snapshot_data), "transitionHeadSha256": receipt_sha,
        "legacyOrigin": {"sourceKind": "legacy-orca", "pointerSha256": origin["pointerSha256"], "bundleId": origin["bundleId"], "stateSha256": origin["stateSha256"], "snapshotSha256": origin["snapshotSha256"]},
        "policy": POLICY, "tasks": tasks, "phases": phases, "nextRunnableTaskIds": sorted(task["taskId"] for task in tasks if task["status"] == "ready"),
        "publication": {"ledgerPath": LOCAL_MARKDOWN_REL.as_posix(), "pointerPath": LOCAL_POINTER_REL.as_posix(), "storePath": LOCAL_STORE_REL.as_posix(), "sourceKind": "local-auto-review"},
    }
    state_data = document(state)
    markdown = render_markdown(state, snapshot)
    return snapshot, state, snapshot_data, state_data, markdown


def render_markdown(state: dict[str, Any], snapshot: dict[str, Any]) -> bytes:
    lines = ["# glTF Scene Editor Parity Local Progress", "", "> Local receipt projection. The legacy Orca ledger is a frozen origin and is not an active executor.", "", "## Policy", "", "- F42: `BLOCKED`", "- R24: `BLOCKED`", "- Metal: `Not Verified`", "- release/distribution: `NOT PASS`", "", "## Runnable Tasks", "", "| Task | Status | Dependencies | Completion Evidence |", "|---|---|---|---|"]
    for task in state["tasks"]:
        evidence = task["completionEvidence"]
        evidence_text = "" if evidence is None else f"{evidence['sourceKind']}:{evidence.get('receiptSha256', evidence.get('legacyTaskSha256', ''))}"
        lines.append(f"| `{task['taskId']}` {task['displayName']} | `{task['status']}` | `{', '.join(task['dependencies'])}` | `{evidence_text}` |")
    lines.extend(["", "## Phase Gates", "", "| Phase | Execution | Gate | Ready Tasks |", "|---|---|---|---|"])
    for phase in state["phases"]:
        ready = ", ".join(task_id for task_id in state["nextRunnableTaskIds"] if task_id in phase["taskIds"])
        lines.append(f"| {phase['phase']} | {phase['executionStatus']} | {phase['engineeringGate']} | `{ready}` |")
    lines.extend(["", "## Receipt Chain", "", f"- Base legacy bundle: `{state['baseBundleId']}`", f"- Transition head: `{snapshot['transitionHeadSha256']}`", "", "| Receipt | Bytes |", "|---|---:|"])
    for record in snapshot["transitionReceipts"]:
        lines.append(f"| `{record['sha256']}` | {record['bytes']} |")
    lines.append("")
    return "\n".join(lines).encode("utf-8")


def make_bundle(repo: Path, state_data: bytes, snapshot_data: bytes, markdown: bytes) -> tuple[str, Path]:
    base = {"documentKind": "local-progress-ledger-bundle", "schemaVersion": 1, "stateSha256": digest(state_data), "snapshotSha256": digest(snapshot_data), "markdownSha256": digest(markdown)}
    bundle_id = digest(payload(base))
    descriptor = document({"bundleId": bundle_id, **base})
    target = repo / LOCAL_STORE_REL / bundle_id
    if target.exists():
        need(read_file(target / "bundle.json", "existing local descriptor") == descriptor and read_file(target / "state.json", "existing local state") == state_data and read_file(target / "snapshot.json", "existing local snapshot") == snapshot_data and read_file(target / "progress.md", "existing local Markdown") == markdown, "local bundle identity collision")
        return bundle_id, target
    target.parent.mkdir(parents=True, exist_ok=True)
    temporary = target.parent / f".{bundle_id}.{uuid.uuid4().hex}.tmp"
    temporary.mkdir()
    try:
        write_atomic(temporary / "state.json", state_data, "local bundle state")
        write_atomic(temporary / "snapshot.json", snapshot_data, "local bundle snapshot")
        write_atomic(temporary / "progress.md", markdown, "local bundle Markdown")
        write_atomic(temporary / "bundle.json", descriptor, "local bundle descriptor")
        os.replace(temporary, target)
    finally:
        if temporary.exists():
            shutil.rmtree(temporary)
    return bundle_id, target


def run_independent_validator(repo: Path, pointer: Path | None = None) -> None:
    command = [sys.executable, "-B", str(repo / LOCAL_VALIDATOR_REL), "--repo-root", str(repo), "--check"]
    if pointer is not None:
        command.extend(["--pointer", str(pointer)])
    run = subprocess.run(command, text=True, encoding="utf-8", capture_output=True, check=False)
    need(run.returncode == 0, f"independent local-ledger validation failed: {(run.stderr or run.stdout).strip()}")


def run_legacy_validator(repo: Path) -> None:
    # The legacy validator's external evidence contains Windows absolute
    # paths.  Run it on the Windows host where those identities are meaningful;
    # POSIX callers still pin the selected pointer and bundle byte-for-byte.
    if os.name != "nt":
        return
    command = [sys.executable, "-B", str(repo / LEGACY_VALIDATOR_REL), "--repo-root", str(repo), "--check"]
    run = subprocess.run(command, text=True, encoding="utf-8", capture_output=True, check=False)
    need(run.returncode == 0, f"frozen legacy-ledger validation failed: {(run.stderr or run.stdout).strip()}")


def captured_parent_pointer_sha(parent: dict[str, Any] | None) -> str | None:
    if parent is None:
        return None
    pointer_data = parent.get("pointerData")
    need(isinstance(pointer_data, bytes),
         "selected local parent pointer bytes are absent")
    selected = digest(pointer_data)
    need(parent.get("pointerSha256") == selected,
         "selected local parent pointer identity is inconsistent")
    return selected


def publish_local(repo: Path, origin: dict[str, Any], parent: dict[str, Any] | None, snapshot: dict[str, Any], state: dict[str, Any], snapshot_data: bytes, state_data: bytes, markdown: bytes, stage: Path, expected_pointer_sha: str | None) -> str:
    lock, token, _, guard = acquire_lock(repo)
    paths = publication_paths(repo)
    old = {name: preimage(path) for name, path in paths.items()}
    old_hashes = preimage_hashes(old, set(paths))
    new_hashes = None
    committed = False
    try:
        need(expected_pointer_sha == captured_parent_pointer_sha(parent),
             "local publication expected pointer is not bound to its parent")
        current = current_hash(paths["pointer"])
        if expected_pointer_sha is not None:
            need(current == expected_pointer_sha, "local pointer changed since receipt was prepared")
        else:
            need(current is None, "local pointer already exists; first publication requires an explicit parent")
            need(current_hash(repo / LEGACY_POINTER_REL) == origin["pointerSha256"], "legacy pointer changed before first local publication")
        for evidence in (stage / "evidence").iterdir():
            data = read_file(evidence, "staged local evidence")
            need(evidence.name == digest(data), "staged local evidence filename is not its hash")
            destination = repo / EVIDENCE_REL / evidence.name
            destination.parent.mkdir(parents=True, exist_ok=True)
            if destination.exists():
                need(read_file(destination, "existing local evidence") == data, "local evidence content collision")
            else:
                write_atomic(destination, data, "local evidence")
        bundle_id, bundle = make_bundle(repo, state_data, snapshot_data, markdown)
        pointer = {"documentKind": "local-progress-ledger-pointer", "schemaVersion": 1, "ledgerId": LEDGER_ID, "basePointerSha256": origin["pointerSha256"], "baseBundleId": origin["bundleId"], "baseStateSha256": origin["stateSha256"], "baseSnapshotSha256": origin["snapshotSha256"], "bundleId": bundle_id, "bundlePath": bundle.relative_to(repo).as_posix(), "stateSha256": digest(state_data), "snapshotSha256": digest(snapshot_data), "markdownSha256": digest(markdown)}
        pointer_data = document(pointer)
        new_hashes = {"pointer": digest(pointer_data),
                      "state": digest(state_data),
                      "ledger": digest(markdown)}
        require_publication_hashes(paths, old_hashes, "journal preparation")
        journal = {"documentKind": "local-progress-ledger-publication-journal", "schemaVersion": 1, "transactionId": token, "commitPoint": "pointer-replacement", "preimages": old, "newHashes": new_hashes}
        write_atomic(repo / JOURNAL_REL, document(journal), "local publication journal")
        require_publication_hashes(paths, old_hashes, "mirror preparation")
        write_atomic(paths["state"], state_data, "local state mirror")
        require_publication_hashes(
            paths,
            {"pointer": old_hashes["pointer"],
             "state": new_hashes["state"],
             "ledger": old_hashes["ledger"]},
            "Markdown preparation",
        )
        write_atomic(paths["ledger"], markdown, "local Markdown mirror")
        prepared_hashes = {"pointer": old_hashes["pointer"],
                           "state": new_hashes["state"],
                           "ledger": new_hashes["ledger"]}
        require_publication_hashes(paths, prepared_hashes,
                                   "candidate validation")
        validate_local_publication(repo, pointer)
        candidate_pointer = stage / "candidate-local-pointer.json"
        write_atomic(candidate_pointer, pointer_data, "candidate local pointer")
        run_independent_validator(repo, candidate_pointer)
        require_publication_hashes(paths, prepared_hashes,
                                   "pointer commit")
        write_atomic(paths["pointer"], pointer_data, "local pointer")
        require_publication_hashes(paths, new_hashes,
                                   "post-commit verification")
        committed = True
        run_independent_validator(repo)
        unlink_durable(repo / JOURNAL_REL)
        return bundle_id
    except BaseException as exc:
        pointer_committed = bool(
            new_hashes is not None and
            current_hash(paths["pointer"]) == new_hashes["pointer"])
        if committed or pointer_committed:
            try:
                require_publication_hashes(paths, new_hashes,
                                           "committed failure recovery")
            except BaseException as recovery_exc:
                raise LocalLedgerError(
                    "local pointer committed but publication files drifted; "
                    f"journal retained: {recovery_exc}") from exc
            # The exact candidate was independently validated before the
            # pointer commit. A redundant post-commit check cannot retract it.
            unlink_durable(repo / JOURNAL_REL)
            return bundle_id
        try:
            restore_preimages_guarded(paths, old, new_hashes)
        except BaseException as rollback_exc:
            raise LocalLedgerError(
                f"local publication failed and rollback refused: {rollback_exc}") from exc
        unlink_durable(repo / JOURNAL_REL)
        raise
    finally:
        release_lock(lock, token, guard)


def validate_local_publication(repo: Path, pointer: dict[str, Any] | None = None) -> dict[str, Any] | None:
    origin = legacy_origin(repo)
    local = load_local(repo, origin) if pointer is None else None
    if pointer is not None:
        bundle = repo / pointer["bundlePath"]
        state, _ = read_json(bundle / "state.json", "local candidate state")
        snapshot, _ = read_json(bundle / "snapshot.json", "local candidate snapshot")
        validate_local_snapshot(snapshot, origin, repo)
        definitions = snapshot.get("recoveryTaskDefinitions", {})
        events = snapshot.get("gateEvents", [])
        replacements = snapshot.get("dependencyReplacements", [])
        tasks = effective_tasks(
            origin, snapshot["taskOverrides"], repo, definitions, events,
            replacements)
        validate_local_state(
            state, snapshot, origin, repo, pointer, tasks, events,
            definitions)
        assert_no_banned_keys(snapshot, "local candidate snapshot")
        assert_no_banned_keys(state, "local candidate state")
        return state
    return local["state"] if local else None


def arguments() -> argparse.Namespace:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--repo-root", type=Path, default=Path(__file__).resolve().parents[1])
    parser.add_argument("--receipt", type=Path, action="append", default=[])
    parser.add_argument("--evidence-root", type=Path, action="append", default=[])
    parser.add_argument("--publish", action="store_true")
    parser.add_argument("--candidate-dir", type=Path)
    parser.add_argument("--check", action="store_true")
    parser.add_argument("--print-effective", action="store_true")
    return parser.parse_args()


def main() -> int:
    try:
        args = arguments()
        repo = lexical(args.repo_root)
        no_reparse(repo, "repository root")
        recovered = recover_local_publication(repo)
        if recovered:
            print("Recovered an interrupted local progress-ledger publication.")
        run_legacy_validator(repo)
        origin = legacy_origin(repo)
        if args.check or args.print_effective:
            local = load_local(repo, origin)
            if local is None:
                print("No local progress ledger has been published.")
            elif args.print_effective:
                print(json.dumps(local["tasks"], ensure_ascii=True, sort_keys=True, indent=2))
            else:
                run_independent_validator(repo)
                print(f"Local progress ledger is valid: bundle={local['bundleId']} head={local['snapshot']['transitionHeadSha256']}")
            return 0
        need(len(args.receipt) == 1, "exactly one --receipt is required")
        roots = [lexical(path) for path in args.evidence_root]
        need(roots, "--evidence-root is required for local receipt artifacts")
        parent = load_local(repo, origin)
        expected_pointer_sha = captured_parent_pointer_sha(parent)
        stage_root = Path(tempfile.mkdtemp(prefix="mgif-local-ledger-", dir=str(repo / "docs/editor-parity")))
        stage = stage_root / "stage"
        (stage / "evidence").mkdir(parents=True)
        try:
            draft, _ = read_json(lexical(args.receipt[0]), "local receipt draft")
            sealed, receipt_sha = validate_receipt_draft(draft, origin, parent, repo, roots, stage)
            receipt_data = document(sealed)
            receipt_record = {"path": (EVIDENCE_REL / receipt_sha).as_posix(), "bytes": len(receipt_data), "sha256": receipt_sha}
            snapshot, state, snapshot_data, state_data, markdown = build_local(origin, parent, sealed, receipt_sha, receipt_record, repo)
            candidate = args.candidate_dir
            if candidate:
                candidate = lexical(candidate)
                candidate.mkdir(parents=True, exist_ok=True)
                write_atomic(candidate / "snapshot.json", snapshot_data, "local candidate snapshot")
                write_atomic(candidate / "state.json", state_data, "local candidate state")
                write_atomic(candidate / "progress.md", markdown, "local candidate Markdown")
                for item in (stage / "evidence").iterdir():
                    write_atomic(candidate / "evidence" / item.name, read_file(item, "candidate evidence"), "candidate evidence")
                print(f"Built internally checked local candidate: {candidate}")
                return 0
            need(args.publish, "select --publish or --candidate-dir")
            bundle_id = publish_local(repo, origin, parent, snapshot, state, snapshot_data, state_data, markdown, stage, expected_pointer_sha)
            print(f"Published local progress bundle {bundle_id}: receipt={receipt_sha}")
            return 0
        finally:
            shutil.rmtree(stage_root, ignore_errors=True)
    except (LocalLedgerError, OSError, ValueError, UnicodeError) as exc:
        print(f"Local editor progress-ledger update failed: {exc}", file=os.sys.stderr)
        return 1


if __name__ == "__main__":
    raise SystemExit(main())
