@echo off
setlocal EnableExtensions EnableDelayedExpansion

set "ROOT_DIR=%~dp0"
set "ANDROID_PROJECT_DIR=%ROOT_DIR%android"
set "APP_BUILD_GRADLE=%ANDROID_PROJECT_DIR%\app\build.gradle"
set "LOCAL_PROPERTIES=%ANDROID_PROJECT_DIR%\local.properties"
set "REQUIRED_NDK=23.1.7779620"

echo [VKDemo] Preparing Android Gradle project
echo.

if not exist "%ANDROID_PROJECT_DIR%\settings.gradle" (
  echo [ERROR] Android Gradle project was not found:
  echo         %ANDROID_PROJECT_DIR%
  exit /b 1
)

if not exist "%APP_BUILD_GRADLE%" (
  echo [ERROR] Missing app Gradle file:
  echo         %APP_BUILD_GRADLE%
  exit /b 1
)

call :FindAndroidSdk
if errorlevel 1 exit /b 1

call :WriteLocalProperties
if errorlevel 1 exit /b 1

call :CheckAndroidSdkTools
call :CheckCachedDeps
call :CheckVulkanSdk
call :CheckGradle

echo.
echo [OK] Android Gradle project is ready:
echo      %ANDROID_PROJECT_DIR%
echo.
echo Open this folder in Android Studio, or run Gradle from this directory:
echo      cd /d "%ANDROID_PROJECT_DIR%"
echo      gradle assembleDebug
echo.
echo The native target is DemoAndroid and the ABI is arm64-v8a.
exit /b 0

:FindAndroidSdk
set "ANDROID_SDK="

if not defined ANDROID_SDK (
  if defined LOCALAPPDATA (
    if exist "%LOCALAPPDATA%\Android\Sdk\platforms" set "ANDROID_SDK=%LOCALAPPDATA%\Android\Sdk"
  )
)

if not defined ANDROID_SDK (
  if defined USERPROFILE (
    if exist "%USERPROFILE%\AppData\Local\Android\Sdk\platforms" set "ANDROID_SDK=%USERPROFILE%\AppData\Local\Android\Sdk"
  )
)

if not defined ANDROID_SDK (
  if defined ANDROID_SDK_ROOT (
    if exist "%ANDROID_SDK_ROOT%\platforms" set "ANDROID_SDK=%ANDROID_SDK_ROOT%"
  )
)

if not defined ANDROID_SDK (
  if defined ANDROID_HOME (
    if exist "%ANDROID_HOME%\platforms" set "ANDROID_SDK=%ANDROID_HOME%"
  )
)

if not defined ANDROID_SDK (
  echo [ERROR] Android SDK was not found.
  echo         Set ANDROID_SDK_ROOT or ANDROID_HOME, or install Android Studio SDK.
  exit /b 1
)

echo [OK] Android SDK: %ANDROID_SDK%
exit /b 0

:WriteLocalProperties
set "SDK_DIR_FOR_GRADLE=%ANDROID_SDK:\=/%"
> "%LOCAL_PROPERTIES%" echo sdk.dir=%SDK_DIR_FOR_GRADLE%
if errorlevel 1 (
  echo [ERROR] Failed to write:
  echo         %LOCAL_PROPERTIES%
  exit /b 1
)

echo [OK] Wrote android\local.properties
exit /b 0

:CheckAndroidSdkTools
if exist "%ANDROID_SDK%\ndk\%REQUIRED_NDK%" (
  echo [OK] NDK: %REQUIRED_NDK%
) else (
  echo [WARN] Required NDK is missing: %REQUIRED_NDK%
  echo        Install it in Android Studio SDK Manager or run:
  echo        sdkmanager "ndk;%REQUIRED_NDK%"
)

if exist "%ANDROID_SDK%\cmake\3.22.1" (
  echo [OK] CMake: 3.22.1
) else (
  echo [WARN] SDK CMake 3.22.1 is not installed.
  echo        Android Gradle Plugin may install/use another compatible CMake.
)

if exist "%ANDROID_SDK%\platforms\android-35" (
  echo [OK] Android platform: android-35
) else (
  echo [WARN] Android platform android-35 is missing.
)

if exist "%ANDROID_SDK%\build-tools\35.0.0" (
  echo [OK] Build tools: 35.0.0
) else (
  echo [WARN] Build tools 35.0.0 are missing.
)
exit /b 0

:CheckCachedDeps
set "CACHED_DEPS=%ROOT_DIR%out\build\x64-Debug\_deps"
if exist "%CACHED_DEPS%" (
  echo [OK] Cached desktop dependencies: out\build\x64-Debug\_deps
) else (
  echo [WARN] Cached dependencies were not found:
  echo        %CACHED_DEPS%
  echo        Gradle/CMake may download FetchContent dependencies if network is available.
)
exit /b 0

:CheckVulkanSdk
set "VULKAN_INCLUDE="
if defined VULKAN_SDK (
  if exist "%VULKAN_SDK%\Include\vulkan\vulkan_core.h" set "VULKAN_INCLUDE=%VULKAN_SDK%\Include"
)

if not defined VULKAN_INCLUDE (
  for /f "delims=" %%D in ('dir /b /ad "C:\VulkanSDK" 2^>nul') do (
    if exist "C:\VulkanSDK\%%D\Include\vulkan\vulkan_core.h" set "VULKAN_INCLUDE=C:\VulkanSDK\%%D\Include"
  )
)

if defined VULKAN_INCLUDE (
  echo [OK] Vulkan headers: %VULKAN_INCLUDE%
) else (
  echo [WARN] Vulkan SDK headers were not found.
  echo        Install Vulkan SDK or set VULKAN_SDK if CMake cannot find vulkan_core.h.
)
exit /b 0

:CheckGradle
where gradle >nul 2>nul
if not errorlevel 1 (
  for /f "delims=" %%G in ('where gradle 2^>nul') do (
    echo [OK] Gradle command: %%G
    exit /b 0
  )
)

if exist "%ANDROID_PROJECT_DIR%\gradlew.bat" (
  echo [OK] Gradle wrapper: android\gradlew.bat
) else (
  echo [WARN] No gradle command or android\gradlew.bat was found.
  echo        Android Studio can still import the project.
)
exit /b 0
