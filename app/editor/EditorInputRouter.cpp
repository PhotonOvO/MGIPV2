#include "EditorInputRouter.h"

#include "render/editor/GizmoGeometry.h"

#include <algorithm>
#include <cmath>
#include <limits>
#include <utility>

namespace demo::editor::ui {
namespace {

using namespace demo::editor;

void incrementObservedCount(std::uint64_t &value) noexcept {
  if (value != (std::numeric_limits<std::uint64_t>::max)()) ++value;
}

[[nodiscard]] std::optional<LogicalViewportPoint> projectProbePoint(
    OverlayVec3 local, const GizmoComponentDrawPacket &draw,
    const demo::EditorOverlayFrameSnapshot &snapshot, bool ndcYDown) noexcept {
  const auto world = transformOverlayPoint(draw.model, local);
  if (!world) return std::nullopt;
  const auto &matrix = snapshot.viewProjection.elements;
  const double clipW = matrix[12] * world->x + matrix[13] * world->y +
                       matrix[14] * world->z + matrix[15];
  if (!std::isfinite(clipW) || clipW <= 1.0e-12) return std::nullopt;
  const auto ndc = transformOverlayPoint(snapshot.viewProjection, *world);
  const double minimumDepth =
      snapshot.viewport.convention.depthRange ==
              EditorClipDepthRange::negativeOneToOne
          ? -1.0
          : 0.0;
  if (!ndc || ndc->x < -1.0 || ndc->x > 1.0 || ndc->y < -1.0 ||
      ndc->y > 1.0 || ndc->z < minimumDepth || ndc->z > 1.0)
    return std::nullopt;
  const double projectedU = (ndc->x + 1.0) * 0.5;
  const double projectedV =
      ndcYDown ? (ndc->y + 1.0) * 0.5 : (1.0 - ndc->y) * 0.5;
  double u = projectedU;
  double v = projectedV;
  // Inverse of makeGizmoRay's logical-to-surface transform.
  switch (snapshot.viewport.surfaceTransform) {
  case EditorViewportSurfaceTransform::identity:
    break;
  case EditorViewportSurfaceTransform::rotate90:
    u = projectedV;
    v = 1.0 - projectedU;
    break;
  case EditorViewportSurfaceTransform::rotate180:
    u = 1.0 - projectedU;
    v = 1.0 - projectedV;
    break;
  case EditorViewportSurfaceTransform::rotate270:
    u = 1.0 - projectedV;
    v = projectedU;
    break;
  case EditorViewportSurfaceTransform::mirrorHorizontal:
    u = 1.0 - projectedU;
    break;
  case EditorViewportSurfaceTransform::mirrorVertical:
    v = 1.0 - projectedV;
    break;
  case EditorViewportSurfaceTransform::mirrorHorizontalRotate90:
    u = 1.0 - projectedV;
    v = 1.0 - projectedU;
    break;
  case EditorViewportSurfaceTransform::mirrorHorizontalRotate270:
    u = projectedV;
    v = projectedU;
    break;
  default:
    return std::nullopt;
  }
  const auto &logical = snapshot.viewport.logical;
  LogicalViewportPoint point{logical.origin.x + u * logical.extent.width,
                             logical.origin.y + v * logical.extent.height};
  if (!std::isfinite(point.x) || !std::isfinite(point.y) || u < 0.0 || u >= 1.0 ||
      v < 0.0 || v >= 1.0)
    return std::nullopt;
  const auto &origin = snapshot.viewport.framebufferLogicalOrigin;
  const auto &scale = snapshot.viewport.framebufferScale;
  point.x = origin.x + std::round((point.x - origin.x) * scale.x) / scale.x;
  point.y = origin.y + std::round((point.y - origin.y) * scale.y) / scale.y;
  if (!snapshot.viewport.containsLogical(point)) return std::nullopt;
  return point;
}

[[nodiscard]] GizmoLocalTransform previewFromSolution(
    const GizmoAttachment &attachment, GizmoComponent component,
    const GizmoRay &startRay, const GizmoRay &currentRay,
    OverlayVec3 viewDirection, GizmoBasisToken basis,
    bool snapEnabled, double snapTranslation, double snapRotation,
    double snapScale,
    LogicalViewportPoint startPointer,
    LogicalViewportPoint currentPointer) noexcept {
  GizmoLocalTransform value = attachment.local;
  std::optional<GizmoTranslationSolution> translation;
  std::optional<GizmoRotationSolution> rotation;
  std::optional<GizmoScaleSolution> scale;
  if (component == GizmoComponent::translateX ||
      component == GizmoComponent::translateY ||
      component == GizmoComponent::translateZ ||
      component == GizmoComponent::translateXY ||
      component == GizmoComponent::translateXZ ||
      component == GizmoComponent::translateYZ) {
    translation = solveGizmoTranslation(component, startRay, currentRay,
                                        viewDirection, attachment, basis,
                                        snapEnabled && snapTranslation > 0.0
                                            ? std::optional<double>(snapTranslation)
                                            : std::nullopt);
    if (translation) {
      value.translation = translation->resultingLocalPosition;
    }
  } else if (component == GizmoComponent::rotateX ||
             component == GizmoComponent::rotateY ||
             component == GizmoComponent::rotateZ) {
    rotation = solveGizmoRotation(component, startRay, currentRay, attachment,
                                   basis, snapRotation > 0.0
                                             && snapEnabled
                                             ? std::optional<double>(snapRotation)
                                             : std::nullopt);
    if (rotation) {
      value.rotation = rotation->resultingLocalRotation;
    }
  } else if (component == GizmoComponent::scaleX ||
             component == GizmoComponent::scaleY ||
             component == GizmoComponent::scaleZ ||
             component == GizmoComponent::uniformScale) {
    scale = solveGizmoScale(component, attachment.local.scale,
                             {startPointer.x, startPointer.y},
                             {currentPointer.x, currentPointer.y},
                             snapEnabled && snapScale > 0.0
                                 ? std::optional<double>(snapScale)
                                 : std::nullopt);
    if (scale) {
      value.scale = scale->resultingLocalScale;
    }
  }
  return value;
}

} // namespace

DesktopGizmoIntegrationAdapter::DesktopGizmoIntegrationAdapter(
    DesktopGizmoCallbacks callbacks) noexcept
    : m_callbacks(std::move(callbacks)) {}

bool DesktopGizmoIntegrationAdapter::active() const noexcept {
  return m_interaction.phase == GizmoInteractionPhase::dragging ||
         m_interaction.phase == GizmoInteractionPhase::draggingLatched;
}

bool DesktopGizmoIntegrationAdapter::inputBlockedByUi() const noexcept {
  return m_frame.modalActive || m_frame.imguiPointerCaptured ||
         m_frame.imguiKeyboardCaptured || m_frame.imguiTextCaptured;
}

bool DesktopGizmoIntegrationAdapter::attachmentMatchesFrame() const noexcept {
  if (!m_frame.attachment || !m_frame.selectedNode ||
      !validGizmoAttachment(*m_frame.attachment) ||
      m_frame.mappingEpoch == 0u ||
      m_frame.attachment->nodeId != stableGizmoNodeId(*m_frame.selectedNode) ||
      m_frame.attachment->documentRevision != m_frame.documentRevision ||
      m_frame.attachment->selectionRevision != m_frame.selectionRevision) {
    return false;
  }
  return true;
}

void DesktopGizmoIntegrationAdapter::terminateStaleDrag() noexcept {
  if (!active()) return;
  const std::optional<NodeId> node = m_frame.selectedNode;
  bool finishAccepted = !node || !m_callbacks.finishTransform;
  try {
    if (node && m_callbacks.finishTransform) {
      finishAccepted = m_callbacks.finishTransform(
          *node, DesktopGizmoTerminal::detachRetainingPreview);
      if (finishAccepted) {
        recordAcceptedTerminal(DesktopGizmoTerminal::detachRetainingPreview, 0u);
      }
    }
  } catch (...) {
    // A stale-fence cleanup callback is best effort.  The adapter still drops
    // its input ownership below, which is the fail-closed state when the
    // authoritative controller cannot detach the transaction.
    finishAccepted = false;
  }
  (void)finishAccepted;
  m_interaction = {};
  m_terminalBatch = {};
  m_terminalCallbackSequences = {};
  m_terminalBatchOverflowed = false;
  m_dragStartRay.reset();
  m_dragStartTransform = {};
  m_hovered = GizmoComponent::none;
  m_previewAcceptedThisFrame = false;
  m_frame.gizmoSettings.hovered = GizmoComponent::none;
  m_frame.gizmoSettings.active = GizmoComponent::none;
}

void DesktopGizmoIntegrationAdapter::beginFrame(
    DesktopGizmoFrameInput input) noexcept {
  try {
    if (input.frameSerial == 0u ||
        (m_frame.frameSerial != 0u &&
         input.frameSerial <= m_frame.frameSerial)) {
      return;
    }
    const bool wasActive = active();
    const GizmoAttachmentFence oldFence = m_interaction.fence;
    if (wasActive) {
      const bool sameNode = input.attachment && input.selectedNode &&
                            input.attachment->nodeId == oldFence.nodeId;
      const bool sameSelection =
          input.selectionRevision == oldFence.selectionRevision;
      const bool sameMapping = input.mappingEpoch == m_frame.mappingEpoch;
      const bool acceptedProjectionAdvance =
          m_previewAcceptedThisFrame && sameNode && sameSelection &&
          input.documentRevision > m_frame.documentRevision &&
          input.mappingEpoch > m_frame.mappingEpoch;
      if (!sameNode || !sameSelection ||
          input.documentRevision < oldFence.documentRevision ||
          (!sameMapping && !acceptedProjectionAdvance) ||
          !input.attachment || !validGizmoAttachment(*input.attachment)) {
        terminateStaleDrag();
      } else if (input.documentRevision != oldFence.documentRevision) {
        // Continuous preview publishes a new document revision while keeping
        // the same mapping and selection fence. Advance only that revision;
        // replacement/selection changes above still detach fail-closed.
        m_interaction.fence.documentRevision = input.documentRevision;
      }
    }
    m_frame = std::move(input);
    m_terminalBatch = {};
    m_terminalCallbackSequences = {};
    m_terminalBatchOverflowed = false;
    m_havePointer = false;
    m_previewAcceptedThisFrame = false;
    m_snapshot.reset();
    if (!attachmentMatchesFrame()) {
      if (!active()) {
        m_interaction = {};
        m_hovered = GizmoComponent::none;
      }
    } else if (m_interaction.phase == GizmoInteractionPhase::detached) {
      m_interaction = attachGizmo(*m_frame.attachment);
    }
  } catch (...) {
    reset();
  }
}

bool DesktopGizmoIntegrationAdapter::route(
    const EditorInputEvent &event, std::uint64_t callbackSequence) noexcept {
  try {
    if (!event.frame.valid() || event.frame.value != m_frame.frameSerial ||
        !event.sequence.valid()) {
      return false;
    }
    if (const auto *move = std::get_if<EditorPointerMoveEvent>(&event.payload)) {
      m_pointer = move->position;
      m_havePointer = std::isfinite(m_pointer.x) && std::isfinite(m_pointer.y);
      if (!m_havePointer) return suppressesCameraAndPicking();
      if (active()) {
        updatePreview(m_pointer);
        return true;
      }
      if (inputBlockedByUi()) {
        m_hovered = GizmoComponent::none;
        m_frame.gizmoSettings.hovered = GizmoComponent::none;
        return false;
      }
      updateHover(m_pointer);
      return m_hovered != GizmoComponent::none;
    }
    if (const auto *button =
            std::get_if<EditorPointerButtonEvent>(&event.payload)) {
      if (button->button != EditorPointerButton::primary) {
        return suppressesCameraAndPicking();
      }
      m_pointer = button->position;
      m_havePointer = std::isfinite(m_pointer.x) && std::isfinite(m_pointer.y);
      if (active()) {
        if (button->action == EditorInputAction::release) {
          appendTerminal(button->delivered
                             ? GizmoTerminalEventKind::deliveredRelease
                             : GizmoTerminalEventKind::suppressedPhysicalMouseUp,
                         event.sequence.value, callbackSequence);
        }
        return true;
      }
      if (button->action == EditorInputAction::press && button->delivered &&
          m_havePointer && m_frame.attachment && m_frame.selectedNode &&
          !m_frame.modalActive && !m_frame.imguiPointerCaptured &&
          !m_frame.imguiKeyboardCaptured && !m_frame.imguiTextCaptured) {
        updateHover(m_pointer);
        if (m_hovered != GizmoComponent::none && m_callbacks.beginTransform) {
          const auto ray = worldRay(m_pointer);
          GizmoInteractionState candidate = attachGizmo(*m_frame.attachment);
          const std::uint64_t transactionId = m_nextTransactionId == 0u
                                                  ? 1u
                                                  : m_nextTransactionId;
          if (m_nextTransactionId != (std::numeric_limits<std::uint64_t>::max)()) {
            ++m_nextTransactionId;
          } else {
            m_nextTransactionId = 1u;
          }
          const auto resolution =
              ray ? beginGizmoDrag(candidate, *m_frame.attachment,
                                   transactionId, m_hovered)
                  : GizmoActionResolution{};
          if (ray && resolution.actionCount != 0u &&
              (!m_callbacks.beginTransform ||
               m_callbacks.beginTransform(*m_frame.selectedNode))) {
            m_dragStartRay = ray;
            m_dragStartPointer = m_pointer;
            m_dragStartTransform = m_frame.attachment->local;
            m_interaction = resolution.state;
            incrementObservedCount(m_observation.acceptedBeginCount);
            m_observation.gestureNode = m_frame.selectedNode;
            return true;
          }
        }
      }
      return false;
    }
    if (const auto *key = std::get_if<EditorKeyEvent>(&event.payload)) {
      if (key->key == EditorInputKey::escape &&
          key->action == EditorInputAction::press &&
          active()) {
        appendTerminal(GizmoTerminalEventKind::escapePressed,
                       event.sequence.value, callbackSequence);
        return true;
      }
      return suppressesCameraAndPicking();
    }
    if (const auto *focus = std::get_if<EditorFocusEvent>(&event.payload)) {
      if (focus->action == EditorFocusAction::lost &&
          active()) {
        appendTerminal(GizmoTerminalEventKind::focusLost, event.sequence.value,
                       callbackSequence);
        return true;
      }
      return false;
    }
    return suppressesCameraAndPicking();
  } catch (...) {
    return suppressesCameraAndPicking();
  }
}

void DesktopGizmoIntegrationAdapter::closeFrame() noexcept {
  try {
    if (m_terminalBatch.count != 0u &&
        (m_interaction.phase == GizmoInteractionPhase::dragging ||
         m_interaction.phase == GizmoInteractionPhase::draggingLatched)) {
      m_observation.lastTerminalEvents = m_terminalBatch.events;
      m_observation.lastTerminalCallbackSequences = m_terminalCallbackSequences;
      m_observation.lastTerminalEventCount = m_terminalBatch.count;
      m_observation.lastTerminalEventsOverflowed = m_terminalBatchOverflowed;
      m_observation.lastTerminalFrame = m_frame.frameSerial;
      m_observation.lastTerminalSequence =
          m_terminalBatch.events[m_terminalBatch.count - 1u].sequence;
      const auto resolution =
          resolveGizmoTerminalBatch(m_interaction, m_terminalBatch);
      applyTerminalResolution(resolution);
    }
    m_terminalBatch = {};
    m_terminalCallbackSequences = {};
    m_terminalBatchOverflowed = false;
    rebuildSnapshot();
  } catch (...) {
    m_snapshot.reset();
  }
}

void DesktopGizmoIntegrationAdapter::refreshAttachment(
    std::uint64_t documentRevision, std::uint64_t selectionRevision,
    std::uint64_t mappingEpoch, std::optional<NodeId> selectedNode,
    std::optional<GizmoAttachment> attachment) noexcept {
  if (active()) {
    const bool sameNode = attachment && selectedNode &&
                          attachment->nodeId == m_interaction.fence.nodeId;
    const bool sameSelection =
        selectionRevision == m_interaction.fence.selectionRevision;
    const bool sameMapping = mappingEpoch == m_frame.mappingEpoch;
    const bool acceptedProjectionAdvance =
        m_previewAcceptedThisFrame && sameNode && sameSelection &&
        documentRevision > m_frame.documentRevision &&
        mappingEpoch > m_frame.mappingEpoch;
    if (!sameNode || !sameSelection ||
        documentRevision < m_interaction.fence.documentRevision ||
        (!sameMapping && !acceptedProjectionAdvance)) {
      terminateStaleDrag();
    } else {
      m_interaction.fence.documentRevision = documentRevision;
    }
  }
  m_frame.documentRevision = documentRevision;
  m_frame.selectionRevision = selectionRevision;
  m_frame.mappingEpoch = mappingEpoch;
  m_frame.selectedNode = std::move(selectedNode);
  m_frame.attachment = std::move(attachment);
}

void DesktopGizmoIntegrationAdapter::reset() noexcept {
  m_frame = {};
  m_interaction = {};
  m_terminalBatch = {};
  m_terminalCallbackSequences = {};
  m_terminalBatchOverflowed = false;
  m_hovered = GizmoComponent::none;
  m_dragStartRay.reset();
  m_dragStartTransform = {};
  m_havePointer = false;
  m_previewAcceptedThisFrame = false;
  m_snapshot.reset();
  m_observation = {};
}

bool DesktopGizmoIntegrationAdapter::suppressesCameraAndPicking() const noexcept {
  return m_interaction.phase == GizmoInteractionPhase::dragging ||
         m_interaction.phase == GizmoInteractionPhase::draggingLatched;
}

bool DesktopGizmoIntegrationAdapter::pointerCaptured() const noexcept {
  return m_interaction.captureHeld;
}

GizmoInteractionPhase DesktopGizmoIntegrationAdapter::phase() const noexcept {
  return m_interaction.phase;
}

GizmoComponent DesktopGizmoIntegrationAdapter::hoveredComponent() const noexcept {
  return m_hovered;
}

const std::optional<demo::EditorOverlayFrameSnapshot> &
DesktopGizmoIntegrationAdapter::snapshot() const noexcept {
  return m_snapshot;
}

std::optional<DesktopGizmoComponentProbe>
DesktopGizmoIntegrationAdapter::componentProbe(
    GizmoComponent component) const noexcept {
  if (component == GizmoComponent::none || !m_snapshot ||
      !m_snapshot->gizmoVisible || !attachmentMatchesFrame() ||
      m_snapshot->frameSerial != m_frame.frameSerial ||
      m_snapshot->documentRevision != m_frame.documentRevision ||
      m_snapshot->selectionRevision != m_frame.selectionRevision ||
      m_snapshot->mappingEpoch != m_frame.mappingEpoch ||
      m_snapshot->gizmo.generation != m_frame.camera.generation)
    return std::nullopt;
  const auto draw = std::find_if(
      m_snapshot->gizmo.components.begin(), m_snapshot->gizmo.components.end(),
      [component](const auto &candidate) {
        return candidate.visible && candidate.range.component == component;
      });
  if (draw == m_snapshot->gizmo.components.end()) return std::nullopt;
  const auto &geometry = canonicalGizmoGeometry();
  const auto &indices = geometry.indices();
  const auto &vertices = geometry.vertices();
  const std::size_t first = draw->range.firstIndex;
  const std::size_t count = draw->range.indexCount;
  if (first > indices.size() || count > indices.size() - first)
    return std::nullopt;
  // Triangle centroids lie on rendered component geometry, unlike an AABB
  // center which may be in empty space (especially for rotation rings).
  for (std::size_t index = first; index + 2u < first + count; index += 3u) {
    const auto ia = indices[index];
    const auto ib = indices[index + 1u];
    const auto ic = indices[index + 2u];
    if (ia >= vertices.size() || ib >= vertices.size() || ic >= vertices.size())
      return std::nullopt;
    const auto &a = vertices[ia].position;
    const auto &b = vertices[ib].position;
    const auto &c = vertices[ic].position;
    const OverlayVec3 center{(a.x + b.x + c.x) / 3.0,
                             (a.y + b.y + c.y) / 3.0,
                             (a.z + b.z + c.z) / 3.0};
    const auto projected =
        projectProbePoint(center, *draw, *m_snapshot, m_frame.camera.ndcYDown);
    if (!projected || hit(*projected).component != component) continue;
    return DesktopGizmoComponentProbe{
        .frameSerial = m_snapshot->frameSerial,
        .documentRevision = m_snapshot->documentRevision,
        .selectionRevision = m_snapshot->selectionRevision,
        .mappingEpoch = m_snapshot->mappingEpoch,
        .node = *m_frame.selectedNode,
        .component = component,
        .point = *projected,
        .hovered = m_hovered == component,
        .active = m_interaction.activeComponent == component && active(),
        .captureHeld = m_interaction.captureHeld,
        .enabled = !m_frame.modalActive,
    };
  }
  return std::nullopt;
}

DesktopGizmoInteractionObservation
DesktopGizmoIntegrationAdapter::interactionObservation() const noexcept {
  DesktopGizmoInteractionObservation observation = m_observation;
  observation.frameSerial = m_frame.frameSerial;
  observation.documentRevision = m_frame.documentRevision;
  observation.selectionRevision = m_frame.selectionRevision;
  observation.mappingEpoch = m_frame.mappingEpoch;
  observation.node = m_frame.selectedNode;
  observation.phase = m_interaction.phase;
  observation.hoveredComponent = m_hovered;
  observation.activeComponent = m_interaction.activeComponent;
  observation.captureHeld = m_interaction.captureHeld;
  observation.hasPointer = m_havePointer;
  observation.pointer = m_havePointer ? m_pointer : LogicalViewportPoint{};
  observation.logicalViewport = m_frame.viewport.logical;
  observation.localTransform = m_frame.attachment
                                   ? std::optional(m_frame.attachment->local)
                                   : std::nullopt;
  observation.ndcYDown = m_frame.camera.ndcYDown;
  return observation;
}

std::optional<GizmoRay> DesktopGizmoIntegrationAdapter::worldRay(
    LogicalViewportPoint pointer) const noexcept {
  return makeGizmoRay(pointer, m_frame.viewport, m_frame.camera);
}

GizmoHit DesktopGizmoIntegrationAdapter::hit(
    LogicalViewportPoint pointer) const noexcept {
  if (!m_frame.attachment || !m_frame.selectedNode ||
      !validGizmoAttachment(*m_frame.attachment)) {
    return {};
  }
  const auto packet = buildGizmoDrawPacket(
      canonicalGizmoGeometry(), *m_frame.attachment, m_frame.viewport,
      m_frame.viewDepth, m_frame.gizmoSettings);
  const auto ray = worldRay(pointer);
  if (!packet || !ray) return {};
  const auto model = std::find_if(
      packet->components.begin(), packet->components.end(),
      [](const GizmoComponentDrawPacket &draw) { return draw.visible; });
  if (model == packet->components.end()) return {};
  const auto inverse = invertOverlayMatrix(model->model);
  if (!inverse) return {};
  const auto localOrigin = transformOverlayPoint(*inverse, ray->origin);
  const auto localDirection = transformOverlayDirection(*inverse, ray->direction);
  if (!localOrigin || !localDirection) return {};
  return classifyGizmoHit({*localOrigin, *localDirection},
                          canonicalGizmoGeometry(), {},
                          m_frame.gizmoSettings.visibility);
}

void DesktopGizmoIntegrationAdapter::updateHover(
    LogicalViewportPoint pointer) noexcept {
  const GizmoHit candidate = hit(pointer);
  m_hovered = candidate.hit() ? candidate.component : GizmoComponent::none;
  m_frame.gizmoSettings.hovered = m_hovered;
}

void DesktopGizmoIntegrationAdapter::updatePreview(
    LogicalViewportPoint pointer) noexcept {
  if (!m_dragStartRay || !m_frame.attachment || !m_frame.selectedNode) return;
  const auto currentRay = worldRay(pointer);
  if (!currentRay) return;
  // All drag solutions are measured from the immutable begin snapshot. Using
  // the last published attachment here would accumulate the same mouse delta
  // once per frame and diverge from the P4 continuous-edit baseline.
  GizmoAttachment solveAttachment = *m_frame.attachment;
  solveAttachment.local = m_dragStartTransform;
  const GizmoLocalTransform preview = previewFromSolution(
      solveAttachment, m_interaction.activeComponent, *m_dragStartRay,
      *currentRay, m_frame.viewDirectionWorld, m_frame.gizmoSettings.basis,
      m_frame.gizmoSettings.snapEnabled, m_frame.gridSettings.baseUnit,
      m_frame.snapRotationDegrees, m_frame.snapScale, m_dragStartPointer,
      pointer);
  const auto resolution =
      updateGizmoPreview(m_interaction, solveAttachment, preview);
  if (resolution.actionCount == 0u) return;
  bool terminal = false;
  for (std::size_t index = 0u; index < resolution.actionCount; ++index) {
    const GizmoActionKind kind = resolution.actions[index].kind;
    terminal = terminal || kind == GizmoActionKind::detachRetainingPreview ||
               kind == GizmoActionKind::releaseCapture;
  }
  if (terminal) {
    applyTerminalResolution(resolution);
    return;
  }
  if (m_callbacks.previewTransform) {
    if (!m_callbacks.previewTransform(*m_frame.selectedNode, preview)) {
      // A rejected preview must not advance the CPU interaction baseline or
      // the draw attachment; the controller remains authoritative.
      return;
    }
    if (!m_interaction.hasRetainedPreview ||
        m_interaction.retainedPreview != resolution.state.retainedPreview) {
      incrementObservedCount(m_observation.acceptedPreviewChangeCount);
    }
  }
  m_interaction = resolution.state;
  if (m_frame.attachment) {
    m_frame.attachment->local = preview;
  }
  m_previewAcceptedThisFrame = true;
}

void DesktopGizmoIntegrationAdapter::refreshRevisions(
    std::uint64_t documentRevision, std::uint64_t selectionRevision,
    std::uint64_t mappingEpoch) noexcept {
  const bool wasActive = active();
  const bool selectionChanged =
      wasActive && selectionRevision != m_interaction.fence.selectionRevision;
  const bool mappingChanged =
      wasActive && mappingEpoch != m_frame.mappingEpoch;
  const bool acceptedProjectionAdvance =
      wasActive && m_previewAcceptedThisFrame && !selectionChanged &&
      documentRevision > m_frame.documentRevision &&
      mappingEpoch > m_frame.mappingEpoch;
  const bool documentRevisionRegressed =
      wasActive && documentRevision < m_interaction.fence.documentRevision;
  if (selectionChanged || documentRevisionRegressed ||
      (mappingChanged && !acceptedProjectionAdvance) ||
      mappingEpoch == 0u) {
    if (wasActive) terminateStaleDrag();
  }
  m_frame.documentRevision = documentRevision;
  m_frame.selectionRevision = selectionRevision;
  m_frame.mappingEpoch = mappingEpoch;
  if (m_frame.attachment) {
    m_frame.attachment->documentRevision = documentRevision;
    m_frame.attachment->selectionRevision = selectionRevision;
  }
  if (wasActive && !selectionChanged &&
      (!mappingChanged || acceptedProjectionAdvance)) {
    // Continuous preview may advance only the document revision. Keep the
    // stable node/selection/mapping fence while accepting that revision.
    m_interaction.fence.documentRevision = documentRevision;
  }
  m_previewAcceptedThisFrame = false;
}

void DesktopGizmoIntegrationAdapter::appendTerminal(
    GizmoTerminalEventKind kind, std::uint64_t sequence,
    std::uint64_t callbackSequence) noexcept {
  switch (kind) {
  case GizmoTerminalEventKind::deliveredRelease:
    incrementObservedCount(m_observation.deliveredReleaseCount);
    break;
  case GizmoTerminalEventKind::escapePressed:
    incrementObservedCount(m_observation.escapePressCount);
    break;
  case GizmoTerminalEventKind::focusLost:
    incrementObservedCount(m_observation.focusLossCount);
    break;
  case GizmoTerminalEventKind::suppressedPhysicalMouseUp:
    incrementObservedCount(m_observation.suppressedReleaseCount);
    break;
  }
  if (m_terminalBatch.count >= m_terminalBatch.events.size()) {
    m_terminalBatchOverflowed = true;
    return;
  }
  m_terminalCallbackSequences[m_terminalBatch.count] = callbackSequence;
  m_terminalBatch.events[m_terminalBatch.count++] = {kind, sequence};
}

void DesktopGizmoIntegrationAdapter::applyTerminalResolution(
    const GizmoActionResolution &resolution) noexcept {
  const auto terminalSequence = [&](GizmoTerminalEventKind kind) {
    std::uint64_t sequence = 0u;
    for (std::size_t index = 0u; index < m_terminalBatch.count; ++index) {
      if (m_terminalBatch.events[index].kind == kind)
        sequence = std::max(sequence, m_terminalBatch.events[index].sequence);
    }
    return sequence;
  };
  bool terminalAccepted = true;
  if (m_frame.selectedNode) {
    for (std::size_t index = 0u; index < resolution.actionCount; ++index) {
      const GizmoAction &action = resolution.actions[index];
      if (action.kind == GizmoActionKind::commitPreviewOnce &&
          m_callbacks.finishTransform) {
        bool accepted = false;
        try {
          accepted = m_callbacks.finishTransform(*m_frame.selectedNode,
                                                 DesktopGizmoTerminal::commit);
        } catch (...) {
          accepted = false;
        }
        if (accepted) {
          recordAcceptedTerminal(
              DesktopGizmoTerminal::commit,
              terminalSequence(GizmoTerminalEventKind::deliveredRelease));
        }
        else {
          terminalAccepted = false;
          break;
        }
      } else if (action.kind == GizmoActionKind::commitPreviewOnce) {
        terminalAccepted = false;
        break;
      } else if (action.kind == GizmoActionKind::detachRetainingPreview) {
        bool accepted = false;
        try {
          accepted = m_callbacks.finishTransform &&
                     m_callbacks.finishTransform(
                         *m_frame.selectedNode,
                         DesktopGizmoTerminal::detachRetainingPreview);
        } catch (...) {
          accepted = false;
        }
        if (accepted) {
            recordAcceptedTerminal(
                DesktopGizmoTerminal::detachRetainingPreview,
                terminalSequence(GizmoTerminalEventKind::escapePressed));
          if (m_callbacks.clearSelection) {
            try {
              m_callbacks.clearSelection();
            } catch (...) {
              // The controller has accepted the detach; keep the detached
              // adapter state even if the optional UI cleanup throws.
            }
          }
        } else {
          terminalAccepted = false;
          break;
        }
      } else if (action.kind == GizmoActionKind::latchRetainingPreview) {
        bool accepted = false;
        try {
          accepted = m_callbacks.finishTransform &&
                     m_callbacks.finishTransform(
                         *m_frame.selectedNode,
                         DesktopGizmoTerminal::latchRetainingPreview);
        } catch (...) {
          accepted = false;
        }
        if (accepted) {
          recordAcceptedTerminal(
              DesktopGizmoTerminal::latchRetainingPreview,
              terminalSequence(GizmoTerminalEventKind::focusLost));
        } else {
          terminalAccepted = false;
          break;
        }
      }
    }
  } else if (resolution.actionCount != 0u) {
    terminalAccepted = false;
  }
  // Do not let a rejected authoritative terminal silently release the local
  // transaction.  Keeping the current state retains capture/ownership so a
  // later Escape or release can retry the controller operation.
  if (!terminalAccepted) return;
  m_interaction = resolution.state;
  if (m_interaction.phase != GizmoInteractionPhase::dragging &&
      m_interaction.phase != GizmoInteractionPhase::draggingLatched) {
    m_dragStartRay.reset();
    m_dragStartTransform = {};
  }
}

void DesktopGizmoIntegrationAdapter::recordAcceptedTerminal(
    DesktopGizmoTerminal terminal, std::uint64_t sequence) noexcept {
  switch (terminal) {
  case DesktopGizmoTerminal::commit:
    incrementObservedCount(m_observation.acceptedCommitCount);
    break;
  case DesktopGizmoTerminal::detachRetainingPreview:
    incrementObservedCount(m_observation.acceptedDetachCount);
    break;
  case DesktopGizmoTerminal::latchRetainingPreview:
    incrementObservedCount(m_observation.acceptedLatchCount);
    break;
  }
  m_observation.lastTerminal = terminal;
  if (sequence != 0u) m_observation.lastTerminalFrame = m_frame.frameSerial;
  m_observation.lastTerminalSequence = sequence;
}

void DesktopGizmoIntegrationAdapter::rebuildSnapshot() noexcept {
  if (m_frame.frameSerial == 0u || !m_frame.viewport.valid()) {
    m_snapshot.reset();
    return;
  }
  demo::EditorOverlayFrameSnapshot snapshot;
  snapshot.frameSerial = m_frame.frameSerial;
  snapshot.documentRevision = m_frame.documentRevision;
  snapshot.selectionRevision = m_frame.selectionRevision;
  snapshot.mappingEpoch = m_frame.mappingEpoch;
  snapshot.viewport = m_frame.viewport;
  snapshot.viewProjection = m_frame.viewProjection;

  const GridModelResult grid =
      buildGridModel(m_frame.gridSettings, m_frame.gridInput, m_frame.viewport);
  if (grid.status == GridModelStatus::ready) {
    snapshot.gridVisible = true;
    snapshot.grid = grid.draw;
    snapshot.gridLines = grid.lines;
    snapshot.gridAxes.assign(grid.axes.begin(),
                             grid.axes.begin() + grid.axisCount);
  }

  if (m_frame.attachment && m_frame.selectedNode &&
      m_interaction.phase != GizmoInteractionPhase::detached) {
    m_frame.gizmoSettings.hovered = m_hovered;
    m_frame.gizmoSettings.active = m_interaction.activeComponent;
    const auto gizmo = buildGizmoDrawPacket(
        canonicalGizmoGeometry(), *m_frame.attachment, m_frame.viewport,
        m_frame.viewDepth, m_frame.gizmoSettings);
    if (gizmo) {
      snapshot.gizmoVisible = true;
      snapshot.gizmo = *gizmo;
    }
  }
  m_snapshot = std::move(snapshot);
}

GizmoStableNodeId stableGizmoNodeId(NodeId node) noexcept {
  if (!node.valid()) return {};
  // Mix the complete 64-bit authority and slot. The previous high-word shift
  // discarded the authority's low 32 bits, allowing unrelated documents to
  // alias the same Gizmo identity.
  std::uint64_t value = node.authority().value + 0x9e3779b97f4a7c15ull;
  value ^= static_cast<std::uint64_t>(node.slot()) +
           0x9e3779b97f4a7c15ull + (value << 6u) + (value >> 2u);
  value ^= static_cast<std::uint64_t>(node.generation()) +
           0x517cc1b727220a95ull + (value << 7u) + (value >> 3u);
  if (value == 0u) value = 1u;
  return {node.generation(), value == 0u ? 1u : value};
}

} // namespace demo::editor::ui
