cmake_minimum_required(VERSION 3.27)

# The final editor gate consumes replies from this same configure invocation.
# Disk query files created by project code are too late for a first configure.
cmake_file_api(
  QUERY
  API_VERSION 1
  CODEMODEL 2
  CACHE 2
  TOOLCHAINS 1
)

# These hooks execute arbitrary caller-controlled CMake before or around
# project(). A build-time check cannot defend its own ordering edge if such a
# hook cancels deferred finalization, so reject them during configure.
foreach(DEMO_EDITOR_INJECTION_CHANNEL IN ITEMS
    CMAKE_PROJECT_INCLUDE
    CMAKE_PROJECT_INCLUDE_BEFORE
    CMAKE_PROJECT_TOP_LEVEL_INCLUDES)
  if(DEFINED ${DEMO_EDITOR_INJECTION_CHANNEL}
     AND NOT "${${DEMO_EDITOR_INJECTION_CHANNEL}}" STREQUAL "")
    message(FATAL_ERROR
      "The production editor dependency gate forbids ${DEMO_EDITOR_INJECTION_CHANNEL}")
  endif()
endforeach()

project(Demo VERSION 1.0 LANGUAGES C CXX)

# Keep the editor transaction serialization sources explicit for bounded build
# and dependency audits. Debug keeps the project-wide runtime checks enabled.
option(BUILD_TESTING "Build test targets" OFF)
include(CTest)
set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON CACHE BOOL "Required by the editor production dependency gate" FORCE)
set(DEMO_EDITOR_DEPENDENCY_IDENTITY "${CMAKE_BINARY_DIR}/editor-dependency-build-identity.json")

option(DEMO_RHI_CONTRACT_ONLY "Build only dependency-free public/backend RHI contract targets" OFF)
option(DEMO_EDITOR_CORE_ONLY "Build only the dependency-free scene editor core and its CPU tests" OFF)
# The configured acceptance audit requires an external Ninja build tree.
# Normal IDE builds omit it; acceptance presets explicitly enable it.
option(DEMO_SKIP_EDITOR_DEPENDENCY_PREFLIGHT
  "Omit the external-Ninja editor acceptance audit from normal builds" ON)
# FetchContent creates an empty cache entry itself; only a non-empty value
# redirects dependency resolution and therefore constitutes an override.
if((DEFINED FETCHCONTENT_SOURCE_DIR_IMGUI AND NOT "${FETCHCONTENT_SOURCE_DIR_IMGUI}" STREQUAL "")
   OR (DEFINED ENV{FETCHCONTENT_SOURCE_DIR_IMGUI} AND NOT "$ENV{FETCHCONTENT_SOURCE_DIR_IMGUI}" STREQUAL ""))
  message(FATAL_ERROR "FETCHCONTENT_SOURCE_DIR_IMGUI overrides are forbidden by the editor dependency identity contract")
endif()
if(DEMO_RHI_CONTRACT_ONLY AND DEMO_EDITOR_CORE_ONLY)
  message(FATAL_ERROR "DEMO_RHI_CONTRACT_ONLY and DEMO_EDITOR_CORE_ONLY are mutually exclusive")
endif()

if(DEMO_RHI_CONTRACT_ONLY)
  add_executable(rhi_public_contract_tests tests/rhi_public_contract_tests.cpp)
  target_include_directories(rhi_public_contract_tests PRIVATE "${CMAKE_CURRENT_LIST_DIR}")

  add_executable(editor_rhi_contract_tests tests/editor/editor_rhi_contract_tests.cpp)
  target_compile_definitions(editor_rhi_contract_tests PRIVATE
    MGIF_DEPENDENCY_FREE_RHI_CONTRACT=1
    MGIF_TEST_SOURCE_DIR="${CMAKE_CURRENT_LIST_DIR}")
  target_include_directories(editor_rhi_contract_tests PRIVATE
    "${CMAKE_CURRENT_LIST_DIR}/scene/editor/include"
    "${CMAKE_CURRENT_LIST_DIR}"
  )

  if(BUILD_TESTING)
    add_test(NAME rhi_public_contract COMMAND rhi_public_contract_tests)
    add_test(NAME scene_editor.rhi_contract COMMAND editor_rhi_contract_tests)
  endif()

  if(WIN32)
    add_library(rhi_d3d12_contract OBJECT
      rhi/d3d12/D3D12Device.cpp
      rhi/d3d12/D3D12Queue.cpp
      rhi/d3d12/D3D12CommandAllocator.cpp
      rhi/d3d12/D3D12CommandBuffer.cpp
      rhi/d3d12/D3D12PipelineDevice.cpp
      rhi/d3d12/D3D12Swapchain.cpp
    )
    target_include_directories(rhi_d3d12_contract PRIVATE "${CMAKE_CURRENT_LIST_DIR}")
    target_compile_definitions(rhi_d3d12_contract PRIVATE DEMO_HAS_D3D12=1)
  endif()

  if(APPLE)
    enable_language(OBJCXX)
    find_library(DEMO_METAL_FRAMEWORK Metal REQUIRED)
    find_library(DEMO_FOUNDATION_FRAMEWORK Foundation REQUIRED)
    add_executable(metal4_contract_tests
      tests/metal4_contract_tests.mm
      rhi/metal/Metal4Availability.mm
      rhi/metal/Metal4Contract.mm
    )
    target_include_directories(metal4_contract_tests PRIVATE "${CMAKE_CURRENT_LIST_DIR}")
    target_link_libraries(metal4_contract_tests PRIVATE
      "${DEMO_METAL_FRAMEWORK}"
      "${DEMO_FOUNDATION_FRAMEWORK}"
    )
    if(BUILD_TESTING)
      add_test(NAME metal4_contract COMMAND metal4_contract_tests)
    endif()
  endif()
  return()
endif()

include(FetchContent)

FetchContent_Declare(
  tinygltf
  GIT_REPOSITORY https://github.com/syoyo/tinygltf.git
  GIT_TAG v2.9.3
)
set(TINYGLTF_BUILD_LOADER_EXAMPLE OFF CACHE BOOL "" FORCE)
set(TINYGLTF_BUILD_VALIDATION_EXAMPLE OFF CACHE BOOL "" FORCE)
set(TINYGLTF_BUILD_GL_EXAMPLE OFF CACHE BOOL "" FORCE)
set(TINYGLTF_BUILD_VALIDATION_TESTS OFF CACHE BOOL "" FORCE)
set(TINYGLTF_INSTALL OFF CACHE BOOL "" FORCE)
set(TINYGLTF_HEADER_ONLY ON CACHE BOOL "Use explicitly owned TinyGLTF implementation translation units" FORCE)
FetchContent_MakeAvailable(tinygltf)

FetchContent_Declare(
  glm
  GIT_REPOSITORY https://github.com/g-truc/glm.git
  GIT_TAG master
)
FetchContent_MakeAvailable(glm)

add_library(scene_change_contract STATIC
  scene/editor/include/mgif/scene_editor/SceneChangeSet.h
  scene/editor/include/mgif/scene_editor/SceneChangeCanonicalizer.h
  scene/editor/SceneChangeSet.cpp
  scene/editor/SceneChangeCanonicalizer.cpp
)
add_library(MGIF::scene_change_contract ALIAS scene_change_contract)
target_include_directories(scene_change_contract
  PUBLIC "${CMAKE_CURRENT_LIST_DIR}/scene/editor/include"
)
target_compile_features(scene_change_contract PUBLIC cxx_std_20)
target_link_libraries(scene_change_contract PRIVATE "$<$<PLATFORM_ID:Windows>:bcrypt;crypt32>")
add_library(scene_editor_document STATIC
  loader/TinyGltfImplementation.cpp
  scene/editor/include/mgif/scene_editor/EditorLimits.h
  scene/editor/include/mgif/scene_editor/EditorObjectId.h
  scene/editor/include/mgif/scene_editor/EditorHistoryPolicy.h
  scene/editor/include/mgif/scene_editor/GltfDocument.h
  scene/editor/include/mgif/scene_editor/StructuralDelta.h
  scene/editor/include/mgif/scene_editor/SceneCommands.h
  scene/editor/include/mgif/scene_editor/GltfDocumentIO.h
  scene/editor/include/mgif/scene_editor/GltfSemanticCompare.h
  scene/editor/include/mgif/scene_editor/GltfValidator.h
  scene/editor/GltfDocument.cpp
  scene/editor/StructuralDelta.cpp
  scene/editor/SceneCommands.cpp
  scene/editor/GltfDocumentIO.cpp
  scene/editor/GltfSemanticCompare.cpp
  scene/editor/GltfValidator.cpp
)
add_library(MGIF::scene_editor_document ALIAS scene_editor_document)
target_include_directories(scene_editor_document
  PUBLIC "${CMAKE_CURRENT_LIST_DIR}/scene/editor/include"
)
target_compile_features(scene_editor_document PUBLIC cxx_std_20)
target_link_libraries(scene_editor_document PRIVATE tinygltf)
if(BUILD_TESTING)
  target_compile_definitions(scene_editor_document PRIVATE
    MGIF_EDITOR_TESTING=1)
endif()

add_library(scene_editor_core STATIC
  scene/editor/include/mgif/scene_editor/EditorClock.h
  scene/editor/include/mgif/scene_editor/EditorFaultInjection.h
  scene/editor/include/mgif/scene_editor/EditorSelection.h
  scene/editor/include/mgif/scene_editor/testing/EditorStateInspector.h
  scene/editor/include/mgif/scene_editor/commands/EditorCommand.h
  scene/editor/include/mgif/scene_editor/commands/EditorHistory.h
  scene/editor/include/mgif/scene_editor/commands/HistoryLifecycle.h
  scene/editor/include/mgif/scene_editor/commands/TransformCommands.h
  scene/editor/include/mgif/scene_editor/commands/HierarchyCommands.h
  scene/editor/include/mgif/scene_editor/commands/PropertyCommands.h
  scene/editor/include/mgif/scene_editor/commands/ContinuousEdit.h
  scene/editor/include/mgif/scene_editor/EditorSession.h
  scene/editor/include/mgif/scene_editor/SceneEditor.h
  scene/editor/SceneEditorCore.cpp
  scene/editor/EditorSelection.cpp
  scene/editor/commands/EditorCommand.cpp
  scene/editor/commands/EditorHistory.cpp
  scene/editor/commands/HistoryLifecycle.cpp
  scene/editor/commands/TransformCommands.cpp
  scene/editor/commands/HierarchyCommands.cpp
  scene/editor/commands/PropertyCommands.cpp
  scene/editor/commands/ContinuousEdit.cpp
  scene/editor/EditorSession.cpp
  scene/editor/SceneEditor.cpp
)
add_library(MGIF::scene_editor_core ALIAS scene_editor_core)
target_include_directories(scene_editor_core PUBLIC "${CMAKE_CURRENT_LIST_DIR}/scene/editor/include")
target_compile_features(scene_editor_core PUBLIC cxx_std_20)
target_link_libraries(scene_editor_core PRIVATE scene_editor_document)
if(BUILD_TESTING AND NOT ANDROID)
  add_library(scene_editor_core_legacy_test STATIC scene/editor/include/mgif/scene_editor/EditorClock.h scene/editor/include/mgif/scene_editor/EditorFaultInjection.h scene/editor/include/mgif/scene_editor/EditorSelection.h scene/editor/include/mgif/scene_editor/testing/EditorStateInspector.h scene/editor/include/mgif/scene_editor/commands/EditorCommand.h scene/editor/include/mgif/scene_editor/commands/EditorHistory.h scene/editor/include/mgif/scene_editor/commands/HistoryLifecycle.h scene/editor/include/mgif/scene_editor/commands/TransformCommands.h scene/editor/include/mgif/scene_editor/commands/HierarchyCommands.h scene/editor/include/mgif/scene_editor/commands/PropertyCommands.h scene/editor/include/mgif/scene_editor/commands/ContinuousEdit.h scene/editor/include/mgif/scene_editor/EditorSession.h scene/editor/include/mgif/scene_editor/SceneEditor.h scene/editor/SceneEditorCore.cpp scene/editor/EditorSelection.cpp scene/editor/commands/EditorCommand.cpp scene/editor/commands/EditorHistory.cpp scene/editor/commands/HistoryLifecycle.cpp scene/editor/commands/TransformCommands.cpp scene/editor/commands/HierarchyCommands.cpp scene/editor/commands/PropertyCommands.cpp scene/editor/commands/ContinuousEdit.cpp scene/editor/EditorSession.cpp scene/editor/SceneEditor.cpp)
  target_include_directories(scene_editor_core_legacy_test PUBLIC "${CMAKE_CURRENT_LIST_DIR}/scene/editor/include")
  target_compile_features(scene_editor_core_legacy_test PUBLIC cxx_std_20)
  target_compile_definitions(scene_editor_core_legacy_test PUBLIC MGIF_LEGACY_SELECTION_TESTING=1)
  target_link_libraries(scene_editor_core_legacy_test PRIVATE scene_editor_document)
endif()
add_library(scene_change_capture_runtime STATIC
  scene/editor/include/mgif/scene_editor/SceneChangeCapture.h
  scene/editor/SceneChangeCapture.cpp
)
add_library(MGIF::scene_change_capture_runtime ALIAS
  scene_change_capture_runtime)
target_include_directories(scene_change_capture_runtime
  PUBLIC "${CMAKE_CURRENT_LIST_DIR}/scene/editor/include"
)
target_compile_features(scene_change_capture_runtime PUBLIC cxx_std_20)
target_link_libraries(scene_change_capture_runtime PUBLIC scene_change_contract)

add_library(scene_runtime_mapping_runtime STATIC
  scene/editor/include/mgif/scene_editor/SceneRuntimeMapping.h
  scene/editor/SceneRuntimeMapping.cpp
)
add_library(MGIF::scene_runtime_mapping_runtime ALIAS
  scene_runtime_mapping_runtime)
target_include_directories(scene_runtime_mapping_runtime
  PUBLIC "${CMAKE_CURRENT_LIST_DIR}/scene/editor/include"
)
target_compile_features(scene_runtime_mapping_runtime PUBLIC cxx_std_20)
target_link_libraries(scene_runtime_mapping_runtime PUBLIC
  scene_change_contract)

# Android keeps only allocation/reverse lookup; desktop also validates document projection.
if(ANDROID)
  target_compile_definitions(scene_runtime_mapping_runtime PRIVATE
    MGIF_SCENE_MAPPING_FACTORY_ONLY=1)
else()
  target_include_directories(scene_runtime_mapping_runtime PRIVATE
    "${CMAKE_CURRENT_BINARY_DIR}/_deps/glm-src")
  target_link_libraries(scene_runtime_mapping_runtime PUBLIC
    scene_editor_document)
endif()

add_library(scene_editor_interaction_contract STATIC
  scene/editor/include/mgif/scene_editor/EditorPicking.h
  scene/editor/EditorPicking.cpp
)
add_library(MGIF::scene_editor_interaction_contract ALIAS
  scene_editor_interaction_contract)
target_include_directories(scene_editor_interaction_contract PUBLIC "${CMAKE_CURRENT_LIST_DIR}/scene/editor/include" "${CMAKE_CURRENT_LIST_DIR}")
target_compile_features(scene_editor_interaction_contract PUBLIC cxx_std_20)
target_link_libraries(scene_editor_interaction_contract PUBLIC
  scene_editor_core
  scene_runtime_mapping_runtime
)

if(NOT ANDROID)
  add_library(animation_playback_runtime STATIC
    scene/editor/include/mgif/scene_editor/AnimationPlayback.h
    scene/editor/AnimationPlayback.cpp
  )
  add_library(MGIF::animation_playback_runtime ALIAS animation_playback_runtime)
  target_include_directories(animation_playback_runtime
    PUBLIC "${CMAKE_CURRENT_LIST_DIR}/scene/editor/include"
    PRIVATE "${CMAKE_CURRENT_BINARY_DIR}/_deps/tinygltf-src"
  )
  target_compile_features(animation_playback_runtime PUBLIC cxx_std_20)
  target_link_libraries(animation_playback_runtime PRIVATE scene_editor_document)

  add_library(scene_compactor_runtime STATIC
    scene/editor/include/mgif/scene_editor/SceneCompactor.h
    scene/editor/SceneCompactor.cpp
  )
  add_library(MGIF::scene_compactor_runtime ALIAS scene_compactor_runtime)
  target_include_directories(scene_compactor_runtime
    PUBLIC "${CMAKE_CURRENT_LIST_DIR}/scene/editor/include"
    PRIVATE "${CMAKE_CURRENT_BINARY_DIR}/_deps/tinygltf-src"
  )
  target_compile_features(scene_compactor_runtime PUBLIC cxx_std_20)
  target_link_libraries(scene_compactor_runtime PRIVATE scene_editor_document)

  add_library(scene_merger_runtime STATIC
    scene/editor/include/mgif/scene_editor/SceneMerger.h
    scene/editor/SceneMerger.cpp
  )
  add_library(MGIF::scene_merger_runtime ALIAS scene_merger_runtime)
  target_include_directories(scene_merger_runtime
    PUBLIC "${CMAKE_CURRENT_LIST_DIR}/scene/editor/include"
    PRIVATE "${CMAKE_CURRENT_BINARY_DIR}/_deps/tinygltf-src"
  )
  target_compile_features(scene_merger_runtime PUBLIC cxx_std_20)
  target_link_libraries(scene_merger_runtime PRIVATE scene_editor_document)

  add_library(scene_runtime_mapping_contract INTERFACE)
  add_library(MGIF::scene_runtime_mapping_contract ALIAS
    scene_runtime_mapping_contract)
  target_link_libraries(scene_runtime_mapping_contract INTERFACE
    scene_runtime_mapping_runtime)

  add_library(renderer_scene_transaction STATIC
    render/editor/SceneDeltaCounters.h
    render/editor/SceneDeltaCounters.cpp
    render/editor/SceneDeltaResourceJournal.h
    render/editor/SceneDeltaResourceJournal.cpp
    render/editor/SceneRuntimePublisher.h
    render/editor/SceneRuntimePublisher.cpp
  )
  add_library(MGIF::renderer_scene_transaction ALIAS renderer_scene_transaction)
  target_include_directories(renderer_scene_transaction
    PUBLIC
      "${CMAKE_CURRENT_LIST_DIR}"
      "${CMAKE_CURRENT_LIST_DIR}/scene/editor/include"
  )
  target_compile_features(renderer_scene_transaction PUBLIC cxx_std_20)
  target_link_libraries(renderer_scene_transaction PUBLIC
    scene_change_contract
    glm
    tinygltf)

  add_library(editor_overlay_runtime STATIC
    render/editor/include/mgif/editor/EditorGridMath.h
    render/editor/include/mgif/editor/EditorOverlay.h
    render/editor/include/mgif/editor/TransformGizmoMath.h
    render/editor/EditorGridMath.cpp
    render/editor/EditorOverlay.cpp
    render/editor/TransformGizmoMath.cpp
  )
  add_library(MGIF::editor_overlay_runtime ALIAS editor_overlay_runtime)
  target_include_directories(editor_overlay_runtime
    PUBLIC
      "${CMAKE_CURRENT_LIST_DIR}/render/editor/include"
      "${CMAKE_CURRENT_BINARY_DIR}/_deps/glm-src"
  )
  target_compile_features(editor_overlay_runtime PUBLIC cxx_std_20)

endif()

if(NOT ANDROID AND NOT DEMO_EDITOR_CORE_ONLY AND NOT DEMO_SKIP_EDITOR_DEPENDENCY_PREFLIGHT)
  find_package(Python3 REQUIRED COMPONENTS Interpreter)
  add_custom_target(scene_editor_dependency_preflight
    COMMAND "${Python3_EXECUTABLE}"
            "${CMAKE_CURRENT_LIST_DIR}/tools/validate_editor_p0_artifacts.py"
            --repo-root "${CMAKE_CURRENT_LIST_DIR}"
            --mode configured-dependency-preflight
            --configured-build-root "${CMAKE_BINARY_DIR}"
            --config "$<CONFIG>"
    COMMENT "Inspect the final configured production-editor graph before compilation"
    VERBATIM
    USES_TERMINAL
  )
endif()

if(BUILD_TESTING AND NOT ANDROID)
  find_package(Python3 REQUIRED COMPONENTS Interpreter)
  add_executable(portable_bit_representation_tests
    tests/editor/portable_bit_representation_tests.cpp
  )
  target_include_directories(portable_bit_representation_tests PRIVATE
    "${CMAKE_CURRENT_LIST_DIR}/scene/editor/include"
  )
  target_compile_features(portable_bit_representation_tests PRIVATE cxx_std_20)
  add_test(NAME scene_editor.portable_bit_representation
    COMMAND portable_bit_representation_tests)

  add_executable(automation_asset_identity_tests
    tests/editor/automation_asset_identity_tests.cpp
  )
  target_include_directories(automation_asset_identity_tests PRIVATE
    "${CMAKE_CURRENT_LIST_DIR}")
  target_compile_definitions(automation_asset_identity_tests PRIVATE
    MGIF_TEST_SOURCE_DIR="${CMAKE_CURRENT_LIST_DIR}")
  target_compile_features(automation_asset_identity_tests PRIVATE cxx_std_20)
  add_test(NAME scene_editor.automation_asset_identity
    COMMAND automation_asset_identity_tests)

  add_executable(editor_transaction_smoke_tests
    tests/editor/editor_transaction_smoke_tests.cpp
  )
  target_include_directories(editor_transaction_smoke_tests PRIVATE
    "${CMAKE_CURRENT_LIST_DIR}")
  target_compile_features(editor_transaction_smoke_tests PRIVATE cxx_std_20)
  add_test(NAME scene_editor.editor_transaction_smoke
    COMMAND editor_transaction_smoke_tests)
  add_test(NAME scene_editor.editor_transaction_smoke_validator
    COMMAND "${Python3_EXECUTABLE}" -B
            "${CMAKE_CURRENT_LIST_DIR}/tests/editor/editor_transaction_smoke_validator_tests.py"
            -v)
  set_tests_properties(scene_editor.editor_transaction_smoke PROPERTIES
    LABELS "scene-editor-p5-automation")
  set_tests_properties(scene_editor.editor_transaction_smoke_validator PROPERTIES
    LABELS "scene-editor-p5-automation")

  add_executable(editor_transaction_path_tests
    tests/editor/editor_transaction_path_tests.cpp
    app/editor/EditorTransactionPath.cpp
  )
  target_include_directories(editor_transaction_path_tests PRIVATE
    "${CMAKE_CURRENT_LIST_DIR}")
  target_compile_features(editor_transaction_path_tests PRIVATE cxx_std_20)
  add_test(NAME scene_editor.editor_transaction_path
    COMMAND editor_transaction_path_tests)
  set_tests_properties(scene_editor.editor_transaction_path PROPERTIES
    LABELS "scene-editor-p5-automation")

  add_executable(scene_change_set_tests
    tests/editor/scene_change_set_tests.cpp
  )
  target_link_libraries(scene_change_set_tests PRIVATE scene_change_contract)
  add_test(NAME scene_editor.scene_change_set COMMAND scene_change_set_tests)

  add_executable(scene_change_capture_tests
    tests/editor/scene_change_capture_tests.cpp
  )
  target_link_libraries(scene_change_capture_tests PRIVATE scene_editor_core_legacy_test)
  target_link_libraries(scene_change_capture_tests PRIVATE
    scene_editor_document scene_change_capture_runtime)
  add_test(NAME scene_editor.scene_change_capture COMMAND scene_change_capture_tests)

  add_executable(scene_runtime_mapping_tests
    tests/editor/scene_runtime_mapping_tests.cpp
  )
  target_compile_definitions(scene_runtime_mapping_tests PRIVATE
    MGIF_SCENE_MAPPING_FACTORY_ONLY=1)
  target_link_libraries(scene_runtime_mapping_tests PRIVATE scene_editor_document)
  target_link_libraries(scene_runtime_mapping_tests PRIVATE
    scene_runtime_mapping_contract)
  add_test(NAME scene_editor.scene_runtime_mapping COMMAND scene_runtime_mapping_tests)

  add_executable(renderer_scene_publication_tests
    tests/editor/renderer_scene_publication_tests.cpp
  )
  target_link_libraries(renderer_scene_publication_tests PRIVATE
    renderer_scene_transaction)
  add_test(NAME scene_editor.renderer_scene_publication
    COMMAND renderer_scene_publication_tests)

  add_executable(scene_delta_fake_rhi_tests
    tests/editor/scene_delta_fake_rhi_tests.cpp
  )
  target_link_libraries(scene_delta_fake_rhi_tests PRIVATE
    renderer_scene_transaction)
  add_test(NAME scene_editor.scene_delta_fake_rhi
    COMMAND scene_delta_fake_rhi_tests)

  add_executable(scene_editor_p0_support_tests
    tests/editor/editor_p0_support_tests.cpp
    tests/editor/support/EditorTestSupport.h
  )
  target_include_directories(scene_editor_p0_support_tests PRIVATE "${CMAKE_CURRENT_LIST_DIR}")
  target_link_libraries(scene_editor_p0_support_tests PRIVATE scene_editor_core)
  add_test(NAME scene_editor.p0_support COMMAND scene_editor_p0_support_tests)

  add_executable(scene_editor_identity_tests
    tests/editor/editor_identity_tests.cpp
  )
  target_include_directories(scene_editor_identity_tests PRIVATE "${CMAKE_CURRENT_LIST_DIR}")
  target_link_libraries(scene_editor_identity_tests PRIVATE scene_editor_core)
  add_test(NAME scene_editor.identity COMMAND scene_editor_identity_tests)

  add_executable(gltf_document_load_tests
    tests/editor/gltf_document_load_tests.cpp
  )
  target_include_directories(gltf_document_load_tests PRIVATE "${CMAKE_CURRENT_LIST_DIR}")
  target_compile_definitions(gltf_document_load_tests PRIVATE
    MGIF_TEST_SOURCE_DIR="${CMAKE_CURRENT_LIST_DIR}"
  )
  target_link_libraries(gltf_document_load_tests PRIVATE scene_editor_core)
  add_test(NAME scene_editor.gltf_document_load COMMAND gltf_document_load_tests)

  add_executable(gltf_document_roundtrip_tests
    tests/editor/gltf_document_roundtrip_tests.cpp
  )
  target_include_directories(gltf_document_roundtrip_tests PRIVATE "${CMAKE_CURRENT_LIST_DIR}")
  target_compile_definitions(gltf_document_roundtrip_tests PRIVATE
    MGIF_TEST_SOURCE_DIR="${CMAKE_CURRENT_LIST_DIR}"
  )
  target_link_libraries(gltf_document_roundtrip_tests PRIVATE scene_editor_core)
  add_test(NAME scene_editor.gltf_document_roundtrip COMMAND gltf_document_roundtrip_tests)

  add_executable(gltf_document_io_tests
    tests/editor/gltf_document_io_tests.cpp
  )
  target_include_directories(gltf_document_io_tests PRIVATE "${CMAKE_CURRENT_LIST_DIR}")
  target_compile_definitions(gltf_document_io_tests PRIVATE
    MGIF_TEST_SOURCE_DIR="${CMAKE_CURRENT_LIST_DIR}"
  )
  target_link_libraries(gltf_document_io_tests PRIVATE scene_editor_core)
  add_test(NAME scene_editor.gltf_document_io COMMAND gltf_document_io_tests)

  add_executable(gltf_validator_tests
    tests/editor/gltf_validator_tests.cpp
  )
  target_include_directories(gltf_validator_tests PRIVATE "${CMAKE_CURRENT_LIST_DIR}")
  target_compile_definitions(gltf_validator_tests PRIVATE
    MGIF_TEST_SOURCE_DIR="${CMAKE_CURRENT_LIST_DIR}"
  )
  target_link_libraries(gltf_validator_tests PRIVATE scene_editor_core tinygltf)
  add_test(NAME scene_editor.gltf_validator COMMAND gltf_validator_tests)

  add_executable(gltf_accessor_payload_tests
    tests/editor/gltf_accessor_payload_tests.cpp
  )
  target_include_directories(gltf_accessor_payload_tests PRIVATE "${CMAKE_CURRENT_LIST_DIR}")
  target_compile_definitions(gltf_accessor_payload_tests PRIVATE
    MGIF_TEST_SOURCE_DIR="${CMAKE_CURRENT_LIST_DIR}"
  )
  target_link_libraries(gltf_accessor_payload_tests PRIVATE scene_editor_core tinygltf)
  add_test(NAME scene_editor.gltf_accessor_payload COMMAND gltf_accessor_payload_tests)

  add_executable(animation_playback_tests
    tests/editor/animation_playback_tests.cpp
  )
  target_compile_features(animation_playback_tests PRIVATE cxx_std_20)
  target_compile_definitions(animation_playback_tests PRIVATE
    MGIF_TEST_SOURCE_DIR="${CMAKE_CURRENT_LIST_DIR}"
  )
  target_link_libraries(animation_playback_tests PRIVATE animation_playback_runtime)
  add_test(NAME scene_editor.animation_playback COMMAND animation_playback_tests)

  add_executable(p2_projection_cpu_tests
    tests/editor/scene_projection_tests.cpp
    scene/editor/SceneProjection.cpp
    scene/editor/SceneRuntimeMapping.cpp
    loader/GltfLoader.cpp
    loader/SceneCacheSerializer.cpp
    scene/SceneAssetBuilder.cpp
    scene/SceneAssetSerializer.cpp
  )
  target_include_directories(p2_projection_cpu_tests PRIVATE
    "${CMAKE_CURRENT_LIST_DIR}"
    "${CMAKE_CURRENT_LIST_DIR}/scene/editor/include"
  )
  target_compile_definitions(p2_projection_cpu_tests PRIVATE
    MGIF_TEST_SOURCE_DIR="${CMAKE_CURRENT_LIST_DIR}"
  )
  target_link_libraries(p2_projection_cpu_tests PRIVATE scene_editor_document glm tinygltf)
  add_test(NAME scene_editor.scene_projection COMMAND p2_projection_cpu_tests)

  add_executable(scene_editor_scene_commands_tests
    tests/editor/scene_commands_tests.cpp
    scene/editor/SceneProjection.cpp
    scene/editor/SceneRuntimeMapping.cpp
    loader/GltfLoader.cpp
    loader/SceneCacheSerializer.cpp
    scene/SceneAssetBuilder.cpp
    scene/SceneAssetSerializer.cpp
  )
  target_include_directories(scene_editor_scene_commands_tests PRIVATE
    "${CMAKE_CURRENT_LIST_DIR}"
    "${CMAKE_CURRENT_LIST_DIR}/scene/editor/include"
  )
  target_compile_definitions(scene_editor_scene_commands_tests PRIVATE
    MGIF_TEST_SOURCE_DIR="${CMAKE_CURRENT_LIST_DIR}"
  )
  target_link_libraries(scene_editor_scene_commands_tests PRIVATE
    scene_editor_core_legacy_test glm tinygltf
  )
  add_test(NAME scene_editor.scene_commands COMMAND scene_editor_scene_commands_tests)

  add_executable(scene_editor_mutation_tests
    tests/editor/scene_editor_mutation_tests.cpp
  )
  target_include_directories(scene_editor_mutation_tests PRIVATE "${CMAKE_CURRENT_LIST_DIR}")
  target_compile_definitions(scene_editor_mutation_tests PRIVATE
    MGIF_TEST_SOURCE_DIR="${CMAKE_CURRENT_LIST_DIR}"
  )
  target_link_libraries(scene_editor_mutation_tests PRIVATE scene_editor_core_legacy_test)
  add_test(NAME scene_editor.mutation COMMAND scene_editor_mutation_tests)

  add_executable(scene_editor_p3_regression_tests
    tests/editor/scene_editor_p3_regression_tests.cpp
    scene/editor/SceneProjection.cpp
    scene/editor/SceneRuntimeMapping.cpp
    loader/GltfLoader.cpp
    loader/SceneCacheSerializer.cpp
    scene/SceneAssetBuilder.cpp
    scene/SceneAssetSerializer.cpp
  )
  target_include_directories(scene_editor_p3_regression_tests PRIVATE
    "${CMAKE_CURRENT_LIST_DIR}"
    "${CMAKE_CURRENT_LIST_DIR}/scene/editor/include"
  )
  target_compile_definitions(scene_editor_p3_regression_tests PRIVATE
    MGIF_TEST_SOURCE_DIR="${CMAKE_CURRENT_LIST_DIR}"
  )
  target_link_libraries(scene_editor_p3_regression_tests PRIVATE
    scene_editor_core glm tinygltf
  )
  add_test(NAME scene_editor.p3_regression COMMAND scene_editor_p3_regression_tests)

  set(DEMO_EDITOR_P4_PROJECTION_TEST_SOURCES
    scene/editor/SceneProjection.cpp
    scene/editor/SceneRuntimeMapping.cpp
    loader/GltfLoader.cpp
    loader/SceneCacheSerializer.cpp
    scene/SceneAssetBuilder.cpp
    scene/SceneAssetSerializer.cpp
  )

  set(DEMO_EDITOR_P4_HEADER_SELF_CONTAINMENT_SOURCES
    tests/editor/header_self_containment/editor_history_policy_header.cpp
    tests/editor/header_self_containment/structural_delta_header.cpp
    tests/editor/header_self_containment/editor_selection_header.cpp
    tests/editor/header_self_containment/editor_state_inspector_header.cpp
    tests/editor/header_self_containment/editor_command_header.cpp
    tests/editor/header_self_containment/editor_history_header.cpp
    tests/editor/header_self_containment/history_lifecycle_header.cpp
    tests/editor/header_self_containment/editor_session_header.cpp
    tests/editor/header_self_containment/transform_commands_header.cpp
    tests/editor/header_self_containment/hierarchy_commands_header.cpp
    tests/editor/header_self_containment/property_commands_header.cpp
    tests/editor/header_self_containment/continuous_edit_header.cpp
  )

  add_executable(editor_history_core_tests
    tests/editor/editor_history_core_tests.cpp
    ${DEMO_EDITOR_P4_HEADER_SELF_CONTAINMENT_SOURCES}
  )
  add_executable(editor_history_structural_tests
    tests/editor/editor_history_structural_tests.cpp
    ${DEMO_EDITOR_P4_PROJECTION_TEST_SOURCES}
  )
  add_executable(editor_history_property_tests
    tests/editor/editor_history_property_tests.cpp
    ${DEMO_EDITOR_P4_PROJECTION_TEST_SOURCES}
  )
  add_executable(editor_continuous_edit_tests
    tests/editor/editor_continuous_edit_tests.cpp
    ${DEMO_EDITOR_P4_PROJECTION_TEST_SOURCES}
  )

  foreach(DEMO_EDITOR_P4_TEST_TARGET IN ITEMS
      editor_history_core_tests
      editor_history_structural_tests
      editor_history_property_tests
      editor_continuous_edit_tests)
    target_include_directories(${DEMO_EDITOR_P4_TEST_TARGET} PRIVATE
      "${CMAKE_CURRENT_LIST_DIR}"
      "${CMAKE_CURRENT_LIST_DIR}/scene/editor/include"
    )
    target_compile_definitions(${DEMO_EDITOR_P4_TEST_TARGET} PRIVATE
      MGIF_EDITOR_TESTING=1
      MGIF_TEST_SOURCE_DIR="${CMAKE_CURRENT_LIST_DIR}"
    )
    target_link_libraries(${DEMO_EDITOR_P4_TEST_TARGET} PRIVATE scene_editor_core_legacy_test)
  endforeach()

  foreach(DEMO_EDITOR_P4_PROJECTION_TEST_TARGET IN ITEMS
      editor_history_structural_tests
      editor_history_property_tests
      editor_continuous_edit_tests)
    target_link_libraries(${DEMO_EDITOR_P4_PROJECTION_TEST_TARGET} PRIVATE glm tinygltf)
  endforeach()

  add_test(NAME scene_editor.p4.core_execute
    COMMAND editor_history_core_tests core)
  add_test(NAME scene_editor.p4.capacity_merge
    COMMAND editor_history_core_tests capacity-merge)
  add_test(NAME scene_editor.p4.branch_clear
    COMMAND editor_history_core_tests branch-clear)
  add_test(NAME scene_editor.p4.structural
    COMMAND editor_history_structural_tests)
  add_test(NAME scene_editor.p4.properties
    COMMAND editor_history_property_tests)
  add_test(NAME scene_editor.p4.continuous_lifecycle
    COMMAND editor_continuous_edit_tests)

  set_tests_properties(
    scene_editor.p4.core_execute
    scene_editor.p4.capacity_merge
    scene_editor.p4.branch_clear
    scene_editor.p4.structural
    scene_editor.p4.properties
    scene_editor.p4.continuous_lifecycle
    PROPERTIES LABELS "scene-editor-p4-history"
  )

  set(DEMO_SCENE_EDITOR_TEST_TARGETS
    scene_change_set_tests
    scene_change_capture_tests
    scene_runtime_mapping_tests
    renderer_scene_publication_tests
    scene_delta_fake_rhi_tests
    scene_editor_p0_support_tests
    scene_editor_identity_tests
    gltf_document_load_tests
    gltf_document_roundtrip_tests
    gltf_document_io_tests
    gltf_validator_tests
    gltf_accessor_payload_tests
    animation_playback_tests
    p2_projection_cpu_tests
    scene_editor_scene_commands_tests
    scene_editor_mutation_tests
    scene_editor_p3_regression_tests
  )
  list(APPEND DEMO_SCENE_EDITOR_TEST_TARGETS
    editor_history_core_tests
    editor_history_structural_tests
    editor_history_property_tests
    editor_continuous_edit_tests
    editor_transaction_smoke_tests
  )
  foreach(DEMO_SCENE_EDITOR_P5_CAPTURE_CONSUMER IN LISTS
      DEMO_SCENE_EDITOR_TEST_TARGETS)
    target_link_libraries(${DEMO_SCENE_EDITOR_P5_CAPTURE_CONSUMER} PRIVATE
      scene_change_capture_runtime)
  endforeach()

  add_executable(editor_gizmo_math_tests
    tests/editor/editor_gizmo_math_tests.cpp
    render/editor/TransformGizmoMath.cpp
  )
  target_include_directories(editor_gizmo_math_tests PRIVATE
    "${CMAKE_CURRENT_LIST_DIR}/render/editor/include"
  )
  target_link_libraries(editor_gizmo_math_tests PRIVATE glm)
  add_test(NAME scene_editor.gizmo_math COMMAND editor_gizmo_math_tests)

  add_executable(editor_grid_math_tests
    tests/editor/editor_grid_math_tests.cpp
    render/editor/EditorGridMath.cpp
  )
  target_include_directories(editor_grid_math_tests PRIVATE
    "${CMAKE_CURRENT_LIST_DIR}/render/editor/include"
  )
  target_link_libraries(editor_grid_math_tests PRIVATE glm)
  add_test(NAME scene_editor.grid_math COMMAND editor_grid_math_tests)

  list(APPEND DEMO_SCENE_EDITOR_TEST_TARGETS
    editor_gizmo_math_tests
    editor_grid_math_tests
  )
  set_tests_properties(
    scene_editor.gizmo_math
    scene_editor.grid_math
    PROPERTIES LABELS "scene-editor-p7-math"
  )

  add_executable(scene_compactor_tests
    tests/editor/scene_compactor_tests.cpp
  )
  target_compile_definitions(scene_compactor_tests PRIVATE
    MGIF_TEST_SOURCE_DIR="${CMAKE_CURRENT_LIST_DIR}"
  )
  target_link_libraries(scene_compactor_tests PRIVATE
    scene_compactor_runtime
    scene_change_contract
    tinygltf
  )
  add_test(NAME scene_editor.compactor COMMAND scene_compactor_tests)

  add_executable(editor_overlay_tests
    tests/editor/editor_overlay_tests.cpp
  )
  target_link_libraries(editor_overlay_tests PRIVATE editor_overlay_runtime)
  add_test(NAME scene_editor.overlay COMMAND editor_overlay_tests)

  list(APPEND DEMO_SCENE_EDITOR_TEST_TARGETS
    scene_compactor_tests
    editor_overlay_tests
  )
  set_tests_properties(scene_editor.compactor PROPERTIES
    LABELS "scene-editor-p9-compaction")

	add_executable(scene_merger_tests
		tests/editor/scene_merger_tests.cpp
	)
  target_compile_definitions(scene_merger_tests PRIVATE
    MGIF_TEST_SOURCE_DIR="${CMAKE_CURRENT_LIST_DIR}"
  )
  target_link_libraries(scene_merger_tests PRIVATE
    scene_merger_runtime
    scene_change_contract
    tinygltf
  )
  add_test(NAME scene_editor.merger COMMAND scene_merger_tests)
  list(APPEND DEMO_SCENE_EDITOR_TEST_TARGETS scene_merger_tests)
	set_tests_properties(scene_editor.merger PROPERTIES
		LABELS "scene-editor-p9-merge")

	add_executable(gltf_new_scene_tests
		tests/editor/gltf_new_scene_tests.cpp
	)
	target_link_libraries(gltf_new_scene_tests PRIVATE
		scene_merger_runtime
		scene_change_contract
		tinygltf
	)
	add_test(NAME scene_editor.new_scene COMMAND gltf_new_scene_tests)
	list(APPEND DEMO_SCENE_EDITOR_TEST_TARGETS gltf_new_scene_tests)
	set_tests_properties(scene_editor.new_scene PROPERTIES
		LABELS "scene-editor-p9-lifecycle")

  add_executable(editor_selection_contract_tests
    tests/editor/editor_selection_contract_tests.cpp
  )
  target_compile_definitions(editor_selection_contract_tests PRIVATE
    MGIF_TEST_SOURCE_DIR="${CMAKE_CURRENT_LIST_DIR}"
  )
  target_link_libraries(editor_selection_contract_tests PRIVATE
    scene_editor_core_legacy_test
    scene_change_contract
  )
  add_test(NAME scene_editor.selection_contract
    COMMAND editor_selection_contract_tests)

  add_executable(editor_picking_contract_tests
    tests/editor/editor_picking_contract_tests.cpp
  )
  target_compile_definitions(editor_picking_contract_tests PRIVATE
    MGIF_TEST_SOURCE_DIR="${CMAKE_CURRENT_LIST_DIR}"
  )
  target_link_libraries(editor_picking_contract_tests PRIVATE
    scene_editor_interaction_contract
  )
  add_test(NAME scene_editor.picking_contract
    COMMAND editor_picking_contract_tests)

  list(APPEND DEMO_SCENE_EDITOR_TEST_TARGETS
    editor_selection_contract_tests
    editor_picking_contract_tests
  )
  set_tests_properties(
    scene_editor.selection_contract
    scene_editor.picking_contract
    PROPERTIES LABELS "scene-editor-interaction-contract"
  )

  if(NOT DEMO_EDITOR_CORE_ONLY)
    add_executable(editor_rhi_contract_tests tests/editor/editor_rhi_contract_tests.cpp)
    target_compile_definitions(editor_rhi_contract_tests PRIVATE
      MGIF_ENABLE_SCENE_TRANSACTIONS=1
      MGIF_TEST_SOURCE_DIR="${CMAKE_CURRENT_LIST_DIR}")
    target_include_directories(editor_rhi_contract_tests PRIVATE
      "${CMAKE_CURRENT_LIST_DIR}/scene/editor/include"
      "${CMAKE_CURRENT_LIST_DIR}"
    )
    target_link_libraries(editor_rhi_contract_tests PRIVATE
      renderer_scene_transaction
      editor_overlay_rhi_runtime
      imgui_demo
      glm)
    add_test(NAME scene_editor.rhi_contract COMMAND editor_rhi_contract_tests)
    list(APPEND DEMO_SCENE_EDITOR_TEST_TARGETS editor_rhi_contract_tests)
  endif()

  add_test(NAME scene_editor.p0_artifacts
    COMMAND "${Python3_EXECUTABLE}"
            "${CMAKE_CURRENT_LIST_DIR}/tools/validate_editor_p0_artifacts.py"
            --repo-root "${CMAKE_CURRENT_LIST_DIR}"
  )
  add_test(NAME scene_editor.acceptance_report_validator
    COMMAND "${Python3_EXECUTABLE}"
            "${CMAKE_CURRENT_LIST_DIR}/tests/editor/acceptance_report_validator_tests.py"
            -v
  )
  add_test(NAME scene_editor.p0_artifact_validator
    COMMAND "${Python3_EXECUTABLE}"
            "${CMAKE_CURRENT_LIST_DIR}/tests/editor/p0_artifact_validator_tests.py"
            -v
  )
  add_test(NAME scene_editor.dependency_gate_integration
    COMMAND "${Python3_EXECUTABLE}"
            "${CMAKE_CURRENT_LIST_DIR}/tests/editor/editor_dependency_gate_integration_tests.py"
            -v
  )
  add_test(NAME scene_editor.android_viewer_contract
    COMMAND "${Python3_EXECUTABLE}"
            "${CMAKE_CURRENT_LIST_DIR}/tests/editor/android_viewer_contract_tests.py"
            -v
  )
  add_test(NAME scene_editor.progress_ledger_protocol
    COMMAND "${Python3_EXECUTABLE}" -B
            "${CMAKE_CURRENT_LIST_DIR}/tests/editor/progress_ledger_validator_tests.py"
            -v
  )
  add_test(NAME scene_editor.progress_ledger_validator
    COMMAND "${Python3_EXECUTABLE}" -B
            "${CMAKE_CURRENT_LIST_DIR}/tools/validate_editor_progress_ledger.py"
            --repo-root "${CMAKE_CURRENT_LIST_DIR}"
            --check
  )
  add_test(NAME scene_editor.progress_ledger_updater_check
    COMMAND "${Python3_EXECUTABLE}" -B
            "${CMAKE_CURRENT_LIST_DIR}/tools/update_editor_progress_ledger.py"
            --repo-root "${CMAKE_CURRENT_LIST_DIR}"
            --check
  )
  add_test(NAME scene_editor.local_progress_ledger_protocol
    COMMAND "${Python3_EXECUTABLE}" -B
            "${CMAKE_CURRENT_LIST_DIR}/tests/editor/local_progress_ledger_tests.py"
            -v
  )
  add_test(NAME scene_editor.local_progress_ledger_validator
    COMMAND "${Python3_EXECUTABLE}" -B
            "${CMAKE_CURRENT_LIST_DIR}/tools/validate_editor_local_progress.py"
            --repo-root "${CMAKE_CURRENT_LIST_DIR}"
            --check
  )
  add_test(NAME scene_editor.local_progress_ledger_publisher_check
    COMMAND "${Python3_EXECUTABLE}" -B
            "${CMAKE_CURRENT_LIST_DIR}/tools/local_editor_progress_ledger.py"
            --repo-root "${CMAKE_CURRENT_LIST_DIR}"
            --check
  )

  add_custom_target(scene_editor_progress_ledger_check
    COMMAND "${Python3_EXECUTABLE}" -B
            "${CMAKE_CURRENT_LIST_DIR}/tests/editor/progress_ledger_validator_tests.py"
            -v
    COMMAND "${Python3_EXECUTABLE}" -B
            "${CMAKE_CURRENT_LIST_DIR}/tools/validate_editor_progress_ledger.py"
            --repo-root "${CMAKE_CURRENT_LIST_DIR}"
            --check
    COMMAND "${Python3_EXECUTABLE}" -B
            "${CMAKE_CURRENT_LIST_DIR}/tools/update_editor_progress_ledger.py"
            --repo-root "${CMAKE_CURRENT_LIST_DIR}"
            --check
    COMMAND "${Python3_EXECUTABLE}" -B
            "${CMAKE_CURRENT_LIST_DIR}/tests/editor/local_progress_ledger_tests.py"
            -v
    COMMAND "${Python3_EXECUTABLE}" -B
            "${CMAKE_CURRENT_LIST_DIR}/tools/validate_editor_local_progress.py"
            --repo-root "${CMAKE_CURRENT_LIST_DIR}"
            --check
    COMMAND "${Python3_EXECUTABLE}" -B
            "${CMAKE_CURRENT_LIST_DIR}/tools/local_editor_progress_ledger.py"
            --repo-root "${CMAKE_CURRENT_LIST_DIR}"
            --check
    COMMENT "Validate frozen legacy and active local editor progress protocols"
    VERBATIM
    USES_TERMINAL
  )

  set_tests_properties(
    scene_editor.portable_bit_representation
    scene_editor.p0_support
    scene_editor.p0_artifacts
    scene_editor.p0_artifact_validator
    scene_editor.dependency_gate_integration
    scene_editor.android_viewer_contract
    scene_editor.acceptance_report_validator
    scene_editor.progress_ledger_protocol
    scene_editor.progress_ledger_validator
    scene_editor.progress_ledger_updater_check
    scene_editor.local_progress_ledger_protocol
    scene_editor.local_progress_ledger_validator
    scene_editor.local_progress_ledger_publisher_check
    PROPERTIES LABELS "scene-editor-p0-scaffold"
  )
  set_tests_properties(
    scene_editor.identity
    scene_editor.gltf_document_load
    scene_editor.gltf_document_roundtrip
    scene_editor.gltf_document_io
    scene_editor.gltf_validator
    scene_editor.gltf_accessor_payload
    scene_editor.scene_projection
    PROPERTIES LABELS "scene-editor-p1-document"
  )
  set_tests_properties(
    scene_editor.scene_commands
    scene_editor.mutation
    scene_editor.p3_regression
    scene_editor.animation_playback
    PROPERTIES LABELS "scene-editor-p3-mutation"
  )
  set_tests_properties(scene_editor.scene_projection PROPERTIES
    LABELS "scene-editor-p1-document;scene-editor-p3-mutation")
  if(TARGET editor_rhi_contract_tests)
    set_tests_properties(scene_editor.rhi_contract PROPERTIES LABELS "scene-editor-p0-scaffold")
  endif()

  add_custom_target(scene_editor_tests DEPENDS
    ${DEMO_SCENE_EDITOR_TEST_TARGETS}
    portable_bit_representation_tests
    automation_asset_identity_tests
    editor_transaction_path_tests
    scene_editor_progress_ledger_check)
  add_custom_target(scene_editor_p0_scaffold
    COMMAND "${CMAKE_CTEST_COMMAND}"
            --test-dir "${CMAKE_BINARY_DIR}"
            -C "$<CONFIG>"
            --output-on-failure
    DEPENDS scene_editor_tests
    USES_TERMINAL
  )
  add_custom_target(scene_editor_p0
    COMMAND "${Python3_EXECUTABLE}"
            "${CMAKE_CURRENT_LIST_DIR}/tools/generate_editor_dependency_identity.py"
            --repo-root "${CMAKE_CURRENT_LIST_DIR}"
            --build-root "${CMAKE_BINARY_DIR}"
            --source-dir "${CMAKE_BINARY_DIR}/_deps/imgui-src"
            --expected-source-dir "${CMAKE_BINARY_DIR}/_deps/imgui-src"
            --config "$<CONFIG>"
            --output "${DEMO_EDITOR_DEPENDENCY_IDENTITY}"
    COMMAND "${Python3_EXECUTABLE}"
            "${CMAKE_CURRENT_LIST_DIR}/tools/validate_editor_p0_artifacts.py"
            --repo-root "${CMAKE_CURRENT_LIST_DIR}"
            --mode gate
            --resolved-build-identity "${DEMO_EDITOR_DEPENDENCY_IDENTITY}"
    DEPENDS scene_editor_p0_scaffold
    COMMENT "Validate P0 closure and P1 engineering readiness with the resolved build identity"
    USES_TERMINAL
  )
  add_custom_target(scene_editor_release_gate
    COMMAND "${Python3_EXECUTABLE}"
            "${CMAKE_CURRENT_LIST_DIR}/tools/generate_editor_dependency_identity.py"
            --repo-root "${CMAKE_CURRENT_LIST_DIR}"
            --build-root "${CMAKE_BINARY_DIR}"
            --source-dir "${CMAKE_BINARY_DIR}/_deps/imgui-src"
            --expected-source-dir "${CMAKE_BINARY_DIR}/_deps/imgui-src"
            --config "$<CONFIG>"
            --output "${DEMO_EDITOR_DEPENDENCY_IDENTITY}"
    COMMAND "${Python3_EXECUTABLE}"
            "${CMAKE_CURRENT_LIST_DIR}/tools/validate_editor_p0_artifacts.py"
            --repo-root "${CMAKE_CURRENT_LIST_DIR}"
            --mode release
            --resolved-build-identity "${DEMO_EDITOR_DEPENDENCY_IDENTITY}"
    DEPENDS scene_editor_p0_scaffold
    COMMENT "Validate final release licensing; blocked until the owner selects a root license"
    USES_TERMINAL
  )
endif()

# CMAKE_PROJECT_TOP_LEVEL_INCLUDES and toolchain files can schedule mutations
# before repository targets exist. Its deferred calls run first; this invariant
# then seals the final core state in core-only and full configurations.
function(mgif_finalize_editor_dependency_targets)
  set(DEMO_EXPECTED_SCENE_EDITOR_SOURCES
    "${CMAKE_SOURCE_DIR}/scene/editor/include/mgif/scene_editor/EditorClock.h"
    "${CMAKE_SOURCE_DIR}/scene/editor/include/mgif/scene_editor/EditorFaultInjection.h"
    "${CMAKE_SOURCE_DIR}/scene/editor/include/mgif/scene_editor/EditorSelection.h"
    "${CMAKE_SOURCE_DIR}/scene/editor/include/mgif/scene_editor/testing/EditorStateInspector.h"
    "${CMAKE_SOURCE_DIR}/scene/editor/include/mgif/scene_editor/commands/EditorCommand.h"
    "${CMAKE_SOURCE_DIR}/scene/editor/include/mgif/scene_editor/commands/EditorHistory.h"
    "${CMAKE_SOURCE_DIR}/scene/editor/include/mgif/scene_editor/commands/HistoryLifecycle.h"
    "${CMAKE_SOURCE_DIR}/scene/editor/include/mgif/scene_editor/commands/TransformCommands.h"
    "${CMAKE_SOURCE_DIR}/scene/editor/include/mgif/scene_editor/commands/HierarchyCommands.h"
    "${CMAKE_SOURCE_DIR}/scene/editor/include/mgif/scene_editor/commands/PropertyCommands.h"
    "${CMAKE_SOURCE_DIR}/scene/editor/include/mgif/scene_editor/commands/ContinuousEdit.h"
    "${CMAKE_SOURCE_DIR}/scene/editor/include/mgif/scene_editor/EditorSession.h"
    "${CMAKE_SOURCE_DIR}/scene/editor/include/mgif/scene_editor/SceneEditor.h"
    "${CMAKE_SOURCE_DIR}/scene/editor/SceneEditorCore.cpp"
    "${CMAKE_SOURCE_DIR}/scene/editor/EditorSelection.cpp"
    "${CMAKE_SOURCE_DIR}/scene/editor/commands/EditorCommand.cpp"
    "${CMAKE_SOURCE_DIR}/scene/editor/commands/EditorHistory.cpp"
    "${CMAKE_SOURCE_DIR}/scene/editor/commands/HistoryLifecycle.cpp"
    "${CMAKE_SOURCE_DIR}/scene/editor/commands/TransformCommands.cpp"
    "${CMAKE_SOURCE_DIR}/scene/editor/commands/HierarchyCommands.cpp"
    "${CMAKE_SOURCE_DIR}/scene/editor/commands/PropertyCommands.cpp"
    "${CMAKE_SOURCE_DIR}/scene/editor/commands/ContinuousEdit.cpp"
    "${CMAKE_SOURCE_DIR}/scene/editor/EditorSession.cpp"
    "${CMAKE_SOURCE_DIR}/scene/editor/SceneEditor.cpp"
  )
  get_target_property(DEMO_FINAL_SCENE_EDITOR_SOURCES scene_editor_core SOURCES)
  set(DEMO_FINAL_SCENE_EDITOR_ABSOLUTE_SOURCES)
  foreach(DEMO_FINAL_SCENE_EDITOR_SOURCE IN LISTS DEMO_FINAL_SCENE_EDITOR_SOURCES)
    if(IS_ABSOLUTE "${DEMO_FINAL_SCENE_EDITOR_SOURCE}")
      list(APPEND DEMO_FINAL_SCENE_EDITOR_ABSOLUTE_SOURCES "${DEMO_FINAL_SCENE_EDITOR_SOURCE}")
    else()
      list(APPEND DEMO_FINAL_SCENE_EDITOR_ABSOLUTE_SOURCES
        "${CMAKE_SOURCE_DIR}/${DEMO_FINAL_SCENE_EDITOR_SOURCE}")
    endif()
  endforeach()
  if(NOT "${DEMO_FINAL_SCENE_EDITOR_ABSOLUTE_SOURCES}" STREQUAL "${DEMO_EXPECTED_SCENE_EDITOR_SOURCES}")
    message(FATAL_ERROR "final scene_editor_core SOURCES differ from the locked production source set")
  endif()

  get_target_property(DEMO_FINAL_SCENE_EDITOR_LINKS scene_editor_core LINK_LIBRARIES)
  if(NOT "${DEMO_FINAL_SCENE_EDITOR_LINKS}" STREQUAL "scene_editor_document")
    message(FATAL_ERROR
      "final scene_editor_core must link only the CPU document implementation; LINK_LIBRARIES=${DEMO_FINAL_SCENE_EDITOR_LINKS}")
  endif()
  get_target_property(DEMO_FINAL_SCENE_EDITOR_INTERFACE_LINKS scene_editor_core INTERFACE_LINK_LIBRARIES)
  if(NOT "${DEMO_FINAL_SCENE_EDITOR_INTERFACE_LINKS}" STREQUAL "$<LINK_ONLY:scene_editor_document>")
    message(FATAL_ERROR
      "final scene_editor_core must not re-export dependencies; INTERFACE_LINK_LIBRARIES=${DEMO_FINAL_SCENE_EDITOR_INTERFACE_LINKS}")
  endif()

  set(DEMO_EXPECTED_SCENE_EDITOR_DOCUMENT_SOURCES
    "${CMAKE_SOURCE_DIR}/loader/TinyGltfImplementation.cpp"
    "${CMAKE_SOURCE_DIR}/scene/editor/include/mgif/scene_editor/EditorLimits.h"
    "${CMAKE_SOURCE_DIR}/scene/editor/include/mgif/scene_editor/EditorObjectId.h"
    "${CMAKE_SOURCE_DIR}/scene/editor/include/mgif/scene_editor/EditorHistoryPolicy.h"
    "${CMAKE_SOURCE_DIR}/scene/editor/include/mgif/scene_editor/GltfDocument.h"
    "${CMAKE_SOURCE_DIR}/scene/editor/include/mgif/scene_editor/StructuralDelta.h"
    "${CMAKE_SOURCE_DIR}/scene/editor/include/mgif/scene_editor/SceneCommands.h"
    "${CMAKE_SOURCE_DIR}/scene/editor/include/mgif/scene_editor/GltfDocumentIO.h"
    "${CMAKE_SOURCE_DIR}/scene/editor/include/mgif/scene_editor/GltfSemanticCompare.h"
    "${CMAKE_SOURCE_DIR}/scene/editor/include/mgif/scene_editor/GltfValidator.h"
    "${CMAKE_SOURCE_DIR}/scene/editor/GltfDocument.cpp"
    "${CMAKE_SOURCE_DIR}/scene/editor/StructuralDelta.cpp"
    "${CMAKE_SOURCE_DIR}/scene/editor/SceneCommands.cpp"
    "${CMAKE_SOURCE_DIR}/scene/editor/GltfDocumentIO.cpp"
    "${CMAKE_SOURCE_DIR}/scene/editor/GltfSemanticCompare.cpp"
    "${CMAKE_SOURCE_DIR}/scene/editor/GltfValidator.cpp"
  )
  get_target_property(DEMO_FINAL_SCENE_EDITOR_DOCUMENT_SOURCES scene_editor_document SOURCES)
  set(DEMO_FINAL_SCENE_EDITOR_DOCUMENT_ABSOLUTE_SOURCES)
  foreach(DEMO_FINAL_SCENE_EDITOR_DOCUMENT_SOURCE IN LISTS DEMO_FINAL_SCENE_EDITOR_DOCUMENT_SOURCES)
    if(IS_ABSOLUTE "${DEMO_FINAL_SCENE_EDITOR_DOCUMENT_SOURCE}")
      list(APPEND DEMO_FINAL_SCENE_EDITOR_DOCUMENT_ABSOLUTE_SOURCES
        "${DEMO_FINAL_SCENE_EDITOR_DOCUMENT_SOURCE}")
    else()
      list(APPEND DEMO_FINAL_SCENE_EDITOR_DOCUMENT_ABSOLUTE_SOURCES
        "${CMAKE_SOURCE_DIR}/${DEMO_FINAL_SCENE_EDITOR_DOCUMENT_SOURCE}")
    endif()
  endforeach()
  if(NOT "${DEMO_FINAL_SCENE_EDITOR_DOCUMENT_ABSOLUTE_SOURCES}" STREQUAL
         "${DEMO_EXPECTED_SCENE_EDITOR_DOCUMENT_SOURCES}")
    message(FATAL_ERROR "final scene_editor_document SOURCES differ from the locked production source set")
  endif()
  get_target_property(DEMO_FINAL_SCENE_EDITOR_DOCUMENT_LINKS scene_editor_document LINK_LIBRARIES)
  if(NOT "${DEMO_FINAL_SCENE_EDITOR_DOCUMENT_LINKS}" STREQUAL "tinygltf")
    message(FATAL_ERROR
      "final scene_editor_document must link only pinned TinyGLTF; LINK_LIBRARIES=${DEMO_FINAL_SCENE_EDITOR_DOCUMENT_LINKS}")
  endif()
  get_target_property(DEMO_FINAL_SCENE_EDITOR_DOCUMENT_INTERFACE_LINKS
    scene_editor_document INTERFACE_LINK_LIBRARIES)
  if(NOT "${DEMO_FINAL_SCENE_EDITOR_DOCUMENT_INTERFACE_LINKS}" STREQUAL "$<LINK_ONLY:tinygltf>")
    message(FATAL_ERROR
      "final scene_editor_document must not re-export dependencies; INTERFACE_LINK_LIBRARIES=${DEMO_FINAL_SCENE_EDITOR_DOCUMENT_INTERFACE_LINKS}")
  endif()

  get_target_property(DEMO_FINAL_SCENE_EDITOR_DOCUMENT_DEFINITIONS
    scene_editor_document COMPILE_DEFINITIONS)
  get_target_property(DEMO_FINAL_SCENE_EDITOR_DOCUMENT_INTERFACE_DEFINITIONS
    scene_editor_document INTERFACE_COMPILE_DEFINITIONS)
  if(DEMO_FINAL_SCENE_EDITOR_DOCUMENT_INTERFACE_DEFINITIONS)
    message(FATAL_ERROR
      "scene_editor_document must not expose MGIF_EDITOR_TESTING on its interface")
  endif()
  get_target_property(DEMO_FINAL_SCENE_EDITOR_CORE_DEFINITIONS
    scene_editor_core COMPILE_DEFINITIONS)
  get_target_property(DEMO_FINAL_SCENE_EDITOR_CORE_INTERFACE_DEFINITIONS
    scene_editor_core INTERFACE_COMPILE_DEFINITIONS)
  if(DEMO_FINAL_SCENE_EDITOR_CORE_DEFINITIONS OR
     DEMO_FINAL_SCENE_EDITOR_CORE_INTERFACE_DEFINITIONS)
    message(FATAL_ERROR
      "scene_editor_core must not carry testing compile definitions")
  endif()
  set(DEMO_EDITOR_P4_TEST_TARGETS
    editor_history_core_tests
    editor_history_structural_tests
    editor_history_property_tests
    editor_continuous_edit_tests
  )
  if(BUILD_TESTING)
    if(NOT "${DEMO_FINAL_SCENE_EDITOR_DOCUMENT_DEFINITIONS}" STREQUAL
       "MGIF_EDITOR_TESTING=1")
      message(FATAL_ERROR
        "scene_editor_document must privately carry exactly MGIF_EDITOR_TESTING=1 in testing builds")
    endif()
    foreach(DEMO_EDITOR_P4_TEST_TARGET IN LISTS DEMO_EDITOR_P4_TEST_TARGETS)
      if(NOT TARGET ${DEMO_EDITOR_P4_TEST_TARGET})
        message(FATAL_ERROR "missing P4 test target ${DEMO_EDITOR_P4_TEST_TARGET}")
      endif()
      get_target_property(DEMO_EDITOR_P4_TEST_DEFINITIONS
        ${DEMO_EDITOR_P4_TEST_TARGET} COMPILE_DEFINITIONS)
      get_target_property(DEMO_EDITOR_P4_TEST_INTERFACE_DEFINITIONS
        ${DEMO_EDITOR_P4_TEST_TARGET} INTERFACE_COMPILE_DEFINITIONS)
      if(NOT "MGIF_EDITOR_TESTING=1" IN_LIST DEMO_EDITOR_P4_TEST_DEFINITIONS OR
         DEMO_EDITOR_P4_TEST_INTERFACE_DEFINITIONS)
        message(FATAL_ERROR
          "${DEMO_EDITOR_P4_TEST_TARGET} must carry MGIF_EDITOR_TESTING=1 privately")
      endif()
    endforeach()
  elseif(DEMO_FINAL_SCENE_EDITOR_DOCUMENT_DEFINITIONS)
    message(FATAL_ERROR
      "MGIF_EDITOR_TESTING must be absent when BUILD_TESTING is OFF")
  endif()

  if(TARGET imgui_demo)
    set(DEMO_EXPECTED_IMGUI_SOURCES
      "${imgui_SOURCE_DIR}/imgui.cpp"
      "${imgui_SOURCE_DIR}/imgui.h"
      "${imgui_SOURCE_DIR}/imgui_demo.cpp"
      "${imgui_SOURCE_DIR}/imgui_draw.cpp"
      "${imgui_SOURCE_DIR}/imgui_tables.cpp"
      "${imgui_SOURCE_DIR}/imgui_widgets.cpp"
    )
    if(ANDROID)
      list(APPEND DEMO_EXPECTED_IMGUI_SOURCES
        "${imgui_SOURCE_DIR}/backends/imgui_impl_android.cpp"
        "${imgui_SOURCE_DIR}/backends/imgui_impl_android.h"
      )
      set(DEMO_EXPECTED_IMGUI_LINKS android)
    else()
      list(APPEND DEMO_EXPECTED_IMGUI_SOURCES
        "${imgui_SOURCE_DIR}/backends/imgui_impl_glfw.cpp"
        "${imgui_SOURCE_DIR}/backends/imgui_impl_glfw.h"
      )
      set(DEMO_EXPECTED_IMGUI_LINKS glfw)
    endif()
    get_target_property(DEMO_FINAL_IMGUI_SOURCES imgui_demo SOURCES)
    if(NOT "${DEMO_FINAL_IMGUI_SOURCES}" STREQUAL "${DEMO_EXPECTED_IMGUI_SOURCES}")
      message(FATAL_ERROR "final imgui_demo SOURCES differ from the canonical platform source set")
    endif()
    get_target_property(DEMO_FINAL_IMGUI_LINKS imgui_demo LINK_LIBRARIES)
    if(NOT "${DEMO_FINAL_IMGUI_LINKS}" STREQUAL "${DEMO_EXPECTED_IMGUI_LINKS}")
      message(FATAL_ERROR "final imgui_demo dependency set differs from the canonical platform target")
    endif()
  endif()

  if(TARGET scene_editor_dependency_preflight AND
     NOT DEMO_SKIP_EDITOR_DEPENDENCY_PREFLIGHT)
    set(DEMO_EDITOR_FENCED_TARGETS)
    mgif_collect_editor_compile_targets("${CMAKE_SOURCE_DIR}" DEMO_EDITOR_FENCED_TARGETS)
    list(REMOVE_DUPLICATES DEMO_EDITOR_FENCED_TARGETS)
    list(SORT DEMO_EDITOR_FENCED_TARGETS COMPARE STRING CASE INSENSITIVE)
    foreach(DEMO_EDITOR_FENCED_TARGET IN LISTS DEMO_EDITOR_FENCED_TARGETS)
      add_dependencies(${DEMO_EDITOR_FENCED_TARGET} scene_editor_dependency_preflight)
      get_target_property(DEMO_EDITOR_FENCED_DEFINITIONS
        ${DEMO_EDITOR_FENCED_TARGET} COMPILE_DEFINITIONS)
      if("MGIF_EDITOR_TESTING=1" IN_LIST DEMO_EDITOR_FENCED_DEFINITIONS AND
         NOT DEMO_EDITOR_FENCED_TARGET STREQUAL "scene_editor_document" AND
         NOT DEMO_EDITOR_FENCED_TARGET IN_LIST DEMO_EDITOR_P4_TEST_TARGETS)
        message(FATAL_ERROR
          "MGIF_EDITOR_TESTING escaped its five private target owners: ${DEMO_EDITOR_FENCED_TARGET}")
      endif()
    endforeach()
    set(DEMO_EDITOR_FENCED_TARGETS "${DEMO_EDITOR_FENCED_TARGETS}"
      CACHE INTERNAL "Final compile-target census protected by the editor preflight" FORCE)
    if(TARGET scene_editor_p0_scaffold)
      add_dependencies(scene_editor_p0_scaffold ${DEMO_EDITOR_FENCED_TARGETS})
    endif()
  endif()
endfunction()
function(mgif_collect_editor_compile_targets DEMO_EDITOR_DIRECTORY DEMO_EDITOR_OUTPUT)
  get_property(DEMO_EDITOR_LOCAL_TARGETS
    DIRECTORY "${DEMO_EDITOR_DIRECTORY}" PROPERTY BUILDSYSTEM_TARGETS)
  set(DEMO_EDITOR_RESULT)
  foreach(DEMO_EDITOR_TARGET IN LISTS DEMO_EDITOR_LOCAL_TARGETS)
    get_target_property(DEMO_EDITOR_IMPORTED ${DEMO_EDITOR_TARGET} IMPORTED)
    get_target_property(DEMO_EDITOR_TYPE ${DEMO_EDITOR_TARGET} TYPE)
    if(NOT DEMO_EDITOR_IMPORTED
       AND DEMO_EDITOR_TYPE MATCHES
         "^(EXECUTABLE|STATIC_LIBRARY|SHARED_LIBRARY|MODULE_LIBRARY|OBJECT_LIBRARY)$")
      list(APPEND DEMO_EDITOR_RESULT ${DEMO_EDITOR_TARGET})
    endif()
  endforeach()
  get_property(DEMO_EDITOR_SUBDIRECTORIES
    DIRECTORY "${DEMO_EDITOR_DIRECTORY}" PROPERTY SUBDIRECTORIES)
  foreach(DEMO_EDITOR_SUBDIRECTORY IN LISTS DEMO_EDITOR_SUBDIRECTORIES)
    mgif_collect_editor_compile_targets(
      "${DEMO_EDITOR_SUBDIRECTORY}" DEMO_EDITOR_CHILD_TARGETS)
    list(APPEND DEMO_EDITOR_RESULT ${DEMO_EDITOR_CHILD_TARGETS})
  endforeach()
  set(${DEMO_EDITOR_OUTPUT} "${DEMO_EDITOR_RESULT}" PARENT_SCOPE)
endfunction()

function(mgif_editor_dependency_tail_drain)
  get_property(DEMO_EDITOR_DRAIN_COUNT GLOBAL PROPERTY MGIF_EDITOR_DRAIN_COUNT)
  if(NOT DEMO_EDITOR_DRAIN_COUNT)
    set(DEMO_EDITOR_DRAIN_COUNT 0)
  endif()
  math(EXPR DEMO_EDITOR_DRAIN_COUNT "${DEMO_EDITOR_DRAIN_COUNT} + 1")
  if(DEMO_EDITOR_DRAIN_COUNT GREATER 64)
    message(FATAL_ERROR "production editor deferred-call tail did not stabilize")
  endif()
  set_property(GLOBAL PROPERTY MGIF_EDITOR_DRAIN_COUNT ${DEMO_EDITOR_DRAIN_COUNT})
  cmake_language(DEFER DIRECTORY "${CMAKE_SOURCE_DIR}"
    GET_CALL_IDS DEMO_EDITOR_PENDING_CALLS)
  if(DEMO_EDITOR_PENDING_CALLS)
    cmake_language(DEFER DIRECTORY "${CMAKE_SOURCE_DIR}"
      CALL mgif_editor_dependency_tail_drain)
    return()
  endif()
  mgif_finalize_editor_dependency_targets()
  mgif_finalize_p6_selection_targets()
endfunction()

if(BUILD_TESTING AND NOT ANDROID)
  add_executable(scene_selection_tests tests/editor/scene_selection_tests.cpp app/editor/SceneSelection.cpp)
  target_include_directories(scene_selection_tests PRIVATE "${CMAKE_CURRENT_LIST_DIR}" "${CMAKE_CURRENT_LIST_DIR}/scene/editor/include")
  target_compile_definitions(scene_selection_tests PRIVATE MGIF_TEST_SOURCE_DIR="${CMAKE_CURRENT_LIST_DIR}")
  target_link_libraries(scene_selection_tests PRIVATE scene_editor_core scene_change_contract)
  add_test(NAME scene_editor.scene_selection COMMAND scene_selection_tests)
  add_executable(material_clipboard_codec_tests tests/editor/material_clipboard_codec_tests.cpp app/editor/MaterialClipboardCodec.cpp)
  target_include_directories(material_clipboard_codec_tests PRIVATE "${CMAKE_CURRENT_LIST_DIR}" "${CMAKE_CURRENT_LIST_DIR}/scene/editor/include")
  target_compile_definitions(material_clipboard_codec_tests PRIVATE MGIF_TEST_SOURCE_DIR="${CMAKE_CURRENT_LIST_DIR}")
  target_link_libraries(material_clipboard_codec_tests PRIVATE scene_editor_core scene_change_contract)
  add_test(NAME scene_editor.material_clipboard_codec COMMAND material_clipboard_codec_tests)
  add_executable(editor_viewport_metrics_tests tests/editor/editor_viewport_metrics_tests.cpp)
  target_include_directories(editor_viewport_metrics_tests PRIVATE "${CMAKE_CURRENT_LIST_DIR}")
  add_test(NAME scene_editor.viewport_metrics COMMAND editor_viewport_metrics_tests)
  add_executable(scene_render_occurrence_mapping_tests tests/editor/scene_render_occurrence_mapping_tests.cpp scene/editor/SceneRenderOccurrenceMapping.cpp)
  target_include_directories(scene_render_occurrence_mapping_tests PRIVATE "${CMAKE_CURRENT_LIST_DIR}" "${CMAKE_CURRENT_LIST_DIR}/scene/editor/include")
  target_compile_definitions(scene_render_occurrence_mapping_tests PRIVATE MGIF_EDITOR_PICKING_TESTING=1)
  add_test(NAME scene_editor.render_occurrence_mapping COMMAND scene_render_occurrence_mapping_tests)
  add_executable(editor_picking_rhi_tests tests/editor/editor_picking_rhi_tests.cpp scene/editor/SceneRenderOccurrenceMapping.cpp render/editor/ScenePicking.cpp render/passes/EditorPickingPass.cpp)
  target_include_directories(editor_picking_rhi_tests PRIVATE "${CMAKE_CURRENT_LIST_DIR}" "${CMAKE_CURRENT_LIST_DIR}/scene/editor/include")
  target_compile_definitions(editor_picking_rhi_tests PRIVATE MGIF_EDITOR_PICKING_TESTING=1 MGIF_TEST_SOURCE_DIR="${CMAKE_CURRENT_LIST_DIR}")
  add_test(NAME scene_editor.picking_rhi COMMAND editor_picking_rhi_tests)
  add_dependencies(scene_editor_tests scene_selection_tests material_clipboard_codec_tests editor_viewport_metrics_tests scene_render_occurrence_mapping_tests editor_picking_rhi_tests)
endif()
function(mgif_finalize_p6_selection_targets)
  set(DEMO_P6_EARLY_LEGACY_TESTS scene_change_capture_tests scene_editor_scene_commands_tests scene_editor_mutation_tests editor_history_core_tests editor_history_structural_tests editor_history_property_tests editor_continuous_edit_tests editor_selection_contract_tests)
  if(BUILD_TESTING AND NOT ANDROID)
    if(NOT TARGET scene_editor_core_legacy_test)
      message(FATAL_ERROR "missing legacy selection core")
    endif()
    get_target_property(DEMO_P6_EARLY_PROD_SOURCES scene_editor_core SOURCES)
    get_target_property(DEMO_P6_EARLY_LEGACY_SOURCES scene_editor_core_legacy_test SOURCES)
    get_target_property(DEMO_P6_EARLY_LEGACY_LINKS scene_editor_core_legacy_test LINK_LIBRARIES)
    get_target_property(DEMO_P6_EARLY_LEGACY_INTERFACE_LINKS scene_editor_core_legacy_test INTERFACE_LINK_LIBRARIES)
    get_target_property(DEMO_P6_EARLY_LEGACY_DEFS scene_editor_core_legacy_test COMPILE_DEFINITIONS)
    get_target_property(DEMO_P6_EARLY_LEGACY_INTERFACE_DEFS scene_editor_core_legacy_test INTERFACE_COMPILE_DEFINITIONS)
    if(NOT "${DEMO_P6_EARLY_PROD_SOURCES}" STREQUAL "${DEMO_P6_EARLY_LEGACY_SOURCES}" OR NOT "${DEMO_P6_EARLY_LEGACY_LINKS}" STREQUAL "scene_editor_document" OR NOT "${DEMO_P6_EARLY_LEGACY_INTERFACE_LINKS}" STREQUAL "$<LINK_ONLY:scene_editor_document>" OR NOT "${DEMO_P6_EARLY_LEGACY_DEFS}" STREQUAL "MGIF_LEGACY_SELECTION_TESTING=1" OR NOT "${DEMO_P6_EARLY_LEGACY_INTERFACE_DEFS}" STREQUAL "MGIF_LEGACY_SELECTION_TESTING=1")
      message(FATAL_ERROR "legacy selection core ABI/link closure differs")
    endif()
    foreach(DEMO_P6_EARLY_TEST IN LISTS DEMO_P6_EARLY_LEGACY_TESTS)
      get_target_property(DEMO_P6_EARLY_TEST_LINKS ${DEMO_P6_EARLY_TEST} LINK_LIBRARIES)
      if(NOT "scene_editor_core_legacy_test" IN_LIST DEMO_P6_EARLY_TEST_LINKS OR "scene_editor_core" IN_LIST DEMO_P6_EARLY_TEST_LINKS)
        message(FATAL_ERROR "${DEMO_P6_EARLY_TEST} has a mixed selection ABI")
      endif()
    endforeach()
    mgif_collect_editor_compile_targets("${CMAKE_SOURCE_DIR}" DEMO_P6_EARLY_ALL_COMPILE_TARGETS)
    foreach(DEMO_P6_EARLY_COMPILE_TARGET IN LISTS DEMO_P6_EARLY_ALL_COMPILE_TARGETS)
      get_target_property(DEMO_P6_EARLY_COMPILE_LINKS ${DEMO_P6_EARLY_COMPILE_TARGET} LINK_LIBRARIES)
      if("scene_editor_core_legacy_test" IN_LIST DEMO_P6_EARLY_COMPILE_LINKS AND NOT DEMO_P6_EARLY_COMPILE_TARGET IN_LIST DEMO_P6_EARLY_LEGACY_TESTS)
        message(FATAL_ERROR "legacy selection ABI escaped to ${DEMO_P6_EARLY_COMPILE_TARGET}")
      endif()
    endforeach()
  elseif(TARGET scene_editor_core_legacy_test)
    message(FATAL_ERROR "legacy selection core escaped desktop test builds")
  endif()
endfunction()
if(DEMO_EDITOR_CORE_ONLY)
  cmake_language(DEFER CALL mgif_editor_dependency_tail_drain)
  return()
endif()
option(USE_SLANG "Use Slang shaders instead of GLSL" ON)
# GLSL remains a compatibility-only, non-authoritative shader path.
option(VK_SEMAPHORE_DEBUG "Enable semaphore debugging logs" OFF)
if(WIN32)
  option(DEMO_ENABLE_D3D12 "Build the Direct3D 12 RHI foundation on Windows" ON)
else()
  option(DEMO_ENABLE_D3D12 "Build the Direct3D 12 RHI foundation on Windows" OFF)
endif()
if(DEMO_ENABLE_D3D12 AND NOT WIN32)
  message(FATAL_ERROR "DEMO_ENABLE_D3D12 is supported only on Windows")
endif()
if(MSVC)
    add_compile_options(/utf-8)
endif()
list(APPEND CMAKE_MODULE_PATH "${CMAKE_CURRENT_LIST_DIR}/cmake")
include(CompilerSlangShader)
include(CompilerGlslShader)
include(DownloadPackage)
find_package(Vulkan REQUIRED)
set(DEMO_VULKAN_HEADER_DIR "" CACHE PATH "Optional Vulkan header directory to use before the platform SDK headers")
if(DEMO_VULKAN_HEADER_DIR)
  include_directories(BEFORE "${DEMO_VULKAN_HEADER_DIR}")
endif()
if(ANDROID)
  add_compile_definitions(VK_USE_PLATFORM_ANDROID_KHR)
  set(CMAKE_SHARED_LINKER_FLAGS "${CMAKE_SHARED_LINKER_FLAGS} -u ANativeActivity_onCreate")
  add_compile_definitions(NDEBUG)
endif()
if(USE_SLANG)
  set(Slang_VERSION "2026.3.1")
  file(GLOB DEMO_STALE_SLANG_DIRS "${CMAKE_BINARY_DIR}/_deps/Slang-*")
  foreach(DEMO_STALE_SLANG_DIR IN LISTS DEMO_STALE_SLANG_DIRS)
    if(IS_DIRECTORY "${DEMO_STALE_SLANG_DIR}"
       AND NOT EXISTS "${DEMO_STALE_SLANG_DIR}/bin"
       AND NOT EXISTS "${DEMO_STALE_SLANG_DIR}/include")
      file(REMOVE_RECURSE "${DEMO_STALE_SLANG_DIR}")
    endif()
  endforeach()
  find_package(Slang REQUIRED)
endif()
if(NOT ANDROID)
  FetchContent_Declare(
    glfw
    GIT_REPOSITORY https://github.com/glfw/glfw.git
    GIT_TAG 3.4
  )
  set(GLFW_BUILD_DOCS OFF CACHE BOOL "" FORCE)
  set(GLFW_BUILD_EXAMPLES OFF CACHE BOOL "" FORCE)
  set(GLFW_BUILD_TESTS OFF CACHE BOOL "" FORCE)
  set(GLFW_BUILD_WAYLAND OFF CACHE BOOL "" FORCE)
  set(GLFW_INSTALL OFF CACHE BOOL "" FORCE)
  FetchContent_MakeAvailable(glfw)
endif()
FetchContent_Declare(volk GIT_REPOSITORY https://github.com/zeux/volk.git GIT_TAG master)
FetchContent_MakeAvailable(volk)
FetchContent_Declare(
  meshoptimizer
  GIT_REPOSITORY https://github.com/zeux/meshoptimizer.git
  GIT_TAG v0.23
)
FetchContent_MakeAvailable(meshoptimizer)
FetchContent_Declare(
  VulkanMemoryAllocator
  GIT_REPOSITORY https://github.com/GPUOpen-LibrariesAndSDKs/VulkanMemoryAllocator.git
  GIT_TAG master
)
FetchContent_MakeAvailable(VulkanMemoryAllocator)
FetchContent_Declare(
  imgui
  GIT_REPOSITORY https://github.com/ocornut/imgui.git
  GIT_TAG fd13a1e8923a0a7077b404fc36fd063b25a0c0b5
)
FetchContent_MakeAvailable(imgui)
add_library(imgui_demo STATIC
  ${imgui_SOURCE_DIR}/imgui.cpp
  ${imgui_SOURCE_DIR}/imgui.h
  ${imgui_SOURCE_DIR}/imgui_demo.cpp
  ${imgui_SOURCE_DIR}/imgui_draw.cpp
  ${imgui_SOURCE_DIR}/imgui_tables.cpp
  ${imgui_SOURCE_DIR}/imgui_widgets.cpp
)
if(ANDROID)
  target_sources(imgui_demo PRIVATE
    ${imgui_SOURCE_DIR}/backends/imgui_impl_android.cpp
    ${imgui_SOURCE_DIR}/backends/imgui_impl_android.h
  )
else()
  target_sources(imgui_demo PRIVATE
    ${imgui_SOURCE_DIR}/backends/imgui_impl_glfw.cpp
    ${imgui_SOURCE_DIR}/backends/imgui_impl_glfw.h
  )
endif()
if(NOT Python3_Interpreter_FOUND)
  find_package(Python3 REQUIRED COMPONENTS Interpreter)
endif()
target_include_directories(imgui_demo PUBLIC
  ${imgui_SOURCE_DIR}
  ${imgui_SOURCE_DIR}/backends
)
if(ANDROID)
  target_link_libraries(imgui_demo PUBLIC android)
else()
  target_link_libraries(imgui_demo PUBLIC glfw)
endif()
set(STB_IMAGE_URL "https://raw.githubusercontent.com/nothings/stb/master/stb_image.h")
set(STB_IMAGE_PATH "${CMAKE_CURRENT_BINARY_DIR}/stb_image.h")
if(NOT EXISTS "${STB_IMAGE_PATH}")
  set(STB_IMAGE_CACHED_PATH "${CMAKE_CURRENT_LIST_DIR}/out/build/x64-Debug/stb_image.h")
  if(EXISTS "${STB_IMAGE_CACHED_PATH}")
    message(STATUS "Copying cached stb_image.h for Demo...")
    file(COPY_FILE "${STB_IMAGE_CACHED_PATH}" "${STB_IMAGE_PATH}")
  else()
    message(STATUS "Downloading stb_image.h for Demo...")
    file(DOWNLOAD "${STB_IMAGE_URL}" "${STB_IMAGE_PATH}")
  endif()
endif()
if(UNIX AND NOT APPLE AND NOT ANDROID)
  find_package(X11 REQUIRED)
  find_package(Threads REQUIRED)
endif()
file(GLOB SHADER_SLANG_FILES CONFIGURE_DEPENDS
  "${CMAKE_CURRENT_LIST_DIR}/shaders/*.slang")
file(GLOB SHADER_GLSL_FILES CONFIGURE_DEPENDS
  "${CMAKE_CURRENT_LIST_DIR}/shaders/*.glsl")
file(GLOB SHADER_H_FILES     "${CMAKE_CURRENT_LIST_DIR}/shaders/*.h")
# Android is viewer-only and excludes every desktop editor render shader.
if(ANDROID)
  list(FILTER SHADER_SLANG_FILES EXCLUDE REGEX "editor_(picking|highlight|overlay)\\.slang$")
  list(FILTER SHADER_GLSL_FILES EXCLUDE REGEX "editor_(picking|highlight|overlay)\\.glsl$")
endif()
list(FILTER SHADER_SLANG_FILES EXCLUDE REGEX "shader\\.ibl_common\\.slang$")
list(FILTER SHADER_SLANG_FILES EXCLUDE REGEX "sdf_common\\.slang$")
list(FILTER SHADER_SLANG_FILES EXCLUDE REGEX "ddgi_common\\.slang$")
list(FILTER SHADER_SLANG_FILES EXCLUDE REGEX "ddgi_sample_probe\\.slang$")
list(FILTER SHADER_SLANG_FILES EXCLUDE REGEX "surface_atlas_common\\.slang$")
list(FILTER SHADER_SLANG_FILES EXCLUDE REGEX "flax_ddgi_common\\.slang$")
list(FIND SHADER_SLANG_FILES "${CMAKE_CURRENT_LIST_DIR}/shaders/shader.light_gpu_driven.slang" DEMO_GPU_LIGHT_SHADER_INDEX)
if(DEMO_GPU_LIGHT_SHADER_INDEX EQUAL -1)
  list(APPEND SHADER_SLANG_FILES "${CMAKE_CURRENT_LIST_DIR}/shaders/shader.light_gpu_driven.slang")
endif()
set(SHADER_OUTPUT_DIR "${CMAKE_CURRENT_BINARY_DIR}/_autogen")
if(USE_SLANG)
  compile_slang(
    "${SHADER_SLANG_FILES}"
    "${SHADER_OUTPUT_DIR}"
    HEADERS_VAR GENERATED_SHADER_SLANG_HEADERS
    SPVS_VAR GENERATED_SHADER_SPV_BINARIES
    EXTRA_FLAGS "-I${CMAKE_CURRENT_LIST_DIR}"
  )
  # Reflection metadata will be extracted from SPIR-V at build time or runtime
  # For v1, we establish the contract structure; actual extraction is a follow-on task
else()
  set(GENERATED_SHADER_SLANG_HEADERS "")
  set(GENERATED_SHADER_SPV_BINARIES "")
endif()

# The Windows Vulkan final-color pass uses the same DXC downstream as D3D12 so
# UNORM quantization follows identical floating-point code generation. DXC emits
# separate image/sampler arrays; the renderer supplies a Vulkan-only public RHI
# layout for those two bindings while every other pass keeps the authoritative
# Slang SPIR-V module.
if(WIN32 AND USE_SLANG AND DEMO_ENABLE_D3D12)
  get_filename_component(_VULKAN_TOOL_DIR "${Vulkan_GLSLANG_VALIDATOR_EXECUTABLE}" DIRECTORY)
  find_program(DEMO_DXC_SPIRV_EXECUTABLE
    NAMES dxc
    HINTS "${_VULKAN_TOOL_DIR}"
    REQUIRED
  )

  set(DEMO_DXC_FINAL_HLSL "${SHADER_OUTPUT_DIR}/shader.light.final.dxc.hlsl")
  set(DEMO_DXC_FINAL_SPV "${SHADER_OUTPUT_DIR}/shader.light.final.dxc.spv")
  set(DEMO_DXC_FINAL_HEADER "${SHADER_OUTPUT_DIR}/shader.light.final.dxc.spv.h")
  set(DEMO_DXC_FINAL_DEPFILE "${SHADER_OUTPUT_DIR}/shader.light.final.dxc.dep")
  add_custom_command(
    OUTPUT
      "${DEMO_DXC_FINAL_HLSL}"
      "${DEMO_DXC_FINAL_SPV}"
      "${DEMO_DXC_FINAL_HEADER}"
    COMMAND "${Slang_SLANGC_EXECUTABLE}"
      "${CMAKE_CURRENT_LIST_DIR}/shaders/shader.light_gpu_driven.slang"
      -depfile "${DEMO_DXC_FINAL_DEPFILE}"
      -lang slang
      -matrix-layout-column-major
      -O0
      -g1
      -entry fragmentFinalColorMain
      -stage fragment
      -I "${CMAKE_CURRENT_LIST_DIR}"
      -DDEMO_D3D12=1
      -target hlsl
      -profile sm_6_6
      -o "${DEMO_DXC_FINAL_HLSL}"
    COMMAND "${DEMO_DXC_SPIRV_EXECUTABLE}"
      -spirv
      -T ps_6_6
      -E fragmentFinalColorMain
      -Od
      -enable-16bit-types
      -HV 2021
      -fspv-target-env=vulkan1.3
      -fvk-use-scalar-layout
      -fspv-preserve-interface
      -fvk-bind-register t0 1 0 0
      -fvk-bind-register s0 2 12 0
      -fvk-bind-register b4 0 4 1
      -Fo "${DEMO_DXC_FINAL_SPV}"
      "${DEMO_DXC_FINAL_HLSL}"
    COMMAND "${CMAKE_COMMAND}"
      "-DINPUT_FILE=${DEMO_DXC_FINAL_SPV}"
      "-DOUTPUT_FILE=${DEMO_DXC_FINAL_HEADER}"
      "-DVARIABLE_NAME=shader_light_final_dxc_spv"
      -P "${CMAKE_CURRENT_LIST_DIR}/cmake/EmbedSpirv.cmake"
    DEPENDS
      "${CMAKE_CURRENT_LIST_DIR}/shaders/shader.light_gpu_driven.slang"
      "${CMAKE_CURRENT_LIST_DIR}/shaders/shader.light.slang"
      "${CMAKE_CURRENT_LIST_DIR}/shaders/shader_io.h"
      "${CMAKE_CURRENT_LIST_DIR}/cmake/EmbedSpirv.cmake"
    DEPFILE "${DEMO_DXC_FINAL_DEPFILE}"
    COMMENT "Compiling DXC SPIR-V final-color shader"
    VERBATIM
  )
  list(APPEND GENERATED_SHADER_SLANG_HEADERS "${DEMO_DXC_FINAL_HEADER}")
  list(APPEND GENERATED_SHADER_SPV_BINARIES "${DEMO_DXC_FINAL_SPV}")
  set(DEMO_HAS_DXC_FINAL_SPIRV TRUE)
endif()

compile_glsl(
  "${SHADER_GLSL_FILES}"
  "${SHADER_OUTPUT_DIR}"
  GENERATED_SHADER_GLSL_HEADERS
  EXTRA_FLAGS "-I${CMAKE_CURRENT_LIST_DIR}"
)

if(NOT ANDROID)
  # P7 owns the backend-neutral packet path, the RHI overlay pass, and the
  # desktop input adapter. Keep these targets outside the frozen P0 target
  # declaration block so dependency-policy source locations remain stable.
  add_library(editor_gizmo_overlay_runtime STATIC
    render/editor/EditorOverlayTypes.h
    render/editor/GizmoGeometry.h
    render/editor/GizmoGeometry.cpp
    render/editor/GridModel.h
    render/editor/GridModel.cpp
    render/editor/TransformGizmo.h
    render/editor/TransformGizmo.cpp
  )
  add_library(MGIF::editor_gizmo_overlay_runtime ALIAS
    editor_gizmo_overlay_runtime)
  target_include_directories(editor_gizmo_overlay_runtime PUBLIC
    "${CMAKE_CURRENT_LIST_DIR}"
    "${CMAKE_CURRENT_BINARY_DIR}/_deps/glm-src")
  target_compile_features(editor_gizmo_overlay_runtime PUBLIC cxx_std_20)

  add_library(editor_overlay_rhi_runtime STATIC
    render/editor/EditorOverlayShaderInterop.h
    render/passes/EditorOverlayPass.h
    render/passes/EditorOverlayPass.cpp
  )
  add_library(MGIF::editor_overlay_rhi_runtime ALIAS
    editor_overlay_rhi_runtime)
  target_include_directories(editor_overlay_rhi_runtime PUBLIC
    "${CMAKE_CURRENT_LIST_DIR}")
  target_compile_features(editor_overlay_rhi_runtime PUBLIC cxx_std_20)
  target_link_libraries(editor_overlay_rhi_runtime PUBLIC
    editor_gizmo_overlay_runtime)

  add_library(editor_input_router_runtime STATIC
    scene/editor/include/mgif/scene_editor/EditorInputRouter.h
    scene/editor/EditorInputRouter.cpp
  )
  add_library(MGIF::editor_input_router_runtime ALIAS
    editor_input_router_runtime)
  target_include_directories(editor_input_router_runtime
    PUBLIC "${CMAKE_CURRENT_LIST_DIR}/scene/editor/include")
  target_compile_features(editor_input_router_runtime PUBLIC cxx_std_20)
  target_link_libraries(editor_input_router_runtime PUBLIC
    scene_editor_interaction_contract)
endif()

if(NOT ANDROID)
  add_library(desktop_scene_editor_ui STATIC
    app/editor/EditorController.h
    app/editor/EditorController.cpp
    app/editor/EditorUiProbe.h
    app/editor/EditorInputRouter.h
    app/editor/EditorInputRouter.cpp
    app/editor/EditorRendererAdapter.h
    app/editor/EditorRendererAdapter.cpp
    app/editor/EditorViewportRuntime.h
    app/editor/EditorViewportRuntime.cpp
    app/editor/SceneEditorUi.h
    app/editor/SceneEditorUi.cpp
    app/editor/SceneEditorUiModel.h
    app/editor/EditorTransactionSmoke.h
    scene/editor/include/mgif/scene_editor/SceneProjection.h
    scene/editor/SceneProjection.cpp
  )
  add_library(MGIF::desktop_scene_editor_ui ALIAS desktop_scene_editor_ui)
  target_compile_features(desktop_scene_editor_ui PUBLIC cxx_std_20)
  target_compile_definitions(desktop_scene_editor_ui PRIVATE
    MGIF_ENABLE_SCENE_TRANSACTIONS=1)
  target_include_directories(desktop_scene_editor_ui
    PUBLIC
      "${CMAKE_CURRENT_LIST_DIR}"
      "${CMAKE_CURRENT_LIST_DIR}/scene/editor/include"
      "${CMAKE_CURRENT_LIST_DIR}/render/editor/include"
    PRIVATE
      "${imgui_SOURCE_DIR}"
      "${CMAKE_CURRENT_BINARY_DIR}/_deps/glm-src"
      "${CMAKE_CURRENT_BINARY_DIR}/_deps/tinygltf-src"
  )
  target_link_libraries(desktop_scene_editor_ui PRIVATE
    animation_playback_runtime imgui_demo scene_editor_core
    editor_input_router_runtime
    editor_overlay_runtime
    editor_gizmo_overlay_runtime
    scene_compactor_runtime
    scene_merger_runtime
  )
endif()

# Acceptance-only state/action bridge.  Keep this outside the production UI
# target so a normal Demo build has no driver object or replay behavior.  The
# bridge receives immutable FrameModel values and typed UiCommand callbacks;
# it never links a controller implementation of its own.
if(BUILD_TESTING AND NOT ANDROID)
  add_library(editor_test_driver STATIC
    app/editor/EditorTestDriver.h
    app/editor/EditorTestDriver.cpp
  )
  target_compile_features(editor_test_driver PUBLIC cxx_std_20)
  target_compile_definitions(editor_test_driver PRIVATE
    MGIF_EDITOR_ACCEPTANCE_DRIVER=1)
  target_include_directories(editor_test_driver PUBLIC
    "${CMAKE_CURRENT_LIST_DIR}"
    "${CMAKE_CURRENT_LIST_DIR}/scene/editor/include")
  target_link_libraries(editor_test_driver PUBLIC desktop_scene_editor_ui)
endif()

if(BUILD_TESTING AND NOT ANDROID)
  add_library(editor_ui_os_replay_plan STATIC
    app/editor/EditorUiOsReplayPlan.h
    app/editor/EditorUiOsReplayPlan.cpp)
  target_compile_features(editor_ui_os_replay_plan PUBLIC cxx_std_20)
  target_include_directories(editor_ui_os_replay_plan PUBLIC
    "${CMAKE_CURRENT_LIST_DIR}"
    "${CMAKE_CURRENT_LIST_DIR}/scene/editor/include")
  target_link_libraries(editor_ui_os_replay_plan PUBLIC
    desktop_scene_editor_ui)

  add_library(editor_ui_os_replay_runtime STATIC
    app/editor/EditorUiOsReplayRuntime.h
    app/editor/EditorUiOsReplayRuntime.cpp)
  target_compile_features(editor_ui_os_replay_runtime PUBLIC cxx_std_20)
  target_include_directories(editor_ui_os_replay_runtime PUBLIC
    "${CMAKE_CURRENT_LIST_DIR}")
  target_link_libraries(editor_ui_os_replay_runtime PUBLIC
    editor_ui_os_replay_plan)

  add_library(editor_ui_os_replay_cli STATIC
    app/editor/EditorUiOsReplayCli.h
    app/editor/EditorUiOsReplayCli.cpp)
  target_compile_features(editor_ui_os_replay_cli PUBLIC cxx_std_20)
  target_include_directories(editor_ui_os_replay_cli PUBLIC
    "${CMAKE_CURRENT_LIST_DIR}")
  target_link_libraries(editor_ui_os_replay_cli PUBLIC
    editor_ui_os_replay_plan)

  add_executable(editor_ui_os_replay_plan_tests
    tests/editor/editor_ui_os_replay_plan_tests.cpp)
  target_link_libraries(editor_ui_os_replay_plan_tests PRIVATE
    editor_ui_os_replay_plan)
  add_test(NAME scene_editor.ui_os_replay_plan
    COMMAND editor_ui_os_replay_plan_tests)

  add_executable(editor_ui_os_replay_runtime_tests
    tests/editor/editor_ui_os_replay_runtime_tests.cpp)
  target_link_libraries(editor_ui_os_replay_runtime_tests PRIVATE
    editor_ui_os_replay_runtime)
  add_test(NAME scene_editor.ui_os_replay_runtime
    COMMAND editor_ui_os_replay_runtime_tests)

  add_executable(editor_ui_os_replay_cli_tests
    tests/editor/editor_ui_os_replay_cli_tests.cpp)
  target_compile_definitions(editor_ui_os_replay_cli_tests PRIVATE
    MGIF_EDITOR_OS_REPLAY=1)
  target_link_libraries(editor_ui_os_replay_cli_tests PRIVATE
    editor_ui_os_replay_cli)
  add_test(NAME scene_editor.ui_os_replay_cli
    COMMAND editor_ui_os_replay_cli_tests)

  if(WIN32)
    add_test(NAME scene_editor.ui_os_replay_contract
      COMMAND "${Python3_EXECUTABLE}" -B
        "${CMAKE_CURRENT_LIST_DIR}/tests/editor/editor_ui_os_replay_contract_tests.py"
        -q)
  endif()
  set_tests_properties(
    scene_editor.ui_os_replay_plan
    scene_editor.ui_os_replay_runtime
    scene_editor.ui_os_replay_cli
    PROPERTIES LABELS "scene-editor-p8-os-replay")
  if(WIN32)
    set_tests_properties(scene_editor.ui_os_replay_contract PROPERTIES
      LABELS "scene-editor-p8-os-replay")
  endif()
  add_dependencies(scene_editor_tests
    editor_ui_os_replay_plan_tests
    editor_ui_os_replay_runtime_tests
    editor_ui_os_replay_cli_tests)
endif()

if(BUILD_TESTING AND NOT ANDROID)
  add_executable(editor_controller_animation_tests
    tests/editor/editor_controller_animation_tests.cpp
    loader/GltfLoader.cpp
    loader/SceneCacheSerializer.cpp
    scene/SceneAssetBuilder.cpp
    scene/SceneAssetSerializer.cpp
  )
  target_compile_features(editor_controller_animation_tests PRIVATE cxx_std_20)
  target_compile_definitions(editor_controller_animation_tests PRIVATE
    MGIF_TEST_SOURCE_DIR="${CMAKE_CURRENT_LIST_DIR}"
  )
  target_link_libraries(editor_controller_animation_tests PRIVATE
    desktop_scene_editor_ui
    scene_change_capture_runtime
    glm
    imgui_demo
    tinygltf
  )
  add_test(NAME scene_editor.editor_controller_animation
    COMMAND editor_controller_animation_tests)
  set_tests_properties(scene_editor.editor_controller_animation PROPERTIES
    LABELS "scene-editor-p3-mutation")
  add_dependencies(scene_editor_tests editor_controller_animation_tests)

  add_executable(editor_controller_transaction_tests
    tests/editor/editor_controller_transaction_tests.cpp
    loader/GltfLoader.cpp
    loader/SceneCacheSerializer.cpp
    scene/SceneAssetBuilder.cpp
    scene/SceneAssetSerializer.cpp
  )
  target_compile_features(editor_controller_transaction_tests PRIVATE cxx_std_20)
  target_compile_definitions(editor_controller_transaction_tests PRIVATE
    MGIF_TEST_SOURCE_DIR="${CMAKE_CURRENT_LIST_DIR}"
  )
  target_link_libraries(editor_controller_transaction_tests PRIVATE
    desktop_scene_editor_ui
    scene_change_capture_runtime
    glm
    imgui_demo
    tinygltf
  )
  add_test(NAME scene_editor.editor_controller_transaction
    COMMAND editor_controller_transaction_tests)
  set_tests_properties(scene_editor.editor_controller_transaction PROPERTIES
    LABELS "scene-editor-p5-automation"
    DEPENDS scene_editor.editor_transaction_smoke)
  add_dependencies(scene_editor_tests editor_controller_transaction_tests)
endif()

if(BUILD_TESTING AND NOT ANDROID)
  add_executable(editor_ui_probe_tests
    tests/editor/editor_ui_probe_tests.cpp)
  target_compile_features(editor_ui_probe_tests PRIVATE cxx_std_20)
  target_include_directories(editor_ui_probe_tests PRIVATE
    "${CMAKE_CURRENT_LIST_DIR}"
    "${imgui_SOURCE_DIR}")
  target_link_libraries(editor_ui_probe_tests PRIVATE
    desktop_scene_editor_ui
    imgui_demo glm)
  add_test(NAME scene_editor.ui_probe
    COMMAND editor_ui_probe_tests)
  set_tests_properties(scene_editor.ui_probe PROPERTIES
    LABELS "scene-editor-p8-ui")

  add_executable(editor_ui_semantic_tests
    tests/editor/editor_ui_semantic_tests.cpp)
  target_compile_features(editor_ui_semantic_tests PRIVATE cxx_std_20)
  target_include_directories(editor_ui_semantic_tests PRIVATE
    "${CMAKE_CURRENT_LIST_DIR}"
    "${imgui_SOURCE_DIR}")
  target_link_libraries(editor_ui_semantic_tests PRIVATE
    desktop_scene_editor_ui
    imgui_demo)
  add_test(NAME scene_editor.ui_semantic
    COMMAND editor_ui_semantic_tests)
  set_tests_properties(scene_editor.ui_semantic PROPERTIES
    LABELS "scene-editor-p8-ui")
  add_dependencies(scene_editor_tests
    editor_ui_probe_tests
    editor_ui_semantic_tests)
endif()

if(BUILD_TESTING AND NOT ANDROID)
  add_executable(editor_ui_state_tests
    tests/editor/editor_ui_state_tests.cpp)
  target_compile_features(editor_ui_state_tests PRIVATE cxx_std_20)
  target_include_directories(editor_ui_state_tests PRIVATE
    "${CMAKE_CURRENT_LIST_DIR}")
  target_link_libraries(editor_ui_state_tests PRIVATE editor_test_driver)
  add_test(NAME scene_editor.ui_state
    COMMAND editor_ui_state_tests)
  set_tests_properties(scene_editor.ui_state PROPERTIES
    LABELS "scene-editor-p8-ui")

  add_executable(editor_ui_action_tests
    tests/editor/editor_ui_action_tests.cpp)
  target_compile_features(editor_ui_action_tests PRIVATE cxx_std_20)
  target_include_directories(editor_ui_action_tests PRIVATE
    "${CMAKE_CURRENT_LIST_DIR}")
  target_link_libraries(editor_ui_action_tests PRIVATE editor_test_driver)
  add_test(NAME scene_editor.ui_action
    COMMAND editor_ui_action_tests)
  set_tests_properties(scene_editor.ui_action PROPERTIES
    LABELS "scene-editor-p8-ui")

  add_executable(editor_ui_replay_tests
    tests/editor/editor_ui_replay_tests.cpp
    loader/GltfLoader.cpp
    loader/SceneCacheSerializer.cpp
    scene/SceneAssetBuilder.cpp
    scene/SceneAssetSerializer.cpp)
  target_compile_features(editor_ui_replay_tests PRIVATE cxx_std_20)
  target_compile_definitions(editor_ui_replay_tests PRIVATE
    MGIF_TEST_SOURCE_DIR="${CMAKE_CURRENT_LIST_DIR}")
  target_include_directories(editor_ui_replay_tests PRIVATE
    "${CMAKE_CURRENT_LIST_DIR}")
  target_link_libraries(editor_ui_replay_tests PRIVATE
    editor_test_driver
    scene_change_capture_runtime
    glm
    imgui_demo
    tinygltf)
  add_test(NAME scene_editor.ui_replay
    COMMAND editor_ui_replay_tests
      --output
      "${CMAKE_CURRENT_BINARY_DIR}/editor-ui-replay-evidence-$<CONFIG>.json")
  set_tests_properties(scene_editor.ui_replay PROPERTIES
    LABELS "scene-editor-p8-ui")

  add_test(NAME scene_editor.minimal_app_authority_contract
    COMMAND "${Python3_EXECUTABLE}" -B
      "${CMAKE_CURRENT_LIST_DIR}/tests/editor/minimal_app_editor_authority_contract_tests.py"
      -q)
  set_tests_properties(scene_editor.minimal_app_authority_contract PROPERTIES
    LABELS "scene-editor-p8-ui")

  add_dependencies(scene_editor_tests
    editor_ui_state_tests
    editor_ui_action_tests
    editor_ui_replay_tests)
endif()

if(BUILD_TESTING AND NOT ANDROID)
  # P7 overlay tests are attached after the frozen scene-editor test target is
  # declared. This keeps the target census deterministic while still making
  # `scene_editor_tests` build every overlay contract executable.
  add_executable(transform_gizmo_cpu_tests
    tests/editor/transform_gizmo_cpu_tests.cpp)
  target_link_libraries(transform_gizmo_cpu_tests PRIVATE
    editor_gizmo_overlay_runtime)
  add_test(NAME scene_editor.transform_gizmo_cpu
    COMMAND transform_gizmo_cpu_tests)

  add_executable(grid_model_tests
    tests/editor/grid_model_tests.cpp)
  target_link_libraries(grid_model_tests PRIVATE
    editor_gizmo_overlay_runtime)
  add_test(NAME scene_editor.grid_model COMMAND grid_model_tests)

  add_executable(editor_overlay_rhi_tests
    tests/editor/editor_overlay_rhi_tests.cpp)
  target_compile_definitions(editor_overlay_rhi_tests PRIVATE
    MGIF_TEST_SOURCE_DIR="${CMAKE_CURRENT_LIST_DIR}")
  target_link_libraries(editor_overlay_rhi_tests PRIVATE
    editor_overlay_rhi_runtime)
  add_test(NAME scene_editor.overlay_rhi COMMAND editor_overlay_rhi_tests)

  add_executable(editor_input_router_tests
    tests/editor/editor_input_router_tests.cpp)
  target_link_libraries(editor_input_router_tests PRIVATE
    editor_input_router_runtime)
  add_test(NAME scene_editor.input_router COMMAND editor_input_router_tests)

  set_tests_properties(scene_editor.overlay PROPERTIES
    LABELS "scene-editor-p7-overlay")
  set_tests_properties(
    scene_editor.transform_gizmo_cpu
    scene_editor.grid_model
    scene_editor.overlay_rhi
    PROPERTIES LABELS "scene-editor-p7-overlay")
  set_tests_properties(scene_editor.input_router PROPERTIES
    LABELS "scene-editor-p6-input")
  add_dependencies(scene_editor_tests
    transform_gizmo_cpu_tests
    grid_model_tests
    editor_overlay_rhi_tests
    editor_input_router_tests)
endif()

add_library(demo_core STATIC
  rhi/RHITypes.h
  rhi/RHIHandles.h
  rhi/RHIResult.h
  rhi/RHIResidency.h
  rhi/RHIDebugCounters.h
  rhi/RHIDebugValidation.h
  rhi/RHIBindlessTypes.h
  rhi/RHIStageBarrier.h
  rhi/RHIArgumentTable.h
  rhi/RHIEncoder.h
  rhi/RHICommandBuffer.h
  rhi/RHICommandAllocator.h
  rhi/RHISwapchain.h
  rhi/RHIDevice.h
  rhi/RHIFactory.h
  rhi/RHIFactory.cpp
  rhi/RHISurface.h
  rhi/RHIQueue.h
  rhi/RHIResourceLifetime.h
  rhi/RHIShaderReflection.h
  rhi/RHIPipeline.h
  # vulkan backend
  rhi/vulkan/VulkanFormatUtils.h
  rhi/vulkan/VulkanResourceConversions.h
  rhi/vulkan/VulkanShaderConversions.h
  rhi/vulkan/VulkanBarrierConversions.h
  rhi/vulkan/VulkanPipelineConversions.h
  rhi/vulkan/VulkanQueueConversions.h
  rhi/vulkan/VulkanSurface.h
  rhi/vulkan/VulkanSurface.cpp
  rhi/vulkan/VulkanDevice.h
  rhi/vulkan/VulkanDevice.cpp
  rhi/vulkan/VulkanCommandBuffer.h
  rhi/vulkan/VulkanCommandBuffer.cpp
  rhi/vulkan/VulkanCommandAllocator.h
  rhi/vulkan/VulkanCommandAllocator.cpp
  rhi/vulkan/VulkanQueue.h
  rhi/vulkan/VulkanQueue.cpp
  rhi/vulkan/VulkanResourceTable.h
  rhi/vulkan/VulkanResourceTable.cpp
  rhi/vulkan/VulkanPipelines.h
  rhi/vulkan/VulkanPipelines.cpp
  rhi/vulkan/VulkanSwapchain.h
  rhi/vulkan/VulkanSwapchain.cpp
  render/RenderDevice.cpp
  render/RenderDevice.h
  render/ImGuiRhiTypes.h
  render/ImGuiRhiRenderer.h
  render/ImGuiRhiRenderer.cpp
  render/RenderTypes.h
  render/RendererFacade.h
  render/RendererFacade.cpp
  render/AsyncLoadingCoordinator.h  
  render/AsyncLoadingCoordinator.cpp
  render/GPUDrivenRenderer.h
  render/GPUDrivenRenderer.cpp
  render/MeshSDFQuality.h
  render/GPUDrivenLightResources.h
  render/GPUDrivenLightResources.cpp
  render/GPUSceneRegistry.h
  render/GPUSceneRegistry.cpp
  render/DDGIProbeVolume.h
  render/DDGIProbeVolume.cpp
  render/HiZDepthPyramid.h
  render/HiZDepthPyramid.cpp
  render/GPUBatchBuilder.h
  render/GPUBatchBuilder.cpp
  render/MeshletConverter.h
  render/MeshletConverter.cpp
  render/GPUMeshletBuffer.h
  render/GPUMeshletBuffer.cpp
  render/ImguiAxis.cpp
  render/ImguiAxis.h
  render/passes/GPUDrivenDepthPyramidPass.h
  render/passes/GPUDrivenDepthPyramidPass.cpp
  render/passes/GPUDrivenCullingPass.h
  render/passes/GPUDrivenCullingPass.cpp
  render/passes/GPUDrivenDepthPrepass.h
  render/passes/GPUDrivenDepthPrepass.cpp
  render/passes/GPUDrivenForwardPass.h
  render/passes/GPUDrivenForwardPass.cpp
  render/passes/GPUDrivenGBufferPass.h
  render/passes/GPUDrivenGBufferPass.cpp
  render/passes/GPUDrivenLightPass.h
  render/passes/GPUDrivenLightPass.cpp
  render/passes/GPUDrivenClusteredLightCullingPass.h
  render/passes/GPUDrivenClusteredLightCullingPass.cpp
  render/passes/GPUDrivenAOPass.h
  render/passes/GPUDrivenAOPass.cpp
  render/passes/GlobalSDFPass.h
  render/passes/GlobalSDFPass.cpp
  render/passes/DDGIRayTracePass.h
  render/passes/DDGIRayTracePass.cpp
  render/passes/DDGIProbeUpdatePass.h
  render/passes/DDGIProbeUpdatePass.cpp
  render/passes/FlaxDDGIPass.h
  render/passes/FlaxDDGIPass.cpp
  render/passes/DDGIDebugPass.h
  render/passes/DDGIDebugPass.cpp
  render/passes/GlobalSurfaceAtlasPass.h
  render/passes/GlobalSurfaceAtlasPass.cpp
  render/passes/SurfaceAtlasRasterPass.h
  render/passes/SurfaceAtlasRasterPass.cpp
  render/FlaxDDGIResources.h
  render/FlaxDDGIResources.cpp
  render/FlaxGIDebugModel.h
  render/GlobalSurfaceAtlasObject.h
  render/GlobalSurfaceAtlasTilePacker.h
  render/GlobalSurfaceAtlasData.h
  render/passes/GPUDrivenSSRPass.h
  render/passes/GPUDrivenSSRPass.cpp
  render/passes/GPUDrivenShadowAtlasPass.h
  render/passes/GPUDrivenShadowAtlasPass.cpp
  render/passes/GPUDrivenSkyboxPass.h
  render/passes/GPUDrivenSkyboxPass.cpp
  render/passes/GPUDrivenBloomPrefilterPass.h
  render/passes/GPUDrivenBloomPrefilterPass.cpp
  render/passes/GPUDrivenBloomDownsamplePass.h
  render/passes/GPUDrivenBloomDownsamplePass.cpp
  render/passes/GPUDrivenFinalColorPass.h
  render/passes/GPUDrivenFinalColorPass.cpp
  render/passes/GPUDrivenVelocityPass.h
  render/passes/GPUDrivenVelocityPass.cpp
  render/passes/GPUDrivenTAAResolvePass.h
  render/passes/GPUDrivenTAAResolvePass.cpp
  render/passes/GPUDrivenLightCullingPass.h
  render/passes/GPUDrivenLightCullingPass.cpp
  render/passes/GPUDrivenCSMShadowPass.h
  render/passes/GPUDrivenCSMShadowPass.cpp
  render/passes/GPUDrivenDebugPass.h
  render/passes/GPUDrivenDebugPass.cpp
  render/passes/GPUDrivenPresentPass.h
  render/passes/GPUDrivenPresentPass.cpp
  render/passes/GPUDrivenImguiPass.h
  render/passes/GPUDrivenImguiPass.cpp
  render/passes/GPUDrivenVisibilitySortPass.h
  render/passes/GPUDrivenVisibilitySortPass.cpp
  render/ArgumentTables.h
  render/Pass.h
  render/TransientAllocator.cpp
  render/TransientAllocator.h
  render/PassExecutor.cpp
  render/PassExecutor.h
  render/FrameScheduler.h
  render/FrameScheduler.cpp
  render/UploadManager.h
  render/UploadManager.cpp
  render/SceneResources.cpp
  render/SceneResources.h
  render/LightResources.cpp
  render/LightResources.h
  render/IBLResources.cpp
  render/IBLResources.h
  render/DrawStream.h
  render/DrawStreamDecoder.cpp
  render/DrawStreamDecoder.h
  render/DrawStreamRecorder.cpp
  render/DrawStreamRecorder.h
  render/DrawStreamWriter.cpp
  render/DrawStreamWriter.h
  render/BatchUploadContext.cpp
  render/BatchUploadContext.h
  render/MipmapGenerator.cpp
  render/MipmapGenerator.h
  render/UploadUtils.cpp
  render/UploadUtils.h
  common/Common.h
  common/FrameDeferredValue.h
  common/Handles.h
  common/HandlePool.h
  common/ProfilerMarkers.h
  loader/GltfLoader.h
  loader/GltfLoader.cpp
  loader/Ktx2Loader.h
  loader/Ktx2Loader.cpp
  loader/SDFLoader.h
  loader/SDFLoader.cpp
  loader/SceneCacheSerializer.h
  loader/SceneCacheSerializer.cpp
  scene/SceneAsset.h
  scene/SceneCamera.h
  scene/SceneAssetView.h
  scene/SceneAssetBuilder.h
  scene/SceneAssetBuilder.cpp
  scene/SceneAssetSerializer.h
  scene/SceneAssetSerializer.cpp
  scene/SceneLoadJob.h
  scene/SceneLoadJobGraph.h
  scene/SceneJobSystem.h
  scene/SceneJobSystem.cpp
  scene/ParallelSceneLoader.h
  scene/ParallelSceneLoader.cpp
  scene/SceneUploadPlan.h
  scene/SceneUploadPlanner.h
  scene/SceneUploadPlanner.cpp
  scene/TransformSystem.h
  scene/TransformSystem.cpp
  render/MeshPool.h
  render/MeshPool.cpp
  render/Camera.h
  render/Camera.cpp
  render/SceneViewResolver.h
  render/SceneViewResolver.cpp
  render/CSMShadowResources.h
  render/CSMShadowResources.cpp
)

if(NOT ANDROID)
  target_sources(demo_core PRIVATE
    app/MinimalLatestApp.h
    rhi/metal/MetalDevice.h
    rhi/metal/MetalDevice.cpp
    rhi/metal/MetalSwapchain.h
    rhi/metal/MetalSwapchain.cpp
    rhi/metal/MetalPipelines.h
    rhi/metal/MetalPipelines.cpp
  )
endif()

if(APPLE)
  enable_language(OBJCXX)
  target_sources(demo_core PRIVATE
    rhi/metal/Metal4Availability.h
    rhi/metal/Metal4Availability.mm
    rhi/metal/Metal4Contract.h
    rhi/metal/Metal4Contract.mm
  )
  find_library(DEMO_METAL_FRAMEWORK Metal REQUIRED)
  find_library(DEMO_FOUNDATION_FRAMEWORK Foundation REQUIRED)
  target_link_libraries(demo_core PUBLIC
    "${DEMO_METAL_FRAMEWORK}"
    "${DEMO_FOUNDATION_FRAMEWORK}"
  )
endif()

if(WIN32 AND DEMO_ENABLE_D3D12)
  target_sources(demo_core PRIVATE
    rhi/d3d12/D3D12Device.h
    rhi/d3d12/D3D12Device.cpp
    rhi/d3d12/D3D12Surface.h
    rhi/d3d12/D3D12Surface.cpp
    rhi/d3d12/D3D12Swapchain.h
    rhi/d3d12/D3D12Swapchain.cpp
    rhi/d3d12/D3D12Pipelines.h
    rhi/d3d12/D3D12Pipelines.cpp
    rhi/d3d12/D3D12PipelineDevice.cpp
    rhi/d3d12/D3D12CommandBuffer.h
    rhi/d3d12/D3D12CommandBuffer.cpp
    rhi/d3d12/D3D12CommandAllocator.h
    rhi/d3d12/D3D12CommandAllocator.cpp
    rhi/d3d12/D3D12Queue.h
    rhi/d3d12/D3D12Queue.cpp
  )
  target_compile_definitions(demo_core PRIVATE
    DEMO_SLANGC_PATH="${Slang_SLANGC_EXECUTABLE}"
    DEMO_DXC_PATH="${DEMO_DXC_SPIRV_EXECUTABLE}"
    DEMO_SHADER_SOURCE_DIR="${CMAKE_CURRENT_LIST_DIR}/shaders"
    DEMO_SHADER_AUTOGEN_DIR="${SHADER_OUTPUT_DIR}"
  )
  target_compile_definitions(demo_core PUBLIC DEMO_HAS_D3D12=1)
endif()
if(ANDROID)
  add_library(native_app_glue STATIC
    ${ANDROID_NDK}/sources/android/native_app_glue/android_native_app_glue.c
  )
  target_include_directories(native_app_glue PUBLIC
    ${ANDROID_NDK}/sources/android/native_app_glue
  )
endif()

target_compile_definitions(demo_core PUBLIC VK_NO_PROTOTYPES)
if(USE_SLANG)
  target_compile_definitions(demo_core PUBLIC USE_SLANG)
endif()
if(DEMO_HAS_DXC_FINAL_SPIRV)
  target_compile_definitions(demo_core PRIVATE DEMO_HAS_DXC_FINAL_SPIRV=1)
endif()
if(VK_SEMAPHORE_DEBUG)
  target_compile_definitions(demo_core PUBLIC NVVK_SEMAPHORE_DEBUG=1)
endif()

target_include_directories(demo_core
  PUBLIC
    ${CMAKE_CURRENT_LIST_DIR}
    ${CMAKE_CURRENT_BINARY_DIR}
    ${volk_SOURCE_DIR}
    ${imgui_SOURCE_DIR}
    ${imgui_SOURCE_DIR}/backends
    ${meshoptimizer_SOURCE_DIR}/src
    ${vulkanmemoryallocator_SOURCE_DIR}/include
)

set(VKDEMO_NVTX_INCLUDE_DIR "" CACHE PATH "Optional NVTX3 include directory")
if(NOT VKDEMO_NVTX_INCLUDE_DIR)
  set(VKDEMO_NVTX_CANDIDATES
    "C:/Program Files/NVIDIA Corporation/Nsight Systems 2025.5.1/target-windows-x64/nvtx/include"
    "C:/Program Files/NVIDIA Corporation/Nsight Graphics 2025.5.0/target/Resources/NVTX3/include"
  )
  foreach(VKDEMO_NVTX_CANDIDATE IN LISTS VKDEMO_NVTX_CANDIDATES)
    if(EXISTS "${VKDEMO_NVTX_CANDIDATE}/nvtx3/nvToolsExt.h")
      set(VKDEMO_NVTX_INCLUDE_DIR "${VKDEMO_NVTX_CANDIDATE}" CACHE PATH "Optional NVTX3 include directory" FORCE)
      break()
    endif()
  endforeach()
endif()

if(VKDEMO_NVTX_INCLUDE_DIR AND EXISTS "${VKDEMO_NVTX_INCLUDE_DIR}/nvtx3/nvToolsExt.h")
  message(STATUS "NVTX markers: enabled (${VKDEMO_NVTX_INCLUDE_DIR})")
  target_include_directories(demo_core PUBLIC "${VKDEMO_NVTX_INCLUDE_DIR}")
  target_compile_definitions(demo_core PUBLIC VKDEMO_HAS_NVTX=1)
else()
  message(STATUS "NVTX markers: disabled (set VKDEMO_NVTX_INCLUDE_DIR to enable)")
endif()

   
target_sources(demo_core PRIVATE
  ${vulkanmemoryallocator_SOURCE_DIR}/include/vk_mem_alloc.h
  ${STB_IMAGE_PATH}
  ${GENERATED_SHADER_SLANG_HEADERS}
  ${GENERATED_SHADER_SPV_BINARIES}
  ${GENERATED_SHADER_GLSL_HEADERS}
  ${SHADER_H_FILES}
)

target_link_libraries(demo_core
  PUBLIC
    scene_editor_core
    volk
    imgui_demo
    glm
    tinygltf
    meshoptimizer
    Vulkan::Vulkan
)

if(NOT ANDROID)
  target_compile_definitions(demo_core PUBLIC
    MGIF_ENABLE_SCENE_TRANSACTIONS=1)
  target_link_libraries(demo_core PUBLIC
    desktop_scene_editor_ui
    scene_change_capture_runtime
    scene_runtime_mapping_contract
    renderer_scene_transaction)
  target_link_libraries(demo_core PRIVATE editor_overlay_rhi_runtime)
endif()

if(ANDROID)
  target_link_libraries(demo_core
    PRIVATE scene_runtime_mapping_runtime
    PUBLIC android log native_app_glue)
else()
  target_link_libraries(demo_core PUBLIC glfw)
endif()

if(UNIX AND NOT APPLE AND NOT ANDROID)
  target_link_libraries(demo_core PUBLIC ${X11_LIBRARIES} Threads::Threads)
endif()

if(WIN32)
  target_link_libraries(demo_core PUBLIC winmm.lib)
  if(DEMO_ENABLE_D3D12)
    target_link_libraries(demo_core PUBLIC d3d12.lib dxgi.lib dxguid.lib)
  endif()
endif()

if(ANDROID)
  add_library(DemoAndroid SHARED app/AndroidNativeApp.cpp)
  target_link_libraries(DemoAndroid PRIVATE demo_core)
else()
  add_executable(Demo
    app/MinimalLatestApp.cpp
    app/editor/EditorTransactionPath.cpp
  )
  target_link_libraries(Demo PRIVATE demo_core)
endif()
if(BUILD_TESTING AND NOT ANDROID)
  add_executable(editor_ui_live_replay_acceptance
    app/editor/EditorUiLiveReplayMain.cpp
    app/editor/EditorTransactionPath.cpp)
  target_compile_features(editor_ui_live_replay_acceptance PRIVATE cxx_std_20)
  target_compile_definitions(editor_ui_live_replay_acceptance PRIVATE
    MGIF_EDITOR_LIVE_REPLAY=1
    MGIF_SOURCE_DIR="${CMAKE_CURRENT_LIST_DIR}")
  target_link_libraries(editor_ui_live_replay_acceptance PRIVATE
    demo_core
    editor_test_driver)
  set_target_properties(editor_ui_live_replay_acceptance PROPERTIES
    OUTPUT_NAME editor_ui_live_replay)
  add_custom_target(editor_ui_live_replay
    DEPENDS editor_ui_live_replay_acceptance)

  if(WIN32)
  add_library(editor_ui_os_replay_host STATIC
    app/editor/EditorUiOsReplayHost.h
    app/editor/EditorUiOsReplayHost.cpp)
  target_compile_features(editor_ui_os_replay_host PUBLIC cxx_std_20)
  target_include_directories(editor_ui_os_replay_host PUBLIC
    "${CMAKE_CURRENT_LIST_DIR}")
  target_link_libraries(editor_ui_os_replay_host PUBLIC
    demo_core
    editor_ui_os_replay_runtime
    editor_ui_os_replay_cli)

  add_executable(editor_ui_os_replay_acceptance
    app/editor/EditorUiOsReplayMain.cpp
    app/editor/EditorTransactionPath.cpp)
  target_compile_features(editor_ui_os_replay_acceptance PRIVATE cxx_std_20)
  target_compile_definitions(editor_ui_os_replay_acceptance PRIVATE
    MGIF_EDITOR_OS_REPLAY=1
    MGIF_SOURCE_DIR="${CMAKE_CURRENT_LIST_DIR}")
  target_link_libraries(editor_ui_os_replay_acceptance PRIVATE
    demo_core
    editor_ui_os_replay_plan
    editor_ui_os_replay_runtime
    editor_ui_os_replay_cli
    editor_ui_os_replay_host)
  set_target_properties(editor_ui_os_replay_acceptance PROPERTIES
    OUTPUT_NAME editor_ui_os_replay)
  add_custom_target(editor_ui_os_replay
    DEPENDS editor_ui_os_replay_acceptance)
  endif()
endif()
if(BUILD_TESTING AND NOT ANDROID)
  add_executable(editor_renderer_integration_tests
    tests/editor/editor_renderer_integration_tests.cpp
  )
  target_link_libraries(editor_renderer_integration_tests PRIVATE demo_core)
  add_test(NAME scene_editor.renderer_integration
    COMMAND editor_renderer_integration_tests)
  add_dependencies(scene_editor_tests editor_renderer_integration_tests)

  add_executable(flax_gi_debug_model_tests
    tests/flax_gi_debug_model_tests.cpp
  )
  target_include_directories(flax_gi_debug_model_tests PRIVATE "${CMAKE_CURRENT_LIST_DIR}")
  add_test(NAME flax_gi_debug_model COMMAND flax_gi_debug_model_tests)

  add_executable(scene_import_defaults_tests
    tests/scene_import_defaults_tests.cpp
  )
  target_link_libraries(scene_import_defaults_tests PRIVATE demo_core)
  add_test(NAME scene_import_defaults COMMAND scene_import_defaults_tests)

  add_executable(imgui_rhi_contract_tests
    tests/imgui_rhi_contract_tests.cpp
  )
  target_include_directories(imgui_rhi_contract_tests PRIVATE "${CMAKE_CURRENT_LIST_DIR}")
  add_test(NAME imgui_rhi_contract COMMAND imgui_rhi_contract_tests)

  add_executable(mesh_sdf_quality_tests
    tests/mesh_sdf_quality_tests.cpp
  )
  target_link_libraries(mesh_sdf_quality_tests PRIVATE glm)
  target_include_directories(mesh_sdf_quality_tests PRIVATE "${CMAKE_CURRENT_LIST_DIR}")
  add_test(NAME mesh_sdf_quality COMMAND mesh_sdf_quality_tests)

  add_executable(gpu_scene_registry_model_tests
    tests/gpu_scene_registry_model_tests.cpp
  )
  target_link_libraries(gpu_scene_registry_model_tests PRIVATE demo_core)
  add_test(NAME gpu_scene_registry_model COMMAND gpu_scene_registry_model_tests)

  add_executable(csm_shadow_stability_tests
    tests/csm_shadow_stability_tests.cpp
  )
  target_link_libraries(csm_shadow_stability_tests PRIVATE demo_core)
  add_test(NAME csm_shadow_stability COMMAND csm_shadow_stability_tests)

  add_executable(gpu_visibility_depth_model_tests
    tests/gpu_visibility_depth_model_tests.cpp
  )
  add_test(NAME gpu_visibility_depth_model COMMAND gpu_visibility_depth_model_tests)

  add_executable(rhi_public_contract_tests
    tests/rhi_public_contract_tests.cpp
  )
  target_include_directories(rhi_public_contract_tests PRIVATE "${CMAKE_CURRENT_LIST_DIR}")
  add_test(NAME rhi_public_contract COMMAND rhi_public_contract_tests)

  add_executable(vulkan_conversion_tests
    tests/vulkan_conversion_tests.cpp
  )
  target_link_libraries(vulkan_conversion_tests PRIVATE demo_core)
  add_test(NAME vulkan_conversion COMMAND vulkan_conversion_tests)

  add_executable(vulkan_command_buffer_handle_tests
    tests/rhi/vulkan_command_buffer_handle_tests.cpp
  )
  target_link_libraries(vulkan_command_buffer_handle_tests PRIVATE demo_core)
  add_test(NAME vulkan_command_buffer_handles COMMAND vulkan_command_buffer_handle_tests)

  add_executable(vulkan_allocation_failure_tests
    tests/rhi/vulkan_allocation_failure_tests.cpp
  )
  target_link_libraries(vulkan_allocation_failure_tests PRIVATE demo_core)
  add_test(NAME vulkan_allocation_failures COMMAND vulkan_allocation_failure_tests)

  add_executable(rhi_backend_factory_tests
    tests/rhi_backend_factory_tests.cpp
  )
  target_link_libraries(rhi_backend_factory_tests PRIVATE demo_core)
  add_test(NAME rhi_backend_factory COMMAND rhi_backend_factory_tests)

  add_dependencies(scene_editor_p0_scaffold
    flax_gi_debug_model_tests
    scene_import_defaults_tests
    imgui_rhi_contract_tests
    mesh_sdf_quality_tests
    gpu_scene_registry_model_tests
    csm_shadow_stability_tests
    gpu_visibility_depth_model_tests
    rhi_public_contract_tests
    vulkan_conversion_tests
    vulkan_command_buffer_handle_tests
    vulkan_allocation_failure_tests
    rhi_backend_factory_tests
  )

  if(WIN32 AND DEMO_ENABLE_D3D12)
    add_executable(d3d12_device_smoke_tests
      tests/d3d12_device_smoke_tests.cpp
    )
    target_link_libraries(d3d12_device_smoke_tests PRIVATE demo_core)
    add_test(NAME d3d12_device_smoke COMMAND d3d12_device_smoke_tests)
    set_tests_properties(d3d12_device_smoke PROPERTIES SKIP_RETURN_CODE 77)

    add_executable(d3d12_clear_present_tests
      tests/d3d12_clear_present_tests.cpp
    )
    target_link_libraries(d3d12_clear_present_tests PRIVATE demo_core)
    add_test(NAME d3d12_clear_present COMMAND d3d12_clear_present_tests)

    add_dependencies(scene_editor_p0_scaffold
      d3d12_device_smoke_tests
      d3d12_clear_present_tests
    )
  endif()
endif()

if(NOT ANDROID)
add_executable(texture_converter  
  tools/texture_converter.cpp
)

set(NVTT_ROOT "C:/Program Files/NVIDIA Corporation/NVIDIA Texture Tools" CACHE PATH "Path to the NVIDIA Texture Tools install root")
set(NVTT_INCLUDE_DIR "${NVTT_ROOT}/include")
set(NVTT_LIBRARY "${NVTT_ROOT}/lib/x64-v142/nvtt30205.lib")    
set(NVTT_DLL "${NVTT_ROOT}/nvtt30205.dll")

if(NOT EXISTS "${NVTT_INCLUDE_DIR}/nvtt/nvtt.h")
  message(FATAL_ERROR "NVTT headers were not found under ${NVTT_INCLUDE_DIR}")
endif()

if(NOT EXISTS "${NVTT_LIBRARY}")
  message(FATAL_ERROR "NVTT import library was not found at ${NVTT_LIBRARY}")
endif()
if(NOT EXISTS "${NVTT_DLL}")
  message(FATAL_ERROR "NVTT runtime DLL was not found at ${NVTT_DLL}")
endif()

target_compile_definitions(texture_converter PRIVATE NVTT_SHARED=1)
target_include_directories(texture_converter PRIVATE
  ${CMAKE_CURRENT_BINARY_DIR}
  ${volk_SOURCE_DIR}
  ${NVTT_INCLUDE_DIR}
)
target_link_libraries(texture_converter PRIVATE "${NVTT_LIBRARY}")

add_custom_command(TARGET texture_converter POST_BUILD
  COMMAND ${CMAKE_COMMAND} -E copy_if_different
          "${NVTT_DLL}"
          "$<TARGET_FILE_DIR:texture_converter>/nvtt30205.dll"
)

# --- Offline Mesh SDF baker (DDGI Wave D1-1) ---
# Pure-CPU asset tool: no RHI/Vulkan/NVTT dependency.
# Not a Demo dependency — built separately via the sdf_baker target.
add_executable(sdf_baker
  tools/sdf_baker/main.cpp
  tools/sdf_baker/SDFBaker.cpp
  tools/sdf_baker/SDFBaker.h
)
target_link_libraries(sdf_baker PRIVATE glm tinygltf)

set(COMPRESS_TEXTURES_SCRIPT_TEMPLATE [=[@echo off
setlocal
set "SCRIPT_DIR=%~dp0"
if "%SCRIPT_DIR:~-1%"=="\" set "SCRIPT_DIR=%SCRIPT_DIR:~0,-1%"
set "CONVERTER=%SCRIPT_DIR%\..\texture_converter.exe"
if not exist "%CONVERTER%" (
  echo texture_converter.exe was not found next to the Demo executable.
  exit /b 1
)
"%CONVERTER%" "%SCRIPT_DIR%" "%SCRIPT_DIR%"
]=])
string(CONFIGURE "${COMPRESS_TEXTURES_SCRIPT_TEMPLATE}" COMPRESS_TEXTURES_SCRIPT @ONLY)
set(COMPRESS_RESOURCES_BC7_SCRIPT_SOURCE "${CMAKE_CURRENT_LIST_DIR}/tools/compress_resources_bc7_ktx2.cmd")
set(COMPRESS_RESOURCES_ASTC_SCRIPT_SOURCE "${CMAKE_CURRENT_LIST_DIR}/tools/compress_resources_astc_ktx2.cmd")
set(CONVERT_DDS_BC7_SCRIPT_SOURCE "${CMAKE_CURRENT_LIST_DIR}/tools/convert_dds_to_bc7_ktx2.cmd")

#====================================================================================
# RESOURCE PREP - Copy resources and generate a manual texture conversion script
#====================================================================================
set(DEMO_RESOURCES_SOURCE_DIR "${CMAKE_CURRENT_LIST_DIR}/resources")
set(DEMO_RESOURCES_OUTPUT_DIR "$<TARGET_FILE_DIR:Demo>/resources")

file(GENERATE
  OUTPUT "${CMAKE_CURRENT_BINARY_DIR}/compress_textures_$<CONFIG>.cmd"
  CONTENT "${COMPRESS_TEXTURES_SCRIPT}"
)

add_custom_target(copy_demo_resources
  COMMAND ${CMAKE_COMMAND} -E make_directory "${DEMO_RESOURCES_OUTPUT_DIR}"
  COMMAND ${CMAKE_COMMAND} -E copy_directory "${DEMO_RESOURCES_SOURCE_DIR}" "${DEMO_RESOURCES_OUTPUT_DIR}"
  COMMAND ${CMAKE_COMMAND} -E copy_if_different
          "${CMAKE_CURRENT_BINARY_DIR}/compress_textures_$<CONFIG>.cmd"
          "${DEMO_RESOURCES_OUTPUT_DIR}/compress_textures.cmd"
  COMMAND ${CMAKE_COMMAND} -E copy_if_different
          "${COMPRESS_RESOURCES_BC7_SCRIPT_SOURCE}"
          "$<TARGET_FILE_DIR:Demo>/compress_resources_bc7_ktx2.cmd"
  COMMAND ${CMAKE_COMMAND} -E copy_if_different
          "${COMPRESS_RESOURCES_ASTC_SCRIPT_SOURCE}"
          "$<TARGET_FILE_DIR:Demo>/compress_resources_astc_ktx2.cmd"
  COMMAND ${CMAKE_COMMAND} -E copy_if_different
          "${CONVERT_DDS_BC7_SCRIPT_SOURCE}"
          "$<TARGET_FILE_DIR:Demo>/convert_dds_to_bc7_ktx2.cmd"
  COMMENT "Copying resources to build directory..."
)

add_dependencies(Demo texture_converter copy_demo_resources)

# --- RHI Boundary Guard (VER-01) ---
find_package(Python3 REQUIRED COMPONENTS Interpreter)

add_custom_target(check_rhi_boundary
  COMMAND "${Python3_EXECUTABLE}"
          "${CMAKE_CURRENT_LIST_DIR}/tools/check_rhi_boundary.py"
          "--root" "${CMAKE_CURRENT_LIST_DIR}"
          "--baseline" "${CMAKE_CURRENT_LIST_DIR}/.rhi-boundary-baseline.json"
          "--allowlist" "${CMAKE_CURRENT_LIST_DIR}/.rhi-boundary-allow"
  WORKING_DIRECTORY "${CMAKE_CURRENT_LIST_DIR}"
  COMMENT "Running RHI boundary guard..."
  VERBATIM
)

# --- D-11: Promote boundary guard to default build dependency (Phase 7 freeze) ---
# check_rhi_boundary runs on every 'cmake --build ... --target Demo' invocation.
# Disable with -DDEMO_SKIP_BOUNDARY_CHECK=ON for inner-loop speed.
# SEQUENCING: This wiring is safe only after .rhi-boundary-baseline.json is zeroed
#             (Phase 7 Plan 02). Do not enable earlier — wedges builds during Phases 2-6.
option(DEMO_SKIP_BOUNDARY_CHECK "Skip RHI boundary guard during normal builds (for inner-loop speed; not for acceptance gate)" OFF)
if(NOT DEMO_SKIP_BOUNDARY_CHECK)
  add_dependencies(Demo check_rhi_boundary)
endif()
if(TARGET editor_ui_os_replay_acceptance)
  add_dependencies(editor_ui_os_replay_acceptance
    texture_converter
    copy_demo_resources
    check_rhi_boundary)
endif()
endif()

# P6 desktop ownership is appended after every frozen custom-command line so
# the existing dependency-effect identity remains byte-position stable.
if(NOT ANDROID)
  target_sources(desktop_scene_editor_ui PRIVATE
    app/editor/SceneSelection.h
    app/editor/SceneSelection.cpp
    app/editor/MaterialClipboardCodec.h
    app/editor/MaterialClipboardCodec.cpp
    app/editor/ImGuiClipboardAdapter.h
    app/editor/ImGuiClipboardAdapter.cpp)
  add_library(editor_picking_runtime STATIC
    scene/editor/include/mgif/scene_editor/SceneRenderOccurrenceMapping.h
    scene/editor/SceneRenderOccurrenceMapping.cpp
    render/editor/ScenePicking.h
    render/editor/ScenePicking.cpp
    render/passes/EditorPickingPass.h
    render/passes/EditorPickingPass.cpp)
  target_include_directories(editor_picking_runtime PUBLIC
    "${CMAKE_CURRENT_LIST_DIR}"
    "${CMAKE_CURRENT_LIST_DIR}/scene/editor/include")
  target_compile_features(editor_picking_runtime PUBLIC cxx_std_20)
  target_link_libraries(demo_core PRIVATE editor_picking_runtime)
  if(BUILD_TESTING)
    add_executable(editor_selection_picking_integration_tests
      tests/editor/editor_selection_picking_integration_tests.cpp)
    target_compile_definitions(editor_selection_picking_integration_tests PRIVATE
      MGIF_TEST_SOURCE_DIR="${CMAKE_CURRENT_LIST_DIR}")
    target_link_libraries(editor_selection_picking_integration_tests PRIVATE
      demo_core editor_picking_runtime editor_input_router_runtime)
    add_test(NAME scene_editor.selection_picking_integration
      COMMAND editor_selection_picking_integration_tests)
    add_dependencies(scene_editor_tests
      editor_selection_picking_integration_tests)

    add_executable(editor_gizmo_integration_tests
      tests/editor/editor_gizmo_integration_tests.cpp)
    target_compile_definitions(editor_gizmo_integration_tests PRIVATE
      MGIF_TEST_SOURCE_DIR="${CMAKE_CURRENT_LIST_DIR}")
    target_include_directories(editor_gizmo_integration_tests PRIVATE
      "${CMAKE_CURRENT_LIST_DIR}"
      "${CMAKE_CURRENT_LIST_DIR}/scene/editor/include")
    target_link_libraries(editor_gizmo_integration_tests PRIVATE
      demo_core
      editor_gizmo_overlay_runtime
      editor_overlay_rhi_runtime
      editor_input_router_runtime)
    add_test(NAME scene_editor.gizmo_integration
      COMMAND editor_gizmo_integration_tests)
    set_tests_properties(scene_editor.gizmo_integration PROPERTIES
      LABELS "scene-editor-p7-overlay")
    add_dependencies(scene_editor_tests editor_gizmo_integration_tests)
  endif()
endif()

# Targets created before the directory-wide MSVC /utf-8 option above never
# received it, because add_compile_options() only reaches targets defined after
# the call. Such a target reads UTF-8 sources through the OEM code page, and on
# a CP936 host one multi-byte comment swallows the following declaration:
# loader/GltfLoader.cpp lost readTinyGltfInt for p2_projection_cpu_tests,
# scene_editor_scene_commands_tests and scene_editor_p3_regression_tests while
# demo_core compiled the same translation unit cleanly. The repair is applied
# here, after every frozen add_custom_target/add_custom_command line anchor, so
# no approved CMake effect changes position.
get_property(MGIF_CHARSET_TARGETS DIRECTORY PROPERTY BUILDSYSTEM_TARGETS)
foreach(MGIF_CHARSET_TARGET IN LISTS MGIF_CHARSET_TARGETS)
  get_target_property(MGIF_CHARSET_ALIAS ${MGIF_CHARSET_TARGET} ALIASED_TARGET)
  get_target_property(MGIF_CHARSET_IMPORTED ${MGIF_CHARSET_TARGET} IMPORTED)
  get_target_property(MGIF_CHARSET_TYPE ${MGIF_CHARSET_TARGET} TYPE)
  get_target_property(MGIF_CHARSET_OPTIONS ${MGIF_CHARSET_TARGET} COMPILE_OPTIONS)
  if(MSVC AND NOT MGIF_CHARSET_ALIAS AND NOT MGIF_CHARSET_IMPORTED
     AND MGIF_CHARSET_TYPE MATCHES "^(STATIC_LIBRARY|SHARED_LIBRARY|OBJECT_LIBRARY|MODULE_LIBRARY|EXECUTABLE)$"
     AND NOT MGIF_CHARSET_OPTIONS MATCHES "/utf-8")
    target_compile_options(${MGIF_CHARSET_TARGET} PRIVATE /utf-8)
  endif()
endforeach()

# Keep production scene-editor implementation libraries visible for dependency
# navigation, but separate them from optional test targets in IDE generators.
foreach(MGIF_SCENE_EDITOR_RUNTIME_TARGET IN ITEMS
    scene_change_contract
    scene_editor_document
    scene_editor_core
    scene_change_capture_runtime
    scene_runtime_mapping_runtime
    scene_editor_interaction_contract
    scene_compactor_runtime
    scene_merger_runtime
    renderer_scene_transaction
    animation_playback_runtime
    editor_overlay_runtime
    editor_gizmo_overlay_runtime
    editor_overlay_rhi_runtime
    editor_input_router_runtime
    editor_picking_runtime
    desktop_scene_editor_ui)
  if(TARGET ${MGIF_SCENE_EDITOR_RUNTIME_TARGET})
    set_target_properties(${MGIF_SCENE_EDITOR_RUNTIME_TARGET} PROPERTIES
      FOLDER "Runtime/Scene Editor")
  endif()
endforeach()
if(TARGET scene_editor_dependency_preflight)
  set_target_properties(scene_editor_dependency_preflight PROPERTIES
    FOLDER "Validation")
endif()

# Resource paths are relative to the runtime directory populated by
# copy_demo_resources (Debug/Release for the Visual Studio generator).
if(TARGET Demo AND WIN32)
  set_target_properties(Demo PROPERTIES
    VS_DEBUGGER_WORKING_DIRECTORY "$<TARGET_FILE_DIR:Demo>")
endif()

# Literal top-level EOF scheduling is part of the forward-coverage contract.
# The bounded drain yields to any calls that were deferred earlier (including
# calls they enqueue) and seals the final target census only after it stabilizes.
cmake_language(DEFER CALL mgif_editor_dependency_tail_drain)
