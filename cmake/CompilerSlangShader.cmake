#==================================================================================================
# compile_slang: Compile Slang shaders into C++ headers and SPIR-V binaries
#==================================================================================================
#
# This function compiles Slang shader files using slangc, generating:
#   - Embedded C++ headers (.h) with SPIR-V code as text
#   - Binary SPIR-V files (.spv)
#   - Dependency files (.dep) for incremental builds
#
# Basic usage:
#
#   set(SHADER_FILES
#     ${CMAKE_SOURCE_DIR}/shaders/shader1.slang
#     ${CMAKE_SOURCE_DIR}/shaders/shader2.slang
#   )
#
#   compile_slang(
#     "${SHADER_FILES}"              # List of shader files to compile
#     "${CMAKE_BINARY_DIR}/_autogen" # Output directory for generated files
#     HEADERS_VAR GENERATED_SHADER_HEADERS
#   )
#
#   target_sources(MyProject PRIVATE ${GENERATED_SHADER_HEADERS})
#
#==================================================================================================
# Optional arguments:
#==================================================================================================
#
# Compiler target & language:
#   LANGUAGE <lang>                - Source language: slang, glsl, hlsl (default: slang)
#   PROFILE <profile>              - Target profile (default: spirv_1_6)
#   TARGET <target>                - Code generation target (default: spirv)
#
# Capabilities:
#   CAPABILITIES <cap1> <cap2>...  - SPIR-V capabilities (joined with + for slangc)
#
# Build settings:
#   DEBUG_LEVEL <0-3>              - Debug information level (default: 1)
#                                      0 = no debug info
#                                      1 = embeds shader source for Nsight and other tools
#                                      2 = standard debug info
#                                      3 = full debug info
#   OPTIMIZATION_LEVEL <0-3>       - Optimization level (default: 0)
#                                      0 = no optimization (faster builds; also avoids spirv-opt
#                                          bugs that have broken some of our shaders)
#                                      1 = minimal optimization
#                                      2 = default optimization
#                                      3 = aggressive optimization
#
# Output variables:
#   HEADERS_VAR <variable_name>    - Variable to receive list of generated .h files
#   SPVS_VAR <variable_name>       - Variable to receive list of generated .spv files
#   At least one must be provided; output type is inferred from which are present.
#
# Other options:
#   NAME_SUFFIX <suffix>           - Append suffix to output filenames
#   EXTRA_FLAGS <flags...>         - Additional raw compiler flags (e.g. -I, -D)
#   VERBOSE ON|OFF                 - Display full command lines (default: OFF)
#   SHOW_TIMING ON|OFF             - Display compilation time per shader (default: OFF)
#
# Advanced examples:
#
#   # Generate both headers and SPIR-V binaries with capabilities
#   compile_slang(
#     "${SHADER_FILES}"
#     "${OUTPUT_DIR}"
#     HEADERS_VAR MY_HEADERS
#     SPVS_VAR MY_SPVS
#     CAPABILITIES spvInt64Atomics                     # 64-bit atomic operations
#                  spvShaderClockKHR                   # Shader clock for profiling
#                  spvRayTracingMotionBlurNV           # Motion blur for ray tracing
#                  spvRayQueryKHR                      # Ray query operations
#                  SPV_KHR_compute_shader_derivatives  # Derivatives in compute shaders
#   )
#
#   # Generate only SPIR-V binaries (no embedded headers)
#   compile_slang(
#     "${SHADER_FILES}"
#     "${OUTPUT_DIR}"
#     SPVS_VAR MY_SPVS
#   )
#
#   # Debug mode: show full commands and timing
#   compile_slang(
#     "${SHADER_FILES}"
#     "${OUTPUT_DIR}"
#     HEADERS_VAR MY_HEADERS
#     VERBOSE ON
#     SHOW_TIMING ON
#   )
#
#==================================================================================================

option(COMPILE_SHADER_VERBOSE "Show full Slang compiler command lines" OFF)
option(COMPILE_SHADER_SHOW_TIMING "Show shader compilation timing" OFF)


function(compile_slang SHADER_FILES OUTPUT_DIR)

  # Validate slangc
  if(NOT Slang_SLANGC_EXECUTABLE)
    message(FATAL_ERROR "compile_slang: Slang_SLANGC_EXECUTABLE is not defined. "
                        "Make sure FindSlang.cmake has been included and Slang package was found.")
  endif()

  if(NOT EXISTS "${Slang_SLANGC_EXECUTABLE}")
    message(FATAL_ERROR "compile_slang: Slang compiler not found at: ${Slang_SLANGC_EXECUTABLE}")
  endif()

  # Nothing to do if no shader files provided
  if(NOT DEFINED SHADER_FILES OR "${SHADER_FILES}" STREQUAL "")
    return()
  endif()

  #------------------------------------------------------------------------------------------------
  # Parse arguments
  #------------------------------------------------------------------------------------------------

  set(oneValueArgs
    LANGUAGE
    PROFILE
    TARGET
    DEBUG_LEVEL
    OPTIMIZATION_LEVEL
    HEADERS_VAR
    SPVS_VAR
    NAME_SUFFIX
    VERBOSE
    SHOW_TIMING
  )

  set(multiValueArgs
    CAPABILITIES
    EXTRA_FLAGS
  )

  cmake_parse_arguments(COMPILE_SHADER "" "${oneValueArgs}" "${multiValueArgs}" ${ARGN})

  # Legacy compatibility: detect positional SHADER_HEADERS_VAR (third argument).
  # Old form: compile_slang("${FILES}" "${DIR}" GENERATED_HEADERS ...)
  # New form: compile_slang("${FILES}" "${DIR}" HEADERS_VAR GENERATED_HEADERS ...)
  if(COMPILE_SHADER_UNPARSED_ARGUMENTS AND NOT DEFINED COMPILE_SHADER_HEADERS_VAR)
    list(GET COMPILE_SHADER_UNPARSED_ARGUMENTS 0 _LEGACY_HEADERS_VAR)
    set(COMPILE_SHADER_HEADERS_VAR "${_LEGACY_HEADERS_VAR}")
    message(AUTHOR_WARNING
      "compile_slang: Passing the headers variable as a positional argument is deprecated. "
      "Use the named argument instead:\n"
      "  HEADERS_VAR ${_LEGACY_HEADERS_VAR}")
  endif()

  #------------------------------------------------------------------------------------------------
  # Validation
  #------------------------------------------------------------------------------------------------

  if(NOT DEFINED OUTPUT_DIR OR "${OUTPUT_DIR}" STREQUAL "")
    message(FATAL_ERROR "compile_slang: OUTPUT_DIR is not defined or empty.")
  endif()

  if(EXISTS "${OUTPUT_DIR}" AND NOT IS_DIRECTORY "${OUTPUT_DIR}")
    message(FATAL_ERROR "compile_slang: OUTPUT_DIR '${OUTPUT_DIR}' exists but is not a directory.")
  endif()

  # Determine what to generate based on which output variables were provided
  set(_GENERATE_HEADER FALSE)
  set(_GENERATE_SPV FALSE)

  if(DEFINED COMPILE_SHADER_HEADERS_VAR AND NOT "${COMPILE_SHADER_HEADERS_VAR}" STREQUAL "")
    set(_GENERATE_HEADER TRUE)
  endif()

  if(DEFINED COMPILE_SHADER_SPVS_VAR AND NOT "${COMPILE_SHADER_SPVS_VAR}" STREQUAL "")
    set(_GENERATE_SPV TRUE)
  endif()

  if(NOT _GENERATE_HEADER AND NOT _GENERATE_SPV)
    message(FATAL_ERROR "compile_slang: At least one of HEADERS_VAR or SPVS_VAR must be provided.")
  endif()

  foreach(_shader_file ${SHADER_FILES})
    if(NOT EXISTS "${_shader_file}")
      message(FATAL_ERROR "compile_slang: Shader file not found: ${_shader_file}")
    endif()
  endforeach()

  #------------------------------------------------------------------------------------------------
  # Defaults
  #------------------------------------------------------------------------------------------------

  if(NOT DEFINED COMPILE_SHADER_LANGUAGE)
    set(COMPILE_SHADER_LANGUAGE "slang")
  endif()

  if(NOT DEFINED COMPILE_SHADER_PROFILE)
    set(COMPILE_SHADER_PROFILE "spirv_1_6")
  endif()

  if(NOT DEFINED COMPILE_SHADER_TARGET)
    set(COMPILE_SHADER_TARGET "spirv")
  endif()

  # Debug level is now handled via generator expressions in _BUILD_FLAGS below.
  # This allows proper per-configuration handling for multi-config generators (VS, Xcode).
  # Users can still override by explicitly setting DEBUG_LEVEL.

  # Default to no optimization for faster builds, and to avoid spirv-opt bugs that have
  # broken some of our shaders
  if(NOT DEFINED COMPILE_SHADER_OPTIMIZATION_LEVEL)
    set(COMPILE_SHADER_OPTIMIZATION_LEVEL 0)
  endif()

  # Join capabilities list with "+" for slangc -capability flag
  string(REPLACE ";" "+" COMPILE_SHADER_CAPABILITIES "${COMPILE_SHADER_CAPABILITIES}")

  #------------------------------------------------------------------------------------------------
  # Build compiler flags
  #------------------------------------------------------------------------------------------------

  file(MAKE_DIRECTORY "${OUTPUT_DIR}")

  set(SHADER_HEADERS "")
  set(SHADER_SPVS "")

  set(_CORE_FLAGS
    -emit-spirv-directly
    -matrix-layout-column-major
    -force-glsl-scalar-layout
    -fvk-use-entrypoint-name
  )

  set(_CONFIGURABLE_FLAGS
    -lang ${COMPILE_SHADER_LANGUAGE}
    -profile ${COMPILE_SHADER_PROFILE}
    -target ${COMPILE_SHADER_TARGET}
  )

  set(_OPTIONAL_FLAGS "")
  if(COMPILE_SHADER_CAPABILITIES)
    list(APPEND _OPTIONAL_FLAGS -capability ${COMPILE_SHADER_CAPABILITIES})
  endif()

  # Build flags (optimization level is always set)
  set(_BUILD_FLAGS
    -O${COMPILE_SHADER_OPTIMIZATION_LEVEL}
  )

  # Debug level: use generator expression for per-configuration handling
  # Default: Debug/RelWithDebInfo get -g1 (embed source), Release/MinSizeRel get -g0 (no debug)
  # Users can override by explicitly setting DEBUG_LEVEL
  if(DEFINED COMPILE_SHADER_DEBUG_LEVEL AND NOT "${COMPILE_SHADER_DEBUG_LEVEL}" STREQUAL "")
    # User explicitly specified debug level - use it directly
    set(_DEBUG_LEVEL ${COMPILE_SHADER_DEBUG_LEVEL})
  else()
    # Will use generator expression in the command
    set(_DEBUG_LEVEL "auto")
  endif()

  # User flags listed last so they can override earlier settings
  set(_SLANG_FLAGS
    ${_CORE_FLAGS}
    ${_CONFIGURABLE_FLAGS}
    ${_OPTIONAL_FLAGS}
    ${_BUILD_FLAGS}
    ${COMPILE_SHADER_EXTRA_FLAGS}
  )

  # Debug flag - either user-specified or determined by build configuration
  if(_DEBUG_LEVEL STREQUAL "auto")
    # Generator expression: Debug/RelWithDebInfo get -g1, others get -g0
    set(_DEBUG_FLAG_GENEX "$<IF:$<OR:$<CONFIG:Debug>,$<CONFIG:RelWithDebInfo>>,-g1,-g0>")
  else()
    # User specified explicit level
    set(_DEBUG_FLAG_GENEX "-g${_DEBUG_LEVEL}")
  endif()

  #------------------------------------------------------------------------------------------------
  # Process each shader file
  #------------------------------------------------------------------------------------------------

  foreach(SHADER ${SHADER_FILES})
    get_filename_component(SHADER_NAME ${SHADER} NAME)
    string(APPEND SHADER_NAME "${COMPILE_SHADER_NAME_SUFFIX}")

    # Generate valid C/C++ variable name (e.g. "shader.comp.slang" -> "shader_comp_slang")
    string(REPLACE "." "_" _EMBED_NAME ${SHADER_NAME})

    set(OUTPUT_FILE ${OUTPUT_DIR}/${SHADER_NAME})
    set(OUTPUT_H_FILE "${OUTPUT_FILE}.h")
    set(OUTPUT_SPV_FILE "${OUTPUT_FILE}.spv")
    set(OUTPUT_DEP_FILE "${OUTPUT_FILE}.dep")

    set(_COMMANDS)
    set(_OUTPUT_FILES)
    set(_DEPFILE_ADDED FALSE)

    # Generate embedded header (.h)
    if(_GENERATE_HEADER)
      set(_COMMAND_H ${Slang_SLANGC_EXECUTABLE}
        -depfile "${OUTPUT_DEP_FILE}"
        -source-embed-name ${_EMBED_NAME}
        -source-embed-style text
        ${_SLANG_FLAGS}
        "${_DEBUG_FLAG_GENEX}"
        -o "${OUTPUT_H_FILE}"
        ${SHADER}
      )
      set(_DEPFILE_ADDED TRUE)

      list(APPEND _COMMANDS COMMAND ${CMAKE_COMMAND} -E echo "  Generating header: ${SHADER_NAME}.h")
      if(COMPILE_SHADER_VERBOSE)
        string(REPLACE ";" " " _COMMAND_H_STR "${_COMMAND_H}")
        list(APPEND _COMMANDS COMMAND ${CMAKE_COMMAND} -E echo "  ${_COMMAND_H_STR}")
      endif()
      if(COMPILE_SHADER_SHOW_TIMING)
        list(APPEND _COMMANDS COMMAND ${CMAKE_COMMAND} -E time ${_COMMAND_H})
      else()
        list(APPEND _COMMANDS COMMAND ${_COMMAND_H})
      endif()
      list(APPEND _COMMANDS COMMAND ${CMAKE_COMMAND}
        "-DINPUT_FILE=${OUTPUT_H_FILE}"
        -P "${CMAKE_CURRENT_FUNCTION_LIST_DIR}/NormalizeEmbeddedSpirv.cmake")

      list(APPEND _OUTPUT_FILES "${OUTPUT_H_FILE}")
      list(APPEND SHADER_HEADERS "${OUTPUT_H_FILE}")
    endif()

    # Generate SPIR-V binary (.spv)
    if(_GENERATE_SPV)
      set(_COMMAND_S ${Slang_SLANGC_EXECUTABLE}
        ${_SLANG_FLAGS}
        "${_DEBUG_FLAG_GENEX}"
        -o "${OUTPUT_SPV_FILE}"
        ${SHADER}
      )
      if(NOT _DEPFILE_ADDED)
        list(INSERT _COMMAND_S 1 -depfile "${OUTPUT_DEP_FILE}")
      endif()

      list(APPEND _COMMANDS COMMAND ${CMAKE_COMMAND} -E echo "  Generating SPIR-V: ${SHADER_NAME}.spv")
      if(COMPILE_SHADER_VERBOSE)
        string(REPLACE ";" " " _COMMAND_S_STR "${_COMMAND_S}")
        list(APPEND _COMMANDS COMMAND ${CMAKE_COMMAND} -E echo "  ${_COMMAND_S_STR}")
      endif()
      if(COMPILE_SHADER_SHOW_TIMING)
        list(APPEND _COMMANDS COMMAND ${CMAKE_COMMAND} -E time ${_COMMAND_S})
      else()
        list(APPEND _COMMANDS COMMAND ${_COMMAND_S})
      endif()

      list(APPEND _OUTPUT_FILES "${OUTPUT_SPV_FILE}")
      list(APPEND SHADER_SPVS "${OUTPUT_SPV_FILE}")
    endif()

    # Single custom command per shader: all outputs built together so that
    # VS right-click "Compile" on a .slang file produces everything.
    # Cross-shader parallelism is still achieved since each shader has its own command.
    # COMMAND_EXPAND_LISTS is required for generator expressions to be evaluated.
    if(_OUTPUT_FILES)
      add_custom_command(
        OUTPUT ${_OUTPUT_FILES}
        ${_COMMANDS}
        MAIN_DEPENDENCY ${SHADER}
        DEPENDS "${CMAKE_CURRENT_FUNCTION_LIST_DIR}/NormalizeEmbeddedSpirv.cmake"
        DEPFILE "${OUTPUT_DEP_FILE}"
        COMMENT "Compiling Slang shader: ${SHADER_NAME}"
        COMMAND_EXPAND_LISTS
        VERBATIM
      )
    endif()

  endforeach()

  #------------------------------------------------------------------------------------------------
  # Export output variables to caller scope
  #------------------------------------------------------------------------------------------------

  if(_GENERATE_HEADER)
    set(${COMPILE_SHADER_HEADERS_VAR} ${SHADER_HEADERS} PARENT_SCOPE)
  endif()

  if(_GENERATE_SPV)
    set(${COMPILE_SHADER_SPVS_VAR} ${SHADER_SPVS} PARENT_SCOPE)
  endif()

endfunction()

# Compile each Slang module to MSL, use the selected Xcode toolchain to produce
# a metallib, and generate a lookup header keyed by the corresponding embedded
# SPIR-V array. This keeps the renderer's shader-loading call sites backend
# neutral while ensuring Metal consumes native libraries only.
function(compile_slang_metal SHADER_FILES OUTPUT_DIR)
  set(oneValueArgs
    HEADERS_VAR
    CATALOG_HEADER
    ICB_VARIANT_CATALOG_HEADER
    METAL_TOOLCHAIN
    METAL_SDK)
  set(multiValueArgs EXTRA_FLAGS ICB_VARIANT_SHADERS)
  cmake_parse_arguments(METAL_SHADER "" "${oneValueArgs}" "${multiValueArgs}" ${ARGN})

  if(NOT METAL_SHADER_HEADERS_VAR OR NOT METAL_SHADER_CATALOG_HEADER)
    message(FATAL_ERROR
      "compile_slang_metal requires HEADERS_VAR and CATALOG_HEADER")
  endif()
  if(NOT EXISTS "${Slang_SLANGC_EXECUTABLE}")
    message(FATAL_ERROR "compile_slang_metal could not find slangc")
  endif()
  if(NOT METAL_SHADER_METAL_TOOLCHAIN)
    message(FATAL_ERROR
      "compile_slang_metal requires the Xcode Metal compiler component. "
      "Install it with: xcodebuild -downloadComponent MetalToolchain")
  endif()
  if(NOT METAL_SHADER_METAL_SDK)
    set(METAL_SHADER_METAL_SDK "macosx")
  endif()
  if(METAL_SHADER_ICB_VARIANT_SHADERS AND
     NOT METAL_SHADER_ICB_VARIANT_CATALOG_HEADER)
    message(FATAL_ERROR
      "compile_slang_metal requires ICB_VARIANT_CATALOG_HEADER when "
      "ICB_VARIANT_SHADERS are provided")
  endif()

  foreach(_icb_shader IN LISTS METAL_SHADER_ICB_VARIANT_SHADERS)
    list(FIND SHADER_FILES "${_icb_shader}" _icb_shader_index)
    if(_icb_shader_index EQUAL -1)
      message(FATAL_ERROR
        "Metal ICB variant shader is not in SHADER_FILES: ${_icb_shader}")
    endif()
  endforeach()

  file(MAKE_DIRECTORY "${OUTPUT_DIR}")
  set(_metal_headers "")
  set(_catalog_includes "")
  set(_catalog_cases "")
  set(_icb_catalog_includes "")
  set(_icb_catalog_cases "")

  foreach(_shader ${SHADER_FILES})
    get_filename_component(_shader_name "${_shader}" NAME)
    string(REPLACE "." "_" _embed_name "${_shader_name}")
    set(_base "${OUTPUT_DIR}/${_shader_name}")
    set(_metal_source "${_base}.metal")
    set(_air_file "${_base}.air")
    set(_metallib_file "${_base}.metallib")
    set(_metallib_header "${_metallib_file}.h")
    set(_dep_file "${_base}.metal.dep")
    set(_metal_embed_name "${_embed_name}_metal_lib")

    set(_shader_outputs
      "${_metal_source}"
      "${_air_file}"
      "${_metallib_file}"
      "${_metallib_header}")
    set(_icb_compile_commands "")
    list(FIND METAL_SHADER_ICB_VARIANT_SHADERS "${_shader}"
      _icb_variant_index)
    if(NOT _icb_variant_index EQUAL -1)
      set(_icb_metal_source "${_base}.icb.metal")
      set(_icb_air_file "${_base}.icb.air")
      set(_icb_metallib_file "${_base}.icb.metallib")
      set(_icb_metallib_header "${_icb_metallib_file}.h")
      set(_icb_metal_embed_name "${_embed_name}_metal_icb_lib")
      list(APPEND _shader_outputs
        "${_icb_metal_source}"
        "${_icb_air_file}"
        "${_icb_metallib_file}"
        "${_icb_metallib_header}")
      list(APPEND _icb_compile_commands
        COMMAND "${Slang_SLANGC_EXECUTABLE}"
          -lang slang
          -target metal
          -matrix-layout-column-major
          -O0
          "$<IF:$<OR:$<CONFIG:Debug>,$<CONFIG:RelWithDebInfo>>,-g1,-g0>"
          -DDEMO_METAL=1
          -DDEMO_METAL_ICB=1
          ${METAL_SHADER_EXTRA_FLAGS}
          -o "${_icb_metal_source}"
          "${_shader}"
        COMMAND /usr/bin/xcrun --sdk "${METAL_SHADER_METAL_SDK}" --toolchain
          "${METAL_SHADER_METAL_TOOLCHAIN}" metal
          -std=metal4.0
          "-fmodules-cache-path=${OUTPUT_DIR}/metal-module-cache"
          -c "${_icb_metal_source}"
          -o "${_icb_air_file}"
        COMMAND /usr/bin/xcrun --sdk "${METAL_SHADER_METAL_SDK}" --toolchain
          "${METAL_SHADER_METAL_TOOLCHAIN}" metallib
          "${_icb_air_file}"
          -o "${_icb_metallib_file}"
        COMMAND "${CMAKE_COMMAND}"
          "-DINPUT_FILE=${_icb_metallib_file}"
          "-DOUTPUT_FILE=${_icb_metallib_header}"
          "-DVARIABLE_NAME=${_icb_metal_embed_name}"
          -P "${CMAKE_CURRENT_FUNCTION_LIST_DIR}/EmbedBinary.cmake")
    endif()

    add_custom_command(
      OUTPUT ${_shader_outputs}
      COMMAND "${Slang_SLANGC_EXECUTABLE}"
        -depfile "${_dep_file}"
        -lang slang
        -target metal
        -matrix-layout-column-major
        -O0
        "$<IF:$<OR:$<CONFIG:Debug>,$<CONFIG:RelWithDebInfo>>,-g1,-g0>"
        -DDEMO_METAL=1
        ${METAL_SHADER_EXTRA_FLAGS}
        -o "${_metal_source}"
        "${_shader}"
      COMMAND /usr/bin/xcrun --sdk "${METAL_SHADER_METAL_SDK}" --toolchain
        "${METAL_SHADER_METAL_TOOLCHAIN}" metal
        -std=metal4.0
        "-fmodules-cache-path=${OUTPUT_DIR}/metal-module-cache"
        -c "${_metal_source}"
        -o "${_air_file}"
      COMMAND /usr/bin/xcrun --sdk "${METAL_SHADER_METAL_SDK}" --toolchain
        "${METAL_SHADER_METAL_TOOLCHAIN}" metallib
        "${_air_file}"
        -o "${_metallib_file}"
      COMMAND "${CMAKE_COMMAND}"
        "-DINPUT_FILE=${_metallib_file}"
        "-DOUTPUT_FILE=${_metallib_header}"
        "-DVARIABLE_NAME=${_metal_embed_name}"
        -P "${CMAKE_CURRENT_FUNCTION_LIST_DIR}/EmbedBinary.cmake"
      ${_icb_compile_commands}
      MAIN_DEPENDENCY "${_shader}"
      DEPENDS "${CMAKE_CURRENT_FUNCTION_LIST_DIR}/EmbedBinary.cmake"
      DEPFILE "${_dep_file}"
      COMMENT "Compiling Metal shader library: ${_shader_name}"
      COMMAND_EXPAND_LISTS
      VERBATIM)

    list(APPEND _metal_headers "${_metallib_header}")
    string(APPEND _catalog_includes
      "#include \"${_shader_name}.h\"\n#include \"${_shader_name}.metallib.h\"\n")
    string(APPEND _catalog_cases
      "  if (spirv == ${_embed_name})\n"
      "    return {reinterpret_cast<const std::byte*>(${_metal_embed_name}), ${_metal_embed_name}_sizeInBytes};\n")

    if(NOT _icb_variant_index EQUAL -1)
      list(APPEND _metal_headers "${_icb_metallib_header}")
      string(APPEND _icb_catalog_includes
        "#include \"${_shader_name}.metallib.h\"\n#include \"${_shader_name}.icb.metallib.h\"\n")
      string(APPEND _icb_catalog_cases
        "  if (metalLibrary == reinterpret_cast<const std::byte*>(${_metal_embed_name}))\n"
        "    return {reinterpret_cast<const std::byte*>(${_icb_metal_embed_name}), ${_icb_metal_embed_name}_sizeInBytes};\n")
    endif()
  endforeach()

  set(_catalog_content
    "#pragma once\n\n#include <cstddef>\n#include <cstdint>\n\n${_catalog_includes}\nnamespace demo {\nstruct EmbeddedMetalLibrary {\n  const std::byte* data{};\n  size_t size{};\n};\n\ninline EmbeddedMetalLibrary findEmbeddedMetalLibrary(const uint32_t* spirv)\n{\n${_catalog_cases}  return {};\n}\n} // namespace demo\n")
  file(GENERATE OUTPUT "${METAL_SHADER_CATALOG_HEADER}"
    CONTENT "${_catalog_content}")

  if(METAL_SHADER_ICB_VARIANT_CATALOG_HEADER)
    set(_icb_catalog_content
      "#pragma once\n\n#include <cstddef>\n\n${_icb_catalog_includes}\nnamespace demo {\nstruct EmbeddedMetalICBVariantLibrary {\n  const std::byte* data{};\n  size_t size{};\n};\n\ninline EmbeddedMetalICBVariantLibrary findEmbeddedMetalICBVariantLibrary(const std::byte* metalLibrary)\n{\n${_icb_catalog_cases}  return {};\n}\n} // namespace demo\n")
    file(GENERATE OUTPUT "${METAL_SHADER_ICB_VARIANT_CATALOG_HEADER}"
      CONTENT "${_icb_catalog_content}")
  endif()

  set(${METAL_SHADER_HEADERS_VAR} ${_metal_headers} PARENT_SCOPE)
endfunction()

# Compile selected Slang entry points independently, link their AIR objects into
# one metallib, and embed that binary in a generated C++ header. Compiling each
# entry independently keeps its entry-point parameter bindings local to zero.
function(compile_slang_metal_embedded SOURCE_FILE OUTPUT_DIR)
  set(oneValueArgs
    NAME
    VARIABLE_NAME
    HEADER_VAR
    METAL_TOOLCHAIN
    METAL_SDK)
  set(multiValueArgs ENTRY_POINTS EXTRA_FLAGS)
  cmake_parse_arguments(EMBEDDED_METAL "" "${oneValueArgs}" "${multiValueArgs}" ${ARGN})

  if(NOT EMBEDDED_METAL_NAME OR NOT EMBEDDED_METAL_VARIABLE_NAME OR
     NOT EMBEDDED_METAL_HEADER_VAR OR NOT EMBEDDED_METAL_ENTRY_POINTS)
    message(FATAL_ERROR
      "compile_slang_metal_embedded requires NAME, VARIABLE_NAME, HEADER_VAR, and ENTRY_POINTS")
  endif()
  if(NOT EXISTS "${Slang_SLANGC_EXECUTABLE}")
    message(FATAL_ERROR "compile_slang_metal_embedded could not find slangc")
  endif()
  if(NOT EMBEDDED_METAL_METAL_TOOLCHAIN)
    message(FATAL_ERROR
      "compile_slang_metal_embedded requires the Xcode Metal compiler component")
  endif()
  if(NOT EMBEDDED_METAL_METAL_SDK)
    set(EMBEDDED_METAL_METAL_SDK "macosx")
  endif()

  file(MAKE_DIRECTORY "${OUTPUT_DIR}")
  set(_outputs "")
  set(_air_files "")
  set(_compile_commands "")
  foreach(_entry_spec IN LISTS EMBEDDED_METAL_ENTRY_POINTS)
    string(REPLACE ":" ";" _entry_parts "${_entry_spec}")
    list(LENGTH _entry_parts _entry_part_count)
    if(NOT _entry_part_count EQUAL 2)
      message(FATAL_ERROR
        "Metal entry point '${_entry_spec}' must use the entry:stage form")
    endif()
    list(GET _entry_parts 0 _entry_name)
    list(GET _entry_parts 1 _entry_stage)
    set(_metal_source "${OUTPUT_DIR}/${EMBEDDED_METAL_NAME}.${_entry_name}.metal")
    set(_air_file "${OUTPUT_DIR}/${EMBEDDED_METAL_NAME}.${_entry_name}.air")
    set(_dep_file "${OUTPUT_DIR}/${EMBEDDED_METAL_NAME}.${_entry_name}.dep")
    list(APPEND _outputs "${_metal_source}" "${_air_file}")
    list(APPEND _air_files "${_air_file}")
    list(APPEND _compile_commands
      COMMAND "${Slang_SLANGC_EXECUTABLE}"
        -depfile "${_dep_file}"
        -lang slang
        -target metal
        -matrix-layout-column-major
        -O0
        "$<IF:$<OR:$<CONFIG:Debug>,$<CONFIG:RelWithDebInfo>>,-g1,-g0>"
        -DDEMO_METAL=1
        ${EMBEDDED_METAL_EXTRA_FLAGS}
        -entry "${_entry_name}"
        -stage "${_entry_stage}"
        -o "${_metal_source}"
        "${SOURCE_FILE}"
      COMMAND /usr/bin/xcrun --sdk "${EMBEDDED_METAL_METAL_SDK}" --toolchain
        "${EMBEDDED_METAL_METAL_TOOLCHAIN}" metal
        -std=metal4.0
        "-fmodules-cache-path=${OUTPUT_DIR}/metal-module-cache"
        -c "${_metal_source}"
        -o "${_air_file}")
  endforeach()

  set(_metallib_file "${OUTPUT_DIR}/${EMBEDDED_METAL_NAME}.metallib")
  set(_header_file "${_metallib_file}.h")
  add_custom_command(
    OUTPUT ${_outputs} "${_metallib_file}" "${_header_file}"
    ${_compile_commands}
    COMMAND /usr/bin/xcrun --sdk "${EMBEDDED_METAL_METAL_SDK}" --toolchain
      "${EMBEDDED_METAL_METAL_TOOLCHAIN}" metallib
      ${_air_files}
      -o "${_metallib_file}"
    COMMAND "${CMAKE_COMMAND}"
      "-DINPUT_FILE=${_metallib_file}"
      "-DOUTPUT_FILE=${_header_file}"
      "-DVARIABLE_NAME=${EMBEDDED_METAL_VARIABLE_NAME}"
      -P "${CMAKE_CURRENT_FUNCTION_LIST_DIR}/EmbedBinary.cmake"
    MAIN_DEPENDENCY "${SOURCE_FILE}"
    DEPENDS "${CMAKE_CURRENT_FUNCTION_LIST_DIR}/EmbedBinary.cmake"
    COMMENT "Compiling embedded Metal Slang library: ${EMBEDDED_METAL_NAME}"
    COMMAND_EXPAND_LISTS
    VERBATIM)

  set(${EMBEDDED_METAL_HEADER_VAR} "${_header_file}" PARENT_SCOPE)
endfunction()
