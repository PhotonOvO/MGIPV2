@echo off
setlocal

set "SCRIPT_DIR=%~dp0"
if "%SCRIPT_DIR:~-1%"=="\" set "SCRIPT_DIR=%SCRIPT_DIR:~0,-1%"

set "CONVERTER=%SCRIPT_DIR%\texture_converter.exe"
if not exist "%CONVERTER%" if exist "%SCRIPT_DIR%\..\out\build\x64-Debug\texture_converter.exe" set "CONVERTER=%SCRIPT_DIR%\..\out\build\x64-Debug\texture_converter.exe"
if not exist "%CONVERTER%" if exist "%SCRIPT_DIR%\..\out\build\x64-debug\texture_converter.exe" set "CONVERTER=%SCRIPT_DIR%\..\out\build\x64-debug\texture_converter.exe"
set "DEFAULT_RESOURCE_DIR=%SCRIPT_DIR%\resources"
if not exist "%DEFAULT_RESOURCE_DIR%" if exist "%SCRIPT_DIR%\..\resources" set "DEFAULT_RESOURCE_DIR=%SCRIPT_DIR%\..\resources"

set "INPUT_DIR="
set "OUTPUT_DIR="
set "QUALITY="
set "QUALITY_ARG="

if "%~1"=="" (
  set "INPUT_DIR=%DEFAULT_RESOURCE_DIR%"
  set "OUTPUT_DIR=%DEFAULT_RESOURCE_DIR%"
  set "QUALITY=normal"
) else if /I "%~1"=="fastest" (
  set "INPUT_DIR=%DEFAULT_RESOURCE_DIR%"
  set "OUTPUT_DIR=%DEFAULT_RESOURCE_DIR%"
  set "QUALITY=%~1"
) else if /I "%~1"=="normal" (
  set "INPUT_DIR=%DEFAULT_RESOURCE_DIR%"
  set "OUTPUT_DIR=%DEFAULT_RESOURCE_DIR%"
  set "QUALITY=%~1"
) else if /I "%~1"=="production" (
  set "INPUT_DIR=%DEFAULT_RESOURCE_DIR%"
  set "OUTPUT_DIR=%DEFAULT_RESOURCE_DIR%"
  set "QUALITY=%~1"
) else if /I "%~1"=="highest" (
  set "INPUT_DIR=%DEFAULT_RESOURCE_DIR%"
  set "OUTPUT_DIR=%DEFAULT_RESOURCE_DIR%"
  set "QUALITY=%~1"
) else (
  set "INPUT_DIR=%~1"
  set "OUTPUT_DIR=%~2"
  set "QUALITY=%~3"
  if "%OUTPUT_DIR%"=="" set "OUTPUT_DIR=%INPUT_DIR%"
  if "%QUALITY%"=="" set "QUALITY=normal"
)

if /I "%QUALITY%"=="fastest" set "QUALITY_ARG=--quality fastest"
if /I "%QUALITY%"=="normal" set "QUALITY_ARG=--quality normal"
if /I "%QUALITY%"=="production" set "QUALITY_ARG=--quality production"
if /I "%QUALITY%"=="highest" set "QUALITY_ARG=--quality highest"

if not defined QUALITY_ARG (
  echo Unsupported quality preset: "%QUALITY%"
  goto :usage
)

if not exist "%CONVERTER%" (
  echo texture_converter.exe was not found.
  echo Looked next to this script and under out\build\x64-Debug.
  exit /b 1
)

if not exist "%INPUT_DIR%" (
  echo DDS input directory was not found: "%INPUT_DIR%"
  exit /b 1
)

echo Using texture converter: "%CONVERTER%"
echo DDS input directory: "%INPUT_DIR%"
echo KTX2 output directory: "%OUTPUT_DIR%"
echo Compression quality preset: %QUALITY%

"%CONVERTER%" "%INPUT_DIR%" "%OUTPUT_DIR%" --format bc7 --input-ext .dds %QUALITY_ARG%
exit /b %ERRORLEVEL%

:usage
echo Usage: %~nx0 [fastest^|normal^|production^|highest]
echo        %~nx0 input_dir [output_dir] [fastest^|normal^|production^|highest]
echo Converts DDS files under resources to BC7 KTX2 files when no input_dir is provided.
exit /b 1
