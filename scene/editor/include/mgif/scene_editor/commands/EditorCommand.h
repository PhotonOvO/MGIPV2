#pragma once

#include <mgif/scene_editor/EditorHistoryPolicy.h>
#include <mgif/scene_editor/EditorSelection.h>
#include <mgif/scene_editor/SceneCommands.h>

#include <array>
#include <compare>
#include <cstdint>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <variant>
#include <vector>

namespace demo::editor {

enum class EditorCommandKind : std::uint8_t {
  transform = 0,
  renameNode,
  addChildNode,
  addLight,
  duplicateNode,
  deleteNode,
  reparentNode,
  editMaterial,
  editLight,
};

enum class CommandStatus : std::uint8_t {
  applied = 0,
  historyEnvelopeNoop,
  unchanged,
  rejected,
};

struct CommandMergeKey final {
  EditorCommandKind kind{EditorCommandKind::transform};
  EditorObjectId target{};
  auto operator<=>(const CommandMergeKey &) const = default;
};

struct EditorCommandContext final {
  GltfDocument &document;
  SceneCommands &mutations;
  EditorSelectionState &selection;
};

struct CommandResult final {
  CommandStatus status{CommandStatus::rejected};
  MutationErrorCode error{MutationErrorCode::internalError};
  HistoryPolicy historyPolicy{HistoryPolicy::unclassified};
  std::uint64_t beforeRevision{0};
  std::uint64_t afterRevision{0};
  SelectionRestoreStatus selectionStatus{SelectionRestoreStatus::restored};
  SceneMutationEvidence changes;
  MutationRemap remap;
  MutationToken transactionToken;
  std::optional<FullRebuildReason> fullSnapshotReason;
  std::optional<SceneActivationResult> sceneActivationTransition;
  std::optional<SceneRootStateTransition> rootStateTransition;
  std::string message;

  [[nodiscard]] bool accepted() const noexcept {
    return status == CommandStatus::applied ||
           status == CommandStatus::historyEnvelopeNoop;
  }
};

enum class ExecutedValidationStatus : std::uint8_t {
  matches = 0,
  stale,
  valueMismatch,
  wrongCommandState,
};

struct ExecutedValidation final {
  ExecutedValidationStatus status{ExecutedValidationStatus::wrongCommandState};
  MutationErrorCode error{MutationErrorCode::staleTransaction};
  [[nodiscard]] bool matches() const noexcept {
    return status == ExecutedValidationStatus::matches;
  }
};

enum class CommandLifecycleState : std::uint8_t {
  prepared = 0,
  executed,
  undone,
};

using CommandStateSha256 = std::array<std::uint8_t, 32>;

struct CanonicalPayloadInspection final {
  std::string typeTag;
  std::vector<std::uint8_t> canonicalBytes;
  auto operator<=>(const CanonicalPayloadInspection &) const = default;
};

struct StructuralSideInspection final {
  bool present{false};
  std::vector<std::uint8_t> canonicalBytes;
  auto operator<=>(const StructuralSideInspection &) const = default;
};

struct RetainedCommandInspection final {
  std::uint32_t schemaVersion{1};
  EditorCommandKind kind{EditorCommandKind::transform};
  CommandLifecycleState lifecycle{CommandLifecycleState::prepared};
  std::uint64_t stateRevision{0};
  std::uint64_t retainedSnapshotBytes{0};
  std::optional<EditorObjectId> stableTarget;
  EditorDocumentAuthority authority{};
  MutationToken preparedToken{};
  CanonicalPayloadInspection beforePayload;
  CanonicalPayloadInspection finalPayload;
  StructuralSideInspection undoStructuralSide;
  StructuralSideInspection redoStructuralSide;
  EditorSelectionSnapshot executeSelection;
  EditorSelectionSnapshot undoSelection;
  EditorSelectionSnapshot redoSelection;
  CommandStateSha256 canonicalSha256{};
  auto operator<=>(const RetainedCommandInspection &) const = default;
};

class EditorCommand {
public:
  virtual ~EditorCommand() = default;

  [[nodiscard]] virtual CommandResult execute(EditorCommandContext &) = 0;
  [[nodiscard]] virtual CommandResult undo(EditorCommandContext &) = 0;
  [[nodiscard]] virtual CommandResult redo(EditorCommandContext &) = 0;
  [[nodiscard]] virtual HistoryPolicy historyPolicy() const noexcept = 0;
  [[nodiscard]] virtual std::uint64_t snapshotBytes() const noexcept = 0;
  [[nodiscard]] virtual std::string_view description() const noexcept = 0;
  [[nodiscard]] virtual std::optional<CommandMergeKey>
  mergeKey() const noexcept;
  [[nodiscard]] virtual RetainedCommandInspection
  inspectRetainedState() const = 0;

  [[nodiscard]] virtual ExecutedValidation
  validateExecuted(const EditorCommandContext &) const noexcept;
  [[nodiscard]] virtual std::optional<std::uint64_t>
  mergedSnapshotBytes(const EditorCommand &newer) const noexcept;
  [[nodiscard]] virtual bool tryAbsorbNewer(EditorCommand &newer) noexcept;
};

struct CommandPrepareResult final {
  std::unique_ptr<EditorCommand> command;
  MutationErrorCode error{MutationErrorCode::none};
  std::string message;

  [[nodiscard]] bool succeeded() const noexcept { return command != nullptr; }
};

enum class ContinuousValueKind : std::uint8_t {
  nodeTransform = 0,
  material,
  light,
};

using ContinuousCommandValue =
    std::variant<NodeTransform, MaterialSnapshot, LightSnapshot>;

struct SnapshotByteResult final {
  MutationErrorCode error{MutationErrorCode::none};
  std::uint64_t bytes{0};
  [[nodiscard]] bool succeeded() const noexcept {
    return error == MutationErrorCode::none;
  }
};

class ContinuousEditorCommand : public EditorCommand {
public:
  [[nodiscard]] virtual ContinuousValueKind valueKind() const noexcept = 0;
  [[nodiscard]] virtual SnapshotByteResult prospectiveSnapshotBytes(
      const ContinuousCommandValue &value) const noexcept = 0;
  [[nodiscard]] virtual CommandResult
  applyPreview(EditorCommandContext &context,
               const ContinuousCommandValue &value) = 0;
  [[nodiscard]] virtual bool initialEqualsFinal() const noexcept = 0;
};

struct ContinuousCommandPrepareResult final {
  std::unique_ptr<ContinuousEditorCommand> command;
  MutationErrorCode error{MutationErrorCode::none};
  std::string message;

  [[nodiscard]] bool succeeded() const noexcept { return command != nullptr; }
};

namespace detail {

[[nodiscard]] CommandStateSha256 retainedCommandInspectionSha256(
    const RetainedCommandInspection &inspection) noexcept;
void finalizeRetainedCommandInspection(
    RetainedCommandInspection &inspection) noexcept;
[[nodiscard]] bool retainedCommandInspectionHashMatches(
    const RetainedCommandInspection &inspection) noexcept;

} // namespace detail

} // namespace demo::editor
