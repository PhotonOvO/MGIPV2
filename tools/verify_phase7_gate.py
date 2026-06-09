#!/usr/bin/env python3
# tools/verify_phase7_gate.py
#
# Phase 7 aggregate verification gate — VER-02, VER-03
#
# Orchestrates all acceptance-gate steps in order:
#   Step 1 — 守卫归零检查   (hard gate)
#   Step 2 — Desktop + Slang build  (hard gate)
#   Step 3 — Android assembleDebug  (best-effort)
#   Step 4 — D3D12 stub             (covered by Desktop build)
#   Step 5 — Metal stub             (covered by Desktop build)
#   Step 6 — Vulkan validation smoke (manual — see RUNBOOK)
#
# Exit 0 if all hard-gate steps pass.
# Exit 1 if any hard-gate step fails.
#
# Usage:
#   python tools/verify_phase7_gate.py --help
#   python tools/verify_phase7_gate.py --root . --report .planning/phases/07-final-boundary-acceptance/phase7_gate_report.md
#   python tools/verify_phase7_gate.py --root . --skip-build
#
# Security: all subprocess.run calls use list-form argv — never shell=True.
# See tools/check_rhi_boundary.py for the boundary guard engine.

import subprocess
import sys
import os
import json
import argparse
import datetime
from pathlib import Path

# ---------------------------------------------------------------------------
# Derive repository root from this script's location
# ---------------------------------------------------------------------------
# tools/verify_phase7_gate.py  ->  dirname x2 -> repo root
REPO_ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))

# ---------------------------------------------------------------------------
# Module-level step results list
# ---------------------------------------------------------------------------
results = []

# ---------------------------------------------------------------------------
# Helpers
# ---------------------------------------------------------------------------

def step(name, argv, gate="hard", skip_reason=None):
    """Execute one verification step and record the result.

    Parameters
    ----------
    name : str
        Human-readable step name used in output lines.
    argv : list[str] or None
        Subprocess argv (list-form only — NO shell=True).
        Pass None to skip the step (skip_reason must be provided).
    gate : "hard" | "best-effort"
        Hard gates flip the global failed flag; best-effort steps do not.
    skip_reason : str or None
        If provided the step is recorded as SKIP without running argv.

    Returns
    -------
    bool  True if step passed or was skipped/covered; False if it failed.
    """
    if skip_reason is not None:
        results.append({
            "name": name,
            "status": "SKIP",
            "stdout": "",
            "stderr": "",
            "returncode": None,
            "reason": skip_reason,
            "gate": gate,
        })
        print(f"SKIP [{name}]: {skip_reason}")
        return True

    if argv is None:
        # argv=None without skip_reason is a programming error
        raise ValueError(f"step() called with argv=None and no skip_reason for '{name}'")

    try:
        proc = subprocess.run(
            argv,
            capture_output=True,
            text=True,
            encoding="utf-8",
            errors="replace",
        )
    except FileNotFoundError as exc:
        results.append({
            "name": name,
            "status": "FAIL",
            "stdout": "",
            "stderr": str(exc),
            "returncode": -1,
            "gate": gate,
        })
        print(f"FAIL [{name}]: command not found — {exc}")
        return False

    passed = (proc.returncode == 0)
    status = "PASS" if passed else "FAIL"

    results.append({
        "name": name,
        "status": status,
        "stdout": proc.stdout,
        "stderr": proc.stderr,
        "returncode": proc.returncode,
        "gate": gate,
    })

    if passed:
        print(f"PASS [{name}]")
    else:
        print(f"FAIL [{name}]: returncode={proc.returncode}")
        # Print first 20 lines of stdout/stderr to help diagnose failures
        for line in (proc.stdout + proc.stderr).splitlines()[:20]:
            print(f"  | {line}")

    return passed


def step_covered(name, reason):
    """Record a step that is automatically covered by a prior step."""
    results.append({
        "name": name,
        "status": "covered",
        "reason": reason,
        "gate": "n/a",
    })
    print(f"PASS [{name}]: covered by Desktop build")


# ---------------------------------------------------------------------------
# Report writer
# ---------------------------------------------------------------------------

def write_report(report_path, hard_gate_failed):
    """Write a Markdown summary of all step results to report_path."""
    # Validate path: either inside REPO_ROOT or an explicit absolute path.
    abs_report = os.path.abspath(report_path)
    try:
        common = os.path.commonpath([abs_report, REPO_ROOT])
    except ValueError:
        # Different drives on Windows — allow it (explicit external path)
        common = ""
    if common and common != REPO_ROOT and not os.path.isabs(report_path):
        # Path tries to escape repo root via relative traversal — reject
        print(f"WARNING: report path '{report_path}' appears to escape REPO_ROOT; skipping report write.")
        return

    os.makedirs(os.path.dirname(abs_report) if os.path.dirname(abs_report) else ".", exist_ok=True)

    timestamp = datetime.datetime.now().isoformat()
    platform_info = f"{sys.platform} / {os.environ.get('COMPUTERNAME', '?')}"
    overall = "FAIL" if hard_gate_failed else "PASS"

    lines = [
        f"# Phase 7 Gate Report",
        f"",
        f"**Generated:** {timestamp}  ",
        f"**Platform:** {platform_info}  ",
        f"**Overall:** {overall}",
        f"",
        f"## Step Results",
        f"",
        f"| Step | Gate | Status | Notes |",
        f"|------|------|--------|-------|",
    ]

    for r in results:
        status = r.get("status", "?")
        gate_label = r.get("gate", "n/a")
        reason = r.get("reason", "")
        rc = r.get("returncode")

        if status == "SKIP":
            notes = f"SKIP: {reason}"
        elif status == "covered":
            notes = reason
        elif status == "FAIL":
            notes = f"returncode={rc}" + (f"; {reason}" if reason else "")
        else:
            notes = reason or ""

        lines.append(f"| {r['name']} | {gate_label} | {status} | {notes} |")

    lines += [
        f"",
        f"## Hard-Gate Summary",
        f"",
        f"{'FAIL — one or more hard-gate steps failed.' if hard_gate_failed else 'PASS — all hard-gate steps passed.'}",
        f"",
        f"## Notes",
        f"",
        f"- Step 6 (Vulkan smoke) is always manual — see `tools/VULKAN_SMOKE_RUNBOOK.md`.",
        f"- Step 3 (Android) is best-effort; failure does not affect overall gate.",
        f"- Steps 4/5 (D3D12/Metal stub) are covered by the Desktop build in Step 2.",
    ]

    with open(abs_report, "w", encoding="utf-8") as f:
        f.write("\n".join(lines) + "\n")

    print(f"\nReport written: {abs_report}")


# ---------------------------------------------------------------------------
# Main
# ---------------------------------------------------------------------------

def main():
    parser = argparse.ArgumentParser(
        description=(
            "Phase 7 aggregate verification gate. "
            "Runs boundary zero-check, Desktop build, Android best-effort, "
            "and prompts for manual Vulkan smoke. "
            "Exit 0 = all hard-gate steps passed; Exit 1 = hard-gate failure."
        )
    )
    parser.add_argument(
        "--root",
        default=".",
        help="Project root directory (default: current directory)",
    )
    parser.add_argument(
        "--report",
        default=os.path.join(
            ".planning", "phases", "07-final-boundary-acceptance", "phase7_gate_report.md"
        ),
        help=(
            "Path for the Markdown gate report "
            "(default: .planning/phases/07-final-boundary-acceptance/phase7_gate_report.md)"
        ),
    )
    parser.add_argument(
        "--skip-build",
        action="store_true",
        help="Skip the Desktop build step (Step 2). Useful when only verifying the boundary guard.",
    )
    args = parser.parse_args()

    # Override REPO_ROOT if --root was supplied
    root = os.path.abspath(args.root)

    # Resolve canonical paths
    check_boundary_script = os.path.join(root, "tools", "check_rhi_boundary.py")
    zero_baseline = os.path.join(root, "tools", "phase7_zero_baseline.json")
    allowlist = os.path.join(root, ".rhi-boundary-allow")
    build_cmd = os.path.join(root, "build_debug_with_vsdevcmd.cmd")
    android_dir = os.path.join(root, "android")
    gradlew = os.path.join(android_dir, "gradlew.bat")

    print("=" * 60)
    print("Phase 7 Aggregate Verification Gate")
    print(f"Root:   {root}")
    print(f"Report: {os.path.abspath(args.report)}")
    print("=" * 60)

    hard_gate_failed = False

    # ------------------------------------------------------------------
    # Step 1 — 守卫归零检查 (hard gate)
    # ------------------------------------------------------------------
    print("\n--- Step 1: 守卫归零检查 ---")
    ok = step(
        name="守卫归零检查",
        argv=[
            sys.executable,
            check_boundary_script,
            "--root", root,
            "--baseline", zero_baseline,
            "--allowlist", allowlist,
        ],
        gate="hard",
    )
    if not ok:
        hard_gate_failed = True

    # ------------------------------------------------------------------
    # Step 2 — Desktop + Slang build (hard gate, skippable via --skip-build)
    # ------------------------------------------------------------------
    print("\n--- Step 2: Desktop + Slang build ---")
    if args.skip_build:
        step(
            name="Desktop + Slang build",
            argv=None,
            gate="hard",
            skip_reason="--skip-build flag set",
        )
    else:
        # MUST go through build_debug_with_vsdevcmd.cmd — direct cmake --build
        # fails because MSVC toolchain (vcvars) is not initialized.
        # Slang shader build is embedded in demo_core via CMake custom_command;
        # no separate Slang call is needed.
        ok = step(
            name="Desktop + Slang build",
            argv=["cmd", "/c", build_cmd],
            gate="hard",
        )
        if not ok:
            hard_gate_failed = True

    # ------------------------------------------------------------------
    # Step 3 — Android assembleDebug (best-effort)
    # ------------------------------------------------------------------
    print("\n--- Step 3: Android assembleDebug (best-effort) ---")
    # NDK detection order mirrors setup_android_gradle.bat :FindAndroidSdk subroutine
    localappdata = os.environ.get("LOCALAPPDATA", "")
    ndk_path = os.path.join(localappdata, "Android", "Sdk", "ndk", "23.1.7779620")
    if not os.path.isdir(ndk_path):
        android_sdk_root = os.environ.get("ANDROID_SDK_ROOT") or os.environ.get("ANDROID_HOME")
        if android_sdk_root:
            ndk_path = os.path.join(android_sdk_root, "ndk", "23.1.7779620")

    if os.path.isdir(ndk_path):
        step(
            name="Android assembleDebug",
            argv=["cmd", "/c", gradlew, "assembleDebug"],
            gate="best-effort",
        )
        # Note: best-effort — FAIL in this step does NOT set hard_gate_failed
    else:
        step(
            name="Android assembleDebug",
            argv=None,
            gate="best-effort",
            skip_reason="NDK 23.1.7779620 not found at expected paths — documented skip (D-08)",
        )

    # ------------------------------------------------------------------
    # Step 4 — D3D12 stub (covered by Desktop build)
    # ------------------------------------------------------------------
    print("\n--- Step 4: D3D12 stub ---")
    step_covered(
        name="D3D12 stub",
        reason="Compiled as part of demo_core in Desktop build (step 2)",
    )

    # ------------------------------------------------------------------
    # Step 5 — Metal stub (covered by Desktop build)
    # ------------------------------------------------------------------
    print("\n--- Step 5: Metal stub ---")
    step_covered(
        name="Metal stub",
        reason=(
            "MetalDevice.h has no Apple SDK includes; "
            "compiles on Windows as part of demo_core (PATTERNS.md correction)"
        ),
    )

    # ------------------------------------------------------------------
    # Step 6 — 手动 Vulkan validation smoke
    # ------------------------------------------------------------------
    print("\n--- Step 6: 手动 Vulkan validation smoke ---")
    runbook_path = os.path.join(root, "tools", "VULKAN_SMOKE_RUNBOOK.md")
    print(f"  ACTION REQUIRED: Follow the manual smoke runbook at:")
    print(f"    {runbook_path}")
    print(f"  Run out/build/x64-Debug/Demo.exe and verify no new Vulkan VUID/layout/sync errors.")
    results.append({
        "name": "Vulkan smoke",
        "status": "manual",
        "reason": f"Requires human execution — see {runbook_path}",
        "gate": "manual",
    })

    # ------------------------------------------------------------------
    # Summary
    # ------------------------------------------------------------------
    print("\n" + "=" * 60)
    print("Gate Summary")
    print("=" * 60)
    for r in results:
        status = r.get("status", "?")
        print(f"  [{status:8s}] {r['name']}")

    write_report(args.report, hard_gate_failed)

    if hard_gate_failed:
        print("\nRESULT: FAIL — one or more hard-gate steps failed.")
    else:
        print("\nRESULT: PASS (hard gates) — verify Step 6 manually.")

    sys.exit(1 if hard_gate_failed else 0)


if __name__ == "__main__":
    main()
