#include <mgif/scene_editor/commands/HistoryLifecycle.h>

#include <mgif/scene_editor/commands/EditorHistory.h>

namespace demo::editor {

void applyHistoryLifecycle(EditorHistory &history, HistoryLifecycleEvent event,
                           LifecycleCommitState state) noexcept {
  if (state != LifecycleCommitState::changed)
    return;
  if (event == HistoryLifecycleEvent::merge ||
      event == HistoryLifecycleEvent::compact) {
    history.clear();
  }
}

} // namespace demo::editor
