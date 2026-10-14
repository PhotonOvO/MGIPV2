#!/usr/bin/env python3
"""Fail closed when a Ninja build tree contains C++ objects with no recorded deps.

An object whose deps-log entry is ``#deps 0`` never rebuilds when a header it
includes changes.  That silently produces one binary out of translation units
compiled against different revisions of the same class definition (an ODR /
layout mismatch), which shows up later as memory corruption far away from the
real cause.

The audit consumes a ``ninja -t deps`` dump so it runs identically on any host:

    tools\\dump_ninja_deps.cmd G:\\Temp\\<tree> G:\\Temp\\ninjadeps.txt
    python3 tools/check_ninja_deps_orphans.py --deps-dump G:/Temp/ninjadeps.txt

Exit status is 0 only when every scanned object records at least one dependency
(or is explicitly allow-listed), so the check can gate evidence collection.
"""

from __future__ import annotations

import argparse
import re
import sys
from pathlib import Path

_HEADER_LINE = re.compile(r"^(?P<output>\S+\.obj): #deps (?P<count>\d+), deps mtime \d+ \((?P<state>\w+)\)")


def parse_dump(text: str) -> list[tuple[str, int, str]]:
    entries: list[tuple[str, int, str]] = []
    for line in text.splitlines():
        match = _HEADER_LINE.match(line.strip())
        if match is None:
            continue
        entries.append((match.group("output"), int(match.group("count")), match.group("state")))
    return entries


def main(argv: list[str] | None = None) -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--deps-dump", required=True, type=Path, help="file produced by 'ninja -t deps'")
    parser.add_argument("--tree", type=Path, help="optional build tree used to report object existence")
    parser.add_argument(
        "--allow-list",
        type=Path,
        help="optional file of object paths (one per line) permitted to record zero deps",
    )
    parser.add_argument("--report", type=Path, help="optional path to write the orphan list")
    args = parser.parse_args(argv)

    dump = args.deps_dump.read_text(encoding="utf-8", errors="replace")
    entries = parse_dump(dump)
    if not entries:
        print(f"deps audit failed: no object entries found in {args.deps_dump}", file=sys.stderr)
        return 2

    allowed: set[str] = set()
    if args.allow_list is not None:
        allowed = {
            line.strip().replace("\\", "/")
            for line in args.allow_list.read_text(encoding="utf-8", errors="replace").splitlines()
            if line.strip() and not line.strip().startswith("#")
        }

    orphans: list[str] = []
    stale: list[str] = []
    for output, count, state in entries:
        normalized = output.replace("\\", "/")
        if state != "VALID":
            stale.append(f"{normalized} ({state})")
        if count == 0 and normalized not in allowed:
            orphans.append(normalized)

    if args.report is not None:
        args.report.write_text("\n".join(orphans) + ("\n" if orphans else ""), encoding="utf-8")

    missing: list[str] = []
    if args.tree is not None and orphans:
        for orphan in orphans:
            if not (args.tree / orphan).exists():
                missing.append(orphan)

    print(
        "ninja deps audit: objects={objects} orphan_deps0={orphans} non_valid={stale} allow_listed={allowed}".format(
            objects=len(entries),
            orphans=len(orphans),
            stale=len(stale),
            allowed=len(allowed),
        )
    )
    for orphan in orphans[:40]:
        print(f"  orphan: {orphan}")
    if len(orphans) > 40:
        print(f"  ... {len(orphans) - 40} more orphans")

    if orphans or stale:
        print(
            "ninja deps audit failed: rebuild the listed objects so every translation unit "
            "records its header dependencies before trusting binary evidence",
            file=sys.stderr,
        )
        return 1
    if missing:
        print("ninja deps audit failed: orphan objects are absent from the tree", file=sys.stderr)
        return 1
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
