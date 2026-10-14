#pragma once

#include "SceneEditorUi.h"
#include "SceneSelection.h"

#include <mgif/scene_editor/GltfDocumentIO.h>
#include <mgif/scene_editor/SceneChangeCapture.h>
#include <mgif/scene_editor/SceneProjection.h>
#include <mgif/scene_editor/EditorSession.h>
#include <mgif/scene_editor/EditorPicking.h>
#include <mgif/scene_editor/SceneCommands.h>
#include <mgif/scene_editor/SceneMaterialPreview.h>

#include "../../scene/editor/SceneProjectionDetail.h"

#include <functional>
#include <cstdint>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace demo::editor {
class IOsTextClipboard;
}

namespace demo::editor::ui {

struct EditorControllerTestAccess;

// MGIF-owned bridge between the Dear ImGui shell and the CPU editor domain.
// The bridge deliberately exposes value callbacks only: the UI never receives
// a TinyGLTF model, SceneAsset mutable reference, renderer handle, or backend
// native object.
class EditorController final : public SceneEditorUiHost {
public:
  // A publication is a synchronous, immutable view crossing the
  // UI/application boundary. `canonical` is present for committed document,
  // activation, and root-state mutations and absent for UI-only refreshes.
  struct ProjectionPublication final {
    const SceneProjectionResult *projection{nullptr};
    ProjectionRefresh refresh{ProjectionRefresh::none};
    const SceneCanonicalizationResult *canonical{nullptr};
  };

  struct Callbacks final {
    std::function<void(const SceneProjectionResult &, ProjectionRefresh)> onProjection;
    std::function<void(const ProjectionPublication &)> onPublication;
    // Lifecycle publications are value-only and are emitted before and after
    // candidate/operation transitions.  A callback may synchronously call
    // buildFrameModel() to capture the transient state; exceptions are
    // swallowed at the controller boundary.
    std::function<void(LifecycleState, const OperationState &)> onLifecycle;
    std::function<void(std::string_view)> onError;
    std::function<std::optional<std::filesystem::path>(const NativeDialogRequest &)> fileDialog;
    IOsTextClipboard *clipboard{nullptr};
    std::function<RuntimeActionResult(const CameraRequest &)> cameraRequest;
    std::function<void()> requestExit;
    // Called after the document/session has been cleared. This is the single
    // lifecycle seam for the host to reset the renderer scene; no empty
    // projection is published for a no-document state.
    std::function<void()> onDocumentReset;
    // Keep viewport controls disabled until the host wires the input router,
    // picking, gizmo and overlay contracts into its central viewport.
    std::function<bool()> viewportControlsAvailable;
    std::function<bool()> rendererBusy;
    std::function<bool()> rendererPublicationFailed;
  };

  explicit EditorController(Callbacks callbacks = {});
  ~EditorController() override;

  EditorController(const EditorController &) = delete;
  EditorController &operator=(const EditorController &) = delete;

  [[nodiscard]] bool open(const std::filesystem::path &path,
                          std::string *error = nullptr);
  [[nodiscard]] bool replaceDocument(std::unique_ptr<GltfDocument> document,
                                      std::string *error = nullptr);
  void unload() noexcept;

  [[nodiscard]] bool hasDocument() const noexcept;
  [[nodiscard]] EditorSessionBundle *session() noexcept;
  [[nodiscard]] const EditorSessionBundle *session() const noexcept;
  [[nodiscard]] const SceneProjectionResult *projection() const noexcept;
  [[nodiscard]] SceneProjectionResult *projection() noexcept;
  [[nodiscard]] SceneEditorUi &ui() noexcept { return m_ui; }
  [[nodiscard]] std::optional<SceneMaterialColorPreview>
  materialColorPreview() const noexcept;
  [[nodiscard]] const SceneEditorUi &ui() const noexcept { return m_ui; }
  [[nodiscard]] FrameModel buildFrameModel(LayoutState layout = {}) const;
  [[nodiscard]] RenderResult render(LayoutState layout = {},
                                    EditorUiProbe *probe = nullptr);

  // The controller is the sole lifecycle authority.  Overrides are intended
  // for the host's modal/viewport producer and deterministic acceptance
  // probes; they never expose a mutable document or renderer object.
  [[nodiscard]] LifecycleState lifecycleState() const noexcept;
  [[nodiscard]] OperationState operationState() const;
  [[nodiscard]] LifecycleSnapshot lifecycleSnapshot() const;
  [[nodiscard]] std::uint64_t lifecycleRevision() const noexcept;
  void setLifecycleOverride(LifecycleState state,
                            OperationState operation = {});
  void setLifecycleOverride(std::optional<LifecycleState> state,
                            OperationState operation = {});
  void clearLifecycleOverride() noexcept;
  void setOperationProgress(std::optional<float> progress);
  void dismissError() noexcept;

  [[nodiscard]] RuntimeActionResult dispatch(const UiCommand &command) override;
  [[nodiscard]] RuntimeActionResult
  commitMaterialDraft(const MaterialEditDraft &draft) override;
  [[nodiscard]] RuntimeActionResult cameraRequest(const CameraRequest &request) override;
  [[nodiscard]] RuntimeActionResult setCameraFromView(
      CameraId camera, NodeId node, const CameraProperties &properties,
      const NodeTransform &transform);
  // Central viewport interaction enters the same continuous-edit/history path
  // as the Inspector. These methods intentionally expose value types only.
  [[nodiscard]] RuntimeActionResult applyViewportSelection(
      const EditorSelectionSnapshot &selection, bool toggle);
  [[nodiscard]] RuntimeActionResult
  applyPickingSelection(const EditorSelectionEffect &effect);
  void setSelectionOccurrenceLookup(
      const ISceneSelectionOccurrenceLookup *occurrences) noexcept;
  [[nodiscard]] const SceneSelection &sceneSelection() const noexcept;
  [[nodiscard]] SelectionRevision selectionRevision() const noexcept;
  [[nodiscard]] std::vector<SelectionChanged> drainSelectionChanges();
  [[nodiscard]] std::optional<NodeTransform>
  viewportNodeTransform(NodeId node) const noexcept;
  [[nodiscard]] std::vector<NodeId> sceneNodeParentPath(
      SceneId scene, NodeId node) const;
  [[nodiscard]] RuntimeActionResult beginGizmoTransform(NodeId node);
  [[nodiscard]] RuntimeActionResult previewGizmoTransform(
      NodeId node, const NodeTransform &transform);
  [[nodiscard]] RuntimeActionResult finishGizmoTransform(
      NodeId node, bool physicalRelease, bool escapePressed,
      bool focusLost);
  // Advances runtime-only animation playback. This never mutates the
  // document or history; a changed pose is published as a full projection.
  [[nodiscard]] RuntimeActionResult updateAnimation(double deltaSeconds);
  [[nodiscard]] NodeDropValidation
  validateNodeDrop(const ReparentNode &request) const override;
  [[nodiscard]] ViewportSettings viewportSettings() const override;
  void setViewportSettings(const ViewportSettings &settings) override;
  [[nodiscard]] bool viewportControlsAvailable() const noexcept override;
  void setViewportInteraction(bool hovered, bool focused) noexcept;
  // The central viewport reports handle hover/drag separately from generic
  // viewport focus.  Begin/preview/finishGizmoTransform update dragging too;
  // this producer covers hover-only frames and headless snapshots.
  void setGizmoInteraction(bool hovered, bool dragging) noexcept;
  void setGizmoHover(bool hovered) noexcept;
  void noteViewportEscapeHandled() noexcept;
  void discardViewportEscapeHandling() noexcept;
  void refreshProjection(ProjectionRefresh refresh) override;

  [[nodiscard]] std::optional<std::filesystem::path>
  nativeFileDialog(const NativeDialogRequest &request) override;
  [[nodiscard]] std::string clipboardText() const override;
  bool setClipboardText(std::string_view text) override;

  [[nodiscard]] const PersistentState &persistentState() const noexcept;
  void setPersistentState(PersistentState state) noexcept;

private:
  friend struct EditorControllerTestAccess;
  class SelectionAuthorityBridge;

  struct LifecycleOverride final {
    LifecycleState state{LifecycleState::noDocument};
    OperationState operation;
  };

  struct LifecycleOperationFrame final {
    bool owner{false};
    bool finished{false};
  };

  [[nodiscard]] RuntimeActionResult invoke(const InvokeCommand &command);
  [[nodiscard]] RuntimeActionResult select(const SelectObject &command);
  [[nodiscard]] RuntimeActionResult activate(const ActivateScene &command);
  [[nodiscard]] RuntimeActionResult hierarchy(const HierarchyAction &command);
  [[nodiscard]] RuntimeActionResult rename(const RenameObject &command);
  [[nodiscard]] RuntimeActionResult reparent(const ReparentNode &command);
  [[nodiscard]] RuntimeActionResult animation(const AnimationAction &command);
  [[nodiscard]] RuntimeActionResult property(const PropertyEdit &command);
  [[nodiscard]] RuntimeActionResult material(const MaterialAction &command);
  [[nodiscard]] RuntimeActionResult camera(const CameraAction &command);
  [[nodiscard]] RuntimeActionResult sceneTransform(const SceneTransformAction &command);

  [[nodiscard]] RuntimeActionResult publishMutation(
      ProjectionRefresh refresh, const MutationResult &result);
  [[nodiscard]] RuntimeActionResult publishCommand(ProjectionRefresh refresh,
                                                   const CommandResult &result);
  [[nodiscard]] RuntimeActionResult publishContinuousPreview(
      ProjectionRefresh refresh, const ContinuousResult &result);
  [[nodiscard]] SceneCaptureSnapshot captureSnapshot(
      const SceneProjectionResult &projection, bool includePayload = true) const;
  [[nodiscard]] std::optional<SceneCanonicalizationResult>
  canonicalizeMutation(const SceneCaptureSnapshot &before,
                       const SceneProjectionResult &after,
                       const SceneMutationEvidence &evidence,
                       std::optional<FullRebuildReason> reason) const;
  [[nodiscard]] std::optional<SceneCanonicalizationResult>
  canonicalizeMutation(const SceneProjectionResult &before,
                       const SceneProjectionResult &after,
                       const SceneMutationEvidence &evidence,
                       std::optional<FullRebuildReason> reason) const;
  void captureBeforeMutation();
  void restoreBeforeMutationAnimation() noexcept;
  [[nodiscard]] LifecycleOperationFrame beginLifecycleOperation(
      LifecycleState state, LifecycleOperation operation,
      std::string label);
  void finishLifecycleOperation(LifecycleOperationFrame &frame, bool succeeded,
                                std::string error = {});
  void setLifecycleError(std::string message);
  void clearLifecycleError() noexcept;
  void notifyLifecycle() noexcept;
  [[nodiscard]] LifecycleState computedLifecycleState(
      bool rendererPublicationFailed = false,
      bool rendererBusy = false) const noexcept;
  void emitProjection(ProjectionRefresh refresh,
                      std::optional<SceneCanonicalizationResult> canonical =
                          std::nullopt);
  [[nodiscard]] bool rebuildProjection(ProjectionRefresh refresh,
                                        std::string *error = nullptr,
                                        bool publish = true,
                                        std::optional<bool> animationLoopOverride =
                                            std::nullopt,
                                        const SceneMutationEvidence* evidence = nullptr);
  [[nodiscard]] RuntimeActionResult publishAnimationProjection(
      bool animationLoopOverride);
  [[nodiscard]] RuntimeActionResult publishAnimationProjectionAt(
      double requestedTime, bool animationLoopOverride);
  [[nodiscard]] bool saveAs(const std::filesystem::path &path,
                            std::string *error = nullptr);
  void rememberRecentFile(const std::filesystem::path &path);
  void resetRuntimeControls() noexcept;
  [[nodiscard]] bool canReuseFrameModelCache() const;
  void refreshFrameModelTransientState(FrameModel &model,
                                       LayoutState layout) const;
  [[nodiscard]] RuntimeActionResult unsupported(std::string message);

  Callbacks m_callbacks;
  // Declared before the session so reverse destruction retires every command
  // user before destroying the sole selection authority bridge.
  std::unique_ptr<SelectionAuthorityBridge> m_selectionAuthority;
  ActiveEditorSession m_activeSession;
  std::optional<SceneProjectionResult> m_projection;
  // Held from the first event of a mutation/continuous edit until its
  // committed publication. Preview frames deliberately keep this value.
  std::optional<SceneProjectionResult> m_beforeMutationProjection;
  SceneEditorUi m_ui;
  PersistentState m_persistent;
  std::uint64_t m_commandSequence{0};
  // Native gesture terminals do not consume SceneEditorUi command sequences.
  std::uint64_t m_gizmoFinishSequence{0};
  std::uint64_t m_projectionEpoch{0};
  std::string m_lastError;
  std::optional<LifecycleOverride> m_lifecycleOverride;
  std::optional<LifecycleOverride> m_savedLifecycleOverride;
  OperationState m_operationState;
  std::uint32_t m_lifecycleOperationDepth{0};
  std::uint64_t m_lifecycleRevision{0};
  std::optional<AnimationId> m_currentAnimation;
  std::optional<std::uint32_t> m_currentVariant;
  bool m_animationPlaying{true};
  double m_animationSpeed{1.0};
  double m_animationTime{0.0};
  bool m_animationLoop{true};
  std::optional<SceneAnimationPose> m_animationPose;
  struct AnimationRuntimeState final {
    std::optional<AnimationId> current;
    bool playing{true};
    double speed{1.0};
    double time{0.0};
    bool loop{true};
    std::optional<SceneAnimationPose> pose;
  };
  struct NodeNameTransactionHint final {
    NodeId node{};
    std::string beforeName;
    std::string afterName;
    detail::NodeNameProjectionDigestCache before;
    detail::NodeNameProjectionDigestCache after;
  };
  std::optional<AnimationRuntimeState> m_beforeMutationAnimation;
  std::optional<NodeNameTransactionHint> m_nodeNameTransaction;
  bool m_nodeNameFastPathUsed{false};
  std::vector<RecentFile> m_recentFiles;
  bool m_viewportHovered{false};
  bool m_viewportFocused{false};
  bool m_gizmoHovered{false};
  bool m_gizmoDragging{false};
  // Building the complete browser/inspector model walks every scene
  // occurrence and allocates thousands of row strings. Keep one immutable
  // structural snapshot for stable frames; render() holds a shared owner so a
  // synchronous UI command may invalidate/replace the cache safely.
  mutable std::shared_ptr<FrameModel> m_frameModelCache;
  mutable std::optional<std::uint32_t> m_frameModelCacheVariant;
  mutable const SceneProjectionResult *m_frameModelCacheProjection{nullptr};
  mutable std::uint64_t m_frameModelCacheMappingEpoch{0};
};

} // namespace demo::editor::ui
