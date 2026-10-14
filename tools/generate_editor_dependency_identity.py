#!/usr/bin/env python3
"""Generate a fail-closed Dear ImGui identity from final configured build evidence."""

from __future__ import annotations

import argparse
import hashlib
import json
import os
import re
import shlex
import shutil
import stat
import subprocess
import tempfile
from dataclasses import dataclass, field
from pathlib import Path, PurePosixPath
from typing import Any


EXPECTED_REPOSITORY = "https://github.com/ocornut/imgui.git"
EXPECTED_COMMIT = "fd13a1e8923a0a7077b404fc36fd063b25a0c0b5"
EXPECTED_TREE = "0c8986ccb662ea838c04f7bed4cc56d3379132ef"
EXPECTED_TARGET = "imgui_demo"
PREFLIGHT_TARGET = "scene_editor_dependency_preflight"
BASE_TARGET_INPUTS = (
    "imgui.cpp",
    "imgui.h",
    "imgui_demo.cpp",
    "imgui_draw.cpp",
    "imgui_tables.cpp",
    "imgui_widgets.cpp",
)
BACKEND_TARGET_INPUTS = {
    "android": ("backends/imgui_impl_android.cpp", "backends/imgui_impl_android.h"),
    "glfw": ("backends/imgui_impl_glfw.cpp", "backends/imgui_impl_glfw.h"),
}
COMPILED_SUFFIXES = {".c", ".cc", ".cpp", ".cxx", ".m", ".mm"}
REQUIRED_FILE_API_KINDS = {"cache", "codemodel", "toolchains"}
FORBIDDEN_DEPENDENCY_PATTERNS = (
    ("nvpro_core", re.compile(r"(?i)(?<![A-Za-z0-9])nvpro[_-]?core(?:2)?[A-Za-z0-9_]*")),
    ("nvcore", re.compile(r"(?i)(?<![A-Za-z0-9])nvcore(?:2)?[A-Za-z0-9_]*")),
    ("nvapp", re.compile(r"(?i)(?<![A-Za-z0-9])nvapp[A-Za-z0-9_]*")),
    ("nvgui", re.compile(r"(?i)(?<![A-Za-z0-9])nvgui[A-Za-z0-9_]*")),
    ("nvvkgltf", re.compile(r"(?i)(?<![A-Za-z0-9])nvvkgltf[A-Za-z0-9_]*")),
    ("nvvk", re.compile(r"(?i)(?<![A-Za-z0-9])nvvk[A-Za-z0-9_]*")),
    (
        "reference_vulkan_helpers",
        re.compile(
            r"(?i)(?<![A-Za-z0-9])(?:nvh|nvgl|nvmath|nvutils|nvshaders|nvpsystem)"
            r"[A-Za-z0-9_]*|(?<![A-Za-z0-9])vk_gltf_renderer[A-Za-z0-9_]*"
        ),
    ),
)
OUT_OF_SCOPE_RENDERER_SENTINEL = "NVVK_SEMAPHORE_DEBUG"
EXPECTED_SCENE_EDITOR_DOCUMENT_SOURCES = (
    "loader/TinyGltfImplementation.cpp",
    "scene/editor/include/mgif/scene_editor/EditorLimits.h",
    "scene/editor/include/mgif/scene_editor/EditorObjectId.h",
    "scene/editor/include/mgif/scene_editor/EditorHistoryPolicy.h",
    "scene/editor/include/mgif/scene_editor/GltfDocument.h",
    "scene/editor/include/mgif/scene_editor/StructuralDelta.h",
    "scene/editor/include/mgif/scene_editor/SceneCommands.h",
    "scene/editor/include/mgif/scene_editor/GltfDocumentIO.h",
    "scene/editor/include/mgif/scene_editor/GltfSemanticCompare.h",
    "scene/editor/include/mgif/scene_editor/GltfValidator.h",
    "scene/editor/GltfDocument.cpp",
    "scene/editor/StructuralDelta.cpp",
    "scene/editor/SceneCommands.cpp",
    "scene/editor/GltfDocumentIO.cpp",
    "scene/editor/GltfSemanticCompare.cpp",
    "scene/editor/GltfValidator.cpp",
)
EXPECTED_SCENE_EDITOR_DOCUMENT_COMPILED_SOURCES = (
    "loader/TinyGltfImplementation.cpp",
    "scene/editor/GltfDocument.cpp",
    "scene/editor/StructuralDelta.cpp",
    "scene/editor/SceneCommands.cpp",
    "scene/editor/GltfDocumentIO.cpp",
    "scene/editor/GltfSemanticCompare.cpp",
    "scene/editor/GltfValidator.cpp",
)
EXPECTED_SCENE_EDITOR_SOURCES = (
    "scene/editor/include/mgif/scene_editor/EditorClock.h",
    "scene/editor/include/mgif/scene_editor/EditorFaultInjection.h",
    "scene/editor/include/mgif/scene_editor/EditorSelection.h",
    "scene/editor/include/mgif/scene_editor/testing/EditorStateInspector.h",
    "scene/editor/include/mgif/scene_editor/commands/EditorCommand.h",
    "scene/editor/include/mgif/scene_editor/commands/EditorHistory.h",
    "scene/editor/include/mgif/scene_editor/commands/HistoryLifecycle.h",
    "scene/editor/include/mgif/scene_editor/commands/TransformCommands.h",
    "scene/editor/include/mgif/scene_editor/commands/HierarchyCommands.h",
    "scene/editor/include/mgif/scene_editor/commands/PropertyCommands.h",
    "scene/editor/include/mgif/scene_editor/commands/ContinuousEdit.h",
    "scene/editor/include/mgif/scene_editor/EditorSession.h",
    "scene/editor/include/mgif/scene_editor/SceneEditor.h",
    "scene/editor/SceneEditorCore.cpp",
    "scene/editor/EditorSelection.cpp",
    "scene/editor/commands/EditorCommand.cpp",
    "scene/editor/commands/EditorHistory.cpp",
    "scene/editor/commands/HistoryLifecycle.cpp",
    "scene/editor/commands/TransformCommands.cpp",
    "scene/editor/commands/HierarchyCommands.cpp",
    "scene/editor/commands/PropertyCommands.cpp",
    "scene/editor/commands/ContinuousEdit.cpp",
    "scene/editor/EditorSession.cpp",
    "scene/editor/SceneEditor.cpp",
)
EXPECTED_SCENE_EDITOR_COMPILED_SOURCES = (
    "scene/editor/SceneEditorCore.cpp",
    "scene/editor/EditorSelection.cpp",
    "scene/editor/commands/EditorCommand.cpp",
    "scene/editor/commands/EditorHistory.cpp",
    "scene/editor/commands/HistoryLifecycle.cpp",
    "scene/editor/commands/TransformCommands.cpp",
    "scene/editor/commands/HierarchyCommands.cpp",
    "scene/editor/commands/PropertyCommands.cpp",
    "scene/editor/commands/ContinuousEdit.cpp",
    "scene/editor/EditorSession.cpp",
    "scene/editor/SceneEditor.cpp",
)
UNAPPROVED_CONFIGURE_CHANNELS = {
    "CMAKE_C_COMPILER_ARG1",
    "CMAKE_C_COMPILER_LAUNCHER",
    "CMAKE_CXX_COMPILER_ARG1",
    "CMAKE_CXX_COMPILER_EXTERNAL_TOOLCHAIN",
    "CMAKE_CXX_COMPILER_LAUNCHER",
    "CMAKE_CXX_COMPILER_TARGET",
    "CMAKE_PROJECT_INCLUDE",
    "CMAKE_PROJECT_INCLUDE_BEFORE",
    "CMAKE_PROJECT_TOP_LEVEL_INCLUDES",
    "CMAKE_RULE_LAUNCH_COMPILE",
    "CMAKE_RULE_LAUNCH_LINK",
    "CMAKE_TOOLCHAIN_FILE",
    "CMAKE_USER_MAKE_RULES_OVERRIDE",
    "CMAKE_USER_MAKE_RULES_OVERRIDE_C",
    "CMAKE_USER_MAKE_RULES_OVERRIDE_CXX",
}
COMPILER_ENVIRONMENT_CHANNELS = ("CC", "CFLAGS", "CPPFLAGS", "CXX", "CXXFLAGS", "CL", "_CL_")
LINK_EFFECT_CACHE_PATTERN = re.compile(
    r"CMAKE_(?:(?:C|CXX)_LINK_EXECUTABLE|(?:C|CXX)_LINKER_LAUNCHER|"
    r"(?:C|CXX)_STANDARD_LIBRARIES(?:_[A-Z0-9_]+)?|"
    r"(?:EXE|MODULE|SHARED)_LINKER_FLAGS(?:_[A-Z0-9_]+)?|RULE_LAUNCH_LINK)$",
    re.IGNORECASE,
)
FORBIDDEN_FRAMEWORK_NAMES = {"metal", "foundation"}
COMPILE_CAPABLE_TARGET_TYPES = {
    "EXECUTABLE",
    "MODULE_LIBRARY",
    "OBJECT_LIBRARY",
    "SHARED_LIBRARY",
    "STATIC_LIBRARY",
}
RESPONSE_MAX_DEPTH = 16
RESPONSE_MAX_FILES = 64
RESPONSE_MAX_FILE_BYTES = 4 * 1024 * 1024
RESPONSE_MAX_BYTES = 16 * 1024 * 1024
RESPONSE_MAX_TOKENS = 262144
RESPONSE_MAX_TOKEN_BYTES = 1024 * 1024
PRODUCTION_EDITOR_ROOTS = ("scene/editor", "app/editor", "render/editor")
FIRST_PARTY_SOURCE_ROOTS = ("scene", "app", "render", "rhi", "loader")


class IdentityError(RuntimeError):
    pass


def require(condition: bool, message: str) -> None:
    if not condition:
        raise IdentityError(message)


def sha256(path: Path) -> str:
    digest = hashlib.sha256()
    with path.open("rb") as stream:
        for chunk in iter(lambda: stream.read(1024 * 1024), b""):
            digest.update(chunk)
    return digest.hexdigest()


def text_sha256(value: str) -> str:
    return hashlib.sha256(value.encode("utf-8")).hexdigest()


def load_json(path: Path) -> Any:
    try:
        return json.loads(path.read_text(encoding="utf-8"))
    except (OSError, UnicodeDecodeError, json.JSONDecodeError) as exc:
        raise IdentityError(f"cannot read configured build evidence {path}: {exc}") from exc


def _windows_command_line_tokens(value: str, label: str) -> list[str]:
    """Parse the quoting/backslash rules used by cl.exe response files."""
    tokens: list[str] = []
    index = 0
    while index < len(value):
        while index < len(value) and value[index] in " \t\r\n":
            index += 1
        if index == len(value):
            break
        token: list[str] = []
        quoted = False
        while index < len(value):
            character = value[index]
            if character in " \t\r\n" and not quoted:
                break
            if character == "\\":
                start = index
                while index < len(value) and value[index] == "\\":
                    index += 1
                slash_count = index - start
                if index < len(value) and value[index] == '"':
                    token.extend("\\" * (slash_count // 2))
                    if slash_count % 2:
                        token.append('"')
                    else:
                        quoted = not quoted
                    index += 1
                else:
                    token.extend("\\" * slash_count)
                continue
            if character == '"':
                quoted = not quoted
                index += 1
                continue
            token.append(character)
            index += 1
        require(not quoted, f"unterminated quote in {label}")
        tokens.append("".join(token))
        while index < len(value) and value[index] in " \t\r\n":
            index += 1
    return tokens


def _compiler_tokens(value: str, style: str, label: str) -> list[str]:
    if style == "msvc":
        return _windows_command_line_tokens(value, label)
    try:
        return shlex.split(value, posix=True, comments=False)
    except ValueError as exc:
        raise IdentityError(f"cannot parse {label}: {exc}") from exc


def _host_command_tokens(value: str, label: str) -> list[str]:
    if os.name == "nt":
        return _windows_command_line_tokens(value, label)
    return _compiler_tokens(value, "posix", label)


def _decode_response_file(data: bytes, path: Path, style: str) -> str:
    try:
        if data.startswith((b"\xff\xfe", b"\xfe\xff")):
            require(style == "msvc", f"UTF-16 compiler response file is unsupported by this compiler dialect: {path}")
            text = data.decode("utf-16")
        else:
            text = data.decode("utf-8-sig")
    except UnicodeError as exc:
        raise IdentityError(
            f"compiler response file has unsupported or invalid encoding (expected UTF-8 or BOM-marked UTF-16): {path}"
        ) from exc
    require("\0" not in text, f"compiler response file contains an embedded NUL: {path}")
    return text


@dataclass
class _ResponseExpansionState:
    files: int = 0
    total_bytes: int = 0
    tokens: int = 0
    active: list[Path] = field(default_factory=list)
    active_identities: list[tuple[int, int]] = field(default_factory=list)
    evidence: list[dict[str, Any]] = field(default_factory=list)


def _expand_response_tokens(
    tokens: list[str],
    *,
    directory: Path,
    style: str,
    label: str,
    state: _ResponseExpansionState | None = None,
    depth: int = 0,
) -> tuple[list[str], list[dict[str, Any]]]:
    """Expand every compiler @file using the compiler process working directory.

    Both MSVC and the GNU/Clang drivers resolve nested relative @file names from
    the process working directory. Keeping that root fixed avoids a gate/compiler
    disagreement when identically named files exist beside a parent response file.
    """
    if state is None:
        state = _ResponseExpansionState()
    require(depth <= RESPONSE_MAX_DEPTH, f"compiler response-file nesting exceeds {RESPONSE_MAX_DEPTH} in {label}")
    expanded: list[str] = []
    for token in tokens:
        if not token.startswith("@"):
            expanded.append(token)
            state.tokens += 1
            require(state.tokens <= RESPONSE_MAX_TOKENS, f"expanded compiler arguments exceed {RESPONSE_MAX_TOKENS} tokens in {label}")
            continue
        response_name = token[1:]
        require(response_name, f"empty compiler response-file reference in {label}")
        response = Path(response_name)
        require(
            not (os.name == "nt" and re.match(r"^[A-Za-z]:[^\\/]", response_name)),
            f"drive-relative compiler response-file reference is ambiguous in {label}: {response_name!r}",
        )
        if not response.is_absolute():
            response = directory / response
        try:
            response = response.resolve(strict=True)
        except (OSError, RuntimeError) as exc:
            raise IdentityError(f"missing or unreadable compiler response file {response_name!r} in {label}: {exc}") from exc
        require(response.is_file(), f"compiler response-file reference is not a file in {label}: {response}")
        try:
            before = response.stat()
        except OSError as exc:
            raise IdentityError(f"cannot stat compiler response file {response} in {label}: {exc}") from exc
        identity = (before.st_dev, before.st_ino)
        if response in state.active or identity in state.active_identities:
            cycle = " -> ".join(str(path) for path in (*state.active, response))
            raise IdentityError(f"compiler response-file cycle in {label}: {cycle}")
        state.files += 1
        require(state.files <= RESPONSE_MAX_FILES, f"compiler response-file count exceeds {RESPONSE_MAX_FILES} in {label}")
        try:
            data = response.read_bytes()
        except OSError as exc:
            raise IdentityError(f"cannot read compiler response file {response} in {label}: {exc}") from exc
        try:
            after = response.stat()
        except OSError as exc:
            raise IdentityError(f"cannot restat compiler response file {response} in {label}: {exc}") from exc
        require(
            (before.st_dev, before.st_ino, before.st_size, before.st_mtime_ns)
            == (after.st_dev, after.st_ino, after.st_size, after.st_mtime_ns)
            and len(data) == after.st_size,
            f"compiler response file changed while it was inspected: {response}",
        )
        require(len(data) <= RESPONSE_MAX_FILE_BYTES, f"compiler response file exceeds {RESPONSE_MAX_FILE_BYTES} bytes: {response}")
        state.total_bytes += len(data)
        require(
            state.total_bytes <= RESPONSE_MAX_BYTES,
            f"compiler response-file bytes exceed {RESPONSE_MAX_BYTES} in {label}",
        )
        _assert_no_forbidden_dependency(
            data.decode("latin-1", errors="strict"),
            f"raw compiler response file {response}",
        )
        response_text = _decode_response_file(data, response, style)
        response_tokens = _compiler_tokens(response_text, style, f"compiler response file {response}")
        require(
            all(len(value.encode("utf-8")) <= RESPONSE_MAX_TOKEN_BYTES for value in response_tokens),
            f"compiler response file contains a token larger than {RESPONSE_MAX_TOKEN_BYTES} bytes: {response}",
        )
        state.evidence.append({"path": response.as_posix(), "bytes": len(data), "sha256": sha256(response)})
        state.active.append(response)
        state.active_identities.append(identity)
        nested, _ = _expand_response_tokens(
            response_tokens,
            directory=directory,
            style=style,
            label=label,
            state=state,
            depth=depth + 1,
        )
        state.active.pop()
        state.active_identities.pop()
        expanded.extend(nested)
    return expanded, list(state.evidence)


def _compile_entry_tokens(entry: dict[str, Any], style: str, label: str) -> list[str]:
    arguments = entry.get("arguments")
    if isinstance(arguments, list):
        require(arguments and all(isinstance(value, str) for value in arguments), f"{label} arguments are invalid")
        return list(arguments)
    command = entry.get("command")
    require(isinstance(command, str) and command.strip(), f"{label} lacks command/arguments")
    return _host_command_tokens(command, label)


def _canonical_file(path: Path, label: str) -> Path:
    try:
        resolved = path.resolve(strict=True)
    except (OSError, RuntimeError) as exc:
        raise IdentityError(f"cannot resolve {label} {path}: {exc}") from exc
    require(resolved.is_file(), f"{label} is not a file: {resolved}")
    return resolved


def _same_file(left: Path, right: Path) -> bool:
    try:
        return left.samefile(right)
    except OSError:
        return left.resolve() == right.resolve()


def _powershell_json(script: str, *arguments: str) -> Any:
    system_root = Path(os.environ.get("SystemRoot", r"C:\Windows"))
    powershell = system_root / "System32/WindowsPowerShell/v1.0/powershell.exe"
    require(powershell.is_file(), "trusted Windows PowerShell executable is unavailable")
    environment = os.environ.copy()
    environment["MGIF_COMPILER_TRUST_QUERY_ARGS"] = json.dumps(arguments)
    completed = subprocess.run(
        [str(powershell), "-NoLogo", "-NoProfile", "-NonInteractive", "-Command", script],
        check=False,
        capture_output=True,
        text=True,
        encoding="utf-8-sig",
        errors="replace",
        env=environment,
    )
    require(completed.returncode == 0, f"Windows compiler trust query failed: {completed.stderr.strip()}")
    try:
        return json.loads(completed.stdout)
    except json.JSONDecodeError as exc:
        raise IdentityError("Windows compiler trust query returned invalid JSON") from exc


def _windows_authenticode(path: Path) -> dict[str, str]:
    record = _powershell_json(
        "Import-Module \"$env:SystemRoot\\System32\\WindowsPowerShell\\v1.0\\Modules\\"
        "Microsoft.PowerShell.Security\\Microsoft.PowerShell.Security.psd1\" -ErrorAction Stop; "
        "$a=@(ConvertFrom-Json $env:MGIF_COMPILER_TRUST_QUERY_ARGS); "
        "$s=Get-AuthenticodeSignature -LiteralPath ([string]$a[0]); "
        "[pscustomobject]@{Status=[string]$s.Status;Subject=[string]$s.SignerCertificate.Subject;"
        "Thumbprint=[string]$s.SignerCertificate.Thumbprint}|ConvertTo-Json -Compress",
        str(path),
    )
    require(isinstance(record, dict), f"Authenticode evidence changed for compiler {path}")
    require(record.get("Status") == "Valid", f"configured compiler lacks a valid Authenticode signature: {path}")
    subject = str(record.get("Subject", ""))
    require("Microsoft" in subject, f"configured Windows compiler signer is not trusted: {subject!r}")
    return {"subject": subject, "thumbprint": str(record.get("Thumbprint", ""))}


def _visual_studio_installations() -> list[Path]:
    roots = [
        Path(os.environ.get("ProgramFiles(x86)", r"C:\Program Files (x86)"))
        / "Microsoft Visual Studio/Installer/vswhere.exe",
        Path(os.environ.get("ProgramFiles", r"C:\Program Files"))
        / "Microsoft Visual Studio/Installer/vswhere.exe",
    ]
    vswhere = next((path for path in roots if path.is_file()), None)
    require(vswhere is not None, "vswhere is required to authenticate the configured Windows compiler")
    completed = subprocess.run(
        [str(vswhere), "-products", "*", "-format", "json", "-utf8"],
        check=False,
        capture_output=True,
        text=True,
        encoding="utf-8-sig",
        errors="replace",
    )
    require(completed.returncode == 0, f"vswhere compiler trust query failed: {completed.stderr.strip()}")
    try:
        records = json.loads(completed.stdout)
    except json.JSONDecodeError as exc:
        raise IdentityError("vswhere compiler trust query returned invalid JSON") from exc
    require(isinstance(records, list), "vswhere compiler trust evidence changed")
    installations: list[Path] = []
    for record in records:
        if not isinstance(record, dict) or record.get("isComplete") is not True:
            continue
        value = record.get("installationPath")
        if isinstance(value, str) and value:
            installations.append(Path(value).resolve())
    require(installations, "vswhere found no complete Visual Studio installation")
    return installations


def _trusted_compiler_identity(compiler_path: Path, compiler_id: str, compiler_version: str) -> dict[str, Any]:
    compiler = _canonical_file(compiler_path, "configured CXX compiler")
    name = compiler.name.lower()
    if compiler_id == "MSVC":
        require(name == "cl.exe", f"configured MSVC executable is not cl.exe: {compiler}")
    elif compiler_id in {"Clang", "AppleClang"}:
        require(name in {"clang", "clang++", "clang.exe", "clang++.exe", "clang-cl.exe"}, f"configured Clang executable identity is invalid: {compiler}")
    elif compiler_id == "GNU":
        require(re.search(r"(?:^|-)g\+\+(?:-[0-9.]+)?(?:\.exe)?$", name) is not None or name in {"c++", "c++.exe"}, f"configured GNU executable identity is invalid: {compiler}")
    else:
        raise IdentityError(f"unapproved configured CXX compiler id: {compiler_id!r}")

    trust: dict[str, Any]
    if os.name == "nt":
        require(
            compiler_id in {"MSVC", "Clang"},
            f"configured {compiler_id} compiler has no approved Windows platform trust policy",
        )
        installations = _visual_studio_installations()
        candidates: list[tuple[Path, Path]] = []
        for installation in installations:
            patterns = (
                "VC/Tools/MSVC/*/bin/Host*/*/cl.exe",
                "VC/Tools/Llvm/**/bin/clang.exe",
                "VC/Tools/Llvm/**/bin/clang++.exe",
                "VC/Tools/Llvm/**/bin/clang-cl.exe",
            )
            for pattern in patterns:
                for candidate in installation.glob(pattern):
                    if candidate.is_file():
                        candidates.append((installation, candidate.resolve()))
        match = next(((root, candidate) for root, candidate in candidates if _same_file(compiler, candidate)), None)
        require(match is not None, f"configured Windows compiler is not an executable registered by vswhere: {compiler}")
        installation, _ = match
        if compiler_id == "MSVC":
            try:
                relative = compiler.relative_to(installation / "VC/Tools/MSVC")
            except ValueError as exc:
                raise IdentityError(f"configured MSVC compiler is outside the registered VC Tools layout: {compiler}") from exc
            require(
                len(relative.parts) == 5
                and re.fullmatch(r"[0-9]+(?:\.[0-9]+)+", relative.parts[0]) is not None
                and relative.parts[1].lower() == "bin"
                and relative.parts[2].lower().startswith("host")
                and relative.parts[-1].lower() == "cl.exe",
                f"configured MSVC compiler layout is not canonical: {compiler}",
            )
        signature = _windows_authenticode(compiler)
        trust = {
            "provider": "vswhere+authenticode",
            "installationRoot": installation.as_posix(),
            "signerSubject": signature["subject"],
            "signerThumbprint": signature["thumbprint"],
        }
    elif os.sys.platform == "darwin" and compiler_id == "AppleClang":
        xcrun = subprocess.run(
            ["xcrun", "--find", "clang++"], check=False, capture_output=True, text=True, encoding="utf-8"
        )
        require(xcrun.returncode == 0 and xcrun.stdout.strip(), "xcrun cannot authenticate AppleClang")
        require(_same_file(compiler, _canonical_file(Path(xcrun.stdout.strip()), "xcrun AppleClang")), "configured AppleClang differs from xcrun")
        codesign = subprocess.run(["codesign", "--verify", "--strict", str(compiler)], check=False, capture_output=True, text=True)
        require(codesign.returncode == 0, f"configured AppleClang code signature is invalid: {codesign.stderr.strip()}")
        trust = {"provider": "xcrun+codesign"}
    else:
        metadata = compiler.stat()
        require(metadata.st_uid == 0, f"configured POSIX compiler is not owned by root: {compiler}")
        require(not (metadata.st_mode & (stat.S_IWGRP | stat.S_IWOTH)), f"configured POSIX compiler is group/world writable: {compiler}")
        owner: str | None = None
        queries = (
            (["dpkg-query", "-S", str(compiler)], "dpkg"),
            (["rpm", "-qf", str(compiler)], "rpm"),
            (["pacman", "-Qo", str(compiler)], "pacman"),
        )
        for command, provider in queries:
            if shutil.which(command[0]) is None:
                continue
            completed = subprocess.run(command, check=False, capture_output=True, text=True, encoding="utf-8", errors="replace")
            if completed.returncode == 0 and completed.stdout.strip():
                owner = f"{provider}:{completed.stdout.strip().splitlines()[0]}"
                break
        require(owner is not None, f"configured POSIX compiler is not owned by a supported system package database: {compiler}")
        trust = {"provider": "root-owned-system-package", "package": owner}

    return {
        "family": compiler_id,
        "version": compiler_version,
        "canonicalPath": compiler.as_posix(),
        "sha256": sha256(compiler),
        "bytes": compiler.stat().st_size,
        "trust": trust,
    }


def _require_exact_compiler(tokens: list[str], compiler_identity: dict[str, Any], label: str) -> None:
    require(tokens, f"{label} is empty")
    executable = Path(tokens[0])
    require(executable.is_absolute(), f"{label} uses a PATH-resolved compiler or launcher: {tokens[0]}")
    executable = _canonical_file(executable, f"{label} executable")
    compiler = _canonical_file(Path(str(compiler_identity["canonicalPath"])), "trusted configured compiler")
    require(_same_file(executable, compiler), f"{label} compiler/launcher differs from the configured executable: {tokens[0]}")
    require(sha256(executable) == compiler_identity["sha256"], f"{label} compiler binary changed after identity binding")


def run_git(source_root: Path, *arguments: str) -> str:
    completed = subprocess.run(
        ["git", "-C", str(source_root), *arguments],
        check=False,
        capture_output=True,
        text=True,
        encoding="utf-8",
        errors="replace",
    )
    require(
        completed.returncode == 0,
        f"git {' '.join(arguments)} failed for resolved Dear ImGui checkout: {completed.stderr.strip()}",
    )
    return completed.stdout.strip()


def normalize_repository(value: str) -> str:
    return value.strip().replace("\\", "/").rstrip("/").lower()


def relative_target_inputs(source_root: Path, target_sources: list[Path]) -> list[str]:
    relative_paths: list[str] = []
    for source in target_sources:
        require("$<" not in str(source), "Dear ImGui target sources contain an unresolved generator expression")
        resolved = source.resolve()
        try:
            relative = resolved.relative_to(source_root)
        except ValueError as exc:
            raise IdentityError(f"Dear ImGui target input escapes its resolved checkout: {resolved}") from exc
        require(resolved.is_file(), f"Dear ImGui target input is missing: {resolved}")
        relative_text = relative.as_posix()
        require(
            relative_text == str(PurePosixPath(relative_text)) and ".." not in relative.parts,
            f"Dear ImGui target input is not canonical: {relative_text}",
        )
        relative_paths.append(relative_text)
    require(len(relative_paths) == len(set(relative_paths)), "Dear ImGui target sources contain duplicate inputs")
    return sorted(relative_paths)


def _file_api_objects(build_root: Path) -> tuple[Path, dict[str, Path], dict[str, Any]]:
    reply_root = build_root / ".cmake/api/v1/reply"
    indexes = sorted(reply_root.glob("index-*.json"))
    require(indexes, f"configured build lacks a CMake File API reply index under {reply_root}")
    index_path = indexes[-1]
    index = load_json(index_path)
    require(isinstance(index, dict), "CMake File API index is not an object")
    objects: dict[str, Path] = {}
    for record in index.get("objects", []):
        if not isinstance(record, dict):
            continue
        kind = record.get("kind")
        json_file = record.get("jsonFile")
        if isinstance(kind, str) and isinstance(json_file, str):
            candidate = _file_api_reply_path(
                reply_root,
                json_file,
                f"CMake File API {kind} reply",
            )
            objects[kind] = candidate
    missing = sorted(REQUIRED_FILE_API_KINDS - set(objects))
    require(not missing, f"configured build lacks required CMake File API objects: {', '.join(missing)}")
    return index_path, objects, index


def _file_api_reply_path(reply_root: Path, json_file: str, label: str) -> Path:
    require(bool(json_file) and "\x00" not in json_file, f"{label} jsonFile is invalid")
    portable = json_file.replace("\\", "/")
    relative = Path(json_file)
    portable_path = PurePosixPath(portable)
    require(
        not relative.is_absolute()
        and not portable_path.is_absolute()
        and not re.match(r"^[A-Za-z]:", portable),
        f"{label} jsonFile must be relative to the reply directory",
    )
    require(
        ".." not in portable_path.parts,
        f"{label} jsonFile contains traversal outside its declared reply identity",
    )
    resolved_reply_root = reply_root.resolve()
    candidate = (resolved_reply_root / relative).resolve()
    try:
        candidate.relative_to(resolved_reply_root)
    except ValueError as exc:
        raise IdentityError(f"{label} escapes the reply directory") from exc
    require(candidate.is_file(), f"{label} is missing: {candidate}")
    return candidate


def _file_api_target_references(configuration: dict[str, Any]) -> list[dict[str, str]]:
    raw_references = configuration.get("targets")
    require(isinstance(raw_references, list), "CMake File API configuration lacks targets")
    references: list[dict[str, str]] = []
    names: set[str] = set()
    target_ids: set[str] = set()
    for raw_reference in raw_references:
        require(isinstance(raw_reference, dict), "CMake File API target reference is invalid")
        name = raw_reference.get("name")
        target_id = raw_reference.get("id")
        json_file = raw_reference.get("jsonFile")
        require(
            isinstance(name, str)
            and bool(name)
            and isinstance(target_id, str)
            and bool(target_id)
            and isinstance(json_file, str)
            and bool(json_file),
            "CMake File API target reference is incomplete",
        )
        require(name not in names, f"CMake File API target name is ambiguous: {name}")
        require(target_id not in target_ids, f"CMake File API target ID is ambiguous: {target_id}")
        names.add(name)
        target_ids.add(target_id)
        references.append({"name": name, "id": target_id, "jsonFile": json_file})
    return references


def _configured_target(
    codemodel_path: Path,
    target_name: str,
    requested_config: str,
) -> tuple[str, Path, dict[str, Any], dict[str, Any]]:
    codemodel = load_json(codemodel_path)
    require(isinstance(codemodel, dict), "CMake File API codemodel is not an object")
    configurations = codemodel.get("configurations")
    require(isinstance(configurations, list) and configurations, "CMake File API codemodel has no configurations")
    matching = [
        config
        for config in configurations
        if isinstance(config, dict) and (not requested_config or config.get("name") == requested_config)
    ]
    require(
        len(matching) == 1,
        f"configured build does not have exactly one requested configuration {requested_config!r}",
    )
    configuration = matching[0]
    require(isinstance(configuration, dict), "CMake File API configuration is not an object")
    all_references = _file_api_target_references(configuration)
    references = [
        target
        for target in all_references
        if target["name"] == target_name
    ]
    require(len(references) == 1, f"CMake File API does not contain exactly one {target_name} target")
    reference = references[0]
    target_path = _file_api_reply_path(
        codemodel_path.resolve().parent,
        reference["jsonFile"],
        f"CMake File API {target_name} target reply",
    )
    target = load_json(target_path)
    require(
        isinstance(target, dict)
        and target.get("name") == reference["name"]
        and target.get("id") == reference["id"],
        f"CMake File API {target_name} target reply identity changed",
    )
    return str(configuration.get("name", "")), target_path, target, codemodel


def _resolve_file_api_source_path(
    repository_root: Path,
    build_root: Path,
    record: dict[str, Any],
    label: str,
) -> Path:
    """Resolve a codemodel sources[].path against its top-level root."""
    require(isinstance(record, dict), f"{label} record changed")
    value = record.get("path")
    require(isinstance(value, str) and value, f"{label} path changed")
    source = Path(value)
    if source.is_absolute():
        return source.resolve()

    root = build_root if record.get("isGenerated") is True else repository_root
    root = root.resolve()
    source = (root / source).resolve()
    try:
        source.relative_to(root)
    except ValueError as exc:
        kind = "build" if record.get("isGenerated") is True else "source"
        raise IdentityError(f"{label} relative path escapes the codemodel top-level {kind} root: {value}") from exc
    return source


def _resolved_target_sources(
    repository_root: Path,
    build_root: Path,
    target: dict[str, Any],
    *,
    require_all_sources: bool = True,
) -> tuple[list[Path], list[Path]]:
    sources = target.get("sources")
    target_name = str(target.get("name", "<unknown>"))
    require(isinstance(sources, list) and sources, f"configured target {target_name} has no sources")
    all_sources: list[Path] = []
    compiled_sources: list[Path] = []
    for record in sources:
        resolved = _resolve_file_api_source_path(
            repository_root,
            build_root,
            record,
            f"configured target {target_name} source",
        )
        if require_all_sources or "compileGroupIndex" in record:
            require(resolved.is_file(), f"configured target {target_name} source is missing before preflight: {resolved}")
        all_sources.append(resolved)
        if "compileGroupIndex" in record:
            compiled_sources.append(resolved)
    require(len(all_sources) == len(set(all_sources)), f"configured target {target_name} contains duplicate sources")
    require(
        len(compiled_sources) == len(set(compiled_sources)),
        f"configured target {target_name} contains duplicate compiled sources",
    )
    return all_sources, compiled_sources


def _compile_commands_for_target(
    build_root: Path,
    target_name: str,
) -> tuple[Path, list[dict[str, Any]]]:
    compile_commands_path = build_root / "compile_commands.json"
    require(compile_commands_path.is_file(), "configured build lacks compile_commands.json")
    database = load_json(compile_commands_path)
    require(isinstance(database, list) and database, "compile_commands.json is not a non-empty array")
    marker = f"/cmakefiles/{target_name.lower()}.dir/"
    entries: list[dict[str, Any]] = []
    for entry in database:
        if not isinstance(entry, dict):
            continue
        output = entry.get("output")
        if isinstance(output, str) and marker in output.replace("\\", "/").lower():
            entries.append(entry)
    require(entries, f"compile_commands.json has no entries owned by {target_name}")
    return compile_commands_path, entries


def _compile_entry_source(entry: dict[str, Any]) -> Path:
    source = entry.get("file")
    directory = entry.get("directory")
    require(isinstance(source, str) and isinstance(directory, str), "compile command lacks file/directory")
    path = Path(source)
    if not path.is_absolute():
        path = Path(directory) / path
    return path.resolve()


def _compile_entry_text(entry: dict[str, Any]) -> str:
    command = entry.get("command")
    if isinstance(command, str) and command.strip():
        return command
    arguments = entry.get("arguments")
    require(
        isinstance(arguments, list) and arguments and all(isinstance(value, str) for value in arguments),
        "compile command lacks command/arguments",
    )
    return "\0".join(arguments)


def _assert_no_forbidden_dependency(
    text: str,
    label: str,
    *,
    allow_renderer_sentinel: bool = False,
) -> None:
    for family, pattern in FORBIDDEN_DEPENDENCY_PATTERNS:
        for match in pattern.finditer(text):
            if allow_renderer_sentinel and match.group(0).upper() == OUT_OF_SCOPE_RENDERER_SENTINEL:
                continue
            line = text.count("\n", 0, match.start()) + 1
            raise IdentityError(
                f"forbidden editor dependency {match.group(0)!r} ({family}) in {label}:{line}"
            )


def _cache_entries(cache: Any) -> dict[str, Any]:
    require(isinstance(cache, dict) and isinstance(cache.get("entries"), list), "File API cache reply changed")
    result: dict[str, Any] = {}
    for entry in cache["entries"]:
        require(isinstance(entry, dict), "File API cache contains a non-object entry")
        name = entry.get("name")
        require(isinstance(name, str) and name, "File API cache entry lacks a name")
        require(name not in result, f"File API cache contains duplicate entry {name}")
        result[name] = entry.get("value")
    return result


def _is_unapproved_configure_channel(name: str) -> bool:
    return name in UNAPPROVED_CONFIGURE_CHANNELS or bool(
        re.fullmatch(r"CMAKE_PROJECT_[A-Za-z0-9_]+_INCLUDE(?:_BEFORE)?", name)
    )


def _normalized_linker_arguments(values: list[str]) -> list[str]:
    pending = list(reversed(values))
    normalized: list[str] = []
    while pending:
        value = pending.pop().strip().strip('"')
        if not value:
            continue
        upper = value.upper()
        parts: list[str] | None = None
        if upper.startswith("LINKER:SHELL:"):
            try:
                parts = shlex.split(value[len("LINKER:SHELL:") :], posix=True)
            except ValueError:
                parts = [value[len("LINKER:SHELL:") :]]
        elif upper.startswith("SHELL:"):
            try:
                parts = shlex.split(value[len("SHELL:") :], posix=True)
            except ValueError:
                parts = [value[len("SHELL:") :]]
        elif upper.startswith("LINKER:"):
            parts = value[len("LINKER:") :].split(",")
        elif value.lower().startswith("-wl,"):
            parts = value[4:].split(",")
        elif ";" in value:
            parts = value.split(";")
        elif any(character.isspace() for character in value):
            try:
                parts = shlex.split(value, posix=True)
            except ValueError:
                parts = [value]
        if parts is None or parts == [value]:
            normalized.append(value)
            continue
        pending.extend(reversed(parts))
    return normalized


def _framework_link_operands(values: list[str]) -> list[str]:
    arguments = _normalized_linker_arguments(values)
    frameworks: list[str] = []
    index = 0
    while index < len(arguments):
        value = arguments[index]
        inline = re.fullmatch(r"-(?:weak_)?framework(?:=|,)(.+)", value, re.IGNORECASE)
        if inline:
            frameworks.append(inline.group(1))
        elif value.lower() in {"-framework", "-weak_framework"}:
            next_index = index + 1
            while next_index < len(arguments) and arguments[next_index].lower() == "-xlinker":
                next_index += 1
            if next_index < len(arguments):
                frameworks.append(arguments[next_index])
                index = next_index
        else:
            path_match = re.search(r"(?:^|[/\\])([^/\\]+)\.framework(?:[/\\]|$)", value, re.IGNORECASE)
            if path_match:
                frameworks.append(path_match.group(1))
        index += 1
    return frameworks


def _scan_forced_header(header: Path, include_directories: list[Path], label: str) -> None:
    pending = [header.resolve()]
    visited: set[Path] = set()
    include_pattern = re.compile(r'^\s*#\s*include\s*[<"]([^>"]+)[>"]', re.MULTILINE)
    while pending:
        path = pending.pop()
        if path in visited:
            continue
        require(len(visited) < 2048, f"configured forced-include closure is unbounded for {label}")
        visited.add(path)
        try:
            text = path.read_bytes().decode("utf-8", errors="replace")
        except OSError as exc:
            raise IdentityError(f"cannot read configured forced include {path}: {exc}") from exc
        _assert_no_forbidden_dependency(text, f"{label} forced include {path}")
        for match in include_pattern.finditer(text):
            include_name = match.group(1).replace("\\", "/")
            candidates = [path.parent / include_name]
            candidates.extend(directory / include_name for directory in include_directories)
            resolved = next((candidate.resolve() for candidate in candidates if candidate.is_file()), None)
            if resolved is not None and resolved not in visited:
                pending.append(resolved)


def _semantic_compiler_tokens(tokens: list[str]) -> list[str]:
    semantic: list[str] = []
    index = 0
    while index < len(tokens):
        token = tokens[index]
        if token == "-Xclang":
            require(index + 1 < len(tokens), "dangling -Xclang in configured compiler arguments")
            semantic.append(tokens[index + 1])
            index += 2
            continue
        if token.lower().startswith("/clang:"):
            semantic.append(token[len("/clang:") :])
            index += 1
            continue
        if token.startswith("-Wp,"):
            try:
                semantic.extend(next(iter(__import__("csv").reader([token[4:]], escapechar="\\"))))
            except (StopIteration, ValueError) as exc:
                raise IdentityError(f"cannot parse compiler preprocessor option {token!r}") from exc
            index += 1
            continue
        semantic.append(token)
        index += 1
    return semantic


def _option_values(tokens: list[str], options: tuple[str, ...], label: str) -> list[str]:
    values: list[str] = []
    ordered = sorted(options, key=len, reverse=True)
    index = 0
    while index < len(tokens):
        token = tokens[index]
        lowered = token.lower()
        matched = False
        for option in ordered:
            option_lower = option.lower()
            if lowered == option_lower:
                require(index + 1 < len(tokens), f"configured compiler option {token!r} lacks a value in {label}")
                values.append(tokens[index + 1])
                index += 2
                matched = True
                break
            if lowered.startswith(option_lower + "="):
                values.append(token[len(option) + 1 :])
                index += 1
                matched = True
                break
            if lowered.startswith(option_lower) and len(token) > len(option):
                values.append(token[len(option) :])
                index += 1
                matched = True
                break
        if not matched:
            index += 1
    return values


def _scan_compile_tokens(tokens: list[str], directory: Path, label: str) -> None:
    _assert_no_forbidden_dependency("\n".join(tokens), label)
    semantic = _semantic_compiler_tokens(tokens)
    include_directories: list[Path] = []
    include_options = (
        "/external:I",
        "-external:I",
        "/imsvc",
        "-imsvc",
        "-isystem",
        "-iquote",
        "-idirafter",
        "/I",
        "-I",
    )
    for value in _option_values(semantic, include_options, label):
        require(value and not value.startswith("@"), f"invalid configured include-search value {value!r} in {label}")
        candidate = Path(value)
        if not candidate.is_absolute():
            candidate = directory / candidate
        candidate = candidate.resolve()
        _assert_no_forbidden_dependency(str(candidate), f"{label} include-search path")
        include_directories.append(candidate)
    for value in _option_values(semantic, ("/FI", "-include", "-imacros"), label):
        require(value and not value.startswith("@"), f"invalid configured forced-include value {value!r} in {label}")
        candidate = Path(value)
        candidates = [candidate] if candidate.is_absolute() else [directory / candidate]
        if not candidate.is_absolute():
            candidates.extend(include_directory / candidate for include_directory in include_directories)
        forced = next((path.resolve() for path in candidates if path.is_file()), None)
        require(forced is not None, f"cannot resolve {label} forced include {value!r}")
        _scan_forced_header(forced, include_directories, label)

    # These options can redirect the compiler to opaque configuration, plugin,
    # precompiled-header, module, or toolchain inputs that are not represented
    # by ordinary include paths. Production editor compilations do not need
    # them, so accepting one would create an unscanned dependency channel.
    opaque_options = (
        "--config",
        "--gcc-toolchain",
        "--sysroot",
        "-B",
        "-fmodule-file",
        "-fmodule-map-file",
        "-fplugin",
        "-ivfsoverlay",
        "-plugin",
        "-resource-dir",
        "-specs",
        "/analyze:plugin",
    )
    lowered_options = tuple(value.lower() for value in opaque_options)
    for token in semantic:
        if token == "/Fp" or token.startswith("/Fp") or token == "/Yu" or token.startswith("/Yu"):
            raise IdentityError(f"opaque compiler-controlled input option {token!r} is forbidden in {label}")
        lowered = token.lower()
        if any(
            lowered == option
            or lowered.startswith(option + "=")
            or (option == "-b" and lowered.startswith(option) and len(lowered) > len(option))
            for option in lowered_options
        ):
            raise IdentityError(f"opaque compiler-controlled input option {token!r} is forbidden in {label}")


def _scan_compile_command(
    entry: dict[str, Any],
    target_name: str,
    compiler_identity: dict[str, Any],
    style: str,
    *,
    reject_disk_responses: bool = False,
) -> tuple[list[str], list[dict[str, Any]]]:
    directory_value = entry.get("directory")
    require(isinstance(directory_value, str), f"configured {target_name} compile command lacks a directory")
    directory = Path(directory_value).resolve()
    label = f"configured {target_name} compile command"
    raw_tokens = _compile_entry_tokens(entry, style, label)
    _require_exact_compiler(raw_tokens, compiler_identity, label)
    expanded, response_evidence = _expand_response_tokens(
        raw_tokens, directory=directory, style=style, label=label
    )
    _require_exact_compiler(expanded, compiler_identity, label)
    _scan_compile_tokens(expanded[1:], directory, label)
    if reject_disk_responses and response_evidence:
        raise IdentityError(
            f"mutable external compiler response file is forbidden in {label}: "
            + ", ".join(record["path"] for record in response_evidence)
        )
    return expanded, response_evidence


def _scan_cache_compiler_flags(
    cache_entries: dict[str, Any],
    build_root: Path,
    style: str,
) -> list[dict[str, Any]]:
    evidence: list[dict[str, Any]] = []
    for name in sorted(cache_entries):
        if re.fullmatch(r"CMAKE_(?:C|CXX)_FLAGS(?:_[A-Za-z0-9_]+)?", name) is None:
            continue
        value = str(cache_entries.get(name, "") or "")
        if not value.strip():
            continue
        label = f"configured cache compiler flags {name}"
        raw_tokens = _host_command_tokens(value, label)
        expanded, records = _expand_response_tokens(
            raw_tokens,
            directory=build_root,
            style=style,
            label=label,
        )
        _scan_compile_tokens(expanded, build_root, label)
        evidence.extend(records)
        if records:
            raise IdentityError(
                f"mutable external compiler response file is forbidden in {label}: "
                + ", ".join(record["path"] for record in records)
            )
    return evidence


def _run_ninja_tool(build_root: Path, make_program: str, *arguments: str) -> str:
    completed = subprocess.run(
        [make_program, "-C", str(build_root), "-t", *arguments],
        check=False,
        capture_output=True,
        text=True,
        encoding="utf-8",
        errors="replace",
    )
    require(
        completed.returncode == 0,
        f"Ninja {' '.join(arguments)} inspection failed: {completed.stderr.strip()}",
    )
    return completed.stdout


def _cache_list(value: Any, label: str) -> list[str]:
    require(isinstance(value, str) and value.strip(), f"configured cache lacks {label}")
    values = [item for item in value.split(";") if item]
    require(values == sorted(set(values), key=str.lower), f"configured cache {label} is not a sorted unique target list")
    return values


def _contains_editor_marker(value: str) -> bool:
    normalized = value.replace("\\", "/").lower()
    if any(f"/{root}/" in f"/{normalized.strip('/')}/" for root in PRODUCTION_EDITOR_ROOTS):
        return True
    parts = PurePosixPath(normalized).parts
    return any(
        part in FIRST_PARTY_SOURCE_ROOTS and any("editor" in descendant for descendant in parts[index + 1 :])
        for index, part in enumerate(parts)
    )


def _is_nonproduction_target(name: str) -> bool:
    words = [
        match.group(0).lower()
        for component in re.split(r"[^A-Za-z0-9]+", name)
        for match in re.finditer(r"[A-Z]+(?=[A-Z][a-z]|\d|$)|[A-Z]?[a-z]+|\d+", component)
    ]
    return bool(set(words) & {"test", "tests", "testing", "probe", "reference", "fixture", "scaffold", "acceptance"})


def _target_dependencies(target: dict[str, Any]) -> set[str]:
    return {
        str(dependency.get("id", "")).split("::@", 1)[0]
        for dependency in target.get("dependencies", [])
        if isinstance(dependency, dict) and dependency.get("id")
    }


def _target_compiled_sources_by_language(
    repository_root: Path,
    build_root: Path,
    target: dict[str, Any],
) -> dict[Path, str]:
    groups = target.get("compileGroups", [])
    require(isinstance(groups, list), f"configured target {target.get('name')} compile groups changed")
    group_languages: dict[int, str] = {}
    for index, group in enumerate(groups):
        require(isinstance(group, dict) and isinstance(group.get("language"), str), f"configured target {target.get('name')} compile group changed")
        group_languages[index] = group["language"]
    result: dict[Path, str] = {}
    for record in target.get("sources", []):
        require(isinstance(record, dict), f"configured target {target.get('name')} source record changed")
        if "compileGroupIndex" not in record:
            continue
        group_index = record["compileGroupIndex"]
        require(type(group_index) is int and group_index in group_languages, f"configured target {target.get('name')} source has invalid compile group")
        source = _resolve_file_api_source_path(
            repository_root,
            build_root,
            record,
            f"configured target {target.get('name')} compiled source",
        )
        require(source.is_file(), f"configured target {target.get('name')} compiled source is missing before preflight: {source}")
        require(source not in result, f"configured target {target.get('name')} repeats a compiled source: {source}")
        result[source] = group_languages[group_index]
    return result


def _configured_editor_anchors(
    repository_root: Path,
    build_root: Path,
    target_models: dict[str, dict[str, Any]],
) -> set[str]:
    anchors: set[str] = set()
    for name, target in target_models.items():
        if target.get("type") not in COMPILE_CAPABLE_TARGET_TYPES or _is_nonproduction_target(name):
            continue
        source_records = target.get("sources", [])
        require(isinstance(source_records, list), f"configured target {name} sources changed")
        marked_source = False
        for record in source_records:
            source = _resolve_file_api_source_path(
                repository_root,
                build_root,
                record,
                f"configured target {name} source",
            )
            try:
                relative = source.relative_to(repository_root).as_posix()
            except ValueError:
                relative = source.as_posix()
            if _contains_editor_marker(relative):
                marked_source = True
                break
        if "editor" in name.lower() or marked_source:
            anchors.add(name)
    return anchors


def _configured_repository_closure(
    anchors: set[str],
    target_models: dict[str, dict[str, Any]],
    repository_root: Path,
) -> set[str]:
    """Build the same down-adapter/up-consumer closure as the static model.

    External FetchContent targets are kept under their separate canonical
    identity checks and are intentionally not claimed as repository adapters.
    """
    repository_targets: set[str] = set()
    for name, target in target_models.items():
        paths = target.get("paths", {})
        if not isinstance(paths, dict) or not isinstance(paths.get("source"), str):
            continue
        source_directory = Path(paths["source"])
        if not source_directory.is_absolute():
            source_directory = repository_root / source_directory
        try:
            source_directory.resolve().relative_to(repository_root)
        except ValueError:
            continue
        else:
            repository_targets.add(name)
    repository_build_targets = {
        name
        for name in repository_targets
        if target_models[name].get("type") != "UTILITY"
    }
    production = {
        name
        for name in repository_build_targets
        if target_models[name].get("type") != "UTILITY" and not _is_nonproduction_target(name)
    }
    closure = set(anchors)
    changed = True
    while changed:
        before = set(closure)
        for name in tuple(closure):
            closure.update(_target_dependencies(target_models[name]) & repository_build_targets)
        for name in production - closure:
            if _target_dependencies(target_models[name]) & closure:
                closure.add(name)
                closure.update(_target_dependencies(target_models[name]) & repository_build_targets)
        changed = closure != before
    return closure


def _entry_output_relative(entry: dict[str, Any], build_root: Path, label: str) -> str:
    output = entry.get("output")
    require(isinstance(output, str) and output, f"{label} lacks output")
    path = Path(output)
    if path.is_absolute():
        try:
            path = path.resolve().relative_to(build_root)
        except ValueError as exc:
            raise IdentityError(f"{label} output escapes configured build: {output}") from exc
    return path.as_posix()


def _ninja_compile_entries(
    build_root: Path,
    make_program: str,
    fenced_targets: list[str],
    *,
    expand_response_content: bool,
) -> list[dict[str, Any]]:
    try:
        arguments = ["compdb-targets"]
        if expand_response_content:
            arguments.append("-x")
        arguments.extend(fenced_targets)
        output = _run_ninja_tool(build_root, make_program, *arguments)
        database = json.loads(output)
    except (IdentityError, json.JSONDecodeError) as exc:
        raise IdentityError("Ninja compdb-targets evidence is unavailable or invalid") from exc
    require(isinstance(database, list), "Ninja compdb-targets evidence is not an array")
    markers = tuple(f"/cmakefiles/{name.lower()}.dir/" for name in fenced_targets)
    entries: list[dict[str, Any]] = []
    for entry in database:
        require(isinstance(entry, dict), "Ninja compdb-targets contains a non-object entry")
        output_value = entry.get("output")
        if not isinstance(output_value, str):
            continue
        normalized = "/" + output_value.replace("\\", "/").lower().lstrip("/")
        if any(marker in normalized for marker in markers):
            entries.append(entry)
    return entries


def _compile_edge_key(entry: dict[str, Any], build_root: Path, label: str) -> tuple[Path, str]:
    return (
        _compile_entry_source(entry),
        _entry_output_relative(entry, build_root, label).replace("\\", "/").lower(),
    )


def _normalize_compiler_path_token(token: str) -> str:
    """Make equivalent Windows path spellings comparable across CMake/Ninja."""
    return token.replace("\\", "/") if os.name == "nt" else token


def _without_dependency_bookkeeping(tokens: list[str]) -> list[str]:
    result: list[str] = []
    index = 0
    path_options = {"-c", "-include", "-idirafter", "-iquote", "-isystem", "-l", "-L", "-o"}
    while index < len(tokens):
        lowered = tokens[index].lower()
        if lowered in {"/showincludes", "-md", "-mmd", "-mp"}:
            index += 1
            continue
        if lowered in {"-mf", "-mt", "-mq", "-dependency-file"}:
            require(index + 1 < len(tokens), f"dangling compiler dependency option {tokens[index]!r}")
            index += 2
            continue
        if any(lowered.startswith(prefix) and len(lowered) > len(prefix) for prefix in ("-mf", "-mt", "-mq")):
            index += 1
            continue
        if lowered in path_options:
            result.append(tokens[index])
            if index + 1 < len(tokens):
                result.append(_normalize_compiler_path_token(tokens[index + 1]))
                index += 2
                continue
        if any(
            lowered.startswith(prefix) and len(lowered) > len(prefix)
            for prefix in ("-i", "-l")
        ):
            result.append(_normalize_compiler_path_token(tokens[index]))
            index += 1
            continue
        result.append(tokens[index])
        index += 1
    return result


def _verify_ninja_preflight_ancestry(
    build_root: Path,
    make_program: str,
    compile_entries: list[dict[str, Any]],
) -> dict[str, list[str]]:
    outputs = [_entry_output_relative(entry, build_root, "compile command") for entry in compile_entries]
    require(len(outputs) == len(set(value.lower() for value in outputs)), "configured compile outputs are not unique")
    command = [make_program, "-C", str(build_root), "-t", "multi-inputs", *outputs]
    completed = subprocess.run(
        command,
        check=False,
        capture_output=True,
        text=True,
        encoding="utf-8",
        errors="replace",
    )
    require(completed.returncode == 0, f"Ninja multi-inputs inspection failed: {completed.stderr.strip()}")
    ancestry: dict[str, list[str]] = {value.replace("\\", "/").lower(): [] for value in outputs}
    for line in completed.stdout.splitlines():
        target, separator, dependency = line.partition("\t")
        require(separator, f"Ninja multi-inputs evidence changed: {line!r}")
        key = target.replace("\\", "/").lower()
        require(key in ancestry, f"Ninja multi-inputs returned an unknown target: {target}")
        ancestry[key].append(dependency)
    missing = [
        output
        for output, dependencies in ancestry.items()
        if not any(
            dependency.replace("\\", "/").lower().rstrip("/").endswith(
                f"cmakefiles/{PREFLIGHT_TARGET.lower()}"
            )
            for dependency in dependencies
        )
    ]
    require(not missing, "protected compile edges lack preflight ancestry: " + ", ".join(sorted(missing)))
    return ancestry


def inspect_configured_editor_build(
    repository_root: Path,
    build_root: Path,
    *,
    requested_config: str = "",
    require_ninja_dependencies: bool = True,
    expected_static_model: dict[str, Any] | None = None,
) -> dict[str, Any]:
    repository_root = repository_root.resolve()
    build_root = build_root.resolve()
    require(repository_root.is_dir(), f"configured editor source root is missing: {repository_root}")
    require(build_root.is_dir(), f"configured editor build root is missing: {build_root}")
    try:
        build_root.relative_to(repository_root)
    except ValueError:
        pass
    else:
        raise IdentityError("configured editor build root must remain outside the repository")

    index_path, objects, index = _file_api_objects(build_root)
    cmake_generator = index.get("cmake", {}).get("generator", {}).get("name")
    require(cmake_generator == "Ninja", f"configured editor gate requires Ninja, got {cmake_generator!r}")
    codemodel = load_json(objects["codemodel"])
    require(isinstance(codemodel, dict), "File API codemodel is not an object")
    paths = codemodel.get("paths")
    require(isinstance(paths, dict), "File API codemodel lacks source/build paths")
    require(
        Path(str(paths.get("source", ""))).resolve() == repository_root,
        "File API codemodel belongs to a different source tree",
    )
    require(
        Path(str(paths.get("build", ""))).resolve() == build_root,
        "File API codemodel belongs to a different build tree",
    )
    configurations = codemodel.get("configurations")
    require(isinstance(configurations, list) and configurations, "File API codemodel has no configurations")
    matching_configs = [
        config
        for config in configurations
        if isinstance(config, dict) and (not requested_config or config.get("name") == requested_config)
    ]
    require(
        len(matching_configs) == 1,
        f"configured editor gate does not have exactly one requested configuration {requested_config!r}",
    )
    configuration = matching_configs[0]
    require(isinstance(configuration, dict), "File API configuration is not an object")
    references = _file_api_target_references(configuration)
    target_models: dict[str, dict[str, Any]] = {}
    target_paths: dict[str, Path] = {}
    target_ids: dict[str, str] = {}
    for reference in references:
        name = reference["name"]
        target_id = reference["id"]
        target_path = _file_api_reply_path(
            objects["codemodel"].resolve().parent,
            reference["jsonFile"],
            f"File API target reply {name}",
        )
        target = load_json(target_path)
        require(
            isinstance(target, dict)
            and target.get("name") == name
            and target.get("id") == target_id,
            f"File API target reply identity changed: {name}",
        )
        target_models[name] = target
        target_paths[name] = target_path
        target_ids[name] = target_id
    cache = load_json(objects["cache"])
    cache_entries = _cache_entries(cache)
    require(
        Path(str(cache_entries.get("CMAKE_HOME_DIRECTORY", ""))).resolve() == repository_root,
        "configured cache belongs to a different source tree",
    )
    core_only = str(cache_entries.get("DEMO_EDITOR_CORE_ONLY", "")).strip().upper() in {
        "1",
        "ON",
        "TRUE",
        "YES",
        "Y",
    }
    require(
        PREFLIGHT_TARGET in target_models and target_models[PREFLIGHT_TARGET].get("type") == "UTILITY",
        "configured-dependency preflight target is missing or is not a utility target",
    )
    fenced_targets = _cache_list(cache_entries.get("DEMO_EDITOR_FENCED_TARGETS"), "DEMO_EDITOR_FENCED_TARGETS")
    actual_compile_targets = sorted(
        (
            name
            for name, target in target_models.items()
            if target.get("type") in COMPILE_CAPABLE_TARGET_TYPES
        ),
        key=str.lower,
    )
    require(
        fenced_targets == actual_compile_targets,
        "configured compile-target census differs from the final File API target set",
    )
    configured_anchors = _configured_editor_anchors(repository_root, build_root, target_models)
    configured_repository_closure = _configured_repository_closure(
        configured_anchors, target_models, repository_root
    )
    if expected_static_model is not None:
        static_anchors = set(expected_static_model.get("editorAnchors", []))
        static_protected = set(expected_static_model.get("protectedTargets", []))
        unclassified = configured_anchors - static_anchors
        require(
            not unclassified,
            "unclassified configured production editor targets: " + ", ".join(sorted(unclassified)),
        )
        expected_configured_closure = static_protected & set(target_models)
        require(
            configured_repository_closure == expected_configured_closure,
            "configured production editor closure differs from static discovery "
            f"(configured={sorted(configured_repository_closure)}, static={sorted(expected_configured_closure)})",
        )
        protected_targets = sorted(configured_repository_closure)
    else:
        protected_targets = sorted(configured_repository_closure)
    require("scene_editor_core" in protected_targets, "final configured graph lacks scene_editor_core protection")
    require(
        "scene_editor_document" in protected_targets,
        "final configured graph lacks scene_editor_document protection",
    )
    if not core_only:
        protected_targets = sorted(set(protected_targets) | {EXPECTED_TARGET})
    if core_only:
        require(EXPECTED_TARGET not in target_models, "core-only configured build unexpectedly contains imgui_demo")

    document_sources, document_compiled = _resolved_target_sources(
        repository_root,
        build_root,
        target_models["scene_editor_document"],
    )
    expected_document_sources = [
        (repository_root / value).resolve() for value in EXPECTED_SCENE_EDITOR_DOCUMENT_SOURCES
    ]
    expected_document_compiled = [
        (repository_root / value).resolve()
        for value in EXPECTED_SCENE_EDITOR_DOCUMENT_COMPILED_SOURCES
    ]
    require(
        document_sources == expected_document_sources,
        "final File API scene_editor_document source identity changed",
    )
    require(
        document_compiled == expected_document_compiled,
        "final File API scene_editor_document compiled source identity changed",
    )
    document_dependencies = _target_dependencies(target_models["scene_editor_document"])
    require(
        PREFLIGHT_TARGET in document_dependencies,
        "final File API scene_editor_document lacks the configured-dependency preflight edge",
    )

    scene_sources, scene_compiled = _resolved_target_sources(
        repository_root,
        build_root,
        target_models["scene_editor_core"],
    )
    expected_scene_sources = [(repository_root / value).resolve() for value in EXPECTED_SCENE_EDITOR_SOURCES]
    expected_scene_compiled = [
        (repository_root / value).resolve() for value in EXPECTED_SCENE_EDITOR_COMPILED_SOURCES
    ]
    require(scene_sources == expected_scene_sources, "final File API scene_editor_core source identity changed")
    require(scene_compiled == expected_scene_compiled, "final File API scene_editor_core compiled source identity changed")
    scene_dependencies = _target_dependencies(target_models["scene_editor_core"])
    require(
        PREFLIGHT_TARGET in scene_dependencies,
        "final File API scene_editor_core lacks the configured-dependency preflight edge",
    )

    imgui_source_root: Path | None = None
    imgui_sources: list[Path] = []
    imgui_compiled: list[Path] = []
    backend: str | None = None
    if not core_only:
        imgui_source_root = (build_root / "_deps/imgui-src").resolve()
        imgui_sources, imgui_compiled = _resolved_target_sources(
            repository_root,
            build_root,
            target_models[EXPECTED_TARGET],
        )
        try:
            imgui_relative = sorted(path.relative_to(imgui_source_root).as_posix() for path in imgui_sources)
            imgui_compiled_relative = sorted(
                path.relative_to(imgui_source_root).as_posix() for path in imgui_compiled
            )
        except ValueError as exc:
            raise IdentityError("final File API imgui_demo source escapes the canonical FetchContent checkout") from exc
        backend = next(
            (
                name
                for name, backend_inputs in BACKEND_TARGET_INPUTS.items()
                if imgui_relative == sorted((*BASE_TARGET_INPUTS, *backend_inputs))
            ),
            None,
        )
        require(backend is not None, "final File API imgui_demo SOURCES do not equal a canonical platform source list")
        expected_imgui_compiled = sorted(
            value for value in imgui_relative if PurePosixPath(value).suffix.lower() in COMPILED_SUFFIXES
        )
        require(
            imgui_compiled_relative == expected_imgui_compiled,
            "final File API imgui_demo compiled source set is not canonical",
        )
        expected_imgui_dependencies = ({"glfw"} if backend == "glfw" else set()) | {PREFLIGHT_TARGET}
        actual_imgui_dependencies = {
            str(dependency.get("id", "")).split("::@", 1)[0]
            for dependency in target_models[EXPECTED_TARGET].get("dependencies", [])
            if isinstance(dependency, dict)
        }
        require(
            actual_imgui_dependencies == expected_imgui_dependencies,
            "final File API imgui_demo dependency set is not canonical",
        )

    for target_name in protected_targets:
        target = target_models[target_name]
        _assert_no_forbidden_dependency(
            json.dumps(target, sort_keys=True),
            f"configured File API target {target_name}",
            allow_renderer_sentinel=target_name not in configured_anchors,
        )
        backtrace_files = target.get("backtraceGraph", {}).get("files", [])
        require(isinstance(backtrace_files, list), f"configured target {target_name} lacks backtrace files")
        for value in backtrace_files:
            require(isinstance(value, str) and value, f"configured target {target_name} has an invalid backtrace file")
            path = Path(value)
            if not path.is_absolute():
                path = repository_root / path
            path = path.resolve()
            try:
                path.relative_to(repository_root)
            except ValueError as exc:
                raise IdentityError(
                    f"configured target {target_name} was mutated by non-repository CMake: {path}"
                ) from exc

    toolchains = load_json(objects["toolchains"])
    require(
        isinstance(toolchains, dict) and isinstance(toolchains.get("toolchains"), list),
        "File API toolchains reply changed",
    )
    cxx_toolchains = [
        item for item in toolchains["toolchains"] if isinstance(item, dict) and item.get("language") == "CXX"
    ]
    require(len(cxx_toolchains) == 1, "configured editor build lacks exactly one CXX toolchain")
    compiler_record = cxx_toolchains[0].get("compiler", {})
    require(isinstance(compiler_record, dict), "configured CXX compiler record changed")
    compiler_path = compiler_record.get("path")
    require(isinstance(compiler_path, str) and Path(compiler_path).is_file(), "configured CXX compiler path is invalid")
    compiler_id = compiler_record.get("id")
    require(compiler_id in {"AppleClang", "Clang", "GNU", "MSVC"}, f"unapproved configured CXX compiler id: {compiler_id!r}")
    compiler_version = compiler_record.get("version")
    require(isinstance(compiler_version, str) and compiler_version, "configured CXX compiler version is missing")
    try:
        Path(compiler_path).resolve().relative_to(repository_root)
    except ValueError:
        pass
    else:
        raise IdentityError("repository-owned CXX compiler/wrapper is forbidden")
    try:
        Path(compiler_path).resolve().relative_to(build_root)
    except ValueError:
        pass
    else:
        raise IdentityError("build-local CXX compiler/wrapper is forbidden")
    compiler_identity = _trusted_compiler_identity(Path(compiler_path), compiler_id, compiler_version)
    cache_compiler_value = cache_entries.get("CMAKE_CXX_COMPILER")
    require(
        isinstance(cache_compiler_value, str) and Path(cache_compiler_value).is_absolute(),
        "configured cache lacks an absolute CMAKE_CXX_COMPILER",
    )
    cache_compiler = _canonical_file(Path(cache_compiler_value), "cached CXX compiler")
    trusted_compiler = _canonical_file(Path(compiler_identity["canonicalPath"]), "trusted configured compiler")
    require(
        _same_file(cache_compiler, trusted_compiler),
        "File API toolchain and CMAKE_CXX_COMPILER cache identify different executables",
    )
    require(
        sha256(cache_compiler) == compiler_identity["sha256"],
        "cached CXX compiler binary changed after trust binding",
    )
    compiler_style = (
        "msvc"
        if compiler_id == "MSVC" or Path(compiler_path).name.lower() == "clang-cl.exe"
        else "posix"
    )
    _assert_no_forbidden_dependency(
        json.dumps(toolchains, sort_keys=True),
        "configured File API toolchains",
        allow_renderer_sentinel=True,
    )
    require(
        not str(cache_entries.get("FETCHCONTENT_SOURCE_DIR_IMGUI", "") or "").strip(),
        "FETCHCONTENT_SOURCE_DIR_IMGUI cache/CLI override is forbidden",
    )
    cache_response_evidence = _scan_cache_compiler_flags(
        cache_entries, build_root, compiler_style
    )
    for name, value in cache_entries.items():
        value_text = str(value or "").strip()
        if value_text and _is_unapproved_configure_channel(name):
            raise IdentityError(
                f"unapproved configured CMake injection channel is non-empty: {name}={value_text}"
            )
        if value_text and LINK_EFFECT_CACHE_PATTERN.fullmatch(name):
            frameworks = {
                re.sub(r"^[^A-Za-z]+|[^A-Za-z]+$", "", framework).lower()
                for framework in _framework_link_operands([value_text])
            }
            require(
                not frameworks & FORBIDDEN_FRAMEWORK_NAMES,
                f"configured CMake link-effect cache entry bypasses exclusive APPLE framework authority: {name}",
            )
        if value_text:
            _assert_no_forbidden_dependency(
                value_text,
                f"configured CMake cache entry {name}",
                allow_renderer_sentinel=True,
            )
    for name in COMPILER_ENVIRONMENT_CHANNELS:
        value = os.environ.get(name, "").strip()
        if value:
            raise IdentityError(f"unapproved compiler environment injection channel is non-empty: {name}")

    compile_commands_path = build_root / "compile_commands.json"
    compile_database = load_json(compile_commands_path)
    require(isinstance(compile_database, list) and compile_database, "configured build lacks a compile database")
    protected_compiled_sources: dict[str, dict[Path, str]] = {}
    for target_name in protected_targets:
        target = target_models[target_name]
        if target.get("type") not in COMPILE_CAPABLE_TARGET_TYPES:
            continue
        compiled = _target_compiled_sources_by_language(repository_root, build_root, target)
        if not compiled:
            continue
        protected_compiled_sources[target_name] = compiled
        require(
            PREFLIGHT_TARGET in _target_dependencies(target),
            f"configured protected compile target {target_name} lacks the preflight dependency",
        )
        for source in compiled:
            try:
                source_text = source.read_text(encoding="utf-8", errors="replace")
            except OSError as exc:
                raise IdentityError(f"cannot scan configured compiled source {source}: {exc}") from exc
            _assert_no_forbidden_dependency(
                source_text,
                f"configured compiled source {source}",
                allow_renderer_sentinel=target_name not in configured_anchors,
            )
    protected_compile_targets = sorted(protected_compiled_sources)
    require(protected_compile_targets, "configured production editor closure has no compiled targets")
    declared_production_owners = {
        (repository_root / relative).resolve(): target_name
        for target_name, relatives in (
            ("scene_editor_document", EXPECTED_SCENE_EDITOR_DOCUMENT_COMPILED_SOURCES),
            ("scene_editor_core", EXPECTED_SCENE_EDITOR_COMPILED_SOURCES),
        )
        for relative in relatives
    }
    for target_name, compiled in protected_compiled_sources.items():
        for source in compiled:
            declared_owner = declared_production_owners.get(source)
            require(
                declared_owner is None or target_name == declared_owner,
                f"production source {source} is compiled outside its declared target {declared_owner}",
            )

    command_entries_by_target: dict[str, list[dict[str, Any]]] = {
        name: [] for name in protected_compile_targets
    }
    for entry in compile_database:
        require(isinstance(entry, dict), "compile_commands.json contains a non-object entry")
        output = entry.get("output")
        require(isinstance(output, str) and output, "compile command lacks target-owned output")
        normalized_output = output.replace("\\", "/").lower()
        for target_name in protected_compile_targets:
            if f"/cmakefiles/{target_name.lower()}.dir/" in normalized_output:
                command_entries_by_target[target_name].append(entry)
                break
    compile_entries_by_key: dict[tuple[Path, str], dict[str, Any]] = {}
    for target_name, entries in command_entries_by_target.items():
        require(entries, f"compile_commands.json has no entries for {target_name}")
        command_sources = sorted(_compile_entry_source(entry) for entry in entries)
        require(
            command_sources == sorted(protected_compiled_sources[target_name]),
            f"compile_commands.json {target_name} compiled source set differs from File API",
        )
        for entry in entries:
            key = _compile_edge_key(entry, build_root, f"compile_commands {target_name} entry")
            require(key not in compile_entries_by_key, f"compile_commands repeats protected compile edge {key}")
            compile_entries_by_key[key] = entry
            tokens = _compile_entry_tokens(entry, compiler_style, f"compile_commands {target_name} entry")
            _require_exact_compiler(tokens, compiler_identity, f"compile_commands {target_name} entry")

    make_program = str(cache_entries.get("CMAKE_MAKE_PROGRAM", "") or "ninja")
    require(
        Path(make_program).is_file() or shutil.which(make_program) is not None,
        "configured Ninja executable is unavailable",
    )
    build_system_path = build_root / "build.ninja"
    require(build_system_path.is_file(), "configured Ninja build file is missing")
    ninja_raw_entries = _ninja_compile_entries(
        build_root,
        make_program,
        protected_compile_targets,
        expand_response_content=False,
    )
    ninja_expanded_entries = _ninja_compile_entries(
        build_root,
        make_program,
        protected_compile_targets,
        expand_response_content=True,
    )

    def keyed(entries: list[dict[str, Any]], label: str) -> dict[tuple[Path, str], dict[str, Any]]:
        result: dict[tuple[Path, str], dict[str, Any]] = {}
        for entry in entries:
            key = _compile_edge_key(entry, build_root, label)
            require(key not in result, f"{label} repeats protected compile edge {key}")
            result[key] = entry
        return result

    ninja_raw_by_key = keyed(ninja_raw_entries, "raw Ninja compdb-targets")
    ninja_expanded_by_key = keyed(ninja_expanded_entries, "expanded Ninja compdb-targets")
    expected_keys = set(compile_entries_by_key)
    require(
        set(ninja_raw_by_key) == expected_keys == set(ninja_expanded_by_key),
        "File API, compile_commands, and Ninja protected source/output cardinality differs",
    )
    expanded_command_tokens: dict[tuple[Path, str], list[str]] = {}
    generator_response_edges: list[str] = []
    command_response_evidence: list[dict[str, Any]] = []
    for key in sorted(expected_keys, key=lambda value: (str(value[0]).lower(), value[1])):
        compile_entry = compile_entries_by_key[key]
        raw_entry = ninja_raw_by_key[key]
        expanded_entry = ninja_expanded_by_key[key]
        compile_tokens = _compile_entry_tokens(compile_entry, compiler_style, "compile_commands protected edge")
        raw_tokens = _compile_entry_tokens(raw_entry, compiler_style, "raw Ninja protected edge")
        _require_exact_compiler(raw_tokens, compiler_identity, "raw Ninja protected edge")
        raw_responses = [token for token in raw_tokens[1:] if token.startswith("@")]
        for token in raw_responses:
            response = Path(token[1:])
            if not response.is_absolute():
                response = build_root / response
            try:
                response.resolve().relative_to(build_root)
            except ValueError as exc:
                raise IdentityError(
                    f"Ninja generator response path escapes the configured build: {token}"
                ) from exc
        expanded_tokens, records = _scan_compile_command(
            expanded_entry,
            next(
                name
                for name in protected_compile_targets
                if f"/cmakefiles/{name.lower()}.dir/" in ("/" + key[1])
            ),
            compiler_identity,
            compiler_style,
            reject_disk_responses=True,
        )
        command_response_evidence.extend(records)
        expanded_command_tokens[key] = expanded_tokens
        if raw_responses:
            require(
                _without_dependency_bookkeeping(raw_tokens)
                != _without_dependency_bookkeeping(expanded_tokens),
                f"Ninja -t compdb-targets -x did not expand generator response content for {key}",
            )
            require(
                _without_dependency_bookkeeping(compile_tokens)
                == _without_dependency_bookkeeping(expanded_tokens),
                f"compile_commands and expanded Ninja arguments differ for protected edge {key}",
            )
            generator_response_edges.append(key[1])
        else:
            require(
                _without_dependency_bookkeeping(compile_tokens)
                == _without_dependency_bookkeeping(raw_tokens),
                f"compile_commands and raw Ninja arguments differ for protected edge {key}",
            )

    preflight_ancestry = _verify_ninja_preflight_ancestry(
        build_root, make_program, ninja_expanded_entries
    )

    if require_ninja_dependencies:
        ninja_dependencies = _run_ninja_tool(build_root, make_program, "deps")
        relevant_dependency_blocks: list[str] = []
        current: list[str] = []
        current_relevant = False
        markers = tuple(f"/cmakefiles/{name.lower()}.dir/" for name in protected_compile_targets)
        for line in ninja_dependencies.splitlines():
            if line and not line[0].isspace():
                if current_relevant:
                    relevant_dependency_blocks.append("\n".join(current))
                current = [line]
                normalized = "/" + line.replace("\\", "/").lower().lstrip("/")
                current_relevant = any(marker in normalized for marker in markers)
            elif current:
                current.append(line)
        if current_relevant:
            relevant_dependency_blocks.append("\n".join(current))
        expected_records = sum(len(entries) for entries in command_entries_by_target.values())
        require(
            len(relevant_dependency_blocks) >= expected_records,
            "Ninja dependency database lacks built records for final editor compiled inputs",
        )
        _assert_no_forbidden_dependency(
            "\n".join(relevant_dependency_blocks),
            "configured Ninja production-editor dependency database",
        )

    return {
        "indexPath": index_path,
        "objects": objects,
        "configuration": str(configuration.get("name", "")),
        "coreOnly": core_only,
        "targetModels": target_models,
        "targetPaths": target_paths,
        "targetIds": target_ids,
        # Keep a second immutable snapshot so callers cannot replace every
        # in-memory ID and have the private configured check accept a coherent
        # forgery without comparing against the File API reply set.
        "targetReferenceIds": dict(target_ids),
        "cacheEntries": cache_entries,
        "imguiBackend": backend,
        "imguiSourceRoot": imgui_source_root,
        "imguiSources": imgui_sources,
        "imguiCompiledSources": imgui_compiled,
        "compileCommandsPath": compile_commands_path,
        "compileEntries": command_entries_by_target,
        "compilerIdentity": compiler_identity,
        "protectedTargets": protected_targets,
        "protectedCompileTargets": protected_compile_targets,
        "expandedCommandTokens": expanded_command_tokens,
        "generatorResponseEdges": sorted(generator_response_edges),
        "responseFileEvidence": cache_response_evidence + command_response_evidence,
        "preflightAncestry": preflight_ancestry,
        "buildSystemPath": build_system_path,
        "cmakeGenerator": cmake_generator,
    }


def generate_identity(args: argparse.Namespace) -> dict[str, object]:
    source_root = args.source_dir.resolve()
    expected_source_root = args.expected_source_dir.resolve()
    repository_root = args.repo_root.resolve()
    build_root = args.build_root.resolve()
    require(source_root.is_dir(), f"resolved Dear ImGui source directory is missing: {source_root}")
    require(source_root == expected_source_root, "resolved Dear ImGui source directory is not the canonical FetchContent checkout")
    require(source_root != repository_root and repository_root not in source_root.parents, "Dear ImGui checkout must be outside the repository")
    require(build_root in source_root.parents, "Dear ImGui checkout must be contained by the configured build tree")
    require((repository_root / "CMakeLists.txt").is_file(), "MGIF CMakeLists.txt is missing")
    generator_path = Path(__file__).resolve()
    try:
        generator_relative = generator_path.relative_to(repository_root).as_posix()
    except ValueError as exc:
        raise IdentityError("identity generator is not repository-owned") from exc
    require(not args.cache_override, "FETCHCONTENT_SOURCE_DIR_IMGUI cache/CLI override is forbidden")
    require(not args.environment_override, "FETCHCONTENT_SOURCE_DIR_IMGUI environment override is forbidden")

    state = inspect_configured_editor_build(
        repository_root,
        build_root,
        requested_config=getattr(args, "config", ""),
        require_ninja_dependencies=True,
    )
    index_path = state["indexPath"]
    objects = state["objects"]
    configuration = state["configuration"]
    target_path = state["targetPaths"][EXPECTED_TARGET]
    target_sources = state["imguiSources"]
    file_api_compiled_sources = state["imguiCompiledSources"]
    relative_inputs = relative_target_inputs(source_root, target_sources)
    relative_file_api_compiled = relative_target_inputs(source_root, file_api_compiled_sources)
    matched_backend = next(
        (
            backend
            for backend, backend_inputs in BACKEND_TARGET_INPUTS.items()
            if relative_inputs == sorted((*BASE_TARGET_INPUTS, *backend_inputs))
        ),
        None,
    )
    require(matched_backend is not None, "final File API imgui_demo SOURCES do not equal a canonical platform source list")
    expected_compiled = sorted(
        value for value in relative_inputs if PurePosixPath(value).suffix.lower() in COMPILED_SUFFIXES
    )
    require(
        relative_file_api_compiled == expected_compiled,
        "final File API imgui_demo compile groups do not equal the canonical compiled source list",
    )

    compile_commands_path = state["compileCommandsPath"]
    command_entries = state["compileEntries"][EXPECTED_TARGET]
    command_sources = [_compile_entry_source(entry) for entry in command_entries]
    relative_command_sources = relative_target_inputs(source_root, command_sources)
    require(
        relative_command_sources == expected_compiled,
        "final compile_commands imgui_demo inputs do not equal the File API compiled source list",
    )
    command_by_source = {
        source.resolve().relative_to(source_root).as_posix(): _compile_entry_text(entry)
        for source, entry in zip(command_sources, command_entries)
    }
    require(len(command_by_source) == len(command_entries), "imgui_demo has duplicate compile-command inputs")

    commit = run_git(source_root, "rev-parse", "HEAD^{commit}").lower()
    tree = run_git(source_root, "rev-parse", "HEAD^{tree}").lower()
    remote = run_git(source_root, "remote", "get-url", "origin")
    require(commit == EXPECTED_COMMIT, f"resolved Dear ImGui commit changed: {commit}")
    require(tree == EXPECTED_TREE, f"resolved Dear ImGui tree changed: {tree}")
    require(normalize_repository(remote) == normalize_repository(EXPECTED_REPOSITORY), "resolved Dear ImGui origin is not canonical upstream")
    status = run_git(source_root, "status", "--porcelain=v1", "--untracked-files=all")
    require(not status, "resolved Dear ImGui checkout differs from the pinned Git tree")

    hashes = {relative: sha256(source_root.joinpath(*PurePosixPath(relative).parts)) for relative in relative_inputs}
    build_system_path = state["buildSystemPath"]

    def evidence(path: Path) -> dict[str, str]:
        return {"path": path.resolve().relative_to(build_root).as_posix(), "sha256": sha256(path)}

    cmake_generator = state["cmakeGenerator"]
    return {
        "schemaVersion": 2,
        "kind": "mgif.editor-dependency-build-identity",
        "dependency": "dear_imgui",
        "repository": EXPECTED_REPOSITORY,
        "target": EXPECTED_TARGET,
        "sourceMode": "fetchcontent_git_outside_repository",
        "projectSourceRoot": repository_root.as_posix(),
        "projectCMakeSha256": sha256(repository_root / "CMakeLists.txt"),
        "generator": {"path": generator_relative, "sha256": sha256(generator_path)},
        "resolvedSourceRoot": source_root.as_posix(),
        "buildRoot": build_root.as_posix(),
        "sourceOverride": {"cacheOrCli": False, "environment": False},
        "resolvedGitCommit": commit,
        "resolvedGitTree": tree,
        "platformBackend": matched_backend,
        "targetSourceRelativePaths": relative_inputs,
        "compiledSourceRelativePaths": expected_compiled,
        "compiledSourceSha256": {relative: hashes[relative] for relative in expected_compiled},
        "targetSourceSha256": hashes,
        "configuredBuildEvidence": {
            "cmakeGenerator": cmake_generator,
            "configuration": configuration,
            "fileApiIndex": evidence(index_path),
            "fileApiCodemodel": evidence(objects["codemodel"]),
            "fileApiCache": evidence(objects["cache"]),
            "fileApiToolchains": evidence(objects["toolchains"]),
            "fileApiImguiTarget": evidence(target_path),
            "compileCommands": evidence(compile_commands_path),
            "buildSystem": evidence(build_system_path),
            "compiledCommandSha256": {
                relative: text_sha256(command_by_source[relative]) for relative in sorted(command_by_source)
            },
        },
    }


def parse_args() -> argparse.Namespace:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--repo-root", type=Path, required=True)
    parser.add_argument("--build-root", type=Path, required=True)
    parser.add_argument("--source-dir", type=Path, required=True)
    parser.add_argument("--expected-source-dir", type=Path, required=True)
    parser.add_argument("--config", default="")
    parser.add_argument("--output", type=Path, required=True)
    parser.add_argument("--cache-override", action="store_true")
    parser.add_argument("--environment-override", action="store_true")
    return parser.parse_args()


def main() -> int:
    args = parse_args()
    try:
        identity = generate_identity(args)
        args.output.parent.mkdir(parents=True, exist_ok=True)
        with tempfile.NamedTemporaryFile(
            mode="w",
            encoding="utf-8",
            newline="\n",
            dir=args.output.parent,
            prefix=f".{args.output.name}.",
            suffix=".tmp",
            delete=False,
        ) as stream:
            json.dump(identity, stream, ensure_ascii=True, indent=2, sort_keys=True)
            stream.write("\n")
            temporary = Path(stream.name)
        os.replace(temporary, args.output)
    except (IdentityError, OSError) as exc:
        print(f"Dear ImGui build identity generation failed: {exc}", file=os.sys.stderr)
        return 1
    print(f"Generated final configured Dear ImGui build identity: {args.output}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
