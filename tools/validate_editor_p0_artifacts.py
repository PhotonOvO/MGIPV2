#!/usr/bin/env python3
"""Validate tracked P0 glTF editor parity artifacts without third-party modules."""

from __future__ import annotations

import argparse
import glob
import hashlib
import json
import os
import re
import shlex
import subprocess
import sys
from datetime import datetime
from pathlib import Path, PurePosixPath
from typing import Any, NamedTuple

import generate_editor_dependency_identity as dependency_identity
from json_schema_subset import JsonSchemaError, check_schema_supported, validate_json_schema


EXPECTED_SPEC_SHA256 = "bfd4b5ffe8d73b9bb887047a8cad5ad96c7184d182eeb752aa47028b05bd679e"
EXPECTED_REFERENCE_COMMIT = "50373c7b0ff58078727af2c2aac70e29d5e74752"
EXPECTED_REFERENCE_TREE = "03e44de44217b109ab71dfbf6aee3989593d76b0"
EXPECTED_TARGET_COMMIT = "6c36567625b305286755a734a66188a22df18e77"
EXPECTED_MGIF_BASELINE_SHA256 = "2afbbc051b9188b88f0f506215c34448047683449ba87aa751520d53766d0b34"
EXPECTED_REFERENCE_SOURCE_EVIDENCE_SHA256 = "457117f8576c699833dd41006d762c65b3f1459325efca247134e6628e919a97"
EXPECTED_LICENSE_SOURCES_SHA256 = "b302ff87a11192a284b23dad7baa493826473b98aaf34f288994ee39ffcd3971"
EXPECTED_DEAR_IMGUI_REPOSITORY = "https://github.com/ocornut/imgui.git"
EXPECTED_DEAR_IMGUI_COMMIT = "fd13a1e8923a0a7077b404fc36fd063b25a0c0b5"
EXPECTED_DEAR_IMGUI_TREE = "0c8986ccb662ea838c04f7bed4cc56d3379132ef"
EXPECTED_DEAR_IMGUI_TARGET = "imgui_demo"
EXPECTED_GLFW_REPOSITORY = "https://github.com/glfw/glfw.git"
EXPECTED_GLFW_TAG = "3.4"
EXPECTED_VOLK_REPOSITORY = "https://github.com/zeux/volk.git"
EXPECTED_VOLK_TAG = "master"
EXPECTED_VOLK_TARGET = "volk"
EXPECTED_DEMO_CORE_COMMON_RENDERER_LINK = (
    "demo_core",
    "PUBLIC",
    "scene_editor_core",
    EXPECTED_VOLK_TARGET,
    EXPECTED_DEAR_IMGUI_TARGET,
    "glm",
    "tinygltf",
    "meshoptimizer",
    "Vulkan::Vulkan",
)
EXPECTED_DEMO_CORE_RENDERER_FOUNDATIONS = frozenset(
    EXPECTED_DEMO_CORE_COMMON_RENDERER_LINK[5:]
)
EXPECTED_RENDERER_SCENE_TRANSACTION_LINK = (
    "renderer_scene_transaction",
    "PUBLIC",
    "scene_change_contract",
    "glm",
    "tinygltf",
)
EXPECTED_DEMO_CORE_ANDROID_LINK = (
    "demo_core",
    "PRIVATE",
    "scene_runtime_mapping_runtime",
    "PUBLIC",
    "android",
    "log",
    "native_app_glue",
)
EXPECTED_DEMO_CORE_ANDROID_FOUNDATIONS = frozenset(
    EXPECTED_DEMO_CORE_ANDROID_LINK[4:]
)
EXPECTED_DEMO_CORE_UNIX_LINK = (
    "demo_core",
    "PUBLIC",
    "${X11_LIBRARIES}",
    "Threads::Threads",
)
EXPECTED_DEMO_CORE_UNIX_FOUNDATIONS = frozenset(
    EXPECTED_DEMO_CORE_UNIX_LINK[2:]
)
EXPECTED_DEMO_CORE_WINMM_LINK = ("demo_core", "PUBLIC", "winmm.lib")
EXPECTED_DEMO_CORE_D3D12_LINK = (
    "demo_core",
    "PUBLIC",
    "d3d12.lib",
    "dxgi.lib",
    "dxguid.lib",
)
EXPECTED_DEMO_CORE_WINDOWS_FOUNDATIONS = frozenset(
    (*EXPECTED_DEMO_CORE_WINMM_LINK[2:], *EXPECTED_DEMO_CORE_D3D12_LINK[2:])
)
EXPECTED_NVTT_VARIABLES = {
    "NVTT_ROOT": [
        "NVTT_ROOT",
        "C:/Program Files/NVIDIA Corporation/NVIDIA Texture Tools",
        "CACHE",
        "PATH",
        "Path to the NVIDIA Texture Tools install root",
    ],
    "NVTT_INCLUDE_DIR": ["NVTT_INCLUDE_DIR", "${NVTT_ROOT}/include"],
    "NVTT_LIBRARY": [
        "NVTT_LIBRARY",
        "${NVTT_ROOT}/lib/x64-v142/nvtt30205.lib",
    ],
    "NVTT_DLL": ["NVTT_DLL", "${NVTT_ROOT}/nvtt30205.dll"],
}
EXPECTED_DEAR_IMGUI_CORE_SOURCES = (
    "imgui.cpp",
    "imgui.h",
    "imgui_draw.cpp",
    "imgui_tables.cpp",
    "imgui_widgets.cpp",
)
# Static validation locks acquisition and compiled inputs. A later build-identity
# record must additionally prove the resolved Git commit/tree and SHA-256 of
# every compiled file below imgui_SOURCE_DIR; command-line FetchContent source
# overrides cannot be proven from repository text alone.
DEAR_IMGUI_BUILD_IDENTITY_HASH_REQUIREMENTS = (
    "resolvedGitCommit",
    "resolvedGitTree",
    "targetSourceRelativePaths",
    "targetSourceSha256",
    "compiledSourceRelativePaths",
    "compiledSourceSha256",
    "configuredBuildEvidence",
)
EXPECTED_DEAR_IMGUI_TARGET_SOURCE_VARIANTS = {
    "android": [
        "backends/imgui_impl_android.cpp",
        "backends/imgui_impl_android.h",
        "imgui.cpp",
        "imgui.h",
        "imgui_demo.cpp",
        "imgui_draw.cpp",
        "imgui_tables.cpp",
        "imgui_widgets.cpp",
    ],
    "glfw": [
        "backends/imgui_impl_glfw.cpp",
        "backends/imgui_impl_glfw.h",
        "imgui.cpp",
        "imgui.h",
        "imgui_demo.cpp",
        "imgui_draw.cpp",
        "imgui_tables.cpp",
        "imgui_widgets.cpp",
    ],
}
EXPECTED_DEAR_IMGUI_COMPILED_SOURCE_VARIANTS = {
    platform: [value for value in values if PurePosixPath(value).suffix.lower() != ".h"]
    for platform, values in EXPECTED_DEAR_IMGUI_TARGET_SOURCE_VARIANTS.items()
}
EXPECTED_FEATURE_IDS = [f"F{index:02d}" for index in range(1, 43)]
EXPECTED_ORACLE_IDS = [
    "O01-same-parent-move-noop",
    "O02-root-to-root-move-noop",
    "O03-reparent-undo-append-order",
    "O04-unique-root-delete",
    "O05-gizmo-escape-only",
    "O06-gizmo-escape-release-same-frame",
    "O07-gizmo-focus-loss",
    "O08-viewport-click-classifier",
    "O09-multi-scene-structural-matrix",
    "O10-material-clipboard",
]
EXPECTED_EDGE_IDS = [f"E{index:03d}" for index in range(1, 111)]
EXPECTED_EDGE_COUNTS = {
    "adjacency": 16,
    "boundary": 8,
    "concurrency": 21,
    "empty": 17,
    "encoding": 3,
    "idempotency": 21,
    "ordering": 16,
    "precision": 8,
}
EXPECTED_EDGE_CATEGORY_ORDER = list(EXPECTED_EDGE_COUNTS)
EXPECTED_DEPENDENCY_BOUNDARY = {
    "status": "locked_no_exceptions",
    "requirements": ["R21", "R24"],
    "productionEditorRoots": ["scene/editor", "app/editor", "render/editor"],
    "allowedUiFoundation": [
        "repository_upstream_dear_imgui",
        "mgif_owned_app_adapters",
        "mgif_owned_rhi_adapters",
        "mgif_owned_scene_adapters",
    ],
    "forbiddenDependencyFamilies": [
        "nvpro_core",
        "nvcore",
        "nvapp",
        "nvgui",
        "nvvk",
        "nvvkgltf",
        "reference_vulkan_helpers",
    ],
    "externalEvidencePolicy": (
        "Reference executables and probes may retain native nvpro dependencies only in isolated external "
        "evidence graphs and must never become MGIF source, target, include, link, or transitive production dependencies."
    ),
    "exceptionApprovalRequirements": [
        "prior_explicit_user_approval",
        "minimality",
        "alternatives_considered",
        "platform_impact",
        "license_impact",
        "binary_size_impact",
        "exact_symbols_and_targets",
        "owned_dated_removal_plan",
    ],
    "approvedExceptions": [],
    "targetEvidence": ["T-RHI-01", "T-RHI-02", "T-RHI-03", "T-LIC-01"],
    "allowedOutOfScopeSentinels": [
        "NVVK_SEMAPHORE_DEBUG in unrelated existing renderer declarations outside editor scope"
    ],
}
EXPECTED_DEPENDENCY_BOUNDARY_BINDINGS = {
    "status": "assigned",
    "executed": 0,
    "requirements": {
        "R21": ["T-RHI-01", "T-RHI-02", "T-RHI-03", "T-LIC-01"],
        "R24": ["T-LIC-01"],
    },
}
PRODUCTION_EDITOR_ROOTS = tuple(EXPECTED_DEPENDENCY_BOUNDARY["productionEditorRoots"])
FIRST_PARTY_SOURCE_ROOTS = ("app", "scene", "render", "rhi", "loader", "shaders", "common", "include")
CMAKE_SEARCH_ROOTS = ("cmake", *FIRST_PARTY_SOURCE_ROOTS)
ALLOWED_TRANSITIVE_CMAKE_VARIABLES = {
    "ANDROID_NDK",
    "CMAKE_COMMAND",
    "DEMO_FOUNDATION_FRAMEWORK",
    "DEMO_METAL_FRAMEWORK",
    "DEMO_DXC_SPIRV_EXECUTABLE",
    "GENERATED_SHADER_GLSL_HEADERS",
    "GENERATED_SHADER_SLANG_HEADERS",
    "GENERATED_SHADER_SPV_BINARIES",
    "SHADER_H_FILES",
    "Slang_SLANGC_EXECUTABLE",
    "VKDEMO_NVTX_CANDIDATE",
    "X11_LIBRARIES",
    "imgui_SOURCE_DIR",
    "meshoptimizer_SOURCE_DIR",
    "Python3_EXECUTABLE",
    "volk_SOURCE_DIR",
    "vulkanmemoryallocator_SOURCE_DIR",
}
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
CONFIGURED_EDITOR_TARGETS = (
    "scene_editor_document",
    "scene_editor_core",
    EXPECTED_DEAR_IMGUI_TARGET,
)
EXPECTED_SCENE_EDITOR_TARGET_SOURCES = [
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
]
EXPECTED_SCENE_EDITOR_COMPILED_SOURCES = [
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
]
EXPECTED_TINYGLTF_REPOSITORY = "https://github.com/syoyo/tinygltf.git"
EXPECTED_TINYGLTF_TAG = "v2.9.3"
EXPECTED_TINYGLTF_TARGET = "tinygltf"
EXPECTED_SCENE_EDITOR_DOCUMENT_TARGET = "scene_editor_document"
EXPECTED_SCENE_EDITOR_DOCUMENT_SOURCES = {
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
}
EXPECTED_TINYGLTF_CONSUMERS = {
    "loader/TinyGltfImplementation.cpp",
    "scene/editor/GltfDocument.cpp",
    "scene/editor/SceneCompactor.cpp",
    "scene/editor/SceneMerger.cpp",
    "scene/editor/StructuralDelta.cpp",
    "scene/editor/SceneCommands.cpp",
    "scene/editor/GltfDocumentIO.cpp",
    "scene/editor/GltfValidator.cpp",
    "scene/editor/AnimationPlayback.cpp",
}
UNAPPROVED_CONFIGURE_CHANNELS = {
    "CMAKE_C_COMPILER_LAUNCHER",
    "CMAKE_CXX_COMPILER_LAUNCHER",
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
EXPECTED_INPUT_NORMALIZED_SHA256 = "a008988dd8d1a8fe4cfb978a14853887c919be539a97fa14435b1a1eae83e4d5"
EXPECTED_D20_CRASH_CASE_IDS = {
    "D20_S0_node_drop_noncurrent_root_to_noncurrent_child_switch_before_undo",
    "D20_S1_node_drop_noncurrent_root_to_noncurrent_child_switch_before_undo",
}
EXPECTED_D20_REQUIREMENT_TOTALS = {
    "D20-SW": 2,
    "D20-RN": 4,
    "D20-RL": 24,
    "D20-N-DUP": 24,
    "D20-N-DEL": 24,
    "D20-N-REN": 24,
    "D20-N-AC-E": 24,
    "D20-N-AC-P": 24,
    "D20-N-AC-D": 24,
    "D20-N-AC-S": 24,
    "D20-RD": 48,
    "D20-DD": 52,
}
UI_RUNTIME_EVIDENCE_ROOT = r"G:\ReferenceOracleRuns\vkgr-50373c7b-ui-runtime"
EXPECTED_UI_RUNTIME_REFERENCE = {
    "application": {
        "repository": "vk_gltf_renderer",
        "commit": EXPECTED_REFERENCE_COMMIT,
        "tree": EXPECTED_REFERENCE_TREE,
        "sourceArchive": {
            "path": UI_RUNTIME_EVIDENCE_ROOT + r"\source-vk-gltf-renderer-50373c7b.tar.gz",
            "sha256": "d4a293e76a349d85c1f220fe8d0cd9ec01de92f50dff5b81a644a53a5d458c6e",
        },
    },
    "companion": {
        "repository": "nvpro_core2",
        "commit": "9b482eb8961433c51b77e04ac48964f4eb585481",
        "tree": "d9881fe4bfed55f8d9238e4ae16ac44d00bcaaf8",
        "sourceArchive": {
            "path": UI_RUNTIME_EVIDENCE_ROOT + r"\source-nvpro-core2-9b482eb8.tar.gz",
            "sha256": "ed698eb45b838042c3536226e764aee0fce11973e2569adfd473bb4ee40b08f2",
        },
    },
    "executable": {
        "path": UI_RUNTIME_EVIDENCE_ROOT + r"\runtime-exact-name\_bin\RelWithDebInfo\vk_gltf_renderer.exe",
        "relativePath": "runtime-exact-name/_bin/RelWithDebInfo/vk_gltf_renderer.exe",
        "bytes": 16502784,
        "sha256": "a02572b63f4af50bbaa26cf1909e1938bf7d72a7ae28904766d7e83394fd326b",
        "configuration": "RelWithDebInfo",
    },
}
EXPECTED_UI_RUNTIME_SOURCE_EVIDENCE = {
    "report": (
        "REPORT.md",
        "5add6d86f88cb33c351509cf8d9f3bfc1c3d2730d177e3ba287e97114b52d4c5",
    ),
    "screenshotIndexJson": (
        "SCREENSHOT-INDEX.json",
        "bf38b15883a7144b912a3dbfd4f95064c5777a44c15458c1508c8935d7a92df6",
    ),
    "screenshotIndexMarkdown": (
        "SCREENSHOT-INDEX.md",
        "ce46f8c67f75975320b0b954fc14e0fdbfc30efcfca2df7016f01b5f61b05c20",
    ),
    "artifactHashManifest": (
        "SHA256SUMS.txt",
        "b446338749310b3e0ee30de85422577f8ffd3531ff014841eeab69734b207191",
    ),
    "buildIdentity": (
        "provenance/build-identity.json",
        "aa88097b0c4991b19fac6bcf1064b8ba7316a2e4d0be723e273985e9d93e7b96",
    ),
    "buildRuntimeProvenance": (
        "provenance/build-runtime-provenance.md",
        "81c33d2208e4562281031abd0e68a6ce966d429b8c0168b213b43e2060c63d87",
    ),
    "exportVerification": (
        "provenance/export-verification-v2.json",
        "4b20dee41fa765d50814012c393a26b837b25bb654b199d1d37729361a7e0220",
    ),
    "actionTraceIndex": (
        "provenance/action-trace-index.json",
        "5cd9e9a72b08d0c345156e17989861ed097a44b7ad5bfbbac330b2b42a79f7a4",
    ),
}
EXPECTED_UI_RUNTIME_COVERAGE = {
    "P0-T02": (
        "targeted-partial-only",
        "No exhaustive hover, active, disabled, busy, viewport-size, or DPI matrix.",
    ),
    "O04": (
        "not-closed",
        "Generic Delete modal surface only; not the unique-root lifecycle or results.",
    ),
    "O05": (
        "uncovered",
        "No active-drag Escape or release-ordering trace.",
    ),
    "O06": (
        "partial",
        "Duplicate, Undo, and history labels only.",
    ),
    "O07": (
        "uncovered",
        "No focus-loss partition or synthetic-release trace.",
    ),
    "O08": (
        "not-closed",
        "Live viewport only; no picking, camera, Primitive, or browser-focus results.",
    ),
    "O09": (
        "partial",
        "Multiple-scene controls and generic actions only.",
    ),
    "O10": (
        "not-closed",
        "Visible Scene List and Inspector surfaces only; no clipboard or assignment/history traces.",
    ),
}
EXPECTED_UI_RUNTIME_PRIMARY_SCREENSHOTS = {
    "screenshots/01-clean-first-layout.png": "9905c5c48864cd198d80d8415b0c776d8d54d1ae13784cd1ee327359dbe97fcf",
    "screenshots/02-file-menu.png": "a6567dbd032a8b02dad55b100a8e0e9cf31999216f83d7a93b4fade3e1466942",
    "screenshots/03b-edit-menu.png": "8979691e2501ecfdb9ae1394ea5b3f2dedaaa4539bbc200672a49ec87054f4f1",
    "screenshots/04b-view-menu.png": "cb3c22b214978182cb84b5ff1eff0ecc3a8e89478db153dcaa09847a16d8a431",
    "screenshots/05b-windows-menu.png": "6872f0cb2e3345014b2a3bebd13b3018ece93dcba3333e95db8aeaf43a5a348b",
    "screenshots/06b-tools-menu.png": "4d5dbab4755c81b9634726042aea73cbae6a3ad9a48ba4794a1847062c7fa9f0",
    "screenshots/07-multi-scene-clean-layout.png": "fa75c031b42c28778d4dc6457313751b87efc45c3e2975343d49d34d57299dac",
    "screenshots/08-settings-multiple-scenes-expanded.png": "74c96267542fbbbe71ef036b4184395690f918e7c59cc9dc8b392afa119f2c1f",
    "screenshots/09-settings-scene-1-active.png": "f5926113859b9084201c1dede032d92ec244fb216e620e617f51f6783e91ec4f",
    "screenshots/10-scene-list-tab.png": "ae05b9071793d263e273522d388dc0893a2f16b0d0ddef2e35bfa924f10bcc2e",
    "screenshots/11-hierarchy-node-context.png": "4f3176d22aa5277abe68fdf9b1489cc943033991be03adcc1c97b5ca07b68a74",
    "screenshots/12-delete-confirm-modal.png": "f85d0625b8d2485c8b89a44bfc157de4994d1b6c7f1989b36765ec60646a31ee",
    "screenshots/13-rename-modal.png": "b28153c87aa65ba37d5ed25b4c64bb06064c035fba02a08e00e80c8c60ae7cbc",
    "screenshots/14-duplicate-executed.png": "df4714370d2f70105b80328641e0cb375c5d612a4498bece1f46bb0502231cd7",
    "screenshots/15-edit-menu-undo-duplicate.png": "a20ce0b86f058d7670a74b117d41d44cde3aae4c07ced1d064521af0b8906e6a",
    "screenshots/16-undo-duplicate-executed.png": "b3254d2daaeb20332bb36186061acdd8e2d5334ebd5ca79de6ad615d51351641",
    "screenshots/17-edit-menu-redo-duplicate.png": "593a37ddc239c113e85b7145b037ce14728cab48cce88394fd2d4762109265cd",
    "screenshots/18-scene-row-context.png": "7d4cd0496a2e6fef22fdcae150c70e4c1b0d7b4e0e75f02b0cf6131d1052158a",
    "screenshots/19-add-child-submenu.png": "457b52d3ac23f1e3cd2c41c367efff73ee53622ce2daca8e4a7259974c383214",
}
EXPECTED_UI_RUNTIME_SUPERSEDED_SCREENSHOTS = {
    "screenshots/03-edit-menu.png": "fc3270244948c645847ec5253cdbefe010b5a7048c728f3546b4561a6663042e",
    "screenshots/04-view-menu.png": "edde9a4a0bebfaebebb832c8c7a888e7fe970110e6cba89e66063072cfdc341d",
    "screenshots/05-windows-menu.png": "0954bd901fdc160c703fde617f408cbec2ae1696377e544bc8d3bad2ca457375",
    "screenshots/06-tools-menu.png": "9d88fb104516a6eadb9355ba95ff82b6ac6ffd6190d67015f0a31761e8412f2b",
}
EXPECTED_UI_RUNTIME_SCREENSHOT_METADATA_SHA256 = "9b3eb4037ab1f215615d269a218416a15220f460636e7425d8e3d000c42da244"
EXPECTED_TRACE_REPOSITORY_ARTIFACTS = {
    "docs/editor-parity/reference-traces/D20-HIERARCHY-REPORT.md": "f2c9da0153f4350cd9e2bca6b5569c46f1a4d9d95c58f73ffeae65fb9af812ab",
    "docs/editor-parity/reference-traces/d20-hierarchy-index.json": "b49deaf8a08a378c3162de6166bf3646e1d91d3e52a85c1779a974bac747be91",
    "docs/editor-parity/reference-traces/INPUT-CLIPBOARD-REPORT.md": "75075e053c189993abff5408262e2271fdf0390f5348e9279ed585841816ff33",
    "docs/editor-parity/reference-traces/input-clipboard-index.json": "0f70065eb843ee21589ea774b6efb8d036becbbcab2e796849a4d7169aeb0dcd",
    "docs/editor-parity/reference-traces/UI-RUNTIME-REPORT.md": "f0777adf0a3fa4b05faa314d750be26f56e5be423b5c8d533aee37d4626b9d07",
    "docs/editor-parity/reference-traces/ui-runtime-index.json": "792daa4c8bc9afe83625ec1f8658e19df74600c4c08d280757f43187032d08d8",
    "docs/editor-parity/reference-traces/UI-SEMANTIC-REPORT.md": "052ad4e35c68eca8cf9ae7383371e09440156046ade69adaea0b76d928bb3c50",
    "docs/editor-parity/reference-traces/ui-semantic-index.json": "27281605d0d6e297c3ca5a30fc3c092e57b354928e3fef2f913615addf7c5438",
    "docs/editor-parity/reference-traces/O09-REAL-UI-REPORT.md": "44194eccc13144a9e1f245539910f6bb63978ced5071cb7a02a9f9ecd18759dd",
    "docs/editor-parity/reference-traces/o09-real-ui-index.json": "b30b1d173a4f63733bfdad068e49a91d405763b8f7b6581839905ca34177280e",
}
EXPECTED_UI_SEMANTIC_INDEX = {
    "root": r"G:\ReferenceOracleRuns\vkgr-50373c7b-ui-semantic-20260822T100212Z",
    "reportSha256": "052ad4e35c68eca8cf9ae7383371e09440156046ade69adaea0b76d928bb3c50",
    "indexSha256": "27281605d0d6e297c3ca5a30fc3c092e57b354928e3fef2f913615addf7c5438",
    "nativeCitableCases": 20,
    "diagnosticCases": 21,
    "manifestEntries": 768,
}
EXPECTED_O09_REAL_UI_INDEX = {
    "root": r"G:\ReferenceOracleRuns\vkgr-50373c7b-o09-real-ui-20260822T132518Z",
    "reportSha256": "44194eccc13144a9e1f245539910f6bb63978ced5071cb7a02a9f9ecd18759dd",
    "indexSha256": "b30b1d173a4f63733bfdad068e49a91d405763b8f7b6581839905ca34177280e",
    "externalReportSha256": "7d676c34c16e892ff261d30c158c5b1756eee436bac08a8101a6acb9a2282221",
    "externalIndexSha256": "3c52c80daab9690224666d054c763de428b678e0f3898996d852938a73671b02",
    "artifactIndexSha256": "6aefe5474f64e7a0442d865bf0a4249f31833110aced48543bd845b811700d64",
    "integritySha256": "0448376ae2264562e9484dfa2166b644e7132f0bd0734984a5de57f68f92a86f",
    "finalSealSha256": "df21d2f4e90c022ba23f0d37737a5b5e4dcf90536b0dea5737f4d45b2afd1c69",
    "sha256ManifestSha256": "13943eac4d6ce8ce58ac28f539ff76fe102af0056a8bcb841a5b457a93ab654b",
}
EXPECTED_F39_REPAIR_ARTIFACTS = {
    "docs/editor-parity/F39-REPAIR.md": "c63ae0931b7ad215a3080974bc29289cc4e3bcc89c18f8db52e6379fa6763088",
    "docs/editor-parity/f39-repair-index.json": "23b533ca2872ba258c27f26d00ca697d3804ebdabd419b8c7afbcd828388305b",
}
EXPECTED_F39_REPAIR_DEMO_SHA256 = "789518c264c43d8426345576e43a7c138082eed9c67466c37c8562c5d8e26f1a"
EXPECTED_ORACLE_ARTIFACTS = {
    "O01-same-parent-move-noop": [
        "docs/editor-parity/reference-traces/D20-HIERARCHY-REPORT.md",
        "docs/editor-parity/reference-traces/d20-hierarchy-index.json",
    ],
    "O02-root-to-root-move-noop": [
        "docs/editor-parity/reference-traces/D20-HIERARCHY-REPORT.md",
        "docs/editor-parity/reference-traces/d20-hierarchy-index.json",
    ],
    "O03-reparent-undo-append-order": [
        "docs/editor-parity/reference-traces/D20-HIERARCHY-REPORT.md",
        "docs/editor-parity/reference-traces/d20-hierarchy-index.json",
    ],
    "O04-unique-root-delete": [
        "docs/editor-parity/reference-traces/D20-HIERARCHY-REPORT.md",
        "docs/editor-parity/reference-traces/d20-hierarchy-index.json",
    ],
    "O05-gizmo-escape-only": [
        "docs/editor-parity/reference-traces/INPUT-CLIPBOARD-REPORT.md",
        "docs/editor-parity/reference-traces/input-clipboard-index.json",
        "docs/editor-parity/reference-traces/UI-SEMANTIC-REPORT.md",
        "docs/editor-parity/reference-traces/ui-semantic-index.json",
    ],
    "O06-gizmo-escape-release-same-frame": [
        "docs/editor-parity/reference-traces/INPUT-CLIPBOARD-REPORT.md",
        "docs/editor-parity/reference-traces/input-clipboard-index.json",
        "docs/editor-parity/reference-traces/UI-SEMANTIC-REPORT.md",
        "docs/editor-parity/reference-traces/ui-semantic-index.json",
    ],
    "O07-gizmo-focus-loss": [
        "docs/editor-parity/reference-traces/INPUT-CLIPBOARD-REPORT.md",
        "docs/editor-parity/reference-traces/input-clipboard-index.json",
        "docs/editor-parity/reference-traces/UI-SEMANTIC-REPORT.md",
        "docs/editor-parity/reference-traces/ui-semantic-index.json",
    ],
    "O08-viewport-click-classifier": [
        "docs/editor-parity/reference-traces/INPUT-CLIPBOARD-REPORT.md",
        "docs/editor-parity/reference-traces/input-clipboard-index.json",
        "docs/editor-parity/reference-traces/UI-SEMANTIC-REPORT.md",
        "docs/editor-parity/reference-traces/ui-semantic-index.json",
    ],
    "O09-multi-scene-structural-matrix": [
        "docs/editor-parity/reference-traces/D20-HIERARCHY-REPORT.md",
        "docs/editor-parity/reference-traces/d20-hierarchy-index.json",
        "docs/editor-parity/reference-traces/O09-REAL-UI-REPORT.md",
        "docs/editor-parity/reference-traces/o09-real-ui-index.json",
    ],
    "O10-material-clipboard": [
        "docs/editor-parity/reference-traces/INPUT-CLIPBOARD-REPORT.md",
        "docs/editor-parity/reference-traces/input-clipboard-index.json",
        "docs/editor-parity/reference-traces/UI-SEMANTIC-REPORT.md",
        "docs/editor-parity/reference-traces/ui-semantic-index.json",
    ],
}
EXPECTED_ORACLE_SCAFFOLD_STATUSES = {
    "O01-same-parent-move-noop": "captured",
    "O02-root-to-root-move-noop": "captured",
    "O03-reparent-undo-append-order": "captured",
    "O04-unique-root-delete": "conflict",
    "O05-gizmo-escape-only": "native_partial",
    "O06-gizmo-escape-release-same-frame": "conflict",
    "O07-gizmo-focus-loss": "native_partial",
    "O08-viewport-click-classifier": "native_partial",
    "O09-multi-scene-structural-matrix": "native_partial",
    "O10-material-clipboard": "native_partial",
}
EXPECTED_ORACLE_GATE_STATUSES = EXPECTED_ORACLE_SCAFFOLD_STATUSES
EXPECTED_DEAR_IMGUI_BUILD_IDENTITY_CONTRACT = {
    "dearImGui": {
        "repository": EXPECTED_DEAR_IMGUI_REPOSITORY,
        "pinnedCommit": EXPECTED_DEAR_IMGUI_COMMIT,
        "pinnedTree": EXPECTED_DEAR_IMGUI_TREE,
        "sourceMode": "fetchcontent_git_outside_repository",
        "repositoryCopyAllowed": False,
        "sourceOverrideAllowed": False,
        "targetName": EXPECTED_DEAR_IMGUI_TARGET,
        "compiledCoreSources": list(EXPECTED_DEAR_IMGUI_CORE_SOURCES),
        "targetSourceVariants": EXPECTED_DEAR_IMGUI_TARGET_SOURCE_VARIANTS,
        "compiledSourceVariants": EXPECTED_DEAR_IMGUI_COMPILED_SOURCE_VARIANTS,
        "resolvedIdentityArtifactKind": "mgif.editor-dependency-build-identity",
        "resolvedIdentityRequiredForP1": True,
        "hashProofsRequired": list(DEAR_IMGUI_BUILD_IDENTITY_HASH_REQUIREMENTS),
    }
}
EXPECTED_DEVELOPMENT_LICENSE_POLICY = {
    "status": "development_rights_reserved",
    "selectedSpdxId": None,
    "selectedFile": None,
    "acceptedSha256": None,
    "approvedBy": None,
    "approvedAt": None,
    "developmentPolicy": {
        "distributionStatus": "unlicensed_rights_reserved",
        "engineeringImplementation": "original_clean_room_unless_provenance_ledger_says_otherwise",
        "upstreamApacheSourceCopied": False,
        "preserveThirdPartyNotices": True,
        "p1EngineeringReadinessIndependent": True,
    },
    "releaseGate": {
        "status": "blocked_pending_owner_root_license_selection",
        "blockedFeatureIds": ["F42"],
        "blockedRequirementIds": ["R24"],
        "passClaimAllowed": False,
    },
    "validation": {
        "utf8Required": True,
        "nonWhitespaceRequired": True,
        "minimumBytes": 256,
        "exactAcceptedDigestRequired": True,
    },
}
EXPECTED_D20_POLICY_CONTRACTS = {
    "exact_defect_parity": {
        "schemaVersion": 1,
        "resolution": "exact_defect_parity",
        "sceneRouting": "reference_current_scene_state",
        "snapshotScope": "reference_current_scene_only",
        "occurrenceRouting": "reference_current_occurrence",
        "sharedNodeHandling": "reference_observed",
        "preflightRejections": ["cycle", "multiparent", "stale_occurrence"],
        "rejectionTiming": "reference_observed",
        "traversal": "reference_recursive",
        "uiParitySurface": [
            "entry",
            "label",
            "layout",
            "shortcut",
            "modal",
            "focus",
            "enablement",
            "history_description",
        ],
        "boundOracleIds": ["O09-multi-scene-structural-matrix"],
        "acceptanceIds": [
            "T-EDIT-07",
            "T-UI-15",
            "U01-empty-new-open",
            "U02-hierarchy",
            "V05-delta-edit",
            "V06-topology-swap",
            "P10-T01",
            "P10-T05",
        ],
    },
    "mgif_safety_exception": {
        "schemaVersion": 1,
        "resolution": "mgif_safety_exception",
        "sceneRouting": "clicked_scene_id",
        "snapshotScope": "all_scenes",
        "occurrenceRouting": "clicked_scene_occurrence",
        "sharedNodeHandling": "preserve_shared_node_occurrence_consistency",
        "preflightRejections": ["cycle", "multiparent", "stale_occurrence"],
        "rejectionTiming": "before_command_or_history_entry",
        "traversal": "bounded_iterative",
        "uiParitySurface": [
            "entry",
            "label",
            "layout",
            "shortcut",
            "modal",
            "focus",
            "enablement",
            "history_description",
        ],
        "boundOracleIds": ["O09-multi-scene-structural-matrix"],
        "acceptanceIds": [
            "T-EDIT-07",
            "T-UI-15",
            "U01-empty-new-open",
            "U02-hierarchy",
            "V05-delta-edit",
            "V06-topology-swap",
            "P10-T01",
            "P10-T05",
        ],
    },
}
EXPECTED_O09_READINESS = {
    "status": "eligible_with_debt",
    "coverageBinding": {
        "artifactPath": "docs/editor-parity/reference-traces/d20-hierarchy-index.json",
        "artifactSha256": EXPECTED_TRACE_REPOSITORY_ARTIFACTS[
            "docs/editor-parity/reference-traces/d20-hierarchy-index.json"
        ],
        "matrixCellCount": 298,
        "executedCellCount": 298,
        "notExecutedCellCount": 0,
        "caseIdSetSha256": "948f9a74ce6b47e5af7df988c55a584a907c8614fe3b038daf1bd9d9b4582046",
    },
    "nativeUiBinding": {
        "artifactPath": "docs/editor-parity/reference-traces/o09-real-ui-index.json",
        "artifactSha256": EXPECTED_TRACE_REPOSITORY_ARTIFACTS[
            "docs/editor-parity/reference-traces/o09-real-ui-index.json"
        ],
        "actionPathCells": 298,
        "numericExitCells": 298,
        "fullVisibleHistoryCells": 290,
        "exactSerializedTopologyCells": 7,
        "exactSerializedTopologyCheckpoints": 20,
    },
    "coverageCeiling": {
        "realUiActionPathsCaptured": 298,
        "fullStateUiReplays": 0,
        "privateStateReadbacks": 0,
        "visibleOnlyCurrentSceneCells": 298,
        "visibleOnlySelectionCells": 294,
        "domainMatrixRelabeledAsUiReplays": False,
        "referenceFailuresPreserved": True,
    },
    "evidenceDebt": {
        "uiIds": ["T-UI-15", "U01-empty-new-open", "U02-hierarchy"],
        "vulkanIds": ["V05-delta-edit", "V06-topology-swap"],
        "p10Ids": ["P10-T01", "P10-T05"],
    },
}
EXPECTED_O04_CONFLICT_DISPOSITION = {
    "status": "disposed_with_debt",
    "observedBehavior": "reference_preserves_semantic_document_and_advances_scene_graph_revision_2_3_4_5",
    "targetBehavior": "mgif_preserves_semantic_document_without_revision_advance",
    "disposition": "target_behavior_authoritative",
    "evidenceDebt": {
        "uiIds": ["T-UI-06", "U02-hierarchy"],
        "vulkanIds": ["V05-delta-edit"],
        "p10Ids": ["P10-T01", "P10-T05"],
    },
}
EXPECTED_O06_CONFLICT_DISPOSITION = {
    "status": "disposed_with_debt",
    "observedBehavior": "both_native_orders_clear_selection_retain_transformed_scene_and_leave_undo_disabled",
    "targetBehavior": "mgif_matches_native_visible_result_without_predicted_transform_history_entry",
    "disposition": "observed_behavior_authoritative",
    "evidenceDebt": {
        "uiIds": ["T-UI-13", "U03-transform-gizmo"],
        "vulkanIds": ["V02-gizmo-components", "V07-dpi-resize"],
        "p10Ids": ["P10-T01", "P10-T05"],
    },
}
EXPECTED_NATIVE_PARTIAL_DEBTS = {
    trace_id: {
        "uiIds": ["T-UI-13", "U03-transform-gizmo"],
        "vulkanIds": ["V02-gizmo-components", "V07-dpi-resize"],
        "p10Ids": ["P10-T01", "P10-T05"],
    }
    for trace_id in (
        "O05-gizmo-escape-only",
        "O07-gizmo-focus-loss",
    )
}
EXPECTED_NATIVE_PARTIAL_DEBTS["O08-viewport-click-classifier"] = {
    "uiIds": ["T-UI-03", "U03-transform-gizmo"],
    "vulkanIds": ["V05-delta-edit", "V07-dpi-resize"],
    "p10Ids": ["P10-T01", "P10-T05"],
}
EXPECTED_NATIVE_PARTIAL_DEBTS["O10-material-clipboard"] = {
    "uiIds": ["T-UI-13", "U04-material"],
    "vulkanIds": ["V05-delta-edit"],
    "p10Ids": ["P10-T01", "P10-T05"],
}
EXPECTED_REQUIREMENT_EDGE_CATEGORIES = {
    "R01": {"adjacency", "concurrency", "empty", "idempotency", "ordering"},
    "R02": {"adjacency", "concurrency", "empty", "idempotency", "ordering"},
    "R03": {"adjacency", "concurrency", "empty", "encoding", "idempotency", "ordering"},
    "R04": {"concurrency", "idempotency"},
    "R05": {"adjacency", "concurrency", "empty", "encoding", "idempotency", "ordering"},
    "R06": {"boundary", "concurrency", "idempotency", "precision"},
    "R07": {"boundary", "concurrency", "idempotency", "precision"},
    "R08": {"adjacency", "boundary", "concurrency", "empty", "idempotency", "ordering", "precision"},
    "R09": {"adjacency", "concurrency", "empty", "idempotency", "ordering"},
    "R10": {"adjacency", "boundary", "concurrency", "empty", "idempotency", "ordering", "precision"},
    "R11": {"adjacency", "boundary", "concurrency", "empty", "idempotency", "ordering", "precision"},
    "R12": {"adjacency", "concurrency", "empty", "idempotency", "ordering"},
    "R13": {"adjacency", "boundary", "concurrency", "empty", "idempotency", "ordering", "precision"},
    "R14": {"adjacency", "boundary", "concurrency", "empty", "idempotency", "ordering", "precision"},
    "R15": {"adjacency", "concurrency", "empty", "idempotency", "ordering"},
    "R16": {"concurrency", "idempotency"},
    "R17": {"adjacency", "concurrency", "empty", "idempotency", "ordering"},
    "R18": {"adjacency", "concurrency", "empty", "idempotency", "ordering"},
    "R19": {"adjacency", "concurrency", "empty", "idempotency", "ordering"},
    "R20": {"concurrency", "empty", "encoding", "idempotency"},
    "R22": {"adjacency", "boundary", "concurrency", "empty", "idempotency", "ordering", "precision"},
}
EXPECTED_EDGE_EVIDENCE_IDS = {
    *(f"T-UI-{index:02d}" for index in range(1, 16)),
    *(f"T-EDIT-{index:02d}" for index in range(1, 8)),
    *(f"T-MAT-{index:02d}" for index in range(1, 8)),
    *(f"T-CMD-{index:02d}" for index in range(1, 7)),
    *(f"T-DELTA-{index:02d}" for index in range(1, 6)),
    *(f"T-GIZMO-{index:02d}" for index in range(1, 6)),
    *(f"T-LIFE-{index:02d}" for index in range(1, 6)),
    *(f"T-IO-{index:02d}" for index in range(1, 5)),
    *(f"T-LIGHT-{index:02d}" for index in range(1, 4)),
    *(f"T-MERGE-{index:02d}" for index in range(1, 4)),
    *(f"T-COMPACT-{index:02d}" for index in range(1, 4)),
    *(f"T-PERF-{index:02d}" for index in range(1, 4)),
    *(f"T-RHI-{index:02d}" for index in range(1, 4)),
    *(f"T-DOC-{index:02d}" for index in range(1, 3)),
    *(f"T-SEL-{index:02d}" for index in range(1, 3)),
    *(f"T-TRS-{index:02d}" for index in range(1, 3)),
    *(f"T-CAM-{index:02d}" for index in range(1, 3)),
    "T-ID-01",
    "T-ANIM-01",
    "T-VIS-01",
    "T-LIC-01",
    "U01-empty-new-open",
    "U02-hierarchy",
    "U03-transform-gizmo",
    "U04-material",
    "U05-camera-light-vis",
    "U06-merge-compact-save",
    "U07-modified-failures",
    "U08-large-unicode-dpi",
    "V01-grid",
    "V02-gizmo-components",
    "V03-parent-space",
    "V04-depth-xray",
    "V05-delta-edit",
    "V06-topology-swap",
    "V07-dpi-resize",
    *EXPECTED_ORACLE_IDS,
}


class ValidationError(RuntimeError):
    pass


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


def require(condition: bool, message: str) -> None:
    if not condition:
        raise ValidationError(message)


def resolve_repo_path(root: Path, relative_path: Any, label: str) -> Path:
    require(isinstance(relative_path, str) and relative_path, f"{label} must be a non-empty string")
    require("\\" not in relative_path, f"{label} must use '/' separators")
    pure_path = PurePosixPath(relative_path)
    require(not pure_path.is_absolute(), f"{label} must be relative")
    require(".." not in pure_path.parts, f"{label} must not escape its root")
    require(not (len(relative_path) >= 2 and relative_path[1] == ":"), f"{label} must not contain a drive")
    resolved_root = root.resolve()
    resolved = resolved_root.joinpath(*pure_path.parts).resolve()
    try:
        resolved.relative_to(resolved_root)
    except ValueError as exc:
        raise ValidationError(f"{label} escapes its root") from exc
    return resolved


def normalized_sha256(value: Any, label: str) -> str:
    require(isinstance(value, str) and re.fullmatch(r"[0-9a-fA-F]{64}", value) is not None, f"{label} must be SHA-256")
    return value.lower()


def json_values_equal(actual: Any, expected: Any) -> bool:
    if type(actual) is not type(expected):
        return False
    if isinstance(expected, dict):
        return set(actual) == set(expected) and all(
            json_values_equal(actual[key], expected[key]) for key in expected
        )
    if isinstance(expected, list):
        return len(actual) == len(expected) and all(
            json_values_equal(actual_item, expected_item)
            for actual_item, expected_item in zip(actual, expected)
        )
    return actual == expected


class CMakeBranch(NamedTuple):
    kind: str
    arguments: tuple[str, ...]
    path: Path
    line: int


class CMakeCommand(NamedTuple):
    name: str
    body: str
    path: Path
    line: int
    instance: int = 0
    source_dir: Path | None = None
    variables: tuple[tuple[str, tuple[str, ...]], ...] = ()
    function_scope: bool = False
    branches: tuple[CMakeBranch, ...] = ()


class CMakeArgument(NamedTuple):
    value: str
    quoted: bool = False


def validate_dependency_boundary_policy(manifest: Any) -> None:
    require(isinstance(manifest, dict), "manifest must be an object")
    boundary = manifest.get("dependencyBoundary")
    require(isinstance(boundary, dict), "manifest dependencyBoundary must be an object")
    require(
        boundary == EXPECTED_DEPENDENCY_BOUNDARY,
        "manifest dependencyBoundary differs from locked D21/N11 policy or claims an unapproved exception",
    )


def _cmake_bracket_end(text: str, start: int) -> tuple[int, str] | None:
    match = re.match(r"\[(=*)\[", text[start:])
    if match is None:
        return None
    closing = "]" + match.group(1) + "]"
    content_start = start + match.end()
    closing_start = text.find(closing, content_start)
    if closing_start < 0:
        raise ValidationError("unterminated CMake bracket argument or comment")
    return closing_start + len(closing), text[content_start:closing_start]


def _parse_cmake_commands(path: Path) -> list[CMakeCommand]:
    try:
        text = path.read_text(encoding="utf-8")
    except (OSError, UnicodeError) as exc:
        raise ValidationError(f"cannot read CMake input {path}: {exc}") from exc

    commands: list[CMakeCommand] = []
    index = 0
    while index < len(text):
        if text[index] == "#":
            bracket = _cmake_bracket_end(text, index + 1)
            if bracket is not None:
                index = bracket[0]
            else:
                newline = text.find("\n", index + 1)
                index = len(text) if newline < 0 else newline + 1
            continue
        if not (text[index].isalpha() or text[index] == "_"):
            index += 1
            continue
        name_start = index
        index += 1
        while index < len(text) and (text[index].isalnum() or text[index] == "_"):
            index += 1
        name = text[name_start:index]
        cursor = index
        while cursor < len(text) and text[cursor].isspace():
            cursor += 1
        if cursor >= len(text) or text[cursor] != "(":
            continue

        body_start = cursor + 1
        cursor = body_start
        depth = 1
        quoted = False
        while cursor < len(text) and depth:
            character = text[cursor]
            if quoted:
                if character == "\\" and cursor + 1 < len(text):
                    cursor += 2
                    continue
                if character == '"':
                    quoted = False
                cursor += 1
                continue
            if character == '"':
                quoted = True
                cursor += 1
                continue
            if character == "#":
                bracket = _cmake_bracket_end(text, cursor + 1)
                if bracket is not None:
                    cursor = bracket[0]
                else:
                    newline = text.find("\n", cursor + 1)
                    cursor = len(text) if newline < 0 else newline + 1
                continue
            bracket = _cmake_bracket_end(text, cursor)
            if bracket is not None:
                cursor = bracket[0]
                continue
            if character == "(":
                depth += 1
            elif character == ")":
                depth -= 1
                if depth == 0:
                    commands.append(
                        CMakeCommand(
                            name=name.lower(),
                            body=text[body_start:cursor],
                            path=path,
                            line=text.count("\n", 0, name_start) + 1,
                        )
                    )
                    cursor += 1
                    break
            cursor += 1
        require(depth == 0, f"unterminated CMake command {name} in {path}")
        index = cursor
    return commands


def _tokenize_cmake_arguments(body: str) -> list[CMakeArgument]:
    tokens: list[CMakeArgument] = []
    index = 0
    while index < len(body):
        if body[index].isspace() or body[index] == ";":
            index += 1
            continue
        if body[index] == "#":
            bracket = _cmake_bracket_end(body, index + 1)
            if bracket is not None:
                index = bracket[0]
            else:
                newline = body.find("\n", index + 1)
                index = len(body) if newline < 0 else newline + 1
            continue
        bracket = _cmake_bracket_end(body, index)
        if bracket is not None:
            index, content = bracket
            tokens.append(CMakeArgument(content, True))
            continue
        if body[index] == '"':
            index += 1
            value: list[str] = []
            while index < len(body):
                if body[index] == "\\" and index + 1 < len(body):
                    value.extend((body[index], body[index + 1]))
                    index += 2
                    continue
                if body[index] == '"':
                    index += 1
                    break
                value.append(body[index])
                index += 1
            else:
                raise ValidationError("unterminated quoted CMake argument")
            tokens.append(CMakeArgument("".join(value), True))
            continue
        start = index
        generator_depth = 0
        while index < len(body):
            if body.startswith("$<", index):
                generator_depth += 1
                index += 2
                continue
            if generator_depth and body[index] == ">":
                generator_depth -= 1
                index += 1
                continue
            if generator_depth == 0 and (body[index].isspace() or body[index] == ";"):
                break
            index += 1
        tokens.append(CMakeArgument(body[start:index]))
    return tokens


def _tokenize_cmake(body: str) -> list[str]:
    return [argument.value for argument in _tokenize_cmake_arguments(body)]


def _cmake_files(root: Path) -> list[Path]:
    paths: set[Path] = set()
    top_level = root / "CMakeLists.txt"
    require(top_level.is_file(), "editor dependency validation requires the repository CMakeLists.txt")
    paths.add(top_level.resolve())
    for relative_root in CMAKE_SEARCH_ROOTS:
        search_root = root / relative_root
        if not search_root.exists():
            continue
        try:
            paths.update(path.resolve() for path in search_root.rglob("CMakeLists.txt") if path.is_file())
            paths.update(path.resolve() for path in search_root.rglob("*.cmake") if path.is_file())
        except OSError as exc:
            raise ValidationError(f"cannot enumerate CMake inputs below {search_root}: {exc}") from exc
    return sorted(paths, key=lambda path: path.as_posix().lower())


def _ordered_cmake_commands(
    root: Path,
    paths: list[Path],
    *,
    cache_variables: dict[str, str] | None = None,
) -> list[CMakeCommand]:
    del paths  # Traversal is driven by the repository entry point, as CMake does.
    root = root.resolve()
    parsed_cache: dict[Path, list[CMakeCommand]] = {}
    ordered: list[CMakeCommand] = []
    active_files: set[Path] = set()
    generated_cmake_outputs: set[Path] = set()
    next_instance = 1

    # A value of True is definitely executed, False is unreachable, and None is
    # a conservatively possible branch whose mutations are merged with prior state.
    BranchState = bool | None
    CallableDefinition = tuple[str, tuple[str, ...], tuple[CMakeCommand, ...]]

    def snapshot(variables: dict[str, list[str]]) -> tuple[tuple[str, tuple[str, ...]], ...]:
        return tuple(sorted((name, tuple(values)) for name, values in variables.items()))

    def branch_and(parent: BranchState, child: BranchState) -> BranchState:
        if parent is False or child is False:
            return False
        if parent is True and child is True:
            return True
        return None

    def truth_atom(tokens: list[CMakeArgument], variables: dict[str, list[str]]) -> BranchState:
        if not tokens:
            return False

        # CMake permits parentheses to touch their operands, so normalize only
        # leading/trailing parenthesis runs before parsing. AND and OR have the
        # same precedence in CMake and are evaluated from left to right.
        expression: list[CMakeArgument] = []
        for argument in tokens:
            token = argument.value
            if not argument.quoted and token and set(token) == {"("}:
                expression.extend(CMakeArgument(character) for character in token)
                continue
            if not argument.quoted and token and set(token) == {")"}:
                expression.extend(CMakeArgument(character) for character in token)
                continue
            if argument.quoted:
                expression.append(argument)
                continue
            leading = len(token) - len(token.lstrip("("))
            trailing = len(token) - len(token.rstrip(")"))
            core_end = len(token) - trailing if trailing else len(token)
            core = token[leading:core_end]
            expression.extend(CMakeArgument("(") for _ in range(leading))
            if core:
                if len(core) >= 2 and core.startswith('"') and core.endswith('"'):
                    expression.append(CMakeArgument(core[1:-1], True))
                else:
                    expression.append(CMakeArgument(core))
            expression.extend(CMakeArgument(")") for _ in range(trailing))

        def negate(value: BranchState) -> BranchState:
            return None if value is None else not value

        def combine(left: BranchState, operator: str, right: BranchState) -> BranchState:
            if operator == "AND":
                if left is False or right is False:
                    return False
                if left is True and right is True:
                    return True
                return None
            if left is True or right is True:
                return True
            if left is False and right is False:
                return False
            return None

        def is_keyword(argument: CMakeArgument, value: str) -> bool:
            return not argument.quoted and argument.value.upper() == value

        def condition_value(argument: CMakeArgument) -> str:
            if not argument.quoted and argument.value in variables:
                return ";".join(variables[argument.value])
            return argument.value

        def evaluate_atomic(values: list[CMakeArgument]) -> BranchState:
            if not values:
                return None
            if is_keyword(values[0], "NOT"):
                return negate(evaluate_atomic(values[1:]))
            if len(values) == 2 and is_keyword(values[0], "DEFINED"):
                return values[1].value in variables
            if len(values) == 3 and not values[1].quoted and values[1].value.upper() in {"STREQUAL", "EQUAL"}:
                left = condition_value(values[0])
                right = condition_value(values[2])
                if values[1].value.upper() == "STREQUAL":
                    return left == right
                try:
                    return float(left) == float(right)
                except ValueError:
                    return None
            if len(values) != 1:
                return None
            value = condition_value(values[0])
            upper = value.upper()
            if upper in {"", "0", "FALSE", "OFF", "NO", "N", "IGNORE", "NOTFOUND"} or upper.endswith(
                "-NOTFOUND"
            ):
                return False
            if upper in {"1", "TRUE", "ON", "YES", "Y"}:
                return True
            try:
                return float(value) != 0
            except ValueError:
                return None

        index = 0

        def parse_operand() -> BranchState:
            nonlocal index
            negate_count = 0
            while index < len(expression) and is_keyword(expression[index], "NOT"):
                negate_count += 1
                index += 1
            if index >= len(expression):
                value: BranchState = None
            elif is_keyword(expression[index], "("):
                index += 1
                value = parse_expression()
                if index >= len(expression) or not is_keyword(expression[index], ")"):
                    value = None
                else:
                    index += 1
            else:
                start = index
                while index < len(expression) and not any(
                    is_keyword(expression[index], keyword) for keyword in {"(", ")", "AND", "OR"}
                ):
                    index += 1
                value = evaluate_atomic(expression[start:index])
            for _ in range(negate_count):
                value = negate(value)
            return value

        def parse_expression() -> BranchState:
            nonlocal index
            value = parse_operand()
            while index < len(expression) and any(
                is_keyword(expression[index], keyword) for keyword in {"AND", "OR"}
            ):
                operator = expression[index].value.upper()
                index += 1
                value = combine(value, operator, parse_operand())
            return value

        result = parse_expression()
        return result if index == len(expression) else None

    def resolve_traversal(
        command: CMakeCommand,
        tokens: list[str],
        variables: dict[str, list[str]],
    ) -> tuple[Path | None, Path | None]:
        if not tokens:
            return None, None
        raw_value = tokens[0]
        if raw_value.upper() in {"OPTIONAL", "SYSTEM", "EXCLUDE_FROM_ALL"}:
            return None, None
        value = Path(raw_value.replace("\\", "/"))
        source_dir = command.source_dir or command.path.parent
        candidates: list[Path] = []
        if value.is_absolute():
            candidates.append(value)
        elif command.name == "add_subdirectory":
            candidates.append(source_dir / value)
        else:
            candidates.extend((command.path.parent / value, source_dir / value))
            try:
                relative_source = source_dir.resolve().relative_to(root)
                candidates.append(root / ".cmake-build" / relative_source / value)
            except (OSError, ValueError):
                pass
            for module_dir in variables.get("CMAKE_MODULE_PATH", []):
                module_path = Path(module_dir.replace("\\", "/"))
                if not module_path.is_absolute():
                    module_path = source_dir / module_path
                candidates.append(module_path / value)
            candidates.append(root / "cmake" / value)
        if command.name == "include" and not value.suffix:
            candidates.extend(candidate.with_suffix(".cmake") for candidate in tuple(candidates))
        if command.name == "add_subdirectory":
            candidates = [candidate / "CMakeLists.txt" if candidate.suffix == "" else candidate for candidate in candidates]
        for candidate in candidates:
            try:
                resolved = candidate.resolve()
            except OSError:
                continue
            if not resolved.is_file():
                continue
            try:
                resolved.relative_to(root)
            except ValueError:
                return None, resolved
            return resolved, None
        return None, None

    def generated_path_candidates(command: CMakeCommand, value: str, *, include: bool) -> set[Path]:
        if not value or "${" in value or "$<" in value:
            return set()
        path = Path(value.replace("\\", "/"))
        source_dir = command.source_dir or command.path.parent
        candidates: list[Path] = []
        if path.is_absolute():
            candidates.append(path)
        else:
            candidates.extend((command.path.parent / path, source_dir / path))
            try:
                relative_source = source_dir.resolve().relative_to(root)
                candidates.append(root / ".cmake-build" / relative_source / path)
            except (OSError, ValueError):
                pass
        if include and not path.suffix:
            candidates.extend(candidate.with_suffix(".cmake") for candidate in tuple(candidates))
        resolved: set[Path] = set()
        for candidate in candidates:
            try:
                resolved.add(candidate.resolve())
            except OSError:
                continue
        return resolved

    def generated_output(command: CMakeCommand, tokens: list[str]) -> str | None:
        if command.name == "configure_file":
            return tokens[1] if len(tokens) >= 2 else None
        if command.name != "file" or len(tokens) < 2:
            return None
        operation = tokens[0].upper()
        if operation in {"WRITE", "APPEND"}:
            return tokens[1]
        if operation not in {"GENERATE", "CONFIGURE"}:
            return None
        for index, token in enumerate(tokens[:-1]):
            if token.upper() == "OUTPUT":
                return tokens[index + 1]
        return None

    def definition_at(
        commands: list[CMakeCommand], index: int
    ) -> tuple[CallableDefinition, int]:
        declaration = _tokenize_cmake(commands[index].body)
        require(declaration, f"unnamed CMake {commands[index].name} in {commands[index].path}:{commands[index].line}")
        depth = 1
        cursor = index + 1
        body: list[CMakeCommand] = []
        while cursor < len(commands) and depth:
            nested = commands[cursor]
            if nested.name in {"function", "macro"}:
                depth += 1
            elif nested.name in {"endfunction", "endmacro"}:
                depth -= 1
                if depth == 0:
                    break
            if depth:
                body.append(nested)
            cursor += 1
        require(depth == 0, f"unterminated CMake {commands[index].name} {declaration[0]} in {commands[index].path}")
        return (commands[index].name, tuple(declaration[1:]), tuple(body)), cursor

    def instantiate_body(
        definition: CallableDefinition,
        invocation: CMakeCommand,
    ) -> list[CMakeCommand]:
        kind, parameters, body = definition
        del kind
        arguments = _tokenize_cmake(invocation.body)
        mapping = {
            name: arguments[index] if index < len(arguments) else ""
            for index, name in enumerate(parameters)
        }
        mapping.update({f"ARGV{index}": value for index, value in enumerate(arguments)})
        mapping["ARGC"] = str(len(arguments))
        mapping["ARGV"] = ";".join(arguments)
        mapping["ARGN"] = ";".join(arguments[len(parameters) :])

        def substitute(match: re.Match[str]) -> str:
            return mapping.get(match.group(1), match.group(0))

        return [
            CMakeCommand(
                inner.name,
                _CMAKE_VARIABLE.sub(substitute, inner.body),
                invocation.path,
                invocation.line,
                source_dir=invocation.source_dir,
            )
            for inner in body
        ]

    def block_end(commands: list[CMakeCommand], start: int, opening: str, closing: str) -> int:
        nested_depth = 1
        cursor = start + 1
        while cursor < len(commands) and nested_depth:
            if commands[cursor].name == opening:
                nested_depth += 1
            elif commands[cursor].name == closing:
                nested_depth -= 1
            cursor += 1
        require(
            nested_depth == 0,
            f"unterminated CMake {opening} in {commands[start].path}:{commands[start].line}",
        )
        return cursor - 1

    def unsupported_control(command: CMakeCommand, detail: str, variables: dict[str, list[str]]) -> None:
        nonlocal next_instance
        ordered.append(
            CMakeCommand(
                "cmake_language",
                f"unsupported CMake control flow {detail}",
                command.path,
                command.line,
                next_instance,
                command.source_dir,
                snapshot(variables),
                command.function_scope,
                command.branches,
            )
        )
        next_instance += 1

    def execute_sequence(
        parsed: list[CMakeCommand],
        variables: dict[str, list[str]],
        callables: dict[str, list[CallableDefinition]],
        source_dir: Path,
        list_path: Path,
        state: BranchState = True,
        depth: int = 0,
        parent_variables: dict[str, list[str]] | None = None,
        function_scope: bool = False,
        branch_context: tuple[CMakeBranch, ...] = (),
    ) -> str | None:
        nonlocal next_instance
        require(depth < 32, f"recursive CMake helper/traversal affects editor dependency validation at {list_path}")
        conditionals: list[dict[str, Any]] = []
        loop_variables: list[dict[str, Any]] = []
        conditional_return = False
        current_state = state
        index = 0
        while index < len(parsed):
            original = parsed[index]
            active_branches = branch_context + tuple(frame["branch"] for frame in conditionals)
            command = CMakeCommand(
                original.name,
                original.body,
                list_path,
                original.line,
                next_instance,
                source_dir,
                snapshot(variables),
                function_scope,
                active_branches,
            )
            next_instance += 1

            if command.name == "if":
                tokens, unresolved = _expand_cmake_condition_arguments(command, variables, root)
                condition = None if unresolved else truth_atom(tokens, variables)
                frame = {
                    "parent": current_state,
                    "seen": [condition],
                    "branch": CMakeBranch(
                        "if",
                        tuple(_tokenize_cmake(command.body)),
                        command.path,
                        command.line,
                    ),
                }
                conditionals.append(frame)
                current_state = branch_and(current_state, condition)
                index += 1
                continue
            if command.name in {"elseif", "else"}:
                require(conditionals, f"unmatched CMake {command.name} in {command.path}:{command.line}")
                frame = conditionals[-1]
                seen: list[BranchState] = frame["seen"]
                condition: BranchState = True
                if command.name == "elseif":
                    tokens, unresolved = _expand_cmake_condition_arguments(command, variables, root)
                    condition = None if unresolved else truth_atom(tokens, variables)
                if condition is False or True in seen:
                    branch = False
                elif None in seen:
                    branch = None
                else:
                    branch = condition
                seen.append(branch)
                frame["branch"] = CMakeBranch(
                    command.name,
                    tuple(_tokenize_cmake(command.body)) if command.name == "elseif" else (),
                    command.path,
                    command.line,
                )
                current_state = branch_and(frame["parent"], branch)
                index += 1
                continue
            if command.name == "endif":
                require(conditionals, f"unmatched CMake endif in {command.path}:{command.line}")
                current_state = conditionals.pop()["parent"]
                if conditional_return:
                    current_state = branch_and(current_state, None)
                if loop_variables and loop_variables[-1]["conditional_exit"]:
                    current_state = branch_and(current_state, None)
                index += 1
                continue
            if command.name == "while":
                end = block_end(parsed, index, "while", "endwhile")
                if current_state is False:
                    index = end + 1
                    continue
                condition_tokens, condition_unresolved = _expand_cmake_condition_arguments(command, variables, root)
                condition = None if condition_unresolved else truth_atom(condition_tokens, variables)
                loop_state = branch_and(current_state, condition)
                if loop_state is False:
                    index = end + 1
                    continue
                ordered.append(command)
                unsupported_control(command, "while loop can affect editor dependency validation", variables)
                index = end + 1
                continue
            if command.name == "endwhile":
                raise ValidationError(f"unmatched CMake endwhile in {command.path}:{command.line}")
            if command.name == "endforeach":
                if current_state is False:
                    index += 1
                    continue
                require(loop_variables, f"unmatched CMake endforeach in {command.path}:{command.line}")
                frame = loop_variables.pop()
                if frame["existed"]:
                    variables[frame["name"]] = frame["prior"]
                else:
                    variables.pop(frame["name"], None)
                current_state = frame["parent"]
                index += 1
                continue
            if current_state is False:
                if command.name == "foreach":
                    index = block_end(parsed, index, "foreach", "endforeach") + 1
                    continue
                if command.name in {"function", "macro"}:
                    _, index = definition_at(parsed, index)
                index += 1
                continue

            if command.name == "return":
                ordered.append(command)
                return_tokens, return_unresolved = _expand_cmake_tokens(command, variables, root)
                if return_unresolved:
                    unsupported_control(command, "unresolved return arguments can affect editor dependency validation", variables)
                if return_tokens:
                    if return_tokens[0].upper() != "PROPAGATE" or parent_variables is None:
                        unsupported_control(command, "unsupported return arguments can affect editor dependency validation", variables)
                    else:
                        for name in return_tokens[1:]:
                            values = list(variables.get(name, []))
                            if current_state is None:
                                parent_variables[name] = list(
                                    dict.fromkeys((*parent_variables.get(name, []), *values))
                                )
                            else:
                                parent_variables[name] = values
                if current_state is None:
                    conditional_return = True
                    current_state = False
                    index += 1
                    continue
                return "return"

            if command.name in {"break", "continue"}:
                ordered.append(command)
                if current_state is None:
                    if not loop_variables:
                        unsupported_control(command, f"{command.name} outside a modeled foreach", variables)
                        return command.name
                    loop_variables[-1]["conditional_exit"] = True
                    current_state = False
                    index += 1
                    continue
                if not loop_variables:
                    unsupported_control(command, f"{command.name} outside a modeled foreach", variables)
                    return command.name
                frame = loop_variables.pop()
                if frame["existed"]:
                    variables[frame["name"]] = frame["prior"]
                else:
                    variables.pop(frame["name"], None)
                index = block_end(parsed, index, "foreach", "endforeach") + 1
                current_state = frame["parent"]
                continue

            if command.name in {"function", "macro"}:
                definition, cursor = definition_at(parsed, index)
                declaration = _tokenize_cmake(command.body)
                name = declaration[0].lower()
                if current_state is True:
                    callables[name] = [definition]
                else:
                    callables[name] = list(dict.fromkeys((*callables.get(name, []), definition)))
                ordered.append(command)
                index = cursor + 1
                continue

            tokens, unresolved = _expand_cmake_tokens(command, variables, root)
            definitions = callables.get(command.name)
            if definitions:
                for definition in definitions:
                    local_variables = variables if definition[0] == "macro" else {
                        name: list(values) for name, values in variables.items()
                    }
                    if definition[0] == "function":
                        parameters = definition[1]
                        for argument_index, parameter in enumerate(parameters):
                            local_variables[parameter] = tokens[argument_index : argument_index + 1]
                        for argument_index, argument in enumerate(tokens):
                            local_variables[f"ARGV{argument_index}"] = [argument]
                        local_variables["ARGC"] = [str(len(tokens))]
                        local_variables["ARGV"] = list(tokens)
                        local_variables["ARGN"] = list(tokens[len(parameters) :])
                    control = execute_sequence(
                        instantiate_body(definition, command),
                        local_variables,
                        callables,
                        source_dir,
                        list_path,
                        None if current_state is None or len(definitions) > 1 else True,
                        depth + 1,
                        variables,
                        function_scope or definition[0] == "function",
                        command.branches,
                    )
                    if control in {"break", "continue"}:
                        unsupported_control(
                            command,
                            f"{control} escaped a CMake helper invocation",
                            variables,
                        )
                    if control == "return" and definition[0] == "macro":
                        unsupported_control(
                            command,
                            "return from a macro can escape its invocation scope",
                            variables,
                        )
                index += 1
                continue

            if command.name == "foreach" and tokens:
                known_empty = not unresolved and len(tokens) == 1
                if not unresolved and len(tokens) >= 3 and tokens[1].upper() == "IN":
                    mode = tokens[2].upper()
                    if mode == "LISTS":
                        known_empty = not any(variables.get(name, []) for name in tokens[3:])
                    elif mode == "ITEMS":
                        known_empty = len(tokens) == 3
                if known_empty:
                    loop_depth = 1
                    cursor = index + 1
                    while cursor < len(parsed) and loop_depth:
                        if parsed[cursor].name == "foreach":
                            loop_depth += 1
                        elif parsed[cursor].name == "endforeach":
                            loop_depth -= 1
                        cursor += 1
                    require(loop_depth == 0, f"unterminated CMake foreach in {command.path}:{command.line}")
                    index = cursor
                    continue

            ordered.append(command)
            if command.name == "foreach" and tokens:
                loop_values: list[str] = []
                if not unresolved and len(tokens) > 1:
                    if tokens[1].upper() == "IN" and len(tokens) >= 3:
                        mode = tokens[2].upper()
                        if mode == "LISTS":
                            for variable_name in tokens[3:]:
                                loop_values.extend(variables.get(variable_name, []))
                        elif mode == "ITEMS":
                            loop_values.extend(tokens[3:])
                    elif tokens[1].upper() == "RANGE":
                        try:
                            bounds = [int(value) for value in tokens[2:]]
                            if len(bounds) == 1:
                                start, stop, step = 0, bounds[0], 1
                            elif len(bounds) == 2:
                                start, stop, step = bounds[0], bounds[1], 1
                            else:
                                start, stop, step = bounds[:3]
                            loop_values.extend(str(value) for value in range(start, stop + (1 if step > 0 else -1), step))
                        except (ValueError, ZeroDivisionError):
                            loop_values = []
                    else:
                        loop_values.extend(tokens[1:])
                if not loop_values:
                    loop_values = [f"${{CMAKE_FOREACH_UNKNOWN_{tokens[0]}}}"]
                loop_variables.append(
                    {
                        "name": tokens[0],
                        "existed": tokens[0] in variables,
                        "prior": list(variables.get(tokens[0], [])),
                        "parent": current_state,
                        "conditional_exit": False,
                    }
                )
                if current_state is None:
                    variables[tokens[0]] = list(
                        dict.fromkeys((*variables.get(tokens[0], []), *loop_values))
                    )
                else:
                    variables[tokens[0]] = loop_values
            _update_cmake_variables(
                command,
                tokens,
                variables,
                conditional=current_state is None,
                parent_variables=parent_variables,
                root=root,
            )
            output = generated_output(command, tokens)
            if output is not None:
                generated_cmake_outputs.update(generated_path_candidates(command, output, include=False))
            if command.name in {"include", "add_subdirectory"} and tokens:
                traversal_candidates = generated_path_candidates(
                    command,
                    tokens[0],
                    include=command.name == "include",
                )
                if command.name == "add_subdirectory":
                    traversal_candidates = {
                        candidate / "CMakeLists.txt" if candidate.suffix == "" else candidate
                        for candidate in traversal_candidates
                    }
                if traversal_candidates & generated_cmake_outputs:
                    ordered.append(
                        CMakeCommand(
                            "cmake_language",
                            f"unsupported generated CMake traversal {tokens[0]}",
                            command.path,
                            command.line,
                            next_instance,
                            source_dir,
                            snapshot(variables),
                            function_scope,
                            command.branches,
                        )
                    )
                    next_instance += 1
            if command.name in {"include", "add_subdirectory"} and not unresolved:
                referenced, external = resolve_traversal(command, tokens, variables)
                if external is not None:
                    ordered.append(
                        CMakeCommand(
                            "cmake_language",
                            f"unsupported external CMake traversal {external.as_posix()}",
                            command.path,
                            command.line,
                            next_instance,
                            root,
                            snapshot(variables),
                            function_scope,
                            command.branches,
                        )
                    )
                    next_instance += 1
                if referenced is not None:
                    if referenced in active_files:
                        ordered.append(
                            CMakeCommand(
                                "cmake_language",
                                f"unsupported recursive repository include {referenced.as_posix()}",
                                command.path,
                                command.line,
                                next_instance,
                                source_dir,
                                snapshot(variables),
                                function_scope,
                                command.branches,
                            )
                        )
                        next_instance += 1
                    else:
                        child_variables = variables
                        child_callables = callables
                        child_source_dir = source_dir
                        child_parent_variables = parent_variables
                        if command.name == "add_subdirectory":
                            child_variables = {name: list(values) for name, values in variables.items()}
                            child_callables = {name: list(values) for name, values in callables.items()}
                            child_source_dir = referenced.parent
                            child_parent_variables = variables
                        active_files.add(referenced)
                        child_parsed = parsed_cache.setdefault(referenced, _parse_cmake_commands(referenced))
                        execute_sequence(
                            child_parsed,
                            child_variables,
                            child_callables,
                            child_source_dir,
                            referenced,
                            current_state,
                            depth + 1,
                            child_parent_variables,
                            function_scope,
                            command.branches,
                        )
                        active_files.remove(referenced)
            index += 1
        require(not conditionals, f"unterminated CMake conditional in {list_path}")
        require(not loop_variables, f"unterminated CMake foreach in {list_path}")
        return None

    top_level = (root / "CMakeLists.txt").resolve()
    parsed_cache[top_level] = _parse_cmake_commands(top_level)
    active_files.add(top_level)
    initial_variables = {name: [value] for name, value in (cache_variables or {}).items()}
    execute_sequence(parsed_cache[top_level], initial_variables, {}, root, top_level)
    active_files.remove(top_level)
    return ordered


def _update_cmake_variables(
    command: CMakeCommand,
    tokens: list[str],
    variables: dict[str, list[str]],
    *,
    conditional: bool = False,
    parent_variables: dict[str, list[str]] | None = None,
    root: Path | None = None,
) -> None:
    def assign(name: str, values: list[str], target: dict[str, list[str]] | None = None) -> None:
        destination = variables if target is None else target
        if not conditional:
            destination[name] = values
            return
        destination[name] = list(dict.fromkeys((*destination.get(name, []), *values)))

    def invalidate_existing(name: str) -> None:
        if name in variables:
            assign(name, [f"${{MGIF_UNSUPPORTED_CMAKE_MUTATION_{name}}}"])

    if not tokens:
        return
    if command.name == "set":
        values: list[str] = []
        parent_scope = False
        cache_assignment = False
        force_cache = False
        for token in tokens[1:]:
            if token.upper() == "PARENT_SCOPE":
                parent_scope = True
                break
            if token.upper() == "CACHE":
                cache_assignment = True
                break
            force_cache = force_cache or token.upper() == "FORCE"
            values.append(token)
        force_cache = force_cache or any(token.upper() == "FORCE" for token in tokens)
        if parent_scope and parent_variables is not None:
            assign(tokens[0], values, parent_variables)
        elif cache_assignment and tokens[0] in variables and not force_cache:
            return
        elif not parent_scope:
            assign(tokens[0], values)
        return
    if command.name == "unset":
        if len(tokens) >= 2 and tokens[1].upper() == "PARENT_SCOPE" and parent_variables is not None:
            if not conditional:
                parent_variables.pop(tokens[0], None)
        elif not conditional:
            variables.pop(tokens[0], None)
        return
    if command.name == "option" and tokens[0] not in variables:
        variables[tokens[0]] = [tokens[2] if len(tokens) >= 3 else "OFF"]
        return
    if command.name == "file" and len(tokens) >= 3 and tokens[0].upper() in {"GLOB", "GLOB_RECURSE"}:
        operation = tokens[0].upper()
        name = tokens[1]
        patterns: list[str] = []
        skip_next = False
        for token in tokens[2:]:
            if skip_next:
                skip_next = False
                continue
            if token.upper() in {"LIST_DIRECTORIES", "RELATIVE"}:
                skip_next = True
                continue
            if token.upper() == "CONFIGURE_DEPENDS":
                continue
            patterns.append(token)
        matches: list[str] = []
        source_dir = command.source_dir or command.path.parent
        for pattern in patterns:
            candidate = Path(pattern.replace("\\", "/"))
            if not candidate.is_absolute():
                candidate = source_dir / candidate
            matches.extend(
                Path(path).resolve().as_posix()
                for path in glob.glob(str(candidate), recursive=operation == "GLOB_RECURSE")
            )
        assign(name, sorted(set(matches)))
        return
    if command.name == "file" and len(tokens) >= 2:
        operation = tokens[0].upper()
        output_name: str | None = None
        if operation in {"READ", "STRINGS", "HASH", "SIZE", "READ_SYMLINK", "TIMESTAMP", "REAL_PATH"}:
            output_name = tokens[2] if len(tokens) >= 3 else None
        elif operation in {"RELATIVE_PATH", "TO_CMAKE_PATH", "TO_NATIVE_PATH"}:
            output_name = tokens[1]
        if output_name:
            invalidate_existing(output_name)
        return
    if command.name == "get_filename_component" and len(tokens) >= 3:
        values = [Path(token.replace("\\", "/")) for token in tokens[1:-1]]
        operation = tokens[-1].upper()
        if operation == "NAME":
            assign(tokens[0], [value.name for value in values])
        elif operation == "DIRECTORY":
            assign(tokens[0], [value.parent.as_posix() for value in values])
        elif operation == "NAME_WE":
            assign(tokens[0], [value.stem for value in values])
        elif operation == "EXT":
            assign(tokens[0], [value.suffix for value in values])
        else:
            assign(tokens[0], [f"${{MGIF_UNSUPPORTED_CMAKE_MUTATION_{tokens[0]}}}"])
        return
    if command.name in {"find_file", "find_library", "find_path", "find_program"}:
        invalidate_existing(tokens[0])
        return
    if command.name in {
        "get_cmake_property",
        "get_directory_property",
        "get_property",
        "get_source_file_property",
        "get_target_property",
    }:
        invalidate_existing(tokens[0])
        return
    if command.name == "get_test_property" and len(tokens) >= 3:
        invalidate_existing(tokens[2])
        return
    if command.name == "math" and len(tokens) >= 2:
        invalidate_existing(tokens[1])
        return
    if command.name == "execute_process":
        for index, token in enumerate(tokens[:-1]):
            if token.upper() in {"OUTPUT_VARIABLE", "ERROR_VARIABLE", "RESULT_VARIABLE", "RESULTS_VARIABLE"}:
                output_name = tokens[index + 1]
                invalidate_existing(output_name)
        return
    if command.name == "cmake_parse_arguments" and root is not None:
        raw = _tokenize_cmake(command.body)
        if len(raw) < 5:
            return

        def expand_argument(value: str) -> list[str]:
            expanded, _ = _expand_cmake_value(value, variables, command, root)
            return expanded.split(";") if expanded else []

        prefix_values = expand_argument(raw[0])
        if not prefix_values:
            return
        prefix = prefix_values[0]
        option_names = set(expand_argument(raw[1]))
        one_value_names = set(expand_argument(raw[2]))
        multi_value_names = set(expand_argument(raw[3]))
        arguments = [part for value in raw[4:] for part in expand_argument(value)]
        keywords = option_names | one_value_names | multi_value_names
        parsed: dict[str, list[str]] = {name: [] for name in keywords}
        unparsed: list[str] = []
        index = 0
        while index < len(arguments):
            name = arguments[index]
            if name in option_names:
                parsed[name] = ["TRUE"]
                index += 1
                continue
            if name in one_value_names:
                parsed[name] = arguments[index + 1 : index + 2]
                index += 2
                continue
            if name in multi_value_names:
                index += 1
                start = index
                while index < len(arguments) and arguments[index] not in keywords:
                    index += 1
                parsed[name] = arguments[start:index]
                continue
            unparsed.append(name)
            index += 1
        for name in option_names:
            parsed.setdefault(name, ["FALSE"])
            if not parsed[name]:
                parsed[name] = ["FALSE"]
        for name, values in parsed.items():
            assign(f"{prefix}_{name}", values)
        assign(f"{prefix}_UNPARSED_ARGUMENTS", unparsed)
        return
    if command.name == "list" and len(tokens) >= 2:
        operation = tokens[0].upper()
        name = tokens[1]
        if operation == "APPEND":
            assign(name, [*variables.get(name, []), *tokens[2:]])
        elif operation == "PREPEND":
            assign(name, [*tokens[2:], *variables.get(name, [])])
        elif operation == "JOIN" and len(tokens) >= 4:
            assign(tokens[3], [tokens[2].join(variables.get(name, []))])
        elif operation in {"POP_BACK", "POP_FRONT"}:
            current = list(variables.get(name, []))
            output_names = tokens[2:]
            pop_count = max(1, len(output_names))
            popped: list[str] = []
            valid = len(current) >= pop_count
            if valid:
                for _ in range(pop_count):
                    popped.append(current.pop(-1 if operation == "POP_BACK" else 0))
                assign(name, current)
                for output_name, value in zip(output_names, popped):
                    assign(output_name, [value])
            else:
                for output_name in (name, *output_names):
                    assign(output_name, [f"${{MGIF_UNSUPPORTED_CMAKE_MUTATION_{output_name}}}"])
        elif operation == "FILTER" and len(tokens) >= 5 and tokens[3].upper() == "REGEX":
            try:
                pattern = re.compile(tokens[4])
                include_matches = tokens[2].upper() == "INCLUDE"
                require(tokens[2].upper() in {"INCLUDE", "EXCLUDE"}, "unsupported CMake list FILTER mode")
                assign(
                    name,
                    [value for value in variables.get(name, []) if bool(pattern.search(value)) == include_matches],
                )
            except (re.error, ValidationError):
                assign(name, [f"${{MGIF_UNSUPPORTED_CMAKE_MUTATION_{name}}}"])
        elif operation == "FIND" and len(tokens) >= 4:
            try:
                position = variables.get(name, []).index(tokens[2])
            except ValueError:
                position = -1
            assign(tokens[3], [str(position)])
        elif operation == "GET" and len(tokens) >= 4:
            output_name = tokens[-1]
            try:
                assign(output_name, [variables.get(name, [])[int(index)] for index in tokens[2:-1]])
            except (IndexError, ValueError):
                assign(output_name, [f"${{MGIF_UNSUPPORTED_CMAKE_MUTATION_{output_name}}}"])
        elif operation == "INSERT" and len(tokens) >= 4:
            try:
                current = list(variables.get(name, []))
                index = int(tokens[2])
                if index < 0:
                    index += len(current) + 1
                if index < 0 or index > len(current):
                    raise IndexError
                assign(name, [*current[:index], *tokens[3:], *current[index:]])
            except (IndexError, ValueError):
                assign(name, [f"${{MGIF_UNSUPPORTED_CMAKE_MUTATION_{name}}}"])
        elif operation == "LENGTH" and len(tokens) >= 3:
            assign(tokens[2], [str(len(variables.get(name, [])))])
        elif operation == "REMOVE_AT" and len(tokens) >= 3:
            try:
                current = list(variables.get(name, []))
                indices = {int(value) % len(current) for value in tokens[2:]}
                assign(name, [value for index, value in enumerate(current) if index not in indices])
            except (ValueError, ZeroDivisionError):
                assign(name, [f"${{MGIF_UNSUPPORTED_CMAKE_MUTATION_{name}}}"])
        elif operation == "REMOVE_DUPLICATES":
            assign(name, list(dict.fromkeys(variables.get(name, []))))
        elif operation == "REMOVE_ITEM":
            removed = set(tokens[2:])
            assign(name, [value for value in variables.get(name, []) if value not in removed])
        elif operation == "REVERSE":
            assign(name, list(reversed(variables.get(name, []))))
        elif operation == "SORT" and len(tokens) == 2:
            assign(name, sorted(variables.get(name, [])))
        elif operation == "SUBLIST" and len(tokens) >= 5:
            output_name = tokens[4]
            try:
                start = int(tokens[2])
                length = int(tokens[3])
                current = variables.get(name, [])
                assign(output_name, current[start:] if length == -1 else current[start : start + length])
            except ValueError:
                assign(output_name, [f"${{MGIF_UNSUPPORTED_CMAKE_MUTATION_{output_name}}}"])
        elif operation in {"SORT", "TRANSFORM"}:
            destinations = {name}
            if operation == "TRANSFORM":
                for index, token in enumerate(tokens[:-1]):
                    if token.upper() == "OUTPUT_VARIABLE":
                        destinations.add(tokens[index + 1])
            for destination in destinations:
                assign(destination, [f"${{MGIF_UNSUPPORTED_CMAKE_MUTATION_{destination}}}"])
        return
    if command.name != "string" or len(tokens) < 2:
        return
    operation = tokens[0].upper()
    if operation in {"APPEND", "PREPEND"} and len(tokens) >= 3:
        name = tokens[1]
        addition = "".join(tokens[2:])
        current = "".join(variables.get(name, []))
        assign(name, [current + addition if operation == "APPEND" else addition + current])
    elif operation == "CONCAT" and len(tokens) >= 3:
        assign(tokens[1], ["".join(tokens[2:])])
    elif operation == "JOIN" and len(tokens) >= 4:
        assign(tokens[2], [tokens[1].join(tokens[3:])])
    elif operation == "REPLACE" and len(tokens) >= 5:
        assign(tokens[3], ["".join(tokens[4:]).replace(tokens[1], tokens[2])])
    elif operation == "ASCII" and len(tokens) >= 3:
        output_name = tokens[-1]
        try:
            code_points = [int(value, 10) for value in tokens[1:-1]]
            if not all(0 <= value <= 255 for value in code_points):
                raise ValueError
            assign(output_name, ["".join(chr(value) for value in code_points)])
        except ValueError:
            assign(output_name, [f"${{MGIF_UNSUPPORTED_CMAKE_MUTATION_{output_name}}}"])
    else:
        destination_index: int | None = None
        if operation == "REGEX" and len(tokens) >= 4:
            destination_index = 4 if tokens[1].upper() == "REPLACE" else 3
        elif operation == "FIND" and len(tokens) >= 4:
            destination_index = 3
        elif operation in {
            "CONFIGURE",
            "GENEX_STRIP",
            "HEX",
            "LENGTH",
            "MAKE_C_IDENTIFIER",
            "STRIP",
            "TOLOWER",
            "TOUPPER",
        } and len(tokens) >= 3:
            destination_index = 2
        elif operation == "SUBSTRING" and len(tokens) >= 5:
            destination_index = 4
        elif operation == "REPEAT" and len(tokens) >= 4:
            destination_index = 3
        elif operation == "COMPARE" and len(tokens) >= 5:
            destination_index = 4
        elif operation in {"MD5", "SHA1", "SHA224", "SHA256", "SHA384", "SHA512", "TIMESTAMP", "UUID", "JSON"}:
            destination_index = 1
        elif operation == "RANDOM":
            destination_index = len(tokens) - 1
        if destination_index is not None and destination_index < len(tokens):
            output_name = tokens[destination_index]
            assign(output_name, [f"${{MGIF_UNSUPPORTED_CMAKE_MUTATION_{output_name}}}"])


_CMAKE_VARIABLE = re.compile(r"\$\{([A-Za-z_][A-Za-z0-9_]*)\}")
_CMAKE_EXTERNAL_VARIABLE = re.compile(r"\$(?:ENV|CACHE)\{([^}]+)\}")
_CMAKE_COMPILE_FLAG_VARIABLE = re.compile(
    r"CMAKE_(?:C|CXX)_FLAGS(?:_[A-Za-z0-9_]+)?",
    re.IGNORECASE,
)
_CMAKE_ENV_COMPILE_FLAG_VARIABLE = re.compile(
    r"ENV\{(?:CFLAGS|CXXFLAGS|CPPFLAGS|CL|_CL_)\}",
    re.IGNORECASE,
)


def _is_cmake_compile_flag_variable(value: str) -> bool:
    return (
        _CMAKE_COMPILE_FLAG_VARIABLE.fullmatch(value) is not None
        or _CMAKE_ENV_COMPILE_FLAG_VARIABLE.fullmatch(value) is not None
    )


def _compile_flag_mutations(
    command: CMakeCommand,
    tokens: list[str],
    root: Path,
) -> tuple[dict[str, list[str]], set[str]]:
    """Return computed global flag outputs and unsupported flag destinations."""
    if not tokens:
        return {}, set()

    output_names: set[str] = set()
    unsupported: set[str] = set()
    supported = False
    if command.name == "set":
        output_names.add(tokens[0])
        supported = True
    elif command.name == "set_property" and len(tokens) >= 5 and tokens[0].upper() == "CACHE":
        try:
            property_index = next(index for index, value in enumerate(tokens) if value.upper() == "PROPERTY")
        except StopIteration:
            property_index = -1
        cache_names = tokens[1:property_index] if property_index >= 0 else []
        if (
            property_index >= 0
            and property_index + 1 < len(tokens)
            and tokens[property_index + 1].upper() == "VALUE"
        ):
            for name in cache_names:
                if _is_cmake_compile_flag_variable(name):
                    output_names.add(name)
            supported = True
        else:
            unsupported.update(name for name in cache_names if _is_cmake_compile_flag_variable(name))
    elif command.name == "list" and len(tokens) >= 2:
        operation = tokens[0].upper()
        if operation in {"APPEND", "PREPEND"}:
            output_names.add(tokens[1])
            supported = True
        elif operation == "JOIN" and len(tokens) >= 4:
            output_names.add(tokens[3])
            supported = True
        else:
            candidate_indices: list[int] = []
            if operation in {
                "FILTER",
                "INSERT",
                "POP_BACK",
                "POP_FRONT",
                "REMOVE_AT",
                "REMOVE_DUPLICATES",
                "REMOVE_ITEM",
                "REVERSE",
                "SORT",
            }:
                candidate_indices.append(1)
            elif operation == "LENGTH" and len(tokens) >= 3:
                candidate_indices.append(2)
            elif operation == "FIND" and len(tokens) >= 4:
                candidate_indices.append(3)
            elif operation == "GET" and len(tokens) >= 3:
                candidate_indices.append(len(tokens) - 1)
            elif operation == "SUBLIST" and len(tokens) >= 5:
                candidate_indices.append(4)
            elif operation == "TRANSFORM":
                candidate_indices.append(1)
                for index, token in enumerate(tokens[:-1]):
                    if token.upper() == "OUTPUT_VARIABLE":
                        candidate_indices.append(index + 1)
            unsupported.update(
                tokens[index]
                for index in candidate_indices
                if index < len(tokens) and _is_cmake_compile_flag_variable(tokens[index])
            )
    elif command.name == "string" and len(tokens) >= 2:
        operation = tokens[0].upper()
        destination_index: int | None = None
        if operation in {"APPEND", "PREPEND", "CONCAT"}:
            destination_index = 1
        elif operation == "JOIN" and len(tokens) >= 3:
            destination_index = 2
        elif operation == "REPLACE" and len(tokens) >= 4:
            destination_index = 3
        elif operation == "ASCII" and len(tokens) >= 3:
            destination_index = len(tokens) - 1
        if destination_index is not None and destination_index < len(tokens):
            output_names.add(tokens[destination_index])
            supported = True
        else:
            candidate_indices: list[int] = []
            if operation == "REGEX" and len(tokens) >= 4:
                candidate_indices.append(4 if tokens[1].upper() == "REPLACE" else 3)
            elif operation == "FIND" and len(tokens) >= 4:
                candidate_indices.append(3)
            elif operation in {
                "CONFIGURE",
                "GENEX_STRIP",
                "HEX",
                "LENGTH",
                "MAKE_C_IDENTIFIER",
                "STRIP",
                "TOLOWER",
                "TOUPPER",
            } and len(tokens) >= 3:
                candidate_indices.append(2)
            elif operation == "SUBSTRING" and len(tokens) >= 5:
                candidate_indices.append(4)
            elif operation == "REPEAT" and len(tokens) >= 4:
                candidate_indices.append(3)
            elif operation == "COMPARE" and len(tokens) >= 5:
                candidate_indices.append(4)
            elif operation in {
                "MD5",
                "RANDOM",
                "SHA1",
                "SHA224",
                "SHA256",
                "SHA384",
                "SHA512",
                "TIMESTAMP",
                "UUID",
                "JSON",
            }:
                candidate_indices.append(1 if operation != "RANDOM" else len(tokens) - 1)
            unsupported.update(
                tokens[index]
                for index in candidate_indices
                if index < len(tokens) and _is_cmake_compile_flag_variable(tokens[index])
            )
    elif command.name == "cmake_parse_arguments":
        supported = True
    elif command.name == "return" and tokens[0].upper() == "PROPAGATE":
        unsupported.update(
            name for name in tokens[1:] if _is_cmake_compile_flag_variable(name)
        )
    elif command.name in {
        "execute_process",
        "file",
        "find_file",
        "find_library",
        "find_path",
        "find_program",
        "get_cmake_property",
        "get_directory_property",
        "get_filename_component",
        "get_property",
        "get_source_file_property",
        "get_target_property",
        "get_test_property",
        "math",
    }:
        unsupported.update(token for token in tokens if _is_cmake_compile_flag_variable(token))

    before = {name: list(values) for name, values in command.variables}
    after = {name: list(values) for name, values in before.items()}
    if supported:
        _update_cmake_variables(command, tokens, after, root=root)
        output_names.update(
            name
            for name in before.keys() | after.keys()
            if before.get(name) != after.get(name)
        )

    mutations: dict[str, list[str]] = {}
    for name in output_names:
        if not _is_cmake_compile_flag_variable(name):
            continue
        values = list(after.get(name, []))
        if command.name == "set":
            values = []
            for token in tokens[1:]:
                if token.upper() in {"CACHE", "PARENT_SCOPE"}:
                    break
                values.append(token)
        elif command.name == "set_property":
            property_index = next(index for index, value in enumerate(tokens) if value.upper() == "PROPERTY")
            values = tokens[property_index + 2 :]
        mutations[name] = values
    return mutations, unsupported


def _generator_end(value: str, start: int) -> int | None:
    depth = 1
    index = start + 2
    while index < len(value):
        if value.startswith("$<", index):
            depth += 1
            index += 2
            continue
        if value[index] == ">":
            depth -= 1
            if depth == 0:
                return index
        index += 1
    return None


def _split_generator(value: str, separator: str) -> list[str]:
    parts: list[str] = []
    start = 0
    depth = 0
    index = 0
    while index < len(value):
        if value.startswith("$<", index):
            depth += 1
            index += 2
            continue
        if depth and value[index] == ">":
            depth -= 1
        elif depth == 0 and value[index] == separator:
            parts.append(value[start:index])
            start = index + 1
        index += 1
    parts.append(value[start:])
    return parts


def _expand_generator_expressions(value: str) -> tuple[str, set[str]]:
    unresolved: set[str] = set()
    for _ in range(32):
        start = value.rfind("$<")
        if start < 0:
            return value, unresolved
        end = _generator_end(value, start)
        if end is None:
            unresolved.add("unterminated-generator-expression")
            return value, unresolved
        inner = value[start + 2 : end]
        fields = _split_generator(inner, ":")
        operation = fields[0].upper()
        payload = ":".join(fields[1:]) if len(fields) > 1 else ""
        replacement = payload
        if operation == "JOIN":
            arguments = _split_generator(payload, ",")
            if len(arguments) != 2:
                unresolved.add("generator-expression:JOIN")
            else:
                replacement = arguments[1].join(arguments[0].split(";"))
        elif operation == "IF":
            arguments = _split_generator(payload, ",")
            if len(arguments) != 3:
                unresolved.add("generator-expression:IF")
            elif arguments[0].upper() in {"0", "FALSE", "OFF", "NO", "N", "IGNORE", "NOTFOUND", ""}:
                replacement = arguments[2]
            elif arguments[0].upper() in {"1", "TRUE", "ON", "YES", "Y"}:
                replacement = arguments[1]
            else:
                replacement = f"{arguments[1]};{arguments[2]}"
        elif operation == "BOOL":
            replacement = (
                "0"
                if payload.upper() in {"0", "FALSE", "OFF", "NO", "N", "IGNORE", "NOTFOUND", ""}
                else "1"
            )
        elif operation in {
            "BUILD_INTERFACE",
            "INSTALL_INTERFACE",
            "LINK_ONLY",
            "DEVICE_LINK",
            "HOST_LINK",
            "GENEX_EVAL",
        }:
            replacement = payload
        elif operation in {
            "TARGET_BUNDLE_CONTENT_DIR",
            "TARGET_BUNDLE_DIR",
            "TARGET_FILE",
            "TARGET_FILE_BASE_NAME",
            "TARGET_FILE_DIR",
            "TARGET_FILE_NAME",
            "TARGET_LINKER_FILE",
            "TARGET_LINKER_FILE_BASE_NAME",
            "TARGET_LINKER_FILE_DIR",
            "TARGET_LINKER_FILE_NAME",
            "TARGET_NAME_IF_EXISTS",
            "TARGET_OBJECTS",
            "TARGET_PDB_FILE",
            "TARGET_PDB_FILE_BASE_NAME",
            "TARGET_PDB_FILE_DIR",
            "TARGET_PDB_FILE_NAME",
            "TARGET_SONAME_FILE",
            "TARGET_SONAME_FILE_DIR",
            "TARGET_SONAME_FILE_NAME",
        }:
            replacement = payload
        elif operation == "LOWER_CASE":
            replacement = payload.lower()
        elif operation == "UPPER_CASE":
            replacement = payload.upper()
        elif operation in {"CONFIG", "COMPILE_LANGUAGE", "COMPILE_LANG_AND_ID", "LINK_LANGUAGE", "PLATFORM_ID"}:
            replacement = "1" if payload else f"<{operation}>"
        elif operation == "SEMICOLON":
            replacement = ";"
        elif operation == "COMMA":
            replacement = ","
        elif operation == "ANGLE-R":
            replacement = ">"
        elif operation in {"0", "FALSE", "OFF"}:
            replacement = ""
        elif operation in {"1", "TRUE", "ON"}:
            replacement = payload
        else:
            unresolved.add(f"generator-expression:{operation or 'unknown'}")
        value = value[:start] + replacement + value[end + 1 :]
    unresolved.add("recursive-generator-expression")
    return value, unresolved


def _expand_cmake_value(
    value: str,
    variables: dict[str, list[str]],
    command: CMakeCommand,
    root: Path,
) -> tuple[str, set[str]]:
    unresolved: set[str] = set()
    source_dir = command.source_dir or command.path.parent
    builtins = {
        "CMAKE_CURRENT_LIST_DIR": command.path.parent.as_posix(),
        "CMAKE_CURRENT_SOURCE_DIR": source_dir.as_posix(),
        "CMAKE_CURRENT_LIST_FILE": command.path.as_posix(),
        "CMAKE_SOURCE_DIR": root.as_posix(),
        "PROJECT_SOURCE_DIR": root.as_posix(),
        "CMAKE_CURRENT_BINARY_DIR": (root / ".cmake-build" / source_dir.relative_to(root)).as_posix(),
        "CMAKE_BINARY_DIR": (root / ".cmake-build").as_posix(),
    }

    def replace(match: re.Match[str]) -> str:
        name = match.group(1)
        if name in builtins:
            return builtins[name]
        if name in variables:
            return ";".join(variables[name])
        unresolved.add(name)
        return match.group(0)

    expanded = value
    for _ in range(16):
        updated = _CMAKE_VARIABLE.sub(replace, expanded)
        if updated == expanded:
            break
        expanded = updated
    else:
        unresolved.add("recursive-variable-expansion")
    expanded, generator_unresolved = _expand_generator_expressions(expanded)
    unresolved.update(generator_unresolved)
    unresolved.update(match.group(1) for match in _CMAKE_EXTERNAL_VARIABLE.finditer(expanded))
    return expanded, unresolved


def _expand_cmake_arguments(
    command: CMakeCommand,
    variables: dict[str, list[str]],
    root: Path,
) -> tuple[list[CMakeArgument], set[str]]:
    expanded: list[CMakeArgument] = []
    unresolved: set[str] = set()
    for argument in _tokenize_cmake_arguments(command.body):
        value, missing = _expand_cmake_value(argument.value, variables, command, root)
        parts = [part for part in value.split(";") if part]
        if parts:
            expanded.extend(CMakeArgument(part, argument.quoted) for part in parts)
        elif argument.quoted:
            expanded.append(CMakeArgument("", True))
        unresolved.update(missing)
    return expanded, unresolved


def _expand_cmake_tokens(
    command: CMakeCommand,
    variables: dict[str, list[str]],
    root: Path,
) -> tuple[list[str], set[str]]:
    arguments, unresolved = _expand_cmake_arguments(command, variables, root)
    return [argument.value for argument in arguments], unresolved


def _expand_cmake_condition_arguments(
    command: CMakeCommand,
    variables: dict[str, list[str]],
    root: Path,
) -> tuple[list[CMakeArgument], set[str]]:
    return _expand_cmake_arguments(command, variables, root)


def _contains_editor_marker(value: str) -> bool:
    normalized = value.replace("\\", "/").lower()
    if any(f"{editor_root}/" in f"{normalized}/" for editor_root in PRODUCTION_EDITOR_ROOTS):
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
    return bool(
        set(words)
        & {"test", "tests", "testing", "probe", "reference", "fixture", "scaffold", "acceptance"}
    )


def _target_owners(command: CMakeCommand, tokens: list[str]) -> set[str]:
    if not tokens:
        return set()
    if command.name in {
        "add_custom_target",
        "add_library",
        "add_executable",
        "add_dependencies",
    } or command.name.startswith("target_"):
        return {tokens[0]}
    if command.name == "add_custom_command" and tokens[0].upper() == "TARGET" and len(tokens) >= 2:
        return {tokens[1]}
    if command.name == "set_target_properties":
        owners: set[str] = set()
        for token in tokens:
            if token.upper() == "PROPERTIES":
                break
            owners.add(token)
        return owners
    if command.name == "set_property" and tokens[0].upper() == "TARGET":
        owners = set()
        for token in tokens[1:]:
            if token.upper() in {"APPEND", "APPEND_STRING", "PROPERTY"}:
                break
            owners.add(token)
        return owners
    return set()


def _assert_no_forbidden_dependency(text: str, label: str, *, allow_renderer_sentinel: bool = False) -> None:
    for family, pattern in FORBIDDEN_DEPENDENCY_PATTERNS:
        for match in pattern.finditer(text):
            if allow_renderer_sentinel and match.group(0).upper() == OUT_OF_SCOPE_RENDERER_SENTINEL:
                continue
            line = text.count("\n", 0, match.start()) + 1
            raise ValidationError(
                f"forbidden editor dependency {match.group(0)!r} ({family}) in {label}:{line}"
            )


def _resolve_declared_source(
    token: str,
    command: CMakeCommand,
    root: Path,
) -> Path | None:
    if not token or token.startswith(("$<", "-")):
        return None
    if token.upper() in {
        "STATIC",
        "SHARED",
        "MODULE",
        "OBJECT",
        "INTERFACE",
        "IMPORTED",
        "GLOBAL",
        "UNKNOWN",
        "ALIAS",
        "EXCLUDE_FROM_ALL",
        "WIN32",
        "MACOSX_BUNDLE",
        "PUBLIC",
        "PRIVATE",
        "BEFORE",
        "SYSTEM",
        "FILE_SET",
        "TYPE",
        "BASE_DIRS",
        "FILES",
    }:
        return None
    normalized = token.replace("\\", "/")
    if not ("/" in normalized or "." in PurePosixPath(normalized).name):
        return None
    candidate = Path(normalized)
    if not candidate.is_absolute():
        candidate = (command.source_dir or command.path.parent) / candidate
    try:
        candidate = candidate.resolve()
    except OSError as exc:
        raise ValidationError(f"cannot resolve editor target source {token!r}: {exc}") from exc
    return candidate if candidate.is_file() else None


def _discover_editor_files(root: Path) -> set[Path]:
    files: set[Path] = set()
    for relative_root in PRODUCTION_EDITOR_ROOTS:
        directory = root.joinpath(*PurePosixPath(relative_root).parts)
        if not directory.exists():
            continue
        require(directory.is_dir(), f"production editor root is not a directory: {relative_root}")
        try:
            files.update(path for path in directory.rglob("*") if path.is_file())
        except OSError as exc:
            raise ValidationError(f"cannot enumerate production editor root {relative_root}: {exc}") from exc
    for relative_root in FIRST_PARTY_SOURCE_ROOTS:
        directory = root / relative_root
        if not directory.exists():
            continue
        try:
            for path in directory.rglob("*"):
                if path.is_file() and _contains_editor_marker(path.relative_to(root).as_posix()):
                    files.add(path)
        except OSError as exc:
            raise ValidationError(f"cannot enumerate editor-named paths below {relative_root}: {exc}") from exc
    return files


def _scan_editor_file(root: Path, path: Path, *, allow_renderer_sentinel: bool) -> None:
    try:
        resolved_root = root.resolve()
        resolved = path.resolve()
        relative = resolved.relative_to(resolved_root)
    except (OSError, ValueError) as exc:
        raise ValidationError(f"editor source escapes or cannot resolve below repository root: {path}") from exc
    _assert_no_forbidden_dependency(
        relative.as_posix(),
        f"editor path {relative.as_posix()}",
        allow_renderer_sentinel=allow_renderer_sentinel,
    )
    try:
        text = resolved.read_bytes().decode("utf-8", errors="replace")
    except OSError as exc:
        raise ValidationError(f"cannot scan editor dependency source {relative.as_posix()}: {exc}") from exc
    _assert_no_forbidden_dependency(
        text,
        relative.as_posix(),
        allow_renderer_sentinel=allow_renderer_sentinel,
    )


_LINK_PROPERTIES = {
    "LINK_LIBRARIES",
    "INTERFACE_LINK_LIBRARIES",
    "INTERFACE_LINK_LIBRARIES_DIRECT",
    "LINK_INTERFACE_LIBRARIES",
    "IMPORTED_LINK_INTERFACE_LIBRARIES",
    "IMPORTED_LINK_DEPENDENT_LIBRARIES",
}
_LINK_OPTION_PROPERTIES = {
    "LINK_OPTIONS",
    "INTERFACE_LINK_OPTIONS",
    "LINK_FLAGS",
    "STATIC_LIBRARY_OPTIONS",
}
_LINK_EFFECT_PROPERTIES = {
    "C_LINKER_LAUNCHER",
    "CXX_LINKER_LAUNCHER",
    "LINKER_LAUNCHER",
    "RULE_LAUNCH_LINK",
}
_COMPILE_EFFECT_PROPERTIES = {
    "C_COMPILER_LAUNCHER",
    "CXX_COMPILER_LAUNCHER",
    "RULE_LAUNCH_COMPILE",
    "COMPILE_FEATURES",
}
_INHERITED_COMPILE_PROPERTIES = {
    "COMPILE_DEFINITIONS",
    "COMPILE_FEATURES",
    "COMPILE_OPTIONS",
    "INCLUDE_DIRECTORIES",
    "PRECOMPILE_HEADERS",
    *_COMPILE_EFFECT_PROPERTIES,
}
_INCLUDE_PROPERTIES = {
    "INCLUDE_DIRECTORIES",
    "INTERFACE_INCLUDE_DIRECTORIES",
}
_SOURCE_PROPERTIES = {
    "SOURCES",
    "INTERFACE_SOURCES",
    "PUBLIC_HEADER",
    "PRIVATE_HEADER",
    "HEADER_SET",
    "INTERFACE_HEADER_SETS",
}
_TARGET_BOUNDARY_PROPERTIES = {
    *_LINK_PROPERTIES,
    *_LINK_OPTION_PROPERTIES,
    *_LINK_EFFECT_PROPERTIES,
    *_COMPILE_EFFECT_PROPERTIES,
    *_INCLUDE_PROPERTIES,
    *_SOURCE_PROPERTIES,
    "COMPILE_DEFINITIONS",
    "INTERFACE_COMPILE_DEFINITIONS",
    "COMPILE_OPTIONS",
    "INTERFACE_COMPILE_OPTIONS",
    "COMPILE_FLAGS",
    "LINK_DIRECTORIES",
    "INTERFACE_LINK_DIRECTORIES",
    "PRECOMPILE_HEADERS",
    "INTERFACE_PRECOMPILE_HEADERS",
}
_SOURCE_BOUNDARY_PROPERTIES = {
    "COMPILE_DEFINITIONS",
    "COMPILE_FLAGS",
    "COMPILE_OPTIONS",
    "INCLUDE_DIRECTORIES",
    "OBJECT_DEPENDS",
}
_KNOWN_SOURCE_PROPERTIES = _SOURCE_BOUNDARY_PROPERTIES | {
    "EXTERNAL_OBJECT",
    "GENERATED",
    "HEADER_FILE_ONLY",
    "KEEP_EXTENSION",
    "LANGUAGE",
    "LOCATION",
    "MACOSX_PACKAGE_LOCATION",
    "SKIP_AUTOGEN",
    "SKIP_PRECOMPILE_HEADERS",
    "SYMBOLIC",
    "UNITY_GROUP",
    "VS_DEPLOYMENT_CONTENT",
    "VS_SHADER_TYPE",
    "VS_TOOL_OVERRIDE",
    "WRAP_EXCLUDE",
}
_KNOWN_TARGET_PROPERTIES = _TARGET_BOUNDARY_PROPERTIES | {
    "ANDROID_API",
    "ARCHIVE_OUTPUT_DIRECTORY",
    "AUTOMOC",
    "AUTORCC",
    "AUTOUIC",
    "C_STANDARD",
    "CXX_STANDARD",
    "CXX_STANDARD_REQUIRED",
    "DEBUG_POSTFIX",
    "EXCLUDE_FROM_ALL",
    "EXPORT_NAME",
    "FOLDER",
    "FRAMEWORK",
    "IMPORTED_CONFIGURATIONS",
    "IMPORTED_IMPLIB",
    "IMPORTED_LOCATION",
    "INTERPROCEDURAL_OPTIMIZATION",
    "LIBRARY_OUTPUT_DIRECTORY",
    "LINKER_LANGUAGE",
    "MACOSX_BUNDLE",
    "OUTPUT_NAME",
    "POSITION_INDEPENDENT_CODE",
    "PREFIX",
    "RUNTIME_OUTPUT_DIRECTORY",
    "SUFFIX",
    "VERSION",
    "WINDOWS_EXPORT_ALL_SYMBOLS",
    "WIN32_EXECUTABLE",
}
_GLOBAL_BOUNDARY_COMMANDS = {
    "add_compile_definitions",
    "add_compile_options",
    "add_definitions",
    "add_link_options",
    "include_directories",
    "link_directories",
    "link_libraries",
}
_TARGET_BOUNDARY_COMMANDS = {
    "add_custom_command",
    "add_custom_target",
    "add_dependencies",
    "target_compile_definitions",
    "target_compile_features",
    "target_compile_options",
    "target_include_directories",
    "target_link_directories",
    "target_link_libraries",
    "target_link_options",
    "target_precompile_headers",
    "target_sources",
}
_CMAKE_DEPENDENCY_KEYWORDS = {
    "AFTER",
    "BEFORE",
    "DEBUG",
    "EXCLUDE_FROM_ALL",
    "GENERAL",
    "INTERFACE",
    "LINK_INTERFACE_LIBRARIES",
    "LINK_PRIVATE",
    "LINK_PUBLIC",
    "OPTIMIZED",
    "PRIVATE",
    "PUBLIC",
    "SYSTEM",
}


def _expand_user_cmake_calls(commands: list[CMakeCommand]) -> list[CMakeCommand]:
    # _ordered_cmake_commands already expands calls in definition-time order
    # while preserving function, macro, and directory scopes.
    return commands


def _expanded_commands(
    commands: list[CMakeCommand],
    root: Path,
) -> dict[CMakeCommand, tuple[list[str], set[str]]]:
    result: dict[CMakeCommand, tuple[list[str], set[str]]] = {}
    for command in commands:
        variables = {name: list(values) for name, values in command.variables}
        tokens, unresolved = _expand_cmake_tokens(command, variables, root)
        result[command] = (tokens, unresolved)
    return result


def _property_entries(command: CMakeCommand, tokens: list[str]) -> tuple[set[str], list[tuple[str, list[str]]]]:
    owners = _target_owners(command, tokens)
    if command.name == "set_property":
        try:
            property_index = next(index for index, token in enumerate(tokens) if token.upper() == "PROPERTY")
        except StopIteration:
            return owners, []
        if property_index + 1 >= len(tokens):
            return owners, []
        return owners, [(tokens[property_index + 1].upper(), tokens[property_index + 2 :])]
    if command.name in {"set_target_properties", "set_directory_properties"}:
        try:
            property_index = next(index for index, token in enumerate(tokens) if token.upper() == "PROPERTIES")
        except StopIteration:
            return owners, []
        values = tokens[property_index + 1 :]
        entries: list[tuple[str, list[str]]] = []
        index = 0
        while index < len(values):
            property_name = values[index].upper()
            index += 1
            property_values: list[str] = []
            while index < len(values):
                candidate_name = values[index].upper()
                is_known_property = candidate_name in _KNOWN_TARGET_PROPERTIES or bool(
                    re.fullmatch(
                        r"(?:IMPORTED_LINK_INTERFACE_LIBRARIES|IMPORTED_LINK_DEPENDENT_LIBRARIES|"
                        r"LINK_INTERFACE_LIBRARIES|IMPORTED_LOCATION|IMPORTED_IMPLIB)_[A-Z0-9_]+",
                        candidate_name,
                    )
                )
                if property_values and is_known_property:
                    break
                property_values.append(values[index])
                index += 1
            entries.append((property_name, property_values))
        return owners, entries
    return owners, []


def _property_family(property_name: str) -> str:
    legacy_config_link = re.fullmatch(
        r"(?:IMPORTED_LINK_INTERFACE_LIBRARIES|IMPORTED_LINK_DEPENDENT_LIBRARIES|LINK_INTERFACE_LIBRARIES)_[A-Z0-9_]+",
        property_name,
    )
    if property_name in _LINK_PROPERTIES or legacy_config_link:
        return "link"
    if property_name in _LINK_OPTION_PROPERTIES:
        return "link_option"
    if property_name in _LINK_EFFECT_PROPERTIES:
        return "link_effect"
    if property_name in _INCLUDE_PROPERTIES:
        return "include"
    if re.fullmatch(r"(?:IMPORTED_LOCATION|IMPORTED_IMPLIB)(?:_[A-Z0-9_]+)?", property_name):
        return "link_artifact"
    if property_name in _SOURCE_PROPERTIES or property_name.startswith("HEADER_SET_"):
        return "source"
    if property_name in _TARGET_BOUNDARY_PROPERTIES:
        return "boundary"
    if re.search(r"(?:^|_)(?:COMPILE|LINK|INCLUDE|PRECOMPILE)(?:_|$)", property_name):
        return "boundary"
    return "metadata"


def _dependency_operands(command: CMakeCommand, tokens: list[str]) -> tuple[set[str], list[str]]:
    owners = _target_owners(command, tokens)
    if command.name in {"target_link_libraries", "add_dependencies"}:
        return owners, tokens[1:]
    if command.name in {"set_property", "set_target_properties"}:
        owners, entries = _property_entries(command, tokens)
        return owners, [value for name, values in entries if _property_family(name) == "link" for value in values]
    return owners, []


def _production_dependency_operands(
    command: CMakeCommand,
    tokens: list[str],
) -> tuple[set[str], list[tuple[str, str]]]:
    owners = _target_owners(command, tokens)
    if command.name == "add_dependencies":
        return owners, [("build_dependency", value) for value in tokens[1:]]
    if command.name == "target_link_libraries":
        return owners, [("link", value) for value in tokens[1:]]
    if command.name == "target_link_options":
        return owners, [("link_option", value) for value in tokens[1:]]
    if command.name == "link_libraries":
        return owners, [("link", value) for value in tokens]
    if command.name == "add_link_options":
        return owners, [("link_option", value) for value in tokens]
    if command.name == "target_include_directories":
        return owners, [("include", value) for value in tokens[1:]]
    if command.name == "target_compile_options":
        return owners, [("compile_option", value) for value in tokens[1:]]
    if command.name in {"set_property", "set_target_properties", "set_directory_properties"}:
        owners, entries = _property_entries(command, tokens)
        operands: list[tuple[str, str]] = []
        for name, values in entries:
            family = _property_family(name)
            if name in {"COMPILE_OPTIONS", "INTERFACE_COMPILE_OPTIONS", "COMPILE_FLAGS"}:
                family = "compile_option"
            if family in {
                "compile_option",
                "include",
                "link",
                "link_artifact",
                "link_option",
                "link_effect",
            }:
                operands.extend((family, value) for value in values)
        return owners, operands
    variable_mutation = _cmake_variable_mutation(command, tokens)
    if variable_mutation is not None:
        name, values = variable_mutation
        if _is_link_effect_variable(name):
            return owners, [("link_effect", value) for value in values]
    return owners, []


def _cmake_variable_mutation(
    command: CMakeCommand,
    tokens: list[str],
) -> tuple[str, list[str]] | None:
    if not tokens:
        return None
    if command.name == "set":
        values: list[str] = []
        for token in tokens[1:]:
            if token.upper() in {"CACHE", "PARENT_SCOPE"}:
                break
            values.append(token)
        return tokens[0], values
    if command.name == "unset":
        return tokens[0], []
    if command.name == "set_property" and len(tokens) >= 5 and tokens[0].upper() == "CACHE":
        try:
            property_index = next(index for index, value in enumerate(tokens) if value.upper() == "PROPERTY")
        except StopIteration:
            return None
        if property_index + 1 < len(tokens) and tokens[property_index + 1].upper() == "VALUE":
            names = tokens[1:property_index]
            if len(names) == 1:
                return names[0], tokens[property_index + 2 :]
    if command.name == "list" and len(tokens) >= 2:
        operation = tokens[0].upper()
        if operation in {
            "APPEND",
            "FILTER",
            "INSERT",
            "POP_BACK",
            "POP_FRONT",
            "PREPEND",
            "REMOVE_AT",
            "REMOVE_DUPLICATES",
            "REMOVE_ITEM",
            "REVERSE",
            "SORT",
            "TRANSFORM",
        }:
            return tokens[1], tokens[2:]
        destination_indexes = {
            "FIND": 3,
            "JOIN": 3,
            "LENGTH": 2,
            "SUBLIST": 4,
        }
        destination_index = destination_indexes.get(operation)
        if operation == "GET" and len(tokens) >= 3:
            destination_index = len(tokens) - 1
        if operation == "TRANSFORM":
            destination_index = 1
            for index, token in enumerate(tokens[:-1]):
                if token.upper() == "OUTPUT_VARIABLE":
                    destination_index = index + 1
        if destination_index is not None and destination_index < len(tokens):
            return tokens[destination_index], tokens[1:destination_index] + tokens[destination_index + 1 :]
    if command.name == "string" and len(tokens) >= 2:
        operation = tokens[0].upper()
        destination_indexes = {
            "APPEND": 1,
            "CONCAT": 1,
            "CONFIGURE": 2,
            "GENEX_STRIP": 2,
            "HEX": 2,
            "JOIN": 2,
            "LENGTH": 2,
            "MAKE_C_IDENTIFIER": 2,
            "PREPEND": 1,
            "REPEAT": 3,
            "STRIP": 2,
            "SUBSTRING": 4,
            "TOLOWER": 2,
            "TOUPPER": 2,
        }
        destination_index = destination_indexes.get(operation)
        if destination_index is not None and destination_index < len(tokens):
            return tokens[destination_index], tokens[destination_index + 1 :]
        if operation == "REPLACE" and len(tokens) >= 4:
            return tokens[3], [*tokens[1:3], *tokens[4:]]
        if operation == "REGEX" and len(tokens) >= 5 and tokens[1].upper() == "REPLACE":
            return tokens[4], [*tokens[2:4], *tokens[5:]]
        if operation == "REGEX" and len(tokens) >= 4:
            return tokens[3], [*tokens[1:3], *tokens[4:]]
        if operation == "COMPARE" and len(tokens) >= 5:
            return tokens[4], [*tokens[1:4], *tokens[5:]]
        if operation in {"MD5", "SHA1", "SHA224", "SHA256", "SHA384", "SHA512", "TIMESTAMP", "UUID", "JSON"}:
            return tokens[1], tokens[2:]
        if operation == "RANDOM":
            return tokens[-1], tokens[1:-1]
    return None


def _is_link_effect_variable(name: str) -> bool:
    upper = name.upper()
    return bool(
        re.fullmatch(
            r"CMAKE_(?:(?:C|CXX)_LINK_EXECUTABLE|(?:C|CXX)_LINKER_LAUNCHER|"
            r"(?:C|CXX)_LINKER_WRAPPER_FLAG(?:_SEP)?|(?:C|CXX)_USING_LINKER_[A-Z0-9_]+|"
            r"(?:C|CXX)_STANDARD_LIBRARIES(?:_[A-Z0-9_]+)?|"
            r"(?:EXE|MODULE|SHARED)_LINKER_FLAGS(?:_[A-Z0-9_]+)?|RULE_LAUNCH_LINK)",
            upper,
        )
    )


def _is_inherited_compile_variable(name: str) -> bool:
    upper = name.upper()
    return bool(
        re.fullmatch(
            r"CMAKE_(?:(?:C|CXX)_FLAGS(?:_[A-Z0-9_]+)?|"
            r"(?:C|CXX)_COMPILER_LAUNCHER|(?:C|CXX)_COMPILE_OBJECT|"
            r"C_(?:STANDARD|STANDARD_REQUIRED|EXTENSIONS)|RULE_LAUNCH_COMPILE)",
            upper,
        )
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
        lowered = value.lower()
        inline = re.fullmatch(r"-(?:weak_)?framework(?:=|,)(.+)", value, re.IGNORECASE)
        if inline:
            frameworks.append(inline.group(1))
        elif lowered in {"-framework", "-weak_framework"}:
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


def _is_link_artifact_operand(value: str, previous: str = "") -> bool:
    normalized = value.strip().strip('"').replace("\\", "/")
    if _framework_link_operands([previous, value]):
        return True
    if previous.upper() in {"-FRAMEWORK", "-WEAK_FRAMEWORK"}:
        return True
    if re.match(r"(?i)^-l[^,\s]+$", normalized):
        return True
    if re.search(r"(?i)(?:^|[,=:])-l[^,\s]+", normalized):
        return True
    if re.search(r"(?i)(?:/DEFAULTLIB:|-DEFAULTLIB:)[^,\s]+", normalized):
        return True
    return re.search(r"(?i)\.(?:a|dylib|lib|so(?:\.[0-9]+)*|tbd)$", normalized) is not None


_UNAPPROVED_UI_OPERAND = re.compile(
    r"(?:^|[^A-Za-z0-9])Qt(?:[4-9])?[A-Za-z0-9_]*(?:[^A-Za-z0-9]|$)",
    re.IGNORECASE,
)


def _is_unapproved_ui_operand(value: str) -> bool:
    return _UNAPPROVED_UI_OPERAND.search(value.replace("\\", "/")) is not None


def _source_property_files(command: CMakeCommand, tokens: list[str], root: Path) -> set[Path]:
    values: list[str] = []
    if command.name == "set_source_files_properties":
        for token in tokens:
            if token.upper() == "PROPERTIES":
                break
            values.append(token)
    elif command.name == "set_property" and tokens and tokens[0].upper() == "SOURCE":
        for token in tokens[1:]:
            if token.upper() in {"APPEND", "APPEND_STRING", "DIRECTORY", "TARGET_DIRECTORY", "PROPERTY"}:
                break
            values.append(token)
    return {
        path
        for token in values
        if (path := _resolve_declared_source(token, command, root)) is not None
    }


def _source_property_entries(command: CMakeCommand, tokens: list[str]) -> list[tuple[str, list[str]]]:
    if command.name == "set_property":
        try:
            property_index = next(index for index, token in enumerate(tokens) if token.upper() == "PROPERTY")
        except StopIteration:
            return []
        if property_index + 1 >= len(tokens):
            return []
        return [(tokens[property_index + 1].upper(), tokens[property_index + 2 :])]
    if command.name != "set_source_files_properties":
        return []
    try:
        property_index = next(index for index, token in enumerate(tokens) if token.upper() == "PROPERTIES")
    except StopIteration:
        return []
    values = tokens[property_index + 1 :]
    entries: list[tuple[str, list[str]]] = []
    index = 0
    while index < len(values):
        property_name = values[index].upper()
        index += 1
        property_values: list[str] = []
        while index < len(values):
            candidate = values[index].upper()
            if property_values and candidate in _KNOWN_SOURCE_PROPERTIES:
                break
            property_values.append(values[index])
            index += 1
        entries.append((property_name, property_values))
    return entries


def _resolve_declared_directory(token: str, command: CMakeCommand, root: Path) -> Path | None:
    if not token or token.startswith(("$<", "-")) or token.upper() in _CMAKE_DEPENDENCY_KEYWORDS:
        return None
    candidate = Path(token.replace("\\", "/"))
    if not candidate.is_absolute():
        candidate = (command.source_dir or command.path.parent) / candidate
    try:
        candidate = candidate.resolve()
        candidate.relative_to(root)
    except (OSError, ValueError):
        return None
    return candidate if candidate.is_dir() else None


def _scan_source_closure(
    root: Path,
    initial_files: dict[Path, bool],
    include_directories: set[Path],
    pinned_tinygltf_consumers: set[Path] | None = None,
) -> None:
    files_to_scan = dict(initial_files)
    pending = list(files_to_scan)
    pinned_tinygltf_consumers = {
        path.resolve() for path in (pinned_tinygltf_consumers or set())
    }
    include_pattern = re.compile(r'^\s*#\s*include\s*([<"])([^>"]+)[>"]', re.MULTILINE)
    dynamic_include = re.compile(r"^\s*#\s*include\s+([^<\"\s].*)$", re.MULTILINE)
    allowed_external_headers = {
        "algorithm", "array", "atomic", "bit", "cassert", "cctype", "cerrno", "charconv", "chrono", "cmath", "cstdarg",
        "compare", "concepts", "condition_variable", "cstddef", "cstdint", "cstdio", "cstdlib", "cstring", "cwchar",
        "ctime", "deque", "exception", "filesystem", "format", "fstream", "functional", "future", "iomanip",
        "initializer_list", "iosfwd", "iostream", "istream", "iterator", "limits", "locale", "map", "memory", "mutex", "new", "numbers", "optional", "ostream",
        "queue", "random", "ranges", "set", "span", "sstream", "stdexcept", "stop_token", "string", "string_view",
        "system_error", "thread", "tuple", "type_traits", "unordered_map", "unordered_set", "utility", "variant", "vector", "numeric",
        "bcrypt.h", "wincrypt.h", "commdlg.h", "debugapi.h", "limits.h", "unistd.h", "windows.h", "imgui.h", "imgui_internal.h",
    }
    allowed_external_prefixes = (
        "glfw/", "glm/", "nvtx3/", "vulkan/", "volk", "backends/imgui_impl_"
    )
    while pending:
        path = pending.pop()
        allow_renderer_sentinel = files_to_scan[path]
        _scan_editor_file(root, path, allow_renderer_sentinel=allow_renderer_sentinel)
        try:
            text = path.read_bytes().decode("utf-8", errors="replace")
        except OSError as exc:
            raise ValidationError(f"cannot scan editor include closure {path}: {exc}") from exc
        dynamic = dynamic_include.search(text)
        if dynamic is not None:
            relative = path.relative_to(root).as_posix()
            raise ValidationError(f"cannot resolve dynamic include in editor dependency source {relative}")
        for match in include_pattern.finditer(text):
            include_name = match.group(2).replace("\\", "/")
            is_tinygltf_header = PurePosixPath(include_name).name.lower() == "tiny_gltf.h"
            if is_tinygltf_header and include_name != "tiny_gltf.h":
                relative = path.relative_to(root).as_posix()
                raise ValidationError(
                    f"non-canonical TinyGLTF header spelling {include_name!r} from {relative}"
                )
            candidates = [path.parent / include_name]
            candidates.extend(directory / include_name for directory in include_directories)
            candidates.append(root / include_name)
            for candidate in candidates:
                try:
                    resolved = candidate.resolve()
                    resolved.relative_to(root)
                except (OSError, ValueError):
                    continue
                if not resolved.is_file():
                    continue
                if is_tinygltf_header:
                    relative = path.relative_to(root).as_posix()
                    raise ValidationError(
                        f"repository-local TinyGLTF lookalike {resolved.relative_to(root).as_posix()!r} "
                        f"is forbidden for {relative}"
                    )
                next_allow = allow_renderer_sentinel
                if resolved in files_to_scan:
                    next_allow = files_to_scan[resolved] and allow_renderer_sentinel
                    if next_allow == files_to_scan[resolved]:
                        break
                files_to_scan[resolved] = next_allow
                pending.append(resolved)
                break
            else:
                if is_tinygltf_header and path in pinned_tinygltf_consumers:
                    continue
                if (
                    not allow_renderer_sentinel
                    and include_name.lower() not in allowed_external_headers
                    and not include_name.lower().startswith(allowed_external_prefixes)
                ):
                    relative = path.relative_to(root).as_posix()
                    raise ValidationError(
                        f"cannot resolve production editor include {include_name!r} from {relative}"
                    )


def validate_editor_dependency_boundary(
    root: Path,
    *,
    validate_policy: bool = True,
    cache_variables: dict[str, str] | None = None,
) -> dict[str, Any]:
    root = root.resolve()
    if validate_policy:
        validate_dependency_boundary_policy(load_json(root / "docs/editor-parity/manifest.json"))

    cmake_paths = _cmake_files(root)
    parsed_commands = _ordered_cmake_commands(root, cmake_paths, cache_variables=cache_variables)
    commands = _expand_user_cmake_calls(parsed_commands)
    expanded_by_command = _expanded_commands(commands, root)
    definitions: dict[str, CMakeCommand] = {}
    aliases: dict[str, str] = {}
    custom_targets: set[str] = set()
    imported_targets: set[str] = set()
    object_library_targets: set[str] = set()
    editor_owned_targets: set[str] = set()
    production_editor_targets: set[str] = set()

    for command in commands:
        if command.name not in {"add_custom_target", "add_library", "add_executable"}:
            continue
        tokens, _ = expanded_by_command[command]
        if not tokens:
            continue
        target = tokens[0]
        definitions[target] = command
        if command.name == "add_custom_target":
            custom_targets.add(target)
            continue
        upper_tokens = [token.upper() for token in tokens[1:]]
        if "OBJECT" in upper_tokens:
            object_library_targets.add(target)
        if "IMPORTED" in upper_tokens:
            imported_targets.add(target)
        if "ALIAS" in upper_tokens:
            alias_index = next(index for index, token in enumerate(tokens) if token.upper() == "ALIAS")
            if alias_index + 1 < len(tokens):
                aliases[target] = tokens[alias_index + 1]
        owns_editor_source = any(_contains_editor_marker(token) for token in tokens[1:])
        if "editor" in target.lower() or owns_editor_source:
            editor_owned_targets.add(target)
            if not _is_nonproduction_target(target):
                production_editor_targets.add(target)

    def canonical(target: str) -> str:
        seen: set[str] = set()
        while target in aliases and target not in seen:
            seen.add(target)
            target = aliases[target]
        return target

    editor_owned_targets = {canonical(target) for target in editor_owned_targets}
    production_editor_targets = {canonical(target) for target in production_editor_targets}
    known_targets = {canonical(target) for target in definitions}
    custom_targets = {canonical(target) for target in custom_targets}
    imported_targets = {canonical(target) for target in imported_targets}
    object_library_targets = {canonical(target) for target in object_library_targets}
    canonical_definitions: dict[str, CMakeCommand] = {}
    for target, definition in definitions.items():
        if target not in aliases:
            canonical_definitions.setdefault(canonical(target), definition)
    definitions = canonical_definitions
    imported_editor_targets = production_editor_targets & imported_targets
    require(
        not imported_editor_targets,
        "imported targets cannot claim production editor identity: "
        + ", ".join(sorted(imported_editor_targets)),
    )

    testing_definition = "MGIF_EDITOR_TESTING=1"
    allowed_testing_definition_targets = {
        "scene_editor_document",
        "editor_history_core_tests",
        "editor_history_structural_tests",
        "editor_history_property_tests",
        "editor_continuous_edit_tests",
    }
    testing_definition_owners: set[str] = set()
    for command in commands:
        tokens, unresolved = expanded_by_command[command]
        raw_tokens = _tokenize_cmake(command.body)
        if not any(
            "MGIF_EDITOR_TESTING" in token for token in (*tokens, *raw_tokens)
        ):
            continue
        require(
            command.name == "target_compile_definitions",
            f"MGIF_EDITOR_TESTING must use target_compile_definitions in {command.path.relative_to(root).as_posix()}:{command.line}",
        )
        require(
            not unresolved and len(tokens) >= 3,
            f"MGIF_EDITOR_TESTING target ownership is unresolved in {command.path.relative_to(root).as_posix()}:{command.line}",
        )
        first_scope = next(
            (
                index
                for index, token in enumerate(tokens)
                if token.upper() in {"PRIVATE", "PUBLIC", "INTERFACE"}
            ),
            -1,
        )
        require(
            first_scope >= 1,
            f"MGIF_EDITOR_TESTING compile-definition scope is missing in {command.path.relative_to(root).as_posix()}:{command.line}",
        )
        owners = {canonical(token) for token in tokens[:first_scope]}
        if len(owners) > 1:
            require(
                bool(raw_tokens) and _CMAKE_VARIABLE.fullmatch(raw_tokens[0]) is not None,
                f"MGIF_EDITOR_TESTING has multiple literal target owners in {command.path.relative_to(root).as_posix()}:{command.line}",
            )
        escaped_owners = owners - allowed_testing_definition_targets
        require(
            not escaped_owners,
            "MGIF_EDITOR_TESTING escaped its five private target owners: "
            + ", ".join(sorted(escaped_owners)),
        )
        active_scope: str | None = None
        occurrence_count = 0
        for token in tokens[first_scope:]:
            upper = token.upper()
            if upper in {"PRIVATE", "PUBLIC", "INTERFACE"}:
                active_scope = upper
                continue
            if "MGIF_EDITOR_TESTING" not in token:
                continue
            occurrence_count += 1
            require(
                token == testing_definition and active_scope == "PRIVATE",
                f"MGIF_EDITOR_TESTING must be the literal private definition in {command.path.relative_to(root).as_posix()}:{command.line}",
            )
        require(
            occurrence_count == 1,
            f"MGIF_EDITOR_TESTING definition cardinality changed in {command.path.relative_to(root).as_posix()}:{command.line}",
        )
        testing_definition_owners.update(owners)

    # Target identity is not fixed at add_library()/add_executable() time. CMake
    # can attach production editor sources later through target_sources() or
    # source-bearing target properties, so collect all attachments before
    # deciding which targets anchor the production dependency closure.
    source_properties_by_target: dict[str, dict[str, set[Path]]] = {
        target: {} for target in known_targets
    }
    raw_source_properties_by_target: dict[str, dict[str, list[str]]] = {
        target: {} for target in known_targets
    }
    for command in commands:
        tokens, _ = expanded_by_command[command]
        owners = {canonical(owner) for owner in _target_owners(command, tokens)} & known_targets
        raw_tokens = _tokenize_cmake(command.body)
        property_updates: list[tuple[str, list[str], list[str], bool]] = []
        if command.name in {"add_library", "add_executable"} and "ALIAS" not in {
            value.upper() for value in tokens[1:]
        }:
            property_updates.append(("SOURCES", tokens[1:], raw_tokens[1:], False))
        elif command.name == "target_sources":
            property_updates.append(("SOURCES", tokens[1:], raw_tokens[1:], True))
        elif command.name in {"set_target_properties", "set_property"}:
            property_owners, entries = _property_entries(command, tokens)
            owners = {canonical(owner) for owner in property_owners} & known_targets
            _, raw_entries = _property_entries(command, raw_tokens)
            raw_by_name = {name: values for name, values in raw_entries}
            append = command.name == "set_property" and any(
                value.upper() in {"APPEND", "APPEND_STRING"}
                for value in tokens[: next((index for index, value in enumerate(tokens) if value.upper() == "PROPERTY"), len(tokens))]
            )
            for property_name, values in entries:
                if _property_family(property_name) == "source":
                    property_updates.append((property_name, values, raw_by_name.get(property_name, values), append))
        if not owners or not property_updates:
            continue
        for owner in owners:
            for property_name, values, raw_values, append in property_updates:
                path_bucket = source_properties_by_target[owner].setdefault(property_name, set())
                raw_bucket = raw_source_properties_by_target[owner].setdefault(property_name, [])
                if not append:
                    path_bucket.clear()
                    raw_bucket.clear()
                raw_bucket.extend(raw_values)
                for value in values:
                    source = _resolve_declared_source(value, command, root)
                    if source is not None:
                        path_bucket.add(source)
    sources_by_target: dict[str, set[Path]] = {
        target: {path for paths in properties.values() for path in paths}
        for target, properties in source_properties_by_target.items()
    }
    late_editor_targets: set[str] = set()
    for owner, properties in raw_source_properties_by_target.items():
        if any(_contains_editor_marker(value) for values in properties.values() for value in values):
            late_editor_targets.add(owner)
    for owner, sources in sources_by_target.items():
        for source in sources:
            try:
                relative_source = source.resolve().relative_to(root).as_posix()
            except ValueError:
                relative_source = source.as_posix()
            if _contains_editor_marker(relative_source):
                late_editor_targets.add(owner)
    editor_owned_targets.update(late_editor_targets)
    production_editor_targets.update(
        target for target in late_editor_targets if not _is_nonproduction_target(target)
    )
    imported_editor_targets = production_editor_targets & imported_targets
    require(
        not imported_editor_targets,
        "imported targets cannot claim production editor identity: "
        + ", ".join(sorted(imported_editor_targets)),
    )
    require(
        production_editor_targets,
        "no production editor target was discovered; dependency validation cannot fail closed",
    )
    document_target = canonical(EXPECTED_SCENE_EDITOR_DOCUMENT_TARGET)
    locked_document_sources = {
        (root / relative).resolve() for relative in EXPECTED_SCENE_EDITOR_DOCUMENT_SOURCES
    }
    locked_core_sources = {
        (root / relative).resolve() for relative in EXPECTED_SCENE_EDITOR_TARGET_SOURCES
    }
    if (
        sources_by_target.get(document_target) == locked_document_sources
        and sources_by_target.get(canonical("scene_editor_core")) == locked_core_sources
        and allowed_testing_definition_targets <= known_targets
    ):
        require(
            testing_definition_owners == allowed_testing_definition_targets,
            "the locked production inventory must bind MGIF_EDITOR_TESTING privately to exactly the document and four P4 test targets",
        )

    def command_directory_scope(command: CMakeCommand) -> Path:
        return (command.source_dir or command.path.parent).resolve()

    def target_in_command_scope(target: str, command: CMakeCommand) -> bool:
        definition = definitions.get(target)
        if definition is None:
            return False
        try:
            (definition.source_dir or definition.path.parent).resolve().relative_to(command_directory_scope(command))
            return True
        except ValueError:
            return False

    dependencies: dict[str, set[str]] = {target: set() for target in known_targets}
    installed_targets: set[str] = set()
    for command in commands:
        tokens, _ = expanded_by_command[command]
        if command.name == "install" and tokens and tokens[0].upper() == "TARGETS":
            for token in tokens[1:]:
                candidate = canonical(token)
                if candidate in known_targets:
                    installed_targets.add(candidate)
                    continue
                if token.upper() in {
                    "ARCHIVE",
                    "BUNDLE",
                    "DESTINATION",
                    "EXPORT",
                    "FRAMEWORK",
                    "INCLUDES",
                    "LIBRARY",
                    "NAMELINK_COMPONENT",
                    "NAMELINK_ONLY",
                    "NAMELINK_SKIP",
                    "OBJECTS",
                    "OPTIONAL",
                    "PERMISSIONS",
                    "PRIVATE_HEADER",
                    "PUBLIC_HEADER",
                    "RESOURCE",
                    "RUNTIME",
                }:
                    break
        if command.name in {"add_executable", "add_library", "target_sources"} and tokens:
            owners = {canonical(owner) for owner in _target_owners(command, tokens)} & known_targets
            raw_tokens = _tokenize_cmake(command.body)
            object_targets = {
                canonical(match.group(1))
                for token in raw_tokens[1:]
                for match in re.finditer(r"\$<TARGET_OBJECTS:([^>$]+)>", token, re.IGNORECASE)
                if canonical(match.group(1)) in known_targets
            }
            object_targets.update(
                canonical(token)
                for token in tokens[1:]
                if canonical(token) in object_library_targets
            )
            for owner in owners:
                dependencies[owner].update(target for target in object_targets if target != owner)
            continue
        if command.name not in {"target_link_libraries", "add_dependencies", "set_target_properties", "set_property"}:
            continue
        if not tokens:
            continue
        owners, operands = _dependency_operands(command, tokens)
        owners = {canonical(owner) for owner in owners} & known_targets
        for owner in owners:
            for operand in operands:
                if operand.upper() in _CMAKE_DEPENDENCY_KEYWORDS or operand.startswith("-"):
                    continue
                candidate = canonical(operand)
                if candidate in known_targets and candidate != owner:
                    dependencies[owner].add(candidate)

    # Directory link_libraries() applies to targets in that directory tree. It
    # is uncommon in modern code but remains dependency-bearing CMake state.
    for command in commands:
        if command.name != "link_libraries":
            continue
        tokens, _ = expanded_by_command[command]
        candidates = {
            canonical(token)
            for token in tokens
            if canonical(token) in known_targets and token.upper() not in _CMAKE_DEPENDENCY_KEYWORDS
        }
        if not candidates:
            continue
        for owner in known_targets:
            if not target_in_command_scope(owner, command):
                continue
            dependencies[owner].update(candidate for candidate in candidates if candidate != owner)

    for command in commands:
        tokens, _ = expanded_by_command[command]
        directory_property = command.name == "set_directory_properties" or (
            command.name == "set_property" and tokens and tokens[0].upper() in {"DIRECTORY", "GLOBAL"}
        )
        if not directory_property:
            continue
        _, entries = _property_entries(command, tokens)
        candidates = {
            canonical(value)
            for name, values in entries
            if _property_family(name) == "link"
            for value in values
            if canonical(value) in known_targets
        }
        for owner in known_targets:
            if target_in_command_scope(owner, command):
                dependencies[owner].update(candidate for candidate in candidates if candidate != owner)

    production_targets = {
        target
        for target in known_targets
        if target not in custom_targets
        and (not _is_nonproduction_target(target) or target in installed_targets)
    }
    production_editor_targets.update(editor_owned_targets & installed_targets)
    relevant_targets = set(production_editor_targets)
    changed = True
    while changed:
        changed = False
        before = len(relevant_targets)
        for target in tuple(relevant_targets):
            relevant_targets.update(dependencies.get(target, set()))
        for target in production_targets - relevant_targets:
            if dependencies.get(target, set()) & relevant_targets:
                relevant_targets.add(target)
                relevant_targets.update(dependencies.get(target, set()))
        changed = len(relevant_targets) != before

    def affects_relevant_directory(command: CMakeCommand) -> bool:
        return any(target_in_command_scope(target, command) for target in relevant_targets)

    acquisition_commands = {
        "externalproject_add",
        "fetchcontent_makeavailable",
        "find_package",
        "find_dependency",
        "find_file",
        "find_library",
        "find_path",
        "fetchcontent_declare",
        "include",
        "pkg_check_modules",
        "add_subdirectory",
    }
    include_directories: set[Path] = {root}
    forced_files: dict[Path, bool] = {}

    def command_label(command: CMakeCommand) -> str:
        return f"{command.path.relative_to(root).as_posix()} CMake {command.name} at line {command.line}"

    read_only_file_operations = {
        "GLOB",
        "GLOB_RECURSE",
        "HASH",
        "READ",
        "READ_SYMLINK",
        "REAL_PATH",
        "RELATIVE_PATH",
        "SIZE",
        "STRINGS",
        "TIMESTAMP",
        "TO_CMAKE_PATH",
        "TO_NATIVE_PATH",
    }
    approved_include_tokens = {
        ("CTest",),
        ("FetchContent",),
        ("CompilerSlangShader",),
        ("CompilerGlslShader",),
        ("DownloadPackage",),
    }
    approved_custom_targets = {
        338: "scene_editor_dependency_preflight",
        906: "scene_editor_progress_ledger_check",
        973: "scene_editor_tests",
        979: "scene_editor_p0_scaffold",
        987: "scene_editor_p0",
        1005: "scene_editor_release_gate",
        2548: "copy_demo_resources",
        2571: "check_rhi_boundary",
    }
    approved_custom_commands = {
        1475: "OUTPUT",
        1506: "OUTPUT",
        1559: "OUTPUT",
        2505: "TARGET",
    }

    def approved_file_effect(raw_tokens: list[str]) -> bool:
        if not raw_tokens:
            return False
        operation = raw_tokens[0].upper()
        if operation in read_only_file_operations:
            return True
        if operation == "COPY_FILE":
            return raw_tokens == ["COPY_FILE", "${STB_IMAGE_CACHED_PATH}", "${STB_IMAGE_PATH}"]
        if operation == "DOWNLOAD":
            return raw_tokens == ["DOWNLOAD", "${STB_IMAGE_URL}", "${STB_IMAGE_PATH}"]
        if operation == "MAKE_DIRECTORY":
            return raw_tokens == ["MAKE_DIRECTORY", "${SHADER_OUTPUT_DIR}"]
        if operation == "REMOVE_RECURSE":
            return raw_tokens == ["REMOVE_RECURSE", "${DEMO_STALE_SLANG_DIR}"]
        if operation == "GENERATE":
            return raw_tokens == [
                "GENERATE",
                "OUTPUT",
                "${CMAKE_CURRENT_BINARY_DIR}/compress_textures_$<CONFIG>.cmd",
                "CONTENT",
                "${COMPRESS_TEXTURES_SCRIPT}",
            ]
        return False

    def approved_custom_effect(command: CMakeCommand, raw_tokens: list[str]) -> bool:
        top_level = (root / "CMakeLists.txt").resolve()
        if command.path.resolve() != top_level or not raw_tokens:
            return False
        if command.name == "add_custom_target":
            expected_target = approved_custom_targets.get(command.line)
            if expected_target is not None and raw_tokens[0] == expected_target:
                return True
            # A command-free target or a diagnostic-only target has no file,
            # process-launch, or dependency-acquisition effect.
            if "COMMAND" not in {value.upper() for value in raw_tokens[1:]}:
                return True
            command_index = next(
                index for index, value in enumerate(raw_tokens) if value.upper() == "COMMAND"
            )
            command_tail = raw_tokens[command_index + 1 :]
            return len(command_tail) >= 4 and command_tail[:3] == ["cmake", "-E", "echo"]
        if command.name == "add_custom_command":
            expected_form = approved_custom_commands.get(command.line)
            if expected_form is not None and raw_tokens[0].upper() == expected_form:
                return True
            command_indices = [
                index for index, value in enumerate(raw_tokens) if value.upper() == "COMMAND"
            ]
            return bool(command_indices) and all(
                raw_tokens[index + 1 : index + 4] == ["cmake", "-E", "echo"]
                for index in command_indices
            )
        return False

    def approved_repository_include(
        command: CMakeCommand,
        raw_tokens: list[str],
        tokens: list[str],
        unresolved: set[str],
    ) -> bool:
        if (
            not raw_tokens
            or not tokens
            or unresolved
            or any("$ENV{" in value.upper() for value in raw_tokens)
        ):
            return False
        include_value = Path(tokens[0].replace("\\", "/"))
        source_dir = command.source_dir or command.path.parent
        candidates: list[Path] = []
        if include_value.is_absolute():
            candidates.append(include_value)
        else:
            candidates.extend((command.path.parent / include_value, source_dir / include_value))
            variables = {name: list(values) for name, values in command.variables}
            for module_dir in variables.get("CMAKE_MODULE_PATH", []):
                module_path = Path(module_dir.replace("\\", "/"))
                if not module_path.is_absolute():
                    module_path = source_dir / module_path
                candidates.append(module_path / include_value)
            candidates.append(root / "cmake" / include_value)
        if not include_value.suffix:
            candidates.extend(candidate.with_suffix(".cmake") for candidate in tuple(candidates))
        for candidate in candidates:
            try:
                resolved = candidate.resolve()
            except OSError:
                continue
            if not resolved.is_file():
                continue
            try:
                resolved.relative_to(root)
            except ValueError as exc:
                raise ValidationError(
                    f"unsupported external CMake traversal in {command_label(command)}"
                ) from exc
            return True
        return False

    def validate_effectful_command_provenance() -> None:
        top_level = (root / "CMakeLists.txt").resolve()

        def effect_context(raw_tokens: list[str], tokens: list[str]) -> str:
            joined = " ".join((*raw_tokens, *tokens)).lower()
            details: list[str] = []
            if re.search(r"(?<![a-z0-9])volk(?:[:/._-]|$)", joined):
                details.append("production editor dependency 'volk' is outside allowedUiFoundation")
            if re.search(r"(?<![a-z0-9])apple(?![a-z0-9])", joined):
                details.append("unsupported APPLE guard provenance")
            if "output_variable" in joined:
                details.append("cannot resolve editor dependency declaration")
            return "; " + "; ".join(details) if details else ""

        for candidate_command in commands:
            raw_tokens = _tokenize_cmake(candidate_command.body)
            tokens, unresolved = expanded_by_command[candidate_command]
            if candidate_command.name == "file":
                if approved_file_effect(raw_tokens):
                    continue
                operation = raw_tokens[0].upper() if raw_tokens else "<missing>"
                raise ValidationError(
                    f"unapproved configure-time file({operation}) effect; unsupported generated CMake traversal "
                    f"in {command_label(candidate_command)}{effect_context(raw_tokens, tokens)}"
                )
            if candidate_command.name in {"configure_file", "execute_process"}:
                generated_detail = (
                    "; unsupported generated CMake traversal"
                    if candidate_command.name == "configure_file"
                    else ""
                )
                raise ValidationError(
                    f"unapproved configure-time {candidate_command.name} effect{generated_detail} in "
                    f"{command_label(candidate_command)}{effect_context(raw_tokens, tokens)}"
                )
            if candidate_command.name in {"add_custom_command", "add_custom_target"}:
                if approved_custom_effect(candidate_command, raw_tokens):
                    continue
                raise ValidationError(
                    f"unapproved {candidate_command.name} effect in {command_label(candidate_command)}"
                    f"{effect_context(raw_tokens, tokens)}"
                )
            if candidate_command.name == "include":
                if (
                    not unresolved
                    and tuple(raw_tokens) in approved_include_tokens
                    and not any("$ENV{" in value.upper() for value in raw_tokens)
                ):
                    continue
                if approved_repository_include(candidate_command, raw_tokens, tokens, unresolved):
                    continue
                raise ValidationError(
                    f"unapproved CMake include/evaluation provenance in {command_label(candidate_command)}"
                    f"{effect_context(raw_tokens, tokens)}"
                )
            if candidate_command.name == "cmake_language":
                if candidate_command.body.startswith(
                    (
                        "unsupported CMake control flow ",
                        "unsupported external CMake traversal ",
                        "unsupported generated CMake traversal ",
                    )
                ):
                    raise ValidationError(candidate_command.body)
                repository_tail_forms = (
                    candidate_command.path.resolve() == top_level
                    and (
                        tokens == ["DEFER", "CALL", "mgif_editor_dependency_tail_drain"]
                        or (
                            len(tokens) == 5
                            and tokens[0] == "DEFER"
                            and tokens[1] == "DIRECTORY"
                            and tokens[3:] in (
                                ["GET_CALL_IDS", "DEMO_EDITOR_PENDING_CALLS"],
                                ["CALL", "mgif_editor_dependency_tail_drain"],
                            )
                        )
                    )
                )
                if repository_tail_forms:
                    continue
                raise ValidationError(
                    f"unsupported dynamic CMake evaluation with unapproved provenance in "
                    f"{command_label(candidate_command)}"
                )
            if candidate_command.name in {
                "externalproject_add",
                "externalproject_add_step",
                "fetchcontent_getproperties",
                "fetchcontent_populate",
            }:
                raise ValidationError(
                    f"unapproved dependency acquisition/evaluation effect in {command_label(candidate_command)}"
                )

    validate_effectful_command_provenance()

    def add_forced_files(command: CMakeCommand, values: list[str], allow_renderer_sentinel: bool) -> None:
        compiler_arguments: list[str] = []
        for value in values:
            if value.upper().startswith("SHELL:"):
                try:
                    compiler_arguments.extend(shlex.split(value[6:], posix=True))
                except ValueError as exc:
                    raise ValidationError(f"cannot parse quoted SHELL: compile options in {command_label(command)}") from exc
            else:
                compiler_arguments.append(value)
        # Clang's driver forwards the argument following each -Xclang. Removing
        # only those wrappers exposes the same forced-include sequence seen by
        # the frontend without changing the remaining option order.
        compiler_arguments = [value for value in compiler_arguments if value.lower() != "-xclang"]

        separated_include_options = {
            "-cxx-isystem",
            "-external:i",
            "-idirafter",
            "-imsvc",
            "-internal-externc-isystem",
            "-internal-isystem",
            "-i",
            "-iquote",
            "-isystem",
            "-isystem-after",
            "-objc-isystem",
            "-objcxx-isystem",
            "/external:i",
            "/i",
            "/imsvc",
        }
        attached_include_options = tuple(
            sorted(separated_include_options - {"-i", "/i"}, key=len, reverse=True)
        )
        unsupported_search_options = (
            "--sysroot",
            "-iframework",
            "-iprefix",
            "-isysroot",
            "-iwithprefix",
            "-resource-dir",
            "/external:env:",
            "/winsysroot",
        )
        expect_directory = False
        for value in compiler_arguments:
            candidate_value = ""
            lowered = value.lower()
            if expect_directory:
                candidate_value = value
                expect_directory = False
            elif lowered in {"-include", "-imacros", "/fi"}:
                continue
            elif lowered.startswith("-include") or lowered.startswith("-imacros") or lowered.startswith("/fi"):
                continue
            elif lowered in separated_include_options:
                expect_directory = True
                continue
            elif any(
                lowered == option or lowered.startswith(option + "=")
                for option in unsupported_search_options
            ) or (value.startswith("-F") and len(value) >= 2):
                raise ValidationError(
                    f"unsupported production editor include-search option {value!r} "
                    f"in {command_label(command)}"
                )
            elif next(
                (
                    option
                    for option in attached_include_options
                    if lowered.startswith(option) and len(value) > len(option)
                ),
                None,
            ) is not None:
                option = next(
                    option
                    for option in attached_include_options
                    if lowered.startswith(option) and len(value) > len(option)
                )
                candidate_value = value[len(option) :]
                if candidate_value.startswith("="):
                    candidate_value = candidate_value[1:]
            elif lowered.startswith("-i") and len(value) > 2:
                candidate_value = value[2:]
            elif lowered.startswith("/i") and len(value) > 2:
                candidate_value = value[2:]
            if not candidate_value:
                continue
            directory = _resolve_declared_directory(candidate_value, command, root)
            if directory is None:
                raise ValidationError(
                    f"cannot resolve production editor include-search directory {candidate_value!r} "
                    f"in {command_label(command)}"
                )
            include_directories.add(directory)

        expect_file = False
        for value in compiler_arguments:
            candidate_value = ""
            if expect_file:
                candidate_value = value
                expect_file = False
            elif value.lower() in {"-include", "-imacros", "/fi"}:
                expect_file = True
                continue
            elif value.lower().startswith("/fi") and len(value) > 3:
                candidate_value = value[3:]
            elif value.lower().startswith("-include="):
                candidate_value = value.split("=", 1)[1]
            if not candidate_value:
                continue
            source = _resolve_declared_source(candidate_value, command, root)
            if source is not None:
                resolved = source.resolve()
                forced_files[resolved] = forced_files.get(resolved, True) and allow_renderer_sentinel
        joined = " ".join(compiler_arguments)
        forced_include_pattern = r'(?i)(?:^|\s)(?:-include(?:=|\s+)|-imacros(?:=|\s+)|/FI\s*)(?:"([^"]+)"|(\S+))'
        for match in re.finditer(forced_include_pattern, joined):
            source = _resolve_declared_source(match.group(1) or match.group(2), command, root)
            if source is not None:
                resolved = source.resolve()
                forced_files[resolved] = forced_files.get(resolved, True) and allow_renderer_sentinel

    for command in commands:
        raw_tokens = _tokenize_cmake(command.body)
        tokens, unresolved = expanded_by_command[command]
        owners = {canonical(owner) for owner in _target_owners(command, tokens)}
        direct_editor_command = bool(owners & editor_owned_targets)
        relevant_owner_command = bool(owners & relevant_targets)
        directory_property = command.name == "set_directory_properties" or (
            command.name == "set_property" and tokens and tokens[0].upper() in {"DIRECTORY", "GLOBAL"}
        )
        compile_flag_mutations, unsupported_flag_mutations = _compile_flag_mutations(command, tokens, root)
        global_from_function = not command.function_scope or bool(
            (
                command.name == "set"
                and (
                    {token.upper() for token in tokens} & {"PARENT_SCOPE", "CACHE"}
                    or (tokens and _CMAKE_ENV_COMPILE_FLAG_VARIABLE.fullmatch(tokens[0]) is not None)
                )
            )
            or (command.name == "set_property" and tokens and tokens[0].upper() == "CACHE")
            or (command.name == "return" and tokens and tokens[0].upper() == "PROPAGATE")
        )
        compile_flag_variable = bool(
            compile_flag_mutations
            and global_from_function
            and affects_relevant_directory(command)
        )
        if unsupported_flag_mutations and global_from_function and affects_relevant_directory(command):
            raise ValidationError(
                f"unsupported computed mutation of global compiler flags "
                f"({', '.join(sorted(unsupported_flag_mutations))}) in {command_label(command)}"
            )
        relevant_command = False
        if command.name in {"add_library", "add_executable"}:
            relevant_command = bool(owners & relevant_targets)
        elif command.name in _TARGET_BOUNDARY_COMMANDS:
            relevant_command = relevant_owner_command
        elif command.name in _GLOBAL_BOUNDARY_COMMANDS:
            relevant_command = affects_relevant_directory(command)
        elif command.name in acquisition_commands:
            relevant_command = command.path == root / "CMakeLists.txt" or relevant_owner_command
        elif command.name in {"set_target_properties", "set_property"} and relevant_owner_command:
            _, entries = _property_entries(command, tokens)
            relevant_command = any(_property_family(name) != "metadata" for name, _ in entries)
        elif directory_property:
            _, entries = _property_entries(command, tokens)
            relevant_command = affects_relevant_directory(command) and any(
                _property_family(name) != "metadata" for name, _ in entries
            )
        elif compile_flag_variable:
            relevant_command = True

        property_sources = _source_property_files(command, tokens, root)
        all_relevant_sources = {path for paths in sources_by_target.values() for path in paths}
        relevant_source_property = bool(property_sources & all_relevant_sources)
        if relevant_source_property and command.name in {"set_source_files_properties", "set_property"}:
            relevant_command = True

        if direct_editor_command or relevant_command:
            if command.name == "set_property" and any(
                value.upper() == "APPEND_STRING" for value in tokens
            ):
                _, append_entries = _property_entries(command, tokens)
                if any(_property_family(name) == "source" for name, _ in append_entries):
                    raise ValidationError(
                        f"unsupported APPEND_STRING mutation of final target SOURCES in {command_label(command)}"
                    )
            unexpected_variables = unresolved - ALLOWED_TRANSITIVE_CMAKE_VARIABLES
            if compile_flag_variable:
                unexpected_variables.difference_update(compile_flag_mutations)
            if unexpected_variables:
                missing = ", ".join(sorted(unexpected_variables))
                raise ValidationError(
                    f"cannot resolve editor dependency declaration variables ({missing}) in "
                    f"{command.path.relative_to(root).as_posix()}:{command.line}"
                )
            command_text = " ".join(
                tokens if any("$<" in token for token in raw_tokens) else (*raw_tokens, *tokens)
            )
            _assert_no_forbidden_dependency(
                command_text,
                command_label(command),
                allow_renderer_sentinel=(
                    not direct_editor_command
                    and command.name not in _GLOBAL_BOUNDARY_COMMANDS
                    and not directory_property
                ),
            )

        if command.name in {"set_target_properties", "set_property"}:
            property_owners, entries = _property_entries(command, tokens)
            for owner in {canonical(value) for value in property_owners} & relevant_targets:
                for property_name, values in entries:
                    if property_name in {"INCLUDE_DIRECTORIES", "INTERFACE_INCLUDE_DIRECTORIES"}:
                        include_directories.update(
                            path
                            for token in values
                            if (path := _resolve_declared_directory(token, command, root)) is not None
                        )
                    if property_name in {"COMPILE_OPTIONS", "INTERFACE_COMPILE_OPTIONS", "COMPILE_FLAGS"}:
                        add_forced_files(command, values, owner not in editor_owned_targets)
                    if property_name in {"PRECOMPILE_HEADERS", "INTERFACE_PRECOMPILE_HEADERS"}:
                        for value in values:
                            source = _resolve_declared_source(value, command, root)
                            if source is not None:
                                resolved = source.resolve()
                                allow_renderer_sentinel = owner not in editor_owned_targets
                                forced_files[resolved] = (
                                    forced_files.get(resolved, True) and allow_renderer_sentinel
                                )

        if directory_property and affects_relevant_directory(command):
            _, entries = _property_entries(command, tokens)
            for property_name, values in entries:
                if property_name in {"INCLUDE_DIRECTORIES", "INTERFACE_INCLUDE_DIRECTORIES"}:
                    include_directories.update(
                        path
                        for token in values
                        if (path := _resolve_declared_directory(token, command, root)) is not None
                    )
                if property_name in {"COMPILE_OPTIONS", "INTERFACE_COMPILE_OPTIONS", "COMPILE_FLAGS"}:
                    add_forced_files(command, values, False)

        if command.name == "target_include_directories" and owners & relevant_targets:
            include_directories.update(
                path
                for token in tokens[1:]
                if (path := _resolve_declared_directory(token, command, root)) is not None
            )
        elif command.name == "include_directories" and affects_relevant_directory(command):
            include_directories.update(
                path
                for token in tokens
                if (path := _resolve_declared_directory(token, command, root)) is not None
            )
        if command.name in {"target_compile_options", "target_precompile_headers"} and owners & relevant_targets:
            add_forced_files(command, tokens[1:], not bool(owners & editor_owned_targets))
        elif command.name in {"add_compile_options", "add_definitions"} and affects_relevant_directory(command):
            add_forced_files(command, tokens, False)
        elif compile_flag_variable:
            add_forced_files(command, [value for values in compile_flag_mutations.values() for value in values], False)

    # Source properties are evaluated after all target source declarations so a
    # property that precedes add_library()/target_sources() is still attributed.
    relevant_source_flags: dict[Path, bool] = {}
    for target, paths in sources_by_target.items():
        if target not in relevant_targets:
            continue
        for path in paths:
            relevant_source_flags[path.resolve()] = target not in editor_owned_targets
    for command in commands:
        if command.name not in {"set_source_files_properties", "set_property"}:
            continue
        tokens, unresolved = expanded_by_command[command]
        property_sources = _source_property_files(command, tokens, root)
        affected = property_sources & set(relevant_source_flags)
        if not affected:
            continue
        unexpected_variables = unresolved - ALLOWED_TRANSITIVE_CMAKE_VARIABLES
        if unexpected_variables:
            raise ValidationError(
                f"cannot resolve editor source property variables ({', '.join(sorted(unexpected_variables))}) in "
                f"{command.path.relative_to(root).as_posix()}:{command.line}"
            )
        allow_renderer_sentinel = all(relevant_source_flags[path] for path in affected)
        _assert_no_forbidden_dependency(
            " ".join((*_tokenize_cmake(command.body), *tokens)),
            command_label(command),
            allow_renderer_sentinel=allow_renderer_sentinel,
        )
        for property_name, property_values in _source_property_entries(command, tokens):
            if property_name not in _SOURCE_BOUNDARY_PROPERTIES:
                continue
            if property_name == "INCLUDE_DIRECTORIES":
                include_directories.update(
                    path
                    for token in property_values
                    if (path := _resolve_declared_directory(token, command, root)) is not None
                )
            elif property_name == "OBJECT_DEPENDS":
                for token in property_values:
                    source = _resolve_declared_source(token, command, root)
                    if source is not None:
                        resolved = source.resolve()
                        forced_files[resolved] = forced_files.get(resolved, True) and allow_renderer_sentinel
            else:
                add_forced_files(command, property_values, allow_renderer_sentinel)

    # Enforce the positive UI foundation across the entire forward production
    # closure. Internal repository targets are MGIF-owned; an exact Dear ImGui
    # identity is authority only when its canonical source target proves the
    # upstream core source set. Name fragments and imported identities are not.
    forward_editor_targets = set(production_editor_targets)
    changed = True
    while changed:
        before = len(forward_editor_targets)
        for target in tuple(forward_editor_targets):
            forward_editor_targets.update(dependencies.get(target, set()))
        changed = len(forward_editor_targets) != before
    exact_dear_imgui_target = "DearImGui::DearImGui"
    top_level_source_commands = _parse_cmake_commands((root / "CMakeLists.txt").resolve())

    expected_apple_frameworks = (
        ("DEMO_METAL_FRAMEWORK", "Metal"),
        ("DEMO_FOUNDATION_FRAMEWORK", "Foundation"),
    )
    expected_apple_framework_names = {
        framework.lower() for _, framework in expected_apple_frameworks
    }
    protected_apple_identifiers = {
        identifier: re.compile(
            rf"(?<![A-Za-z0-9_]){re.escape(identifier)}(?![A-Za-z0-9_])",
        )
        for identifier in (
            "APPLE",
            *(variable for variable, _ in expected_apple_frameworks),
        )
    }

    def arguments_mention_protected_identifier(tokens: list[str], identifier: str) -> bool:
        return any(protected_apple_identifiers[identifier].search(token) is not None for token in tokens)

    # Guard provenance must be checked from source as well as from the modeled
    # command stream. A hostile writer can otherwise make the parser consider
    # if(APPLE) false and hide the framework edge that activates this check.
    apple_framework_authority_source_present = any(
        (
            source_command.name == "find_library"
            and any(
                token.lower() in expected_apple_framework_names
                for token in _tokenize_cmake(source_command.body)
            )
        )
        or any(
            arguments_mention_protected_identifier(
                _tokenize_cmake(source_command.body),
                variable,
            )
            for variable, _ in expected_apple_frameworks
        )
        for source_command in top_level_source_commands
    )
    if apple_framework_authority_source_present:
        for source_command in top_level_source_commands:
            source_tokens = _tokenize_cmake(source_command.body)
            if source_command.name not in {"if", "elseif"} and arguments_mention_protected_identifier(
                source_tokens,
                "APPLE",
            ):
                raise ValidationError(
                    "demo_core Apple framework authority has unsupported APPLE guard provenance in "
                    f"CMakeLists.txt CMake {source_command.name} at line {source_command.line}"
                )
        for candidate_command in commands:
            raw_tokens = _tokenize_cmake(candidate_command.body)
            expanded_tokens = expanded_by_command[candidate_command][0]
            if arguments_mention_protected_identifier(
                [*raw_tokens, *expanded_tokens],
                "APPLE",
            ):
                raise ValidationError(
                    "demo_core Apple framework authority has unsupported APPLE guard provenance in "
                    f"{command_label(candidate_command)}"
                )

    def repository_foundation_target_allowed(target: str) -> bool:
        if target in editor_owned_targets:
            return True
        for source in sources_by_target.get(target, set()):
            try:
                relative = source.resolve().relative_to(root)
            except ValueError:
                continue
            if relative.parts and relative.parts[0].lower() in {"app", "rhi", "scene"}:
                return True
        words = {
            match.group(0).lower()
            for component in re.split(r"[^A-Za-z0-9]+", target)
            for match in re.finditer(r"[A-Z]+(?=[A-Z][a-z]|\d|$)|[A-Z]?[a-z]+|\d+", component)
        }
        return bool(words & {"app", "application", "file", "input", "platform", "rhi", "scene", "window"}) and bool(
            words & {"adapter", "bridge", "contract"}
        )

    def repository_tinygltf_target_allowed() -> bool:
        document_target = canonical(EXPECTED_SCENE_EDITOR_DOCUMENT_TARGET)
        if (
            document_target not in known_targets
            or document_target in imported_targets
            or canonical(EXPECTED_TINYGLTF_TARGET) in known_targets
        ):
            return False
        declarations = [
            (command, tokens)
            for command in commands
            if command.name == "fetchcontent_declare"
            and (tokens := expanded_by_command[command][0])
            and tokens[0].lower() == EXPECTED_TINYGLTF_TARGET
        ]
        if len(declarations) != 1:
            return False
        declaration, declaration_tokens = declarations[0]
        upper = [value.upper() for value in declaration_tokens]
        if (
            len(declaration_tokens) != 5
            or upper[0] != EXPECTED_TINYGLTF_TARGET.upper()
            or upper[1] != "GIT_REPOSITORY"
            or declaration_tokens[2].rstrip("/").lower() != EXPECTED_TINYGLTF_REPOSITORY.lower()
            or upper[3] != "GIT_TAG"
            or declaration_tokens[4] != EXPECTED_TINYGLTF_TAG
        ):
            return False
        if not any(
            command.name == "fetchcontent_makeavailable"
            and command.instance > declaration.instance
            and [value.lower() for value in expanded_by_command[command][0]] == [EXPECTED_TINYGLTF_TARGET]
            for command in commands
        ):
            return False
        if any(
            command.name in {"function", "macro"}
            and (raw := _tokenize_cmake(command.body))
            and raw[0].lower() in {"fetchcontent_declare", "fetchcontent_makeavailable"}
            for command in parsed_commands
        ):
            return False

        header_only_sets = []
        for command in commands:
            raw = _tokenize_cmake(command.body)
            if not raw:
                continue
            lowered = [value.lower() for value in raw]
            if any(
                value in {"tinygltf_source_dir", "fetchcontent_source_dir_tinygltf"}
                for value in lowered
            ):
                return False
            if lowered[0] == "tinygltf_header_only":
                if command.name != "set":
                    return False
                header_only_sets.append(raw)
        if len(header_only_sets) != 1 or len(header_only_sets[0]) < 2 or header_only_sets[0][1].upper() != "ON":
            return False

        try:
            document_sources = {
                source.resolve().relative_to(root).as_posix()
                for source in sources_by_target.get(document_target, set())
            }
        except ValueError:
            return False
        if document_sources != EXPECTED_SCENE_EDITOR_DOCUMENT_SOURCES:
            return False

        direct_edges: list[tuple[list[str], list[str]]] = []
        for command in commands:
            if command.name != "target_link_libraries":
                continue
            tokens, _ = expanded_by_command[command]
            owners = {canonical(owner) for owner in _target_owners(command, tokens)}
            if document_target in owners and EXPECTED_TINYGLTF_TARGET in tokens[1:]:
                direct_edges.append((tokens, _tokenize_cmake(command.body)))
        expected_edge = [document_target, "PRIVATE", EXPECTED_TINYGLTF_TARGET]
        if direct_edges != [(expected_edge, expected_edge)]:
            return False

        implementation = root / "loader/TinyGltfImplementation.cpp"
        loader_source = root / "loader/GltfLoader.cpp"
        try:
            implementation_text = implementation.read_text(encoding="utf-8")
            loader_text = loader_source.read_text(encoding="utf-8")
        except OSError:
            return False
        implementation_macros = (
            "TINYGLTF_IMPLEMENTATION",
            "STB_IMAGE_IMPLEMENTATION",
            "STB_IMAGE_WRITE_IMPLEMENTATION",
        )
        if any(implementation_text.count(f"#define {macro}") != 1 for macro in implementation_macros):
            return False
        if any(f"#define {macro}" in loader_text for macro in implementation_macros):
            return False
        return True

    def repository_dear_imgui_target_allowed(target: str) -> bool:
        if target not in known_targets or target in imported_targets:
            return False
        declarations = [
            (command, tokens)
            for command in commands
            if command.name == "fetchcontent_declare"
            and (tokens := expanded_by_command[command][0])
            and tokens[0].lower() == "imgui"
        ]
        if len(declarations) != 1:
            return False
        declaration, declaration_tokens = declarations[0]
        upper = [value.upper() for value in declaration_tokens]
        if (
            len(declaration_tokens) != 5
            or upper[0] != "IMGUI"
            or upper[1] != "GIT_REPOSITORY"
            or declaration_tokens[2].rstrip("/").lower() != EXPECTED_DEAR_IMGUI_REPOSITORY.lower()
            or upper[3] != "GIT_TAG"
            or declaration_tokens[4].lower() != EXPECTED_DEAR_IMGUI_COMMIT
        ):
            return False
        populated = any(
            command.name == "fetchcontent_makeavailable"
            and command.instance > declaration.instance
            and [value.lower() for value in expanded_by_command[command][0]] == ["imgui"]
            for command in commands
        )
        if not populated:
            return False
        if any(
            command.name in {"function", "macro"}
            and (raw := _tokenize_cmake(command.body))
            and raw[0].lower() in {"fetchcontent_declare", "fetchcontent_makeavailable"}
            for command in parsed_commands
        ):
            return False
        for command in commands:
            raw = _tokenize_cmake(command.body)
            if not raw:
                continue
            destination = raw[0].lower()
            if command.name in {"set", "option", "unset"} and destination in {
                "imgui_source_dir",
                "fetchcontent_source_dir_imgui",
            }:
                return False
            if command.name in {"list", "string", "cmake_parse_arguments"} and any(
                value.lower() in {"imgui_source_dir", "fetchcontent_source_dir_imgui"} for value in raw
            ):
                return False

        declared_inputs = {
            value.replace("\\", "/")
            for values in raw_source_properties_by_target.get(target, {}).values()
            for value in values
        }
        required_inputs = {f"${{imgui_SOURCE_DIR}}/{name}" for name in EXPECTED_DEAR_IMGUI_CORE_SOURCES}
        if not required_inputs <= declared_inputs:
            return False
        source_keywords = {
            "ALIAS",
            "EXCLUDE_FROM_ALL",
            "GLOBAL",
            "IMPORTED",
            "INTERFACE",
            "MODULE",
            "OBJECT",
            "PRIVATE",
            "PUBLIC",
            "SHARED",
            "STATIC",
        }
        for value in declared_inputs:
            if value.upper() in source_keywords:
                continue
            if value.startswith("${imgui_SOURCE_DIR}/") and (
                value == "${imgui_SOURCE_DIR}/imgui_demo.cpp"
                or value.startswith("${imgui_SOURCE_DIR}/backends/")
                or value in required_inputs
            ):
                continue
            return False
        return True

    # GLFW is the platform windowing provider for the pinned ImGui GLFW
    # backend, not an MGIF production-editor adapter. Keep this association at
    # the one canonical imgui_demo edge so an editor target cannot inherit a
    # generic GLFW exception.
    def repository_dear_imgui_glfw_edge_allowed(command: CMakeCommand, owners: set[str], operand: str) -> bool:
        imgui_target = canonical(EXPECTED_DEAR_IMGUI_TARGET)
        if operand != "glfw" or {canonical(owner) for owner in owners} != {imgui_target}:
            return False
        if canonical("glfw") in known_targets or not repository_dear_imgui_target_allowed(imgui_target):
            return False

        expected_edge = [EXPECTED_DEAR_IMGUI_TARGET, "PUBLIC", "glfw"]
        glfw_edges = []
        for candidate_command in commands:
            if candidate_command.name != "target_link_libraries":
                continue
            candidate_tokens, _ = expanded_by_command[candidate_command]
            candidate_owners = {
                canonical(owner) for owner in _target_owners(candidate_command, candidate_tokens)
            }
            if imgui_target in candidate_owners and "glfw" in candidate_tokens[1:]:
                glfw_edges.append((candidate_tokens, _tokenize_cmake(candidate_command.body)))
        if glfw_edges != [(expected_edge, expected_edge)] or _tokenize_cmake(command.body) != expected_edge:
            return False

        declared_imgui_inputs = {
            value.replace("\\", "/")
            for values in raw_source_properties_by_target.get(imgui_target, {}).values()
            for value in values
            if value.replace("\\", "/").startswith("${imgui_SOURCE_DIR}/")
        }
        expected_imgui_input_variants = {
            frozenset(
                f"${{imgui_SOURCE_DIR}}/{relative}"
                for relative in values
            )
            for values in EXPECTED_DEAR_IMGUI_TARGET_SOURCE_VARIANTS.values()
        }
        # Static traversal conservatively merges mutually exclusive ANDROID
        # and desktop branches. Accept either exact platform set or their
        # exact union, while rejecting every unrelated source attachment.
        if frozenset(declared_imgui_inputs) not in (
            expected_imgui_input_variants
            | {frozenset().union(*expected_imgui_input_variants)}
        ):
            return False

        declarations = [
            (candidate_command, candidate_tokens)
            for candidate_command in commands
            if candidate_command.name == "fetchcontent_declare"
            and (candidate_tokens := expanded_by_command[candidate_command][0])
            and candidate_tokens[0].lower() == "glfw"
        ]
        if len(declarations) != 1:
            return False
        declaration, declaration_tokens = declarations[0]
        expected_declaration = [
            "glfw",
            "GIT_REPOSITORY",
            EXPECTED_GLFW_REPOSITORY,
            "GIT_TAG",
            EXPECTED_GLFW_TAG,
        ]
        if declaration_tokens != expected_declaration or _tokenize_cmake(declaration.body) != expected_declaration:
            return False
        if not any(
            candidate_command.name == "fetchcontent_makeavailable"
            and candidate_command.instance > declaration.instance
            and expanded_by_command[candidate_command][0] == ["glfw"]
            and _tokenize_cmake(candidate_command.body) == ["glfw"]
            for candidate_command in commands
        ):
            return False

        for candidate_command in commands:
            raw = _tokenize_cmake(candidate_command.body)
            if not raw:
                continue
            destination = raw[0].lower()
            if candidate_command.name in {"set", "option", "unset"} and destination in {
                "glfw_source_dir",
                "fetchcontent_source_dir_glfw",
            }:
                return False
            if candidate_command.name in {"list", "string", "cmake_parse_arguments"} and any(
                value.lower() in {"glfw_source_dir", "fetchcontent_source_dir_glfw"} for value in raw
            ):
                return False
        return True

    # The Android ImGui backend has one platform-system dependency of its own.
    # Keep it as narrowly constrained as the GLFW edge above: a literal,
    # top-level imgui_demo PUBLIC android link under the direct ANDROID guard.
    def repository_dear_imgui_android_edge_allowed(
        command: CMakeCommand,
        owners: set[str],
        operand: str,
    ) -> bool:
        expected_edge = [EXPECTED_DEAR_IMGUI_TARGET, "PUBLIC", "android"]
        top_level = (root / "CMakeLists.txt").resolve()
        if (
            operand != "android"
            or {canonical(owner) for owner in owners}
            != {canonical(EXPECTED_DEAR_IMGUI_TARGET)}
            or command.path.resolve() != top_level
            or command.function_scope
            or expanded_by_command[command][0] != expected_edge
            or _tokenize_cmake(command.body) != expected_edge
            or len(command.branches) != 1
        ):
            return False
        branch = command.branches[0]
        return (
            branch.kind == "if"
            and branch.arguments == ("ANDROID",)
            and branch.path.resolve() == top_level
            and repository_dear_imgui_target_allowed(canonical(EXPECTED_DEAR_IMGUI_TARGET))
        )

    def repository_demo_core_glfw_edge_allowed(
        command: CMakeCommand,
        owners: set[str],
        operand: str,
    ) -> bool:
        expected_edge = ["demo_core", "PUBLIC", "glfw"]
        if (
            operand != "glfw"
            or {canonical(owner) for owner in owners} != {canonical("demo_core")}
            or command.path.resolve() != (root / "CMakeLists.txt").resolve()
            or command.function_scope
            or expanded_by_command[command][0] != expected_edge
            or _tokenize_cmake(command.body) != expected_edge
            or len(command.branches) != 1
        ):
            return False
        branch = command.branches[0]
        if (
            branch.kind != "else"
            or branch.arguments
            or branch.path.resolve() != (root / "CMakeLists.txt").resolve()
        ):
            return False

        imgui_target = canonical(EXPECTED_DEAR_IMGUI_TARGET)
        imgui_edges = []
        for candidate_command in commands:
            if candidate_command.name != "target_link_libraries":
                continue
            candidate_tokens, _ = expanded_by_command[candidate_command]
            candidate_owners = _target_owners(candidate_command, candidate_tokens)
            if (
                imgui_target in {canonical(owner) for owner in candidate_owners}
                and "glfw" in candidate_tokens[1:]
            ):
                imgui_edges.append((candidate_command, candidate_owners))
        return (
            len(imgui_edges) == 1
            and repository_dear_imgui_glfw_edge_allowed(
                imgui_edges[0][0],
                imgui_edges[0][1],
                "glfw",
            )
        )

    # Volk is the repository's FetchContent-provided Vulkan loader foundation,
    # not a general editor dependency. Static traversal deliberately does not
    # enter fetched source trees, so bind this one unresolved target name to
    # the exact acquisition and common-renderer edge; configured validation
    # separately proves the resulting target and checkout identity.
    volk_foundation_authority: dict[str, Any] | None = None

    def repository_demo_core_volk_edge_allowed(
        command: CMakeCommand,
        owners: set[str],
        operand: str,
    ) -> bool:
        nonlocal volk_foundation_authority
        expected_target = EXPECTED_VOLK_TARGET
        demo_core = canonical("demo_core")
        top_level = (root / "CMakeLists.txt").resolve()
        expected_edge = list(EXPECTED_DEMO_CORE_COMMON_RENDERER_LINK)
        if (
            operand != expected_target
            or demo_core != "demo_core"
            or {canonical(owner) for owner in owners} != {demo_core}
            or command.path.resolve() != top_level
            or command.function_scope
            or command.branches
            or expanded_by_command[command][0] != expected_edge
            or _tokenize_cmake(command.body) != expected_edge
        ):
            return False

        definition = definitions.get(demo_core)
        definition_tokens = expanded_by_command[definition][0] if definition is not None else []
        literal_definitions = [
            candidate_command
            for candidate_command in commands
            if candidate_command.name in {"add_library", "add_executable"}
            and (raw := _tokenize_cmake(candidate_command.body))
            and raw[0] == "demo_core"
        ]
        if (
            definition is None
            or literal_definitions != [definition]
            or definition.name != "add_library"
            or definition.path.resolve() != top_level
            or definition.function_scope
            or definition.branches
            or len(definition_tokens) < 2
            or definition_tokens[:2] != ["demo_core", "STATIC"]
            or demo_core in imported_targets
        ):
            return False

        # A repository CMake definition, import, or alias named volk would
        # shadow the FetchContent product. The external definition is proven
        # later from the configured codemodel instead of being synthesized as
        # a known static-analysis target.
        if (
            any(
                alias_name.lower() == expected_target
                or alias_name.lower().startswith(f"{expected_target}::")
                or alias_target.lower() == expected_target
                or alias_target.lower().startswith(f"{expected_target}::")
                for alias_name, alias_target in aliases.items()
            )
            or canonical(expected_target) != expected_target
            or expected_target in definitions
            or expected_target in known_targets
            or expected_target in imported_targets
        ):
            return False

        expected_declaration = [
            expected_target,
            "GIT_REPOSITORY",
            EXPECTED_VOLK_REPOSITORY,
            "GIT_TAG",
            EXPECTED_VOLK_TAG,
        ]
        declarations = [
            candidate_command
            for candidate_command in commands
            if candidate_command.name == "fetchcontent_declare"
            and (tokens := expanded_by_command[candidate_command][0])
            and tokens[0].lower() == expected_target
        ]
        populations = [
            candidate_command
            for candidate_command in commands
            if candidate_command.name == "fetchcontent_makeavailable"
            and (tokens := expanded_by_command[candidate_command][0])
            and any(value.lower() == expected_target for value in tokens)
        ]
        if len(declarations) != 1 or len(populations) != 1:
            return False
        declaration = declarations[0]
        population = populations[0]
        if (
            declaration.path.resolve() != top_level
            or declaration.function_scope
            or declaration.branches
            or expanded_by_command[declaration][0] != expected_declaration
            or _tokenize_cmake(declaration.body) != expected_declaration
            or population.path.resolve() != top_level
            or population.function_scope
            or population.branches
            or expanded_by_command[population][0] != [expected_target]
            or _tokenize_cmake(population.body) != [expected_target]
        ):
            return False

        def literal_source_matches(
            candidate_command: CMakeCommand,
            expected_name: str,
            expected_tokens: list[str],
        ) -> bool:
            return any(
                source_command.name == expected_name
                and source_command.line == candidate_command.line
                and _tokenize_cmake(source_command.body) == expected_tokens
                for source_command in top_level_source_commands
            )

        def dependency_operands(candidate_command: CMakeCommand, tokens: list[str]) -> list[str]:
            if candidate_command.name == "link_libraries":
                return tokens
            if candidate_command.name == "set_directory_properties" or (
                candidate_command.name == "set_property"
                and tokens
                and tokens[0].upper() in {"DIRECTORY", "GLOBAL"}
            ):
                _, entries = _property_entries(candidate_command, tokens)
                return [
                    value
                    for property_name, values in entries
                    if _property_family(property_name) == "link"
                    for value in values
                ]
            _, operands = _production_dependency_operands(candidate_command, tokens)
            return [
                value
                for family, value in operands
                if family in {"build_dependency", "link"}
            ]

        volk_operand = re.compile(
            rf"(?<![A-Za-z0-9_.-]){re.escape(expected_target)}"
            rf"(?:::{re.escape(expected_target)})?(?![A-Za-z0-9_.-])",
            re.IGNORECASE,
        )

        def mentions_volk_dependency(value: str) -> bool:
            return volk_operand.search(value) is not None

        source_declarations = [
            source_command
            for source_command in top_level_source_commands
            if source_command.name == "fetchcontent_declare"
            and (raw := _tokenize_cmake(source_command.body))
            and raw[0].lower() == expected_target
        ]
        source_populations = [
            source_command
            for source_command in top_level_source_commands
            if source_command.name == "fetchcontent_makeavailable"
            and (raw := _tokenize_cmake(source_command.body))
            and any(value.lower() == expected_target for value in raw)
        ]
        source_edges = [
            source_command
            for source_command in top_level_source_commands
            if any(
                mentions_volk_dependency(value)
                for value in dependency_operands(
                    source_command,
                    _tokenize_cmake(source_command.body),
                )
            )
        ]
        alternate_acquisition_commands = {
            "add_custom_command",
            "add_custom_target",
            "add_executable",
            "add_library",
            "add_subdirectory",
            "cmake_language",
            "configure_file",
            "execute_process",
            "externalproject_add",
            "externalproject_add_step",
            "fetchcontent_declare",
            "fetchcontent_getproperties",
            "fetchcontent_makeavailable",
            "fetchcontent_populate",
            "file",
            "find_dependency",
            "find_file",
            "find_library",
            "find_package",
            "find_path",
            "find_program",
            "include",
            "pkg_check_modules",
        }

        volk_artifact_marker = re.compile(
            rf"(?<![A-Za-z0-9]){re.escape(expected_target)}(?![A-Za-z0-9])",
            re.IGNORECASE,
        )

        def mentions_volk_acquisition(tokens: list[str]) -> bool:
            return any(volk_artifact_marker.search(value.replace("\\", "/")) for value in tokens)

        def is_canonical_volk_acquisition(name: str, tokens: list[str]) -> bool:
            return (
                name == "fetchcontent_declare" and tokens == expected_declaration
            ) or (
                name == "fetchcontent_makeavailable" and tokens == [expected_target]
            )

        if any(
            candidate_command.name in alternate_acquisition_commands
            and not is_canonical_volk_acquisition(candidate_command.name, candidate_tokens)
            and mentions_volk_acquisition(candidate_tokens)
            for candidate_command, candidate_tokens in (
                (source_command, _tokenize_cmake(source_command.body))
                for source_command in top_level_source_commands
            )
        ) or any(
            candidate_command.name in alternate_acquisition_commands
            and not is_canonical_volk_acquisition(
                candidate_command.name,
                expanded_by_command[candidate_command][0],
            )
            and mentions_volk_acquisition(expanded_by_command[candidate_command][0])
            for candidate_command in commands
        ):
            return False

        volk_owner_mutations: list[CMakeCommand] = []
        for candidate_command in commands:
            if candidate_command.name not in (
                _TARGET_BOUNDARY_COMMANDS
                | {"set_property", "set_target_properties"}
            ):
                continue
            candidate_tokens = expanded_by_command[candidate_command][0]
            candidate_owners = _target_owners(candidate_command, candidate_tokens)
            if any(
                owner.lower() in {expected_target, f"{expected_target}::{expected_target}"}
                for owner in candidate_owners
            ):
                volk_owner_mutations.append(candidate_command)
        for mutation in volk_owner_mutations:
            mutation_tokens = expanded_by_command[mutation][0]
            _assert_no_forbidden_dependency(
                " ".join((*_tokenize_cmake(mutation.body), *mutation_tokens)),
                command_label(mutation),
            )
        if volk_owner_mutations:
            return False

        allowed_inherited_compile_forms = {
            ("add_compile_options", ("/utf-8",), "MSVC"),
            ("add_compile_definitions", ("VK_USE_PLATFORM_ANDROID_KHR",), "ANDROID"),
            ("add_compile_definitions", ("NDEBUG",), "ANDROID"),
            (
                "include_directories",
                ("BEFORE", "${DEMO_VULKAN_HEADER_DIR}"),
                "DEMO_VULKAN_HEADER_DIR",
            ),
        }
        inherited_compile_authority: list[dict[str, Any]] = []
        seen_inherited_forms: set[tuple[str, tuple[str, ...], str]] = set()
        for candidate_command in commands:
            candidate_tokens = expanded_by_command[candidate_command][0]
            raw_tokens = _tokenize_cmake(candidate_command.body)
            mutation_kind = ""
            mutation_values: list[str] = []
            if candidate_command.name in {
                "add_compile_definitions",
                "add_compile_options",
                "add_definitions",
                "include_directories",
            }:
                mutation_kind = candidate_command.name
                mutation_values = raw_tokens
            elif candidate_command.name == "set_directory_properties" or (
                candidate_command.name == "set_property"
                and candidate_tokens
                and candidate_tokens[0].upper() in {"DIRECTORY", "GLOBAL"}
            ):
                _, entries = _property_entries(candidate_command, candidate_tokens)
                compile_entries = [
                    (name, values)
                    for name, values in entries
                    if name in _INHERITED_COMPILE_PROPERTIES
                    or _property_family(name) == "boundary"
                    and re.search(r"(?:^|_)(?:COMPILE|INCLUDE|PRECOMPILE)(?:_|$)", name)
                ]
                if compile_entries:
                    mutation_kind = "directory_compile_property"
                    mutation_values = [
                        value
                        for name, values in compile_entries
                        for value in (name, *values)
                    ]
            variable_mutation = _cmake_variable_mutation(candidate_command, candidate_tokens)
            if variable_mutation is not None and _is_inherited_compile_variable(variable_mutation[0]):
                mutation_kind = "inherited_compile_variable"
                mutation_values = [variable_mutation[0], *variable_mutation[1]]
            if not mutation_kind:
                continue

            branch = candidate_command.branches[0] if len(candidate_command.branches) == 1 else None
            canonical_form = (
                candidate_command.name,
                tuple(raw_tokens),
                branch.arguments[0]
                if branch is not None
                and branch.kind == "if"
                and len(branch.arguments) == 1
                else "",
            )
            if (
                canonical_form not in allowed_inherited_compile_forms
                or canonical_form in seen_inherited_forms
                or candidate_command.path.resolve() != top_level
                or candidate_command.function_scope
                or branch is None
                or branch.path.resolve() != top_level
                or candidate_tokens != raw_tokens
                or not literal_source_matches(
                    candidate_command,
                    candidate_command.name,
                    raw_tokens,
                )
            ):
                _assert_no_forbidden_dependency(
                    " ".join((*raw_tokens, *candidate_tokens, *mutation_values)),
                    command_label(candidate_command),
                )
                return False
            seen_inherited_forms.add(canonical_form)
            inherited_compile_authority.append(
                {
                    "command": candidate_command.name,
                    "arguments": raw_tokens,
                    "line": candidate_command.line,
                    "guard": branch.arguments[0],
                }
            )

        for candidate_command in commands:
            if candidate_command.name != "set_source_files_properties" and not (
                candidate_command.name == "set_property"
                and (candidate_tokens := expanded_by_command[candidate_command][0])
                and candidate_tokens[0].upper() == "SOURCE"
            ):
                continue
            raw_tokens = _tokenize_cmake(candidate_command.body)
            candidate_tokens = expanded_by_command[candidate_command][0]
            if mentions_volk_acquisition([*raw_tokens, *candidate_tokens]):
                return False

        if (
            len(source_declarations) != 1
            or len(source_populations) != 1
            or len(source_edges) != 1
            or not literal_source_matches(declaration, "fetchcontent_declare", expected_declaration)
            or not literal_source_matches(population, "fetchcontent_makeavailable", [expected_target])
            or not literal_source_matches(command, "target_link_libraries", expected_edge)
        ):
            return False

        dependency_mentions: list[CMakeCommand] = []
        for candidate_command in commands:
            candidate_tokens = expanded_by_command[candidate_command][0]
            if any(
                mentions_volk_dependency(value)
                for value in dependency_operands(candidate_command, candidate_tokens)
            ):
                dependency_mentions.append(candidate_command)
        if dependency_mentions != [command]:
            return False

        protected_variables = {
            "fetchcontent_source_dir_volk",
            "volk_headers_only",
            "volk_install",
            "volk_namespace",
            "volk_pull_in_vulkan",
            "volk_source_dir",
            "volk_static_defines",
            "vulkan_headers_install_dir",
        }
        for candidate_command in commands:
            raw = _tokenize_cmake(candidate_command.body)
            if not raw:
                continue
            lowered = [value.lower() for value in raw]
            if candidate_command.name in {"set", "option", "unset"} and lowered[0] in protected_variables:
                return False
            if candidate_command.name in {"cmake_parse_arguments", "list", "string"} and any(
                value in protected_variables for value in lowered
            ):
                return False

        if not (
            declaration.line < population.line < command.line
            and declaration.instance < population.instance < command.instance
        ):
            return False

        authority = {
            "target": expected_target,
            "repository": EXPECTED_VOLK_REPOSITORY,
            "tag": EXPECTED_VOLK_TAG,
            "owner": "demo_core",
            "declarationLine": declaration.line,
            "populationLine": population.line,
            "edgeLine": command.line,
            "inheritedCompileAuthority": inherited_compile_authority,
        }
        if volk_foundation_authority is not None and volk_foundation_authority != authority:
            return False
        volk_foundation_authority = authority
        return True

    # glm, TinyGLTF, meshoptimizer, and the Vulkan SDK are pre-existing
    # renderer foundations on demo_core, not UI-foundation exceptions.  They
    # are authorized only as operands of the same literal common-renderer edge
    # whose Volk acquisition/owner authority was closed above.  This must not
    # become a name-based allowlist: a second edge, wrapper, reordered list, or
    # different owner remains outside allowedUiFoundation.
    def repository_demo_core_renderer_foundation_edge_allowed(
        command: CMakeCommand,
        owners: set[str],
        operand: str,
    ) -> bool:
        expected_edge = list(EXPECTED_DEMO_CORE_COMMON_RENDERER_LINK)
        if (
            operand not in EXPECTED_DEMO_CORE_RENDERER_FOUNDATIONS
            or volk_foundation_authority is None
            or {canonical(owner) for owner in owners} != {canonical("demo_core")}
            or command.path.resolve() != (root / "CMakeLists.txt").resolve()
            or command.function_scope
            or command.branches
            or expanded_by_command[command][0] != expected_edge
            or _tokenize_cmake(command.body) != expected_edge
        ):
            return False
        if operand == EXPECTED_TINYGLTF_TARGET:
            return repository_tinygltf_target_allowed()
        return True

    def repository_renderer_scene_transaction_foundation_edge_allowed(
        command: CMakeCommand,
        owners: set[str],
        operand: str,
    ) -> bool:
        expected_edge = list(EXPECTED_RENDERER_SCENE_TRANSACTION_LINK)
        if (
            operand not in {"glm", EXPECTED_TINYGLTF_TARGET}
            or {canonical(owner) for owner in owners}
            != {canonical("renderer_scene_transaction")}
            or command.path.resolve() != (root / "CMakeLists.txt").resolve()
            or command.function_scope
            or len(command.branches) != 1
            or command.branches[0].kind != "if"
            or command.branches[0].arguments != ("NOT", "ANDROID")
            or command.branches[0].path.resolve()
            != (root / "CMakeLists.txt").resolve()
            or expanded_by_command[command][0] != expected_edge
            or _tokenize_cmake(command.body) != expected_edge
        ):
            return False
        definition = definitions.get(canonical("renderer_scene_transaction"))
        if definition is None:
            return False
        definition_tokens = expanded_by_command[definition][0]
        if (
            definition.name != "add_library"
            or definition.path.resolve() != (root / "CMakeLists.txt").resolve()
            or definition.function_scope
            or len(definition.branches) != 1
            or definition.branches[0].kind != "if"
            or definition.branches[0].arguments != ("NOT", "ANDROID")
            or definition.branches[0].path.resolve()
            != (root / "CMakeLists.txt").resolve()
            or definition_tokens[:2] != ["renderer_scene_transaction", "STATIC"]
        ):
            return False
        return (
            operand != EXPECTED_TINYGLTF_TARGET
            or repository_tinygltf_target_allowed()
        )

    # Android's NDK system libraries and native_app_glue are platform inputs of
    # the runtime viewer, not editor UI foundations.  Keep them confined to the
    # repository's one literal demo_core edge under the direct ANDROID guard.
    def repository_demo_core_android_edge_allowed(
        command: CMakeCommand,
        owners: set[str],
        operand: str,
    ) -> bool:
        expected_edge = list(EXPECTED_DEMO_CORE_ANDROID_LINK)
        if (
            operand not in EXPECTED_DEMO_CORE_ANDROID_FOUNDATIONS
            or {canonical(owner) for owner in owners} != {canonical("demo_core")}
            or command.path.resolve() != (root / "CMakeLists.txt").resolve()
            or command.function_scope
            or expanded_by_command[command][0] != expected_edge
            or _tokenize_cmake(command.body) != expected_edge
            or len(command.branches) != 1
        ):
            return False
        branch = command.branches[0]
        return (
            branch.kind == "if"
            and branch.arguments == ("ANDROID",)
            and branch.path.resolve() == (root / "CMakeLists.txt").resolve()
        )

    def repository_demo_core_unix_edge_allowed(
        command: CMakeCommand,
        owners: set[str],
        operand: str,
    ) -> bool:
        top_level = (root / "CMakeLists.txt").resolve()
        expected_guard = ("UNIX", "AND", "NOT", "APPLE", "AND", "NOT", "ANDROID")
        expected_edge = list(EXPECTED_DEMO_CORE_UNIX_LINK)
        if (
            operand not in EXPECTED_DEMO_CORE_UNIX_FOUNDATIONS
            or {canonical(owner) for owner in owners} != {canonical("demo_core")}
            or command.path.resolve() != top_level
            or command.function_scope
            or expanded_by_command[command][0] != expected_edge
            or _tokenize_cmake(command.body) != expected_edge
            or len(command.branches) != 1
        ):
            return False
        branch = command.branches[0]
        if (
            branch.kind != "if"
            or branch.arguments != expected_guard
            or branch.path.resolve() != top_level
        ):
            return False

        expected_packages = {("X11", "REQUIRED"), ("Threads", "REQUIRED")}
        packages = set()
        for candidate_command in commands:
            if candidate_command.name != "find_package":
                continue
            raw = _tokenize_cmake(candidate_command.body)
            if not raw or raw[0] not in {"X11", "Threads"}:
                continue
            if (
                candidate_command.path.resolve() != top_level
                or candidate_command.function_scope
                or expanded_by_command[candidate_command][0] != raw
                or len(candidate_command.branches) != 1
                or candidate_command.branches[0].kind != "if"
                or candidate_command.branches[0].arguments != expected_guard
            ):
                return False
            packages.add(tuple(raw))
        return packages == expected_packages

    def repository_demo_core_windows_edge_allowed(
        command: CMakeCommand,
        owners: set[str],
        operand: str,
    ) -> bool:
        top_level = (root / "CMakeLists.txt").resolve()
        if (
            operand not in EXPECTED_DEMO_CORE_WINDOWS_FOUNDATIONS
            or {canonical(owner) for owner in owners} != {canonical("demo_core")}
            or command.path.resolve() != top_level
            or command.function_scope
            or expanded_by_command[command][0] != _tokenize_cmake(command.body)
        ):
            return False
        tokens = expanded_by_command[command][0]
        if tokens == list(EXPECTED_DEMO_CORE_WINMM_LINK):
            expected_branches = (("if", ("WIN32",)),)
        elif tokens == list(EXPECTED_DEMO_CORE_D3D12_LINK):
            expected_branches = (
                ("if", ("WIN32",)),
                ("if", ("DEMO_ENABLE_D3D12",)),
            )
        else:
            return False
        return tuple(
            (branch.kind, branch.arguments) for branch in command.branches
        ) == expected_branches and all(
            branch.path.resolve() == top_level for branch in command.branches
        )

    def repository_texture_converter_nvtt_edge_allowed(
        command: CMakeCommand,
        owners: set[str],
        operand: str,
    ) -> bool:
        top_level = (root / "CMakeLists.txt").resolve()
        if (
            {canonical(owner) for owner in owners} != {canonical("texture_converter")}
            or command.path.resolve() != top_level
            or command.function_scope
            or _tokenize_cmake(command.body)
            != ["texture_converter", "PRIVATE", "${NVTT_LIBRARY}"]
            or len(command.branches) != 1
            or command.branches[0].kind != "if"
            or command.branches[0].arguments != ("NOT", "ANDROID")
            or command.branches[0].path.resolve() != top_level
            or not operand.replace("\\", "/").lower().endswith(
                "/nvidia texture tools/lib/x64-v142/nvtt30205.lib"
            )
        ):
            return False

        observed: dict[str, list[str]] = {}
        for candidate_command in commands:
            raw = _tokenize_cmake(candidate_command.body)
            if (
                candidate_command.name not in {"set", "unset", "option", "list", "string"}
                or not raw
                or raw[0] not in EXPECTED_NVTT_VARIABLES
            ):
                continue
            if (
                candidate_command.name != "set"
                or candidate_command.path.resolve() != top_level
                or candidate_command.function_scope
                or len(candidate_command.branches) != 1
                or candidate_command.branches[0].kind != "if"
                or candidate_command.branches[0].arguments != ("NOT", "ANDROID")
                or raw != EXPECTED_NVTT_VARIABLES[raw[0]]
                or raw[0] in observed
            ):
                return False
            observed[raw[0]] = raw
        return observed == EXPECTED_NVTT_VARIABLES

    # Metal and Foundation are platform SDK inputs of demo_core, not UI
    # foundation targets. Authorize only the repository's literal APPLE arm:
    # exact find_library provenance, one static demo_core owner, and one link
    # edge containing both variables. The variables remain non-authoritative
    # everywhere else in the production-editor closure.
    def repository_demo_core_apple_framework_edge_allowed(
        command: CMakeCommand,
        owners: set[str],
        operand: str,
    ) -> bool:
        expected_operands = [f"${{{variable}}}" for variable, _ in expected_apple_frameworks]
        if operand not in expected_operands:
            return False

        top_level = (root / "CMakeLists.txt").resolve()
        demo_core = canonical("demo_core")
        if (
            demo_core != "demo_core"
            or {canonical(owner) for owner in owners} != {demo_core}
            or command.path.resolve() != top_level
            or command.function_scope
        ):
            return False

        definition = definitions.get(demo_core)
        if definition is None:
            return False
        definition_tokens = expanded_by_command[definition][0]
        literal_definitions = [
            candidate_command
            for candidate_command in commands
            if candidate_command.name in {"add_library", "add_executable"}
            and (raw := _tokenize_cmake(candidate_command.body))
            and raw[0] == "demo_core"
        ]
        if (
            literal_definitions != [definition]
            or definition.name != "add_library"
            or definition.path.resolve() != top_level
            or definition.function_scope
            or definition.branches
            or len(definition_tokens) < 2
            or definition_tokens[:2] != ["demo_core", "STATIC"]
            or demo_core in imported_targets
        ):
            return False

        expected_edge = ["demo_core", "PUBLIC", *expected_operands]
        if (
            expanded_by_command[command][0] != expected_edge
            or _tokenize_cmake(command.body) != expected_edge
            or len(command.branches) != 1
        ):
            return False
        apple_branch = command.branches[0]
        if (
            apple_branch.kind != "if"
            or apple_branch.arguments != ("APPLE",)
            or apple_branch.path.resolve() != top_level
        ):
            return False

        def is_literal_top_level_command(
            expected_name: str,
            expected_line: int,
            expected_tokens: list[str],
        ) -> bool:
            return any(
                source_command.name == expected_name
                and source_command.line == expected_line
                and _tokenize_cmake(source_command.body) == expected_tokens
                for source_command in top_level_source_commands
            )

        if not is_literal_top_level_command("if", apple_branch.line, ["APPLE"]):
            return False

        declarations: list[CMakeCommand] = []
        for variable, framework in expected_apple_frameworks:
            producers = [
                candidate_command
                for candidate_command in commands
                if (raw := _tokenize_cmake(candidate_command.body))
                and raw[0] == variable
            ]
            expected_declaration = [variable, framework, "REQUIRED"]
            if (
                len(producers) != 1
                or producers[0].name != "find_library"
                or producers[0].path.resolve() != top_level
                or producers[0].function_scope
                or producers[0].branches != command.branches
                or _tokenize_cmake(producers[0].body) != expected_declaration
                or expanded_by_command[producers[0]][0] != expected_declaration
                or not is_literal_top_level_command(
                    "find_library",
                    producers[0].line,
                    expected_declaration,
                )
            ):
                return False
            declarations.append(producers[0])

        if not (
            apple_branch.line < declarations[0].line < declarations[1].line < command.line
            and declarations[0].instance < declarations[1].instance < command.instance
            and is_literal_top_level_command("target_link_libraries", command.line, expected_edge)
        ):
            return False

        framework_variable_names = {variable for variable, _ in expected_apple_frameworks}
        expected_mentions = {command, *declarations}
        for candidate_command in commands:
            raw = _tokenize_cmake(candidate_command.body)
            expanded = expanded_by_command[candidate_command][0]
            mentioned = {
                variable
                for variable in framework_variable_names
                if arguments_mention_protected_identifier([*raw, *expanded], variable)
            }
            if mentioned and candidate_command not in expected_mentions:
                return False
        return True

    for command in commands:
        if command.name not in {
            "add_dependencies",
            "add_link_options",
            "link_libraries",
            "list",
            "set_directory_properties",
            "set_target_properties",
            "set_property",
            "set",
            "string",
            "target_compile_options",
            "target_include_directories",
            "target_link_libraries",
            "target_link_options",
            "unset",
        }:
            continue
        tokens, unresolved = expanded_by_command[command]
        owners, operands = _production_dependency_operands(command, tokens)
        variable_mutation = _cmake_variable_mutation(command, tokens)
        directory_wide = command.name in {
            "add_link_options",
            "link_libraries",
            "set_directory_properties",
        } or (
            command.name == "set_property"
            and bool(tokens)
            and tokens[0].upper() in {"DIRECTORY", "GLOBAL"}
        ) or (
            variable_mutation is not None
            and _is_link_effect_variable(variable_mutation[0])
        )
        if directory_wide and not owners and affects_relevant_directory(command):
            owners = {
                target
                for target in forward_editor_targets
                if tokens and tokens[0].upper() == "GLOBAL"
                or target_in_command_scope(target, command)
            }
        relevant_owners = {canonical(owner) for owner in owners} & forward_editor_targets
        if not relevant_owners:
            continue

        link_effect_values = [value for family, value in operands if family == "link_effect"]
        if link_effect_values:
            raw_owners, raw_operands = _production_dependency_operands(
                command,
                _tokenize_cmake(command.body),
            )
            del raw_owners
            link_effect_values.extend(
                value for family, value in raw_operands if family == "link_effect"
            )
            self_reference = variable_mutation[0] if variable_mutation is not None else ""
            unresolved_effects = unresolved - ({self_reference} if self_reference else set())
            frameworks = _framework_link_operands(link_effect_values)
            if frameworks:
                raise ValidationError(
                    "production editor framework link effect "
                    f"{frameworks[0]!r} is outside the exclusive APPLE framework authority in "
                    f"{command_label(command)}"
                )
            if unresolved_effects:
                raise ValidationError(
                    "cannot resolve production editor link-effect payload "
                    f"({', '.join(sorted(unresolved_effects))}) in {command_label(command)}"
                )
            if any(_is_unapproved_ui_operand(value) for value in link_effect_values):
                raise ValidationError(
                    "production editor link-effect payload is outside allowedUiFoundation in "
                    f"{command_label(command)}"
                )
        previous = ""
        for family, operand in operands:
            if family == "link_effect":
                continue
            if operand.upper() in _CMAKE_DEPENDENCY_KEYWORDS:
                previous = operand
                continue
            candidate = canonical(operand)
            if family == "build_dependency":
                allowed = candidate in known_targets and candidate not in imported_targets
            elif family in {"compile_option", "include"}:
                allowed = not _is_unapproved_ui_operand(operand)
            elif family == "link_option":
                allowed = not _is_link_artifact_operand(operand, previous) and not _is_unapproved_ui_operand(operand)
            elif family == "link" and operand in {"bcrypt", "crypt32"}:
                # OS SHA-256/Base64 backends, with private Windows-only owners.
                allowed = (
                    command.name == "target_link_libraries"
                    and command.path == (root / "CMakeLists.txt").resolve()
                    and not command.function_scope
                    and operand not in known_targets
                    and _tokenize_cmake(command.body) == [
                        "scene_change_contract", "PRIVATE",
                        "$<$<PLATFORM_ID:Windows>:bcrypt;crypt32>",
                    ]
                )
            elif (
                family in {"link", "link_artifact"}
                and operand in EXPECTED_DEMO_CORE_WINDOWS_FOUNDATIONS
            ):
                allowed = repository_demo_core_windows_edge_allowed(
                    command,
                    owners,
                    operand,
                )
            elif (
                family in {"link", "link_artifact"}
                and operand.replace("\\", "/").lower().endswith(
                    "/nvidia texture tools/lib/x64-v142/nvtt30205.lib"
                )
            ):
                allowed = repository_texture_converter_nvtt_edge_allowed(
                    command,
                    owners,
                    operand,
                )
            elif family == "link_artifact":
                allowed = False
            elif family == "link" and _is_link_artifact_operand(operand, previous):
                allowed = False
            elif family == "link" and operand == EXPECTED_VOLK_TARGET:
                allowed = repository_demo_core_volk_edge_allowed(command, owners, operand)
            elif operand == exact_dear_imgui_target or candidate == "imgui_demo":
                allowed = repository_dear_imgui_target_allowed(candidate)
            elif family == "link" and operand == EXPECTED_TINYGLTF_TARGET:
                allowed = (
                    canonical(EXPECTED_SCENE_EDITOR_DOCUMENT_TARGET) in {
                        canonical(owner) for owner in owners
                    }
                    and repository_tinygltf_target_allowed()
                ) or repository_demo_core_renderer_foundation_edge_allowed(
                    command,
                    owners,
                    operand,
                ) or repository_renderer_scene_transaction_foundation_edge_allowed(
                    command,
                    owners,
                    operand,
                )
            elif (
                family == "link"
                and operand in EXPECTED_DEMO_CORE_RENDERER_FOUNDATIONS
            ):
                allowed = (
                    repository_demo_core_renderer_foundation_edge_allowed(
                        command,
                        owners,
                        operand,
                    )
                    or repository_renderer_scene_transaction_foundation_edge_allowed(
                        command,
                        owners,
                        operand,
                    )
                )
            elif (
                family == "link"
                and operand in EXPECTED_DEMO_CORE_ANDROID_FOUNDATIONS
            ):
                allowed = (
                    repository_dear_imgui_android_edge_allowed(command, owners, operand)
                    or repository_demo_core_android_edge_allowed(command, owners, operand)
                )
            elif (
                family == "link"
                and operand in EXPECTED_DEMO_CORE_UNIX_FOUNDATIONS
            ):
                allowed = repository_demo_core_unix_edge_allowed(
                    command,
                    owners,
                    operand,
                )
            elif family == "link" and operand == "glfw":
                allowed = repository_dear_imgui_glfw_edge_allowed(
                    command,
                    owners,
                    operand,
                ) or repository_demo_core_glfw_edge_allowed(
                    command,
                    owners,
                    operand,
                )
            elif family == "link" and operand in {
                "${DEMO_METAL_FRAMEWORK}",
                "${DEMO_FOUNDATION_FRAMEWORK}",
            }:
                allowed = repository_demo_core_apple_framework_edge_allowed(command, owners, operand)
            elif operand.startswith("-"):
                allowed = not _is_link_artifact_operand(operand, previous) and not _is_unapproved_ui_operand(operand)
            else:
                allowed = (
                    candidate in known_targets
                    and candidate not in imported_targets
                    and repository_foundation_target_allowed(candidate)
                )
            if allowed:
                previous = operand
                continue
            identity = "Dear ImGui upstream identity" if operand == exact_dear_imgui_target else "dependency"
            raise ValidationError(
                f"production editor {identity} {operand!r} is outside allowedUiFoundation in "
                f"{command_label(command)}"
            )

    # Unknown command invocations and dynamic evaluation are rejected when an
    # editor/relevant target is an operand. Defined helper functions/macros that
    # affect dependency state were expanded above.
    declared_callables = {
        declaration[0].lower()
        for command in parsed_commands
        if command.name in {"function", "macro"}
        and (declaration := _tokenize_cmake(command.body))
    }
    known_commands = declared_callables | _TARGET_BOUNDARY_COMMANDS | {
        "add_custom_command",
        "add_custom_target",
        "add_executable",
        "add_library",
        "add_subdirectory",
        "add_test",
        "break",
        "cmake_file_api",
        "cmake_minimum_required",
        "cmake_parse_arguments",
        "continue",
        "configure_file",
        "else",
        "elseif",
        "enable_language",
        "endforeach",
        "endfunction",
        "endif",
        "endmacro",
        "endwhile",
        "execute_process",
        "externalproject_add",
        "fetchcontent_declare",
        "fetchcontent_makeavailable",
        "file",
        "find_file",
        "find_library",
        "find_package",
        "find_package_handle_standard_args",
        "find_path",
        "find_program",
        "foreach",
        "function",
        "get_cmake_property",
        "get_filename_component",
        "get_directory_property",
        "get_property",
        "get_source_file_property",
        "get_target_property",
        "get_test_property",
        "if",
        "include",
        "install",
        "list",
        "mark_as_advanced",
        "math",
        "message",
        "macro",
        "option",
        "pkg_check_modules",
        "project",
        "return",
        "set",
        "set_directory_properties",
        "set_property",
        "set_source_files_properties",
        "set_target_properties",
        "set_tests_properties",
        "string",
        "target_compile_definitions",
        "target_compile_features",
        "target_compile_options",
        "target_include_directories",
        "target_link_directories",
        "target_link_libraries",
        "target_link_options",
        "target_precompile_headers",
        "target_sources",
        "unset",
        "while",
    } | _GLOBAL_BOUNDARY_COMMANDS
    for command in commands:
        tokens, unresolved = expanded_by_command[command]
        mentions_relevant = any(canonical(token) in relevant_targets for token in tokens)
        relevant_scope = affects_relevant_directory(command)
        raw_tokens = _tokenize_cmake(command.body)
        unresolved_target_owner = (
            command.name in _TARGET_BOUNDARY_COMMANDS
            and raw_tokens
            and bool(_CMAKE_VARIABLE.search(raw_tokens[0]))
            and bool(unresolved - ALLOWED_TRANSITIVE_CMAKE_VARIABLES)
        )
        if unresolved_target_owner and relevant_scope:
            raise ValidationError(
                f"unsupported dynamic CMake target can affect an editor target in {command_label(command)}"
        )
        if command.name == "cmake_language" and relevant_scope:
            if command.path.resolve() == (root / "CMakeLists.txt").resolve():
                repository_tail_forms = (
                    tokens == ["DEFER", "CALL", "mgif_editor_dependency_tail_drain"]
                    or (
                        len(tokens) == 5
                        and tokens[0] == "DEFER"
                        and tokens[1] == "DIRECTORY"
                        and tokens[3:] == ["GET_CALL_IDS", "DEMO_EDITOR_PENDING_CALLS"]
                    )
                    or (
                        len(tokens) == 5
                        and tokens[0] == "DEFER"
                        and tokens[1] == "DIRECTORY"
                        and tokens[3:] == ["CALL", "mgif_editor_dependency_tail_drain"]
                    )
                )
                if repository_tail_forms:
                    continue
            if command.body.startswith(
                (
                    "unsupported CMake control flow ",
                    "unsupported external CMake traversal ",
                    "unsupported generated CMake traversal ",
                )
            ):
                raise ValidationError(command.body)
            raise ValidationError(
                f"unsupported dynamic CMake evaluation can affect an editor target in {command_label(command)}"
            )
        if command.name not in known_commands and (mentions_relevant or relevant_scope):
            raise ValidationError(f"unsupported CMake helper can affect an editor target in {command_label(command)}")
        if command.name in {"include", "add_subdirectory"} and unresolved and relevant_scope:
            missing = ", ".join(sorted(unresolved))
            raise ValidationError(f"cannot resolve repository CMake traversal ({missing}) in {command_label(command)}")

    strict_editor_files = _discover_editor_files(root)
    files_to_scan: dict[Path, bool] = {path.resolve(): False for path in strict_editor_files}
    for target, paths in sources_by_target.items():
        if target not in relevant_targets:
            continue
        for path in paths:
            resolved = path.resolve()
            allow_renderer_sentinel = target not in editor_owned_targets and resolved not in files_to_scan
            files_to_scan[resolved] = files_to_scan.get(resolved, True) and allow_renderer_sentinel
    for path, allow_renderer_sentinel in forced_files.items():
        files_to_scan[path] = files_to_scan.get(path, True) and allow_renderer_sentinel
    pinned_tinygltf_consumers = set()
    if repository_tinygltf_target_allowed():
        pinned_tinygltf_consumers = {
            (root / relative).resolve() for relative in EXPECTED_TINYGLTF_CONSUMERS
        }
    _scan_source_closure(
        root,
        files_to_scan,
        include_directories,
        pinned_tinygltf_consumers,
    )
    return {
        "editorAnchors": sorted(production_editor_targets),
        "protectedTargets": sorted(relevant_targets - custom_targets - imported_targets),
        "knownTargets": sorted(known_targets),
        "aliases": dict(sorted(aliases.items())),
        "objectLibraries": sorted(object_library_targets),
        "importedTargets": sorted(imported_targets),
        "repositoryExternalFoundations": (
            {EXPECTED_VOLK_TARGET: volk_foundation_authority}
            if volk_foundation_authority is not None
            else {}
        ),
    }


def validate_input_clipboard_index_data(data: Any) -> None:
    label = "input/clipboard reference index"
    require(isinstance(data, dict), f"{label} must be an object")
    require(type(data.get("schemaVersion")) is int and data["schemaVersion"] == 1, f"{label} schemaVersion must be 1")
    require(data.get("kind") == "mgif.editor-parity.input-clipboard-reference-index", f"{label} kind changed")
    require(data.get("oracleIds") == ["O05", "O06", "O07", "O08", "O10"], f"{label} oracle IDs changed")
    require(
        data.get("manifestOracleIds") == EXPECTED_ORACLE_IDS[4:8] + [EXPECTED_ORACLE_IDS[9]],
        f"{label} manifest oracle IDs changed",
    )

    reference = data.get("reference")
    require(isinstance(reference, dict), f"{label} reference must be an object")
    require(reference.get("commit") == EXPECTED_REFERENCE_COMMIT, f"{label} reference commit changed")
    require(reference.get("tree") == EXPECTED_REFERENCE_TREE, f"{label} reference tree changed")
    require(reference.get("headAndTreeVerifiedDirectly") is True, f"{label} must verify HEAD and tree directly")
    require(reference.get("sourceTreeDiffFileCount") == 0, f"{label} fixed reference source tree is dirty")

    evidence_root = data.get("externalEvidenceRoot")
    require(evidence_root == r"G:\ReferenceOracleRuns\vkgr-50373c7b-input-probe", f"{label} evidence root changed")
    expected_raw = {
        "sealedReport": ("REPORT.md", 16302, "8305d258858bbd72768a743cbbb305244e60ea8fb0415aa525ee483229dae856"),
        "checksumIndex": ("SHA256SUMS.txt", 6716, "0f691cbea80776aff10a95cb2cbcd7be6a4ec171837f25a7940faece560905a1"),
        "identity": ("identity.json", 3319, "4483652c5174c627298aa6699aeca59363d7d9c918cb3f90cb4382bb8c85732f"),
        "primaryTrace": ("trace.jsonl", 72534, "28a1f1aed76d5a07d121ed87e6370fd3f9ec570507592927d8b7c6e885f1c184"),
        "repeatTrace": ("trace-repeat.jsonl", 72534, "e280c87392e482e4ceb644052a16026784c9b87d2816249df66d9a3d63a37b07"),
        "probeExecutable": (r"bin\vkgr_p0_input_trace_probe.exe", 7391232, "e96024230565e6b1823879d39fa74a052e241ee2da47a03b707c855727744aa4"),
        "probeSource": (r"probe-source\p0_input_trace_probe.cpp", 43068, "0e13db85182a04f885de36a7589bb50ab59624808a2b559ab682c5f45b828842"),
        "traceValidation": ("trace-validation.txt", 359, "bef7151f3267752e218f139c1e9efcdf184b4e6fcc858e10216c01c2a42b841a"),
        "repeatCheck": ("repeat-check.txt", 374, "5df50d42b7fd892abf032f45038e379c3d87349e1711b03ef4c164a2ab9802bc"),
        "recordedCommands": ("COMMANDS.md", 2711, "730389701eb83802b63fa2f8ab946e10993477d24832c5cb43ac177499f9cf76"),
    }
    artifacts = data.get("rawArtifacts")
    require(isinstance(artifacts, list), f"{label} rawArtifacts must be an array")
    require([item.get("role") for item in artifacts if isinstance(item, dict)] == list(expected_raw), f"{label} raw artifact roles changed")
    for item in artifacts:
        role = item["role"]
        suffix, expected_bytes, expected_hash = expected_raw[role]
        require(item.get("path") == str(Path(evidence_root) / suffix), f"{label} {role} path changed")
        require(item.get("bytes") == expected_bytes, f"{label} {role} byte count changed")
        require(normalized_sha256(item.get("sha256"), f"{label} {role}") == expected_hash, f"{label} {role} hash changed")

    integrity = data.get("integrity")
    require(isinstance(integrity, dict), f"{label} integrity must be an object")
    require(integrity.get("checksumIndexEntries") == 68, f"{label} must retain 68 checksum entries")
    require(integrity.get("checksumIndexParsedEntries") == 68, f"{label} must parse all 68 checksum entries")
    require(integrity.get("missingFiles") == 0, f"{label} reports missing files")
    require(integrity.get("hashMismatches") == 0, f"{label} reports hash mismatches")
    require(integrity.get("allIndexedFilesMatch") is True, f"{label} checksum index is not closed")
    require(integrity.get("frozenReferenceSourcesComparedToWorktree") == 7, f"{label} frozen source comparison changed")
    require(integrity.get("frozenReferenceSourceMismatches") == 0, f"{label} frozen source mismatch")

    coverage = data.get("coverage")
    require(isinstance(coverage, dict), f"{label} coverage must be an object")
    case_ids = coverage.get("caseIds")
    require(isinstance(case_ids, list) and len(case_ids) == 30, f"{label} must retain 30 validated cases")
    require(len(case_ids) == len(set(case_ids)), f"{label} case IDs must be unique")
    require(coverage.get("validatedCaseIdCount") == 30, f"{label} validated case count changed")

    summary = data.get("recordSummary")
    require(isinstance(summary, dict), f"{label} recordSummary must be an object")
    for run_name in ("primary", "repeat"):
        run = summary.get(run_name)
        require(isinstance(run, dict), f"{label} {run_name} summary must be an object")
        require(run.get("records") == 144, f"{label} {run_name} must contain 144 records")
        require(run.get("jsonParseErrors") == 0, f"{label} {run_name} has JSON parse errors")
        require(run.get("assertions") == 40, f"{label} {run_name} must contain 40 assertions")
        require(run.get("assertionFailures") == 0, f"{label} {run_name} has assertion failures")
        require(run.get("summaryExitCode") == 0, f"{label} {run_name} summary failed")
        require(run.get("stderrBytes") == 0, f"{label} {run_name} stderr is not empty")
    require(summary.get("assertionsByOracle") == {"O05": 4, "O06": 8, "O07": 11, "O08": 12, "O10": 5}, f"{label} assertion allocation changed")
    tests = summary.get("existingTests")
    require(isinstance(tests, dict), f"{label} existingTests must be an object")
    require(
        {key: tests.get(key) for key in ("registered", "passed", "skipped", "failed", "exitCode")}
        == {"registered": 123, "passed": 43, "skipped": 80, "failed": 0, "exitCode": 0},
        f"{label} existing test summary changed",
    )

    repeatability = data.get("repeatability")
    require(isinstance(repeatability, dict), f"{label} repeatability must be an object")
    require(repeatability.get("normalizedEqual") is True, f"{label} normalized runs differ")
    require(repeatability.get("independentStructuralNormalizationDifferentRecords") == 0, f"{label} independent normalization differs")
    require(
        normalized_sha256(repeatability.get("sealedNormalizedPrimarySha256"), f"{label} primary normalized hash")
        == EXPECTED_INPUT_NORMALIZED_SHA256,
        f"{label} primary normalized hash changed",
    )
    require(
        normalized_sha256(repeatability.get("sealedNormalizedRepeatSha256"), f"{label} repeat normalized hash")
        == EXPECTED_INPUT_NORMALIZED_SHA256,
        f"{label} repeat normalized hash changed",
    )

    scope = data.get("evidenceScope")
    require(isinstance(scope, dict), f"{label} evidenceScope must be an object")
    require(scope.get("fullInteractiveApplicationObservation") is False, f"{label} must not claim full interactive application observation")
    require(set(scope.get("grades", {})) == {
        "EXECUTED_SOURCE_LEVEL_PROBE",
        "EXECUTED_FIXED_CORE_PLUS_SOURCE_ORDER_MODEL",
        "EXECUTED_FIXED_PARSER_PLUS_SOURCE_UI_MODEL",
    }, f"{label} evidence grades changed")
    require(data.get("osClipboard", {}).get("primaryRestored") is True, f"{label} primary clipboard was not restored")
    require(data.get("osClipboard", {}).get("repeatRestored") is True, f"{label} repeat clipboard was not restored")
    require(data.get("safetyDecision", {}).get("id") == "D16", f"{label} D16 safety decision is missing")
    require(isinstance(data.get("runtimeGaps"), list) and len(data["runtimeGaps"]) >= 5, f"{label} runtime gaps are incomplete")


def validate_ui_runtime_index_data(data: Any) -> None:
    label = "native UI/Vulkan reference index"
    require(isinstance(data, dict), f"{label} must be an object")
    require(
        set(data)
        == {
            "schemaVersion",
            "schema",
            "indexId",
            "evidenceKind",
            "sealedEvidenceRoot",
            "repositoryMirrorReport",
            "largeArtifactsCopiedIntoMgif",
            "copiedArtifacts",
            "reference",
            "sourceEvidence",
            "integrity",
            "dependencyBoundary",
            "coverageCeiling",
            "screenshots",
        },
        f"{label} top-level fields changed",
    )
    require(data.get("schemaVersion") == 1, f"{label} schemaVersion must be 1")
    require(
        data.get("schema") == "mgif.editor-parity.native-ui-vulkan-reference-index",
        f"{label} schema changed",
    )
    require(data.get("indexId") == "UI-RUNTIME", f"{label} index ID changed")
    require(
        data.get("evidenceKind") == "external-reference-pointer-mirror",
        f"{label} evidence kind changed",
    )
    require(data.get("sealedEvidenceRoot") == UI_RUNTIME_EVIDENCE_ROOT, f"{label} sealed evidence root changed")
    require(
        data.get("repositoryMirrorReport") == "docs/editor-parity/reference-traces/UI-RUNTIME-REPORT.md",
        f"{label} repository report binding changed",
    )
    require(data.get("largeArtifactsCopiedIntoMgif") is False, f"{label} cannot claim copied large artifacts")
    require(data.get("copiedArtifacts") == [], f"{label} copied artifact list must remain empty")

    reference = data.get("reference")
    require(isinstance(reference, dict), f"{label} reference must be an object")
    require(set(reference) == set(EXPECTED_UI_RUNTIME_REFERENCE), f"{label} reference identity roles changed")
    for role, expected in EXPECTED_UI_RUNTIME_REFERENCE.items():
        require(json_values_equal(reference.get(role), expected), f"{label} locked {role} identity changed")

    source_evidence = data.get("sourceEvidence")
    require(isinstance(source_evidence, dict), f"{label} sourceEvidence must be an object")
    require(
        set(source_evidence) == set(EXPECTED_UI_RUNTIME_SOURCE_EVIDENCE),
        f"{label} sealed source evidence roles changed",
    )
    for role, (relative_path, expected_hash) in EXPECTED_UI_RUNTIME_SOURCE_EVIDENCE.items():
        expected_path = UI_RUNTIME_EVIDENCE_ROOT + "\\" + relative_path.replace("/", "\\")
        require(
            json_values_equal(source_evidence.get(role), {"path": expected_path, "sha256": expected_hash}),
            f"{label} sealed source evidence identity changed: {role}",
        )

    integrity = data.get("integrity")
    require(isinstance(integrity, dict), f"{label} integrity must be an object")
    require(
        json_values_equal(integrity.get("exports"), {
            "expected": 1778,
            "verified": 1778,
            "exactByteMatches": 201,
            "gitCleanNormalizedMatches": 1577,
            "missing": 0,
            "mismatched": 0,
            "extra": 0,
            "passed": True,
        }),
        f"{label} export integrity summary changed",
    )
    require(
        json_values_equal(integrity.get("artifactHashes"), {
            "expected": 7104,
            "verified": 7104,
            "missing": 0,
            "mismatched": 0,
            "malformedManifestEntries": 0,
            "passed": True,
        }),
        f"{label} artifact hash integrity summary changed",
    )
    require(
        json_values_equal(integrity.get("screenshots"), {
            "expected": 23,
            "indexed": 23,
            "primary": 19,
            "superseded": 4,
            "nonBlank": 23,
            "passed": True,
        }),
        f"{label} screenshot integrity summary changed",
    )
    require(
        integrity.get("verificationMode")
        == "read-only recalculation and field comparison against the external sealed root",
        f"{label} integrity verification mode changed",
    )
    require(
        set(integrity) == {"exports", "artifactHashes", "screenshots", "verificationMode"},
        f"{label} integrity fields changed",
    )

    dependency_boundary = data.get("dependencyBoundary")
    require(
        json_values_equal(dependency_boundary, {
            "externalReferenceProvenanceOnly": ["nvpro_core2", "nvcore", "nvapp", "nvgui", "nvvk", "nvvkgltf"],
            "forbiddenMgifProductionDependencies": ["nvpro_core2", "nvcore", "nvapp", "nvgui", "nvvk", "nvvkgltf"],
            "mgifArchitecture": "Direct Dear ImGui plus MGIF-owned app, RHI, and scene abstractions.",
            "copyLinkAdaptOrIntroduceReferenceInterfacesAuthorized": False,
        }),
        f"{label} dependency boundary changed",
    )

    coverage = data.get("coverageCeiling")
    require(isinstance(coverage, dict), f"{label} coverageCeiling must be an object")
    require(
        set(coverage) == {*EXPECTED_UI_RUNTIME_COVERAGE, "d20"},
        f"{label} coverage ceiling scopes changed",
    )
    for scope_id, (expected_status, expected_detail) in EXPECTED_UI_RUNTIME_COVERAGE.items():
        scope = coverage.get(scope_id)
        require(isinstance(scope, dict), f"{label} {scope_id} coverage must be an object")
        require(set(scope) == {"status", "detail"}, f"{label} {scope_id} coverage fields changed")
        require(scope.get("status") == expected_status, f"{label} {scope_id} must remain {expected_status}")
        require(scope.get("detail") == expected_detail, f"{label} {scope_id} coverage detail changed")

    d20 = coverage.get("d20")
    require(isinstance(d20, dict), f"{label} D20 coverage must be an object")
    require(
        set(d20)
        == {
            "totalCells",
            "realUiCellsReplayed",
            "sourceOrCpuEvidenceUpgradedToUiGpu",
            "policySelected",
            "selectedPolicy",
            "candidatePolicies",
        },
        f"{label} D20 coverage fields changed",
    )
    require(type(d20.get("totalCells")) is int and d20["totalCells"] == 298, f"{label} D20 total must remain 298")
    require(
        type(d20.get("realUiCellsReplayed")) is int and d20["realUiCellsReplayed"] == 0,
        f"{label} D20 real UI replay count must remain zero",
    )
    require(
        d20.get("sourceOrCpuEvidenceUpgradedToUiGpu") is False,
        f"{label} D20 source/CPU evidence cannot be upgraded to UI/GPU evidence",
    )
    require(d20.get("policySelected") is False, f"{label} D20 policy must remain pending")
    require(d20.get("selectedPolicy") is None, f"{label} D20 cannot select a policy")
    require(
        d20.get("candidatePolicies") == ["exact_defect_parity", "mgif_safety_exception"],
        f"{label} D20 candidate policies changed",
    )

    screenshots = data.get("screenshots")
    require(isinstance(screenshots, dict), f"{label} screenshots must be an object")
    require(
        set(screenshots) == {"primaryCount", "supersededCount", "allExternal", "primary", "superseded"},
        f"{label} screenshot fields changed",
    )
    require(
        type(screenshots.get("primaryCount")) is int and screenshots["primaryCount"] == 19,
        f"{label} must retain 19 primary screenshots",
    )
    require(
        type(screenshots.get("supersededCount")) is int and screenshots["supersededCount"] == 4,
        f"{label} must retain 4 superseded screenshots",
    )
    require(screenshots.get("allExternal") is True, f"{label} screenshots must remain external")
    primary = screenshots.get("primary")
    superseded = screenshots.get("superseded")
    require(isinstance(primary, list) and len(primary) == 19, f"{label} must contain 19 primary screenshot rows")
    require(
        isinstance(superseded, list) and len(superseded) == 4,
        f"{label} must contain 4 superseded screenshot rows",
    )
    for kind, rows, expected_rows in (
        ("primary", primary, EXPECTED_UI_RUNTIME_PRIMARY_SCREENSHOTS),
        ("superseded", superseded, EXPECTED_UI_RUNTIME_SUPERSEDED_SCREENSHOTS),
    ):
        files: list[str] = []
        for index, row in enumerate(rows):
            require(isinstance(row, dict), f"{label} {kind} screenshot row {index} must be an object")
            require(
                set(row) == {"file", "sha256", "state", "grade", "runtime", "processId"},
                f"{label} {kind} screenshot row {index} fields changed",
            )
            file_name = row.get("file")
            require(isinstance(file_name, str) and file_name in expected_rows, f"{label} {kind} screenshot identity changed")
            require(
                normalized_sha256(row.get("sha256"), f"{label} {kind} screenshot {file_name}")
                == expected_rows[file_name],
                f"{label} {kind} screenshot hash changed: {file_name}",
            )
            require(isinstance(row.get("state"), str) and row["state"], f"{label} {kind} screenshot state is missing")
            if kind == "primary":
                require(
                    row.get("grade") in {"primary-native-ui-vulkan", "primary-native-ui-vulkan-gpu-correlated"},
                    f"{label} primary screenshot grade changed: {file_name}",
                )
            else:
                require(row.get("grade") == "superseded-not-citable", f"{label} superseded screenshot became citable")
            files.append(file_name)
        require(files == list(expected_rows), f"{label} {kind} screenshot inventory or order changed")
    primary_files = {row["file"] for row in primary}
    superseded_files = {row["file"] for row in superseded}
    require(not (primary_files & superseded_files), f"{label} primary and superseded screenshot identities overlap")
    screenshot_metadata = json.dumps(
        screenshots,
        ensure_ascii=True,
        sort_keys=True,
        separators=(",", ":"),
    ).encode("utf-8")
    require(
        hashlib.sha256(screenshot_metadata).hexdigest() == EXPECTED_UI_RUNTIME_SCREENSHOT_METADATA_SHA256,
        f"{label} locked screenshot metadata changed",
    )


def validate_d20_trace_index_data(data: Any) -> None:
    label = "D20 hierarchy reference index"
    require(isinstance(data, dict), f"{label} must be an object")
    require(data.get("schemaVersion") == 1, f"{label} schemaVersion must be 1")
    require(data.get("schema") == "mgif.editor-parity.d20-hierarchy-reference-index", f"{label} schema changed")
    require(data.get("indexId") == "D20-HIERARCHY", f"{label} index ID changed")
    require(data.get("oracleIds") == ["O01", "O02", "O03", "O04", "O09"], f"{label} oracle IDs changed")
    reference = data.get("reference")
    require(isinstance(reference, dict), f"{label} reference must be an object")
    require(reference.get("commit") == EXPECTED_REFERENCE_COMMIT, f"{label} reference commit changed")
    require(reference.get("tree") == EXPECTED_REFERENCE_TREE, f"{label} reference tree changed")
    require(data.get("evidenceRoot") == r"G:\ReferenceOracleRuns\vkgr-50373c7b-d20-probe", f"{label} evidence root changed")

    scope = data.get("scope")
    require(
        scope == {
            "domainExecution": True,
            "exactUiCommandEnvelopes": True,
            "freshProcessPerCase": True,
            "imguiRuntime": False,
            "vulkanRuntime": False,
            "gpuRuntime": False,
        },
        f"{label} scope flags changed",
    )
    runtime_gaps = data.get("runtimeGaps")
    require(isinstance(runtime_gaps, list) and len(runtime_gaps) == 3, f"{label} must retain three runtime gaps")
    require(all(item.get("status") == "not_executed" for item in runtime_gaps), f"{label} runtime gaps must remain not_executed")

    totals = data.get("totals")
    require(isinstance(totals, dict), f"{label} totals must be an object")
    require(
        {key: totals.get(key) for key in ("cases", "passed", "processCrashes", "timeouts")}
        == {"cases": 304, "passed": 302, "processCrashes": 2, "timeouts": 0},
        f"{label} execution totals changed",
    )
    require(totals.get("orientations") == {"S0": 155, "S1": 149}, f"{label} orientation totals changed")
    require(totals.get("processExitCodes") == {"0x00000000": 302, "0xC00000FD": 2}, f"{label} process exit totals changed")

    verification = data.get("verification")
    require(isinstance(verification, dict), f"{label} verification must be an object")
    sealed = verification.get("sealedHashList")
    require(isinstance(sealed, dict), f"{label} sealedHashList must be an object")
    require(
        {key: sealed.get(key) for key in ("entries", "filesFound", "byteMismatches", "sha256Mismatches")}
        == {"entries": 626, "filesFound": 626, "byteMismatches": 0, "sha256Mismatches": 0},
        f"{label} sealed hash verification changed",
    )
    raw = verification.get("rawTraces")
    require(isinstance(raw, dict), f"{label} rawTraces must be an object")
    require(
        {key: raw.get(key) for key in ("files", "uniquePaths", "passedWithCaseResult", "crashedWithoutCaseResult", "timeouts")}
        == {"files": 304, "uniquePaths": 304, "passedWithCaseResult": 302, "crashedWithoutCaseResult": 2, "timeouts": 0},
        f"{label} raw trace verification changed",
    )
    for flag in ("checkpointNamesMatchRawTrace", "caseIdsUnique", "o09CaseIdsEqualD20CaseIds"):
        require(raw.get(flag) is True, f"{label} {flag} must be true")

    registry = data.get("caseRegistry")
    require(isinstance(registry, list) and len(registry) == 304, f"{label} must contain 304 case registry rows")
    case_ids = [row.get("caseId") for row in registry if isinstance(row, dict)]
    require(len(case_ids) == 304 and len(set(case_ids)) == 304, f"{label} case IDs must be complete and unique")
    require(sum(row.get("orientation") == "S0" for row in registry) == 155, f"{label} S0 registry count changed")
    require(sum(row.get("orientation") == "S1" for row in registry) == 149, f"{label} S1 registry count changed")
    require(all(row.get("timedOut") is False for row in registry), f"{label} contains a timed-out case")
    crash_rows = [row for row in registry if row.get("processExitCodeHex") == "0xC00000FD"]
    require({row.get("caseId") for row in crash_rows} == EXPECTED_D20_CRASH_CASE_IDS, f"{label} crash case IDs changed")
    require(all(row.get("processExitCode") == -1073741571 for row in crash_rows), f"{label} stack-overflow exit code changed")
    require(all(row.get("caseResultPassed") is None for row in crash_rows), f"{label} crash cases cannot claim case_result pass")
    require(
        all(row.get("executionStatus") == "process_crash_stack_overflow_0xC00000FD" for row in crash_rows),
        f"{label} stack-overflow execution status changed",
    )
    passed_rows = [row for row in registry if row.get("processExitCodeHex") == "0x00000000"]
    require(len(passed_rows) == 302, f"{label} must retain 302 passed rows")
    require(all(row.get("caseResultPassed") is True for row in passed_rows), f"{label} a zero-exit case lacks a passing result")
    require(all(row.get("executionStatus") == "passed" for row in passed_rows), f"{label} zero-exit execution status changed")
    for row in registry:
        raw_trace = row.get("rawTrace")
        require(isinstance(raw_trace, dict), f"{label} case {row.get('caseId')} lacks rawTrace")
        require(isinstance(raw_trace.get("bytes"), int) and raw_trace["bytes"] > 0, f"{label} case {row.get('caseId')} has invalid trace bytes")
        normalized_sha256(raw_trace.get("sha256"), f"{label} case {row.get('caseId')} raw trace")
        require(Path(raw_trace.get("path", "")).name == f"{row.get('caseId')}.jsonl", f"{label} case {row.get('caseId')} trace path changed")
        require(isinstance(row.get("checkpointNames"), list) and row["checkpointNames"], f"{label} case {row.get('caseId')} lacks checkpoints")

    o09 = data.get("o09Coverage")
    require(isinstance(o09, dict), f"{label} o09Coverage must be an object")
    require(o09.get("status") == "partial", f"{label} O09 must remain partial at this evidence ceiling")
    require(
        {key: o09.get(key) for key in ("matrixCellCount", "executedCellCount", "notExecutedCellCount", "processCrashCellCount")}
        == {"matrixCellCount": 298, "executedCellCount": 298, "notExecutedCellCount": 0, "processCrashCellCount": 2},
        f"{label} O09 coverage totals changed",
    )
    require(o09.get("requirementTotals") == EXPECTED_D20_REQUIREMENT_TOTALS, f"{label} O09 requirement totals changed")
    cells = o09.get("cells")
    require(isinstance(cells, list) and len(cells) == 298, f"{label} must contain 298 O09 cells")
    cell_ids = [cell.get("caseId") for cell in cells if isinstance(cell, dict)]
    expected_d20_ids = {case_id for case_id in case_ids if isinstance(case_id, str) and case_id.startswith("D20_")}
    require(len(cell_ids) == 298 and set(cell_ids) == expected_d20_ids, f"{label} O09 cells do not equal D20 case registry")
    registry_by_id = {row["caseId"]: row for row in registry if row.get("caseId") in expected_d20_ids}
    requirement_prefixes = (
        ("scene_switch_", "D20-SW"),
        ("row_add_node_", "D20-RN"),
        ("row_add_", "D20-RL"),
        ("add_child_empty_", "D20-N-AC-E"),
        ("add_child_point_light_", "D20-N-AC-P"),
        ("add_child_directional_light_", "D20-N-AC-D"),
        ("add_child_spot_light_", "D20-N-AC-S"),
        ("duplicate_", "D20-N-DUP"),
        ("delete_", "D20-N-DEL"),
        ("rename_", "D20-N-REN"),
        ("root_drop_", "D20-RD"),
        ("node_drop_", "D20-DD"),
    )
    source_envelopes = {
        "D20-SW": ("partial_ui_command_envelope", "src/ui_renderer.cpp:374-390"),
        "D20-RN": ("exact_ui_domain_call", "src/ui_scene_browser.cpp:418-432"),
        "D20-RL": ("exact_ui_command_envelope", "src/ui_scene_browser.cpp:435-453"),
        "D20-N-AC-E": ("exact_ui_command_envelope", "src/ui_scene_browser.cpp:1432-1441"),
        "D20-N-AC-P": ("exact_ui_command_envelope", "src/ui_scene_browser.cpp:1432-1461"),
        "D20-N-AC-D": ("exact_ui_command_envelope", "src/ui_scene_browser.cpp:1432-1461"),
        "D20-N-AC-S": ("exact_ui_command_envelope", "src/ui_scene_browser.cpp:1432-1461"),
        "D20-N-DUP": ("exact_ui_command_envelope", "src/ui_scene_browser.cpp:1410-1419"),
        "D20-N-DEL": ("exact_ui_command_envelope", "src/ui_renderer.cpp:501-524"),
        "D20-N-REN": ("exact_ui_command_envelope", "src/ui_scene_browser.cpp:1538-1586"),
        "D20-RD": ("exact_ui_command_envelope", "src/ui_scene_browser.cpp:459-470"),
        "D20-DD": ("exact_ui_command_envelope", "src/ui_scene_browser.cpp:752-771"),
    }
    source_ui_branches = {
        "D20-SW": "Settings/Multiple Scenes radio -> Scene::setCurrentScene; Vulkan rebuild omitted",
        "D20-RN": "UiSceneBrowser scene context Add Node; clicked sceneID is ignored and no command is created",
        "D20-RL": "UiSceneBrowser scene-row context Add Light; clicked sceneID ignored",
        "D20-N-AC-E": "UiSceneBrowser node context Add Child/Empty Node",
        "D20-N-AC-P": "UiSceneBrowser node context Add Child/Light",
        "D20-N-AC-D": "UiSceneBrowser node context Add Child/Light",
        "D20-N-AC-S": "UiSceneBrowser node context Add Child/Light",
        "D20-N-DUP": "UiSceneBrowser node context Duplicate",
        "D20-N-DEL": "GltfRenderer::renderUI DeleteConfirmation confirmed",
        "D20-N-REN": "UiSceneBrowser Rename modal commitRename -> RenameNodeCommand",
        "D20-RD": "UiSceneBrowser scene-row drop; clicked sceneID is ignored",
        "D20-DD": "UiSceneBrowser::renderNodeHierarchy node-to-node drop cycle preflight",
    }
    dimension_fields = {
        "D20-SW": {"orientation", "action"},
        "D20-RN": {"orientation", "action", "clickedRow", "history"},
        "D20-RL": {"orientation", "action", "clickedRow", "lightType", "history"},
        "D20-N-AC-E": {"orientation", "action", "targetCategory", "history"},
        "D20-N-AC-P": {"orientation", "action", "targetCategory", "lightType", "history"},
        "D20-N-AC-D": {"orientation", "action", "targetCategory", "lightType", "history"},
        "D20-N-AC-S": {"orientation", "action", "targetCategory", "lightType", "history"},
        "D20-N-DUP": {"orientation", "action", "targetCategory", "history"},
        "D20-N-DEL": {"orientation", "action", "targetCategory", "history"},
        "D20-N-REN": {"orientation", "action", "targetCategory", "history"},
        "D20-RD": {"orientation", "action", "sourceCategory", "clickedRow", "history"},
        "D20-DD": {"orientation", "action", "sourceCategory", "targetCategory", "history"},
    }
    dimension_actions = {
        "D20-SW": "scene_switch_roundtrip",
        "D20-RN": "row_add_node",
        "D20-RL": "row_add_light",
        "D20-N-AC-E": "add_child_empty",
        "D20-N-AC-P": "add_child_light",
        "D20-N-AC-D": "add_child_light",
        "D20-N-AC-S": "add_child_light",
        "D20-N-DUP": "duplicate",
        "D20-N-DEL": "delete",
        "D20-N-REN": "rename",
        "D20-RD": "drop_to_scene_root_row",
        "D20-DD": "node_to_node_drop",
    }
    source_argument_fields = {
        "D20-SW": {"fromScene", "toScene"},
        "D20-RN": {"call", "clickedSceneRow", "currentSceneAtCall"},
        "D20-RL": {"clickedSceneRow", "currentSceneAtExecute", "lightType", "name", "parentIndex", "rowCategory", "switchBeforeUndo"},
        "D20-N-AC-E": {"explicitParent", "name", "switchBeforeUndo", "targetCategory"},
        "D20-N-AC-P": {"explicitParent", "lightType", "name", "switchBeforeUndo", "targetCategory"},
        "D20-N-AC-D": {"explicitParent", "lightType", "name", "switchBeforeUndo", "targetCategory"},
        "D20-N-AC-S": {"explicitParent", "lightType", "name", "switchBeforeUndo", "targetCategory"},
        "D20-N-DUP": {"currentParentFromCurrentMap", "source", "switchBeforeUndo", "targetCategory"},
        "D20-N-DEL": {"currentSceneAtExecute", "switchBeforeUndo", "targetCategory", "targetNode"},
        "D20-N-REN": {"commit", "newName", "oldName", "switchBeforeUndo", "target", "targetCategory"},
        "D20-RD": {"clickedSceneRow", "newParent", "oldParentFromCurrentMap", "rowCategory", "source", "sourceCategory", "switchBeforeUndo"},
        "D20-DD": {"oldParentFromCurrentMap", "source", "sourceCategory", "switchBeforeUndo", "target", "targetCategory", "wouldCreateCycleResult"},
    }
    actual_requirement_totals: dict[str, int] = {}
    actual_crashes = 0
    for cell in cells:
        require(isinstance(cell, dict), f"{label} O09 cell must be an object")
        case_id = cell.get("caseId")
        row = registry_by_id[case_id]
        require(
            set(cell)
            == {
                "caseId",
                "requirement",
                "dimensions",
                "outcome",
                "status",
                "exit",
                "rawTraceSha256",
                "sourceEnvelope",
                "uiRuntimeDriven",
            },
            f"{label} O09 cell {case_id} fields changed",
        )
        case_suffix = re.sub(r"^D20_S[01]_", "", case_id)
        expected_requirement = next(
            (requirement for prefix, requirement in requirement_prefixes if case_suffix.startswith(prefix)),
            None,
        )
        require(expected_requirement is not None, f"{label} O09 case {case_id} has an unknown requirement envelope")
        requirement = cell.get("requirement")
        require(requirement == expected_requirement, f"{label} O09 case {case_id} requirement classification changed")
        actual_requirement_totals[requirement] = actual_requirement_totals.get(requirement, 0) + 1
        require(cell.get("status") == "executed_domain_with_exact_ui_envelope", f"{label} O09 case {case_id} status changed")
        require(cell.get("outcome") == row.get("executionStatus"), f"{label} O09 case {case_id} outcome differs from registry")
        require(
            cell.get("exit")
            == {
                "processExitCode": row.get("processExitCode"),
                "processExitCodeHex": row.get("processExitCodeHex"),
                "timedOut": row.get("timedOut"),
            },
            f"{label} O09 case {case_id} exit differs from registry",
        )
        require(
            normalized_sha256(cell.get("rawTraceSha256"), f"{label} O09 case {case_id} raw trace")
            == normalized_sha256(row["rawTrace"].get("sha256"), f"{label} registry case {case_id} raw trace"),
            f"{label} O09 case {case_id} raw trace hash differs from registry",
        )
        dimensions = cell.get("dimensions")
        require(
            isinstance(dimensions, dict)
            and set(dimensions) == dimension_fields[requirement]
            and dimensions.get("orientation") == row.get("orientation"),
            f"{label} O09 case {case_id} orientation differs from registry",
        )
        require(
            dimensions.get("action") == dimension_actions[requirement],
            f"{label} O09 case {case_id} action classification changed",
        )
        envelope = cell.get("sourceEnvelope")
        require(
            isinstance(envelope, dict)
            and set(envelope) == {"evidenceKind", "source", "uiBranch", "arguments", "limitation"},
            f"{label} O09 case {case_id} source envelope fields changed",
        )
        expected_kind, expected_source = source_envelopes[requirement]
        require(
            (envelope.get("evidenceKind"), envelope.get("source")) == (expected_kind, expected_source),
            f"{label} O09 case {case_id} source envelope classification changed",
        )
        require(
            envelope.get("uiBranch") == source_ui_branches[requirement],
            f"{label} O09 case {case_id} UI branch changed",
        )
        require(
            isinstance(envelope.get("arguments"), dict)
            and set(envelope["arguments"]) == source_argument_fields[requirement],
            f"{label} O09 case {case_id} source argument envelope changed",
        )
        arguments = envelope["arguments"]
        if "history" in dimensions and dimensions["history"] != "non_history_action":
            require(
                arguments.get("switchBeforeUndo") is (dimensions["history"] == "switch_before_undo"),
                f"{label} O09 case {case_id} history/source argument classification changed",
            )
        for category in ("sourceCategory", "targetCategory"):
            if category in dimensions and category in arguments:
                require(
                    dimensions[category] == arguments[category],
                    f"{label} O09 case {case_id} {category} differs from source arguments",
                )
        if "lightType" in dimensions:
            require(
                dimensions["lightType"] == arguments.get("lightType"),
                f"{label} O09 case {case_id} light type differs from source arguments",
            )
        if requirement in {"D20-RL", "D20-RD"}:
            require(
                dimensions["clickedRow"] == arguments["rowCategory"],
                f"{label} O09 case {case_id} clicked row differs from source arguments",
            )
        if requirement == "D20-SW":
            expected_from = 0 if dimensions["orientation"] == "S0" else 1
            require(
                arguments == {"fromScene": expected_from, "toScene": 1 - expected_from},
                f"{label} O09 case {case_id} scene-switch arguments changed",
            )
        if requirement == "D20-SW":
            require(isinstance(envelope.get("limitation"), str) and envelope["limitation"].strip(), f"{label} O09 scene-switch limitation is missing")
        else:
            require(envelope.get("limitation") is None, f"{label} O09 case {case_id} invented a source limitation")
        require(cell.get("uiRuntimeDriven") is False, f"{label} O09 case {case_id} must not claim UI runtime")
        actual_crashes += int(row.get("processExitCodeHex") == "0xC00000FD")
    require(actual_requirement_totals == EXPECTED_D20_REQUIREMENT_TOTALS, f"{label} O09 per-cell requirement totals changed")
    require(actual_crashes == o09.get("processCrashCellCount"), f"{label} O09 per-cell crash total changed")

    critical = data.get("criticalEvidence")
    require(isinstance(critical, dict), f"{label} criticalEvidence must be an object")
    critical_crashes = critical.get("crashCases")
    require(isinstance(critical_crashes, list), f"{label} crashCases must be an array")
    require({row.get("caseId") for row in critical_crashes} == EXPECTED_D20_CRASH_CASE_IDS, f"{label} critical crash IDs changed")
    b0 = critical.get("b0DeleteImmediate", {})
    require(b0.get("caseId") == "D20_S0_delete_noncurrent_root_immediate", f"{label} B0 immediate case changed")
    checkpoints = b0.get("checkpoints")
    require(isinstance(checkpoints, list) and [row.get("name") for row in checkpoints] == ["before", "execute", "undo", "redo"], f"{label} B0 checkpoints changed")
    scene1_roots = []
    for checkpoint in checkpoints:
        scene1 = next((row for row in checkpoint.get("roots", []) if row.get("scene") == 1), None)
        require(scene1 is not None, f"{label} B0 checkpoint lacks Scene 1 roots")
        scene1_roots.append(scene1.get("names"))
    require(
        scene1_roots == [["B0", "B1", "SharedRoot"], ["B1", "SharedRoot"], ["B0", "B1"], ["B1"]],
        f"{label} B0 destructive root transition changed",
    )
    o04 = critical.get("oracles", {}).get("O04", {})
    require(o04.get("sceneGraphRevisionTransition") == [2, 3, 4, 5], f"{label} O04 revision conflict changed")
    require(len(set(o04.get("runtimeSha256Transition", []))) == 4, f"{label} O04 runtime hashes must change at every checkpoint")
    require(o04.get("deleteModalExecuted") is False, f"{label} O04 must not claim modal execution")

    decision = data.get("decisionGate")
    require(isinstance(decision, dict), f"{label} decisionGate must be an object")
    require(decision.get("status") == "pending_user_decision", f"{label} must remain pending_user_decision")
    require(decision.get("selectedPolicy") is None, f"{label} cannot select the user's D20 policy")
    require(decision.get("allowedPolicies") == ["exact_defect_parity", "mgif_safety_exception"], f"{label} D20 policies changed")


def verify_external_artifact(record: Any, label: str) -> None:
    require(isinstance(record, dict), f"{label} must be an object")
    path_text = record.get("path")
    require(isinstance(path_text, str) and path_text, f"{label}.path must be a non-empty string")
    path = Path(path_text)
    require(path.is_file(), f"{label} is missing: {path}")
    require(path.stat().st_size == record.get("bytes"), f"{label} byte count mismatch")
    require(sha256(path) == normalized_sha256(record.get("sha256"), label), f"{label} SHA-256 mismatch")


def verify_input_checksum_index(data: dict[str, Any]) -> None:
    root = Path(data["externalEvidenceRoot"]).resolve()
    checksum_record = next(item for item in data["rawArtifacts"] if item["role"] == "checksumIndex")
    checksum_path = Path(checksum_record["path"])
    entries: list[tuple[str, Path]] = []
    for line_number, line in enumerate(checksum_path.read_text(encoding="utf-8").splitlines(), start=1):
        match = re.fullmatch(r"([0-9a-fA-F]{64})\s{2}(.+)", line)
        require(match is not None, f"input checksum index line {line_number} is malformed")
        relative = PurePosixPath(match.group(2))
        require(not relative.is_absolute() and ".." not in relative.parts, f"input checksum index line {line_number} escapes the evidence root")
        path = root.joinpath(*relative.parts).resolve()
        try:
            path.relative_to(root)
        except ValueError as exc:
            raise ValidationError(f"input checksum index line {line_number} escapes the evidence root") from exc
        entries.append((match.group(1).lower(), path))
    require(len(entries) == 68 and len({path for _, path in entries}) == 68, "input checksum index must contain 68 unique entries")
    for expected_hash, path in entries:
        require(path.is_file(), f"input checksum artifact is missing: {path}")
        require(sha256(path) == expected_hash, f"input checksum artifact hash mismatch: {path}")


def verify_d20_checksum_index(data: dict[str, Any]) -> None:
    checksum_path = Path(data["artifacts"]["rawHashList"]["path"])
    allowed_roots = [Path(data["evidenceRoot"]).resolve(), Path(data["reference"]["sourceRoot"]).resolve()]
    entries: list[tuple[str, int, Path]] = []
    lines = checksum_path.read_text(encoding="utf-8").splitlines()
    require(lines and lines[0] == "# SHA-256  bytes  absolute-path", "D20 checksum index header changed")
    for line_number, line in enumerate(lines[1:], start=2):
        match = re.fullmatch(r"([0-9a-fA-F]{64})\s+(\d+)\s+(.+)", line)
        require(match is not None, f"D20 checksum index line {line_number} is malformed")
        path = Path(match.group(3)).resolve()
        require(any(path == allowed or allowed in path.parents for allowed in allowed_roots), f"D20 checksum index line {line_number} uses an unapproved root")
        entries.append((match.group(1).lower(), int(match.group(2)), path))
    require(len(entries) == 626 and len({path for _, _, path in entries}) == 626, "D20 checksum index must contain 626 unique entries")
    for expected_hash, expected_bytes, path in entries:
        require(path.is_file(), f"D20 checksum artifact is missing: {path}")
        require(path.stat().st_size == expected_bytes, f"D20 checksum artifact byte count mismatch: {path}")
        require(sha256(path) == expected_hash, f"D20 checksum artifact hash mismatch: {path}")


def verify_ui_runtime_external(
    data: dict[str, Any],
    *,
    evidence_root: Path | None = None,
    expected_entries: int | None = None,
) -> None:
    label = "native UI/Vulkan external evidence"
    root = (evidence_root or Path(data["sealedEvidenceRoot"])).resolve()
    require(root.is_dir(), f"{label} root is missing: {root}")

    def checked_path(path_text: Any, item_label: str) -> Path:
        require(isinstance(path_text, str) and path_text, f"{item_label}.path must be a non-empty string")
        path = Path(path_text).resolve()
        try:
            path.relative_to(root)
        except ValueError as exc:
            raise ValidationError(f"{item_label} escapes the sealed evidence root") from exc
        require(path.is_file(), f"{item_label} is missing: {path}")
        return path

    critical: dict[Path, str] = {}

    def verify_hash_record(record: Any, item_label: str, *, verify_bytes: bool = False) -> Path:
        require(isinstance(record, dict), f"{item_label} must be an object")
        path = checked_path(record.get("path"), item_label)
        expected_hash = normalized_sha256(record.get("sha256"), item_label)
        if verify_bytes:
            require(path.stat().st_size == record.get("bytes"), f"{item_label} byte count mismatch")
        require(sha256(path) == expected_hash, f"{item_label} SHA-256 mismatch")
        critical[path] = expected_hash
        return path

    reference = data.get("reference", {})
    for role in ("application", "companion"):
        verify_hash_record(reference.get(role, {}).get("sourceArchive"), f"{label} {role} source archive")
    executable = verify_hash_record(reference.get("executable"), f"{label} executable", verify_bytes=True)
    relative_executable = reference.get("executable", {}).get("relativePath")
    require(
        isinstance(relative_executable, str)
        and root.joinpath(*PurePosixPath(relative_executable).parts).resolve() == executable,
        f"{label} executable relative path does not bind to its sealed file",
    )

    source_evidence = data.get("sourceEvidence")
    require(isinstance(source_evidence, dict), f"{label} sourceEvidence must be an object")
    manifest_path: Path | None = None
    for role, record in source_evidence.items():
        path = verify_hash_record(record, f"{label} source evidence {role}")
        if role == "artifactHashManifest":
            manifest_path = path
    require(manifest_path is not None, f"{label} artifact hash manifest is missing")

    screenshots = data.get("screenshots")
    require(isinstance(screenshots, dict), f"{label} screenshots must be an object")
    screenshot_rows = [*screenshots.get("primary", []), *screenshots.get("superseded", [])]
    require(len(screenshot_rows) == 23 or expected_entries is not None, f"{label} must retain 23 screenshots")
    for index, row in enumerate(screenshot_rows):
        require(isinstance(row, dict), f"{label} screenshot {index} must be an object")
        relative_path = row.get("file")
        require(isinstance(relative_path, str), f"{label} screenshot {index} file is invalid")
        pure_path = PurePosixPath(relative_path)
        require(
            not pure_path.is_absolute() and ".." not in pure_path.parts and "\\" not in relative_path,
            f"{label} screenshot {index} escapes the sealed root",
        )
        path = root.joinpath(*pure_path.parts).resolve()
        require(path.is_file(), f"{label} screenshot is missing: {relative_path}")
        expected_hash = normalized_sha256(row.get("sha256"), f"{label} screenshot {relative_path}")
        require(sha256(path) == expected_hash, f"{label} screenshot SHA-256 mismatch: {relative_path}")
        critical[path] = expected_hash

    entries: dict[Path, str] = {}
    try:
        lines = manifest_path.read_text(encoding="utf-8").splitlines()
    except (OSError, UnicodeError) as exc:
        raise ValidationError(f"cannot read {label} artifact hash manifest: {exc}") from exc
    for line_number, line in enumerate(lines, start=1):
        match = re.fullmatch(r"([0-9a-fA-F]{64})  ([^\\]+)", line)
        require(match is not None, f"{label} hash manifest line {line_number} is malformed")
        relative_text = match.group(2)
        pure_path = PurePosixPath(relative_text)
        require(
            relative_text
            and not pure_path.is_absolute()
            and ".." not in pure_path.parts
            and not (len(relative_text) >= 2 and relative_text[1] == ":"),
            f"{label} hash manifest line {line_number} escapes the sealed root",
        )
        path = root.joinpath(*pure_path.parts).resolve()
        try:
            path.relative_to(root)
        except ValueError as exc:
            raise ValidationError(f"{label} hash manifest line {line_number} escapes the sealed root") from exc
        require(path not in entries, f"{label} hash manifest contains duplicate path: {relative_text}")
        entries[path] = match.group(1).lower()
    expected_count = expected_entries
    if expected_count is None:
        expected_count = data.get("integrity", {}).get("artifactHashes", {}).get("expected")
    require(type(expected_count) is int and expected_count > 0, f"{label} expected hash count is invalid")
    require(len(entries) == expected_count, f"{label} hash manifest must contain {expected_count} unique entries")
    for path, expected_hash in entries.items():
        require(path.is_file(), f"{label} indexed artifact is missing: {path}")
        require(sha256(path) == expected_hash, f"{label} indexed artifact SHA-256 mismatch: {path}")
    for path, expected_hash in critical.items():
        if path == manifest_path:
            continue
        require(entries.get(path) == expected_hash, f"{label} critical artifact is absent or differs in hash manifest: {path}")
    try:
        actual_files = {path.resolve() for path in root.rglob("*") if path.is_file() and path.resolve() != manifest_path}
    except OSError as exc:
        raise ValidationError(f"cannot enumerate {label} root: {exc}") from exc
    require(actual_files == set(entries), f"{label} file inventory differs from the sealed hash manifest")


def _verify_detached_sha_manifest_inventory(
    root: Path,
    manifest_record: dict[str, Any],
    *,
    expected_entries: int,
    critical_records: list[dict[str, Any]],
    label: str,
) -> None:
    root = root.resolve()
    verify_external_artifact(manifest_record, f"{label} detached SHA-256 manifest")
    manifest_path = Path(manifest_record["path"]).resolve()
    entries: dict[Path, str] = {}
    for line_number, line in enumerate(manifest_path.read_text(encoding="utf-8").splitlines(), start=1):
        match = re.fullmatch(r"([0-9a-fA-F]{64})  ([^\\]+)", line)
        require(match is not None, f"{label} SHA-256 manifest line {line_number} is malformed")
        relative_text = match.group(2)
        relative = PurePosixPath(relative_text)
        require(
            relative_text
            and not relative.is_absolute()
            and ".." not in relative.parts
            and not (len(relative_text) >= 2 and relative_text[1] == ":"),
            f"{label} SHA-256 manifest line {line_number} escapes the sealed root",
        )
        path = root.joinpath(*relative.parts).resolve()
        try:
            path.relative_to(root)
        except ValueError as exc:
            raise ValidationError(f"{label} SHA-256 manifest line {line_number} escapes the sealed root") from exc
        require(path not in entries, f"{label} SHA-256 manifest contains a duplicate path: {relative_text}")
        entries[path] = match.group(1).lower()
    require(len(entries) == expected_entries, f"{label} SHA-256 manifest must contain {expected_entries} entries")
    for record in critical_records:
        critical_path = Path(record["path"]).resolve()
        require(
            entries.get(critical_path) == normalized_sha256(record.get("sha256"), f"{label} critical artifact"),
            f"{label} critical artifact is absent or differs in the SHA-256 manifest: {critical_path}",
        )
    actual_files = {path.resolve() for path in root.rglob("*") if path.is_file() and path.resolve() != manifest_path}
    require(actual_files == set(entries), f"{label} filesystem inventory differs from the detached SHA-256 manifest")


def validate_ui_semantic_index_data(data: Any) -> None:
    label = "native UI semantic reference index"
    require(isinstance(data, dict), f"{label} must be an object")
    require(data.get("schemaVersion") == 1, f"{label} schemaVersion changed")
    require(data.get("schema") == "mgif.editor-parity.native-ui-semantic-reference-index", f"{label} schema changed")
    require(data.get("indexId") == "UI-SEMANTIC", f"{label} indexId changed")
    require(data.get("evidenceKind") == "external-reference-pointer-mirror", f"{label} evidence kind changed")
    require(data.get("sealedEvidenceRoot") == EXPECTED_UI_SEMANTIC_INDEX["root"], f"{label} sealed root changed")
    require(data.get("largeArtifactsCopiedIntoMgif") is False and data.get("copiedArtifacts") == [], f"{label} copied external artifacts into MGIF")

    application = data.get("reference", {}).get("application", {})
    executable = data.get("reference", {}).get("executable", {})
    require(application.get("commit") == EXPECTED_REFERENCE_COMMIT, f"{label} application commit changed")
    require(application.get("tree") == EXPECTED_REFERENCE_TREE, f"{label} application tree changed")
    require(executable.get("sha256") == EXPECTED_UI_RUNTIME_REFERENCE["executable"]["sha256"], f"{label} executable changed")
    integrity = data.get("integrity", {})
    require(integrity.get("filesystem", {}).get("filesExpectedInManifest") == 768, f"{label} manifest inventory changed")
    require(integrity.get("sha256Manifest", {}).get("verified") == 768, f"{label} hash verification count changed")
    require(integrity.get("sha256Manifest", {}).get("passed") is True, f"{label} hash verification is not passing")
    classifications = integrity.get("classifications", {})
    require(classifications.get("nativeCitableCases") == 20, f"{label} citable case count changed")
    require(classifications.get("diagnosticCases") == 21, f"{label} diagnostic case count changed")

    rows = data.get("coverageCeiling")
    require(isinstance(rows, list), f"{label} coverage ceiling must be an array")
    by_oracle = {row.get("oracle"): row for row in rows if isinstance(row, dict)}
    require(set(by_oracle) == {"O05", "O06", "O07", "O08", "O10", "D20"}, f"{label} oracle coverage set changed")
    require(by_oracle["O06"].get("status") == "native-observed-both-orders", f"{label} O06 conflict observation changed")
    for oracle in ("O05", "O07", "O08", "O10"):
        require("partial" in by_oracle[oracle].get("status", ""), f"{label} {oracle} must remain partial")
        require(bool(by_oracle[oracle].get("gaps")), f"{label} {oracle} lost its bounded gaps")
    require(by_oracle["D20"].get("status") == "not-evaluated-unresolved", f"{label} must not retroactively claim D20")
    decision = data.get("d20DecisionBoundary", {})
    require(decision.get("evaluatedByThisRun") is False, f"{label} cannot claim D20 evaluation")
    require(decision.get("policySelectedByThisRun") is False and decision.get("selectedPolicy") is None, f"{label} cannot select D20")
    require(data.get("dependencyBoundary", {}).get("copyLinkAdaptOrIntroduceReferenceArtifactsOrInterfacesAuthorized") is False, f"{label} cannot authorize production reference dependencies")


def verify_ui_semantic_external(data: dict[str, Any]) -> None:
    label = "native UI semantic external evidence"
    root = Path(data["sealedEvidenceRoot"]).resolve()
    require(root.is_dir(), f"{label} root is missing: {root}")
    records = data.get("externalArtifacts", {})
    require(isinstance(records, dict) and len(records) == 11, f"{label} artifact bindings changed")
    for role, record in records.items():
        verify_external_artifact(record, f"{label} {role}")
    _verify_detached_sha_manifest_inventory(
        root,
        records["sha256Manifest"],
        expected_entries=768,
        critical_records=[record for role, record in records.items() if role != "sha256Manifest"],
        label=label,
    )


def validate_o09_real_ui_index_data(data: Any) -> None:
    label = "O09 real-UI reference index"
    require(isinstance(data, dict), f"{label} must be an object")
    require(data.get("schemaVersion") == 1, f"{label} schemaVersion changed")
    require(data.get("schema") == "mgif.editor-parity.o09-real-ui-reference-index", f"{label} schema changed")
    require(data.get("indexId") == "O09-REAL-UI", f"{label} indexId changed")
    require(data.get("sealedEvidenceRoot") == EXPECTED_O09_REAL_UI_INDEX["root"], f"{label} sealed root changed")
    require(data.get("largeArtifactsCopiedIntoMgif") is False and data.get("copiedArtifacts") == [], f"{label} copied external artifacts into MGIF")
    reference = data.get("reference", {})
    require(reference.get("commit") == EXPECTED_REFERENCE_COMMIT and reference.get("tree") == EXPECTED_REFERENCE_TREE, f"{label} reference identity changed")
    require(reference.get("executableSha256") == EXPECTED_UI_RUNTIME_REFERENCE["executable"]["sha256"], f"{label} executable changed")
    coverage = data.get("coverage", {})
    require(
        coverage
        == {
            "registryCells": 298,
            "realUiActionPathsCaptured": 298,
            "numericExitIdentitiesCaptured": 298,
            "cpuExpectationAndMinimumPhaseMatches": 292,
            "cleanSelectedExits": 290,
            "expectedStackOverflowExits": 2,
            "unexpectedAccessViolationExits": 6,
            "fullVisibleHistorySequences": 290,
            "exactSerializedTopologyCells": 7,
            "exactSerializedTopologyCheckpoints": 20,
            "visibleOnlyCurrentSceneCells": 298,
            "visibleOnlySelectionCells": 294,
            "privateStateReadbackCells": 0,
            "gpuCorroboratedCells": 298,
            "screenshots": 2186,
            "checksumEntries": 8660,
            "integrityFailures": 0,
        },
        f"{label} exact coverage ceiling changed",
    )
    ceiling = data.get("coverageCeiling", {})
    require(ceiling.get("status") == "native_partial", f"{label} must remain native_partial")
    require(ceiling.get("allCellsHaveRealUiActionPath") is True, f"{label} lost real UI action coverage")
    require(ceiling.get("allCellsAreFullStateUiReplays") is False, f"{label} cannot claim 298 full UI state replays")
    require(ceiling.get("domainMatrixRelabeledAsUiReplays") is False, f"{label} relabeled the domain matrix")
    require(ceiling.get("visibleStateIsNotPrivateState") is True and ceiling.get("referenceFailuresPreserved") is True, f"{label} lost its observation boundary")
    decision = data.get("decisionBoundary", {})
    require(decision.get("policySelectedByEvidenceRun") is False and decision.get("selectedPolicy") is None, f"{label} evidence run cannot select D20")


def _validate_o09_selected_attempt_outcome(attempt: dict[str, Any], case_id: str) -> str:
    label = f"O09 real-UI external evidence {case_id}"
    observed_hex = attempt.get("observedExitHex")
    require(isinstance(observed_hex, str), f"{label} lacks a numeric exit identity")
    observed_hex = observed_hex.upper()
    require(
        observed_hex in {"0X00000000", "0XC00000FD", "0XC0000005"},
        f"{label} selected attempt has an unclassified numeric exit",
    )
    if observed_hex == "0X00000000":
        require(
            attempt.get("executionComplete") is True
            and attempt.get("expectedExitMatch") is True
            and attempt.get("grade") == "complete-clean",
            f"{label} clean selected attempt is not complete and expectation-matched",
        )
    elif observed_hex == "0XC00000FD":
        require(
            attempt.get("executionComplete") is True
            and attempt.get("expectedExitMatch") is True
            and attempt.get("grade") == "complete-expected-stack-overflow",
            f"{label} expected stack-overflow attempt classification changed",
        )
    else:
        require(
            attempt.get("executionComplete") is False
            and attempt.get("expectedExitMatch") is False
            and attempt.get("grade") == "action-captured-unexpected-exit",
            f"{label} unexpected access-violation attempt classification changed",
        )
    return observed_hex


def verify_o09_real_ui_external(data: dict[str, Any], d20_data: dict[str, Any]) -> None:
    label = "O09 real-UI external evidence"
    root = Path(data["sealedEvidenceRoot"]).resolve()
    require(root.is_dir(), f"{label} root is missing: {root}")
    records = data.get("externalArtifacts", {})
    require(isinstance(records, dict) and set(records) == {"report", "index", "artifactIndex", "integrityReverification", "finalSeal", "detachedSha256Manifest"}, f"{label} artifact bindings changed")
    for role, record in records.items():
        verify_external_artifact(record, f"{label} {role}")

    external = load_json(Path(records["index"]["path"]))
    final_seal = load_json(Path(records["finalSeal"]["path"]))
    integrity = load_json(Path(records["integrityReverification"]["path"]))
    coverage = external.get("coverage", {})
    require(external.get("reference", {}).get("commit") == EXPECTED_REFERENCE_COMMIT, f"{label} commit changed")
    require(external.get("reference", {}).get("tree") == EXPECTED_REFERENCE_TREE, f"{label} tree changed")
    require(external.get("reference", {}).get("executableSha256", "").lower() == EXPECTED_UI_RUNTIME_REFERENCE["executable"]["sha256"], f"{label} executable changed")
    require(coverage.get("registryCellsRequested") == 298 and coverage.get("realUiActionCellsCaptured") == 298, f"{label} action coverage changed")
    require(coverage.get("exitIdentityCapturedCells") == 298 and coverage.get("unexecutedLogicalCaseCount") == 0, f"{label} exit coverage changed")
    require(coverage.get("cleanExitCells") == 290 and coverage.get("expectedStackOverflowCells") == 2 and coverage.get("accessViolationExitCells") == 6, f"{label} exit classification changed")
    require(coverage.get("fullHistoryUndoRedoVisibleCells") == 290, f"{label} visible history ceiling changed")
    require(coverage.get("exactApplicationSerializedTopologyCells") == 7 and coverage.get("exactApplicationSerializedCheckpointCount") == 20, f"{label} topology ceiling changed")
    require(coverage.get("privateStateReadbackCells") == 0 and coverage.get("visibleOnlyCurrentSceneCells") == 298 and coverage.get("visibleOnlySelectionCells") == 294, f"{label} private/visible state ceiling changed")

    require(final_seal.get("schema") == "vkgr-o09-final-seal/v1" and final_seal.get("integrityPassed") is True, f"{label} final seal is not passing")
    require(final_seal.get("coverage") == coverage, f"{label} final seal coverage differs from INDEX.json")
    require(final_seal.get("verdict", {}).get("o09CanTruthfullyBecomeCaptured") is False, f"{label} cannot claim captured")
    require(final_seal.get("verdict", {}).get("recommendedStatus") == "partial", f"{label} final status changed")
    require(integrity.get("schema") == "vkgr-o09-integrity-reverification/v1" and integrity.get("passed") is True, f"{label} integrity reverification is not passing")
    require(integrity.get("allProducerProcessesExited") is True and integrity.get("producerProcessCount") == 0, f"{label} was not sealed after producer exit")
    require(integrity.get("artifactIndex", {}).get("payloadEntries") == 8657 and integrity.get("artifactIndex", {}).get("verificationFailureCount") == 0, f"{label} artifact-index verification changed")
    require(integrity.get("matrix", {}).get("requested") == 298 and integrity.get("matrix", {}).get("unexecuted") == 0, f"{label} integrity matrix changed")
    require(integrity.get("mgif", {}).get("productionDependencyImplication") == "none", f"{label} cannot imply an MGIF production dependency")

    domain_cells = d20_data.get("o09Coverage", {}).get("cells", [])
    native_cells = external.get("cells", [])
    require(len(domain_cells) == 298 and len(native_cells) == 298, f"{label} must contain exactly 298 domain/native cells")
    domain_by_id = {cell.get("caseId"): cell for cell in domain_cells if isinstance(cell, dict)}
    require(len(domain_by_id) == 298, f"{label} domain registry case IDs are not unique")
    expected_match_count = 0
    observed_exit_counts: dict[str, int] = {}
    selection_grade_counts: dict[str, int] = {}
    for ordinal, native in enumerate(native_cells):
        require(isinstance(native, dict) and native.get("ordinal") == ordinal, f"{label} cell ordinal {ordinal} changed")
        case_id = native.get("caseId")
        require(case_id in domain_by_id, f"{label} cell is absent from the domain registry: {case_id}")
        domain = domain_by_id[case_id]
        require(native.get("requirement") == domain.get("requirement"), f"{label} {case_id} requirement differs from registry")
        require(native.get("dimensions") == domain.get("dimensions"), f"{label} {case_id} dimensions differ from registry")
        require(domain.get("status") == "executed_domain_with_exact_ui_envelope", f"{label} {case_id} domain status changed")
        require(domain.get("uiRuntimeDriven") is False, f"{label} {case_id} domain evidence was dishonestly upgraded to UI runtime")
        binding = native.get("registryBinding", {})
        exit_record = domain.get("exit", {})
        require(binding.get("expectedOutcome") == domain.get("outcome"), f"{label} {case_id} outcome differs from registry")
        require(binding.get("expectedExitHex", "").lower() == exit_record.get("processExitCodeHex", "").lower(), f"{label} {case_id} expected exit differs from registry")
        require(exit_record.get("timedOut") is False, f"{label} {case_id} registry timeout state changed")
        require(binding.get("rawTraceSha256", "").lower() == domain.get("rawTraceSha256", "").lower(), f"{label} {case_id} raw trace hash differs from registry")
        envelope = domain.get("sourceEnvelope", {})
        for field in ("evidenceKind", "source", "uiBranch"):
            require(binding.get(field) == envelope.get(field), f"{label} {case_id} source envelope {field} differs from registry")

        attempts = native.get("attempts", [])
        selected = [attempt for attempt in attempts if isinstance(attempt, dict) and attempt.get("selected") is True]
        require(len(selected) == 1 and selected[0].get("executionId") == native.get("selectedExecutionId"), f"{label} {case_id} selected attempt is ambiguous")
        attempt = selected[0]
        require(attempt.get("freshProcessAndFixture") is True and attempt.get("referenceBindingMatches") is True, f"{label} {case_id} selected attempt is not fresh/reference-bound")
        require(attempt.get("hasCaseResult") is True, f"{label} {case_id} selected attempt lacks a case result")
        require(attempt.get("actionDispatched") is True and attempt.get("actionPathCaptured") is True, f"{label} {case_id} selected action path was not captured")
        require(attempt.get("expectedExitHex", "").lower() == binding.get("expectedExitHex", "").lower(), f"{label} {case_id} selected attempt expected exit differs from registry")
        observed_hex = _validate_o09_selected_attempt_outcome(attempt, case_id)
        expected_match_count += int(attempt.get("expectedExitMatch") is True)
        observed_exit_counts[observed_hex] = observed_exit_counts.get(observed_hex, 0) + 1

        observed = native.get("observed", {})
        require(observed.get("realUiAction") == "captured-dispatched" and observed.get("freshProcessAndFixture") is True, f"{label} {case_id} is not a captured real UI action")
        require(observed.get("currentScene") == "captured-visible-settings-radio-not-private", f"{label} {case_id} current-scene grade changed")
        selection_grade = observed.get("selection")
        require(selection_grade in {"captured-visible-row-and-inspector-not-private", "not-captured"}, f"{label} {case_id} selection grade changed")
        if selection_grade == "not-captured":
            require(observed_hex == "0XC0000005", f"{label} {case_id} lost selection without a preserved access-violation exit")
        selection_grade_counts[selection_grade] = selection_grade_counts.get(selection_grade, 0) + 1
        private = observed.get("privateStateReadback", {})
        require(private == {"currentScene": False, "selection": False, "undoRedoStack": False, "method": "none"}, f"{label} {case_id} private-state ceiling changed")

        case_result_record = attempt.get("caseResult")
        require(isinstance(case_result_record, dict), f"{label} {case_id} selected case result is missing")
        case_result_path = root.joinpath(*PurePosixPath(case_result_record.get("path", "")).parts).resolve()
        require(case_result_path.is_file(), f"{label} {case_id} selected case result file is missing")
        require(case_result_path.stat().st_size == case_result_record.get("bytes"), f"{label} {case_id} case result byte count changed")
        require(sha256(case_result_path) == normalized_sha256(case_result_record.get("sha256"), f"{label} {case_id} case result"), f"{label} {case_id} case result hash changed")
        case_result = load_json(case_result_path)
        require(case_result.get("caseId") == case_id and case_result.get("requirement") == domain.get("requirement"), f"{label} {case_id} case result identity differs")
        require(case_result.get("dimensions") == domain.get("dimensions"), f"{label} {case_id} case-result dimensions differ")
        require(case_result.get("freshProcessAndFixture") is True, f"{label} {case_id} case result is not fresh")
        require(case_result.get("reference", {}).get("commit") == EXPECTED_REFERENCE_COMMIT and case_result.get("reference", {}).get("tree") == EXPECTED_REFERENCE_TREE, f"{label} {case_id} case-result reference identity changed")
        cpu_binding = case_result.get("cpuRegistryBinding", {})
        require(cpu_binding.get("expectedOutcome") == domain.get("outcome"), f"{label} {case_id} case-result outcome differs")
        require(cpu_binding.get("processExitCode") == exit_record.get("processExitCode"), f"{label} {case_id} case-result expected exit differs")
        require(cpu_binding.get("rawTraceSha256", "").lower() == domain.get("rawTraceSha256", "").lower(), f"{label} {case_id} case-result raw trace differs")
        require(cpu_binding.get("envelope") == envelope, f"{label} {case_id} case-result source envelope differs")
        require(case_result.get("realUiExecution", {}).get("attempted") is True and case_result.get("realUiExecution", {}).get("actionDispatched") is True, f"{label} {case_id} case-result action was not driven")
        require(case_result.get("exit", {}).get("waitCompleted") is True, f"{label} {case_id} case result did not capture process completion")
        require(case_result.get("exit", {}).get("exitCodeHex", "").upper() == observed_hex, f"{label} {case_id} case-result exit differs from selected attempt")
        require(case_result.get("observedFieldGrade", {}).get("uiPath") == "captured", f"{label} {case_id} UI path grade changed")

    require(expected_match_count == 292, f"{label} expected-exit match count changed")
    require(observed_exit_counts == {"0X00000000": 290, "0XC00000FD": 2, "0XC0000005": 6}, f"{label} selected numeric exit distribution changed")
    require(
        selection_grade_counts
        == {"captured-visible-row-and-inspector-not-private": 294, "not-captured": 4},
        f"{label} visible-selection observation distribution changed",
    )
    _verify_detached_sha_manifest_inventory(
        root,
        records["detachedSha256Manifest"],
        expected_entries=8660,
        critical_records=[records[role] for role in ("report", "index", "artifactIndex", "integrityReverification", "finalSeal")],
        label=label,
    )


def validate_f39_repair(root: Path, *, verify_external: bool) -> None:
    label = "F39 renderer repair evidence"
    for relative_path, expected_hash in EXPECTED_F39_REPAIR_ARTIFACTS.items():
        path = root / relative_path
        require(path.is_file() and sha256(path) == expected_hash, f"{label} repository artifact changed: {relative_path}")
    index = load_json(root / "docs/editor-parity/f39-repair-index.json")
    historical = index.get("historicalBaseline", {})
    require(historical.get("commit") == EXPECTED_TARGET_COMMIT, f"{label} historical commit changed")
    require(historical.get("demoSha256") == "92226d901a95d0e6b2abbcb5c33acc788f081473ae7dca11d45e8ababb1f08c8", f"{label} historical executable changed")
    require(historical.get("passed") == 0 and historical.get("caseCount") == 5 and historical.get("failure") == "SYNC-HAZARD-READ-AFTER-WRITE" and historical.get("waived") is False, f"{label} historical failure was rewritten or waived")
    repair = index.get("currentRepair", {})
    require(repair.get("demoSha256") == EXPECTED_F39_REPAIR_DEMO_SHA256, f"{label} repaired executable changed")
    require(repair.get("caseCount") == 5 and repair.get("passed") == 5 and repair.get("failed") == 0, f"{label} 5/5 result changed")
    for flag in ("allExitZero", "allRequiredMarkersExactlyOnce", "allExecutableHashesStable", "allStderrEmpty", "allFailureMatchesEmpty"):
        require(repair.get(flag) is True, f"{label} {flag} is not true")
    if not verify_external:
        return
    summary_record = repair.get("summary", {})
    identity_record = repair.get("identity", {})
    for role, record in (("summary", summary_record), ("identity", identity_record)):
        path = resolve_repo_path(root, record.get("path"), f"{label} {role}")
        require(path.stat().st_size == record.get("bytes"), f"{label} {role} byte count changed")
        require(sha256(path) == normalized_sha256(record.get("sha256"), f"{label} {role}"), f"{label} {role} hash changed")
    summary = load_json(resolve_repo_path(root, summary_record["path"], f"{label} summary"))
    identity = load_json(resolve_repo_path(root, identity_record["path"], f"{label} identity"))
    require(summary.get("demo_sha256") == EXPECTED_F39_REPAIR_DEMO_SHA256 and identity.get("demo_sha256") == EXPECTED_F39_REPAIR_DEMO_SHA256, f"{label} summary/identity executable mismatch")
    require(summary.get("case_count") == 5 and summary.get("passed") == 5 and summary.get("failed") == 0, f"{label} summary is not 5/5")
    cases = summary.get("cases")
    require(isinstance(cases, list) and len(cases) == 5, f"{label} summary case inventory changed")
    markers = {"config", "scene_ready", "start", "last_moving", "first_still", "settled", "complete"}
    for case in cases:
        case_id = case.get("case")
        require(isinstance(case_id, str) and case_id, f"{label} has an invalid case ID")
        result_path = resolve_repo_path(root, f"{repair['evidenceRoot']}/{case_id}.result.json", f"{label} {case_id} result")
        result = load_json(result_path)
        require(result == case, f"{label} {case_id} summary differs from its result file")
        require(case.get("timed_out") is False and case.get("exit_code") == 0 and case.get("passed") is True, f"{label} {case_id} did not exit cleanly")
        require(set(case.get("marker_counts", {})) == markers and all(value == 1 for value in case["marker_counts"].values()), f"{label} {case_id} required markers are not exact")
        require(case.get("invalid_markers") == [] and case.get("failure_matches") == [], f"{label} {case_id} contains invalid/failure markers")
        require(case.get("demo_sha256_before") == EXPECTED_F39_REPAIR_DEMO_SHA256 and case.get("demo_sha256_after") == EXPECTED_F39_REPAIR_DEMO_SHA256 and case.get("demo_sha256_stable") is True, f"{label} {case_id} executable identity changed")
        stdout_path = Path(case.get("stdout_log", ""))
        stderr_path = Path(case.get("stderr_log", ""))
        require(stdout_path.is_file() and stderr_path.is_file() and stderr_path.stat().st_size == 0, f"{label} {case_id} log files changed")
        stdout = stdout_path.read_text(encoding="utf-8", errors="replace")
        for marker in markers:
            require(len(re.findall(rf"\[CSM_AUTOMATION\] marker={re.escape(marker.replace('_', '-'))}\b", stdout)) == 1, f"{label} {case_id} marker {marker} count differs from log")


def validate_reference_trace_artifacts(root: Path, *, verify_external: bool = False) -> None:
    for relative_path, expected_hash in EXPECTED_TRACE_REPOSITORY_ARTIFACTS.items():
        path = root / relative_path
        require(path.is_file(), f"required reference trace artifact is missing: {relative_path}")
        if expected_hash is not None:
            require(sha256(path) == expected_hash, f"reference trace artifact hash changed: {relative_path}")

    d20 = load_json(root / "docs/editor-parity/reference-traces/d20-hierarchy-index.json")
    input_clipboard = load_json(root / "docs/editor-parity/reference-traces/input-clipboard-index.json")
    ui_runtime = load_json(root / "docs/editor-parity/reference-traces/ui-runtime-index.json")
    ui_semantic = load_json(root / "docs/editor-parity/reference-traces/ui-semantic-index.json")
    o09_real_ui = load_json(root / "docs/editor-parity/reference-traces/o09-real-ui-index.json")
    validate_d20_trace_index_data(d20)
    validate_input_clipboard_index_data(input_clipboard)
    validate_ui_runtime_index_data(ui_runtime)
    validate_ui_semantic_index_data(ui_semantic)
    validate_o09_real_ui_index_data(o09_real_ui)

    d20_report = (root / "docs/editor-parity/reference-traces/D20-HIERARCHY-REPORT.md").read_text(encoding="utf-8")
    input_report = (root / "docs/editor-parity/reference-traces/INPUT-CLIPBOARD-REPORT.md").read_text(encoding="utf-8")
    require(
        re.search(r"<code>imguiRuntime</code>\s*\|\s*<code>false</code>", d20_report) is not None
        and re.search(r"<code>gpuRuntime</code>\s*\|\s*<code>false</code>", d20_report) is not None,
        "D20 report lost its runtime ceiling",
    )
    require("O04" in d20_report and "2 -> 3 -> 4 -> 5" in d20_report, "D20 report lost the O04 conflict")
    require("not a full interactive application observation" in input_report, "input report lost its evidence ceiling")

    if not verify_external:
        return
    for item in input_clipboard["rawArtifacts"]:
        verify_external_artifact(item, f"input raw artifact {item['role']}")
    verify_input_checksum_index(input_clipboard)

    artifacts = d20["artifacts"]
    for role in ("rawReport", "rawIndex", "rawHashList", "probeExecutable", "probePdb", "probeSource", "testExecutable"):
        verify_external_artifact(artifacts[role], f"D20 raw artifact {role}")
    for role, item in artifacts["fixtures"].items():
        verify_external_artifact(item, f"D20 fixture {role}")
    verify_d20_checksum_index(d20)
    verify_ui_runtime_external(ui_runtime)
    verify_ui_semantic_external(ui_semantic)
    verify_o09_real_ui_external(o09_real_ui, d20)


def validate_p0_baseline_and_license(
    root: Path,
    features: list[Any],
    license_policy: Any,
    *,
    release_readiness: bool = False,
    verify_repair: bool = False,
) -> None:
    baseline_path = root / "docs/editor-parity/MGIF-BASELINE.md"
    source_path = root / "docs/editor-parity/REFERENCE-SOURCE-EVIDENCE.md"
    license_path = root / "docs/editor-parity/LICENSE-SOURCES.md"
    require(sha256(baseline_path) == EXPECTED_MGIF_BASELINE_SHA256, "MGIF baseline evidence hash changed")
    require(sha256(source_path) == EXPECTED_REFERENCE_SOURCE_EVIDENCE_SHA256, "reference source evidence hash changed")
    if not release_readiness:
        require(sha256(license_path) == EXPECTED_LICENSE_SOURCES_SHA256, "license/source ledger hash changed")

    baseline = baseline_path.read_text(encoding="utf-8")
    require(EXPECTED_TARGET_COMMIT in baseline, "MGIF baseline evidence lost the fixed commit")
    require("Full CTest | PASS | 12/12" in baseline, "MGIF baseline evidence lost the full CTest result")
    require("Windows Vulkan validation smoke | FAIL | 0/5" in baseline, "MGIF baseline evidence must retain the 0/5 Vulkan failure")
    require(re.search(r"This record is not a\s+waiver", baseline) is not None, "MGIF baseline failure cannot be silently waived")
    require("SYNC-HAZARD-READ-AFTER-WRITE" in baseline, "MGIF baseline evidence lost the Vulkan failure identity")
    require(
        "789518c264c43d8426345576e43a7c138082eed9c67466c37c8562c5d8e26f1a" in baseline
        and "passed 5/5 focused Vulkan synchronization smoke" in baseline,
        "MGIF baseline evidence lost the current renderer repair supplement",
    )
    validate_f39_repair(root, verify_external=verify_repair)

    license_text = license_path.read_text(encoding="utf-8")
    for artifact in (
        "docs/editor-parity/reference-traces/D20-HIERARCHY-REPORT.md",
        "docs/editor-parity/reference-traces/d20-hierarchy-index.json",
        "docs/editor-parity/reference-traces/INPUT-CLIPBOARD-REPORT.md",
        "docs/editor-parity/reference-traces/input-clipboard-index.json",
        "docs/editor-parity/reference-traces/UI-SEMANTIC-REPORT.md",
        "docs/editor-parity/reference-traces/ui-semantic-index.json",
        "docs/editor-parity/reference-traces/O09-REAL-UI-REPORT.md",
        "docs/editor-parity/reference-traces/o09-real-ui-index.json",
        "docs/editor-parity/F39-REPAIR.md",
        "docs/editor-parity/f39-repair-index.json",
    ):
        require(f"`{artifact}`" in license_text, f"license/source ledger is missing {artifact}")
    root_license_names = ("LICENSE", "LICENSE.txt", "LICENSE.md", "COPYING", "NOTICE")

    feature_by_id = {feature.get("id"): feature for feature in features if isinstance(feature, dict)}
    f39 = feature_by_id["F39"]["mgifEvidence"]
    require(
        f39.get("grade") == "executable" and f39.get("status") == "provisional",
        "F39 must bind the repaired smoke without claiming full feature PASS",
    )
    require(
        f39.get("artifacts")
        == [
            "docs/editor-parity/MGIF-BASELINE.md",
            "docs/editor-parity/F39-REPAIR.md",
            "docs/editor-parity/f39-repair-index.json",
        ],
        "F39 baseline and repair artifact binding changed",
    )
    require(
        "Historical fixed baseline" in f39.get("notes", "")
        and "not waived" in f39.get("notes", "")
        and "passes the focused 5/5 smoke" in f39.get("notes", ""),
        "F39 must retain both the unwaived historical failure and bounded current repair",
    )
    f40 = feature_by_id["F40"]["mgifEvidence"]
    require(f40.get("grade") == "test" and f40.get("status") == "provisional", "F40 must retain partial platform coverage")
    f42 = feature_by_id["F42"]["mgifEvidence"]
    if release_readiness:
        require(isinstance(license_policy, dict), "release readiness requires an explicit root license policy")
        require(license_policy.get("status") == "selected", "release readiness blocked: owner root-license selection is pending")
        require(
            license_policy.get("developmentPolicy")
            == {
                "distributionStatus": "licensed",
                "engineeringImplementation": "original_clean_room_unless_provenance_ledger_says_otherwise",
                "upstreamApacheSourceCopied": False,
                "preserveThirdPartyNotices": True,
                "p1EngineeringReadinessIndependent": True,
            },
            "release readiness selected license development policy changed",
        )
        require(
            license_policy.get("releaseGate")
            == {
                "status": "ready_with_selected_root_license",
                "blockedFeatureIds": [],
                "blockedRequirementIds": [],
                "passClaimAllowed": True,
            },
            "release readiness selected root license must unblock F42/R24 release claims",
        )
        require(
            license_policy.get("validation") == EXPECTED_DEVELOPMENT_LICENSE_POLICY["validation"],
            "release readiness root license validation contract changed",
        )
        selected_file = license_policy.get("selectedFile")
        require(selected_file in root_license_names, "release readiness selected license file is not canonical")
        require(
            isinstance(license_policy.get("selectedSpdxId"), str) and bool(license_policy["selectedSpdxId"].strip()),
            "release readiness selected license requires a non-empty SPDX identity",
        )
        require(
            isinstance(license_policy.get("approvedBy"), str) and bool(license_policy["approvedBy"].strip()),
            "release readiness selected license requires an approving user identity",
        )
        _require_timezone_aware_timestamp(license_policy.get("approvedAt"), "selected license approvedAt")
        accepted_digest = normalized_sha256(
            license_policy.get("acceptedSha256"),
            "release readiness accepted root license digest",
        )
        declared_licenses = [name for name in root_license_names if (root / name).is_file()]
        require(declared_licenses == [selected_file], "release readiness requires exactly the selected root license/notice")
        selected_path = root / selected_file
        try:
            license_bytes = selected_path.read_bytes()
            selected_text = license_bytes.decode("utf-8")
        except (OSError, UnicodeError) as exc:
            raise ValidationError(f"cannot read selected root license as UTF-8: {exc}") from exc
        require(len(license_bytes) >= 256, "selected root license is empty, whitespace-only, or truncated")
        require(bool(selected_text.strip()) and "\x00" not in selected_text, "selected root license is empty or invalid text")
        require(len([line for line in selected_text.splitlines() if line.strip()]) >= 8, "selected root license is truncated")
        require(sha256(selected_path) == accepted_digest, "selected root license does not match its exact accepted digest")
        require(f42.get("grade") == "source" and f42.get("status") == "pass", "release readiness requires resolved F42 license evidence")
        require(
            f42.get("artifacts") == ["docs/editor-parity/LICENSE-SOURCES.md", selected_file],
            "release readiness F42 must bind the license ledger and declared root license",
        )
        require(
            "no `LICENSE`, `COPYING`, or `NOTICE` file" not in license_text,
            "release readiness license/source ledger still claims the root license is missing",
        )
        require(f"`{selected_file}`" in license_text, "release readiness license/source ledger does not bind the root license")
    else:
        require(
            json_values_equal(license_policy, EXPECTED_DEVELOPMENT_LICENSE_POLICY),
            "engineering readiness must retain the explicit rights-reserved development policy",
        )
        require(
            "no `LICENSE`, `COPYING`, or `NOTICE` file" in license_text,
            "license/source ledger lost the rights-reserved root-license disclosure",
        )
        require(f42.get("grade") == "source" and f42.get("status") == "blocked", "F42 must remain blocked until a root license is declared")
        require(f42.get("artifacts") == ["docs/editor-parity/LICENSE-SOURCES.md"], "F42 license artifact changed")
        require(
            not any((root / name).exists() for name in root_license_names),
            "F42 is stale because a root license/notice now exists",
        )
        require(
            "unlicensed/rights-reserved" in license_text
            and "P1 engineering readiness may proceed" in license_text
            and "F42, R24" in license_text,
            "license/source ledger lost the engineering-vs-release readiness boundary",
        )


def verify_mgif_baseline_external() -> None:
    local_app_data = os.environ.get("LOCALAPPDATA")
    require(bool(local_app_data), "LOCALAPPDATA is required to verify the fixed MGIF baseline evidence")
    baseline_root = Path(local_app_data) / "Temp/mgif-baseline-6c36567-20260822-132137-13c71a43"
    expected = {
        "baseline-evidence-report.md": (13957, "06d58de11cc4375a545f0c229460e54ea9859baf5acd51086974f2f734b39cd7"),
        "build/full/Demo.exe": (12466688, "92226d901a95d0e6b2abbcb5c33acc788f081473ae7dca11d45e8ababb1f08c8"),
        "src/out/validation/csm-vulkan-smoke-20260822-134255-92226d901a95/summary.json": (
            9393,
            "e97aa14474561b2b24a536920a1b6c19643c171024c70896eaf6fb3a81e66717",
        ),
    }
    for relative_path, (expected_bytes, expected_hash) in expected.items():
        path = baseline_root.joinpath(*PurePosixPath(relative_path).parts)
        require(path.is_file(), f"fixed MGIF baseline artifact is missing: {path}")
        require(path.stat().st_size == expected_bytes, f"fixed MGIF baseline artifact byte count changed: {relative_path}")
        require(sha256(path) == expected_hash, f"fixed MGIF baseline artifact hash changed: {relative_path}")


def _validate_configured_identity_record(
    build_root: Path,
    record: Any,
    expected_path: Path,
    label: str,
) -> None:
    require(isinstance(record, dict) and set(record) == {"path", "sha256"}, f"{label} binding changed")
    relative = record.get("path")
    require(isinstance(relative, str) and relative, f"{label} path is missing")
    candidate = (build_root / Path(*PurePosixPath(relative).parts)).resolve()
    try:
        candidate.relative_to(build_root)
    except ValueError as exc:
        raise ValidationError(f"{label} path escapes the configured build") from exc
    require(candidate == expected_path.resolve(), f"{label} points at a stale configured artifact")
    require(candidate.is_file(), f"{label} configured artifact is missing")
    require(
        normalized_sha256(record.get("sha256"), label) == sha256(candidate),
        f"{label} hash differs from the final configured artifact",
    )


def _validate_configured_volk_foundation(
    root: Path,
    build_root: Path,
    static_model: dict[str, Any] | None,
    state: dict[str, Any],
) -> None:
    if static_model is None:
        return
    foundations = static_model.get("repositoryExternalFoundations", {})
    require(isinstance(foundations, dict), "static repository external-foundation model changed")
    authority = foundations.get(EXPECTED_VOLK_TARGET)
    if authority is None:
        return
    expected_authority_keys = {
        "target",
        "repository",
        "tag",
        "owner",
        "declarationLine",
        "populationLine",
        "edgeLine",
        "inheritedCompileAuthority",
    }
    require(
        isinstance(authority, dict)
        and set(authority) == expected_authority_keys
        and authority.get("target") == EXPECTED_VOLK_TARGET
        and authority.get("repository") == EXPECTED_VOLK_REPOSITORY
        and authority.get("tag") == EXPECTED_VOLK_TAG
        and authority.get("owner") == "demo_core",
        "static volk foundation authority changed",
    )
    inherited_compile_authority = authority.get("inheritedCompileAuthority")
    require(
        isinstance(inherited_compile_authority, list)
        and all(
            isinstance(record, dict)
            and set(record) == {"command", "arguments", "line", "guard"}
            and isinstance(record.get("command"), str)
            and isinstance(record.get("arguments"), list)
            and all(isinstance(value, str) for value in record["arguments"])
            and type(record.get("line")) is int
            and record["line"] > 0
            and isinstance(record.get("guard"), str)
            for record in inherited_compile_authority
        ),
        "static volk inherited compile authority changed",
    )

    target_models = state.get("targetModels")
    require(isinstance(target_models, dict), "configured state lacks target models for volk provenance")
    volk = target_models.get(EXPECTED_VOLK_TARGET)
    demo_core = target_models.get("demo_core")
    require(isinstance(volk, dict), "configured graph lacks the real volk target")
    require(isinstance(demo_core, dict), "configured graph lacks the demo_core volk owner")
    require(
        volk.get("name") == EXPECTED_VOLK_TARGET
        and str(volk.get("id", "")).split("::@", 1)[0] == EXPECTED_VOLK_TARGET
        and volk.get("type") == "STATIC_LIBRARY",
        "configured volk must be a real non-imported, non-aliased static target",
    )
    model_ids: dict[str, str] = {}
    for model_name, model in target_models.items():
        if not isinstance(model, dict) or not isinstance(model.get("id"), str):
            continue
        model_id = model["id"]
        require(model_id not in model_ids.values(), f"configured target ID is ambiguous: {model_id}")
        model_ids[model_name] = model_id
    volk_id = volk.get("id")
    demo_core_id = demo_core.get("id")
    require(
        isinstance(volk_id, str)
        and bool(volk_id)
        and isinstance(demo_core_id, str)
        and bool(demo_core_id),
        "configured volk owner/target IDs are missing",
    )
    reference_ids = state.get("targetIds")
    require(
        isinstance(reference_ids, dict)
        and set(reference_ids) == set(target_models)
        and all(
            isinstance(model, dict)
            and isinstance(reference_ids.get(name), str)
            and model.get("name") == name
            and model.get("id") == reference_ids[name]
            for name, model in target_models.items()
        ),
        "configured target references and replies disagree on exact IDs/names",
    )
    pending_dependency_names = [EXPECTED_VOLK_TARGET]
    scanned_dependency_names: set[str] = set()
    while pending_dependency_names:
        dependency_owner = pending_dependency_names.pop()
        if dependency_owner in scanned_dependency_names:
            continue
        scanned_dependency_names.add(dependency_owner)
        dependency_model = target_models.get(dependency_owner)
        if not isinstance(dependency_model, dict):
            continue
        for dependency in dependency_model.get("dependencies", []):
            if not isinstance(dependency, dict):
                continue
            dependency_name = str(dependency.get("id", "")).split("::@", 1)[0]
            _assert_no_forbidden_dependency(
                dependency_name,
                f"configured volk dependency closure through {dependency_owner}",
            )
            pending_dependency_names.append(dependency_name)
    reference_paths = state.get("targetPaths")
    require(
        isinstance(reference_paths, dict) and set(reference_paths) == set(target_models),
        "configured target references and reply paths disagree",
    )
    reply_root = (build_root / ".cmake/api/v1/reply").resolve()
    target_replies: dict[str, dict[str, Any]] = {}
    for target_name, target_path_value in reference_paths.items():
        require(
            isinstance(target_path_value, (str, Path)),
            f"configured target reply path is invalid: {target_name}",
        )
        target_path = Path(target_path_value)
        if not target_path.is_absolute():
            target_path = reply_root / target_path
        target_path = target_path.resolve()
        try:
            target_path.relative_to(reply_root)
        except ValueError as exc:
            raise ValidationError(
                f"configured target reply path escapes the File API reply root: {target_name}"
            ) from exc
        require(target_path.is_file(), f"configured target reply is missing: {target_name}")
        try:
            target_reply = dependency_identity.load_json(target_path)
        except dependency_identity.IdentityError as exc:
            raise ValidationError(f"configured target reply cannot be read: {target_name}") from exc
        require(
            isinstance(target_reply, dict)
            and target_reply.get("name") == target_name
            and target_reply.get("id") == reference_ids[target_name],
            f"configured target reply identity changed: {target_name}",
        )
        target_replies[target_name] = target_reply

    pending_targets = [volk]
    visited_targets: set[str] = set()
    while pending_targets:
        configured_target = pending_targets.pop()
        configured_name = str(configured_target.get("name", ""))
        if configured_name in visited_targets:
            continue
        visited_targets.add(configured_name)
        dependencies = configured_target.get("dependencies", [])
        require(isinstance(dependencies, list), f"configured target {configured_name!r} dependencies changed")
        for dependency in dependencies:
            require(isinstance(dependency, dict), f"configured target {configured_name!r} has an invalid dependency")
            dependency_id = dependency.get("id")
            require(isinstance(dependency_id, str) and dependency_id, "configured volk closure dependency ID is invalid")
            dependency_name = dependency_id.split("::@", 1)[0]
            _assert_no_forbidden_dependency(
                dependency_name,
                f"configured volk dependency closure through {configured_name}",
            )
            dependency_target = target_models.get(dependency_name)
            require(
                isinstance(dependency_target, dict)
                and reference_ids.get(dependency_name) == dependency_id,
                "configured volk closure dependency does not bind the exact referenced target ID",
            )
            if dependency_name not in visited_targets:
                pending_targets.append(dependency_target)

    root = root.resolve()
    build_root = build_root.resolve()
    literal_source_root = build_root / "_deps/volk-src"
    literal_target_build_root = build_root / "_deps/volk-build"
    source_root = literal_source_root.resolve()
    target_build_root = literal_target_build_root.resolve()
    require(
        source_root == literal_source_root and target_build_root == literal_target_build_root,
        "configured volk FetchContent roots must not use redirected filesystem links",
    )
    paths = volk.get("paths")
    require(isinstance(paths, dict), "configured volk target lacks source/build provenance")
    source_value = paths.get("source")
    build_value = paths.get("build")
    require(
        isinstance(source_value, str) and Path(source_value).is_absolute(),
        "configured volk source provenance must be an absolute FetchContent checkout",
    )
    configured_source_root = Path(source_value).resolve()
    configured_build_root = (
        Path(build_value).resolve()
        if isinstance(build_value, str) and Path(build_value).is_absolute()
        else (build_root / str(build_value or "")).resolve()
    )
    require(
        configured_source_root == source_root and configured_build_root == target_build_root,
        "configured volk target escaped the canonical build-local FetchContent source/build roots",
    )
    require(source_root.is_dir(), "configured volk FetchContent source checkout is missing")
    require(target_build_root.is_dir(), "configured volk FetchContent build directory is missing")

    cache_entries = state.get("cacheEntries")
    if cache_entries is None:
        try:
            _, configured_objects, _ = dependency_identity._file_api_objects(build_root)
            cache_entries = dependency_identity._cache_entries(
                dependency_identity.load_json(configured_objects["cache"])
            )
        except (dependency_identity.IdentityError, OSError) as exc:
            raise ValidationError("configured volk target lacks final cache provenance") from exc
    require(isinstance(cache_entries, dict), "configured volk target lacks final cache provenance")
    expected_cache_values = {
        "VOLK_HEADERS_ONLY": "OFF",
        "VOLK_INSTALL": "OFF",
        "VOLK_NAMESPACE": "OFF",
        "VOLK_PULL_IN_VULKAN": "ON",
        "VOLK_STATIC_DEFINES": "",
        "VULKAN_HEADERS_INSTALL_DIR": "",
    }
    require(
        set(expected_cache_values) <= set(cache_entries)
        and all(str(cache_entries[name]).upper() == value for name, value in expected_cache_values.items()),
        "configured volk cache options differ from the canonical static build",
    )
    require(
        Path(str(cache_entries.get("volk_SOURCE_DIR", ""))).resolve() == source_root
        and Path(str(cache_entries.get("volk_BINARY_DIR", ""))).resolve() == target_build_root,
        "configured volk cache roots differ from the canonical FetchContent roots",
    )
    vulkan_include_value = cache_entries.get("Vulkan_INCLUDE_DIR")
    require(
        isinstance(vulkan_include_value, str)
        and Path(vulkan_include_value).is_absolute()
        and Path(vulkan_include_value).resolve().is_dir(),
        "configured volk target lacks the resolved Vulkan include identity",
    )
    vulkan_include_root = Path(vulkan_include_value).resolve()

    expected_sources = {
        (source_root / "volk.h").resolve(): False,
        (source_root / "volk.c").resolve(): True,
    }
    source_records = volk.get("sources")
    require(isinstance(source_records, list), "configured volk target lacks source records")
    actual_sources: dict[Path, bool] = {}
    source_compile_groups: dict[Path, int | None] = {}
    source_indexes: dict[Path, int] = {}
    for source_index, record in enumerate(source_records):
        require(isinstance(record, dict), "configured volk target contains an invalid source record")
        value = record.get("path")
        require(isinstance(value, str) and Path(value).is_absolute(), "configured volk source path is not absolute")
        path = Path(value).resolve()
        require(path not in actual_sources, f"configured volk target repeats source {path}")
        compile_group_index = record.get("compileGroupIndex")
        require(
            compile_group_index is None or type(compile_group_index) is int,
            "configured volk source compile-group index is invalid",
        )
        actual_sources[path] = compile_group_index is not None
        source_compile_groups[path] = compile_group_index
        source_indexes[path] = source_index
    require(actual_sources == expected_sources, "configured volk target source/compiled identity changed")
    require(all(path.is_file() for path in expected_sources), "configured volk target source files are missing")

    def backtrace_origin(target: dict[str, Any], index: Any, label: str) -> tuple[str, Path, int]:
        graph = target.get("backtraceGraph")
        require(isinstance(graph, dict), f"{label} lacks a backtrace graph")
        commands = graph.get("commands")
        files = graph.get("files")
        nodes = graph.get("nodes")
        require(
            isinstance(commands, list) and isinstance(files, list) and isinstance(nodes, list),
            f"{label} backtrace graph changed",
        )
        require(type(index) is int and 0 <= index < len(nodes), f"{label} backtrace index is invalid")
        node = nodes[index]
        require(isinstance(node, dict), f"{label} backtrace node is invalid")
        command_index = node.get("command")
        file_index = node.get("file")
        line = node.get("line")
        require(
            type(command_index) is int
            and 0 <= command_index < len(commands)
            and type(file_index) is int
            and 0 <= file_index < len(files)
            and type(line) is int,
            f"{label} backtrace origin is incomplete",
        )
        file_value = files[file_index]
        require(isinstance(file_value, str) and file_value, f"{label} backtrace file is invalid")
        file_path = Path(file_value)
        if not file_path.is_absolute():
            file_path = root / file_path
        command_name = commands[command_index]
        require(isinstance(command_name, str), f"{label} backtrace command is invalid")
        return command_name, file_path.resolve(), line

    source_commands_by_path: dict[Path, list[CMakeCommand]] = {}

    def source_command_at(origin: tuple[str, Path, int], label: str) -> CMakeCommand:
        command_name, source_path, source_line = origin
        require(source_path.is_file(), f"{label} backtrace source file is missing")
        if source_path not in source_commands_by_path:
            source_commands_by_path[source_path] = _parse_cmake_commands(source_path)
        matches = [
            candidate
            for candidate in source_commands_by_path[source_path]
            if candidate.line == source_line and candidate.name == command_name.lower()
        ]
        if len(matches) != 1:
            if "demo_core volk" in label:
                detail = "literal repository edge"
            elif "volk target" in label:
                detail = "canonical FetchContent target definition"
            else:
                detail = "canonical source command"
            raise ValidationError(f"{label} backtrace does not match the {detail}")
        return matches[0]

    def validate_backtrace_graph(target: dict[str, Any], label: str) -> None:
        graph = target.get("backtraceGraph")
        require(isinstance(graph, dict), f"{label} lacks a backtrace graph")
        commands = graph.get("commands")
        files = graph.get("files")
        nodes = graph.get("nodes")
        require(
            isinstance(commands, list) and isinstance(files, list) and isinstance(nodes, list),
            f"{label} backtrace graph changed",
        )
        for node_index, node in enumerate(nodes):
            require(isinstance(node, dict), f"{label} backtrace node is invalid")
            parent = node.get("parent")
            require(
                parent is None or type(parent) is int and 0 <= parent < len(nodes) and parent != node_index,
                f"{label} backtrace parent is invalid",
            )
            seen: set[int] = set()
            cursor = node_index
            while True:
                require(cursor not in seen, f"{label} backtrace contains a parent cycle")
                seen.add(cursor)
                next_parent = nodes[cursor].get("parent")
                if next_parent is None:
                    break
                require(
                    type(next_parent) is int and 0 <= next_parent < len(nodes),
                    f"{label} backtrace parent is invalid",
                )
                cursor = next_parent
            # Command/file/line bounds and parent ancestry are checked here;
            # each consumed origin is matched against its exact source command
            # below so the rejection identifies the affected provenance.

    validate_backtrace_graph(volk, "configured volk target")
    validate_backtrace_graph(demo_core, "configured demo_core target")

    compile_groups = volk.get("compileGroups")
    require(
        isinstance(compile_groups, list) and len(compile_groups) == 1,
        "configured volk target must have exactly one compile group",
    )
    compile_group = compile_groups[0]
    require(
        isinstance(compile_group, dict)
        and set(compile_group)
        <= {"compileCommandFragments", "defines", "includes", "language", "sourceIndexes"},
        "configured volk compile-group shape changed",
    )
    volk_c = (source_root / "volk.c").resolve()
    require(
        source_compile_groups.get(volk_c) == 0
        and compile_group.get("language") == "C"
        and compile_group.get("sourceIndexes") == [source_indexes[volk_c]],
        "configured volk.c compile-group language/source identity changed",
    )
    defines = compile_group.get("defines", [])
    require(
        isinstance(defines, list) and not defines,
        "configured volk compile definitions differ from the canonical empty set",
    )
    compile_fragments = compile_group.get("compileCommandFragments", [])
    require(isinstance(compile_fragments, list), "configured volk compile fragments changed")

    def fragment_tokens(value: str) -> list[str]:
        try:
            if re.search(r"(?:^|\s)/[A-Za-z]", value):
                return dependency_identity._windows_command_line_tokens(
                    value,
                    "configured volk compile fragment",
                )
            return shlex.split(value, posix=True)
        except (ValueError, dependency_identity.IdentityError) as exc:
            raise ValidationError("configured volk compile fragment cannot be parsed") from exc

    configuration = state.get("configuration")
    require(isinstance(configuration, str), "configured volk configuration identity is missing")
    cache_flag_names = ["CMAKE_C_FLAGS"]
    if configuration:
        cache_flag_names.append(f"CMAKE_C_FLAGS_{configuration.upper()}")
    cache_fragment_tokens: list[str] = []
    for cache_flag_name in cache_flag_names:
        value = str(cache_entries.get(cache_flag_name, "") or "")
        if value.strip():
            cache_fragment_tokens.extend(fragment_tokens(value))
    allowed_platform_definitions = {"NDEBUG", "WIN32", "_WINDOWS"}
    for token in cache_fragment_tokens:
        definition = re.fullmatch(r"(?:/D|-D)(.+)", token, re.IGNORECASE)
        require(
            definition is None or definition.group(1).split("=", 1)[0] in allowed_platform_definitions,
            f"configured volk cache compiler definition is unauthorized: {token}",
        )
        require(
            not token.startswith("@")
            and not re.match(r"(?i)^(?:/FI|-include|-imacros)(?:$|=|[^A-Za-z])", token),
            f"configured volk cache compiler input is unauthorized: {token}",
        )

    baseline_fragments: list[str] = []
    authorized_fragment_origins = {
        (
            record["command"],
            (root / "CMakeLists.txt").resolve(),
            record["line"],
        ): record
        for record in inherited_compile_authority
        if record["command"] == "add_compile_options"
    }
    for fragment_index, fragment in enumerate(compile_fragments):
        require(
            isinstance(fragment, dict)
            and set(fragment) in ({"fragment"}, {"fragment", "backtrace"})
            and isinstance(fragment.get("fragment"), str)
            and bool(fragment["fragment"].strip()),
            "configured volk compile fragment record is invalid",
        )
        value = fragment["fragment"]
        if "backtrace" not in fragment:
            baseline_fragments.append(value)
            continue
        origin = backtrace_origin(
            volk,
            fragment.get("backtrace"),
            f"configured volk compile fragment {fragment_index}",
        )
        record = authorized_fragment_origins.get(origin)
        require(record is not None, "configured volk compile fragment has unauthorized CMake provenance")
        source_command = source_command_at(origin, f"configured volk compile fragment {fragment_index}")
        require(
            _tokenize_cmake(source_command.body) == record["arguments"]
            and fragment_tokens(value) == record["arguments"],
            "configured volk compile fragment differs from its static source authority",
        )

    actual_baseline_tokens = [
        token for value in baseline_fragments for token in fragment_tokens(value)
    ]
    generated_compile_tokens = {
        "-MD",
        "-MDd",
        "-MT",
        "-MTd",
        "-Zi",
        "-Z7",
        "/MD",
        "/MDd",
        "/MT",
        "/MTd",
        "/Zi",
        "/Z7",
        "-fPIC",
        "-fPIE",
    }
    remaining_baseline = list(actual_baseline_tokens)
    for expected_token in cache_fragment_tokens:
        require(
            expected_token in remaining_baseline,
            "configured volk compile fragments omit the configured toolchain baseline",
        )
        remaining_baseline.remove(expected_token)
    require(
        all(token in generated_compile_tokens for token in remaining_baseline),
        "configured volk compile fragments contain unauthorized options",
    )
    includes = compile_group.get("includes")
    require(
        isinstance(includes, list) and len(includes) == 2,
        "configured volk include identity changed",
    )
    include_paths: list[Path] = []
    for include_index, include in enumerate(includes):
        require(isinstance(include, dict), "configured volk include record is invalid")
        include_value = include.get("path")
        require(
            isinstance(include_value, str) and Path(include_value).is_absolute(),
            "configured volk include path is not absolute",
        )
        include_path = Path(include_value).resolve()
        include_paths.append(include_path)
        include_origin = backtrace_origin(
            volk,
            include.get("backtrace"),
            f"configured volk include {include_index}",
        )
        require(
            include_origin[0] == "target_include_directories"
            and include_origin[1] == (source_root / "CMakeLists.txt").resolve(),
            "configured volk include was not produced by canonical FetchContent CMake",
        )
        include_command = source_command_at(
            include_origin,
            f"configured volk include {include_index}",
        )
        expected_include_tokens = (
            [
                "volk",
                "PUBLIC",
                "$<BUILD_INTERFACE:${CMAKE_CURRENT_LIST_DIR}>",
                "$<INSTALL_INTERFACE:include>",
            ]
            if include_index == 0
            else ["volk", "PUBLIC", "${VOLK_INCLUDES}"]
        )
        require(
            _tokenize_cmake(include_command.body) == expected_include_tokens,
            "configured volk include backtrace does not match canonical source arguments",
        )
    require(
        include_paths == [source_root, vulkan_include_root],
        "configured volk include roots differ from the FetchContent/Vulkan identities",
    )

    volk_origin = backtrace_origin(volk, volk.get("backtrace"), "configured volk target")
    require(
        volk_origin[0] == "add_library" and volk_origin[1] == (source_root / "CMakeLists.txt").resolve(),
        "configured volk target was not defined by its canonical FetchContent source",
    )
    require(
        _tokenize_cmake(source_command_at(volk_origin, "configured volk target").body)
        == ["volk", "STATIC", "volk.h", "volk.c"],
        "configured volk target definition arguments are not canonical",
    )
    for source_record in source_records:
        source_origin = backtrace_origin(
            volk,
            source_record.get("backtrace"),
            "configured volk source",
        )
        require(
            source_origin == volk_origin,
            "configured volk source was not attached by the canonical target definition",
        )
    allowed_backtrace_files = {(source_root / "CMakeLists.txt").resolve(), (root / "CMakeLists.txt").resolve()}
    backtrace_files = volk.get("backtraceGraph", {}).get("files", [])
    resolved_backtrace_files = set()
    for value in backtrace_files:
        require(isinstance(value, str) and value, "configured volk target has an invalid backtrace file")
        candidate = Path(value)
        if not candidate.is_absolute():
            candidate = root / candidate
        resolved_backtrace_files.add(candidate.resolve())
    require(
        resolved_backtrace_files <= allowed_backtrace_files
        and (source_root / "CMakeLists.txt").resolve() in resolved_backtrace_files,
        "configured volk target was mutated by non-canonical CMake provenance",
    )

    volk_dependencies = [
        dependency
        for dependency in demo_core.get("dependencies", [])
        if isinstance(dependency, dict)
        and str(dependency.get("id", "")).split("::@", 1)[0] == EXPECTED_VOLK_TARGET
    ]
    require(
        len(volk_dependencies) == 1 and volk_dependencies[0].get("id") == volk_id,
        "configured demo_core must have exactly one real volk dependency with the exact target ID",
    )
    edge_origin = backtrace_origin(
        demo_core,
        volk_dependencies[0].get("backtrace"),
        "configured demo_core volk dependency",
    )
    require(
        edge_origin
        == (
            "target_link_libraries",
            (root / "CMakeLists.txt").resolve(),
            authority["edgeLine"],
        ),
        "configured demo_core volk dependency does not match the literal repository edge",
    )
    require(
        _tokenize_cmake(source_command_at(edge_origin, "configured demo_core volk dependency").body)
        == list(EXPECTED_DEMO_CORE_COMMON_RENDERER_LINK),
        "configured demo_core volk dependency source arguments are not canonical",
    )
    immutable_reference_ids = state.get("targetReferenceIds")
    require(
        isinstance(immutable_reference_ids, dict)
        and immutable_reference_ids == reference_ids,
        "configured target reference identity snapshot is missing or changed",
    )
    require(
        all(target_replies[name] == model for name, model in target_models.items()),
        "configured in-memory target models differ from their exact File API replies",
    )

    def git_output(*arguments: str) -> str:
        try:
            completed = subprocess.run(
                ["git", "-C", str(source_root), *arguments],
                check=False,
                capture_output=True,
                text=True,
                encoding="utf-8",
                errors="replace",
            )
        except OSError as exc:
            raise ValidationError(f"cannot execute Git for configured volk provenance: {exc}") from exc
        require(
            completed.returncode == 0,
            f"cannot verify configured volk Git provenance ({' '.join(arguments)}): {completed.stderr.strip()}",
        )
        return completed.stdout.strip()

    require(
        _normalized_git_remote(git_output("remote", "get-url", "origin"))
        == _normalized_git_remote(EXPECTED_VOLK_REPOSITORY),
        "configured volk Git origin differs from the repository declaration",
    )
    require(
        git_output("symbolic-ref", "--quiet", "--short", "HEAD") == EXPECTED_VOLK_TAG,
        "configured volk checkout is not on the declared master branch",
    )
    require(
        not git_output("status", "--porcelain", "--untracked-files=no"),
        "configured volk checkout has tracked source mutations",
    )


def _configured_editor_static_model(root: Path, build_root: Path) -> dict[str, Any] | None:
    if not (root / "docs/editor-parity/manifest.json").is_file():
        return None
    _, objects, _ = dependency_identity._file_api_objects(build_root)
    cache = dependency_identity._cache_entries(dependency_identity.load_json(objects["cache"]))
    # Match these opt-in branches to the generated build, not option() defaults.
    # The remaining source/dependency checks and configured graph checks still run.
    options = {
        name: str(cache[name])
        for name in ("BUILD_TESTING", "DEMO_SKIP_EDITOR_DEPENDENCY_PREFLIGHT")
        if name in cache
    }
    return validate_editor_dependency_boundary(root, cache_variables=options)


def validate_configured_editor_build_identity(root: Path, identity: dict[str, Any]) -> None:
    build_root = Path(identity.get("buildRoot", "")).resolve()
    try:
        static_model = _configured_editor_static_model(root, build_root)
        state = dependency_identity.inspect_configured_editor_build(
            root,
            build_root,
            requested_config=str(identity.get("configuredBuildEvidence", {}).get("configuration", "")),
            require_ninja_dependencies=True,
            expected_static_model=static_model,
        )
        _validate_configured_volk_foundation(root, build_root, static_model, state)
    except dependency_identity.IdentityError as exc:
        raise ValidationError(f"final configured editor dependency gate failed: {exc}") from exc

    evidence = identity.get("configuredBuildEvidence")
    expected_keys = {
        "cmakeGenerator",
        "configuration",
        "fileApiIndex",
        "fileApiCodemodel",
        "fileApiCache",
        "fileApiToolchains",
        "fileApiImguiTarget",
        "compileCommands",
        "buildSystem",
        "compiledCommandSha256",
    }
    require(isinstance(evidence, dict) and set(evidence) == expected_keys, "configured build evidence fields changed")
    require(evidence.get("cmakeGenerator") == state["cmakeGenerator"], "identity CMake generator differs from File API")
    require(evidence.get("configuration") == state["configuration"], "identity configuration differs from File API")
    _validate_configured_identity_record(build_root, evidence.get("fileApiIndex"), state["indexPath"], "identity File API index")
    _validate_configured_identity_record(
        build_root,
        evidence.get("fileApiCodemodel"),
        state["objects"]["codemodel"],
        "identity File API codemodel",
    )
    _validate_configured_identity_record(
        build_root,
        evidence.get("fileApiCache"),
        state["objects"]["cache"],
        "identity File API cache",
    )
    _validate_configured_identity_record(
        build_root,
        evidence.get("fileApiToolchains"),
        state["objects"]["toolchains"],
        "identity File API toolchains",
    )
    _validate_configured_identity_record(
        build_root,
        evidence.get("fileApiImguiTarget"),
        state["targetPaths"][EXPECTED_DEAR_IMGUI_TARGET],
        "identity File API imgui_demo target",
    )
    _validate_configured_identity_record(
        build_root,
        evidence.get("compileCommands"),
        state["compileCommandsPath"],
        "identity compile commands",
    )
    _validate_configured_identity_record(
        build_root,
        evidence.get("buildSystem"),
        state["buildSystemPath"],
        "identity Ninja build system",
    )
    source_root = state["imguiSourceRoot"]
    actual_command_hashes = {
        dependency_identity._compile_entry_source(entry).relative_to(source_root).as_posix():
            dependency_identity.text_sha256(dependency_identity._compile_entry_text(entry))
        for entry in state["compileEntries"][EXPECTED_DEAR_IMGUI_TARGET]
    }
    require(
        evidence.get("compiledCommandSha256") == dict(sorted(actual_command_hashes.items())),
        "identity compiled command hashes differ from final compile_commands.json",
    )
    configured_sources = sorted(path.relative_to(source_root).as_posix() for path in state["imguiSources"])
    configured_compiled = sorted(path.relative_to(source_root).as_posix() for path in state["imguiCompiledSources"])
    require(
        identity.get("targetSourceRelativePaths") == configured_sources,
        "Dear ImGui identity target inputs differ from final File API sources",
    )
    require(
        identity.get("compiledSourceRelativePaths") == configured_compiled,
        "Dear ImGui identity compiled inputs differ from final File API/compile commands",
    )


def validate_configured_editor_build(
    root: Path,
    build_root: Path,
    config: str = "",
    *,
    require_ninja_dependencies: bool = True,
) -> None:
    try:
        static_model = _configured_editor_static_model(root, build_root)
        state = dependency_identity.inspect_configured_editor_build(
            root,
            build_root,
            requested_config=config,
            require_ninja_dependencies=require_ninja_dependencies,
            expected_static_model=static_model,
        )
        _validate_configured_volk_foundation(root, build_root, static_model, state)
    except dependency_identity.IdentityError as exc:
        raise ValidationError(f"final configured editor dependency gate failed: {exc}") from exc


def _identity_git(source_root: Path, *arguments: str) -> str:
    try:
        completed = subprocess.run(
            ["git", "-C", str(source_root), *arguments],
            check=False,
            capture_output=True,
            text=True,
            encoding="utf-8",
            errors="replace",
        )
    except OSError as exc:
        raise ValidationError(f"cannot execute Git for resolved Dear ImGui identity: {exc}") from exc
    require(
        completed.returncode == 0,
        f"cannot verify resolved Dear ImGui Git identity ({' '.join(arguments)}): {completed.stderr.strip()}",
    )
    return completed.stdout.strip()


def _normalized_git_remote(value: str) -> str:
    return value.strip().replace("\\", "/").rstrip("/").lower()


def validate_resolved_dear_imgui_identity(root: Path, identity_path: Path | None) -> None:
    require(identity_path is not None, "P1 readiness requires --resolved-build-identity from the configured build")
    root = root.resolve()
    identity_path = identity_path.resolve()
    require(identity_path.is_file(), f"resolved Dear ImGui build identity is missing: {identity_path}")
    try:
        identity_path.relative_to(root)
    except ValueError:
        pass
    else:
        raise ValidationError("resolved Dear ImGui build identity must be build-local and outside the repository")
    identity = load_json(identity_path)
    expected_keys = {
        "schemaVersion",
        "kind",
        "dependency",
        "repository",
        "target",
        "sourceMode",
        "projectSourceRoot",
        "projectCMakeSha256",
        "generator",
        "resolvedSourceRoot",
        "buildRoot",
        "sourceOverride",
        "resolvedGitCommit",
        "resolvedGitTree",
        "platformBackend",
        "targetSourceRelativePaths",
        "compiledSourceRelativePaths",
        "compiledSourceSha256",
        "targetSourceSha256",
        "configuredBuildEvidence",
    }
    require(isinstance(identity, dict) and set(identity) == expected_keys, "resolved Dear ImGui identity fields changed")
    require(identity.get("schemaVersion") == 2, "resolved Dear ImGui identity schemaVersion changed")
    require(identity.get("kind") == "mgif.editor-dependency-build-identity", "resolved Dear ImGui identity kind changed")
    require(identity.get("dependency") == "dear_imgui", "resolved Dear ImGui dependency identity changed")
    require(identity.get("repository") == EXPECTED_DEAR_IMGUI_REPOSITORY, "resolved Dear ImGui repository changed")
    require(identity.get("target") == EXPECTED_DEAR_IMGUI_TARGET, "resolved Dear ImGui target changed")
    require(identity.get("sourceMode") == "fetchcontent_git_outside_repository", "resolved Dear ImGui source mode changed")
    require(identity.get("sourceOverride") == {"cacheOrCli": False, "environment": False}, "Dear ImGui source override is forbidden")
    require(identity.get("resolvedGitCommit") == EXPECTED_DEAR_IMGUI_COMMIT, "resolved Dear ImGui commit changed")
    require(identity.get("resolvedGitTree") == EXPECTED_DEAR_IMGUI_TREE, "resolved Dear ImGui tree changed")

    project_root = Path(identity.get("projectSourceRoot", "")).resolve()
    build_root = Path(identity.get("buildRoot", "")).resolve()
    source_root = Path(identity.get("resolvedSourceRoot", "")).resolve()
    require(project_root == root, "resolved Dear ImGui identity belongs to a different project source tree")
    require(build_root.is_dir() and identity_path.parent == build_root, "resolved Dear ImGui identity is not rooted in its configured build")
    require(source_root == (build_root / "_deps/imgui-src").resolve(), "resolved Dear ImGui checkout is not the canonical build-local FetchContent source")
    require(source_root.is_dir(), "resolved Dear ImGui checkout is missing")
    require(root not in source_root.parents, "resolved Dear ImGui checkout cannot be repository-local")
    require(identity.get("projectCMakeSha256") == sha256(root / "CMakeLists.txt"), "resolved identity is stale for the current CMakeLists.txt")
    generator = identity.get("generator")
    require(
        isinstance(generator, dict) and set(generator) == {"path", "sha256"},
        "resolved Dear ImGui generator binding changed",
    )
    generator_path = resolve_repo_path(root, generator.get("path"), "resolved identity generator path")
    require(
        generator_path == (root / "tools/generate_editor_dependency_identity.py").resolve(),
        "resolved Dear ImGui identity used a noncanonical generator",
    )
    require(generator.get("sha256") == sha256(generator_path), "resolved Dear ImGui identity generator hash changed")

    backend = identity.get("platformBackend")
    require(backend in EXPECTED_DEAR_IMGUI_TARGET_SOURCE_VARIANTS, "resolved Dear ImGui platform backend changed")
    target_sources = identity.get("targetSourceRelativePaths")
    compiled_sources = identity.get("compiledSourceRelativePaths")
    require(
        target_sources == EXPECTED_DEAR_IMGUI_TARGET_SOURCE_VARIANTS[backend],
        "resolved Dear ImGui final target SOURCES are not canonical",
    )
    require(
        compiled_sources == EXPECTED_DEAR_IMGUI_COMPILED_SOURCE_VARIANTS[backend],
        "resolved Dear ImGui compiled source list is not canonical",
    )
    target_hashes = identity.get("targetSourceSha256")
    compiled_hashes = identity.get("compiledSourceSha256")
    require(isinstance(target_hashes, dict) and set(target_hashes) == set(target_sources), "resolved Dear ImGui target source hashes are incomplete")
    require(isinstance(compiled_hashes, dict) and set(compiled_hashes) == set(compiled_sources), "resolved Dear ImGui compiled source hashes are incomplete")
    for relative in target_sources:
        path = source_root.joinpath(*PurePosixPath(relative).parts)
        require(path.is_file(), f"resolved Dear ImGui target source is missing: {relative}")
        expected_hash = normalized_sha256(target_hashes.get(relative), f"resolved Dear ImGui target source {relative}")
        require(sha256(path) == expected_hash, f"resolved Dear ImGui target source hash changed: {relative}")
        if relative in compiled_hashes:
            require(
                normalized_sha256(compiled_hashes.get(relative), f"resolved Dear ImGui compiled source {relative}")
                == expected_hash,
                f"resolved Dear ImGui compiled source hash differs from target input hash: {relative}",
            )
    require(
        _identity_git(source_root, "rev-parse", "HEAD^{commit}").lower() == EXPECTED_DEAR_IMGUI_COMMIT,
        "live Dear ImGui checkout commit differs from resolved identity",
    )
    require(
        _identity_git(source_root, "rev-parse", "HEAD^{tree}").lower() == EXPECTED_DEAR_IMGUI_TREE,
        "live Dear ImGui checkout tree differs from resolved identity",
    )
    require(
        _normalized_git_remote(_identity_git(source_root, "remote", "get-url", "origin"))
        == _normalized_git_remote(EXPECTED_DEAR_IMGUI_REPOSITORY),
        "live Dear ImGui checkout origin differs from canonical upstream",
    )
    require(
        not _identity_git(source_root, "status", "--porcelain=v1", "--untracked-files=all"),
        "live Dear ImGui checkout differs from the pinned Git tree",
    )
    validate_configured_editor_build_identity(root, identity)


def validate_evidence(root: Path, evidence: Any, label: str) -> None:
    require(isinstance(evidence, dict), f"{label} must be an object")
    grade = evidence.get("grade")
    status = evidence.get("status")
    artifacts = evidence.get("artifacts")
    require(grade in {"none", "source", "test", "executable"}, f"{label} has an invalid grade")
    require(status in {"pending", "provisional", "pass", "fail", "blocked", "not_implemented"}, f"{label} has an invalid status")
    require(isinstance(artifacts, list), f"{label}.artifacts must be an array")
    require(len(artifacts) == len(set(artifacts)), f"{label}.artifacts must be unique")
    if grade == "none":
        require(not artifacts, f"{label} with grade none cannot claim artifacts")
        require(status not in {"provisional", "pass", "fail"}, f"{label} cannot claim {status} with grade none")
    if status in {"provisional", "pass", "fail"}:
        require(grade != "none", f"{label} status {status} requires an evidence grade")
        require(bool(artifacts), f"{label} status {status} requires at least one artifact")
    for index, artifact in enumerate(artifacts):
        artifact_path = resolve_repo_path(root, artifact, f"{label}.artifacts[{index}]")
        require(artifact_path.is_file(), f"{label} artifact is missing: {artifact}")


def _require_timezone_aware_timestamp(value: Any, label: str) -> None:
    require(isinstance(value, str) and value.strip(), f"{label} must be a non-empty timestamp")
    try:
        parsed = datetime.fromisoformat(value.replace("Z", "+00:00"))
    except ValueError as exc:
        raise ValidationError(f"{label} must be an ISO-8601 timestamp") from exc
    require(parsed.tzinfo is not None and parsed.utcoffset() is not None, f"{label} must include a timezone offset")


def validate_manifest(
    root: Path,
    mode: str = "scaffold",
    resolved_build_identity: Path | None = None,
) -> None:
    manifest_path = root / "docs/editor-parity/manifest.json"
    manifest = load_json(manifest_path)

    schema = load_json(root / "docs/editor-parity/schemas/parity-manifest.schema.json")
    try:
        validate_json_schema(manifest, schema)
    except JsonSchemaError as exc:
        raise ValidationError(f"manifest does not match parity-manifest.schema.json: {exc}") from exc

    require(manifest.get("schemaVersion") == 1, "manifest schemaVersion must be 1")
    require(
        manifest.get("specification", {}).get("sha256") == EXPECTED_SPEC_SHA256,
        "manifest specification hash is not the locked P0 task hash",
    )
    require(
        sha256(resolve_repo_path(root, manifest["specification"]["path"], "manifest specification path")) == EXPECTED_SPEC_SHA256,
        "live task document differs from the manifest specification hash",
    )
    require(
        manifest.get("reference", {}).get("commit") == EXPECTED_REFERENCE_COMMIT,
        "reference commit is not locked to 50373c7",
    )
    require(
        manifest.get("target", {}).get("commit") == EXPECTED_TARGET_COMMIT,
        "target baseline commit is not locked to 6c36567",
    )
    validate_dependency_boundary_policy(manifest)
    license_policy = manifest.get("licensePolicy")

    features = manifest.get("features")
    require(isinstance(features, list), "manifest features must be an array")
    feature_ids = [feature.get("id") for feature in features]
    require(feature_ids == EXPECTED_FEATURE_IDS, "manifest must contain ordered F01-F42 exactly once")
    for feature in features:
        validate_evidence(root, feature.get("referenceEvidence"), f"feature {feature['id']} referenceEvidence")
        validate_evidence(root, feature.get("mgifEvidence"), f"feature {feature['id']} mgifEvidence")

    traces = manifest.get("oracleTraces")
    require(isinstance(traces, list), "oracleTraces must be an array")
    trace_ids = [trace.get("id") for trace in traces]
    require(trace_ids == EXPECTED_ORACLE_IDS, "manifest must contain canonical O01-O10 traces in order")
    trace_by_id = {trace["id"]: trace for trace in traces}
    for trace in traces:
        trace_id = trace["id"]
        artifacts = trace.get("artifacts")
        require(isinstance(artifacts, list), f"trace {trace_id} artifacts must be an array")
        require(len(artifacts) == len(set(artifacts)), f"trace {trace_id} artifacts must be unique")
        require(trace.get("status") != "not_applicable", f"required trace {trace_id} cannot be not_applicable")
        allowed_statuses = {EXPECTED_ORACLE_SCAFFOLD_STATUSES[trace_id]}
        require(trace.get("status") in allowed_statuses, f"trace {trace_id} status does not match its evidence ceiling")
        require(artifacts == EXPECTED_ORACLE_ARTIFACTS[trace_id], f"trace {trace_id} must use its canonical reference artifacts")
        for index, artifact in enumerate(artifacts):
            artifact_path = resolve_repo_path(root, artifact, f"trace {trace_id} artifacts[{index}]")
            require(artifact_path.is_file(), f"trace artifact is missing: {artifact}")
    validate_reference_trace_artifacts(root)

    d20 = manifest.get("decisionGates", {}).get("D20", {})
    require(
        d20.get("status") in {"pending_executable_trace", "pending_user_decision", "resolved"},
        "D20 has an invalid status",
    )
    require(
        d20.get("allowedResolutions") == ["exact_defect_parity", "mgif_safety_exception"],
        "D20 allowed resolutions changed",
    )
    require(d20.get("requiredTraceIds") == ["O09-multi-scene-structural-matrix"], "D20 required trace set changed")
    if d20.get("status") != "resolved":
        require(d20.get("blocks") == [f"P{index}" for index in range(1, 11)], "unresolved D20 must block P1-P10")
        require(d20.get("status") == "pending_user_decision", "D20 executable evidence is closed and now requires the user's decision")
        require(d20.get("selectedResolution") is None, "unresolved D20 cannot select a resolution")
        require(d20.get("approvedBy") is None and d20.get("approvedAt") is None, "unresolved D20 cannot claim approval")
        require(d20.get("selectedPolicyContract") is None, "unresolved D20 cannot claim a selected-policy contract")
        require(manifest.get("status") != "p0_passed", "P0 cannot pass while D20 is unresolved")
    else:
        require(d20.get("blocks") == [], "resolved D20 must not retain stale P1-P10 blocks")
        require(
            d20.get("selectedResolution") in {"exact_defect_parity", "mgif_safety_exception"},
            "resolved D20 must select one approved resolution",
        )
        require(
            isinstance(d20.get("approvedBy"), str) and bool(d20["approvedBy"].strip()),
            "resolved D20 must record a non-empty approving user identity",
        )
        _require_timezone_aware_timestamp(d20.get("approvedAt"), "resolved D20 approvedAt")
        require(d20.get("approvedBy") == "MGIF project owner (user)", "resolved D20 approval identity changed")
        require(d20.get("approvedAt") == "2026-08-22T16:55:24Z", "resolved D20 durable attestation timestamp changed")
        require(d20.get("approvalEvidenceText") == "参考你的建议", "resolved D20 approval evidence text changed")
        timestamp_provenance = d20.get("approvalTimestampProvenance")
        require(
            timestamp_provenance
            == {
                "originalUserMessageTimestamp": None,
                "durableCoordinatorAttestationAt": "2026-08-22T16:55:24Z",
                "note": "The original user-message timestamp is unavailable and was not fabricated; approvedAt is the first durable coordinator recording/attestation time.",
            },
            "resolved D20 must distinguish unavailable user-message time from durable coordinator attestation",
        )
        for trace_id in d20.get("requiredTraceIds", []):
            require(trace_id in trace_by_id, f"D20 references unknown trace {trace_id}")
            trace = trace_by_id[trace_id]
            require(
                trace.get("status") in {"native_partial", "captured"},
                f"resolved D20 requires captured or hash-bound native_partial trace {trace_id}",
            )
            require(bool(trace.get("artifacts")), f"resolved D20 trace {trace_id} requires artifacts")
        selected_resolution = d20["selectedResolution"]
        require(
            json_values_equal(d20.get("selectedPolicyContract"), EXPECTED_D20_POLICY_CONTRACTS[selected_resolution]),
            "resolved D20 requires the complete machine-checkable selected-policy contract",
        )

    blocked_feature = next(feature for feature in features if feature["id"] == "F02")
    if d20.get("status") != "resolved":
        require(
            blocked_feature["referenceEvidence"]["status"] == "blocked",
            "F02 reference evidence must remain blocked while D20 is unresolved",
        )

    if manifest.get("status") == "p0_passed":
        require(d20.get("status") == "resolved", "complete P0 gate requires resolved D20")
        raise ValidationError("legacy manifest.status=p0_passed cannot represent P1 engineering readiness evidence debts")

    if mode == "scaffold":
        validate_p0_baseline_and_license(root, features, license_policy)
        return

    require(d20.get("status") == "resolved", "P1 engineering readiness gate requires resolved D20")
    require(manifest.get("status") == "p1_engineering_ready", "P1 engineering readiness gate requires manifest.status=p1_engineering_ready")
    require(
        json_values_equal(manifest.get("upstreamBuildIdentityContract"), EXPECTED_DEAR_IMGUI_BUILD_IDENTITY_CONTRACT),
        "P1 readiness requires the immutable Dear ImGui build-identity hashing contract",
    )
    validate_resolved_dear_imgui_identity(root, resolved_build_identity)
    for trace in traces:
        trace_id = trace["id"]
        require(
            trace.get("status") == EXPECTED_ORACLE_GATE_STATUSES[trace_id],
            f"P1 readiness trace {trace_id} status does not match its immutable observation ceiling",
        )
        require(bool(trace.get("artifacts")), f"P1 readiness trace lacks artifacts: {trace_id}")
    require(
        json_values_equal(
            trace_by_id["O09-multi-scene-structural-matrix"].get("p1Readiness"),
            EXPECTED_O09_READINESS,
        ),
        "O09 native_partial P1 readiness requires complete hash-bound 298-cell coverage and unchanged UI/Vulkan/GPU ceiling",
    )
    for trace_id, expected_disposition in {
        "O04-unique-root-delete": EXPECTED_O04_CONFLICT_DISPOSITION,
        "O06-gizmo-escape-release-same-frame": EXPECTED_O06_CONFLICT_DISPOSITION,
    }.items():
        require(
            json_values_equal(trace_by_id[trace_id].get("conflictDisposition"), expected_disposition),
            f"{trace_id} conflict requires an explicit observed-vs-target disposition and downstream acceptance debts",
        )
    for trace_id, expected_debt in EXPECTED_NATIVE_PARTIAL_DEBTS.items():
        require(
            json_values_equal(trace_by_id[trace_id].get("readinessDebt"), expected_debt),
            f"{trace_id} native_partial evidence debt must remain assigned to later UI/Vulkan/P10 acceptance IDs",
        )
    for feature in features:
        status = feature["referenceEvidence"]["status"]
        require(status in {"provisional", "pass", "fail", "not_implemented"}, f"feature {feature['id']} lacks closed reference evidence")
    validate_p0_baseline_and_license(
        root,
        features,
        license_policy,
        release_readiness=mode == "release",
        verify_repair=True,
    )
    validate_reference_trace_artifacts(root, verify_external=True)
    verify_mgif_baseline_external()


def validate_schemas(root: Path) -> None:
    schema_root = root / "docs/editor-parity/schemas"
    for name in ("parity-manifest.schema.json", "acceptance-report.schema.json"):
        schema = load_json(schema_root / name)
        require(schema.get("$schema") == "https://json-schema.org/draft/2020-12/schema", f"{name} must use Draft 2020-12")
        require(schema.get("type") == "object", f"{name} root type must be object")
        try:
            check_schema_supported(schema)
        except JsonSchemaError as exc:
            raise ValidationError(f"{name} uses an unsupported or invalid schema construct: {exc}") from exc


def validate_gltf_asset(document: Any, relative_path: str) -> tuple[list[Any], list[Any]]:
    require(isinstance(document, dict), f"fixture root must be an object: {relative_path}")
    require(document.get("asset", {}).get("version") == "2.0", f"fixture must declare glTF 2.0: {relative_path}")
    scenes = document.get("scenes", [])
    nodes = document.get("nodes", [])
    require(isinstance(scenes, list), f"fixture scenes must be an array: {relative_path}")
    require(isinstance(nodes, list), f"fixture nodes must be an array: {relative_path}")
    return scenes, nodes


def validate_fixture_semantics(relative_path: str, expected: str, document: Any) -> None:
    scenes, nodes = validate_gltf_asset(document, relative_path)
    invalid_child_edges: list[tuple[int, Any]] = []
    for parent_index, node in enumerate(nodes):
        require(isinstance(node, dict), f"fixture node must be an object: {relative_path} node {parent_index}")
        children = node.get("children", [])
        require(isinstance(children, list), f"fixture children must be an array: {relative_path} node {parent_index}")
        for child in children:
            if not isinstance(child, int) or isinstance(child, bool) or child < 0 or child >= len(nodes):
                invalid_child_edges.append((parent_index, child))

    if expected == "valid":
        require(not invalid_child_edges, f"fixture marked valid has an invalid child index: {relative_path}")
        for scene_index, scene in enumerate(scenes):
            require(isinstance(scene, dict), f"fixture scene must be an object: {relative_path} scene {scene_index}")
            roots = scene.get("nodes", [])
            require(isinstance(roots, list), f"fixture scene roots must be an array: {relative_path} scene {scene_index}")
            require(all(isinstance(root, int) and not isinstance(root, bool) and 0 <= root < len(nodes) for root in roots), f"fixture has invalid scene root: {relative_path}")
    elif expected == "invalid":
        require(
            relative_path in {
                "invalid_child_index.gltf",
                "invalid_cycle.gltf",
                "invalid_multiparent.gltf",
            },
            f"unexpected invalid editor fixture: {relative_path}",
        )
    else:
        raise ValidationError(f"fixture expected must be valid or invalid: {relative_path}")

    if relative_path == "minimal_scene.gltf":
        require(len(scenes) == 1 and scenes[0].get("nodes") == [0], "minimal_scene.gltf must have exactly one root")
        require(len(nodes) == 1 and nodes[0].get("name") == "Root", "minimal_scene.gltf root shape changed")
    elif relative_path == "multi_scene_oracle.gltf":
        require(document.get("scene") == 0, "multi_scene_oracle.gltf default scene must be 0")
        require(len(scenes) == 2, "multi_scene_oracle.gltf must have exactly two scenes")
        require(scenes[0].get("nodes") == [0, 1, 6], "multi-scene scene 0 roots must be [A0, A1, SharedRoot]")
        require(scenes[1].get("nodes") == [3, 4, 6], "multi-scene scene 1 roots must be [B0, B1, SharedRoot]")
        require(len(nodes) == 8, "multi_scene_oracle.gltf must have exactly eight nodes")
        require(nodes[0].get("children") == [2], "A0 must own AChild")
        require(nodes[3].get("children") == [5], "B0 must own BChild")
        require(nodes[6].get("children") == [7], "SharedRoot must own SharedChild")
        names = [node.get("name") for node in nodes]
        stable_ids = [node.get("extras", {}).get("fixtureStableId") for node in nodes]
        require(all(isinstance(name, str) and name for name in names), "multi-scene nodes require non-empty names")
        require(len(names) == len(set(names)), "multi-scene node names must be unique")
        require(all(isinstance(stable_id, str) and stable_id for stable_id in stable_ids), "multi-scene nodes require fixtureStableId")
        require(len(stable_ids) == len(set(stable_ids)), "multi-scene fixtureStableId values must be unique")
    elif relative_path == "unicode_names.gltf":
        names = [node.get("name") for node in nodes]
        require(names == ["场景节点", "emoji-🚀", "combining-é"], "unicode_names.gltf sentinel names changed")
    elif relative_path == "unknown_extension_roundtrip.gltf":
        require(document.get("extensionsUsed") == ["MGIF_test_unknown"], "unknown extension declaration changed")
        require("MGIF_test_unknown" in document.get("extensions", {}), "document unknown extension is missing")
        require(scenes and "MGIF_test_unknown" in scenes[0].get("extensions", {}), "scene unknown extension is missing")
        require(nodes and "MGIF_test_unknown" in nodes[0].get("extensions", {}), "node unknown extension is missing")
        require(document.get("asset", {}).get("extras", {}).get("roundtripSentinel") == "asset-extras-must-survive", "asset extras sentinel is missing")
        require(nodes[0].get("extras", {}).get("roundtripSentinel") == "node-extras-must-survive", "node extras sentinel is missing")
    elif relative_path == "invalid_child_index.gltf":
        require(invalid_child_edges == [(0, 7)], "invalid_child_index.gltf must contain exactly child index 7 on node 0")
    elif relative_path == "invalid_cycle.gltf":
        require(not invalid_child_edges, "invalid_cycle.gltf must use only in-range child indices")
        require(len(nodes) == 2, "invalid_cycle.gltf must contain exactly two nodes")
        require(nodes[0].get("children") == [1] and nodes[1].get("children") == [0], "invalid_cycle.gltf must contain the exact two-node cycle")
    elif relative_path == "invalid_multiparent.gltf":
        require(not invalid_child_edges, "invalid_multiparent.gltf must use only in-range child indices")
        require(len(nodes) == 3, "invalid_multiparent.gltf must contain exactly three nodes")
        require(nodes[0].get("children") == [2] and nodes[1].get("children") == [2], "invalid_multiparent.gltf must share child 2 between both parents")
    elif relative_path == "external_buffer.gltf":
        require(
            document.get("buffers") == [{"name": "ExternalBuffer", "uri": "external_buffer.bin", "byteLength": 9}],
            "external_buffer.gltf must reference the exact original nine-byte payload",
        )


def validate_corpus(root: Path) -> None:
    corpus_root = root / "tests/fixtures/gltf_editor"
    corpus = load_json(corpus_root / "corpus.json")
    require(corpus.get("schemaVersion") == 1, "corpus schemaVersion must be 1")
    policy = corpus.get("policy")
    require(isinstance(policy, dict), "corpus policy must be an object")
    require(policy.get("externalBytes") is True, "P1 corpus must declare its external buffer fixture")
    require(policy.get("externalBytesOrigin") == "hand-authored for MGIF", "external fixture origin must remain explicit")
    require(policy.get("thirdPartyBytes") is False, "editor corpus must not contain third-party asset bytes")
    require(policy.get("provenance") == "docs/editor-parity/LICENSE-SOURCES.md", "corpus provenance must use the authoritative ledger")
    require((root / policy["provenance"]).is_file(), "corpus provenance ledger is missing")
    fixtures = corpus.get("fixtures")
    require(isinstance(fixtures, list) and fixtures, "corpus must list fixtures")

    paths = [fixture.get("path") for fixture in fixtures]
    require(len(paths) == len(set(paths)), "corpus fixture paths must be unique")
    for fixture in fixtures:
        relative_path = fixture.get("path")
        require(isinstance(relative_path, str) and relative_path, "fixture path must be non-empty")
        fixture_path = resolve_repo_path(corpus_root, relative_path, f"fixture path {relative_path!r}")
        require(fixture_path.is_file(), f"fixture is missing: {relative_path}")
        require(fixture_path.stat().st_size == fixture.get("bytes"), f"fixture length mismatch: {relative_path}")
        require(sha256(fixture_path) == fixture.get("sha256"), f"fixture hash mismatch: {relative_path}")
        covers = fixture.get("covers")
        require(isinstance(covers, list) and covers and all(isinstance(item, str) and item for item in covers), f"fixture covers must be non-empty: {relative_path}")
        if fixture_path.suffix == ".gltf":
            validate_fixture_semantics(relative_path, fixture.get("expected"), load_json(fixture_path))
        else:
            original_payloads = {
                "external_buffer.bin": b"MGIFDATA\n",
                "external_image.payload": b"MGIF-P1-ORIGINAL-IMAGE-PAYLOAD\n",
            }
            require(relative_path in original_payloads, f"unsupported non-JSON fixture: {relative_path}")
            require(fixture.get("expected") == "binary", f"{relative_path} must be classified as binary")
            require(
                fixture_path.read_bytes() == original_payloads[relative_path],
                f"{relative_path} original byte sentinel changed",
            )

    committed_fixture_paths = {
        path.name
        for path in corpus_root.iterdir()
        if path.is_file() and path.name not in {"README.md", "corpus.json"}
    }
    require(set(paths) == committed_fixture_paths, "corpus must bind every committed fixture and no absent fixture")

    required = {
        "minimal_scene.gltf",
        "multi_scene_oracle.gltf",
        "unknown_extension_roundtrip.gltf",
        "invalid_child_index.gltf",
        "invalid_cycle.gltf",
        "invalid_multiparent.gltf",
        "external_buffer.gltf",
        "external_buffer.bin",
        "external_image.gltf",
        "external_image.payload",
        "feature_preservation.gltf",
        "unicode_names.gltf",
    }
    require(required.issubset(set(paths)), "corpus is missing one or more mandatory P0/P1 fixtures")


def validate_edge_obligations(root: Path) -> None:
    path = root / "docs/editor-parity/edge-obligations.json"
    data = load_json(path)
    require(data.get("schemaVersion") == 1, "edge obligations schemaVersion must be 1")
    require(data.get("total") == 110, "edge obligations total must be 110")
    specification = data.get("specification")
    require(isinstance(specification, dict), "edge obligations specification must be an object")
    require(specification.get("path") == "docs/gltf-scene-editor-parity-task.md", "edge obligations specification path changed")
    require(specification.get("sha256") == EXPECTED_SPEC_SHA256, "edge obligations specification hash changed")
    counts = data.get("categoryCounts")
    require(counts == EXPECTED_EDGE_COUNTS, "edge obligation category counts differ from section 13.1")
    require(data.get("categoryOrder") == EXPECTED_EDGE_CATEGORY_ORDER, "edge obligation category order differs from section 13.1")
    require(data.get("status") == "assigned", "P0 edge registry status must be assigned without claiming execution")
    semantics = data.get("statusSemantics")
    require(isinstance(semantics, dict), "edge obligation statusSemantics must be an object")
    require(
        set(semantics) == {"assigned", "executed_passed", "failed", "blocked", "unresolved"},
        "edge obligation statusSemantics keys changed",
    )
    require(all(isinstance(value, str) and value for value in semantics.values()), "edge obligation status semantics must be non-empty")
    require(
        data.get("assignmentTally") == {"assigned": 110, "unassigned": 0, "executed": 0, "passed": 0},
        "P0 edge assignment tally must remain 110 assigned and zero executed",
    )
    require(
        data.get("dependencyBoundaryBindings") == EXPECTED_DEPENDENCY_BOUNDARY_BINDINGS,
        "edge dependencyBoundaryBindings must remain assigned to R21/R24 without claiming execution",
    )
    obligations = data.get("obligations")
    require(isinstance(obligations, list), "edge obligations must contain an obligations array")
    ids = [row.get("id") if isinstance(row, dict) else None for row in obligations]
    require(ids == EXPECTED_EDGE_IDS, "edge obligations must contain ordered E001-E110 exactly once")
    behaviors: list[str] = []
    actual_counts = {category: 0 for category in EXPECTED_EDGE_COUNTS}
    actual_requirement_categories: set[tuple[str, str]] = set()
    all_target_evidence: set[str] = set()
    expected_category_sequence = [
        category
        for category, count in EXPECTED_EDGE_COUNTS.items()
        for _ in range(count)
    ]
    actual_category_sequence: list[str] = []
    manifest = load_json(root / "docs/editor-parity/manifest.json")
    feature_requirements = {
        feature["id"]: set(feature["requirements"])
        for feature in manifest["features"]
    }
    for row in obligations:
        category = row.get("category")
        require(category in actual_counts, f"edge {row['id']} has unknown category")
        actual_counts[category] += 1
        actual_category_sequence.append(category)
        behavior = row.get("behavior") or row.get("obligation")
        require(isinstance(behavior, str) and behavior, f"edge {row['id']} requires behavior text")
        behaviors.append(behavior)
        evidence_type = row.get("requiredEvidenceType")
        require(isinstance(evidence_type, str) and evidence_type, f"edge {row['id']} requires requiredEvidenceType")
        require(row.get("status") == "assigned", f"edge {row['id']} must remain assigned until acceptance execution")
        requirements = row.get("requirements")
        features = row.get("features")
        target_evidence = row.get("targetEvidence")
        require(isinstance(requirements, list) and len(requirements) == 1, f"edge {row['id']} must map one section 13.2 requirement/category incidence")
        require(all(isinstance(item, str) and item in {f'R{index:02d}' for index in range(1, 25)} for item in requirements), f"edge {row['id']} has invalid requirement mapping")
        require(len(requirements) == len(set(requirements)), f"edge {row['id']} requirement mappings must be unique")
        actual_requirement_categories.add((requirements[0], category))
        require(isinstance(features, list) and features, f"edge {row['id']} requires feature mappings")
        require(all(item in EXPECTED_FEATURE_IDS for item in features), f"edge {row['id']} has invalid feature mapping")
        require(len(features) == len(set(features)), f"edge {row['id']} feature mappings must be unique")
        require(
            all(requirements[0] in feature_requirements[item] for item in features),
            f"edge {row['id']} feature mapping does not share requirement {requirements[0]}",
        )
        require(isinstance(target_evidence, list) and target_evidence, f"edge {row['id']} requires targetEvidence")
        require(len(target_evidence) == len(set(target_evidence)), f"edge {row['id']} targetEvidence must be unique")
        require(
            all(isinstance(item, str) and item in EXPECTED_EDGE_EVIDENCE_IDS for item in target_evidence),
            f"edge {row['id']} references undefined targetEvidence",
        )
        all_target_evidence.update(target_evidence)
    require(actual_counts == EXPECTED_EDGE_COUNTS, "edge obligation rows do not match categoryCounts")
    require(actual_category_sequence == expected_category_sequence, "edge obligation rows must use contiguous section 13.1 category order")
    require(len(behaviors) == len(set(behaviors)), "edge obligation behavior text must be unique")
    expected_requirement_categories = {
        (requirement, category)
        for requirement, categories in EXPECTED_REQUIREMENT_EDGE_CATEGORIES.items()
        for category in categories
    }
    require(
        actual_requirement_categories == expected_requirement_categories,
        "edge obligations do not cover section 13.2 requirement/category incidences exactly once",
    )
    require(
        set(EXPECTED_ORACLE_IDS).issubset(all_target_evidence),
        "edge obligations must allocate every required O01-O10 reference oracle",
    )


def parse_args() -> argparse.Namespace:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument(
        "--repo-root",
        type=Path,
        default=Path(__file__).resolve().parents[1],
        help="MGIF repository root (defaults to the parent of tools/)",
    )
    parser.add_argument(
        "--mode",
        choices=("scaffold", "configured-dependency-preflight", "configured-dependency", "gate", "release"),
        default="scaffold",
        help="Validate artifact integrity, P1 engineering readiness, or final release licensing",
    )
    parser.add_argument(
        "--resolved-build-identity",
        type=Path,
        help="Build-local Dear ImGui identity generated from the configured imgui_demo target (required in gate/release mode)",
    )
    parser.add_argument(
        "--configured-build-root",
        type=Path,
        help="Generated CMake/Ninja build root (required in configured-dependency mode)",
    )
    parser.add_argument("--config", default="", help="File API configuration name for configured-dependency mode")
    return parser.parse_args()


def main() -> int:
    args = parse_args()
    root = args.repo_root.resolve()
    try:
        if args.mode in {"configured-dependency-preflight", "configured-dependency"}:
            require(
                args.configured_build_root is not None,
                "configured-dependency mode requires --configured-build-root",
            )
            preflight = args.mode == "configured-dependency-preflight"
            validate_configured_editor_build(
                root,
                args.configured_build_root.resolve(),
                args.config,
                require_ninja_dependencies=not preflight,
            )
            if preflight:
                print("Final configured production-editor dependency preflight passed")
            else:
                print("Final configured production-editor dependency gate passed")
            return 0
        validate_manifest(root, mode=args.mode, resolved_build_identity=args.resolved_build_identity)
        validate_schemas(root)
        validate_editor_dependency_boundary(root)
        validate_corpus(root)
        validate_edge_obligations(root)
    except ValidationError as exc:
        print(f"P0 artifact validation failed: {exc}", file=sys.stderr)
        return 1
    if args.mode == "gate":
        print(
            "P1 engineering readiness gate passed: D20 policy, bounded native evidence debts, "
            "per-build Dear ImGui identity, and rights-reserved development policy are explicit; "
            "F42/R24 release licensing remains blocked"
        )
    elif args.mode == "release":
        print("Release-license readiness gate passed: owner-selected root license and F42/R24 evidence are explicit")
    else:
        print(
            "P0 scaffold validation passed: 42 features, 10 traces, 110 edges, schemas, "
            "dependency boundary, and corpus semantics"
        )
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
