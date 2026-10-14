#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <limits>

namespace demo::editor::ui {

class NativeGizmoTerminalQueue final {
public:
  enum class Kind : std::uint8_t {
    escapePressed = 0,
    primaryReleased,
    // A real GLFW focus callback is retained in the same FIFO as the other
    // native terminals so release/focus ordering cannot be reconstructed from
    // the final window state.
    focusLost,
  };

  struct Event final {
    Kind kind{Kind::escapePressed};
    std::uint64_t callbackSequence{0u};

    bool operator==(const Event &) const = default;
  };

  static constexpr std::size_t kCapacity = 32u;

  struct Batch final {
    std::array<Event, kCapacity> events{};
    std::size_t count{0u};
    bool overflowed{false};
    // The host must use its existing focus/release suppression for this batch.
    bool focusInterrupted{false};

    bool operator==(const Batch &) const = default;
  };

  explicit NativeGizmoTerminalQueue(
      std::uint64_t initialCallbackSequence = 0u) noexcept
      : m_callbackSequence(initialCallbackSequence) {}

  // Callers supply actual press/release callbacks, excluding key repeats.
  void observeEscape(bool down, bool nativeActive) noexcept {
    if (advance() && down && nativeActive)
      append(Kind::escapePressed);
  }

  void observePrimaryButton(bool down, bool nativeActive) noexcept {
    // Preserve every native release in callback order.  The host classifies a
    // release from an interrupted batch as suppressed; dropping it here loses
    // provenance and lets the later GLFW polling edge move the terminal into a
    // different application frame.
    if (advance() && !down && nativeActive)
      append(Kind::primaryReleased);
  }

  void observeFocus(bool focused, bool nativeActive = false) noexcept {
    const bool advanced = advance();
    if (!focused) {
      m_pending.focusInterrupted = true;
      if (nativeActive && advanced)
        append(Kind::focusLost);
    }
  }

  [[nodiscard]] bool focusInterrupted() const noexcept {
    return m_pending.focusInterrupted;
  }

  [[nodiscard]] std::uint64_t callbackSequence() const noexcept {
    return m_callbackSequence;
  }

  [[nodiscard]] std::size_t pendingCount() const noexcept {
    return m_pending.count;
  }

  [[nodiscard]] Batch take() noexcept {
    const Batch batch = m_pending;
    m_pending = {};
    return batch;
  }

  // Discard callbacks that belong to a document/window lifetime which has
  // already ended, while keeping the process-wide callback clock monotonic.
  void discardPending() noexcept { m_pending = {}; }

private:
  [[nodiscard]] bool advance() noexcept {
    if (m_callbackSequence ==
        (std::numeric_limits<std::uint64_t>::max)()) {
      m_pending.overflowed = true;
      return false;
    }
    ++m_callbackSequence;
    return true;
  }

  void append(Kind kind) noexcept {
    if (m_pending.count == m_pending.events.size()) {
      m_pending.overflowed = true;
      return;
    }
    m_pending.events[m_pending.count++] = {kind, m_callbackSequence};
  }

  std::uint64_t m_callbackSequence{0u};
  Batch m_pending{};
};

} // namespace demo::editor::ui
