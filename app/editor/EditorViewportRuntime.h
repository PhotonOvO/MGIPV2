#pragma once

#include "SceneEditorUiModel.h"
#include "SceneSelection.h"

#ifndef GLM_ENABLE_EXPERIMENTAL
#define GLM_ENABLE_EXPERIMENTAL
#endif

#include <mgif/editor/EditorOverlay.h>
#include <mgif/editor/TransformGizmoMath.h>
#include <mgif/scene_editor/EditorClock.h>
#include <mgif/scene_editor/EditorInputRouter.h>
#include <mgif/scene_editor/EditorPicking.h>
#include <mgif/scene_editor/SceneProjection.h>

#include <glm/glm.hpp>

#include <cstdint>
#include <functional>
#include <optional>
#include <string_view>
#include <utility>
#include <vector>

namespace demo::editor::ui {

struct EditorViewportRuntimeTestAccess;

// Desktop-only, backend-neutral viewport orchestration.  It owns input
// arbitration and CPU interaction state; the host supplies immutable scene
// projections and receives ordinary controller actions.  No RHI/native
// handles cross this boundary.
class EditorViewportRuntime final {
public:
  struct Callbacks final {
    std::function<RuntimeActionResult(const EditorSelectionEffect &)>
        applySelection;
    std::function<bool(const EditorCameraFocusEffect &)> applyCameraFocus;
    std::function<std::optional<std::pair<EditorPickFence, std::uint64_t>>()>
        pickPublication;
    std::function<PickSubmitResult(const EditorPickRequest &)> submitPick;
    std::function<void(std::vector<EditorPickResult> &)> pollPickResults;
    std::function<void()> cancelPicks;
    std::function<bool()> rendererBusy;
    std::function<std::optional<RenderOccurrenceRecord>(
        const EditorPickFence &, std::uint64_t, RenderOccurrenceId)>
        resolveOccurrence;
    std::function<std::optional<RenderOccurrenceRecord>(
        const EditorPickFence &, std::uint64_t, PrimitiveOccurrenceKey)>
        resolveOccurrenceKey;
    std::function<bool(const EditorPickFence &, std::uint64_t, NodeId)>
        nodeHighlightEligible;
    std::function<void(const ISceneSelectionOccurrenceLookup *)>
        bindSelectionOccurrences;
    std::function<SceneSelectionValue()> selectionValue;
    std::function<SelectionRevision()> selectionRevision;
    std::function<std::vector<SelectionChanged>()> drainSelectionChanges;
    std::function<void(const SceneBrowserFocusRequest &)> browserFocus;
    std::function<void(const SceneSelectionValue &, SelectionRevision)>
        setHighlight;
    std::function<RuntimeActionResult(NodeId, EditPhase, const NodeTransform &)>
        transform;
    std::function<std::optional<NodeTransform>(NodeId)> getTransform;
    std::function<RuntimeActionResult(NodeId, bool, bool, bool)> finishTransform;
    std::function<void(std::string_view)> error;
  };

  struct FrameInput final {
    EditorViewportMetrics viewport{};
    glm::mat4 view{1.0f};
    glm::mat4 cameraProjection{1.0f};
    glm::mat4 viewProjection{1.0f};
    glm::vec3 cameraPosition{0.0f};
    bool windowFocused{true};
    bool modalActive{false};
    bool imguiPointerCaptured{false};
    bool imguiKeyboardCaptured{false};
    bool imguiTextCaptured{false};
    bool cameraNavigationEnabled{true};
    bool showGrid{true};
    bool showGizmo{true};
    bool snapEnabled{false};
    double gridUnit{1.0};
    double snapRotationDegrees{45.0};
    double snapScale{0.1};
    const SceneProjectionResult *projection{nullptr};
    SceneSelectionValue selection{};
    SelectionRevision selectionRevision{0};
  };

  explicit EditorViewportRuntime(Callbacks callbacks = {}) noexcept;
  ~EditorViewportRuntime() noexcept;

  EditorViewportRuntime(const EditorViewportRuntime &) = delete;
  EditorViewportRuntime &operator=(const EditorViewportRuntime &) = delete;

  void beginFrame(FrameInput input, EditorInputFrameId frame) noexcept;
  // Refreshes the non-owning scene snapshot after a controller callback may
  // have published a new projection. This keeps subsequent overlay work from
  // dereferencing an invalidated optional projection address.
  void updateSceneSnapshot(const SceneProjectionResult *projection,
                           SceneSelectionValue selection,
                           SelectionRevision selectionRevision) noexcept;
  void updateViewSnapshot(EditorViewportMetrics viewport,
                          const glm::mat4 &view,
                          const glm::mat4 &cameraProjection,
                          const glm::mat4 &viewProjection,
                          const glm::vec3 &cameraPosition) noexcept;
  [[nodiscard]] EditorInputDecision route(const EditorInputEvent &event) noexcept;
  [[nodiscard]] EditorInputDecision tick() noexcept;
  void endFrame() noexcept;

  [[nodiscard]] bool ready() const noexcept;
  [[nodiscard]] bool cameraNavigationSuppressed() const noexcept;
  [[nodiscard]] bool pointerCaptured() const noexcept;
  [[nodiscard]] bool gizmoDragging() const noexcept;
  [[nodiscard]] TransformGizmoComponent hoveredGizmo() const noexcept;
  [[nodiscard]] const std::optional<EditorOverlayFrame> &overlay() const noexcept;
  [[nodiscard]] std::uint64_t viewportEpoch() const noexcept;
  [[nodiscard]] std::uint64_t frameValue() const noexcept;
  [[nodiscard]] std::uint64_t nextInputSequence() const noexcept;
  [[nodiscard]] EditorClickClassifierPhase clickPhase() const noexcept;
  [[nodiscard]] static PhysicalViewportExtent physicalExtentForLogical(
      LogicalViewportExtent logical,
      EditorFramebufferScale framebufferScale) noexcept;

  // Draws the backend-neutral overlay packet using the host's existing ImGui
  // draw list.  This is intentionally the only ImGui-facing method.
  void drawImGuiOverlay(void *drawList) const noexcept;

private:
  struct CoherentPickPublication final {
    EditorPickFence fence;
    std::uint64_t occurrenceGeneration{0};
    std::uint64_t rawSceneActivationEpoch{0};

    bool operator==(const CoherentPickPublication &) const = default;
  };

  class OccurrenceLookup final : public IEditorPickingOccurrenceLookup,
                                 public ISceneSelectionOccurrenceLookup {
  public:
    explicit OccurrenceLookup(const EditorViewportRuntime &owner) noexcept
        : m_owner(&owner) {}
    [[nodiscard]] std::optional<RenderOccurrenceRecord>
    resolve(RenderOccurrenceId occurrence) const noexcept override;
    [[nodiscard]] EditorDocumentAuthority authority() const noexcept override;
    [[nodiscard]] std::uint64_t documentRevision() const noexcept override;
    [[nodiscard]] std::uint64_t sceneActivationEpoch() const noexcept override;
    [[nodiscard]] std::uint64_t generation() const noexcept override;
    [[nodiscard]] bool contains(PrimitiveOccurrenceKey occurrence) const
        noexcept override;
    [[nodiscard]] bool visible(PrimitiveOccurrenceKey occurrence) const
        noexcept override;
    [[nodiscard]] bool nodeHighlightEligible(NodeId node) const noexcept override;

  private:
    const EditorViewportRuntime *m_owner{nullptr};
  };

  struct GizmoDrag final {
    NodeId node{};
    NodeTransform initial{};
    TransformGizmoFrame frame{};
    TransformGizmoComponent component{TransformGizmoComponent::None};
    TransformGizmoRay startRay{};
    glm::vec2 startMouse{0.0f};
    bool active{false};
  };

  [[nodiscard]] EditorInputRoutingContext routingContext() const noexcept;
  [[nodiscard]] std::optional<CoherentPickPublication>
  coherentPickPublication() const noexcept;
  [[nodiscard]] std::optional<TransformGizmoRay>
  rayAt(LogicalViewportPoint position) const noexcept;
  [[nodiscard]] TransformGizmoHit gizmoHit(LogicalViewportPoint position) const noexcept;
  [[nodiscard]] std::optional<EditorPickRequest>
  makePickRequest(const EditorPickEmission &click) noexcept;
  [[nodiscard]] std::optional<NodeId> selectedNode() const noexcept;
  [[nodiscard]] std::optional<std::uint32_t>
  projectedNodeIndex(NodeId node) const noexcept;
  [[nodiscard]] glm::mat4 nodeWorld(std::uint32_t nodeIndex) const noexcept;
  [[nodiscard]] std::optional<TransformGizmoFrame> gizmoFrame(NodeId node) const noexcept;
  [[nodiscard]] NodeTransform applyGizmoDelta(const GizmoDrag &drag,
                                               LogicalViewportPoint position) const noexcept;
  void consumeDecision(const EditorInputEvent &event,
                       const EditorInputDecision &decision) noexcept;
  void beginGizmo() noexcept;
  void updateGizmo(LogicalViewportPoint position) noexcept;
  void finishGizmo(bool physicalRelease, bool escape, bool focusLost) noexcept;
  void enqueuePick(const EditorPickEmission &click) noexcept;
  void resolveReadyPicks() noexcept;
  void refreshSelection() noexcept;
  void drainSelectionAtFrameEnd() noexcept;
  void rebuildOverlay() noexcept;
  void report(std::string_view message) const noexcept;

  SystemEditorClock m_clock;
  EditorInputRouter m_router;
  EditorPickingController m_picking;
  OccurrenceLookup m_occurrences{*this};
  Callbacks m_callbacks;
  FrameInput m_input{};
  std::optional<GizmoDrag> m_drag;
  std::optional<EditorOverlayFrame> m_overlay;
  TransformGizmoComponent m_hovered{TransformGizmoComponent::None};
  EditorInputEventSequence m_nextSequence{1u};
  std::uint64_t m_nextPickId{1u};
  EditorInputFrameId m_frame{};
  std::uint64_t m_lastViewportEpoch{0u};
  LogicalViewportPoint m_lastPointer{};
  bool m_havePointer{false};

  friend struct EditorViewportRuntimeTestAccess;
};

} // namespace demo::editor::ui
