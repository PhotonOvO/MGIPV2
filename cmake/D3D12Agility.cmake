# Direct3D 12 Agility SDK.
#
# Vendors Microsoft.Direct3D.D3D12 so D3D12Core.dll ships with the application instead of
# coming from the operating system. The import library still comes from the Windows SDK:
# D3D12CreateDevice is exported by the system d3d12.dll, which acts as the loader and, when
# the executable advertises a version, resolves the vendored core DLL. What the package
# actually provides is a newer d3d12.h plus the runtime binaries.
#
# D3D12_AGILITY_SDK_VERSION is the version reported to the loader; the loader then looks for
# the core DLL under the path given by D3D12_AGILITY_SDK_PATH next to the executable. Both
# are advertised by exporting D3D12SDKVersion and D3D12SDKPath from the executable, which is
# what D3D12AgilitySDK.cpp does.

set(DEMO_D3D12_AGILITY_PACKAGE_VERSION "1.619.6"
    CACHE STRING "Microsoft.Direct3D.D3D12 NuGet package version")
set(DEMO_D3D12_AGILITY_SDK_VERSION "619"
    CACHE STRING "Agility SDK runtime version advertised to the D3D12 loader")
set(DEMO_D3D12_AGILITY_ARCH "x64"
    CACHE STRING "Runtime binaries to vendor from the package")

set(_demo_agility_root "${CMAKE_BINARY_DIR}/_deps/d3d12-agility")
set(_demo_agility_include "${_demo_agility_root}/build/native/include")
set(_demo_agility_bin "${_demo_agility_root}/build/native/bin/${DEMO_D3D12_AGILITY_ARCH}")

if(NOT EXISTS "${_demo_agility_include}/d3d12.h")
  set(_demo_agility_package
      "${CMAKE_BINARY_DIR}/_deps/microsoft.direct3d.d3d12.${DEMO_D3D12_AGILITY_PACKAGE_VERSION}.nupkg")
  if(NOT EXISTS "${_demo_agility_package}")
    message(STATUS "D3D12 Agility SDK: downloading ${DEMO_D3D12_AGILITY_PACKAGE_VERSION}")
    file(DOWNLOAD
      "https://api.nuget.org/v3-flatcontainer/microsoft.direct3d.d3d12/${DEMO_D3D12_AGILITY_PACKAGE_VERSION}/microsoft.direct3d.d3d12.${DEMO_D3D12_AGILITY_PACKAGE_VERSION}.nupkg"
      "${_demo_agility_package}"
      STATUS _demo_agility_download_status
      TLS_VERIFY ON)
    list(GET _demo_agility_download_status 0 _demo_agility_download_code)
    if(NOT _demo_agility_download_code EQUAL 0)
      list(GET _demo_agility_download_status 1 _demo_agility_download_message)
      message(FATAL_ERROR
        "D3D12 Agility SDK download failed: ${_demo_agility_download_message}")
    endif()
  endif()

  # A .nupkg is a zip, so ARCHIVE_EXTRACT handles it directly.
  file(ARCHIVE_EXTRACT INPUT "${_demo_agility_package}" DESTINATION "${_demo_agility_root}")
  if(NOT EXISTS "${_demo_agility_include}/d3d12.h")
    message(FATAL_ERROR
      "D3D12 Agility SDK package did not contain ${_demo_agility_include}/d3d12.h")
  endif()
endif()

set(DEMO_D3D12_AGILITY_INCLUDE_DIR "${_demo_agility_include}")
set(DEMO_D3D12_AGILITY_BIN_DIR "${_demo_agility_bin}")

# Puts the Agility headers ahead of the Windows SDK ones, advertises the version, and stages
# the runtime next to the produced binary.
function(demo_apply_d3d12_agility target)
  target_include_directories(${target} BEFORE PRIVATE "${DEMO_D3D12_AGILITY_INCLUDE_DIR}")
  target_compile_definitions(${target} PRIVATE
    D3D12_AGILITY_SDK_VERSION=${DEMO_D3D12_AGILITY_SDK_VERSION}
    D3D12_AGILITY_SDK_PATH="D3D12")

  # The loader requires the directory to live beside the executable, and reads the layer DLL
  # from the same place, so both are staged on every build rather than installed separately.
  add_custom_command(TARGET ${target} POST_BUILD
    COMMAND "${CMAKE_COMMAND}" -E make_directory
            "$<TARGET_FILE_DIR:${target}>/D3D12"
    COMMAND "${CMAKE_COMMAND}" -E copy_if_different
            "${DEMO_D3D12_AGILITY_BIN_DIR}/D3D12Core.dll"
            "$<TARGET_FILE_DIR:${target}>/D3D12/D3D12Core.dll"
    COMMAND "${CMAKE_COMMAND}" -E copy_if_different
            "${DEMO_D3D12_AGILITY_BIN_DIR}/d3d12SDKLayers.dll"
            "$<TARGET_FILE_DIR:${target}>/D3D12/d3d12SDKLayers.dll"
    COMMENT "Staging D3D12 Agility SDK runtime for ${target}"
    VERBATIM)
endfunction()
