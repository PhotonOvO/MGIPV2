#include <mgif/scene_editor/EditorPicking.h>

#include <algorithm>
#include <cmath>
#include <limits>
#include <utility>

namespace demo::editor {
namespace {

[[nodiscard]] bool finite(LogicalViewportPoint value) noexcept {
  return std::isfinite(value.x) && std::isfinite(value.y);
}

[[nodiscard]] bool validOccurrenceKey(
    const PrimitiveOccurrenceKey &key,
    const EditorSceneFence &fence) noexcept {
  return key.scene.valid() && key.node.valid() && key.primitive.valid() &&
         key.scene == fence.scene && key.scene.authority() == fence.authority &&
         key.node.authority() == fence.authority &&
         key.primitive.authority() == fence.authority;
}

[[nodiscard]] bool hitMatchesRequest(const EditorPickRequest &request,
                                     const EditorPickResult &result) noexcept {
  if (!result.worldHit || !result.rayDistance ||
      !result.worldHit->finite() || !std::isfinite(*result.rayDistance) ||
      *result.rayDistance <= 0.0) {
    return false;
  }
  const double distance = *result.rayDistance;
  const EditorWorldPoint expected{
      request.ray.origin.x + request.ray.direction.x * distance,
      request.ray.origin.y + request.ray.direction.y * distance,
      request.ray.origin.z + request.ray.direction.z * distance};
  const double dx = result.worldHit->x - expected.x;
  const double dy = result.worldHit->y - expected.y;
  const double dz = result.worldHit->z - expected.z;
  const double errorSquared = dx * dx + dy * dy + dz * dz;
  const double tolerance = 1.0e-5 * std::max(1.0, distance);
  return expected.finite() && std::isfinite(errorSquared) &&
         errorSquared <= tolerance * tolerance;
}

[[nodiscard]] EditorWorldPoint expectedHitPoint(
    const EditorPickRequest &request, double distance) noexcept {
  return {request.ray.origin.x + request.ray.direction.x * distance,
          request.ray.origin.y + request.ray.direction.y * distance,
          request.ray.origin.z + request.ray.direction.z * distance};
}

[[nodiscard]] EditorSelectionEffect clearEffect(
    const EditorPickingCommitContext &context) noexcept {
  EditorSelectionEffect effect;
  effect.kind = EditorSelectionEffectKind::clear;
  effect.cause = SelectionCause::viewportSingle;
  effect.expectedRevision = context.selectionRevision;
  return effect;
}

} // namespace

bool EditorPickEmission::valid() const noexcept {
  return validPickIntent(intent) && finite(logicalPosition) && sample.valid() &&
         viewportGeneration != 0 && projection.valid() && view.valid();
}

EditorClickClassifier::EditorClickClassifier(
    const EditorClock &clock, EditorClickClassifierConfig config) noexcept
    : m_clock(&clock), m_config(config) {}

bool EditorClickClassifier::valid() const noexcept {
  return m_clock != nullptr &&
         m_config.doubleClickInterval > EditorTimePoint::duration::zero() &&
         std::isfinite(m_config.dragThresholdLogicalPixels) &&
         m_config.dragThresholdLogicalPixels > 0.0;
}

EditorClickClassifierPhase EditorClickClassifier::phase() const noexcept {
  return m_phase;
}

bool EditorClickClassifier::hasPendingSingle() const noexcept {
  return m_phase == EditorClickClassifierPhase::singlePending;
}

bool EditorClickClassifier::primaryDown() const noexcept {
  return m_primaryDown;
}

std::optional<EditorPickEmission> EditorClickClassifier::emission(
    PickIntent intent, LogicalViewportPoint currentPointer,
    const EditorViewportMetrics &currentMetrics,
    EditorTimePoint now) const noexcept {
  if (!valid() || !currentMetrics.valid() || !finite(currentPointer)) {
    return std::nullopt;
  }
  const std::optional<PhysicalViewportPixel> pixel =
      currentMetrics.logicalToPhysical(currentPointer);
  if (!pixel) {
    return std::nullopt;
  }
  EditorPickEmission result;
  result.intent = intent;
  result.logicalPosition = currentPointer;
  result.sample = {pixel->localX, pixel->localY,
                   currentMetrics.physical.extent.width,
                   currentMetrics.physical.extent.height};
  result.viewportGeneration = currentMetrics.viewportGeneration;
  result.projection = currentMetrics.projection;
  result.view = currentMetrics.view;
  result.emittedAt = now;
  return result.valid() ? std::optional<EditorPickEmission>{result}
                        : std::nullopt;
}

std::optional<EditorPickEmission> EditorClickClassifier::primaryPress(
    LogicalViewportPoint currentPointer,
    const EditorViewportMetrics &currentMetrics,
    bool recognizedDouble) noexcept {
  if (!valid() || m_primaryDown || !finite(currentPointer) ||
      !currentMetrics.containsLogical(currentPointer)) {
    return std::nullopt;
  }
  const EditorTimePoint now = m_clock->now();
  if (recognizedDouble &&
      m_phase == EditorClickClassifierPhase::singlePending) {
    const std::optional<EditorPickEmission> result = emission(
        PickIntent::doubleFocus, currentPointer, currentMetrics, now);
    if (result) {
      m_phase = EditorClickClassifierPhase::doubleClick;
      m_primaryDown = true;
      m_pressPosition = currentPointer;
      m_pressTime = now;
    }
    return result;
  }
  if (m_phase != EditorClickClassifierPhase::idle) {
    return std::nullopt;
  }
  m_primaryDown = true;
  m_pressPosition = currentPointer;
  m_pressTime = now;
  return std::nullopt;
}

void EditorClickClassifier::pointerMove(
    LogicalViewportPoint currentPointer) noexcept {
  if (!m_primaryDown || m_phase != EditorClickClassifierPhase::idle ||
      !finite(currentPointer) || !valid()) {
    return;
  }
  const double dx = currentPointer.x - m_pressPosition.x;
  const double dy = currentPointer.y - m_pressPosition.y;
  const double distance = std::hypot(dx, dy);
  if (!std::isfinite(distance) ||
      distance > m_config.dragThresholdLogicalPixels) {
    m_phase = EditorClickClassifierPhase::drag;
  }
}

void EditorClickClassifier::primaryRelease(
    LogicalViewportPoint currentPointer) noexcept {
  if (!m_primaryDown) {
    return;
  }
  if (!finite(currentPointer) || !valid() ||
      m_phase == EditorClickClassifierPhase::drag ||
      m_phase == EditorClickClassifierPhase::doubleClick) {
    reset();
    return;
  }
  m_primaryDown = false;
  m_phase = EditorClickClassifierPhase::singlePending;
}

std::optional<EditorPickEmission> EditorClickClassifier::update(
    LogicalViewportPoint currentPointer,
    const EditorViewportMetrics &currentMetrics, bool eligible) noexcept {
  if (!eligible || !valid() ||
      m_phase != EditorClickClassifierPhase::singlePending ||
      !finite(currentPointer) ||
      !currentMetrics.containsLogical(currentPointer)) {
    return std::nullopt;
  }
  const EditorTimePoint now = m_clock->now();
  if (now < m_pressTime ||
      now - m_pressTime <= m_config.doubleClickInterval) {
    return std::nullopt;
  }
  const std::optional<EditorPickEmission> result =
      emission(PickIntent::singleSelect, currentPointer, currentMetrics, now);
  if (result) {
    m_phase = EditorClickClassifierPhase::idle;
  }
  return result;
}

void EditorClickClassifier::cancelPhysicalPress() noexcept {
  if (!m_primaryDown) {
    return;
  }
  m_phase = EditorClickClassifierPhase::idle;
  m_pressPosition = {};
  m_pressTime = {};
  m_primaryDown = false;
}

void EditorClickClassifier::reset() noexcept {
  m_phase = EditorClickClassifierPhase::idle;
  m_pressPosition = {};
  m_pressTime = {};
  m_primaryDown = false;
}

std::optional<EditorPickRequest> makeEditorPickRequest(
    std::uint64_t serial, const EditorPickEmission &emission,
    const EditorPickFence &fence, std::uint64_t occurrenceGeneration,
    const WorldRay &ray) noexcept {
  if (!emission.valid() || !fence.valid() ||
      fence.viewportGeneration != emission.viewportGeneration ||
      fence.scene.projectionRevision != emission.projection.revision ||
      fence.viewRevision != emission.view.revision) {
    return std::nullopt;
  }
  EditorPickRequest request{serial, emission.intent, fence,
                            occurrenceGeneration, emission.sample, ray};
  return request.valid() ? std::optional<EditorPickRequest>{request}
                         : std::nullopt;
}

bool EditorCameraFocusEffect::valid() const noexcept {
  return center.finite() && std::isfinite(navigationSpeed) &&
         navigationSpeed > 0.0 && expectedViewRevision != 0 &&
         expectedViewRevision != std::numeric_limits<std::uint64_t>::max() &&
         nextViewRevision == expectedViewRevision + 1u && keepEye;
}

bool EditorPickingCommitContext::valid() const noexcept {
  if (!currentFence.valid() || occurrenceGeneration == 0 ||
      occurrences == nullptr) {
    return false;
  }
  if (std::holds_alternative<std::monostate>(currentSelection)) {
    return true;
  }
  const std::optional<EditorDocumentAuthority> authority =
      sceneSelectionAuthority(currentSelection);
  return authority && *authority == currentFence.scene.authority;
}

EditorPickingTrackResult
EditorPickingController::trackAccepted(const EditorPickRequest &request) {
  if (!request.valid()) {
    return {EditorPickingTrackStatus::rejectedInvalid, request.serial};
  }
  if (request.serial <= m_lastAcceptedSerial) {
    return {EditorPickingTrackStatus::rejectedSerial, request.serial};
  }
  m_pending.push_back(Pending{request, std::nullopt});
  m_lastAcceptedSerial = request.serial;
  ++m_stats.trackedRequests;
  return {EditorPickingTrackStatus::accepted, request.serial};
}

bool EditorPickingController::acceptTerminal(
    const EditorPickResult &result) noexcept {
  if (result.serial <= m_lastCommittedSerial && result.serial != 0) {
    ++m_stats.ignoredDuplicateTerminals;
    return false;
  }
  const std::size_t index = pendingIndex(result.serial);
  if (index == m_pending.size()) {
    ++m_stats.ignoredUnknownTerminals;
    return false;
  }
  if (m_pending[index].terminal) {
    ++m_stats.ignoredDuplicateTerminals;
    return false;
  }

  m_pending[index].terminal = result;
  return true;
}

std::optional<EditorPickingEffect> EditorPickingController::commitReady(
    const EditorPickingCommitContext &context) noexcept {
  if (m_pending.empty() || !m_pending.front().terminal) {
    return std::nullopt;
  }
  EditorPickingEffect effect = commit(m_pending.front(), context);
  if (effect.disposition == EditorPickingEffectDisposition::stale) {
    ++m_stats.staleTerminals;
  }
  m_lastCommittedSerial = m_pending.front().request.serial;
  m_pending.erase(m_pending.begin());
  ++m_stats.committedTerminals;
  return effect;
}

EditorPickingEffect EditorPickingController::commit(
    const Pending &pending,
    const EditorPickingCommitContext &context) noexcept {
  const EditorPickRequest &request = pending.request;
  const EditorPickResult &terminal = *pending.terminal;
  EditorPickingEffect effect;
  effect.serial = request.serial;
  effect.intent = request.intent;

  if (!terminal.terminalPayloadValid() || terminal.serial != request.serial ||
      terminal.intent != request.intent || terminal.fence != request.fence ||
      terminal.occurrenceGeneration != request.occurrenceGeneration) {
    effect.disposition = EditorPickingEffectDisposition::invalidTerminal;
    return effect;
  }
  if (!context.valid() ||
      !sameEditorPickFence(request.fence, context.currentFence) ||
      request.occurrenceGeneration != context.occurrenceGeneration) {
    effect.disposition = EditorPickingEffectDisposition::stale;
    return effect;
  }
  if (terminal.completion == PickCompletion::cancelled) {
    effect.disposition = EditorPickingEffectDisposition::cancelled;
    return effect;
  }
  if (terminal.completion == PickCompletion::error) {
    effect.disposition = EditorPickingEffectDisposition::error;
    return effect;
  }

  std::optional<RenderOccurrenceRecord> occurrence;
  if (terminal.completion == PickCompletion::hit && terminal.occurrence) {
    occurrence = context.occurrences->resolve(*terminal.occurrence);
  }
  const bool validHit =
      terminal.completion == PickCompletion::hit && occurrence &&
      occurrence->id == *terminal.occurrence && occurrence->visible &&
      validOccurrenceKey(occurrence->key, context.currentFence.scene) &&
      hitMatchesRequest(request, terminal);
  if (terminal.completion == PickCompletion::miss || !validHit) {
    effect.disposition = EditorPickingEffectDisposition::blank;
    if (request.intent == PickIntent::singleSelect) {
      effect.selection = clearEffect(context);
    }
    return effect;
  }

  if (request.intent == PickIntent::singleSelect) {
    effect.disposition = EditorPickingEffectDisposition::selection;
    EditorSelectionEffect selection;
    selection.occurrence = occurrence->key;
    selection.expectedRevision = context.selectionRevision;
    const auto *selected =
        std::get_if<PrimitiveOccurrenceKey>(&context.currentSelection);
    if (selected != nullptr && *selected == occurrence->key) {
      selection.kind = EditorSelectionEffectKind::togglePrimitive;
      selection.cause = SelectionCause::viewportSamePrimitiveToggle;
      // Exact Primitive occurrence equality is the primary toggle contract.
      // The sole-highlight flag is only an additional Node edge upstream.
      selection.soleHighlightedOccurrenceMatches = true;
    } else {
      selection.kind = EditorSelectionEffectKind::setPrimitive;
      selection.cause = SelectionCause::viewportSingle;
      if (context.selectionRevision !=
          std::numeric_limits<SelectionRevision>::max()) {
        effect.browserFocus = SceneBrowserFocusRequest{
            context.selectionRevision + 1u,
            SceneSelectionValue{occurrence->key}, true, true, false};
      }
    }
    effect.selection = selection;
    return effect;
  }

  if (context.currentFence.viewRevision ==
      std::numeric_limits<std::uint64_t>::max()) {
    effect.disposition = EditorPickingEffectDisposition::error;
    return effect;
  }
  EditorCameraFocusEffect camera{
      expectedHitPoint(request, *terminal.rayDistance), *terminal.rayDistance,
      context.currentFence.viewRevision,
      context.currentFence.viewRevision + 1u, true};
  if (!camera.valid()) {
    effect.disposition = EditorPickingEffectDisposition::invalidTerminal;
    return effect;
  }
  effect.disposition = EditorPickingEffectDisposition::camera;
  effect.camera = camera;
  return effect;
}

void EditorPickingController::resetPending() noexcept {
  m_stats.resetPendingRequests += m_pending.size();
  m_pending.clear();
  m_lastCommittedSerial = std::max(m_lastCommittedSerial, m_lastAcceptedSerial);
}

std::size_t EditorPickingController::pendingCount() const noexcept {
  return m_pending.size();
}

std::optional<std::uint64_t>
EditorPickingController::headSerial() const noexcept {
  return m_pending.empty() ? std::nullopt
                           : std::optional<std::uint64_t>{
                                 m_pending.front().request.serial};
}

const EditorPickingControllerStats &
EditorPickingController::stats() const noexcept {
  return m_stats;
}

std::size_t
EditorPickingController::pendingIndex(std::uint64_t serial) const noexcept {
  for (std::size_t index = 0; index < m_pending.size(); ++index) {
    if (m_pending[index].request.serial == serial) {
      return index;
    }
  }
  return m_pending.size();
}

} // namespace demo::editor
