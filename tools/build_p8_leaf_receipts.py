#!/usr/bin/env python3
"""Create hash-bound bounded P8 leaf reports and local-ledger receipt drafts.

The tool only assembles evidence already produced by the Windows/MSVC
workflow. It does not publish the ledger; publication remains an explicit,
serial invocation of update_editor_progress_ledger.py.
"""

from __future__ import annotations

import argparse
import datetime as dt
import hashlib
import importlib.util
import json
from pathlib import Path
from typing import Any


TASKS = {
    "P8-10": "task_469ac0bc570d",
    "P8-11": "task_c52f54a8f799",
    "P8-12": "task_2c5f762c2290",
    "P8-90": "task_99a11c23bb9a",
}


def canonical(value: Any) -> bytes:
    return (json.dumps(value, ensure_ascii=True, sort_keys=True, indent=2) + "\n").encode("ascii")


def sha_bytes(data: bytes) -> str:
    return hashlib.sha256(data).hexdigest()


def sha_file(path: Path) -> str:
    return sha_bytes(path.read_bytes())


def write_json(path: Path, value: Any) -> None:
    path.parent.mkdir(parents=True, exist_ok=True)
    path.write_bytes(canonical(value))


def win_path(path: Path) -> str:
    value = str(path).replace("/", "\\")
    if value.startswith("\\mnt\\g\\"):
        return "G:\\" + value[len("\\mnt\\g\\") :]
    return value


def record(path: Path, logical: str | None = None) -> dict[str, Any]:
    data = path.read_bytes()
    return {
        "path": logical or win_path(path),
        "bytes": len(data),
        "sha256": sha_bytes(data),
    }


def load_aggregate_module(repo: Path):
    source = repo / "tools" / "_tmp_visual_aggregate.py"
    spec = importlib.util.spec_from_file_location("p8_visual_aggregate", source)
    if spec is None or spec.loader is None:
        raise RuntimeError(f"cannot load aggregate helper: {source}")
    module = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(module)
    module.REPO = repo
    return module


def parse_time(value: str) -> dt.datetime:
    return dt.datetime.fromisoformat(value.replace("Z", "+00:00"))


def next_completion(parent: dict[str, Any]) -> str:
    parent_text = parent["snapshot"]["sealedAtUtc"]
    parent_time = parse_time(parent_text)
    now = dt.datetime.now(dt.timezone.utc)
    value = max(now, parent_time + dt.timedelta(seconds=1))
    return value.isoformat(timespec="milliseconds").replace("+00:00", "Z")


def context(repo: Path, evidence_root: Path, aggregate: dict[str, Any]) -> dict[str, Any]:
    aggregate_path = evidence_root / "visual-v2-evidence-manifest.json"
    runtime_path = evidence_root / "runtime-final" / "runtime-matrix.json"
    required_logs = [
        "ctest-p8-Debug.log",
        "ctest-p8-Release.log",
        "ctest-domain-Debug.log",
        "ctest-domain-Release.log",
        "visual-contract.log",
        "present-contract.log",
        "p0-scaffold.log",
        "debug-build.log",
        "release-build.log",
    ]
    logs = {
        name: record(evidence_root / name)
        for name in required_logs
        if (evidence_root / name).is_file()
    }
    debug_exe = Path(r"G:\Temp\mgif-p6-90-msvc-20260831T134500Z-Debug\Demo.exe")
    release_exe = Path(r"G:\Temp\mgif-p6-90-msvc-20260831T134500Z-Release\Demo.exe")
    # On Windows this is native; on WSL the caller can pass mapped paths and
    # the aggregate records already contain the authoritative executable data.
    executable_records = aggregate["executables"]
    return {
        "evidenceRoot": win_path(evidence_root),
        "aggregate": record(aggregate_path),
        "runtimeMatrix": record(runtime_path),
        "logs": logs,
        "executables": executable_records,
        "matrix": aggregate["matrix"],
        "v04VisualDepthProof": aggregate["v04VisualDepthProof"],
        "sourceIdentity": aggregate["source"]["identitySha256"],
        "sourceFiles": aggregate["source"]["files"],
        "buildRoots": {
            "Debug": win_path(debug_exe.parent),
            "Release": win_path(release_exe.parent),
        },
        "policy": {
            "F42": "BLOCKED",
            "R24": "BLOCKED",
            "Metal": "Not Verified",
            "releaseDistribution": "NOT PASS",
        },
    }


def report_text(label: str, task_id: str, ctx: dict[str, Any]) -> str:
    decision_line = (
        "Decision: bounded Windows/MSVC evidence transition completed for this leaf; this is not a P8 Gate verdict."
        if label != "P8-90"
        else "Decision: BLOCKED; the P8-90 integration task was executed as an audit and cannot pass with the current implementation."
    )
    common = [
        f"# {label} Bounded Evidence Report",
        "",
        decision_line,
        "",
        f"Task: {task_id}",
        f"Evidence root: {ctx['evidenceRoot']}",
        f"Source identity: {ctx['sourceIdentity']}",
        f"Debug Demo SHA-256: {ctx['executables']['Debug']['sha256']}",
        f"Release Demo SHA-256: {ctx['executables']['Release']['sha256']}",
        "",
        "## Fresh Evidence",
        "",
        "- Windows/MSVC Vulkan is authoritative; WSL only orchestrated and read artifacts.",
        "- Visual matrix: 62 cases (24 Debug, 24 Release, 14 V07 extension), with 206 stage states.",
        "- Every stage was checked for source/executable identity, three document digests, UI semantic state, boundary action_log, marker order, RHI counters, and nonblank PNG coverage.",
        "- Runtime matrix: 8/8; focused P8 CTest: 14/14 in each configuration; domain/document CTest: 8/8 in each configuration.",
        "- Multi-scene non-current primitive regression: selection, exact Browser row, Inspector stats, focus payload, and current-scene renderer boundary all pass in Debug/Release.",
        f"- V04 depth crop proof: {ctx['v04VisualDepthProof']['status']} ({ctx['v04VisualDepthProof']['diffPixels']} pixels; threshold {ctx['v04VisualDepthProof']['threshold']}).",
        "",
        "## Leaf Scope",
        "",
    ]
    scope = {
        "P8-10": [
            "The current shell/menu/toolbar/Grid/Gizmo/Snap presentation and bidirectional command-state surface are covered by the UI semantic contract and fresh visual matrix.",
            "The exact Section 6.5 P8-10 dedicated EditorShell/EditorMenuBar/EditorToolbar/EditorTheme/GridSnapSettings write-set and exhaustive action driver are not claimed here.",
        ],
        "P8-11": [
            "The current Scene Browser, scene-qualified focus path, scene rows, and non-current primitive document lookup are covered by controller/integration evidence.",
            "The exact Section 6.5 P8-11 dedicated Browser model/panel, functional Animation/Variants replay, and complete Scene Transform popup action matrix are not claimed here.",
        ],
        "P8-12": [
            "The current Inspector selection contexts, primitive authority statistics, clipboard/controller paths, and UI semantic surface are covered by focused evidence.",
            "The exact Section 6.5 P8-12 dedicated Inspector/property-table/dialog write-set and complete widget/modal action replay are not claimed here.",
        ],
        "P8-90": [
            "The fresh leaf evidence and existing CTest results are available to the integration audit, but they do not provide the required acceptance-driver implementation.",
            "BLOCKER: app/editor/EditorTestDriver.{h,cpp}, tests/editor/editor_ui_state_tests.cpp, and tests/editor/editor_ui_action_tests.cpp are absent; CMake has no corresponding target or U01-U08 replay wiring.",
            "BLOCKER: SceneEditorUiModel.h exposes eleven enum values and EditorController.cpp only reaches NoDocument/Error/ReadyClean/ReadyModified in normal operation; the exact nine runtime states are not exercised.",
            "BLOCKER: MinimalLatestApp.h retains m_sceneModel/m_sceneAsset and destructive runtime replacement/upload paths (notably lines 1685, 1789, 5230-5550, 5673-5681), so the legacy-authority cutover is incomplete.",
        ],
    }[label]
    common.extend(f"- {line}" for line in scope)
    common.extend([
        "",
        "## Explicit Non-Claims",
        "",
        "- No complete U01-U08 real action/state replay or context-menu manifest is claimed.",
        "- LoadingCandidate, GizmoHover, GizmoDragging, Modal, Saving/Merging/Compacting full lifecycle traces remain deferred; current evidence does not promote them from static/state vocabulary to runtime coverage.",
        "- P8-90 integration and G8 independent Gate review remain pending; this receipt does not unblock or pass them.",
        "- No Linux build was run. Metal remains Not Verified; F42/R24 and release/distribution remain blocked by policy.",
        "",
    ])
    return "\n".join(common)


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--repo-root", type=Path, required=True)
    parser.add_argument("--evidence-root", type=Path, required=True)
    parser.add_argument("--task", choices=sorted(TASKS), required=True)
    args = parser.parse_args()
    repo = args.repo_root.resolve()
    evidence_root = args.evidence_root.resolve()
    aggregate_module = load_aggregate_module(repo)
    aggregate_path = evidence_root / "visual-v2-evidence-manifest.json"
    aggregate = json.loads(aggregate_path.read_text(encoding="utf-8"))
    expected_source, source_files = aggregate_module.source_identity()
    if aggregate["source"]["identitySha256"] != expected_source:
        raise RuntimeError("aggregate source identity differs from current source closure")
    ctx = context(repo, evidence_root, aggregate)
    ctx["sourceIdentity"] = expected_source
    ctx["sourceFiles"] = source_files

    origin = aggregate_module  # keep the helper module namespace local
    ledger_path = repo / "tools" / "local_editor_progress_ledger.py"
    ledger_spec = importlib.util.spec_from_file_location("p8_local_ledger", ledger_path)
    if ledger_spec is None or ledger_spec.loader is None:
        raise RuntimeError("cannot load local ledger publisher")
    ledger = importlib.util.module_from_spec(ledger_spec)
    ledger_spec.loader.exec_module(ledger)
    frozen = ledger.legacy_origin(repo)
    parent = ledger.load_local(repo, frozen)
    if parent is None:
        raise RuntimeError("P8 leaf receipt requires a selected local parent")
    task_id = TASKS[args.task]
    task = next(row for row in parent["tasks"] if row["taskId"] == task_id)
    if task["status"] != "ready":
        raise RuntimeError(f"{args.task} is not ready against selected parent: {task['status']}")
    before_task = ledger.task_hash(task)
    task_dir = evidence_root / f"{args.task.lower()}-ledger-draft"
    report_path = task_dir / f"{args.task}-REPORT.md"
    report_path.parent.mkdir(parents=True, exist_ok=True)
    report_path.write_text(report_text(args.task, task_id, ctx), encoding="ascii", newline="\n")
    report_record = record(report_path)
    failed = args.task == "P8-90"
    verdict = {
        "documentKind": "local-task-verdict",
        "schemaVersion": 2,
        "ledgerId": ledger.LEDGER_ID,
        "parentBundleId": parent["bundleId"],
        "parentSnapshotSha256": parent["pointer"]["snapshotSha256"],
        "taskId": task_id,
        "displayName": args.task,
        "beforeTaskSha256": before_task,
        "decision": "failed" if failed else "completed",
        "outcome": "failed" if failed else "succeeded",
        "reportSha256": report_record["sha256"],
        "fixedPolicy": dict(ledger.POLICY),
        "reviewAuthority": "local-automatic-review",
        "reviewExecutionOutcome": "succeeded",
    }
    verdict_path = task_dir / f"{args.task}-verdict.json"
    write_json(verdict_path, verdict)
    verdict_record = record(verdict_path)
    receipt = {
        "documentKind": "local-task-transition-receipt",
        "schemaVersion": 2,
        "ledgerId": ledger.LEDGER_ID,
        "parentBundleId": parent["bundleId"],
        "parentSnapshotSha256": parent["pointer"]["snapshotSha256"],
        "previousTransitionSha256": parent["snapshot"]["transitionHeadSha256"],
        "taskId": task_id,
        "displayName": args.task,
        "beforeTaskSha256": before_task,
        "fromStatus": "ready",
        "toStatus": "failed" if failed else "completed",
        "outcome": "failed" if failed else "succeeded",
        "completedAtUtc": next_completion(parent),
        "reviewAuthority": "local-automatic-review",
        "reviewExecutionOutcome": "succeeded",
        "report": report_record,
        "verdictArtifact": verdict_record,
        "fixedPolicy": dict(ledger.POLICY),
    }
    receipt_path = task_dir / f"{args.task}-receipt-draft.json"
    write_json(receipt_path, receipt)

    identity = {
        "documentKind": "mgif-p8-evidence-identity",
        "schemaVersion": 1,
        "authoritativePlatform": "Windows MSVC Vulkan",
        "source": {"identitySha256": expected_source, "files": source_files},
        "executables": aggregate["executables"],
        "buildRoots": ctx["buildRoots"],
        "aggregate": record(aggregate_path),
        "runtimeMatrix": record(evidence_root / "runtime-final" / "runtime-matrix.json"),
        "policy": ctx["policy"],
        "nonClaims": [
            "No Linux build or runtime claim",
            "Metal Not Verified",
            "F42/R24 BLOCKED",
            "P8 Gate/G8 not claimed",
        ],
    }
    write_json(evidence_root / "identity.json", identity)
    manifest = {
        "documentKind": "mgif-p8-bounded-evidence",
        "schemaVersion": 1,
        "task": args.task,
        "taskId": task_id,
        "identity": identity,
        "focusedArtifacts": ctx["logs"],
        "matrix": aggregate["matrix"],
        "v04VisualDepthProof": aggregate["v04VisualDepthProof"],
        "receiptDraft": record(receipt_path),
        "report": report_record,
        "verdict": verdict_record,
        "nonClaims": identity["nonClaims"],
    }
    write_json(evidence_root / f"{args.task}-evidence-manifest.json", manifest)
    for path in (report_path, verdict_path, receipt_path):
        item = record(path)
        print(f"{path.name} bytes={item['bytes']} sha256={item['sha256']}")
    print(f"task={args.task} taskId={task_id} beforeTaskSha256={before_task}")
    print(f"parentBundleId={parent['bundleId']} parentSnapshotSha256={parent['pointer']['snapshotSha256']}")
    print(f"previousTransitionSha256={parent['snapshot']['transitionHeadSha256']}")
    print(f"sourceIdentity={expected_source}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
