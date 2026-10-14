#!/usr/bin/env python3
"""Verify a bounded editor live-replay cross matrix.

The matrix layout is ``<root>/<Configuration>/<case>-<W>x<H>-dpi<token>/``
with ``matrix.tsv`` at the root.  Every check is fail-closed and derived from
filesystem evidence only.

Two families of checks run:

*Per-root* -- status, exact client extent, window decoration, applied UI DPI,
pixel-audit ratios, PNG digest integrity, step artifacts, runtime-log scan, and
a single scoped source identity across the whole matrix.

*Cross-run* -- a stale-capture detector.  A full-screen undecorated swapchain
window can be promoted to independent flip, after which a window-DC BitBlt
reads a DWM redirection surface left over from an earlier run.  The returned
image is a genuine editor frame, so no per-frame audit can reject it.  What it
cannot survive is a comparison against the same case at a different UI DPI:
the measured genuine layout difference is about 12% of sampled pixels, while an
observed stale pair differed in 28 of 921600 samples (0.003%).
"""

from __future__ import annotations

import argparse
import hashlib
import io
import json
import re
import sys
from pathlib import Path

FORBIDDEN_LOG_PATTERNS = (
    "VUID-",
    "SYNC-HAZARD",
    "Validation Error",
    "device lost",
    "VK_ERROR",
    "stale editor command sequence",
)

# Audit floors recalibrated 2026-09-09 against frames the visual harness
# already accepts as genuine: V01-grid perspective-horizon measures
# uniqueColors 49 / nonBlackRatio 0.023 / colorfulRatio 0.0030 /
# edgeRatio 0.0246 / coherentRunRatio 0.285 / structuredRowRatio 0.330, and the
# 1440p undecorated probe 49 / 0.014 / 0.0019 / 0.0120 / 0.242 / 0.242.  The
# earlier brightness-led floors (0.05 / 0.02 / 0.03, uniqueColors 16) were tuned
# on a 20260908 live frame whose panels sat on a lit scene, so they rejected
# genuine dark boundary frames while accepting the two-colour phantom-monitor
# placeholder and the foreign bright window.  Structure decides; brightness only
# rejects uniform surfaces.  Degraded measures coherentRunRatio 0.1444 /
# structuredRowRatio 0.1778 and black blank 0.0 / 0.0, so both stay rejected.
# Unique-colour figures assume the [int] packing cast in the harness audit: under
# the old [byte] -shl packing the field was a blue-channel count (141 instead of
# 562 on the same frame).
AUDIT_FLOORS = {
    "nonBlackRatio": 0.01,
    "colorfulRatio": 0.001,
    "edgeRatio": 0.008,
    "coherentRunRatio": 0.19,
    "structuredRowRatio": 0.21,
}
UNIQUE_COLOR_FLOOR = 32
# Uniform-surface rejector: near-full non-black coverage with very few distinct
# sampled colours is the two-colour placeholder / foreign-window signature.
# Measured with the [int] packing: two-colour placeholder 114, white blank 1-2,
# genuine bright frames 444-562.
UNIFORM_SURFACE_NON_BLACK_RATIO = 0.90
UNIFORM_SURFACE_MIN_COLORS = 200

ROOT_NAME = re.compile(
    r"^(?P<case>[A-Za-z0-9\-]+?)-(?P<width>\d+)x(?P<height>\d+)-dpi(?P<dpi>[\dp]+)$"
)


def _load(path: Path):
    with io.open(path, "r", encoding="utf-8") as handle:
        return json.load(handle)


def _sha256(path: Path) -> str:
    digest = hashlib.sha256()
    with io.open(path, "rb") as handle:
        for block in iter(lambda: handle.read(1 << 20), b""):
            digest.update(block)
    return digest.hexdigest()


def _dpi_value(token: str) -> float:
    return float(token.replace("p", "."))


def _sampled_difference(left: Path, right: Path, stride: int = 4, threshold: int = 12):
    """Return (fraction, sampled) of pixels whose luminance differs."""
    from PIL import Image, ImageChops  # local import: only the diff check needs it

    a = Image.open(left).convert("L")
    b = Image.open(right).convert("L")
    if a.size != b.size:
        raise ValueError(f"extent mismatch {a.size} vs {b.size}")
    diff = ImageChops.difference(a, b)
    pixels = diff.load()
    width, height = diff.size
    sampled = 0
    differing = 0
    for y in range(0, height, stride):
        for x in range(0, width, stride):
            sampled += 1
            if pixels[x, y] > threshold:
                differing += 1
    return differing / max(1, sampled), sampled


def verify(matrix_root: Path, min_dpi_difference: float) -> int:
    failures: list[str] = []
    notes: list[str] = []
    roots: list[dict] = []

    for config_dir in sorted(p for p in matrix_root.iterdir() if p.is_dir()):
        for root in sorted(p for p in config_dir.iterdir() if p.is_dir()):
            match = ROOT_NAME.match(root.name)
            if match is None:
                failures.append(f"{root}: name does not match the matrix layout")
                continue
            width = int(match.group("width"))
            height = int(match.group("height"))
            dpi = _dpi_value(match.group("dpi"))
            case = match.group("case")
            configuration = config_dir.name
            label = f"{configuration}/{root.name}"

            manifest_path = root / "manifest.json"
            png_path = root / "client.png"
            state_path = root / "app" / "state.json"
            identity_path = root / "source-identity.json"
            if not manifest_path.is_file():
                failures.append(f"{label}: manifest.json is missing")
                continue
            for required in (png_path, state_path, identity_path):
                if not required.is_file():
                    failures.append(f"{label}: {required.name} is missing")

            manifest = _load(manifest_path)
            state = _load(state_path) if state_path.is_file() else {}
            identity = _load(identity_path) if identity_path.is_file() else {}

            if manifest.get("status") != "PASS_BOUNDED_LIVE_PROOF":
                failures.append(f"{label}: status is {manifest.get('status')!r}")
            if manifest.get("case") != case:
                failures.append(f"{label}: manifest case is {manifest.get('case')!r}")
            if manifest.get("configuration") != configuration:
                failures.append(
                    f"{label}: manifest configuration is {manifest.get('configuration')!r}"
                )

            requested = manifest.get("requestedClientExtent") or {}
            if (requested.get("width"), requested.get("height")) != (width, height):
                failures.append(f"{label}: requested extent is {requested}")
            shot = manifest.get("screenshot") or {}
            if (shot.get("width"), shot.get("height")) != (width, height):
                failures.append(
                    f"{label}: captured extent is {shot.get('width')}x{shot.get('height')}"
                )

            expected_decoration = (
                "undecorated" if (width, height) == (2560, 1440) else "decorated"
            )
            if manifest.get("windowDecoration") != expected_decoration:
                failures.append(
                    f"{label}: decoration is {manifest.get('windowDecoration')!r}, "
                    f"expected {expected_decoration!r}"
                )

            ui_dpi = manifest.get("uiDpi") or {}
            if abs(float(ui_dpi.get("requested", 0.0)) - dpi) > 1e-3 or abs(
                float(ui_dpi.get("applied", 0.0)) - dpi
            ) > 1e-3:
                failures.append(f"{label}: uiDpi is {ui_dpi}, expected {dpi}")
            if ui_dpi.get("source") != "imgui-font-global-scale":
                failures.append(f"{label}: uiDpi source is {ui_dpi.get('source')!r}")
            if abs(float(state.get("uiDpi", {}).get("applied", 0.0)) - dpi) > 1e-3:
                failures.append(f"{label}: app state uiDpi disagrees with the request")

            audit = shot.get("audit") or {}
            for key, floor in AUDIT_FLOORS.items():
                value = audit.get(key)
                if value is None or float(value) < floor:
                    failures.append(f"{label}: audit {key}={value} below floor {floor}")
            if int(audit.get("uniqueColors", 0)) < UNIQUE_COLOR_FLOOR:
                failures.append(
                    f"{label}: audit uniqueColors={audit.get('uniqueColors')} below "
                    f"{UNIQUE_COLOR_FLOOR}"
                )
            if float(audit.get("nonBlackRatio", 0.0)) >= UNIFORM_SURFACE_NON_BLACK_RATIO and int(
                audit.get("uniqueColors", 0)
            ) < UNIFORM_SURFACE_MIN_COLORS:
                failures.append(
                    f"{label}: audit is a uniform non-editor surface "
                    f"(nonBlackRatio={audit.get('nonBlackRatio')} "
                    f"uniqueColors={audit.get('uniqueColors')})"
                )

            if png_path.is_file():
                actual = _sha256(png_path)
                if actual != shot.get("sha256"):
                    failures.append(
                        f"{label}: client.png digest {actual[:16]} differs from the "
                        f"manifest {str(shot.get('sha256'))[:16]}"
                    )

            steps = sorted((root / "app" / "steps").glob("*.json")) if (
                root / "app" / "steps"
            ).is_dir() else []
            if not steps:
                failures.append(f"{label}: no step artifacts")
            if not str(state.get("state", {}).get("state", "")):
                failures.append(f"{label}: terminal state is missing")

            for log_name in ("stderr.log", "stdout.log"):
                log_path = root / log_name
                if not log_path.is_file():
                    failures.append(f"{label}: {log_name} is missing")
                    continue
                text = log_path.read_text(encoding="utf-8", errors="replace")
                for pattern in FORBIDDEN_LOG_PATTERNS:
                    if pattern in text:
                        failures.append(f"{label}: {log_name} contains {pattern!r}")

            roots.append(
                {
                    "label": label,
                    "configuration": configuration,
                    "case": case,
                    "width": width,
                    "height": height,
                    "dpi": dpi,
                    "steps": len(steps),
                    "terminalState": state.get("state", {}).get("state"),
                    "transport": shot.get("transport"),
                    "captureAttempts": shot.get("captureAttempts"),
                    "pngSha256": shot.get("sha256"),
                    "executableSha256": (manifest.get("identity") or {})
                    .get("executable", {})
                    .get("sha256"),
                    "sourceIdentitySha256": identity.get("aggregateSha256"),
                    "audit": audit,
                    "path": root,
                }
            )

    identities = {r["sourceIdentitySha256"] for r in roots}
    if len(identities) != 1:
        failures.append(f"matrix spans {len(identities)} scoped source identities: {identities}")
    for configuration in sorted({r["configuration"] for r in roots}):
        executables = {
            r["executableSha256"] for r in roots if r["configuration"] == configuration
        }
        if len(executables) != 1:
            failures.append(
                f"{configuration} spans {len(executables)} executable digests: {executables}"
            )

    digests = {}
    for record in roots:
        digests.setdefault(record["pngSha256"], []).append(record["label"])
    for digest, labels in sorted(digests.items()):
        if len(labels) > 1:
            failures.append(
                f"client.png digest {str(digest)[:16]} is shared by {labels}"
            )

    groups: dict[tuple, list[dict]] = {}
    for record in roots:
        groups.setdefault(
            (record["configuration"], record["case"], record["width"], record["height"]),
            [],
        ).append(record)

    difference_rows = []
    for key in sorted(groups):
        by_dpi = {record["dpi"]: record for record in groups[key]}
        base = by_dpi.get(1.0)
        if base is None:
            failures.append(f"{'/'.join(map(str, key))}: no dpi=1 baseline run")
            continue
        for dpi in sorted(d for d in by_dpi if d != 1.0):
            other = by_dpi[dpi]
            try:
                fraction, sampled = _sampled_difference(
                    base["path"] / "client.png", other["path"] / "client.png"
                )
            except Exception as exc:  # noqa: BLE001 - report and fail closed
                failures.append(f"{other['label']}: difference check errored: {exc}")
                continue
            difference_rows.append(
                {
                    "group": f"{key[0]} {key[1]} {key[2]}x{key[3]}",
                    "dpi": dpi,
                    "differenceFraction": round(fraction, 6),
                    "sampled": sampled,
                }
            )
            if fraction < min_dpi_difference:
                failures.append(
                    f"{other['label']}: frame differs from the dpi=1 baseline in only "
                    f"{fraction:.6f} of sampled pixels (< {min_dpi_difference}); the "
                    f"capture is stale or the UI DPI lever had no observable effect"
                )

    report = {
        "documentKind": "mgif.editor-live-matrix-verification",
        "schemaVersion": 1,
        "matrixRoot": str(matrix_root),
        "scopedSourceIdentity": sorted(identities)[0] if identities else None,
        "rootCount": len(roots),
        "minDpiDifference": min_dpi_difference,
        "failures": failures,
        "notes": notes,
        "dpiDifferenceMeasurements": difference_rows,
        "roots": [
            {k: v for k, v in record.items() if k != "path"} for record in roots
        ],
    }
    (matrix_root / "verification.json").write_text(
        json.dumps(report, indent=2, sort_keys=True) + "\n", encoding="utf-8"
    )

    print(f"roots verified: {len(roots)}")
    print(f"scoped source identity: {report['scopedSourceIdentity']}")
    for configuration in sorted({r['configuration'] for r in roots}):
        subset = [r for r in roots if r["configuration"] == configuration]
        print(
            f"  {configuration}: {len(subset)} roots, executable "
            f"{str(subset[0]['executableSha256'])[:16]}"
        )
    if difference_rows:
        fractions = [row["differenceFraction"] for row in difference_rows]
        print(
            "dpi difference vs dpi=1 baseline: min=%.6f max=%.6f (floor %.4f)"
            % (min(fractions), max(fractions), min_dpi_difference)
        )
    if failures:
        print(f"FAILURES: {len(failures)}")
        for failure in failures[:40]:
            print(f"  - {failure}")
        return 1
    print("matrix verification passed")
    return 0


def main(argv: list[str]) -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--matrix-root", required=True, type=Path)
    parser.add_argument("--min-dpi-difference", type=float, default=0.01)
    args = parser.parse_args(argv)
    if not args.matrix_root.is_dir():
        print(f"matrix root is not a directory: {args.matrix_root}", file=sys.stderr)
        return 2
    return verify(args.matrix_root, args.min_dpi_difference)


if __name__ == "__main__":
    raise SystemExit(main(sys.argv[1:]))
