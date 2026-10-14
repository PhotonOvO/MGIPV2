#include <mgif/scene_editor/commands/TransformCommands.h>
#include <mgif/scene_editor/detail/BitwiseCast.h>

#include <array>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <memory>
#include <new>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <variant>
#include <vector>

namespace demo::editor {
namespace {

constexpr std::uint64_t kStringAllocationCharge =
    static_cast<std::uint64_t>(sizeof(void *) * 2u);

void appendU64(std::vector<std::uint8_t> &bytes, std::uint64_t value) {
  for (unsigned shift = 0; shift < 64u; shift += 8u)
    bytes.push_back(static_cast<std::uint8_t>(value >> shift));
}

void appendBool(std::vector<std::uint8_t> &bytes, bool value) {
  bytes.push_back(value ? 1u : 0u);
}

void appendDouble(std::vector<std::uint8_t> &bytes, double value) {
  appendU64(bytes, detail::bitwiseCast<std::uint64_t>(value));
}

template <std::size_t Size>
void appendDoubles(std::vector<std::uint8_t> &bytes,
                   const std::array<double, Size> &values) {
  appendU64(bytes, static_cast<std::uint64_t>(Size));
  for (const double value : values)
    appendDouble(bytes, value);
}

void appendTransform(std::vector<std::uint8_t> &bytes,
                     const NodeTransform &value) {
  appendDoubles(bytes, value.translation);
  appendDoubles(bytes, value.rotation);
  appendDoubles(bytes, value.scale);
}

void appendString(std::vector<std::uint8_t> &bytes, std::string_view value) {
  appendU64(bytes, static_cast<std::uint64_t>(value.size()));
  bytes.insert(bytes.end(), value.begin(), value.end());
}

[[nodiscard]] CommandResult commandResult(const EditorCommandContext &context,
                                          CommandStatus status,
                                          MutationErrorCode error) noexcept {
  CommandResult result;
  result.status = status;
  result.error = error;
  result.historyPolicy = HistoryPolicy::undoable;
  result.beforeRevision = context.document.revision();
  result.afterRevision = context.document.revision();
  return result;
}

[[nodiscard]] CommandResult commandResult(MutationResult mutation) noexcept {
  CommandResult result;
  result.status = mutation.applied()
                      ? CommandStatus::applied
                      : (mutation.domainNoop() ? CommandStatus::unchanged
                                               : CommandStatus::rejected);
  result.error = mutation.error;
  result.historyPolicy = HistoryPolicy::undoable;
  result.beforeRevision = mutation.beforeRevision;
  result.afterRevision = mutation.afterRevision;
  result.changes = std::move(mutation.changes);
  result.remap = std::move(mutation.remap);
  result.transactionToken = mutation.transactionToken;
  result.fullSnapshotReason = mutation.fullSnapshotReason;
  result.sceneActivationTransition = mutation.sceneActivationTransition;
  result.rootStateTransition = mutation.rootStateTransition;
  result.message = std::move(mutation.message);
  return result;
}

[[nodiscard]] ExecutedValidation
executedValidation(const PreparedValueValidation &value) noexcept {
  if (value.matches()) {
    return ExecutedValidation{ExecutedValidationStatus::matches,
                              MutationErrorCode::none};
  }
  return ExecutedValidation{value.status == PreparedValueStatus::valueMismatch
                                ? ExecutedValidationStatus::valueMismatch
                                : ExecutedValidationStatus::stale,
                            value.error};
}

class TransformCommand final : public ContinuousEditorCommand {
public:
  TransformCommand(NodeId node, NodeTransform initial, NodeTransform final,
                   MutationToken token, EditorDocumentAuthority authority,
                   EditorSelectionSnapshot selection, std::string description,
                   EditorFaultInjector *faultInjector) noexcept
      : m_node(node), m_initial(initial), m_final(final), m_token(token),
        m_authority(authority), m_executeSelection(selection),
        m_undoSelection(selection), m_redoSelection(selection),
        m_description(std::move(description)), m_faultInjector(faultInjector) {
    std::uint64_t bytes = sizeof(TransformCommand);
    std::uint64_t descriptionBytes =
        static_cast<std::uint64_t>(m_description.capacity());
    if (editorCheckedAdd(descriptionBytes, 1u, descriptionBytes) &&
        editorCheckedAdd(descriptionBytes, kStringAllocationCharge,
                         descriptionBytes) &&
        editorCheckedAdd(bytes, descriptionBytes, m_snapshotBytes))
      m_accountingValid = true;
  }

  [[nodiscard]] bool accountingValid() const noexcept {
    return m_accountingValid;
  }

  [[nodiscard]] CommandResult execute(EditorCommandContext &context) override {
    if (m_lifecycle != CommandLifecycleState::prepared) {
      return commandResult(context, CommandStatus::rejected,
                           MutationErrorCode::staleTransaction);
    }
    if (!canAdvanceState()) {
      return commandResult(context, CommandStatus::rejected,
                           MutationErrorCode::internalError);
    }
    const PreparedValueValidation validation =
        context.mutations.validateNodeTransform(m_node, m_initial, m_token);
    if (!validation.matches()) {
      return commandResult(context, CommandStatus::rejected, validation.error);
    }
    if (m_initial == m_final) {
      return commandResult(context, CommandStatus::unchanged,
                           MutationErrorCode::noOp);
    }

    MutationGuard guard;
    guard.expectedToken = m_token;
    MutationResult mutation =
        context.mutations.setNodeTransform(m_node, m_final, guard);
    if (mutation.applied()) {
      m_token = mutation.transactionToken;
      m_lifecycle = CommandLifecycleState::executed;
      ++m_stateRevision;
    }
    return commandResult(std::move(mutation));
  }

  [[nodiscard]] CommandResult undo(EditorCommandContext &context) override {
    if (m_lifecycle != CommandLifecycleState::executed) {
      return commandResult(context, CommandStatus::rejected,
                           MutationErrorCode::staleTransaction);
    }
    if (!canAdvanceState()) {
      return commandResult(context, CommandStatus::rejected,
                           MutationErrorCode::internalError);
    }
    const PreparedValueValidation validation =
        context.mutations.validateNodeTransform(m_node, m_final, m_token);
    if (!validation.matches()) {
      return commandResult(context, CommandStatus::rejected, validation.error);
    }

    MutationGuard guard;
    guard.expectedToken = m_token;
    MutationResult mutation =
        context.mutations.setNodeTransform(m_node, m_initial, guard);
    if (mutation.applied()) {
      m_token = mutation.transactionToken;
      m_lifecycle = CommandLifecycleState::undone;
      ++m_stateRevision;
    }
    return commandResult(std::move(mutation));
  }

  [[nodiscard]] CommandResult redo(EditorCommandContext &context) override {
    if (m_lifecycle != CommandLifecycleState::undone) {
      return commandResult(context, CommandStatus::rejected,
                           MutationErrorCode::staleTransaction);
    }
    if (!canAdvanceState()) {
      return commandResult(context, CommandStatus::rejected,
                           MutationErrorCode::internalError);
    }
    const PreparedValueValidation validation =
        context.mutations.validateNodeTransform(m_node, m_initial, m_token);
    if (!validation.matches()) {
      return commandResult(context, CommandStatus::rejected, validation.error);
    }

    MutationGuard guard;
    guard.expectedToken = m_token;
    MutationResult mutation =
        context.mutations.setNodeTransform(m_node, m_final, guard);
    if (mutation.applied()) {
      m_token = mutation.transactionToken;
      m_lifecycle = CommandLifecycleState::executed;
      ++m_stateRevision;
    }
    return commandResult(std::move(mutation));
  }

  [[nodiscard]] HistoryPolicy historyPolicy() const noexcept override {
    return HistoryPolicy::undoable;
  }

  [[nodiscard]] std::uint64_t snapshotBytes() const noexcept override {
    return m_snapshotBytes;
  }

  [[nodiscard]] std::string_view description() const noexcept override {
    return m_description;
  }

  [[nodiscard]] std::optional<CommandMergeKey>
  mergeKey() const noexcept override {
    return CommandMergeKey{EditorCommandKind::transform, m_node.untyped()};
  }

  [[nodiscard]] RetainedCommandInspection
  inspectRetainedState() const override {
    RetainedCommandInspection result;
    result.kind = EditorCommandKind::transform;
    result.lifecycle = m_lifecycle;
    result.stateRevision = m_stateRevision;
    result.retainedSnapshotBytes = m_snapshotBytes;
    result.stableTarget = m_node.untyped();
    result.authority = m_authority;
    result.preparedToken = m_token;
    result.beforePayload.typeTag = "node-transform.v1";
    appendTransform(result.beforePayload.canonicalBytes, m_initial);
    appendString(result.beforePayload.canonicalBytes, m_description);
    appendU64(result.beforePayload.canonicalBytes,
              static_cast<std::uint64_t>(m_description.capacity()));
    appendBool(result.beforePayload.canonicalBytes, m_accountingValid);
    result.finalPayload.typeTag = "node-transform.v1";
    appendTransform(result.finalPayload.canonicalBytes, m_final);
    result.executeSelection = m_executeSelection;
    result.undoSelection = m_undoSelection;
    result.redoSelection = m_redoSelection;
    detail::finalizeRetainedCommandInspection(result);
    return result;
  }

  [[nodiscard]] ExecutedValidation validateExecuted(
      const EditorCommandContext &context) const noexcept override {
    if (m_lifecycle != CommandLifecycleState::executed)
      return {};
    return executedValidation(
        context.mutations.validateNodeTransform(m_node, m_final, m_token));
  }

  [[nodiscard]] std::optional<std::uint64_t>
  mergedSnapshotBytes(const EditorCommand &newer) const noexcept override {
    const auto *typed = dynamic_cast<const TransformCommand *>(&newer);
    return compatibleWith(typed) ? std::optional<std::uint64_t>{m_snapshotBytes}
                                 : std::nullopt;
  }

  [[nodiscard]] bool tryAbsorbNewer(EditorCommand &newer) noexcept override {
    auto *typed = dynamic_cast<TransformCommand *>(&newer);
    if (!compatibleWith(typed) || !canAdvanceState())
      return false;
    if (m_faultInjector != nullptr &&
        m_faultInjector->shouldFail(EditorFaultPoint::allocation)) {
      return false;
    }

    m_final = typed->m_final;
    m_token = typed->m_token;
    ++m_stateRevision;
    return true;
  }

  [[nodiscard]] ContinuousValueKind valueKind() const noexcept override {
    return ContinuousValueKind::nodeTransform;
  }

  [[nodiscard]] SnapshotByteResult prospectiveSnapshotBytes(
      const ContinuousCommandValue &value) const noexcept override {
    if (!std::holds_alternative<NodeTransform>(value)) {
      return SnapshotByteResult{MutationErrorCode::invalidSnapshot, 0};
    }
    if (!m_accountingValid) {
      return SnapshotByteResult{MutationErrorCode::snapshotBudgetExceeded, 0};
    }
    return SnapshotByteResult{MutationErrorCode::none, m_snapshotBytes};
  }

  [[nodiscard]] CommandResult
  applyPreview(EditorCommandContext &context,
               const ContinuousCommandValue &value) override {
    const auto *transform = std::get_if<NodeTransform>(&value);
    if (transform == nullptr) {
      return commandResult(context, CommandStatus::rejected,
                           MutationErrorCode::invalidSnapshot);
    }
    if (m_lifecycle != CommandLifecycleState::prepared &&
        m_lifecycle != CommandLifecycleState::executed) {
      return commandResult(context, CommandStatus::rejected,
                           MutationErrorCode::staleTransaction);
    }
    if (!canAdvanceState()) {
      return commandResult(context, CommandStatus::rejected,
                           MutationErrorCode::internalError);
    }
    const PreparedValueValidation validation =
        context.mutations.validateNodeTransform(m_node, m_final, m_token);
    if (!validation.matches()) {
      return commandResult(context, CommandStatus::rejected, validation.error);
    }
    if (*transform == m_final) {
      return commandResult(context, CommandStatus::unchanged,
                           MutationErrorCode::noOp);
    }

    MutationGuard guard;
    guard.expectedToken = m_token;
    MutationResult mutation =
        context.mutations.setNodeTransform(m_node, *transform, guard);
    if (mutation.applied()) {
      m_final = *transform;
      m_token = mutation.transactionToken;
      m_lifecycle = CommandLifecycleState::executed;
      ++m_stateRevision;
    }
    return commandResult(std::move(mutation));
  }

  [[nodiscard]] bool initialEqualsFinal() const noexcept override {
    return m_initial == m_final;
  }

private:
  [[nodiscard]] bool
  compatibleWith(const TransformCommand *newer) const noexcept {
    return newer != nullptr && m_lifecycle == CommandLifecycleState::executed &&
           newer->m_lifecycle == CommandLifecycleState::executed &&
           m_node == newer->m_node && m_authority == newer->m_authority &&
           m_final == newer->m_initial && m_initial != newer->m_final &&
           m_accountingValid && newer->m_accountingValid;
  }

  [[nodiscard]] bool canAdvanceState() const noexcept {
    return m_stateRevision != std::numeric_limits<std::uint64_t>::max();
  }

  NodeId m_node;
  NodeTransform m_initial;
  NodeTransform m_final;
  MutationToken m_token{};
  EditorDocumentAuthority m_authority{};
  EditorSelectionSnapshot m_executeSelection;
  EditorSelectionSnapshot m_undoSelection;
  EditorSelectionSnapshot m_redoSelection;
  std::string m_description;
  EditorFaultInjector *m_faultInjector{nullptr};
  CommandLifecycleState m_lifecycle{CommandLifecycleState::prepared};
  std::uint64_t m_stateRevision{0};
  std::uint64_t m_snapshotBytes{0};
  bool m_accountingValid{false};
};

[[nodiscard]] MutationErrorCode targetError(const EditorCommandContext &context,
                                            NodeId node) noexcept {
  if (node.authority() != context.document.documentAuthority())
    return MutationErrorCode::crossDocument;
  if (!context.document.contains(node.untyped()))
    return MutationErrorCode::staleId;
  return MutationErrorCode::none;
}

[[nodiscard]] std::string
transformDescription(const EditorCommandContext &context, NodeId node) {
  std::string result{"Transform '"};
  result.append(context.document.objectName(node.untyped()));
  result.push_back('\'');
  return result;
}

[[nodiscard]] std::unique_ptr<TransformCommand>
makeTransformCommand(EditorCommandContext &context, NodeId node,
                     NodeTransform initial, NodeTransform final,
                     EditorFaultInjector *faultInjector) {
  return std::make_unique<TransformCommand>(
      node, initial, final, context.mutations.token(),
      context.document.documentAuthority(), context.selection.snapshot(),
      transformDescription(context, node), faultInjector);
}

} // namespace

CommandPrepareResult
prepareTransformCommand(EditorCommandContext &context, NodeId node,
                        NodeTransform before, NodeTransform after,
                        EditorFaultInjector *faultInjector) noexcept {
  CommandPrepareResult result;
  const MutationErrorCode error = targetError(context, node);
  if (error != MutationErrorCode::none) {
    result.error = error;
    return result;
  }
  const std::optional<NodeTransform> current =
      context.mutations.nodeTransform(node);
  if (!current) {
    result.error = MutationErrorCode::staleId;
    return result;
  }
  if (*current != before) {
    result.error = MutationErrorCode::staleTransaction;
    return result;
  }
  try {
    if (faultInjector != nullptr &&
        faultInjector->shouldFail(EditorFaultPoint::allocation)) {
      result.error = MutationErrorCode::allocationFailed;
      return result;
    }
    auto command =
        makeTransformCommand(context, node, before, after, faultInjector);
    if (!command->accountingValid()) {
      result.error = MutationErrorCode::snapshotBudgetExceeded;
      return result;
    }
    result.command = std::move(command);
    return result;
  } catch (const std::bad_alloc &) {
    result.error = MutationErrorCode::allocationFailed;
    return result;
  } catch (...) {
    result.error = MutationErrorCode::internalError;
    return result;
  }
}

ContinuousCommandPrepareResult
prepareContinuousTransformCommand(EditorCommandContext &context, NodeId node,
                                  EditorFaultInjector *faultInjector) noexcept {
  ContinuousCommandPrepareResult result;
  const MutationErrorCode error = targetError(context, node);
  if (error != MutationErrorCode::none) {
    result.error = error;
    return result;
  }
  const std::optional<NodeTransform> initial =
      context.mutations.nodeTransform(node);
  if (!initial) {
    result.error = MutationErrorCode::staleId;
    return result;
  }
  try {
    if (faultInjector != nullptr &&
        faultInjector->shouldFail(EditorFaultPoint::allocation)) {
      result.error = MutationErrorCode::allocationFailed;
      return result;
    }
    auto command =
        makeTransformCommand(context, node, *initial, *initial, faultInjector);
    if (!command->accountingValid()) {
      result.error = MutationErrorCode::snapshotBudgetExceeded;
      return result;
    }
    result.command = std::move(command);
    return result;
  } catch (const std::bad_alloc &) {
    result.error = MutationErrorCode::allocationFailed;
    return result;
  } catch (...) {
    result.error = MutationErrorCode::internalError;
    return result;
  }
}

} // namespace demo::editor
