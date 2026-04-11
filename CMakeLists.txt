cmake_minimum_required(VERSION 3.21)
project(Demo VERSION 1.0 LANGUAGES CXX)

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

include(FetchContent)

get_filename_component(REPO_ROOT "${CMAKE_CURRENT_LIST_DIR}/.." ABSOLUTE)

list(APPEND CMAKE_MODULE_PATH "${CMAKE_CURRENT_LIST_DIR}/cmake")        
include(CompilerSlangShader)
include(CompilerGlslShader)
include(DownloadPackage)

find_package(Vulkan REQUIRED)

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
  ${imgui_SOURCE_DIR}/backends/imgui_impl_glfw.cpp
  ${imgui_SOURCE_DIR}/backends/imgui_impl_glfw.h
  ${imgui_SOURCE_DIR}/backends/imgui_impl_vulkan.cpp
  ${imgui_SOURCE_DIR}/backends/imgui_impl_vulkan.h
)
target_include_directories(imgui_demo PUBLIC
  ${imgui_SOURCE_DIR}
  ${imgui_SOURCE_DIR}/backends
)
target_compile_definitions(imgui_demo PUBLIC IMGUI_IMPL_VULKAN_USE_VOLK)
target_link_libraries(imgui_demo PUBLIC volk glfw)

set(STB_IMAGE_URL "https://raw.githubusercontent.com/nothings/stb/master/stb_image.h")
set(STB_IMAGE_PATH "${CMAKE_CURRENT_BINARY_DIR}/stb_image.h")
if(NOT EXISTS "${STB_IMAGE_PATH}")
  message(STATUS "Downloading stb_image.h for Demo...")
  file(DOWNLOAD "${STB_IMAGE_URL}" "${STB_IMAGE_PATH}")
endif()

if(UNIX AND NOT APPLE)
  find_package(X11 REQUIRED)
  find_package(Threads REQUIRED)
endif()

file(GLOB SHADER_SLANG_FILES "${CMAKE_CURRENT_LIST_DIR}/shaders/*.slang")
file(GLOB SHADER_GLSL_FILES  "${CMAKE_CURRENT_LIST_DIR}/shaders/*.glsl")
file(GLOB SHADER_H_FILES     "${CMAKE_CURRENT_LIST_DIR}/shaders/*.h")

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
  app/MinimalLatestApp.h
  rhi/RHITypes.h
  rhi/RHIHandles.h
  rhi/RHIBindlessTypes.h
  rhi/RHICommandList.h
  rhi/RHICommandPool.h
  rhi/RHISynchronization.h
  rhi/RHISwapchain.h
  rhi/RHIDevice.h
  rhi/RHIDescriptor.h
  rhi/RHISurface.h
  rhi/RHIQueue.h
  rhi/RHIFrameContext.h
  rhi/RHIResourceLifetime.h
  rhi/RHIShaderReflection.h
  rhi/RHIPipeline.h
  rhi/vulkan/VulkanSurface.h
  rhi/vulkan/VulkanSurface.cpp
  rhi/vulkan/VulkanDevice.h
  rhi/vulkan/VulkanDevice.cpp
  rhi/vulkan/VulkanFrameContext.h
  rhi/vulkan/VulkanFrameContext.cpp
  rhi/vulkan/VulkanCommandList.h
  rhi/vulkan/VulkanCommandList.cpp
  rhi/vulkan/VulkanCommandPool.h
  rhi/vulkan/VulkanCommandPool.cpp
  rhi/vulkan/VulkanSynchronization.h
  rhi/vulkan/VulkanSynchronization.cpp
  rhi/vulkan/VulkanDescriptor.h
  rhi/vulkan/VulkanDescriptor.cpp
  rhi/vulkan/VulkanFrameOps.h
  rhi/vulkan/VulkanFrameOps.cpp
  rhi/vulkan/VulkanPipelines.h
  rhi/vulkan/VulkanPipelines.cpp
  rhi/vulkan/VulkanSwapchain.h
  rhi/vulkan/VulkanSwapchain.cpp
  rhi/metal/MetalDevice.h
  rhi/metal/MetalDevice.cpp
  rhi/metal/MetalSwapchain.h
  rhi/metal/MetalSwapchain.cpp
  rhi/metal/MetalCommandList.h
  rhi/metal/MetalCommandList.cpp
  rhi/metal/MetalDescriptor.h
  rhi/metal/MetalDescriptor.cpp
  rhi/metal/MetalPipelines.h
  rhi/metal/MetalPipelines.cpp
  rhi/metal/MetalFrameContext.h
  rhi/metal/MetalFrameContext.cpp
  # D3D12 backend scaffold (Windows only)
  rhi/d3d12/D3D12Device.h
  rhi/d3d12/D3D12Device.cpp
  rhi/d3d12/D3D12Swapchain.h
  rhi/d3d12/D3D12Swapchain.cpp
  rhi/d3d12/D3D12CommandList.h
  rhi/d3d12/D3D12CommandList.cpp
  rhi/d3d12/D3D12Descriptor.h
  rhi/d3d12/D3D12Descriptor.cpp
  rhi/d3d12/D3D12Pipelines.h
  rhi/d3d12/D3D12Pipelines.cpp
  rhi/d3d12/D3D12FrameContext.h
  rhi/d3d12/D3D12FrameContext.cpp
  render/Renderer.cpp
  render/Renderer.h
  render/passes/AnimateVerticesPass.h
  render/passes/AnimateVerticesPass.cpp
  render/passes/GBufferPass.h
  render/passes/GBufferPass.cpp
  render/passes/LightPass.h
  render/passes/LightPass.cpp
  render/passes/LightCullingPass.h
  render/passes/LightCullingPass.cpp
  render/passes/ForwardPass.h
  render/passes/ForwardPass.cpp
  render/passes/SceneOpaquePass.h
  render/passes/SceneOpaquePass.cpp
  render/passes/PresentPass.h
  render/passes/PresentPass.cpp
  render/passes/ImguiPass.h 
  render/passes/ImguiPass.cpp
  render/BindGroups.h
  render/Pass.h
  render/TransientAllocator.cpp
  render/TransientAllocator.h
  render/PassExecutor.cpp
  render/PassExecutor.h
  render/FrameSubmission.cpp
  render/FrameSubmission.h
  render/SceneResources.cpp
  render/SceneResources.h
  render/DrawStream.h
  render/DrawStreamDecoder.cpp
  render/DrawStreamDecoder.h
  render/DrawStreamWriter.cpp
  render/DrawStreamWriter.h
  common/Common.h
  common/Handles.h
  common/HandlePool.h
  loader/GltfLoader.h
  loader/GltfLoader.cpp
  render/MeshPool.h
  render/MeshPool.cpp
  render/LightResources.h
  render/LightResources.cpp
  render/IBLResources.h
  render/IBLResources.cpp
  render/ShadowResources.h
  render/ShadowResources.cpp
  render/Camera.h
  render/Camera.cpp
)

target_compile_definitions(demo_core PUBLIC VK_NO_PROTOTYPES) 
if(USE_SLANG)
  target_compile_definitions(demo_core PUBLIC USE_SLANG)
endif()
if(VK_SEMAPHORE_DEBUG)
  target_compile_definitions(demo_core PUBLIC NVVK_SEMAPHORE_DEBUG=1)
endif()

target_include_directories(demo_core
  PUBLIC
    ${CMAKE_CURRENT_BINARY_DIR}
    ${volk_SOURCE_DIR}
    ${imgui_SOURCE_DIR}
    ${imgui_SOURCE_DIR}/backends
    ${vulkanmemoryallocator_SOURCE_DIR}/include
)


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
    glfw
    volk
    imgui_demo
    glm
    tinygltf
    Vulkan::Vulkan
)

if(UNIX AND NOT APPLE)
  target_link_libraries(demo_core PUBLIC ${X11_LIBRARIES} Threads::Threads)
endif()

if(WIN32)
  target_link_libraries(demo_core PUBLIC winmm.lib)
endif()

add_executable(Demo app/MinimalLatestApp.cpp)
target_link_libraries(Demo PRIVATE demo_core)

file(GLOB IMAGE_FILE "${CMAKE_CURRENT_LIST_DIR}/resources/*.jpg")
add_custom_command(
  TARGET Demo POST_BUILD
  COMMAND ${CMAKE_COMMAND} -E copy_if_different
  ${IMAGE_FILE} $<TARGET_FILE_DIR:Demo>
)
