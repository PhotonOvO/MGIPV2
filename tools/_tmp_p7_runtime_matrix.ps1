[CmdletBinding()]
param(
    [Parameter(Mandatory = $true)][string]$DebugRoot,
    [Parameter(Mandatory = $true)][string]$ReleaseRoot,
    [Parameter(Mandatory = $true)][string]$EvidenceRoot,
    [ValidateSet("debug-unfiltered-no-ui", "debug-unfiltered-ui", "debug-filtered-no-ui", "debug-filtered-ui", "release-unfiltered-no-ui", "release-unfiltered-ui", "release-filtered-no-ui", "release-filtered-ui", "all")]
    [string]$Case = "all",
    [ValidateRange(10, 900)][int]$TimeoutSeconds = 180
)

Set-StrictMode -Version Latest
$ErrorActionPreference = "Stop"

$DebugRoot = [IO.Path]::GetFullPath($DebugRoot)
$ReleaseRoot = [IO.Path]::GetFullPath($ReleaseRoot)
$EvidenceRoot = [IO.Path]::GetFullPath($EvidenceRoot)
if (Test-Path -LiteralPath $EvidenceRoot) {
    throw "Runtime evidence root must be fresh: $EvidenceRoot"
}
New-Item -ItemType Directory -Path $EvidenceRoot -Force | Out-Null

$KnownExternalVuid = "VUID-vkCmdDraw-None-09600"
$cases = @(
    [ordered]@{ name = "debug-unfiltered-no-ui"; configuration = "Debug"; root = $DebugRoot; ui = $false; filtered = $false },
    [ordered]@{ name = "debug-unfiltered-ui"; configuration = "Debug"; root = $DebugRoot; ui = $true; filtered = $false },
    [ordered]@{ name = "debug-filtered-no-ui"; configuration = "Debug"; root = $DebugRoot; ui = $false; filtered = $true },
    [ordered]@{ name = "debug-filtered-ui"; configuration = "Debug"; root = $DebugRoot; ui = $true; filtered = $true },
    [ordered]@{ name = "release-unfiltered-no-ui"; configuration = "Release"; root = $ReleaseRoot; ui = $false; filtered = $false },
    [ordered]@{ name = "release-unfiltered-ui"; configuration = "Release"; root = $ReleaseRoot; ui = $true; filtered = $false },
    [ordered]@{ name = "release-filtered-no-ui"; configuration = "Release"; root = $ReleaseRoot; ui = $false; filtered = $true },
    [ordered]@{ name = "release-filtered-ui"; configuration = "Release"; root = $ReleaseRoot; ui = $true; filtered = $true }
)
if ($Case -ne "all") {
    $cases = @($cases | Where-Object { $_.name -eq $Case })
    if ($cases.Count -ne 1) { throw "Requested runtime case is not in the closed matrix: $Case" }
}

function Write-Text([string]$Path, [string]$Value) {
    [IO.File]::WriteAllText($Path, $Value, [Text.UTF8Encoding]::new($false))
}

function Get-Hash([string]$Path) {
    return (Get-FileHash -LiteralPath $Path -Algorithm SHA256).Hash.ToLowerInvariant()
}

function Quote-Argument([string]$Value) {
    return '"' + $Value.Replace('"', '\"') + '"'
}

function Invoke-RuntimeCase($Spec) {
    $caseRoot = Join-Path $EvidenceRoot $Spec.name
    New-Item -ItemType Directory -Path $caseRoot -Force | Out-Null
    $exe = Join-Path $Spec.root "Demo.exe"
    if (-not (Test-Path -LiteralPath $exe -PathType Leaf)) {
        throw "Missing $($Spec.configuration) Demo: $exe"
    }
    $stdoutPath = Join-Path $caseRoot "stdout.log"
    $stderrPath = Join-Path $caseRoot "stderr.log"
    $metricsPath = Join-Path $caseRoot "metrics.json"
    $argvPath = Join-Path $caseRoot "argv.txt"
    $environmentPath = Join-Path $caseRoot "environment.txt"
    $exitPath = Join-Path $caseRoot "exit-code.txt"
    $hashPath = Join-Path $caseRoot "executable-sha256.txt"
    $summaryPath = Join-Path $caseRoot "case-summary.json"
    $arguments = @(
        "--backend=vulkan", "--automation=csm-translate-stop", "--fixed-dt=0.016666667",
        "--warmup-frames=8", "--motion-frames=24", "--hold-frames=8",
        "--no-post", "--no-ddgi", "--auto-exit", "--metrics-output=$metricsPath"
    )
    if (-not $Spec.ui) { $arguments += "--no-ui" }
    Write-Text $argvPath (($exe + " " + ($arguments -join " ")) + [Environment]::NewLine)
    $layerValue = "VK_LAYER_KHRONOS_validation"
    $process = $null
    $stdoutTask = $null
    $stderrTask = $null
    try {
        $startInfo = [Diagnostics.ProcessStartInfo]::new()
        $startInfo.FileName = $exe
        $startInfo.Arguments = (($arguments | ForEach-Object { Quote-Argument $_ }) -join " ")
        $startInfo.WorkingDirectory = $Spec.root
        $startInfo.UseShellExecute = $false
        $startInfo.CreateNoWindow = $true
        $startInfo.RedirectStandardOutput = $true
        $startInfo.RedirectStandardError = $true
        # Set every validation control on the launched process itself.  This
        # prevents a concurrently running host or a stale parent environment
        # from changing the loader chain between matrix cases.
        $startInfo.EnvironmentVariables["VK_INSTANCE_LAYERS"] = $layerValue
        $startInfo.EnvironmentVariables["VK_LOADER_LAYERS_DISABLE"] = "~implicit~"
        $startInfo.EnvironmentVariables["VK_LOADER_LAYERS_ENABLE"] = $layerValue
        if ($Spec.filtered) {
            $startInfo.EnvironmentVariables["VK_LAYER_MESSAGE_ID_FILTER"] = $KnownExternalVuid
        } else {
            [void]$startInfo.EnvironmentVariables.Remove("VK_LAYER_MESSAGE_ID_FILTER")
        }
        $startInfo.EnvironmentVariables["VK_KHRONOS_VALIDATION_VALIDATE_SYNC"] = "true"
        $startInfo.EnvironmentVariables["VK_KHRONOS_VALIDATION_SYNCVAL_SUBMIT_TIME_VALIDATION"] = "true"
        $filterForLog = if ($Spec.filtered) { $KnownExternalVuid } else { "" }
        $environment = @(
            "VK_INSTANCE_LAYERS=$($startInfo.EnvironmentVariables["VK_INSTANCE_LAYERS"])",
            "VK_LOADER_LAYERS_DISABLE=$($startInfo.EnvironmentVariables["VK_LOADER_LAYERS_DISABLE"])",
            "VK_LOADER_LAYERS_ENABLE=$($startInfo.EnvironmentVariables["VK_LOADER_LAYERS_ENABLE"])",
            "VK_LAYER_MESSAGE_ID_FILTER=$filterForLog",
            "VK_KHRONOS_VALIDATION_VALIDATE_SYNC=$($startInfo.EnvironmentVariables["VK_KHRONOS_VALIDATION_VALIDATE_SYNC"])",
            "VK_KHRONOS_VALIDATION_SYNCVAL_SUBMIT_TIME_VALIDATION=$($startInfo.EnvironmentVariables["VK_KHRONOS_VALIDATION_SYNCVAL_SUBMIT_TIME_VALIDATION"])"
        )
        Write-Text $environmentPath (($environment -join [Environment]::NewLine) + [Environment]::NewLine)
        $process = [Diagnostics.Process]::new()
        $process.StartInfo = $startInfo
        if (-not $process.Start()) { throw "Could not start Demo for $($Spec.name)" }
        # Start both asynchronous drains before waiting.  This avoids the
        # bounded-pipe deadlock that can occur when validation emits a large
        # unfiltered log before the process exits.
        $stdoutTask = $process.StandardOutput.ReadToEndAsync()
        $stderrTask = $process.StandardError.ReadToEndAsync()
        $timedOut = -not $process.WaitForExit($TimeoutSeconds * 1000)
        if ($timedOut) {
            try { $process.Kill() } catch { }
            $process.WaitForExit()
        } else {
            # Complete the redirect flush and refresh ExitCode before collecting artifacts.
            $process.WaitForExit()
            $process.Refresh()
        }
        $stdoutTask.Wait()
        $stderrTask.Wait()
        $exitCode = if ($timedOut) { -1 } else { $process.ExitCode }
        if ($null -eq $exitCode) { throw "Process exit code was not available for $($Spec.name)" }
        Write-Text $exitPath ([Convert]::ToString($exitCode, [Globalization.CultureInfo]::InvariantCulture) + [Environment]::NewLine)
        Write-Text $hashPath ((Get-Hash $exe) + [Environment]::NewLine)
        $stdout = $stdoutTask.Result
        $stderr = $stderrTask.Result
        Write-Text $stdoutPath $stdout
        Write-Text $stderrPath $stderr
        $combined = $stdout + $stderr
        $vuidMatches = [regex]::Matches($combined, "VUID-[A-Za-z0-9]+(?:-[A-Za-z0-9]+)+")
        $vuidIds = @($vuidMatches | ForEach-Object { $_.Value } | Sort-Object -Unique)
        $syncCount = ([regex]::Matches($combined, "SYNC-HAZARD", "IgnoreCase")).Count
        $validationCount = ([regex]::Matches($combined, "Validation Error", "IgnoreCase")).Count
        # The known external VUID is intentionally tolerated only when it is
        # the complete validation record.  Any other validation record must
        # fail the case; counting all `Validation Error` strings would
        # incorrectly reject the documented unfiltered Release baseline.
        $nonExternalValidationText = [regex]::Replace(
            $combined,
            "(?im)^.*Validation Error.*" + [regex]::Escape($KnownExternalVuid) + ".*(?:\r?\n|$)",
            "")
        $unexpectedValidationCount = ([regex]::Matches(
            $nonExternalValidationText, "Validation Error", "IgnoreCase")).Count
        $assertCount = ([regex]::Matches($combined, "Assertion failed|device lost|VK_ERROR_DEVICE_LOST", "IgnoreCase")).Count
        $markers = [ordered]@{}
        foreach ($marker in @("config", "scene-ready", "start", "last-moving", "first-still", "settled", "complete")) {
            $markers[$marker] = ([regex]::Matches($combined, "marker=$marker")).Count
        }
        $metricsValid = $false
        $stableBudget = $false
        if (Test-Path -LiteralPath $metricsPath -PathType Leaf) {
            try {
                $metrics = Get-Content -LiteralPath $metricsPath -Raw | ConvertFrom-Json
                $metricsValid = [string]$metrics.schema -eq "mgif-rhi-stable-window-v1"
                $stableBudget = [bool]$metrics.stable_recording_budget_met
            } catch {
                $metricsValid = $false
                $stableBudget = $false
            }
        }
        $requiredMarkers = @("config", "scene-ready", "start", "last-moving", "first-still", "settled", "complete")
        $markersComplete = $true
        foreach ($marker in $requiredMarkers) {
            if ([int]$markers[$marker] -ne 1) { $markersComplete = $false }
        }
        $diagnosticClean = $syncCount -eq 0 -and $unexpectedValidationCount -eq 0 -and
            $assertCount -eq 0 -and $metricsValid -and $stableBudget -and $markersComplete
        $classification = "failed"
        if (-not $timedOut -and $exitCode -eq 0 -and $diagnosticClean -and ($vuidIds.Count -eq 0 -or ($vuidIds.Count -eq 1 -and $vuidIds[0] -eq $KnownExternalVuid))) {
            if ($vuidIds.Count -eq 0) { $classification = "clean" }
            else { $classification = "external-nvidia-09600-only" }
        }
        $summary = [ordered]@{
            documentKind = "mgif-p7-final-runtime-case"
            schemaVersion = 1
            case = $Spec.name
            configuration = $Spec.configuration
            ui = [bool]$Spec.ui
            filter = if ($Spec.filtered) { $KnownExternalVuid } else { $null }
            executable = [ordered]@{ path = $exe; bytes = (Get-Item -LiteralPath $exe).Length; sha256 = Get-Hash $exe }
            exitCode = $exitCode
            timedOut = $timedOut
            classification = $classification
            vuidIds = $vuidIds
            diagnostics = [ordered]@{ vuid = $vuidMatches.Count; validationError = $validationCount; unexpectedValidationError = $unexpectedValidationCount; syncHazard = $syncCount; assertionOrDeviceLost = $assertCount }
            markers = $markers
            stableRecordingBudget = $stableBudget
            metricsValid = $metricsValid
            markersComplete = $markersComplete
            artifacts = [ordered]@{
                stdout = [ordered]@{ path = "stdout.log"; bytes = (Get-Item $stdoutPath).Length; sha256 = Get-Hash $stdoutPath }
                stderr = [ordered]@{ path = "stderr.log"; bytes = (Get-Item $stderrPath).Length; sha256 = Get-Hash $stderrPath }
                metrics = if (Test-Path -LiteralPath $metricsPath -PathType Leaf) { [ordered]@{ path = "metrics.json"; bytes = (Get-Item $metricsPath).Length; sha256 = Get-Hash $metricsPath } } else { $null }
            }
        }
        Write-Text $summaryPath (($summary | ConvertTo-Json -Depth 12) + [Environment]::NewLine)
        if ($classification -eq "failed") { throw "$($Spec.name) runtime classification failed" }
        return $summary
    }
    finally {
        if ($null -ne $process) {
            try {
                if (-not $process.HasExited) { $process.Kill() }
            } catch { }
            try { $process.WaitForExit() } catch { }
            if ($null -ne $stdoutTask) { try { $stdoutTask.Wait() } catch { } }
            if ($null -ne $stderrTask) { try { $stderrTask.Wait() } catch { } }
            $process.Dispose()
        }
    }
}

$results = @()
foreach ($spec in $cases) {
    Write-Output "Running $($spec.name)"
    $results += Invoke-RuntimeCase $spec
}
$matrix = [ordered]@{
    documentKind = "mgif-p7-final-runtime-matrix"
    schemaVersion = 1
    authoritativePlatform = "Windows MSVC Vulkan"
    knownExternalMessage = $KnownExternalVuid
    cases = $results
}
Write-Text (Join-Path $EvidenceRoot "runtime-matrix.json") (($matrix | ConvertTo-Json -Depth 16) + [Environment]::NewLine)
Write-Output "Runtime matrix passed: $EvidenceRoot"
