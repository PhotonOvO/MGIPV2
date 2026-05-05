# FindKtxCli.cmake
#
# Locates the KTX-Software `ktx` command line tool.
#
# Resolution order:
# 1. Reuse an explicit KtxCli_EXECUTABLE cache entry if it still exists.
# 2. Reuse a `ktx` already available on PATH.
# 3. On Windows, download the official KTX-Software installer and install it
#    into the build tree under `_deps`.
#
# Sets the following variables:
#   KtxCli_VERSION     : Requested KTX-Software version.
#   KtxCli_ROOT        : Root directory that contains the installed tool bundle.
#   KtxCli_EXECUTABLE  : Full path to the `ktx` executable.

set(KtxCli_VERSION "4.4.2" CACHE STRING "KTX-Software version used for the texture conversion CLI")
mark_as_advanced(KtxCli_VERSION)

set(KtxCli_EXECUTABLE "" CACHE FILEPATH "Path to the KTX-Software ktx executable")

if(KtxCli_EXECUTABLE AND EXISTS "${KtxCli_EXECUTABLE}")
  get_filename_component(_KTXCLI_BIN_DIR "${KtxCli_EXECUTABLE}" DIRECTORY)
  get_filename_component(KtxCli_ROOT "${_KTXCLI_BIN_DIR}" DIRECTORY)
else()
  unset(KtxCli_EXECUTABLE CACHE)
  find_program(KtxCli_EXECUTABLE
    NAMES ktx
    DOC "KTX-Software ktx executable"
  )

  if(KtxCli_EXECUTABLE)
    get_filename_component(_KTXCLI_BIN_DIR "${KtxCli_EXECUTABLE}" DIRECTORY)
    get_filename_component(KtxCli_ROOT "${_KTXCLI_BIN_DIR}" DIRECTORY)
  elseif(WIN32)
    include(DownloadPackage)

    string(TOLOWER "${CMAKE_SYSTEM_PROCESSOR}" KTXCLI_ARCH_PROC)
    if(KTXCLI_ARCH_PROC MATCHES "^(arm|aarch64)")
      set(_KTXCLI_WINDOWS_ARCH "arm64")
    elseif(KTXCLI_ARCH_PROC MATCHES "^(x86_64|amd64|i[3-6]86)")
      set(_KTXCLI_WINDOWS_ARCH "x64")
    else()
      message(FATAL_ERROR "Unhandled architecture '${CMAKE_SYSTEM_PROCESSOR}' for KTX-Software download")
    endif()

    set(_KTXCLI_PACKAGE_BASENAME "KTX-Software-${KtxCli_VERSION}-Windows-${_KTXCLI_WINDOWS_ARCH}")
    set(_KTXCLI_PACKAGE_FILENAME "${_KTXCLI_PACKAGE_BASENAME}.exe")
    set(_KTXCLI_BUNDLE_DIR "${CMAKE_BINARY_DIR}/_deps/${_KTXCLI_PACKAGE_BASENAME}")
    set(KtxCli_ROOT "${_KTXCLI_BUNDLE_DIR}/install" CACHE PATH "Path to the downloaded KTX-Software CLI bundle")
    mark_as_advanced(KtxCli_ROOT)

    set(_KTXCLI_EXECUTABLE_CANDIDATE "${KtxCli_ROOT}/bin/ktx.exe")
    if(NOT EXISTS "${_KTXCLI_EXECUTABLE_CANDIDATE}")
      download_files(
        FILENAMES "${_KTXCLI_PACKAGE_FILENAME}"
        URLS "https://github.com/KhronosGroup/KTX-Software/releases/download/v${KtxCli_VERSION}/${_KTXCLI_PACKAGE_FILENAME}"
        TARGET_DIR "${_KTXCLI_BUNDLE_DIR}"
        NOINSTALL
      )

      file(MAKE_DIRECTORY "${KtxCli_ROOT}")
      file(TO_NATIVE_PATH "${KtxCli_ROOT}" _KTXCLI_INSTALL_DIR_NATIVE)
      file(TO_NATIVE_PATH "${_KTXCLI_BUNDLE_DIR}/${_KTXCLI_PACKAGE_FILENAME}" _KTXCLI_INSTALLER_NATIVE)
      execute_process(
        COMMAND cmd /c ${_KTXCLI_INSTALLER_NATIVE} /S /D=${_KTXCLI_INSTALL_DIR_NATIVE}
        RESULT_VARIABLE _KTXCLI_INSTALL_RESULT
        OUTPUT_VARIABLE _KTXCLI_INSTALL_STDOUT
        ERROR_VARIABLE _KTXCLI_INSTALL_STDERR
      )

      if(NOT _KTXCLI_INSTALL_RESULT EQUAL 0)
        message(FATAL_ERROR
          "Failed to install KTX-Software from '${_KTXCLI_PACKAGE_FILENAME}' into '${KtxCli_ROOT}'.\n"
          "Exit code: ${_KTXCLI_INSTALL_RESULT}\n"
          "stdout:\n${_KTXCLI_INSTALL_STDOUT}\n"
          "stderr:\n${_KTXCLI_INSTALL_STDERR}")
      endif()

      if(NOT EXISTS "${_KTXCLI_EXECUTABLE_CANDIDATE}")
        message(FATAL_ERROR
          "KTX-Software installer completed but '${_KTXCLI_EXECUTABLE_CANDIDATE}' was not found afterwards.")
      endif()
    endif()

    set(KtxCli_EXECUTABLE "${_KTXCLI_EXECUTABLE_CANDIDATE}" CACHE FILEPATH "Path to the KTX-Software ktx executable" FORCE)
  else()
    message(FATAL_ERROR
      "KTX-Software CLI was not found on PATH and automatic download is only implemented for Windows in this project.")
  endif()
endif()

include(FindPackageHandleStandardArgs)
find_package_handle_standard_args(KtxCli
  REQUIRED_VARS
    KtxCli_EXECUTABLE
    KtxCli_ROOT
  VERSION_VAR
    KtxCli_VERSION
)
