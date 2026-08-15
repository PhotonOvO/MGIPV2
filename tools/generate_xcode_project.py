#!/usr/bin/env python3

"""Generate an Apple Xcode project with the newest supported Xcode install."""

from __future__ import annotations

import argparse
from dataclasses import dataclass
import os
from pathlib import Path
import plistlib
import re
import shlex
import subprocess
import sys
from typing import Iterable, Sequence


ROOT = Path(__file__).resolve().parents[1]
MINIMUM_XCODE_MAJOR = 26

PLATFORM_OPTIONS = {
    "macos": (
        "build-macos-xcode{major}",
        (
            "-DCMAKE_OSX_SYSROOT=macosx",
            "-DCMAKE_OSX_DEPLOYMENT_TARGET=26.0",
            "-DDEMO_ENABLE_METAL4=ON",
            "-DDEMO_ENABLE_VULKAN=OFF",
            "-DUSE_SLANG=ON",
        ),
    ),
    "ios-simulator": (
        "build-ios-xcode{major}",
        (
            "-DCMAKE_SYSTEM_NAME=iOS",
            "-DCMAKE_OSX_SYSROOT=iphonesimulator",
            "-DCMAKE_OSX_ARCHITECTURES=arm64",
            "-DCMAKE_OSX_DEPLOYMENT_TARGET=26.0",
            "-DDEMO_ENABLE_METAL4=ON",
            "-DDEMO_ENABLE_VULKAN=OFF",
            "-DUSE_SLANG=ON",
            "-DBUILD_TESTING=OFF",
        ),
    ),
    "ios-device": (
        "build-ios-device-xcode{major}",
        (
            "-DCMAKE_SYSTEM_NAME=iOS",
            "-DCMAKE_OSX_SYSROOT=iphoneos",
            "-DCMAKE_OSX_ARCHITECTURES=arm64",
            "-DCMAKE_OSX_DEPLOYMENT_TARGET=26.0",
            "-DDEMO_ENABLE_METAL4=ON",
            "-DDEMO_ENABLE_VULKAN=OFF",
            "-DUSE_SLANG=ON",
            "-DBUILD_TESTING=OFF",
        ),
    ),
}


class GenerationError(RuntimeError):
    pass


@dataclass(frozen=True)
class XcodeInstallation:
    app_path: Path
    developer_dir: Path
    version_text: str
    version: tuple[int, ...]
    build_version: str
    is_prerelease: bool

    @property
    def major(self) -> int:
        return self.version[0]

    @property
    def display_name(self) -> str:
        prerelease = " prerelease" if self.is_prerelease else ""
        build = f" ({self.build_version})" if self.build_version else ""
        return f"Xcode {self.version_text}{prerelease}{build}"


def parse_version(version_text: str) -> tuple[int, ...]:
    parts = tuple(int(value) for value in re.findall(r"\d+", version_text))
    if not parts:
        raise GenerationError(f"Invalid Xcode version: {version_text!r}")
    return parts


def xcode_app_from_path(candidate: Path) -> Path:
    path = candidate.expanduser().absolute()
    if path.name == "Developer" and path.parent.name == "Contents":
        return path.parent.parent
    if path.name == "Contents" and path.parent.suffix.lower() == ".app":
        return path.parent
    return path


def read_xcode_installation(candidate: Path) -> XcodeInstallation | None:
    app_path = xcode_app_from_path(candidate)
    info_path = app_path / "Contents" / "Info.plist"
    version_path = app_path / "Contents" / "version.plist"
    developer_dir = app_path / "Contents" / "Developer"
    xcodebuild = developer_dir / "usr" / "bin" / "xcodebuild"
    if app_path.suffix.lower() != ".app" or not info_path.is_file() or not xcodebuild.is_file():
        return None

    try:
        with info_path.open("rb") as stream:
            info = plistlib.load(stream)
    except (OSError, plistlib.InvalidFileException):
        return None

    if info.get("CFBundleIdentifier") != "com.apple.dt.Xcode":
        return None

    version_info: dict[str, object] = {}
    if version_path.is_file():
        try:
            with version_path.open("rb") as stream:
                version_info = plistlib.load(stream)
        except (OSError, plistlib.InvalidFileException):
            version_info = {}

    version_text = str(
        version_info.get("CFBundleShortVersionString")
        or info.get("CFBundleShortVersionString", "")
    )
    try:
        version = parse_version(version_text)
    except GenerationError:
        return None

    name = app_path.name.lower()
    prerelease_markers = ("beta", "preview", "release candidate", "-rc", "_rc")
    is_prerelease = any(marker in name for marker in prerelease_markers)
    return XcodeInstallation(
        app_path=app_path,
        developer_dir=developer_dir,
        version_text=version_text,
        version=version,
        build_version=str(
            version_info.get("ProductBuildVersion") or info.get("DTXcodeBuild", "")
        ),
        is_prerelease=is_prerelease,
    )


def _directory_candidates(directory: Path) -> Iterable[Path]:
    try:
        children = tuple(directory.iterdir())
    except OSError:
        return ()
    return (
        child
        for child in children
        if child.is_dir()
        and child.suffix.lower() == ".app"
        and child.name.lower().startswith("xcode")
    )


def _selected_developer_dir() -> Path | None:
    try:
        result = subprocess.run(
            ("xcode-select", "-p"),
            check=False,
            capture_output=True,
            text=True,
        )
    except OSError:
        return None
    value = result.stdout.strip()
    return Path(value) if result.returncode == 0 and value else None


def discover_xcodes(
    *,
    search_roots: Sequence[Path] | None = None,
    extra_candidates: Sequence[Path] = (),
    include_system_selection: bool = True,
) -> list[XcodeInstallation]:
    if search_roots is None:
        search_roots = (
            Path("/Applications"),
            Path.home() / "Applications",
            Path.home() / "Downloads",
        )

    candidates: list[Path] = list(extra_candidates)
    for root in search_roots:
        if root.suffix.lower() == ".app":
            candidates.append(root)
        else:
            candidates.extend(_directory_candidates(root))

    extra_search_paths = os.environ.get("MGIP_XCODE_SEARCH_PATHS", "")
    for value in extra_search_paths.split(os.pathsep):
        if not value:
            continue
        path = Path(value)
        if path.suffix.lower() == ".app":
            candidates.append(path)
        else:
            candidates.extend(_directory_candidates(path))

    if include_system_selection:
        developer_dir = os.environ.get("DEVELOPER_DIR")
        if developer_dir:
            candidates.append(Path(developer_dir))
        selected = _selected_developer_dir()
        if selected is not None:
            candidates.append(selected)

    installations: list[XcodeInstallation] = []
    seen: set[Path] = set()
    for candidate in candidates:
        installation = read_xcode_installation(candidate)
        if installation is None:
            continue
        try:
            identity = installation.app_path.resolve()
        except OSError:
            identity = installation.app_path
        if identity in seen:
            continue
        seen.add(identity)
        installations.append(installation)
    return installations


def _version_key(installation: XcodeInstallation) -> tuple[object, ...]:
    padded_version = installation.version[:4] + (0,) * max(0, 4 - len(installation.version))
    stable_tiebreaker = 0 if installation.is_prerelease else 1
    return padded_version, stable_tiebreaker, installation.build_version, str(installation.app_path)


def select_xcode(
    installations: Sequence[XcodeInstallation],
    minimum_major: int = MINIMUM_XCODE_MAJOR,
) -> XcodeInstallation:
    supported = [installation for installation in installations if installation.major >= minimum_major]
    if supported:
        return max(supported, key=_version_key)

    if installations:
        found = ", ".join(
            f"{installation.version_text} at {installation.app_path}"
            for installation in sorted(installations, key=_version_key, reverse=True)
        )
        raise GenerationError(
            f"Xcode {minimum_major} or newer is required; found only: {found}"
        )
    raise GenerationError(
        f"Xcode {minimum_major} or newer was not found. Use --xcode /path/to/Xcode.app."
    )


def resolve_explicit_xcode(path: Path) -> XcodeInstallation:
    installation = read_xcode_installation(path)
    if installation is None:
        raise GenerationError(f"Not a valid Xcode application or Developer directory: {path}")
    if installation.major < MINIMUM_XCODE_MAJOR:
        raise GenerationError(
            f"Xcode {MINIMUM_XCODE_MAJOR} or newer is required; "
            f"{installation.app_path} is {installation.version_text}."
        )
    return installation


def default_build_dir(source_dir: Path, platform: str, xcode_major: int) -> Path:
    directory_template, _ = PLATFORM_OPTIONS[platform]
    return source_dir / directory_template.format(major=xcode_major)


def configure_command(
    source_dir: Path,
    build_dir: Path,
    platform: str,
    extra_cmake_args: Sequence[str],
) -> list[str]:
    _, platform_options = PLATFORM_OPTIONS[platform]
    return [
        "cmake",
        "-S",
        str(source_dir),
        "-B",
        str(build_dir),
        "-G",
        "Xcode",
        *platform_options,
        *extra_cmake_args,
    ]


def parse_args(argv: Sequence[str]) -> argparse.Namespace:
    parser = argparse.ArgumentParser(
        description=(
            "Generate an Xcode project with the newest installed Xcode 26 or newer. "
            "Xcode 27 prerelease applications are intentionally eligible."
        )
    )
    parser.add_argument("--platform", choices=PLATFORM_OPTIONS, default="macos")
    parser.add_argument(
        "--xcode",
        type=Path,
        help="Explicit Xcode .app or Contents/Developer path; overrides automatic selection.",
    )
    parser.add_argument("--source-dir", type=Path, default=ROOT)
    parser.add_argument("--build-dir", type=Path)
    parser.add_argument(
        "--dry-run",
        action="store_true",
        help="Print the selected Xcode and CMake command without generating files.",
    )
    parser.add_argument(
        "cmake_args",
        nargs=argparse.REMAINDER,
        help="Additional CMake arguments after --.",
    )
    return parser.parse_args(argv)


def main(argv: Sequence[str] | None = None) -> int:
    args = parse_args(sys.argv[1:] if argv is None else argv)
    source_dir = args.source_dir.expanduser().absolute()
    if not (source_dir / "CMakeLists.txt").is_file():
        raise GenerationError(f"Source directory has no CMakeLists.txt: {source_dir}")

    override = args.xcode
    if override is None and os.environ.get("MGIP_XCODE_APP"):
        override = Path(os.environ["MGIP_XCODE_APP"])

    if override is not None:
        selected = resolve_explicit_xcode(override)
    else:
        selected = select_xcode(discover_xcodes())

    build_dir = (
        args.build_dir.expanduser().absolute()
        if args.build_dir is not None
        else default_build_dir(source_dir, args.platform, selected.major)
    )
    extra_cmake_args = list(args.cmake_args)
    if extra_cmake_args and extra_cmake_args[0] == "--":
        extra_cmake_args.pop(0)
    command = configure_command(source_dir, build_dir, args.platform, extra_cmake_args)

    print(f"Selected: {selected.display_name}")
    print(f"Xcode app: {selected.app_path}")
    print(f"Developer directory: {selected.developer_dir}")
    print(f"Build directory: {build_dir}")
    print(f"Configure command: {shlex.join(command)}")
    if args.dry_run:
        return 0
    sys.stdout.flush()

    environment = os.environ.copy()
    environment["DEVELOPER_DIR"] = str(selected.developer_dir)
    try:
        result = subprocess.run(command, env=environment, check=False)
    except OSError as error:
        raise GenerationError(f"Could not execute CMake: {error}") from error
    if result.returncode != 0:
        return result.returncode

    project_path = build_dir / "Demo.xcodeproj"
    print(f"Generated: {project_path}")
    print(
        "Open with: "
        + shlex.join(("open", "-a", str(selected.app_path), str(project_path)))
    )
    print(
        "Build with: "
        + f"DEVELOPER_DIR={shlex.quote(str(selected.developer_dir))} "
        + shlex.join(("cmake", "--build", str(build_dir), "--target", "Demo"))
    )
    return 0


if __name__ == "__main__":
    try:
        raise SystemExit(main())
    except GenerationError as error:
        print(f"error: {error}", file=sys.stderr)
        raise SystemExit(2)
