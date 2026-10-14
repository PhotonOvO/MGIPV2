#include <mgif/scene_editor/commands/EditorHistory.h>

#include <mgif/scene_editor/testing/EditorStateInspector.h>

#include <cassert>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <memory>
#include <new>
#include <stdexcept>
#include <string_view>
#include <utility>

namespace demo::editor {
namespace {

[[nodiscard]] CommandResult
resultWithoutCallback(const EditorCommandContext &context,
                      MutationErrorCode error, HistoryPolicy policy) noexcept {
  CommandResult result;
  result.status = CommandStatus::rejected;
  result.error = error;
  result.historyPolicy = policy;
  result.beforeRevision = context.document.revision();
  result.afterRevision = context.document.revision();
  return result;
}

[[nodiscard]] HistoryResult noCommandResult(const EditorCommandContext &context,
                                            HistoryStatus status) noexcept {
  HistoryResult result;
  result.status = status;
  result.command = resultWithoutCallback(context, MutationErrorCode::none,
                                         HistoryPolicy::notApplicable);
  return result;
}

[[nodiscard]] CommandResult invokeCommand(
    EditorCommand &command, EditorCommandContext &context,
    CommandResult (EditorCommand::*callback)(EditorCommandContext &)) noexcept {
  try {
    return (command.*callback)(context);
  } catch (const std::bad_alloc &) {
    return resultWithoutCallback(context, MutationErrorCode::allocationFailed,
                                 command.historyPolicy());
  } catch (...) {
    return resultWithoutCallback(context, MutationErrorCode::internalError,
                                 command.historyPolicy());
  }
}

[[nodiscard]] bool callbackIsAcceptedUndoable(CommandResult &result) noexcept {
  if (!result.accepted())
    return false;
  assert(result.historyPolicy == HistoryPolicy::undoable);
  if (result.historyPolicy != HistoryPolicy::undoable) {
    // The callback has already published. Keep it tracked in release builds
    // rather than creating an untracked mutation from a command-contract bug.
    result.historyPolicy = HistoryPolicy::undoable;
    result.error = MutationErrorCode::internalError;
  }
  return true;
}

[[nodiscard]] HistoryEntryInspection
inspectEntry(const EditorCommand &command) {
  HistoryEntryInspection result;
  result.description = command.description();
  result.snapshotBytes = command.snapshotBytes();
  result.retainedCommand = command.inspectRetainedState();
  if (result.retainedCommand.retainedSnapshotBytes != result.snapshotBytes)
    throw std::logic_error("retained command byte inspection mismatch");
  if (!detail::retainedCommandInspectionHashMatches(result.retainedCommand))
    throw std::logic_error("retained command canonical SHA-256 mismatch");
  return result;
}

} // namespace

EditorHistory::EditorHistory(EditorCommandContext &context, EditorClock &clock,
                             EditorLimits limits) noexcept
    : m_context(context), m_clock(clock), m_limits(limits) {}

HistoryAdmission EditorHistory::admitProspectivePush(
    std::uint64_t snapshotBytes) const noexcept {
  HistoryAdmission result;
  if (snapshotBytes > m_limits.maxHistoryEntrySnapshotBytes)
    return result;

  std::uint64_t retained = m_undo.snapshotBytes;
  if (m_undo.size == kCapacity) {
    assert(m_undo.entries[0] != nullptr);
    const std::uint64_t evicted = m_undo.entries[0]->snapshotBytes();
    if (evicted > retained)
      return result;
    retained -= evicted;
  }
  if (!editorCheckedAdd(retained, snapshotBytes, result.prospectiveBytes))
    return result;
  if (result.prospectiveBytes > m_limits.maxHistoryAggregateSnapshotBytes)
    return result;
  result.admitted = true;
  result.error = MutationErrorCode::none;
  return result;
}

HistoryResult EditorHistory::execute(std::unique_ptr<EditorCommand> command) {
  if (!command)
    return noCommandResult(m_context, HistoryStatus::noCommand);
  const HistoryPolicy policy = command->historyPolicy();
  if (policy != HistoryPolicy::undoable) {
    return HistoryResult{HistoryStatus::rejected,
                         resultWithoutCallback(m_context,
                                               MutationErrorCode::internalError,
                                               policy)};
  }
  const std::uint64_t admittedBytes = command->snapshotBytes();
  const HistoryAdmission admission = admitProspectivePush(admittedBytes);
  if (!admission.admitted) {
    return HistoryResult{
        HistoryStatus::rejected,
        resultWithoutCallback(m_context, admission.error, policy)};
  }

  CommandResult commandResult =
      invokeCommand(*command, m_context, &EditorCommand::execute);
  if (!callbackIsAcceptedUndoable(commandResult))
    return HistoryResult{HistoryStatus::rejected, std::move(commandResult)};
  assert(command->snapshotBytes() == admittedBytes);

  clearStack(m_redo);
  pushWithCountEviction(m_undo, std::move(command));
  return HistoryResult{HistoryStatus::pushed, std::move(commandResult)};
}

HistoryResult
EditorHistory::commitExecuted(std::unique_ptr<EditorCommand> command) {
  if (!command)
    return noCommandResult(m_context, HistoryStatus::noCommand);
  const HistoryPolicy policy = command->historyPolicy();
  if (policy != HistoryPolicy::undoable) {
    return HistoryResult{HistoryStatus::rejected,
                         resultWithoutCallback(m_context,
                                               MutationErrorCode::internalError,
                                               policy)};
  }
  const std::uint64_t splitBytes = command->snapshotBytes();
  const HistoryAdmission splitAdmission = admitProspectivePush(splitBytes);
  if (!splitAdmission.admitted) {
    return HistoryResult{
        HistoryStatus::rejected,
        resultWithoutCallback(m_context, splitAdmission.error, policy)};
  }

  const ExecutedValidation validation = command->validateExecuted(m_context);
  if (!validation.matches()) {
    return HistoryResult{
        HistoryStatus::rejected,
        resultWithoutCallback(m_context, validation.error, policy)};
  }

  const EditorTimePoint now = m_clock.now();
  bool mergeCandidate = false;
  if (m_undo.size != 0 && m_lastContinuousPush &&
      now >= *m_lastContinuousPush &&
      now - *m_lastContinuousPush < kMergeWindow) {
    const auto olderKey = m_undo.entries[m_undo.size - 1u]->mergeKey();
    const auto newerKey = command->mergeKey();
    mergeCandidate = olderKey && newerKey && *olderKey == *newerKey;
  }

  if (mergeCandidate) {
    EditorCommand &older = *m_undo.entries[m_undo.size - 1u];
    const std::uint64_t olderBytes = older.snapshotBytes();
    const std::optional<std::uint64_t> mergedBytes =
        older.mergedSnapshotBytes(*command);
    bool mergeAdmitted =
        mergedBytes && *mergedBytes <= m_limits.maxHistoryEntrySnapshotBytes &&
        olderBytes <= m_undo.snapshotBytes;
    std::uint64_t prospectiveMerged = 0;
    if (mergeAdmitted) {
      mergeAdmitted =
          editorCheckedAdd(m_undo.snapshotBytes - olderBytes, *mergedBytes,
                           prospectiveMerged) &&
          prospectiveMerged <= m_limits.maxHistoryAggregateSnapshotBytes;
    }
    if (mergeAdmitted && older.tryAbsorbNewer(*command)) {
      const std::uint64_t actualBytes = older.snapshotBytes();
      assert(actualBytes == *mergedBytes);
      assert(actualBytes <= m_limits.maxHistoryEntrySnapshotBytes);
      std::uint64_t actualTotal = 0;
      const bool totalValid = editorCheckedAdd(
          m_undo.snapshotBytes - olderBytes, actualBytes, actualTotal);
      assert(totalValid &&
             actualTotal <= m_limits.maxHistoryAggregateSnapshotBytes);
      if (totalValid)
        m_undo.snapshotBytes = actualTotal;
      clearStack(m_redo);
      m_lastContinuousPush = now;
      CommandResult commandResult = resultWithoutCallback(
          m_context, MutationErrorCode::none, HistoryPolicy::undoable);
      commandResult.status = CommandStatus::applied;
      return HistoryResult{HistoryStatus::merged, std::move(commandResult)};
    }
  }

  assert(command->snapshotBytes() == splitBytes);
  clearStack(m_redo);
  pushWithCountEviction(m_undo, std::move(command));
  m_lastContinuousPush = now;
  CommandResult commandResult = resultWithoutCallback(
      m_context, MutationErrorCode::none, HistoryPolicy::undoable);
  commandResult.status = CommandStatus::applied;
  return HistoryResult{HistoryStatus::pushed, std::move(commandResult)};
}

HistoryResult EditorHistory::undo() {
  if (m_undo.size == 0)
    return noCommandResult(m_context, HistoryStatus::empty);
  EditorCommand &command = *m_undo.entries[m_undo.size - 1u];
#ifndef NDEBUG
  const std::uint64_t retainedBytes = command.snapshotBytes();
#endif
  CommandResult commandResult =
      invokeCommand(command, m_context, &EditorCommand::undo);
  if (!callbackIsAcceptedUndoable(commandResult))
    return HistoryResult{HistoryStatus::rejected, std::move(commandResult)};
  assert(command.snapshotBytes() == retainedBytes);
  moveTop(m_undo, m_redo);
  return HistoryResult{HistoryStatus::moved, std::move(commandResult)};
}

HistoryResult EditorHistory::redo() {
  if (m_redo.size == 0)
    return noCommandResult(m_context, HistoryStatus::empty);
  EditorCommand &command = *m_redo.entries[m_redo.size - 1u];
#ifndef NDEBUG
  const std::uint64_t retainedBytes = command.snapshotBytes();
#endif
  CommandResult commandResult =
      invokeCommand(command, m_context, &EditorCommand::redo);
  if (!callbackIsAcceptedUndoable(commandResult))
    return HistoryResult{HistoryStatus::rejected, std::move(commandResult)};
  assert(command.snapshotBytes() == retainedBytes);
  moveTop(m_redo, m_undo);
  return HistoryResult{HistoryStatus::moved, std::move(commandResult)};
}

void EditorHistory::clear() noexcept {
  clearStack(m_undo);
  clearStack(m_redo);
}

bool EditorHistory::canUndo() const noexcept { return m_undo.size != 0; }

bool EditorHistory::canRedo() const noexcept { return m_redo.size != 0; }

std::size_t EditorHistory::undoCount() const noexcept { return m_undo.size; }

std::size_t EditorHistory::redoCount() const noexcept { return m_redo.size; }

std::uint64_t EditorHistory::retainedSnapshotBytes() const noexcept {
  std::uint64_t result = 0;
  const bool valid =
      editorCheckedAdd(m_undo.snapshotBytes, m_redo.snapshotBytes, result);
  assert(valid);
  return valid ? result : std::numeric_limits<std::uint64_t>::max();
}

std::string_view EditorHistory::undoDescription() const noexcept {
  return m_undo.size == 0 ? std::string_view{}
                          : m_undo.entries[m_undo.size - 1u]->description();
}

std::string_view EditorHistory::redoDescription() const noexcept {
  return m_redo.size == 0 ? std::string_view{}
                          : m_redo.entries[m_redo.size - 1u]->description();
}

void EditorHistory::clearStack(FixedCommandStack &stack) noexcept {
  for (std::size_t index = 0; index < stack.size; ++index)
    stack.entries[index].reset();
  stack.size = 0;
  stack.snapshotBytes = 0;
}

void EditorHistory::pushWithCountEviction(
    FixedCommandStack &stack, std::unique_ptr<EditorCommand> command) noexcept {
  assert(command != nullptr);
  if (stack.size == kCapacity) {
    const std::uint64_t evictedBytes = stack.entries[0]->snapshotBytes();
    assert(evictedBytes <= stack.snapshotBytes);
    stack.entries[0].reset();
    for (std::size_t index = 1; index < stack.size; ++index)
      stack.entries[index - 1u] = std::move(stack.entries[index]);
    --stack.size;
    stack.snapshotBytes -= evictedBytes;
  }
  const std::uint64_t commandBytes = command->snapshotBytes();
  std::uint64_t updatedBytes = 0;
  const bool valid =
      editorCheckedAdd(stack.snapshotBytes, commandBytes, updatedBytes);
  assert(valid);
  stack.entries[stack.size] = std::move(command);
  ++stack.size;
  if (valid)
    stack.snapshotBytes = updatedBytes;
}

void EditorHistory::moveTop(FixedCommandStack &source,
                            FixedCommandStack &destination) noexcept {
  assert(source.size != 0);
  assert(destination.size < kCapacity);
  const std::uint64_t bytes = source.entries[source.size - 1u]->snapshotBytes();
  assert(bytes <= source.snapshotBytes);
  std::uint64_t destinationBytes = 0;
  const bool valid =
      editorCheckedAdd(destination.snapshotBytes, bytes, destinationBytes);
  assert(valid);
  destination.entries[destination.size] =
      std::move(source.entries[source.size - 1u]);
  ++destination.size;
  --source.size;
  source.snapshotBytes -= bytes;
  if (valid)
    destination.snapshotBytes = destinationBytes;
}

HistoryInspection EditorStateInspector::inspect(const EditorHistory &history) {
  HistoryInspection result;
  result.undoOldestToNewest.reserve(history.m_undo.size);
  for (std::size_t index = 0; index < history.m_undo.size; ++index)
    result.undoOldestToNewest.push_back(
        inspectEntry(*history.m_undo.entries[index]));

  result.redoOldestToNewest.reserve(history.m_redo.size);
  for (std::size_t index = history.m_redo.size; index != 0; --index)
    result.redoOldestToNewest.push_back(
        inspectEntry(*history.m_redo.entries[index - 1u]));

  result.lastContinuousPush = history.m_lastContinuousPush;
  result.retainedSnapshotBytes = history.retainedSnapshotBytes();
  return result;
}

} // namespace demo::editor
