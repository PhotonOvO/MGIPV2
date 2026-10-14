#pragma once

#include <mgif/scene_editor/EditorPicking.h>

#include <array>
#include <compare>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <variant>

namespace demo::editor {

struct EditorInputEventSequence final {
  std::uint64_t value{0};

  [[nodiscard]] constexpr bool valid() const noexcept { return value != 0; }
  auto operator<=>(const EditorInputEventSequence &) const = default;
};

struct EditorInputFrameId final {
  std::uint64_t value{0};

  [[nodiscard]] constexpr bool valid() const noexcept { return value != 0; }
  auto operator<=>(const EditorInputFrameId &) const = default;
};

struct EditorInputLifecycleFence final {
  EditorDocumentAuthority authority;
  SceneId scene;
  std::uint64_t sceneEpoch{0};
  std::uint64_t viewportGeneration{0};

  [[nodiscard]] constexpr bool valid() const noexcept {
    return authority.valid() && scene.valid() &&
           scene.authority() == authority && sceneEpoch != 0 &&
           viewportGeneration != 0;
  }

  auto operator<=>(const EditorInputLifecycleFence &) const = default;
};

enum class EditorPointerButton : std::uint8_t {
  primary = 0,
  secondary,
  middle,
};

enum class EditorInputAction : std::uint8_t {
  press = 0,
  release,
};

enum class EditorInputKey : std::uint8_t {
  other = 0,
  escape,
};

enum class EditorFocusAction : std::uint8_t {
  gained = 0,
  lost,
};

struct EditorPointerMoveEvent final {
  LogicalViewportPoint position;
  bool operator==(const EditorPointerMoveEvent &) const = default;
};

struct EditorPointerButtonEvent final {
  LogicalViewportPoint position;
  EditorPointerButton button{EditorPointerButton::primary};
  EditorInputAction action{EditorInputAction::press};
  bool recognizedDouble{false};
  bool delivered{true};

  bool operator==(const EditorPointerButtonEvent &) const = default;
};

struct EditorPointerWheelEvent final {
  LogicalViewportPoint position;
  double horizontalDelta{0.0};
  double verticalDelta{0.0};

  bool operator==(const EditorPointerWheelEvent &) const = default;
};

struct EditorKeyEvent final {
  EditorInputKey key{EditorInputKey::other};
  EditorInputAction action{EditorInputAction::press};

  bool operator==(const EditorKeyEvent &) const = default;
};

struct EditorTextInputEvent final {
  char32_t codePoint{0};
  bool operator==(const EditorTextInputEvent &) const = default;
};

struct EditorFocusEvent final {
  EditorFocusAction action{EditorFocusAction::gained};
  bool operator==(const EditorFocusEvent &) const = default;
};

struct EditorInputTickEvent final {
  bool operator==(const EditorInputTickEvent &) const = default;
};

struct EditorInputFrameCloseEvent final {
  bool operator==(const EditorInputFrameCloseEvent &) const = default;
};

struct EditorInputLifecycleResetEvent final {
  bool operator==(const EditorInputLifecycleResetEvent &) const = default;
};

using EditorInputEventPayload =
    std::variant<EditorInputTickEvent, EditorInputFrameCloseEvent,
                 EditorInputLifecycleResetEvent, EditorPointerMoveEvent,
                 EditorPointerButtonEvent, EditorPointerWheelEvent,
                 EditorKeyEvent, EditorTextInputEvent, EditorFocusEvent>;

struct EditorInputEvent final {
  EditorInputEventSequence sequence;
  EditorInputFrameId frame;
  EditorInputEventPayload payload;

  bool operator==(const EditorInputEvent &) const = default;
};

struct EditorInputRoutingContext final {
  EditorViewportMetrics viewport;
  EditorInputLifecycleFence lifecycle;
  bool modalActive{false};
  bool gizmoActive{false};
  bool gizmoLatched{false};
  bool imguiPointerCaptured{false};
  bool imguiKeyboardCaptured{false};
  bool imguiTextCaptured{false};
  bool viewportPickingEnabled{false};
  bool cameraNavigationEnabled{false};

  bool operator==(const EditorInputRoutingContext &) const = default;
};

enum class EditorInputConsumer : std::uint8_t {
  router = 0,
  cameraNavigation,
  viewportPicking,
  imguiCapturedWidget,
  activeGizmo,
  modal,
};

enum class EditorInputRouteStatus : std::uint8_t {
  routed = 0,
  ignored,
  rejectedInvalid,
  rejectedStale,
  rejectedState,
};

struct EditorInputCoordinates final {
  LogicalViewportPoint logical;
  std::optional<PhysicalViewportPixel> physical;

  [[nodiscard]] bool insideViewport() const noexcept {
    return physical.has_value();
  }

  bool operator==(const EditorInputCoordinates &) const = default;
};

struct EditorInputEffects final {
  bool beginGizmoDrag{false};
  bool updateGizmoDrag{false};
  bool commitGizmoDrag{false};
  bool detachGizmo{false};
  bool latchGizmo{false};
  bool releasePointerCapture{false};
  bool clearSelection{false};
  std::optional<EditorPickEmission> pick;

  bool operator==(const EditorInputEffects &) const = default;
};

struct EditorInputDecision final {
  EditorInputEventSequence sequence;
  EditorInputFrameId frame;
  EditorInputRouteStatus status{EditorInputRouteStatus::ignored};
  EditorInputConsumer consumer{EditorInputConsumer::router};
  std::optional<EditorInputCoordinates> coordinates;
  EditorInputEffects effects;

  [[nodiscard]] bool routed() const noexcept {
    return status == EditorInputRouteStatus::routed;
  }
  [[nodiscard]] bool rejected() const noexcept {
    return status == EditorInputRouteStatus::rejectedInvalid ||
           status == EditorInputRouteStatus::rejectedStale ||
           status == EditorInputRouteStatus::rejectedState;
  }

  bool operator==(const EditorInputDecision &) const = default;
};

class EditorInputRouter final {
public:
  explicit EditorInputRouter(
      const EditorClock &clock,
      EditorClickClassifierConfig clickConfig = {}) noexcept;

  [[nodiscard]] bool valid() const noexcept;
  [[nodiscard]] EditorInputDecision
  route(const EditorInputEvent &event,
        const EditorInputRoutingContext &context) noexcept;
  void resetPhysicalIntent() noexcept;

  [[nodiscard]] bool gizmoDragActive() const noexcept;
  [[nodiscard]] bool gizmoDragLatched() const noexcept;
  [[nodiscard]] bool hasPointerCapture() const noexcept;
  [[nodiscard]] bool buttonDown(EditorPointerButton button) const noexcept;
  [[nodiscard]] EditorInputConsumer
  capturedConsumer(EditorPointerButton button) const noexcept;
  [[nodiscard]] EditorClickClassifierPhase clickPhase() const noexcept;
  [[nodiscard]] EditorInputEventSequence lastEventSequence() const noexcept;
  [[nodiscard]] EditorInputFrameId currentFrame() const noexcept;

private:
  struct ButtonState final {
    bool down{false};
    EditorInputConsumer owner{EditorInputConsumer::router};
  };

  [[nodiscard]] static std::optional<std::size_t>
  buttonIndex(EditorPointerButton button) noexcept;
  [[nodiscard]] static bool
  payloadValid(const EditorInputEventPayload &payload) noexcept;
  [[nodiscard]] static EditorInputConsumer
  higherPriority(EditorInputConsumer left,
                 EditorInputConsumer right) noexcept;
  [[nodiscard]] EditorInputConsumer
  pointerConsumer(EditorPointerButton button, bool inside,
                  const EditorInputRoutingContext &context) const noexcept;
  [[nodiscard]] EditorInputCoordinates
  coordinates(LogicalViewportPoint point,
              const EditorInputRoutingContext &context) const noexcept;
  [[nodiscard]] bool pickingEligible(
      const EditorInputRoutingContext &context) const noexcept;
  [[nodiscard]] EditorInputDecision decisionFor(
      const EditorInputEvent &event) const noexcept;
  void syncLifecycle(const EditorInputRoutingContext &context,
                     EditorInputDecision &decision) noexcept;
  void cancelGizmoWithoutHistory(EditorInputEffects &effects,
                                 bool clearSelection) noexcept;

  [[nodiscard]] EditorInputDecision
  routePointerMove(const EditorInputEvent &event,
                   const EditorPointerMoveEvent &move,
                   const EditorInputRoutingContext &context) noexcept;
  [[nodiscard]] EditorInputDecision
  routePointerButton(const EditorInputEvent &event,
                     const EditorPointerButtonEvent &button,
                     const EditorInputRoutingContext &context) noexcept;
  [[nodiscard]] EditorInputDecision
  routeWheel(const EditorInputEvent &event,
             const EditorPointerWheelEvent &wheel,
             const EditorInputRoutingContext &context) noexcept;
  [[nodiscard]] EditorInputDecision
  routeKey(const EditorInputEvent &event, const EditorKeyEvent &key,
           const EditorInputRoutingContext &context) noexcept;
  [[nodiscard]] EditorInputDecision
  routeFocus(const EditorInputEvent &event, const EditorFocusEvent &focus,
             const EditorInputRoutingContext &context) noexcept;
  [[nodiscard]] EditorInputDecision
  routeTick(const EditorInputEvent &event,
            const EditorInputRoutingContext &context) noexcept;
  [[nodiscard]] EditorInputDecision
  closeFrame(const EditorInputEvent &event,
             const EditorInputRoutingContext &context) noexcept;

  const EditorClock *m_clock{nullptr};
  EditorClickClassifier m_clickClassifier;
  std::array<ButtonState, 3> m_buttons{};
  std::optional<EditorInputLifecycleFence> m_lifecycle;
  EditorInputEventSequence m_lastSequence;
  EditorInputFrameId m_currentFrame;
  LogicalViewportPoint m_lastPointer;
  bool m_havePointer{false};
  bool m_gizmoDragActive{false};
  bool m_gizmoDragLatched{false};
  bool m_pointerCaptured{false};
  bool m_escapeDown{false};
  bool m_releaseThisFrame{false};
  bool m_escapeThisFrame{false};
  bool m_focusLostThisFrame{false};
  bool m_cancelThisFrame{false};
};

} // namespace demo::editor
