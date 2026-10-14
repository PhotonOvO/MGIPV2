#pragma once

#include "EditorUiProbe.h"

#include <cstddef>
#include <array>
#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace demo::editor::testing::osreplay {

inline constexpr std::uint32_t kSchemaVersion = 1u;
inline constexpr std::string_view kProtocol =
    "mgif-editor-ui-os-replay-v1";
inline constexpr std::string_view kU02Case = "U02-hierarchy";
inline constexpr std::string_view kU03Case = "U03-transform-gizmo";
inline constexpr std::string_view kU03InspectorCase = "U03-inspector-transform";
inline constexpr std::string_view kU03GizmoCase = "U03-gizmo-translate";
inline constexpr std::string_view kU03EscapeCase = "U03-gizmo-escape";
inline constexpr std::string_view kU03EscapeReleaseCase = "U03-gizmo-escape-release";
inline constexpr std::string_view kU03ReleaseEscapeCase = "U03-gizmo-release-escape";
inline constexpr std::string_view kU03FocusNoReleaseCase =
    "U03-gizmo-focus-no-release";
inline constexpr std::string_view kU03FocusSameFrameCase =
    "U03-gizmo-focus-same-frame";
inline constexpr std::string_view kU03FocusDeliveredReleaseCase =
    "U03-gizmo-focus-delivered-release";

[[nodiscard]] constexpr bool isSameFrameGizmoCase(std::string_view caseId) noexcept {
  return caseId == kU03EscapeReleaseCase || caseId == kU03ReleaseEscapeCase;
}

[[nodiscard]] constexpr bool isEscapeGizmoCase(std::string_view caseId) noexcept {
  return caseId == kU03EscapeCase || isSameFrameGizmoCase(caseId);
}

[[nodiscard]] constexpr bool isFocusGizmoCase(std::string_view caseId) noexcept {
  return caseId == kU03FocusNoReleaseCase ||
         caseId == kU03FocusSameFrameCase ||
         caseId == kU03FocusDeliveredReleaseCase;
}

[[nodiscard]] constexpr bool isNativeGizmoReplayCase(
    std::string_view caseId) noexcept {
  return caseId == kU03GizmoCase || isEscapeGizmoCase(caseId) ||
         isFocusGizmoCase(caseId);
}

// The plan is intentionally expressed in semantic targets. The app-side probe
// resolves each graphRowRole to one live GraphRowKey immediately before it
// publishes a ready file. The external harness never guesses row coordinates.
enum class TargetSemantic : std::uint8_t {
  client = 0,
  graphRow,
  sceneAddNode,
  nodeDuplicate,
  nodeDelete,
  nodeAddChildMenu,
  nodeAddChildNode,
  nodeRename,
  renameInput,
  deleteAccept,
  nodeDragSource,
  hierarchyDropTarget,
  viewport,
  gizmoTranslate,
  gizmoRotate,
  gizmoScale,
  inspectorTranslationX,
  inspectorTranslationY,
  inspectorTranslationZ,
  inspectorRotationX,
  inspectorRotationY,
  inspectorRotationZ,
  inspectorScaleX,
  inspectorScaleY,
  inspectorScaleZ,
  toolbarGrid,
  toolbarGizmo,
  toolbarSnap,
  gizmoTranslateY,
  viewportOutside,
};

enum class ProbePhase : std::uint8_t {
  target = 0,
  hover,
  focus,
  dragTarget,
  dropHover,
  settled,
  boundary,
};

// Every non-none value is implemented by user32!SendInput in the external
// harness. In particular, there is no PostMessage or mouse_event fallback.
enum class InputKind : std::uint8_t {
  none = 0,
  movePointer,
  leftClick,
  rightClick,
  rightDown,
  rightUp,
  leftDown,
  leftUp,
  keyChord,
  replaceText,
  pressKey,
  keyDown,
  keyUp,
  escapeReleaseBatch,
  focusLoss,
  focusRestore,
  leftUpFocusLoss,
};

enum class CapturePoint : std::uint8_t {
  none = 0,
  before,
  after,
};

enum class EffectKind : std::uint8_t {
  presentationOnly = 0,
  selectionOnly,
  documentMutation,
  historyMove,
  documentNoopHistoryEnvelope,
  authorityReset,
};

enum class HistoryEffect : std::uint8_t {
  unchanged = 0,
  pushUndoClearRedo,
  moveUndoToRedo,
  moveRedoToUndo,
  reset,
};

enum class DispatchKind : std::uint8_t {
  invokeCommand = 0,
  selectObject,
  hierarchyAction,
  renameObject,
  reparentNode,
  propertyEdit,
};

struct DispatchExpectation final {
  ui::CommandSource source{ui::CommandSource::sceneBrowser};
  DispatchKind kind{DispatchKind::selectObject};
  std::string operation;
  std::string targetRole;
  std::string secondaryRole;
  std::string text;
  bool accepted{true};
  ui::ProjectionRefresh refresh{ui::ProjectionRefresh::none};
  // PropertyEdit payload details. Empty for all other dispatch kinds.
  std::string propertyPhase;
  std::string propertyValue;
};

struct TargetSpec final {
  TargetSemantic semantic{TargetSemantic::client};
  // Stable semantic role, for example "primary-scene" or "menu:add-node".
  std::string role;
  // Non-empty for every target anchored to a scene-graph occurrence. The
  // ready probe must include the exact resolved GraphRowKey for this role.
  std::string graphRowRole;

  [[nodiscard]] bool requiresGraphRowKey() const noexcept {
    return !graphRowRole.empty();
  }
};

struct InputSpec final {
  InputKind kind{InputKind::none};
  // Keys/text, or an optional "dx,dy" physical-pixel offset for movePointer.
  std::string value;
};

struct GizmoExpectation final {
  std::uint32_t phase{0};
  bool captureHeld{false};
  std::uint64_t beginCount{0};
  std::uint64_t previewCount{0};
  std::uint64_t commitCount{0};
  std::optional<bool> pointerOutside;
  std::uint64_t detachCount{0}, latchCount{0};
  bool noSelection{false};
  std::uint64_t deliveredReleaseCount{0};
  std::uint64_t escapePressCount{0};
  std::uint64_t focusLossCount{0};
  std::uint64_t suppressedReleaseCount{0};
};

struct PhaseSpec final {
  std::uint32_t ordinal{0};
  std::string id;
  ProbePhase phase{ProbePhase::target};
  TargetSpec target;
  InputSpec input;
  bool requireHovered{false};
  bool requireFocused{false};
  bool requireDropAccepted{false};
  CapturePoint capture{CapturePoint::none};
  // Number of dispatches already observed before this phase injects input.
  // U02 retains its frozen implicit timing; newer plans must specify this.
  std::optional<std::uint32_t> expectedDispatchCount;
  std::optional<GizmoExpectation> gizmo;
};

struct GraphRoleSpec final {
  std::string role;
  std::string subcase;
  EditorObjectKind objectKind{EditorObjectKind::invalid};
  std::string labelContains;
  std::string introducedByStep;
  std::string sameObjectAsRole;
  std::string directParentRole;
  std::string distinctFromRole;
  bool requireRoot{false};
};

struct StepSpec final {
  std::uint32_t index{0};
  std::string id;
  std::string subcase;
  EffectKind effect{EffectKind::presentationOnly};
  // The settled ready revision minus the target ready revision. A reset step
  // changes authority and therefore does not compare numeric revisions.
  std::int32_t expectedRevisionDelta{0};
  HistoryEffect history{HistoryEffect::unchanged};
  bool expectHierarchyOrderDigestChange{false};
  // A true value is a harness-owned fresh-process boundary. The app never
  // replaces the document internally to manufacture an authority reset.
  bool relaunchProcessBefore{false};
  std::vector<DispatchExpectation> expectedDispatches;
  std::vector<PhaseSpec> phases;
};

struct PlanValidation final {
  bool valid{false};
  std::string error;
};

struct ReplayPlan final {
  std::uint32_t schemaVersion{kSchemaVersion};
  std::string protocol{std::string(kProtocol)};
  std::string caseId{std::string(kU02Case)};
  std::string description;
  std::vector<GraphRoleSpec> graphRoles;
  std::vector<StepSpec> steps;

  [[nodiscard]] std::size_t phaseCount() const noexcept;
  [[nodiscard]] PlanValidation validate() const;
  // Cross-language canonical bytes used by the PowerShell harness when it
  // binds a capability and every ready/continue exchange to one exact plan.
  [[nodiscard]] std::string contractTrace() const;
  [[nodiscard]] std::string toJson() const;
};

// App-side probe value types. Rectangles use physical virtual-screen pixels,
// matching SendInput's MOUSEEVENTF_VIRTUALDESK coordinate space.
struct ScreenRect final {
  std::int32_t left{0};
  std::int32_t top{0};
  std::int32_t right{0};
  std::int32_t bottom{0};

  [[nodiscard]] bool valid() const noexcept {
    return right > left && bottom > top;
  }
};

struct ClientProbe final {
  std::uint64_t hwnd{0};
  std::uint32_t pid{0};
  std::uint32_t width{0};
  std::uint32_t height{0};
  std::uint32_t dpi{0};
  ScreenRect screenRect;
};

struct TargetProbe final {
  TargetSemantic semantic{TargetSemantic::client};
  std::string role;
  std::string graphRowRole;
  std::optional<ui::GraphRowKey> graphRowKey;
  std::string label;
  std::uint64_t order{0};
  std::uint32_t itemId{0};
  std::uint32_t windowToken{0};
  std::uint32_t popupToken{0};
  std::uint32_t modalToken{0};
  ui::EditorUiProbeCoordinateSpace probeCoordinateSpace{
      ui::EditorUiProbeCoordinateSpace::imguiScreenLogical};
  ui::EditorUiProbeRect probeRect;
  std::uint64_t revision{0};
  ScreenRect rect;
  std::uint32_t matchCount{0};
  bool visible{false};
  bool enabled{false};
  bool hovered{false};
  bool focused{false};
  bool dropAccepted{false};
  std::string observationSource;
};

struct GizmoTerminalProbe final {
  std::string kind;
  std::uint64_t sequence{0};
  std::uint64_t callbackSequence{0};
};

struct GizmoProbe final {
  std::uint64_t renderedFrame{0}, inputFrame{0};
  std::uint64_t documentRevision{0}, selectionRevision{0}, mappingEpoch{0};
  std::optional<EditorObjectId> node;
  std::optional<EditorObjectId> gestureNode;
  ui::EditorUiProbeRect viewportRect, componentRect, outsideRect;
  ui::EditorUiProbePoint pointer;
  bool hasPointer{false}, componentValid{false}, componentEnabled{false};
  bool ndcYDown{false};
  std::uint32_t hoveredComponent{255}, activeComponent{255}, phase{0};
  bool captureHeld{false};
  std::uint64_t beginCount{0}, previewCount{0}, commitCount{0};
  std::uint64_t detachCount{0}, latchCount{0};
  std::uint64_t lastTerminalFrame{0}, lastTerminalSequence{0};
  std::uint64_t nativeCallbackSequence{0};
  std::string terminal;
  std::uint64_t deliveredReleaseCount{0}, escapePressCount{0};
  std::uint64_t focusLossCount{0}, suppressedReleaseCount{0};
  bool lastTerminalEventsOverflowed{false};
  std::vector<GizmoTerminalProbe> lastTerminalEvents;
  std::array<double, 3> translation{0.0, 0.0, 0.0};
  std::array<double, 4> rotation{0.0, 0.0, 0.0, 1.0};
  std::array<double, 3> scale{1.0, 1.0, 1.0};
  [[nodiscard]] std::string toJson() const;
};

struct AuthorityProbe final {
  std::uint64_t documentAuthority{0};
  std::uint64_t documentRevision{0};
  std::uint64_t selectionRevision{0};
  std::size_t undoCount{0};
  std::size_t redoCount{0};
  std::string documentDigest;
  std::string projectionDigest;
  std::string runtimeContentDigest;
  // SHA-256 over UTF-8 lines in document-index order. Each scene line is
  // "scene<TAB>ID<TAB>root-ID...<LF>" and each node line is
  // "node<TAB>ID<TAB>child-ID...<LF>". IDs are
  // "kind:slot:generation:authority" and collection order is preserved.
  // This proves append-tail behavior without exposing mutable collections.
  std::string hierarchyOrderDigest;
  std::string selectionKind;
  std::optional<ui::GraphRowKey> selectedGraphRowKey;
  std::optional<EditorObjectId> selectedObject;
  std::string undoDescription;
  std::string redoDescription;
};

struct ObservedDispatch final {
  std::uint64_t sequence{0};
  ui::CommandSource source{ui::CommandSource::sceneBrowser};
  DispatchKind kind{DispatchKind::selectObject};
  std::string operation;
  std::string targetRole;
  std::string secondaryRole;
  std::string text;
  bool accepted{false};
  ui::ProjectionRefresh refresh{ui::ProjectionRefresh::none};
  std::string error;
  // Populated only for PropertyEdit dispatches; retained in ready evidence so
  // begin/preview/commit and the exact value are externally auditable.
  std::string propertyPhase;
  std::string propertyValue;
};

// The SHA-256 of the exact UTF-8 ready-file bytes is deliberately not stored
// inside this self-contained envelope. The harness computes it and binds it in
// the matching continue receipt as readySha256.
struct ReadyProbe final {
  std::uint32_t schemaVersion{kSchemaVersion};
  std::string protocol{std::string(kProtocol)};
  std::string runNonce;
  std::string caseId{std::string(kU02Case)};
  std::string planSha256;
  std::string executableSha256;
  std::string sourceSha256;
  std::uint32_t step{0};
  std::string stepId;
  std::string phaseId;
  std::uint64_t renderedFrame{0};
  ui::EditorUiProbeFrameBinding frame;
  ClientProbe client;
  AuthorityProbe authority;
  TargetProbe target;
  // Cumulative for the current StepSpec. Non-terminal phases may contain an
  // exact prefix; settled/boundary phases must contain the complete sequence.
  std::vector<ObservedDispatch> observedDispatches;
  std::optional<GizmoProbe> gizmo;

  [[nodiscard]] PlanValidation validateAgainst(const ReplayPlan &plan) const;
  [[nodiscard]] std::string toJson() const;
};

[[nodiscard]] std::string_view targetSemanticName(TargetSemantic value) noexcept;
[[nodiscard]] std::optional<ui::EditorUiProbeSemantic>
editorProbeSemantic(TargetSemantic value) noexcept;
[[nodiscard]] std::string_view probePhaseName(ProbePhase value) noexcept;
[[nodiscard]] std::string_view inputKindName(InputKind value) noexcept;
[[nodiscard]] std::string_view capturePointName(CapturePoint value) noexcept;
[[nodiscard]] std::string_view effectKindName(EffectKind value) noexcept;
[[nodiscard]] std::string_view historyEffectName(HistoryEffect value) noexcept;
[[nodiscard]] std::string_view dispatchKindName(DispatchKind value) noexcept;

[[nodiscard]] ReplayPlan makeU02OsReplayPlan();
[[nodiscard]] ReplayPlan makeU03OsReplayPlan();
[[nodiscard]] ReplayPlan makeU03InspectorOsReplayPlan();
[[nodiscard]] ReplayPlan makeU03GizmoOsReplayPlan();
[[nodiscard]] ReplayPlan makeU03EscapeOsReplayPlan();
[[nodiscard]] ReplayPlan makeU03SameFrameOsReplayPlan(bool escapeFirst);
[[nodiscard]] ReplayPlan makeU03FocusOsReplayPlan(std::string_view caseId);

} // namespace demo::editor::testing::osreplay
