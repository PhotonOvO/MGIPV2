[CmdletBinding()]
param(
    [Parameter(Mandatory = $true)][string]$BuildRoot,
    [Parameter(Mandatory = $true)][string]$EvidenceRoot,
    [ValidateSet(
        "U01-empty-new-open",
        "U02-hierarchy",
        "U03-transform-gizmo",
        "U04-material",
        "U05-camera-light-vis",
        "U06-merge-compact-save",
        "U07-modified-failures",
        "U08-large-unicode-dpi")]
    [string]$Case = "U02-hierarchy",
    [ValidateSet("Debug", "Release")][string]$Configuration = "Debug",
    [ValidateRange(640, 3840)][int]$Width = 1280,
    [ValidateRange(480, 2160)][int]$Height = 720,
    [ValidateRange(10, 600)][int]$TimeoutSeconds = 180,
    [ValidateRange(1, 4)][double]$DpiScale = 1.0,
    [switch]$Undecorated,
    # Screen capture is the only accepted screenshot channel, and the order
    # matters.  A window-DC BitBlt reads the DWM redirection surface, which
    # for a hardware-presented swapchain window can still hold a pooled frame
    # left over from an earlier run: in the 192-run 2026-09-09 matrix, 48
    # roots shared a byte-identical client.png with an unrelated run (other
    # case, other configuration, other UI DPI), which no per-frame audit can
    # reject.  CopyFromScreen reads the composited screen output instead, so
    # it is attempted first.
    [ValidateSet("desktop-copy", "window-dc-bitblt", "print-window")]
    [string[]]$CaptureOrder = @("desktop-copy", "window-dc-bitblt", "print-window")
)

Set-StrictMode -Version Latest
$ErrorActionPreference = "Stop"

$BuildRoot = [IO.Path]::GetFullPath($BuildRoot)
$EvidenceRoot = [IO.Path]::GetFullPath($EvidenceRoot)
$RepoRoot = [IO.Path]::GetFullPath((Join-Path $PSScriptRoot ".."))
$Executable = Join-Path $BuildRoot "editor_ui_live_replay.exe"
if (-not (Test-Path -LiteralPath $Executable -PathType Leaf)) {
    $candidate = Join-Path $BuildRoot "bin\$Configuration\editor_ui_live_replay.exe"
    if (Test-Path -LiteralPath $candidate -PathType Leaf) {
        $Executable = $candidate
    }
}
if (-not (Test-Path -LiteralPath $Executable -PathType Leaf)) {
    throw "editor_ui_live_replay executable is missing under $BuildRoot"
}
$CachePath = Join-Path $BuildRoot "CMakeCache.txt"
if (-not (Test-Path -LiteralPath $CachePath -PathType Leaf)) {
    throw "CMakeCache.txt is missing under BuildRoot"
}
$CacheLines = Get-Content -LiteralPath $CachePath
function Get-CacheValue([string]$Name) {
    $line = $CacheLines | Where-Object { $_ -match ("^" + [regex]::Escape($Name) + ":[^=]+=") }
    if (@($line).Count -ne 1) {
        throw "CMake cache key has invalid cardinality: $Name"
    }
    return ([string]$line).Substring(([string]$line).IndexOf("=") + 1).Trim()
}
$ConfiguredBuildType = Get-CacheValue "CMAKE_BUILD_TYPE"
$ConfiguredTesting = Get-CacheValue "BUILD_TESTING"
$ConfiguredPreflightSkip = Get-CacheValue "DEMO_SKIP_EDITOR_DEPENDENCY_PREFLIGHT"
if ($ConfiguredBuildType -ne $Configuration -or
    $ConfiguredTesting -ne "ON" -or
    $ConfiguredPreflightSkip -ne "OFF") {
    throw "BuildRoot configuration does not match the live acceptance contract"
}
$CacheHash = (Get-FileHash -LiteralPath $CachePath -Algorithm SHA256).Hash.ToLowerInvariant()
if (Test-Path -LiteralPath $EvidenceRoot) {
    throw "EvidenceRoot must be fresh and absent: $EvidenceRoot"
}
New-Item -ItemType Directory -Path $EvidenceRoot | Out-Null

$AppEvidence = Join-Path $EvidenceRoot "app"
$SyncRoot = Join-Path $EvidenceRoot "sync"
$PngPath = Join-Path $EvidenceRoot "client.png"
$StdoutPath = Join-Path $EvidenceRoot "stdout.log"
$StderrPath = Join-Path $EvidenceRoot "stderr.log"
$ManifestPath = Join-Path $EvidenceRoot "manifest.json"
$SourceIdentityPath = Join-Path $EvidenceRoot "source-identity.json"

function Get-Hash([string]$Path) {
    return (Get-FileHash -LiteralPath $Path -Algorithm SHA256).Hash.ToLowerInvariant()
}

function Get-BytesHash([byte[]]$Bytes) {
    $algorithm = [Security.Cryptography.SHA256]::Create()
    try {
        return ([BitConverter]::ToString($algorithm.ComputeHash($Bytes))).Replace("-", "").ToLowerInvariant()
    }
    finally {
        $algorithm.Dispose()
    }
}

function Write-Json([string]$Path, [object]$Value) {
    $json = $Value | ConvertTo-Json -Depth 100
    [IO.File]::WriteAllText(
        $Path, $json + [Environment]::NewLine,
        [Text.UTF8Encoding]::new($false))
}

function Quote-Argument([string]$Value) {
    return '"' + $Value.Replace('"', '\"') + '"'
}

function Get-EvidenceRelativePath([string]$Path) {
    $rootPrefix = $EvidenceRoot.TrimEnd('\', '/') + [IO.Path]::DirectorySeparatorChar
    $fullPath = [IO.Path]::GetFullPath($Path)
    if (-not $fullPath.StartsWith(
            $rootPrefix, [StringComparison]::OrdinalIgnoreCase)) {
        throw "Artifact path escapes EvidenceRoot: $fullPath"
    }
    return $fullPath.Substring($rootPrefix.Length).Replace('\', '/')
}

$SourcePaths = @(
    "CMakeLists.txt",
    "app/MinimalLatestApp.h",
    "app/MinimalLatestApp.cpp",
    "app/editor/EditorUiLiveReplayMain.cpp",
    "app/editor/EditorTestDriver.h",
    "app/editor/EditorTestDriver.cpp",
    "app/editor/EditorController.h",
    "app/editor/EditorController.cpp",
    "app/editor/SceneEditorUi.h",
    "app/editor/SceneEditorUi.cpp",
    "app/editor/SceneEditorUiModel.h",
    "app/editor/EditorRendererAdapter.h",
    "app/editor/EditorRendererAdapter.cpp",
    "app/editor/EditorViewportRuntime.h",
    "app/editor/EditorViewportRuntime.cpp",
    "scene/editor/include/mgif/scene_editor/GltfDocument.h",
    "scene/editor/GltfDocument.cpp",
    "scene/editor/include/mgif/scene_editor/SceneCommands.h",
    "scene/editor/SceneCommands.cpp",
    "scene/editor/include/mgif/scene_editor/SceneProjection.h",
    "scene/editor/SceneProjection.cpp",
    "render/RendererFacade.h",
    "render/RendererFacade.cpp",
    "render/RenderDevice.h",
    "render/RenderDevice.cpp",
    "render/FrameScheduler.h",
    "render/FrameScheduler.cpp",
    "render/PassExecutor.h",
    "render/PassExecutor.cpp",
    "render/SceneResources.h",
    "render/SceneResources.cpp",
    "render/GPUDrivenRenderer.h",
    "render/GPUDrivenRenderer.cpp",
    "render/passes/GPUDrivenPresentPass.h",
    "render/passes/GPUDrivenPresentPass.cpp",
    "render/passes/GPUDrivenImguiPass.h",
    "render/passes/GPUDrivenImguiPass.cpp",
    "render/passes/EditorOverlayPass.h",
    "render/passes/EditorOverlayPass.cpp",
    "render/passes/EditorPickingPass.h",
    "render/passes/EditorPickingPass.cpp",
    "rhi/vulkan/VulkanDevice.h",
    "rhi/vulkan/VulkanDevice.cpp",
    "rhi/vulkan/VulkanQueue.h",
    "rhi/vulkan/VulkanQueue.cpp",
    "rhi/vulkan/VulkanSwapchain.h",
    "rhi/vulkan/VulkanSwapchain.cpp",
    "rhi/vulkan/VulkanCommandBuffer.h",
    "rhi/vulkan/VulkanCommandBuffer.cpp",
    "shaders/editor_overlay.slang",
    "shaders/editor_picking.slang",
    "tests/fixtures/gltf_editor/r09_shared_mesh.gltf",
    "tests/fixtures/gltf_editor/live_camera_light.gltf",
    "tests/fixtures/gltf_editor/feature_preservation.gltf",
    "tests/fixtures/gltf_editor/material_extensions.gltf",
    "tests/fixtures/gltf_editor/unicode_names.gltf",
    "tests/fixtures/gltf_editor/multi_scene_oracle.gltf",
    "tests/fixtures/gltf_editor/minimal_scene.gltf",
    "tools/validate_editor_p0_artifacts.py",
    "tools/editor_ui_live_replay.ps1",
    "tools/generate_editor_dependency_identity.py",
    "tests/editor/editor_ui_action_tests.cpp",
    "tests/editor/editor_ui_probe_tests.cpp",
    "tests/editor/editor_ui_replay_tests.cpp",
    "tests/editor/editor_ui_semantic_tests.cpp",
    "tests/editor/editor_ui_state_tests.cpp",
    "tests/editor/editor_ui_os_replay_cli_tests.cpp",
    "tests/editor/editor_ui_os_replay_plan_tests.cpp",
    "tests/editor/editor_ui_os_replay_runtime_tests.cpp",
    "tests/editor/minimal_app_editor_authority_contract_tests.py",
    "tests/editor/editor_dependency_gate_integration_tests.py"
)
$SourceRecords = @(
    foreach ($relative in ($SourcePaths | Sort-Object -CaseSensitive)) {
        $path = Join-Path $RepoRoot $relative
        if (-not (Test-Path -LiteralPath $path -PathType Leaf)) {
            throw "Source identity path is missing: $relative"
        }
        [ordered]@{
            path = $relative.Replace("\", "/")
            bytes = (Get-Item -LiteralPath $path).Length
            sha256 = Get-Hash $path
        }
    }
)
$IdentityLines = @(
    foreach ($record in $SourceRecords) {
        $record.path + [char]9 + $record.bytes + [char]9 + $record.sha256
    }
)
$IdentityPayload = ($IdentityLines -join [char]10) + [char]10
$SourceIdentityHash = Get-BytesHash(
    [Text.UTF8Encoding]::new($false).GetBytes($IdentityPayload))
$ExecutableHash = Get-Hash $Executable
Write-Json $SourceIdentityPath ([ordered]@{
    documentKind = "mgif.editor-ui-live-source-identity"
    schemaVersion = 1
    algorithm = "sha256"
    aggregateSha256 = $SourceIdentityHash
    records = $SourceRecords
})

Add-Type -AssemblyName System.Drawing
Add-Type @"
using System;
using System.Runtime.InteropServices;
public static class MGIFEditorLiveNative {
    [StructLayout(LayoutKind.Sequential)] public struct RECT {
        public int Left; public int Top; public int Right; public int Bottom;
    }
    [StructLayout(LayoutKind.Sequential)] public struct POINT {
        public int X; public int Y;
    }
    [DllImport("user32.dll")] public static extern bool GetClientRect(IntPtr hWnd, out RECT rect);
    [DllImport("user32.dll")] public static extern bool GetWindowRect(IntPtr hWnd, out RECT rect);
    [DllImport("user32.dll")] public static extern bool AdjustWindowRectEx(ref RECT rect, uint style, bool menu, uint exStyle);
    [DllImport("user32.dll")] public static extern int GetSystemMetrics(int index);
    [DllImport("user32.dll")] public static extern bool ClientToScreen(IntPtr hWnd, ref POINT point);
    [DllImport("user32.dll")] public static extern IntPtr GetForegroundWindow();
    [DllImport("user32.dll")] public static extern IntPtr WindowFromPoint(POINT point);
    [DllImport("user32.dll")] public static extern uint GetWindowThreadProcessId(IntPtr hWnd, out uint processId);
    [DllImport("user32.dll")] public static extern bool SetForegroundWindow(IntPtr hWnd);
    [DllImport("user32.dll")] public static extern bool BringWindowToTop(IntPtr hWnd);
    [DllImport("user32.dll")] public static extern bool ShowWindow(IntPtr hWnd, int command);
    [DllImport("user32.dll")] public static extern bool SetWindowPos(
        IntPtr hWnd, IntPtr insertAfter, int x, int y, int cx, int cy, uint flags);
    [DllImport("dwmapi.dll")] public static extern int DwmFlush();
    [DllImport("user32.dll")] public static extern bool PrintWindow(IntPtr hWnd, IntPtr hdc, uint flags);
    [DllImport("user32.dll")] public static extern IntPtr GetDC(IntPtr hWnd);
    [DllImport("user32.dll")] public static extern int ReleaseDC(IntPtr hWnd, IntPtr hdc);
    [DllImport("gdi32.dll")] public static extern bool BitBlt(
        IntPtr destination, int x, int y, int width, int height,
        IntPtr source, int sourceX, int sourceY, uint operation);
}
"@

function Resize-Client([Diagnostics.Process]$Process) {
    $Process.Refresh()
    $handle = $Process.MainWindowHandle
    if ($handle -eq [IntPtr]::Zero) { return $false }
    $client = New-Object MGIFEditorLiveNative+RECT
    $window = New-Object MGIFEditorLiveNative+RECT
    if (-not [MGIFEditorLiveNative]::GetClientRect($handle, [ref]$client) -or
        -not [MGIFEditorLiveNative]::GetWindowRect($handle, [ref]$window)) {
        throw "Could not query live replay window bounds"
    }
    $clientWidth = $client.Right - $client.Left
    $clientHeight = $client.Bottom - $client.Top
    $outerWidth = ($window.Right - $window.Left) + $Width - $clientWidth
    $outerHeight = ($window.Bottom - $window.Top) + $Height - $clientHeight
    if (-not [MGIFEditorLiveNative]::SetWindowPos(
            $handle, [IntPtr](-1), 0, 0, $outerWidth, $outerHeight, 0x0040)) {
        throw "Could not resize live replay client"
    }
    Start-Sleep -Milliseconds 150
    return $true
}

function Get-PixelAudit([Drawing.Bitmap]$Bitmap) {
    $sampled = 0
    $nonBlack = 0
    $colorful = 0
    $edges = 0
    $auditRows = 0
    $coherentRuns = 0
    $structuredRows = 0
    $luminanceSum = 0.0
    $luminanceSquareSum = 0.0
    $colors = [Collections.Generic.HashSet[int]]::new()
    for ($y = 0; $y -lt $Bitmap.Height; $y += 4) {
        $previousLuminance = -1.0
        $runLength = 0
        $rowNonBlack = 0
        ++$auditRows
        for ($x = 0; $x -lt $Bitmap.Width; $x += 4) {
            $pixel = $Bitmap.GetPixel($x, $y)
            $max = [Math]::Max($pixel.R, [Math]::Max($pixel.G, $pixel.B))
            $min = [Math]::Min($pixel.R, [Math]::Min($pixel.G, $pixel.B))
            ++$sampled
            if ($max -gt 20) {
                ++$nonBlack
                ++$rowNonBlack
                ++$runLength
            }
            else {
                if ($runLength -ge 3) { ++$coherentRuns }
                $runLength = 0
            }
            if (($max - $min) -gt 20 -and $max -gt 40) { ++$colorful }
            # Windows PowerShell 5.1 keeps the operand type for -shl, so a
            # [byte] shifted by 16 or by 8 truncates to 0 and the packed value
            # degenerates to the blue channel alone.  Measured on the same
            # frames, the byte form reports 141 / 86 / 46 / 171 / 41 distinct
            # values where the [int] form reports 562 / 114 / 51 / 444 / 49,
            # matching an independent PIL count exactly.  Every uniqueColors
            # value recorded before this fix is a blue-channel count, not a
            # colour count.
            [void]$colors.Add((([int]$pixel.R) -shl 16) -bor
                              (([int]$pixel.G) -shl 8) -bor ([int]$pixel.B))
            $luminance = ($pixel.R + $pixel.G + $pixel.B) / 3.0
            $luminanceSum += $luminance
            $luminanceSquareSum += $luminance * $luminance
            if ($previousLuminance -ge 0 -and
                [Math]::Abs($luminance - $previousLuminance) -gt 24) { ++$edges }
            $previousLuminance = $luminance
        }
        if ($runLength -ge 3) { ++$coherentRuns }
        if ($rowNonBlack -ge 8) { ++$structuredRows }
    }
    $luminanceMean = $luminanceSum / $sampled
    $variance = ($luminanceSquareSum / $sampled) - ($luminanceMean * $luminanceMean)
    $luminanceStdDev = [Math]::Sqrt([Math]::Max(0.0, $variance))
    $nonBlackRatio = $nonBlack / $sampled
    $colorfulRatio = $colorful / $sampled
    $edgeRatio = $edges / $sampled
    $coherentRunRatio = $coherentRuns / $auditRows
    $structuredRowRatio = $structuredRows / $auditRows
    # Blank-capture gate, recalibrated 2026-09-09 against frames that the
    # visual harness already accepts as genuine.  The previous brightness-led
    # floors (nonBlackRatio>=0.05, colorfulRatio>=0.02, edgeRatio>=0.03) were
    # tuned on a 20260908 live frame whose panels sat on a lit scene
    # (nonBlackRatio 1.000, uniqueColors 562).  Genuine dark frames in the
    # current binary measure far lower, e.g. V01-grid perspective-horizon
    # uniqueColors 48 / distinct2x2 181 / nonBlackRatio 0.023 /
    # coherentRunRatio 0.285 / structuredRowRatio 0.330, and the 1440p
    # undecorated probe 49 / 162 / 0.014 / 0.242 / 0.242.  The live boundary
    # frame measured 51 / 178 / 0.033 / 0.283 / 0.267, i.e. inside the genuine
    # range, yet was rejected as blank -- so the gate was selecting for the
    # bright garbage (two-colour phantom-monitor placeholder, foreign window)
    # and rejecting real frames.  Structure now decides, brightness only
    # rejects uniform surfaces:
    # Unique-colour figures below are measured with the [int] cast above:
    #   genuine dark   coherent 0.242-0.285  structured 0.242-0.330  unique 49-51
    #   genuine bright coherent 1.000        structured 1.000        unique 444-562
    #   degraded       coherent 0.1444       structured 0.1778
    #   black blank    coherent 0.000        structured 0.000        unique 1
    #   white blank    coherent 1.000        structured 1.000        unique 1-2
    #   two-colour placeholder nonBlackRatio 1.000 but unique 114
    # Mean and standard deviation stay recorded, never gated, so a future light
    # theme is not rejected by theme-specific numbers.
    # Brightness floors sit below the genuine minima (nonBlackRatio 0.014,
    # colorfulRatio 0.0019, edgeRatio 0.0120) and above every blank
    # measurement (0.0), so structure decides and brightness only rejects
    # uniform surfaces.
    $uniformSurface = ($nonBlackRatio -ge 0.90 -and $colors.Count -lt 200)
    if ($colors.Count -lt 32 -or
        $coherentRunRatio -lt 0.19 -or $structuredRowRatio -lt 0.21 -or
        $nonBlackRatio -lt 0.01 -or $colorfulRatio -lt 0.001 -or
        $edgeRatio -lt 0.008 -or $uniformSurface) {
        throw ("Client PNG is blank or low-information: nonBlackRatio=" +
               [Math]::Round($nonBlackRatio, 4) + " colorfulRatio=" +
               [Math]::Round($colorfulRatio, 4) + " edgeRatio=" +
               [Math]::Round($edgeRatio, 4) + " unique=" + $colors.Count +
               " coherentRunRatio=" + [Math]::Round($coherentRunRatio, 4) +
               " structuredRowRatio=" + [Math]::Round($structuredRowRatio, 4) +
               " uniformSurface=" + $uniformSurface +
               " edges=" + $edges + " luminanceMean=" +
               [Math]::Round($luminanceMean, 1))
    }
    return [ordered]@{
        sampled = $sampled
        nonBlack = $nonBlack
        colorful = $colorful
        uniqueColors = $colors.Count
        edgeTransitions = $edges
        nonBlackRatio = [Math]::Round($nonBlackRatio, 4)
        colorfulRatio = [Math]::Round($colorfulRatio, 4)
        edgeRatio = [Math]::Round($edgeRatio, 4)
        coherentRunRatio = [Math]::Round($coherentRunRatio, 4)
        structuredRowRatio = [Math]::Round($structuredRowRatio, 4)
        uniformSurface = $uniformSurface
        luminanceMean = [Math]::Round($luminanceMean, 1)
        luminanceStdDev = [Math]::Round($luminanceStdDev, 1)
    }
}

function Capture-Client([Diagnostics.Process]$Process, [string]$Path) {
    $Process.Refresh()
    $handle = $Process.MainWindowHandle
    if ($handle -eq [IntPtr]::Zero) { throw "Live replay has no main window" }
    [void][MGIFEditorLiveNative]::ShowWindow($handle, 9)
    [void][MGIFEditorLiveNative]::BringWindowToTop($handle)
    [void][MGIFEditorLiveNative]::SetForegroundWindow($handle)
    Start-Sleep -Milliseconds 350
    [void][MGIFEditorLiveNative]::DwmFlush()

    $rect = New-Object MGIFEditorLiveNative+RECT
    $origin = New-Object MGIFEditorLiveNative+POINT
    if (-not [MGIFEditorLiveNative]::GetClientRect($handle, [ref]$rect) -or
        -not [MGIFEditorLiveNative]::ClientToScreen($handle, [ref]$origin)) {
        throw "Could not resolve live replay client rectangle"
    }
    $width = $rect.Right - $rect.Left
    $height = $rect.Bottom - $rect.Top
    if ($width -lt 640 -or $height -lt 480) {
        throw ("Live replay client is unexpectedly small: " + $width + "x" + $height)
    }
    if ($width -gt $primaryWidth -or $height -gt $primaryHeight) {
        throw ("Live replay client " + $width + "x" + $height + " does not fit the " +
               $primaryWidth + "x" + $primaryHeight +
               " primary monitor, so its presented pixels cannot be captured here")
    }
    # GLFW centres the window on the phantom second half of the virtual screen
    # (measured client origin 3222,440 for a 1280x720 client), where no display
    # exists.  Every live client.png captured there was the same two-colour
    # placeholder (48,10,36)/(40,50,40) with no ImGui panels at all, and it was
    # byte-identical across unrelated cases, configurations and DPI values.
    # Pin the client rectangle inside the physical primary monitor first.
    $targetClientX = [int][Math]::Floor(($primaryWidth - $width) / 2)
    $targetClientY = [int][Math]::Floor(($primaryHeight - $height) / 2)
    $pinnedToPrimary = $false
    if ($origin.X -ne $targetClientX -or $origin.Y -ne $targetClientY) {
        $windowRect = New-Object MGIFEditorLiveNative+RECT
        if (-not [MGIFEditorLiveNative]::GetWindowRect($handle, [ref]$windowRect)) {
            throw "Could not query live replay window bounds for primary-monitor pinning"
        }
        $targetWindowX = $windowRect.Left + ($targetClientX - $origin.X)
        $targetWindowY = $windowRect.Top + ($targetClientY - $origin.Y)
        # HWND_TOPMOST = -1, SWP_NOSIZE (0x0001) | SWP_NOACTIVATE (0x0010)
        if (-not [MGIFEditorLiveNative]::SetWindowPos(
                $handle, [IntPtr](-1), $targetWindowX, $targetWindowY, 0, 0, 0x0011)) {
            throw "Could not pin the live replay window inside the primary monitor"
        }
        Start-Sleep -Milliseconds 300
        [void][MGIFEditorLiveNative]::BringWindowToTop($handle)
        [void][MGIFEditorLiveNative]::SetForegroundWindow($handle)
        Start-Sleep -Milliseconds 200
        [void][MGIFEditorLiveNative]::DwmFlush()
        $rect = New-Object MGIFEditorLiveNative+RECT
        $origin = New-Object MGIFEditorLiveNative+POINT
        if (-not [MGIFEditorLiveNative]::GetClientRect($handle, [ref]$rect) -or
            -not [MGIFEditorLiveNative]::ClientToScreen($handle, [ref]$origin)) {
            throw "Could not re-resolve live replay client rectangle after pinning"
        }
        $pinnedToPrimary = $true
        $width = $rect.Right - $rect.Left
        $height = $rect.Bottom - $rect.Top
    }
    if ($origin.X -lt 0 -or $origin.Y -lt 0 -or
        ($origin.X + $width) -gt $primaryWidth -or
        ($origin.Y + $height) -gt $primaryHeight) {
        throw ("Live replay client origin (" + $origin.X + "," + $origin.Y + ") with extent " +
               $width + "x" + $height + " is not inside the " + $primaryWidth + "x" +
               $primaryHeight + " primary monitor; refusing to capture a region that " +
               "has no physical display surface")
    }
    # A screen copy reads whatever window is topmost at those coordinates.  Once
    # the client rectangle sits on the physical monitor it can still be covered by
    # an unrelated bright window, which silently photographs that window instead
    # of the editor (measured luminance mean 151.8 with 33% pure white against a
    # source-pinned dark theme: StyleColorsDark + WindowBg 0.018/0.022/0.030).
    # Assert topmost ownership of the client centre before any transport reads it.
    $centre = New-Object MGIFEditorLiveNative+POINT
    $centre.X = $origin.X + [int][Math]::Floor($width / 2)
    $centre.Y = $origin.Y + [int][Math]::Floor($height / 2)
    $processId = [uint32]$Process.Id
    $topmostVerified = $false
    $foregroundMatches = $false
    for ($attempt = 1; $attempt -le 3; ++$attempt) {
        [void][MGIFEditorLiveNative]::SetWindowPos(
            $handle, [IntPtr](-1), 0, 0, 0, 0, 0x0013)   # NOSIZE|NOMOVE|NOACTIVATE
        [void][MGIFEditorLiveNative]::BringWindowToTop($handle)
        [void][MGIFEditorLiveNative]::SetForegroundWindow($handle)
        Start-Sleep -Milliseconds 250
        [void][MGIFEditorLiveNative]::DwmFlush()
        $foregroundMatches =
            ([MGIFEditorLiveNative]::GetForegroundWindow() -eq $handle)
        $hit = [MGIFEditorLiveNative]::WindowFromPoint($centre)
        if ($hit -eq $handle) { $topmostVerified = $true; break }
        if ($hit -ne [IntPtr]::Zero) {
            $hitProcess = [uint32]0
            [void][MGIFEditorLiveNative]::GetWindowThreadProcessId($hit, [ref]$hitProcess)
            if ($hitProcess -eq $processId) { $topmostVerified = $true; break }
        }
    }
    if (-not $topmostVerified) {
        throw ("Live replay client centre (" + $centre.X + "," + $centre.Y +
               ") is covered by another process window, so a screen copy would " +
               "photograph that window instead of the editor")
    }
    $bitmap = [Drawing.Bitmap]::new($width, $height)
    $graphics = [Drawing.Graphics]::FromImage($bitmap)
    $transport = "none"
    $audit = $null
    $lastAuditError = "none"
    try {
        foreach ($candidate in $CaptureOrder) {
            $filled = $false
            if ($candidate -eq "desktop-copy") {
                $graphics.CopyFromScreen(
                    $origin.X, $origin.Y, 0, 0,
                    [Drawing.Size]::new($width, $height),
                    [Drawing.CopyPixelOperation]::SourceCopy)
                $filled = $true
            }
            elseif ($candidate -eq "window-dc-bitblt") {
                $source = [MGIFEditorLiveNative]::GetDC($handle)
                if ($source -ne [IntPtr]::Zero) {
                    $destination = $graphics.GetHdc()
                    try {
                        $filled = [MGIFEditorLiveNative]::BitBlt(
                            $destination, 0, 0, $width, $height,
                            $source, 0, 0, 0x00CC0020)
                    }
                    finally {
                        $graphics.ReleaseHdc($destination)
                        [void][MGIFEditorLiveNative]::ReleaseDC($handle, $source)
                    }
                }
            }
            elseif ($candidate -eq "print-window") {
                $destination = $graphics.GetHdc()
                try {
                    $filled = [MGIFEditorLiveNative]::PrintWindow(
                        $handle, $destination, 0x00000003)
                }
                finally {
                    $graphics.ReleaseHdc($destination)
                }
            }
            else {
                throw ("Unknown live capture transport: " + $candidate)
            }
            if (-not $filled) { continue }
            try {
                $audit = Get-PixelAudit $bitmap
                $transport = $candidate
                break
            }
            catch { $audit = $null; $lastAuditError = $_.Exception.Message }
        }
        if ($null -eq $audit) {
            # Keep the rejected surface so a blank verdict can be diagnosed from
            # deterministic filesystem evidence instead of guesswork.
            $rejectedPath = $Path + ".rejected.png"
            try { $bitmap.Save($rejectedPath, [Drawing.Imaging.ImageFormat]::Png) } catch {}
            throw ("Live capture failed every transport in order: " +
                   ($CaptureOrder -join ",") +
                   " clientOrigin=(" + $origin.X + "," + $origin.Y + ")" +
                   " extent=" + $width + "x" + $height +
                   " lastAuditError=" + $lastAuditError +
                   " rejected=" + $rejectedPath)
        }
        $bitmap.Save($Path, [Drawing.Imaging.ImageFormat]::Png)
    }
    finally {
        $graphics.Dispose()
        $bitmap.Dispose()
    }
    return [ordered]@{
        width = $width
        height = $height
        transport = $transport
        audit = $audit
        clientOriginX = $origin.X
        clientOriginY = $origin.Y
        pinnedToPrimary = $pinnedToPrimary
        topmostVerified = $topmostVerified
        foregroundMatches = $foregroundMatches
    }
}

# A decorated window's outer rectangle is limited to the virtual screen by the
# default WM_GETMINMAXINFO max track size, so the client area can never reach
# the full screen extent.  Measured on this host (virtual screen 2560x1440,
# work area 2560x1392): a --width=2560 --height=1440 request produced a
# 2560x1421 client in both Debug and Release because the non-client frame is
# 19 px tall, and moving the window did not change it.  Fail before launching
# instead of after a full run; the post-capture extent check stays
# authoritative.
$virtualWidth = [MGIFEditorLiveNative]::GetSystemMetrics(78)
$virtualHeight = [MGIFEditorLiveNative]::GetSystemMetrics(79)
# This host reports a 5120x1440 virtual screen while the only physical display is
# the 2560x1440 primary monitor at (0,0).  The second half (x >= 2560) has no
# display surface, so reading it back yields a deterministic DWM placeholder
# instead of the presented swapchain image.  Reachability and capture are
# therefore judged against the primary monitor, not the virtual screen.
$primaryWidth = [MGIFEditorLiveNative]::GetSystemMetrics(0)
$primaryHeight = [MGIFEditorLiveNative]::GetSystemMetrics(1)
$probeRect = New-Object MGIFEditorLiveNative+RECT
$probeRect.Left = 0
$probeRect.Top = 0
$probeRect.Right = $Width
$probeRect.Bottom = $Height
# WS_CAPTION | WS_THICKFRAME | WS_MINIMIZEBOX | WS_MAXIMIZEBOX | WS_SYSMENU
$probeStyle = [uint32]0x00CF0000
if ($Undecorated) { $probeStyle = [uint32]0 }
if (-not [MGIFEditorLiveNative]::AdjustWindowRectEx([ref]$probeRect, $probeStyle, $false, [uint32]0)) {
    throw "AdjustWindowRectEx failed while probing the requested client extent"
}
$requiredOuterWidth = $probeRect.Right - $probeRect.Left
$requiredOuterHeight = $probeRect.Bottom - $probeRect.Top
if ($Undecorated) { $WindowDecoration = "undecorated" } else { $WindowDecoration = "decorated" }
if ($requiredOuterWidth -gt $primaryWidth -or $requiredOuterHeight -gt $primaryHeight) {
    throw ("Requested client " + $Width + "x" + $Height + " needs a " +
           $requiredOuterWidth + "x" + $requiredOuterHeight + " " + $WindowDecoration +
           " outer window, which exceeds the " + $primaryWidth + "x" + $primaryHeight +
           " primary monitor (measured decorated reachable maximum 2560x1421). Pass " +
           "-Undecorated to drop the non-client frame, or use a smaller client extent.")
}

$DpiScaleToken = $DpiScale.ToString("0.###", [Globalization.CultureInfo]::InvariantCulture)
$Arguments = @(
    "--backend=vulkan",
    "--editor-live-case=$Case",
    "--editor-live-evidence-dir=$AppEvidence",
    "--editor-live-sync-dir=$SyncRoot",
    "--editor-live-executable-sha256=$ExecutableHash",
    "--editor-live-source-sha256=$SourceIdentityHash",
    "--capture-sync-timeout-ms=$($TimeoutSeconds * 1000)",
    "--width=$Width",
    "--height=$Height",
    "--editor-ui-dpi-scale=$DpiScaleToken"
)
if ($Undecorated) { $Arguments += "--editor-window-undecorated" }
$StartInfo = [Diagnostics.ProcessStartInfo]::new()
$StartInfo.FileName = $Executable
$StartInfo.WorkingDirectory = $BuildRoot
$StartInfo.Arguments = ($Arguments | ForEach-Object { Quote-Argument $_ }) -join " "
$StartInfo.UseShellExecute = $false
$StartInfo.CreateNoWindow = $false
$StartInfo.RedirectStandardOutput = $true
$StartInfo.RedirectStandardError = $true
$StartInfo.EnvironmentVariables["VK_INSTANCE_LAYERS"] = "VK_LAYER_KHRONOS_validation"
$StartInfo.EnvironmentVariables["VK_LOADER_LAYERS_ENABLE"] = "VK_LAYER_KHRONOS_validation"
$StartInfo.EnvironmentVariables["VK_LAYER_ENABLES"] =
    "VK_VALIDATION_FEATURE_ENABLE_SYNCHRONIZATION_VALIDATION_EXT"
[void]$StartInfo.EnvironmentVariables.Remove("VK_LAYER_MESSAGE_ID_FILTER")
$Process = [Diagnostics.Process]::new()
$Process.StartInfo = $StartInfo
if (-not $Process.Start()) { throw "Could not start editor_ui_live_replay" }
$StdoutTask = $Process.StandardOutput.ReadToEndAsync()
$StderrTask = $Process.StandardError.ReadToEndAsync()

$Deadline = [DateTime]::UtcNow.AddSeconds($TimeoutSeconds)
# The live executable creates the requested extent itself. Resizing before its
# first submitted frame can race swapchain/viewport attachment initialization;
# capture the completed client at the post-render fence instead.
$Resized = $true
$Ready = $null
$ReadyPath = $null
$Capture = $null
try {
    while (-not $Process.HasExited -and [DateTime]::UtcNow -lt $Deadline) {
        if (-not $Resized) {
            $Resized = Resize-Client $Process
            if (-not $Resized) {
                Start-Sleep -Milliseconds 50
                continue
            }
        }
        if (Test-Path -LiteralPath $SyncRoot -PathType Container) {
            $readyFiles = @(Get-ChildItem -LiteralPath $SyncRoot -Filter "*.ready.json" -File)
            if ($readyFiles.Count -gt 1) {
                throw "Live replay emitted more than one unconsumed ready marker"
            }
            if ($readyFiles.Count -eq 1) {
                $ReadyPath = $readyFiles[0].FullName
                $Ready = Get-Content -LiteralPath $ReadyPath -Raw | ConvertFrom-Json
                break
            }
        }
        Start-Sleep -Milliseconds 50
        $Process.Refresh()
    }
    if ($null -eq $Ready) {
        throw "Timed out waiting for live replay ready marker"
    }
    if ((Get-Hash $Executable) -ne $ExecutableHash) {
        throw "Live replay executable changed before the ready boundary"
    }
    $ExpectedMarker = "$Case-boundary-only"
    if ([string]$Ready.protocol -ne "mgif-csm-capture-sync-v1" -or
        [string]$Ready.phase -ne "post-render" -or
        [string]$Ready.marker -ne $ExpectedMarker -or
        $null -eq $Ready.rendered_frame) {
        throw "Live replay ready marker contract is invalid"
    }
    if ($null -eq $Ready.live -or
        [string]$Ready.live.case -ne $Case -or
        [string]$Ready.live.dispatch_mode -ne "direct-typed-command" -or
        [bool]$Ready.live.os_click -or [bool]$Ready.live.native_dialog -or
        [string]$Ready.live.executable_sha256 -ne $ExecutableHash -or
        [string]$Ready.live.source_sha256 -ne $SourceIdentityHash) {
        throw "Live replay ready identity/action binding is invalid"
    }
    $StatePath = [string]$Ready.live.state_path
    $ResultPath = [string]$Ready.live.result_path
    if (-not (Test-Path -LiteralPath $StatePath -PathType Leaf) -or
        -not (Test-Path -LiteralPath $ResultPath -PathType Leaf)) {
        throw "Live replay state/result files are missing at the ready fence"
    }
    $State = Get-Content -LiteralPath $StatePath -Raw | ConvertFrom-Json
    $Result = Get-Content -LiteralPath $ResultPath -Raw | ConvertFrom-Json
    if ([string]$State.kind -ne "mgif.editor-ui-live-state" -or
        [string]$Result.kind -ne "mgif.editor-ui-live-result" -or
        [string]$State.case -ne $Case -or [bool]$State.complete -or
        [string]$State.dispatchMode -ne "direct-typed-command" -or
        [bool]$State.osClick -or [bool]$State.nativeDialog -or
        [bool]$State.screenshotClaim -or -not [bool]$State.renderer.settled -or
        -not [bool]$State.renderer.gpu_idle_waited -or
        [string]$State.uiFocus.method -ne
            "acceptance-imgui-window-focus" -or
        -not [bool]$State.uiFocus.scene_browser_tab_visible) {
        throw "Live replay state contract is invalid"
    }
    # The pinned DPI axis is carried by io.FontGlobalScale, so the applied
    # scale is read back from Dear ImGui rather than assumed from the request.
    if ([Math]::Abs([double]$State.uiDpi.requested - $DpiScale) -gt 0.001 -or
        [Math]::Abs([double]$State.uiDpi.applied - $DpiScale) -gt 0.001 -or
        [string]$State.uiDpi.source -ne "imgui-font-global-scale") {
        throw ("Live replay UI DPI contract is invalid: requested=" +
               $State.uiDpi.requested + " applied=" + $State.uiDpi.applied +
               " source=" + $State.uiDpi.source + " expected=" + $DpiScaleToken)
    }
    # A terminal NoDocument boundary (U07 modified-failure recovery) has no
    # document and no projection, so its authority digests are legitimately
    # empty.  Only that exact state may omit them: any document-bearing
    # boundary must still publish complete 64-hex digests.
    $NoDocumentBoundary =
        [string]$State.state.state -eq "NoDocument" -and
        -not [bool]$State.state.authority.hasDocument -and
        -not [bool]$State.state.authority.hasProjection
    foreach ($digest in @(
            [string]$State.state.authority.documentDigest,
            [string]$State.state.authority.projectionDigest)) {
        if ($NoDocumentBoundary -and $digest -eq "") { continue }
        if ($digest -notmatch "^[0-9a-f]{64}$") {
            throw "Live replay state contains a malformed digest"
        }
    }
    if ([string]$State.renderer.runtime_content_digest -notmatch
        "^[0-9a-f]{64}$") {
        throw "Live replay state contains a malformed digest"
    }
    if ([string]$Ready.live.document_digest -ne
            [string]$State.state.authority.documentDigest -or
        [string]$Ready.live.projection_digest -ne
            [string]$State.state.authority.projectionDigest -or
        -not [bool]$Result.result.passed -or
        @($Result.result.steps).Count -ne @($Result.script.steps).Count -or
        @($Result.result.steps).Count -lt 2) {
        throw "Live replay action/result binding is incomplete"
    }
    foreach ($step in @($Result.result.steps)) {
        if ($null -eq $step.beforeState.authority -or
            $null -eq $step.afterState.authority) {
            throw "Live replay action lacks before/after authority"
        }
    }

    # The window transports intermittently observe a blank desktop while the
    # NVIDIA LocalSystem Container service crash-loops (SCM 7023/7031 roughly
    # every 17 s): the same binary yielded a 123-unique-color capture and a
    # 2-unique-color blank ten minutes apart.  Retry the bounded capture
    # instead of recording a blank boundary; the pixel audit is never relaxed.
    # Measured behaviour: blank epochs last minutes, and a run either captures
    # on its first attempt or stays blank for the whole budget, so the in-run
    # budget is deliberately short and the caller reruns the whole case.  The
    # attempt count is recorded in the manifest either way.
    $captureAttempts = 0
    $Capture = $null
    $captureFailure = ""
    while ($null -eq $Capture) {
        if ($Process.HasExited) {
            throw "Live replay exited before a non-blank client capture"
        }
        if ($captureAttempts -ge 12 -or [DateTime]::UtcNow -ge $Deadline) {
            throw ("Client capture stayed blank after " + $captureAttempts +
                   " attempts: " + $captureFailure)
        }
        ++$captureAttempts
        try {
            $Capture = Capture-Client $Process $PngPath
        }
        catch {
            $captureFailure = $_.Exception.Message
            Start-Sleep -Milliseconds 1500
        }
    }
    if ([int]$Capture.width -ne $Width -or [int]$Capture.height -ne $Height) {
        throw ("Captured client extent differs: " + $Capture.width + "x" +
               $Capture.height + ", expected " + $Width + "x" + $Height)
    }
    $ContinuePath = Join-Path $SyncRoot "$ExpectedMarker.continue"
    [IO.File]::WriteAllBytes($ContinuePath, [byte[]]@())
    if (-not $Process.WaitForExit($TimeoutSeconds * 1000)) {
        $Process.Kill()
        throw "Live replay did not exit after continue"
    }
}
finally {
    if (-not $Process.HasExited) {
        try { $Process.Kill() } catch { }
    }
    $StdoutTask.Wait()
    $StderrTask.Wait()
    [IO.File]::WriteAllText(
        $StdoutPath, $StdoutTask.Result, [Text.UTF8Encoding]::new($false))
    [IO.File]::WriteAllText(
        $StderrPath, $StderrTask.Result, [Text.UTF8Encoding]::new($false))
}

if ($Process.ExitCode -ne 0) {
    throw "editor_ui_live_replay exited with $($Process.ExitCode)"
}
if ((Get-Hash $Executable) -ne $ExecutableHash) {
    throw "Live replay executable changed before process completion"
}
$FinalResult = Get-Content -LiteralPath (
    Join-Path $AppEvidence "result.json") -Raw | ConvertFrom-Json
if (-not [bool]$FinalResult.complete -or
    -not [bool]$FinalResult.result.passed -or
    [string]$FinalResult.result.status -ne "passed") {
    throw "Final live replay result is incomplete"
}
$ExpectedStepCount = @($FinalResult.result.steps).Count
$StepFiles = @(Get-ChildItem -LiteralPath (Join-Path $AppEvidence "steps") -Filter "*.json" -File |
    Sort-Object { [int](($_.BaseName -split "-", 2)[0]) })
if ($StepFiles.Count -ne $ExpectedStepCount) {
    throw "Live step artifact count differs from the result"
}
$StepRecords = @(
    for ($index = 0; $index -lt $StepFiles.Count; ++$index) {
        $stepFile = $StepFiles[$index]
        $step = Get-Content -LiteralPath $stepFile.FullName -Raw | ConvertFrom-Json
        if ([string]$step.kind -ne "mgif.editor-ui-live-step" -or
            [int64]$step.actionResult.sequence -ne $index -or
            [string]$step.identity.executable_sha256 -ne $ExecutableHash -or
            [string]$step.identity.source_sha256 -ne $SourceIdentityHash -or
            [string]$step.dispatchMode -ne "direct-typed-command" -or
            [bool]$step.osClick -or [bool]$step.nativeDialog) {
            throw "Live step artifact sequence/identity is invalid at $index"
        }
        [ordered]@{
            sequence = $index
            actionId = [string]$step.actionResult.actionId
            path = Get-EvidenceRelativePath $stepFile.FullName
            bytes = $stepFile.Length
            sha256 = Get-Hash $stepFile.FullName
        }
    }
)
$CombinedLog = (Get-Content -LiteralPath $StdoutPath -Raw) +
               [Environment]::NewLine +
               (Get-Content -LiteralPath $StderrPath -Raw)
if ($CombinedLog -match
    "(?im)(VUID-|SYNC-HAZARD|Validation Error|device lost|marker=failed|Assertion failed|Traceback)") {
    throw "Live replay emitted runtime validation diagnostics"
}
foreach ($required in @(
        "[EDITOR_LIVE_REPLAY] marker=scene-ready",
        "[EDITOR_LIVE_REPLAY] marker=script-ready",
        "[EDITOR_LIVE_REPLAY] marker=complete")) {
    if ([regex]::Matches(
            $CombinedLog, [regex]::Escape($required)).Count -ne 1) {
        throw "Live replay log marker cardinality is invalid: $required"
    }
}

$Manifest = [ordered]@{
    documentKind = "mgif.editor-ui-live-capture"
    schemaVersion = 1
    case = $Case
    configuration = $Configuration
    status = "PASS_BOUNDED_LIVE_PROOF"
    dispatchMode = "direct-typed-command"
    osClickClaim = $false
    nativeDialogClaim = $false
    screenshotClaim = "real-client-png-at-boundary"
    uiFocus = [ordered]@{
        method = "acceptance-imgui-window-focus"
        sceneBrowserTabVisible =
            [bool]$State.uiFocus.scene_browser_tab_visible
    }
    requestedClientExtent = [ordered]@{ width = $Width; height = $Height }
    windowDecoration = $WindowDecoration
    virtualScreen = [ordered]@{ width = $virtualWidth; height = $virtualHeight }
    primaryMonitor = [ordered]@{ width = $primaryWidth; height = $primaryHeight }
    requiredOuterExtent = [ordered]@{ width = $requiredOuterWidth; height = $requiredOuterHeight }
    requestedUiDpi = $DpiScale
    uiDpi = [ordered]@{
        requested = [double]$State.uiDpi.requested
        applied = [double]$State.uiDpi.applied
        nativeContentScale = [double]$State.uiDpi.nativeContentScale
        source = [string]$State.uiDpi.source
    }
    identity = [ordered]@{
        executable = [ordered]@{
            path = $Executable
            bytes = (Get-Item -LiteralPath $Executable).Length
            sha256 = $ExecutableHash
        }
        source = [ordered]@{
            path = "source-identity.json"
            bytes = (Get-Item -LiteralPath $SourceIdentityPath).Length
            sha256 = $SourceIdentityHash
            artifactSha256 = Get-Hash $SourceIdentityPath
        }
        configuredBuild = [ordered]@{
            cachePath = $CachePath
            cacheSha256 = $CacheHash
            buildType = $ConfiguredBuildType
            buildTesting = $ConfiguredTesting
            skipEditorDependencyPreflight = $ConfiguredPreflightSkip
        }
    }
    handshake = [ordered]@{
        protocol = [string]$Ready.protocol
        marker = [string]$Ready.marker
        phase = [string]$Ready.phase
        frame = [int64]$Ready.frame
        renderedFrame = [int64]$Ready.rendered_frame
        readyPath = Get-EvidenceRelativePath $ReadyPath
    }
    authority = [ordered]@{
        documentDigest = [string]$Ready.live.document_digest
        projectionDigest = [string]$Ready.live.projection_digest
        runtimeContentDigest = [string]$State.renderer.runtime_content_digest
        actionCount = @($FinalResult.result.steps).Count
        resultStatus = [string]$FinalResult.result.status
    }
    validation = [ordered]@{
        layers = @("VK_LAYER_KHRONOS_validation")
        synchronization = $true
        messageFilter = $null
    }
    screenshot = [ordered]@{
        path = "client.png"
        bytes = (Get-Item -LiteralPath $PngPath).Length
        sha256 = Get-Hash $PngPath
        width = $Capture.width
        height = $Capture.height
        transport = $Capture.transport
        transportOrder = ($CaptureOrder -join ",")
        captureAttempts = $captureAttempts
        clientOrigin = [ordered]@{ x = $Capture.clientOriginX; y = $Capture.clientOriginY }
        pinnedToPrimary = [bool]$Capture.pinnedToPrimary
        topmostVerified = [bool]$Capture.topmostVerified
        foregroundMatches = [bool]$Capture.foregroundMatches
        audit = $Capture.audit
    }
    artifacts = [ordered]@{
        state = [ordered]@{
            path = "app/state.json"
            bytes = (Get-Item -LiteralPath (Join-Path $AppEvidence "state.json")).Length
            sha256 = Get-Hash (Join-Path $AppEvidence "state.json")
        }
        result = [ordered]@{
            path = "app/result.json"
            bytes = (Get-Item -LiteralPath (Join-Path $AppEvidence "result.json")).Length
            sha256 = Get-Hash (Join-Path $AppEvidence "result.json")
        }
        script = [ordered]@{
            path = "app/script.json"
            bytes = (Get-Item -LiteralPath (Join-Path $AppEvidence "script.json")).Length
            sha256 = Get-Hash (Join-Path $AppEvidence "script.json")
        }
        stdout = [ordered]@{
            path = "stdout.log"
            bytes = (Get-Item -LiteralPath $StdoutPath).Length
            sha256 = Get-Hash $StdoutPath
        }
        stderr = [ordered]@{
            path = "stderr.log"
            bytes = (Get-Item -LiteralPath $StderrPath).Length
            sha256 = Get-Hash $StderrPath
        }
        steps = $StepRecords
    }
    limitations = @(
        "Typed commands were dispatched directly at application frame boundaries; no OS click is claimed.",
        "No native file dialog was invoked or claimed.",
        "Typed controller traces are direct application-boundary evidence; OS click/dialog behavior and full visual coverage remain separate claims."
    )
}
Write-Json $ManifestPath $Manifest
Write-Output "Editor UI live replay passed: $EvidenceRoot"
