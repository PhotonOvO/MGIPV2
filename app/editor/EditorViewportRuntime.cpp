#include "EditorViewportRuntime.h"

#include <imgui.h>

#include <glm/gtc/matrix_inverse.hpp>
#include <glm/gtc/quaternion.hpp>
#include <glm/gtx/quaternion.hpp>

#include <algorithm>
#include <array>
#include <cmath>
#include <limits>
#include <string>
#include <string_view>
#include <utility>

namespace demo::editor::ui {
namespace {

constexpr float kEpsilon = 1.0e-6f;

[[nodiscard]] bool finite(float value) noexcept { return std::isfinite(value); }

[[nodiscard]] bool finite(const glm::vec3 &value) noexcept {
  return finite(value.x) && finite(value.y) && finite(value.z);
}

[[nodiscard]] bool finite(const glm::vec4 &value) noexcept {
  return finite(value.x) && finite(value.y) && finite(value.z) && finite(value.w);
}

[[nodiscard]] bool finite(const glm::mat4 &value) noexcept {
  for (int column = 0; column < 4; ++column) {
    for (int row = 0; row < 4; ++row) {
      if (!finite(value[column][row])) return false;
    }
  }
  return true;
}

[[nodiscard]] std::optional<glm::mat4>
inverseFinite(const glm::mat4 &value) noexcept {
  if (!finite(value)) return std::nullopt;
  const float determinant = glm::determinant(value);
  if (!finite(determinant) || std::abs(determinant) <= kEpsilon) {
    return std::nullopt;
  }
  const glm::mat4 inverse = glm::inverse(value);
  return finite(inverse) ? std::optional<glm::mat4>(inverse) : std::nullopt;
}

[[nodiscard]] glm::vec3 transformPoint(const glm::mat4 &matrix,
                                        const glm::vec3 &point) noexcept {
  const glm::vec4 transformed = matrix * glm::vec4(point, 1.0f);
  if (!finite(transformed) || std::abs(transformed.w) <= kEpsilon) {
    return glm::vec3(std::numeric_limits<float>::quiet_NaN());
  }
  return glm::vec3(transformed) / transformed.w;
}

[[nodiscard]] std::optional<glm::vec2>
projectPoint(const glm::mat4 &viewProjection, const glm::vec3 &point,
            const EditorViewportMetrics &viewport) noexcept {
  const glm::vec4 clip = viewProjection * glm::vec4(point, 1.0f);
  if (!finite(clip) || std::abs(clip.w) <= kEpsilon) return std::nullopt;
  const glm::vec2 ndc = glm::vec2(clip) / clip.w;
  if (!finite(ndc.x) || !finite(ndc.y)) return std::nullopt;
  return glm::vec2(
      static_cast<float>(viewport.logical.origin.x +
                         (static_cast<double>(ndc.x) * 0.5 + 0.5) *
                             viewport.logical.extent.width),
      static_cast<float>(viewport.logical.origin.y +
                         (static_cast<double>(ndc.y) * 0.5 + 0.5) *
                             viewport.logical.extent.height));
}

[[nodiscard]] std::uint32_t color(const glm::vec4 &value) noexcept {
  const glm::vec4 clamped = glm::clamp(value, glm::vec4(0.0f), glm::vec4(1.0f));
  return ImGui::ColorConvertFloat4ToU32(
      ImVec4(clamped.r, clamped.g, clamped.b, clamped.a));
}

[[nodiscard]] std::uint32_t color(const ImVec4 &value) noexcept {
  return ImGui::ColorConvertFloat4ToU32(value);
}

[[nodiscard]] glm::vec3 componentAxis(TransformGizmoComponent component) noexcept {
  return transformGizmoComponentAxis(component);
}

[[nodiscard]] bool isTranslation(TransformGizmoComponent component) noexcept {
  return component >= TransformGizmoComponent::TranslateX &&
         component <= TransformGizmoComponent::TranslateYZ;
}

[[nodiscard]] bool isRotation(TransformGizmoComponent component) noexcept {
  return component >= TransformGizmoComponent::RotateX &&
         component <= TransformGizmoComponent::RotateZ;
}

[[nodiscard]] bool isScale(TransformGizmoComponent component) noexcept {
  return component >= TransformGizmoComponent::ScaleX &&
         component <= TransformGizmoComponent::ScaleUniform;
}

[[nodiscard]] glm::mat4 normalizedBasis(const glm::mat4 &value) noexcept {
  glm::mat4 result(1.0f);
  for (int column = 0; column < 3; ++column) {
    const glm::vec3 basis(value[column]);
    const float length = glm::length(basis);
    if (!finite(length) || length <= kEpsilon) return glm::mat4(1.0f);
    result[column] = glm::vec4(basis / length, 0.0f);
  }
  return result;
}

} // namespace

EditorViewportRuntime::EditorViewportRuntime(Callbacks callbacks) noexcept
    : m_router(m_clock), m_callbacks(std::move(callbacks)) {}

EditorViewportRuntime::~EditorViewportRuntime() noexcept {
  try {
    if (m_callbacks.cancelPicks) m_callbacks.cancelPicks();
  } catch (...) {
  }
  try {
    if (m_callbacks.bindSelectionOccurrences) {
      m_callbacks.bindSelectionOccurrences(nullptr);
    }
  } catch (...) {
  }
}

void EditorViewportRuntime::beginFrame(FrameInput input,
                                       EditorInputFrameId frame) noexcept {
  m_input = std::move(input);
  m_frame = frame;
  if (m_lastViewportEpoch != m_input.viewport.viewportGeneration) {
    if (m_lastViewportEpoch != 0u && m_callbacks.cancelPicks) {
      m_callbacks.cancelPicks();
    }
    m_lastViewportEpoch = m_input.viewport.viewportGeneration;
    m_hovered = TransformGizmoComponent::None;
    m_overlay.reset();
  }
  refreshSelection();
  resolveReadyPicks();
  rebuildOverlay();
}

void EditorViewportRuntime::updateSceneSnapshot(
    const SceneProjectionResult *projection,
    SceneSelectionValue selection,
    SelectionRevision selectionRevision) noexcept {
  m_input.projection = projection;
  m_input.selection = std::move(selection);
  m_input.selectionRevision = selectionRevision;
  rebuildOverlay();
}

void EditorViewportRuntime::updateViewSnapshot(
    EditorViewportMetrics viewport, const glm::mat4 &view,
    const glm::mat4 &cameraProjection, const glm::mat4 &viewProjection,
    const glm::vec3 &cameraPosition) noexcept {
  m_input.viewport = std::move(viewport);
  m_input.view = view;
  m_input.cameraProjection = cameraProjection;
  m_input.viewProjection = viewProjection;
  m_input.cameraPosition = cameraPosition;
  rebuildOverlay();
}

bool EditorViewportRuntime::ready() const noexcept {
  return m_input.projection != nullptr && m_input.viewport.valid() &&
         m_input.projection->succeeded();
}

bool EditorViewportRuntime::cameraNavigationSuppressed() const noexcept {
  return !m_input.cameraNavigationEnabled || !m_input.windowFocused ||
         m_input.modalActive ||
         m_input.imguiPointerCaptured || m_input.imguiKeyboardCaptured ||
         m_input.imguiTextCaptured || m_router.hasPointerCapture() ||
         m_drag.has_value() || m_hovered != TransformGizmoComponent::None;
}

bool EditorViewportRuntime::pointerCaptured() const noexcept {
  return m_router.hasPointerCapture();
}

bool EditorViewportRuntime::gizmoDragging() const noexcept {
  return m_drag.has_value() && m_drag->active;
}

TransformGizmoComponent EditorViewportRuntime::hoveredGizmo() const noexcept {
  return m_hovered;
}

const std::optional<EditorOverlayFrame> &
EditorViewportRuntime::overlay() const noexcept {
  return m_overlay;
}

std::uint64_t EditorViewportRuntime::viewportEpoch() const noexcept {
  return m_input.viewport.viewportGeneration;
}

std::uint64_t EditorViewportRuntime::frameValue() const noexcept {
  return m_frame.value;
}

std::uint64_t EditorViewportRuntime::nextInputSequence() const noexcept {
  return m_nextSequence.value;
}

EditorClickClassifierPhase EditorViewportRuntime::clickPhase() const noexcept {
  return m_router.clickPhase();
}

PhysicalViewportExtent EditorViewportRuntime::physicalExtentForLogical(
    LogicalViewportExtent logical,
    EditorFramebufferScale framebufferScale) noexcept {
  const auto extent = [](double logicalValue, double scale) noexcept {
    const double physical = logicalValue * scale;
    if (!std::isfinite(physical) || physical <= 0.0) return std::uint32_t{1u};
    const double rounded = std::ceil(physical);
    if (rounded >=
        static_cast<double>(std::numeric_limits<std::uint32_t>::max())) {
      return std::numeric_limits<std::uint32_t>::max();
    }
    return static_cast<std::uint32_t>(std::max(rounded, 1.0));
  };
  return {extent(logical.width, framebufferScale.x),
          extent(logical.height, framebufferScale.y)};
}

EditorInputRoutingContext
EditorViewportRuntime::routingContext() const noexcept {
  EditorInputRoutingContext context;
  context.viewport = m_input.viewport;
  const std::optional<CoherentPickPublication> publication =
      coherentPickPublication();
  if (publication) {
    context.lifecycle = {publication->fence.scene.authority,
                         publication->fence.scene.scene,
                         publication->fence.scene.sceneEpoch,
                         publication->fence.viewportGeneration};
  }
  context.modalActive = m_input.modalActive;
  context.gizmoActive = m_input.showGizmo &&
                        (m_drag.has_value() ||
                         m_hovered != TransformGizmoComponent::None);
  context.gizmoLatched = m_router.gizmoDragLatched();
  context.imguiPointerCaptured = m_input.imguiPointerCaptured;
  context.imguiKeyboardCaptured = m_input.imguiKeyboardCaptured;
  context.imguiTextCaptured = m_input.imguiTextCaptured;
  const bool rendererBusy = m_callbacks.rendererBusy &&
                            m_callbacks.rendererBusy();
  context.viewportPickingEnabled = publication.has_value() && !rendererBusy &&
                                   context.lifecycle.valid();
  context.cameraNavigationEnabled = m_input.cameraNavigationEnabled &&
                                   !m_drag.has_value();
  return context;
}

std::optional<EditorViewportRuntime::CoherentPickPublication>
EditorViewportRuntime::coherentPickPublication() const noexcept {
  if (!ready() || !m_callbacks.pickPublication) return std::nullopt;
  const auto publication = m_callbacks.pickPublication();
  if (!publication || !publication->first.valid() ||
      publication->second == 0u) {
    return std::nullopt;
  }

  const SceneProjectionResult &projection = *m_input.projection;
  if (!projection.ownerCapability.validateMapping(projection.mapping) ||
      projection.sceneActivationEpoch ==
          std::numeric_limits<std::uint64_t>::max() ||
      !projection.activeScene) {
    return std::nullopt;
  }
  const SceneRuntimeMappingFence &projectionFence = projection.mapping.fence();
  if (projectionFence.authority != projection.mapping.authority() ||
      projectionFence.appliedDocumentRevision != projection.documentRevision ||
      projectionFence.activeScene != projection.activeScene ||
      projectionFence.sceneActivationEpoch != projection.sceneActivationEpoch ||
      projectionFence.projectionDigest != projection.projectionDigest ||
      projectionFence.mappingEpoch != projection.mappingEpoch) {
    return std::nullopt;
  }
  const std::optional<SceneId> scene =
      SceneId::from(editorObjectId(*projection.activeScene));
  if (!scene) return std::nullopt;

  const EditorPickFence &fence = publication->first;
  if (fence.scene.authority != projectionFence.authority ||
      fence.scene.documentRevision != projection.documentRevision ||
      fence.scene.projectionRevision != m_input.viewport.projection.revision ||
      projection.mappingEpoch != m_input.viewport.projection.value ||
      fence.scene.scene != *scene ||
      fence.scene.sceneEpoch != projection.sceneActivationEpoch + 1u ||
      fence.viewportGeneration != m_input.viewport.viewportGeneration ||
      fence.viewRevision != m_input.viewport.view.revision) {
    return std::nullopt;
  }
  return CoherentPickPublication{fence, publication->second,
                                 projection.sceneActivationEpoch};
}

std::optional<TransformGizmoRay>
EditorViewportRuntime::rayAt(LogicalViewportPoint position) const noexcept {
  if (!ready()) return std::nullopt;
  const auto render = m_input.viewport.logicalToPhysical(position);
  if (!render || m_input.viewport.physical.extent.width == 0u ||
      m_input.viewport.physical.extent.height == 0u) {
    return std::nullopt;
  }
  return makeTransformGizmoRay(
      glm::vec2(static_cast<float>(render->localX),
                static_cast<float>(render->localY)),
      m_input.view, m_input.cameraProjection,
      glm::vec2(static_cast<float>(m_input.viewport.physical.extent.width),
                static_cast<float>(m_input.viewport.physical.extent.height)));
}

std::optional<std::uint32_t>
EditorViewportRuntime::projectedNodeIndex(NodeId node) const noexcept {
  if (!m_input.projection || !node.valid()) return std::nullopt;
  return m_input.projection->mapping.sceneAssetIndex(node.untyped());
}

glm::mat4 EditorViewportRuntime::nodeWorld(std::uint32_t nodeIndex) const noexcept {
  if (!m_input.projection || nodeIndex >= m_input.projection->asset.nodes.size()) {
    return glm::mat4(1.0f);
  }
  const auto &asset = m_input.projection->asset;
  std::vector<std::uint32_t> chain;
  chain.reserve(32u);
  std::uint32_t current = nodeIndex;
  for (std::size_t guard = 0u; guard <= asset.nodes.size(); ++guard) {
    if (current >= asset.nodes.size()) return glm::mat4(1.0f);
    if (std::find(chain.begin(), chain.end(), current) != chain.end()) {
      return glm::mat4(1.0f);
    }
    chain.push_back(current);
    const int parent = asset.nodes[current].parent;
    if (parent < 0) break;
    current = static_cast<std::uint32_t>(parent);
  }
  glm::mat4 result(1.0f);
  for (auto it = chain.rbegin(); it != chain.rend(); ++it) {
    result *= asset.nodes[*it].localTransform;
  }
  const std::uint32_t root = chain.back();
  bool applyRoot = true;
  for (std::size_t index = 0u; index < asset.rootNodes.size(); ++index) {
    if (asset.rootNodes[index] == root &&
        index < asset.sceneRootTransformApplyToRoot.size()) {
      applyRoot = asset.sceneRootTransformApplyToRoot[index] != 0u;
      break;
    }
  }
  if (applyRoot) result = asset.sceneRootTransform.matrix * result;
  return finite(result) ? result : glm::mat4(1.0f);
}

std::optional<TransformGizmoFrame>
EditorViewportRuntime::gizmoFrame(NodeId node) const noexcept {
  const auto index = projectedNodeIndex(node);
  if (!index || !m_input.projection) return std::nullopt;
  const glm::mat4 world = nodeWorld(*index);
  const glm::vec3 pivot = glm::vec3(world * glm::vec4(0.0f, 0.0f, 0.0f, 1.0f));
  glm::mat4 parent(1.0f);
  const int parentIndex = m_input.projection->asset.nodes[*index].parent;
  if (parentIndex >= 0) {
    parent = nodeWorld(static_cast<std::uint32_t>(parentIndex));
  } else {
    parent = m_input.projection->asset.sceneRootTransform.matrix;
  }
  if (!finite(pivot) || !finite(parent)) return std::nullopt;
  return TransformGizmoFrame{pivot, parent};
}

TransformGizmoHit
EditorViewportRuntime::gizmoHit(LogicalViewportPoint position) const noexcept {
  if (!m_input.showGizmo || !ready()) return {};
  const auto node = selectedNode();
  if (!node) return {};
  const auto frame = gizmoFrame(*node);
  const auto ray = rayAt(position);
  if (!frame || !ray) return {};

  const glm::vec3 viewSpace = glm::vec3(m_input.view * glm::vec4(frame->pivotWorld, 1.0f));
  const float depth = std::max(std::abs(viewSpace.z), 0.01f);
  const float projectionY = std::abs(m_input.cameraProjection[1][1]);
  const float physicalHeight = static_cast<float>(
      std::max(m_input.viewport.physical.extent.height, 1u));
  const float framebufferScaleY =
      static_cast<float>(m_input.viewport.physical.extent.height) /
      static_cast<float>(std::max(m_input.viewport.logical.extent.height, 1.0));
  const float worldScale = projectionY > kEpsilon
                               ? (2.0f * depth /
                                  (projectionY * physicalHeight)) *
                                     50.0f *
                                     framebufferScaleY
                               : 1.0f;
  glm::mat4 model = normalizedBasis(frame->parentWorld);
  model[0] *= worldScale;
  model[1] *= worldScale;
  model[2] *= worldScale;
  model[3] = glm::vec4(frame->pivotWorld, 1.0f);
  const auto inverse = inverseFinite(model);
  if (!inverse) return {};
  const glm::vec3 localOrigin = glm::vec3(*inverse * glm::vec4(ray->origin, 1.0f));
  const glm::vec3 localDirection = glm::normalize(
      glm::vec3(*inverse * glm::vec4(ray->direction, 0.0f)));
  if (!finite(localOrigin) || !finite(localDirection) ||
      glm::length(localDirection) <= kEpsilon) {
    return {};
  }
  return classifyTransformGizmoHit(
      TransformGizmoRay{localOrigin, localDirection}, {},
      TransformGizmoVisibility::All);
}

EditorInputDecision
EditorViewportRuntime::route(const EditorInputEvent &event) noexcept {
  // Host events may come from GLFW callbacks with their own monotonically
  // increasing sequence. Keep internally generated tick events ahead of them
  // so the router never receives a duplicate sequence.
  if (event.sequence.valid() &&
      event.sequence.value >= m_nextSequence.value &&
      event.sequence.value != std::numeric_limits<std::uint64_t>::max()) {
    m_nextSequence.value = event.sequence.value + 1u;
  }
  if (const auto *move = std::get_if<EditorPointerMoveEvent>(&event.payload)) {
    m_lastPointer = move->position;
    m_havePointer = true;
    m_hovered = m_drag ? m_drag->component : gizmoHit(move->position).component;
  } else if (const auto *button =
                 std::get_if<EditorPointerButtonEvent>(&event.payload)) {
    m_lastPointer = button->position;
    m_havePointer = true;
    m_hovered = m_drag ? m_drag->component : gizmoHit(button->position).component;
  }
  EditorInputDecision decision = m_router.route(event, routingContext());
  consumeDecision(event, decision);
  return decision;
}

EditorInputDecision EditorViewportRuntime::tick() noexcept {
  EditorInputEvent event;
  event.sequence = m_nextSequence;
  ++m_nextSequence.value;
  event.frame = m_frame;
  event.payload = EditorInputTickEvent{};
  return route(event);
}

void EditorViewportRuntime::consumeDecision(
    const EditorInputEvent &event, const EditorInputDecision &decision) noexcept {
  (void)event;
  if (decision.effects.pick) enqueuePick(*decision.effects.pick);
  if (decision.effects.beginGizmoDrag) beginGizmo();
  if (decision.effects.updateGizmoDrag && m_havePointer) {
    updateGizmo(m_lastPointer);
  }
  if (decision.effects.commitGizmoDrag) {
    finishGizmo(true, false, false);
  }
  if (decision.effects.latchGizmo) {
    finishGizmo(false, false, true);
  }
  if (decision.effects.detachGizmo) {
    finishGizmo(false, true, false);
  }
  if (decision.effects.clearSelection && m_callbacks.applySelection) {
    const EditorSelectionEffect effect{
        .kind = EditorSelectionEffectKind::clear,
        .cause = SelectionCause::escape,
        .expectedRevision = m_input.selectionRevision,
    };
    const RuntimeActionResult result = m_callbacks.applySelection(effect);
    if (!result.accepted) report(result.error);
    refreshSelection();
  }
}

std::optional<EditorPickRequest>
EditorViewportRuntime::makePickRequest(const EditorPickEmission &click) noexcept {
  const std::optional<CoherentPickPublication> publication =
      coherentPickPublication();
  const std::optional<TransformGizmoRay> ray = rayAt(click.logicalPosition);
  if (!publication || !ray) return std::nullopt;
  const glm::vec3 direction = glm::normalize(ray->direction);
  const WorldRay worldRay{
      {ray->origin.x, ray->origin.y, ray->origin.z},
      {direction.x, direction.y, direction.z},
  };
  return demo::editor::makeEditorPickRequest(
      m_nextPickId, click, publication->fence,
      publication->occurrenceGeneration, worldRay);
}

void EditorViewportRuntime::enqueuePick(const EditorPickEmission &click) noexcept {
  const auto request = makePickRequest(click);
  if (!request || !m_callbacks.submitPick) return;
  if (m_nextPickId == std::numeric_limits<std::uint64_t>::max()) return;
  ++m_nextPickId;
  try {
    const PickSubmitResult submitted = m_callbacks.submitPick(*request);
    if (!submitted.accepted()) return;
    const EditorPickingTrackResult tracked = m_picking.trackAccepted(*request);
    if (!tracked.accepted() && m_callbacks.cancelPicks) {
      m_callbacks.cancelPicks();
    }
  } catch (...) {
    if (m_callbacks.cancelPicks) m_callbacks.cancelPicks();
    report("Failed to enqueue editor pick");
  }
}

void EditorViewportRuntime::resolveReadyPicks() noexcept {
  if (!m_callbacks.pollPickResults) return;
  try {
    std::vector<EditorPickResult> terminals;
    m_callbacks.pollPickResults(terminals);
    for (const EditorPickResult &terminal : terminals) {
      (void)m_picking.acceptTerminal(terminal);
    }
    while (true) {
      EditorPickingCommitContext context;
      if (const auto publication = coherentPickPublication()) {
        context.currentFence = publication->fence;
        context.occurrenceGeneration = publication->occurrenceGeneration;
      }
      context.currentSelection = m_input.selection;
      context.selectionRevision = m_input.selectionRevision;
      context.soleHighlightedOccurrenceMatches =
          std::holds_alternative<PrimitiveOccurrenceKey>(m_input.selection);
      context.occurrences = &m_occurrences;
      const std::optional<EditorPickingEffect> effect =
          m_picking.commitReady(context);
      if (!effect) break;
      bool accepted = true;
      if (effect->selection && m_callbacks.applySelection) {
        const RuntimeActionResult action =
            m_callbacks.applySelection(*effect->selection);
        accepted = action.accepted;
        if (!accepted) report(action.error);
      }
      if (effect->camera && m_callbacks.applyCameraFocus) {
        accepted = m_callbacks.applyCameraFocus(*effect->camera);
        if (!accepted) report("Camera focus effect was rejected");
      }
      if (effect->disposition == EditorPickingEffectDisposition::error ||
          effect->disposition ==
              EditorPickingEffectDisposition::invalidTerminal) {
        report("Editor picking backend returned an invalid terminal");
      }
      refreshSelection();
      (void)accepted;
    }
  } catch (...) {
    report("Failed to process editor pick terminals");
  }
}

void EditorViewportRuntime::refreshSelection() noexcept {
  if (m_callbacks.selectionValue) {
    m_input.selection = m_callbacks.selectionValue();
  }
  if (m_callbacks.selectionRevision) {
    m_input.selectionRevision = m_callbacks.selectionRevision();
  }
  if (m_callbacks.bindSelectionOccurrences) {
    m_callbacks.bindSelectionOccurrences(
        coherentPickPublication() ? &m_occurrences : nullptr);
  }
}

void EditorViewportRuntime::drainSelectionAtFrameEnd() noexcept {
  if (m_callbacks.drainSelectionChanges) {
    try {
      const std::vector<SelectionChanged> changes =
          m_callbacks.drainSelectionChanges();
      for (const SelectionChanged &change : changes) {
        if (change.browserFocus && m_callbacks.browserFocus) {
          m_callbacks.browserFocus(*change.browserFocus);
        }
      }
    } catch (...) {
      report("Failed to drain selection changes");
    }
  }
  refreshSelection();
  if (m_callbacks.setHighlight) {
    m_callbacks.setHighlight(m_input.selection, m_input.selectionRevision);
  }
}

std::optional<RenderOccurrenceRecord>
EditorViewportRuntime::OccurrenceLookup::resolve(
    RenderOccurrenceId occurrence) const noexcept {
  if (m_owner == nullptr || !m_owner->m_callbacks.resolveOccurrence) {
    return std::nullopt;
  }
  const auto publication = m_owner->coherentPickPublication();
  return publication
             ? m_owner->m_callbacks.resolveOccurrence(
                   publication->fence, publication->occurrenceGeneration,
                   occurrence)
             : std::nullopt;
}

EditorDocumentAuthority
EditorViewportRuntime::OccurrenceLookup::authority() const noexcept {
  const auto publication =
      m_owner != nullptr ? m_owner->coherentPickPublication() : std::nullopt;
  return publication ? publication->fence.scene.authority
                     : EditorDocumentAuthority{};
}

std::uint64_t
EditorViewportRuntime::OccurrenceLookup::documentRevision() const noexcept {
  const auto publication =
      m_owner != nullptr ? m_owner->coherentPickPublication() : std::nullopt;
  return publication ? publication->fence.scene.documentRevision : 0u;
}

std::uint64_t
EditorViewportRuntime::OccurrenceLookup::sceneActivationEpoch() const noexcept {
  const auto publication =
      m_owner != nullptr ? m_owner->coherentPickPublication() : std::nullopt;
  return publication ? publication->rawSceneActivationEpoch : 0u;
}

std::uint64_t EditorViewportRuntime::OccurrenceLookup::generation() const noexcept {
  const auto publication =
      m_owner != nullptr ? m_owner->coherentPickPublication() : std::nullopt;
  return publication ? publication->occurrenceGeneration : 0u;
}

bool EditorViewportRuntime::OccurrenceLookup::contains(
    PrimitiveOccurrenceKey occurrence) const noexcept {
  if (m_owner == nullptr || !m_owner->m_callbacks.resolveOccurrenceKey) {
    return false;
  }
  const auto publication = m_owner->coherentPickPublication();
  return publication && m_owner->m_callbacks.resolveOccurrenceKey(
                            publication->fence,
                            publication->occurrenceGeneration,
                            occurrence).has_value();
}

bool EditorViewportRuntime::OccurrenceLookup::visible(
    PrimitiveOccurrenceKey occurrence) const noexcept {
  if (m_owner == nullptr || !m_owner->m_callbacks.resolveOccurrenceKey) {
    return false;
  }
  const auto publication = m_owner->coherentPickPublication();
  if (!publication) return false;
  const auto record = m_owner->m_callbacks.resolveOccurrenceKey(
      publication->fence, publication->occurrenceGeneration, occurrence);
  return record && record->visible;
}

bool EditorViewportRuntime::OccurrenceLookup::nodeHighlightEligible(
    NodeId node) const noexcept {
  if (m_owner == nullptr || !m_owner->m_callbacks.nodeHighlightEligible) {
    return false;
  }
  const auto publication = m_owner->coherentPickPublication();
  return publication && m_owner->m_callbacks.nodeHighlightEligible(
                            publication->fence,
                            publication->occurrenceGeneration, node);
}

std::optional<NodeId> EditorViewportRuntime::selectedNode() const noexcept {
  if (const auto *node = std::get_if<NodeId>(&m_input.selection)) {
    return *node;
  }
  if (const auto *primitive =
          std::get_if<PrimitiveOccurrenceKey>(&m_input.selection)) {
    return primitive->node;
  }
  return std::nullopt;
}

NodeTransform EditorViewportRuntime::applyGizmoDelta(
    const GizmoDrag &drag, LogicalViewportPoint position) const noexcept {
  NodeTransform value = drag.initial;
  const auto currentRay = rayAt(position);
  if (!currentRay) return value;
  const glm::vec3 viewDirection = glm::normalize(
      glm::vec3(glm::inverse(m_input.view)[2]));
  if (!finite(viewDirection)) return value;
  if (isTranslation(drag.component)) {
    const auto delta = computeTransformGizmoTranslationDelta(
        drag.component, drag.startRay, *currentRay, viewDirection, drag.frame);
    if (!delta) return value;
    glm::vec3 translation = glm::vec3(
        static_cast<float>(drag.initial.translation[0]),
        static_cast<float>(drag.initial.translation[1]),
        static_cast<float>(drag.initial.translation[2]));
    translation += delta->local;
    if (m_input.snapEnabled) {
      const float step = static_cast<float>(std::max(m_input.gridUnit, 0.001));
      if (const auto snapped = snapTransformGizmoVector(translation, step)) {
        translation = *snapped;
      }
    }
    value.translation = {translation.x, translation.y, translation.z};
  } else if (isRotation(drag.component)) {
    const auto degrees = computeTransformGizmoRotationDeltaDegrees(
        drag.component, drag.startRay, *currentRay, drag.frame);
    if (!degrees) return value;
    float angle = *degrees;
    if (m_input.snapEnabled) {
      if (const auto snapped = snapTransformGizmoValue(
              angle, static_cast<float>(std::max(m_input.snapRotationDegrees,
                                                 0.001)))) {
        angle = *snapped;
      }
    }
    const glm::vec3 axis = componentAxis(drag.component);
    const glm::quat initial(
        static_cast<float>(drag.initial.rotation[3]),
        static_cast<float>(drag.initial.rotation[0]),
        static_cast<float>(drag.initial.rotation[1]),
        static_cast<float>(drag.initial.rotation[2]));
    const glm::quat delta = glm::angleAxis(glm::radians(angle), axis);
    const glm::quat result = glm::normalize(delta * initial);
    value.rotation = {result.x, result.y, result.z, result.w};
  } else if (isScale(drag.component)) {
    const auto delta = computeTransformGizmoScaleDelta(
        drag.component,
        glm::vec3(static_cast<float>(drag.initial.scale[0]),
                  static_cast<float>(drag.initial.scale[1]),
                  static_cast<float>(drag.initial.scale[2])),
        drag.startMouse, glm::vec2(static_cast<float>(position.x),
                                   static_cast<float>(position.y)));
    if (!delta) return value;
    glm::vec3 scale = delta->value;
    if (m_input.snapEnabled) {
      if (const auto snapped = snapTransformGizmoVector(
              scale, static_cast<float>(std::max(m_input.snapScale, 0.001)))) {
        scale = glm::max(*snapped, glm::vec3(0.01f));
      }
    }
    value.scale = {scale.x, scale.y, scale.z};
  }
  return value;
}

void EditorViewportRuntime::beginGizmo() noexcept {
  if (m_drag || !m_havePointer || !m_callbacks.transform) return;
  const auto node = selectedNode();
  if (!node) return;
  const auto frame = gizmoFrame(*node);
  const auto ray = rayAt(m_lastPointer);
  if (!frame || !ray || m_hovered == TransformGizmoComponent::None ||
      !m_input.projection) {
    return;
  }
  const auto projected = projectedNodeIndex(*node);
  if (!projected) return;
  // The controller remains the authoritative transform source.  The caller
  // supplies the typed value through the existing command/continuous path.
  NodeTransform initial;
  // `FrameInput` intentionally carries no mutable document.  The host sends
  // the current value through the transform callback's begin path; initialize
  // from the projected TRS so the first preview is deterministic.
  const demo::SceneNode &sceneNode = m_input.projection->asset.nodes[*projected];
  bool authoritativeTransform = false;
  if (m_callbacks.getTransform) {
    if (const auto authoritative = m_callbacks.getTransform(*node)) {
      initial = *authoritative;
      authoritativeTransform = true;
    }
  }
  if (!authoritativeTransform) {
    initial.translation = {sceneNode.translation.x, sceneNode.translation.y,
                           sceneNode.translation.z};
    initial.rotation = {sceneNode.rotation.x, sceneNode.rotation.y,
                        sceneNode.rotation.z, sceneNode.rotation.w};
    initial.scale = {sceneNode.scale.x, sceneNode.scale.y, sceneNode.scale.z};
  }
  const RuntimeActionResult action = m_callbacks.transform(
      *node, EditPhase::begin, initial);
  if (!action.accepted) {
    report(action.error);
    return;
  }
  m_drag = GizmoDrag{*node, initial, *frame, m_hovered, *ray,
                     glm::vec2(static_cast<float>(m_lastPointer.x),
                               static_cast<float>(m_lastPointer.y)),
                     true};
}

void EditorViewportRuntime::updateGizmo(LogicalViewportPoint position) noexcept {
  if (!m_drag || !m_drag->active || !m_callbacks.transform) return;
  const NodeTransform value = applyGizmoDelta(*m_drag, position);
  const RuntimeActionResult action =
      m_callbacks.transform(m_drag->node, EditPhase::preview, value);
  if (!action.accepted) report(action.error);
}

void EditorViewportRuntime::finishGizmo(bool physicalRelease, bool escape,
                                        bool focusLost) noexcept {
  if (!m_drag || !m_drag->active) return;
  GizmoDrag drag = *m_drag;
  if (m_callbacks.finishTransform) {
    const RuntimeActionResult action =
        m_callbacks.finishTransform(drag.node, physicalRelease, escape,
                                    focusLost);
    if (!action.accepted && !action.error.empty()) report(action.error);
  }
  // Focus loss suspends the continuous session. Keep the local drag record
  // and pointer capture until a real release (or Escape) arrives.
  if (!(focusLost && !physicalRelease && !escape)) {
    m_drag.reset();
  }
}

void EditorViewportRuntime::endFrame() noexcept {
  EditorInputEvent close;
  close.sequence = m_nextSequence;
  if (m_nextSequence.value != std::numeric_limits<std::uint64_t>::max()) {
    ++m_nextSequence.value;
  }
  close.frame = m_frame;
  close.payload = EditorInputFrameCloseEvent{};
  const EditorInputDecision decision = m_router.route(close, routingContext());
  consumeDecision(close, decision);
  resolveReadyPicks();
  drainSelectionAtFrameEnd();
  rebuildOverlay();
}

void EditorViewportRuntime::rebuildOverlay() noexcept {
  m_overlay.reset();
  if (!ready()) return;
  EditorOverlayInput input;
  input.viewport.logicalExtent = glm::vec2(
      static_cast<float>(m_input.viewport.logical.extent.width),
      static_cast<float>(m_input.viewport.logical.extent.height));
  input.viewport.framebufferScale = glm::vec2(
      static_cast<float>(m_input.viewport.physical.extent.width) /
          static_cast<float>(std::max(m_input.viewport.logical.extent.width, 1.0)),
      static_cast<float>(m_input.viewport.physical.extent.height) /
          static_cast<float>(std::max(m_input.viewport.logical.extent.height, 1.0)));
  input.viewport.physicalRenderArea = EditorOverlayPixelRect{
      {m_input.viewport.physical.origin.x, m_input.viewport.physical.origin.y},
      {m_input.viewport.physical.extent.width,
       m_input.viewport.physical.extent.height}};
  input.viewport.colorTargetExtent = {
      m_input.viewport.colorAttachment.width,
      m_input.viewport.colorAttachment.height};
  input.viewport.sceneDepthExtent = {
      m_input.viewport.depthAttachment.width,
      m_input.viewport.depthAttachment.height};
  input.camera.viewProjection = m_input.viewProjection;
  input.camera.depthConvention = EditorOverlayDepthConvention::Reverse;
  if (m_input.showGrid) {
    input.grid.visible = true;
    input.grid.math.cameraPosition = m_input.cameraPosition;
    input.grid.math.cameraBackward = glm::vec3(glm::inverse(m_input.view)[2]);
    input.grid.math.baseUnit = static_cast<float>(m_input.gridUnit);
    input.grid.math.lineCount = 151u;
    input.grid.math.projection = EditorGridProjection::Perspective;
  }
  if (m_input.showGizmo) {
    if (const auto node = selectedNode()) {
      if (const auto frame = gizmoFrame(*node)) {
        const glm::vec3 viewSpace = glm::vec3(
            m_input.view * glm::vec4(frame->pivotWorld, 1.0f));
        input.gizmo.visible = true;
        input.gizmo.frame = *frame;
        input.gizmo.hovered = m_hovered;
        input.gizmo.active = m_drag ? m_drag->component
                                    : TransformGizmoComponent::None;
        input.gizmo.viewSpaceDepth = std::max(std::abs(viewSpace.z), 0.01f);
        input.gizmo.projectionScaleY = m_input.cameraProjection[1][1];
        input.gizmo.projection = EditorGridProjection::Perspective;
        input.gizmo.depthMode = EditorOverlayDepthMode::XRaySolid;
      }
    }
  }
  m_overlay = buildEditorOverlayFrame(input);
}

void EditorViewportRuntime::report(std::string_view message) const noexcept {
  if (message.empty() || !m_callbacks.error) return;
  try {
    m_callbacks.error(message);
  } catch (...) {
  }
}

void EditorViewportRuntime::drawImGuiOverlay(void *drawListPointer) const noexcept {
  if (drawListPointer == nullptr || !m_overlay) return;
  ImDrawList &drawList = *static_cast<ImDrawList *>(drawListPointer);
  const EditorOverlayFrame &frame = *m_overlay;
  const auto toScreen = [&](const glm::vec3 &point) {
    return projectPoint(frame.viewProjection, point, m_input.viewport);
  };
  const ImVec2 minimum(static_cast<float>(m_input.viewport.logical.origin.x),
                       static_cast<float>(m_input.viewport.logical.origin.y));
  const ImVec2 maximum(
      static_cast<float>(m_input.viewport.logical.origin.x +
                         m_input.viewport.logical.extent.width),
      static_cast<float>(m_input.viewport.logical.origin.y +
                         m_input.viewport.logical.extent.height));
  drawList.PushClipRect(minimum, maximum, true);

  for (const EditorOverlayDrawCommand &command : frame.commands()) {
    if (const auto *grid =
            std::get_if<EditorOverlayGridLinesCommand>(&command.payload)) {
      for (const EditorOverlayGridLevel &level : grid->visibleLevels) {
        const int half = static_cast<int>(grid->parameters.lineCount / 2u);
        for (int line = -half; line <= half; ++line) {
          const float x = level.center.x + static_cast<float>(line) * level.step;
          const float z0 = level.clipMinimum.y;
          const float z1 = level.clipMaximum.y;
          const auto a = toScreen(glm::vec3(x, 0.0f, z0));
          const auto b = toScreen(glm::vec3(x, 0.0f, z1));
          if (a && b) {
            ImVec4 c(grid->minorColor.r, grid->minorColor.g, grid->minorColor.b,
                     grid->minorColor.a * level.perspectiveAlpha);
            drawList.AddLine(ImVec2(a->x, a->y), ImVec2(b->x, b->y), color(c),
                             grid->lineWidthLogicalPixels);
          }
          const float z = level.center.y + static_cast<float>(line) * level.step;
          const auto c0 = toScreen(glm::vec3(level.clipMinimum.x, 0.0f, z));
          const auto c1 = toScreen(glm::vec3(level.clipMaximum.x, 0.0f, z));
          if (c0 && c1) {
            ImVec4 c(grid->minorColor.r, grid->minorColor.g, grid->minorColor.b,
                     grid->minorColor.a * level.perspectiveAlpha);
            drawList.AddLine(ImVec2(c0->x, c0->y), ImVec2(c1->x, c1->y), color(c),
                             grid->lineWidthLogicalPixels);
          }
        }
      }
    } else if (const auto *axes =
                   std::get_if<EditorOverlayGridAxesCommand>(&command.payload)) {
      const float extent = std::max(axes->clipExtent.x, axes->clipExtent.y);
      const std::array<glm::vec3, 3> origins{glm::vec3(0.0f), glm::vec3(0.0f),
                                             glm::vec3(0.0f)};
      const std::array<glm::vec3, 3> ends{glm::vec3(extent, 0.0f, 0.0f),
                                          glm::vec3(0.0f, extent, 0.0f),
                                          glm::vec3(0.0f, 0.0f, extent)};
      for (std::size_t index = 0u; index < ends.size(); ++index) {
        const auto a = toScreen(origins[index]);
        const auto b = toScreen(ends[index]);
        if (a && b) {
          drawList.AddLine(ImVec2(a->x, a->y), ImVec2(b->x, b->y),
                           color(axes->colors[index]), axes->lineWidthLogicalPixels);
        }
      }
    } else if (const auto *handle =
                   std::get_if<EditorOverlayGizmoHandleCommand>(&command.payload)) {
      const glm::vec3 pivot = glm::vec3(handle->localToWorld[3]);
      const glm::vec3 axis = glm::normalize(glm::vec3(handle->localToWorld[0]));
      const glm::vec3 axisY = glm::normalize(glm::vec3(handle->localToWorld[1]));
      const glm::vec3 axisZ = glm::normalize(glm::vec3(handle->localToWorld[2]));
      const float length = 1.0f;
      if (handle->geometry == EditorOverlayGizmoGeometryKind::RotationRing) {
        std::vector<ImVec2> points;
        points.reserve(65u);
        for (int sample = 0; sample <= 64; ++sample) {
          const float angle = static_cast<float>(sample) *
                              6.28318530718f / 64.0f;
          const glm::vec3 point = pivot +
                                  (std::cos(angle) * axisY +
                                   std::sin(angle) * axisZ) * length;
          if (const auto screen = toScreen(point)) {
            points.emplace_back(screen->x, screen->y);
          }
        }
        if (points.size() > 1u) {
          drawList.AddPolyline(points.data(), static_cast<int>(points.size()),
                               color(handle->color), ImDrawFlags_None, 2.0f);
        }
      } else if (handle->geometry == EditorOverlayGizmoGeometryKind::TranslationPlane) {
        const glm::vec3 p0 = pivot + axis * 0.35f;
        const glm::vec3 p1 = p0 + axisY * 0.22f;
        const glm::vec3 p2 = p0 + axisZ * 0.22f;
        const auto a = toScreen(p0);
        const auto b = toScreen(p1);
        const auto c = toScreen(p2);
        if (a && b && c) {
          const ImVec2 points[4] = {ImVec2(a->x, a->y), ImVec2(b->x, b->y),
                                   ImVec2(b->x + c->x - a->x, b->y + c->y - a->y),
                                   ImVec2(c->x, c->y)};
          drawList.AddConvexPolyFilled(points, 4, color(handle->color));
        }
      } else {
        const auto a = toScreen(pivot);
        const auto b = toScreen(pivot + axis * length);
        if (a && b) {
          drawList.AddLine(ImVec2(a->x, a->y), ImVec2(b->x, b->y),
                           color(handle->color), 3.0f);
          drawList.AddCircleFilled(ImVec2(b->x, b->y), 5.0f,
                                   color(handle->color));
        }
      }
    }
  }

  if (const auto node = selectedNode(); node && m_input.projection) {
    if (const auto index = projectedNodeIndex(*node)) {
        const demo::SceneNode &sceneNode = m_input.projection->asset.nodes[*index];
      for (const std::uint32_t meshIndex : sceneNode.meshRefs) {
        if (meshIndex >= m_input.projection->asset.meshes.size()) continue;
        const demo::SceneMesh &mesh = m_input.projection->asset.meshes[meshIndex];
        const glm::mat4 world = nodeWorld(*index);
        std::array<glm::vec3, 8> corners{};
        const glm::vec3 minimum = mesh.localBoundsMin;
        const glm::vec3 maximum = mesh.localBoundsMax;
        std::size_t cornerIndex = 0u;
        for (int x = 0; x < 2; ++x) {
          for (int y = 0; y < 2; ++y) {
            for (int z = 0; z < 2; ++z) {
              corners[cornerIndex++] = transformPoint(
                  world, glm::vec3(x ? maximum.x : minimum.x,
                                   y ? maximum.y : minimum.y,
                                   z ? maximum.z : minimum.z));
            }
          }
        }
        glm::vec2 minScreen(std::numeric_limits<float>::infinity());
        glm::vec2 maxScreen(-std::numeric_limits<float>::infinity());
        bool valid = true;
        for (const glm::vec3 &corner : corners) {
          const auto screen = toScreen(corner);
          if (!screen) {
            valid = false;
            break;
          }
          minScreen = glm::min(minScreen, *screen);
          maxScreen = glm::max(maxScreen, *screen);
        }
        if (valid && finite(minScreen.x) && finite(maxScreen.x)) {
          drawList.AddRect(ImVec2(minScreen.x, minScreen.y),
                           ImVec2(maxScreen.x, maxScreen.y),
                           IM_COL32(72, 196, 255, 210), 2.0f, 0, 2.0f);
        }
      }
    }
  }
  drawList.PopClipRect();
}

} // namespace demo::editor::ui
