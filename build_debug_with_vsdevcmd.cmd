@echo off
setlocal EnableExtensions DisableDelayedExpansion

set "MGIF_DRY_RUN="
if "%~1"=="" goto ArgumentsReady
if /i not "%~1"=="--dry-run" goto Usage
if not "%~2"=="" goto Usage
set "MGIF_DRY_RUN=1"

:ArgumentsReady
if defined MGIF_TOOLCHAIN_SETUP goto CustomToolchain
call "C:\Program Files\Microsoft Visual Studio\2022\Community\Common7\Tools\VsDevCmd.bat" -arch=x64 -host_arch=x64
goto ToolchainReady

:CustomToolchain
call "%MGIF_TOOLCHAIN_SETUP%"

:ToolchainReady
if errorlevel 1 exit /b %errorlevel%
if /i "%VSCMD_ARG_TGT_ARCH%"=="x64" goto ToolchainArchitectureReady
echo ERROR: Toolchain target architecture must be exactly x64; got "%VSCMD_ARG_TGT_ARCH%".
exit /b 1

:ToolchainArchitectureReady
cd /d "%~dp0"
if errorlevel 1 exit /b %errorlevel%

for %%I in ("%CD%") do set "MGIF_CANONICAL_ROOT=%%~fI"
set "MGIF_BUILD_DIR=%MGIF_CANONICAL_ROOT%\out\build\x64-debug"
set "MGIF_CACHE_FILE=%MGIF_BUILD_DIR%\CMakeCache.txt"
set "MGIF_BUILD_GRAPH=%MGIF_BUILD_DIR%\build.ninja"
set "MGIF_DEMO_EXE=%MGIF_BUILD_DIR%\Demo.exe"

call :ConfigureCanonicalTree
if errorlevel 1 exit /b %errorlevel%

call :ValidateCanonicalTree
if errorlevel 1 exit /b %errorlevel%

if defined MGIF_DRY_RUN goto DryRun
set "MGIF_PROOF_TOKEN=%RANDOM%-%RANDOM%"
set "MGIF_PREBUILD_SNAPSHOT=%MGIF_BUILD_DIR%\.mgif-demo-%MGIF_PROOF_TOKEN%.json"
set "MGIF_CURRENTNESS_LOG=%MGIF_BUILD_DIR%\.mgif-demo-%MGIF_PROOF_TOKEN%.log"

call :SnapshotCanonicalDemo
set "MGIF_RESULT=%errorlevel%"
if not "%MGIF_RESULT%"=="0" goto FinishBuild

call cmake --build "%MGIF_BUILD_DIR%" --target Demo
set "MGIF_RESULT=%errorlevel%"
if not "%MGIF_RESULT%"=="0" goto FinishBuild

call :ValidateCanonicalTree
set "MGIF_RESULT=%errorlevel%"
if not "%MGIF_RESULT%"=="0" goto FinishBuild

call cmake --build "%MGIF_BUILD_DIR%" --target Demo -- -n >"%MGIF_CURRENTNESS_LOG%" 2>&1
set "MGIF_RESULT=%errorlevel%"
if not "%MGIF_RESULT%"=="0" goto FinishBuild

call :ValidateCanonicalTree
set "MGIF_RESULT=%errorlevel%"
if not "%MGIF_RESULT%"=="0" goto FinishBuild

call :ValidateCanonicalDemoCurrent
set "MGIF_RESULT=%errorlevel%"

:FinishBuild
call :CleanupProofFiles
exit /b %MGIF_RESULT%

:DryRun
call cmake --build "%MGIF_BUILD_DIR%" --target Demo -- -n
set "MGIF_RESULT=%errorlevel%"
if not "%MGIF_RESULT%"=="0" exit /b %MGIF_RESULT%
call :ValidateCanonicalTree
exit /b %errorlevel%

:ConfigureCanonicalTree
if exist "%MGIF_CACHE_FILE%" exit /b 0
call cmake -S "%MGIF_CANONICAL_ROOT%" -B "%MGIF_BUILD_DIR%" -G Ninja -DCMAKE_BUILD_TYPE=Debug
exit /b %errorlevel%

:ValidateCanonicalTree
call :ValidateCanonicalCache
if errorlevel 1 exit /b %errorlevel%
call :ValidateCanonicalCompilers
if errorlevel 1 exit /b %errorlevel%
call :ValidateCanonicalGraph
exit /b %errorlevel%

:ValidateCanonicalCache
"%SystemRoot%\System32\WindowsPowerShell\v1.0\powershell.exe" -NoLogo -NoProfile -NonInteractive -ExecutionPolicy Bypass -Command ^
 "$ErrorActionPreference = 'Stop'; try {" ^
 "$cache = [IO.File]::ReadAllLines($env:MGIF_CACHE_FILE); function GetValue([string]$name) { $pattern = '^' + [Regex]::Escape($name) + ':[^=]+=(.*)$'; $values = [Collections.Generic.List[string]]::new(); foreach ($line in $cache) { if ($line -cmatch $pattern) { $null = $values.Add($Matches[1]) } }; if ($values.Count -ne 1) { throw ('Cache must contain exactly one ' + $name + ' entry.') }; return $values[0] }; function IsAbsolutePath([string]$value) { return $value -match '^(?:[A-Za-z]:[\\/]|[\\/]{2}[^\\/]+[\\/][^\\/]+)' }; function NormalizePath([string]$value) { return [IO.Path]::GetFullPath($value).TrimEnd([char[]]'\/') };" ^
 "$generator = GetValue 'CMAKE_GENERATOR'; if ($generator -cne 'Ninja') { throw ('Expected CMAKE_GENERATOR=Ninja, got ' + $generator + '.') }; $buildType = GetValue 'CMAKE_BUILD_TYPE'; if ($buildType -cne 'Debug') { throw ('Expected CMAKE_BUILD_TYPE=Debug, got ' + $buildType + '.') };" ^
 "$cacheHomeValue = GetValue 'CMAKE_HOME_DIRECTORY'; if (-not (IsAbsolutePath $cacheHomeValue)) { throw ('CMAKE_HOME_DIRECTORY must be an absolute path: ' + $cacheHomeValue + '.') }; $cacheHome = NormalizePath $cacheHomeValue; $expectedHome = NormalizePath $env:MGIF_CANONICAL_ROOT; if (-not [StringComparer]::OrdinalIgnoreCase.Equals($cacheHome, $expectedHome)) { throw ('CMAKE_HOME_DIRECTORY mismatch: ' + $cacheHome + ' != ' + $expectedHome + '.') }" ^
 "} catch { [Console]::Error.WriteLine(('ERROR: ' + $_.Exception.Message)); exit 1 }"
exit /b %errorlevel%

:ValidateCanonicalCompilers
"%SystemRoot%\System32\WindowsPowerShell\v1.0\powershell.exe" -NoLogo -NoProfile -NonInteractive -ExecutionPolicy Bypass -Command ^
 "$ErrorActionPreference = 'Stop'; try {" ^
 "$cache = [IO.File]::ReadAllLines($env:MGIF_CACHE_FILE); function GetValue([string]$name) { $pattern = '^' + [Regex]::Escape($name) + ':[^=]+=(.*)$'; $values = [Collections.Generic.List[string]]::new(); foreach ($line in $cache) { if ($line -cmatch $pattern) { $null = $values.Add($Matches[1]) } }; if ($values.Count -ne 1) { throw ('Cache must contain exactly one ' + $name + ' entry.') }; return $values[0] }; function IsAbsolutePath([string]$value) { return $value -match '^(?:[A-Za-z]:[\\/]|[\\/]{2}[^\\/]+[\\/][^\\/]+)' }; function NormalizePath([string]$value) { return [IO.Path]::GetFullPath($value).TrimEnd([char[]]'\/') };" ^
 "$version = (GetValue 'CMAKE_CACHE_MAJOR_VERSION') + '.' + (GetValue 'CMAKE_CACHE_MINOR_VERSION') + '.' + (GetValue 'CMAKE_CACHE_PATCH_VERSION'); function ValidateCompiler([string]$language) { $cacheName = 'CMAKE_' + $language + '_COMPILER'; $compilerValue = GetValue $cacheName; if (-not (IsAbsolutePath $compilerValue)) { throw ($cacheName + ' must be an absolute path: ' + $compilerValue + '.') }; $compiler = NormalizePath $compilerValue; $compilerWindows = $compiler.Replace('/', '\'); if ($compilerWindows -notmatch '\\VC\\Tools\\MSVC\\[^\\]+\\bin\\Hostx64\\x64\\cl\.exe$') { throw ($cacheName + ' is not an x64 MSVC cl.exe: ' + $compiler + '.') }; if (-not [IO.File]::Exists($compiler)) { throw ('Cached ' + $cacheName + ' does not exist: ' + $compiler + '.') };" ^
 "$compilerStatePath = Join-Path $env:MGIF_BUILD_DIR ('CMakeFiles\' + $version + '\CMake' + $language + 'Compiler.cmake'); if (-not [IO.File]::Exists($compilerStatePath)) { throw ('Missing active compiler metadata: ' + $compilerStatePath + '.') }; $compilerState = [IO.File]::ReadAllText($compilerStatePath); $compilerPattern = '(?m)^set\(CMAKE_' + $language + '_COMPILER \x22([^\x22]+)\x22\)\r?$'; $compilerStateMatch = [Regex]::Match($compilerState, $compilerPattern); if (-not $compilerStateMatch.Success) { throw ('Active compiler metadata has no exact ' + $cacheName + ' entry.') }; $metadataCompiler = NormalizePath $compilerStateMatch.Groups[1].Value; if (-not [StringComparer]::OrdinalIgnoreCase.Equals($compiler, $metadataCompiler)) { throw ($cacheName + ' cache/metadata mismatch: ' + $compiler + ' != ' + $metadataCompiler + '.') };" ^
 "$idPattern = '(?m)^set\(CMAKE_' + $language + '_COMPILER_ID \x22MSVC\x22\)\r?$'; if (-not [Regex]::IsMatch($compilerState, $idPattern)) { throw ('Active compiler metadata must report CMAKE_' + $language + '_COMPILER_ID=MSVC.') }; $archPattern = '(?m)^set\(CMAKE_' + $language + '_COMPILER_ARCHITECTURE_ID (?:\x22)?x64(?:\x22)?\)\r?$'; if (-not [Regex]::IsMatch($compilerState, $archPattern)) { throw ('Active compiler metadata must report CMAKE_' + $language + '_COMPILER_ARCHITECTURE_ID=x64.') }; return $compiler };" ^
 "$cxxCompiler = ValidateCompiler 'CXX'; $cCompiler = ValidateCompiler 'C'; if (-not [StringComparer]::OrdinalIgnoreCase.Equals($cxxCompiler, $cCompiler)) { throw ('CMAKE_C_COMPILER and CMAKE_CXX_COMPILER must resolve to the same active x64 MSVC cl.exe: ' + $cCompiler + ' != ' + $cxxCompiler + '.') }" ^
 "} catch { [Console]::Error.WriteLine(('ERROR: ' + $_.Exception.Message)); exit 1 }"
exit /b %errorlevel%

:ValidateCanonicalGraph
"%SystemRoot%\System32\WindowsPowerShell\v1.0\powershell.exe" -NoLogo -NoProfile -NonInteractive -ExecutionPolicy Bypass -Command ^
 "$ErrorActionPreference = 'Stop'; try {" ^
 "if (-not [IO.File]::Exists($env:MGIF_BUILD_GRAPH)) { throw ('Missing Ninja build graph: ' + $env:MGIF_BUILD_GRAPH + '.') }; $graph = [IO.File]::ReadAllText($env:MGIF_BUILD_GRAPH); $linkRules = [Regex]::Matches($graph, '(?m)^build ([^:\r\n]+): CXX_EXECUTABLE_LINKER__Demo_Debug(?:[ \t][^\r\n]*)?\r?$'); if ($linkRules.Count -ne 1 -or $linkRules[0].Groups[1].Value.Trim() -cne 'Demo.exe') { throw 'Ninja graph must contain exactly one Debug linker rule whose output is canonical Demo.exe.' }; $aliases = [Regex]::Matches($graph, '(?m)^build Demo: phony ([^\r\n]+)\r?$'); if ($aliases.Count -ne 1 -or $aliases[0].Groups[1].Value.Trim() -cne 'Demo.exe') { throw 'Ninja target Demo must map exactly to canonical Demo.exe.' }" ^
 "} catch { [Console]::Error.WriteLine(('ERROR: ' + $_.Exception.Message)); exit 1 }"
exit /b %errorlevel%

:SnapshotCanonicalDemo
"%SystemRoot%\System32\WindowsPowerShell\v1.0\powershell.exe" -NoLogo -NoProfile -NonInteractive -ExecutionPolicy Bypass -Command ^
 "$ErrorActionPreference = 'Stop'; function GetSha256([string]$path) { $stream = [IO.File]::Open($path, [IO.FileMode]::Open, [IO.FileAccess]::Read, [IO.FileShare]::ReadWrite); $sha = [Security.Cryptography.SHA256]::Create(); try { return ([BitConverter]::ToString($sha.ComputeHash($stream))).Replace('-', '') } finally { $sha.Dispose(); $stream.Dispose() } }; try {" ^
 "$snapshot = [ordered]@{ Exists = $false; Length = 0; CreationTimeUtcTicks = 0; LastWriteTimeUtcTicks = 0; Sha256 = '' }; if ([IO.File]::Exists($env:MGIF_DEMO_EXE)) { $item = Get-Item -LiteralPath $env:MGIF_DEMO_EXE -Force; if ($item.PSIsContainer) { throw ('Canonical Demo path is not a file: ' + $env:MGIF_DEMO_EXE + '.') }; $snapshot.Exists = $true; $snapshot.Length = $item.Length; $snapshot.CreationTimeUtcTicks = $item.CreationTimeUtc.Ticks; $snapshot.LastWriteTimeUtcTicks = $item.LastWriteTimeUtc.Ticks; $snapshot.Sha256 = GetSha256 $env:MGIF_DEMO_EXE } elseif ([IO.Directory]::Exists($env:MGIF_DEMO_EXE)) { throw ('Canonical Demo path is not a file: ' + $env:MGIF_DEMO_EXE + '.') };" ^
 "$json = $snapshot | ConvertTo-Json -Compress; [IO.File]::WriteAllText($env:MGIF_PREBUILD_SNAPSHOT, $json, [Text.UTF8Encoding]::new($false))" ^
 "} catch { [Console]::Error.WriteLine(('ERROR: Could not record pre-build Demo identity: ' + $_.Exception.Message)); exit 1 }"
exit /b %errorlevel%

:ValidateCanonicalDemoCurrent
"%SystemRoot%\System32\WindowsPowerShell\v1.0\powershell.exe" -NoLogo -NoProfile -NonInteractive -ExecutionPolicy Bypass -Command ^
 "$ErrorActionPreference = 'Stop'; function GetSha256([string]$path) { $stream = [IO.File]::Open($path, [IO.FileMode]::Open, [IO.FileAccess]::Read, [IO.FileShare]::ReadWrite); $sha = [Security.Cryptography.SHA256]::Create(); try { return ([BitConverter]::ToString($sha.ComputeHash($stream))).Replace('-', '') } finally { $sha.Dispose(); $stream.Dispose() } }; try {" ^
 "if (-not [IO.File]::Exists($env:MGIF_PREBUILD_SNAPSHOT)) { throw ('Missing pre-build identity snapshot: ' + $env:MGIF_PREBUILD_SNAPSHOT + '.') }; if (-not [IO.File]::Exists($env:MGIF_CURRENTNESS_LOG)) { throw ('Missing post-build dry-run log: ' + $env:MGIF_CURRENTNESS_LOG + '.') }; if (-not [IO.File]::Exists($env:MGIF_DEMO_EXE)) { throw ('Successful Demo target did not produce canonical ' + $env:MGIF_DEMO_EXE + '.') };" ^
 "$item = Get-Item -LiteralPath $env:MGIF_DEMO_EXE -Force; if ($item.PSIsContainer) { throw ('Canonical Demo path is not a file: ' + $env:MGIF_DEMO_EXE + '.') }; $before = ConvertFrom-Json ([IO.File]::ReadAllText($env:MGIF_PREBUILD_SNAPSHOT)); $hash = GetSha256 $env:MGIF_DEMO_EXE; $changed = (-not [bool]$before.Exists) -or ([long]$before.Length -ne $item.Length) -or ([long]$before.CreationTimeUtcTicks -ne $item.CreationTimeUtc.Ticks) -or ([long]$before.LastWriteTimeUtcTicks -ne $item.LastWriteTimeUtc.Ticks) -or ([string]$before.Sha256 -cne $hash);" ^
 "$dryRun = [IO.File]::ReadAllText($env:MGIF_CURRENTNESS_LOG); if ([String]::IsNullOrWhiteSpace($dryRun)) { throw 'Post-build dry-run produced no currentness evidence.' }; $linkPending = [Regex]::IsMatch($dryRun, '(?im)^\s*(?:\[[^\]]+\]\s*)?Linking CXX executable Demo\.exe\s*$'); if (-not $changed -and $linkPending) { throw 'Canonical Demo.exe was unchanged and the post-build dry-run still schedules its linker rule.' }; if ($changed) { Write-Output 'Canonical Demo.exe was newly produced or updated by the successful build.' } else { Write-Output 'Canonical Demo.exe is unchanged and definitively current in the validated Ninja graph.' }" ^
 "} catch { [Console]::Error.WriteLine(('ERROR: ' + $_.Exception.Message)); exit 1 }"
exit /b %errorlevel%

:CleanupProofFiles
"%SystemRoot%\System32\WindowsPowerShell\v1.0\powershell.exe" -NoLogo -NoProfile -NonInteractive -ExecutionPolicy Bypass -Command ^
 "$ErrorActionPreference = 'SilentlyContinue'; if ($env:MGIF_PREBUILD_SNAPSHOT) { Remove-Item -LiteralPath $env:MGIF_PREBUILD_SNAPSHOT -Force }; if ($env:MGIF_CURRENTNESS_LOG) { Remove-Item -LiteralPath $env:MGIF_CURRENTNESS_LOG -Force }"
exit /b 0

:Usage
echo Usage: %~nx0 [--dry-run]
exit /b 2
