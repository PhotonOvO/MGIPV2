#!/usr/bin/env python3
"""Independent validator for the post-Orca local editor progress sidecar."""

from __future__ import annotations

import argparse
import hashlib
import importlib.util
import json
import os
import re
import stat
from datetime import datetime, timezone
from pathlib import Path
from typing import Any


def _load_schema_helper() -> Any:
    helper_path = Path(__file__).resolve().with_name("json_schema_subset.py")
    module_name = "_mgif_local_json_schema_" + hashlib.sha256(os.fsencode(helper_path)).hexdigest()[:16]
    module = sys.modules.get(module_name)
    if module is not None:
        return module
    spec = importlib.util.spec_from_file_location(module_name, helper_path)
    if spec is None or spec.loader is None:
        raise ImportError(f"cannot load local progress schema helper from {helper_path}")
    module = importlib.util.module_from_spec(spec)
    sys.modules[module_name] = module
    spec.loader.exec_module(module)
    return module


import sys

_SCHEMA_HELPER = _load_schema_helper()
JsonSchemaError = _SCHEMA_HELPER.JsonSchemaError
JsonSchemaValidator = _SCHEMA_HELPER.JsonSchemaValidator


LEDGER_ID = "mgif-editor-parity"
LEGACY_POINTER_REL = Path("docs/editor-parity/progress-ledger-current.json")
LOCAL_POINTER_REL = Path("docs/editor-parity/progress-ledger-local-current.json")
LOCAL_STATE_REL = Path("docs/editor-parity/progress-ledger-local-state.json")
LOCAL_MARKDOWN_REL = Path("docs/gltf-scene-editor-parity-local-progress.md")
LOCAL_STORE_REL = Path("docs/editor-parity/progress-ledger/local-bundles/sha256")
EVIDENCE_REL = Path("docs/editor-parity/progress-ledger/evidence/sha256")
SCHEMA_REL = Path("docs/editor-parity/schemas/local-progress-ledger.schema.json")
POLICY = {"f42": "BLOCKED", "r24": "BLOCKED", "metal": "Not Verified", "releaseDistribution": "NOT PASS"}
TERMINAL_STATES = {"completed", "failed"}
RECOVERY_ROLES = {"repair", "integration", "gate", "reconciliation"}
GATE_PASS = "ENGINEERING PASS"
GATE_FAIL = "FAIL/BLOCKED"
SHA_RE = re.compile(r"^[0-9a-f]{64}$")
BANNED_KEYS = {"runtimeId", "runGeneration", "dispatch", "dispatchId", "workerResult", "workerMessageId", "workerMessages", "orcaResult", "orcaResults"}
_SCHEMA_VALIDATOR: JsonSchemaValidator | None = None


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


def no_reparse(path: Path, label: str) -> None:
    absolute = Path(os.path.abspath(os.fspath(path)))
    cursor = Path(absolute.anchor)
    for part in absolute.parts[1:]:
        cursor /= part
        if not os.path.lexists(cursor):
            continue
        info = os.lstat(cursor)
        insist(not stat.S_ISLNK(info.st_mode) and not (getattr(info, "st_file_attributes", 0) & 0x400),
                f"{label} traverses a reparse point: {cursor}")


def read_file(path: Path, label: str) -> bytes:
    no_reparse(path, label)
    insist(path.is_file(), f"{label} is not a regular file: {path}")
    return path.read_bytes()


def read_json(path: Path, label: str) -> tuple[Any, bytes]:
    data = read_file(path, label)
    try:
        value = json.loads(data.decode("utf-8"))
    except (UnicodeDecodeError, json.JSONDecodeError) as exc:
        raise ValidationError(f"{label} is not valid UTF-8 JSON: {exc}") from exc
    insist(canonical_document(value) == data, f"{label} is not canonical JSON")
    return value, data


def exact(value: Any, keys: set[str], label: str) -> dict[str, Any]:
    insist(isinstance(value, dict), f"{label} must be an object")
    actual = set(value)
    insist(actual == keys, f"{label} keys differ: missing={sorted(keys - actual)}, extra={sorted(actual - keys)}")
    return value


def schema_validate(value: Any, repo: Path, label: str) -> None:
    global _SCHEMA_VALIDATOR
    if _SCHEMA_VALIDATOR is None:
        schema, _ = read_json(repo / SCHEMA_REL, "local progress schema")
        try:
            _SCHEMA_VALIDATOR = JsonSchemaValidator(schema)
        except JsonSchemaError as exc:
            raise ValidationError(f"local progress schema is invalid at {exc.path}: {exc.message}") from exc
    try:
        _SCHEMA_VALIDATOR.validate(value)
    except JsonSchemaError as exc:
        raise ValidationError(f"{label} fails schema at {exc.path}: {exc.message}") from exc


def normalized_time(value: Any, label: str) -> str:
    insist(isinstance(value, str) and value.strip(), f"{label} must be a timestamp")
    text = value.strip().replace(" ", "T", 1)
    if text.endswith("Z"):
        text = text[:-1] + "+00:00"
    try:
        parsed = datetime.fromisoformat(text)
    except ValueError as exc:
        raise ValidationError(f"{label} is not ISO-8601") from exc
    if parsed.tzinfo is None:
        parsed = parsed.replace(tzinfo=timezone.utc)
    rendered = parsed.astimezone(timezone.utc).isoformat(timespec="milliseconds")
    return rendered[:-6] + "Z"


def reject_banned(value: Any, label: str) -> None:
    if isinstance(value, dict):
        for key, item in value.items():
            insist(key not in BANNED_KEYS, f"{label} contains forbidden legacy field {key}")
            reject_banned(item, label)
    elif isinstance(value, list):
        for item in value:
            reject_banned(item, label)


def artifact(record: Any, repo: Path, label: str) -> tuple[dict[str, Any], bytes]:
    record = exact(record, {"path", "bytes", "sha256"}, label)
    insist(isinstance(record["bytes"], int) and record["bytes"] > 0 and SHA_RE.fullmatch(record.get("sha256", "")),
            f"{label} identity is invalid")
    logical = Path(str(record["path"]).replace("\\", "/"))
    insist(not logical.is_absolute() and ".." not in logical.parts and logical == EVIDENCE_REL / record["sha256"],
            f"{label} path is not canonical content-addressed evidence")
    data = read_file(repo / logical, label)
    insist(len(data) == record["bytes"] and digest(data) == record["sha256"], f"{label} bytes/hash differ")
    return record, data


def load_legacy(repo: Path) -> dict[str, Any]:
    pointer, pointer_data = read_json(repo / LEGACY_POINTER_REL, "legacy pointer")
    exact(pointer, {"bundleId", "bundlePath", "documentKind", "markdownSha256", "schemaVersion", "snapshotSha256", "stateSha256"}, "legacy pointer")
    insist(pointer["documentKind"] == "progress-ledger-pointer" and pointer["schemaVersion"] == 2, "legacy pointer kind/version differs")
    bundle = repo / pointer["bundlePath"]
    descriptor, descriptor_data = read_json(bundle / "bundle.json", "legacy bundle descriptor")
    exact(descriptor, {"bundleId", "documentKind", "markdownSha256", "schemaVersion", "snapshotSha256", "stateSha256"}, "legacy bundle descriptor")
    base = {key: descriptor[key] for key in ("documentKind", "schemaVersion", "stateSha256", "markdownSha256", "snapshotSha256")}
    insist(descriptor["bundleId"] == pointer["bundleId"] == digest(canonical_payload(base)), "legacy bundle ID differs")
    insist(canonical_document(descriptor) == descriptor_data, "legacy bundle descriptor differs")
    state, state_data = read_json(bundle / "state.json", "legacy state")
    snapshot, snapshot_data = read_json(bundle / "snapshot.json", "legacy snapshot")
    markdown_data = read_file(bundle / "progress.md", "legacy Markdown")
    insist(digest(state_data) == pointer["stateSha256"] and digest(snapshot_data) == pointer["snapshotSha256"] and digest(markdown_data) == pointer["markdownSha256"], "legacy bundle artifact hash differs")
    insist(snapshot.get("documentKind") == "orca-progress-snapshot" and snapshot.get("schemaVersion") == 4, "legacy origin is not the frozen v4 snapshot")
    insist(state.get("documentKind") == "progress-ledger-state" and state.get("schemaVersion") == 4, "legacy origin state is not v4")
    state_tasks = state.get("tasks", {})
    initial_tasks = state.get("materialization", {}).get("initialTasks", {})
    insist(isinstance(initial_tasks, dict) and len(initial_tasks) == 33, "legacy materialized task identity map differs")
    normalized_tasks: dict[str, dict[str, Any]] = {}
    for task_id in initial_tasks:
        raw = snapshot["runTasks"].get(task_id)
        insist(isinstance(raw, dict), f"legacy materialized task {task_id} is absent")
        source = {**raw}
        projected = state_tasks.get(task_id) if isinstance(state_tasks, dict) else None
        if isinstance(projected, dict):
            for key in ("dependencies", "completedAtUtc"):
                if key not in source and key in projected:
                    source[key] = projected[key]
        source.setdefault("dependencies", [])
        source.setdefault("completedAtUtc", None)
        normalized_tasks[task_id] = source
    return {"pointer": pointer, "pointerSha256": digest(pointer_data), "bundleId": pointer["bundleId"], "stateSha256": pointer["stateSha256"], "snapshotSha256": pointer["snapshotSha256"], "state": state, "snapshot": snapshot, "tasks": normalized_tasks}


def task_view(task_id: str, task: dict[str, Any]) -> dict[str, Any]:
    evidence = None
    if task["status"] in {"completed", "failed"}:
        evidence = {"sourceKind": "legacy-orca", "legacyTaskSha256": digest(canonical_payload(task))}
    return {"taskId": task_id, "displayName": task["displayName"], "status": task["status"],
            "dependencies": sorted(task["dependencies"]), "createdAtUtc": normalized_time(task["createdAtUtc"], "task creation"),
            "completedAtUtc": normalized_time(task["completedAtUtc"], "task completion") if task["completedAtUtc"] else None,
            "completionEvidence": evidence}


def task_digest(task: dict[str, Any]) -> str:
    keys = ("taskId", "displayName", "status", "dependencies", "createdAtUtc", "completedAtUtc", "completionEvidence")
    return digest(canonical_payload({key: task[key] for key in keys}))


def gate_phase(display_name: str) -> str | None:
    match = re.fullmatch(r"G(10|[0-9])(?:-[A-Za-z0-9._:-]+)?", display_name)
    return f"P{match.group(1)}" if match else None


def recovery_task_view(definition: dict[str, Any]) -> dict[str, Any]:
    return {
        "taskId": definition["taskId"],
        "displayName": definition["displayName"],
        "status": "pending",
        "dependencies": sorted(definition["dependencies"]),
        "createdAtUtc": normalized_time(
            definition["createdAtUtc"],
            f"recovery task {definition['taskId']} creation"),
        "completedAtUtc": None,
        "completionEvidence": None,
    }


def latest_gate_events(events: list[dict[str, Any]]) -> dict[str, dict[str, Any]]:
    latest: dict[str, dict[str, Any]] = {}
    known: dict[str, dict[str, Any]] = {}
    for event in events:
        event_id = event["eventId"]
        insist(event_id not in known, f"duplicate local Gate event {event_id}")
        prior = event["supersedesEventId"]
        if prior is None:
            insist(event["phase"] not in latest,
                    "local Gate event omits required supersession")
        else:
            insist(prior in known,
                    "local Gate event supersedes an unknown event")
            insist(known[prior]["phase"] == event["phase"],
                    "local Gate event supersedes a different phase")
            insist(latest.get(event["phase"], {}).get("eventId") == prior,
                    "local Gate event does not supersede the latest event")
        known[event_id] = event
        latest[event["phase"]] = event
    return latest


def check_task_graph(tasks: dict[str, dict[str, Any]]) -> None:
    for task_id, task in tasks.items():
        insist(len(task["dependencies"]) == len(set(task["dependencies"])),
                f"task {task_id} has duplicate dependencies")
    visiting: set[str] = set()
    visited: set[str] = set()

    def visit(task_id: str) -> None:
        insist(task_id not in visiting,
                f"local task graph contains a cycle at {task_id}")
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


def derive_ready(
    tasks: dict[str, dict[str, Any]],
    gate_events: list[dict[str, Any]] | None = None,
    replacements: list[dict[str, Any]] | None = None,
    recovery_definitions: dict[str, dict[str, Any]] | None = None,
) -> None:
    check_task_graph(tasks)
    active = active_dependency_replacements(
        gate_events or [], replacements or [])
    recovery_replacements: dict[str, str] = {}
    for recovery_id, definition in (recovery_definitions or {}).items():
        superseded_id = definition.get("supersedesTaskId")
        if (definition.get("role") == "integration"
                and definition.get("phase") == "P8"
                and isinstance(definition.get("displayName"), str)
                and definition["displayName"].endswith("-R1")
                and isinstance(superseded_id, str)
                and tasks.get(recovery_id, {}).get("status") == "completed"
                and tasks.get(superseded_id, {}).get("status") == "failed"):
            insist(superseded_id not in recovery_replacements,
                   f"multiple recovery integrations supersede {superseded_id}")
            recovery_replacements[superseded_id] = recovery_id
    changed = True
    while changed:
        changed = False
        for row in tasks.values():
            if row["status"] != "pending":
                continue
            dependency_states = [
                "completed"
                if (row["taskId"], dependency) in active
                or dependency in recovery_replacements
                else tasks[dependency]["status"]
                for dependency in row["dependencies"]
                if dependency in tasks
            ]
            if dependency_states and all(value == "completed" for value in dependency_states):
                row["status"] = "ready"
                changed = True
            elif any(value in {"failed", "blocked"} for value in dependency_states):
                row["status"] = "blocked"
                changed = True


RECOVERY_TASK_KEYS = {
    "taskId", "displayName", "dependencies", "createdAtUtc", "phase",
    "role", "supersedesTaskId",
}


def validate_recovery_task_definition(value: Any, label: str) -> dict[str, Any]:
    definition = exact(value, RECOVERY_TASK_KEYS, label)
    task_id = definition["taskId"]
    insist(isinstance(task_id, str)
           and re.fullmatch(r"task_[A-Za-z0-9._:-]+", task_id),
           f"{label} task ID is invalid")
    insist(isinstance(definition["displayName"], str)
           and definition["displayName"].strip() == definition["displayName"]
           and definition["displayName"], f"{label} display name is invalid")
    dependencies = definition["dependencies"]
    insist(isinstance(dependencies, list)
           and bool(dependencies)
           and dependencies == sorted(dependencies)
           and len(dependencies) == len(set(dependencies))
           and all(isinstance(item, str) and item for item in dependencies),
           f"{label} dependencies are invalid")
    insist(task_id not in dependencies, f"{label} contains a self cycle")
    insist(isinstance(definition["phase"], str)
           and re.fullmatch(r"P(?:10|[0-9])", definition["phase"]),
           f"{label} phase is invalid")
    insist(definition["role"] in RECOVERY_ROLES, f"{label} role is invalid")
    insist(normalized_time(definition["createdAtUtc"], f"{label} creation")
           == definition["createdAtUtc"],
           f"{label} creation time is not canonical")
    supersedes = definition["supersedesTaskId"]
    insist(supersedes is None or (isinstance(supersedes, str) and supersedes),
           f"{label} superseded task is invalid")
    if definition["role"] == "gate":
        insist(gate_phase(definition["displayName"]) == definition["phase"],
               f"{label} Gate display name and phase differ")
        insist(supersedes is not None,
               f"{label} recovery Gate must supersede a prior task")
    return definition


def validate_gate_event(value: Any, definition: dict[str, Any], label: str) -> dict[str, Any] | None:
    if definition["role"] != "gate":
        insist(value is None, f"{label} is only valid for a Gate recovery task")
        return None
    event = exact(value, {"phase", "gateVerdict", "supersedesEventId"}, label)
    insist(event["phase"] == definition["phase"],
           f"{label} phase differs from its task")
    insist(event["gateVerdict"] in {GATE_PASS, GATE_FAIL},
           f"{label} verdict is invalid")
    insist(isinstance(event["supersedesEventId"], str)
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
    insist(isinstance(values, list), f"{label} must be an array")
    if definition["role"] != "gate":
        insist(not values, f"{label} is only valid for a Gate recovery task")
        return []
    assert gate_event is not None
    if gate_event["gateVerdict"] != GATE_PASS:
        insist(not values,
               f"{label} failed Gate cannot declare dependency replacements")
    known = {event["eventId"]: event for event in prior_events}
    superseded = known.get(gate_event["supersedesEventId"])
    insist(superseded is not None,
           f"{label} Gate supersedes an unknown event")
    insist(superseded["phase"] == gate_event["phase"],
           f"{label} Gate supersedes a different phase")
    insist(superseded["taskId"] == definition["supersedesTaskId"],
           f"{label} Gate event and superseded task differ")
    latest = latest_gate_events(prior_events)
    insist(latest.get(gate_event["phase"], {}).get("eventId")
           == gate_event["supersedesEventId"],
           f"{label} Gate does not supersede the latest event")
    result: list[dict[str, Any]] = []
    pairs: set[tuple[str, str]] = set()
    for index, raw in enumerate(values):
        item = exact(raw, {"taskId", "replacedDependencyTaskId",
                           "acceptedGate"}, f"{label} {index}")
        accepted = exact(item["acceptedGate"], {"phase", "gateVerdict"},
                         f"{label} {index} accepted Gate")
        insist(accepted == {"phase": gate_event["phase"],
                            "gateVerdict": GATE_PASS},
               f"{label} {index} accepted Gate predicate differs")
        target = tasks.get(item["taskId"])
        insist(target is not None, f"{label} {index} names an unknown task")
        insist(target["status"] not in TERMINAL_STATES,
               f"{label} {index} targets a terminal task")
        insist(item["replacedDependencyTaskId"] in target["dependencies"],
               f"{label} {index} does not replace an actual dependency")
        insist(item["replacedDependencyTaskId"] == superseded["taskId"],
               f"{label} {index} does not replace the superseded Gate task")
        pair = (item["taskId"], item["replacedDependencyTaskId"])
        insist(pair not in pairs, f"{label} contains a duplicate replacement")
        pairs.add(pair)
        result.append(item)
    return result


def implicit_v1_gate_event(receipt: dict[str, Any], receipt_sha: str) -> dict[str, Any] | None:
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


# A few early schema-2 local transitions used the ordinary receipt shape for a
# Gate and therefore have no stored Gate event.  Preserve those old snapshots;
# only an explicit later recovery Gate that names the exact receipt hash may
# materialize a virtual predecessor.  The bridge is restricted to the frozen
# materialized Gate task and a terminal failure so an ordinary success can never
# silently promote a phase.
_BRIDGE_RECOVERY_FIELDS = frozenset({
    "taskDefinition", "authorityArtifact", "gateEvent",
    "dependencyReplacements",
})


def ordinary_schema2_failed_gate_event(
    receipt: Any,
    receipt_sha: str,
    origin_tasks: dict[str, dict[str, Any]],
) -> dict[str, Any] | None:
    """Return the narrowly-compatible virtual event for an old Gate receipt."""
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
        if digest(canonical_document(receipt)) != receipt_sha:
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


def maybe_bridge_gate_predecessor(
    gate_event: dict[str, Any],
    definition: dict[str, Any],
    prior_events: list[dict[str, Any]],
    candidates: dict[str, dict[str, Any]],
    label: str,
) -> None:
    """Append a virtual predecessor only for an explicitly named candidate."""
    reference = gate_event["supersedesEventId"]
    if any(event["eventId"] == reference for event in prior_events):
        return
    candidate = candidates.get(reference)
    if candidate is None:
        # The ordinary validator below retains the existing unknown-event
        # failure for all references that are not bridgeable.
        return
    insist(candidate["phase"] == gate_event["phase"],
           f"{label} ordinary Gate bridge phase differs")
    insist(candidate["taskId"] == definition["supersedesTaskId"],
           f"{label} ordinary Gate bridge task differs")
    insist(not any(event["phase"] == candidate["phase"]
                   for event in prior_events),
           f"{label} ordinary Gate bridge would duplicate a phase")
    prior_events.append(candidate)
    latest_gate_events(prior_events)


def validate_verdict(value: Any, receipt: dict[str, Any]) -> None:
    keys = {"documentKind", "schemaVersion", "ledgerId", "parentBundleId", "parentSnapshotSha256", "taskId", "displayName", "beforeTaskSha256", "decision", "outcome", "reportSha256", "fixedPolicy", "reviewAuthority", "reviewExecutionOutcome"}
    is_recovery = receipt["schemaVersion"] == 2 and "taskDefinition" in receipt
    if is_recovery:
        keys |= {"taskDefinitionSha256", "authoritySha256", "gateEvent",
                 "dependencyReplacementsSha256"}
    value = exact(value, keys, "local verdict")
    insist(value["documentKind"] == "local-task-verdict"
           and value["schemaVersion"] == receipt["schemaVersion"]
           and value["ledgerId"] == LEDGER_ID,
           "local verdict identity differs")
    expected = {"parentBundleId": receipt["parentBundleId"], "parentSnapshotSha256": receipt["parentSnapshotSha256"], "taskId": receipt["taskId"], "displayName": receipt["displayName"], "beforeTaskSha256": receipt["beforeTaskSha256"], "decision": receipt["toStatus"], "outcome": receipt["outcome"], "reportSha256": receipt["report"]["sha256"], "fixedPolicy": POLICY, "reviewAuthority": "local-automatic-review", "reviewExecutionOutcome": "succeeded"}
    insist(all(value[key] == item for key, item in expected.items()), "local verdict is not bound to receipt")
    if is_recovery:
        insist(value["taskDefinitionSha256"]
               == digest(canonical_payload(receipt["taskDefinition"]))
               and value["authoritySha256"]
               == receipt["authorityArtifact"]["sha256"]
               and value["gateEvent"] == receipt["gateEvent"]
               and value["dependencyReplacementsSha256"]
               == digest(canonical_payload(receipt["dependencyReplacements"])),
               "local recovery verdict is not bound to receipt")


def local_bundle_map(repo: Path) -> dict[str, dict[str, Any]]:
    result: dict[str, dict[str, Any]] = {}
    root = repo / LOCAL_STORE_REL
    if not root.exists():
        return result
    no_reparse(root, "local bundle store")
    for child in root.iterdir():
        if not child.is_dir() or not SHA_RE.fullmatch(child.name):
            continue
        descriptor, descriptor_data = read_json(child / "bundle.json", "local bundle descriptor")
        base = {key: descriptor[key] for key in ("documentKind", "schemaVersion", "stateSha256", "markdownSha256", "snapshotSha256")}
        insist(descriptor.get("bundleId") == child.name == digest(canonical_payload(base)) and canonical_document(descriptor) == descriptor_data, "local bundle descriptor does not recompute")
        snapshot, snapshot_data = read_json(child / "snapshot.json", "local bundle snapshot")
        insist(digest(snapshot_data) == descriptor["snapshotSha256"], "local bundle snapshot hash differs")
        result[child.name] = {"descriptor": descriptor, "snapshot": snapshot}
    return result


def replay(snapshot: dict[str, Any], legacy: dict[str, Any], repo: Path, bundles: dict[str, dict[str, Any]]) -> dict[str, Any]:
    previous = None
    previous_completion = None
    overrides: dict[str, Any] = {}
    definitions: dict[str, dict[str, Any]] = {}
    gate_events: list[dict[str, Any]] = []
    replacements: list[dict[str, Any]] = []
    saw_v2 = False
    ordinary_gate_candidates: dict[str, dict[str, Any]] = {}

    def project_tasks() -> dict[str, dict[str, Any]]:
        projected = {task_id: task_view(task_id, raw)
                     for task_id, raw in legacy["tasks"].items()}
        for task_id, definition in definitions.items():
            insist(task_id not in projected,
                   f"recovery task {task_id} is not new")
            projected[task_id] = recovery_task_view(definition)
        check_task_graph(projected)
        for task_id, override in overrides.items():
            insist(task_id in projected,
                   f"local override names unknown task {task_id}")
            projected[task_id] = {
                **projected[task_id], "status": override["status"],
                "completedAtUtc": override["completedAtUtc"],
                "completionEvidence": override["completionEvidence"],
            }
        derive_ready(projected, gate_events, replacements, definitions)
        return projected

    tasks = project_tasks()
    records = snapshot["transitionReceipts"]
    for index, record in enumerate(records):
        record, receipt_data = artifact(record, repo, f"transition receipt {index}")
        receipt = json.loads(receipt_data.decode("ascii"))
        schema_validate(receipt, repo, f"transition receipt {index}")
        version = receipt.get("schemaVersion")
        insist(version in {1, 2}, "transition receipt version differs")
        if saw_v2:
            insist(version == 2, "local receipt chain cannot downgrade from v2")
        saw_v2 = saw_v2 or version == 2
        keys = {"documentKind", "schemaVersion", "ledgerId", "parentBundleId", "parentSnapshotSha256", "previousTransitionSha256", "taskId", "displayName", "beforeTaskSha256", "fromStatus", "toStatus", "outcome", "completedAtUtc", "reviewAuthority", "reviewExecutionOutcome", "report", "verdictArtifact", "fixedPolicy"}
        recovery_fields = {"taskDefinition", "authorityArtifact", "gateEvent",
                           "dependencyReplacements"}
        present_recovery_fields = set(receipt) & recovery_fields
        insist(not present_recovery_fields
               or (version == 2 and present_recovery_fields == recovery_fields),
               "transition receipt recovery fields are partial or invalid")
        is_recovery = bool(present_recovery_fields)
        if is_recovery:
            keys |= recovery_fields
        receipt = exact(receipt, keys, f"transition receipt {index}")
        insist(receipt["documentKind"] == "local-task-transition-receipt"
               and receipt["ledgerId"] == LEDGER_ID,
               "transition receipt identity differs")
        insist(receipt["previousTransitionSha256"] == previous, "transition predecessor differs")
        if index == 0:
            insist(receipt["parentBundleId"] == legacy["bundleId"] and receipt["parentSnapshotSha256"] == legacy["snapshotSha256"], "first transition is not anchored to legacy origin")
        else:
            parent = bundles.get(receipt["parentBundleId"])
            insist(parent is not None and parent["descriptor"]["snapshotSha256"] == receipt["parentSnapshotSha256"], "transition parent local bundle is absent or mismatched")
            insist(parent["snapshot"]["transitionHeadSha256"] == previous, "transition parent does not end at predecessor")
            insist(parent["snapshot"]["transitionReceipts"] == records[:index], "transition parent receipt prefix differs")
        insist(receipt["fromStatus"] == "ready" and receipt["toStatus"] in {"completed", "failed"} and receipt["outcome"] == ("succeeded" if receipt["toStatus"] == "completed" else "failed"), "transition status/outcome differs")
        insist(receipt["reviewAuthority"] == "local-automatic-review" and receipt["reviewExecutionOutcome"] == "succeeded" and receipt["fixedPolicy"] == POLICY, "transition authority/policy differs")
        completed = normalized_time(receipt["completedAtUtc"],
                                    "transition completion")
        if previous_completion is not None:
            insist(completed > previous_completion,
                   "transition completion time is not strictly increasing")
            parent_completed = normalized_time(
                parent["snapshot"]["sealedAtUtc"],
                "transition parent seal time")
            insist(completed > parent_completed,
                   "transition completion time is not newer than its parent snapshot")
        else:
            legacy_completed = normalized_time(
                legacy["snapshot"]["capturedAtUtc"],
                "legacy origin capture time")
            insist(completed > legacy_completed,
                   "first transition completion time is not newer than the legacy origin")
        pending_event = None
        pending_replacements: list[dict[str, Any]] = []
        if is_recovery:
            definition = validate_recovery_task_definition(
                receipt["taskDefinition"],
                f"transition receipt {index} task definition")
            insist(definition["taskId"] == receipt["taskId"]
                   and definition["displayName"] == receipt["displayName"],
                   "recovery task definition differs from receipt")
            insist(definition["taskId"] not in legacy["tasks"]
                   and definition["taskId"] not in definitions,
                   "recovery task ID is not new")
            insist(definition["createdAtUtc"] > (
                normalized_time(parent["snapshot"]["sealedAtUtc"],
                                "recovery parent seal time")
                if index else normalized_time(
                    legacy["snapshot"]["capturedAtUtc"],
                    "recovery origin time")),
                "recovery task creation is not newer than its parent")
            insist(completed > definition["createdAtUtc"],
                   "recovery task completion is not newer than creation")
            superseded = tasks.get(definition["supersedesTaskId"])
            if definition["supersedesTaskId"] is not None:
                insist(superseded is not None
                       and superseded["status"] in TERMINAL_STATES,
                       "recovery task does not supersede a terminal task")
                if (definition["role"] == "integration"
                        and definition["phase"] == "P8"
                        and definition["displayName"].endswith("-R1")):
                    insist(superseded["status"] == "failed",
                           "recovery integration must supersede a failed task")
                    insist(definition["supersedesTaskId"] not in definition["dependencies"],
                           "recovery integration cannot depend on its superseded task")
            insist(all(dependency in tasks
                       for dependency in definition["dependencies"]),
                   "recovery task names an unknown dependency")
            authority_record, authority_data = artifact(
                receipt["authorityArtifact"], repo,
                "recovery task authority")
            authority = json.loads(authority_data.decode("utf-8"))
            schema_validate(authority, repo, "recovery task authority")
            authority = exact(authority, {
                "documentKind", "schemaVersion", "ledgerId",
                "parentBundleId", "parentSnapshotSha256",
                "taskDefinitionSha256", "authorizedAtUtc", "fixedPolicy",
            }, "recovery task authority")
            insist(authority["documentKind"] == "local-recovery-task-authority"
                   and authority["schemaVersion"] == 1
                   and authority["ledgerId"] == LEDGER_ID
                   and authority["parentBundleId"] == receipt["parentBundleId"]
                   and authority["parentSnapshotSha256"] == receipt["parentSnapshotSha256"]
                   and authority["taskDefinitionSha256"]
                   == digest(canonical_payload(definition))
                   and authority["authorizedAtUtc"] == definition["createdAtUtc"]
                   and authority["fixedPolicy"] == POLICY
                   and canonical_document(authority) == authority_data,
                   "recovery task authority is not bound")
            prior_tasks = tasks
            gate_event = validate_gate_event(
                receipt["gateEvent"], definition,
                "recovery Gate event")
            if gate_event is not None:
                insist(gate_event["gateVerdict"] == (
                    GATE_PASS if receipt["toStatus"] == "completed"
                        else GATE_FAIL),
                    "recovery Gate verdict differs from task outcome")
                maybe_bridge_gate_predecessor(
                    gate_event, definition, gate_events,
                    ordinary_gate_candidates,
                    f"transition receipt {index} Gate event")
            replacement_drafts = validate_dependency_replacement_drafts(
                receipt["dependencyReplacements"], definition, gate_event,
                prior_tasks, gate_events, "dependency replacements")
            definitions[definition["taskId"]] = {
                **definition, "authorityArtifact": authority_record}
            if gate_event is not None:
                pending_event = {"eventId": record["sha256"],
                                 "taskId": definition["taskId"],
                                 **gate_event}
            pending_replacements = [
                {"eventId": record["sha256"], **replacement}
                for replacement in replacement_drafts]
            tasks = project_tasks()
        elif version == 2:
            event = ordinary_schema2_failed_gate_event(
                receipt, record["sha256"], legacy["tasks"])
            if event is not None:
                ordinary_gate_candidates[record["sha256"]] = event
        task = tasks.get(receipt["taskId"])
        insist(task is not None and task["displayName"] == receipt["displayName"] and task["status"] == "ready", "transition task is unknown or not ready")
        insist(receipt["beforeTaskSha256"] == task_digest(task), "transition before-task hash differs")
        report, _ = artifact(receipt["report"], repo, "transition report")
        _, verdict_data = artifact(receipt["verdictArtifact"], repo, "transition verdict")
        verdict = json.loads(verdict_data.decode("utf-8"))
        schema_validate(verdict, repo, "transition verdict")
        validate_verdict(verdict, receipt)
        evidence = {"sourceKind": "local-automatic-review", "receiptSha256": record["sha256"], "outcome": receipt["outcome"], "reportSha256": report["sha256"], "verdictSha256": receipt["verdictArtifact"]["sha256"]}
        overrides[receipt["taskId"]] = {"status": receipt["toStatus"], "completedAtUtc": completed, "completionEvidence": evidence}
        if version == 1:
            pending_event = implicit_v1_gate_event(receipt, record["sha256"])
        if pending_event is not None:
            gate_events.append(pending_event)
            latest_gate_events(gate_events)
        replacements.extend(pending_replacements)
        tasks = project_tasks()
        previous = record["sha256"]
        previous_completion = completed
    insist(overrides == snapshot["taskOverrides"], "local task overrides differ from receipt replay")
    insist(previous == snapshot["transitionHeadSha256"], "local transition head differs")
    sealed_at = normalized_time(snapshot["sealedAtUtc"],
                                "local snapshot seal time")
    insist(previous_completion is not None and sealed_at == previous_completion,
           "local snapshot seal time does not match its final receipt")
    return {
        "tasks": sorted(tasks.values(),
                        key=lambda row: (row["createdAtUtc"], row["taskId"])),
        "overrides": overrides,
        "recoveryTaskDefinitions": definitions,
        "gateEvents": gate_events,
        "dependencyReplacements": replacements,
    }


def validate_snapshot(snapshot: Any, legacy: dict[str, Any], repo: Path, bundles: dict[str, dict[str, Any]]) -> dict[str, Any]:
    version = snapshot.get("schemaVersion") if isinstance(snapshot, dict) else None
    insist(version in {1, 2}, "local snapshot version differs")
    keys = {"documentKind", "schemaVersion", "ledgerId", "base", "parent", "sealedAtUtc", "consistencyMode", "transitionReceipts", "transitionHeadSha256", "taskOverrides", "revisionSha256"}
    if version == 2:
        keys |= {"recoveryTaskDefinitions", "gateEvents",
                 "dependencyReplacements"}
    snapshot = exact(snapshot, keys, "local snapshot")
    schema_validate(snapshot, repo, "local snapshot")
    insist(snapshot["documentKind"] == "local-progress-snapshot"
           and snapshot["ledgerId"] == LEDGER_ID
           and snapshot["consistencyMode"] == "frozen-origin-plus-receipt-chain",
           "local snapshot identity differs")
    reject_banned(snapshot, "local snapshot")
    base = exact(snapshot["base"], {"pointerSha256", "bundleId", "stateSha256", "snapshotSha256", "revisionSha256"}, "local snapshot base")
    expected_base = {"pointerSha256": legacy["pointerSha256"], "bundleId": legacy["bundleId"], "stateSha256": legacy["stateSha256"], "snapshotSha256": legacy["snapshotSha256"], "revisionSha256": legacy["snapshot"].get("revisionSha256")}
    insist(base == expected_base, "local snapshot base differs from frozen origin")
    insist(isinstance(snapshot["transitionReceipts"], list) and 1 <= len(snapshot["transitionReceipts"]) <= 100, "local receipt list bounds differ")
    if len(snapshot["transitionReceipts"]) == 1:
        insist(snapshot["parent"] is None, "first local snapshot must have no local parent")
    else:
        parent = exact(snapshot["parent"], {"bundleId", "snapshotSha256", "transitionHeadSha256"}, "local snapshot parent")
        bundle = bundles.get(parent["bundleId"])
        insist(bundle is not None and bundle["descriptor"]["snapshotSha256"] == parent["snapshotSha256"] and bundle["snapshot"]["transitionHeadSha256"] == parent["transitionHeadSha256"], "local snapshot parent differs")
        insist(bundle["snapshot"]["transitionReceipts"] == snapshot["transitionReceipts"][:-1], "local snapshot parent prefix differs")
    body = {key: snapshot[key] for key in keys if key != "revisionSha256"}
    insist(snapshot["revisionSha256"] == digest(canonical_payload(body)), "local snapshot revision differs")
    result = replay(snapshot, legacy, repo, bundles)
    insist(snapshot["taskOverrides"] == result["overrides"],
           "local task overrides differ from receipt replay")
    if version == 1:
        insist(not result["recoveryTaskDefinitions"],
               "v1 snapshot contains recovery task definitions")
    else:
        insist(snapshot["recoveryTaskDefinitions"]
               == result["recoveryTaskDefinitions"],
               "local recovery task definitions differ from receipt replay")
        insist(snapshot["gateEvents"] == result["gateEvents"],
               "local Gate events differ from receipt replay")
        insist(snapshot["dependencyReplacements"]
               == result["dependencyReplacements"],
               "local dependency replacements differ from receipt replay")
    return result


def phase_from_label(label: str) -> str | None:
    match = re.match(r"^[PG](10|[0-9])(?:\b|-)", label)
    return f"P{match.group(1)}" if match else None


def derive_phases(
    legacy: dict[str, Any], tasks: list[dict[str, Any]],
    local_gate_events: list[dict[str, Any]] | None = None,
    recovery_definitions: dict[str, dict[str, Any]] | None = None,
) -> list[dict[str, Any]]:
    latest: dict[str, dict[str, Any]] = {}
    rows = legacy["state"]["gateHistory"]["rows"]
    receipts = legacy["state"]["gateHistory"]["receiptSha256s"]
    for index, row in enumerate(rows):
        latest[row["phase"]] = {**row, "eventId": receipts[index]}
    local_events = local_gate_events or []
    for phase, event in latest_gate_events(local_events).items():
        latest[phase] = {"phase": phase,
                         "gateVerdict": event["gateVerdict"],
                         "gateTaskId": event["taskId"],
                         "eventId": event["eventId"]}
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
        gate = latest.get(phase)
        verdict = gate.get("gateVerdict") if gate else None
        if verdict and (verdict.endswith("ENGINEERING PASS") or verdict.endswith("READINESS: READY")):
            engineering, execution, summary = "PASS", "GATED", f"{phase} engineering Gate is accepted; release policy remains blocked."
        elif verdict:
            engineering, execution, summary = "FAIL/BLOCKED", "BLOCKED", f"{phase} Gate is FAIL/BLOCKED and retained in immutable Gate history."
        else:
            engineering, execution, summary = "NOT_RUN", "MATERIALIZED_PENDING", f"{phase} is materialized; no local Gate receipt exists."
        history = [receipts[i] for i, row in enumerate(rows)
                   if row["phase"] == phase]
        history.extend(event["eventId"] for event in local_events
                       if event["phase"] == phase)
        result.append({"phase": phase, "executionStatus": execution,
                       "engineeringGate": engineering,
                       "gateTaskId": gate.get("gateTaskId") if gate else None,
                       "gateHistoryReceiptSha256s": history,
                       "taskIds": sorted(by_phase[phase]), "summary": summary})
    return result


def render_markdown(state: dict[str, Any], snapshot: dict[str, Any]) -> bytes:
    lines = ["# glTF Scene Editor Parity Local Progress", "", "> Local receipt projection. The legacy Orca ledger is a frozen origin and is not an active executor.", "", "## Policy", "", "- F42: `BLOCKED`", "- R24: `BLOCKED`", "- Metal: `Not Verified`", "- release/distribution: `NOT PASS`", "", "## Runnable Tasks", "", "| Task | Status | Dependencies | Completion Evidence |", "|---|---|---|---|"]
    for task in state["tasks"]:
        evidence = task["completionEvidence"]
        text = "" if evidence is None else f"{evidence['sourceKind']}:{evidence.get('receiptSha256', evidence.get('legacyTaskSha256', ''))}"
        lines.append(f"| `{task['taskId']}` {task['displayName']} | `{task['status']}` | `{', '.join(task['dependencies'])}` | `{text}` |")
    lines.extend(["", "## Phase Gates", "", "| Phase | Execution | Gate | Ready Tasks |", "|---|---|---|---|"])
    for phase in state["phases"]:
        ready = ", ".join(task_id for task_id in state["nextRunnableTaskIds"] if task_id in phase["taskIds"])
        lines.append(f"| {phase['phase']} | {phase['executionStatus']} | {phase['engineeringGate']} | `{ready}` |")
    lines.extend(["", "## Receipt Chain", "", f"- Base legacy bundle: `{state['baseBundleId']}`", f"- Transition head: `{snapshot['transitionHeadSha256']}`", "", "| Receipt | Bytes |", "|---|---:|"])
    for record in snapshot["transitionReceipts"]:
        lines.append(f"| `{record['sha256']}` | {record['bytes']} |")
    lines.append("")
    return "\n".join(lines).encode("utf-8")


def validate_state(state: Any, snapshot: dict[str, Any], replayed: dict[str, Any], legacy: dict[str, Any], pointer: dict[str, Any], repo: Path) -> None:
    keys = {"documentKind", "schemaVersion", "ledgerId", "sourceKind", "basePointerSha256", "baseBundleId", "baseStateSha256", "baseSnapshotSha256", "snapshotSha256", "transitionHeadSha256", "legacyOrigin", "policy", "tasks", "phases", "nextRunnableTaskIds", "publication"}
    state = exact(state, keys, "local state")
    schema_validate(state, repo, "local state")
    reject_banned(state, "local state")
    insist(state["documentKind"] == "local-progress-ledger-state"
           and state["schemaVersion"] == snapshot["schemaVersion"]
           and state["ledgerId"] == LEDGER_ID
           and state["sourceKind"] == "local-auto-review",
           "local state identity differs")
    insist(state["basePointerSha256"] == legacy["pointerSha256"] and state["baseBundleId"] == legacy["bundleId"] and state["baseStateSha256"] == legacy["stateSha256"] and state["baseSnapshotSha256"] == legacy["snapshotSha256"], "local state base differs")
    insist(state["snapshotSha256"] == pointer["snapshotSha256"] and state["transitionHeadSha256"] == snapshot["transitionHeadSha256"], "local state snapshot binding differs")
    tasks = replayed["tasks"]
    events = replayed["gateEvents"] if snapshot["schemaVersion"] == 2 else []
    definitions = replayed["recoveryTaskDefinitions"]
    insist(state["policy"] == POLICY and state["tasks"] == tasks
           and state["phases"]
           == derive_phases(legacy, tasks, events, definitions),
           "local state projection differs")
    ready = sorted(task["taskId"] for task in tasks if task["status"] == "ready")
    insist(state["nextRunnableTaskIds"] == ready, "local ready-task projection differs")
    expected_origin = {"sourceKind": "legacy-orca", "pointerSha256": legacy["pointerSha256"], "bundleId": legacy["bundleId"], "stateSha256": legacy["stateSha256"], "snapshotSha256": legacy["snapshotSha256"]}
    insist(state["legacyOrigin"] == expected_origin, "local legacy-origin record differs")


def validate(repo: Path, pointer_path: Path) -> dict[str, Any]:
    legacy = load_legacy(repo)
    pointer, pointer_data = read_json(pointer_path, "local pointer")
    keys = {"documentKind", "schemaVersion", "ledgerId", "basePointerSha256", "baseBundleId", "baseStateSha256", "baseSnapshotSha256", "bundleId", "bundlePath", "stateSha256", "snapshotSha256", "markdownSha256"}
    pointer = exact(pointer, keys, "local pointer")
    schema_validate(pointer, repo, "local pointer")
    insist(pointer["documentKind"] == "local-progress-ledger-pointer" and pointer["schemaVersion"] == 1 and pointer["ledgerId"] == LEDGER_ID, "local pointer identity differs")
    insist(pointer["basePointerSha256"] == legacy["pointerSha256"] and pointer["baseBundleId"] == legacy["bundleId"] and pointer["baseStateSha256"] == legacy["stateSha256"] and pointer["baseSnapshotSha256"] == legacy["snapshotSha256"], "local pointer base differs")
    insist(pointer["bundlePath"] == (LOCAL_STORE_REL / pointer["bundleId"]).as_posix(), "local pointer bundle path differs")
    bundle = repo / pointer["bundlePath"]
    descriptor, descriptor_data = read_json(bundle / "bundle.json", "selected local descriptor")
    base = {key: descriptor[key] for key in ("documentKind", "schemaVersion", "stateSha256", "markdownSha256", "snapshotSha256")}
    insist(descriptor.get("bundleId") == pointer["bundleId"] == digest(canonical_payload(base)) and canonical_document(descriptor) == descriptor_data, "selected local bundle identity differs")
    state, state_data = read_json(bundle / "state.json", "selected local state")
    snapshot, snapshot_data = read_json(bundle / "snapshot.json", "selected local snapshot")
    markdown = read_file(bundle / "progress.md", "selected local Markdown")
    insist(digest(state_data) == pointer["stateSha256"] and digest(snapshot_data) == pointer["snapshotSha256"] and digest(markdown) == pointer["markdownSha256"], "selected local bundle hash differs")
    bundles = local_bundle_map(repo)
    replayed = validate_snapshot(snapshot, legacy, repo, bundles)
    validate_state(state, snapshot, replayed, legacy, pointer, repo)
    insist(markdown == render_markdown(state, snapshot), "selected local Markdown is not deterministic")
    if pointer_path == repo / LOCAL_POINTER_REL:
        insist(read_file(repo / LOCAL_STATE_REL, "local state mirror") == state_data and read_file(repo / LOCAL_MARKDOWN_REL, "local Markdown mirror") == markdown, "local mirrors differ from pointer")
    return state


def arguments() -> argparse.Namespace:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--repo-root", type=Path, default=Path(__file__).resolve().parents[1])
    parser.add_argument("--pointer", type=Path)
    parser.add_argument("--check", action="store_true")
    return parser.parse_args()


def main() -> int:
    try:
        args = arguments()
        repo = Path(os.path.abspath(os.fspath(args.repo_root)))
        pointer = Path(os.path.abspath(os.fspath(args.pointer))) if args.pointer else repo / LOCAL_POINTER_REL
        if not pointer.exists() and args.check:
            print("No local progress ledger has been published.")
            return 0
        state = validate(repo, pointer)
        print(f"Local editor progress ledger validated: head={state['transitionHeadSha256']} ready={len(state['nextRunnableTaskIds'])}")
        return 0
    except (ValidationError, OSError, UnicodeError, ValueError) as exc:
        print(f"Local editor progress-ledger validation failed: {exc}", file=os.sys.stderr)
        return 1


if __name__ == "__main__":
    raise SystemExit(main())
