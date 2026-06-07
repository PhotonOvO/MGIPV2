[CmdletBinding()]
param(
  [string]$OutputDir,
  [string]$ProjectName = "VKDemoAndroid",
  [string]$ApplicationId = "com.vkdemo",
  [string]$Namespace = "com.vkdemo",
  [int]$CompileSdk = 35,
  [int]$MinSdk = 26,
  [int]$TargetSdk = 35,
  [string]$NdkVersion = "23.1.7779620",
  [string]$AndroidGradlePluginVersion = "8.12.3",
  [string[]]$AbiFilters = @("arm64-v8a"),
  [string]$SdkDir,
  [switch]$UpdateLocalProperties
)

$ErrorActionPreference = "Stop"

function Write-Utf8NoBomFile {
  param(
    [Parameter(Mandatory = $true)][string]$Path,
    [Parameter(Mandatory = $true)][string]$Content
  )

  $parent = Split-Path -Parent $Path
  if ($parent -and -not (Test-Path -LiteralPath $parent)) {
    New-Item -ItemType Directory -Force -Path $parent | Out-Null
  }

  $body = $Content.TrimStart("`r", "`n").TrimEnd("`r", "`n")
  $lineEnding = "`r`n"
  $normalizedContent = [regex]::Replace($body, "\r\n|\r|\n", $lineEnding) + $lineEnding
  $encoding = New-Object System.Text.UTF8Encoding($false)
  [System.IO.File]::WriteAllText($Path, $normalizedContent, $encoding)
}

function Convert-ToGradlePath {
  param([Parameter(Mandatory = $true)][string]$Path)
  return $Path.Replace("\", "/")
}

$scriptDir = Split-Path -Parent $MyInvocation.MyCommand.Path
$repoRoot = Resolve-Path (Join-Path $scriptDir "..")

if (-not $OutputDir) {
  $OutputDir = Join-Path $repoRoot "android"
}

$outputRoot = if ([System.IO.Path]::IsPathRooted($OutputDir)) {
  $OutputDir
} else {
  Join-Path $repoRoot $OutputDir
}

$cmakeListsPath = Join-Path $repoRoot "CMakeLists.txt"
if (-not (Test-Path -LiteralPath $cmakeListsPath)) {
  throw "CMakeLists.txt was not found at $cmakeListsPath"
}

$nativeSourcePath = Join-Path $repoRoot "app\AndroidNativeApp.cpp"
if (-not (Test-Path -LiteralPath $nativeSourcePath)) {
  throw "Android native entry point was not found at $nativeSourcePath"
}

$manifestDir = Join-Path $outputRoot "app\src\main"

$settingsGradle = @'
pluginManagement {
    repositories {
        maven { url = file("local-maven") }
        google()
        mavenCentral()
        gradlePluginPortal()
    }
}

dependencyResolutionManagement {
    repositoriesMode.set(RepositoriesMode.FAIL_ON_PROJECT_REPOS)
    repositories {
        maven { url = file("local-maven") }
        google()
        mavenCentral()
    }
}

rootProject.name = "@@PROJECT_NAME@@"
include ":app"
'@
$settingsGradle = $settingsGradle.Replace("@@PROJECT_NAME@@", $ProjectName)

$rootBuildGradle = @'
plugins {
    id "com.android.application" version "@@AGP_VERSION@@" apply false
}
'@
$rootBuildGradle = $rootBuildGradle.Replace("@@AGP_VERSION@@", $AndroidGradlePluginVersion)

$abiFilterList = ($AbiFilters | ForEach-Object { '"' + $_ + '"' }) -join ", "

$appBuildGradle = @'
plugins {
    id "com.android.application"
}

def repoRoot = rootProject.file("..")
def cachedDeps = rootProject.file("../out/build/x64-Debug/_deps")
def cmakeArguments = [
        "-DANDROID_STL=c++_shared",
        "-DUSE_SLANG=ON"
]

def addCmakePathIfExists = { name, path ->
    if (path.exists()) {
        cmakeArguments.add("-D${name}=${path.absolutePath.replace('\\', '/')}")
    }
}

addCmakePathIfExists("Slang_ROOT", new File(cachedDeps, "Slang-windows-x86_64-2026.3.1"))
addCmakePathIfExists("FETCHCONTENT_SOURCE_DIR_GLM", new File(cachedDeps, "glm-src"))
addCmakePathIfExists("FETCHCONTENT_SOURCE_DIR_VOLK", new File(cachedDeps, "volk-src"))
addCmakePathIfExists("FETCHCONTENT_SOURCE_DIR_TINYGLTF", new File(cachedDeps, "tinygltf-src"))
addCmakePathIfExists("FETCHCONTENT_SOURCE_DIR_MESHOPTIMIZER", new File(cachedDeps, "meshoptimizer-src"))
addCmakePathIfExists("FETCHCONTENT_SOURCE_DIR_VULKANMEMORYALLOCATOR", new File(cachedDeps, "vulkanmemoryallocator-src"))
addCmakePathIfExists("FETCHCONTENT_SOURCE_DIR_IMGUI", new File(cachedDeps, "imgui-src"))

def vulkanSdkRoot = new File("C:/VulkanSDK")
if (vulkanSdkRoot.exists()) {
    def vulkanSdkVersions = vulkanSdkRoot.listFiles()
            ?.findAll { new File(it, "Include/vulkan/vulkan_core.h").exists() }
            ?.sort { a, b -> a.name <=> b.name }
    if (vulkanSdkVersions && !vulkanSdkVersions.isEmpty()) {
        addCmakePathIfExists("DEMO_VULKAN_HEADER_DIR", new File(vulkanSdkVersions.last(), "Include"))
    }
}

android {
    namespace "@@NAMESPACE@@"
    compileSdk @@COMPILE_SDK@@
    ndkVersion "@@NDK_VERSION@@"

    defaultConfig {
        applicationId "@@APPLICATION_ID@@"
        minSdk @@MIN_SDK@@
        targetSdk @@TARGET_SDK@@
        versionCode 1
        versionName "1.0"

        externalNativeBuild {
            cmake {
                arguments.addAll(cmakeArguments)
                targets "DemoAndroid"
            }
        }

        ndk {
            abiFilters @@ABI_FILTERS@@
        }
    }

    externalNativeBuild {
        cmake {
            path "../../CMakeLists.txt"
        }
    }

    sourceSets {
        main {
            assets.srcDirs = [new File(repoRoot, "resources")]
        }
    }
}
'@
$appBuildGradle = $appBuildGradle.
  Replace("@@NAMESPACE@@", $Namespace).
  Replace("@@COMPILE_SDK@@", [string]$CompileSdk).
  Replace("@@NDK_VERSION@@", $NdkVersion).
  Replace("@@APPLICATION_ID@@", $ApplicationId).
  Replace("@@MIN_SDK@@", [string]$MinSdk).
  Replace("@@TARGET_SDK@@", [string]$TargetSdk).
  Replace("@@ABI_FILTERS@@", $abiFilterList)

$manifest = @'
<manifest xmlns:android="http://schemas.android.com/apk/res/android">
    <uses-feature android:name="android.hardware.vulkan.version" android:version="0x00400000" android:required="true" />

    <application
        android:allowBackup="false"
        android:hasCode="false"
        android:label="VKDemo">
        <activity
            android:name="android.app.NativeActivity"
            android:configChanges="keyboard|keyboardHidden|orientation|screenSize"
            android:exported="true">
            <meta-data
                android:name="android.app.lib_name"
                android:value="DemoAndroid" />
            <intent-filter>
                <action android:name="android.intent.action.MAIN" />
                <category android:name="android.intent.category.LAUNCHER" />
            </intent-filter>
        </activity>
    </application>
</manifest>
'@

Write-Utf8NoBomFile -Path (Join-Path $outputRoot "settings.gradle") -Content $settingsGradle
Write-Utf8NoBomFile -Path (Join-Path $outputRoot "build.gradle") -Content $rootBuildGradle
Write-Utf8NoBomFile -Path (Join-Path $outputRoot "app\build.gradle") -Content $appBuildGradle
Write-Utf8NoBomFile -Path (Join-Path $manifestDir "AndroidManifest.xml") -Content $manifest

$localPropertiesPath = Join-Path $outputRoot "local.properties"
if (-not $SdkDir) {
  if ($env:ANDROID_HOME) {
    $SdkDir = $env:ANDROID_HOME
  } elseif ($env:ANDROID_SDK_ROOT) {
    $SdkDir = $env:ANDROID_SDK_ROOT
  }
}

if ($SdkDir) {
  if ($UpdateLocalProperties -or -not (Test-Path -LiteralPath $localPropertiesPath)) {
    $sdkPath = Convert-ToGradlePath (Resolve-Path $SdkDir)
    Write-Utf8NoBomFile -Path $localPropertiesPath -Content "sdk.dir=$sdkPath`n"
  }
}

Write-Host "Generated Android Gradle project:"
Write-Host "  $outputRoot"
Write-Host ""
Write-Host "Build from the generated project with:"
Write-Host "  cd $outputRoot"
Write-Host "  gradle :app:assembleDebug"
