# FindSlang.cmake
#
# Downloads the Slang SDK.
#
# Sets the following variables:
# Slang_VERSION: The downloaded version of Slang.
# Slang_ROOT: Path to the Slang SDK root directory.
# Slang_INCLUDE_DIR: Directory that includes slang.h.
# Slang_SLANGC_EXECUTABLE: Path to the Slang compiler.
# Slang_LIBRARY: Linker library.
# Slang_DLL: Shared library.
#
# Creates an imported library, `Slang`, that can be linked against.

set(Slang_VERSION "2025.13.1" CACHE STRING "Slang version. If you change this and ran CMake before, you will need to delete the other Slang_* cache variables")

set(_SLANG_USING_PROVIDED_ROOT FALSE)
if(DEFINED Slang_ROOT AND EXISTS "${Slang_ROOT}/bin" AND EXISTS "${Slang_ROOT}/include")
  set(_SLANG_USING_PROVIDED_ROOT TRUE)
  set(Slang_SOURCE_DIR "${Slang_ROOT}" CACHE PATH "Path to the Slang SDK root directory" FORCE)
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
else()
    set(SLANG_OS "linux")
endif()

# Download Slang SDK.
# We provide two URLs here since some users' proxies might break one or the other.
# The "d4i3qtqj3r0z5.cloudfront.net" address is the public Omniverse Packman
# server; it is not private.
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

# On Linux, the Cloudfront download of Slang might not have the executable bit
# set on its executables and DLLs. This causes find_program to fail. To fix this,
# call chmod a+rwx on those directories:
if(UNIX AND NOT _SLANG_USING_PROVIDED_ROOT)
  file(CHMOD_RECURSE ${Slang_SOURCE_DIR}/bin ${Slang_SOURCE_DIR}/lib
       FILE_PERMISSIONS OWNER_READ OWNER_WRITE OWNER_EXECUTE GROUP_READ GROUP_WRITE GROUP_EXECUTE WORLD_READ WORLD_WRITE WORLD_EXECUTE
  )
endif()

set(Slang_ROOT ${Slang_SOURCE_DIR} CACHE PATH "Path to the Slang SDK root directory")
mark_as_advanced(Slang_ROOT)

set(_SLANG_HOST_TOOL_ONLY FALSE)
if(_SLANG_USING_PROVIDED_ROOT AND CMAKE_CROSSCOMPILING)
  set(_SLANG_HOST_TOOL_ONLY TRUE)
endif()

if(_SLANG_HOST_TOOL_ONLY)
  set(Slang_INCLUDE_DIR "${Slang_ROOT}/include" CACHE PATH "Directory that includes slang.h." FORCE)
else()
  find_path(Slang_INCLUDE_DIR
    slang.h
    HINTS ${Slang_ROOT}/include
    NO_DEFAULT_PATH
    DOC "Directory that includes slang.h."
  )
endif()
mark_as_advanced(Slang_INCLUDE_DIR)

if(_SLANG_HOST_TOOL_ONLY)
  find_program(Slang_SLANGC_EXECUTABLE
    NAMES slangc.exe slangc
    HINTS ${Slang_SOURCE_DIR}/bin
    NO_DEFAULT_PATH
    DOC "Slang compiler (slangc)"
  )
else()
  find_program(Slang_SLANGC_EXECUTABLE
    NAMES slangc
    HINTS ${Slang_SOURCE_DIR}/bin
    NO_DEFAULT_PATH
    DOC "Slang compiler (slangc)"
  )
endif()
mark_as_advanced(Slang_SLANGC_EXECUTABLE)

if(_SLANG_HOST_TOOL_ONLY)
  find_program(Slang_SLANGD_EXECUTABLE
    NAMES slangd.exe slangd
    HINTS ${Slang_SOURCE_DIR}/bin
    NO_DEFAULT_PATH
    DOC "Slang language server (slangd)"
  )
else()
  find_program(Slang_SLANGD_EXECUTABLE
    NAMES slangd
    HINTS ${Slang_SOURCE_DIR}/bin
    NO_DEFAULT_PATH
    DOC "Slang language server (slangd)"
  )
endif()
mark_as_advanced(Slang_SLANGD_EXECUTABLE)

if(_SLANG_HOST_TOOL_ONLY)
  if(EXISTS "${Slang_SOURCE_DIR}/lib/slang.lib")
    set(Slang_LIBRARY "${Slang_SOURCE_DIR}/lib/slang.lib" CACHE FILEPATH "Slang linker library" FORCE)
  elseif(EXISTS "${Slang_SOURCE_DIR}/lib/libslang.so")
    set(Slang_LIBRARY "${Slang_SOURCE_DIR}/lib/libslang.so" CACHE FILEPATH "Slang linker library" FORCE)
  else()
    set(Slang_LIBRARY "${Slang_SOURCE_DIR}/bin/slang.dll" CACHE FILEPATH "Slang linker library" FORCE)
  endif()
else()
  find_library(Slang_LIBRARY
    NAMES slang
    HINTS ${Slang_SOURCE_DIR}/lib
    NO_DEFAULT_PATH
    DOC "Slang linker library"
  )
endif()
mark_as_advanced(Slang_LIBRARY)

if(_SLANG_HOST_TOOL_ONLY)
  if(EXISTS "${Slang_SOURCE_DIR}/bin/slang.dll")
    set(Slang_DLL "${Slang_SOURCE_DIR}/bin/slang.dll" CACHE FILEPATH "Slang shared library" FORCE)
  else()
    set(Slang_DLL "${Slang_LIBRARY}" CACHE FILEPATH "Slang shared library" FORCE)
  endif()
elseif(WIN32)
  find_file(Slang_DLL
    NAMES slang.dll
    HINTS ${Slang_SOURCE_DIR}/bin
    NO_DEFAULT_PATH
    DOC "Slang shared library (.dll)"
  )
else() # Unix; uses .so
  set(Slang_DLL ${Slang_LIBRARY} CACHE PATH "Slang shared library (.so)")
endif()
mark_as_advanced(Slang_DLL)

# CMake Import library
if(NOT TARGET Slang)
  add_library(Slang SHARED IMPORTED)
  set_target_properties(Slang PROPERTIES
                        IMPORTED_LOCATION ${Slang_DLL}
                        # NOTE(nbickford): Setting INTERFACE_INCLUDE_DIRECTORIES
                        # should make the include directory propagate upwards...
                        # but in CMake 3.31.6, it doesn't. In fact, it does the
                        # opposite; adding INTERFACE_INCLUDE_DIRECTORIES makes
                        # attempts to add it later have no effect.
                        # INTERFACE_INCLUDE_DIRECTORIES ${Slang_INCLUDE_DIR}
  )
  if(WIN32)
    set_property(TARGET Slang PROPERTY IMPORTED_IMPLIB ${Slang_LIBRARY})
  else()
    # Vulkan SDK includes 'libslang.so' and sets LD_LIBRARY_PATH, which conflict
    # with the downloaded slang. This uses the deprecated RPATH instead of
    # RUNPATH to take priority over LD_LIBRARY_PATH.
    set_target_properties(Slang PROPERTIES
      INTERFACE_LINK_OPTIONS "-Wl,--disable-new-dtags"
    )
  endif()
endif()

# If we want to use Slang with .enableGLSL = true, then we should copy the Slang
# GLSL module to the output directory as well. Otherwise, Slang might use the
# slang-glsl-module.dll under the Vulkan SDK directory (if the Vulkan SDK is
# on PATH), which may be incompatible.
# To make this work, we make the GLSL module an IMPORTED library, with the same
# IMPLIB as core Slang.
if(_SLANG_HOST_TOOL_ONLY)
  if(EXISTS "${Slang_SOURCE_DIR}/bin/slang-glsl-module.dll")
    set(Slang_GLSL_MODULE "${Slang_SOURCE_DIR}/bin/slang-glsl-module.dll" CACHE FILEPATH "Slang embedded GLSL module" FORCE)
  else()
    find_file(Slang_GLSL_MODULE
      NAMES libslang-glsl-module.so
      HINTS ${Slang_SOURCE_DIR}/lib
      NO_DEFAULT_PATH
      DOC "Slang embedded GLSL module"
    )
  endif()
else()
  find_file(Slang_GLSL_MODULE
    NAMES ${CMAKE_SHARED_LIBRARY_PREFIX}slang-glsl-module${CMAKE_SHARED_LIBRARY_SUFFIX}
    HINTS ${Slang_SOURCE_DIR}/bin
          ${Slang_SOURCE_DIR}/lib
    NO_DEFAULT_PATH
    DOC "Slang embedded GLSL module"
  )
endif()
mark_as_advanced(Slang_GLSL_MODULE)

if(NOT TARGET SlangGlslModule)
  add_library(SlangGlslModule SHARED IMPORTED)
  set_target_properties(SlangGlslModule PROPERTIES
    IMPORTED_NO_SONAME ON # See https://github.com/shader-slang/slang/issues/7722
    IMPORTED_LOCATION ${Slang_GLSL_MODULE}
  )
  if(WIN32)
    set_property(TARGET SlangGlslModule PROPERTY IMPORTED_IMPLIB ${Slang_LIBRARY})
  endif()
endif()

# Additionally, SLANG_OPTIMIZATION_LEVEL_HIGH requires slang-glslang.dll.
# Find it and link with it by default:
if(_SLANG_HOST_TOOL_ONLY)
  if(EXISTS "${Slang_SOURCE_DIR}/bin/slang-glslang.dll")
    set(Slang_GLSLANG "${Slang_SOURCE_DIR}/bin/slang-glslang.dll" CACHE FILEPATH "slang-glslang shared library" FORCE)
  else()
    find_file(Slang_GLSLANG
      NAMES libslang-glslang.so
      HINTS ${Slang_SOURCE_DIR}/lib
      NO_DEFAULT_PATH
      DOC "slang-glslang shared library"
    )
  endif()
else()
  find_file(Slang_GLSLANG
    NAMES ${CMAKE_SHARED_LIBRARY_PREFIX}slang-glslang${CMAKE_SHARED_LIBRARY_SUFFIX}
    HINTS ${Slang_SOURCE_DIR}/bin
          ${Slang_SOURCE_DIR}/lib
    NO_DEFAULT_PATH
    DOC "slang-glslang shared library"
  )
endif()
mark_as_advanced(Slang_GLSLANG)

if(NOT TARGET SlangGlslang)
  add_library(SlangGlslang SHARED IMPORTED)
  set_target_properties(SlangGlslang PROPERTIES
    IMPORTED_NO_SONAME ON # See https://github.com/shader-slang/slang/issues/7722
    IMPORTED_LOCATION ${Slang_GLSLANG}
  )
  if(WIN32)
    set_property(TARGET SlangGlslang PROPERTY IMPORTED_IMPLIB ${Slang_LIBRARY})
  endif()
endif()


message(STATUS "--> using SLANGC under: ${Slang_SLANGC_EXECUTABLE}")

include(FindPackageHandleStandardArgs)
find_package_handle_standard_args(Slang
  REQUIRED_VARS
    Slang_ROOT
    Slang_SLANGC_EXECUTABLE
    Slang_LIBRARY
    Slang_DLL
    Slang_INCLUDE_DIR
  VERSION_VAR
    Slang_VERSION
)
