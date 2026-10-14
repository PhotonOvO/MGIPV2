[CmdletBinding()]
param(
    [string]$BuildRoot = "",
    [string]$EvidenceRoot = "",
    [ValidateSet("U02-hierarchy", "U03-transform-gizmo", "U03-inspector-transform", "U03-gizmo-translate", "U03-gizmo-escape", "U03-gizmo-escape-release", "U03-gizmo-release-escape", "U03-gizmo-focus-no-release", "U03-gizmo-focus-same-frame", "U03-gizmo-focus-delivered-release")][string]$Case = "U02-hierarchy",
    [ValidateSet("Debug", "Release")][string]$Configuration = "Debug",
    [ValidateRange(640, 3840)][int]$Width = 1280,
    [ValidateRange(480, 2160)][int]$Height = 720,
    [ValidateRange(0, 480)][int]$ExpectedDpi = 0,
    [ValidateRange(10, 900)][int]$TimeoutSeconds = 180,
    [string]$ExecutablePath = "",
    [switch]$ContractOnly,
    [switch]$ContractTraceOnly,
    [switch]$UseVulkanScreenshotLayer
)

Set-StrictMode -Version Latest
$ErrorActionPreference = "Stop"

$Protocol = "mgif-editor-ui-os-replay-v1"
$SchemaVersion = 1

function Get-BytesHash([byte[]]$Bytes) {
    $algorithm = [Security.Cryptography.SHA256]::Create()
    try {
        return ([BitConverter]::ToString(
            $algorithm.ComputeHash($Bytes))).Replace("-", "").ToLowerInvariant()
    }
    finally {
        $algorithm.Dispose()
    }
}

function Get-Hash([string]$Path) {
    return (Get-FileHash -LiteralPath $Path -Algorithm SHA256).Hash.ToLowerInvariant()
}

function New-Target(
    [string]$Semantic,
    [string]$Role,
    [string]$GraphRowRole = "") {
    return [pscustomobject][ordered]@{
        semantic = $Semantic
        role = $Role
        graphRowRole = $GraphRowRole
    }
}

function New-Input([string]$Kind, [string]$Value = "") {
    return [pscustomobject][ordered]@{ kind = $Kind; value = $Value }
}

function New-Dispatch(
    [string]$Source,
    [string]$PayloadKind,
    [string]$Operation,
    [string]$TargetRole = "",
    [string]$SecondaryRole = "",
    [string]$Text = "",
    [string]$Refresh = "none",
    [string]$EditPhase = "",
    [string]$EditValue = "") {
    return [pscustomobject][ordered]@{
        source = $Source
        payloadKind = $PayloadKind
        operation = $Operation
        targetRole = $TargetRole
        secondaryRole = $SecondaryRole
        text = $Text
        accepted = $true
        refresh = $Refresh
        phase = $EditPhase
        value = $EditValue
    }
}

function New-Phase(
    [string]$Id,
    [string]$Phase,
    [object]$Target,
    [object]$InputSpec,
    [bool]$RequireHovered = $false,
    [bool]$RequireFocused = $false,
    [bool]$RequireDropAccepted = $false,
    [string]$Capture = "none",
    [Nullable[int]]$ExpectedDispatchCount = $null) {
    $result = [pscustomobject][ordered]@{
        ordinal = -1
        id = $Id
        phase = $Phase
        target = $Target
        input = $InputSpec
        requireHovered = $RequireHovered
        requireFocused = $RequireFocused
        requireDropAccepted = $RequireDropAccepted
        capture = $Capture
    }
    if ($null -ne $ExpectedDispatchCount) {
        $result | Add-Member -NotePropertyName expectedDispatchCount `
            -NotePropertyValue ([int]$ExpectedDispatchCount)
    }
    return $result
}

function New-Step(
    [string]$Id,
    [string]$Subcase,
    [string]$Effect,
    [int]$RevisionDelta,
    [string]$History,
    [object[]]$Phases,
    [bool]$OrderDigestChanges = $false,
    [bool]$RelaunchProcessBefore = $false) {
    return [pscustomobject][ordered]@{
        index = -1
        id = $Id
        subcase = $Subcase
        effect = $Effect
        expectedRevisionDelta = $RevisionDelta
        history = $History
        expectHierarchyOrderDigestChange = $OrderDigestChanges
        relaunchProcessBefore = $RelaunchProcessBefore
        expectedDispatches = @()
        phases = @($Phases)
    }
}

function Set-GizmoExpectation(
    [object]$Phase,
    [int]$GizmoPhase,
    [bool]$CaptureHeld,
    [int]$BeginCount,
    [int]$PreviewCount,
    [int]$CommitCount,
    [Nullable[bool]]$PointerOutside = $null,
    [int]$DetachCount = 0,
    [int]$LatchCount = 0,
    [bool]$NoSelection = $false,
    [int]$DeliveredReleaseCount = 0,
    [int]$EscapePressCount = 0,
    [int]$FocusLossCount = 0,
    [int]$SuppressedReleaseCount = 0) {
    $Phase | Add-Member -NotePropertyName gizmo -NotePropertyValue (
        [pscustomobject][ordered]@{
            phase = $GizmoPhase
            captureHeld = $CaptureHeld
            beginCount = $BeginCount
            previewCount = $PreviewCount
            commitCount = $CommitCount
            pointerOutside = $PointerOutside
        })
    if ($Case -in @("U03-gizmo-escape", "U03-gizmo-escape-release", "U03-gizmo-release-escape")) {
        $Phase.gizmo | Add-Member -NotePropertyName detachCount -NotePropertyValue $DetachCount
        $Phase.gizmo | Add-Member -NotePropertyName latchCount -NotePropertyValue $LatchCount
        $Phase.gizmo | Add-Member -NotePropertyName noSelection -NotePropertyValue $NoSelection
    }
    if ($Case -in @("U03-gizmo-focus-no-release", "U03-gizmo-focus-same-frame", "U03-gizmo-focus-delivered-release")) {
        $Phase.gizmo | Add-Member -NotePropertyName detachCount -NotePropertyValue $DetachCount
        $Phase.gizmo | Add-Member -NotePropertyName latchCount -NotePropertyValue $LatchCount
        $Phase.gizmo | Add-Member -NotePropertyName noSelection -NotePropertyValue $NoSelection
        $Phase.gizmo | Add-Member -NotePropertyName deliveredReleaseCount -NotePropertyValue $DeliveredReleaseCount
        $Phase.gizmo | Add-Member -NotePropertyName escapePressCount -NotePropertyValue $EscapePressCount
        $Phase.gizmo | Add-Member -NotePropertyName focusLossCount -NotePropertyValue $FocusLossCount
        $Phase.gizmo | Add-Member -NotePropertyName suppressedReleaseCount -NotePropertyValue $SuppressedReleaseCount
    }
}

function New-ClickStep(
    [string]$Id,
    [string]$Subcase,
    [string]$Semantic,
    [string]$Role,
    [string]$GraphRowRole,
    [string]$Click,
    [string]$Effect = "presentation-only",
    [int]$RevisionDelta = 0,
    [string]$History = "unchanged",
    [bool]$OrderDigestChanges = $false) {
    $target = New-Target $Semantic $Role $GraphRowRole
    $clientTarget = New-Target "client" "client"
    $step = New-Step $Id $Subcase $Effect $RevisionDelta $History @(
        (New-Phase "target" "target" $target (New-Input "move-pointer") `
            $false $false $false "before"),
        (New-Phase "hover" "hover" $target (New-Input $Click) $true),
        (New-Phase "settled" "settled" $clientTarget (New-Input "none") `
            $false $false $false "after")
    ) $OrderDigestChanges
    if ($Semantic -eq "graph-row" -and $Click -eq "left-click") {
        $step.expectedDispatches = @(
            (New-Dispatch "scene-browser" "SelectObject" "select" `
                $GraphRowRole "" "" "selection-only"))
    }
    elseif ($Semantic -in @(
            "scene-add-node", "node-add-child-node", "node-duplicate")) {
        $operation = if ($Semantic -eq "scene-add-node") { "add-root" } `
            elseif ($Semantic -eq "node-add-child-node") { "add-child" } `
            else { "duplicate" }
        $step.expectedDispatches = @(
            (New-Dispatch "scene-browser" "HierarchyAction" $operation `
                $GraphRowRole "" "" "hierarchy"))
    }
    elseif ($Semantic -eq "delete-accept") {
        $step.expectedDispatches = @(
            (New-Dispatch "dialog" "HierarchyAction" "delete" `
                $GraphRowRole "" "" `
                $(if ($Effect -eq "document-noop-history-envelope") {
                    "none"
                } else { "hierarchy" })))
    }
    return $step
}

function New-ContextOpenStep(
    [string]$Id,
    [string]$Subcase,
    [string]$GraphRole,
    [string]$PopupSemantic,
    [string]$PopupRole) {
    $graphTarget = New-Target "graph-row" $GraphRole $GraphRole
    $popupTarget = New-Target $PopupSemantic $PopupRole $GraphRole
    return New-Step $Id $Subcase "presentation-only" 0 "unchanged" @(
        (New-Phase "target" "target" $graphTarget `
            (New-Input "move-pointer") $false $false $false "before"),
        (New-Phase "hover" "hover" $graphTarget `
            (New-Input "right-down") $true),
        (New-Phase "release" "hover" $graphTarget `
            (New-Input "right-up") $true),
        (New-Phase "settled" "settled" $popupTarget `
            (New-Input "none") $false $false $false "after")
    )
}

function New-ChordStep(
    [string]$Id,
    [string]$Subcase,
    [string]$Chord,
    [string]$Effect,
    [int]$RevisionDelta,
    [string]$History,
    [bool]$OrderDigestChanges = $false) {
    $client = New-Target "client" "client"
    $step = New-Step $Id $Subcase $Effect $RevisionDelta $History @(
        (New-Phase "focus" "focus" $client (New-Input "key-chord" $Chord) `
            $false $true $false "before"),
        (New-Phase "settled" "settled" $client (New-Input "none") `
            $false $false $false "after")
    ) $OrderDigestChanges
    $step.expectedDispatches = @(
        (New-Dispatch "shortcut" "InvokeCommand" `
            $(if ($Id -match "undo") { "undo" } else { "redo" }) `
            "" "" "" $(if ($RevisionDelta -eq 0) { "none" } `
                         else { "properties" })))
    return $step
}

function New-DragStep(
    [string]$Id,
    [string]$Subcase,
    [string]$SourceRole,
    [string]$TargetSemantic,
    [string]$TargetRole,
    [string]$TargetGraphRole,
    [string]$Effect,
    [int]$RevisionDelta,
    [string]$History) {
    $source = New-Target "node-drag-source" $SourceRole $SourceRole
    $destination = New-Target $TargetSemantic $TargetRole $TargetGraphRole
    $step = New-Step $Id $Subcase $Effect $RevisionDelta $History @(
        (New-Phase "source-target" "target" $source `
            (New-Input "move-pointer") $false $false $false "before"),
        (New-Phase "source-hover" "hover" $source `
            (New-Input "left-down") $true),
        (New-Phase "drag-target" "drag-target" $destination `
            (New-Input "move-pointer")),
        (New-Phase "drop-hover" "drop-hover" $destination `
            (New-Input "left-up") $true $false $true),
        (New-Phase "settled" "settled" $destination `
            (New-Input "none") $false $false $false "after")
    ) ($Effect -eq "document-mutation")
    $step.expectedDispatches = @(
        (New-Dispatch "scene-browser" "SelectObject" "select" `
            $SourceRole "" "" "selection-only"),
        (New-Dispatch "scene-browser" "ReparentNode" "reparent" `
            $SourceRole $TargetGraphRole "" `
            $(if ($Effect -eq "document-noop-history-envelope") { "none" } `
              else { "hierarchy" })))
    return $step
}

$ExpectedSteps = [Collections.Generic.List[object]]::new()
$client = New-Target "client" "client"
$ExpectedSteps.Add((New-Step "bootstrap" "main" "presentation-only" 0 "unchanged" @(
    (New-Phase "target" "target" $client (New-Input "none") `
        $false $false $false "before"),
    (New-Phase "boundary" "boundary" $client (New-Input "none") `
        $false $false $false "after")
)))
$ExpectedSteps.Add((New-ContextOpenStep "scene-context-open" "main" `
    "primary-scene" "scene-add-node" "menu:add-node"))
$ExpectedSteps.Add((New-ClickStep "scene-add-node" "main" `
    "scene-add-node" "menu:add-node" "primary-scene" "left-click" `
    "document-mutation" 1 "unchanged" $true))
$ExpectedSteps.Add((New-ContextOpenStep "node-context-add-child" "main" `
    "primary-node" "node-add-child-menu" "menu:add-child"))
$ExpectedSteps.Add((New-ClickStep "node-add-child-submenu" "main" `
    "node-add-child-menu" "menu:add-child" "primary-node" "none"))
$ExpectedSteps.Add((New-ClickStep "node-add-child-empty" "main" `
    "node-add-child-node" "menu:add-child-empty" "primary-node" `
    "left-click" "document-mutation" 1 "push-undo-clear-redo" $true))
$ExpectedSteps.Add((New-ContextOpenStep "node-context-rename" "main" `
    "primary-node" "node-rename" "menu:rename"))
$ExpectedSteps.Add((New-ClickStep "node-rename-menu" "main" `
    "node-rename" "menu:rename" "primary-node" "left-click"))

$renameInput = New-Target "rename-input" "rename:text" "primary-node"
$ExpectedSteps.Add((New-Step "rename-replace-text" "main" `
    "presentation-only" 0 "unchanged" @(
    (New-Phase "target" "target" $renameInput (New-Input "move-pointer") `
        $false $false $false "before"),
    (New-Phase "hover" "hover" $renameInput (New-Input "left-click") $true),
    (New-Phase "focus" "focus" $renameInput `
        (New-Input "replace-text" "OS Replay Hierarchy Node") `
        $false $true),
    (New-Phase "settled" "settled" $renameInput (New-Input "none") `
        $false $false $false "after")
)))
$renameSubmit = New-Step "rename-submit" "main" `
    "document-mutation" 1 "push-undo-clear-redo" @(
    (New-Phase "focus" "focus" $renameInput (New-Input "press-key" "Enter") `
        $false $true $false "before"),
    (New-Phase "settled" "settled" $client (New-Input "none") `
        $false $false $false "after")
)
$renameSubmit.expectedDispatches = @(
    (New-Dispatch "dialog" "RenameObject" "rename" "primary-node" "" `
        "OS Replay Hierarchy Node" "hierarchy"))
$ExpectedSteps.Add($renameSubmit)
$ExpectedSteps.Add((New-ContextOpenStep "node-context-duplicate" "main" `
    "primary-node" "node-duplicate" "menu:duplicate"))
$ExpectedSteps.Add((New-ClickStep "node-duplicate-menu" "main" `
    "node-duplicate" "menu:duplicate" "primary-node" "left-click" `
    "document-mutation" 1 "push-undo-clear-redo" $true))
$ExpectedSteps.Add((New-DragStep "cross-parent-drag" "main" `
    "cross-parent-source" "hierarchy-drop-target" "cross-parent-target" `
    "cross-parent-target" "document-mutation" 1 "push-undo-clear-redo"))
$ExpectedSteps.Add((New-ChordStep "undo-cross-parent" "main" "Ctrl+Z" `
    "history-move" 1 "move-undo-to-redo" $true))
$ExpectedSteps.Add((New-ChordStep "redo-cross-parent" "main" "Ctrl+Y" `
    "history-move" 1 "move-redo-to-undo" $true))
$ExpectedSteps.Add((New-DragStep "same-parent-drag-noop" "main" `
    "same-parent-child" "hierarchy-drop-target" "same-parent-parent" `
    "same-parent-parent" "document-noop-history-envelope" 0 `
    "push-undo-clear-redo"))
$ExpectedSteps.Add((New-DragStep "root-root-drag-noop" "main" `
    "root-noop-source" "hierarchy-drop-target" "primary-scene" "primary-scene" `
    "document-noop-history-envelope" 0 "push-undo-clear-redo"))
$ExpectedSteps.Add((New-ContextOpenStep "node-context-delete" "main" `
    "regular-delete-node" "node-delete" "menu:delete"))
$ExpectedSteps.Add((New-ClickStep "node-delete-menu" "main" `
    "node-delete" "menu:delete" "regular-delete-node" "left-click"))
$ExpectedSteps.Add((New-ClickStep "delete-confirm" "main" `
    "delete-accept" "delete:confirm" "regular-delete-node" "left-click" `
    "document-mutation" 1 "push-undo-clear-redo" $true))
$ExpectedSteps.Add((New-Step "main-process-boundary" "main" `
    "presentation-only" 0 "unchanged" @(
    (New-Phase "target" "target" $client (New-Input "none") `
        $false $false $false "before"),
    (New-Phase "boundary" "boundary" $client (New-Input "none") `
        $false $false $false "after")
)))
$uniqueRootSubcase = New-Step "unique-root-subcase" "unique-root" `
    "authority-reset" 0 "reset" @(
    (New-Phase "target" "target" $client (New-Input "none") `
        $false $false $false "before"),
    (New-Phase "settled" "settled" $client (New-Input "none") `
        $false $false $false "after")
)
$uniqueRootSubcase.relaunchProcessBefore = $true
$ExpectedSteps.Add($uniqueRootSubcase)
$ExpectedSteps.Add((New-ClickStep "unique-root-select" "unique-root" `
    "graph-row" "unique-root-node" "unique-root-node" "left-click" `
    "selection-only" 0 "unchanged"))
$ExpectedSteps.Add((New-ContextOpenStep "unique-root-context-delete" `
    "unique-root" "unique-root-node" "node-delete" "menu:delete"))
$ExpectedSteps.Add((New-ClickStep "unique-root-delete-menu" "unique-root" `
    "node-delete" "menu:delete" "unique-root-node" "left-click"))
$ExpectedSteps.Add((New-ClickStep "unique-root-delete-confirm" "unique-root" `
    "delete-accept" "delete:confirm" "unique-root-node" "left-click" `
    "document-noop-history-envelope" 0 "push-undo-clear-redo"))
$ExpectedSteps.Add((New-ChordStep "unique-root-undo" "unique-root" "Ctrl+Z" `
    "history-move" 0 "move-undo-to-redo"))
$ExpectedSteps.Add((New-ChordStep "unique-root-redo" "unique-root" "Ctrl+Y" `
    "history-move" 0 "move-redo-to-undo"))
$ExpectedSteps.Add((New-Step "final-boundary" "unique-root" `
    "presentation-only" 0 "unchanged" @(
    (New-Phase "target" "target" $client (New-Input "none") `
        $false $false $false "before"),
    (New-Phase "boundary" "boundary" $client (New-Input "none") `
        $false $false $false "after")
)))

if ($Case -eq "U03-transform-gizmo") {
    $GraphRoles = @(
        [pscustomobject][ordered]@{ role="primary-scene"; subcase="main"; objectKind="Scene"; labelContains="R09 Scene"; introducedByStep="bootstrap"; sameObjectAsRole=""; directParentRole=""; distinctFromRole=""; requireRoot=$false },
        [pscustomobject][ordered]@{ role="transform-node"; subcase="main"; objectKind="Node"; labelContains="Selected"; introducedByStep="bootstrap"; sameObjectAsRole=""; directParentRole=""; distinctFromRole=""; requireRoot=$true }
    )
    $ExpectedSteps.Clear()
    $viewport = New-Target "viewport" "viewport"
    $blank = New-Target "viewport" "viewport-blank"
    $translate = New-Target "gizmo-translate" "gizmo-translate"
    $rotate = New-Target "gizmo-rotate" "gizmo-rotate"
    $scale = New-Target "gizmo-scale" "gizmo-scale"
    $inspector = New-Target "inspector-translation-x" "inspector-translation-x" "transform-node"
    $toolbarGizmo = New-Target "toolbar-gizmo" "toolbar-gizmo"
    $toolbarGrid = New-Target "toolbar-grid" "toolbar-grid"
    $toolbarSnap = New-Target "toolbar-snap" "toolbar-snap"
    $addPassive = {
        param([string]$Id, [object]$Target, [string]$Effect = "presentation-only")
        $ExpectedSteps.Add((New-Step $Id "main" $Effect 0 "unchanged" @(
            (New-Phase "target" "target" $Target (New-Input "move-pointer") $false $false $false "before"),
            (New-Phase "hover" "hover" $Target (New-Input "left-click") $true),
            (New-Phase "settled" "settled" $client (New-Input "none") $false $false $false "after")
        )))
    }
    $ExpectedSteps.Add((New-Step "bootstrap" "main" "presentation-only" 0 "unchanged" @(
        (New-Phase "target" "target" $client (New-Input "none") $false $false $false "before"),
        (New-Phase "boundary" "boundary" $client (New-Input "none") $false $false $false "after")
    )))
    $selectNode = New-Step "select-transform-node" "main" "selection-only" 0 "unchanged" @(
        (New-Phase "target" "target" (New-Target "graph-row" "transform-node" "transform-node") (New-Input "move-pointer") $false $false $false "before"),
        (New-Phase "hover" "hover" (New-Target "graph-row" "transform-node" "transform-node") (New-Input "left-click") $true),
        (New-Phase "settled" "settled" $client (New-Input "none") $false $false $false "after")
    )
    $selectNode.expectedDispatches = @(
        (New-Dispatch "scene-browser" "SelectObject" "select" "transform-node" "" "" "selection-only")
    )
    $ExpectedSteps.Add($selectNode)
    & $addPassive "viewport-single-hit" $viewport "selection-only"
    & $addPassive "viewport-toggle" $viewport "selection-only"
    & $addPassive "viewport-blank" $blank "selection-only"
    & $addPassive "gizmo-translate-single-hit" $translate
    & $addPassive "gizmo-rotate-single-hit" $rotate
    & $addPassive "gizmo-scale-single-hit" $scale
    $trsStep = New-Step "inspector-translation-x" "main" "document-mutation" 1 "push-undo-clear-redo" @(
        (New-Phase "target" "target" $inspector (New-Input "move-pointer") $false $false $false "before"),
        (New-Phase "focus" "focus" $inspector (New-Input "left-down") $true $false),
        (New-Phase "preview" "focus" $inspector (New-Input "move-pointer" "80,0") $true $true),
        (New-Phase "commit" "focus" $inspector (New-Input "left-up") $false $false),
        (New-Phase "settled" "settled" $client (New-Input "none") $false $false $false "after")
    )
    $trsStep.expectedDispatches = @(
        (New-Dispatch "inspector" "PropertyEdit" "translation" "transform-node" "" "" "none" "begin" "0.0,0.0,0.0"),
        (New-Dispatch "inspector" "PropertyEdit" "translation" "transform-node" "" "" "properties" "preview" "0.8,0.0,0.0"),
        (New-Dispatch "inspector" "PropertyEdit" "translation" "transform-node" "" "" "none" "commit" "0.8,0.0,0.0")
    )
    $ExpectedSteps.Add($trsStep)
    & $addPassive "toolbar-gizmo-toggle" $toolbarGizmo
    & $addPassive "toolbar-grid-toggle" $toolbarGrid
    & $addPassive "toolbar-snap-toggle" $toolbarSnap
    $dragPhases = @(
        (New-Phase "target" "target" $translate (New-Input "move-pointer") $false $false $false "before"),
            (New-Phase "press" "drag-target" $translate (New-Input "left-down") $true),
        (New-Phase "preview" "drag-target" $translate (New-Input "move-pointer") $true),
            (New-Phase "outside" "drag-target" (New-Target "viewport" "viewport-outside") (New-Input "move-pointer")),
            (New-Phase "release" "drag-target" (New-Target "viewport" "viewport-outside") (New-Input "left-up")),
            (New-Phase "settled" "settled" $client (New-Input "none") $false $false $false "after")
    )
    $ExpectedSteps.Add((New-Step "gizmo-drag-outside-release" "main" "document-mutation" 1 "push-undo-clear-redo" $dragPhases))
    $ExpectedSteps.Add((New-Step "undo-transform" "main" "history-move" 0 "move-undo-to-redo" @(
        (New-Phase "target" "focus" $client (New-Input "press-key" "Ctrl+Z") $false $false $false "before"),
        (New-Phase "boundary" "boundary" $client (New-Input "none") $false $false $false "after")
    )))
    $ExpectedSteps[$ExpectedSteps.Count - 1].expectedRevisionDelta = 1
    $ExpectedSteps[$ExpectedSteps.Count - 1].expectedDispatches = @(
        (New-Dispatch "shortcut" "InvokeCommand" "undo" "" "" "" "properties"))
    $ExpectedSteps.Add((New-Step "redo-transform" "main" "history-move" 0 "move-redo-to-undo" @(
        (New-Phase "target" "focus" $client (New-Input "press-key" "Ctrl+Y") $false $false $false "before"),
        (New-Phase "boundary" "boundary" $client (New-Input "none") $false $false $false "after")
    )))
    $ExpectedSteps[$ExpectedSteps.Count - 1].expectedRevisionDelta = 1
    $ExpectedSteps[$ExpectedSteps.Count - 1].expectedDispatches = @(
        (New-Dispatch "shortcut" "InvokeCommand" "redo" "" "" "" "properties"))
    foreach ($escapeId in @("gizmo-escape-only", "gizmo-escape-release-same-frame")) {
        $tail = if ($escapeId -eq "gizmo-escape-only") { @(
            (New-Phase "target" "target" $translate (New-Input "move-pointer") $false $false $false "before"),
            (New-Phase "press" "drag-target" $translate (New-Input "left-down") $true),
            (New-Phase "preview" "drag-target" $translate (New-Input "move-pointer") $true),
            (New-Phase "outside" "drag-target" (New-Target "viewport" "viewport-outside") (New-Input "move-pointer")),
            (New-Phase "release" "drag-target" (New-Target "viewport" "viewport-outside") (New-Input "none")),
            (New-Phase "escape" "drag-target" $translate (New-Input "press-key" "Escape")),
            (New-Phase "settled" "settled" $client (New-Input "none") $false $false $false "after")
        ) } else { @(
            (New-Phase "target" "target" $translate (New-Input "move-pointer") $false $false $false "before"),
            (New-Phase "press" "drag-target" $translate (New-Input "left-down") $true),
            (New-Phase "preview" "drag-target" $translate (New-Input "move-pointer") $true),
            (New-Phase "outside" "drag-target" (New-Target "viewport" "viewport-outside") (New-Input "move-pointer")),
            (New-Phase "release" "drag-target" (New-Target "viewport" "viewport-outside") (New-Input "left-up")),
            (New-Phase "escape" "drag-target" $translate (New-Input "press-key" "Escape")),
            (New-Phase "settled" "settled" $client (New-Input "none") $false $false $false "after")
        ) }
        $ExpectedSteps.Add((New-Step $escapeId "main" "selection-only" 0 "unchanged" $tail))
    }
    foreach ($focusId in @("focus-loss-no-release", "focus-loss-with-release")) {
        $focusPhases = if ($focusId -eq "focus-loss-no-release") { @(
            (New-Phase "target" "target" $client (New-Input "none") $false $false $false "before"),
            (New-Phase "focus-loss" "boundary" $client (New-Input "none") $false $false $false),
            (New-Phase "late-release" "drag-target" $client (New-Input "left-up")),
        (New-Phase "settled" "settled" $client (New-Input "none") $false $false $false "after")
        ) } else { @(
            (New-Phase "target" "target" $client (New-Input "none") $false $false $false "before"),
            (New-Phase "release" "drag-target" $client (New-Input "left-up")),
            (New-Phase "focus-loss" "boundary" $client (New-Input "none") $false $false $false),
            (New-Phase "settled" "settled" $client (New-Input "none") $false $false $false "after")
        ) }
        $focusEffect = if ($focusId -eq "focus-loss-with-release") { "document-mutation" } else { "presentation-only" }
        $focusRevision = if ($focusId -eq "focus-loss-with-release") { 1 } else { 0 }
        $focusHistory = if ($focusId -eq "focus-loss-with-release") { "push-undo-clear-redo" } else { "unchanged" }
        $ExpectedSteps.Add((New-Step $focusId "main" $focusEffect $focusRevision $focusHistory $focusPhases))
    }
    $ExpectedSteps.Add((New-Step "final-boundary" "main" "presentation-only" 0 "unchanged" @(
            (New-Phase "target" "target" $client (New-Input "none") $false $false $false "before"),
        (New-Phase "boundary" "boundary" $client (New-Input "none") $false $false $false "after")
    )))
}

if ($Case -eq "U03-inspector-transform") {
    $ExpectedSteps.Clear()
    $ExpectedSteps.Add((New-Step "bootstrap" "main" "presentation-only" 0 "unchanged" @(
        (New-Phase "target" "target" $client (New-Input "none") $false $false $false "before" 0),
        (New-Phase "boundary" "boundary" $client (New-Input "none") $false $false $false "after" 0)
    )))
    $node = New-Target "graph-row" "transform-node" "transform-node"
    $selectNode = New-Step "select-transform-node" "main" "selection-only" 0 "unchanged" @(
        (New-Phase "target" "target" $node (New-Input "move-pointer") $false $false $false "before" 0),
        (New-Phase "hover" "hover" $node (New-Input "left-click") $true $false $false "none" 0),
        (New-Phase "settled" "settled" $client (New-Input "none") $false $false $false "after" 1)
    )
    $selectNode.expectedDispatches = @(
        (New-Dispatch "scene-browser" "SelectObject" "select" "transform-node" "" "" "selection-only")
    )
    $ExpectedSteps.Add($selectNode)
    foreach ($field in @(
        [pscustomobject]@{ name = "translation"; before = "0.0,0.0,0.0"; after = "0.8,0.0,0.0" },
        [pscustomobject]@{ name = "rotation"; before = "0.0,0.0,0.0"; after = "8.0,0.0,0.0" },
        [pscustomobject]@{ name = "scale"; before = "1.0,1.0,1.0"; after = "1.8,1.0,1.0" }
    )) {
        $id = "inspector-$($field.name)-x"
        $target = New-Target $id $id "transform-node"
        $trsStep = New-Step $id "main" "document-mutation" 1 "push-undo-clear-redo" @(
            (New-Phase "target" "target" $target (New-Input "move-pointer") $false $false $false "before" 0),
            (New-Phase "focus" "focus" $target (New-Input "left-down") $true $false $false "none" 0),
            (New-Phase "preview" "focus" $target (New-Input "move-pointer" "80,0") $true $true $false "none" 1),
            (New-Phase "commit" "focus" $target (New-Input "left-up") $false $false $false "none" 2),
            (New-Phase "settled" "settled" $client (New-Input "none") $false $false $false "after" 3)
        )
        $trsStep.expectedDispatches = @(
            (New-Dispatch "inspector" "PropertyEdit" $field.name "transform-node" "" "" "none" "begin" $field.before),
            (New-Dispatch "inspector" "PropertyEdit" $field.name "transform-node" "" "" "properties" "preview" $field.after),
            (New-Dispatch "inspector" "PropertyEdit" $field.name "transform-node" "" "" "none" "commit" $field.after)
        )
        $ExpectedSteps.Add($trsStep)
    }
    foreach ($history in @(
        [pscustomobject]@{ operation = "undo"; chord = "Ctrl+Z"; effect = "move-undo-to-redo" },
        [pscustomobject]@{ operation = "redo"; chord = "Ctrl+Y"; effect = "move-redo-to-undo" }
    )) {
        $step = New-Step "$($history.operation)-transform" "main" "history-move" 1 $history.effect @(
            (New-Phase "target" "focus" $client (New-Input "key-chord" $history.chord) $false $true $false "before" 0),
            (New-Phase "boundary" "boundary" $client (New-Input "none") $false $false $false "after" 1)
        )
        $step.expectedDispatches = @(
            (New-Dispatch "shortcut" "InvokeCommand" $history.operation "" "" "" "properties")
        )
        $ExpectedSteps.Add($step)
    }
    $ExpectedSteps.Add((New-Step "final-boundary" "main" "presentation-only" 0 "unchanged" @(
        (New-Phase "target" "target" $client (New-Input "none") $false $false $false "before" 0),
        (New-Phase "boundary" "boundary" $client (New-Input "none") $false $false $false "after" 0)
    )))
}

if ($Case -eq "U03-gizmo-translate") {
    $ExpectedSteps.Clear()
    $ExpectedSteps.Add((New-Step "bootstrap" "main" "presentation-only" 0 "unchanged" @(
        (New-Phase "target" "target" $client (New-Input "none") $false $false $false "before" 0),
        (New-Phase "boundary" "boundary" $client (New-Input "none") $false $false $false "after" 0)
    )))
    $node = New-Target "graph-row" "transform-node" "transform-node"
    $selectNode = New-Step "select-transform-node" "main" "selection-only" 0 "unchanged" @(
        (New-Phase "target" "target" $node (New-Input "move-pointer") $false $false $false "before" 0),
        (New-Phase "hover" "hover" $node (New-Input "left-click") $true $false $false "none" 0),
        (New-Phase "settled" "settled" $client (New-Input "none") $false $false $false "after" 1)
    )
    $selectNode.expectedDispatches = @(
        (New-Dispatch "scene-browser" "SelectObject" "select" "transform-node" "" "" "selection-only")
    )
    $ExpectedSteps.Add($selectNode)
    $gizmo = New-Target "gizmo-translate-y" "gizmo-translate-y" "transform-node"
    $outside = New-Target "viewport-outside" "viewport-outside" "transform-node"
    $ExpectedSteps.Add((New-Step "drag-outside-release" "main" "document-mutation" 1 "push-undo-clear-redo" @(
        (New-Phase "target" "target" $gizmo (New-Input "move-pointer") $false $false $false "before" 0),
        (New-Phase "hover" "hover" $gizmo (New-Input "left-down") $true $false $false "none" 0),
        (New-Phase "outside" "drag-target" $outside (New-Input "move-pointer") $false $false $false "none" 0),
        (New-Phase "release" "drag-target" $client (New-Input "left-up") $false $false $false "none" 0),
        (New-Phase "settled" "settled" $client (New-Input "none") $false $false $false "after" 0)
    )))
    foreach ($history in @(
        [pscustomobject]@{ operation = "undo"; chord = "Ctrl+Z"; effect = "move-undo-to-redo" },
        [pscustomobject]@{ operation = "redo"; chord = "Ctrl+Y"; effect = "move-redo-to-undo" }
    )) {
        $step = New-Step "$($history.operation)-transform" "main" "history-move" 1 $history.effect @(
            (New-Phase "target" "focus" $client (New-Input "key-chord" $history.chord) $false $true $false "before" 0),
            (New-Phase "boundary" "boundary" $client (New-Input "none") $false $false $false "after" 1)
        )
        $step.expectedDispatches = @(
            (New-Dispatch "shortcut" "InvokeCommand" $history.operation "" "" "" "properties")
        )
        $ExpectedSteps.Add($step)
    }
    $ExpectedSteps.Add((New-Step "final-boundary" "main" "presentation-only" 0 "unchanged" @(
        (New-Phase "target" "target" $client (New-Input "none") $false $false $false "before" 0),
        (New-Phase "boundary" "boundary" $client (New-Input "none") $false $false $false "after" 0)
    )))
    Set-GizmoExpectation $ExpectedSteps[1].phases[2] 1 $false 0 0 0
    Set-GizmoExpectation $ExpectedSteps[2].phases[0] 1 $false 0 0 0
    Set-GizmoExpectation $ExpectedSteps[2].phases[1] 1 $false 0 0 0
    Set-GizmoExpectation $ExpectedSteps[2].phases[2] 2 $true 1 0 0 $false
    Set-GizmoExpectation $ExpectedSteps[2].phases[3] 2 $true 1 1 0 $true
    Set-GizmoExpectation $ExpectedSteps[2].phases[4] 1 $false 1 1 1 $true
}

if ($Case -in @("U03-gizmo-escape", "U03-gizmo-escape-release", "U03-gizmo-release-escape")) {
    $ExpectedSteps.Clear()
    $ExpectedSteps.Add((New-Step "bootstrap" "main" "presentation-only" 0 "unchanged" @(
        (New-Phase "target" "target" $client (New-Input "none") $false $false $false "before" 0),
        (New-Phase "boundary" "boundary" $client (New-Input "none") $false $false $false "after" 0)
    )))
    $node = New-Target "graph-row" "transform-node" "transform-node"
    $selectNode = New-Step "select-transform-node" "main" "selection-only" 0 "unchanged" @(
        (New-Phase "target" "target" $node (New-Input "move-pointer") $false $false $false "before" 0),
        (New-Phase "hover" "hover" $node (New-Input "left-click") $true $false $false "none" 0),
        (New-Phase "settled" "settled" $client (New-Input "none") $false $false $false "after" 1)
    )
    $selectNode.expectedDispatches = @(
        (New-Dispatch "scene-browser" "SelectObject" "select" "transform-node" "" "" "selection-only")
    )
    $ExpectedSteps.Add($selectNode)
    $gizmo = New-Target "gizmo-translate-y" "gizmo-translate-y" "transform-node"
    $outside = New-Target "viewport-outside" "viewport-outside" "transform-node"
    $ExpectedSteps.Add((New-Step "drag-escape-only" "main" "document-mutation" 1 "unchanged" @(
        (New-Phase "target" "target" $gizmo (New-Input "move-pointer") $false $false $false "before" 0),
        (New-Phase "hover" "hover" $gizmo (New-Input "left-down") $true $false $false "none" 0),
        (New-Phase "outside" "drag-target" $outside (New-Input "move-pointer") $false $false $false "none" 0),
        (New-Phase "escape" "drag-target" $client (New-Input "key-down" "Escape") $false $false $false "none" 0),
        (New-Phase "settled" "settled" $client (New-Input "none") $false $false $false "after" 0)
    )))
    $ExpectedSteps.Add((New-Step "late-release" "main" "presentation-only" 0 "unchanged" @(
        (New-Phase "target" "target" $client (New-Input "key-up" "Escape") $false $false $false "before" 0),
        (New-Phase "release" "drag-target" $client (New-Input "left-up") $false $false $false "none" 0),
        (New-Phase "settled" "settled" $client (New-Input "none") $false $false $false "after" 0)
    )))
    $ExpectedSteps.Add((New-Step "final-boundary" "main" "presentation-only" 0 "unchanged" @(
        (New-Phase "target" "target" $client (New-Input "none") $false $false $false "before" 0),
        (New-Phase "boundary" "boundary" $client (New-Input "none") $false $false $false "after" 0)
    )))
    Set-GizmoExpectation $ExpectedSteps[1].phases[2] 1 $false 0 0 0
    Set-GizmoExpectation $ExpectedSteps[2].phases[0] 1 $false 0 0 0
    Set-GizmoExpectation $ExpectedSteps[2].phases[1] 1 $false 0 0 0
    Set-GizmoExpectation $ExpectedSteps[2].phases[2] 2 $true 1 0 0 $false
    Set-GizmoExpectation $ExpectedSteps[2].phases[3] 2 $true 1 1 0 $true
    Set-GizmoExpectation $ExpectedSteps[2].phases[4] 0 $false 1 1 0 $true 1 0 $true
    foreach ($stepIndex in @(3, 4)) {
        foreach ($phase in $ExpectedSteps[$stepIndex].phases) {
            Set-GizmoExpectation $phase 0 $false 1 1 0 $true 1 0 $true
        }
    }
    if ($Case -ne "U03-gizmo-escape") {
        $ExpectedSteps[2].id = "drag-same-frame"
        $ExpectedSteps[2].phases[3].id = "terminal-batch"
        $order = if ($Case -eq "U03-gizmo-escape-release") { "escape-first" } else { "release-first" }
        $ExpectedSteps[2].phases[3].input = New-Input "escape-release-batch" $order
    }
}

if ($Case -in @("U03-gizmo-focus-no-release", "U03-gizmo-focus-same-frame", "U03-gizmo-focus-delivered-release")) {
    $ExpectedSteps.Clear()
    $ExpectedSteps.Add((New-Step "bootstrap" "main" "presentation-only" 0 "unchanged" @(
        (New-Phase "target" "target" $client (New-Input "none") $false $false $false "before" 0),
        (New-Phase "boundary" "boundary" $client (New-Input "none") $false $false $false "after" 0)
    )))
    $node = New-Target "graph-row" "transform-node" "transform-node"
    $selectNode = New-Step "select-transform-node" "main" "selection-only" 0 "unchanged" @(
        (New-Phase "target" "target" $node (New-Input "move-pointer") $false $false $false "before" 0),
        (New-Phase "hover" "hover" $node (New-Input "left-click") $true $false $false "none" 0),
        (New-Phase "settled" "settled" $client (New-Input "none") $false $false $false "after" 1)
    )
    $selectNode.expectedDispatches = @(
        (New-Dispatch "scene-browser" "SelectObject" "select" "transform-node" "" "" "selection-only")
    )
    Set-GizmoExpectation $selectNode.phases[2] 1 $false 0 0 0 $null 0 0 $false 0 0 0 0
    $ExpectedSteps.Add($selectNode)

    $component = New-Target "gizmo-translate-y" "gizmo-translate-y" "transform-node"
    $outside = New-Target "viewport-outside" "viewport-outside" "transform-node"
    $deliveredRelease = $Case -eq "U03-gizmo-focus-delivered-release"
    $sameFrameRelease = $Case -eq "U03-gizmo-focus-same-frame"
    $dragId = if ($deliveredRelease) { "drag-focus-delivered-release" }
        elseif ($sameFrameRelease) { "drag-focus-same-frame" }
        else { "drag-focus-no-release" }
    $dragPhases = [Collections.Generic.List[object]]::new()
    $dragPhases.Add((New-Phase "target" "target" $component (New-Input "move-pointer") $false $false $false "before" 0))
    $dragPhases.Add((New-Phase "hover" "hover" $component (New-Input "left-down") $true $false $false "none" 0))
    $dragPhases.Add((New-Phase "outside" "drag-target" $outside (New-Input "move-pointer") $false $false $false "none" 0))
    if ($deliveredRelease) {
        $dragPhases.Add((New-Phase "release" "drag-target" $client (New-Input "left-up") $false $false $false "none" 0))
        $dragPhases.Add((New-Phase "focus-loss" "drag-target" $client (New-Input "focus-loss" "sink") $false $false $false "none" 0))
        $dragPhases.Add((New-Phase "focus-restore" "drag-target" $client (New-Input "focus-restore" "editor") $false $false $false "none" 0))
    }
    elseif ($sameFrameRelease) {
        $dragPhases.Add((New-Phase "focus-loss" "drag-target" $client (New-Input "left-up-focus-loss" "release-then-focus-loss") $false $false $false "none" 0))
        $dragPhases.Add((New-Phase "focus-restore" "drag-target" $client (New-Input "focus-restore" "editor") $false $false $false "none" 0))
        $dragPhases.Add((New-Phase "settle-focus" "drag-target" $client (New-Input "none") $false $false $false "none" 0))
    }
    else {
        $dragPhases.Add((New-Phase "focus-loss" "drag-target" $client (New-Input "focus-loss" "sink") $false $false $false "none" 0))
        $dragPhases.Add((New-Phase "focus-restore" "drag-target" $client (New-Input "focus-restore" "editor") $false $false $false "none" 0))
        $dragPhases.Add((New-Phase "late-release" "drag-target" $client (New-Input "left-up") $false $false $false "none" 0))
    }
    $dragPhases.Add((New-Phase "settled" "settled" $client (New-Input "none") $false $false $false "after" 0))
    $drag = New-Step $dragId "main" "document-mutation" 1 `
        $(if ($deliveredRelease) { "push-undo-clear-redo" } else { "unchanged" }) `
        @($dragPhases.ToArray())
    # Keep the expectation fields in the same order and shape as the native plan.
    Set-GizmoExpectation $drag.phases[0] 1 $false 0 0 0 $null 0 0 $false 0 0 0 0
    Set-GizmoExpectation $drag.phases[1] 1 $false 0 0 0 $null 0 0 $false 0 0 0 0
    Set-GizmoExpectation $drag.phases[2] 2 $true 1 0 0 $false 0 0 $false 0 0 0 0
    Set-GizmoExpectation $drag.phases[3] 2 $true 1 1 0 $true 0 0 $false 0 0 0 0
    if ($deliveredRelease) {
        foreach ($phaseIndex in 4..6) {
            # The focus-loss ready (phase 5) is published while the editor is
            # unfocused, so ImGui exposes no usable pointer until restore.
            $pointerOutside = if ($phaseIndex -eq 5) { $null } else { $true }
            Set-GizmoExpectation $drag.phases[$phaseIndex] 1 $false 1 1 1 $pointerOutside 0 0 $false 1 0 0 0
        }
    }
    else {
        foreach ($phaseIndex in 4..6) {
            # A Win32 focus-loss callback has no mouse-up callback.  The first
            # suppressed polling release is visible only after focus restore,
            # so the no-release branch has a zero-suppressed prefix at phase 4.
            $suppressed = if (-not $sameFrameRelease -and $phaseIndex -eq 4) { 0 }
                elseif (-not $sameFrameRelease -and $phaseIndex -ge 6) { 2 }
                else { 1 }
            $pointerOutside = if ($phaseIndex -eq 4) { $null } else { $true }
            Set-GizmoExpectation $drag.phases[$phaseIndex] 3 $false 1 1 0 $pointerOutside 0 1 $false 0 0 1 $suppressed
        }
    }
    $ExpectedSteps.Add($drag)
    $boundary = New-Step "final-boundary" "main" "presentation-only" 0 "unchanged" @(
        (New-Phase "target" "target" $client (New-Input "none") $false $false $false "before" 0),
        (New-Phase "boundary" "boundary" $client (New-Input "none") $false $false $false "after" 0)
    )
    if ($deliveredRelease) {
        Set-GizmoExpectation $boundary.phases[0] 1 $false 1 1 1 $true 0 0 $false 1 0 0 0
        Set-GizmoExpectation $boundary.phases[1] 1 $false 1 1 1 $true 0 0 $false 1 0 0 0
    }
    else {
        $suppressed = if (-not $sameFrameRelease) { 2 } else { 1 }
        Set-GizmoExpectation $boundary.phases[0] 3 $false 1 1 0 $true 0 1 $false 0 0 1 $suppressed
        Set-GizmoExpectation $boundary.phases[1] 3 $false 1 1 0 $true 0 1 $false 0 0 1 $suppressed
    }
    $ExpectedSteps.Add($boundary)
}

$GraphRoles = @(
    [pscustomobject][ordered]@{
        role = "primary-scene"; subcase = "main"; objectKind = "Scene"
        labelContains = "R09 Scene"; introducedByStep = "scene-context-open"
        sameObjectAsRole = ""; directParentRole = ""; distinctFromRole = ""
        requireRoot = $false
    },
    [pscustomobject][ordered]@{
        role = "primary-node"; subcase = "main"; objectKind = "Node"
        labelContains = "Node"; introducedByStep = "scene-add-node"
        sameObjectAsRole = ""; directParentRole = ""; distinctFromRole = ""
        requireRoot = $true
    },
    [pscustomobject][ordered]@{
        role = "cross-parent-source"; subcase = "main"; objectKind = "Node"
        labelContains = "Selected"; introducedByStep = "bootstrap"
        sameObjectAsRole = ""; directParentRole = ""
        distinctFromRole = ""; requireRoot = $true
    },
    [pscustomobject][ordered]@{
        role = "cross-parent-target"; subcase = "main"; objectKind = "Node"
        labelContains = "Shared Peer"; introducedByStep = "bootstrap"
        sameObjectAsRole = ""; directParentRole = ""
        distinctFromRole = "cross-parent-source"; requireRoot = $true
    },
    [pscustomobject][ordered]@{
        role = "same-parent-child"; subcase = "main"; objectKind = "Node"
        labelContains = "Node"; introducedByStep = "node-add-child-empty"
        sameObjectAsRole = ""
        directParentRole = "primary-node"; distinctFromRole = ""
        requireRoot = $false
    },
    [pscustomobject][ordered]@{
        role = "same-parent-parent"; subcase = "main"; objectKind = "Node"
        labelContains = ""; introducedByStep = "scene-add-node"
        sameObjectAsRole = "primary-node"; directParentRole = ""
        distinctFromRole = ""; requireRoot = $true
    },
    [pscustomobject][ordered]@{
        role = "root-noop-source"; subcase = "main"; objectKind = "Node"
        labelContains = "No Material"; introducedByStep = "bootstrap"
        sameObjectAsRole = ""; directParentRole = ""
        distinctFromRole = "cross-parent-source"; requireRoot = $true
    },
    [pscustomobject][ordered]@{
        role = "regular-delete-node"; subcase = "main"; objectKind = "Node"
        labelContains = "OS Replay Hierarchy Node"
        introducedByStep = "node-duplicate-menu"; sameObjectAsRole = ""
        directParentRole = ""; distinctFromRole = "primary-node"
        requireRoot = $true
    },
    [pscustomobject][ordered]@{
        role = "unique-root-node"; subcase = "unique-root"; objectKind = "Node"
        labelContains = "Root"; introducedByStep = "unique-root-select"
        sameObjectAsRole = ""; directParentRole = ""; distinctFromRole = ""
        requireRoot = $true
    }
)

if ($Case -ne "U02-hierarchy") {
    $GraphRoles = @(
        [pscustomobject][ordered]@{ role="primary-scene"; subcase="main"; objectKind="Scene"; labelContains="R09 Scene"; introducedByStep="bootstrap"; sameObjectAsRole=""; directParentRole=""; distinctFromRole=""; requireRoot=$false },
        [pscustomobject][ordered]@{ role="transform-node"; subcase="main"; objectKind="Node"; labelContains="Selected"; introducedByStep="bootstrap"; sameObjectAsRole=""; directParentRole=""; distinctFromRole=""; requireRoot=$true }
    )
}

$ordinal = 0
for ($stepIndex = 0; $stepIndex -lt @($ExpectedSteps).Count; ++$stepIndex) {
    $ExpectedSteps[$stepIndex].index = $stepIndex
    foreach ($phase in @($ExpectedSteps[$stepIndex].phases)) {
        $phase.ordinal = $ordinal
        ++$ordinal
    }
}
$ExpectedPhaseCount = $ordinal

function Test-ExpectedPlan {
    $requiredStepCount = switch ($Case) { "U02-hierarchy" { 29 }; "U03-inspector-transform" { 8 }; "U03-gizmo-translate" { 6 }; { $_ -in @("U03-gizmo-escape", "U03-gizmo-escape-release", "U03-gizmo-release-escape") } { 5 }; { $_ -in @("U03-gizmo-focus-no-release", "U03-gizmo-focus-same-frame", "U03-gizmo-focus-delivered-release") } { 4 }; default { 20 } }
    $requiredPhaseCount = switch ($Case) { "U02-hierarchy" { 91 }; "U03-inspector-transform" { 26 }; "U03-gizmo-translate" { 16 }; { $_ -in @("U03-gizmo-escape", "U03-gizmo-escape-release", "U03-gizmo-release-escape") } { 15 }; { $_ -in @("U03-gizmo-focus-no-release", "U03-gizmo-focus-same-frame", "U03-gizmo-focus-delivered-release") } { 14 }; default { 71 } }
    if (@($ExpectedSteps).Count -ne $requiredStepCount -or $ExpectedPhaseCount -ne $requiredPhaseCount) {
        throw ("OS replay plan cardinality changed without a protocol revision: " +
               "steps=$(@($ExpectedSteps).Count) phases=$ExpectedPhaseCount")
    }
    $ids = [Collections.Generic.HashSet[string]]::new(
        [StringComparer]::Ordinal)
    $roleNames = [Collections.Generic.HashSet[string]]::new(
        [StringComparer]::Ordinal)
    foreach ($role in $GraphRoles) {
        if (-not $roleNames.Add([string]$role.role) -or
            [string]$role.objectKind -notmatch "^(Scene|Node)$") {
            throw "Graph role identity/kind is invalid"
        }
    }
    $nextOrdinal = 0
    $splitContextSteps = 0
    $plannedHeldKeys = [Collections.Generic.HashSet[string]]::new([StringComparer]::Ordinal)
    foreach ($step in $ExpectedSteps) {
        if ([int]$step.index -lt 0 -or
            -not $ids.Add([string]$step.id) -or
            @($step.phases).Count -lt 2 -or
            [string]$step.phases[0].capture -ne "before" -or
            [string]$step.phases[-1].capture -ne "after") {
            throw "OS replay step identity/pre-post capture contract is invalid"
        }
        if (([string]$step.effect -eq "authority-reset") -ne
            [bool]$step.relaunchProcessBefore) {
            throw "Authority reset is not bound to a fresh process relaunch"
        }
        if ([bool]$step.relaunchProcessBefore -and
            ([int]$step.index -eq 0 -or
             [string]$ExpectedSteps[[int]$step.index - 1].id -ne
                "main-process-boundary")) {
            throw "Fresh process relaunch is missing its preceding boundary"
        }
        if ($Case -eq "U02-hierarchy" -and @($step.expectedDispatches).Count -gt 2) {
            throw "OS replay step has excess expected dispatches"
        }
        $inputKinds = @($step.phases | ForEach-Object {
            [string]$_.input.kind
        })
        $rightDownCount = @($inputKinds | Where-Object {
            $_ -eq "right-down"
        }).Count
        $rightUpCount = @($inputKinds | Where-Object {
            $_ -eq "right-up"
        }).Count
        if ($inputKinds -contains "right-click" -or
            $rightDownCount -ne $rightUpCount) {
            throw "Context input is batched or has unmatched right-button edges"
        }
        if ($rightDownCount -ne 0) {
            if (@($step.phases).Count -ne 4 -or
                [string]$step.phases[1].id -ne "hover" -or
                [string]$step.phases[1].input.kind -ne "right-down" -or
                [string]$step.phases[2].id -ne "release" -or
                [string]$step.phases[2].input.kind -ne "right-up" -or
                [string]$step.phases[-1].target.semantic -eq "client") {
                throw "Context step does not terminate on its popup target"
            }
            ++$splitContextSteps
        }
        foreach ($dispatch in @($step.expectedDispatches)) {
            if ([string]::IsNullOrEmpty([string]$dispatch.source) -or
                [string]::IsNullOrEmpty([string]$dispatch.payloadKind) -or
                [string]::IsNullOrEmpty([string]$dispatch.operation) -or
                -not [bool]$dispatch.accepted -or
                [string]::IsNullOrEmpty([string]$dispatch.refresh)) {
                throw "OS replay expected dispatch summary is incomplete"
            }
        }
        foreach ($phase in @($step.phases)) {
            if ([int]$phase.ordinal -ne $nextOrdinal++) {
                throw "OS replay phase ordinals are not contiguous"
            }
            if ($null -eq $phase.input -or
                $null -eq $phase.input.PSObject.Properties["kind"]) {
                throw ("OS replay phase input is malformed: " +
                       "$($step.id)/$($phase.id) value=$($phase.input)")
            }
            if ($null -ne $phase.PSObject.Properties["expectedDispatchCount"] -and
                ($Case -eq "U02-hierarchy" -or
                 [int]$phase.expectedDispatchCount -lt 0 -or
                 [int]$phase.expectedDispatchCount -gt @($step.expectedDispatches).Count)) {
                throw "OS replay explicit dispatch prefix is invalid"
            }
            if ($null -ne $phase.PSObject.Properties["gizmo"] -and
                ($Case -notin @("U03-gizmo-translate", "U03-gizmo-escape", "U03-gizmo-escape-release", "U03-gizmo-release-escape", "U03-gizmo-focus-no-release", "U03-gizmo-focus-same-frame", "U03-gizmo-focus-delivered-release") -or
                 [int]$phase.gizmo.phase -notin @(0, 1, 2, 3) -or
                 [int]$phase.gizmo.beginCount -lt 0 -or
                 [int]$phase.gizmo.previewCount -lt 0 -or
                 [int]$phase.gizmo.commitCount -lt 0)) {
                throw "OS replay native gizmo expectation is invalid"
            }
            if ($Case -in @("U03-gizmo-focus-no-release", "U03-gizmo-focus-same-frame", "U03-gizmo-focus-delivered-release") -and
                $null -ne $phase.PSObject.Properties["gizmo"] -and
                ([bool]$phase.gizmo.noSelection -or
                 [int]$phase.gizmo.detachCount -ne 0 -or
                 [int]$phase.gizmo.deliveredReleaseCount -lt 0 -or
                 [int]$phase.gizmo.escapePressCount -ne 0 -or
                 [int]$phase.gizmo.focusLossCount -lt 0 -or
                 [int]$phase.gizmo.suppressedReleaseCount -lt 0 -or
                 [int]$phase.gizmo.deliveredReleaseCount -gt 1 -or
                 [int]$phase.gizmo.focusLossCount -gt 1 -or
                 [int]$phase.gizmo.suppressedReleaseCount -gt 2 -or
                 [int]$phase.gizmo.commitCount -gt 1 -or
                 [int]$phase.gizmo.latchCount -gt 1 -or
                 ([int]$phase.gizmo.deliveredReleaseCount -gt 0 -and
                  ([int]$phase.gizmo.commitCount -eq 0 -or
                   [int]$phase.gizmo.latchCount -ne 0 -or
                   [int]$phase.gizmo.focusLossCount -ne 0 -or
                   [int]$phase.gizmo.suppressedReleaseCount -ne 0)) -or
                 ([int]$phase.gizmo.commitCount -gt 0 -and
                  [int]$phase.gizmo.deliveredReleaseCount -eq 0) -or
                 ([int]$phase.gizmo.focusLossCount -gt 0 -and
                  [int]$phase.gizmo.latchCount -ne 1) -or
                 ([int]$phase.gizmo.latchCount -gt 0 -and
                  ([int]$phase.gizmo.commitCount -gt 0 -or
                   [int]$phase.gizmo.deliveredReleaseCount -gt 0)) -or
                 ([int]$phase.gizmo.suppressedReleaseCount -gt 0 -and
                  ([int]$phase.gizmo.focusLossCount -eq 0 -or
                   [int]$phase.gizmo.latchCount -eq 0)) -or
                 ($Case -eq "U03-gizmo-focus-delivered-release" -and
                  ([int]$phase.gizmo.focusLossCount -ne 0 -or
                   [int]$phase.gizmo.suppressedReleaseCount -ne 0 -or
                   [int]$phase.gizmo.latchCount -ne 0)) -or
                 ($Case -ne "U03-gizmo-focus-delivered-release" -and
                  ([int]$phase.gizmo.deliveredReleaseCount -ne 0 -or
                   [int]$phase.gizmo.commitCount -ne 0)))) {
                throw "OS replay focus-loss terminal counter expectation is invalid"
            }
            if ([string]$phase.input.kind -in @("key-down", "key-up")) {
                if ($Case -notin @("U03-gizmo-escape", "U03-gizmo-escape-release", "U03-gizmo-release-escape") -or
                    [string]$phase.input.value -ne "Escape") {
                    throw "Held-key edge is outside the scoped Escape replay contract"
                }
                $balanced = if ([string]$phase.input.kind -eq "key-down") {
                    $plannedHeldKeys.Add([string]$phase.input.value)
                } else { $plannedHeldKeys.Remove([string]$phase.input.value) }
                if (-not $balanced) { throw "OS replay plan has unmatched or repeated held-key edges" }
            }
            if ([string]$phase.input.kind -eq "escape-release-batch") {
                $order = if ($Case -eq "U03-gizmo-escape-release") { "escape-first" }
                    elseif ($Case -eq "U03-gizmo-release-escape") { "release-first" } else { "" }
                if ($order -eq "" -or [string]$phase.input.value -ne $order -or
                    -not $plannedHeldKeys.Add("Escape")) {
                    throw "OS replay same-frame batch order or held-key contract is invalid"
                }
            }
            if ([string]$phase.input.kind -eq "focus-loss" -and
                ($Case -notin @("U03-gizmo-focus-no-release", "U03-gizmo-focus-same-frame", "U03-gizmo-focus-delivered-release") -or
                 [string]$phase.input.value -ne "sink")) {
                throw "OS replay focus-loss input is outside its scoped case"
            }
            if ([string]$phase.input.kind -eq "focus-restore" -and
                ($Case -notin @("U03-gizmo-focus-no-release", "U03-gizmo-focus-same-frame", "U03-gizmo-focus-delivered-release") -or
                 [string]$phase.input.value -ne "editor")) {
                throw "OS replay focus-restore input is outside its scoped case"
            }
            if ([string]$phase.input.kind -eq "left-up-focus-loss" -and
                ($Case -notin @("U03-gizmo-focus-no-release", "U03-gizmo-focus-same-frame", "U03-gizmo-focus-delivered-release") -or
                 [string]$phase.input.value -ne "release-then-focus-loss")) {
                throw "OS replay combined focus-loss input is outside its scoped case"
            }
            if ([string]$phase.input.kind -eq "move-pointer" -and
                [string]$phase.input.value -ne "" -and
                [string]$phase.input.value -notmatch '^(-?[0-9]+),(-?[0-9]+)$') {
                throw "OS replay pointer offset is not explicit signed x,y"
            }
            if ([string]$phase.target.semantic -eq "client") {
                if ([string]$phase.target.graphRowRole -ne "") {
                    throw "Client phase unexpectedly requires GraphRowKey"
                }
            }
            elseif ($Case -eq "U02-hierarchy" -and [string]$phase.target.graphRowRole -eq "") {
                throw "UI phase is not anchored to a GraphRowKey role"
            }
            elseif ([string]$phase.target.graphRowRole -ne "") {
                $contract = @($GraphRoles | Where-Object {
                    [string]$_.role -eq [string]$phase.target.graphRowRole
                })
                if ($contract.Count -ne 1 -or
                    [string]$contract[0].subcase -ne [string]$step.subcase) {
                    throw "Phase GraphRowKey role is outside its subcase"
                }
            }
        }
    }
    if ($Case -eq "U02-hierarchy" -and $splitContextSteps -ne 6) {
        throw "OS replay plan does not contain six split context steps"
    }
    if ($plannedHeldKeys.Count -ne 0) { throw "OS replay plan leaves a keyboard key held" }
    foreach ($role in $GraphRoles) {
        if (-not $ids.Contains([string]$role.introducedByStep)) {
            throw "Graph role introduction step is unknown: $($role.role)"
        }
        foreach ($reference in @(
            [string]$role.sameObjectAsRole,
            [string]$role.directParentRole,
            [string]$role.distinctFromRole)) {
            if ($reference -ne "" -and -not $roleNames.Contains($reference)) {
                throw "Graph role references an unknown alias: $reference"
            }
        }
    }
    $requiredIds = if ($Case -eq "U02-hierarchy") { @(
        "scene-context-open", "scene-add-node", "node-context-add-child",
        "node-add-child-empty", "node-context-rename",
        "rename-replace-text", "rename-submit", "node-context-duplicate",
        "node-duplicate-menu", "cross-parent-drag", "undo-cross-parent",
        "redo-cross-parent", "same-parent-drag-noop",
        "root-root-drag-noop", "node-context-delete", "delete-confirm",
        "main-process-boundary", "unique-root-subcase",
        "unique-root-select", "unique-root-delete-confirm",
        "unique-root-undo", "unique-root-redo") } elseif ($Case -eq "U03-inspector-transform") { @(
        "bootstrap", "select-transform-node", "inspector-translation-x",
        "inspector-rotation-x", "inspector-scale-x", "undo-transform",
        "redo-transform", "final-boundary") } elseif ($Case -eq "U03-gizmo-translate") { @(
        "bootstrap", "select-transform-node", "drag-outside-release",
        "undo-transform", "redo-transform", "final-boundary") } elseif ($Case -eq "U03-gizmo-escape") { @(
        "bootstrap", "select-transform-node", "drag-escape-only",
        "late-release", "final-boundary") } elseif ($Case -in @("U03-gizmo-escape-release", "U03-gizmo-release-escape")) { @(
        "bootstrap", "select-transform-node", "drag-same-frame",
        "late-release", "final-boundary") } elseif ($Case -in @("U03-gizmo-focus-no-release", "U03-gizmo-focus-same-frame", "U03-gizmo-focus-delivered-release")) { @(
        "bootstrap", "select-transform-node",
        $(if ($Case -eq "U03-gizmo-focus-no-release") { "drag-focus-no-release" }
          elseif ($Case -eq "U03-gizmo-focus-same-frame") { "drag-focus-same-frame" }
          else { "drag-focus-delivered-release" }),
        "final-boundary") } else { @(
        "bootstrap", "viewport-single-hit", "viewport-toggle", "viewport-blank",
        "gizmo-translate-single-hit", "gizmo-rotate-single-hit", "gizmo-scale-single-hit",
        "select-transform-node", "inspector-translation-x", "toolbar-gizmo-toggle", "toolbar-grid-toggle",
        "toolbar-snap-toggle", "gizmo-drag-outside-release", "undo-transform",
        "redo-transform", "gizmo-escape-only", "gizmo-escape-release-same-frame",
        "focus-loss-no-release", "focus-loss-with-release", "final-boundary") }
    foreach ($required in $requiredIds) {
        if (-not $ids.Contains($required)) {
            throw "OS replay plan is missing required action: $required"
        }
    }
}

function Get-PlanContractTrace {
    $lines = [Collections.Generic.List[string]]::new()
    $lines.Add("schema=$SchemaVersion")
    $lines.Add("protocol=$Protocol")
    $lines.Add("case=$Case")
    foreach ($role in $GraphRoles) {
        $lines.Add((@(
            "role",
            [string]$role.role,
            [string]$role.subcase,
            [string]$role.objectKind,
            [string]$role.labelContains,
            [string]$role.introducedByStep,
            [string]$role.sameObjectAsRole,
            [string]$role.directParentRole,
            [string]$role.distinctFromRole,
            $(if ([bool]$role.requireRoot) { "1" } else { "0" })) -join [char]9))
    }
    foreach ($step in $ExpectedSteps) {
        $dispatchTrace = (@(
            foreach ($dispatch in @($step.expectedDispatches)) {
                $fields = @(
                    [string]$dispatch.source,
                    [string]$dispatch.payloadKind,
                    [string]$dispatch.operation,
                    [string]$dispatch.targetRole,
                    [string]$dispatch.secondaryRole,
                    [string]$dispatch.text,
                    $(if ([bool]$dispatch.accepted) { "1" } else { "0" }),
                    [string]$dispatch.refresh)
                ($fields -join ":")
            }
        ) -join ",")
        foreach ($phase in @($step.phases)) {
            $lineFields = @(
                [string]$step.index,
                [string]$step.id,
                [string]$step.subcase,
                [string]$step.effect,
                [string]$step.expectedRevisionDelta,
                [string]$step.history,
                $dispatchTrace,
                $(if ([bool]$step.expectHierarchyOrderDigestChange) { "1" } else { "0" }),
                $(if ([bool]$step.relaunchProcessBefore) { "1" } else { "0" }),
                [string]$phase.ordinal,
                [string]$phase.id,
                [string]$phase.phase,
                [string]$phase.target.semantic,
                [string]$phase.target.role,
                [string]$phase.target.graphRowRole,
                [string]$phase.input.kind,
                [string]$phase.input.value,
                $(if ([bool]$phase.requireHovered) { "1" } else { "0" }),
                $(if ([bool]$phase.requireFocused) { "1" } else { "0" }),
                $(if ([bool]$phase.requireDropAccepted) { "1" } else { "0" }),
                [string]$phase.capture)
            if ($Case -ne "U02-hierarchy") {
                $lineFields += ((@(
                    foreach ($dispatch in @($step.expectedDispatches)) {
                        ([string]$dispatch.phase + "=" + [string]$dispatch.value + ";")
                    }
                ) -join ""))
                $lineFields += $(if ($null -ne $phase.PSObject.Properties["expectedDispatchCount"]) {
                    [string]$phase.expectedDispatchCount
                } else { "" })
            }
            if ($Case -in @("U03-gizmo-translate", "U03-gizmo-escape", "U03-gizmo-escape-release", "U03-gizmo-release-escape", "U03-gizmo-focus-no-release", "U03-gizmo-focus-same-frame", "U03-gizmo-focus-delivered-release")) {
                $lineFields += $(if ($null -ne $phase.PSObject.Properties["gizmo"]) {
                    (@(
                        [string]$phase.gizmo.phase,
                        $(if ([bool]$phase.gizmo.captureHeld) { "1" } else { "0" }),
                        [string]$phase.gizmo.beginCount,
                        [string]$phase.gizmo.previewCount,
                        [string]$phase.gizmo.commitCount,
                        $(if ($null -eq $phase.gizmo.pointerOutside) { "" }
                          elseif ([bool]$phase.gizmo.pointerOutside) { "1" } else { "0" })
                    ) -join ":")
                } else { "" })
            }
            if ($Case -in @("U03-gizmo-escape", "U03-gizmo-escape-release", "U03-gizmo-release-escape")) {
                $lineFields += $(if ($null -ne $phase.PSObject.Properties["gizmo"]) {
                    (@([string]$phase.gizmo.detachCount, [string]$phase.gizmo.latchCount,
                        $(if ([bool]$phase.gizmo.noSelection) { "1" } else { "0" })) -join ":")
                } else { "" })
            }
            if ($Case -in @("U03-gizmo-focus-no-release", "U03-gizmo-focus-same-frame", "U03-gizmo-focus-delivered-release")) {
                $lineFields += $(if ($null -ne $phase.PSObject.Properties["gizmo"]) {
                    (@([string]$phase.gizmo.deliveredReleaseCount,
                        [string]$phase.gizmo.escapePressCount,
                        [string]$phase.gizmo.focusLossCount,
                        [string]$phase.gizmo.suppressedReleaseCount) -join ":")
                } else { "" })
            }
            $lines.Add(($lineFields -join [char]9))
        }
    }
    return ($lines -join [char]10) + [char]10
}

Test-ExpectedPlan
$PlanTrace = Get-PlanContractTrace
$PlanContractHash = Get-BytesHash(
    [Text.UTF8Encoding]::new($false).GetBytes($PlanTrace))

if ($ContractTraceOnly) {
    [Console]::Out.Write($PlanTrace)
    return
}
if ($ContractOnly) {
    $contract = [ordered]@{
        status = "CONTRACT_OK"
        protocol = $Protocol
        case = $Case
        stepCount = @($ExpectedSteps).Count
        phaseCount = $ExpectedPhaseCount
        planSha256 = $PlanContractHash
        sendInputOnly = $true
        appProbeRequired = $true
        passEvidenceProduced = $false
    }
    if ($Case -in @("U03-inspector-transform", "U03-gizmo-translate", "U03-gizmo-escape", "U03-gizmo-escape-release", "U03-gizmo-release-escape", "U03-gizmo-focus-no-release", "U03-gizmo-focus-same-frame", "U03-gizmo-focus-delivered-release")) {
        $contract.parentCase = "U03-transform-gizmo"
        $contract.coverageComplete = $false
    }
    $contract | ConvertTo-Json -Depth 8
    return
}

function Stop-Blocked([string]$Reason) {
    throw "BLOCKED_APP_PROBE_UNAVAILABLE: $Reason"
}

if ([string]::IsNullOrWhiteSpace($BuildRoot) -or
    [string]::IsNullOrWhiteSpace($EvidenceRoot)) {
    Stop-Blocked "BuildRoot and EvidenceRoot are required outside ContractOnly"
}

$BuildRoot = [IO.Path]::GetFullPath($BuildRoot)
$EvidenceRoot = [IO.Path]::GetFullPath($EvidenceRoot)
$RepoRoot = [IO.Path]::GetFullPath((Join-Path $PSScriptRoot ".."))
if ([string]::IsNullOrWhiteSpace($ExecutablePath)) {
    $osCandidate = Join-Path $BuildRoot "editor_ui_os_replay.exe"
    $liveCandidate = Join-Path $BuildRoot "editor_ui_live_replay.exe"
    if (Test-Path -LiteralPath $osCandidate -PathType Leaf) {
        $ExecutablePath = $osCandidate
    }
    elseif (Test-Path -LiteralPath $liveCandidate -PathType Leaf) {
        $ExecutablePath = $liveCandidate
    }
    else {
        Stop-Blocked "no OS-probe-capable executable is present"
    }
}
$Executable = [IO.Path]::GetFullPath($ExecutablePath)
if (-not (Test-Path -LiteralPath $Executable -PathType Leaf)) {
    Stop-Blocked "executable is missing: $Executable"
}
$ExecutableHash = Get-Hash $Executable
try {
    $capabilityStart = [Diagnostics.ProcessStartInfo]::new()
    $capabilityStart.FileName = $Executable
    $capabilityStart.WorkingDirectory = $BuildRoot
    $capabilityStart.Arguments = "--editor-os-capability --editor-os-capability-case=$Case"
    $capabilityStart.UseShellExecute = $false
    $capabilityStart.CreateNoWindow = $true
    $capabilityStart.RedirectStandardOutput = $true
    $capabilityStart.RedirectStandardError = $true
    $capabilityProcess = [Diagnostics.Process]::new()
    $capabilityProcess.StartInfo = $capabilityStart
    if (-not $capabilityProcess.Start()) {
        throw "capability process did not start"
    }
    $capabilityStdout = $capabilityProcess.StandardOutput.ReadToEndAsync()
    $capabilityStderr = $capabilityProcess.StandardError.ReadToEndAsync()
    $capabilityTimeout = [Math]::Min(10000, $TimeoutSeconds * 1000)
    if (-not $capabilityProcess.WaitForExit($capabilityTimeout)) {
        try { $capabilityProcess.Kill() } catch { }
        [void]$capabilityProcess.WaitForExit(5000)
        throw "capability process did not exit within $capabilityTimeout ms"
    }
    $capabilityStdout.Wait(); $capabilityStderr.Wait()
    $capabilityRaw = [string]$capabilityStdout.Result
    $capabilityError = [string]$capabilityStderr.Result
    if ($capabilityProcess.ExitCode -ne 0 -or
        -not [string]::IsNullOrEmpty($capabilityError) -or
        [string]::IsNullOrWhiteSpace($capabilityRaw)) {
        throw "capability process returned no clean JSON capability"
    }
    $capabilityTrimmed = $capabilityRaw.Trim()
    if (-not $capabilityTrimmed.StartsWith("{") -or
        -not $capabilityTrimmed.EndsWith("}") -or
        $capabilityTrimmed -match '"executableSha256"' -or
        $capabilityTrimmed.Contains($ExecutableHash)) {
        throw "capability output is malformed or improperly claims exe identity"
    }
    $Capability = $capabilityTrimmed | ConvertFrom-Json
    $capabilityProperties = @($Capability.PSObject.Properties.Name)
    if ([string]$Capability.documentKind -ne
            "mgif.editor-ui-os-replay-capability" -or
        [int]$Capability.schemaVersion -ne $SchemaVersion -or
        [string]$Capability.protocol -ne $Protocol -or
        [string]$Capability.case -ne $Case -or
        -not [bool]$Capability.integration -or
        -not [bool]$Capability.sendInputOnly -or
        [string]$Capability.planSha256 -ne $PlanContractHash -or
        [string]$Capability.sourceRole -ne "editor-ui-os-replay-cli-v1" -or
        -not [bool]$Capability.requiresHarnessExecutableHashBinding -or
        [int]$Capability.stepCount -ne @($ExpectedSteps).Count -or
        [int]$Capability.phaseCount -ne $ExpectedPhaseCount -or
        @($Capability.processes).Count -ne $(if ($Case -eq "U02-hierarchy") { 2 } else { 1 }) -or
        $capabilityProperties -contains "executableSha256") {
        throw "capability JSON does not match the compiled OS replay contract"
    }
    $mainCapability = @($Capability.processes | Where-Object {
        [string]$_.subcase -eq "main" -and [int]$_.startStep -eq 0 -and
        [int]$_.stopStep -eq $(if ($Case -eq "U02-hierarchy") { 20 } else { @($ExpectedSteps).Count - 1 })
    })
    $uniqueCapability = @($Capability.processes | Where-Object {
        [string]$_.subcase -eq "unique-root" -and [int]$_.startStep -eq 21 -and [int]$_.stopStep -eq 28
    })
    if ($mainCapability.Count -ne 1 -or ($Case -eq "U02-hierarchy" -and $uniqueCapability.Count -ne 1) -or
        (Get-Hash $Executable) -ne $ExecutableHash) {
        throw "capability process ranges or executable stability are invalid"
    }
    $capabilityBytes = [Text.UTF8Encoding]::new($false).GetBytes($capabilityRaw)
    $CapabilityRecord = [ordered]@{
        raw = $capabilityRaw
        bytes = $capabilityBytes.Length
        sha256 = Get-BytesHash $capabilityBytes
    }
}
catch {
    Stop-Blocked ("executable capability probe failed: " +
        $_.Exception.Message)
}

$CachePath = Join-Path $BuildRoot "CMakeCache.txt"
if (-not (Test-Path -LiteralPath $CachePath -PathType Leaf)) {
    Stop-Blocked "configured CMake cache is missing"
}
$CacheLines = Get-Content -LiteralPath $CachePath
function Get-CacheValue([string]$Name) {
    $matches = @($CacheLines | Where-Object {
        $_ -match ("^" + [regex]::Escape($Name) + ":[^=]+=")
    })
    if ($matches.Count -ne 1) {
        throw "CMake cache key has invalid cardinality: $Name"
    }
    return ([string]$matches[0]).Substring(
        ([string]$matches[0]).IndexOf("=") + 1).Trim()
}
$ConfiguredBuildType = Get-CacheValue "CMAKE_BUILD_TYPE"
$ConfiguredTesting = Get-CacheValue "BUILD_TESTING"
$ConfiguredPreflightSkip = Get-CacheValue "DEMO_SKIP_EDITOR_DEPENDENCY_PREFLIGHT"
if ($ConfiguredBuildType -ne $Configuration -or
    $ConfiguredTesting -ne "ON" -or
    $ConfiguredPreflightSkip -ne "OFF") {
    throw "BuildRoot configuration violates the OS replay contract"
}
if (Test-Path -LiteralPath $EvidenceRoot) {
    throw "EvidenceRoot must be fresh and absent: $EvidenceRoot"
}

$SourcePaths = @(
    "CMakeLists.txt",
    "app/MinimalLatestApp.h",
    "app/MinimalLatestApp.cpp",
    "app/editor/EditorUiOsReplayPlan.h",
    "app/editor/EditorUiOsReplayPlan.cpp",
    "app/editor/EditorUiOsReplayCli.h",
    "app/editor/EditorUiOsReplayCli.cpp",
    "app/editor/EditorUiOsReplayRuntime.h",
    "app/editor/EditorUiOsReplayRuntime.cpp",
    "app/editor/EditorUiOsReplayHost.h",
    "app/editor/EditorUiOsReplayHost.cpp",
    "app/editor/EditorUiOsReplayMain.cpp",
    "app/editor/EditorUiProbe.h",
    "app/editor/EditorController.h",
    "app/editor/EditorController.cpp",
    "app/editor/EditorInputRouter.h",
    "app/editor/EditorInputRouter.cpp",
    "app/editor/EditorNativeGizmoInput.h",
    "app/editor/EditorViewportRuntime.h",
    "app/editor/EditorViewportRuntime.cpp",
    "app/editor/SceneEditorUi.h",
    "app/editor/SceneEditorUi.cpp",
    "app/editor/SceneEditorUiModel.h",
    "app/editor/EditorRendererAdapter.h",
    "app/editor/EditorRendererAdapter.cpp",
    "scene/editor/GltfDocument.cpp",
    "scene/editor/EditorSession.cpp",
    "scene/editor/EditorInputRouter.cpp",
    "scene/editor/SceneRuntimeMapping.cpp",
    "scene/editor/commands/ContinuousEdit.cpp",
    "scene/editor/commands/EditorCommand.cpp",
    "scene/editor/commands/EditorHistory.cpp",
    "scene/editor/commands/HierarchyCommands.cpp",
    "scene/editor/commands/HistoryLifecycle.cpp",
    "scene/editor/commands/TransformCommands.cpp",
    "scene/editor/include/mgif/scene_editor/GltfDocument.h",
    "scene/editor/include/mgif/scene_editor/EditorSession.h",
    "scene/editor/include/mgif/scene_editor/EditorInputRouter.h",
    "scene/editor/include/mgif/scene_editor/SceneRuntimeMapping.h",
    "scene/editor/include/mgif/scene_editor/commands/ContinuousEdit.h",
    "scene/editor/include/mgif/scene_editor/commands/EditorCommand.h",
    "scene/editor/include/mgif/scene_editor/commands/EditorHistory.h",
    "scene/editor/include/mgif/scene_editor/commands/HierarchyCommands.h",
    "scene/editor/include/mgif/scene_editor/commands/HistoryLifecycle.h",
    "scene/editor/include/mgif/scene_editor/commands/TransformCommands.h",
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
    "render/editor/GizmoGeometry.h",
    "render/editor/GizmoGeometry.cpp",
    "render/editor/TransformGizmoMath.cpp",
    "render/editor/include/mgif/editor/TransformGizmoMath.h",
    "render/editor/EditorOverlayTypes.h",
    "render/editor/EditorOverlay.cpp",
    "render/editor/include/mgif/editor/EditorOverlay.h",
    "render/GPUDrivenRenderer.h",
    "render/GPUDrivenRenderer.cpp",
    "render/passes/GPUDrivenPresentPass.h",
    "render/passes/GPUDrivenPresentPass.cpp",
    "render/passes/GPUDrivenImguiPass.h",
    "render/passes/GPUDrivenImguiPass.cpp",
    "rhi/vulkan/VulkanDevice.h",
    "rhi/vulkan/VulkanDevice.cpp",
    "rhi/vulkan/VulkanQueue.h",
    "rhi/vulkan/VulkanQueue.cpp",
    "rhi/vulkan/VulkanSwapchain.h",
    "rhi/vulkan/VulkanSwapchain.cpp",
    "rhi/vulkan/VulkanCommandBuffer.h",
    "rhi/vulkan/VulkanCommandBuffer.cpp",
    "tests/fixtures/gltf_editor/r09_shared_mesh.gltf",
    "tests/fixtures/gltf_editor/minimal_scene.gltf",
    "tests/editor/editor_ui_os_replay_cli_tests.cpp",
    "tests/editor/editor_ui_os_replay_plan_tests.cpp",
    "tests/editor/editor_ui_os_replay_runtime_tests.cpp",
    "tests/editor/editor_ui_os_replay_contract_tests.py",
    "tools/validate_editor_p0_artifacts.py",
    "tools/editor_ui_os_replay.ps1"
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
$CacheHash = Get-Hash $CachePath

New-Item -ItemType Directory -Path $EvidenceRoot | Out-Null
$AppEvidence = Join-Path $EvidenceRoot "app"
$SyncRoot = Join-Path $EvidenceRoot "sync"
$ScreenshotRoot = Join-Path $EvidenceRoot "screenshots"
$StdoutPath = Join-Path $EvidenceRoot "stdout.log"
$StderrPath = Join-Path $EvidenceRoot "stderr.log"
$ManifestPath = Join-Path $EvidenceRoot "manifest.json"
$SourceIdentityPath = Join-Path $EvidenceRoot "source-identity.json"
$CapabilityArtifactPath = Join-Path $EvidenceRoot "capability.json"
$VulkanLayerInfo = $null
$VulkanSdkRoot = $null
$VulkanLayerOutputRoot = $null
$VulkanLayerFrameRecords = [Collections.Generic.List[object]]::new()
if ($UseVulkanScreenshotLayer) {
    $sdkRoot = [Environment]::GetEnvironmentVariable("VULKAN_SDK")
    if ([string]::IsNullOrWhiteSpace($sdkRoot)) {
        Stop-Blocked "VULKAN_SDK is required for the explicit screenshot-layer fallback"
    }
    $layerManifestPath = [IO.Path]::GetFullPath(
        (Join-Path $sdkRoot "Bin\VkLayer_screenshot.json"))
    $layerDllPath = [IO.Path]::GetFullPath(
        (Join-Path $sdkRoot "Bin\VkLayer_screenshot.dll"))
    if (-not (Test-Path -LiteralPath $layerManifestPath -PathType Leaf) -or
        -not (Test-Path -LiteralPath $layerDllPath -PathType Leaf)) {
        Stop-Blocked "Vulkan screenshot layer manifest or DLL is missing"
    }
    try {
        $layerManifest = Get-Content -LiteralPath $layerManifestPath -Raw |
            ConvertFrom-Json
    }
    catch {
        Stop-Blocked ("Vulkan screenshot layer manifest is invalid: " +
            $_.Exception.Message)
    }
    $platforms = @($layerManifest.layer.platforms | ForEach-Object {
        [string]$_
    })
    $settings = @($layerManifest.layer.features.settings)
    $settingKeys = @($settings | ForEach-Object { [string]$_.key })
    $settingEnvs = @($settings | ForEach-Object { [string]$_.env })
    $formatSetting = @($settings | Where-Object { [string]$_.key -eq "format" })
    $formatFlags = @($formatSetting | ForEach-Object {
        @($_.flags | ForEach-Object { [string]$_.key })
    })
    if ([string]$layerManifest.layer.name -ne "VK_LAYER_LUNARG_screenshot" -or
        [string]$layerManifest.file_format_version -ne "1.2.0" -or
        [string]$layerManifest.layer.type -ne "GLOBAL" -or
        [string]::IsNullOrWhiteSpace([string]$layerManifest.layer.api_version) -or
        [string]$layerManifest.layer.library_path -notmatch
            "(?i)(^|[\\/])VkLayer_screenshot\.dll$" -or
        $platforms -notcontains "WINDOWS" -or
        $settingKeys.Count -ne 3 -or
        $settingKeys -notcontains "frames" -or
        $settingKeys -notcontains "dir" -or
        $settingKeys -notcontains "format" -or
        $settingEnvs -notcontains "VK_SCREENSHOT_FRAMES" -or
        $settingEnvs -notcontains "VK_SCREENSHOT_DIR" -or
        $settingEnvs -notcontains "VK_SCREENSHOT_FORMAT" -or
        $formatSetting.Count -ne 1 -or $formatFlags -notcontains "SRGB") {
        Stop-Blocked "Vulkan screenshot layer manifest identity is not the expected Windows layer"
    }
    $VulkanLayerInfo = [ordered]@{
        name = [string]$layerManifest.layer.name
        manifestPath = $layerManifestPath
        manifestSha256 = Get-Hash $layerManifestPath
        dllPath = $layerDllPath
        dllSha256 = Get-Hash $layerDllPath
        outputFormat = "SRGB"
    }
    $VulkanSdkRoot = $sdkRoot
}
New-Item -ItemType Directory -Path $AppEvidence | Out-Null
New-Item -ItemType Directory -Path $SyncRoot | Out-Null
New-Item -ItemType Directory -Path $ScreenshotRoot | Out-Null
if ($UseVulkanScreenshotLayer) {
    $VulkanLayerOutputRoot = Join-Path $EvidenceRoot "vulkan-screenshot-layer"
    New-Item -ItemType Directory -Path $VulkanLayerOutputRoot | Out-Null
    $layerDirectory = Get-Item -LiteralPath $VulkanLayerOutputRoot -Force
    if (($layerDirectory.Attributes -band [IO.FileAttributes]::ReparsePoint) -ne 0) {
        throw "Vulkan screenshot layer output directory cannot be a reparse point"
    }
}

function Get-EvidenceRelativePath([string]$Path) {
    $root = [IO.Path]::GetFullPath($EvidenceRoot).TrimEnd('\', '/')
    $full = [IO.Path]::GetFullPath($Path)
    $prefix = $root + [IO.Path]::DirectorySeparatorChar
    if (-not $full.StartsWith($prefix, [StringComparison]::OrdinalIgnoreCase)) {
        throw "Evidence artifact escapes EvidenceRoot: $full"
    }
    $cursor = Get-Item -LiteralPath $full -Force
    while ($null -ne $cursor -and
           -not [string]::Equals($cursor.FullName, $root,
               [StringComparison]::OrdinalIgnoreCase)) {
        if (($cursor.Attributes -band [IO.FileAttributes]::ReparsePoint) -ne 0) {
            throw "Evidence artifact traverses a reparse point: $full"
        }
        $cursor = if ($cursor -is [IO.FileInfo]) {
            $cursor.Directory
        }
        else {
            $cursor.Parent
        }
    }
    $rootItem = Get-Item -LiteralPath $root -Force
    if (($rootItem.Attributes -band [IO.FileAttributes]::ReparsePoint) -ne 0) {
        throw "EvidenceRoot cannot be a reparse point"
    }
    return $full.Substring($prefix.Length).Replace('\', '/')
}

function Write-Json([string]$Path, [object]$Value) {
    $json = $Value | ConvertTo-Json -Depth 100
    $temporary = $Path + ".tmp"
    [IO.File]::WriteAllText(
        $temporary, $json + [Environment]::NewLine,
        [Text.UTF8Encoding]::new($false))
    Move-Item -LiteralPath $temporary -Destination $Path
}

[IO.File]::WriteAllText(
    $CapabilityArtifactPath, [string]$CapabilityRecord.raw,
    [Text.UTF8Encoding]::new($false))
if ((Get-Hash $CapabilityArtifactPath) -ne [string]$CapabilityRecord.sha256) {
    throw "Persisted capability bytes differ from executable stdout"
}

Write-Json $SourceIdentityPath ([ordered]@{
    documentKind = "mgif.editor-ui-os-replay-source-identity"
    schemaVersion = $SchemaVersion
    aggregateSha256 = $SourceIdentityHash
    records = $SourceRecords
})

Add-Type -AssemblyName System.Drawing
Add-Type -AssemblyName System.Windows.Forms
Add-Type @"
using System;
using System.Runtime.InteropServices;
public static class MGIFEditorOsNative {
    [StructLayout(LayoutKind.Sequential)] public struct RECT {
        public int Left; public int Top; public int Right; public int Bottom;
    }
    [StructLayout(LayoutKind.Sequential)] public struct POINT {
        public int X; public int Y;
    }
    [StructLayout(LayoutKind.Sequential)] public struct MONITORINFO {
        public uint cbSize; public RECT rcMonitor; public RECT rcWork;
        public uint dwFlags;
    }
    [StructLayout(LayoutKind.Sequential)] public struct MOUSEINPUT {
        public int dx; public int dy; public uint mouseData; public uint dwFlags;
        public uint time; public UIntPtr dwExtraInfo;
    }
    [StructLayout(LayoutKind.Sequential)] public struct KEYBDINPUT {
        public ushort wVk; public ushort wScan; public uint dwFlags;
        public uint time; public UIntPtr dwExtraInfo;
    }
    [StructLayout(LayoutKind.Explicit)] public struct INPUTUNION {
        [FieldOffset(0)] public MOUSEINPUT mi;
        [FieldOffset(0)] public KEYBDINPUT ki;
    }
    [StructLayout(LayoutKind.Sequential)] public struct INPUT {
        public uint type; public INPUTUNION U;
    }
    [DllImport("user32.dll", SetLastError=true)]
    public static extern uint SendInput(uint count, INPUT[] inputs, int size);
    [DllImport("user32.dll")] public static extern IntPtr GetForegroundWindow();
    [DllImport("user32.dll", SetLastError=true)]
    public static extern uint GetWindowThreadProcessId(IntPtr hwnd, out uint pid);
    [DllImport("user32.dll")] public static extern bool IsWindow(IntPtr hwnd);
    [DllImport("user32.dll")] public static extern bool IsWindowVisible(IntPtr hwnd);
    [DllImport("user32.dll")] public static extern bool IsWindowEnabled(IntPtr hwnd);
    [DllImport("user32.dll")] public static extern bool IsIconic(IntPtr hwnd);
    [DllImport("user32.dll")] public static extern bool GetClientRect(IntPtr hwnd, out RECT rect);
    [DllImport("user32.dll")] public static extern bool ClientToScreen(IntPtr hwnd, ref POINT point);
    [DllImport("user32.dll")] public static extern bool GetWindowRect(IntPtr hwnd, out RECT rect);
    [DllImport("user32.dll")] public static extern bool SetWindowPos(IntPtr hwnd, IntPtr after, int x, int y, int width, int height, uint flags);
    [DllImport("user32.dll")] public static extern IntPtr GetWindow(IntPtr hwnd, uint command);
    [DllImport("user32.dll")] public static extern IntPtr GetAncestor(IntPtr hwnd, uint flags);
    [DllImport("user32.dll")] public static extern IntPtr WindowFromPoint(POINT point);
    [DllImport("user32.dll")] public static extern IntPtr MonitorFromRect(ref RECT rect, uint flags);
    [DllImport("user32.dll", CharSet=CharSet.Auto)]
    public static extern bool GetMonitorInfo(IntPtr monitor, ref MONITORINFO info);
    [DllImport("user32.dll")] public static extern uint GetDpiForWindow(IntPtr hwnd);
    [DllImport("user32.dll")] public static extern IntPtr SetThreadDpiAwarenessContext(IntPtr value);
    [DllImport("user32.dll")] public static extern IntPtr GetThreadDpiAwarenessContext();
    [DllImport("user32.dll")] public static extern bool AreDpiAwarenessContextsEqual(IntPtr first, IntPtr second);
    [DllImport("user32.dll")] public static extern int GetSystemMetrics(int index);
    [DllImport("user32.dll")] public static extern bool GetCursorPos(out POINT point);
    [DllImport("user32.dll")] public static extern bool SetForegroundWindow(IntPtr hwnd);
    [DllImport("user32.dll")] public static extern bool BringWindowToTop(IntPtr hwnd);
    [DllImport("user32.dll")] public static extern bool ShowWindow(IntPtr hwnd, int command);
    [DllImport("dwmapi.dll")] public static extern int DwmFlush();
    [DllImport("dwmapi.dll")] public static extern int DwmGetWindowAttribute(
        IntPtr hwnd, uint attribute, out int value, uint size);
    [DllImport("user32.dll")] public static extern bool PrintWindow(IntPtr hwnd, IntPtr hdc, uint flags);
    [DllImport("user32.dll")] public static extern IntPtr GetDC(IntPtr hwnd);
    [DllImport("user32.dll")] public static extern int ReleaseDC(IntPtr hwnd, IntPtr hdc);
    [DllImport("gdi32.dll")] public static extern bool BitBlt(
        IntPtr destination, int x, int y, int width, int height,
        IntPtr source, int sourceX, int sourceY, uint operation);
}
"@

Add-Type -ReferencedAssemblies "System.Drawing.dll" @"
using System;
using System.Drawing;
using System.Drawing.Imaging;
using System.IO;
using System.Runtime.InteropServices;
using System.Text;

public static class MGIFVulkanScreenshot {
    private static bool IsWhitespace(byte value) {
        return value == 9 || value == 10 || value == 11 || value == 12 ||
            value == 13 || value == 32;
    }

    private static string NextToken(byte[] bytes, ref int offset) {
        while (offset < bytes.Length) {
            if (IsWhitespace(bytes[offset])) {
                ++offset;
                continue;
            }
            if (bytes[offset] == (byte)'#') {
                while (offset < bytes.Length && bytes[offset] != 10 &&
                       bytes[offset] != 13) ++offset;
                continue;
            }
            break;
        }
        int start = offset;
        while (offset < bytes.Length && !IsWhitespace(bytes[offset]) &&
               bytes[offset] != (byte)'#') ++offset;
        if (start == offset) throw new InvalidDataException("PPM token is empty");
        return Encoding.ASCII.GetString(bytes, start, offset - start);
    }

    private static int ParsePositive(string text, string label) {
        if (String.IsNullOrEmpty(text) ||
            (text.Length > 1 && text[0] == '0'))
            throw new InvalidDataException("PPM " + label + " is invalid");
        long value = 0;
        for (int index = 0; index < text.Length; ++index) {
            char character = text[index];
            if (character < '0' || character > '9')
                throw new InvalidDataException("PPM " + label + " is invalid");
            value = value * 10 + (character - '0');
            if (value > Int32.MaxValue)
                throw new InvalidDataException("PPM " + label + " is invalid");
        }
        if (value <= 0) throw new InvalidDataException("PPM " + label + " is invalid");
        return (int)value;
    }

    public static void ConvertPpm(string source, string destination,
                                  int expectedWidth, int expectedHeight) {
        byte[] bytes = File.ReadAllBytes(source);
        int offset = 0;
        string magic = NextToken(bytes, ref offset);
        int width = ParsePositive(NextToken(bytes, ref offset), "width");
        int height = ParsePositive(NextToken(bytes, ref offset), "height");
        string maxText = NextToken(bytes, ref offset);
        int maximum = ParsePositive(maxText, "maximum");
        if (magic != "P6" || width != expectedWidth || height != expectedHeight ||
            maximum != 255)
            throw new InvalidDataException("PPM header does not match the client contract");
        if (offset >= bytes.Length || !IsWhitespace(bytes[offset]))
            throw new InvalidDataException("PPM header has no payload delimiter");
        // Consume only the header delimiter. Pixel bytes may themselves be
        // whitespace and must never be skipped.
        if (bytes[offset] == 13 && offset + 1 < bytes.Length &&
            bytes[offset + 1] == 10) offset += 2;
        else ++offset;
        long expectedPayload = (long)width * (long)height * 3L;
        if ((long)bytes.Length - offset != expectedPayload)
            throw new InvalidDataException("PPM payload length differs from the client contract");

        using (Bitmap bitmap = new Bitmap(width, height, PixelFormat.Format24bppRgb)) {
            Rectangle rectangle = new Rectangle(0, 0, width, height);
            BitmapData locked = bitmap.LockBits(rectangle, ImageLockMode.WriteOnly,
                                                PixelFormat.Format24bppRgb);
            try {
                if (locked.Stride <= 0)
                    throw new InvalidDataException("PNG converter received a negative bitmap stride");
                int rowBytes = checked(width * 3);
                byte[] row = new byte[rowBytes];
                for (int y = 0; y < height; ++y) {
                    int sourceRow = checked(offset + y * rowBytes);
                    for (int x = 0; x < width; ++x) {
                        int sourcePixel = sourceRow + x * 3;
                        int destinationPixel = x * 3;
                        row[destinationPixel] = bytes[sourcePixel + 2];
                        row[destinationPixel + 1] = bytes[sourcePixel + 1];
                        row[destinationPixel + 2] = bytes[sourcePixel];
                    }
                    Marshal.Copy(row, 0,
                                 IntPtr.Add(locked.Scan0, y * locked.Stride),
                                 row.Length);
                }
            }
            finally { bitmap.UnlockBits(locked); }
            bitmap.Save(destination, ImageFormat.Png);
        }
    }
}
"@

$PerMonitorAwareV2 = [IntPtr](-4)
$PreviousDpiAwareness =
    [MGIFEditorOsNative]::SetThreadDpiAwarenessContext($PerMonitorAwareV2)
if ($PreviousDpiAwareness -eq [IntPtr]::Zero -or
    -not [MGIFEditorOsNative]::AreDpiAwarenessContextsEqual(
        [MGIFEditorOsNative]::GetThreadDpiAwarenessContext(),
        $PerMonitorAwareV2)) {
    throw "Could not enter Per-Monitor-V2 DPI awareness"
}

function New-MouseInput([int]$X, [int]$Y, [uint32]$Flags) {
    $mouse = New-Object MGIFEditorOsNative+MOUSEINPUT
    $mouse.dx = $X; $mouse.dy = $Y; $mouse.dwFlags = $Flags
    $union = New-Object MGIFEditorOsNative+INPUTUNION
    $union.mi = $mouse
    $entry = New-Object MGIFEditorOsNative+INPUT
    $entry.type = 0; $entry.U = $union
    return $entry
}

function New-KeyInput([uint16]$VirtualKey, [uint16]$Scan, [uint32]$Flags) {
    $key = New-Object MGIFEditorOsNative+KEYBDINPUT
    $key.wVk = $VirtualKey; $key.wScan = $Scan; $key.dwFlags = $Flags
    $union = New-Object MGIFEditorOsNative+INPUTUNION
    $union.ki = $key
    $entry = New-Object MGIFEditorOsNative+INPUT
    $entry.type = 1; $entry.U = $union
    return $entry
}

function Send-NativeInput([object[]]$Inputs) {
    if ($Inputs.Count -eq 0) { return 0 }
    [MGIFEditorOsNative+INPUT[]]$typed = $Inputs
    $size = [Runtime.InteropServices.Marshal]::SizeOf(
        [type][MGIFEditorOsNative+INPUT])
    $sent = [MGIFEditorOsNative]::SendInput($typed.Length, $typed, $size)
    if ($sent -ne $typed.Length) {
        $errorCode = [Runtime.InteropServices.Marshal]::GetLastWin32Error()
        throw "SendInput delivered $sent/$($typed.Length), error=$errorCode"
    }
    return [int]$sent
}

function Send-PointerMove([int]$X, [int]$Y) {
    $left = [MGIFEditorOsNative]::GetSystemMetrics(76)
    $top = [MGIFEditorOsNative]::GetSystemMetrics(77)
    $virtualWidth = [MGIFEditorOsNative]::GetSystemMetrics(78)
    $virtualHeight = [MGIFEditorOsNative]::GetSystemMetrics(79)
    if ($virtualWidth -le 1 -or $virtualHeight -le 1 -or
        $X -lt $left -or $Y -lt $top -or
        $X -ge ($left + $virtualWidth) -or
        $Y -ge ($top + $virtualHeight)) {
        throw "Target center is outside the virtual desktop"
    }
    $absoluteX = [int][Math]::Round((($X - $left) * 65535.0) / ($virtualWidth - 1))
    $absoluteY = [int][Math]::Round((($Y - $top) * 65535.0) / ($virtualHeight - 1))
    return Send-NativeInput @(
        (New-MouseInput $absoluteX $absoluteY ([uint32]0xC001)))
}

function Send-MouseButton([string]$Kind) {
    switch ($Kind) {
        "left-click" {
            return Send-NativeInput @(
                (New-MouseInput 0 0 ([uint32]0x0002)),
                (New-MouseInput 0 0 ([uint32]0x0004)))
        }
        "right-click" {
            return Send-NativeInput @(
                (New-MouseInput 0 0 ([uint32]0x0008)),
                (New-MouseInput 0 0 ([uint32]0x0010)))
        }
        "right-down" {
            return Send-NativeInput @((New-MouseInput 0 0 ([uint32]0x0008)))
        }
        "right-up" {
            return Send-NativeInput @((New-MouseInput 0 0 ([uint32]0x0010)))
        }
        "left-down" {
            return Send-NativeInput @((New-MouseInput 0 0 ([uint32]0x0002)))
        }
        "left-up" {
            return Send-NativeInput @((New-MouseInput 0 0 ([uint32]0x0004)))
        }
        default { throw "Unsupported pointer button input: $Kind" }
    }
}

function Wait-EditorInputDeliveryFence {
    if ($null -eq $Process) {
        throw "Cannot establish an editor input delivery fence without a process"
    }
    $script:LastInputDeliveryFence = "none"
    try {
        # SendInput only queues the packet.  WaitForInputIdle gives the target
        # GUI thread a bounded opportunity to dispatch the WM_LBUTTONUP/GLFW
        # callback before the subsequent foreground transfer can generate
        # WM_KILLFOCUS. Some GLFW/Vulkan processes reject WaitForInputIdle
        # with ERROR_NOT_GUI_PROCESS even though they own a live window; in
        # that case the host-side native queue/focus fence remains authoritative
        # and the callback order is checked in the next ready observation.
        $idle = $false
        try { $idle = $Process.WaitForInputIdle(5000) } catch { $idle = $false }
        if ($idle) {
            $script:LastInputDeliveryFence = "process-input-idle"
            return
        }
        $Process.Refresh()
        if ($Process.HasExited) { throw "editor process exited during input fence" }
        if ([MGIFEditorOsNative]::GetForegroundWindow() -ne $WindowHandle) {
            throw "editor foreground changed during input fence"
        }
        Start-Sleep -Milliseconds 25
        $Process.Refresh()
        if ($Process.HasExited) { throw "editor process exited during input fence" }
        $script:LastInputDeliveryFence = "native-queue-before-continue"
    }
    catch {
        throw "Could not establish editor input delivery fence: $($_.Exception.Message)"
    }
}

function Get-VirtualKey([string]$Name) {
    switch ($Name) {
        "G" { return [uint16]0x47 }
        "T" { return [uint16]0x54 }
        "Escape" { return [uint16]0x1B }
        "A" { return [uint16]0x41 }
        "Y" { return [uint16]0x59 }
        "Z" { return [uint16]0x5A }
        "Enter" { return [uint16]0x0D }
        default { throw "Unsupported virtual key: $Name" }
    }
}

function Send-KeyChord([string]$Chord) {
    if ($Chord -notmatch "^Ctrl\+([AGTYZ])$") {
        throw "Unsupported key chord: $Chord"
    }
    $key = Get-VirtualKey $Matches[1]
    return Send-NativeInput @(
        (New-KeyInput ([uint16]0x11) 0 0),
        (New-KeyInput $key 0 0),
        (New-KeyInput $key 0 ([uint32]0x0002)),
        (New-KeyInput ([uint16]0x11) 0 ([uint32]0x0002)))
}

function Send-HeldKeyEdge([string]$Kind, [string]$Name) {
    $key = Get-VirtualKey $Name
    if ($Kind -eq "key-down") {
        if (-not $HeldKeys.Add($Name)) {
            throw "Replay key-down duplicates an already held key"
        }
        return Send-NativeInput @((New-KeyInput $key 0 0))
    }
    if ($Kind -ne "key-up" -or -not $HeldKeys.Contains($Name)) {
        throw "Replay key-up does not match a held key"
    }
    $count = Send-NativeInput @((New-KeyInput $key 0 ([uint32]0x0002)))
    [void]$HeldKeys.Remove($Name)
    return $count
}

function Send-EscapeReleaseBatch([string]$Order) {
    $expectedOrder = if ($Case -eq "U03-gizmo-escape-release") { "escape-first" }
        elseif ($Case -eq "U03-gizmo-release-escape") { "release-first" } else { "" }
    if ($expectedOrder -eq "" -or $Order -ne $expectedOrder -or
        -not $LeftButtonHeld -or $HeldKeys.Contains("Escape")) {
        throw "Same-frame terminal batch has an invalid order or held-input state"
    }
    $escape = New-KeyInput (Get-VirtualKey "Escape") 0 0
    $release = New-MouseInput 0 0 ([uint32]0x0004)
    $events = if ($Order -eq "escape-first") { @($escape, $release) }
        else { @($release, $escape) }
    [void]$HeldKeys.Add("Escape")
    $sent = Send-NativeInput $events
    if ($sent -ne 2) { throw "Same-frame terminal batch did not inject both native events" }
    $script:LeftButtonHeld = $false
    return $sent
}

function Send-UnicodeText([string]$Text) {
    $inputs = [Collections.Generic.List[object]]::new()
    foreach ($character in $Text.ToCharArray()) {
        $scan = [uint16][char]$character
        $inputs.Add((New-KeyInput 0 $scan ([uint32]0x0004)))
        $inputs.Add((New-KeyInput 0 $scan ([uint32]0x0006)))
    }
    return Send-NativeInput @($inputs)
}

function Convert-Hwnd([string]$Value) {
    if ($Value -notmatch "^0x[0-9a-fA-F]+$") {
        throw "Ready HWND is not hexadecimal"
    }
    $numeric = [Convert]::ToUInt64($Value.Substring(2), 16)
    if ($numeric -eq 0 -or $numeric -gt [Int64]::MaxValue) {
        throw "Ready HWND is outside the supported range"
    }
    return [IntPtr]([Int64]$numeric)
}

function Get-ClientContract([IntPtr]$Hwnd) {
    $rect = New-Object MGIFEditorOsNative+RECT
    $origin = New-Object MGIFEditorOsNative+POINT
    if (-not [MGIFEditorOsNative]::GetClientRect($Hwnd, [ref]$rect) -or
        -not [MGIFEditorOsNative]::ClientToScreen($Hwnd, [ref]$origin)) {
        throw "Could not resolve native client bounds"
    }
    return [ordered]@{
        left = $origin.X
        top = $origin.Y
        right = $origin.X + ($rect.Right - $rect.Left)
        bottom = $origin.Y + ($rect.Bottom - $rect.Top)
        width = $rect.Right - $rect.Left
        height = $rect.Bottom - $rect.Top
        dpi = [int][MGIFEditorOsNative]::GetDpiForWindow($Hwnd)
    }
}

function Test-SameClientContract([object]$First, [object]$Second) {
    return $First.left -eq $Second.left -and
        $First.top -eq $Second.top -and
        $First.right -eq $Second.right -and
        $First.bottom -eq $Second.bottom -and
        $First.width -eq $Second.width -and
        $First.height -eq $Second.height -and
        $First.dpi -eq $Second.dpi
}

function Test-RectanglesIntersect(
    [int]$LeftA, [int]$TopA, [int]$RightA, [int]$BottomA,
    [int]$LeftB, [int]$TopB, [int]$RightB, [int]$BottomB) {
    return $LeftA -lt $RightB -and $RightA -gt $LeftB -and
        $TopA -lt $BottomB -and $BottomA -gt $TopB
}

function Test-CaptureWindowState(
    [IntPtr]$Hwnd,
    [object]$ExpectedClient,
    [int]$TargetX,
    [int]$TargetY) {
    if (-not [MGIFEditorOsNative]::AreDpiAwarenessContextsEqual(
            [MGIFEditorOsNative]::GetThreadDpiAwarenessContext(),
            $PerMonitorAwareV2)) {
        throw "Capture thread lost Per-Monitor-V2 DPI awareness"
    }
    if ($null -eq $Process) {
        throw "Capture process authority is unavailable"
    }
    $Process.Refresh()
    [uint32]$nativePid = 0
    if (-not [MGIFEditorOsNative]::IsWindow($Hwnd) -or
        -not [MGIFEditorOsNative]::IsWindowVisible($Hwnd) -or
        -not [MGIFEditorOsNative]::IsWindowEnabled($Hwnd) -or
        [MGIFEditorOsNative]::IsIconic($Hwnd) -or
        [MGIFEditorOsNative]::GetForegroundWindow() -ne $Hwnd -or
        [MGIFEditorOsNative]::GetAncestor($Hwnd, 2) -ne $Hwnd -or
        [MGIFEditorOsNative]::GetWindowThreadProcessId(
            $Hwnd, [ref]$nativePid) -eq 0 -or
        $nativePid -ne [uint32]$Process.Id -or
        $WindowHandle -ne $Hwnd) {
        throw "Capture HWND/foreground/PID authority changed"
    }
    $client = Get-ClientContract $Hwnd
    if (-not (Test-SameClientContract $client $ExpectedClient) -or
        $client.width -ne $Width -or $client.height -ne $Height -or
        $client.dpi -ne $ObservedDpi) {
        throw "Capture client rectangle or DPI changed"
    }

    [int64]$virtualLeft = [MGIFEditorOsNative]::GetSystemMetrics(76)
    [int64]$virtualTop = [MGIFEditorOsNative]::GetSystemMetrics(77)
    [int64]$virtualWidth = [MGIFEditorOsNative]::GetSystemMetrics(78)
    [int64]$virtualHeight = [MGIFEditorOsNative]::GetSystemMetrics(79)
    [int64]$virtualRight = $virtualLeft + $virtualWidth
    [int64]$virtualBottom = $virtualTop + $virtualHeight
    if ($virtualWidth -le 0 -or $virtualHeight -le 0 -or
        [int64]$client.left -lt $virtualLeft -or
        [int64]$client.top -lt $virtualTop -or
        [int64]$client.right -gt $virtualRight -or
        [int64]$client.bottom -gt $virtualBottom) {
        throw "PNG client rectangle is outside the virtual desktop"
    }

    $nativeClient = New-Object MGIFEditorOsNative+RECT
    $nativeClient.Left = $client.left; $nativeClient.Top = $client.top
    $nativeClient.Right = $client.right; $nativeClient.Bottom = $client.bottom
    $monitor = [MGIFEditorOsNative]::MonitorFromRect(
        [ref]$nativeClient, 0)
    $monitorInfo = New-Object MGIFEditorOsNative+MONITORINFO
    $monitorInfo.cbSize = [uint32][Runtime.InteropServices.Marshal]::SizeOf(
        [type][MGIFEditorOsNative+MONITORINFO])
    if ($monitor -eq [IntPtr]::Zero -or
        -not [MGIFEditorOsNative]::GetMonitorInfo(
            $monitor, [ref]$monitorInfo) -or
        $client.left -lt $monitorInfo.rcMonitor.Left -or
        $client.top -lt $monitorInfo.rcMonitor.Top -or
        $client.right -gt $monitorInfo.rcMonitor.Right -or
        $client.bottom -gt $monitorInfo.rcMonitor.Bottom) {
        throw "PNG client rectangle crosses a monitor boundary"
    }

    $seenWindows = [Collections.Generic.HashSet[int64]]::new()
    $above = [MGIFEditorOsNative]::GetWindow($Hwnd, 3)
    $examined = 0
    while ($above -ne [IntPtr]::Zero) {
        if (++$examined -gt 4096 -or
            -not $seenWindows.Add($above.ToInt64())) {
            throw "Capture Z-order enumeration is invalid"
        }
        if ([MGIFEditorOsNative]::IsWindowVisible($above) -and
            -not [MGIFEditorOsNative]::IsIconic($above)) {
            [int]$cloaked = 0
            $cloakStatus = [MGIFEditorOsNative]::DwmGetWindowAttribute(
                $above, 14, [ref]$cloaked, 4)
            if ($cloakStatus -ne 0 -or $cloaked -eq 0) {
                $aboveRect = New-Object MGIFEditorOsNative+RECT
                if (-not [MGIFEditorOsNative]::GetWindowRect(
                        $above, [ref]$aboveRect)) {
                    throw "Could not audit a visible window above the target"
                }
                if (Test-RectanglesIntersect `
                        $client.left $client.top $client.right $client.bottom `
                        $aboveRect.Left $aboveRect.Top `
                        $aboveRect.Right $aboveRect.Bottom) {
                    throw "Another top-level window occludes the capture client"
                }
            }
        }
        $above = [MGIFEditorOsNative]::GetWindow($above, 3)
    }

    $points = [Collections.Generic.List[object]]::new()
    foreach ($fractionY in @(0.1, 0.3, 0.5, 0.7, 0.9)) {
        foreach ($fractionX in @(0.1, 0.3, 0.5, 0.7, 0.9)) {
            $points.Add([ordered]@{
                x = $client.left + [int][Math]::Round(
                    ($client.width - 1) * $fractionX)
                y = $client.top + [int][Math]::Round(
                    ($client.height - 1) * $fractionY)
            })
        }
    }
    if ($TargetX -lt $client.left -or $TargetX -ge $client.right -or
        $TargetY -lt $client.top -or $TargetY -ge $client.bottom) {
        throw "Capture target sample is outside the client"
    }
    $points.Add([ordered]@{ x = $TargetX; y = $TargetY })
    $sampledPoints = [Collections.Generic.HashSet[string]]::new()
    foreach ($sample in $points) {
        $key = "$($sample.x),$($sample.y)"
        if (-not $sampledPoints.Add($key)) { continue }
        $point = New-Object MGIFEditorOsNative+POINT
        $point.X = $sample.x; $point.Y = $sample.y
        $hit = [MGIFEditorOsNative]::WindowFromPoint($point)
        if ($hit -eq [IntPtr]::Zero -or
            [MGIFEditorOsNative]::GetAncestor($hit, 2) -ne $Hwnd) {
            throw "Capture ownership sample does not belong to the target HWND"
        }
    }
    return [ordered]@{
        client = $client
        monitor = [ordered]@{
            left = $monitorInfo.rcMonitor.Left
            top = $monitorInfo.rcMonitor.Top
            right = $monitorInfo.rcMonitor.Right
            bottom = $monitorInfo.rcMonitor.Bottom
        }
        ownershipSamples = $sampledPoints.Count
        occludingWindows = 0
    }
}

function Test-ObjectId([object]$Value, [string]$ExpectedKind = "") {
    if ($null -eq $Value -or [int64]$Value.generation -le 0 -or
        [string]$Value.authority -notmatch "^[1-9][0-9]*$" -or
        (-not [string]::IsNullOrEmpty($ExpectedKind) -and
         [string]$Value.kind -ne $ExpectedKind)) {
        throw "Ready GraphRowKey contains an invalid object identity"
    }
}

function Test-Sha256([string]$Value, [string]$Name) {
    if ($Value -notmatch "^[0-9a-f]{64}$") {
        throw "Ready $Name is not a lowercase SHA-256 digest"
    }
}

function Get-CanonicalGraphKey([object]$Target, [string]$GraphRowRole) {
    if ([string]::IsNullOrEmpty($GraphRowRole)) {
        if ($null -ne $Target.graphRowKey -or
            $null -ne $Target.graphRowRole) {
            throw "Client target unexpectedly contains GraphRowKey data"
        }
        return ""
    }
    if ([string]$Target.graphRowRole -ne $GraphRowRole -or
        $null -eq $Target.graphRowKey) {
        throw "Ready target is missing its required GraphRowKey binding"
    }
    Test-ObjectId $Target.graphRowKey.scene "Scene"
    Test-ObjectId $Target.graphRowKey.object
    if ($null -ne $Target.graphRowKey.directParent) {
        Test-ObjectId $Target.graphRowKey.directParent "Node"
    }
    if ([int64]$Target.graphRowKey.occurrence -lt 0 -or
        [string]$Target.graphRowKey.scene.authority -ne
            [string]$Target.graphRowKey.object.authority -or
        ($null -ne $Target.graphRowKey.directParent -and
         [string]$Target.graphRowKey.scene.authority -ne
            [string]$Target.graphRowKey.directParent.authority)) {
        throw "Ready GraphRowKey authority/occurrence is invalid"
    }
    return ($Target.graphRowKey | ConvertTo-Json -Compress -Depth 12)
}

function Get-FixedProbeLabel([string]$Semantic) {
    switch ($Semantic) {
        "scene-add-node" { return "Add Node" }
        "node-duplicate" { return "Duplicate" }
        "node-delete" { return "Delete" }
        "node-add-child-menu" { return "Add Child" }
        "node-add-child-node" { return "Empty Node" }
        "node-rename" { return "Rename" }
        "rename-input" { return "##name" }
        "delete-accept" { return "Delete" }
        default { return "" }
    }
}

function Test-GizmoRect([object]$Rect) {
    if ($null -eq $Rect -or
        [string]$Rect.coordinateSpace -ne "imgui-screen-logical") {
        throw "Native gizmo rectangle lost its screen-logical coordinate space"
    }
    foreach ($field in @("minimumX", "minimumY", "maximumX", "maximumY")) {
        $value = [double]$Rect.$field
        if ([double]::IsNaN($value) -or [double]::IsInfinity($value)) {
            throw "Native gizmo rectangle contains a non-finite coordinate"
        }
    }
    if ([double]$Rect.maximumX -le [double]$Rect.minimumX -or
        [double]$Rect.maximumY -le [double]$Rect.minimumY) {
        throw "Native gizmo rectangle has no area"
    }
}

function Test-ObservedFocusGizmo([object]$Ready, [object]$Step, [object]$Phase) {
    $expected = if ($null -ne $Phase.PSObject.Properties["gizmo"]) {
        $Phase.gizmo
    } else { $null }
    $nativeTarget = [string]$Phase.target.semantic -in @(
        "gizmo-translate-y", "viewport-outside")
    $gizmo = if ($null -ne $Ready.PSObject.Properties["gizmo"]) {
        $Ready.gizmo
    } else { $null }
    if ($null -eq $gizmo) {
        if ($null -ne $expected -or $nativeTarget) {
            throw "Required native focus-loss gizmo observation is missing"
        }
        return
    }
    if ([string]$gizmo.source -ne "editor-gizmo-adapter-v1" -or
        -not [bool]$gizmo.ndcYDown -or
        [uint64]$gizmo.renderedFrame -ne [uint64]$Ready.frame.frameSerial -or
        [uint64]$gizmo.inputFrame -eq 0 -or
        [uint64]$gizmo.documentRevision -ne [uint64]$Ready.authority.documentRevision -or
        [uint64]$gizmo.selectionRevision -ne [uint64]$Ready.authority.selectionRevision -or
        [int]$gizmo.phase -notin @(0, 1, 2, 3) -or
        [int64]$gizmo.beginCount -lt 0 -or
        [int64]$gizmo.previewCount -lt 0 -or
        [int64]$gizmo.commitCount -lt 0 -or
        [int64]$gizmo.detachCount -ne 0 -or
        [int64]$gizmo.latchCount -gt 1 -or
        [int64]$gizmo.deliveredReleaseCount -gt 1 -or
        [int64]$gizmo.escapePressCount -ne 0 -or
        [int64]$gizmo.focusLossCount -gt 1 -or
        [int64]$gizmo.suppressedReleaseCount -gt 2) {
        throw "Native focus-loss gizmo observation lost its frame, authority, or action identity"
    }
    $previousCallbackClock = [uint64]$GizmoLastCallbackClock
    if ([uint64]$gizmo.nativeCallbackSequence -lt $previousCallbackClock) {
        throw "Native gizmo callback sequence clock regressed behind accepted evidence"
    }
    $terminalExpected = $null -ne $expected -and
        ([int64]$expected.deliveredReleaseCount -gt 0 -or
         [int64]$expected.focusLossCount -gt 0 -or
         [int64]$expected.suppressedReleaseCount -gt 0)
    $terminalCountersAdvanced =
        [int64]$gizmo.deliveredReleaseCount -gt $GizmoLastDeliveredReleaseCount -or
        [int64]$gizmo.escapePressCount -gt $GizmoLastEscapePressCount -or
        [int64]$gizmo.focusLossCount -gt $GizmoLastFocusLossCount -or
        [int64]$gizmo.suppressedReleaseCount -gt $GizmoLastSuppressedReleaseCount
    $callbackBackedTerminal = @($gizmo.lastTerminalEvents |
        Where-Object { [uint64]$_.callbackSequence -ne 0 }).Count -gt 0
    # A focus-first native batch may contain a trailing mouse-up callback that
    # the app deliberately holds through the restore frame.  The callback
    # ordinal is therefore already present in the prior focus-loss clock when
    # the suppression is replayed.  Permit only this exact one-event,
    # suppression-only transition; all other callback-backed terminals must
    # advance the clock strictly.
    $deferredFocusSuppression =
        [int64]$gizmo.suppressedReleaseCount -gt $GizmoLastSuppressedReleaseCount -and
        $GizmoLastFocusLossCount -gt 0 -and
        [int64]$gizmo.deliveredReleaseCount -eq $GizmoLastDeliveredReleaseCount -and
        [int64]$gizmo.escapePressCount -eq $GizmoLastEscapePressCount -and
        [int64]$gizmo.focusLossCount -eq $GizmoLastFocusLossCount -and
        @($gizmo.lastTerminalEvents).Count -eq 1 -and
        [string]$gizmo.lastTerminalEvents[0].kind -eq "suppressed-physical-mouse-up" -and
        [uint64]$gizmo.lastTerminalEvents[0].callbackSequence -ne 0 -and
        [uint64]$gizmo.lastTerminalEvents[0].callbackSequence -le $previousCallbackClock -and
        [uint64]$gizmo.lastTerminalEvents[0].callbackSequence -le [uint64]$gizmo.nativeCallbackSequence
    if ($terminalExpected -and $terminalCountersAdvanced -and
        $callbackBackedTerminal -and
        [uint64]$gizmo.nativeCallbackSequence -le $previousCallbackClock -and
        -not $deferredFocusSuppression) {
        throw "Native focus-loss terminal callback ordinal predates its ready boundary"
    }
    $script:GizmoLastCallbackClock = [uint64]$gizmo.nativeCallbackSequence
    $script:GizmoLastDeliveredReleaseCount = [uint64]$gizmo.deliveredReleaseCount
    $script:GizmoLastEscapePressCount = [uint64]$gizmo.escapePressCount
    $script:GizmoLastFocusLossCount = [uint64]$gizmo.focusLossCount
    $script:GizmoLastSuppressedReleaseCount = [uint64]$gizmo.suppressedReleaseCount
    foreach ($field in @("translation", "rotation", "scale")) {
        $requiredCount = if ($field -eq "rotation") { 4 } else { 3 }
        if (@($gizmo.$field).Count -ne $requiredCount) {
            throw "Native focus-loss gizmo TRS component count is invalid"
        }
        foreach ($value in @($gizmo.$field)) {
            if ($null -eq $value -or [double]::IsNaN([double]$value) -or
                [double]::IsInfinity([double]$value)) {
                throw "Native focus-loss gizmo TRS contains a non-finite value"
            }
        }
    }
    if ($null -eq $expected) {
        if ([int64]$gizmo.beginCount -ne 0 -or
            [int64]$gizmo.previewCount -ne 0 -or
            [int64]$gizmo.commitCount -ne 0 -or
            [int64]$gizmo.detachCount -ne 0 -or
            [int64]$gizmo.latchCount -ne 0 -or
            [int64]$gizmo.deliveredReleaseCount -ne 0 -or
            [int64]$gizmo.escapePressCount -ne 0 -or
            [int64]$gizmo.focusLossCount -ne 0 -or
            [int64]$gizmo.suppressedReleaseCount -ne 0 -or
            @($gizmo.lastTerminalEvents).Count -ne 0 -or
            [string]$gizmo.terminal -notin @("", "none")) {
            throw "Native focus-loss prelude contains active or terminal evidence"
        }
        if ($null -ne $gizmo.node) {
            Test-ObjectId $gizmo.node "Node"
            if ([string]$gizmo.node.authority -ne [string]$Ready.authority.documentAuthority) {
                throw "Native focus-loss prelude contains a foreign node"
            }
        }
        return
    }
    if ($null -eq $gizmo.node) {
        throw "Native focus-loss gesture did not retain its selected node"
    }
    Test-ObjectId $gizmo.node "Node"
    if ([string]$gizmo.node.authority -ne [string]$Ready.authority.documentAuthority -or
        [uint64]$gizmo.mappingEpoch -eq 0 -or
        ($gizmo.node | ConvertTo-Json -Compress -Depth 8) -ne
            ($Ready.authority.selectedObject | ConvertTo-Json -Compress -Depth 8) -or
        [string]$Ready.authority.selectionKind -ne "Node") {
        throw "Native focus-loss gesture did not retain its selected node"
    }
    if ([int64]$gizmo.beginCount -eq 0 -and $null -ne $gizmo.gestureNode) {
        throw "Native focus-loss pre-terminal observation invented a gesture identity"
    }
    if ([int64]$gizmo.beginCount -gt 0) {
        if ($null -eq $gizmo.gestureNode) {
            throw "Native focus-loss gesture lacks retained gesture identity"
        }
        Test-ObjectId $gizmo.gestureNode "Node"
        if ([string]$gizmo.gestureNode.authority -ne
                [string]$Ready.authority.documentAuthority -or
            ($gizmo.gestureNode | ConvertTo-Json -Compress -Depth 8) -ne
                ($gizmo.node | ConvertTo-Json -Compress -Depth 8)) {
            throw "Native focus-loss gesture identity differs from the observed selected node"
        }
    }
    if ($null -ne $expected) {
        foreach ($field in @("phase", "captureHeld", "beginCount", "previewCount",
                             "commitCount", "latchCount", "deliveredReleaseCount",
                             "escapePressCount", "focusLossCount",
                             "suppressedReleaseCount")) {
            if ($gizmo.$field -ne $expected.$field) {
                throw "Native focus-loss gizmo observation differs from phase expectation: $field"
            }
        }
        if ([int]$gizmo.activeComponent -ne $(if ([bool]$expected.captureHeld) { 1 } else { 255 })) {
            throw "Native focus-loss gizmo expected attachment/capture component is absent"
        }
        if ([int64]$expected.latchCount -gt 0) {
            if ([int64]$Ready.authority.undoCount -ne 0 -or
                [int64]$Ready.authority.redoCount -ne 0 -or
                [string]$Ready.authority.undoDescription -ne "" -or
                [string]$Ready.authority.redoDescription -ne "") {
                throw "Focus-loss latch unexpectedly changed the history envelope"
            }
        }
        elseif ([int64]$expected.commitCount -gt 0 -and
                ([int64]$Ready.authority.undoCount -ne 1 -or
                 [string]::IsNullOrEmpty([string]$Ready.authority.undoDescription) -or
                 [int64]$Ready.authority.redoCount -ne 0)) {
            throw "Delivered focus-loss release has an invalid history envelope"
        }
        if ($null -ne $expected.pointerOutside) {
            Test-GizmoRect $gizmo.viewportRect
            if (-not [bool]$gizmo.hasPointer -or
                [double]::IsNaN([double]$gizmo.pointer.x) -or
                [double]::IsNaN([double]$gizmo.pointer.y) -or
                [double]::IsInfinity([double]$gizmo.pointer.x) -or
                [double]::IsInfinity([double]$gizmo.pointer.y)) {
                throw "Native focus-loss gizmo pointer observation is missing or non-finite"
            }
            $outside = [double]$gizmo.pointer.x -lt [double]$gizmo.viewportRect.minimumX -or
                [double]$gizmo.pointer.x -ge [double]$gizmo.viewportRect.maximumX -or
                [double]$gizmo.pointer.y -lt [double]$gizmo.viewportRect.minimumY -or
                [double]$gizmo.pointer.y -ge [double]$gizmo.viewportRect.maximumY
            if ($outside -ne [bool]$expected.pointerOutside) {
                throw "Native focus-loss gizmo pointer outside-viewport observation differs"
            }
        }
    }
    $focusLossPhase = [int64]$gizmo.focusLossCount -gt 0
    $terminalExpected = [int64]$gizmo.commitCount -gt 0 -or
        [int64]$gizmo.latchCount -gt 0
    if ($terminalExpected) {
        $terminal = if ([int64]$gizmo.commitCount -gt 0) { "commit" } else { "latch" }
        if ([string]$gizmo.terminal -ne $terminal -or
            [uint64]$gizmo.lastTerminalFrame -eq 0 -or
            [uint64]$gizmo.lastTerminalSequence -eq 0) {
            throw "Native focus-loss terminal lacks its observed event"
        }
    }
    elseif ([string]$gizmo.terminal -notin @("", "none")) {
        throw "Native focus-loss gizmo reported an unexpected terminal event"
    }
    if ([bool]$gizmo.lastTerminalEventsOverflowed) {
        throw "Native focus-loss terminal journal overflowed"
    }
    $delivered = [int64]$gizmo.deliveredReleaseCount -gt 0
    $sameFrame = $Case -eq "U03-gizmo-focus-same-frame"
    $events = @($gizmo.lastTerminalEvents)
    if ($delivered -and (@($events).Count -ne 1 -or
            [string]$events[0].kind -ne "delivered-release")) {
        throw "Delivered focus-loss release lacks its exact terminal journal"
    }
    if ($delivered -and ([uint64]$events[0].sequence -eq 0 -or
                         [uint64]$events[0].callbackSequence -eq 0 -or
                         [uint64]$gizmo.lastTerminalSequence -ne
                             [uint64]$events[0].sequence)) {
        throw "Delivered focus-loss release lacks terminal provenance"
    }
    if (-not $delivered -and $focusLossPhase) {
        $firstFocusBatch = $null -ne $expected -and
            [int64]$expected.focusLossCount -gt 0 -and
            [int64]$expected.suppressedReleaseCount -eq 0
        # The adapter exposes the most recent nonempty terminal batch.  A
        # focus-loss frame contains only focus-lost; after focus restoration a
        # polling release replaces that batch with one suppressed-up event.
        # Only the explicit same-frame partition contains two events.
        $expectedKinds = @(
            if ($sameFrame) {
            @("suppressed-physical-mouse-up", "focus-lost")
            } elseif ($firstFocusBatch) {
            @("focus-lost")
            } else { @("suppressed-physical-mouse-up") }
        )
        if (@($events).Count -ne @($expectedKinds).Count) {
            throw "Focus-loss terminal journal cardinality is incomplete"
        }
        for ($index = 0; $index -lt @($expectedKinds).Count; ++$index) {
            if ([string]$events[$index].kind -ne $expectedKinds[$index]) {
                throw "Focus-loss journal order or suppression classification is invalid: phase=$($Phase.id) index=$index actual=$([string]$events[$index].kind) expected=$([string]$expectedKinds[$index]) focus=$([int64]$gizmo.focusLossCount) suppressed=$([int64]$gizmo.suppressedReleaseCount)"
            }
            $pollingSuppressed = [string]$events[$index].kind -eq
                "suppressed-physical-mouse-up" -and -not $sameFrame -and
                @($events).Count -eq 1
            if ([uint64]$events[$index].sequence -eq 0 -or
                (-not $pollingSuppressed -and
                 [uint64]$events[$index].callbackSequence -eq 0) -or
                [uint64]$events[$index].callbackSequence -gt
                    [uint64]$gizmo.nativeCallbackSequence -or
                ($index -gt 0 -and
                  ([uint64]$events[$index].sequence -le [uint64]$events[$index - 1].sequence -or
                  [uint64]$events[$index].callbackSequence -le [uint64]$events[$index - 1].callbackSequence))) {
                throw "Focus-loss journal lacks increasing routed/native event provenance"
            }
        }
        if ([uint64]$gizmo.lastTerminalSequence -eq 0 -or
            [uint64]$events[-1].sequence -ne [uint64]$gizmo.lastTerminalSequence) {
            throw "Focus-loss terminal sequence does not bind its latest batch"
        }
    }
}

function Test-ObservedGizmo([object]$Ready, [object]$Step, [object]$Phase) {
    if ($Case -in @("U03-gizmo-focus-no-release", "U03-gizmo-focus-same-frame", "U03-gizmo-focus-delivered-release")) {
        Test-ObservedFocusGizmo $Ready $Step $Phase
        return
    }
    if ($Case -notin @("U03-gizmo-translate", "U03-gizmo-escape", "U03-gizmo-escape-release", "U03-gizmo-release-escape")) { return }
    $sameFrameCase = $Case -in @("U03-gizmo-escape-release", "U03-gizmo-release-escape")
    $escapeCase = $Case -eq "U03-gizmo-escape" -or $sameFrameCase
    $expected = if ($null -ne $Phase.PSObject.Properties["gizmo"]) {
        $Phase.gizmo
    } else { $null }
    $nativeTarget = [string]$Phase.target.semantic -in @(
        "gizmo-translate-y", "viewport-outside")
    $gizmo = if ($null -ne $Ready.PSObject.Properties["gizmo"]) {
        $Ready.gizmo
    } else { $null }
    if ($null -eq $gizmo) {
        if ($escapeCase -or $null -ne $expected -or $nativeTarget -or
            [string]$Step.id -in @("undo-transform", "redo-transform", "final-boundary")) {
            throw "Required native gizmo observation is missing"
        }
        return
    }
    if ([string]$gizmo.source -ne "editor-gizmo-adapter-v1" -or
        -not [bool]$gizmo.ndcYDown -or
        [uint64]$gizmo.renderedFrame -ne [uint64]$Ready.frame.frameSerial -or
        [uint64]$gizmo.inputFrame -eq 0 -or
        [uint64]$gizmo.documentRevision -ne [uint64]$Ready.authority.documentRevision -or
        [uint64]$gizmo.selectionRevision -ne [uint64]$Ready.authority.selectionRevision -or
        [int]$gizmo.phase -notin @(0, 1, 2, 3) -or
        [int64]$gizmo.beginCount -lt 0 -or
        [int64]$gizmo.previewCount -lt 0 -or
        [int64]$gizmo.commitCount -lt 0 -or
        [int64]$gizmo.detachCount -lt 0 -or
        [int64]$gizmo.detachCount -gt $(if ($escapeCase) { 1 } else { 0 }) -or
        [int64]$gizmo.latchCount -ne 0) {
        throw "Native gizmo observation lost its frame, authority, or action identity"
    }
    foreach ($field in @("translation", "rotation", "scale")) {
        $requiredCount = if ($field -eq "rotation") { 4 } else { 3 }
        if (@($gizmo.$field).Count -ne $requiredCount) {
            throw "Native gizmo TRS component count is invalid"
        }
        foreach ($value in @($gizmo.$field)) {
            if ($null -eq $value -or [double]::IsNaN([double]$value) -or
                [double]::IsInfinity([double]$value)) {
                throw "Native gizmo TRS contains a non-finite value"
            }
        }
    }
    if ($null -ne $gizmo.node) {
        Test-ObjectId $gizmo.node "Node"
        if ([string]$gizmo.node.authority -ne [string]$Ready.authority.documentAuthority -or
            [uint64]$gizmo.mappingEpoch -eq 0 -or
            ($gizmo.node | ConvertTo-Json -Compress -Depth 8) -ne
                ($Ready.authority.selectedObject | ConvertTo-Json -Compress -Depth 8)) {
            throw "Native gizmo attachment does not match selected document authority"
        }
    }
    if ($null -ne $expected) {
        foreach ($field in @("phase", "captureHeld", "beginCount", "previewCount", "commitCount")) {
            if ($gizmo.$field -ne $expected.$field) {
                throw "Native gizmo observation differs from phase expectation: $field"
            }
        }
        $noSelection = $escapeCase -and [bool]$expected.noSelection
        if ((-not $noSelection -and $null -eq $gizmo.node) -or
            [int]$gizmo.activeComponent -ne $(if ([bool]$expected.captureHeld) { 1 } else { 255 })) {
            throw "Native gizmo expected attachment/capture component is absent"
        }
        if ($escapeCase -and
            ([int64]$gizmo.detachCount -ne [int64]$expected.detachCount -or
             [int64]$gizmo.latchCount -ne [int64]$expected.latchCount)) {
            throw "Native Escape detach/latch counts differ from phase expectation"
        }
        if ($noSelection -and
            ($null -ne $gizmo.node -or [int]$gizmo.phase -ne 0 -or
             $null -ne $Ready.authority.selectedObject -or
             $null -ne $Ready.authority.selectedGraphRowKey -or
             [string]$Ready.authority.selectionKind -ne "None")) {
            throw "Native Escape did not detach and clear actual selection"
        }
        if ($null -ne $expected.pointerOutside) {
            Test-GizmoRect $gizmo.viewportRect
            if (-not [bool]$gizmo.hasPointer -or
                [double]::IsNaN([double]$gizmo.pointer.x) -or
                [double]::IsNaN([double]$gizmo.pointer.y) -or
                [double]::IsInfinity([double]$gizmo.pointer.x) -or
                [double]::IsInfinity([double]$gizmo.pointer.y)) {
                throw "Native gizmo pointer observation is missing or non-finite"
            }
            $outside = [double]$gizmo.pointer.x -lt [double]$gizmo.viewportRect.minimumX -or
                [double]$gizmo.pointer.x -ge [double]$gizmo.viewportRect.maximumX -or
                [double]$gizmo.pointer.y -lt [double]$gizmo.viewportRect.minimumY -or
                [double]$gizmo.pointer.y -ge [double]$gizmo.viewportRect.maximumY
            if ($outside -ne [bool]$expected.pointerOutside) {
                throw "Native gizmo pointer outside-viewport observation differs"
            }
        }
    }
    if ($escapeCase) {
        $expectedReleaseCount = if ($sameFrameCase -and [int64]$gizmo.detachCount -eq 1) { 1 } else { 0 }
        if ([int64]$Ready.authority.undoCount -ne 0 -or
            [int64]$Ready.authority.redoCount -ne 0 -or
            [string]$Ready.authority.undoDescription -ne "" -or
            [string]$Ready.authority.redoDescription -ne "" -or
            [int64]$gizmo.commitCount -ne 0 -or
            [int64]$gizmo.deliveredReleaseCount -ne $expectedReleaseCount -or
            [int64]$gizmo.focusLossCount -ne 0 -or
            [int64]$gizmo.suppressedReleaseCount -ne 0 -or
            [bool]$gizmo.lastTerminalEventsOverflowed) {
            throw "Native Escape unexpectedly committed, changed history, or received another terminal"
        }
        if ([int64]$gizmo.beginCount -gt 0) {
            Test-ObjectId $gizmo.gestureNode "Node"
            if (-not $ProcessGraphKeys.ContainsKey("transform-node")) {
                throw "Native Escape gesture lacks its selected graph role"
            }
            $gestureKey = $ProcessGraphKeys["transform-node"] | ConvertFrom-Json
            if ([string]$gizmo.gestureNode.authority -ne [string]$Ready.authority.documentAuthority -or
                [uint64]$gizmo.mappingEpoch -eq 0 -or
                ($gizmo.gestureNode | ConvertTo-Json -Compress -Depth 8) -ne
                    ($gestureKey.object | ConvertTo-Json -Compress -Depth 8)) {
                throw "Native Escape retained TRS is not bound to the actual gesture node"
            }
        }
        if ($sameFrameCase) {
            if ([uint64]$gizmo.nativeCallbackSequence -lt $GizmoLastCallbackClock) {
                throw "Native callback sequence clock regressed behind accepted evidence"
            }
            $script:GizmoLastCallbackClock = [uint64]$gizmo.nativeCallbackSequence
        }
        if (([string]$Step.id -eq "drag-escape-only" -and [string]$Phase.id -eq "escape") -or
            ([string]$Step.id -eq "drag-same-frame" -and [string]$Phase.id -eq "terminal-batch")) {
            $script:GizmoEscapePreview = $gizmo
        }
        if ([int64]$gizmo.detachCount -eq 1) {
            $events = @($gizmo.lastTerminalEvents)
            if ([int64]$gizmo.escapePressCount -ne 1 -or
                [string]$gizmo.terminal -ne "detach" -or
                [uint64]$gizmo.lastTerminalFrame -eq 0 -or
                [uint64]$gizmo.lastTerminalSequence -eq 0) {
                throw "Native Escape detach lacks its exact single Escape terminal event"
            }
            if ($sameFrameCase) {
                $expectedKinds = @(
                    if ($Case -eq "U03-gizmo-escape-release") {
                        @("escape-pressed", "delivered-release")
                    } else { @("delivered-release", "escape-pressed") }
                )
                if ($null -eq $GizmoEscapePreview -or @($events).Count -ne 2 -or
                    [uint64]$gizmo.lastTerminalFrame -le [uint64]$GizmoEscapePreview.inputFrame -or
                    [uint64]$gizmo.lastTerminalFrame -gt [uint64]$gizmo.inputFrame) {
                    throw "Same-frame terminal batch lacks its actual post-boundary input frame"
                }
                $previousRoutingSequence = [uint64]0
                $previousCallbackSequence = [uint64]$GizmoEscapePreview.nativeCallbackSequence
                for ($index = 0; $index -lt 2; ++$index) {
                    $event = $events[$index]
                    if ([string]$event.kind -ne $expectedKinds[$index] -or
                        [uint64]$event.sequence -le $previousRoutingSequence -or
                        [uint64]$event.callbackSequence -le $previousCallbackSequence -or
                        [uint64]$event.callbackSequence -gt [uint64]$gizmo.nativeCallbackSequence) {
                        throw "Same-frame terminal journal lost native callback arrival order or provenance"
                    }
                    if ([string]$event.kind -eq "escape-pressed" -and
                        [uint64]$gizmo.lastTerminalSequence -ne [uint64]$event.sequence) {
                        throw "Same-frame detach sequence is not its actual Escape event"
                    }
                    $previousRoutingSequence = [uint64]$event.sequence
                    $previousCallbackSequence = [uint64]$event.callbackSequence
                }
            }
            elseif (@($events).Count -ne 1 -or
                [string]$events[0].kind -ne "escape-pressed" -or
                [uint64]$events[0].sequence -ne [uint64]$gizmo.lastTerminalSequence) {
                throw "Native Escape detach lacks its exact single Escape terminal event"
            }
            $journal = [ordered]@{
                frame = [uint64]$gizmo.lastTerminalFrame
                sequence = [uint64]$gizmo.lastTerminalSequence
                events = $events
            } | ConvertTo-Json -Compress -Depth 8
            if ($null -eq $GizmoDetachedJournal) {
                $script:GizmoDetachedJournal = $journal
            }
            elseif ($journal -ne $GizmoDetachedJournal) {
                throw "Detached native gesture terminal journal changed after acceptance"
            }
            Test-GizmoTrsEqual $gizmo $GizmoEscapePreview
        }
        elseif ([int64]$gizmo.escapePressCount -ne 0 -or
            [string]$gizmo.terminal -ne "" -or @($gizmo.lastTerminalEvents).Count -ne 0) {
            throw "Native Escape terminal was observed before its input phase"
        }
    }
    elseif ([int64]$gizmo.commitCount -gt 0) {
        if ([string]$gizmo.terminal -ne "commit" -or
            [uint64]$gizmo.lastTerminalFrame -eq 0 -or
            [uint64]$gizmo.lastTerminalSequence -eq 0) {
            throw "Native gizmo commit lacks its observed terminal event"
        }
    }
    elseif ([string]$gizmo.terminal -ne "") {
        throw "Native gizmo reported an unexpected terminal event"
    }
    if (-not $escapeCase -and [string]$Step.id -eq "final-boundary" -and
        ([int64]$gizmo.beginCount -ne 1 -or
         [int64]$gizmo.previewCount -ne 1 -or
         [int64]$gizmo.commitCount -ne 1 -or [bool]$gizmo.captureHeld)) {
        throw "Native gizmo final action/capture boundary is incomplete"
    }
}

function Test-GizmoTrsEqual([object]$Actual, [object]$Expected) {
    if ($null -eq $Actual -or $null -eq $Expected) {
        throw "Native gizmo transform comparison lacks an observation"
    }
    foreach ($field in @("translation", "rotation", "scale")) {
        if (@($Actual.$field).Count -ne @($Expected.$field).Count) {
            throw "Native gizmo transform vector size changed"
        }
        for ($index = 0; $index -lt @($Expected.$field).Count; ++$index) {
            if ([Math]::Abs([double]$Actual.$field[$index] - [double]$Expected.$field[$index]) -gt 0.000001) {
                throw "Native gizmo history did not restore the captured transform"
            }
        }
    }
}

function Test-ObservedDispatches(
    [object]$Ready,
    [object]$Step,
    [object]$Phase) {
    $observed = @($Ready.observedDispatches)
    $expected = @($Step.expectedDispatches)
    if (@($observed).Count -gt @($expected).Count) {
        throw "Ready observed excess SceneEditorUi dispatches"
    }
    $previousSequence = 0L
    for ($index = 0; $index -lt @($observed).Count; ++$index) {
        $actual = $observed[$index]
        $wanted = $expected[$index]
        if ([int64]$actual.sequence -le $previousSequence -or
            [string]$actual.source -ne [string]$wanted.source -or
            [string]$actual.payloadKind -ne [string]$wanted.payloadKind -or
            [string]$actual.operation -ne [string]$wanted.operation -or
            [string]$actual.targetRole -ne [string]$wanted.targetRole -or
            [string]$actual.secondaryRole -ne [string]$wanted.secondaryRole -or
            ((-not ([string]$wanted.text -eq "" -and [string]$wanted.value -eq "")) -and
             [string]$actual.text -ne [string]$wanted.text) -or
            ($Case -ne "U02-hierarchy" -and
             ([string]$actual.propertyPhase -ne [string]$wanted.phase -or
              (([string]$wanted.value -ne "") -and
               [string]$actual.propertyValue -ne [string]$wanted.value))) -or
            [bool]$actual.accepted -ne [bool]$wanted.accepted -or
            [string]$actual.refresh -ne [string]$wanted.refresh -or
            -not [string]::IsNullOrEmpty([string]$actual.error)) {
            throw "Ready SceneEditorUi dispatch summary differs at index $index"
        }
        $previousSequence = [int64]$actual.sequence
    }
    if ($null -ne $Phase.PSObject.Properties["expectedDispatchCount"] -and
        @($observed).Count -ne [int]$Phase.expectedDispatchCount) {
        throw "Ready dispatch cardinality differs from the explicit phase prefix"
    }
    if ([string]$Phase.phase -in @("settled", "boundary") -and
        @($observed).Count -ne @($expected).Count) {
        throw "Terminal ready lacks exact expected dispatch cardinality"
    }
}

function Get-PixelAudit([Drawing.Bitmap]$Bitmap, [object]$ViewportMask = $null) {
    $maskedViewport = $Case -in @("U03-gizmo-translate", "U03-gizmo-escape", "U03-gizmo-escape-release", "U03-gizmo-release-escape", "U03-gizmo-focus-no-release", "U03-gizmo-focus-same-frame", "U03-gizmo-focus-delivered-release")
    if ($maskedViewport) {
        if ($null -eq $ViewportMask -or
            [string]$ViewportMask.source -ne "editor-gizmo-adapter-v1" -or
            [uint64]$ViewportMask.renderedFrame -eq 0 -or
            [string]$ViewportMask.coordinateSpace -ne "client-physical-px" -or
            [int]$ViewportMask.left -lt 0 -or [int]$ViewportMask.top -lt 0 -or
            [int]$ViewportMask.right -gt $Bitmap.Width -or
            [int]$ViewportMask.bottom -gt $Bitmap.Height -or
            [int]$ViewportMask.right -le [int]$ViewportMask.left -or
            [int]$ViewportMask.bottom -le [int]$ViewportMask.top) {
            throw "Native viewport pixel audit lacks its observed physical mask"
        }
    }
    elseif ($null -ne $ViewportMask) {
        throw "Viewport pixel mask is outside its scoped native replay case"
    }
    $sampled = 0; $nonBlack = 0; $colorful = 0
    $colors = [Collections.Generic.HashSet[int]]::new()
    $zoneSamples = [int[]]::new(9)
    $zoneNonBlack = [int[]]::new(9)
    $zoneColors = [Collections.Generic.List[object]]::new()
    $zoneUiSamples = [int[]]::new(9)
    $zoneUiNonBlack = [int[]]::new(9)
    $zoneUiColors = [Collections.Generic.List[object]]::new()
    $viewportSamples = 0; $viewportNonBlack = 0; $viewportColorful = 0
    $viewportFilledColorfulBlocks = 0
    $viewportColors = [Collections.Generic.HashSet[int]]::new()
    for ($zoneIndex = 0; $zoneIndex -lt 9; ++$zoneIndex) {
        $zoneColors.Add([Collections.Generic.HashSet[int]]::new())
        $zoneUiColors.Add([Collections.Generic.HashSet[int]]::new())
    }
    for ($y = 0; $y -lt $Bitmap.Height; $y += 4) {
        for ($x = 0; $x -lt $Bitmap.Width; $x += 4) {
            $pixel = $Bitmap.GetPixel($x, $y)
            $max = [Math]::Max($pixel.R, [Math]::Max($pixel.G, $pixel.B))
            $min = [Math]::Min($pixel.R, [Math]::Min($pixel.G, $pixel.B))
            $zoneX = [Math]::Min(2, [int][Math]::Floor(
                (3.0 * $x) / $Bitmap.Width))
            $zoneY = [Math]::Min(2, [int][Math]::Floor(
                (3.0 * $y) / $Bitmap.Height))
            $zone = $zoneY * 3 + $zoneX
            # Windows PowerShell 5.1 keeps the operand type for -shl, so a
            # [byte] shifted by 16 or by 8 truncates to 0 and $packed
            # degenerates to the blue channel alone.  The same frame measures
            # 141 distinct values with the byte form and 562 with the [int]
            # form, matching an independent PIL count.  The $nativePacked
            # branch below already used the [int] form, so the two packings
            # disagreed by construction.  Every uniqueColors value recorded
            # before this fix is a blue-channel count, not a colour count;
            # the blank gates here are lower bounds, so the corrected counts
            # only make them stricter in information, never in acceptance.
            $packed = ([int]$pixel.R -shl 16) -bor
                ([int]$pixel.G -shl 8) -bor [int]$pixel.B
            ++$sampled
            ++$zoneSamples[$zone]
            if ($max -gt 20) {
                ++$nonBlack
                ++$zoneNonBlack[$zone]
            }
            $isColorful = ($max - $min) -gt 20 -and $max -gt 40
            if ($isColorful) { ++$colorful }
            [void]$colors.Add($packed)
            [void]$zoneColors[$zone].Add($packed)
            if ($maskedViewport) {
                $nativePacked = ([int]$pixel.R -shl 16) -bor
                    ([int]$pixel.G -shl 8) -bor [int]$pixel.B
                $inViewport = $x -ge [int]$ViewportMask.left -and
                    $x -lt [int]$ViewportMask.right -and
                    $y -ge [int]$ViewportMask.top -and
                    $y -lt [int]$ViewportMask.bottom
                if ($inViewport) {
                    ++$viewportSamples
                    if ($max -gt 20) { ++$viewportNonBlack }
                    if ($isColorful) { ++$viewportColorful }
                    [void]$viewportColors.Add($nativePacked)
                    # Filled sample blocks distinguish visible model surfaces from thin axes.
                    if ($isColorful -and $x + 4 -lt [int]$ViewportMask.right -and
                        $y + 4 -lt [int]$ViewportMask.bottom) {
                        $filled = $true
                        foreach ($offset in @(@(4, 0), @(0, 4), @(4, 4))) {
                            $neighbor = $Bitmap.GetPixel($x + $offset[0], $y + $offset[1])
                            $neighborMax = [Math]::Max($neighbor.R, [Math]::Max($neighbor.G, $neighbor.B))
                            $neighborMin = [Math]::Min($neighbor.R, [Math]::Min($neighbor.G, $neighbor.B))
                            if ($neighborMax -le 40 -or $neighborMax - $neighborMin -le 20) {
                                $filled = $false
                                break
                            }
                        }
                        if ($filled) { ++$viewportFilledColorfulBlocks }
                    }
                }
                else {
                    ++$zoneUiSamples[$zone]
                    if ($max -gt 20) { ++$zoneUiNonBlack[$zone] }
                    [void]$zoneUiColors[$zone].Add($nativePacked)
                }
            }
        }
    }
    if ($nonBlack -lt 500 -or $colorful -lt 20 -or $colors.Count -lt 16) {
        throw "OS replay PNG is blank or low-information: sampled=$sampled nonBlack=$nonBlack colorful=$colorful colors=$($colors.Count)"
    }
    if ($maskedViewport -and
        ($viewportNonBlack -lt 128 -or $viewportColorful -lt 64 -or
         $viewportColors.Count -lt 3 -or $viewportFilledColorfulBlocks -lt 16)) {
        throw "Observed viewport PNG is blank or lacks visible model surfaces: sampled=$viewportSamples nonBlack=$viewportNonBlack colorful=$viewportColorful colors=$($viewportColors.Count) filledBlocks=$viewportFilledColorfulBlocks"
    }
    $zones = [Collections.Generic.List[object]]::new()
    for ($zone = 0; $zone -lt 9; ++$zone) {
        $requiredSamples = if ($maskedViewport) { $zoneUiSamples[$zone] } else { $zoneSamples[$zone] }
        $actualNonBlack = if ($maskedViewport) { $zoneUiNonBlack[$zone] } else { $zoneNonBlack[$zone] }
        $actualColors = if ($maskedViewport) { $zoneUiColors[$zone].Count } else { $zoneColors[$zone].Count }
        $minimumNonBlack = [Math]::Max(
            8, [int][Math]::Ceiling($requiredSamples * 0.002))
        $minimumColors = 2
        if ($maskedViewport) {
            $minimumNonBlack = [Math]::Min($requiredSamples, $minimumNonBlack)
            $minimumColors = [Math]::Min(2, $requiredSamples)
        }
        if ($requiredSamples -gt 0 -and
            ($actualNonBlack -lt $minimumNonBlack -or
             $actualColors -lt $minimumColors)) {
            throw "OS replay PNG has an uninformative zone: zone=$zone sampled=$($zoneSamples[$zone]) nonBlack=$($zoneNonBlack[$zone]) colors=$($zoneColors[$zone].Count)"
        }
        $zoneRecord = [ordered]@{
            index = $zone
            sampled = $zoneSamples[$zone]
            nonBlack = $zoneNonBlack[$zone]
            uniqueColors = $zoneColors[$zone].Count
        }
        if ($maskedViewport) {
            $zoneRecord.viewportSamples = $zoneSamples[$zone] - $zoneUiSamples[$zone]
            $zoneRecord.uiSamples = $zoneUiSamples[$zone]
            $zoneRecord.uiNonBlack = $zoneUiNonBlack[$zone]
            $zoneRecord.uiUniqueColors = $zoneUiColors[$zone].Count
            $zoneRecord.rule = if ($requiredSamples -gt 0) {
                "ui-outside-observed-viewport"
            } else { "observed-viewport-only" }
        }
        $zones.Add($zoneRecord)
    }
    $result = [ordered]@{
        sampled = $sampled; nonBlack = $nonBlack
        colorful = $colorful; uniqueColors = $colors.Count
        zones = @($zones)
    }
    if ($maskedViewport) {
        $result.viewport = [ordered]@{
            mask = $ViewportMask
            sampled = $viewportSamples; nonBlack = $viewportNonBlack
            colorful = $viewportColorful; uniqueColors = $viewportColors.Count
            filledColorfulBlocks = $viewportFilledColorfulBlocks
            minimumNonBlack = 128; minimumColorful = 64
            minimumUniqueColors = 3; minimumFilledColorfulBlocks = 16
        }
    }
    return $result
}

function Get-VulkanLayerFrame(
    [object]$ReadyRecord,
    [object]$Client,
    [string]$DestinationPath) {
    if (-not $UseVulkanScreenshotLayer -or
        [string]::IsNullOrWhiteSpace([string]$VulkanLayerRunRoot) -or
        $null -eq $ReadyRecord) {
        throw "Vulkan screenshot-layer fallback was not initialized"
    }
    $readyPath = [string]$ReadyRecord.path
    [void](Get-EvidenceRelativePath $VulkanLayerRunRoot)
    [void](Get-EvidenceRelativePath $readyPath)
    $readyFile = Get-Item -LiteralPath $readyPath -Force
    if (($readyFile.Attributes -band [IO.FileAttributes]::ReparsePoint) -ne 0) {
        throw "Ready boundary traverses a reparse point"
    }
    $readyBytes = [IO.File]::ReadAllBytes($readyPath)
    $readyHash = Get-BytesHash $readyBytes
    if ($readyBytes.Length -ne [int]$ReadyRecord.bytes -or
        $readyHash -ne [string]$ReadyRecord.sha256) {
        throw "Ready boundary changed before Vulkan screenshot-layer capture"
    }
    $readyWriteUtc = $readyFile.LastWriteTimeUtc
    $frameSerial = [int64]$ReadyRecord.value.frame.frameSerial
    if ($frameSerial -le 0) {
        throw "Ready boundary has no positive application frame serial"
    }
    $expectedIndex = $frameSerial - 1
    if ($expectedIndex -le [int64]$LastVulkanLayerFrameIndex) {
        throw "Vulkan screenshot-layer frame index regressed or was reused"
    }
    $candidatePath = Join-Path $VulkanLayerRunRoot ("{0}.ppm" -f $expectedIndex)
    $deadline = [DateTime]::UtcNow.AddSeconds(8)
    $lastError = "expected Vulkan screenshot-layer frame is not complete"
    while ([DateTime]::UtcNow -lt $deadline) {
        if (-not (Test-Path -LiteralPath $candidatePath -PathType Leaf)) {
            Start-Sleep -Milliseconds 20
            continue
        }
        $candidate = Get-Item -LiteralPath $candidatePath -Force
        if (($candidate.Attributes -band [IO.FileAttributes]::ReparsePoint) -ne 0) {
            throw "Vulkan screenshot-layer frame traverses a reparse point"
        }
        $length = [int64]$candidate.Length
        if ($length -lt 16) {
            Start-Sleep -Milliseconds 20
            continue
        }
        try {
            $sourceBytesBefore = [IO.File]::ReadAllBytes($candidate.FullName)
            $sourceHashBefore = Get-BytesHash $sourceBytesBefore
            $sourceLengthBefore = $sourceBytesBefore.Length
            Start-Sleep -Milliseconds 8
            $candidate.Refresh()
            if ([int64]$candidate.Length -ne $sourceLengthBefore) {
                throw "Vulkan screenshot-layer frame is still being written"
            }
            $sourceBytesAfter = [IO.File]::ReadAllBytes($candidate.FullName)
            if ($sourceBytesAfter.Length -ne $sourceLengthBefore -or
                (Get-BytesHash $sourceBytesAfter) -ne $sourceHashBefore) {
                throw "Vulkan screenshot-layer frame changed during capture"
            }
            $writeUtc = $candidate.LastWriteTimeUtc
            if ($writeUtc -lt $VulkanLayerProcessStartUtc.AddSeconds(-2) -or
                $writeUtc -gt [DateTime]::UtcNow.AddSeconds(1) -or
                $writeUtc -gt $readyWriteUtc.AddMilliseconds(250)) {
                throw "Vulkan screenshot-layer frame is outside the ready boundary"
            }
            $temporaryPng = $DestinationPath + ".tmp"
            if (Test-Path -LiteralPath $temporaryPng -PathType Leaf) {
                [IO.File]::Delete($temporaryPng)
            }
            [MGIFVulkanScreenshot]::ConvertPpm(
                $candidate.FullName, $temporaryPng,
                [int]$Client.width, [int]$Client.height)
            $convertedBytes = [IO.File]::ReadAllBytes($temporaryPng)
            if ($convertedBytes.Length -eq 0) {
                throw "Vulkan screenshot-layer converter produced an empty PNG"
            }
            $sourceBytesAfter = [IO.File]::ReadAllBytes($candidate.FullName)
            if ($sourceBytesAfter.Length -ne $sourceLengthBefore -or
                (Get-BytesHash $sourceBytesAfter) -ne $sourceHashBefore) {
                throw "Vulkan screenshot-layer source changed during conversion"
            }
            Move-Item -LiteralPath $temporaryPng -Destination $DestinationPath
            $persistedConvertedBytes = [IO.File]::ReadAllBytes($DestinationPath)
            if ($persistedConvertedBytes.Length -ne $convertedBytes.Length -or
                (Get-BytesHash $persistedConvertedBytes) -ne
                    (Get-BytesHash $convertedBytes)) {
                throw "Converted Vulkan screenshot PNG changed after atomic rename"
            }
            $convertedBytes = $persistedConvertedBytes
            $readyAfter = [IO.File]::ReadAllBytes($readyPath)
            if ($readyAfter.Length -ne $readyBytes.Length -or
                (Get-BytesHash $readyAfter) -ne $readyHash) {
                throw "Ready boundary changed during Vulkan screenshot-layer conversion"
            }
            $script:LastVulkanLayerFrameIndex = $expectedIndex
            $record = [ordered]@{
                index = $expectedIndex
                path = Get-EvidenceRelativePath $candidate.FullName
                bytes = $sourceLengthBefore
                sha256 = $sourceHashBefore
                sourceFormat = "P6"
                width = [int]$Client.width
                height = [int]$Client.height
                maxValue = 255
                sourceChannelOrder = "BGR"
                destinationChannelOrder = "RGB"
                orientation = "top-left"
                rowOrder = "top-left"
                alpha = "opaque"
                converter = "mgif-vulkan-ppm-to-png-v1"
                writeTimeUtc = $writeUtc.ToString("o",
                    [Globalization.CultureInfo]::InvariantCulture)
                readyWriteTimeUtc = $readyWriteUtc.ToString("o",
                    [Globalization.CultureInfo]::InvariantCulture)
                deltaMilliseconds = ($writeUtc - $readyWriteUtc).TotalMilliseconds
                readySha256 = $readyHash
                readyStep = [int]$ReadyRecord.value.step.index
                readyPhase = [string]$ReadyRecord.value.step.phase
                readyFrameSerial = $frameSerial
                processPid = [int]$Process.Id
                runNonce = $RunNonce
                convertedPng = [ordered]@{
                    path = Get-EvidenceRelativePath $DestinationPath
                    bytes = $convertedBytes.Length
                    sha256 = Get-BytesHash $convertedBytes
                }
                frameFence = [ordered]@{
                    relation = "vulkan-present-index=ready.frame.frameSerial-1"
                    expectedIndex = $expectedIndex
                    hostContractRequiresGpuIdle = $true
                    readyBytesStable = $true
                    sourceBytesStable = $true
                }
            }
            $VulkanLayerFrameRecords.Add($record)
            return $record
        }
        catch {
            $lastError = $_.Exception.Message
            $temporaryPng = $DestinationPath + ".tmp"
            if (Test-Path -LiteralPath $temporaryPng -PathType Leaf) {
                [IO.File]::Delete($temporaryPng)
            }
            if (Test-Path -LiteralPath $DestinationPath -PathType Leaf) {
                [IO.File]::Delete($DestinationPath)
            }
            Start-Sleep -Milliseconds 20
        }
    }
    throw "Vulkan screenshot-layer fallback could not produce a validated PNG: $lastError"
}

function Capture-Client(
    [IntPtr]$Hwnd,
    [string]$Path,
    [int]$TargetX,
    [int]$TargetY,
    [object]$ViewportRect = $null,
    [object]$ReadyRecord = $null) {
    $client = Get-ClientContract $Hwnd
    if ($client.width -ne $Width -or $client.height -ne $Height) {
        throw "PNG client extent changed before capture"
    }
    $viewportMask = $null
    if ($Case -in @("U03-gizmo-translate", "U03-gizmo-escape", "U03-gizmo-escape-release", "U03-gizmo-release-escape", "U03-gizmo-focus-no-release", "U03-gizmo-focus-same-frame", "U03-gizmo-focus-delivered-release")) {
        if ($null -eq $ViewportRect -or
            [string]$ViewportRect.coordinateSpace -ne "screen-physical-px" -or
            [string]$ViewportRect.source -ne "editor-gizmo-adapter-v1" -or
            [int]$ViewportRect.clientLeft -ne $client.left -or
            [int]$ViewportRect.clientTop -ne $client.top -or
            [int]$ViewportRect.clientRight -ne $client.right -or
            [int]$ViewportRect.clientBottom -ne $client.bottom) {
            throw "PNG observed viewport/client binding changed before capture"
        }
        $viewportMask = [ordered]@{
            source = [string]$ViewportRect.source
            renderedFrame = [uint64]$ViewportRect.renderedFrame
            coordinateSpace = "client-physical-px"
            left = [int]$ViewportRect.left - $client.left
            top = [int]$ViewportRect.top - $client.top
            right = [int]$ViewportRect.right - $client.left
            bottom = [int]$ViewportRect.bottom - $client.top
        }
    }
    $bitmap = [Drawing.Bitmap]::new($client.width, $client.height)
    $graphics = [Drawing.Graphics]::FromImage($bitmap)
    $transport = "none"; $audit = $null; $captureBefore = $null
    $captureAfter = $null; $lastCaptureError = "no transport succeeded"
    $layerFrame = $null
    try {
        if (-not $UseVulkanScreenshotLayer) {
        if ([MGIFEditorOsNative]::DwmFlush() -ne 0) {
            throw "DwmFlush failed before PrintWindow capture"
        }
        $captureBefore = Test-CaptureWindowState `
            $Hwnd $client $TargetX $TargetY
        $graphics.Clear([Drawing.Color]::Black)
        $destination = $graphics.GetHdc()
        if ($destination -eq [IntPtr]::Zero) {
            throw "Could not acquire destination HDC for PrintWindow"
        }
        try {
            $printed = [MGIFEditorOsNative]::PrintWindow(
                $Hwnd, $destination, 0x00000003)
        }
        finally { $graphics.ReleaseHdc($destination) }
        if ($printed) {
            try {
                $audit = Get-PixelAudit $bitmap $viewportMask
                $transport = "print-window"
            }
            catch {
                $lastCaptureError = $_.Exception.Message
                $audit = $null
            }
        }
        else {
            $lastCaptureError = "PrintWindow returned false"
        }
        }
        if (-not $UseVulkanScreenshotLayer -and $null -eq $audit) {
            if ([MGIFEditorOsNative]::DwmFlush() -ne 0) {
                throw "DwmFlush failed before client DC capture"
            }
            $captureBefore = Test-CaptureWindowState `
                $Hwnd $client $TargetX $TargetY
            $graphics.Clear([Drawing.Color]::Black)
            $source = [MGIFEditorOsNative]::GetDC($Hwnd)
            if ($source -ne [IntPtr]::Zero) {
                $destination = $graphics.GetHdc()
                if ($destination -eq [IntPtr]::Zero) {
                    throw "Could not acquire destination HDC for client capture"
                }
                try {
                    $copied = [MGIFEditorOsNative]::BitBlt(
                        $destination, 0, 0, $client.width, $client.height,
                        $source, 0, 0, 0x00CC0020)
                }
                finally {
                    $graphics.ReleaseHdc($destination)
                    [void][MGIFEditorOsNative]::ReleaseDC($Hwnd, $source)
                }
                if ($copied) {
                    try {
                        $audit = Get-PixelAudit $bitmap $viewportMask
                        $transport = "client-dc"
                    }
                    catch {
                        $lastCaptureError = $_.Exception.Message
                        $audit = $null
                    }
                }
                else {
                    $lastCaptureError = "Client DC BitBlt returned false"
                }
            }
            else {
                $lastCaptureError = "Could not acquire client source HDC"
            }
        }
        if (-not $UseVulkanScreenshotLayer -and $null -eq $audit) {
            if ([MGIFEditorOsNative]::DwmFlush() -ne 0) {
                throw "DwmFlush failed before desktop DC capture"
            }
            $captureBefore = Test-CaptureWindowState `
                $Hwnd $client $TargetX $TargetY
            $graphics.Clear([Drawing.Color]::Black)
            $source = [MGIFEditorOsNative]::GetDC([IntPtr]::Zero)
            if ($source -ne [IntPtr]::Zero) {
                $destination = $graphics.GetHdc()
                if ($destination -eq [IntPtr]::Zero) {
                    throw "Could not acquire destination HDC for desktop capture"
                }
                try {
                    $copied = [MGIFEditorOsNative]::BitBlt(
                        $destination, 0, 0, $client.width, $client.height,
                        $source, $client.left, $client.top,
                        [uint32]0x40CC0020)
                }
                finally {
                    $graphics.ReleaseHdc($destination)
                    [void][MGIFEditorOsNative]::ReleaseDC(
                        [IntPtr]::Zero, $source)
                }
                if ($copied) {
                    try {
                        $audit = Get-PixelAudit $bitmap $viewportMask
                        $transport = "desktop-dc-client-rect"
                    }
                    catch {
                        $lastCaptureError = $_.Exception.Message
                        $audit = $null
                    }
                }
                else {
                    $lastCaptureError = "Desktop DC BitBlt returned false"
                }
            }
            else {
                $lastCaptureError = "Could not acquire desktop source HDC"
            }
        }
        if ($null -eq $audit) {
            if ($UseVulkanScreenshotLayer) {
                try {
                    if ([MGIFEditorOsNative]::DwmFlush() -ne 0) {
                        throw "DwmFlush failed before Vulkan screenshot-layer capture"
                    }
                    $captureBefore = Test-CaptureWindowState `
                        $Hwnd $client $TargetX $TargetY
                    $layerFrame = Get-VulkanLayerFrame $ReadyRecord $client $Path
                    $layerBitmap = [Drawing.Bitmap]::new($Path)
                    try {
                        $audit = Get-PixelAudit $layerBitmap $viewportMask
                        $transport = "vulkan-screenshot-layer"
                    }
                    finally { $layerBitmap.Dispose() }
                }
                catch {
                    $lastCaptureError = $_.Exception.Message
                    $audit = $null
                    $layerFrame = $null
                }
            }
        }
        if ($null -eq $audit) {
            try {
                $bitmap.Save(
                    ($Path + ".failed-desktop.png"),
                    [Drawing.Imaging.ImageFormat]::Png)
            }
            catch { }
            throw "Could not capture informative client PNG: $lastCaptureError"
        }
        if ([MGIFEditorOsNative]::DwmFlush() -ne 0) {
            throw "DwmFlush failed after client capture"
        }
        $captureAfter = Test-CaptureWindowState `
            $Hwnd $client $TargetX $TargetY
        if (-not (Test-SameClientContract `
                $captureBefore.client $captureAfter.client)) {
            throw "Capture client contract changed during pixel acquisition"
        }
        if ($transport -ne "vulkan-screenshot-layer") {
            $bitmap.Save($Path, [Drawing.Imaging.ImageFormat]::Png)
        }
    }
    finally { $graphics.Dispose(); $bitmap.Dispose() }

    $persisted = [Drawing.Bitmap]::new($Path)
    try {
        if ($persisted.Width -ne $client.width -or
            $persisted.Height -ne $client.height) {
            throw "Persisted PNG dimensions differ from the client"
        }
        $persistedAudit = Get-PixelAudit $persisted $viewportMask
        if ($persistedAudit.sampled -ne $audit.sampled -or
            $persistedAudit.nonBlack -ne $audit.nonBlack -or
            $persistedAudit.colorful -ne $audit.colorful -or
            $persistedAudit.uniqueColors -ne $audit.uniqueColors -or
            (($persistedAudit.zones | ConvertTo-Json -Compress -Depth 8) -ne
             ($audit.zones | ConvertTo-Json -Compress -Depth 8))) {
            throw "Persisted PNG pixels differ from the audited bitmap"
        }
        if ($Case -in @("U03-gizmo-translate", "U03-gizmo-escape", "U03-gizmo-escape-release", "U03-gizmo-release-escape", "U03-gizmo-focus-no-release", "U03-gizmo-focus-same-frame", "U03-gizmo-focus-delivered-release") -and
            ($persistedAudit.viewport | ConvertTo-Json -Compress -Depth 8) -ne
                ($audit.viewport | ConvertTo-Json -Compress -Depth 8)) {
            throw "Persisted PNG viewport mask counts differ from the audited bitmap"
        }
    }
    finally { $persisted.Dispose() }
    return [ordered]@{
        path = $Path; width = $client.width; height = $client.height
        transport = $transport; audit = $audit
        captureBefore = $captureBefore; captureAfter = $captureAfter
        layerFrame = $layerFrame
        bytes = (Get-Item -LiteralPath $Path).Length; sha256 = Get-Hash $Path
    }
}

function Quote-Argument([string]$Value) {
    return '"' + $Value.Replace('"', '\"') + '"'
}

$RunNonce = ""
$CurrentSubcase = ""
$CurrentStartStep = -1
$CurrentStopStep = -1
$CurrentProcessRecorded = $false
$CurrentFixtureRelative = ""
$CurrentFixturePath = ""
$Process = $null
$StdoutTask = $null
$StderrTask = $null
$Deadline = [DateTime]::MinValue
$WindowHandle = [IntPtr]::Zero
$ObservedDpi = 0
$ConsumedReady = [Collections.Generic.HashSet[string]]::new(
    [StringComparer]::OrdinalIgnoreCase)
$ReadyRecords = [Collections.Generic.List[object]]::new()
$ContinueRecords = [Collections.Generic.List[object]]::new()
$ScreenshotRecords = [Collections.Generic.List[object]]::new()
$InputRecords = [Collections.Generic.List[object]]::new()
$BindingRecords = [Collections.Generic.List[object]]::new()
$CleanupInputRecords = [Collections.Generic.List[object]]::new()
$ProcessRecords = [Collections.Generic.List[object]]::new()
$StdoutSegments = [Collections.Generic.List[string]]::new()
$StderrSegments = [Collections.Generic.List[string]]::new()
$ProcessGraphKeys = @{}
$LeftButtonHeld = $false
$RightButtonHeld = $false
$HeldKeys = [Collections.Generic.HashSet[string]]::new([StringComparer]::Ordinal)
$GizmoEscapePreview = $null
$GizmoDetachedJournal = $null
$GizmoLastCallbackClock = [uint64]0
$GizmoLastDeliveredReleaseCount = [uint64]0
$GizmoLastEscapePressCount = [uint64]0
$GizmoLastFocusLossCount = [uint64]0
$GizmoLastSuppressedReleaseCount = [uint64]0
$LastState = $null
$UniqueRootSelectionState = $null
$FocusSink = $null
$FocusSinkActive = $false
$FocusTransferRecords = [Collections.Generic.List[object]]::new()
$StartupActivationRecords = [Collections.Generic.List[object]]::new()
$VulkanLayerRunRoot = $null
$LastVulkanLayerFrameIndex = -1
$VulkanLayerProcessStartUtc = [DateTime]::MinValue
$VulkanLayerEnvironmentRecords = [Collections.Generic.List[object]]::new()
$LastInputDeliveryFence = "none"

function Get-ExpectedForeground {
    if ($FocusSinkActive -and $null -ne $FocusSink) { return $FocusSink.Handle }
    return $WindowHandle
}

function Move-FocusToSink {
    if ($null -eq $script:FocusSink) {
        $script:FocusSink = [Windows.Forms.Form]::new()
        $FocusSink.FormBorderStyle = [Windows.Forms.FormBorderStyle]::FixedToolWindow
        $FocusSink.ShowInTaskbar = $false
        $FocusSink.StartPosition = [Windows.Forms.FormStartPosition]::Manual
        $FocusSink.Size = [Drawing.Size]::new(160, 60)
        $FocusSink.Location = [Drawing.Point]::new(8, 8)
        $FocusSink.Text = "MGIF replay focus sink"
    }
    $FocusSink.Show()
    $FocusSink.Activate()
    [Windows.Forms.Application]::DoEvents()
    [void][MGIFEditorOsNative]::SetForegroundWindow($FocusSink.Handle)
    if ([MGIFEditorOsNative]::GetForegroundWindow() -ne $FocusSink.Handle) {
        throw "OS replay focus sink could not acquire foreground"
    }
    $script:FocusSinkActive = $true
    $FocusTransferRecords.Add([ordered]@{
        operation = "focus-loss"; editorHwnd = $WindowHandle.ToInt64()
        recipientHwnd = $FocusSink.Handle.ToInt64(); leftButtonHeld = $LeftButtonHeld
        releaseInjected = $false
    })
}

function Restore-EditorFocus {
    if (-not $FocusSinkActive) { return }
    $FocusSink.Hide()
    [void][MGIFEditorOsNative]::SetForegroundWindow($WindowHandle)
    if ([MGIFEditorOsNative]::GetForegroundWindow() -ne $WindowHandle) {
        throw "OS replay editor could not reacquire foreground"
    }
    $script:FocusSinkActive = $false
    $FocusTransferRecords.Add([ordered]@{
        operation = "focus-restored"; editorHwnd = $WindowHandle.ToInt64()
        recipientHwnd = $WindowHandle.ToInt64(); leftButtonHeld = $LeftButtonHeld
        releaseInjected = $false
    })
}

function New-RunNonce {
    $bytes = New-Object byte[] 16
    $generator = [Security.Cryptography.RandomNumberGenerator]::Create()
    try { $generator.GetBytes($bytes) } finally { $generator.Dispose() }
    return ([BitConverter]::ToString($bytes)).Replace("-", "").ToLowerInvariant()
}

function Start-ReplayProcess(
    [string]$Subcase,
    [int]$StartStep,
    [int]$StopStep) {
    $script:RunNonce = New-RunNonce
    $script:CurrentSubcase = $Subcase
    $script:CurrentStartStep = $StartStep
    $script:CurrentStopStep = $StopStep
    $script:CurrentProcessRecorded = $false
    $script:WindowHandle = [IntPtr]::Zero
    $script:ProcessGraphKeys = @{}
    $script:GizmoLastCallbackClock = 0
    $script:GizmoLastDeliveredReleaseCount = 0
    $script:GizmoLastEscapePressCount = 0
    $script:GizmoLastFocusLossCount = 0
    $script:GizmoLastSuppressedReleaseCount = 0
    $script:Deadline = [DateTime]::UtcNow.AddSeconds($TimeoutSeconds)
    $runEvidence = Join-Path $AppEvidence $Subcase
    if ($UseVulkanScreenshotLayer) {
        $script:VulkanLayerRunRoot = Join-Path $VulkanLayerOutputRoot `
            ("{0}-{1}" -f $Subcase, $RunNonce)
        New-Item -ItemType Directory -Path $VulkanLayerRunRoot | Out-Null
        $runLayerDirectory = Get-Item -LiteralPath $VulkanLayerRunRoot -Force
        if (($runLayerDirectory.Attributes -band [IO.FileAttributes]::ReparsePoint) -ne 0) {
            throw "Vulkan screenshot layer run directory cannot be a reparse point"
        }
        $script:LastVulkanLayerFrameIndex = -1
    }
    $script:VulkanLayerProcessStartUtc = [DateTime]::UtcNow
    $fixtureRelative = if ($Subcase -eq "unique-root") {
        "tests/fixtures/gltf_editor/minimal_scene.gltf"
    }
    else {
        "tests/fixtures/gltf_editor/r09_shared_mesh.gltf"
    }
    $fixturePath = Join-Path $RepoRoot $fixtureRelative
    if (-not (Test-Path -LiteralPath $fixturePath -PathType Leaf)) {
        throw "OS replay subcase fixture is missing: $fixtureRelative"
    }
    $script:CurrentFixtureRelative = $fixtureRelative
    $script:CurrentFixturePath = $fixturePath
    $arguments = @(
        "--editor-os-case=$Case",
        "--editor-os-subcase=$Subcase",
        "--editor-os-start-step=$StartStep",
        "--editor-os-stop-step=$StopStep",
        "--editor-os-evidence-dir=$runEvidence",
        "--editor-os-sync-dir=$SyncRoot",
        "--editor-os-run-nonce=$RunNonce",
        "--editor-os-plan-sha256=$PlanContractHash",
        "--editor-os-executable-sha256=$ExecutableHash",
        "--editor-os-source-sha256=$SourceIdentityHash",
        "--editor-os-timeout-ms=$($TimeoutSeconds * 1000)",
        "--editor-os-width=$Width",
        "--editor-os-height=$Height"
    )
    $startInfo = [Diagnostics.ProcessStartInfo]::new()
    $startInfo.FileName = $Executable
    $startInfo.WorkingDirectory = $BuildRoot
    $startInfo.Arguments =
        ($arguments | ForEach-Object { Quote-Argument $_ }) -join " "
    $startInfo.UseShellExecute = $false
    $startInfo.CreateNoWindow = $false
    $startInfo.RedirectStandardOutput = $true
    $startInfo.RedirectStandardError = $true
    $startInfo.EnvironmentVariables["VK_INSTANCE_LAYERS"] =
        $(if ($UseVulkanScreenshotLayer) {
            "VK_LAYER_KHRONOS_validation;VK_LAYER_LUNARG_screenshot"
        } else { "VK_LAYER_KHRONOS_validation" })
    $startInfo.EnvironmentVariables["VK_LOADER_LAYERS_ENABLE"] =
        $(if ($UseVulkanScreenshotLayer) {
            "VK_LAYER_KHRONOS_validation;VK_LAYER_LUNARG_screenshot"
        } else { "VK_LAYER_KHRONOS_validation" })
    $startInfo.EnvironmentVariables["VK_LOADER_LAYERS_DISABLE"] = "~implicit~"
    $startInfo.EnvironmentVariables["VK_LAYER_ENABLES"] =
        "VK_VALIDATION_FEATURE_ENABLE_SYNCHRONIZATION_VALIDATION_EXT"
    [void]$startInfo.EnvironmentVariables.Remove("VK_LAYER_MESSAGE_ID_FILTER")
    if ($UseVulkanScreenshotLayer) {
        $startInfo.EnvironmentVariables["VK_LAYER_PATH"] =
            [IO.Path]::GetDirectoryName([string]$VulkanLayerInfo.manifestPath)
        $startInfo.EnvironmentVariables["VK_SCREENSHOT_FRAMES"] = "all"
        $startInfo.EnvironmentVariables["VK_SCREENSHOT_DIR"] =
            $VulkanLayerRunRoot
        $startInfo.EnvironmentVariables["VK_SCREENSHOT_FORMAT"] =
            [string]$VulkanLayerInfo.outputFormat
    } else {
        foreach ($name in @("VK_LAYER_PATH", "VK_SCREENSHOT_FRAMES",
                            "VK_SCREENSHOT_DIR", "VK_SCREENSHOT_FORMAT")) {
            [void]$startInfo.EnvironmentVariables.Remove($name)
        }
    }
    if ($UseVulkanScreenshotLayer) {
        $environmentPath = Join-Path $VulkanLayerRunRoot "environment.json"
        $environment = [ordered]@{
            documentKind = "mgif.editor-ui-os-replay-vulkan-layer-environment"
            schemaVersion = 1
            runNonce = $RunNonce
            subcase = $Subcase
            sdkRoot = [string]$VulkanSdkRoot
            layerManifestSha256 = [string]$VulkanLayerInfo.manifestSha256
            layerDllSha256 = [string]$VulkanLayerInfo.dllSha256
            synchronizationValidation =
                "VK_LAYER_ENABLES:VK_VALIDATION_FEATURE_ENABLE_SYNCHRONIZATION_VALIDATION_EXT"
            variables = [ordered]@{
                VK_INSTANCE_LAYERS =
                    [string]$startInfo.EnvironmentVariables["VK_INSTANCE_LAYERS"]
                VK_LOADER_LAYERS_ENABLE =
                    [string]$startInfo.EnvironmentVariables["VK_LOADER_LAYERS_ENABLE"]
                VK_LOADER_LAYERS_DISABLE =
                    [string]$startInfo.EnvironmentVariables["VK_LOADER_LAYERS_DISABLE"]
                VK_LAYER_PATH =
                    [string]$startInfo.EnvironmentVariables["VK_LAYER_PATH"]
                VK_SCREENSHOT_FRAMES =
                    [string]$startInfo.EnvironmentVariables["VK_SCREENSHOT_FRAMES"]
                VK_SCREENSHOT_DIR =
                    [string]$startInfo.EnvironmentVariables["VK_SCREENSHOT_DIR"]
                VK_SCREENSHOT_FORMAT =
                    [string]$startInfo.EnvironmentVariables["VK_SCREENSHOT_FORMAT"]
                VK_LAYER_ENABLES =
                    [string]$startInfo.EnvironmentVariables["VK_LAYER_ENABLES"]
            }
        }
        Write-Json $environmentPath $environment
        $VulkanLayerEnvironmentRecords.Add([ordered]@{
            subcase = $Subcase
            runNonce = $RunNonce
            path = Get-EvidenceRelativePath $environmentPath
            bytes = (Get-Item -LiteralPath $environmentPath).Length
            sha256 = Get-Hash $environmentPath
            variables = $environment.variables
        })
    }
    $script:Process = [Diagnostics.Process]::new()
    $Process.StartInfo = $startInfo
    if (-not $Process.Start()) { throw "Could not start OS replay executable" }
    $script:StdoutTask = $Process.StandardOutput.ReadToEndAsync()
    $script:StderrTask = $Process.StandardError.ReadToEndAsync()
}

function Complete-ReplayProcess {
    if ($null -eq $Process -or $CurrentProcessRecorded) { return }
    $StdoutTask.Wait(); $StderrTask.Wait()
    $stdout = [string]$StdoutTask.Result
    $stderr = [string]$StderrTask.Result
    if (-not [string]::IsNullOrEmpty($stdout)) {
        $StdoutSegments.Add(
            "[run=$RunNonce subcase=$CurrentSubcase]`r`n$stdout")
    }
    if (-not [string]::IsNullOrEmpty($stderr)) {
        $StderrSegments.Add(
            "[run=$RunNonce subcase=$CurrentSubcase]`r`n$stderr")
    }
    $phaseCount = 0
    foreach ($step in @($ExpectedSteps | Where-Object {
        [int]$_.index -ge $CurrentStartStep -and
        [int]$_.index -le $CurrentStopStep
    })) { $phaseCount += @($step.phases).Count }
    $processRecord = [ordered]@{
        subcase = $CurrentSubcase
        runNonce = $RunNonce
        pid = $Process.Id
        exitCode = $Process.ExitCode
        startStep = $CurrentStartStep
        stopStep = $CurrentStopStep
        stepCount = $CurrentStopStep - $CurrentStartStep + 1
        phaseCount = $phaseCount
        fixture = $CurrentFixtureRelative
        fixtureSha256 = Get-Hash $CurrentFixturePath
        resultPath = Join-Path (Join-Path $AppEvidence $CurrentSubcase) `
            "result.json"
    }
    if ($UseVulkanScreenshotLayer) {
        $ppmFiles = @(
            Get-ChildItem -LiteralPath $VulkanLayerRunRoot -Filter "*.ppm" -File |
                Where-Object { $_.BaseName -match "^[0-9]+$" } |
                Sort-Object @{ Expression = { [int64]$_.BaseName }; Ascending = $true }
        )
        $indices = @($ppmFiles | ForEach-Object { [int64]$_.BaseName })
        $selected = @($VulkanLayerFrameRecords.ToArray() | Where-Object {
            [string]$_.runNonce -eq $RunNonce
        })
        $selectedIndices = @($selected | ForEach-Object { [int64]$_.index })
        $runReady = @($ReadyRecords.ToArray() | Where-Object {
            [string]$_.runNonce -eq $RunNonce
        })
        $maxReadyFrame = if (@($runReady).Count -eq 0) { 0 } else {
            [int64](($runReady | ForEach-Object {
                [int64]$_.frameSerial
            } | Measure-Object -Maximum).Maximum)
        }
        $maxAllowedIndex = if ($maxReadyFrame -gt 0) { $maxReadyFrame - 1 } else { -1 }
        $unconsumed = @($indices | Where-Object {
            $selectedIndices -notcontains $_
        })
        $postFinal = @($indices | Where-Object { $_ -gt $maxAllowedIndex })
        $processRecord.layer = [ordered]@{
            outputRoot = Get-EvidenceRelativePath $VulkanLayerRunRoot
            ppmCount = @($indices).Count
            selectedCount = @($selectedIndices).Count
            selectedIndices = @($selectedIndices)
            unconsumedCount = @($unconsumed).Count
            unconsumedIndices = @($unconsumed)
            maxReadyFrameSerial = $maxReadyFrame
            maxAllowedIndex = $maxAllowedIndex
            postFinalCount = @($postFinal).Count
            postFinalIndices = @($postFinal)
        }
    }
    $ProcessRecords.Add($processRecord)
    $script:CurrentProcessRecorded = $true
}

function Wait-MainWindow {
    $startupSeconds = [Math]::Min(60, [Math]::Max(10, $TimeoutSeconds))
    $focusDeadline = [DateTime]::UtcNow.AddSeconds($startupSeconds)
    $activationAttempted = $false
    $firstPhase = $ExpectedSteps[$CurrentStartStep].phases[0]
    $firstReady = Join-Path $SyncRoot ("{0}.{1:D3}.{2}.ready.json" -f
        $RunNonce, $CurrentStartStep, [string]$firstPhase.id)
    while (-not $Process.HasExited -and [DateTime]::UtcNow -lt $Deadline -and
           [DateTime]::UtcNow -lt $focusDeadline) {
        $Process.Refresh()
        if (Test-Path -LiteralPath $firstReady -PathType Leaf) {
            $binding = Get-Content -LiteralPath $firstReady -Raw | ConvertFrom-Json
            if ([string]$binding.runNonce -ne $RunNonce -or
                [string]$binding.case -ne $Case -or
                [string]$binding.planSha256 -ne $PlanContractHash -or
                [uint32]$binding.client.pid -ne [uint32]$Process.Id -or
                [string]$binding.client.hwnd -notmatch '^0x[0-9a-fA-F]+$') {
                throw "Startup ready file does not identify the launched process"
            }
            $candidate = [IntPtr]::new([Convert]::ToInt64(
                ([string]$binding.client.hwnd).Substring(2), 16))
            [uint32]$candidatePid = 0
            if (-not [MGIFEditorOsNative]::IsWindow($candidate) -or
                [MGIFEditorOsNative]::GetAncestor($candidate, 2) -ne $candidate -or
                [MGIFEditorOsNative]::GetWindowThreadProcessId($candidate, [ref]$candidatePid) -eq 0 -or
                $candidatePid -ne [uint32]$Process.Id) {
                throw "Startup ready HWND is not owned by the launched process"
            }
            $script:WindowHandle = $candidate
            [void][MGIFEditorOsNative]::ShowWindow($WindowHandle, 9)
            [void][MGIFEditorOsNative]::BringWindowToTop($WindowHandle)
            [void][MGIFEditorOsNative]::SetForegroundWindow($WindowHandle)
            Start-Sleep -Milliseconds 200
            if ([MGIFEditorOsNative]::GetForegroundWindow() -ne $WindowHandle) {
                if (-not $activationAttempted) {
                    $activationAttempted = $true
                    $windowRect = New-Object MGIFEditorOsNative+RECT
                    $clientRect = Get-ClientContract $WindowHandle
                    if (-not [MGIFEditorOsNative]::GetWindowRect($WindowHandle, [ref]$windowRect) -or
                        $clientRect.top - $windowRect.Top -lt 12) {
                        throw "Test window has no safe non-client activation area"
                    }
                    $caption = New-Object MGIFEditorOsNative+POINT
                    $caption.X = [int](($clientRect.left + $clientRect.right) / 2)
                    $caption.Y = [int](($windowRect.Top + $clientRect.top) / 2)
                    $captionRaised = $false
                    if ([MGIFEditorOsNative]::GetAncestor(
                            [MGIFEditorOsNative]::WindowFromPoint($caption), 2) -ne $WindowHandle) {
                        if (-not [MGIFEditorOsNative]::SetWindowPos(
                                $WindowHandle, [IntPtr]::new(-1), 0, 0, 0, 0, 0x0013)) {
                            throw "Could not expose the owned test window caption"
                        }
                        $captionRaised = $true
                        Start-Sleep -Milliseconds 100
                        if ([MGIFEditorOsNative]::GetAncestor(
                                [MGIFEditorOsNative]::WindowFromPoint($caption), 2) -ne $WindowHandle) {
                            throw "Test caption is occluded; startup input was not injected"
                        }
                    }
                    $sent = Send-PointerMove $caption.X $caption.Y
                    if ([MGIFEditorOsNative]::GetAncestor(
                            [MGIFEditorOsNative]::WindowFromPoint($caption), 2) -ne $WindowHandle) {
                        throw "Test caption ownership changed before startup activation"
                    }
                    $script:LeftButtonHeld = $true
                    $sent += Send-MouseButton "left-click"
                    $script:LeftButtonHeld = $false
                    $StartupActivationRecords.Add([ordered]@{
                        runNonce = $RunNonce; pid = $Process.Id
                        hwnd = $WindowHandle.ToInt64(); x = $caption.X; y = $caption.Y
                        recipient = "owned-test-window-caption"; sendInputCount = $sent
                        temporarilyTopmost = $captionRaised
                    })
                    Write-Json (Join-Path $EvidenceRoot "startup-activation.json") `
                        ([ordered]@{ records = @($StartupActivationRecords) })
                }
                continue
            }
            return
        }
        Start-Sleep -Milliseconds 25
    }
    throw "OS replay process did not create and focus its native window within startup timeout"
}

function Wait-Ready([object]$Step, [object]$Phase) {
    $fileName = ("{0}.{1:D3}.{2}.ready.json" -f
        $RunNonce, [int]$Step.index, [string]$Phase.id)
    $expectedPath = Join-Path $SyncRoot $fileName
    while (-not $Process.HasExited -and [DateTime]::UtcNow -lt $Deadline) {
        if ([MGIFEditorOsNative]::GetForegroundWindow() -ne (Get-ExpectedForeground)) {
            throw "OS replay window lost foreground while waiting for ready"
        }
        $unconsumed = @(Get-ChildItem -LiteralPath $SyncRoot `
            -Filter "*.ready.json" -File | Where-Object {
                -not $ConsumedReady.Contains($_.FullName)
            })
        if (@($unconsumed).Count -gt 1 -or
            (@($unconsumed).Count -eq 1 -and
             $unconsumed[0].FullName -ne $expectedPath)) {
            throw "OS replay emitted an unexpected or ambiguous ready file"
        }
        if (Test-Path -LiteralPath $expectedPath -PathType Leaf) {
            $bytes = [IO.File]::ReadAllBytes($expectedPath)
            $raw = [Text.UTF8Encoding]::new($false).GetString($bytes)
            return [ordered]@{
                path = $expectedPath
                bytes = $bytes.Length
                sha256 = Get-BytesHash $bytes
                value = $raw | ConvertFrom-Json
            }
        }
        Start-Sleep -Milliseconds 20
        $Process.Refresh()
    }
    if ($Process.HasExited) {
        throw "OS replay process exited before $($Step.id)/$($Phase.id) ready"
    }
    throw "Timed out waiting for $($Step.id)/$($Phase.id) ready"
}

function Test-Ready(
    [object]$ReadyRecord,
    [object]$Step,
    [object]$Phase,
    [hashtable]$GraphKeys) {
    $ready = $ReadyRecord.value
    if ([string]$ready.documentKind -ne "mgif.editor-ui-os-replay-ready" -or
        [int]$ready.schemaVersion -ne $SchemaVersion -or
        [string]$ready.protocol -ne $Protocol -or
        -not [bool]$ready.integration -or
        [string]$ready.dispatchMode -ne "win32-send-input" -or
        [string]$ready.runNonce -ne $RunNonce -or
        [string]$ready.case -ne $Case -or
        [string]$ready.planSha256 -ne $PlanContractHash -or
        [string]$ready.identity.executableSha256 -ne $ExecutableHash -or
        [string]$ready.identity.sourceSha256 -ne $SourceIdentityHash -or
        [int]$ready.step.index -ne [int]$Step.index -or
        [string]$ready.step.id -ne [string]$Step.id -or
        [string]$ready.step.phase -ne [string]$Phase.id) {
        throw "Ready envelope/identity does not match the expected phase"
    }
    if ([string]$ready.frame.lifecycle -notmatch
            "^(NoDocument|LoadingCandidate|ReadyClean|ReadyModified|GizmoHover|GizmoDragging|Modal|SavingMergingCompacting|Error)$" -or
        [int64]$ready.frame.lifecycleRevision -lt 0 -or
        [int64]$ready.frame.documentRevision -ne
            [int64]$ready.authority.documentRevision -or
        [int64]$ready.frame.selectionRevision -ne
            [int64]$ready.authority.selectionRevision -or
        [int64]$ready.frame.frameSerial -le 0 -or
        [uint32]$ready.frame.mainViewportId -eq 0 -or
        [double]$ready.frame.viewportSize.x -le 0 -or
        [double]$ready.frame.viewportSize.y -le 0 -or
        [double]$ready.frame.displaySize.x -le 0 -or
        [double]$ready.frame.displaySize.y -le 0 -or
        [double]$ready.frame.framebufferScale.x -le 0 -or
        [double]$ready.frame.framebufferScale.y -le 0) {
        throw "Ready lost its EditorUiProbeFrameBinding"
    }
    Test-ObservedDispatches $ready $Step $Phase
    Test-ObservedGizmo $ready $Step $Phase
    if ((Get-Hash $Executable) -ne $ExecutableHash) {
        throw "OS replay executable changed before a ready boundary"
    }
    $readyHwnd = Convert-Hwnd ([string]$ready.client.hwnd)
    $Process.Refresh()
    if ($readyHwnd -ne $WindowHandle -or
        -not [MGIFEditorOsNative]::IsWindow($WindowHandle) -or
        -not [MGIFEditorOsNative]::IsWindowVisible($WindowHandle) -or
        -not [MGIFEditorOsNative]::IsWindowEnabled($WindowHandle) -or
        [MGIFEditorOsNative]::GetForegroundWindow() -ne (Get-ExpectedForeground)) {
        throw "Foreground HWND contract failed before input"
    }
    [uint32]$nativePid = 0
    if ([MGIFEditorOsNative]::GetWindowThreadProcessId(
            $WindowHandle, [ref]$nativePid) -eq 0 -or
        $nativePid -ne [uint32]$Process.Id -or
        [uint32]$ready.client.pid -ne $nativePid) {
        throw "Foreground HWND does not belong to the launched PID"
    }
    $client = Get-ClientContract $WindowHandle
    if ($client.width -ne $Width -or $client.height -ne $Height -or
        [int]$ready.client.width -ne $client.width -or
        [int]$ready.client.height -ne $client.height -or
        [int]$ready.client.dpi -ne $client.dpi -or
        [string]$ready.client.screenRect.coordinateSpace -ne
            "screen-physical-px" -or
        [int]$ready.client.screenRect.left -ne $client.left -or
        [int]$ready.client.screenRect.top -ne $client.top -or
        [int]$ready.client.screenRect.right -ne $client.right -or
        [int]$ready.client.screenRect.bottom -ne $client.bottom) {
        throw "Client extent/DPI/screen rectangle contract failed"
    }
    if ($ObservedDpi -eq 0) { $script:ObservedDpi = $client.dpi }
    if ($client.dpi -ne $ObservedDpi -or
        ($ExpectedDpi -ne 0 -and $client.dpi -ne $ExpectedDpi)) {
        throw "Client DPI changed or differs from ExpectedDpi"
    }
    $scaleX = [double]$ready.frame.framebufferScale.x
    $scaleY = [double]$ready.frame.framebufferScale.y
    $dpiScale = $client.dpi / 96.0
    if ([Math]::Abs($scaleX - $dpiScale) -gt 0.02 -or
        [Math]::Abs($scaleY - $dpiScale) -gt 0.02 -or
        [Math]::Abs(([double]$ready.frame.displaySize.x * $scaleX) -
                    $client.width) -gt 2.0 -or
        [Math]::Abs(([double]$ready.frame.displaySize.y * $scaleY) -
                    $client.height) -gt 2.0) {
        throw "Probe framebuffer scale is inconsistent with native DPI/client"
    }
    if ([string]$ready.target.semantic -ne [string]$Phase.target.semantic -or
        [string]$ready.target.role -ne [string]$Phase.target.role -or
        [int]$ready.target.matchCount -ne 1 -or
        -not [bool]$ready.target.visible -or
        -not [bool]$ready.target.enabled -or
        [string]$ready.target.rect.coordinateSpace -ne
            "screen-physical-px") {
        throw "Target is not the expected unique, visible, enabled semantic"
    }
    $semantic = [string]$ready.target.semantic
    if ($semantic -eq "client") {
        if ([uint64]$ready.target.probe.order -ne 0 -or
            [uint32]$ready.target.probe.itemId -ne 0 -or
            [uint32]$ready.target.probe.windowToken -ne 0 -or
            [uint32]$ready.target.probe.popupToken -ne 0 -or
            [uint32]$ready.target.probe.modalToken -ne 0) {
            throw "Synthetic client target contains an ImGui probe identity"
        }
    }
    elseif ($semantic -in @("gizmo-translate-y", "viewport-outside")) {
        if ($Case -notin @("U03-gizmo-translate", "U03-gizmo-escape", "U03-gizmo-escape-release", "U03-gizmo-release-escape", "U03-gizmo-focus-no-release", "U03-gizmo-focus-same-frame", "U03-gizmo-focus-delivered-release") -or
            [string]$ready.target.probe.source -ne "editor-gizmo-adapter-v1" -or
            [uint64]$ready.target.probe.order -ne 0 -or
            [uint32]$ready.target.probe.itemId -ne 0 -or
            [uint32]$ready.target.probe.windowToken -ne 0 -or
            [uint32]$ready.target.probe.popupToken -ne 0 -or
            [uint32]$ready.target.probe.modalToken -ne 0 -or
            -not [bool]$ready.gizmo.componentValid -or
            -not [bool]$ready.gizmo.componentEnabled -or
            ($ready.gizmo.node | ConvertTo-Json -Compress -Depth 8) -ne
                ($ready.target.graphRowKey.object | ConvertTo-Json -Compress -Depth 8)) {
            throw "Native gizmo target lost its actual adapter identity"
        }
        Test-GizmoRect $ready.target.probe.rect
        Test-GizmoRect $ready.gizmo.viewportRect
        $observedRect = if ($semantic -eq "gizmo-translate-y") {
            $ready.gizmo.componentRect
        } else { $ready.gizmo.outsideRect }
        Test-GizmoRect $observedRect
        foreach ($field in @("minimumX", "minimumY", "maximumX", "maximumY")) {
            if ([Math]::Abs([double]$ready.target.probe.rect.$field -
                    [double]$observedRect.$field) -gt 0.000001) {
                throw "Native gizmo target is not its classified observed rectangle"
            }
        }
        if ($semantic -eq "viewport-outside" -and
            [double]$observedRect.minimumX -lt [double]$ready.gizmo.viewportRect.maximumX -and
            [double]$observedRect.maximumX -gt [double]$ready.gizmo.viewportRect.minimumX -and
            [double]$observedRect.minimumY -lt [double]$ready.gizmo.viewportRect.maximumY -and
            [double]$observedRect.maximumY -gt [double]$ready.gizmo.viewportRect.minimumY) {
            throw "Native outside-release target intersects the observed viewport"
        }
        if ([bool]$Phase.requireHovered -and [int]$ready.gizmo.hoveredComponent -ne 1) {
            throw "Native target hover is not a classified TranslateY hit"
        }
    }
    elseif ($semantic -in @("viewport", "viewport-blank", "gizmo-translate", "gizmo-rotate", "gizmo-scale", "toolbar-grid", "toolbar-gizmo", "toolbar-snap")) {
        throw "Unsupported passive target cannot establish true OS replay evidence"
    }
    else {
        $fixedLabel = Get-FixedProbeLabel $semantic
        if ([string]::IsNullOrEmpty([string]$ready.target.probe.label) -or
            ($fixedLabel -ne "" -and
             [string]$ready.target.probe.label -ne $fixedLabel) -or
            [uint64]$ready.target.probe.order -eq 0 -or
            [uint32]$ready.target.probe.itemId -eq 0 -or
            [uint32]$ready.target.probe.windowToken -eq 0 -or
            [string]$ready.target.probe.rect.coordinateSpace -ne
                "imgui-screen-logical" -or
            [double]$ready.target.probe.rect.maximumX -le
                [double]$ready.target.probe.rect.minimumX -or
            [double]$ready.target.probe.rect.maximumY -le
                [double]$ready.target.probe.rect.minimumY) {
            throw "Ready target lost the actual ImGui probe identity"
        }
        if ($semantic -in @(
                "scene-add-node", "node-duplicate", "node-delete",
                "node-add-child-menu", "node-add-child-node", "node-rename") -and
            [uint32]$ready.target.probe.popupToken -eq 0) {
            throw "Context target is not bound to its popup token"
        }
        if ($semantic -in @("rename-input", "delete-accept") -and
            [uint32]$ready.target.probe.modalToken -eq 0) {
            throw "Modal target is not bound to its modal token"
        }
    }
    $left = [int]$ready.target.rect.left
    $top = [int]$ready.target.rect.top
    $right = [int]$ready.target.rect.right
    $bottom = [int]$ready.target.rect.bottom
    if ($right -le $left -or $bottom -le $top -or
        $left -lt $client.left -or $top -lt $client.top -or
        $right -gt $client.right -or $bottom -gt $client.bottom) {
        throw "Target rectangle is invalid or outside the client"
    }
    if ($semantic -ne "client") {
        $logical = $ready.target.probe.rect
        $expectedLeft = $client.left + [int][Math]::Round(
            ([double]$logical.minimumX -
             [double]$ready.frame.viewportPosition.x) * $scaleX)
        $expectedTop = $client.top + [int][Math]::Round(
            ([double]$logical.minimumY -
             [double]$ready.frame.viewportPosition.y) * $scaleY)
        $expectedRight = $client.left + [int][Math]::Round(
            ([double]$logical.maximumX -
             [double]$ready.frame.viewportPosition.x) * $scaleX)
        $expectedBottom = $client.top + [int][Math]::Round(
            ([double]$logical.maximumY -
             [double]$ready.frame.viewportPosition.y) * $scaleY)
        if ([Math]::Abs($left - $expectedLeft) -gt 1 -or
            [Math]::Abs($top - $expectedTop) -gt 1 -or
            [Math]::Abs($right - $expectedRight) -gt 1 -or
            [Math]::Abs($bottom - $expectedBottom) -gt 1) {
            throw "Physical target rect does not derive from probe frame/rect"
        }
    }
    if ([int64]$ready.target.revision -ne
        [int64]$ready.authority.documentRevision -or
        [string]$ready.authority.documentAuthority -notmatch "^[1-9][0-9]*$" -or
        [int64]$ready.authority.undoCount -lt 0 -or
        [int64]$ready.authority.redoCount -lt 0) {
        throw "Ready authority/revision contract is invalid"
    }
    Test-Sha256 ([string]$ready.authority.documentDigest) "documentDigest"
    Test-Sha256 ([string]$ready.authority.projectionDigest) "projectionDigest"
    Test-Sha256 ([string]$ready.authority.runtimeContentDigest) `
        "runtimeContentDigest"
    Test-Sha256 ([string]$ready.authority.hierarchyOrderDigest) `
        "hierarchyOrderDigest"
    if ([string]$ready.authority.projectionDigest -ne
            [string]$ready.authority.runtimeContentDigest -or
        [string]$ready.authority.selectionKind -notmatch
            "^(None|Node|Primitive|Mesh|Material|Camera|Light)$" -or
        [string]$ready.authority.undoDescription -match "[\x00-\x1f]" -or
        [string]$ready.authority.redoDescription -match "[\x00-\x1f]") {
        throw "Ready digest/selection/history description contract is invalid"
    }
    $selectedGraphKey = ""
    if ($null -ne $ready.authority.selectedGraphRowKey) {
        Test-ObjectId $ready.authority.selectedGraphRowKey.scene "Scene"
        Test-ObjectId $ready.authority.selectedGraphRowKey.object
        if ($null -ne $ready.authority.selectedGraphRowKey.directParent) {
            Test-ObjectId $ready.authority.selectedGraphRowKey.directParent "Node"
        }
        $selectedGraphKey = $ready.authority.selectedGraphRowKey |
            ConvertTo-Json -Compress -Depth 12
    }
    $selectedObject = ""
    if ($null -ne $ready.authority.selectedObject) {
        Test-ObjectId $ready.authority.selectedObject
        if ([string]$ready.authority.selectedObject.authority -ne
            [string]$ready.authority.documentAuthority) {
            throw "Selected object belongs to another document authority"
        }
        $selectedObject = $ready.authority.selectedObject |
            ConvertTo-Json -Compress -Depth 8
    }
    if (([string]$ready.authority.selectionKind -eq "None") -ne
        ([string]::IsNullOrEmpty($selectedObject))) {
        throw "Selection kind and selected object disagree"
    }
    $graphKey = Get-CanonicalGraphKey $ready.target `
        ([string]$Phase.target.graphRowRole)
    if ($graphKey -ne "") {
        $role = [string]$Phase.target.graphRowRole
        if ($GraphKeys.ContainsKey($role) -and $GraphKeys[$role] -ne $graphKey) {
            throw "GraphRowKey changed within one process/subcase role"
        }
        if (-not $GraphKeys.ContainsKey($role)) {
            $roleContract = @($GraphRoles | Where-Object {
                [string]$_.role -eq $role -and
                [string]$_.subcase -eq [string]$Step.subcase
            })
            if ($roleContract.Count -ne 1 -or
                [string]$ready.target.graphRowKey.object.kind -ne
                    [string]$roleContract[0].objectKind) {
                throw "GraphRowKey does not satisfy its normalized role kind"
            }
            $introduced = @($ExpectedSteps | Where-Object {
                [string]$_.id -eq [string]$roleContract[0].introducedByStep
            })
            if ($introduced.Count -ne 1 -or
                [int]$Step.index -lt [int]$introduced[0].index) {
                throw "Graph role was bound before its introduction step"
            }
            if ([string]$roleContract[0].labelContains -ne "" -and
                $semantic -in @(
                    "graph-row", "node-drag-source",
                    "hierarchy-drop-target") -and
                [string]$ready.target.probe.label -notlike
                    ("*" + [string]$roleContract[0].labelContains + "*")) {
                throw "Graph role label constraint failed"
            }
            if ([bool]$roleContract[0].requireRoot -and
                $null -ne $ready.target.graphRowKey.directParent) {
                throw "Graph role expected a scene root occurrence"
            }
            $objectJson = $ready.target.graphRowKey.object |
                ConvertTo-Json -Compress -Depth 8
            $sameRole = [string]$roleContract[0].sameObjectAsRole
            if ($sameRole -ne "") {
                if (-not $GraphKeys.ContainsKey($sameRole)) {
                    throw "Graph same-object alias was bound before its source"
                }
                $sameKey = $GraphKeys[$sameRole] | ConvertFrom-Json
                $sameObjectJson = $sameKey.object |
                    ConvertTo-Json -Compress -Depth 8
                if ($objectJson -ne $sameObjectJson) {
                    throw "Graph same-object alias constraint failed"
                }
            }
            $parentRole = [string]$roleContract[0].directParentRole
            if ($parentRole -ne "") {
                if (-not $GraphKeys.ContainsKey($parentRole) -or
                    $null -eq $ready.target.graphRowKey.directParent) {
                    throw "Graph direct-parent role is unavailable"
                }
                $parentKey = $GraphKeys[$parentRole] | ConvertFrom-Json
                $expectedParentJson = $parentKey.object |
                    ConvertTo-Json -Compress -Depth 8
                $actualParentJson = $ready.target.graphRowKey.directParent |
                    ConvertTo-Json -Compress -Depth 8
                if ($actualParentJson -ne $expectedParentJson) {
                    throw "Graph direct-parent alias constraint failed"
                }
            }
            $distinctRole = [string]$roleContract[0].distinctFromRole
            if ($distinctRole -ne "") {
                if (-not $GraphKeys.ContainsKey($distinctRole)) {
                    throw "Graph distinct-object role is unavailable"
                }
                $distinctKey = $GraphKeys[$distinctRole] | ConvertFrom-Json
                $distinctObjectJson = $distinctKey.object |
                    ConvertTo-Json -Compress -Depth 8
                if ($objectJson -eq $distinctObjectJson) {
                    throw "Graph distinct-object alias constraint failed"
                }
            }
            $BindingRecords.Add([ordered]@{
                subcase = [string]$Step.subcase
                runNonce = $RunNonce
                role = $role
                firstStep = [int]$Step.index
                firstPhase = [string]$Phase.id
                graphRowKey = $ready.target.graphRowKey
                graphRowKeySha256 = Get-BytesHash(
                    [Text.UTF8Encoding]::new($false).GetBytes($graphKey))
            })
        }
        $GraphKeys[$role] = $graphKey
    }
    if ([bool]$Phase.requireHovered) {
        $cursor = New-Object MGIFEditorOsNative+POINT
        if (-not [bool]$ready.target.hovered -or
            -not [MGIFEditorOsNative]::GetCursorPos([ref]$cursor) -or
            $cursor.X -lt $left -or $cursor.X -ge $right -or
            $cursor.Y -lt $top -or $cursor.Y -ge $bottom) {
            throw "App/native hover confirmation failed"
        }
    }
    if ([bool]$Phase.requireFocused -and -not [bool]$ready.target.focused) {
        throw "App focus confirmation failed"
    }
    if ([bool]$Phase.requireDropAccepted -and
        -not [bool]$ready.target.dropAccepted) {
        throw "App drop-target acceptance confirmation failed"
    }
    if ($Case -in @("U03-gizmo-translate", "U03-gizmo-escape", "U03-gizmo-escape-release", "U03-gizmo-release-escape", "U03-gizmo-focus-no-release", "U03-gizmo-focus-same-frame", "U03-gizmo-focus-delivered-release") -and
        $null -ne $Phase.PSObject.Properties["gizmo"] -and
        $null -ne $Phase.gizmo.pointerOutside) {
        $cursor = New-Object MGIFEditorOsNative+POINT
        $pointerX = $client.left + ([double]$ready.gizmo.pointer.x -
            [double]$ready.frame.viewportPosition.x) * $scaleX
        $pointerY = $client.top + ([double]$ready.gizmo.pointer.y -
            [double]$ready.frame.viewportPosition.y) * $scaleY
        if (-not [MGIFEditorOsNative]::GetCursorPos([ref]$cursor) -or
            [Math]::Abs($pointerX - $cursor.X) -gt 1 -or
            [Math]::Abs($pointerY - $cursor.Y) -gt 1) {
            throw "Native gizmo pointer does not match current physical cursor"
        }
    }
    $captureViewport = $null
    if ($Case -in @("U03-gizmo-translate", "U03-gizmo-escape", "U03-gizmo-escape-release", "U03-gizmo-release-escape", "U03-gizmo-focus-no-release", "U03-gizmo-focus-same-frame", "U03-gizmo-focus-delivered-release")) {
        if ($null -eq $ready.gizmo) {
            throw "Native PNG capture lacks its observed viewport"
        }
        Test-GizmoRect $ready.gizmo.viewportRect
        $logicalViewport = $ready.gizmo.viewportRect
        $captureViewport = [ordered]@{
            source = [string]$ready.gizmo.source
            renderedFrame = [uint64]$ready.gizmo.renderedFrame
            coordinateSpace = "screen-physical-px"
            left = $client.left + [int][Math]::Round(
                ([double]$logicalViewport.minimumX - [double]$ready.frame.viewportPosition.x) * $scaleX)
            top = $client.top + [int][Math]::Round(
                ([double]$logicalViewport.minimumY - [double]$ready.frame.viewportPosition.y) * $scaleY)
            right = $client.left + [int][Math]::Round(
                ([double]$logicalViewport.maximumX - [double]$ready.frame.viewportPosition.x) * $scaleX)
            bottom = $client.top + [int][Math]::Round(
                ([double]$logicalViewport.maximumY - [double]$ready.frame.viewportPosition.y) * $scaleY)
            clientLeft = $client.left; clientTop = $client.top
            clientRight = $client.right; clientBottom = $client.bottom
        }
    }
    return [ordered]@{
        runNonce = [string]$ready.runNonce
        pid = [uint32]$ready.client.pid
        authority = [string]$ready.authority.documentAuthority
        revision = [int64]$ready.authority.documentRevision
        undo = [int64]$ready.authority.undoCount
        redo = [int64]$ready.authority.redoCount
        documentDigest = [string]$ready.authority.documentDigest
        projectionDigest = [string]$ready.authority.projectionDigest
        runtimeContentDigest = [string]$ready.authority.runtimeContentDigest
        hierarchyOrderDigest = [string]$ready.authority.hierarchyOrderDigest
        selectionKind = [string]$ready.authority.selectionKind
        selectedGraphRowKey = $selectedGraphKey
        selectedObject = $selectedObject
        undoDescription = [string]$ready.authority.undoDescription
        redoDescription = [string]$ready.authority.redoDescription
        targetCenterX = [int](($left + $right - 1) / 2)
        targetCenterY = [int](($top + $bottom - 1) / 2)
        gizmo = $(if ($null -ne $ready.PSObject.Properties["gizmo"]) { $ready.gizmo } else { $null })
        captureViewport = $captureViewport
    }
}

function Get-PhasePointerPosition([object]$Phase, [object]$State) {
    $x = [int]$State.targetCenterX
    $y = [int]$State.targetCenterY
    $value = [string]$Phase.input.value
    if ([string]$Phase.input.kind -eq "move-pointer" -and $value -ne "") {
        if ($value -notmatch '^(-?[0-9]+),(-?[0-9]+)$') {
            throw "Pointer offset must be explicit signed physical-pixel x,y values"
        }
        $x += [int]$Matches[1]
        $y += [int]$Matches[2]
    }
    return [pscustomobject]@{ x = $x; y = $y }
}

function Send-PhaseInput([object]$Phase, [object]$State) {
    $kind = [string]$Phase.input.kind
    # Focus transfer is an explicit harness operation. It is intentionally
    # checked against the current foreground before moving the sink/editor.
    if ($kind -eq "focus-loss") {
        if ([MGIFEditorOsNative]::GetForegroundWindow() -ne $WindowHandle) {
            throw "Focus-loss phase did not begin with the editor foreground"
        }
        Move-FocusToSink
        return 0
    }
    if ($kind -eq "focus-restore") {
        if (-not $FocusSinkActive -or
            [MGIFEditorOsNative]::GetForegroundWindow() -ne $FocusSink.Handle) {
            throw "Focus-restore phase did not begin with the focus sink foreground"
        }
        Restore-EditorFocus
        return 0
    }
    if ([MGIFEditorOsNative]::GetForegroundWindow() -ne (Get-ExpectedForeground)) {
        throw "Foreground changed immediately before SendInput"
    }
    if (([string]$Phase.id -eq "focus-loss" -and
         $Case -eq "U03-transform-gizmo") -or
        [string]$Phase.input.kind -eq "focus-loss") {
        Move-FocusToSink
        return 0
    }
    if ([string]$Phase.input.kind -eq "focus-restore") {
        Restore-EditorFocus
        return 0
    }
    switch ($kind) {
        "none" { return 0 }
        "move-pointer" {
            $position = Get-PhasePointerPosition $Phase $State
            return Send-PointerMove $position.x $position.y
        }
        { $_ -in @("left-click", "right-click") } {
            return Send-MouseButton ([string]$Phase.input.kind)
        }
        "right-down" {
            $count = Send-MouseButton "right-down"
            $script:RightButtonHeld = $true
            return $count
        }
        "right-up" {
            $count = Send-MouseButton "right-up"
            $script:RightButtonHeld = $false
            return $count
        }
        "left-down" {
            $count = Send-MouseButton "left-down"
            $script:LeftButtonHeld = $true
            return $count
        }
        "left-up" {
            if ($FocusSinkActive) {
                $FocusTransferRecords.Add([ordered]@{
                    operation = "physical-release-to-sink"
                    editorHwnd = $WindowHandle.ToInt64()
                    recipientHwnd = $FocusSink.Handle.ToInt64()
                    leftButtonHeld = $LeftButtonHeld; releaseInjected = $true
                })
            }
            $count = Send-MouseButton "left-up"
            $script:LeftButtonHeld = $false
            return $count
        }
        "left-up-focus-loss" {
            if ($FocusSinkActive -or -not $LeftButtonHeld) {
                throw "Combined focus-loss input requires the focused editor and a held left button"
            }
            $wasHeld = $LeftButtonHeld
            $count = Send-MouseButton "left-up"
            Wait-EditorInputDeliveryFence
            $script:LeftButtonHeld = $false
            Move-FocusToSink
            $FocusTransferRecords[-1].operation = "focus-loss-with-release"
            $FocusTransferRecords[-1].leftButtonHeld = $wasHeld
            $FocusTransferRecords[-1].releaseInjected = $true
            $FocusTransferRecords[-1].deliveryFence = $LastInputDeliveryFence
            return $count
        }
        "key-chord" { return Send-KeyChord ([string]$Phase.input.value) }
        "replace-text" {
            $count = Send-KeyChord "Ctrl+A"
            return $count + (Send-UnicodeText ([string]$Phase.input.value))
        }
        "press-key" {
            $key = Get-VirtualKey ([string]$Phase.input.value)
            return Send-NativeInput @(
                (New-KeyInput $key 0 0),
                (New-KeyInput $key 0 ([uint32]0x0002)))
        }
        { $_ -in @("key-down", "key-up") } {
            return Send-HeldKeyEdge ([string]$Phase.input.kind) ([string]$Phase.input.value)
        }
        "escape-release-batch" { return Send-EscapeReleaseBatch ([string]$Phase.input.value) }
        default { throw "Unknown plan input kind: $($Phase.input.kind)" }
    }
}

function Test-StepEffect(
    [object]$Step, [object]$Before, [object]$After, [object]$BeforeDrop) {
    if ([string]$Step.effect -eq "authority-reset") {
        if ($Before.authority -ne $After.authority -or
            $Before.documentDigest -ne $After.documentDigest -or
            $Before.hierarchyOrderDigest -ne $After.hierarchyOrderDigest -or
            $After.undo -ne 0 -or $After.redo -ne 0) {
            throw "Fresh unique-root process mutated during bootstrap"
        }
        return
    }
    if ($Before.authority -ne $After.authority -or
        $After.revision -ne
            ($Before.revision + [int]$Step.expectedRevisionDelta)) {
        throw "Step authority/revision effect differs from the plan"
    }
    $digestChanged = $After.documentDigest -ne $Before.documentDigest -and
                     $After.projectionDigest -ne $Before.projectionDigest -and
                     $After.runtimeContentDigest -ne $Before.runtimeContentDigest
    $digestUnchanged = $After.documentDigest -eq $Before.documentDigest -and
                       $After.projectionDigest -eq $Before.projectionDigest -and
                       $After.runtimeContentDigest -eq $Before.runtimeContentDigest
    switch ([string]$Step.effect) {
        { $_ -in @("presentation-only", "selection-only",
                   "document-noop-history-envelope") } {
            if (-not $digestUnchanged) {
                throw "Non-document step changed an authority digest"
            }
        }
        "document-mutation" {
            if (-not $digestChanged) {
                throw "Document mutation did not change all authority digests"
            }
        }
        "history-move" {
            if ([int]$Step.expectedRevisionDelta -eq 0) {
                if (-not $digestUnchanged) {
                    throw "No-op history move changed an authority digest"
                }
            }
            elseif (-not $digestChanged) {
                throw "History move did not publish changed authority digests"
            }
        }
        default { throw "Unknown step effect: $($Step.effect)" }
    }
    if ([bool]$Step.expectHierarchyOrderDigestChange) {
        if ($After.hierarchyOrderDigest -eq $Before.hierarchyOrderDigest) {
            throw "Structural step did not change stable root/child order digest"
        }
    }
    elseif ($After.hierarchyOrderDigest -ne $Before.hierarchyOrderDigest) {
        throw "Non-structural step changed stable root/child order digest"
    }
    if ([string]$Step.effect -eq "document-noop-history-envelope" -and
        [string]$Step.id -ne "unique-root-delete-confirm" -and
        ($null -eq $BeforeDrop -or
         $After.selectionKind -ne $BeforeDrop.selectionKind -or
         $After.selectedObject -ne $BeforeDrop.selectedObject -or
         $After.selectedGraphRowKey -ne $BeforeDrop.selectedGraphRowKey)) {
        throw "No-op hierarchy envelope changed selection"
    }
    switch ([string]$Step.history) {
        "unchanged" {
            if ($After.undo -ne $Before.undo -or $After.redo -ne $Before.redo) {
                throw "Step unexpectedly changed history"
            }
        }
        "push-undo-clear-redo" {
            if ($After.undo -ne ($Before.undo + 1) -or $After.redo -ne 0 -or
                [string]::IsNullOrEmpty($After.undoDescription) -or
                -not [string]::IsNullOrEmpty($After.redoDescription)) {
                throw "Step did not push exactly one undo and clear redo"
            }
        }
        "move-undo-to-redo" {
            if ($Before.undo -lt 1 -or $After.undo -ne ($Before.undo - 1) -or
                $After.redo -ne ($Before.redo + 1) -or
                [string]::IsNullOrEmpty($After.redoDescription)) {
                throw "Undo did not move exactly one history entry"
            }
        }
        "move-redo-to-undo" {
            if ($Before.redo -lt 1 -or $After.redo -ne ($Before.redo - 1) -or
                $After.undo -ne ($Before.undo + 1) -or
                [string]::IsNullOrEmpty($After.undoDescription)) {
                throw "Redo did not move exactly one history entry"
            }
        }
        "reset" {
            if ($After.undo -ne 0 -or $After.redo -ne 0) {
                throw "History reset contract failed"
            }
        }
        default { throw "Unknown history effect: $($Step.history)" }
    }
    if ($Case -in @("U03-gizmo-focus-no-release", "U03-gizmo-focus-same-frame", "U03-gizmo-focus-delivered-release")) {
        if ([string]$Step.id -like "drag-focus-*") {
            if ($null -eq $Before.gizmo -or $null -eq $After.gizmo -or
                ([double]$After.gizmo.translation[1] -
                    [double]$Before.gizmo.translation[1]) -le 0.000001) {
                throw "Native focus-loss drag did not retain a positive local Y preview"
            }
            if ($After.selectionKind -ne $Before.selectionKind -or
                $After.selectedObject -ne $Before.selectedObject -or
                $After.selectedGraphRowKey -ne $Before.selectedGraphRowKey) {
                throw "Native focus-loss drag changed selection authority"
            }
            foreach ($field in @("translation", "rotation", "scale")) {
                for ($index = 0; $index -lt @($Before.gizmo.$field).Count; ++$index) {
                    if ($field -eq "translation" -and $index -eq 1) { continue }
                    if ([Math]::Abs([double]$After.gizmo.$field[$index] -
                            [double]$Before.gizmo.$field[$index]) -gt 0.000001) {
                        throw "Native focus-loss drag changed an unrelated TRS component"
                    }
                }
            }
            $script:GizmoBeforeTrs = $Before.gizmo
            $script:GizmoAfterTrs = $After.gizmo
        }
        elseif ([string]$Step.id -eq "final-boundary") {
            Test-GizmoTrsEqual $After.gizmo $GizmoAfterTrs
        }
        return
    }
    if ($Case -in @("U03-gizmo-translate", "U03-gizmo-escape", "U03-gizmo-escape-release", "U03-gizmo-release-escape")) {
        if ([string]$Step.id -in @("drag-outside-release", "drag-escape-only", "drag-same-frame")) {
            if ($null -eq $Before.gizmo -or $null -eq $After.gizmo -or
                ([double]$After.gizmo.translation[1] -
                    [double]$Before.gizmo.translation[1]) -le 0.000001) {
                throw "Native TranslateY drag did not change local Y translation"
            }
            if ($Case -in @("U03-gizmo-escape", "U03-gizmo-escape-release", "U03-gizmo-release-escape") -and
                [double]$After.gizmo.translation[1] -le [double]$Before.gizmo.translation[1] + 0.000001) {
                throw "Native Escape did not retain its positive Y preview"
            }
            foreach ($field in @("translation", "rotation", "scale")) {
                for ($index = 0; $index -lt @($Before.gizmo.$field).Count; ++$index) {
                    if ($field -eq "translation" -and $index -eq 1) { continue }
                    if ([Math]::Abs([double]$After.gizmo.$field[$index] -
                            [double]$Before.gizmo.$field[$index]) -gt 0.000001) {
                        throw "Native TranslateY drag changed an unrelated TRS component"
                    }
                }
            }
            $script:GizmoBeforeTrs = $Before.gizmo
            $script:GizmoAfterTrs = $After.gizmo
        }
        elseif ([string]$Step.id -eq "undo-transform") {
            Test-GizmoTrsEqual $After.gizmo $GizmoBeforeTrs
        }
        elseif ([string]$Step.id -in @("redo-transform", "late-release", "final-boundary")) {
            Test-GizmoTrsEqual $After.gizmo $GizmoAfterTrs
        }
    }
    if ([string]$Step.id -eq "unique-root-delete-confirm" -and
        ($After.selectionKind -ne "None" -or $After.selectedObject -ne "" -or
         $After.selectedGraphRowKey -ne "")) {
        throw "Unique-root Delete did not clear selection"
    }
    if ([string]$Step.id -eq "unique-root-select") {
        if ($After.selectionKind -ne "Node" -or
            $After.selectedGraphRowKey -ne
                $ProcessGraphKeys["unique-root-node"] -or
            [string]::IsNullOrEmpty($After.selectedObject)) {
            throw "OS unique-root selection did not bind the exact root row"
        }
        $script:UniqueRootSelectionState = $After
    }
    if ([string]$Step.id -eq "unique-root-undo" -and
        ($null -eq $UniqueRootSelectionState -or
         $After.selectionKind -ne $UniqueRootSelectionState.selectionKind -or
         $After.selectedObject -ne $UniqueRootSelectionState.selectedObject -or
         $After.selectedGraphRowKey -ne
            $UniqueRootSelectionState.selectedGraphRowKey)) {
        throw "Unique-root Undo did not restore exact selection K"
    }
    if ([string]$Step.id -eq "unique-root-redo" -and
        ($After.selectionKind -ne "None" -or $After.selectedObject -ne "" -or
         $After.selectedGraphRowKey -ne "")) {
        throw "Unique-root Redo did not clear restored selection K"
    }
}

function Test-ProcessAuthorityReset([object]$Previous, [object]$Current) {
    if ($null -eq $Previous -or
        [string]$Current.runNonce -eq [string]$Previous.runNonce -or
        [uint32]$Current.pid -eq 0 -or
        [uint32]$Current.pid -eq [uint32]$Previous.pid -or
        [int64]$Current.undo -ne 0 -or [int64]$Current.redo -ne 0) {
        throw "Unique-root subcase was not launched with fresh process authority"
    }
}

function Test-AuthorityContinuity([object]$Previous, [object]$Current) {
    foreach ($field in @(
        "authority", "revision", "undo", "redo", "documentDigest",
        "projectionDigest", "runtimeContentDigest", "hierarchyOrderDigest",
        "selectionKind", "selectedGraphRowKey", "selectedObject",
        "undoDescription", "redoDescription")) {
        if ($Previous.$field -ne $Current.$field) {
            throw "Authority continuity changed between steps: $field"
        }
    }
}

function Write-Continue(
    [object]$ReadyRecord,
    [object]$Step,
    [object]$Phase,
    [int]$SentCount) {
    $continuePath = $ReadyRecord.path.Replace(".ready.json", ".continue.json")
    Write-Json $continuePath ([ordered]@{
        documentKind = "mgif.editor-ui-os-replay-continue"
        schemaVersion = $SchemaVersion
        protocol = $Protocol
        runNonce = $RunNonce
        case = $Case
        planSha256 = $PlanContractHash
        step = [ordered]@{
            index = [int]$Step.index
            id = [string]$Step.id
            phase = [string]$Phase.id
        }
        readySha256 = [string]$ReadyRecord.sha256
        input = [ordered]@{
            kind = [string]$Phase.input.kind
            sendInputCount = $SentCount
        }
    })
    return [ordered]@{
        runNonce = $RunNonce
        subcase = [string]$Step.subcase
        path = Get-EvidenceRelativePath $continuePath
        bytes = (Get-Item -LiteralPath $continuePath).Length
        sha256 = Get-Hash $continuePath
        readySha256 = [string]$ReadyRecord.sha256
    }
}

try {
    $mainStop = if ($Case -eq "U02-hierarchy") { 20 } else { @($ExpectedSteps).Count - 1 }
    Start-ReplayProcess "main" 0 $mainStop
    Wait-MainWindow
    foreach ($step in $ExpectedSteps) {
        if ([bool]$step.relaunchProcessBefore) {
            if ($LeftButtonHeld -or $RightButtonHeld -or $HeldKeys.Count -ne 0) {
                throw "Cannot relaunch while a pointer button is held"
            }
            if (-not $Process.WaitForExit($TimeoutSeconds * 1000)) {
                throw "Main OS replay process did not exit at its boundary"
            }
            Complete-ReplayProcess
            if ($ProcessRecords[-1].exitCode -ne 0) {
                throw "Main OS replay process failed before unique-root relaunch"
            }
            Start-ReplayProcess "unique-root" ([int]$step.index) `
                (@($ExpectedSteps).Count - 1)
            Wait-MainWindow
        }
        $stepBefore = $null; $stepAfter = $null
        $stepBeforeDrop = $null
        foreach ($phase in @($step.phases)) {
            $readyRecord = Wait-Ready $step $phase
            $state = Test-Ready $readyRecord $step $phase $ProcessGraphKeys
            if ($null -eq $stepBefore) {
                $stepBefore = $state
                if ($null -ne $LastState -and
                    [string]$step.effect -ne "authority-reset") {
                    Test-AuthorityContinuity $LastState $state
                }
                if ([string]$step.effect -eq "authority-reset") {
                    Test-ProcessAuthorityReset $LastState $state
                }
            }
            if ([string]$phase.capture -ne "none") {
                if ($FocusSinkActive) { Restore-EditorFocus }
                $pngName = ("{0:D3}-{1}-{2}.png" -f
                    [int]$step.index, [string]$step.id, [string]$phase.capture)
            $capture = Capture-Client $WindowHandle `
                (Join-Path $ScreenshotRoot $pngName) `
                $state.targetCenterX $state.targetCenterY $state.captureViewport `
                $readyRecord
                $capture.step = [int]$step.index
                $capture.stepId = [string]$step.id
                $capture.subcase = [string]$step.subcase
                $capture.runNonce = $RunNonce
                $capture.point = [string]$phase.capture
                $capture.path = Get-EvidenceRelativePath ([string]$capture.path)
                $ScreenshotRecords.Add($capture)
            }
            if ([bool]$phase.requireDropAccepted -and
                [string]$phase.input.kind -eq "left-up") {
                $stepBeforeDrop = $state
            }
            $sent = Send-PhaseInput $phase $state
            if ([string]$phase.input.kind -ne "none") {
                $inputRecord = [ordered]@{
                    runNonce = $RunNonce
                    ordinal = [int]$phase.ordinal
                    step = [int]$step.index
                    stepId = [string]$step.id
                    phase = [string]$phase.id
                    kind = [string]$phase.input.kind
                    sendInputCount = $sent
                    readySha256 = [string]$readyRecord.sha256
                }
                if ($Case -ne "U02-hierarchy") {
                    $inputRecord.value = [string]$phase.input.value
                    if ([string]$phase.input.kind -eq "move-pointer") {
                        $position = Get-PhasePointerPosition $phase $state
                        $inputRecord.pointer = [ordered]@{
                            coordinateSpace = "screen-physical-px"
                            x = [int]$position.x; y = [int]$position.y
                        }
                    }
                    if ([string]$phase.input.kind -eq "escape-release-batch") {
                        $orderedEvents = if ([string]$phase.input.value -eq "escape-first") {
                            @("key-down:Escape", "left-up")
                        } else { @("left-up", "key-down:Escape") }
                        $inputRecord.nativeBatch = [ordered]@{
                            order = [string]$phase.input.value
                            events = $orderedEvents
                            sendInputCalls = 1
                            eventCount = 2
                            callbackSequenceBoundary = [uint64]$state.gizmo.nativeCallbackSequence
                            inputFrameBoundary = [uint64]$state.gizmo.inputFrame
                            deliveryFence = "native-queue-before-continue"
                        }
                    }
                    if ([string]$phase.input.kind -eq 'left-up-focus-loss') {
                        $inputRecord.deliveryFence = $LastInputDeliveryFence
                    }
                }
                $InputRecords.Add($inputRecord)
            }
            $continue = Write-Continue $readyRecord $step $phase $sent
            $phaseRecord = [ordered]@{
                runNonce = $RunNonce
                ordinal = [int]$phase.ordinal
                step = [int]$step.index
                stepId = [string]$step.id
                phase = [string]$phase.id
                path = Get-EvidenceRelativePath ([string]$readyRecord.path)
                bytes = [int64]$readyRecord.bytes
                sha256 = [string]$readyRecord.sha256
                frameSerial = [int64]$readyRecord.value.frame.frameSerial
                lifecycle = [string]$readyRecord.value.frame.lifecycle
                observedDispatches = @($readyRecord.value.observedDispatches)
            }
            if ($Case -in @("U03-gizmo-translate", "U03-gizmo-escape", "U03-gizmo-escape-release", "U03-gizmo-release-escape", "U03-gizmo-focus-no-release", "U03-gizmo-focus-same-frame", "U03-gizmo-focus-delivered-release")) {
                $phaseRecord.gizmo = $state.gizmo
            }
            $ReadyRecords.Add($phaseRecord)
            $ContinueRecords.Add($continue)
            [void]$ConsumedReady.Add([string]$readyRecord.path)
            $stepAfter = $state
        }
        Test-StepEffect $step $stepBefore $stepAfter $stepBeforeDrop
        $LastState = $stepAfter
    }
    if ($HeldKeys.Count -ne 0) {
        throw "OS replay reached its final boundary with a keyboard key held"
    }
    if (-not $Process.WaitForExit($TimeoutSeconds * 1000)) {
        throw "OS replay process did not exit after final continue"
    }
    Complete-ReplayProcess
}
finally {
    foreach ($heldKey in @($HeldKeys)) {
        $cleanup = [ordered]@{
            kind = "key-up"; value = [string]$heldKey
            reason = "best-effort-finally"
            delivered = $false; sendInputCount = 0
        }
        try {
            $cleanup.sendInputCount = Send-HeldKeyEdge "key-up" ([string]$heldKey)
            $cleanup.delivered = $true
        }
        catch { $cleanup.error = $_.Exception.Message }
        finally {
            [void]$HeldKeys.Remove([string]$heldKey)
            $CleanupInputRecords.Add($cleanup)
            try {
                Write-Json (Join-Path $EvidenceRoot "cleanup-input.json") `
                    ([ordered]@{ records = @($CleanupInputRecords) })
            }
            catch { }
        }
    }
    if ($LeftButtonHeld) {
        $cleanup = [ordered]@{
            kind = "left-up"
            reason = "best-effort-finally"
            delivered = $false
            sendInputCount = 0
        }
        try {
            $cleanup.sendInputCount = Send-MouseButton "left-up"
            $cleanup.delivered = $true
        }
        catch {
            $cleanup.error = $_.Exception.Message
        }
        finally {
            $script:LeftButtonHeld = $false
            $CleanupInputRecords.Add($cleanup)
            try {
                Write-Json (Join-Path $EvidenceRoot "cleanup-input.json") `
                    ([ordered]@{ records = @($CleanupInputRecords) })
            }
            catch { }
        }
    }
    if ($RightButtonHeld) {
        $cleanup = [ordered]@{
            kind = "right-up"
            reason = "best-effort-finally"
            delivered = $false
            sendInputCount = 0
        }
        try {
            $cleanup.sendInputCount = Send-MouseButton "right-up"
            $cleanup.delivered = $true
        }
        catch {
            $cleanup.error = $_.Exception.Message
        }
        finally {
            $script:RightButtonHeld = $false
            $CleanupInputRecords.Add($cleanup)
            try {
                Write-Json (Join-Path $EvidenceRoot "cleanup-input.json") `
                    ([ordered]@{ records = @($CleanupInputRecords) })
            }
            catch { }
        }
    }
    if ($null -ne $Process -and -not $Process.HasExited) {
        try {
            $Process.Kill()
            [void]$Process.WaitForExit(5000)
        }
        catch { }
    }
    if ($null -ne $FocusSink) {
        try { $FocusSink.Close(); $FocusSink.Dispose() } catch { }
        try {
            Write-Json (Join-Path $EvidenceRoot "focus-transfer.json") `
                ([ordered]@{ records = @($FocusTransferRecords) })
        }
        catch { }
    }
    if ($null -ne $Process -and $Process.HasExited) {
        try { Complete-ReplayProcess } catch { }
    }
    [IO.File]::WriteAllText(
        $StdoutPath, ($StdoutSegments -join [Environment]::NewLine),
        [Text.UTF8Encoding]::new($false))
    [IO.File]::WriteAllText(
        $StderrPath, ($StderrSegments -join [Environment]::NewLine),
        [Text.UTF8Encoding]::new($false))
    if ($PreviousDpiAwareness -ne [IntPtr]::Zero) {
        try {
            [void][MGIFEditorOsNative]::SetThreadDpiAwarenessContext(
                $PreviousDpiAwareness)
        }
        catch { }
    }
}

if ($ProcessRecords.Count -ne $(if ($Case -eq "U02-hierarchy") { 2 } else { 1 }) -or
    @($ProcessRecords | Where-Object { [int]$_.exitCode -ne 0 }).Count -ne 0) {
    throw "OS replay did not complete two clean process-bound subcases"
}
if ((Get-Hash $Executable) -ne $ExecutableHash) {
    throw "OS replay executable changed before completion"
}
if ($UseVulkanScreenshotLayer) {
    if ((Get-Hash ([string]$VulkanLayerInfo.manifestPath)) -ne
            [string]$VulkanLayerInfo.manifestSha256 -or
        (Get-Hash ([string]$VulkanLayerInfo.dllPath)) -ne
            [string]$VulkanLayerInfo.dllSha256) {
        throw "Vulkan screenshot layer binaries changed before completion"
    }
    foreach ($environmentRecord in @($VulkanLayerEnvironmentRecords)) {
        $environmentPath = Join-Path $EvidenceRoot ([string]$environmentRecord.path)
        if ((Get-Hash $environmentPath) -ne [string]$environmentRecord.sha256) {
            throw "Vulkan screenshot layer environment artifact changed before completion"
        }
    }
}
if ((Get-Item -LiteralPath $StderrPath).Length -ne 0) {
    throw "OS replay emitted stderr diagnostics"
}
$CombinedLog = (Get-Content -LiteralPath $StdoutPath -Raw) +
               [Environment]::NewLine +
               (Get-Content -LiteralPath $StderrPath -Raw)
if ($CombinedLog -match
    "(?im)(VUID-|SYNC-HAZARD|Validation Error|device lost|marker=failed|Assertion failed|Traceback)") {
    throw "OS replay emitted runtime/validation diagnostics"
}
$FinalResultRecords = [Collections.Generic.List[object]]::new()
foreach ($run in $ProcessRecords) {
    $finalResultPath = [string]$run.resultPath
    if (-not (Test-Path -LiteralPath $finalResultPath -PathType Leaf)) {
        throw "OS replay app result is missing for $($run.subcase)"
    }
    $finalResult = Get-Content -LiteralPath $finalResultPath -Raw |
        ConvertFrom-Json
    if ([string]$finalResult.documentKind -ne
            "mgif.editor-ui-os-replay-result" -or
        [int]$finalResult.schemaVersion -ne $SchemaVersion -or
        [string]$finalResult.protocol -ne $Protocol -or
        [string]$finalResult.runNonce -ne [string]$run.runNonce -or
        [string]$finalResult.case -ne $Case -or
        [string]$finalResult.subcase -ne [string]$run.subcase -or
        [string]$finalResult.planSha256 -ne $PlanContractHash -or
        [string]$finalResult.identity.executableSha256 -ne $ExecutableHash -or
        [string]$finalResult.identity.sourceSha256 -ne $SourceIdentityHash -or
        [string]$finalResult.dispatchMode -ne "win32-send-input" -or
        -not [bool]$finalResult.complete -or -not [bool]$finalResult.passed -or
        [int]$finalResult.startStep -ne [int]$run.startStep -or
        [int]$finalResult.stopStep -ne [int]$run.stopStep -or
        [int]$finalResult.stepCount -ne [int]$run.stepCount -or
        [int]$finalResult.phaseCount -ne [int]$run.phaseCount -or
        [int]$finalResult.validatedReceiptCount -ne [int]$run.phaseCount) {
        throw "OS replay app result is incomplete for $($run.subcase)"
    }
    $expectedRunSteps = @($ExpectedSteps | Where-Object {
        [int]$_.index -ge [int]$run.startStep -and
        [int]$_.index -le [int]$run.stopStep
    })
    $actualRunSteps = @($finalResult.steps)
    if ($actualRunSteps.Count -ne $expectedRunSteps.Count) {
        throw "OS replay result step summary cardinality is incomplete"
    }
    for ($index = 0; $index -lt $expectedRunSteps.Count; ++$index) {
        if ([int]$actualRunSteps[$index].index -ne
                [int]$expectedRunSteps[$index].index -or
            [string]$actualRunSteps[$index].id -ne
                [string]$expectedRunSteps[$index].id) {
            throw "OS replay result step identity is out of order"
        }
        $terminal = [pscustomobject]@{ phase = "settled" }
        $dispatchEnvelope = [pscustomobject]@{
            observedDispatches = @($actualRunSteps[$index].observedDispatches)
        }
        Test-ObservedDispatches $dispatchEnvelope $expectedRunSteps[$index] `
            $terminal
    }
    $FinalResultRecords.Add([ordered]@{
        subcase = [string]$run.subcase
        runNonce = [string]$run.runNonce
        path = Get-EvidenceRelativePath $finalResultPath
        bytes = (Get-Item -LiteralPath $finalResultPath).Length
        sha256 = Get-Hash $finalResultPath
    })
}
$expectedInjectedPhaseCount = @($ExpectedSteps | ForEach-Object { $_.phases } |
    Where-Object { [string]$_.input.kind -ne "none" }).Count
if (@($ReadyRecords).Count -ne $ExpectedPhaseCount -or
    @($ContinueRecords).Count -ne $ExpectedPhaseCount -or
    @($ScreenshotRecords).Count -ne (2 * @($ExpectedSteps).Count) -or
    @($InputRecords).Count -ne $expectedInjectedPhaseCount) {
    throw "OS replay evidence cardinality is incomplete"
}
if ($UseVulkanScreenshotLayer) {
    if (@($VulkanLayerFrameRecords).Count -ne @($ScreenshotRecords).Count) {
        throw "Vulkan screenshot-layer frame cardinality does not match PNG captures"
    }
    $selectedLayerKeys = @($ScreenshotRecords | ForEach-Object {
        if ($null -eq $_.layerFrame) { throw "PNG capture lacks a Vulkan layer frame fence" }
        if ([int64]$_.layerFrame.index -ne
            ([int64]$_.layerFrame.readyFrameSerial - 1)) {
            throw "Vulkan screenshot-layer frame index is not bound to ready frame"
        }
        "{0}:{1}" -f [string]$_.layerFrame.runNonce,
            [int64]$_.layerFrame.index
    })
    if (@($selectedLayerKeys | Sort-Object -Unique).Count -ne
        @($selectedLayerKeys).Count) {
        throw "Vulkan screenshot-layer frame indices were reused"
    }
    if (@($VulkanLayerEnvironmentRecords).Count -ne
        @($ProcessRecords).Count) {
        throw "Vulkan screenshot-layer environment cardinality differs from process cardinality"
    }
}
$ManifestRunRecords = @(
    foreach ($run in $ProcessRecords) {
        $manifestRun = [ordered]@{
            subcase = [string]$run.subcase
            runNonce = [string]$run.runNonce
            pid = [int]$run.pid
            exitCode = [int]$run.exitCode
            startStep = [int]$run.startStep
            stopStep = [int]$run.stopStep
            stepCount = [int]$run.stepCount
            phaseCount = [int]$run.phaseCount
            fixture = [string]$run.fixture
            fixtureSha256 = [string]$run.fixtureSha256
            resultPath = Get-EvidenceRelativePath ([string]$run.resultPath)
        }
        if ($UseVulkanScreenshotLayer) {
            $manifestRun.layer = $run.layer
        }
        $manifestRun
    }
)

$Manifest = [ordered]@{
    documentKind = "mgif.editor-ui-os-replay-capture"
    schemaVersion = $SchemaVersion
    protocol = $Protocol
    case = $Case
    configuration = $Configuration
    status = "PASS_TRUE_OS_REPLAY"
    dispatchMode = "win32-send-input"
    sendInputOnly = $true
    foregroundPidHwndValidated = $true
    targetProbeValidated = $true
    hoverConfirmed = $true
    prePostPng = $true
    runs = $ManifestRunRecords
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
        planSha256 = $PlanContractHash
        capability = [ordered]@{
            path = Get-EvidenceRelativePath $CapabilityArtifactPath
            bytes = (Get-Item -LiteralPath $CapabilityArtifactPath).Length
            sha256 = Get-Hash $CapabilityArtifactPath
            sourceRole = [string]$Capability.sourceRole
            executableHashClaim = $false
        }
        cacheSha256 = $CacheHash
    }
    client = [ordered]@{
        width = $Width; height = $Height; dpi = $ObservedDpi
    }
    validation = [ordered]@{
        layers = @("VK_LAYER_KHRONOS_validation")
        synchronization = $true
        messageFilter = $null
    }
    coverage = [ordered]@{
        steps = @($ExpectedSteps).Count
        phases = $ExpectedPhaseCount
        injectedPhases = $InputRecords.Count
        includesUniqueRoot = ($Case -eq "U02-hierarchy")
        includesSameParentNoop = ($Case -eq "U02-hierarchy")
        includesRootRootNoop = ($Case -eq "U02-hierarchy")
        includesFocusTransfer = ($Case -eq "U03-transform-gizmo" -or
                                 $Case -in @("U03-gizmo-focus-no-release", "U03-gizmo-focus-same-frame", "U03-gizmo-focus-delivered-release"))
    }
    ready = @($ReadyRecords)
    continue = @($ContinueRecords)
    inputs = @($InputRecords)
    inputCleanup = @($CleanupInputRecords)
    focusTransfers = @($FocusTransferRecords)
    startupActivations = @($StartupActivationRecords)
    bindings = @($BindingRecords)
    screenshots = @($ScreenshotRecords)
    results = @($FinalResultRecords)
    stdout = [ordered]@{
        path = "stdout.log"; bytes = (Get-Item $StdoutPath).Length
        sha256 = Get-Hash $StdoutPath
    }
    stderr = [ordered]@{
        path = "stderr.log"; bytes = (Get-Item $StderrPath).Length
        sha256 = Get-Hash $StderrPath
    }
}
if ($UseVulkanScreenshotLayer) {
    $Manifest.captureTransport = "vulkan-screenshot-layer"
    $Manifest.strictPixelOracle = $true
    $Manifest.validation.layers = @(
        "VK_LAYER_KHRONOS_validation", "VK_LAYER_LUNARG_screenshot")
    $Manifest.validation.screenshotLayer = [ordered]@{
        name = [string]$VulkanLayerInfo.name
        manifestPath = [string]$VulkanLayerInfo.manifestPath
        manifestSha256 = [string]$VulkanLayerInfo.manifestSha256
        dllPath = [string]$VulkanLayerInfo.dllPath
        dllSha256 = [string]$VulkanLayerInfo.dllSha256
        outputFormat = [string]$VulkanLayerInfo.outputFormat
        outputRoot = Get-EvidenceRelativePath $VulkanLayerOutputRoot
        frameCount = @($VulkanLayerFrameRecords).Count
        frames = @($VulkanLayerFrameRecords)
        environments = @($VulkanLayerEnvironmentRecords)
    }
}
if ($Case -in @("U03-inspector-transform", "U03-gizmo-translate", "U03-gizmo-escape", "U03-gizmo-escape-release", "U03-gizmo-release-escape", "U03-gizmo-focus-no-release", "U03-gizmo-focus-same-frame", "U03-gizmo-focus-delivered-release")) {
    $Manifest.status = "PASS_TRUE_OS_REPLAY_PARTIAL"
    $Manifest.parentCase = "U03-transform-gizmo"
    $Manifest.coverageComplete = $false
    $Manifest.coverage.scope = if ($Case -eq "U03-inspector-transform") {
        "inspector-trs-undo-redo"
    } elseif ($Case -eq "U03-gizmo-translate") {
        "gizmo-translate-y-outside-release-undo-redo"
    } elseif ($Case -eq "U03-gizmo-escape") {
        "gizmo-translate-y-escape-detach-retained-preview-no-history"
    } elseif ($Case -eq "U03-gizmo-focus-no-release") {
        "gizmo-translate-y-focus-loss-no-release-retained-preview-no-history"
    } elseif ($Case -eq "U03-gizmo-focus-same-frame") {
        "gizmo-translate-y-same-frame-release-focus-loss-retained-preview-no-history"
    } elseif ($Case -eq "U03-gizmo-focus-delivered-release") {
        "gizmo-translate-y-release-before-focus-loss-commit-history"
    } else {
        "gizmo-translate-y-same-frame-detach-retained-preview-no-history"
    }
    if ($Case -in @("U03-gizmo-escape-release", "U03-gizmo-release-escape")) {
        $Manifest.coverage.nativeInputOrder = if ($Case -eq "U03-gizmo-escape-release") {
            "escape-first"
        } else { "release-first" }
        $Manifest.coverage.sameFrameNativeOrderValidated = $true
    }
}
if (@($StartupActivationRecords).Count -gt 0) {
    $startupPath = Join-Path $EvidenceRoot "startup-activation.json"
    $Manifest.startupActivationArtifact = [ordered]@{
        path = "startup-activation.json"
        bytes = (Get-Item -LiteralPath $startupPath).Length
        sha256 = Get-Hash $startupPath
    }
}
Write-Json $ManifestPath $Manifest
Write-Output "Editor UI $Case true OS replay status $($Manifest.status): $EvidenceRoot"
