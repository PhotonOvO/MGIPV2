#!/usr/bin/env python3
"""Refresh generated P8 evidence identity records after aggregate re-sealing."""

from __future__ import annotations

import argparse
import hashlib
import importlib.util
import json
from pathlib import Path
from typing import Any


def canonical(value: Any) -> bytes:
    return (json.dumps(value, ensure_ascii=True, sort_keys=True, indent=2) + "\n").encode("ascii")


def record(path: Path) -> dict[str, Any]:
    data = path.read_bytes()
    return {"path": str(path).replace("/", "\\"), "bytes": len(data), "sha256": hashlib.sha256(data).hexdigest()}


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--repo-root", type=Path, required=True)
    parser.add_argument("--evidence-root", type=Path, required=True)
    args = parser.parse_args()
    repo = args.repo_root.resolve()
    root = args.evidence_root.resolve()
    aggregate_path = root / "visual-v2-evidence-manifest.json"
    aggregate = json.loads(aggregate_path.read_text(encoding="utf-8"))
    spec = importlib.util.spec_from_file_location("p8_aggregate", repo / "tools" / "_tmp_visual_aggregate.py")
    if spec is None or spec.loader is None:
        raise RuntimeError("cannot load visual aggregate helper")
    aggregate_module = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(aggregate_module)
    aggregate_module.REPO = repo
    source, files = aggregate_module.source_identity()
    if aggregate["source"]["identitySha256"] != source:
        raise RuntimeError("aggregate source identity differs from current closure")
    executables = aggregate["executables"]
    identity = {
        "documentKind": "mgif-p8-evidence-identity",
        "schemaVersion": 1,
        "authoritativePlatform": "Windows MSVC Vulkan",
        "source": {"identitySha256": source, "files": files},
        "executables": executables,
        "buildRoots": {
            "Debug": r"G:\Temp\mgif-p6-90-msvc-20260831T134500Z-Debug",
            "Release": r"G:\Temp\mgif-p6-90-msvc-20260831T134500Z-Release",
        },
        "aggregate": record(aggregate_path),
        "runtimeMatrix": record(root / "runtime-final" / "runtime-matrix.json"),
        "policy": {"F42": "BLOCKED", "R24": "BLOCKED", "Metal": "Not Verified", "releaseDistribution": "NOT PASS"},
        "nonClaims": ["No Linux build or runtime claim", "Metal Not Verified", "F42/R24 BLOCKED", "P8 Gate/G8 not claimed"],
    }
    (root / "identity.json").write_bytes(canonical(identity))
    for label in ("P8-10", "P8-11", "P8-12", "P8-90"):
        path = root / f"{label}-evidence-manifest.json"
        if not path.is_file():
            continue
        value = json.loads(path.read_text(encoding="utf-8"))
        value["identity"] = identity
        value["matrix"] = aggregate["matrix"]
        value["v04VisualDepthProof"] = aggregate["v04VisualDepthProof"]
        value["report"] = value.get("report")
        path.write_bytes(canonical(value))
    print(f"identity={source}")
    print(f"aggregate={identity['aggregate']['sha256']}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
