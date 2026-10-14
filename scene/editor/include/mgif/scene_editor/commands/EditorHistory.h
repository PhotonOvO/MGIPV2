#pragma once

#include <mgif/scene_editor/EditorClock.h>
#include <mgif/scene_editor/EditorLimits.h>
#include <mgif/scene_editor/commands/EditorCommand.h>

#include <array>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <optional>
#include <string_view>

namespace demo::editor {

enum class HistoryStatus : std::uint8_t {
  pushed = 0,
  merged,
  moved,
  empty,
  noCommand,
  rejected,
};

struct HistoryResult final {
  HistoryStatus status{HistoryStatus::rejected};
  CommandResult command;

  [[nodiscard]] bool succeeded() const noexcept {
    return status == HistoryStatus::pushed || status == HistoryStatus::merged ||
           status == HistoryStatus::moved;
  }
};

struct HistoryAdmission final {
  bool admitted{false};
  MutationErrorCode error{MutationErrorCode::snapshotBudgetExceeded};
  std::uint64_t prospectiveBytes{0};
};

class EditorHistory final {
public:
  static constexpr std::size_t kCapacity = 100;
  static constexpr auto kMergeWindow = std::chrono::milliseconds{500};

  EditorHistory(EditorCommandContext &context, EditorClock &clock,
                EditorLimits limits = kDefaultEditorLimits) noexcept;

  [[nodiscard]] HistoryResult execute(std::unique_ptr<EditorCommand> command);
  [[nodiscard]] HistoryResult
  commitExecuted(std::unique_ptr<EditorCommand> command);
  [[nodiscard]] HistoryResult undo();
  [[nodiscard]] HistoryResult redo();

  [[nodiscard]] HistoryAdmission
  admitProspectivePush(std::uint64_t snapshotBytes) const noexcept;

  void clear() noexcept;
  [[nodiscard]] bool canUndo() const noexcept;
  [[nodiscard]] bool canRedo() const noexcept;
  [[nodiscard]] std::size_t undoCount() const noexcept;
  [[nodiscard]] std::size_t redoCount() const noexcept;
  [[nodiscard]] std::uint64_t retainedSnapshotBytes() const noexcept;
  [[nodiscard]] std::string_view undoDescription() const noexcept;
  [[nodiscard]] std::string_view redoDescription() const noexcept;

private:
  struct FixedCommandStack final {
    std::array<std::unique_ptr<EditorCommand>, kCapacity> entries{};
    std::size_t size{0};
    std::uint64_t snapshotBytes{0};
  };

  static void clearStack(FixedCommandStack &stack) noexcept;
  static void
  pushWithCountEviction(FixedCommandStack &stack,
                        std::unique_ptr<EditorCommand> command) noexcept;
  static void moveTop(FixedCommandStack &source,
                      FixedCommandStack &destination) noexcept;

  EditorCommandContext &m_context;
  EditorClock &m_clock;
  EditorLimits m_limits;
  FixedCommandStack m_undo;
  FixedCommandStack m_redo;
  std::optional<EditorTimePoint> m_lastContinuousPush;

  friend class EditorStateInspector;
};

} // namespace demo::editor
