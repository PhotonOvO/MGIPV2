[CmdletBinding()]
param(
    [Parameter(Mandatory = $true)][string]$BuildRoot,
    [Parameter(Mandatory = $true)][string]$EvidenceRoot,
    [ValidateSet("V01-grid", "V02-gizmo-components", "V03-parent-space", "V04-depth-xray", "V07-dpi-resize", "all")]
    [string]$Case = "all",
    [string]$Variant = "",
    [ValidateRange(-1, 12)][int]$Component = -1,
    [ValidateSet("Release", "Debug")][string]$Configuration = "Release",
    [ValidateRange(640, 7680)][int]$Width = 1920,
    [ValidateRange(480, 4320)][int]$Height = 1080,
    [ValidateRange(1, 4)][double]$DpiScale = 1,
    [ValidateRange(10, 900)][int]$TimeoutSeconds = 180,
    [switch]$Undecorated
)

Set-StrictMode -Version Latest
$ErrorActionPreference = "Stop"

$BuildRoot = [IO.Path]::GetFullPath($BuildRoot)
$EvidenceRoot = [IO.Path]::GetFullPath($EvidenceRoot)
$RepoRoot = [IO.Path]::GetFullPath((Join-Path $PSScriptRoot ".."))
$Executable = Join-Path $BuildRoot "Demo.exe"
if (-not (Test-Path -LiteralPath $Executable -PathType Leaf)) {
    $candidate = Join-Path $BuildRoot "bin\$Configuration\Demo.exe"
    if (Test-Path -LiteralPath $candidate -PathType Leaf) { $Executable = $candidate }
}
if (-not (Test-Path -LiteralPath $Executable -PathType Leaf)) {
    throw "Demo executable is missing under $BuildRoot"
}
if (Test-Path -LiteralPath $EvidenceRoot) {
    throw "Evidence root must be fresh: $EvidenceRoot"
}
New-Item -ItemType Directory -Path $EvidenceRoot -Force | Out-Null

Add-Type -AssemblyName System.Drawing
Add-Type @"
using System;
using System.Runtime.InteropServices;
public static class MGIFEditorVisualNative {
    [StructLayout(LayoutKind.Sequential)] public struct RECT {
        public int Left; public int Top; public int Right; public int Bottom;
    }
    [StructLayout(LayoutKind.Sequential)] public struct POINT {
        public int X; public int Y;
    }
    [DllImport("user32.dll")] public static extern bool GetClientRect(IntPtr hWnd, out RECT rect);
    [DllImport("user32.dll")] public static extern bool GetWindowRect(IntPtr hWnd, out RECT rect);
    [DllImport("user32.dll")] public static extern bool ClientToScreen(IntPtr hWnd, ref POINT point);
    [DllImport("user32.dll")] public static extern bool SetForegroundWindow(IntPtr hWnd);
    [DllImport("user32.dll")] public static extern bool BringWindowToTop(IntPtr hWnd);
    [DllImport("user32.dll")] public static extern bool ShowWindow(IntPtr hWnd, int command);
    [DllImport("user32.dll")] public static extern IntPtr GetForegroundWindow();
    [DllImport("user32.dll")] public static extern bool SetCursorPos(int x, int y);
    [DllImport("user32.dll")] public static extern bool GetCursorPos(out POINT point);
    [DllImport("user32.dll")] public static extern bool ClipCursor(ref RECT rect);
    [DllImport("user32.dll", EntryPoint = "ClipCursor")] public static extern bool ReleaseCursor(IntPtr rect);
    [DllImport("user32.dll")] public static extern short GetAsyncKeyState(int key);
    [DllImport("user32.dll")] public static extern bool PostMessage(IntPtr hWnd, uint message, UIntPtr wParam, IntPtr lParam);
    [DllImport("user32.dll")] public static extern IntPtr GetDC(IntPtr hWnd);
    [DllImport("user32.dll")] public static extern int ReleaseDC(IntPtr hWnd, IntPtr hdc);
    [DllImport("user32.dll")] public static extern bool PrintWindow(IntPtr hWnd, IntPtr hdc, uint flags);
    [DllImport("user32.dll")] public static extern bool AdjustWindowRectEx(ref RECT rect, uint style, bool menu, uint exStyle);
    [DllImport("user32.dll")] public static extern int GetSystemMetrics(int index);
    [DllImport("dwmapi.dll")] public static extern int DwmFlush();
    [DllImport("gdi32.dll")] public static extern bool BitBlt(IntPtr destination, int x, int y, int width, int height, IntPtr source, int sourceX, int sourceY, uint operation);
    [DllImport("user32.dll")] public static extern void mouse_event(uint flags, uint dx, uint dy, uint data, UIntPtr extraInfo);
    [DllImport("user32.dll")] public static extern bool SetWindowPos(
        IntPtr hWnd, IntPtr insertAfter, int x, int y, int cx, int cy, uint flags);
}
"@

$SourceRelativePaths = @(
    "app/MinimalLatestApp.h",
    "app/MinimalLatestApp.cpp",
    "app/editor/AutomationAssetIdentity.h",
    "app/editor/EditorController.h",
    "app/editor/EditorController.cpp",
    "app/editor/EditorInputRouter.h",
    "app/editor/EditorInputRouter.cpp",
    "app/editor/EditorRendererAdapter.h",
    "app/editor/EditorRendererAdapter.cpp",
    "app/editor/EditorViewportRuntime.h",
    "app/editor/EditorViewportRuntime.cpp",
    "app/editor/ImGuiClipboardAdapter.h",
    "app/editor/ImGuiClipboardAdapter.cpp",
    "app/editor/MaterialClipboardCodec.h",
    "app/editor/MaterialClipboardCodec.cpp",
    "app/editor/SceneEditorUi.h",
    "app/editor/SceneEditorUi.cpp",
    "app/editor/SceneEditorUiModel.h",
    "app/editor/SceneSelection.h",
    "app/editor/SceneSelection.cpp",
    "app/editor/EditorTransactionSmoke.h",
    "app/editor/EditorTransactionPath.h",
    "app/editor/EditorTransactionPath.cpp",
    "tools/editor_visual_capture.ps1",
    "tests/editor/editor_visual_capture_contract_tests.py",
    "tests/editor/editor_overlay_rhi_tests.cpp",
    "tests/editor/editor_renderer_integration_tests.cpp",
    "tests/editor/gpu_driven_present_layout_contract_tests.py",
    "render/editor/EditorGridMath.cpp",
    "render/editor/EditorOverlay.cpp",
    "render/editor/EditorOverlayShaderInterop.h",
    "render/editor/EditorOverlayTypes.h",
    "render/editor/EditorViewportMetrics.h",
    "render/editor/GizmoGeometry.h",
    "render/editor/GizmoGeometry.cpp",
    "render/editor/GridModel.h",
    "render/editor/GridModel.cpp",
    "render/editor/ScenePicking.h",
    "render/editor/ScenePicking.cpp",
    "render/editor/SceneRuntimePublisher.h",
    "render/editor/SceneRuntimePublisher.cpp",
    "render/editor/TransformGizmo.h",
    "render/editor/TransformGizmo.cpp",
    "render/editor/TransformGizmoMath.cpp",
    "render/editor/include/mgif/editor/EditorGridMath.h",
    "render/editor/include/mgif/editor/EditorOverlay.h",
    "render/editor/include/mgif/editor/TransformGizmoMath.h",
    "render/passes/EditorOverlayPass.cpp",
    "render/passes/EditorOverlayPass.h",
    "render/RendererFacade.h",
    "render/RendererFacade.cpp",
    "render/GPUDrivenRenderer.h",
    "render/GPUDrivenRenderer.cpp",
    "render/RenderDevice.h",
    "render/RenderDevice.cpp",
    "render/PassExecutor.h",
    "render/PassExecutor.cpp",
    "rhi/vulkan/VulkanSwapchain.cpp",
    "rhi/vulkan/VulkanSwapchain.h",
    "rhi/vulkan/VulkanQueue.cpp",
    "rhi/vulkan/VulkanQueue.h",
    "scene/editor/include/mgif/scene_editor/EditorInputRouter.h",
    "scene/editor/EditorInputRouter.cpp",
    "scene/editor/include/mgif/scene_editor/EditorSelection.h",
    "scene/editor/include/mgif/scene_editor/EditorSelectionTypes.h",
    "scene/editor/EditorSelection.cpp",
    "scene/editor/include/mgif/scene_editor/EditorSession.h",
    "scene/editor/EditorSession.cpp",
    "scene/editor/include/mgif/scene_editor/SceneEditor.h",
    "scene/editor/SceneEditor.cpp",
    "scene/editor/include/mgif/scene_editor/GltfDocument.h",
    "scene/editor/GltfDocument.cpp",
    "scene/editor/include/mgif/scene_editor/SceneCommands.h",
    "scene/editor/SceneCommands.cpp",
    "scene/editor/include/mgif/scene_editor/commands/HierarchyCommands.h",
    "scene/editor/commands/HierarchyCommands.cpp",
    "scene/editor/include/mgif/scene_editor/SceneProjection.h",
    "scene/editor/SceneProjection.cpp",
    "shaders/editor_overlay.slang",
    "tests/fixtures/gltf_editor/minimal_scene.gltf",
    "tests/fixtures/gltf_editor/r09_shared_mesh.gltf"
)

function Get-BytesSha256([byte[]]$Bytes) {
    $algorithm = [Security.Cryptography.SHA256]::Create()
    try {
        return ([BitConverter]::ToString($algorithm.ComputeHash($Bytes))).Replace("-", "").ToLowerInvariant()
    }
    finally { $algorithm.Dispose() }
}

function Get-Hash([string]$Path) {
    return (Get-FileHash -LiteralPath $Path -Algorithm SHA256).Hash.ToLowerInvariant()
}

function Write-Json([string]$Path, [object]$Value) {
    $json = $Value | ConvertTo-Json -Depth 20
    [IO.File]::WriteAllText($Path, $json + [Environment]::NewLine,
        [Text.UTF8Encoding]::new($false))
}

function Quote-Argument([string]$Value) {
    return '"' + $Value.Replace('"', '\"') + '"'
}

$ExecutableHash = Get-Hash $Executable

# The pinned resolution axis includes 2560x1440.  A decorated window cannot
# present a client area that tall on this host: the default WM_GETMINMAXINFO
# maximum track size is the virtual screen and the non-client frame consumes
# part of it (measured reachable maximum 2560x1421).  Compute the required
# outer size from the real frame metrics and fail before launching instead of
# recording a clamped extent.  An undecorated window has no non-client frame,
# so its client area equals its outer size.
$virtualWidth = [MGIFEditorVisualNative]::GetSystemMetrics(78)
$virtualHeight = [MGIFEditorVisualNative]::GetSystemMetrics(79)
$probeRect = New-Object MGIFEditorVisualNative+RECT
$probeRect.Left = 0
$probeRect.Top = 0
$probeRect.Right = [Math]::Max(640, $Width)
$probeRect.Bottom = [Math]::Max(480, $Height)
# WS_CAPTION | WS_THICKFRAME | WS_MINIMIZEBOX | WS_MAXIMIZEBOX | WS_SYSMENU
$probeStyle = [uint32]0x00CF0000
if ($Undecorated) { $probeStyle = [uint32]0 }
if (-not [MGIFEditorVisualNative]::AdjustWindowRectEx([ref]$probeRect, $probeStyle, $false, [uint32]0)) {
    throw "AdjustWindowRectEx failed while probing the requested client extent"
}
$requiredOuterWidth = $probeRect.Right - $probeRect.Left
$requiredOuterHeight = $probeRect.Bottom - $probeRect.Top
if ($Undecorated) { $WindowDecoration = "undecorated" } else { $WindowDecoration = "decorated" }
if ($requiredOuterWidth -gt $virtualWidth -or $requiredOuterHeight -gt $virtualHeight) {
    throw "Requested client ${Width}x${Height} needs a ${requiredOuterWidth}x${requiredOuterHeight} $WindowDecoration outer window, which exceeds the ${virtualWidth}x${virtualHeight} virtual screen. Pass -Undecorated to drop the non-client frame, or request a smaller client extent."
}
$canonicalSourcePaths = [string[]]$SourceRelativePaths.Clone()
[Array]::Sort($canonicalSourcePaths, [StringComparer]::Ordinal)
$SourceRecords = @(
    foreach ($relative in $canonicalSourcePaths) {
        $path = Join-Path $RepoRoot ($relative.Replace('/', '\\'))
        if (-not (Test-Path -LiteralPath $path -PathType Leaf)) {
            throw "Visual authority source is missing: $relative"
        }
        $item = Get-Item -LiteralPath $path
        [ordered]@{
            path = $relative
            bytes = [int64]$item.Length
            sha256 = Get-Hash $path
        }
    }
)
$sourceIdentityText = (($SourceRecords | ForEach-Object { "$($_.path)=$($_.sha256)`n" }) -join '')
$SourceIdentityHash = Get-BytesSha256 ([Text.UTF8Encoding]::new($false).GetBytes($sourceIdentityText))

function Read-VisualState([string]$Path) {
    if (-not (Test-Path -LiteralPath $Path -PathType Leaf)) { throw "Visual state is missing: $Path" }
    return Get-Content -LiteralPath $Path -Raw | ConvertFrom-Json
}

function Assert-ComponentIndexTable($Components, [string]$Label) {
    if (@($Components).Count -ne 13) { throw "$Label does not contain 13 Gizmo components" }
    $indices = @($Components | ForEach-Object { [int]$_.index })
    if ((@($indices | Sort-Object -Unique) -join ',') -ne '0,1,2,3,4,5,6,7,8,9,10,11,12') {
        throw "$Label component index table is not the unique ordered 0..12 set"
    }
}

function Get-ComponentBoundsAudit($State, [Drawing.Bitmap]$Bitmap) {
    $components = @($State.overlay.gizmo_components)
    Assert-ComponentIndexTable $components "Gizmo state"
    # Desktop screenshots contain window framebuffer pixels. V07 may exercise a
    # larger synthetic offscreen framebuffer, which must not be used to crop a
    # CopyFromScreen bitmap.
    $scaleX = [double]$State.viewport.observed_window_framebuffer_scale[0]
    $scaleY = [double]$State.viewport.observed_window_framebuffer_scale[1]
    if ($scaleX -le 0 -or $scaleY -le 0) { throw "Observed window framebuffer scale is invalid" }
    $rows = @()
    foreach ($component in $components) {
        $bounds = $component.screen_bounds
        $visible = [bool]$component.visible
        if ($visible -and (-not [bool]$bounds.valid -or [double]$bounds.width -le 0 -or [double]$bounds.height -le 0)) {
            throw "Visible component $($component.index) has invalid screen bounds"
        }
        $x0 = 0; $y0 = 0; $x1 = -1; $y1 = -1
        $colorful = 0; $nonBlack = 0
        if ($visible -and [bool]$bounds.valid) {
            # Vulkan's positive-height viewport maps NDC Y directly into the
            # top-left framebuffer coordinate used by CopyFromScreen. The
            # separate ray-input path still performs its required NDC flip.
            $mappedY = [double]$bounds.y
            $x0 = [Math]::Max(0, [int]([Math]::Floor([double]$bounds.x * $scaleX)))
            $y0 = [Math]::Max(0, [int]([Math]::Floor($mappedY * $scaleY)))
            $x1 = [Math]::Min($Bitmap.Width - 1, [int]([Math]::Ceiling(([double]$bounds.x + [double]$bounds.width) * $scaleX)))
            $y1 = [Math]::Min($Bitmap.Height - 1, [int]([Math]::Ceiling(($mappedY + [double]$bounds.height) * $scaleY)))
            if ($x1 -ge $x0 -and $y1 -ge $y0) {
                for ($y = $y0; $y -le $y1; ++$y) {
                    for ($x = $x0; $x -le $x1; ++$x) {
                        $pixel = $Bitmap.GetPixel($x, $y)
                        $maximum = [Math]::Max($pixel.R, [Math]::Max($pixel.G, $pixel.B))
                        $minimum = [Math]::Min($pixel.R, [Math]::Min($pixel.G, $pixel.B))
                        if ($maximum -gt 20) { ++$nonBlack }
                        if (($maximum - $minimum) -gt 24 -and $maximum -gt 48) { ++$colorful }
                    }
                }
            }
        }
        $rows += [ordered]@{
            index = [int]$component.index
            visible = $visible
            state = [int]$component.state
            bounds = [ordered]@{ valid = [bool]$bounds.valid; x = [double]$bounds.x; y = [double]$bounds.y; width = [double]$bounds.width; height = [double]$bounds.height; projectionY = "vulkan-positive-height-top-left" }
            pixelRegion = [ordered]@{ x = $x0; y = $y0; width = [Math]::Max(0, $x1 - $x0 + 1); height = [Math]::Max(0, $y1 - $y0 + 1); nonBlack = $nonBlack; colorful = $colorful; projectionY = "top-left-window-framebuffer" }
        }
    }
    $overlaps = @()
    $visibleRows = @($rows | Where-Object { $_.visible })
    for ($left = 0; $left -lt $visibleRows.Count; ++$left) {
        for ($right = $left + 1; $right -lt $visibleRows.Count; ++$right) {
            $a = $visibleRows[$left].bounds; $b = $visibleRows[$right].bounds
            $ix = [Math]::Max(0.0, [Math]::Min($a.x + $a.width, $b.x + $b.width) - [Math]::Max($a.x, $b.x))
            $iy = [Math]::Max(0.0, [Math]::Min($a.y + $a.height, $b.y + $b.height) - [Math]::Max($a.y, $b.y))
            if ($ix -gt 0 -and $iy -gt 0) {
                $overlaps += [ordered]@{ left = $visibleRows[$left].index; right = $visibleRows[$right].index; area = $ix * $iy }
            }
        }
    }
    return [ordered]@{ componentCount = $rows.Count; visibleCount = $visibleRows.Count; components = $rows; overlapPairs = $overlaps }
}

function Assert-TargetPixels($Audit, [int]$TargetComponent, [string]$Label) {
    if ($TargetComponent -lt 0) { return }
    $row = @($Audit.components | Where-Object { [int]$_.index -eq $TargetComponent })
    if ($row.Count -ne 1 -or -not [bool]$row[0].visible -or
        [int]$row[0].pixelRegion.nonBlack -le 0 -or
        [int]$row[0].pixelRegion.colorful -le 0) {
        throw "$Label has no nonblack pixels for target component $TargetComponent"
    }
}

function Pin-VisualWindow([IntPtr]$Handle) {
    if ($Handle -eq [IntPtr]::Zero) { throw "Cannot pin a null visual window" }
    $window = New-Object MGIFEditorVisualNative+RECT
    if (-not [MGIFEditorVisualNative]::GetWindowRect($Handle, [ref]$window)) {
        throw "GetWindowRect failed while pinning the visual window"
    }
    $outerWidth = $window.Right - $window.Left
    $outerHeight = $window.Bottom - $window.Top
    if ($outerWidth -lt 1 -or $outerHeight -lt 1) { throw "Visual window outer extent is invalid" }
    $clientOrigin = New-Object MGIFEditorVisualNative+POINT
    if (-not [MGIFEditorVisualNative]::ClientToScreen($Handle, [ref]$clientOrigin)) {
        throw "ClientToScreen failed while pinning the visual window"
    }
    # Keep the established 960x540/720 baseline placement unchanged. Larger
    # clients need the non-client frame moved off-screen so their full client
    # surface remains printable instead of returning the compositor's white fill.
    $frameX = $clientOrigin.X - $window.Left
    $frameY = $clientOrigin.Y - $window.Top
    $targetX = 0
    $targetY = 0
    if ($Width -gt 1280 -or $Height -gt 720) {
        $targetX = -$frameX
        $targetY = -$frameY
    }
    if (-not [MGIFEditorVisualNative]::SetWindowPos(
            $Handle, [IntPtr](-1), $targetX, $targetY, $outerWidth, $outerHeight, 0x0040)) {
        throw "SetWindowPos failed while pinning the visual window"
    }
}

function Capture-VisualStage([Diagnostics.Process]$Process, [string]$Path, $State, [int]$TargetComponent, [string]$Label) {
    $lastError = $null
    for ($attempt = 0; $attempt -lt 5; ++$attempt) {
        try {
            $capture = Capture-ClientVerified $Process $Path
            $audit = Get-PngBoundsAudit $Path $State
            Assert-TargetPixels $audit $TargetComponent $Label
            return [ordered]@{ capture = $capture; audit = $audit }
        }
        catch {
            $lastError = $_
            if ($attempt -lt 4) {
                try { [void](Resize-Client $Process) } catch { }
                Start-Sleep -Milliseconds 200
            }
        }
    }
    throw $lastError
}

function Assert-VisualState($State, [string]$Name, [string]$Variant, [int]$TargetComponent, [string]$Boundary, [int]$RequiredState = -1) {
    if ([string]$State.schema -ne "mgif.editor-visual-state.v2") { throw "State schema is invalid for $Name/$Variant" }
    if ([string]$State.case -ne $Name -or [string]$State.variant -ne $Variant) { throw "State case/variant differs for $Name/$Variant" }
    if (($TargetComponent -ge 0 -and [int]$State.target_component -ne $TargetComponent) -or
        ($TargetComponent -lt 0 -and $null -ne $State.target_component)) {
        throw "State target component differs for $Name/$Variant"
    }
    if ([string]$State.boundary -ne $Boundary) { throw "State boundary differs: expected $Boundary, got $($State.boundary)" }
    if ([Math]::Abs([double]$State.requested_dpi - $DpiScale) -gt 0.001) { throw "State requested DPI differs" }
    if ([string]$State.identity.executable_sha256 -ne $ExecutableHash -or [string]$State.identity.source_sha256 -ne $SourceIdentityHash) {
        throw "State identity does not bind the launched executable/source"
    }
    $documentState = $State.document
    foreach ($digestName in @("document_digest", "projection_digest", "runtime_content_digest")) {
        $digestValue = [string]$documentState.$digestName
        if ($digestValue -notmatch "^[0-9a-f]{64}$") {
            throw "Document digest $digestName is missing or malformed for $Name/$Variant"
        }
    }
    $uiState = $State.ui
    if ($null -eq $uiState -or -not [bool]$uiState.has_document -or
        -not [bool]$uiState.scene_browser_visible -or
        -not [bool]$uiState.inspector_visible -or
        [int]$uiState.command_count -le 0 -or
        @($uiState.commands).Count -ne [int]$uiState.command_count -or
        $null -eq $uiState.scenes -or $null -eq $uiState.graph_rows -or
        $null -eq $uiState.inspector) {
        throw "UI semantic state is incomplete for $Name/$Variant"
    }
    $actionLog = @($uiState.action_log)
    if ($actionLog.Count -ne 1 -or [string]$actionLog[0].marker -ne $Boundary -or
        [string]$actionLog[0].source -ne "visual-boundary" -or
        [int64]$actionLog[0].frame -ne [int64]$State.frame) {
        throw "UI action log does not bind the visual boundary for $Name/$Variant"
    }
    if (-not [bool]$State.overlay.present) { throw "Overlay state is absent for $Name/$Variant" }
    $rhiOverlay = $State.rhi_overlay
    if ($null -eq $rhiOverlay -or -not [bool]$rhiOverlay.available -or
        [int64]$rhiOverlay.prepare_calls -le 0 -or [int64]$rhiOverlay.recorded_frames -le 0 -or
        [int64]$rhiOverlay.submitted_frames -le 0 -or [int64]$rhiOverlay.rejected_frames -ne 0 -or
        [int64]$rhiOverlay.failures_before_submit -ne 0 -or [int64]$rhiOverlay.failures_after_submit -ne 0 -or
        [int64]$rhiOverlay.native_escape_calls -ne 0 -or [int]$rhiOverlay.last_error -ne 0) {
        throw "RHI overlay diagnostics are not clean for $Name/$Variant"
    }
    $logical = @($State.viewport.logical_extent)
    $physical = @($State.viewport.physical_extent)
    $depthPhysical = @($State.viewport.depth_physical_extent)
    $colorAttachment = @($State.viewport.color_attachment_extent)
    $depthAttachment = @($State.viewport.depth_attachment_extent)
    $framebufferScale = @($State.viewport.framebuffer_scale)
    $observedScale = @($State.viewport.observed_window_framebuffer_scale)
    if ($logical.Count -ne 2 -or $physical.Count -ne 2 -or $framebufferScale.Count -ne 2 -or
        [double]$logical[0] -le 0 -or [double]$logical[1] -le 0 -or
        [double]$framebufferScale[0] -le 0 -or [double]$framebufferScale[1] -le 0 -or
        [double]$observedScale[0] -le 0 -or [double]$observedScale[1] -le 0) {
        throw "Viewport extents/scales are invalid for $Name/$Variant"
    }
    $expectedPhysicalX = [int][Math]::Ceiling([double]$logical[0] * [double]$framebufferScale[0])
    $expectedPhysicalY = [int][Math]::Ceiling([double]$logical[1] * [double]$framebufferScale[1])
    if ([int]$physical[0] -ne $expectedPhysicalX -or [int]$physical[1] -ne $expectedPhysicalY -or
        [int]$depthPhysical[0] -ne [int]$physical[0] -or [int]$depthPhysical[1] -ne [int]$physical[1] -or
        [int]$colorAttachment[0] -ne [int]$physical[0] -or [int]$colorAttachment[1] -ne [int]$physical[1] -or
        [int]$depthAttachment[0] -ne [int]$physical[0] -or [int]$depthAttachment[1] -ne [int]$physical[1] -or
        [int]$State.viewport.surface_transform_token -ne 0) {
        throw "Viewport/depth/attachment mapping is incoherent for $Name/$Variant"
    }
    if (-not [bool]$State.viewport.mapping_probe.physical_valid) { throw "Viewport mapping probe is invalid" }
    $mapped = @($State.viewport.mapping_probe.local)
    if ($mapped.Count -ne 2 -or [int]$mapped[0] -lt 0 -or [int]$mapped[1] -lt 0 -or
        [int]$mapped[0] -ge [int]$physical[0] -or [int]$mapped[1] -ge [int]$physical[1]) {
        throw "Viewport mapping probe is outside the physical viewport"
    }
    $mappingLogical = @($State.viewport.mapping_probe.logical)
    $logicalOrigin = @($State.viewport.logical_origin)
    $expectedMappedX = [int][Math]::Floor((([double]$mappingLogical[0] - [double]$logicalOrigin[0]) / [double]$logical[0]) * [double]$physical[0])
    $expectedMappedY = [int][Math]::Floor((([double]$mappingLogical[1] - [double]$logicalOrigin[1]) / [double]$logical[1]) * [double]$physical[1])
    if ([int]$mapped[0] -ne $expectedMappedX -or [int]$mapped[1] -ne $expectedMappedY) {
        throw "Viewport mapping probe does not use the physical attachment extent"
    }
    $expectedScaleSource = if ([Math]::Abs([double]$observedScale[0] - $DpiScale) -gt 0.01 -or
        [Math]::Abs([double]$observedScale[1] - $DpiScale) -gt 0.01) { "synthetic-offscreen" } else { "native-window" }
    if ([string]$State.viewport.framebuffer_scale_source -ne $expectedScaleSource) { throw "Framebuffer scale source differs" }
    if ([Math]::Abs([double]$framebufferScale[0] - $DpiScale) -gt 0.01 -or
        [Math]::Abs([double]$framebufferScale[1] - $DpiScale) -gt 0.01) { throw "Renderer framebuffer scale differs from requested DPI" }

    $components = @($State.overlay.gizmo_components)
    if ($Name -eq "V01-grid") {
        if (-not [bool]$State.overlay.grid_visible -or [bool]$State.overlay.gizmo_visible) { throw "V01 grid/Gizmo visibility differs" }
        if ([int64]$rhiOverlay.grid_draws -le 0 -or [int64]$rhiOverlay.axes_draws -le 0) { throw "V01 RHI grid/axes draws are absent" }
        $expectedProjection = if ($Variant.StartsWith("orthographic-")) { "orthographic" } else { "perspective" }
        $expectedUnit = if ($Variant.EndsWith("-0.001")) { 0.001 } elseif ($Variant.EndsWith("-1000")) { 1000.0 } else { 1.0 }
        if ([string]$State.viewport.projection_kind -ne $expectedProjection -or
            [Math]::Abs([double]$State.overlay.grid.base_unit - $expectedUnit) -gt [Math]::Max(1.0e-9, $expectedUnit * 1.0e-9) -or
            @($State.overlay.grid.decade_steps).Count -ne 4 -or @($State.overlay.grid.active_lods).Count -ne 3 -or
            [int]$State.overlay.grid_line_count -le 0 -or [int]$State.overlay.grid_axis_count -ne 2) {
            throw "V01 grid projection/unit/LOD contract differs for $Variant"
        }
        return $State
    }

    if (-not [bool]$State.overlay.gizmo_visible) { throw "$Name did not expose the Gizmo component table" }
    Assert-ComponentIndexTable $components "$Name/$Variant"
    if ([int64]$rhiOverlay.gizmo_draws -le 0) { throw "$Name has no recorded RHI Gizmo draws" }
    $expectedVisible = if ($TargetComponent -ge 0 -and $TargetComponent -le 5) { @(0,1,2,3,4,5) }
        elseif ($TargetComponent -ge 6 -and $TargetComponent -le 8) { @(6,7,8) }
        elseif ($TargetComponent -ge 9) { @(9,10,11,12) }
        else { @(0,1,2,3,4,5,6,7,8,9,10,11,12) }
    foreach ($component in $components) {
        $shouldBeVisible = $expectedVisible -contains [int]$component.index
        if ([bool]$component.visible -ne $shouldBeVisible) { throw "$Name component $($component.index) visibility differs" }
        if ($shouldBeVisible -and ([int]$component.vertices -le 0 -or [int]$component.indices -le 0)) { throw "$Name component $($component.index) is empty" }
    }
    if ($RequiredState -ge 0) {
        if ($TargetComponent -ge 0) {
            $targetRows = @($components | Where-Object { [int]$_.index -eq $TargetComponent })
            if ($targetRows.Count -ne 1 -or [int]$targetRows[0].state -ne $RequiredState) { throw "$Name target $TargetComponent state differs at $Boundary" }
        } elseif (@($components | Where-Object { [int]$_.state -eq $RequiredState }).Count -eq 0) {
            throw "$Name did not expose state token $RequiredState at $Boundary"
        }
    }
    if ($RequiredState -eq 1 -and [bool]$State.overlay.gizmo_pointer_captured) {
        throw "$Name captured the pointer during hover"
    }
    if ($RequiredState -eq 2 -and
        (-not [bool]$State.overlay.gizmo_pointer_captured -or [int]$State.overlay.gizmo_phase -ne 2)) {
        throw "$Name active state did not own pointer capture"
    }
    if ([double]$State.overlay.gizmo_world_scale -le 0 -or [double]$State.overlay.gizmo_canonical_visual_diameter -le 0) { throw "$Name Gizmo scale/diameter is invalid" }
    if ($Name -eq "V02-gizmo-components" -and ([int]$State.overlay.gizmo_snap_token -ne 1 -or
        -not [bool]$State.overlay.pick_probe.valid -or [int]$State.overlay.pick_probe.classified_component -ne $TargetComponent)) {
        throw "V02 target probe/snap contract differs for component $TargetComponent"
    }
    if ($Name -eq "V03-parent-space" -and ([int]$State.overlay.gizmo_basis_token -ne 1 -or
        -not [bool]$State.overlay.parent_world_valid -or -not [bool]$State.overlay.parent_invertible -or
        [double]$State.overlay.parent_determinant -ge -1.0e-9)) { throw "V03 parent-space basis is not rotated negative non-uniform" }
    if ($Name -eq "V04-depth-xray") {
        $expectedDepth = if ($Variant -eq "occlusion-dither") { 1 } else { 0 }
        if ([int]$State.overlay.gizmo_depth_token -ne $expectedDepth -or [int]$State.overlay.gizmo_plane_token -ne 0) { throw "V04 depth/plane token differs for $Variant" }
    }
    return $State
}

function Get-PixelAudit([Drawing.Bitmap]$Bitmap) {
    $sampled = 0; $nonBlack = 0; $colorful = 0
    $coherentRuns = 0; $structuredRows = 0
    # Thin grid/axis lines can fall almost entirely between an 8-pixel lattice
    # once the legacy diagnostic panels are removed. Keep the fail threshold
    # fixed and sample more densely so the audit measures rendered geometry.
    $sampleStride = 4
    $masks = @{
        red = @{ count = 0; minX = $Bitmap.Width; minY = $Bitmap.Height; maxX = -1; maxY = -1 }
        green = @{ count = 0; minX = $Bitmap.Width; minY = $Bitmap.Height; maxX = -1; maxY = -1 }
        blue = @{ count = 0; minX = $Bitmap.Width; minY = $Bitmap.Height; maxX = -1; maxY = -1 }
        yellow = @{ count = 0; minX = $Bitmap.Width; minY = $Bitmap.Height; maxX = -1; maxY = -1 }
        cyan = @{ count = 0; minX = $Bitmap.Width; minY = $Bitmap.Height; maxX = -1; maxY = -1 }
    }
    for ($y = 0; $y -lt $Bitmap.Height; $y += $sampleStride) {
        $rowRun = 0; $rowNonBlack = 0
        for ($x = 0; $x -lt $Bitmap.Width; $x += $sampleStride) {
            $p = $Bitmap.GetPixel($x, $y)
            $max = [Math]::Max($p.R, [Math]::Max($p.G, $p.B))
            $min = [Math]::Min($p.R, [Math]::Min($p.G, $p.B))
            ++$sampled
            if ($max -gt 20) {
                ++$nonBlack; ++$rowNonBlack; ++$rowRun
            }
            elseif ($rowRun -ge 3) { ++$coherentRuns; $rowRun = 0 }
            else { $rowRun = 0 }
            if (($max - $min) -gt 24 -and $max -gt 48) { ++$colorful }
            $kind = $null
            if ($p.R -gt 120 -and $p.R -gt ($p.G * 1.35) -and $p.R -gt ($p.B * 1.35)) { $kind = "red" }
            elseif ($p.G -gt 100 -and $p.G -gt ($p.R * 1.25) -and $p.G -gt ($p.B * 1.25)) { $kind = "green" }
            elseif ($p.B -gt 100 -and $p.B -gt ($p.R * 1.25) -and $p.B -gt ($p.G * 1.10)) { $kind = "blue" }
            elseif ($p.R -gt 120 -and $p.G -gt 100 -and $p.B -lt 120) { $kind = "yellow" }
            elseif ($p.G -gt 100 -and $p.B -gt 100 -and $p.R -lt 120) { $kind = "cyan" }
            if ($null -ne $kind) {
                $m = $masks[$kind]
                ++$m.count
                $m.minX = [Math]::Min($m.minX, $x); $m.minY = [Math]::Min($m.minY, $y)
                $m.maxX = [Math]::Max($m.maxX, $x); $m.maxY = [Math]::Max($m.maxY, $y)
            }
        }
        if ($rowRun -ge 3) { ++$coherentRuns }
        if ($rowNonBlack -ge 8) { ++$structuredRows }
    }
    # Minimal/grid fixtures are intentionally low-chroma; non-black coverage
    # and the structured overlay state provide the stronger nonblank proof.
    if ($nonBlack -lt 500 -or $colorful -lt 20) {
        throw "Pixel audit failed: nonBlack=$nonBlack colorful=$colorful sampled=$sampled"
    }
    # A degraded capture epoch can return a near-black dithered surface whose
    # absolute non-black and chroma counts still clear the fixed floors above
    # (measured on a 1920x1080 client: nonBlack=2461 colorful=539 with mean
    # luminance 2.7), while a genuine dark grid frame is defined by long
    # coherent runs of lit pixels.  Both counters are normalized by the sampled
    # row count so the floor does not depend on the client extent.  Calibration
    # over 72 genuine traces at 1280x720, 1920x1080 and 2560x1421 gives
    # coherentRunRatio 0.2275-9.94 and structuredRowRatio 0.2444-1.0; the
    # degraded sample scores 0.1444 and 0.1778.
    $auditRows = [Math]::Max(1, [Math]::Ceiling($Bitmap.Height / $sampleStride))
    $coherentRunRatio = [Math]::Round($coherentRuns / $auditRows, 6)
    $structuredRowRatio = [Math]::Round($structuredRows / $auditRows, 6)
    if ($coherentRunRatio -lt 0.19 -or $structuredRowRatio -lt 0.21) {
        throw "Pixel audit failed: surface is structurally degraded: coherentRunRatio=$coherentRunRatio structuredRowRatio=$structuredRowRatio nonBlack=$nonBlack colorful=$colorful sampled=$sampled"
    }
    $rects = [ordered]@{}
    foreach ($name in $masks.Keys) {
        $m = $masks[$name]
        $rects[$name] = [ordered]@{ count = $m.count; x = $m.minX; y = $m.minY; width = if ($m.maxX -ge 0) { $m.maxX - $m.minX + 1 } else { 0 }; height = if ($m.maxY -ge 0) { $m.maxY - $m.minY + 1 } else { 0 } }
    }
    return [ordered]@{ sampled = $sampled; nonBlack = $nonBlack; colorful = $colorful; coherentRuns = $coherentRuns; structuredRows = $structuredRows; coherentRunRatio = $coherentRunRatio; structuredRowRatio = $structuredRowRatio; masks = $rects }
}

function Capture-Client([Diagnostics.Process]$Process, [string]$Path) {
    $Process.Refresh()
    $handle = $Process.MainWindowHandle
    if ($handle -eq [IntPtr]::Zero) { throw "Demo has no main window" }
    # A non-interactive PowerShell host can lose the foreground lease to an
    # unrelated desktop window between capture fences. Restore and promote the
    # launch-owned HWND, then require foreground ownership before reading it.
    [void][MGIFEditorVisualNative]::ShowWindow($handle, 9)
    Pin-VisualWindow $handle
    $foreground = $false
    for ($activationAttempt = 0; $activationAttempt -lt 8; ++$activationAttempt) {
        [void][MGIFEditorVisualNative]::BringWindowToTop($handle)
        [void][MGIFEditorVisualNative]::SetForegroundWindow($handle)
        Start-Sleep -Milliseconds 25
        if ([MGIFEditorVisualNative]::GetForegroundWindow() -eq $handle) {
            $foreground = $true
            break
        }
    }
    if (-not $foreground) {
        # Foreground activation is advisory on Windows (focus-stealing
        # prevention can deny it to a background launcher). Keep the
        # launch-owned HWND/topmost binding and let the pixel audit decide
        # whether a transport produced the correct surface.
        Write-Host "[VISUAL_CAPTURE] foreground activation was denied; continuing with bound HWND"
    }
    # Wait for the compositor to publish the completed Vulkan surface.  A
    # dense 0.001-unit grid can finish GPU work before DWM has retired the
    # previous white client surface.
    Start-Sleep -Milliseconds 500
    [void][MGIFEditorVisualNative]::DwmFlush()
    $rect = New-Object MGIFEditorVisualNative+RECT
    if (-not [MGIFEditorVisualNative]::GetClientRect($handle, [ref]$rect)) { throw "GetClientRect failed" }
    $origin = New-Object MGIFEditorVisualNative+POINT
    if (-not [MGIFEditorVisualNative]::ClientToScreen($handle, [ref]$origin)) { throw "ClientToScreen failed" }
    $width = $rect.Right - $rect.Left; $height = $rect.Bottom - $rect.Top
    Write-Host "[VISUAL_CAPTURE] handle=$($handle.ToInt64()) client=${width}x${height} origin=($($origin.X),$($origin.Y))"
    if ($width -lt 640 -or $height -lt 480) { throw "Client area is unexpectedly small: ${width}x${height}" }
    $bitmap = [Drawing.Bitmap]::new($width, $height)
    $graphics = [Drawing.Graphics]::FromImage($bitmap)
    $transport = "none"
    $audit = $null
    try {
        # Do not erase/invalidate the client while the app is paused at a
        # capture fence; GLFW's Vulkan surface cannot repaint synchronously.
        [void][MGIFEditorVisualNative]::DwmFlush()
        $destinationDc = $graphics.GetHdc()
        try {
            $printed = [MGIFEditorVisualNative]::PrintWindow(
                $handle, $destinationDc, 0x00000003)
        }
        finally {
            $graphics.ReleaseHdc($destinationDc)
        }
        if ($printed) {
            $bitmap.Save($Path, [Drawing.Imaging.ImageFormat]::Png)
            try {
                $audit = Get-PixelAudit $bitmap
                $transport = "print-window"
            } catch { $audit = $null }
            if ($null -eq $audit) { Write-Host "[VISUAL_CAPTURE] transport=print-window handle=$($handle.ToInt64()) size=${width}x${height} audit=failed" }
        }

        if ($null -eq $audit) {
            $sourceDc = [MGIFEditorVisualNative]::GetDC($handle)
            if ($sourceDc -ne [IntPtr]::Zero) {
                $destinationDc = $graphics.GetHdc()
                try {
                    $copied = [MGIFEditorVisualNative]::BitBlt(
                        $destinationDc, 0, 0, $width, $height,
                        $sourceDc, 0, 0, 0x00CC0020)
                }
                finally {
                    $graphics.ReleaseHdc($destinationDc)
                    [void][MGIFEditorVisualNative]::ReleaseDC($handle, $sourceDc)
                }
                if ($copied) {
                    $bitmap.Save($Path, [Drawing.Imaging.ImageFormat]::Png)
                    try {
                        $audit = Get-PixelAudit $bitmap
                        $transport = "window-dc-bitblt"
                    } catch { $audit = $null }
                    if ($null -eq $audit) { Write-Host "[VISUAL_CAPTURE] transport=window-dc-bitblt handle=$($handle.ToInt64()) size=${width}x${height} audit=failed" }
                }
            }
        }

        if ($null -eq $audit) {
            $graphics.CopyFromScreen($origin.X, $origin.Y, 0, 0,
                [Drawing.Size]::new($width, $height), [Drawing.CopyPixelOperation]::SourceCopy)
            $bitmap.Save($Path, [Drawing.Imaging.ImageFormat]::Png)
            $audit = Get-PixelAudit $bitmap
            $transport = "desktop-copy"
            Write-Host "[VISUAL_CAPTURE] transport=desktop-copy handle=$($handle.ToInt64()) size=${width}x${height} audit=passed"
        }
    }
    finally {
        $graphics.Dispose(); $bitmap.Dispose()
    }
    return [ordered]@{ width = $width; height = $height; transport = $transport; audit = $audit }
}

function Get-PngBoundsAudit([string]$Path, $State) {
    $bitmap = [Drawing.Bitmap]::new($Path)
    try { return Get-ComponentBoundsAudit $State $bitmap }
    finally { $bitmap.Dispose() }
}

function Capture-ClientVerified([Diagnostics.Process]$Process, [string]$Path) {
    $lastError = $null
    for ($attempt = 0; $attempt -lt 5; ++$attempt) {
        try { return Capture-Client $Process $Path }
        catch {
            $lastError = $_
            Start-Sleep -Milliseconds 150
        }
    }
    throw $lastError
}

function Resize-Client([Diagnostics.Process]$Process) {
    $Process.Refresh()
    $handle = $Process.MainWindowHandle
    if ($handle -eq [IntPtr]::Zero) { return $false }
    $client = New-Object MGIFEditorVisualNative+RECT
    $window = New-Object MGIFEditorVisualNative+RECT
    if (-not [MGIFEditorVisualNative]::GetClientRect($handle, [ref]$client) -or
        -not [MGIFEditorVisualNative]::GetWindowRect($handle, [ref]$window)) {
        throw "Could not query Demo window bounds"
    }
    $currentClientWidth = $client.Right - $client.Left
    $currentClientHeight = $client.Bottom - $client.Top
    # Width/height describe the window client area. Requested DPI is applied to
    # the offscreen editor attachments and is audited separately from the real
    # window framebuffer scale.
    $targetClientWidth = [Math]::Max(640, $Width)
    $targetClientHeight = [Math]::Max(480, $Height)
    $outerWidth = ($window.Right - $window.Left) + $targetClientWidth - $currentClientWidth
    $outerHeight = ($window.Bottom - $window.Top) + $targetClientHeight - $currentClientHeight
    # Preserve the current position but allow the requested size to apply;
    # SWP_NOSIZE/SWP_NOMOVE would silently ignore the dimensions.
    # Keep the short-lived evidence window above unrelated desktop windows.
    # SWP_NOZORDER would discard the HWND_TOPMOST insert-after value and can
    # make CopyFromScreen capture an unrelated foreground window.
    $flags = 0x0040
    # Keep the client area on the primary capture display.  The desktop may
    # retain a stale window position from a prior run; using it can place the
    # logical gizmo bounds below the visible screen and silently miss input.
    $clientOrigin = New-Object MGIFEditorVisualNative+POINT
    if (-not [MGIFEditorVisualNative]::ClientToScreen($handle, [ref]$clientOrigin)) {
        throw "ClientToScreen failed while resizing the visual window"
    }
    $frameX = $clientOrigin.X - $window.Left
    $frameY = $clientOrigin.Y - $window.Top
    $targetX = 0
    $targetY = 0
    if ($Width -gt 1280 -or $Height -gt 720) {
        $targetX = -$frameX
        $targetY = -$frameY
    }
    $changed = [MGIFEditorVisualNative]::SetWindowPos($handle, [IntPtr](-1),
        $targetX, $targetY, $outerWidth, $outerHeight, $flags)
    if (-not $changed) { throw "SetWindowPos failed for V07" }
    Start-Sleep -Milliseconds 150
    return $true
}

function Get-ComponentPoint([Diagnostics.Process]$Process, [string]$StatePath, [int]$TargetComponent = -1) {
    $state = Read-VisualState $StatePath
    $components = @($state.overlay.gizmo_components)
    Assert-ComponentIndexTable $components "Gizmo state"
    if ($TargetComponent -ge 0) {
        if ($TargetComponent -ge 13) { throw "Target Gizmo component is out of range" }
        $probe = $state.overlay.pick_probe
        if (-not [bool]$probe.valid -or [int]$probe.classified_component -ne $TargetComponent) {
            throw "Application did not expose an exact pick probe for component $TargetComponent"
        }
        $clientProbeX = [double]$probe.logical_x
        $clientProbeY = [double]$probe.logical_y
        $drawProbeY = 2.0 * [double]$state.viewport.logical_origin[1] +
            [double]$state.viewport.logical_extent[1] - $clientProbeY
        $selectedRows = @($components | Where-Object { [int]$_.index -eq $TargetComponent })
        if ($selectedRows.Count -ne 1) { throw "Target Gizmo component index is not unique" }
        $selected = $selectedRows[0]
    } elseif ($null -ne $state.overlay.pick_probe -and [bool]$state.overlay.pick_probe.valid) {
        $probe = $state.overlay.pick_probe
        $probeComponent = [int]$probe.classified_component
        if ($probeComponent -lt 0 -or $probeComponent -ge 13) { throw "Application pick probe component is out of range" }
        $clientProbeX = [double]$probe.logical_x
        $clientProbeY = [double]$probe.logical_y
        $drawProbeY = 2.0 * [double]$state.viewport.logical_origin[1] +
            [double]$state.viewport.logical_extent[1] - $clientProbeY
        $selectedRows = @($components | Where-Object { [int]$_.index -eq $probeComponent })
        if ($selectedRows.Count -ne 1) { throw "Application pick probe component index is not unique" }
        $selected = $selectedRows[0]
    } else {
        $preferred = @(2, 1, 11, 10, 0, 9, 8, 7, 6, 12, 5, 4, 3)
        $selected = $null
        foreach ($index in $preferred) {
            $candidate = $components[$index]
            $candidateBounds = $candidate.screen_bounds
            if ([bool]$candidateBounds.valid -and [double]$candidateBounds.width -ge 1.0 -and [double]$candidateBounds.height -ge 1.0) {
                $selected = $candidate
                break
            }
        }
        if ($null -eq $selected) { throw "No stable Gizmo component probe bounds were available" }
        $bounds = $selected.screen_bounds
        $clientProbeX = [double]$bounds.x + [double]$bounds.width * 0.5
        $drawProbeY = [double]$bounds.y + [double]$bounds.height * 0.35
        $viewportOriginY = [double]$state.viewport.logical_origin[1]
        $viewportExtentY = [double]$state.viewport.logical_extent[1]
        $clientProbeY = 2.0 * $viewportOriginY + $viewportExtentY - $drawProbeY
    }
    $Process.Refresh()
    $handle = $Process.MainWindowHandle
    [void][MGIFEditorVisualNative]::ShowWindow($handle, 9)
    [void][MGIFEditorVisualNative]::BringWindowToTop($handle)
    [void][MGIFEditorVisualNative]::SetForegroundWindow($handle)
    Start-Sleep -Milliseconds 50
    $origin = New-Object MGIFEditorVisualNative+POINT
    if (-not [MGIFEditorVisualNative]::ClientToScreen($handle, [ref]$origin)) { throw "ClientToScreen failed for input" }
    return [ordered]@{
        component = [int]$selected.index
        clientX = $clientProbeX
        clientY = $clientProbeY
        drawClientY = $drawProbeY
        x = [int]($origin.X + $clientProbeX)
        y = [int]($origin.Y + $clientProbeY)
        coordinateSpace = "screen-pixels-from-client-logical-bounds-y-mirrored-for-ray"
    }
}

function Set-InputPoint([Diagnostics.Process]$Process, $Point, [bool]$Lock = $false) {
    $Process.Refresh()
    $handle = $Process.MainWindowHandle
    if ($handle -eq [IntPtr]::Zero) { return $false }
    [void][MGIFEditorVisualNative]::ShowWindow($handle, 9)
    [void][MGIFEditorVisualNative]::BringWindowToTop($handle)
    $origin = New-Object MGIFEditorVisualNative+POINT
    if (-not [MGIFEditorVisualNative]::ClientToScreen($handle, [ref]$origin)) { return $false }
    $screenX = [int]($origin.X + [double]$Point.clientX)
    $screenY = [int]($origin.Y + [double]$Point.clientY)
    if ($Lock) {
        $clip = New-Object MGIFEditorVisualNative+RECT
        $clip.Left = $screenX
        $clip.Top = $screenY
        $clip.Right = $screenX + 1
        $clip.Bottom = $screenY + 1
        if (-not [MGIFEditorVisualNative]::ClipCursor([ref]$clip)) {
            throw "ClipCursor failed for Gizmo input"
        }
    }
    if (-not [MGIFEditorVisualNative]::SetCursorPos($screenX, $screenY)) { return $false }
    $observed = New-Object MGIFEditorVisualNative+POINT
    if (-not [MGIFEditorVisualNative]::GetCursorPos([ref]$observed) -or
        [Math]::Abs($observed.X - $screenX) -gt 1 -or
        [Math]::Abs($observed.Y - $screenY) -gt 1) {
        throw "Gizmo input cursor did not stabilize at the requested point"
    }
    return $true
}

function Send-LeftButton([Diagnostics.Process]$Process, $Point, [bool]$Down) {
    $flag = if ($Down) { 0x0002 } else { 0x0004 }
    $message = if ($Down) { 0x0201 } else { 0x0202 }
    $wParam = if ($Down) { [UIntPtr]::new([uint64]1) } else { [UIntPtr]::Zero }
    $clientX = if ($null -ne $Point) { [int][Math]::Round([double]$Point.clientX) } else { 0 }
    $clientY = if ($null -ne $Point) { [int][Math]::Round([double]$Point.clientY) } else { 0 }
    $packed = (($clientY -band 0xffff) -shl 16) -bor ($clientX -band 0xffff)
    for ($attempt = 0; $attempt -lt 5; ++$attempt) {
        if ($null -eq $Point -or -not (Set-InputPoint $Process $Point $false)) {
            throw "Could not stabilize the physical cursor before button injection"
        }
        [MGIFEditorVisualNative]::mouse_event($flag, 0, 0, 0, [UIntPtr]::Zero)
        $Process.Refresh()
        if ($Process.MainWindowHandle -ne [IntPtr]::Zero -and
            -not [MGIFEditorVisualNative]::PostMessage($Process.MainWindowHandle, $message, $wParam, [IntPtr]$packed)) {
            throw "Could not post the left-button message to Demo"
        }
        Start-Sleep -Milliseconds 20
        $pressed = (([int][MGIFEditorVisualNative]::GetAsyncKeyState(0x01)) -band 0x8000) -ne 0
        if ($pressed -eq $Down) { return }
    }
    throw "Left-button injection did not reach the requested state"
}

function Invoke-VisualCase([string]$Name, [string]$VisualVariant, [int]$TargetComponent, [string]$ArtifactId) {
    $caseRoot = Join-Path $EvidenceRoot $ArtifactId
    $syncRoot = Join-Path $caseRoot "sync"
    New-Item -ItemType Directory -Path $syncRoot -Force | Out-Null
    $statePath = Join-Path $caseRoot "state.json"
    $stdoutPath = Join-Path $caseRoot "stdout.log"
    $stderrPath = Join-Path $caseRoot "stderr.log"
    $metricsPath = Join-Path $caseRoot "metrics.json"
    $pngPath = Join-Path $caseRoot "post-release.png"
    $pixelPath = Join-Path $caseRoot "pixel.json"
    $normalPngPath = Join-Path $caseRoot "normal.png"
    $hoverPngPath = Join-Path $caseRoot "hover.png"
    $activePngPath = Join-Path $caseRoot "active.png"
    $normalStatePath = Join-Path $caseRoot "normal-state.json"
    $hoverStatePath = Join-Path $caseRoot "hover-state.json"
    $activeStatePath = Join-Path $caseRoot "active-state.json"
    $postReleaseStatePath = Join-Path $caseRoot "post-release-state.json"
    $inputPath = Join-Path $caseRoot "input.json"
    $manifestPath = Join-Path $caseRoot "manifest.json"

    $args = @(
        "--backend=vulkan", "--automation=csm-translate-stop", "--fixed-dt=0.016666667",
        "--warmup-frames=8", "--motion-frames=24", "--hold-frames=8", "--no-post", "--no-ddgi", "--auto-exit",
        "--editor-visual-case=$Name", "--editor-visual-state=$statePath",
        "--editor-visual-variant=$VisualVariant",
        "--editor-visual-sync-dir=$syncRoot", "--editor-visual-dpi=$DpiScale",
        "--editor-visual-executable-sha256=$ExecutableHash",
        "--editor-visual-source-sha256=$SourceIdentityHash",
        "--metrics-output=$metricsPath",
        "--capture-sync-timeout-ms=$($TimeoutSeconds * 1000)"
    )
    if ($TargetComponent -ge 0) { $args += "--editor-visual-component=$TargetComponent" }
    if ($Undecorated) { $args += "--editor-window-undecorated" }
    $startInfo = [Diagnostics.ProcessStartInfo]::new()
    $startInfo.FileName = $Executable
    $startInfo.Arguments = (($args | ForEach-Object { Quote-Argument $_ }) -join " ")
    $startInfo.WorkingDirectory = $BuildRoot
    $startInfo.UseShellExecute = $false
    $startInfo.CreateNoWindow = $true
    $startInfo.RedirectStandardOutput = $true
    $startInfo.RedirectStandardError = $true
    $startInfo.EnvironmentVariables["VK_INSTANCE_LAYERS"] = "VK_LAYER_KHRONOS_validation"
    $startInfo.EnvironmentVariables["VK_LOADER_LAYERS_DISABLE"] = "~implicit~"
    $startInfo.EnvironmentVariables["VK_LOADER_LAYERS_ENABLE"] = "VK_LAYER_KHRONOS_validation"
    # The host injects a known NVIDIA capture draw that is independently
    # classified in the P6-91 authority; filter only its exact VUID while
    # retaining all other validation and synchronization diagnostics.
    $startInfo.EnvironmentVariables["VK_LAYER_MESSAGE_ID_FILTER"] = "VUID-vkCmdDraw-None-09600"
    $startInfo.EnvironmentVariables["VK_KHRONOS_VALIDATION_VALIDATE_SYNC"] = "true"
    $startInfo.EnvironmentVariables["VK_KHRONOS_VALIDATION_SYNCVAL_SUBMIT_TIME_VALIDATION"] = "true"
    $process = [Diagnostics.Process]::new(); $process.StartInfo = $startInfo
    $stdoutTask = $null; $stderrTask = $null
    if (-not $process.Start()) { throw "Could not start Demo for $ArtifactId" }
    $stdoutTask = $process.StandardOutput.ReadToEndAsync()
    $stderrTask = $process.StandardError.ReadToEndAsync()
    $deadline = [DateTime]::UtcNow.AddSeconds($TimeoutSeconds)
    $handled = @{}; $observedMarkers = @(); $captured = $false; $captureInfo = $null
    $normalBoundsAudit = $null; $controlBoundsAudit = $null; $hoverBoundsAudit = $null; $activeBoundsAudit = $null
    $normalCaptureInfo = $null; $hoverCaptureInfo = $null; $activeCaptureInfo = $null
    $normalRenderedFrame = $null
    $hoverRenderedFrame = $null
    $activeRenderedFrame = $null
    $controlRenderedFrame = $null
    $controlCapturePending = $false
    $controlRenderedObserved = $false
    $normalCaptured = $false; $hoverCaptured = $false; $activeCaptured = $false; $mouseDown = $false
    $normalState = $null; $postReleaseState = $null
    $inputPoint = $null
    $dragPoint = $null
    $pendingOutsideDragPoint = $null
    $inputLocked = $false
    $inputArmed = $false
    $resized = $false
    $expectedMarkers = @("arm-last-moving", "last-moving", "first-still", "normal-rendered", "arm-settled", "settled", "control-still", "control-rendered", "control-presented")
    $expectedFirstStillFrame = 8 + 24
    $expectedNormalRenderedFenceFrame = $expectedFirstStillFrame + 1
    $expectedSettledFrame = $expectedFirstStillFrame + 8 - 1
    $expectedControlStillFrame = $expectedSettledFrame + 1
    $expectedControlRenderedFenceFrame = $expectedControlStillFrame + 1
    $expectedControlPresentedFenceFrame = $expectedControlStillFrame + 2
    try {
        while (-not $process.HasExited -and [DateTime]::UtcNow -lt $deadline) {
            if (-not $resized) {
                $resized = Resize-Client $process
                if (-not $resized) {
                    Start-Sleep -Milliseconds 50
                    continue
                }
            }
            if ($null -ne $inputPoint -and $inputArmed -and -not $activeCaptured -and $Name -ne "V01-grid") {
                # Re-apply the logical probe while the app waits at a marker;
                # this compensates for Win32 non-client/DPI rounding and any
                # window-manager repositioning between frames.
                $holdPoint = if ($null -ne $dragPoint) { $dragPoint } else { $inputPoint }
                [void](Set-InputPoint $process $holdPoint $inputLocked)
            }
            foreach ($ready in @(Get-ChildItem -LiteralPath $syncRoot -Filter "*.ready.json" -File -ErrorAction SilentlyContinue)) {
                if ($handled.ContainsKey($ready.Name)) { continue }
                $payload = Get-Content -LiteralPath $ready.FullName -Raw | ConvertFrom-Json
                $marker = [string]$payload.marker
                if ($observedMarkers.Count -ge $expectedMarkers.Count -or $marker -ne $expectedMarkers[$observedMarkers.Count]) {
                    throw "Capture marker order differs for ${Name}: got $marker at index $($observedMarkers.Count)"
                }
                if ($marker -eq "normal-rendered") {
                    if ([string]$payload.phase -ne "post-render" -or
                        [int64]$payload.frame -ne $expectedNormalRenderedFenceFrame -or
                        [int64]$payload.rendered_frame -ne $expectedFirstStillFrame) {
                        throw "$ArtifactId normal-rendered fence is not bound to the rendered first-still frame"
                    }
                } elseif ($marker -eq "control-rendered") {
                    if ([string]$payload.phase -ne "post-render" -or
                        [int64]$payload.frame -ne $expectedControlRenderedFenceFrame -or
                        [int64]$payload.rendered_frame -ne $expectedControlStillFrame) {
                        throw "$ArtifactId control-rendered fence is not bound to the rendered control frame"
                    }
                } elseif ($marker -eq "control-presented") {
                    if ([string]$payload.phase -ne "post-render" -or
                        [int64]$payload.frame -ne $expectedControlPresentedFenceFrame -or
                        [int64]$payload.rendered_frame -ne ($expectedControlPresentedFenceFrame - 1)) {
                        throw "$ArtifactId control-presented fence is not bound to its preceding rendered frame"
                    }
                } elseif ($marker -eq "arm-settled") {
                    if ([string]$payload.phase -ne "pre-render" -or
                        [int64]$payload.frame -ne ($expectedSettledFrame - 1) -or
                        [int64]$payload.rendered_frame -ne ($expectedSettledFrame - 2)) {
                        throw "$ArtifactId arm-settled fence is not bound to its preceding rendered frame"
                    }
                } elseif ($marker -eq "settled") {
                    if ([string]$payload.phase -ne "pre-render" -or
                        [int64]$payload.frame -ne $expectedSettledFrame -or
                        [int64]$payload.rendered_frame -ne ($expectedSettledFrame - 1)) {
                        throw "$ArtifactId settled fence is not bound to its preceding rendered frame"
                    }
                } elseif ([string]$payload.phase -ne "pre-render") {
                    throw "$ArtifactId capture marker $marker is not a pre-render fence"
                }
                $observedMarkers += $marker
                if ($marker -eq "arm-last-moving" -and $Name -ne "V01-grid" -and -not $inputLocked) {
                    # Hold the real OS pointer outside the viewport until the
                    # normal-state boundary has been captured.
                    $blankPoint = [ordered]@{ clientX = 5.0; clientY = 5.0 }
                    [void](Set-InputPoint $process $blankPoint $true)
                    $inputLocked = $true
                }
                elseif ($marker -eq "control-still" -and -not $captured) {
                    # Keep this pre-render fence as an arm point only. The
                    # screenshot is deferred until control-presented so the
                    # post-release frame has two full loop turns to settle.
                    [void](Assert-VisualState (Read-VisualState $statePath) $Name $VisualVariant $TargetComponent "control-still")
                    $controlCapturePending = $true
                }
                elseif ($marker -eq "control-rendered" -and -not $captured) {
                    if (-not $controlCapturePending) { throw "$ArtifactId control-rendered arrived without control-still" }
                    $intermediateControlState = Assert-VisualState (Read-VisualState $statePath) $Name $VisualVariant $TargetComponent "control-rendered"
                    if (-not [bool]$intermediateControlState.fence.gpu_idle_waited) {
                        throw "$ArtifactId control-rendered fence has no GPU completion wait proof"
                    }
                    $controlRenderedObserved = $true
                }
                elseif ($marker -eq "control-presented" -and -not $captured) {
                    if (-not $controlCapturePending -or -not $controlRenderedObserved) { throw "$ArtifactId control-presented arrived without control-rendered" }
                    $state = Assert-VisualState (Read-VisualState $statePath) $Name $VisualVariant $TargetComponent "control-presented"
                    if (-not [bool]$state.fence.gpu_idle_waited) {
                        throw "$ArtifactId control-presented fence has no GPU completion wait proof"
                    }
                    $controlStage = Capture-VisualStage $process $pngPath $state $TargetComponent "$ArtifactId post-release"
                    $captureInfo = $controlStage.capture
                    $expectedWidth = [Math]::Max(640, $Width)
                    $expectedHeight = [Math]::Max(480, $Height)
                    # Exact on both axes.  A 64 px height tolerance silently
                    # accepted a clamped 2560x1421 client for a requested
                    # 2560x1440 axis value, so the recorded resolution did not
                    # match the pinned matrix entry.
                    if ($captureInfo.width -ne $expectedWidth -or $captureInfo.height -ne $expectedHeight) {
                        throw "$ArtifactId client extent differs: got $($captureInfo.width)x$($captureInfo.height), expected ${expectedWidth}x${expectedHeight} for a $WindowDecoration window"
                    }
                    $controlRenderedFrame = [int64]$payload.rendered_frame
                    $postReleaseState = $state
                    Copy-Item -LiteralPath $statePath -Destination $postReleaseStatePath -Force
                    if ($Name -ne "V01-grid") {
                        $controlBoundsAudit = $controlStage.audit
                        if ([bool]$state.overlay.gizmo_pointer_captured) { throw "$ArtifactId retained pointer capture after release" }
                    }
                    Write-Json $pixelPath ([ordered]@{ documentKind = "p7-editor-visual-pixel-audit"; schemaVersion = 2; case = $Name; variant = $VisualVariant; targetComponent = if ($TargetComponent -ge 0) { $TargetComponent } else { $null }; screenshot = $captureInfo; bounds = $controlBoundsAudit })
                    $captured = $true
                    $controlCapturePending = $false
                }
                elseif ($marker -eq "first-still" -and $Name -ne "V01-grid" -and $null -eq $inputPoint) {
                    $normalState = Assert-VisualState (Read-VisualState $statePath) $Name $VisualVariant $TargetComponent "first-still" 0
                    if (@($normalState.overlay.gizmo_components | Where-Object { [int]$_.state -ne 0 }).Count -ne 0) { throw "$ArtifactId normal state is already interactive" }
                    Copy-Item -LiteralPath $statePath -Destination $normalStatePath -Force
                    $inputPoint = Get-ComponentPoint $process $statePath $TargetComponent
                    Write-Json $inputPath ([ordered]@{ case = $Name; variant = $VisualVariant; targetComponent = if ($TargetComponent -ge 0) { $TargetComponent } else { $null }; component = $inputPoint.component; screen = $inputPoint; action = if ($Name -eq "V03-parent-space") { "hover-drag-outside-release" } else { "hover-press-release" } })
                }
                elseif ($marker -eq "normal-rendered" -and $Name -ne "V01-grid" -and $null -ne $inputPoint -and -not $normalCaptured) {
                    # This is a post-render fence: the application has already
                    # drawn the first-still frame with the cursor at the blank
                    # point. Capture that frame before arming the target.
                    $renderedNormalState = Assert-VisualState (Read-VisualState $statePath) $Name $VisualVariant $TargetComponent "normal-rendered" 0
                    if (@($renderedNormalState.overlay.gizmo_components | Where-Object { [int]$_.state -ne 0 }).Count -ne 0) { throw "$ArtifactId rendered normal state is already interactive" }
                    if (-not [bool]$renderedNormalState.fence.gpu_idle_waited) {
                        throw "$ArtifactId normal-rendered fence has no GPU completion wait proof"
                    }
                    $normalState = $renderedNormalState
                    $normalRenderedFrame = [int64]$payload.rendered_frame
                    Copy-Item -LiteralPath $statePath -Destination $normalStatePath -Force
                    $normalStage = Capture-VisualStage $process $normalPngPath $normalState $inputPoint.component "$ArtifactId normal"
                    $normalCaptureInfo = $normalStage.capture
                    $normalBoundsAudit = $normalStage.audit
                    $normalCaptured = $true
                    [void](Set-InputPoint $process $inputPoint $true)
                    $inputLocked = $true
                    $inputArmed = $true
                }
                elseif ($marker -eq "arm-settled" -and $Name -ne "V01-grid" -and -not $hoverCaptured) {
                    $hoverState = Assert-VisualState (Read-VisualState $statePath) $Name $VisualVariant $TargetComponent "arm-settled" 1
                    $hoverRenderedFrame = [int64]$payload.rendered_frame
                    Copy-Item -LiteralPath $statePath -Destination $hoverStatePath -Force
                    $hoverStage = Capture-VisualStage $process $hoverPngPath $hoverState $inputPoint.component "$ArtifactId hover"
                    $hoverCaptureInfo = $hoverStage.capture
                    $hoverBoundsAudit = $hoverStage.audit
                    $hoverCaptured = $true
                    if ($null -eq $inputPoint) { throw "Gizmo input point was not prepared" }
                    $process.Refresh()
                    [void][MGIFEditorVisualNative]::SetForegroundWindow($process.MainWindowHandle)
                    Start-Sleep -Milliseconds 100
                    [void](Set-InputPoint $process $inputPoint $inputLocked)
                    Send-LeftButton $process $inputPoint $true
                    $mouseDown = $true
                    if ($Name -eq "V03-parent-space") {
                        # Keep the cursor on the hit point for the next
                        # application frame so the gizmo can enter its active
                        # phase before an outside-viewport drag is introduced.
                        [void][MGIFEditorVisualNative]::ReleaseCursor([IntPtr]::Zero)
                        $inputLocked = $false
                        $viewportRight = [double]$hoverState.viewport.logical_origin[0] + [double]$hoverState.viewport.logical_extent[0]
                        $pendingOutsideDragPoint = [ordered]@{
                            clientX = [Math]::Min([double]$Width - 2.0, $viewportRight + 24.0)
                            clientY = [double]$inputPoint.clientY
                        }
                        if ([double]$pendingOutsideDragPoint.clientX -le $viewportRight) {
                            throw "$ArtifactId outside drag point did not cross the viewport boundary"
                        }
                    }
                }
                elseif ($marker -eq "settled" -and $Name -ne "V01-grid" -and $mouseDown -and -not $activeCaptured) {
                    $activeState = Assert-VisualState (Read-VisualState $statePath) $Name $VisualVariant $TargetComponent "settled" 2
                    $activeRenderedFrame = [int64]$payload.rendered_frame
                    Copy-Item -LiteralPath $statePath -Destination $activeStatePath -Force
                    $activeStage = Capture-VisualStage $process $activePngPath $activeState $inputPoint.component "$ArtifactId active"
                    $activeCaptureInfo = $activeStage.capture
                    $activeBoundsAudit = $activeStage.audit
                    $activeCaptured = $true
                    if ($Name -eq "V03-parent-space") {
                        if ($null -eq $pendingOutsideDragPoint) {
                            throw "$ArtifactId outside drag point was not armed after active capture"
                        }
                        $dragPoint = $pendingOutsideDragPoint
                    }
                    $releasePoint = if ($null -ne $dragPoint) { $dragPoint } else { $inputPoint }
                    Send-LeftButton $process $releasePoint $false
                    $mouseDown = $false
                    [void][MGIFEditorVisualNative]::ReleaseCursor([IntPtr]::Zero)
                    $inputLocked = $false
                }
                $continue = Join-Path $syncRoot "$marker.continue"
                # Write and close the marker in one operation. New-Item can
                # briefly retain a handle on Windows while the app consumes it.
                [IO.File]::WriteAllBytes($continue, [byte[]]@())
                $handled[$ready.Name] = $true
            }
            Start-Sleep -Milliseconds 100
            $process.Refresh()
        }
        if (-not $process.HasExited) { $process.Kill(); throw "Demo timed out for $ArtifactId" }
        $stdoutTask.Wait(); $stderrTask.Wait()
        [IO.File]::WriteAllText($stdoutPath, $stdoutTask.Result, [Text.UTF8Encoding]::new($false))
        [IO.File]::WriteAllText($stderrPath, $stderrTask.Result, [Text.UTF8Encoding]::new($false))
        if ($process.ExitCode -ne 0) { throw "Demo exited $($process.ExitCode) for $ArtifactId" }
        $combined = $stdoutTask.Result + $stderrTask.Result
        if ($combined -match "(?im)(VUID-|SYNC-HAZARD|Validation Error|device lost|marker=failed|Assertion failed|Traceback)") { throw "Runtime diagnostics found for $ArtifactId" }
        $requiredLogMarkers = @(
            "marker=config",
            "[EDITOR_VISUAL] marker=scene-ready",
            "[CSM_AUTOMATION] marker=start",
            "[CSM_AUTOMATION] marker=complete"
        )
        foreach ($requiredLogMarker in $requiredLogMarkers) {
            if (@([regex]::Matches($combined, [regex]::Escape($requiredLogMarker))).Count -ne 1) {
                throw "$ArtifactId did not emit exactly one $requiredLogMarker marker"
            }
        }
        if (-not $captured) { throw "No control-still capture was observed for $ArtifactId" }
        if ($controlCapturePending) { throw "Control-rendered capture was not completed for $ArtifactId" }
        if ($observedMarkers.Count -ne $expectedMarkers.Count -or (@($observedMarkers) -join '|') -ne (@($expectedMarkers) -join '|')) {
            throw "Capture marker sequence was incomplete for $ArtifactId"
        }
        if ($Name -ne "V01-grid" -and (-not $normalCaptured -or -not $hoverCaptured -or -not $activeCaptured)) { throw "Normal/hover/active capture sequence was incomplete for $ArtifactId" }
        if (-not (Test-Path -LiteralPath $metricsPath -PathType Leaf)) { throw "Metrics artifact is missing for $ArtifactId" }
        if ($Name -eq "V03-parent-space") {
            $beforeTrs = $normalState.selected_node_transform
            $afterTrs = $postReleaseState.selected_node_transform
            $beforeTrsJson = $beforeTrs | ConvertTo-Json -Compress
            $afterTrsJson = $afterTrs | ConvertTo-Json -Compress
            if (-not [bool]$beforeTrs.available -or -not [bool]$afterTrs.available -or
                $beforeTrsJson -eq $afterTrsJson -or
                [int]$postReleaseState.history.undo -ne 1) {
                throw "V03 outside-viewport drag did not commit one TRS state"
            }
        }
        $manifest = [ordered]@{
            documentKind = "p7-editor-visual-case"
            schemaVersion = 2
            artifactId = $ArtifactId
            case = $Name
            variant = $VisualVariant
            targetComponent = if ($TargetComponent -ge 0) { $TargetComponent } else { $null }
            executable = [ordered]@{ path = $Executable; sha256 = $ExecutableHash }
            source = [ordered]@{ identitySha256 = $SourceIdentityHash; files = $SourceRecords }
            requested = [ordered]@{ width = $Width; height = $Height; dpi = $DpiScale; windowDecoration = $WindowDecoration }
            metrics = [ordered]@{ path = "metrics.json"; bytes = (Get-Item $metricsPath).Length; sha256 = Get-Hash $metricsPath }
            stdout = [ordered]@{ path = "stdout.log"; bytes = (Get-Item $stdoutPath).Length; sha256 = Get-Hash $stdoutPath }
            stderr = [ordered]@{ path = "stderr.log"; bytes = (Get-Item $stderrPath).Length; sha256 = Get-Hash $stderrPath }
            readyMarkers = @($observedMarkers)
            markerCount = $handled.Count
            validation = [ordered]@{
                layers = @("VK_LAYER_KHRONOS_validation")
                synchronization = $true
                filteredMessageIds = @("VUID-vkCmdDraw-None-09600")
                filterDisposition = "P6-91-authorized-external-capture-only"
            }
        }
        if ($Name -ne "V01-grid") {
            $manifest.normal = [ordered]@{ renderedFrame = $normalRenderedFrame; png = [ordered]@{ path = "normal.png"; bytes = (Get-Item $normalPngPath).Length; sha256 = Get-Hash $normalPngPath; width = $normalCaptureInfo.width; height = $normalCaptureInfo.height; audit = $normalCaptureInfo.audit }; state = [ordered]@{ path = "normal-state.json"; bytes = (Get-Item $normalStatePath).Length; sha256 = Get-Hash $normalStatePath }; pixel = $normalBoundsAudit }
            $manifest.hover = [ordered]@{ renderedFrame = $hoverRenderedFrame; png = [ordered]@{ path = "hover.png"; bytes = (Get-Item $hoverPngPath).Length; sha256 = Get-Hash $hoverPngPath; width = $hoverCaptureInfo.width; height = $hoverCaptureInfo.height; audit = $hoverCaptureInfo.audit }; state = [ordered]@{ path = "hover-state.json"; bytes = (Get-Item $hoverStatePath).Length; sha256 = Get-Hash $hoverStatePath }; pixel = $hoverBoundsAudit }
            $manifest.active = [ordered]@{ renderedFrame = $activeRenderedFrame; png = [ordered]@{ path = "active.png"; bytes = (Get-Item $activePngPath).Length; sha256 = Get-Hash $activePngPath; width = $activeCaptureInfo.width; height = $activeCaptureInfo.height; audit = $activeCaptureInfo.audit }; state = [ordered]@{ path = "active-state.json"; bytes = (Get-Item $activeStatePath).Length; sha256 = Get-Hash $activeStatePath }; pixel = $activeBoundsAudit }
            $manifest.postRelease = [ordered]@{ renderedFrame = $controlRenderedFrame; png = [ordered]@{ path = "post-release.png"; bytes = (Get-Item $pngPath).Length; sha256 = Get-Hash $pngPath; width = $captureInfo.width; height = $captureInfo.height; audit = $captureInfo.audit }; state = [ordered]@{ path = "post-release-state.json"; bytes = (Get-Item $postReleaseStatePath).Length; sha256 = Get-Hash $postReleaseStatePath }; pixel = $controlBoundsAudit }
            $manifest.input = [ordered]@{ path = "input.json"; bytes = (Get-Item $inputPath).Length; sha256 = Get-Hash $inputPath }
        } else {
            $manifest.control = [ordered]@{ renderedFrame = $controlRenderedFrame; png = [ordered]@{ path = "post-release.png"; bytes = (Get-Item $pngPath).Length; sha256 = Get-Hash $pngPath; width = $captureInfo.width; height = $captureInfo.height; audit = $captureInfo.audit }; state = [ordered]@{ path = "post-release-state.json"; bytes = (Get-Item $postReleaseStatePath).Length; sha256 = Get-Hash $postReleaseStatePath }; pixel = [ordered]@{ path = "pixel.json"; bytes = (Get-Item $pixelPath).Length; sha256 = Get-Hash $pixelPath } }
        }
        Write-Json $manifestPath $manifest
        return $manifest
    }
    finally {
        try {
            if ($mouseDown) { Send-LeftButton $process $inputPoint $false }
        }
        finally {
            [void][MGIFEditorVisualNative]::ReleaseCursor([IntPtr]::Zero)
        }
        if (-not $process.HasExited) { $process.Kill() }
        $process.WaitForExit()
        if ($null -ne $stdoutTask) { $stdoutTask.Wait(); [IO.File]::WriteAllText($stdoutPath, $stdoutTask.Result, [Text.UTF8Encoding]::new($false)) }
        if ($null -ne $stderrTask) { $stderrTask.Wait(); [IO.File]::WriteAllText($stderrPath, $stderrTask.Result, [Text.UTF8Encoding]::new($false)) }
        $process.Dispose()
    }
}

$traces = @()
if ($Case -eq "all" -and ($Variant -ne "" -or $Component -ne -1)) {
    throw "-Variant/-Component require one explicit -Case"
}
if ($Case -ne "V02-gizmo-components" -and $Component -ne -1) {
    throw "-Component is only valid for V02-gizmo-components"
}

if ($Case -eq "all" -or $Case -eq "V01-grid") {
    $variants = @(
        "perspective-unit-0.001", "perspective-unit-1", "perspective-unit-1000",
        "orthographic-unit-0.001", "orthographic-unit-1", "orthographic-unit-1000",
        "perspective-horizon"
    )
    if ($Variant -ne "") {
        if ($variants -notcontains $Variant) { throw "Invalid V01 variant: $Variant" }
        $variants = @($Variant)
    }
    foreach ($item in $variants) {
        $traces += [ordered]@{ name = "V01-grid"; variant = $item; component = -1; artifactId = "V01-grid--$($item.Replace('.', 'p'))" }
    }
}
if ($Case -eq "all" -or $Case -eq "V02-gizmo-components") {
    if ($Variant -ne "" -and $Variant -ne "component") { throw "V02 variant must be component" }
    $components = if ($Component -ge 0) { @($Component) } else { @(0..12) }
    foreach ($item in $components) {
        $traces += [ordered]@{ name = "V02-gizmo-components"; variant = "component"; component = $item; artifactId = ("V02-gizmo-components--component-{0:D2}" -f $item) }
    }
}
if ($Case -eq "all" -or $Case -eq "V03-parent-space") {
    if ($Variant -ne "" -and $Variant -ne "rotated-negative-nonuniform") { throw "Invalid V03 variant: $Variant" }
    $traces += [ordered]@{ name = "V03-parent-space"; variant = "rotated-negative-nonuniform"; component = -1; artifactId = "V03-parent-space--rotated-negative-nonuniform" }
}
if ($Case -eq "all" -or $Case -eq "V04-depth-xray") {
    $variants = @("xray", "occlusion-dither")
    if ($Variant -ne "") {
        if ($variants -notcontains $Variant) { throw "Invalid V04 variant: $Variant" }
        $variants = @($Variant)
    }
    foreach ($item in $variants) {
        $traces += [ordered]@{ name = "V04-depth-xray"; variant = $item; component = -1; artifactId = "V04-depth-xray--$item" }
    }
}
if ($Case -eq "all" -or $Case -eq "V07-dpi-resize") {
    if ($Variant -ne "" -and $Variant -ne "framebuffer-dpi-resize") { throw "Invalid V07 variant: $Variant" }
    $dpiToken = $DpiScale.ToString("0.###", [Globalization.CultureInfo]::InvariantCulture).Replace('.', 'p')
    $traces += [ordered]@{ name = "V07-dpi-resize"; variant = "framebuffer-dpi-resize"; component = -1; artifactId = "V07-dpi-resize--${Width}x${Height}--dpi-$dpiToken" }
}

$results = @()
foreach ($trace in $traces) {
    Write-Output "Capturing $($trace.artifactId)"
    $results += Invoke-VisualCase $trace.name $trace.variant $trace.component $trace.artifactId
}
if (($Case -eq "all" -or $Case -eq "V04-depth-xray") -and $Variant -eq "") {
    $xray = @($results | Where-Object { $_.case -eq "V04-depth-xray" -and $_.variant -eq "xray" })
    $dither = @($results | Where-Object { $_.case -eq "V04-depth-xray" -and $_.variant -eq "occlusion-dither" })
    if ($xray.Count -ne 1 -or $dither.Count -ne 1 -or
        $xray[0].normal.png.sha256 -eq $dither[0].normal.png.sha256) {
        throw "V04 xray and occlusion-dither normal captures are not a distinct closed pair"
    }
}
$rootManifest = [ordered]@{
    documentKind = "p7-editor-visual-evidence"
    schemaVersion = 2
    authoritativePlatform = "Windows MSVC"
    executable = [ordered]@{ path = $Executable; sha256 = $ExecutableHash }
    source = [ordered]@{ identitySha256 = $SourceIdentityHash; files = $SourceRecords }
    requestedClientLogicalExtent = [ordered]@{ width = $Width; height = $Height }
    windowDecoration = $WindowDecoration
    virtualScreen = [ordered]@{ width = $virtualWidth; height = $virtualHeight }
    requiredOuterExtent = [ordered]@{ width = $requiredOuterWidth; height = $requiredOuterHeight }
    requestedFramebufferScale = $DpiScale
    matrix = [ordered]@{
        v01Variants = @($results | Where-Object { $_.case -eq "V01-grid" }).Count
        v02Components = @($results | Where-Object { $_.case -eq "V02-gizmo-components" }).Count
        v03ParentSpace = @($results | Where-Object { $_.case -eq "V03-parent-space" }).Count
        v04DepthModes = @($results | Where-Object { $_.case -eq "V04-depth-xray" }).Count
        v07DpiResize = @($results | Where-Object { $_.case -eq "V07-dpi-resize" }).Count
    }
    cases = $results
}
Write-Json (Join-Path $EvidenceRoot "evidence-manifest.json") $rootManifest
Write-Output "P7 editor visual capture passed: $EvidenceRoot"
