cmake_minimum_required(VERSION 3.21)
project(Demo VERSION 1.0 LANGUAGES C CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

#====================================================================================
# SHADER AUTHORITY POLICY
#====================================================================================
#
# HLSL/Slang is the SINGLE SOURCE OF TRUTH for shader contracts in this project.
#
# - USE_SLANG=ON is REQUIRED by default (see option below)
# - GLSL compilation is maintained ONLY for compatibility testing
# - Reflection metadata from Slang is the authoritative source for:
#   * Bind layout derivation
#   * Pipeline layout derivation
#   * Resource indexing metadata
#
# - Vulkan-specific descriptor numbering in backend implementations is DERIVED
#   from Slang reflection, NOT the primary RHI contract.
#
# - Build-time verification proves shader compile + metadata generation works.
#
#====================================================================================

option(USE_SLANG "Use Slang shaders instead of GLSL" ON)
if(USE_SLANG)
  message(STATUS "Shader Authority: Slang is the single source of truth")
  message(STATUS "  - Slang compilation: ENABLED")
  message(STATUS "  - Reflection metadata: ENABLED")
  message(STATUS "  - GLSL compilation: DISABLED (compatibility mode only)")
else()
  message(WARNING "Shader Authority: GLSL mode is NOT RECOMMENDED for production")
  message(WARNING "  - Slang compilation: DISABLED")
  message(WARNING "  - Reflection metadata: DISABLED")
  message(WARNING "  - This mode is for testing only, not for authoritative shader contracts")
endif()

option(VK_SEMAPHORE_DEBUG "Enable semaphore debugging logs" OFF)

if(MSVC)
    add_compile_options(/utf-8)
endif()

include(FetchContent)

set(REPO_ROOT "${CMAKE_CURRENT_LIST_DIR}")

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

FetchContent_Declare(
  glm
  GIT_REPOSITORY https://github.com/g-truc/glm.git
  GIT_TAG master
)
FetchContent_MakeAvailable(glm)

FetchContent_Declare(
  volk
  GIT_REPOSITORY https://github.com/zeux/volk.git
  GIT_TAG master
)
FetchContent_MakeAvailable(volk)

FetchContent_Declare(
  tinygltf
  GIT_REPOSITORY https://github.com/syoyo/tinygltf.git
  GIT_TAG v2.9.3
)
set(TINYGLTF_BUILD_LOADER_EXAMPLE OFF CACHE BOOL "" FORCE)
set(TINYGLTF_BUILD_VALIDATION_EXAMPLE OFF CACHE BOOL "" FORCE)
set(TINYGLTF_BUILD_GL_EXAMPLE OFF CACHE BOOL "" FORCE)
set(TINYGLTF_BUILD_VALIDATION_TESTS OFF CACHE BOOL "" FORCE)
FetchContent_MakeAvailable(tinygltf)

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
  GIT_TAG docking
)
FetchContent_MakeAvailable(imgui)

add_library(imgui_demo STATIC
  ${imgui_SOURCE_DIR}/imgui.cpp
  ${imgui_SOURCE_DIR}/imgui.h
  ${imgui_SOURCE_DIR}/imgui_demo.cpp
  ${imgui_SOURCE_DIR}/imgui_draw.cpp
  ${imgui_SOURCE_DIR}/imgui_tables.cpp
  ${imgui_SOURCE_DIR}/imgui_widgets.cpp
  ${imgui_SOURCE_DIR}/backends/imgui_impl_vulkan.cpp
  ${imgui_SOURCE_DIR}/backends/imgui_impl_vulkan.h
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
target_include_directories(imgui_demo PUBLIC
  ${imgui_SOURCE_DIR}
  ${imgui_SOURCE_DIR}/backends
)
target_compile_definitions(imgui_demo PUBLIC IMGUI_IMPL_VULKAN_USE_VOLK)
target_link_libraries(imgui_demo PUBLIC volk)
if(ANDROID)
  target_link_libraries(imgui_demo PUBLIC android)
else()
  target_link_libraries(imgui_demo PUBLIC glfw)
endif()

set(STB_IMAGE_URL "https://raw.githubusercontent.com/nothings/stb/master/stb_image.h")
set(STB_IMAGE_PATH "${CMAKE_CURRENT_BINARY_DIR}/stb_image.h")
if(NOT EXISTS "${STB_IMAGE_PATH}")
  set(STB_IMAGE_CACHED_PATH "${REPO_ROOT}/out/build/x64-Debug/stb_image.h")
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

file(GLOB SHADER_SLANG_FILES "${CMAKE_CURRENT_LIST_DIR}/shaders/*.slang")
file(GLOB SHADER_GLSL_FILES  "${CMAKE_CURRENT_LIST_DIR}/shaders/*.glsl")
file(GLOB SHADER_H_FILES     "${CMAKE_CURRENT_LIST_DIR}/shaders/*.h")
list(FILTER SHADER_SLANG_FILES EXCLUDE REGEX "shader\\.ibl_common\\.slang$")
# Include-only sphere-march module (DDGI Wave D2-2), consumed via #include.
list(FILTER SHADER_SLANG_FILES EXCLUDE REGEX "sdf_common\\.slang$")
# Include-only DDGI octahedral/probe-math module (Wave D2-3), consumed via #include.
list(FILTER SHADER_SLANG_FILES EXCLUDE REGEX "ddgi_common\\.slang$")
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

compile_glsl(
  "${SHADER_GLSL_FILES}"
  "${SHADER_OUTPUT_DIR}"
  GENERATED_SHADER_GLSL_HEADERS
  EXTRA_FLAGS "-I${CMAKE_CURRENT_LIST_DIR}"
)

add_library(demo_core STATIC
  rhi/RHITypes.h
  rhi/RHIHandles.h
  rhi/RHIBindlessTypes.h
  rhi/RHIStageBarrier.h
  rhi/RHIArgumentTable.h
  rhi/RHIEncoder.h
  rhi/RHICommandBuffer.h
  rhi/RHICommandPool.h
  rhi/RHISynchronization.h
  rhi/RHISwapchain.h
  rhi/RHIDevice.h
  rhi/RHIFactory.h
  rhi/RHIFactory.cpp
  rhi/RHISurface.h
  rhi/RHIQueue.h
  rhi/RHIFrameContext.h
  rhi/RHIResourceLifetime.h
  rhi/RHIShaderReflection.h
  rhi/RHIPipeline.h
  # vulkan backend
  rhi/vulkan/VulkanSurface.h
  rhi/vulkan/VulkanSurface.cpp
  rhi/vulkan/VulkanDevice.h
  rhi/vulkan/VulkanDevice.cpp
  rhi/vulkan/VulkanFrameContext.h
  rhi/vulkan/VulkanFrameContext.cpp
  rhi/vulkan/VulkanCommandBuffer.h
  rhi/vulkan/VulkanCommandBuffer.cpp
  rhi/vulkan/VulkanResourceTable.h
  rhi/vulkan/VulkanResourceTable.cpp
  rhi/vulkan/VulkanCommandPool.h
  rhi/vulkan/VulkanCommandPool.cpp
  rhi/vulkan/VulkanSynchronization.h
  rhi/vulkan/VulkanSynchronization.cpp
  rhi/vulkan/VulkanFrameOps.h
  rhi/vulkan/VulkanFrameOps.cpp
  rhi/vulkan/VulkanPipelines.h
  rhi/vulkan/VulkanPipelines.cpp
  rhi/vulkan/VulkanSwapchain.h
  rhi/vulkan/VulkanSwapchain.cpp
  render/RenderDevice.cpp
  render/RenderDevice.h
  render/RenderTypes.h
  render/DebugInteropBridge.h
  render/DebugInteropBridge.cpp
  render/RendererFacade.h
  render/RendererFacade.cpp
  render/AsyncLoadingCoordinator.h  
  render/AsyncLoadingCoordinator.cpp
  render/GPUDrivenRenderer.h
  render/GPUDrivenRenderer.cpp
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
  render/FrameSubmission.cpp
  render/FrameSubmission.h
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
    rhi/metal/MetalFrameContext.h
    rhi/metal/MetalFrameContext.cpp
    rhi/d3d12/D3D12Device.h
    rhi/d3d12/D3D12Device.cpp
    rhi/d3d12/D3D12Swapchain.h
    rhi/d3d12/D3D12Swapchain.cpp
    rhi/d3d12/D3D12Pipelines.h
    rhi/d3d12/D3D12Pipelines.cpp
    rhi/d3d12/D3D12FrameContext.h
    rhi/d3d12/D3D12FrameContext.cpp
  )
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
    volk
    imgui_demo
    glm
    tinygltf
    meshoptimizer
    Vulkan::Vulkan
)

if(ANDROID)
  target_link_libraries(demo_core PUBLIC android log native_app_glue)
else()
  target_link_libraries(demo_core PUBLIC glfw)
endif()

if(UNIX AND NOT APPLE AND NOT ANDROID)
  target_link_libraries(demo_core PUBLIC ${X11_LIBRARIES} Threads::Threads)
endif()

if(WIN32)
  target_link_libraries(demo_core PUBLIC winmm.lib)
endif()

if(ANDROID)
  add_library(DemoAndroid SHARED app/AndroidNativeApp.cpp)
  target_link_libraries(DemoAndroid PRIVATE demo_core)
else()
  add_executable(Demo app/MinimalLatestApp.cpp)
  target_link_libraries(Demo PRIVATE demo_core)
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
# Pure-CPU asset tool: no RHI/Vulkan/NVTT dependency; links glm only.
# Not a Demo dependency — built separately via the sdf_baker target.
add_executable(sdf_baker
  tools/sdf_baker/main.cpp
  tools/sdf_baker/SDFBaker.cpp
  tools/sdf_baker/SDFBaker.h
)
target_link_libraries(sdf_baker PRIVATE glm)

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
endif()
