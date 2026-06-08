#!/usr/bin/env python3
# tools/check_rhi_boundary.py
#
# RHI boundary guard — Phase 1, VER-01
#
# Scans app/, render/, scene/, loader/, gfx/ for three classes of violations:
#   backend_include  — #include referencing rhi/vulkan/, rhi/d3d12/, rhi/metal/ paths
#   vk_token         — Vk*/VK_*/Vma* token usage (word-boundary matched)
#   native_getter    — getBackendDeviceHandle(), resolve*BackendHandle() escape calls
#
# Ratchet model: counts can only decrease over time.
#   current_total <= baseline_total -> PASS (exit 0)
#   current_total >  baseline_total -> FAIL (exit 1, prints regression delta)
#
# Usage:
#   python tools/check_rhi_boundary.py --root . --baseline .rhi-boundary-baseline.json
#   python tools/check_rhi_boundary.py --root . --baseline .rhi-boundary-baseline.json --update-baseline
#
# See .rhi-boundary-allow for exempted path prefixes.
# See .planning/phases/01-boundary-guard-baseline/ for design decisions D-01..D-09.

import re
import json
import sys
import os
import argparse
from pathlib import Path

# ---------------------------------------------------------------------------
# Scan configuration
# ---------------------------------------------------------------------------

SCAN_DIRS = ['app', 'render', 'scene', 'loader', 'gfx']

EXCLUDE_PATTERNS = [
    'android/app/.cxx',
    'android/build',
    'third_party',
    '_autogen',
    'out/',
]

# Three signal patterns (D-08). Do NOT modify these regex literals without
# updating the baseline — any change will invalidate the ratchet counts.
SIGNALS = {
    'backend_include': re.compile(
        r'#\s*include\s*["\'](?:\.\./)*(?:rhi/vulkan/|rhi/d3d12/|rhi/metal/)[^"\']*["\']'
    ),
    'vk_token': re.compile(
        r'(?<![A-Za-z0-9_])'
        r'(Vk[A-Z][A-Za-z0-9_]+|VK_[A-Z][A-Z0-9_]+|Vma[A-Z][A-Za-z0-9_]+)'
        r'(?![A-Za-z0-9_])'
    ),
    'native_getter': re.compile(
        r'(?<![A-Za-z0-9_])'
        r'(?:getBackendDeviceHandle|getBackendPhysicalDeviceHandle|'
        r'getBackendInstanceHandle|resolve[A-Z][A-Za-z0-9]*BackendHandle)'
        r'\s*\('
    ),
}

# ---------------------------------------------------------------------------
# Allowlist loader
# ---------------------------------------------------------------------------

def load_allowlist(path):
    """Load path-prefix exemptions from .rhi-boundary-allow.

    Format: each non-blank, non-comment line is '<category>: <path-prefix>'.
    Returns a list of path-prefix strings (forward-slash normalised).
    """
    if not os.path.exists(path):
        return []
    allowed = []
    with open(path, encoding='utf-8', errors='ignore') as f:
        for line in f:
            line = line.strip()
            if not line or line.startswith('#'):
                continue
            parts = line.split(':', 1)
            prefix = parts[-1].strip().replace('\\', '/')
            if prefix:
                allowed.append(prefix)
    return allowed

# ---------------------------------------------------------------------------
# Scanner
# ---------------------------------------------------------------------------

def scan(root, allowlist_prefixes):
    """Scan SCAN_DIRS under root for the three violation signals.

    Returns:
        dict[signal_name] = {
            'total': int,
            'header_only': int,
            'files': { rel_path: {'total': int, 'header': int} }
        }
    """
    results = {sig: {'total': 0, 'header_only': 0, 'files': {}} for sig in SIGNALS}

    for scan_dir in SCAN_DIRS:
        dirpath = os.path.join(root, scan_dir)
        if not os.path.isdir(dirpath):
            continue

        for r, _dirs, files in os.walk(dirpath):
            for fname in files:
                if not (fname.endswith('.h') or fname.endswith('.cpp')):
                    continue

                fpath = os.path.join(r, fname).replace('\\', '/')
                rel = os.path.relpath(fpath, root).replace('\\', '/')

                # Hard exclusion (EXCLUDE_PATTERNS — no allowlist override)
                if any(excl in fpath for excl in EXCLUDE_PATTERNS):
                    continue

                # Allowlist exclusion (path-prefix match)
                if any(rel.startswith(allowed) for allowed in allowlist_prefixes):
                    continue

                is_header = fname.endswith('.h')

                try:
                    with open(fpath, encoding='utf-8', errors='ignore') as fobj:
                        lines = fobj.readlines()
                except Exception:
                    continue

                for signal, pattern in SIGNALS.items():
                    count = 0
                    for line in lines:
                        # Strip // comment portion before matching vk_token to avoid
                        # counting token names that only appear in inline comments.
                        scan_line = line.split('//')[0] if signal == 'vk_token' else line
                        count += len(pattern.findall(scan_line))

                    if count:
                        results[signal]['total'] += count
                        if is_header:
                            results[signal]['header_only'] += count
                        results[signal]['files'][rel] = {
                            'total': count,
                            'header': count if is_header else 0,
                        }

    return results

# ---------------------------------------------------------------------------
# Main
# ---------------------------------------------------------------------------

def main():
    parser = argparse.ArgumentParser(
        description='RHI boundary guard — ratchet check for platform-isolation violations.'
    )
    parser.add_argument(
        '--root', default='.',
        help='Project root directory to scan (default: current directory)'
    )
    parser.add_argument(
        '--baseline', default='.rhi-boundary-baseline.json',
        help='Path to baseline JSON snapshot (relative to --root or absolute)'
    )
    parser.add_argument(
        '--allowlist', default='.rhi-boundary-allow',
        help='Path to allowlist file (relative to --root or absolute)'
    )
    parser.add_argument(
        '--update-baseline', action='store_true',
        help='Write a new baseline if all signal counts are <= current baseline. '
             'Refuses to increase any count (ratchet protection).'
    )
    args = parser.parse_args()

    root = os.path.abspath(args.root)

    # Resolve baseline and allowlist paths (support both relative-to-root and absolute)
    baseline_path = args.baseline if os.path.isabs(args.baseline) \
        else os.path.join(root, args.baseline)
    allowlist_path = args.allowlist if os.path.isabs(args.allowlist) \
        else os.path.join(root, args.allowlist)

    allowlist_prefixes = load_allowlist(allowlist_path)

    current = scan(root, allowlist_prefixes)

    # Load baseline
    if os.path.exists(baseline_path):
        with open(baseline_path, encoding='utf-8') as f:
            baseline = json.load(f)
    else:
        print(
            'WARNING: baseline file not found at ' + baseline_path + '\n'
            '         Treating all signal baselines as 0.\n'
            '         Run with --update-baseline after reviewing counts to establish baseline.'
        )
        baseline = {'version': 1, 'signals': {}}

    # Ratchet comparison
    failed = False
    for signal, data in current.items():
        baseline_signals = baseline.get('signals', {})
        baseline_total = baseline_signals.get(signal, {}).get('total', 0)
        current_total = data['total']

        if current_total > baseline_total:
            delta = current_total - baseline_total
            print(
                f'FAIL [{signal}]: {current_total} violations > baseline {baseline_total}'
                f' (+{delta} regression)'
            )
            # Show new or increased files
            baseline_files = baseline_signals.get(signal, {}).get('files', {})
            for fpath, fdata in sorted(data['files'].items()):
                old_count = baseline_files.get(fpath, {}).get('total', 0)
                if fdata['total'] > old_count:
                    print(f'  NEW/INCREASED: {fpath}: {fdata["total"]} (was {old_count})')
            failed = True
        else:
            print(f'PASS [{signal}]: {current_total}/{baseline_total}')

    # --update-baseline: only write if ratchet allows (no regressions)
    if args.update_baseline:
        if failed:
            print(
                '\nERROR: --update-baseline refused — one or more signals regressed.\n'
                '       Fix the regressions first, then re-run with --update-baseline.'
            )
        else:
            out = {
                'version': 1,
                'description': (
                    'Phase 1 baseline snapshot — honest count of existing violations. '
                    'Ratchet down per phase.'
                ),
                'signals': {},
            }
            for signal, data in current.items():
                out['signals'][signal] = {
                    'total': data['total'],
                    'header_only': data['header_only'],
                    'files': data['files'],
                }
            with open(baseline_path, 'w', encoding='utf-8') as f:
                json.dump(out, f, indent=2)
                f.write('\n')
            print(f'\nBaseline updated: {baseline_path}')

    sys.exit(1 if failed else 0)


if __name__ == '__main__':
    main()
