"""Compare two rendered frames and report whether they match.

Used as the rendering regression gate for the GPU-driven refactor: the contract
tests only check that the code has the expected shape, so a change that still
compiles but draws the wrong geometry would pass them. This script supplies the
missing signal.

Usage:
    python tools/compare_frames.py BEFORE.png AFTER.png [--max-diff-pct 0.1]

Exit code 0 when the frames match within tolerance, 1 when they differ.

The comparison is deliberately simple: decode both images, compare per pixel,
report the share of pixels whose max channel delta exceeds a small threshold.
A pure-Python PNG decoder is not worth writing, so this prefers Pillow and
falls back to a byte comparison when Pillow is unavailable.
"""

from __future__ import annotations

import argparse
import hashlib
import sys
from pathlib import Path


def sha256(path: Path) -> str:
    digest = hashlib.sha256()
    with path.open("rb") as handle:
        for chunk in iter(lambda: handle.read(1 << 20), b""):
            digest.update(chunk)
    return digest.hexdigest()


def compare_with_pillow(before: Path, after: Path, channel_threshold: int) -> tuple[int, int, int]:
    from PIL import Image, ImageChops  # noqa: PLC0415 - optional dependency

    with Image.open(before) as left_image, Image.open(after) as right_image:
        left = left_image.convert("RGBA")
        right = right_image.convert("RGBA")

    if left.size != right.size:
        raise SystemExit(
            f"size mismatch: {before.name} is {left.size}, {after.name} is {right.size}"
        )

    diff = ImageChops.difference(left, right)
    # A pixel counts as different only when some channel moves beyond the noise floor;
    # float->unorm rounding alone must not fail the gate.
    significant = diff.convert("L").point(lambda value: 255 if value > channel_threshold else 0)
    differing = significant.histogram()[255]
    total = left.size[0] * left.size[1]
    max_delta = max(diff.getextrema(), key=lambda pair: pair[1])[1]
    return differing, total, max_delta


def main(argv: list[str]) -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("before", type=Path)
    parser.add_argument("after", type=Path)
    parser.add_argument(
        "--max-diff-pct",
        type=float,
        default=0.1,
        help="maximum share of differing pixels, in percent (default 0.1)",
    )
    parser.add_argument(
        "--channel-threshold",
        type=int,
        default=1,
        help="per-channel delta that counts as a difference (default 1)",
    )
    options = parser.parse_args(argv)

    for path in (options.before, options.after):
        if not path.is_file():
            print(f"missing frame: {path}", file=sys.stderr)
            return 1

    before_hash = sha256(options.before)
    after_hash = sha256(options.after)

    print(f"before: {options.before}  sha256={before_hash}")
    print(f"after : {options.after}  sha256={after_hash}")

    if before_hash == after_hash:
        print("result: identical (byte-for-byte)")
        return 0

    try:
        differing, total, max_delta = compare_with_pillow(
            options.before, options.after, options.channel_threshold
        )
    except ImportError:
        print("result: pixel comparison unavailable (Pillow is not installed)")
        print("        byte comparison already showed the files differ")
        return 1

    diff_pct = 100.0 * differing / total if total else 0.0
    print(f"pixels: {differing}/{total} differ ({diff_pct:.4f}%), max channel delta {max_delta}")

    if diff_pct <= options.max_diff_pct:
        print(f"result: match (within {options.max_diff_pct}%)")
        return 0

    print(f"result: MISMATCH (exceeds {options.max_diff_pct}%)")
    return 1


if __name__ == "__main__":
    raise SystemExit(main(sys.argv[1:]))
