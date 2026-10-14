#include <mgif/scene_editor/SceneEditor.h>

#include <algorithm>
#include <optional>
#include <string>
#include <utility>

namespace demo::editor {

SceneEditor::SceneEditor(GltfDocument &document, EditorLimits limits,
                         EditorFaultInjector *faultInjector) noexcept
    : m_clock(&m_systemClock), m_commands(document, limits, faultInjector),
      m_context{document, m_commands, m_selection},
      m_history(m_context, *m_clock, limits),
      m_continuous(m_context, m_history, faultInjector) {}

SceneEditor::SceneEditor(GltfDocument &document, EditorClock &clock,
                         EditorLimits limits,
                         EditorFaultInjector *faultInjector) noexcept
    : m_clock(&clock), m_commands(document, limits, faultInjector),
      m_context{document, m_commands, m_selection},
      m_history(m_context, *m_clock, limits),
      m_continuous(m_context, m_history, faultInjector) {}

SceneCommands &SceneEditor::commands() noexcept { return m_commands; }

const SceneCommands &SceneEditor::commands() const noexcept {
  return m_commands;
}

EditorSelectionState &SceneEditor::selection() noexcept { return m_selection; }

const EditorSelectionState &SceneEditor::selection() const noexcept {
  return m_selection;
}

void SceneEditor::bindSelectionAuthority(
    ILegacySelectionAuthority &authority) noexcept {
  m_selection.bindAuthority(authority);
}

void SceneEditor::unbindSelectionAuthority() noexcept {
  m_selection.unbindAuthority();
}

EditorHistory &SceneEditor::history() noexcept { return m_history; }

const EditorHistory &SceneEditor::history() const noexcept { return m_history; }

ContinuousEditSession &SceneEditor::continuousEdit() noexcept {
  return m_continuous;
}

const ContinuousEditSession &SceneEditor::continuousEdit() const noexcept {
  return m_continuous;
}

EditorCommandContext &SceneEditor::commandContext() noexcept {
  return m_context;
}

const EditorCommandContext &SceneEditor::commandContext() const noexcept {
  return m_context;
}

MutationToken SceneEditor::token() const noexcept { return m_commands.token(); }

EditorMutationResult SceneEditor::addRootNode(SceneId scene, std::string name,
                                              const MutationGuard &guard) {
  EditorMutationResult result;
  result.mutation =
      m_commands.addNode(scene, std::move(name), std::nullopt, guard);
  if (!result.mutation.applied())
    return result;

  const auto added = std::find_if(
      result.mutation.remap.nodes.begin(), result.mutation.remap.nodes.end(),
      [](const MutationRemapEntry &entry) {
        return !entry.oldId.valid() &&
               entry.newId.kind == EditorObjectKind::node &&
               entry.newId.valid();
      });
  std::optional<NodeId> node;
  if (added != result.mutation.remap.nodes.end())
    node = NodeId::from(added->newId);
  if (!node) {
    for (const EditorObjectId changed : result.mutation.changedIds) {
      const auto candidate = NodeId::from(changed);
      if (candidate && m_context.document.hasNodeOccurrence(scene, *candidate,
                                                            std::nullopt)) {
        node = candidate;
        break;
      }
    }
  }
  if (!node ||
      !m_context.document.hasNodeOccurrence(scene, *node, std::nullopt)) {
    m_selection.clear();
    result.selectionStatus = SelectionRestoreStatus::clearedStale;
    return result;
  }
  EditorSelectionSnapshot selected;
  selected.authority = m_context.document.documentAuthority();
  selected.value = NodeSelection{scene, *node, std::nullopt};
  result.selectionStatus = m_selection.restore(m_context.document, selected);
  return result;
}

} // namespace demo::editor
