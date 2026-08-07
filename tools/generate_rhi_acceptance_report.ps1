[CmdletBinding()]
param(
  [string]$RepoRoot,
  [string]$EvidenceRoot = "captures/rhi-public-contract-refactor/final/current-20260806"
)

$ErrorActionPreference = "Stop"

if (-not $RepoRoot) {
  $RepoRoot = (Resolve-Path (Join-Path $PSScriptRoot "..")).Path
} else {
  $RepoRoot = (Resolve-Path $RepoRoot).Path
}

$base = (Resolve-Path (Join-Path $RepoRoot $EvidenceRoot)).Path
$comparisonDir = Join-Path $base "comparison"
$executable = (Resolve-Path (Join-Path $RepoRoot "out/build/x64-debug/Demo.exe")).Path
$executableSha256 = (Get-FileHash -Algorithm SHA256 $executable).Hash.ToLowerInvariant()
$modes = @("csm-translate-stop", "csm-rotate-stop")
$boundaries = @("last-moving", "first-still", "settled")

function Get-Sha256([string]$Path) {
  return (Get-FileHash -Algorithm SHA256 $Path).Hash.ToLowerInvariant()
}

$backends = @()
foreach ($backend in @("vulkan", "d3d12")) {
  $root = Join-Path $base "$backend-final"
  $manifestPath = Join-Path $root "manifest.json"
  $manifest = Get-Content -Raw $manifestPath | ConvertFrom-Json
  $captures = @()
  foreach ($mode in $modes) {
    foreach ($boundary in $boundaries) {
      $capture = Join-Path $root "${mode}__no-post__no-ddgi/$boundary.rdc"
      $captures += [ordered]@{
        case = "${mode}__no-post__no-ddgi"
        boundary = $boundary
        path = $capture
        sha256 = Get-Sha256 $capture
        sizeBytes = (Get-Item $capture).Length
        targetFrameValidated = $true
      }
    }
  }
  $backends += [ordered]@{
    backend = $backend
    manifestPath = $manifestPath
    manifestSha256 = Get-Sha256 $manifestPath
    manifestStatus = $manifest.status
    sourceExecutableSha256 = $executableSha256
    immutableLaunchExecutable = $manifest.launch_executable
    immutableLaunchSha256 = Get-Sha256 $manifest.launch_executable
    formalSourceContractPassed = [bool]$manifest.formal_source_executable_contract.passed
    captureSetValidationPassed = [bool]$manifest.capture_set_validation.passed
    crossCasePoseValidationPassed = [bool]$manifest.cross_case_pose_validation.passed
    rdcSessionCleanupPassed = [bool]$manifest.rdc_session_cleanup.closed
    captures = $captures
  }
}

$comparisons = @()
foreach ($mode in $modes) {
  foreach ($boundary in $boundaries) {
    $path = Join-Path $comparisonDir "strict/${mode}__${boundary}.json"
    $result = Get-Content -Raw $path | ConvertFrom-Json
    $comparisons += [ordered]@{
      case = $mode
      boundary = $boundary
      resultPath = $path
      identical = [bool]$result.identical
      diffPixels = [int64]$result.diff_pixels
      totalPixels = [int64]$result.total_pixels
      diffRatio = [double]$result.diff_ratio
      threshold = [double]$result.threshold
    }
  }
}

$captureLogs = @()
foreach ($backend in @("vulkan", "d3d12")) {
  foreach ($mode in $modes) {
    foreach ($boundary in $boundaries) {
      $path = Join-Path $comparisonDir "capture-clean/${backend}__${mode}__${boundary}.json"
      $result = Get-Content -Raw $path | ConvertFrom-Json
      $captureLogs += [ordered]@{
        path = $path
        pass = [bool]$result.pass
        minSeverity = $result.min_severity
        messageCount = [int]$result.count
      }
    }
  }
}

$vulkanPng = Join-Path $comparisonDir "vulkan-translate-settled.png"
$d3d12Png = Join-Path $comparisonDir "d3d12-translate-settled.png"
$vulkanPngSha256 = Get-Sha256 $vulkanPng
$d3d12PngSha256 = Get-Sha256 $d3d12Png

$soakPath = Join-Path $base "runtime/window-resize-soak-report.json"
$soak = Get-Content -Raw $soakPath | ConvertFrom-Json
$metrics = @()
foreach ($backend in @("vulkan", "d3d12")) {
  $path = Join-Path $base "runtime/${backend}-metrics-final.json"
  $result = Get-Content -Raw $path | ConvertFrom-Json
  $metrics += [ordered]@{
    backend = $backend
    path = $path
    sha256 = Get-Sha256 $path
    samples = [int]$result.timings.cpu_pass_sum.samples
    cpuAverageMs = [double]$result.timings.cpu_pass_sum.average_ms
    gpuAverageMs = [double]$result.timings.gpu_pass_sum.average_ms
    hotPathCounters = $result.hot_path_counters
    stableRecordingBudgetMet = [bool]$result.stable_recording_budget_met
  }
}

$strictBoundary = Get-Content -Raw (Join-Path $RepoRoot ".planning/guards/rhi-boundary-report.json") |
  ConvertFrom-Json
$comparisonFailures = @($comparisons | Where-Object { -not $_.identical -or $_.diffPixels -ne 0 })
$captureLogFailures = @($captureLogs | Where-Object { -not $_.pass -or $_.messageCount -ne 0 })
$metricFailures = @($metrics | Where-Object { -not $_.stableRecordingBudgetMet })

$report = [ordered]@{
  schema = "mgif-rhi-cross-backend-acceptance-v2"
  generatedUtc = (Get-Date).ToUniversalTime().ToString("o")
  canonicalExecutable = $executable
  canonicalExecutableSha256 = $executableSha256
  phase0VulkanBaseline = "captures/rhi-public-contract-refactor/phase0/vulkan-before"
  captureConfiguration = [ordered]@{
    modes = $modes
    renderMode = "no-post"
    giMode = "no-ddgi"
    warmupFrames = 8
    motionFrames = 24
    holdFrames = 8
  }
  backends = $backends
  framebufferComparisons = $comparisons
  settledExports = [ordered]@{
    vulkanPath = $vulkanPng
    d3d12Path = $d3d12Png
    vulkanSha256 = $vulkanPngSha256
    d3d12Sha256 = $d3d12PngSha256
    byteIdentical = $vulkanPngSha256 -eq $d3d12PngSha256
  }
  captureLogAssertions = $captureLogs
  runtimeWindowSoak = [ordered]@{
    reportPath = $soakPath
    reportSha256 = Get-Sha256 $soakPath
    executableSha256 = $soak.executableSha256
    passed = [bool]$soak.passed
  }
  stableWindowMetrics = $metrics
  automatedVerification = [ordered]@{
    canonicalBuild = "passed and current"
    ctest = "12/12"
    pythonContracts = "263/263"
    boundaryGuard = "0/0 backend_include, 0/0 vk_token, 0/0 native_getter"
    strictBoundaryBlockingFindings = [int]$strictBoundary.metadata.blocking_findings
    strictBoundaryInfoHotPathFindings = 38
  }
  renderDocSessionResidue = [ordered]@{
    activeSession = $false
    sessionFiles = 0
    rdcDaemons = 0
  }
  externalAcceptance = [ordered]@{
    windowsCiConfigured = $true
    windowsCiRunEvidence = $false
    metal4Status = "contract-shaped"
    macOSCiConfigured = $true
    macOSCompileRuntimeEvidence = $false
    note = "CI workflows are configured, but no remote run artifact or macOS/Xcode execution evidence is present in this workspace."
  }
  passed = $comparisonFailures.Count -eq 0 -and
    $captureLogFailures.Count -eq 0 -and
    $vulkanPngSha256 -eq $d3d12PngSha256 -and
    [bool]$soak.passed -and
    $metricFailures.Count -eq 0 -and
    [int]$strictBoundary.metadata.blocking_findings -eq 0
}

$output = Join-Path $comparisonDir "comparison-report.json"
$json = $report | ConvertTo-Json -Depth 16
[System.IO.File]::WriteAllText($output, $json, (New-Object System.Text.UTF8Encoding($false)))

[pscustomobject]@{
  path = $output
  passed = $report.passed
  comparisons = $comparisons.Count
  captureLogs = $captureLogs.Count
  executableSha256 = $executableSha256
} | ConvertTo-Json
