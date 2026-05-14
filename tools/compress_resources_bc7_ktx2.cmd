@echo off
setlocal

set "SCRIPT_DIR=%~dp0"
if "%SCRIPT_DIR:~-1%"=="\" set "SCRIPT_DIR=%SCRIPT_DIR:~0,-1%"

set "CONVERTER=%SCRIPT_DIR%\texture_converter.exe"
set "RESOURCE_DIR=%SCRIPT_DIR%\resources"
set "QUALITY_ARG="

if /I "%~1"=="production" set "QUALITY_ARG=--quality production"
if /I "%~1"=="highest" set "QUALITY_ARG=--quality highest"
if /I "%~1"=="fastest" set "QUALITY_ARG=--quality fastest"
if /I "%~1"=="normal" set "QUALITY_ARG=--quality normal"

if not exist "%CONVERTER%" (
  echo texture_converter.exe was not found: "%CONVERTER%"
  exit /b 1
)

if not exist "%RESOURCE_DIR%" (
  echo resources directory was not found: "%RESOURCE_DIR%"
  exit /b 1
)

echo Using texture converter: "%CONVERTER%"
echo Compressing textures under: "%RESOURCE_DIR%"
if defined QUALITY_ARG (
  echo Compression quality preset: %~1
  "%CONVERTER%" "%RESOURCE_DIR%" "%RESOURCE_DIR%" %QUALITY_ARG%
) else (
  echo Compression quality preset: normal ^(CUDA if available^)
  "%CONVERTER%" "%RESOURCE_DIR%" "%RESOURCE_DIR%"
)

exit /b %ERRORLEVEL%
