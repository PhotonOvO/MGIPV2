# Vulkan CSM Validation Smoke Runbook

本文只定义可复现流程，不记录任何一次运行的最终通过或失败结论。每次运行的结论必须保存在该次独立证据目录中。

## 1. 固定环境与硬约束

- 仓库根目录：`G:\MGIF`
- CMake 生成器：`Ninja`
- 构建类型：`Debug`
- 构建目录：`G:\Temp\mgif-csm-vulkan-debug`
- 唯一受测可执行文件：`G:\Temp\mgif-csm-vulkan-debug\Demo.exe`
- Debug 构建会启用 `VK_LAYER_KHRONOS_validation`，无需额外设置 `VK_INSTANCE_LAYERS`。
- `--no-ui` 只关闭 ImGui 绘制，不是 headless 模式；测试仍会创建 GLFW/Vulkan 窗口。
- 本流程不启动、不附加、不关闭、不终止 `qrenderdoc.exe`。禁止按进程名执行 `taskkill`；超时时只能终止当前脚本持有句柄的那个 `Demo.exe`。

先确认缓存和可执行文件位置：

```cmd
cd /d G:\MGIF
findstr /B "CMAKE_GENERATOR:INTERNAL= CMAKE_BUILD_TYPE:STRING=" G:\Temp\mgif-csm-vulkan-debug\CMakeCache.txt
if not exist G:\Temp\mgif-csm-vulkan-debug\Demo.exe exit /b 1
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
findstr /B "CMAKE_GENERATOR:INTERNAL= CMAKE_BUILD_TYPE:STRING=" G:\Temp\mgif-csm-vulkan-debug\CMakeCache.txt

cmake --build G:\Temp\mgif-csm-vulkan-debug --parallel 2 --target Demo
if errorlevel 1 exit /b 1
if not exist G:\Temp\mgif-csm-vulkan-debug\Demo.exe exit /b 1
```

仅当 `G:\Temp\mgif-csm-vulkan-debug\CMakeCache.txt` 不存在或确实需要重新配置时，才在上述同一开发环境中先执行：

```cmd
cmake -S . -B G:\Temp\mgif-csm-vulkan-debug -G Ninja -DCMAKE_BUILD_TYPE=Debug -DBUILD_TESTING=ON -DUSE_SLANG=ON
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
\[CSM_AUTOMATION\] marker=config mode=csm-(?:translate|rotate)-stop fixed_dt=0\.01666666[0-9] warmup=8 motion=24 hold=8 no_ui=1 no_post=[01] no_ddgi=[01] taa=[01] auto_exit=1 capture_sync=[01] capture_control=0 capture_sync_timeout_ms=[1-9][0-9]* capture_sync_dir=.*
\[CSM_AUTOMATION\] marker=scene-ready mode=csm-(?:translate|rotate)-stop model=resources/GLTF_Sponza/sponza\.gltf
\[CSM_AUTOMATION\] marker=start mode=csm-(?:translate|rotate)-stop frame=0
\[CSM_AUTOMATION\] marker=last-moving mode=csm-(?:translate|rotate)-stop frame=31 current_pos=\([-+0-9.,]+\) current_yaw_pitch=\([-+0-9.,]+\) previous_pos=\([-+0-9.,]+\) previous_yaw_pitch=\([-+0-9.,]+\)
\[CSM_AUTOMATION\] marker=first-still mode=csm-(?:translate|rotate)-stop frame=32 current_pos=\([-+0-9.,]+\) current_yaw_pitch=\([-+0-9.,]+\) previous_pos=\([-+0-9.,]+\) previous_yaw_pitch=\([-+0-9.,]+\)
\[CSM_AUTOMATION\] marker=settled mode=csm-(?:translate|rotate)-stop frame=39 current_pos=\([-+0-9.,]+\) current_yaw_pitch=\([-+0-9.,]+\) previous_pos=\([-+0-9.,]+\) previous_yaw_pitch=\([-+0-9.,]+\)
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
$ConfiguredTempRoot = (Resolve-Path -LiteralPath 'G:\Temp').Path
$env:TEMP = $ConfiguredTempRoot
$env:TMP = $ConfiguredTempRoot
$BuildRoot = (Resolve-Path -LiteralPath (Join-Path $ConfiguredTempRoot 'mgif-csm-vulkan-debug')).Path
$Demo = (Resolve-Path -LiteralPath (Join-Path $BuildRoot 'Demo.exe')).Path
$DemoItem = Get-Item -LiteralPath $Demo
$ExpectedSha = (Get-FileHash -Algorithm SHA256 -LiteralPath $Demo).Hash.ToLowerInvariant()
$Stamp = Get-Date -Format 'yyyyMMdd-HHmmss'
$EvidenceDir = Join-Path $ConfiguredTempRoot (
    'mgif-csm-vulkan-smoke-{0}-{1}' -f $Stamp, $ExpectedSha.Substring(0, 12)
)
New-Item -ItemType Directory -Path $EvidenceDir -Force | Out-Null
$RuntimeDir = Join-Path $EvidenceDir 'runtime'
New-Item -ItemType Directory -Path $RuntimeDir | Out-Null
Copy-Item -LiteralPath $Demo -Destination (Join-Path $RuntimeDir 'Demo.exe')
Get-ChildItem -LiteralPath $BuildRoot -Filter '*.dll' -File |
    Copy-Item -Destination $RuntimeDir
$Demo = (Resolve-Path -LiteralPath (Join-Path $RuntimeDir 'Demo.exe')).Path
$DemoItem = Get-Item -LiteralPath $Demo
if ((Get-FileHash -Algorithm SHA256 -LiteralPath $Demo).Hash.ToLowerInvariant() -ne $ExpectedSha)
{
    throw 'Evidence-local Demo copy does not match the freshly built executable.'
}

$env:VK_KHRONOS_VALIDATION_VALIDATE_SYNC = 'true'
$env:VK_KHRONOS_VALIDATION_SYNCVAL_SUBMIT_TIME_VALIDATION = 'true'

$Identity = [ordered]@{
    created_at = (Get-Date).ToString('o')
    repository_root = $Repo
    build_root = $BuildRoot
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

function Get-SourceManifestRecords([string] $SourceRoot)
{
    $root = (Resolve-Path -LiteralPath $SourceRoot).Path
    $records = [System.Collections.Generic.List[object]]::new()
    $sourceFiles = Get-ChildItem -LiteralPath (Join-Path $root 'resources\GLTF_Sponza') -File -Recurse
    foreach ($item in $sourceFiles)
    {
        $relative = $item.FullName.Substring($root.Length).TrimStart('\').Replace('\', '/')
        $hash = (Get-FileHash -Algorithm SHA256 -LiteralPath $item.FullName).Hash.ToLowerInvariant()
        $records.Add([ordered]@{
            relative_path = $relative
            size_bytes = [int64]$item.Length
            sha256 = $hash
            role = if ($relative -eq 'resources/GLTF_Sponza/sponza.gltf') { 'model' } else { 'runtime-dependency' }
        })
    }
    return @($records | Sort-Object relative_path)
}

$SourceRoot = $Repo
$RunSourceManifestBaseline = Get-SourceManifestRecords $SourceRoot

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
    '(?im)(VUID-|SYNC-HAZARD|Vulkan validation (?:ERROR|WARNING)\b|' +
    'Vulkan error:|VK_ERROR(?:_[A-Z0-9_]+)?|device lost|' +
    'Frame failed during phase|Failed to create Vulkan instance|' +
    'Could not initialize GLFW|GLFW: Vulkan not supported|' +
    'Failed to load model:|Failed to load DDGI mesh SDF:|' +
    'Scene load failed with exception|Failed to retrieve loaded scene|' +
    'Out of memory while (?:loading|retrieving|preparing)|' +
    'Unknown command-line option:|Unknown --automation mode:|' +
    'marker=automation-failed|Editor transaction automation failed|' +
    'Save As verification failed|semantic mismatch|Assertion failed|Traceback)'
)
$SdfRegex = [regex]::new(
    'Loaded DDGI mesh SDF: .*sponza_(?:sdf|SDF)\.bin',
    [System.Text.RegularExpressions.RegexOptions]::IgnoreCase
)

$Results = [System.Collections.Generic.List[object]]::new()

foreach ($Case in $Cases)
{
    $SourceManifestBefore = Get-SourceManifestRecords $SourceRoot
    if (($SourceManifestBefore | ConvertTo-Json -Depth 6 -Compress) -ne
        ($RunSourceManifestBaseline | ConvertTo-Json -Depth 6 -Compress))
    {
        throw "Sponza source manifest drifted before $($Case.Name)"
    }
    $ShaBefore = (Get-FileHash -Algorithm SHA256 -LiteralPath $Demo).Hash.ToLowerInvariant()
    if ($ShaBefore -ne $ExpectedSha) { throw "Demo SHA changed before $($Case.Name)" }

    $StdoutPath = Join-Path $EvidenceDir "$($Case.Name).stdout.log"
    $StderrPath = Join-Path $EvidenceDir "$($Case.Name).stderr.log"
    $SaveDir = Join-Path (Join-Path $EvidenceDir $Case.Name) 'save'
    New-Item -ItemType Directory -Path $SaveDir -Force | Out-Null
    if (@(Get-ChildItem -LiteralPath $SaveDir -Force).Count -ne 0)
    {
        throw "P5 save directory is not empty before $($Case.Name)"
    }
    $Arguments = @("--automation=$($Case.Mode)") + $Common + $Case.Extra +
        "--editor-transaction-dir=$SaveDir"

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
        config = "\[CSM_AUTOMATION\] marker=config mode=$Mode fixed_dt=0\.01666666[0-9] warmup=8 motion=24 hold=8 no_ui=1 no_post=$($Case.NoPost) no_ddgi=$($Case.NoDdgi) taa=$($Case.Taa) auto_exit=1 capture_sync=0 capture_control=0 capture_sync_timeout_ms=[1-9][0-9]* capture_sync_dir="
        scene_ready = "\[CSM_AUTOMATION\] marker=scene-ready mode=$Mode model=resources/GLTF_Sponza/sponza\.gltf"
        start = "\[CSM_AUTOMATION\] marker=start mode=$Mode frame=0\b"
        automation_start = "\[CSM_AUTOMATION\] marker=automation-start mode=$Mode transaction=rename-undo-redo-save frame_budget=40"
        undo_observed = "\[CSM_AUTOMATION\] marker=undo-observed mode=$Mode revision=[0-9]+ undo=0 redo=1 digest=[0-9a-f]{64} renderer_revision=[0-9]+"
        redo_observed = "\[CSM_AUTOMATION\] marker=redo-observed mode=$Mode revision=[0-9]+ undo=1 redo=0 digest=[0-9a-f]{64} renderer_revision=[0-9]+"
        save_complete = "\[CSM_AUTOMATION\] marker=save-complete mode=$Mode revision=[0-9]+ clean_revision=[0-9]+ bytes=[1-9][0-9]* reload_valid=1 semantic_equal=1"
        last_moving = "\[CSM_AUTOMATION\] marker=last-moving mode=$Mode frame=31 current_pos=\([^)]*\) current_yaw_pitch=\([^)]*\) previous_pos=\([^)]*\) previous_yaw_pitch=\([^)]*\)"
        first_still = "\[CSM_AUTOMATION\] marker=first-still mode=$Mode frame=32 current_pos=\([^)]*\) current_yaw_pitch=\([^)]*\) previous_pos=\([^)]*\) previous_yaw_pitch=\([^)]*\)"
        settled = "\[CSM_AUTOMATION\] marker=settled mode=$Mode frame=39 current_pos=\([^)]*\) current_yaw_pitch=\([^)]*\) previous_pos=\([^)]*\) previous_yaw_pitch=\([^)]*\)"
        complete = "\[CSM_AUTOMATION\] marker=complete mode=$Mode frames=40\b"
        frame_budget_complete = "\[CSM_AUTOMATION\] marker=frame-budget-complete mode=$Mode frames=40"
        automation_complete = "\[CSM_AUTOMATION\] marker=automation-complete mode=$Mode frames=40 auto_exit=1"
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
    if ($Log -match '(?im)(?<![A-Za-z0-9_])frame(?:_index)?["'']?\s*(?:=|:)\s*["'']?40(?![0-9])')
    {
        $FailureMatches += 'forbidden frame=40 evidence'
    }
    $ShaAfter = (Get-FileHash -Algorithm SHA256 -LiteralPath $Demo).Hash.ToLowerInvariant()
    $ShaStable = $ShaAfter -eq $ExpectedSha
    $SaveTarget = Join-Path $SaveDir 'sponza-transaction.glb'
    if (-not (Test-Path -LiteralPath $SaveTarget -PathType Leaf))
    {
        throw "P5 Save As target is missing for $($Case.Name): $SaveTarget"
    }
    $ArtifactItem = Get-Item -LiteralPath $SaveTarget
    $ArtifactHash = (Get-FileHash -Algorithm SHA256 -LiteralPath $SaveTarget).Hash.ToLowerInvariant()
    $SourceManifestAfter = Get-SourceManifestRecords $SourceRoot
    $SourceStable =
        ($SourceManifestBefore | ConvertTo-Json -Depth 6 -Compress) -eq
        ($SourceManifestAfter | ConvertTo-Json -Depth 6 -Compress)

    $Passed =
        (-not $TimedOut) -and
        ($ExitCode -eq 0) -and
        ($InvalidMarkers.Count -eq 0) -and
        $SdfValid -and
        ($FailureMatches.Count -eq 0) -and
        $ShaStable -and
        $SourceStable

    $Result = [ordered]@{
        case = $Case.Name
        mode = $Case.Mode
        no_post = [bool]$Case.NoPost
        no_ddgi = [bool]$Case.NoDdgi
        taa = [bool]$Case.Taa
        command_line = $Demo + ' ' + ($Arguments -join ' ')
        timed_out = $TimedOut
        timeout_seconds = 180
        exit_code = $ExitCode
        marker_counts = $MarkerCounts
        invalid_markers = $InvalidMarkers
        ddgi_sdf_marker_count = $SdfCount
        ddgi_sdf_marker_valid = $SdfValid
        failure_matches = $FailureMatches
        demo_sha256_stable = $ShaStable
        stdout_log = $StdoutPath
        stderr_log = $StderrPath
        stdout = $Stdout
        stderr = $Stderr
        case_dir = $SaveDir
        artifact_path = $SaveTarget
        save_target = $SaveTarget
        artifact_sha256 = $ArtifactHash
        artifact_size_bytes = [int64]$ArtifactItem.Length
        demo_path = $Demo
        expected_demo_sha256 = $ExpectedSha
        demo_sha256 = $ShaAfter
        demo_sha256_before = $ShaBefore
        demo_sha256_after = $ShaAfter
        source_manifest = [ordered]@{
            source_root = $SourceRoot
            before = @($SourceManifestBefore)
            after = @($SourceManifestAfter)
        }
        strict_scene_ready_count = $MarkerCounts.scene_ready
        rendered_frames = 40
        auto_exit_successful = (-not $TimedOut) -and ($ExitCode -eq 0)
        passed = $Passed
    }

    $CaseJsonPath = Join-Path $EvidenceDir "$($Case.Name).case.json"
    $Result | ConvertTo-Json -Depth 12 |
        Set-Content -LiteralPath $CaseJsonPath -Encoding utf8
    $ValidatedPath = Join-Path $EvidenceDir "$($Case.Name).validated.json"
    & python (Join-Path $Repo 'tools\editor_transaction_smoke.py') --case $CaseJsonPath --evidence-root $EvidenceDir --output $ValidatedPath
    if ($LASTEXITCODE -ne 0) { throw "P5 validator rejected $($Case.Name)" }
    $Validated = Get-Content -Raw -LiteralPath $ValidatedPath | ConvertFrom-Json
    $Result = $Validated
    $Result | ConvertTo-Json -Depth 16 |
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
$SummaryPath = Join-Path $EvidenceDir 'summary.json'
$Summary | ConvertTo-Json -Depth 20 |
    Set-Content -LiteralPath $SummaryPath -Encoding utf8
$ValidatedSummaryPath = Join-Path $EvidenceDir 'validated-summary.json'
& python (Join-Path $Repo 'tools\editor_transaction_smoke.py') --summary $SummaryPath --evidence-root $EvidenceDir --repo-root $Repo --output $ValidatedSummaryPath
if ($LASTEXITCODE -ne 0) { throw 'Aggregate P5 validator rejected the five-case matrix.' }

$Results |
    Select-Object case, passed, timed_out, exit_code |
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
(?im)(VUID-|SYNC-HAZARD|Vulkan validation (?:ERROR|WARNING)\b|Vulkan error:|VK_ERROR(?:_[A-Z0-9_]+)?|device lost|Frame failed during phase|Failed to create Vulkan instance|Could not initialize GLFW|GLFW: Vulkan not supported|Failed to load model:|Failed to load DDGI mesh SDF:|Scene load failed with exception|Failed to retrieve loaded scene|Out of memory while (?:loading|retrieving|preparing)|Unknown command-line option:|Unknown --automation mode:|marker=automation-failed|Editor transaction automation failed|Save As verification failed|semantic mismatch|Assertion failed|Traceback)
```

Debug/MSVC 下 validation error 可能触发 `__debugbreak()`，PowerShell 可能看到 `-2147483645`（`0x80000003`），而不是普通的 `1`。因此验收必须同时检查失败正则与 `exit_code == 0`，不能只依赖退出码。

## 7. 通过条件

一组 case 只有同时满足以下条件才可标记为通过：

- 未超过 180 秒；
- `Demo.exe` 退出码严格等于 `0`；
- `config`、`scene-ready`、`start`、frame `31`、frame `32`、frame `39`、`complete frames=40` marker 各出现一次且匹配完整 production 字段；
- 七个 authoritative `[CSM_AUTOMATION]` transaction marker 各出现一次；
- 两组 marker 共同满足 `config -> scene-ready -> start -> automation-start -> undo-observed -> redo-observed -> save-complete -> last-moving -> first-still -> settled -> complete -> frame-budget-complete -> automation-complete`；
- `config` 中的 `no_post`、`no_ddgi`、`taa` 与该 case 完全一致；
- 第五组出现 DDGI SDF 加载成功 marker，前四组不出现；
- stdout 与 stderr 未命中一票否决正则；
- 运行前后 Demo SHA-256 与 `identity.json` 完全一致；
- `sponza-transaction.glb` 是大于 12 字节的普通非 reparse 文件，GLB header/长度、marker byte count、SHA-256、reload 与 semantic evidence 全部通过；
- Sponza glTF 及 runtime dependency 的 normalized before/after manifest 完全一致，且 validator 重新读取并 SHA-256 验证最终 source root；
- 日志和 structured telemetry 中均不存在 counted `frame=40`；
- 没有启动、附加、关闭或终止任何 `qrenderdoc.exe`。

五组必须全部满足上述条件，才能把本次 Vulkan validation smoke 记录为通过。该 smoke 验证自动化边界、运行完整性和 Vulkan/synchronization validation；它不能替代 RenderDoc 像素/资源比较或人工视觉 UAT。

## 9. P5 scene-editor transaction automation

P5 is an explicit opt-in extension of each CSM case. It must never be enabled
implicitly and it must not change an interactive launch. Configure `TEMP` and
`TMP` to the same existing absolute temporary root, then add one absolute,
newly-isolated child directory to the application command line:

```text
--editor-transaction-dir=G:\Temp\mgif-p5-editor-transaction-<run>\<case>\save
```

The option is accepted only with `--automation`, `--no-ui`, `--auto-exit`,
exactly `--warmup-frames=8`, `--motion-frames=24`, `--hold-frames=8`, and
without `--capture-control-frame`. The application creates missing child
components only after validating their existing ancestors. Every existing
ancestor and the final directory must be a real non-symlink/non-reparse
directory; the final directory must be empty, may not contain
`sponza-transaction.glb`, and may not resolve to the source directory. Keep
stdout/stderr and JSON evidence beside the case directory, not inside `save`.

After the strict Sponza scene has rendered and its editor projection is
published, the application performs one real projected-node rename through the
typed `EditorController` API, waits for an accepted `EditorRendererAdapter`
publication, then performs real undo and redo with the same renderer-acceptance
fence. It performs an explicit-path Save As to
`<save>\sponza-transaction.glb`, reloads that file through `GltfDocumentIO`,
checks semantic equivalence and the mutated node name, and proves that the live
document is clean at the new source path. The original Sponza glTF and its
dependencies must be byte-for-byte unchanged.

The authoritative P5 markers are literal, once-only, and ordered as follows;
the existing CSM markers above remain supplementary evidence and must retain
their spelling and order:

```text
[CSM_AUTOMATION] marker=scene-ready mode=<mode> model=resources/GLTF_Sponza/sponza.gltf
[CSM_AUTOMATION] marker=automation-start mode=<mode> transaction=rename-undo-redo-save frame_budget=40
[CSM_AUTOMATION] marker=undo-observed mode=<mode> revision=<u64> undo=0 redo=1 digest=<64-lower-hex> renderer_revision=<same-u64>
[CSM_AUTOMATION] marker=redo-observed mode=<mode> revision=<u64> undo=1 redo=0 digest=<64-lower-hex> renderer_revision=<same-u64>
[CSM_AUTOMATION] marker=save-complete mode=<mode> revision=<u64> clean_revision=<same-u64> bytes=<positive-u64> reload_valid=1 semantic_equal=1
[CSM_AUTOMATION] marker=frame-budget-complete mode=<mode> frames=40
[CSM_AUTOMATION] marker=automation-complete mode=<mode> frames=40 auto_exit=1
```

These full lines are the locked external grammar. An
`[EDITOR_TRANSACTION] marker=<authoritative-marker>` line is never an accepted
alias, even when its remaining fields describe the same operation.

`utils::Logger` prepends a timestamp such as `[21:07:05.418]`; the validator
removes only that production-shaped prefix and still full-matches the complete
marker payload. The authoritative and supplementary ledgers are not validated
independently: their required combined chronology is:

```text
config -> scene-ready -> start -> automation-start -> undo-observed ->
redo-observed -> save-complete -> last-moving -> first-still -> settled ->
complete -> frame-budget-complete -> automation-complete
```

Every case JSON must contain a closed source manifest. `source_root` is the
absolute runtime root from which the strict marker path resolves. Both snapshots
use normalized forward-slash relative paths, byte sizes, and SHA-256 values:

```json
{
  "source_manifest": {
    "source_root": "G:\\MGIF",
    "before": [
      {
        "relative_path": "resources/GLTF_Sponza/sponza.gltf",
        "size_bytes": 123,
        "sha256": "<64-lower-hex>",
        "role": "model"
      }
    ],
    "after": ["the same complete model/dependency records"]
  }
}
```

The lists include every regular file beneath `resources/GLTF_Sponza`, which
necessarily closes over every local URI dependency and the DDGI
`resources/GLTF_Sponza/sponza_sdf.bin`. The validator independently enumerates
that bounded final tree and rejects omitted or extra entries, path escapes,
non-normalized paths, duplicates and case collisions, symlink/reparse path
components, added/removed/changed records, and a final source file whose size
or SHA-256 no longer matches. If a driver supplies structured transaction progression, it
must provide `baseline`, `mutation`, `undo`, and `redo` document revisions plus
mapping epochs; every adjacent value must advance by exactly one and the
undo/redo revisions must match the literal marker fields.

Supply either separate stdout/stderr captures or one `combined_output`, never
`combined_output` together with `stderr`/`stderr_path`; ambiguous evidence is
rejected so a VUID, SYNC hazard, or `marker=automation-failed` cannot be hidden.

The transaction owns a monotonic fail-closed phase. Any rejected/stale command,
wrong renderer stamp, unexpected publication, semantic mismatch, invalid or
aliased output, suspension after scene-ready, timeout, early frame 39, or
attempted frame 40 must emit one `marker=automation-failed` diagnostic and
return a nonzero process exit. A success line without its real predicate is not
valid evidence. Mutation, undo, and redo publications must be accepted within
three subsequent counted frames; Save As must verify by counted frame 8; the
whole transaction deadline is 60 seconds after scene-ready; and the outer
process timeout remains 180 seconds.

For each of the five cases, require the seven P5 marker counts to be exactly
one, strictly increasing in the log, and require `frames=40` with no frame 40.
Require the output to be a regular non-empty GLB (`glTF`, version 2, header
length equal to file length), record its SHA-256 and compare its byte count to
`save-complete`, then rehash the source glTF/dependency manifest and require
exact equality. Add `marker=automation-failed`, `Save As failure`, `semantic
mismatch`, and the existing validation/failure expressions to the one-vote
failure scan. Keep five distinct case output paths; identical content hashes
are allowed.

The bounded production dependency scan for this P5 extension covers only
`app`, `scene/editor`, `render/editor`, and `rhi`; it must report zero
`nvcore`, `nvpro`, `nvvk`, `nvapp`, `nvgui`, `nvvkgltf`, and `reference-helper`
matches. This runbook section does not claim P5/G5 completion until the later
independent review and five driver-backed transaction smokes pass.

## 8. 证据目录

每次运行创建独立目录：

```text
G:\Temp\mgif-csm-vulkan-smoke-YYYYMMDD-HHMMSS-<demo_sha256前12位>\
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
