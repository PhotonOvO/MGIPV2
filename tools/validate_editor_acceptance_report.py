#!/usr/bin/env python3
"""Validate cross-record semantics for one glTF editor acceptance report.

JSON Schema validates the shape of a report. This validator enforces the rules
that Draft 2020-12 cannot express locally: canonical IDs, exact tallies,
artifact containment and hashes, evidence references, and final-run closure.
"""

from __future__ import annotations

import argparse
import hashlib
import json
import re
import sys
from datetime import datetime
from pathlib import Path, PurePosixPath
from typing import Any, Iterable

from json_schema_subset import JsonSchemaError, validate_json_schema


EXPECTED_REFERENCE_COMMIT = "50373c7b0ff58078727af2c2aac70e29d5e74752"
EXPECTED_REQUIREMENT_IDS = [f"R{index:02d}" for index in range(1, 25)]
EXPECTED_FEATURE_IDS = [f"F{index:02d}" for index in range(1, 43)]
EXPECTED_EDGE_IDS = [f"E{index:03d}" for index in range(1, 111)]
VALID_STATUSES = {"pass", "fail", "blocked", "not_run", "not_applicable"}
STATUS_TALLY_KEYS = {
    "pass": "passed",
    "fail": "failed",
    "blocked": "blocked",
    "not_run": "notRun",
    "not_applicable": "notApplicable",
}
SHA256_PATTERN = re.compile(r"^[0-9a-fA-F]{64}$")
GIT_COMMIT_PATTERN = re.compile(r"^[0-9a-f]{40}$")


class ValidationError(RuntimeError):
    pass


def require(condition: bool, message: str) -> None:
    if not condition:
        raise ValidationError(message)


def load_json(path: Path) -> Any:
    try:
        return json.loads(path.read_text(encoding="utf-8"))
    except (OSError, UnicodeError, json.JSONDecodeError) as exc:
        raise ValidationError(f"cannot parse JSON {path}: {exc}") from exc


def sha256(path: Path) -> str:
    digest = hashlib.sha256()
    try:
        with path.open("rb") as stream:
            for chunk in iter(lambda: stream.read(1024 * 1024), b""):
                digest.update(chunk)
    except OSError as exc:
        raise ValidationError(f"cannot hash {path}: {exc}") from exc
    return digest.hexdigest()


def resolve_artifact_path(evidence_root: Path, relative_path: str) -> Path:
    require(isinstance(relative_path, str) and relative_path, "artifact path must be a non-empty string")
    require("\\" not in relative_path, f"artifact path must use '/' separators: {relative_path!r}")
    pure_path = PurePosixPath(relative_path)
    require(not pure_path.is_absolute(), f"artifact path must be relative: {relative_path!r}")
    require(".." not in pure_path.parts, f"artifact path escapes the evidence root: {relative_path!r}")
    require(not re.match(r"^[A-Za-z]:", relative_path), f"artifact path must not contain a drive: {relative_path!r}")

    root = evidence_root.resolve()
    resolved = root.joinpath(*pure_path.parts).resolve()
    try:
        resolved.relative_to(root)
    except ValueError as exc:
        raise ValidationError(f"artifact path escapes the evidence root: {relative_path!r}") from exc
    return resolved


def validate_datetime(value: Any, field: str) -> None:
    require(isinstance(value, str) and value, f"{field} must be a non-empty date-time string")
    try:
        parsed = datetime.fromisoformat(value.replace("Z", "+00:00"))
    except ValueError as exc:
        raise ValidationError(f"{field} is not an ISO 8601 date-time: {value!r}") from exc
    require(parsed.tzinfo is not None, f"{field} must include an explicit UTC offset")


def validate_identity(identity: Any, evidence_root: Path) -> None:
    require(isinstance(identity, dict), "identity must be an object")
    validate_datetime(identity.get("startedAt"), "identity.startedAt")
    require(bool(GIT_COMMIT_PATTERN.fullmatch(str(identity.get("gitHead", "")))), "identity.gitHead must be a lowercase 40-character commit")
    require(bool(SHA256_PATTERN.fullmatch(str(identity.get("dirtyDiffSha256", "")))), "identity.dirtyDiffSha256 must be SHA-256")
    require(identity.get("referenceCommit") == EXPECTED_REFERENCE_COMMIT, "identity.referenceCommit is not the locked reference commit")

    executable = identity.get("executable")
    require(isinstance(executable, dict), "identity.executable must be an object")
    executable_path_value = executable.get("path")
    require(isinstance(executable_path_value, str) and executable_path_value, "identity.executable.path is required")
    executable_path = Path(executable_path_value)
    if not executable_path.is_absolute():
        executable_path = resolve_artifact_path(evidence_root, executable_path_value)
    else:
        executable_path = executable_path.resolve()
    require(executable_path.is_file(), f"identity executable does not exist: {executable_path}")
    expected_hash = executable.get("sha256")
    require(bool(SHA256_PATTERN.fullmatch(str(expected_hash or ""))), "identity.executable.sha256 must be SHA-256")
    require(sha256(executable_path) == str(expected_hash).lower(), "identity executable SHA-256 does not match")

    for field in ("cmake", "compiler", "os", "vulkan", "dpi"):
        value = identity.get(field)
        require(isinstance(value, dict) and value, f"identity.{field} must be a non-empty object")
    gpu = identity.get("gpu")
    require(isinstance(gpu, list) and gpu, "identity.gpu must contain at least one adapter record")
    require(all(isinstance(adapter, dict) and adapter for adapter in gpu), "every identity.gpu entry must be a non-empty object")


def validate_artifacts(artifacts: Any, evidence_root: Path) -> set[str]:
    require(isinstance(artifacts, list), "artifacts must be an array")
    paths: list[str] = []
    for index, artifact in enumerate(artifacts):
        require(isinstance(artifact, dict), f"artifacts[{index}] must be an object")
        relative_path = artifact.get("path")
        resolved = resolve_artifact_path(evidence_root, relative_path)
        paths.append(relative_path)
        require(resolved.is_file(), f"declared artifact does not exist: {relative_path}")
        expected_hash = artifact.get("sha256")
        require(bool(SHA256_PATTERN.fullmatch(str(expected_hash or ""))), f"artifact SHA-256 is invalid: {relative_path}")
        require(sha256(resolved) == str(expected_hash).lower(), f"artifact SHA-256 does not match: {relative_path}")
        require(isinstance(artifact.get("mediaType"), str) and artifact["mediaType"], f"artifact mediaType is missing: {relative_path}")
        require(isinstance(artifact.get("producer"), str) and artifact["producer"], f"artifact producer is missing: {relative_path}")
    require(len(paths) == len(set(paths)), "artifact paths must be unique")
    return set(paths)


def validate_results(
    name: str,
    results: Any,
    expected_ids: list[str] | None,
    artifact_paths: set[str],
    final_run: bool,
) -> list[str]:
    require(isinstance(results, list), f"{name} must be an array")
    ids: list[str] = []
    statuses: list[str] = []
    for index, result in enumerate(results):
        require(isinstance(result, dict), f"{name}[{index}] must be an object")
        result_id = result.get("id")
        require(isinstance(result_id, str) and result_id, f"{name}[{index}].id is required")
        ids.append(result_id)
        status = result.get("status")
        require(status in VALID_STATUSES, f"{name} {result_id} has invalid status {status!r}")
        statuses.append(status)
        evidence = result.get("evidence")
        require(isinstance(evidence, list), f"{name} {result_id} evidence must be an array")
        require(len(evidence) == len(set(evidence)), f"{name} {result_id} has duplicate evidence references")
        for reference in evidence:
            require(reference in artifact_paths, f"{name} {result_id} references undeclared artifact: {reference!r}")
        if status in {"pass", "fail"} or final_run:
            require(bool(evidence), f"{name} {result_id} status {status!r} requires evidence")
        if status in {"blocked", "not_run", "not_applicable"}:
            require(bool(result.get("message")), f"{name} {result_id} status {status!r} requires a message")

    require(len(ids) == len(set(ids)), f"{name} IDs must be unique")
    if expected_ids is not None:
        require(ids == expected_ids, f"{name} must contain canonical IDs in order")
    else:
        require(bool(ids), "platforms must contain at least one result")
    return statuses


def tally(statuses: Iterable[str]) -> dict[str, int]:
    result = {
        "total": 0,
        "passed": 0,
        "failed": 0,
        "blocked": 0,
        "notRun": 0,
        "notApplicable": 0,
    }
    for status in statuses:
        result["total"] += 1
        result[STATUS_TALLY_KEYS[status]] += 1
    return result


def derived_summary_status(statuses: Iterable[str], unresolved_waivers: int) -> str:
    values = list(statuses)
    if "fail" in values or unresolved_waivers:
        return "fail"
    if "blocked" in values:
        return "blocked"
    if "not_run" in values:
        return "not_run"
    return "pass"


def validate_summary(summary: Any, status_groups: dict[str, list[str]], final_run: bool) -> None:
    require(isinstance(summary, dict), "summary must be an object")
    for name, statuses in status_groups.items():
        actual = tally(statuses)
        require(summary.get(name) == actual, f"summary.{name} does not match the result records")

    unresolved = summary.get("unresolvedWaivers")
    require(isinstance(unresolved, int) and not isinstance(unresolved, bool) and unresolved >= 0, "summary.unresolvedWaivers must be a non-negative integer")
    all_statuses = [status for values in status_groups.values() for status in values]
    expected_status = derived_summary_status(all_statuses, unresolved)
    require(summary.get("status") == expected_status, f"summary.status must be {expected_status!r} for the recorded results")

    if final_run:
        require(unresolved == 0, "final report cannot contain unresolved waivers")
        require(all(status == "pass" for status in all_statuses), "final report requires every requirement, feature, edge, and platform result to pass")
        require(summary.get("status") == "pass", "final report summary must pass")


def load_edge_ids(edge_obligations_path: Path) -> list[str]:
    obligations = load_json(edge_obligations_path)
    require(isinstance(obligations, dict), "edge obligations root must be an object")
    rows = obligations.get("obligations")
    require(isinstance(rows, list), "edge obligations must contain an obligations array")
    ids = [row.get("id") if isinstance(row, dict) else None for row in rows]
    require(ids == EXPECTED_EDGE_IDS, "edge obligations must define canonical E001-E110 in order")
    return EXPECTED_EDGE_IDS


def validate_acceptance_report(report_path: Path, evidence_root: Path, edge_obligations_path: Path) -> None:
    report = load_json(report_path)
    require(isinstance(report, dict), "acceptance report root must be an object")
    schema_path = Path(__file__).resolve().parents[1] / "docs/editor-parity/schemas/acceptance-report.schema.json"
    try:
        validate_json_schema(report, load_json(schema_path))
    except JsonSchemaError as exc:
        raise ValidationError(f"acceptance report does not match its JSON Schema: {exc}") from exc
    require(report.get("schemaVersion") == 1, "acceptance report schemaVersion must be 1")
    run_mode = report.get("runMode")
    require(run_mode in {"baseline", "candidate", "final"}, "runMode must be baseline, candidate, or final")
    final_run = run_mode == "final"

    validate_identity(report.get("identity"), evidence_root)
    artifact_paths = validate_artifacts(report.get("artifacts"), evidence_root)
    edge_ids = load_edge_ids(edge_obligations_path)

    status_groups = {
        "requirements": validate_results("requirements", report.get("requirements"), EXPECTED_REQUIREMENT_IDS, artifact_paths, final_run),
        "features": validate_results("features", report.get("features"), EXPECTED_FEATURE_IDS, artifact_paths, final_run),
        "edges": validate_results("edges", report.get("edges"), edge_ids, artifact_paths, final_run),
        "platforms": validate_results("platforms", report.get("platforms"), None, artifact_paths, final_run),
    }
    validate_summary(report.get("summary"), status_groups, final_run)


def parse_args() -> argparse.Namespace:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--report", required=True, type=Path, help="Acceptance report JSON to validate")
    parser.add_argument(
        "--evidence-root",
        type=Path,
        help="Root used to resolve artifact and relative executable paths (defaults to report directory)",
    )
    parser.add_argument(
        "--edge-obligations",
        type=Path,
        default=Path(__file__).resolve().parents[1] / "docs/editor-parity/edge-obligations.json",
        help="Canonical E001-E110 obligation ledger",
    )
    return parser.parse_args()


def main() -> int:
    args = parse_args()
    report_path = args.report.resolve()
    evidence_root = (args.evidence_root or report_path.parent).resolve()
    try:
        validate_acceptance_report(report_path, evidence_root, args.edge_obligations.resolve())
    except ValidationError as exc:
        print(f"Editor acceptance report validation failed: {exc}", file=sys.stderr)
        return 1
    print("Editor acceptance report validation passed: 24 requirements, 42 features, 110 edges, platforms, tallies, and artifact hashes")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
