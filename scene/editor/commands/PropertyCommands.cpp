#include <mgif/scene_editor/commands/PropertyCommands.h>

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

constexpr std::uint64_t kCommandAllocationCharge =
    static_cast<std::uint64_t>(sizeof(void *) * 2u);
constexpr std::uint64_t kStringAllocationCharge =
    static_cast<std::uint64_t>(sizeof(void *) * 2u);
constexpr std::uint64_t kVectorAllocationCharge =
    static_cast<std::uint64_t>(sizeof(void *) * 2u);

void appendU64(std::vector<std::uint8_t> &bytes, std::uint64_t value) {
  for (unsigned shift = 0; shift < 64u; shift += 8u)
    bytes.push_back(static_cast<std::uint8_t>(value >> shift));
}

void appendBool(std::vector<std::uint8_t> &bytes, bool value) {
  bytes.push_back(value ? 1u : 0u);
}

void appendString(std::vector<std::uint8_t> &bytes, std::string_view value) {
  appendU64(bytes, static_cast<std::uint64_t>(value.size()));
  bytes.insert(bytes.end(), value.begin(), value.end());
}

void appendCanonicalRecord(std::vector<std::uint8_t> &bytes,
                           const std::vector<std::uint8_t> &record) {
  appendU64(bytes, static_cast<std::uint64_t>(record.size()));
  appendU64(bytes, static_cast<std::uint64_t>(record.capacity()));
  bytes.insert(bytes.end(), record.begin(), record.end());
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

[[nodiscard]] MutationErrorCode
inspectionError(const SnapshotCanonicalRecord &record) noexcept {
  return record.status == SnapshotInspectionStatus::allocationFailed
             ? MutationErrorCode::allocationFailed
             : MutationErrorCode::invalidSnapshot;
}

template <typename Snapshot> struct PropertyTraits;

template <> struct PropertyTraits<MaterialSnapshot> final {
  using Id = MaterialId;
  static constexpr EditorObjectKind objectKind = EditorObjectKind::material;
  static constexpr EditorCommandKind commandKind =
      EditorCommandKind::editMaterial;
  static constexpr ContinuousValueKind valueKind =
      ContinuousValueKind::material;
  static constexpr std::string_view typeTag = "material-property.v1";
  static constexpr std::string_view descriptionPrefix = "Edit Material '";

  [[nodiscard]] static PreparedValueValidation
  validate(const SceneCommands &commands, Id id, const MaterialSnapshot &value,
           MutationToken token) noexcept {
    return commands.validateMaterialSnapshot(id, value, token);
  }

  [[nodiscard]] static MutationResult set(SceneCommands &commands, Id id,
                                          const MaterialSnapshot &value,
                                          const MutationGuard &guard) {
    return commands.setMaterialSnapshot(id, value, guard);
  }
};

template <> struct PropertyTraits<LightSnapshot> final {
  using Id = LightId;
  static constexpr EditorObjectKind objectKind = EditorObjectKind::light;
  static constexpr EditorCommandKind commandKind = EditorCommandKind::editLight;
  static constexpr ContinuousValueKind valueKind = ContinuousValueKind::light;
  static constexpr std::string_view typeTag = "light-property.v1";
  static constexpr std::string_view descriptionPrefix = "Edit Light '";

  [[nodiscard]] static PreparedValueValidation
  validate(const SceneCommands &commands, Id id, const LightSnapshot &value,
           MutationToken token) noexcept {
    return commands.validateLightSnapshot(id, value, token);
  }

  [[nodiscard]] static MutationResult set(SceneCommands &commands, Id id,
                                          const LightSnapshot &value,
                                          const MutationGuard &guard) {
    return commands.setLightSnapshot(id, value, guard);
  }
};

template <typename Snapshot>
[[nodiscard]] MutationErrorCode
validateSnapshotPair(const EditorCommandContext &context,
                     const Snapshot &before, const Snapshot &after) noexcept {
  using Traits = PropertyTraits<Snapshot>;
  if (!before.valid() || !after.valid())
    return MutationErrorCode::invalidSnapshot;
  if (before.authority() != context.document.documentAuthority() ||
      after.authority() != context.document.documentAuthority())
    return MutationErrorCode::crossDocument;
  if (before.id() != after.id())
    return MutationErrorCode::invalidSnapshot;
  if (!context.document.contains(before.id().untyped()))
    return MutationErrorCode::staleId;
  return Traits::validate(context.mutations, before.id(), before,
                          context.mutations.token())
      .error;
}

template <typename Snapshot>
[[nodiscard]] MutationErrorCode
inspectSnapshot(const Snapshot &snapshot, EditorFaultInjector *faultInjector,
                std::vector<std::uint8_t> &canonicalBytes) noexcept {
  if (faultInjector != nullptr &&
      faultInjector->shouldFail(EditorFaultPoint::allocation)) {
    return MutationErrorCode::allocationFailed;
  }
  SnapshotCanonicalRecord record = snapshot.inspectCanonicalRecord();
  if (!record.complete() ||
      record.schemaVersion != SnapshotCanonicalRecord::kSchemaVersion ||
      record.kind != PropertyTraits<Snapshot>::objectKind) {
    return inspectionError(record);
  }
  canonicalBytes = std::move(record.canonicalBytes);
  return MutationErrorCode::none;
}

template <typename Snapshot>
[[nodiscard]] bool addVectorCharge(const std::vector<std::uint8_t> &value,
                                   std::uint64_t &bytes) noexcept {
  std::uint64_t storage = 0;
  return editorCheckedMultiply(static_cast<std::uint64_t>(value.capacity()),
                               sizeof(std::uint8_t), storage) &&
         editorCheckedAdd(storage, kVectorAllocationCharge, storage) &&
         editorCheckedAdd(bytes, storage, bytes);
}

template <typename Snapshot>
[[nodiscard]] std::optional<std::uint64_t>
retainedBytes(const Snapshot &before, const Snapshot &final,
              const std::vector<std::uint8_t> &beforeCanonical,
              const std::vector<std::uint8_t> &finalCanonical,
              const std::string &description,
              std::uint64_t objectBytes) noexcept {
  std::uint64_t bytes = objectBytes;
  std::uint64_t descriptionBytes =
      static_cast<std::uint64_t>(description.capacity());
  if (!editorCheckedAdd(bytes, kCommandAllocationCharge, bytes) ||
      !editorCheckedAdd(descriptionBytes, 1u, descriptionBytes) ||
      !editorCheckedAdd(descriptionBytes, kStringAllocationCharge,
                        descriptionBytes) ||
      !editorCheckedAdd(bytes, descriptionBytes, bytes) ||
      !editorCheckedAdd(bytes, before.snapshotBytes(), bytes) ||
      !editorCheckedAdd(bytes, final.snapshotBytes(), bytes) ||
      !addVectorCharge<Snapshot>(beforeCanonical, bytes) ||
      !addVectorCharge<Snapshot>(finalCanonical, bytes)) {
    return std::nullopt;
  }
  return bytes;
}

template <typename Snapshot>
class PropertyCommand final : public ContinuousEditorCommand {
public:
  using Traits = PropertyTraits<Snapshot>;
  using Id = typename Traits::Id;

  PropertyCommand(Id target, Snapshot before, Snapshot final,
                  std::vector<std::uint8_t> beforeCanonical,
                  std::vector<std::uint8_t> finalCanonical, MutationToken token,
                  EditorDocumentAuthority authority,
                  EditorSelectionSnapshot selection, std::string description,
                  EditorFaultInjector *faultInjector) noexcept
      : m_target(target), m_before(std::move(before)),
        m_final(std::move(final)),
        m_beforeCanonical(std::move(beforeCanonical)),
        m_finalCanonical(std::move(finalCanonical)), m_token(token),
        m_authority(authority), m_executeSelection(selection),
        m_undoSelection(selection), m_redoSelection(selection),
        m_description(std::move(description)), m_faultInjector(faultInjector) {
    const auto bytes = calculateRetainedBytes(m_final, m_finalCanonical);
    if (bytes) {
      m_snapshotBytes = *bytes;
      m_accountingValid = true;
    }
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
        Traits::validate(context.mutations, m_target, m_before, m_token);
    if (!validation.matches())
      return commandResult(context, CommandStatus::rejected, validation.error);
    if (m_before == m_final) {
      return commandResult(context, CommandStatus::unchanged,
                           MutationErrorCode::noOp);
    }

    MutationGuard guard;
    guard.expectedToken = m_token;
    MutationResult mutation =
        Traits::set(context.mutations, m_target, m_final, guard);
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
        Traits::validate(context.mutations, m_target, m_final, m_token);
    if (!validation.matches())
      return commandResult(context, CommandStatus::rejected, validation.error);

    MutationGuard guard;
    guard.expectedToken = m_token;
    MutationResult mutation =
        Traits::set(context.mutations, m_target, m_before, guard);
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
        Traits::validate(context.mutations, m_target, m_before, m_token);
    if (!validation.matches())
      return commandResult(context, CommandStatus::rejected, validation.error);

    MutationGuard guard;
    guard.expectedToken = m_token;
    MutationResult mutation =
        Traits::set(context.mutations, m_target, m_final, guard);
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
    return CommandMergeKey{Traits::commandKind, m_target.untyped()};
  }

  [[nodiscard]] RetainedCommandInspection
  inspectRetainedState() const override {
    RetainedCommandInspection result;
    result.kind = Traits::commandKind;
    result.lifecycle = m_lifecycle;
    result.stateRevision = m_stateRevision;
    result.retainedSnapshotBytes = m_snapshotBytes;
    result.stableTarget = m_target.untyped();
    result.authority = m_authority;
    result.preparedToken = m_token;
    result.beforePayload.typeTag = Traits::typeTag;
    appendCanonicalRecord(result.beforePayload.canonicalBytes,
                          m_beforeCanonical);
    appendU64(result.beforePayload.canonicalBytes, m_before.snapshotBytes());
    appendString(result.beforePayload.canonicalBytes, m_description);
    appendU64(result.beforePayload.canonicalBytes,
              static_cast<std::uint64_t>(m_description.capacity()));
    appendBool(result.beforePayload.canonicalBytes, m_accountingValid);
    result.finalPayload.typeTag = Traits::typeTag;
    appendCanonicalRecord(result.finalPayload.canonicalBytes, m_finalCanonical);
    appendU64(result.finalPayload.canonicalBytes, m_final.snapshotBytes());
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
        Traits::validate(context.mutations, m_target, m_final, m_token));
  }

  [[nodiscard]] std::optional<std::uint64_t>
  mergedSnapshotBytes(const EditorCommand &newer) const noexcept override {
    const auto *typed = dynamic_cast<const PropertyCommand *>(&newer);
    if (!compatibleWith(typed))
      return std::nullopt;
    return calculateRetainedBytes(typed->m_final, typed->m_finalCanonical);
  }

  [[nodiscard]] bool tryAbsorbNewer(EditorCommand &newer) noexcept override {
    auto *typed = dynamic_cast<PropertyCommand *>(&newer);
    if (!compatibleWith(typed) || !canAdvanceState())
      return false;
    const auto bytes =
        calculateRetainedBytes(typed->m_final, typed->m_finalCanonical);
    if (!bytes)
      return false;
    if (m_faultInjector != nullptr &&
        m_faultInjector->shouldFail(EditorFaultPoint::allocation)) {
      return false;
    }

    m_final = std::move(typed->m_final);
    m_finalCanonical = std::move(typed->m_finalCanonical);
    m_token = typed->m_token;
    m_snapshotBytes = *bytes;
    ++m_stateRevision;
    return true;
  }

  [[nodiscard]] ContinuousValueKind valueKind() const noexcept override {
    return Traits::valueKind;
  }

  [[nodiscard]] SnapshotByteResult prospectiveSnapshotBytes(
      const ContinuousCommandValue &value) const noexcept override {
    const auto *snapshot = std::get_if<Snapshot>(&value);
    if (snapshot == nullptr || !compatibleCandidate(*snapshot)) {
      return SnapshotByteResult{MutationErrorCode::invalidSnapshot, 0};
    }
    if (*snapshot == m_final) {
      return SnapshotByteResult{m_accountingValid
                                    ? MutationErrorCode::none
                                    : MutationErrorCode::snapshotBudgetExceeded,
                                m_accountingValid ? m_snapshotBytes : 0};
    }

    SnapshotCanonicalRecord record = snapshot->inspectCanonicalRecord();
    if (!record.complete() ||
        record.schemaVersion != SnapshotCanonicalRecord::kSchemaVersion ||
        record.kind != Traits::objectKind) {
      return SnapshotByteResult{inspectionError(record), 0};
    }
    const auto bytes = calculateRetainedBytes(*snapshot, record.canonicalBytes);
    return bytes ? SnapshotByteResult{MutationErrorCode::none, *bytes}
                 : SnapshotByteResult{MutationErrorCode::snapshotBudgetExceeded,
                                      0};
  }

  [[nodiscard]] CommandResult
  applyPreview(EditorCommandContext &context,
               const ContinuousCommandValue &value) override {
    const auto *snapshot = std::get_if<Snapshot>(&value);
    if (snapshot == nullptr || !compatibleCandidate(*snapshot)) {
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
        Traits::validate(context.mutations, m_target, m_final, m_token);
    if (!validation.matches())
      return commandResult(context, CommandStatus::rejected, validation.error);
    if (*snapshot == m_final) {
      return commandResult(context, CommandStatus::unchanged,
                           MutationErrorCode::noOp);
    }
    if (m_faultInjector != nullptr &&
        m_faultInjector->shouldFail(EditorFaultPoint::allocation)) {
      return commandResult(context, CommandStatus::rejected,
                           MutationErrorCode::allocationFailed);
    }

    SnapshotCanonicalRecord record = snapshot->inspectCanonicalRecord();
    if (!record.complete() ||
        record.schemaVersion != SnapshotCanonicalRecord::kSchemaVersion ||
        record.kind != Traits::objectKind) {
      return commandResult(context, CommandStatus::rejected,
                           inspectionError(record));
    }
    const auto bytes = calculateRetainedBytes(*snapshot, record.canonicalBytes);
    if (!bytes) {
      return commandResult(context, CommandStatus::rejected,
                           MutationErrorCode::snapshotBudgetExceeded);
    }

    MutationGuard guard;
    guard.expectedToken = m_token;
    MutationResult mutation =
        Traits::set(context.mutations, m_target, *snapshot, guard);
    if (mutation.applied()) {
      m_final = *snapshot;
      m_finalCanonical = std::move(record.canonicalBytes);
      m_token = mutation.transactionToken;
      m_snapshotBytes = *bytes;
      m_lifecycle = CommandLifecycleState::executed;
      ++m_stateRevision;
    }
    return commandResult(std::move(mutation));
  }

  [[nodiscard]] bool initialEqualsFinal() const noexcept override {
    return m_before == m_final;
  }

private:
  [[nodiscard]] bool
  compatibleWith(const PropertyCommand *newer) const noexcept {
    return newer != nullptr && m_lifecycle == CommandLifecycleState::executed &&
           newer->m_lifecycle == CommandLifecycleState::executed &&
           m_target == newer->m_target && m_authority == newer->m_authority &&
           m_final == newer->m_before &&
           m_finalCanonical == newer->m_beforeCanonical &&
           m_before != newer->m_final && m_accountingValid &&
           newer->m_accountingValid;
  }

  [[nodiscard]] bool compatibleCandidate(const Snapshot &value) const noexcept {
    return value.valid() && value.id() == m_target &&
           value.authority() == m_authority;
  }

  [[nodiscard]] std::optional<std::uint64_t> calculateRetainedBytes(
      const Snapshot &final,
      const std::vector<std::uint8_t> &finalCanonical) const noexcept {
    return retainedBytes(m_before, final, m_beforeCanonical, finalCanonical,
                         m_description,
                         static_cast<std::uint64_t>(sizeof(PropertyCommand)));
  }

  [[nodiscard]] bool canAdvanceState() const noexcept {
    return m_stateRevision != std::numeric_limits<std::uint64_t>::max();
  }

  Id m_target;
  Snapshot m_before;
  Snapshot m_final;
  std::vector<std::uint8_t> m_beforeCanonical;
  std::vector<std::uint8_t> m_finalCanonical;
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

template <typename Snapshot>
[[nodiscard]] std::string propertyDescription(const Snapshot &before) {
  std::string result{PropertyTraits<Snapshot>::descriptionPrefix};
  result.append(before.name());
  result.push_back('\'');
  return result;
}

template <typename Snapshot>
[[nodiscard]] std::unique_ptr<PropertyCommand<Snapshot>>
makePropertyCommand(EditorCommandContext &context, Snapshot before,
                    Snapshot after, EditorFaultInjector *faultInjector,
                    MutationErrorCode &error) {
  std::vector<std::uint8_t> beforeCanonical;
  error = inspectSnapshot(before, faultInjector, beforeCanonical);
  if (error != MutationErrorCode::none)
    return nullptr;
  std::vector<std::uint8_t> finalCanonical;
  error = inspectSnapshot(after, faultInjector, finalCanonical);
  if (error != MutationErrorCode::none)
    return nullptr;
  if (faultInjector != nullptr &&
      faultInjector->shouldFail(EditorFaultPoint::allocation)) {
    error = MutationErrorCode::allocationFailed;
    return nullptr;
  }

  std::string description = propertyDescription(before);

  auto command = std::make_unique<PropertyCommand<Snapshot>>(
      before.id(), std::move(before), std::move(after),
      std::move(beforeCanonical), std::move(finalCanonical),
      context.mutations.token(), context.document.documentAuthority(),
      context.selection.snapshot(), std::move(description), faultInjector);
  if (!command->accountingValid()) {
    error = MutationErrorCode::snapshotBudgetExceeded;
    return nullptr;
  }
  error = MutationErrorCode::none;
  return command;
}

template <typename Snapshot>
[[nodiscard]] CommandPrepareResult
prepareDiscrete(EditorCommandContext &context, Snapshot before, Snapshot after,
                EditorFaultInjector *faultInjector) noexcept {
  CommandPrepareResult result;
  result.error = validateSnapshotPair(context, before, after);
  if (result.error != MutationErrorCode::none)
    return result;
  try {
    result.command =
        makePropertyCommand(context, std::move(before), std::move(after),
                            faultInjector, result.error);
    return result;
  } catch (const std::bad_alloc &) {
    result.command.reset();
    result.error = MutationErrorCode::allocationFailed;
    return result;
  } catch (...) {
    result.command.reset();
    result.error = MutationErrorCode::internalError;
    return result;
  }
}

template <typename Snapshot>
[[nodiscard]] ContinuousCommandPrepareResult
prepareContinuous(EditorCommandContext &context,
                  typename PropertyTraits<Snapshot>::Id target,
                  std::optional<Snapshot> current,
                  EditorFaultInjector *faultInjector) noexcept {
  ContinuousCommandPrepareResult result;
  if (target.authority() != context.document.documentAuthority()) {
    result.error = MutationErrorCode::crossDocument;
    return result;
  }
  if (!context.document.contains(target.untyped())) {
    result.error = MutationErrorCode::staleId;
    return result;
  }
  if (!current) {
    result.error = MutationErrorCode::allocationFailed;
    return result;
  }
  try {
    MutationErrorCode error = MutationErrorCode::none;
    auto command =
        makePropertyCommand(context, *current, *current, faultInjector, error);
    result.error = error;
    result.command = std::move(command);
    return result;
  } catch (const std::bad_alloc &) {
    result.command.reset();
    result.error = MutationErrorCode::allocationFailed;
    return result;
  } catch (...) {
    result.command.reset();
    result.error = MutationErrorCode::internalError;
    return result;
  }
}

} // namespace

CommandPrepareResult
prepareEditMaterialCommand(EditorCommandContext &context,
                           MaterialSnapshot before, MaterialSnapshot after,
                           EditorFaultInjector *faultInjector) noexcept {
  return prepareDiscrete(context, std::move(before), std::move(after),
                         faultInjector);
}

CommandPrepareResult
prepareEditLightCommand(EditorCommandContext &context, LightSnapshot before,
                        LightSnapshot after,
                        EditorFaultInjector *faultInjector) noexcept {
  return prepareDiscrete(context, std::move(before), std::move(after),
                         faultInjector);
}

ContinuousCommandPrepareResult
prepareContinuousMaterialCommand(EditorCommandContext &context,
                                 MaterialId material,
                                 EditorFaultInjector *faultInjector) noexcept {
  return prepareContinuous<MaterialSnapshot>(
      context, material, context.mutations.materialSnapshot(material),
      faultInjector);
}

ContinuousCommandPrepareResult
prepareContinuousLightCommand(EditorCommandContext &context, LightId light,
                              EditorFaultInjector *faultInjector) noexcept {
  return prepareContinuous<LightSnapshot>(
      context, light, context.mutations.lightSnapshot(light), faultInjector);
}

} // namespace demo::editor
