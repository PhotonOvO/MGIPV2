#pragma once

#include <cstdint>

namespace demo::editor {

class EditorHistory;

enum class HistoryLifecycleEvent : std::uint8_t {
  merge = 0,
  compact,
  saveAs,
};

enum class LifecycleCommitState : std::uint8_t {
  failed = 0,
  noOp,
  changed,
};

void applyHistoryLifecycle(EditorHistory &history, HistoryLifecycleEvent event,
                           LifecycleCommitState state) noexcept;

} // namespace demo::editor
