@echo off
setlocal

set "SCRIPT_DIR=%~dp0"
if "%SCRIPT_DIR:~-1%"=="\" set "SCRIPT_DIR=%SCRIPT_DIR:~0,-1%"

set "CONVERTER=%SCRIPT_DIR%\texture_converter.exe"
set "RESOURCE_DIR=%SCRIPT_DIR%\resources"
set "ASTC_BLOCK=6x6"
set "QUALITY=normal"

for %%A in (%*) do (
  if /I "%%~A"=="4x4" set "ASTC_BLOCK=4x4"
  if /I "%%~A"=="5x4" set "ASTC_BLOCK=5x4"
  if /I "%%~A"=="5x5" set "ASTC_BLOCK=5x5"
  if /I "%%~A"=="6x5" set "ASTC_BLOCK=6x5"
  if /I "%%~A"=="6x6" set "ASTC_BLOCK=6x6"
  if /I "%%~A"=="8x5" set "ASTC_BLOCK=8x5"
  if /I "%%~A"=="8x6" set "ASTC_BLOCK=8x6"
  if /I "%%~A"=="8x8" set "ASTC_BLOCK=8x8"
  if /I "%%~A"=="10x5" set "ASTC_BLOCK=10x5"
  if /I "%%~A"=="10x6" set "ASTC_BLOCK=10x6"
  if /I "%%~A"=="10x8" set "ASTC_BLOCK=10x8"
  if /I "%%~A"=="10x10" set "ASTC_BLOCK=10x10"
  if /I "%%~A"=="12x10" set "ASTC_BLOCK=12x10"
  if /I "%%~A"=="12x12" set "ASTC_BLOCK=12x12"
  if /I "%%~A"=="fastest" set "QUALITY=fastest"
  if /I "%%~A"=="normal" set "QUALITY=normal"
  if /I "%%~A"=="production" set "QUALITY=production"
  if /I "%%~A"=="highest" set "QUALITY=highest"
)

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
echo Compression format: ASTC %ASTC_BLOCK%
echo Compression quality preset: %QUALITY%
"%CONVERTER%" "%RESOURCE_DIR%" "%RESOURCE_DIR%" --format astc --astc-block %ASTC_BLOCK% --quality %QUALITY%

exit /b %ERRORLEVEL%
