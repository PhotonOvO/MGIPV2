#pragma once

#include <mgif/scene_editor/EditorClock.h>
#include <mgif/scene_editor/EditorFaultInjection.h>
#include <mgif/scene_editor/EditorLimits.h>
#include <mgif/scene_editor/EditorSelection.h>
#include <mgif/scene_editor/SceneCommands.h>
#include <mgif/scene_editor/commands/ContinuousEdit.h>
#include <mgif/scene_editor/commands/EditorHistory.h>

#include <string>

namespace demo::editor {

struct EditorMutationResult final {
  MutationResult mutation;
  SelectionRestoreStatus selectionStatus{SelectionRestoreStatus::restored};
};

class SceneEditor final {
public:
  explicit SceneEditor(GltfDocument &document,
                       EditorLimits limits = kDefaultEditorLimits,
                       EditorFaultInjector *faultInjector = nullptr) noexcept;

  SceneEditor(GltfDocument &document, EditorClock &clock,
              EditorLimits limits = kDefaultEditorLimits,
              EditorFaultInjector *faultInjector = nullptr) noexcept;

  SceneEditor(const SceneEditor &) = delete;
  SceneEditor &operator=(const SceneEditor &) = delete;
  SceneEditor(SceneEditor &&) = delete;
  SceneEditor &operator=(SceneEditor &&) = delete;

  [[nodiscard]] SceneCommands &commands() noexcept;
  [[nodiscard]] const SceneCommands &commands() const noexcept;
  [[nodiscard]] EditorSelectionState &selection() noexcept;
  [[nodiscard]] const EditorSelectionState &selection() const noexcept;
  void bindSelectionAuthority(ILegacySelectionAuthority &authority) noexcept;
  void unbindSelectionAuthority() noexcept;
  [[nodiscard]] EditorHistory &history() noexcept;
  [[nodiscard]] const EditorHistory &history() const noexcept;
  [[nodiscard]] ContinuousEditSession &continuousEdit() noexcept;
  [[nodiscard]] const ContinuousEditSession &continuousEdit() const noexcept;
  // Desktop editor adapters use the same command context as the history and
  // continuous-edit machinery. Mutations still go through SceneCommands or an
  // EditorCommand submitted to EditorHistory.
  [[nodiscard]] EditorCommandContext &commandContext() noexcept;
  [[nodiscard]] const EditorCommandContext &commandContext() const noexcept;
  [[nodiscard]] MutationToken token() const noexcept;
  [[nodiscard]] EditorMutationResult
  addRootNode(SceneId scene, std::string name = {},
              const MutationGuard &guard = {});

private:
  SystemEditorClock m_systemClock;
  EditorClock *m_clock;
  SceneCommands m_commands;
  EditorSelectionState m_selection;
  EditorCommandContext m_context;
  EditorHistory m_history;
  ContinuousEditSession m_continuous;
};

} // namespace demo::editor
