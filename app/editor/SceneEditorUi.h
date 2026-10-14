#pragma once

#include "EditorUiProbe.h"
#include "SceneEditorUiModel.h"

#include <cstdint>
#include <filesystem>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <string_view>

namespace demo::editor::ui {

enum class NativeDialogKind : std::uint8_t {
  openScene = 0,
  importMergeScene,
  saveScene,
};

struct NativeDialogRequest final {
  NativeDialogKind kind{NativeDialogKind::openScene};
  std::string_view title;
  std::filesystem::path initialDirectory;
  std::filesystem::path suggestedFile;
};

enum class CameraRequestKind : std::uint8_t {
  fitScene = 0,
  fitSelection,
  applyDocumentCamera,
  setDocumentCameraFromView,
};

struct CameraRequest final {
  CameraRequestKind kind{CameraRequestKind::fitScene};
  std::optional<CameraId> camera;
  std::optional<NodeId> node;
};

struct NodeDropValidation final {
  bool accepted{false};
  std::string reason;
};

struct MaterialEditDraft final {
  EditorObjectId target;
  std::vector<PropertyEdit> edits;
};

struct IconSet final {
  std::string sceneBrowser{"B"};
  std::string inspector{"I"};
  std::string grid{"G"};
  std::string gizmo{"T"};
  std::string snap{"S"};
  std::string play{">"};
  std::string pause{"||"};
  std::string step{">|"};
  std::string reset{"R"};
  std::string split{"S"};
  std::string merge{"M"};
  std::string assign{"A"};
  std::string copy{"C"};
  std::string paste{"P"};
  std::string debug{"Debug"};
};

class SceneEditorUiHost {
public:
  virtual ~SceneEditorUiHost() = default;

  [[nodiscard]] virtual std::optional<std::filesystem::path>
  nativeFileDialog(const NativeDialogRequest &request) = 0;

  [[nodiscard]] virtual std::string clipboardText() const = 0;
  virtual bool setClipboardText(std::string_view text) = 0;

  [[nodiscard]] virtual RuntimeActionResult
  dispatch(const UiCommand &command) = 0;

  [[nodiscard]] virtual RuntimeActionResult
  commitMaterialDraft(const MaterialEditDraft &) {
    return RuntimeActionResult{false, ProjectionRefresh::none,
                               "Material draft commit is not connected"};
  }

  [[nodiscard]] virtual RuntimeActionResult
  cameraRequest(const CameraRequest &request) = 0;

  [[nodiscard]] virtual NodeDropValidation
  validateNodeDrop(const ReparentNode &request) const = 0;

  [[nodiscard]] virtual ViewportSettings viewportSettings() const = 0;
  virtual void setViewportSettings(const ViewportSettings &settings) = 0;
  // The production host must opt in only after the central viewport has a
  // real input/overlay/picking sink. The default is fail-closed so a checked
  // toolbar state cannot imply a renderer effect that is not wired yet.
  [[nodiscard]] virtual bool viewportControlsAvailable() const noexcept {
    return false;
  }
  virtual void refreshProjection(ProjectionRefresh refresh) = 0;
};

struct RenderResult final {
  std::uint64_t submittedCommandCount{0};
  bool requestedSceneBrowserFocus{false};
  bool requestedInspectorFocus{false};
  std::string lastError;
};

class SceneEditorUi final {
public:
  explicit SceneEditorUi(IconSet icons = {});
  ~SceneEditorUi();

  SceneEditorUi(const SceneEditorUi &) = delete;
  SceneEditorUi &operator=(const SceneEditorUi &) = delete;
  SceneEditorUi(SceneEditorUi &&) noexcept;
  SceneEditorUi &operator=(SceneEditorUi &&) noexcept;

  [[nodiscard]] RenderResult render(const FrameModel &model,
                                    SceneEditorUiHost &host,
                                    EditorUiProbe *probe = nullptr);
  [[nodiscard]] bool hasMaterialEditPreview() const noexcept;
  [[nodiscard]] std::optional<MaterialEditDraft> materialEditDraft() const;

  // Call while the existing MGIF Settings window is active, immediately
  // after its renderer-settings separator.
  void renderMultipleScenesSettings(const FrameModel &model,
                                    SceneEditorUiHost &host);

  // External viewport selection expands the stable-ID parent path and scrolls
  // on the next Scene Graph render. It intentionally does not switch tabs.
  void requestFocusOnSelection(std::span<const EditorObjectId> parentPath,
                               EditorObjectId selected,
                               std::optional<GraphRowKey> exactRow = std::nullopt);
  void requestFocus(const SceneBrowserFocusRequest &request,
                    std::span<const EditorObjectId> parentPath,
                    EditorObjectId selected,
                    std::optional<GraphRowKey> exactRow = std::nullopt);
  void requestSceneBrowserFocus() noexcept;
  void requestInspectorFocus() noexcept;
  // Match native handling to ImGui's possibly trickled Escape press frames.
  void noteViewportEscapeHandled() noexcept;
  void discardViewportEscapeHandling() noexcept;

  [[nodiscard]] const PersistentState &state() const noexcept;
  [[nodiscard]] BrowserTab browserTab() const noexcept;
  [[nodiscard]] bool modalPending() const noexcept;
  void setState(PersistentState state) noexcept;
  // Clears modal/edit state that belongs to the previous document. A new
  // document starts with the same persistent window/viewport preferences.
  void resetDocumentState() noexcept;

  [[nodiscard]] static ClipboardMaterial
  parseMaterialClipboard(std::string_view text,
                         std::span<const MaterialChoice> materials);
  [[nodiscard]] static std::optional<PrimitiveContext>
  primitiveAssignmentContext(const FrameModel &model,
                             const GraphRow &row) noexcept;
  [[nodiscard]] static bool
  viewportShortcutEligible(const FrameModel &model, bool modalPending,
                           bool textInput, bool noModifiers,
                           bool controlsAvailable) noexcept;
  [[nodiscard]] static bool
  viewportCommandAvailable(CommandId id, bool controlsAvailable) noexcept;

private:
  struct Impl;
  std::unique_ptr<Impl> m_impl;
};

} // namespace demo::editor::ui
