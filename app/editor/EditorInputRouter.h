#pragma once

#include "render/RenderTypes.h"
#include "render/editor/GridModel.h"
#include "render/editor/TransformGizmo.h"

#include <mgif/scene_editor/EditorInputRouter.h>

#include <array>
#include <functional>
#include <optional>

namespace demo::editor::ui {

enum class DesktopGizmoTerminal : std::uint8_t {
  commit = 0,
  detachRetainingPreview,
  latchRetainingPreview,
};

struct DesktopGizmoCallbacks final {
  std::function<bool(NodeId)> beginTransform;
  std::function<bool(NodeId, const GizmoLocalTransform &)> previewTransform;
  std::function<bool(NodeId, DesktopGizmoTerminal)> finishTransform;
  std::function<void()> clearSelection;
};

struct DesktopGizmoFrameInput final {
  std::uint64_t frameSerial{0u};
  std::uint64_t documentRevision{0u};
  std::uint64_t selectionRevision{0u};
  std::uint64_t mappingEpoch{0u};
  EditorViewportMetrics viewport{};
  OverlayMatrix4 viewProjection{};
  GizmoCameraSnapshot camera{};
  GizmoViewDepthSnapshot viewDepth{};
  GridModelSettings gridSettings{};
  GridModelInput gridInput{};
  GizmoDrawSettings gizmoSettings{};
  bool modalActive{false};
  bool imguiPointerCaptured{false};
  bool imguiKeyboardCaptured{false};
  bool imguiTextCaptured{false};
  OverlayVec3 viewDirectionWorld{0.0, 0.0, -1.0};
  double snapRotationDegrees{45.0};
  double snapScale{0.1};
  std::optional<NodeId> selectedNode{};
  std::optional<GizmoAttachment> attachment{};
};

struct DesktopGizmoComponentProbe final {
  std::uint64_t frameSerial{0u};
  std::uint64_t documentRevision{0u};
  std::uint64_t selectionRevision{0u};
  std::uint64_t mappingEpoch{0u};
  NodeId node{};
  GizmoComponent component{GizmoComponent::none};
  // A projected geometry point confirmed by the production hit classifier.
  LogicalViewportPoint point{};
  bool hovered{false};
  bool active{false};
  bool captureHeld{false};
  bool enabled{false};

  auto operator<=>(const DesktopGizmoComponentProbe &) const = default;
};

struct DesktopGizmoInteractionObservation final {
  std::uint64_t frameSerial{0u};
  std::uint64_t documentRevision{0u};
  std::uint64_t selectionRevision{0u};
  std::uint64_t mappingEpoch{0u};
  std::optional<NodeId> node{};
  std::optional<NodeId> gestureNode{};
  GizmoInteractionPhase phase{GizmoInteractionPhase::detached};
  GizmoComponent hoveredComponent{GizmoComponent::none};
  GizmoComponent activeComponent{GizmoComponent::none};
  bool captureHeld{false};
  bool hasPointer{false};
  LogicalViewportPoint pointer{};
  LogicalViewportRect logicalViewport{};
  std::optional<GizmoLocalTransform> localTransform{};
  bool ndcYDown{false};
  std::uint64_t acceptedBeginCount{0u};
  std::uint64_t acceptedPreviewChangeCount{0u};
  std::uint64_t acceptedCommitCount{0u};
  std::uint64_t acceptedDetachCount{0u};
  std::uint64_t acceptedLatchCount{0u};
  std::uint64_t deliveredReleaseCount{0u};
  std::uint64_t escapePressCount{0u};
  std::uint64_t focusLossCount{0u};
  std::uint64_t suppressedReleaseCount{0u};
  std::array<GizmoTerminalEvent, 8> lastTerminalEvents{};
  // Parallel to lastTerminalEvents; zero means no native callback provenance.
  std::array<std::uint64_t, 8> lastTerminalCallbackSequences{};
  std::uint8_t lastTerminalEventCount{0u};
  bool lastTerminalEventsOverflowed{false};
  // The most recent nonempty terminal input batch, including suppression-only
  // batches. This is independent of whether the controller accepts a terminal.
  std::uint64_t lastTerminalFrame{0u};
  // Zero denotes an attachment-fence terminal with no input event sequence.
  std::uint64_t lastTerminalSequence{0u};
  std::optional<DesktopGizmoTerminal> lastTerminal{};

  auto operator<=>(const DesktopGizmoInteractionObservation &) const = default;
};

// Desktop-only integration adapter. It owns only the P7 Gizmo priority domain;
// events it does not consume continue to the existing P6 input/picking router.
class DesktopGizmoIntegrationAdapter final {
public:
  explicit DesktopGizmoIntegrationAdapter(
      DesktopGizmoCallbacks callbacks = {}) noexcept;

  void beginFrame(DesktopGizmoFrameInput input) noexcept;
  [[nodiscard]] bool route(const EditorInputEvent &event,
                           std::uint64_t callbackSequence = 0u) noexcept;
  void closeFrame() noexcept;
  void refreshAttachment(std::uint64_t documentRevision,
                         std::uint64_t selectionRevision,
                         std::uint64_t mappingEpoch,
                         std::optional<NodeId> selectedNode,
                         std::optional<GizmoAttachment> attachment) noexcept;
  void refreshRevisions(std::uint64_t documentRevision,
                        std::uint64_t selectionRevision,
                        std::uint64_t mappingEpoch) noexcept;
  void reset() noexcept;

  [[nodiscard]] bool suppressesCameraAndPicking() const noexcept;
  [[nodiscard]] bool pointerCaptured() const noexcept;
  [[nodiscard]] GizmoInteractionPhase phase() const noexcept;
  [[nodiscard]] GizmoComponent hoveredComponent() const noexcept;
  [[nodiscard]] const std::optional<demo::EditorOverlayFrameSnapshot> &
  snapshot() const noexcept;
  [[nodiscard]] std::optional<DesktopGizmoComponentProbe>
  componentProbe(GizmoComponent component) const noexcept;
  [[nodiscard]] DesktopGizmoInteractionObservation
  interactionObservation() const noexcept;

private:
  [[nodiscard]] bool active() const noexcept;
  [[nodiscard]] bool inputBlockedByUi() const noexcept;
  [[nodiscard]] bool attachmentMatchesFrame() const noexcept;
  void terminateStaleDrag() noexcept;

  [[nodiscard]] std::optional<GizmoRay>
  worldRay(LogicalViewportPoint pointer) const noexcept;
  [[nodiscard]] GizmoHit hit(LogicalViewportPoint pointer) const noexcept;
  void updateHover(LogicalViewportPoint pointer) noexcept;
  void updatePreview(LogicalViewportPoint pointer) noexcept;
  void appendTerminal(GizmoTerminalEventKind kind,
                      std::uint64_t sequence,
                      std::uint64_t callbackSequence) noexcept;
  void rebuildSnapshot() noexcept;
  void applyTerminalResolution(const GizmoActionResolution &resolution) noexcept;
  void recordAcceptedTerminal(DesktopGizmoTerminal terminal,
                              std::uint64_t sequence) noexcept;

  DesktopGizmoCallbacks m_callbacks{};
  DesktopGizmoFrameInput m_frame{};
  GizmoInteractionState m_interaction{};
  GizmoTerminalBatch m_terminalBatch{};
  std::array<std::uint64_t, 8> m_terminalCallbackSequences{};
  bool m_terminalBatchOverflowed{false};
  GizmoComponent m_hovered{GizmoComponent::none};
  std::optional<GizmoRay> m_dragStartRay{};
  GizmoLocalTransform m_dragStartTransform{};
  LogicalViewportPoint m_dragStartPointer{};
  LogicalViewportPoint m_pointer{};
  bool m_havePointer{false};
  bool m_previewAcceptedThisFrame{false};
  std::uint64_t m_nextTransactionId{1u};
  std::optional<demo::EditorOverlayFrameSnapshot> m_snapshot{};
  DesktopGizmoInteractionObservation m_observation{};
};

[[nodiscard]] GizmoStableNodeId
stableGizmoNodeId(NodeId node) noexcept;

} // namespace demo::editor::ui
