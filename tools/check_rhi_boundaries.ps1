[CmdletBinding()]
param(
  [string]$RepoRoot,
  [string]$OutputDir,
  [switch]$IncludeBackendDirs,
  [switch]$Enforce
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

function Convert-ToRepoPath {
  param(
    [Parameter(Mandatory = $true)][string]$InputPath,
    [Parameter(Mandatory = $true)]$Root
  )

  $Root = [string]$Root
  $rootFull = [System.IO.Path]::GetFullPath($Root)
  if (-not $rootFull.EndsWith([System.IO.Path]::DirectorySeparatorChar)) {
    $rootFull += [System.IO.Path]::DirectorySeparatorChar
  }

  $pathFull = [System.IO.Path]::GetFullPath($InputPath)
  $rootUri = New-Object System.Uri($rootFull)
  $pathUri = New-Object System.Uri($pathFull)
  $relative = [System.Uri]::UnescapeDataString($rootUri.MakeRelativeUri($pathUri).ToString())
  return $relative.Replace("\", "/")
}

function Get-ScanFiles {
  param([Parameter(Mandatory = $true)]$Root)

  $Root = [string]$Root
  $roots = @("app", "render", "common")
  $files = New-Object System.Collections.Generic.List[string]
  foreach ($scanRoot in $roots) {
    $path = Join-Path $Root $scanRoot
    if (Test-Path -LiteralPath $path) {
      Get-ChildItem -LiteralPath $path -Recurse -File |
        Where-Object { $_.Extension -match '^\.(h|hpp|hh|hxx|c|cc|cpp|cxx|inl)$' } |
        ForEach-Object { $files.Add($_.FullName) }
    }
  }

  $rhiPath = Join-Path $Root "rhi"
  if (Test-Path -LiteralPath $rhiPath) {
    Get-ChildItem -LiteralPath $rhiPath -File -Filter "*.h" |
      ForEach-Object { $files.Add($_.FullName) }
  }

  if ($IncludeBackendDirs -and (Test-Path -LiteralPath $rhiPath)) {
    Get-ChildItem -LiteralPath $rhiPath -Recurse -File |
      Where-Object {
        $_.FullName -match '[\\/]rhi[\\/](vulkan|d3d12|metal)[\\/]' -and
        $_.Extension -match '^\.(h|hpp|hh|hxx|c|cc|cpp|cxx|inl)$'
      } |
      ForEach-Object { $files.Add($_.FullName) }
  }

  return $files | Sort-Object -Unique
}

function Test-Phase2PublicSurface {
  param([Parameter(Mandatory = $true)][string]$RepoPath)

  if ($RepoPath -like "app/*" -or $RepoPath -like "common/*") {
    return $true
  }

  if ($RepoPath -like "rhi/*.h") {
    return $true
  }

  switch ($RepoPath) {
    "render/RendererFacade.h" { return $true }
    "render/RenderTypes.h" { return $true }
    default { return $false }
  }
}

function Get-OwnerPhase {
  param(
    [Parameter(Mandatory = $true)][string]$Category,
    [Parameter(Mandatory = $true)][string]$RepoPath
  )

  if ((Test-Phase2PublicSurface -RepoPath $RepoPath) -and
      ($Category -eq "backend-include" -or $Category -eq "native-symbol" -or $Category -eq "native-getter")) {
    return "Phase 2"
  }

  if ($RepoPath -like "render/*Resources.h" -or
      $RepoPath -like "render/RenderDevice.h" -or
      $RepoPath -like "render/GPUMeshletBuffer.h" -or
      $RepoPath -like "render/GPUSceneRegistry.h" -or
      $RepoPath -like "render/HiZDepthPyramid.h" -or
      $RepoPath -like "render/MipmapGenerator.h" -or
      $RepoPath -like "render/RHIFormatBridge.h") {
    return "Phase 5"
  }

  if ($RepoPath -like "render/passes/*" -or $RepoPath -like "render/*.cpp") {
    if ($Category -eq "native-getter" -or $Category -eq "native-symbol") {
      return "Phase 4"
    }
  }

  switch ($Category) {
    "backend-include" { return "Phase 9" }
    "native-symbol" { return "Phase 9" }
    "native-getter" { return "Phase 9" }
    "legacy-binding-vocab" { return "Phase 3" }
    "hot-path-risk" { return "Phase 8" }
    default { return "Phase 9" }
  }
}

function Get-FindingSeverity {
  param(
    [Parameter(Mandatory = $true)][string]$Category,
    [Parameter(Mandatory = $true)][bool]$BaselineMatch,
    [Parameter(Mandatory = $true)][string]$RepoPath
  )

  if ($BaselineMatch) {
    return "Warning"
  }

  if ((Test-Phase2PublicSurface -RepoPath $RepoPath) -and
      ($Category -eq "backend-include" -or $Category -eq "native-symbol" -or $Category -eq "native-getter")) {
    return "Error"
  }

  return "Info"
}

function Test-EnforceBlockingFinding {
  param([Parameter(Mandatory = $true)]$Finding)

  if ($Finding.category -eq "legacy-binding-vocab") {
    return $true
  }

  if ($Finding.severity -eq "Error") {
    return $true
  }

  if ((Test-Phase2PublicSurface -RepoPath $Finding.path) -and
      ($Finding.category -eq "backend-include" -or $Finding.category -eq "native-symbol" -or $Finding.category -eq "native-getter")) {
    return $true
  }

  return $false
}

function Test-BaselineMatch {
  param(
    $BaselineEntries,
    [Parameter(Mandatory = $true)][string]$RepoPath,
    [Parameter(Mandatory = $true)][string]$Category,
    [Parameter(Mandatory = $true)][string]$LineText
  )

  foreach ($entry in $BaselineEntries) {
    if ($entry.category -ne $Category) {
      continue
    }
    if ($entry.path -and $RepoPath -notlike $entry.path) {
      continue
    }
    if ($entry.pattern -and $LineText -notmatch $entry.pattern) {
      continue
    }
    return $entry
  }

  return $null
}

function Read-BaselineEntries {
  param([Parameter(Mandatory = $true)][string]$Path)

  if (-not (Test-Path -LiteralPath $Path)) {
    return @()
  }

  $json = Get-Content -LiteralPath $Path -Raw | ConvertFrom-Json
  if ($json.entries) {
    return @($json.entries)
  }

  return @()
}

function New-Finding {
  param(
    [Parameter(Mandatory = $true)][string]$Category,
    [Parameter(Mandatory = $true)][string]$RepoPath,
    [Parameter(Mandatory = $true)][int]$Line,
    [Parameter(Mandatory = $true)][string]$Pattern,
    [Parameter(Mandatory = $true)][string]$Text,
    $BaselineEntries
  )

  $baseline = Test-BaselineMatch -BaselineEntries $BaselineEntries -RepoPath $RepoPath -Category $Category -LineText $Text
  $baselineMatch = $null -ne $baseline
  $ownerPhase = if ($baselineMatch -and $baseline.owner_phase) { $baseline.owner_phase } else { Get-OwnerPhase -Category $Category -RepoPath $RepoPath }

  [pscustomobject]@{
    severity = Get-FindingSeverity -Category $Category -BaselineMatch $baselineMatch -RepoPath $RepoPath
    category = $Category
    path = $RepoPath
    line = $Line
    pattern = $Pattern
    text = $Text.Trim()
    baseline_status = if ($baselineMatch) { "baseline" } else { "unbaselined" }
    owner_phase = $ownerPhase
  }
}

function Invoke-RegexScan {
  param(
    [Parameter(Mandatory = $true)][string[]]$Files,
    [Parameter(Mandatory = $true)]$Rules,
    [Parameter(Mandatory = $true)]$Root,
    $BaselineEntries
  )

  $Root = [string]$Root
  $findings = New-Object System.Collections.Generic.List[object]

  foreach ($file in $Files) {
    $repoPath = Convert-ToRepoPath -InputPath $file -Root $Root
    $lines = Get-Content -LiteralPath $file
    for ($index = 0; $index -lt $lines.Count; $index++) {
      $lineText = $lines[$index]
      foreach ($rule in $Rules) {
        if ($lineText -cmatch $rule.pattern) {
          $findings.Add((New-Finding `
            -Category $rule.category `
            -RepoPath $repoPath `
            -Line ($index + 1) `
            -Pattern $rule.pattern `
            -Text $lineText `
            -BaselineEntries $BaselineEntries))
        }
      }
    }
  }

  return $findings
}

$scriptDir = Split-Path -Parent $MyInvocation.MyCommand.Path
if (-not $RepoRoot) {
  $RepoRoot = (Resolve-Path (Join-Path $scriptDir "..")).Path
} else {
  $RepoRoot = (Resolve-Path $RepoRoot).Path
}

if (-not $OutputDir) {
  $OutputDir = Join-Path $RepoRoot ".planning\guards"
}

$markdownPath = Join-Path $OutputDir "rhi-boundary-report.md"
$jsonPath = Join-Path $OutputDir "rhi-boundary-report.json"
$baselinePath = Join-Path $OutputDir "rhi-boundary-baseline.json"
$baselineEntries = @(Read-BaselineEntries -Path $baselinePath)

$rules = @(
  [pscustomobject]@{
    category = "backend-include"
    pattern = '#\s*include\s*[<"][^>"]*rhi[\\/](vulkan|d3d12|metal)[\\/][^>"]*[>"]'
  },
  [pscustomobject]@{
    category = "native-symbol"
    pattern = '\b(Vk[A-Z][A-Za-z0-9_]*|VK_[A-Z0-9_]+|ID3D12[A-Za-z0-9_]*|D3D12_[A-Za-z0-9_]+|MTL[A-Z][A-Za-z0-9_]*|Vma[A-Z][A-Za-z0-9_]*)\b'
  },
  [pscustomobject]@{
    category = "native-getter"
    pattern = '\b(getNative[A-Za-z0-9_]*|resolve[A-Za-z0-9_]*Native[A-Za-z0-9_]*|native(Image|Buffer|Handle|Descriptor|Command|Pipeline|Layout|Allocator)[A-Za-z0-9_]*)\b'
  },
  [pscustomobject]@{
    category = "legacy-binding-vocab"
    pattern = '\b(BindGroup|BindTable|bindBindGroup|DescriptorSetOpaque|registerExternalBindGroup|get[A-Za-z0-9_]*DescriptorSet)\b'
  },
  [pscustomobject]@{
    category = "hot-path-risk"
    pattern = '\b(std::unordered_map|unordered_map|new\s+[A-Za-z_]|make_unique|make_shared|vkCreate[A-Za-z0-9_]*)\b'
  }
)

$files = @(Get-ScanFiles -Root $RepoRoot)
$findings = @(Invoke-RegexScan -Files $files -Rules $rules -Root $RepoRoot -BaselineEntries $baselineEntries)
$blockingFindings = @()
if ($Enforce) {
  $blockingFindings = @($findings | Where-Object { Test-EnforceBlockingFinding -Finding $_ })
}
$now = (Get-Date).ToUniversalTime().ToString("o")

$summary = $findings |
  Group-Object severity, category |
  Sort-Object Name |
  ForEach-Object {
    $parts = $_.Name -split ', '
    [pscustomobject]@{
      severity = $parts[0]
      category = $parts[1]
      count = $_.Count
    }
  }

$baselineReportPath = Convert-ToRepoPath -InputPath $baselinePath -Root $RepoRoot
$markdownReportPath = Convert-ToRepoPath -InputPath $markdownPath -Root $RepoRoot
$jsonReportPath = Convert-ToRepoPath -InputPath $jsonPath -Root $RepoRoot

$metadata = [pscustomobject]@{
  generated_at = $now
  mode = if ($Enforce) { "enforce" } else { "report" }
  exit_policy = if ($Enforce) { "fail-on-public-native-or-terminology" } else { "always-zero" }
  scan_roots = @("app/", "render/", "common/", "rhi/*.h")
  include_backend_dirs = [bool]$IncludeBackendDirs
  baseline_path = $baselineReportPath
  report_paths = @($markdownReportPath, $jsonReportPath)
  blocking_findings = $blockingFindings.Count
}

$report = [pscustomobject]@{
  metadata = $metadata
  summary = @($summary)
  findings = @($findings)
}

$json = $report | ConvertTo-Json -Depth 8
Write-Utf8NoBomFile -Path $jsonPath -Content $json

$markdown = New-Object System.Collections.Generic.List[string]
$markdown.Add("# RHI Boundary Report")
$markdown.Add("")
$markdown.Add("- Generated: $now")
$markdown.Add("- Mode: $(if ($Enforce) { "enforce" } else { "report" })")
$markdown.Add("- Exit policy: $(if ($Enforce) { "fail on public native leaks or terminology" } else { "always zero" })")
$markdown.Add("- Scan roots: app/, render/, common/, rhi/*.h")
$markdown.Add("- Baseline: $baselineReportPath")
$markdown.Add("- Blocking findings: $($blockingFindings.Count)")
$markdown.Add("")
$markdown.Add("## Summary")
$markdown.Add("")
$markdown.Add("| Severity | Category | Count |")
$markdown.Add("|----------|----------|-------|")
if ($summary.Count -eq 0) {
  $markdown.Add("| Info | none | 0 |")
} else {
  foreach ($row in $summary) {
    $markdown.Add("| $($row.severity) | $($row.category) | $($row.count) |")
  }
}
$markdown.Add("")
$markdown.Add("## Findings")
$markdown.Add("")
$markdown.Add("| Severity | Category | Path | Line | Owner Phase | Baseline | Text |")
$markdown.Add("|----------|----------|------|------|-------------|----------|------|")
if ($findings.Count -eq 0) {
  $markdown.Add("| Info | none | - | - | - | - | No findings. |")
} else {
  foreach ($finding in $findings) {
    $text = $finding.text.Replace("|", "\|")
    $markdown.Add("| $($finding.severity) | $($finding.category) | $($finding.path) | $($finding.line) | $($finding.owner_phase) | $($finding.baseline_status) | ``$text`` |")
  }
}
$markdown.Add("")
$markdown.Add("## Notes")
$markdown.Add("")
$markdown.Add("- Report mode is inventory-only and always exits zero.")
$markdown.Add("- Enforce mode exits non-zero for public-surface native leaks or legacy binding terminology.")
$markdown.Add("- Backend implementation directories are not scanned by default.")
$markdown.Add("- Baseline-aware Warning classification is enabled when .planning/guards/rhi-boundary-baseline.json exists.")

Write-Utf8NoBomFile -Path $markdownPath -Content ($markdown -join "`n")

Write-Host "RHI boundary report written:"
Write-Host "  $markdownPath"
Write-Host "  $jsonPath"
Write-Host "Findings: $($findings.Count)"
if ($Enforce -and $blockingFindings.Count -gt 0) {
  Write-Error "RHI boundary enforcement failed with $($blockingFindings.Count) blocking finding(s). See $markdownPath"
  exit 1
}

exit 0
