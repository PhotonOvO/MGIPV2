#pragma once

// This header describes the acceptance-only editor harness.  The production
// application does not link EditorTestDriver.cpp; the class is deliberately a
// value/intent bridge and never exposes a document, SceneAsset, renderer, or
// backend handle.

#include "SceneEditorUiModel.h"

#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <functional>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <variant>
#include <vector>

namespace demo::editor::testing {

// The spelling of these values is part of the acceptance protocol.  The
// Saving/Merging/Compacting value is intentionally one presentation state,
// even when a controller internally distinguishes those operations.
enum class UiState : std::uint8_t {
  noDocument = 0,
  loadingCandidate,
  readyClean,
  readyModified,
  gizmoHover,
  gizmoDragging,
  modal,
  savingMergingCompacting,
  error,
};

inline constexpr std::size_t uiStateCount = 9u;

[[nodiscard]] std::string_view uiStateName(UiState state) noexcept;
[[nodiscard]] std::optional<UiState>
parseUiState(std::string_view name) noexcept;

enum class ActionKind : std::uint8_t {
  invokeCommand = 0,
  selectObject,
  activateScene,
  hierarchy,
  rename,
  reparent,
  animation,
  selectVariant,
  property,
  material,
  camera,
  sceneTransform,
  rebuildRenderNodes,
  controllerAction,
  captureState,
  screenshotMarker,
};

[[nodiscard]] std::string_view actionKindName(ActionKind kind) noexcept;

struct CaptureStateAction final {
  std::string label;
};

struct ScreenshotMarkerAction final {
  std::string label;
};

enum class ControllerActionKind : std::uint8_t {
  setGizmoHover = 0,
  beginGizmoTransform,
  previewGizmoTransform,
  finishGizmoTransform,
  dismissError,
  activateSceneByIndex,
};

// Typed test-only calls for controller entry points that are intentionally not
// UiCommand variants.
struct ControllerAction final {
  ControllerActionKind kind{ControllerActionKind::setGizmoHover};
  std::optional<NodeId> node;
  std::optional<NodeTransform> transform;
  bool hovered{false};
  bool physicalRelease{false};
  bool escapePressed{false};
  bool focusLost{false};
  std::optional<std::size_t> sceneIndex;
};

using ActionPayload = std::variant<ui::InvokeCommand, ui::SelectObject,
                                   ui::ActivateScene, ui::HierarchyAction,
                                   ui::RenameObject, ui::ReparentNode,
                                   ui::AnimationAction, ui::SelectVariant,
                                   ui::PropertyEdit, ui::MaterialAction,
                                   ui::CameraAction, ui::SceneTransformAction,
                                   ui::RebuildRenderNodes, ControllerAction,
                                   CaptureStateAction, ScreenshotMarkerAction>;

struct TestAction final {
  static constexpr std::uint32_t kSchemaVersion = 1;

  std::uint32_t schemaVersion{kSchemaVersion};
  std::uint64_t step{0};
  std::string id;
  ui::CommandSource source{ui::CommandSource::menu};
  ActionPayload payload{CaptureStateAction{}};

  [[nodiscard]] std::string toJson() const;
};

struct ScriptStep final {
  static constexpr std::uint32_t kSchemaVersion = 1;

  std::uint32_t schemaVersion{kSchemaVersion};
  std::string id;
  TestAction action;
  std::optional<UiState> expectedState;
  std::optional<bool> expectedAccepted;

  [[nodiscard]] std::string toJson() const;
};

struct TestScript final {
  static constexpr std::uint32_t kSchemaVersion = 1;

  std::uint32_t schemaVersion{kSchemaVersion};
  std::string id;
  std::string description;
  std::vector<ScriptStep> steps;
  // Explicit non-claims. A script can prove every encoded action while still
  // refusing to claim the full visual workflow is covered.
  std::vector<std::string> coverageGaps;

  [[nodiscard]] bool coverageComplete() const noexcept {
    return coverageGaps.empty();
  }
  [[nodiscard]] std::string toJson() const;
};

struct CommandManifestEntry final {
  ui::CommandId id{ui::CommandId::newScene};
  std::string menu;
  std::string label;
  std::string shortcut;
  bool enabled{false};
  bool checked{false};
};

struct AuthoritySummary final {
  static constexpr std::uint32_t kSchemaVersion = 1;

  std::uint32_t schemaVersion{kSchemaVersion};
  bool hasDocument{false};
  bool hasProjection{false};
  std::uint64_t documentRevision{0};
  std::uint64_t cleanRevision{0};
  std::uint64_t lifecycleRevision{0};
  std::uint64_t mappingEpoch{0};
  std::uint64_t sceneActivationEpoch{0};
  std::uint64_t selectionRevision{0};
  std::size_t undoCount{0};
  std::size_t redoCount{0};
  std::string sourcePath;
  std::string semanticHash;
  std::string documentDigest;
  std::string projectionDigest;
  std::string selectionKind;

  [[nodiscard]] std::string toJson() const;
};

struct StateSnapshot final {
  static constexpr std::uint32_t kSchemaVersion = 1;

  std::uint32_t schemaVersion{kSchemaVersion};
  UiState state{UiState::noDocument};
  ui::FrameModel frame;
  ui::PersistentState persistent;
  std::optional<AuthoritySummary> authority;
  std::vector<CommandManifestEntry> commands;
  std::string captureLabel;

  [[nodiscard]] std::string toJson() const;
};

struct ScreenshotMarker final {
  static constexpr std::uint32_t kSchemaVersion = 1;

  std::uint32_t schemaVersion{kSchemaVersion};
  std::uint64_t sequence{0};
  std::string label;
  UiState state{UiState::noDocument};
};

enum class ActionStatus : std::uint8_t {
  accepted = 0,
  rejected,
  blocked,
  invalid,
  disabled,
};

[[nodiscard]] std::string_view actionStatusName(ActionStatus status) noexcept;

struct ActionResult final {
  static constexpr std::uint32_t kSchemaVersion = 1;

  std::uint32_t schemaVersion{kSchemaVersion};
  std::uint64_t sequence{0};
  std::string actionId;
  ActionKind kind{ActionKind::captureState};
  ActionStatus status{ActionStatus::invalid};
  bool executed{false};
  bool accepted{false};
  std::string error;
  std::optional<ui::RuntimeActionResult> runtime;
  StateSnapshot beforeState;
  StateSnapshot state;

  [[nodiscard]] std::string toJson() const;
};

enum class ReplayStatus : std::uint8_t {
  passed = 0,
  failed,
  invalid,
  blocked,
};

[[nodiscard]] std::string_view replayStatusName(ReplayStatus status) noexcept;

struct ReplayResult final {
  static constexpr std::uint32_t kSchemaVersion = 1;

  std::uint32_t schemaVersion{kSchemaVersion};
  std::string scriptId;
  ReplayStatus status{ReplayStatus::invalid};
  bool passed{false};
  std::string error;
  std::vector<ActionResult> steps;

  [[nodiscard]] std::string toJson() const;
};

struct ScriptRegistrationResult final {
  bool accepted{false};
  std::string error;
};

// Resolved from one live EditorController authority before constructing a
// scenario. IDs are never reused across document replacement.
struct ScenarioBindings final {
  std::filesystem::path primaryDocument;
  std::filesystem::path invalidDocument;
  std::filesystem::path mergeDocument;
  std::filesystem::path saveTarget;
  SceneId primaryScene;
  std::optional<SceneId> secondaryScene;
  NodeId primaryNode;
  std::optional<NodeId> secondaryNode;
  std::optional<NodeTransform> nodeTransform;
  std::optional<SceneNodeOccurrence> reparentSource;
  std::optional<NodeId> reparentTarget;
  EditorSelectionSnapshot nodeSelection;
  EditorSelectionSnapshot primitiveSelection;
  std::optional<ui::PrimitiveContext> primitive;
  std::optional<MaterialId> primaryMaterial;
  std::optional<MaterialId> alternateMaterial;
  std::optional<CameraId> camera;
  std::optional<NodeId> cameraNode;
  EditorSelectionSnapshot cameraSelection;
  std::optional<LightId> light;
  EditorSelectionSnapshot lightSelection;
  std::optional<AnimationId> animation;
  std::string unicodeName;
};

class EditorTestDriver final {
public:
  struct Callbacks final {
    // The driver sends only typed UiCommand values to the application owner.
    std::function<ui::RuntimeActionResult(const ui::UiCommand &)> dispatch;
    std::function<ui::RuntimeActionResult(const ControllerAction &)> control;
    // A provider is sampled into an immutable value snapshot at each action
    // boundary.  It may safely return a controller-owned transient model.
    std::function<ui::FrameModel()> frame;
    std::function<ui::PersistentState()> persistent;
    std::function<AuthoritySummary()> authority;
    std::function<void(const StateSnapshot &)> stateCaptured;
    std::function<void(const ScreenshotMarker &)> screenshot;
  };

  struct Options final {
    // This is a harness option, not a production feature flag.  Acceptance
    // builds leave it enabled; a disabled driver fails closed without calling
    // any callback.
    bool enabled{true};
    bool strictSequence{true};
    bool allowCustomScripts{false};
  };

  explicit EditorTestDriver(Callbacks callbacks = {}, Options options = {});
  explicit EditorTestDriver(bool enabled) noexcept;
  ~EditorTestDriver() = default;

  EditorTestDriver(const EditorTestDriver &) = delete;
  EditorTestDriver &operator=(const EditorTestDriver &) = delete;
  EditorTestDriver(EditorTestDriver &&) noexcept = default;
  EditorTestDriver &operator=(EditorTestDriver &&) noexcept = default;

  [[nodiscard]] bool enabled() const noexcept { return m_options.enabled; }
  void setEnabled(bool enabled) noexcept { m_options.enabled = enabled; }

  // Local snapshots are useful for deterministic headless tests.  A callback
  // takes precedence when installed.
  void setFrameModel(ui::FrameModel frame);
  void setPersistentState(ui::PersistentState state) noexcept;
  [[nodiscard]] StateSnapshot snapshot(std::string_view label = {}) const;
  [[nodiscard]] StateSnapshot captureState(std::string_view label);
  [[nodiscard]] std::span<const StateSnapshot> capturedStates() const noexcept;
  [[nodiscard]] std::span<const ScreenshotMarker>
  screenshotMarkers() const noexcept;

  [[nodiscard]] std::span<const CommandManifestEntry>
  commandManifest() const;
  [[nodiscard]] std::string commandManifestJson() const;
  [[nodiscard]] std::string scriptManifestJson() const;
  [[nodiscard]] static std::string
  surfaceManifestJson(const StateSnapshot &snapshot);

  [[nodiscard]] ActionResult dispatch(const TestAction &action);
  [[nodiscard]] ActionResult dispatch(const ScriptStep &step);
  [[nodiscard]] bool markScreenshot(std::string_view label);

  [[nodiscard]] ScriptRegistrationResult registerScript(TestScript script);
  [[nodiscard]] ScriptRegistrationResult
  registerDefaultScripts(const ScenarioBindings &bindings);
  [[nodiscard]] bool hasScript(std::string_view id) const noexcept;
  [[nodiscard]] std::span<const TestScript> scripts() const noexcept;
  [[nodiscard]] ReplayResult replay(std::string_view scriptId);
  [[nodiscard]] ReplayResult replay(const TestScript &script);

  // Deterministic schema documents are consumed by external acceptance
  // tooling; they intentionally describe only the closed typed vocabulary.
  [[nodiscard]] static std::string actionSchemaJson();
  [[nodiscard]] static std::string stateSchemaJson();
  [[nodiscard]] static std::string scriptSchemaJson();
  [[nodiscard]] static ActionKind
  kindOf(const ActionPayload &payload) noexcept;
  [[nodiscard]] static std::optional<TestScript>
  defaultScript(std::string_view id, const ScenarioBindings &bindings,
                std::string *error = nullptr);
  [[nodiscard]] static std::vector<TestScript>
  defaultScripts(const ScenarioBindings &bindings,
                 std::vector<std::string> *errors = nullptr);

private:
  [[nodiscard]] StateSnapshot readSnapshot(std::string_view label = {}) const;
  [[nodiscard]] ActionResult invalidAction(const TestAction &action,
                                           ActionStatus status,
                                           std::string message) const;
  [[nodiscard]] ActionResult dispatchDomainAction(const TestAction &action,
                                                  StateSnapshot before);
  [[nodiscard]] bool validateScript(const TestScript &script,
                                    std::string *error) const;
  [[nodiscard]] static bool knownScenario(std::string_view id) noexcept;
  [[nodiscard]] static bool isDomainPayload(const ActionPayload &payload) noexcept;

  Callbacks m_callbacks;
  Options m_options;
  ui::FrameModel m_frame;
  ui::PersistentState m_persistent;
  mutable std::vector<CommandManifestEntry> m_manifest;
  std::vector<StateSnapshot> m_capturedStates;
  std::vector<ScreenshotMarker> m_screenshotMarkers;
  std::vector<TestScript> m_scripts;
  std::uint64_t m_nextSequence{0};
};

} // namespace demo::editor::testing
