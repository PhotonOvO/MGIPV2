# Vulkan CSM Validation Smoke Runbook

本文只定义可复现流程，不记录任何一次运行的最终通过或失败结论。每次运行的结论必须保存在该次独立证据目录中。

## 1. 固定环境与硬约束

- 仓库根目录：`G:\MGIF`
- CMake 生成器：`Ninja`
- 构建类型：`Debug`
- 构建目录：`G:\MGIF\out\build\x64-debug`
- 唯一受测可执行文件：`G:\MGIF\out\build\x64-debug\Demo.exe`
- Debug 构建会启用 `VK_LAYER_KHRONOS_validation`，无需额外设置 `VK_INSTANCE_LAYERS`。
- `--no-ui` 只关闭 ImGui 绘制，不是 headless 模式；测试仍会创建 GLFW/Vulkan 窗口。
- 本流程不启动、不附加、不关闭、不终止 `qrenderdoc.exe`。禁止按进程名执行 `taskkill`；超时时只能终止当前脚本持有句柄的那个 `Demo.exe`。

先确认缓存和可执行文件位置：

```cmd
cd /d G:\MGIF
findstr /B "CMAKE_GENERATOR:INTERNAL= CMAKE_BUILD_TYPE:STRING=" out\build\x64-debug\CMakeCache.txt
if not exist out\build\x64-debug\Demo.exe exit /b 1
```

期望值：

```text
CMAKE_BUILD_TYPE:STRING=Debug
CMAKE_GENERATOR:INTERNAL=Ninja
```

## 2. 在同一个 cmd.exe 进程中构建

Visual Studio x64 开发环境和构建命令必须位于同一个 `cmd.exe` 进程。不要在一个临时 `cmd /c` 中调用 `VsDevCmd.bat` 后，再回到原 PowerShell 构建；后者不会继承 MSVC 的 `INCLUDE`、`LIB` 和工具链环境。

```cmd
cmd.exe
call "C:\Program Files\Microsoft Visual Studio\2022\Community\Common7\Tools\VsDevCmd.bat" -arch=x64 -host_arch=x64
if errorlevel 1 exit /b 1

cd /d G:\MGIF
findstr /B "CMAKE_GENERATOR:INTERNAL= CMAKE_BUILD_TYPE:STRING=" out\build\x64-debug\CMakeCache.txt

cmake --build out\build\x64-debug --parallel 2 --target Demo
if errorlevel 1 exit /b 1
if not exist out\build\x64-debug\Demo.exe exit /b 1
```

仅当 `out\build\x64-debug\CMakeCache.txt` 不存在或确实需要重新配置时，才在上述同一开发环境中先执行：

```cmd
cmake -S . -B out\build\x64-debug -G Ninja -DCMAKE_BUILD_TYPE=Debug -DBUILD_TESTING=ON -DUSE_SLANG=ON
if errorlevel 1 exit /b 1
```

## 3. 五组固定 CSM 8/24/8 自动化矩阵

所有场景固定使用：

```text
--fixed-dt=0.0166666667
--warmup-frames=8
--motion-frames=24
--hold-frames=8
--no-ui
--auto-exit
```

应用层参数必须使用 `--automation=...`、`--no-post`、`--taa` 和 `--no-ddgi`。不要使用外层 RenderDoc harness 的 `--mode`、`--render-mode` 或 `--gi-mode`。

| Case | 相机运动 | 后处理 | DDGI | 额外应用参数 |
|---|---|---|---|---|
| `01-translate-no-post-no-ddgi` | 平移后停止 | no-post | 关闭 | `--automation=csm-translate-stop --no-post --no-ddgi` |
| `02-rotate-no-post-no-ddgi` | 旋转后停止 | no-post | 关闭 | `--automation=csm-rotate-stop --no-post --no-ddgi` |
| `03-translate-taa-no-ddgi` | 平移后停止 | TAA | 关闭 | `--automation=csm-translate-stop --taa --no-ddgi` |
| `04-rotate-taa-no-ddgi` | 旋转后停止 | TAA | 关闭 | `--automation=csm-rotate-stop --taa --no-ddgi` |
| `05-rotate-taa-ddgi` | 旋转后停止 | TAA | 开启 | `--automation=csm-rotate-stop --taa` |

`--no-post` 与 `--taa` 互斥。第五组通过不传 `--no-ddgi` 来启用 DDGI。

## 4. 边界帧与必须出现的 marker

帧号为零起始：

```text
last-moving = 8 + 24 - 1 = 31
first-still = 32
settled     = 8 + 24 + 8 - 1 = 39
complete    = 40 frames
```

每组日志必须各出现一次并保持以下顺序：

```regex
\[CSM_AUTOMATION\] marker=config mode=csm-(?:translate|rotate)-stop fixed_dt=0\.0166666\d+ warmup=8 motion=24 hold=8 no_ui=1 no_post=[01] no_ddgi=[01] taa=[01] auto_exit=1
\[CSM_AUTOMATION\] marker=scene-ready mode=csm-(?:translate|rotate)-stop model=resources/GLTF_Sponza/sponza\.gltf
\[CSM_AUTOMATION\] marker=start mode=csm-(?:translate|rotate)-stop frame=0
\[CSM_AUTOMATION\] marker=last-moving mode=csm-(?:translate|rotate)-stop frame=31\b
\[CSM_AUTOMATION\] marker=first-still mode=csm-(?:translate|rotate)-stop frame=32\b
\[CSM_AUTOMATION\] marker=settled mode=csm-(?:translate|rotate)-stop frame=39\b
\[CSM_AUTOMATION\] marker=complete mode=csm-(?:translate|rotate)-stop frames=40\b
```

第五组还必须出现 DDGI SDF 正向证据：

```regex
Loaded DDGI mesh SDF: .*sponza_(?:sdf|SDF)\.bin
```

前四组传入了 `--no-ddgi`，不得出现上述 SDF 加载成功 marker。

## 5. Synchronization Validation 与自动证据采集

以下 PowerShell 脚本在完成构建后运行。它会启用 synchronization validation，绑定 Demo SHA-256，顺序执行五组自动化，并检查 exit code、超时、marker、DDGI SDF 和失败正则。

```powershell
Set-StrictMode -Version Latest
$ErrorActionPreference = 'Stop'

$Repo = (Resolve-Path -LiteralPath 'G:\MGIF').Path
$Demo = (Resolve-Path -LiteralPath (Join-Path $Repo 'out\build\x64-debug\Demo.exe')).Path
$DemoItem = Get-Item -LiteralPath $Demo
$ExpectedSha = (Get-FileHash -Algorithm SHA256 -LiteralPath $Demo).Hash.ToLowerInvariant()
$Stamp = Get-Date -Format 'yyyyMMdd-HHmmss'
$EvidenceDir = Join-Path $Repo (
    'out\validation\csm-vulkan-smoke-{0}-{1}' -f $Stamp, $ExpectedSha.Substring(0, 12)
)
New-Item -ItemType Directory -Path $EvidenceDir -Force | Out-Null

$env:VK_KHRONOS_VALIDATION_VALIDATE_SYNC = 'true'
$env:VK_KHRONOS_VALIDATION_SYNCVAL_SUBMIT_TIME_VALIDATION = 'true'

$Identity = [ordered]@{
    created_at = (Get-Date).ToString('o')
    repository_root = $Repo
    git_head = (git -C $Repo rev-parse HEAD).Trim()
    generator = 'Ninja'
    configuration = 'Debug'
    demo_path = $Demo
    demo_size_bytes = $DemoItem.Length
    demo_sha256 = $ExpectedSha
    validation_environment = [ordered]@{
        VK_KHRONOS_VALIDATION_VALIDATE_SYNC = $env:VK_KHRONOS_VALIDATION_VALIDATE_SYNC
        VK_KHRONOS_VALIDATION_SYNCVAL_SUBMIT_TIME_VALIDATION =
            $env:VK_KHRONOS_VALIDATION_SYNCVAL_SUBMIT_TIME_VALIDATION
    }
}
$Identity | ConvertTo-Json -Depth 5 |
    Set-Content -LiteralPath (Join-Path $EvidenceDir 'identity.json') -Encoding utf8

$Common = @(
    '--fixed-dt=0.0166666667',
    '--warmup-frames=8',
    '--motion-frames=24',
    '--hold-frames=8',
    '--no-ui',
    '--auto-exit'
)

$Cases = @(
    [pscustomobject]@{ Name='01-translate-no-post-no-ddgi'; Mode='csm-translate-stop'; Extra=@('--no-post','--no-ddgi'); NoPost=1; NoDdgi=1; Taa=0; RequireSdf=$false },
    [pscustomobject]@{ Name='02-rotate-no-post-no-ddgi';    Mode='csm-rotate-stop';    Extra=@('--no-post','--no-ddgi'); NoPost=1; NoDdgi=1; Taa=0; RequireSdf=$false },
    [pscustomobject]@{ Name='03-translate-taa-no-ddgi';     Mode='csm-translate-stop'; Extra=@('--taa','--no-ddgi');     NoPost=0; NoDdgi=1; Taa=1; RequireSdf=$false },
    [pscustomobject]@{ Name='04-rotate-taa-no-ddgi';        Mode='csm-rotate-stop';    Extra=@('--taa','--no-ddgi');     NoPost=0; NoDdgi=1; Taa=1; RequireSdf=$false },
    [pscustomobject]@{ Name='05-rotate-taa-ddgi';           Mode='csm-rotate-stop';    Extra=@('--taa');                 NoPost=0; NoDdgi=0; Taa=1; RequireSdf=$true  }
)

$FailureRegex = [regex]::new(
    '(?im)(VUID-|SYNC-HAZARD|Vulkan validation (?:ERROR|WARNING)/|' +
    'Vulkan error:|VK_ERROR(?:_[A-Z0-9_]+)?|device lost|' +
    'Frame failed during phase|Failed to create Vulkan instance|' +
    'Could not initialize GLFW|GLFW: Vulkan not supported|' +
    'Failed to load model:|Failed to load DDGI mesh SDF:|' +
    'Scene load failed with exception|Failed to retrieve loaded scene|' +
    'Out of memory while (?:loading|retrieving|preparing)|' +
    'Unknown command-line option:|Unknown --automation mode:|' +
    'Assertion failed|Traceback)'
)
$SdfRegex = [regex]::new(
    'Loaded DDGI mesh SDF: .*sponza_(?:sdf|SDF)\.bin',
    [System.Text.RegularExpressions.RegexOptions]::IgnoreCase
)

$Results = [System.Collections.Generic.List[object]]::new()

foreach ($Case in $Cases)
{
    $ShaBefore = (Get-FileHash -Algorithm SHA256 -LiteralPath $Demo).Hash.ToLowerInvariant()
    if ($ShaBefore -ne $ExpectedSha) { throw "Demo SHA changed before $($Case.Name)" }

    $StdoutPath = Join-Path $EvidenceDir "$($Case.Name).stdout.log"
    $StderrPath = Join-Path $EvidenceDir "$($Case.Name).stderr.log"
    $Arguments = @("--automation=$($Case.Mode)") + $Common + $Case.Extra

    $Process = Start-Process `
        -FilePath $Demo `
        -ArgumentList $Arguments `
        -WorkingDirectory $Repo `
        -RedirectStandardOutput $StdoutPath `
        -RedirectStandardError $StderrPath `
        -PassThru

    $TimedOut = -not $Process.WaitForExit(180000)
    if ($TimedOut)
    {
        if (-not $Process.HasExited) { $Process.Kill($true) }
        $Process.WaitForExit()
    }
    else
    {
        $Process.WaitForExit()
    }

    $ExitCode = if ($TimedOut) { $null } else { $Process.ExitCode }
    $Stdout = Get-Content -Raw -LiteralPath $StdoutPath -ErrorAction SilentlyContinue
    $Stderr = Get-Content -Raw -LiteralPath $StderrPath -ErrorAction SilentlyContinue
    $Log = "$Stdout`n$Stderr"
    $Mode = [regex]::Escape($Case.Mode)

    $Markers = [ordered]@{
        config = "\[CSM_AUTOMATION\] marker=config mode=$Mode fixed_dt=0\.0166666\d+ warmup=8 motion=24 hold=8 no_ui=1 no_post=$($Case.NoPost) no_ddgi=$($Case.NoDdgi) taa=$($Case.Taa) auto_exit=1"
        scene_ready = "\[CSM_AUTOMATION\] marker=scene-ready mode=$Mode model=resources/GLTF_Sponza/sponza\.gltf"
        start = "\[CSM_AUTOMATION\] marker=start mode=$Mode frame=0\b"
        last_moving = "\[CSM_AUTOMATION\] marker=last-moving mode=$Mode frame=31\b"
        first_still = "\[CSM_AUTOMATION\] marker=first-still mode=$Mode frame=32\b"
        settled = "\[CSM_AUTOMATION\] marker=settled mode=$Mode frame=39\b"
        complete = "\[CSM_AUTOMATION\] marker=complete mode=$Mode frames=40\b"
    }

    $MarkerCounts = [ordered]@{}
    $InvalidMarkers = @()
    foreach ($Entry in $Markers.GetEnumerator())
    {
        $Count = [regex]::Matches($Log, $Entry.Value).Count
        $MarkerCounts[$Entry.Key] = $Count
        if ($Count -ne 1) { $InvalidMarkers += "$($Entry.Key): expected 1, found $Count" }
    }

    $SdfCount = $SdfRegex.Matches($Log).Count
    $SdfValid = if ($Case.RequireSdf) { $SdfCount -ge 1 } else { $SdfCount -eq 0 }
    $FailureMatches = @($FailureRegex.Matches($Log) | ForEach-Object Value | Sort-Object -Unique)
    $ShaAfter = (Get-FileHash -Algorithm SHA256 -LiteralPath $Demo).Hash.ToLowerInvariant()
    $ShaStable = $ShaAfter -eq $ExpectedSha

    $Passed =
        (-not $TimedOut) -and
        ($ExitCode -eq 0) -and
        ($InvalidMarkers.Count -eq 0) -and
        $SdfValid -and
        ($FailureMatches.Count -eq 0) -and
        $ShaStable

    $Result = [ordered]@{
        case = $Case.Name
        command_line = $Demo + ' ' + ($Arguments -join ' ')
        timed_out = $TimedOut
        timeout_seconds = 180
        exit_code = $ExitCode
        marker_counts = $MarkerCounts
        invalid_markers = $InvalidMarkers
        ddgi_sdf_marker_count = $SdfCount
        ddgi_sdf_marker_valid = $SdfValid
        failure_matches = $FailureMatches
        demo_sha256_before = $ShaBefore
        demo_sha256_after = $ShaAfter
        demo_sha256_stable = $ShaStable
        stdout_log = $StdoutPath
        stderr_log = $StderrPath
        passed = $Passed
    }

    $Result | ConvertTo-Json -Depth 8 |
        Set-Content -LiteralPath (Join-Path $EvidenceDir "$($Case.Name).result.json") -Encoding utf8
    $Results.Add([pscustomobject]$Result)
}

$Summary = [ordered]@{
    created_at = (Get-Date).ToString('o')
    evidence_directory = $EvidenceDir
    demo_path = $Demo
    demo_sha256 = $ExpectedSha
    case_count = $Results.Count
    passed = @($Results | Where-Object { $_.passed }).Count
    failed = @($Results | Where-Object { -not $_.passed }).Count
    cases = @($Results)
}
$Summary | ConvertTo-Json -Depth 10 |
    Set-Content -LiteralPath (Join-Path $EvidenceDir 'summary.json') -Encoding utf8

$Results |
    Select-Object case, passed, timed_out, exit_code, demo_sha256_stable |
    Format-Table -AutoSize

Write-Host "Evidence: $EvidenceDir"
if (@($Results | Where-Object { -not $_.passed }).Count -ne 0)
{
    throw 'One or more Vulkan CSM smoke cases failed. Inspect summary.json and the case logs.'
}
```

## 6. 一票否决失败正则

脚本使用以下逻辑正则扫描每组 stdout 和 stderr；任一命中都判失败：

```regex
(?im)(VUID-|SYNC-HAZARD|Vulkan validation (?:ERROR|WARNING)/|Vulkan error:|VK_ERROR(?:_[A-Z0-9_]+)?|device lost|Frame failed during phase|Failed to create Vulkan instance|Could not initialize GLFW|GLFW: Vulkan not supported|Failed to load model:|Failed to load DDGI mesh SDF:|Scene load failed with exception|Failed to retrieve loaded scene|Out of memory while (?:loading|retrieving|preparing)|Unknown command-line option:|Unknown --automation mode:|Assertion failed|Traceback)
```

Debug/MSVC 下 validation error 可能触发 `__debugbreak()`，PowerShell 可能看到 `-2147483645`（`0x80000003`），而不是普通的 `1`。因此验收必须同时检查失败正则与 `exit_code == 0`，不能只依赖退出码。

## 7. 通过条件

一组 case 只有同时满足以下条件才可标记为通过：

- 未超过 180 秒；
- `Demo.exe` 退出码严格等于 `0`；
- `config`、`scene-ready`、`start`、frame `31`、frame `32`、frame `39`、`complete frames=40` marker 各出现一次；
- `config` 中的 `no_post`、`no_ddgi`、`taa` 与该 case 完全一致；
- 第五组出现 DDGI SDF 加载成功 marker，前四组不出现；
- stdout 与 stderr 未命中一票否决正则；
- 运行前后 Demo SHA-256 与 `identity.json` 完全一致；
- 没有启动、附加、关闭或终止任何 `qrenderdoc.exe`。

五组必须全部满足上述条件，才能把本次 Vulkan validation smoke 记录为通过。该 smoke 验证自动化边界、运行完整性和 Vulkan/synchronization validation；它不能替代 RenderDoc 像素/资源比较或人工视觉 UAT。

## 8. 证据目录

每次运行创建独立目录：

```text
G:\MGIF\out\validation\csm-vulkan-smoke-YYYYMMDD-HHMMSS-<demo_sha256前12位>\
```

必须保留：

```text
identity.json
summary.json
01-translate-no-post-no-ddgi.stdout.log
01-translate-no-post-no-ddgi.stderr.log
01-translate-no-post-no-ddgi.result.json
...
05-rotate-taa-ddgi.stdout.log
05-rotate-taa-ddgi.stderr.log
05-rotate-taa-ddgi.result.json
```

不要把不同 Demo SHA、不同构建或不同运行时生成的日志合并到同一证据目录。报告结果时必须同时给出证据目录、`Demo.exe` 绝对路径、完整 SHA-256、五组结果和任何失败正则命中。
