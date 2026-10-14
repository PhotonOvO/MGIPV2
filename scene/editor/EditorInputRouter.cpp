#include <mgif/scene_editor/EditorInputRouter.h>

#include <algorithm>
#include <cmath>

namespace demo::editor {
namespace {

[[nodiscard]] bool finite(LogicalViewportPoint value) noexcept {
  return std::isfinite(value.x) && std::isfinite(value.y);
}

[[nodiscard]] bool validAction(EditorInputAction action) noexcept {
  return action == EditorInputAction::press ||
         action == EditorInputAction::release;
}

[[nodiscard]] bool validButton(EditorPointerButton button) noexcept {
  return button == EditorPointerButton::primary ||
         button == EditorPointerButton::secondary ||
         button == EditorPointerButton::middle;
}

} // namespace

EditorInputRouter::EditorInputRouter(
    const EditorClock &clock, EditorClickClassifierConfig clickConfig) noexcept
    : m_clock(&clock), m_clickClassifier(clock, clickConfig) {}

bool EditorInputRouter::valid() const noexcept {
  return m_clock != nullptr && m_clickClassifier.valid();
}

std::optional<std::size_t>
EditorInputRouter::buttonIndex(EditorPointerButton button) noexcept {
  if (!validButton(button)) {
    return std::nullopt;
  }
  return static_cast<std::size_t>(button);
}

bool EditorInputRouter::payloadValid(
    const EditorInputEventPayload &payload) noexcept {
  if (const auto *move = std::get_if<EditorPointerMoveEvent>(&payload)) {
    return finite(move->position);
  }
  if (const auto *button = std::get_if<EditorPointerButtonEvent>(&payload)) {
    return finite(button->position) && validButton(button->button) &&
           validAction(button->action) &&
           !(button->action == EditorInputAction::press && !button->delivered);
  }
  if (const auto *wheel = std::get_if<EditorPointerWheelEvent>(&payload)) {
    return finite(wheel->position) && std::isfinite(wheel->horizontalDelta) &&
           std::isfinite(wheel->verticalDelta);
  }
  if (const auto *key = std::get_if<EditorKeyEvent>(&payload)) {
    return (key->key == EditorInputKey::other ||
            key->key == EditorInputKey::escape) &&
           validAction(key->action);
  }
  if (const auto *text = std::get_if<EditorTextInputEvent>(&payload)) {
    const std::uint32_t codePoint = static_cast<std::uint32_t>(text->codePoint);
    return codePoint <= 0x10ffffu &&
           !(codePoint >= 0xd800u && codePoint <= 0xdfffu);
  }
  if (const auto *focus = std::get_if<EditorFocusEvent>(&payload)) {
    return focus->action == EditorFocusAction::gained ||
           focus->action == EditorFocusAction::lost;
  }
  return std::holds_alternative<EditorInputTickEvent>(payload) ||
         std::holds_alternative<EditorInputFrameCloseEvent>(payload) ||
         std::holds_alternative<EditorInputLifecycleResetEvent>(payload);
}

EditorInputConsumer EditorInputRouter::higherPriority(
    EditorInputConsumer left, EditorInputConsumer right) noexcept {
  return static_cast<std::uint8_t>(left) >=
                 static_cast<std::uint8_t>(right)
             ? left
             : right;
}

bool EditorInputRouter::pickingEligible(
    const EditorInputRoutingContext &context) const noexcept {
  return context.viewportPickingEnabled && context.lifecycle.valid() &&
         context.viewport.valid() &&
         context.lifecycle.viewportGeneration ==
             context.viewport.viewportGeneration;
}

EditorInputCoordinates EditorInputRouter::coordinates(
    LogicalViewportPoint point,
    const EditorInputRoutingContext &context) const noexcept {
  EditorInputCoordinates result;
  result.logical = point;
  if (context.viewport.valid()) {
    result.physical = context.viewport.logicalToPhysical(point);
  }
  return result;
}

EditorInputConsumer EditorInputRouter::pointerConsumer(
    EditorPointerButton button, bool inside,
    const EditorInputRoutingContext &context) const noexcept {
  EditorInputConsumer consumer = EditorInputConsumer::router;
  if (inside) {
    if (button == EditorPointerButton::primary && pickingEligible(context)) {
      consumer = EditorInputConsumer::viewportPicking;
    } else if (context.cameraNavigationEnabled) {
      consumer = EditorInputConsumer::cameraNavigation;
    }
  }
  for (const ButtonState &state : m_buttons) {
    if (state.down) {
      consumer = higherPriority(consumer, state.owner);
    }
  }
  if (context.imguiPointerCaptured) {
    consumer = higherPriority(consumer,
                              EditorInputConsumer::imguiCapturedWidget);
  }
  if (m_gizmoDragActive || m_gizmoDragLatched || context.gizmoLatched ||
      (context.gizmoActive && inside)) {
    consumer = higherPriority(consumer, EditorInputConsumer::activeGizmo);
  }
  if (context.modalActive) {
    consumer = EditorInputConsumer::modal;
  }
  return consumer;
}

EditorInputDecision
EditorInputRouter::decisionFor(const EditorInputEvent &event) const noexcept {
  EditorInputDecision result;
  result.sequence = event.sequence;
  result.frame = event.frame;
  return result;
}

void EditorInputRouter::cancelGizmoWithoutHistory(
    EditorInputEffects &effects, bool clearSelection) noexcept {
  if (m_gizmoDragActive || m_gizmoDragLatched) {
    effects.detachGizmo = true;
  }
  effects.clearSelection |= clearSelection;
  if (m_pointerCaptured) {
    effects.releasePointerCapture = true;
  }
  for (ButtonState &button : m_buttons) {
    if (button.owner == EditorInputConsumer::activeGizmo) {
      button = {};
    }
  }
  m_gizmoDragActive = false;
  m_gizmoDragLatched = false;
  m_pointerCaptured = false;
}

void EditorInputRouter::syncLifecycle(
    const EditorInputRoutingContext &context,
    EditorInputDecision &decision) noexcept {
  const bool usable = context.lifecycle.valid() &&
                      context.viewport.viewportGeneration ==
                          context.lifecycle.viewportGeneration;
  if (m_lifecycle && usable && *m_lifecycle == context.lifecycle) {
    return;
  }
  if (m_lifecycle || m_gizmoDragActive || m_gizmoDragLatched ||
      m_clickClassifier.hasPendingSingle() ||
      m_clickClassifier.primaryDown()) {
    cancelGizmoWithoutHistory(decision.effects, false);
    m_clickClassifier.reset();
    m_buttons = {};
    m_releaseThisFrame = false;
    m_escapeThisFrame = false;
    m_focusLostThisFrame = false;
    m_cancelThisFrame = false;
    decision.status = EditorInputRouteStatus::routed;
    decision.consumer = EditorInputConsumer::router;
  }
  m_lifecycle = usable
                    ? std::optional<EditorInputLifecycleFence>{context.lifecycle}
                    : std::nullopt;
}

EditorInputDecision EditorInputRouter::route(
    const EditorInputEvent &event,
    const EditorInputRoutingContext &context) noexcept {
  EditorInputDecision rejected = decisionFor(event);
  if (!event.sequence.valid()) {
    rejected.status = EditorInputRouteStatus::rejectedInvalid;
    return rejected;
  }
  if (m_lastSequence.valid() && event.sequence.value <= m_lastSequence.value) {
    rejected.status = EditorInputRouteStatus::rejectedStale;
    return rejected;
  }
  m_lastSequence = event.sequence;
  if (!valid() || !event.frame.valid() || !payloadValid(event.payload)) {
    rejected.status = EditorInputRouteStatus::rejectedInvalid;
    return rejected;
  }
  if (m_currentFrame.valid() && event.frame.value < m_currentFrame.value) {
    rejected.status = EditorInputRouteStatus::rejectedStale;
    return rejected;
  }
  if (m_currentFrame.valid() && event.frame != m_currentFrame &&
      (m_releaseThisFrame || m_escapeThisFrame || m_focusLostThisFrame ||
       m_cancelThisFrame)) {
    rejected.status = EditorInputRouteStatus::rejectedState;
    return rejected;
  }
  m_currentFrame = event.frame;

  EditorInputDecision decision = decisionFor(event);
  syncLifecycle(context, decision);
  if (decision.status == EditorInputRouteStatus::routed) {
    return decision;
  }
  if (std::holds_alternative<EditorInputLifecycleResetEvent>(event.payload)) {
    const bool hadIntent = m_gizmoDragActive || m_gizmoDragLatched ||
                           m_clickClassifier.hasPendingSingle() ||
                           m_clickClassifier.primaryDown();
    cancelGizmoWithoutHistory(decision.effects, false);
    resetPhysicalIntent();
    decision.status = hadIntent ? EditorInputRouteStatus::routed
                                : EditorInputRouteStatus::ignored;
    decision.consumer = EditorInputConsumer::router;
    return decision;
  }
  if (std::holds_alternative<EditorInputFrameCloseEvent>(event.payload)) {
    return closeFrame(event, context);
  }
  if (std::holds_alternative<EditorInputTickEvent>(event.payload)) {
    return routeTick(event, context);
  }
  if (const auto *move = std::get_if<EditorPointerMoveEvent>(&event.payload)) {
    return routePointerMove(event, *move, context);
  }
  if (const auto *button =
          std::get_if<EditorPointerButtonEvent>(&event.payload)) {
    return routePointerButton(event, *button, context);
  }
  if (const auto *wheel =
          std::get_if<EditorPointerWheelEvent>(&event.payload)) {
    return routeWheel(event, *wheel, context);
  }
  if (const auto *key = std::get_if<EditorKeyEvent>(&event.payload)) {
    return routeKey(event, *key, context);
  }
  if (const auto *focus = std::get_if<EditorFocusEvent>(&event.payload)) {
    return routeFocus(event, *focus, context);
  }
  if (std::holds_alternative<EditorTextInputEvent>(event.payload)) {
    if (context.modalActive) {
      decision.consumer = EditorInputConsumer::modal;
    } else if (context.imguiTextCaptured || context.imguiKeyboardCaptured) {
      decision.consumer = EditorInputConsumer::imguiCapturedWidget;
    }
    decision.status = decision.consumer == EditorInputConsumer::router
                          ? EditorInputRouteStatus::ignored
                          : EditorInputRouteStatus::routed;
    return decision;
  }
  decision.status = EditorInputRouteStatus::rejectedInvalid;
  return decision;
}

EditorInputDecision EditorInputRouter::routePointerMove(
    const EditorInputEvent &event, const EditorPointerMoveEvent &move,
    const EditorInputRoutingContext &context) noexcept {
  EditorInputDecision decision = decisionFor(event);
  decision.coordinates = coordinates(move.position, context);
  m_lastPointer = move.position;
  m_havePointer = true;
  const bool inside = decision.coordinates->insideViewport();

  EditorInputConsumer consumer = pointerConsumer(
      EditorPointerButton::primary, inside, context);
  decision.consumer = consumer;
  decision.status = consumer == EditorInputConsumer::router
                        ? EditorInputRouteStatus::ignored
                        : EditorInputRouteStatus::routed;
  if (m_buttons[0].down &&
      m_buttons[0].owner == EditorInputConsumer::viewportPicking) {
    m_clickClassifier.pointerMove(move.position);
  }
  decision.effects.updateGizmoDrag =
      consumer == EditorInputConsumer::activeGizmo && m_gizmoDragActive &&
      !m_releaseThisFrame && !m_escapeThisFrame;
  if (context.modalActive && (m_gizmoDragActive || m_gizmoDragLatched)) {
    m_cancelThisFrame = true;
  }
  return decision;
}

EditorInputDecision EditorInputRouter::routePointerButton(
    const EditorInputEvent &event, const EditorPointerButtonEvent &button,
    const EditorInputRoutingContext &context) noexcept {
  EditorInputDecision decision = decisionFor(event);
  decision.coordinates = coordinates(button.position, context);
  m_lastPointer = button.position;
  m_havePointer = true;
  const bool inside = decision.coordinates->insideViewport();
  const std::size_t index = *buttonIndex(button.button);
  ButtonState &state = m_buttons[index];

  if (button.action == EditorInputAction::press) {
    if (state.down) {
      decision.status = EditorInputRouteStatus::rejectedState;
      return decision;
    }
    const EditorInputConsumer consumer =
        pointerConsumer(button.button, inside, context);
    state = {true, consumer};
    decision.consumer = consumer;
    decision.status = consumer == EditorInputConsumer::router
                          ? EditorInputRouteStatus::ignored
                          : EditorInputRouteStatus::routed;
    if (consumer == EditorInputConsumer::activeGizmo &&
        button.button == EditorPointerButton::primary && inside &&
        !m_gizmoDragActive && !m_gizmoDragLatched) {
      m_gizmoDragActive = true;
      m_pointerCaptured = true;
      decision.effects.beginGizmoDrag = true;
    } else if (consumer == EditorInputConsumer::viewportPicking &&
               button.button == EditorPointerButton::primary) {
      decision.effects.pick = m_clickClassifier.primaryPress(
          button.position, context.viewport, button.recognizedDouble);
    }
    return decision;
  }

  if (!state.down) {
    decision.status = EditorInputRouteStatus::rejectedState;
    return decision;
  }
  const EditorInputConsumer originalOwner = state.owner;
  EditorInputConsumer consumer = originalOwner;
  if (context.imguiPointerCaptured) {
    consumer = higherPriority(consumer,
                              EditorInputConsumer::imguiCapturedWidget);
  }
  if (m_gizmoDragActive || m_gizmoDragLatched || context.gizmoLatched) {
    consumer = higherPriority(consumer, EditorInputConsumer::activeGizmo);
  }
  if (context.modalActive) {
    consumer = EditorInputConsumer::modal;
  }
  state = {};
  decision.consumer = consumer;
  decision.status = consumer == EditorInputConsumer::router
                        ? EditorInputRouteStatus::ignored
                        : EditorInputRouteStatus::routed;

  if (!button.delivered) {
    if (button.button == EditorPointerButton::primary &&
        originalOwner == EditorInputConsumer::viewportPicking) {
      m_clickClassifier.cancelPhysicalPress();
    }
    return decision;
  }
  if (button.button == EditorPointerButton::primary &&
      (m_gizmoDragActive || m_gizmoDragLatched)) {
    if (context.modalActive) {
      m_cancelThisFrame = true;
    } else {
      m_releaseThisFrame = true;
    }
    decision.consumer = context.modalActive ? EditorInputConsumer::modal
                                            : EditorInputConsumer::activeGizmo;
    decision.status = EditorInputRouteStatus::routed;
    return decision;
  }
  if (button.button == EditorPointerButton::primary &&
      originalOwner == EditorInputConsumer::viewportPicking) {
    if (consumer == EditorInputConsumer::viewportPicking) {
      m_clickClassifier.primaryRelease(button.position);
    } else {
      m_clickClassifier.cancelPhysicalPress();
    }
  }
  return decision;
}

EditorInputDecision EditorInputRouter::routeWheel(
    const EditorInputEvent &event, const EditorPointerWheelEvent &wheel,
    const EditorInputRoutingContext &context) noexcept {
  EditorInputDecision decision = decisionFor(event);
  decision.coordinates = coordinates(wheel.position, context);
  decision.consumer = pointerConsumer(EditorPointerButton::middle,
                                      decision.coordinates->insideViewport(),
                                      context);
  decision.status = decision.consumer == EditorInputConsumer::router
                        ? EditorInputRouteStatus::ignored
                        : EditorInputRouteStatus::routed;
  if (context.modalActive && (m_gizmoDragActive || m_gizmoDragLatched)) {
    m_cancelThisFrame = true;
  }
  return decision;
}

EditorInputDecision EditorInputRouter::routeKey(
    const EditorInputEvent &event, const EditorKeyEvent &key,
    const EditorInputRoutingContext &context) noexcept {
  EditorInputDecision decision = decisionFor(event);
  if (key.key != EditorInputKey::escape) {
    EditorInputConsumer consumer = context.cameraNavigationEnabled
                                       ? EditorInputConsumer::cameraNavigation
                                       : EditorInputConsumer::router;
    if (context.imguiKeyboardCaptured || context.imguiTextCaptured) {
      consumer = higherPriority(consumer,
                                EditorInputConsumer::imguiCapturedWidget);
    }
    if (m_gizmoDragActive || m_gizmoDragLatched || context.gizmoLatched) {
      consumer = higherPriority(consumer, EditorInputConsumer::activeGizmo);
    }
    if (context.modalActive) {
      consumer = EditorInputConsumer::modal;
    }
    decision.consumer = consumer;
    decision.status = consumer == EditorInputConsumer::router
                          ? EditorInputRouteStatus::ignored
                          : EditorInputRouteStatus::routed;
    return decision;
  }

  if (key.action == EditorInputAction::press) {
    if (m_escapeDown) {
      decision.status = EditorInputRouteStatus::rejectedState;
      return decision;
    }
    m_escapeDown = true;
    if (context.modalActive) {
      decision.consumer = EditorInputConsumer::modal;
    } else if (m_gizmoDragActive || m_gizmoDragLatched ||
               context.gizmoLatched) {
      decision.consumer = EditorInputConsumer::activeGizmo;
      m_escapeThisFrame = true;
    } else if (context.imguiKeyboardCaptured || context.imguiTextCaptured) {
      decision.consumer = EditorInputConsumer::imguiCapturedWidget;
    } else {
      decision.consumer = EditorInputConsumer::router;
      decision.effects.clearSelection = true;
    }
    decision.status = EditorInputRouteStatus::routed;
    return decision;
  }
  if (!m_escapeDown) {
    decision.status = EditorInputRouteStatus::rejectedState;
    return decision;
  }
  m_escapeDown = false;
  decision.consumer = context.modalActive
                          ? EditorInputConsumer::modal
                          : EditorInputConsumer::router;
  decision.status = context.modalActive ? EditorInputRouteStatus::routed
                                        : EditorInputRouteStatus::ignored;
  return decision;
}

EditorInputDecision EditorInputRouter::routeFocus(
    const EditorInputEvent &event, const EditorFocusEvent &focus,
    const EditorInputRoutingContext &context) noexcept {
  EditorInputDecision decision = decisionFor(event);
  if (focus.action == EditorFocusAction::lost) {
    m_clickClassifier.cancelPhysicalPress();
    m_escapeDown = false;
    if (m_gizmoDragActive || m_gizmoDragLatched) {
      m_focusLostThisFrame = true;
      decision.consumer = EditorInputConsumer::activeGizmo;
      decision.status = EditorInputRouteStatus::routed;
      return decision;
    }
  }
  if (context.modalActive) {
    decision.consumer = EditorInputConsumer::modal;
  } else if (context.imguiPointerCaptured || context.imguiKeyboardCaptured ||
             context.imguiTextCaptured) {
    decision.consumer = EditorInputConsumer::imguiCapturedWidget;
  }
  decision.status = decision.consumer == EditorInputConsumer::router
                        ? EditorInputRouteStatus::ignored
                        : EditorInputRouteStatus::routed;
  return decision;
}

EditorInputDecision EditorInputRouter::routeTick(
    const EditorInputEvent &event,
    const EditorInputRoutingContext &context) noexcept {
  EditorInputDecision decision = decisionFor(event);
  if (context.modalActive) {
    decision.consumer = EditorInputConsumer::modal;
  } else if (m_gizmoDragActive || m_gizmoDragLatched || context.gizmoLatched ||
             context.gizmoActive) {
    decision.consumer = EditorInputConsumer::activeGizmo;
  } else if (context.imguiPointerCaptured || context.imguiKeyboardCaptured ||
             context.imguiTextCaptured) {
    decision.consumer = EditorInputConsumer::imguiCapturedWidget;
  } else if (pickingEligible(context) && m_havePointer) {
    decision.effects.pick = m_clickClassifier.update(
        m_lastPointer, context.viewport, true);
    decision.consumer = decision.effects.pick
                            ? EditorInputConsumer::viewportPicking
                            : EditorInputConsumer::router;
  }
  decision.status = decision.consumer == EditorInputConsumer::router
                        ? EditorInputRouteStatus::ignored
                        : EditorInputRouteStatus::routed;
  return decision;
}

EditorInputDecision EditorInputRouter::closeFrame(
    const EditorInputEvent &event,
    const EditorInputRoutingContext &context) noexcept {
  EditorInputDecision decision = decisionFor(event);
  if (m_escapeThisFrame && (m_gizmoDragActive || m_gizmoDragLatched)) {
    decision.consumer = EditorInputConsumer::activeGizmo;
    cancelGizmoWithoutHistory(decision.effects, true);
  } else if (m_cancelThisFrame &&
             (m_gizmoDragActive || m_gizmoDragLatched)) {
    decision.consumer = context.modalActive ? EditorInputConsumer::modal
                                            : EditorInputConsumer::activeGizmo;
    cancelGizmoWithoutHistory(decision.effects, false);
  } else if (m_focusLostThisFrame &&
             (m_gizmoDragActive || m_gizmoDragLatched)) {
    decision.consumer = EditorInputConsumer::activeGizmo;
    // Focus loss wins over a same-frame delivered-up edge.  If the drag was
    // still active, transition it to the no-history latched state; a repeated
    // loss while already latched is an idempotent ownership-preserving no-op.
    if (m_gizmoDragActive) {
      decision.effects.latchGizmo = true;
      decision.effects.releasePointerCapture = m_pointerCaptured;
      m_gizmoDragActive = false;
      m_gizmoDragLatched = true;
      m_pointerCaptured = false;
    }
  } else if (m_releaseThisFrame && m_gizmoDragActive) {
    decision.consumer = EditorInputConsumer::activeGizmo;
    decision.effects.commitGizmoDrag = true;
    decision.effects.detachGizmo = true;
    decision.effects.releasePointerCapture = m_pointerCaptured;
    m_gizmoDragActive = false;
    m_gizmoDragLatched = false;
    m_pointerCaptured = false;
  } else if (m_releaseThisFrame && m_gizmoDragLatched) {
    // A release observed after focus loss cannot resume or commit the
    // suspended transaction. Keep logical ownership until an explicit detach.
    decision.consumer = EditorInputConsumer::activeGizmo;
  }
  m_releaseThisFrame = false;
  m_escapeThisFrame = false;
  m_focusLostThisFrame = false;
  m_cancelThisFrame = false;

  if (decision.consumer == EditorInputConsumer::router &&
      !decision.effects.detachGizmo && pickingEligible(context) &&
      m_havePointer && !context.modalActive && !context.gizmoActive &&
      !context.gizmoLatched && !context.imguiPointerCaptured &&
      !context.imguiKeyboardCaptured && !context.imguiTextCaptured) {
    decision.effects.pick = m_clickClassifier.update(
        m_lastPointer, context.viewport, true);
    if (decision.effects.pick) {
      decision.consumer = EditorInputConsumer::viewportPicking;
    }
  }
  decision.status =
      decision.consumer != EditorInputConsumer::router ||
              decision.effects.commitGizmoDrag || decision.effects.detachGizmo ||
              decision.effects.latchGizmo || decision.effects.clearSelection ||
              decision.effects.pick
          ? EditorInputRouteStatus::routed
          : EditorInputRouteStatus::ignored;
  return decision;
}

void EditorInputRouter::resetPhysicalIntent() noexcept {
  m_clickClassifier.reset();
  m_buttons = {};
  m_lifecycle.reset();
  m_lastPointer = {};
  m_havePointer = false;
  m_gizmoDragActive = false;
  m_gizmoDragLatched = false;
  m_pointerCaptured = false;
  m_escapeDown = false;
  m_releaseThisFrame = false;
  m_escapeThisFrame = false;
  m_focusLostThisFrame = false;
  m_cancelThisFrame = false;
}

bool EditorInputRouter::gizmoDragActive() const noexcept {
  return m_gizmoDragActive;
}

bool EditorInputRouter::gizmoDragLatched() const noexcept {
  return m_gizmoDragLatched;
}

bool EditorInputRouter::hasPointerCapture() const noexcept {
  if (m_pointerCaptured) {
    return true;
  }
  return std::any_of(m_buttons.begin(), m_buttons.end(),
                     [](const ButtonState &button) {
                       // A button routed to the camera-navigation consumer is
                       // the fly camera's own look gesture, not a capture held
                       // against it. Counting it made
                       // EditorViewportRuntime::cameraNavigationSuppressed()
                       // true for the whole right-drag, so the host never
                       // applied the rotation and immediately released the
                       // disabled cursor again.
                       return button.down &&
                              button.owner != EditorInputConsumer::router &&
                              button.owner !=
                                  EditorInputConsumer::cameraNavigation;
                     });
}

bool EditorInputRouter::buttonDown(EditorPointerButton button) const noexcept {
  const std::optional<std::size_t> index = buttonIndex(button);
  return index && m_buttons[*index].down;
}

EditorInputConsumer EditorInputRouter::capturedConsumer(
    EditorPointerButton button) const noexcept {
  const std::optional<std::size_t> index = buttonIndex(button);
  return index && m_buttons[*index].down
             ? m_buttons[*index].owner
             : EditorInputConsumer::router;
}

EditorClickClassifierPhase EditorInputRouter::clickPhase() const noexcept {
  return m_clickClassifier.phase();
}

EditorInputEventSequence
EditorInputRouter::lastEventSequence() const noexcept {
  return m_lastSequence;
}

EditorInputFrameId EditorInputRouter::currentFrame() const noexcept {
  return m_currentFrame;
}

} // namespace demo::editor
