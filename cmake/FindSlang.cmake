# FindSlang.cmake
#
# The project invokes slangc at build time. A complete Slang SDK is optional;
# official macOS archives may contain only the compiler and its versioned
# runtime dylibs in the archive root.

if(NOT DEFINED Slang_VERSION)
  set(Slang_VERSION "2025.13.1")
endif()
set(Slang_VERSION "${Slang_VERSION}" CACHE STRING
  "Slang version used by the shader build")

set(_SLANG_USING_PROVIDED_ROOT FALSE)
if(DEFINED Slang_ROOT AND
   (EXISTS "${Slang_ROOT}/bin/slangc" OR EXISTS "${Slang_ROOT}/bin/slangc.exe" OR
    EXISTS "${Slang_ROOT}/slangc" OR EXISTS "${Slang_ROOT}/slangc.exe"))
  set(_SLANG_USING_PROVIDED_ROOT TRUE)
  set(Slang_SOURCE_DIR "${Slang_ROOT}" CACHE PATH "Path to the Slang tools" FORCE)
else()
  string(TOLOWER "${CMAKE_SYSTEM_PROCESSOR}" ARCH_PROC)
  if(ARCH_PROC MATCHES "^(arm|aarch64)")
    if(WIN32)
      set(PACKMAN_ARCH "arm64")
    else()
      set(PACKMAN_ARCH "aarch64")
    endif()
    set(GITHUB_ARCH "aarch64")
  elseif(ARCH_PROC MATCHES "^(x86_64|amd64|i[3-6]86)")
    if(WIN32)
      set(PACKMAN_ARCH "x64")
    else()
      set(PACKMAN_ARCH "x86_64")
    endif()
    set(GITHUB_ARCH "x86_64")
  else()
    message(FATAL_ERROR "Unhandled architecture '${ARCH_PROC}'")
  endif()

  if(WIN32)
    set(SLANG_OS "windows")
  elseif(APPLE)
    set(SLANG_OS "macos")
  else()
    set(SLANG_OS "linux")
  endif()

  set(Slang_URLS
    "https://d4i3qtqj3r0z5.cloudfront.net/slang%40v${Slang_VERSION}-${SLANG_OS}-${PACKMAN_ARCH}-release.zip"
    "https://github.com/shader-slang/slang/releases/download/v${Slang_VERSION}/slang-${Slang_VERSION}-${SLANG_OS}-${GITHUB_ARCH}.zip"
  )
  include(DownloadPackage)
  download_package(
    NAME "Slang-${SLANG_OS}-${GITHUB_ARCH}"
    URLS ${Slang_URLS}
    VERSION ${Slang_VERSION}
    LOCATION Slang_SOURCE_DIR
  )
endif()

set(Slang_ROOT "${Slang_SOURCE_DIR}" CACHE PATH "Path to the Slang tools" FORCE)
mark_as_advanced(Slang_ROOT)

find_program(Slang_SLANGC_EXECUTABLE
  NAMES slangc.exe slangc
  HINTS "${Slang_ROOT}/bin" "${Slang_ROOT}"
  NO_DEFAULT_PATH
  DOC "Slang compiler (slangc)"
)
find_program(Slang_SLANGD_EXECUTABLE
  NAMES slangd.exe slangd
  HINTS "${Slang_ROOT}/bin" "${Slang_ROOT}"
  NO_DEFAULT_PATH
  DOC "Slang language server (slangd)"
)
mark_as_advanced(Slang_SLANGC_EXECUTABLE Slang_SLANGD_EXECUTABLE)

if(UNIX AND Slang_SLANGC_EXECUTABLE AND NOT _SLANG_USING_PROVIDED_ROOT)
  file(CHMOD "${Slang_SLANGC_EXECUTABLE}"
    PERMISSIONS OWNER_READ OWNER_WRITE OWNER_EXECUTE
                GROUP_READ GROUP_EXECUTE WORLD_READ WORLD_EXECUTE)
endif()

# Optional SDK pieces. The application does not link Slang, but keeping this
# imported target preserves compatibility for tools that do.
find_path(Slang_INCLUDE_DIR
  slang.h
  HINTS "${Slang_ROOT}/include" "${Slang_ROOT}"
  NO_DEFAULT_PATH
)
find_library(Slang_LIBRARY
  NAMES slang slang-compiler
  HINTS "${Slang_ROOT}/lib" "${Slang_ROOT}/bin" "${Slang_ROOT}"
  NO_DEFAULT_PATH
)
if(NOT Slang_LIBRARY AND APPLE)
  file(GLOB _SLANG_VERSIONED_LIBRARIES
    "${Slang_ROOT}/lib/libslang-compiler*.dylib"
    "${Slang_ROOT}/libslang-compiler*.dylib")
  list(SORT _SLANG_VERSIONED_LIBRARIES)
  if(_SLANG_VERSIONED_LIBRARIES)
    list(GET _SLANG_VERSIONED_LIBRARIES 0 Slang_LIBRARY)
  endif()
endif()

if(WIN32)
  find_file(Slang_DLL
    NAMES slang.dll
    HINTS "${Slang_ROOT}/bin" "${Slang_ROOT}"
    NO_DEFAULT_PATH)
else()
  set(Slang_DLL "${Slang_LIBRARY}")
endif()
mark_as_advanced(Slang_INCLUDE_DIR Slang_LIBRARY Slang_DLL)

if(NOT TARGET Slang)
  if(Slang_LIBRARY)
    add_library(Slang SHARED IMPORTED)
    set_target_properties(Slang PROPERTIES IMPORTED_LOCATION "${Slang_DLL}")
    if(WIN32)
      set_property(TARGET Slang PROPERTY IMPORTED_IMPLIB "${Slang_LIBRARY}")
    endif()
  else()
    add_library(Slang INTERFACE IMPORTED)
  endif()
endif()

find_file(Slang_GLSL_MODULE
  NAMES
    slang-glsl-module.dll
    libslang-glsl-module.so
    libslang-glsl-module.dylib
    "libslang-glsl-module-${Slang_VERSION}.dylib"
  HINTS "${Slang_ROOT}/bin" "${Slang_ROOT}/lib" "${Slang_ROOT}"
  NO_DEFAULT_PATH)
if(Slang_GLSL_MODULE AND NOT TARGET SlangGlslModule)
  add_library(SlangGlslModule SHARED IMPORTED)
  set_target_properties(SlangGlslModule PROPERTIES
    IMPORTED_NO_SONAME ON
    IMPORTED_LOCATION "${Slang_GLSL_MODULE}")
endif()

find_file(Slang_GLSLANG
  NAMES
    slang-glslang.dll
    libslang-glslang.so
    libslang-glslang.dylib
    "libslang-glslang-${Slang_VERSION}.dylib"
  HINTS "${Slang_ROOT}/bin" "${Slang_ROOT}/lib" "${Slang_ROOT}"
  NO_DEFAULT_PATH)
if(Slang_GLSLANG AND NOT TARGET SlangGlslang)
  add_library(SlangGlslang SHARED IMPORTED)
  set_target_properties(SlangGlslang PROPERTIES
    IMPORTED_NO_SONAME ON
    IMPORTED_LOCATION "${Slang_GLSLANG}")
endif()

message(STATUS "--> using SLANGC under: ${Slang_SLANGC_EXECUTABLE}")

include(FindPackageHandleStandardArgs)
find_package_handle_standard_args(Slang
  REQUIRED_VARS Slang_ROOT Slang_SLANGC_EXECUTABLE
  VERSION_VAR Slang_VERSION)
