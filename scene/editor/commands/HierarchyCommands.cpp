#include <mgif/scene_editor/commands/HierarchyCommands.h>

#include <mgif/scene_editor/EditorLimits.h>
#include <mgif/scene_editor/detail/BitwiseCast.h>

#include <algorithm>
#include <cassert>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <memory>
#include <new>
#include <optional>
#include <stdexcept>
#include <string>
#include <string_view>
#include <type_traits>
#include <utility>
#include <variant>
#include <vector>

namespace demo::editor {
namespace {

constexpr std::uint64_t kHierarchyCommandControlCharge = 128u;

void appendU8(std::vector<std::uint8_t> &bytes, std::uint8_t value) {
  bytes.push_back(value);
}

void appendU32(std::vector<std::uint8_t> &bytes, std::uint32_t value) {
  for (unsigned shift = 0; shift < 32u; shift += 8u)
    bytes.push_back(static_cast<std::uint8_t>(value >> shift));
}

void appendU64(std::vector<std::uint8_t> &bytes, std::uint64_t value) {
  for (unsigned shift = 0; shift < 64u; shift += 8u)
    bytes.push_back(static_cast<std::uint8_t>(value >> shift));
}

void appendF64(std::vector<std::uint8_t> &bytes, double value) {
  static_assert(sizeof(double) == sizeof(std::uint64_t));
  appendU64(bytes, detail::bitwiseCast<std::uint64_t>(value));
}

void appendId(std::vector<std::uint8_t> &bytes, EditorObjectId value) {
  appendU8(bytes, static_cast<std::uint8_t>(value.kind));
  appendU32(bytes, value.slot);
  appendU32(bytes, value.generation);
  appendU64(bytes, value.authority.value);
}

template <EditorObjectKind Kind>
void appendId(std::vector<std::uint8_t> &bytes,
              TypedEditorObjectId<Kind> value) {
  appendId(bytes, value.untyped());
}

template <typename Id>
void appendOptionalId(std::vector<std::uint8_t> &bytes,
                      const std::optional<Id> &value) {
  appendU8(bytes, value ? 1u : 0u);
  if (value)
    appendId(bytes, *value);
}

void appendString(std::vector<std::uint8_t> &bytes, const std::string &value) {
  appendU64(bytes, static_cast<std::uint64_t>(value.size()));
  bytes.insert(bytes.end(), value.begin(), value.end());
  appendU64(bytes, static_cast<std::uint64_t>(value.capacity()));
}

void appendOccurrence(std::vector<std::uint8_t> &bytes,
                      const SceneNodeOccurrence &value) {
  appendId(bytes, value.scene);
  appendId(bytes, value.node);
  appendOptionalId(bytes, value.parent);
  appendU64(bytes, value.revision);
}

[[nodiscard]] SceneId reparentTargetScene(
    const ReparentNodeRequest &request) noexcept {
  if (request.targetScene.valid())
    return request.targetScene;
  if (request.parent)
    return request.parent->scene;
  return request.source.scene;
}

void appendLightProperties(std::vector<std::uint8_t> &bytes,
                           const LightProperties &value) {
  appendU64(bytes, static_cast<std::uint64_t>(value.type.size()));
  bytes.insert(bytes.end(), value.type.begin(), value.type.end());
  appendU64(bytes, static_cast<std::uint64_t>(value.type.capacity()));
  for (const double component : value.color)
    appendF64(bytes, component);
  appendF64(bytes, value.intensity);
  appendF64(bytes, value.range);
  appendF64(bytes, value.innerConeAngle);
  appendF64(bytes, value.outerConeAngle);
}

void appendStructuralRequest(std::vector<std::uint8_t> &bytes,
                             const StructuralRequest &request) {
  appendU8(bytes, static_cast<std::uint8_t>(request.index()));
  std::visit(
      [&](const auto &value) {
        using Request = std::remove_cvref_t<decltype(value)>;
        if constexpr (std::is_same_v<Request, AddChildNodeRequest>) {
          appendId(bytes, value.scene);
          appendId(bytes, value.parent);
          appendString(bytes, value.name);
        } else if constexpr (std::is_same_v<Request, AddLightRequest>) {
          appendId(bytes, value.scene);
          appendOptionalId(bytes, value.parent);
          appendString(bytes, value.nodeName);
          appendString(bytes, value.lightName);
          appendLightProperties(bytes, value.properties);
        } else if constexpr (std::is_same_v<Request, DuplicateNodeRequest>) {
          appendOccurrence(bytes, value.source);
          appendU8(bytes, value.parent ? 1u : 0u);
          if (value.parent)
            appendOccurrence(bytes, *value.parent);
          appendU8(bytes, static_cast<std::uint8_t>(value.policy));
        } else if constexpr (std::is_same_v<Request, DeleteNodeRequest>) {
          appendOccurrence(bytes, value.occurrence);
          appendU8(bytes, static_cast<std::uint8_t>(value.policy));
        } else {
          appendOccurrence(bytes, value.source);
          appendU8(bytes, value.parent ? 1u : 0u);
          if (value.parent)
            appendOccurrence(bytes, *value.parent);
          appendU8(bytes, static_cast<std::uint8_t>(value.policy));
          appendId(bytes, reparentTargetScene(value));
        }
      },
      request);
}

[[nodiscard]] bool addBytes(std::uint64_t &total,
                            std::uint64_t value) noexcept {
  std::uint64_t next = 0;
  if (!editorCheckedAdd(total, value, next))
    return false;
  total = next;
  return true;
}

[[nodiscard]] bool addStringBytes(std::uint64_t &total,
                                  const std::string &value) noexcept {
  return addBytes(total, static_cast<std::uint64_t>(value.capacity()) + 1u);
}

[[nodiscard]] bool addRequestBytes(std::uint64_t &total,
                                   const StructuralRequest &request) noexcept {
  return std::visit(
      [&](const auto &value) noexcept {
        using Request = std::remove_cvref_t<decltype(value)>;
        if constexpr (std::is_same_v<Request, AddChildNodeRequest>) {
          return addStringBytes(total, value.name);
        } else if constexpr (std::is_same_v<Request, AddLightRequest>) {
          return addStringBytes(total, value.nodeName) &&
                 addStringBytes(total, value.lightName) &&
                 addStringBytes(total, value.properties.type);
        } else {
          return true;
        }
      },
      request);
}

[[nodiscard]] CommandPrepareResult failure(MutationErrorCode error,
                                           const char *message) noexcept {
  CommandPrepareResult result;
  result.error = error;
  try {
    result.message = message;
  } catch (...) {
  }
  return result;
}

[[nodiscard]] CommandResult rejected(const EditorCommandContext &context,
                                     MutationErrorCode error,
                                     std::string message = {}) {
  CommandResult result;
  result.status = CommandStatus::rejected;
  result.error = error;
  result.historyPolicy = HistoryPolicy::undoable;
  result.beforeRevision = context.document.revision();
  result.afterRevision = context.document.revision();
  result.message = std::move(message);
  return result;
}

[[nodiscard]] CommandResult fromMutation(MutationResult mutation,
                                         CommandStatus status) {
  CommandResult result;
  result.status = status;
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

[[nodiscard]] EditorSelectionSnapshot
nodeSelection(EditorDocumentAuthority authority,
              const SceneNodeOccurrence &occurrence) noexcept {
  return EditorSelectionSnapshot{
      authority,
      NodeSelection{occurrence.scene, occurrence.node, occurrence.parent}};
}

[[nodiscard]] MutationErrorCode
validateOccurrence(const EditorCommandContext &context,
                   const SceneNodeOccurrence &occurrence) noexcept {
  const EditorDocumentAuthority authority =
      context.document.documentAuthority();
  if (occurrence.scene.authority() != authority ||
      occurrence.node.authority() != authority ||
      (occurrence.parent && occurrence.parent->authority() != authority))
    return MutationErrorCode::crossDocument;
  if (occurrence.revision != context.document.revision() ||
      !context.document.contains(occurrence.scene.untyped()) ||
      !context.document.contains(occurrence.node.untyped()) ||
      (occurrence.parent &&
       !context.document.contains(occurrence.parent->untyped())) ||
      !context.document.hasNodeOccurrence(occurrence.scene, occurrence.node,
                                          occurrence.parent))
    return MutationErrorCode::staleOccurrence;
  return MutationErrorCode::none;
}

[[nodiscard]] CommandPrepareResult
occurrenceFailure(MutationErrorCode error, const char *operation) noexcept {
  if (error == MutationErrorCode::crossDocument)
    return failure(error, operation);
  return failure(MutationErrorCode::staleOccurrence, operation);
}

[[nodiscard]] bool isNewEntry(const MutationRemapEntry &entry) noexcept {
  return !entry.oldId.valid() && entry.newId.valid() &&
         entry.newId.kind == EditorObjectKind::node;
}

struct DuplicateRootPosition final {
  std::size_t subtreeSize{0};
  std::size_t rootRank{0};
};

[[nodiscard]] std::optional<DuplicateRootPosition>
duplicateRootPosition(const GltfDocument &document, NodeId root) {
  const auto rootIndex = document.indexOf(root.untyped());
  if (!rootIndex)
    return std::nullopt;
  std::vector<NodeId> stack{root};
  std::vector<std::size_t> indices;
  while (!stack.empty()) {
    const NodeId current = stack.back();
    stack.pop_back();
    const auto index = document.indexOf(current.untyped());
    if (!index ||
        std::find(indices.begin(), indices.end(), *index) != indices.end())
      return std::nullopt;
    indices.push_back(*index);
    const std::vector<NodeId> children = document.nodeChildIds(current);
    stack.insert(stack.end(), children.begin(), children.end());
  }
  std::sort(indices.begin(), indices.end());
  const auto found =
      std::lower_bound(indices.begin(), indices.end(), *rootIndex);
  if (found == indices.end() || *found != *rootIndex)
    return std::nullopt;
  return DuplicateRootPosition{
      indices.size(), static_cast<std::size_t>(found - indices.begin())};
}

class RenameNodeCommand final : public EditorCommand {
public:
  RenameNodeCommand(NodeId node, std::string oldName, std::string newName,
                    std::string description, MutationToken preparedToken,
                    EditorSelectionSnapshot selection,
                    std::uint64_t snapshotBytes) noexcept
      : m_node(node), m_oldName(std::move(oldName)),
        m_newName(std::move(newName)), m_description(std::move(description)),
        m_preparedToken(preparedToken), m_selection(std::move(selection)),
        m_snapshotBytes(snapshotBytes) {}

  [[nodiscard]] CommandResult execute(EditorCommandContext &context) override {
    if (m_lifecycle != CommandLifecycleState::prepared)
      return rejected(context, MutationErrorCode::staleTransaction,
                      "rename command is not prepared");
    MutationGuard guard;
    guard.expectedToken = m_preparedToken;
    MutationResult mutation =
        context.mutations.rename(m_node.untyped(), m_newName, guard);
    if (mutation.domainNoop())
      return fromMutation(std::move(mutation), CommandStatus::unchanged);
    if (!mutation.applied())
      return fromMutation(std::move(mutation), CommandStatus::rejected);
    m_lifecycle = CommandLifecycleState::executed;
    ++m_stateRevision;
    return fromMutation(std::move(mutation), CommandStatus::applied);
  }

  [[nodiscard]] CommandResult undo(EditorCommandContext &context) override {
    if (m_lifecycle != CommandLifecycleState::executed)
      return rejected(context, MutationErrorCode::staleTransaction,
                      "rename command is not executed");
    if (!valueMatches(context, m_newName))
      return rejected(context, MutationErrorCode::staleTransaction,
                      "renamed node no longer matches the command");
    MutationResult mutation =
        context.mutations.rename(m_node.untyped(), m_oldName);
    if (!mutation.applied())
      return fromMutation(std::move(mutation), CommandStatus::rejected);
    m_lifecycle = CommandLifecycleState::undone;
    ++m_stateRevision;
    return fromMutation(std::move(mutation), CommandStatus::applied);
  }

  [[nodiscard]] CommandResult redo(EditorCommandContext &context) override {
    if (m_lifecycle != CommandLifecycleState::undone)
      return rejected(context, MutationErrorCode::staleTransaction,
                      "rename command is not undone");
    if (!valueMatches(context, m_oldName))
      return rejected(context, MutationErrorCode::staleTransaction,
                      "renamed node no longer matches the command");
    MutationResult mutation =
        context.mutations.rename(m_node.untyped(), m_newName);
    if (!mutation.applied())
      return fromMutation(std::move(mutation), CommandStatus::rejected);
    m_lifecycle = CommandLifecycleState::executed;
    ++m_stateRevision;
    return fromMutation(std::move(mutation), CommandStatus::applied);
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

  [[nodiscard]] RetainedCommandInspection
  inspectRetainedState() const override {
    RetainedCommandInspection result;
    result.kind = EditorCommandKind::renameNode;
    result.lifecycle = m_lifecycle;
    result.stateRevision = m_stateRevision;
    result.retainedSnapshotBytes = m_snapshotBytes;
    result.stableTarget = m_node.untyped();
    result.authority = m_node.authority();
    result.preparedToken = m_preparedToken;
    result.beforePayload.typeTag = "mgif.rename-node.before.v1";
    appendId(result.beforePayload.canonicalBytes, m_node);
    appendString(result.beforePayload.canonicalBytes, m_oldName);
    appendString(result.beforePayload.canonicalBytes, m_description);
    result.finalPayload.typeTag = "mgif.rename-node.final.v1";
    appendId(result.finalPayload.canonicalBytes, m_node);
    appendString(result.finalPayload.canonicalBytes, m_newName);
    result.executeSelection = m_selection;
    result.undoSelection = m_selection;
    result.redoSelection = m_selection;
    detail::finalizeRetainedCommandInspection(result);
    return result;
  }

  [[nodiscard]] ExecutedValidation validateExecuted(
      const EditorCommandContext &context) const noexcept override {
    if (m_lifecycle != CommandLifecycleState::executed)
      return {};
    if (m_node.authority() != context.document.documentAuthority() ||
        !context.document.contains(m_node.untyped()))
      return {ExecutedValidationStatus::stale,
              MutationErrorCode::staleTransaction};
    return valueMatches(context, m_newName)
               ? ExecutedValidation{ExecutedValidationStatus::matches,
                                    MutationErrorCode::none}
               : ExecutedValidation{ExecutedValidationStatus::valueMismatch,
                                    MutationErrorCode::staleTransaction};
  }

private:
  [[nodiscard]] bool valueMatches(const EditorCommandContext &context,
                                  const std::string &value) const noexcept {
    return m_node.authority() == context.document.documentAuthority() &&
           context.document.contains(m_node.untyped()) &&
           context.document.objectName(m_node.untyped()) == value;
  }

  NodeId m_node;
  std::string m_oldName;
  std::string m_newName;
  std::string m_description;
  MutationToken m_preparedToken{};
  EditorSelectionSnapshot m_selection{};
  std::uint64_t m_snapshotBytes{0};
  std::uint64_t m_stateRevision{0};
  CommandLifecycleState m_lifecycle{CommandLifecycleState::prepared};
};

class StructuralCommand final : public EditorCommand {
public:
  StructuralCommand(EditorCommandKind kind, StructuralRequest request,
                    std::unique_ptr<PreparedStructuralMutation> prepared,
                    std::string description, MutationToken preparedToken,
                    EditorSelectionSnapshot originalSelection,
                    std::optional<DuplicateRootPosition> duplicatePosition,
                    std::uint64_t snapshotBytes) noexcept
      : m_kind(kind), m_request(std::move(request)),
        m_prepared(std::move(prepared)), m_description(std::move(description)),
        m_preparedToken(preparedToken),
        m_originalSelection(std::move(originalSelection)),
        m_duplicatePosition(duplicatePosition), m_snapshotBytes(snapshotBytes) {
    initializeSelectionEnvelopes();
  }

  [[nodiscard]] CommandResult execute(EditorCommandContext &context) override {
    if (m_lifecycle != CommandLifecycleState::prepared || !m_prepared)
      return rejected(context, MutationErrorCode::staleTransaction,
                      "structural command is not prepared");

    StructuralMutationResult structural =
        context.mutations.commitStructural(std::move(m_prepared));
    if (structural.mutation.domainNoop()) {
      const bool deleteEnvelope =
          m_kind == EditorCommandKind::deleteNode &&
          structural.mutation.error == MutationErrorCode::uniqueRootDeleteNoop;
      const bool moveEnvelope =
          m_kind == EditorCommandKind::reparentNode &&
          structural.mutation.error == MutationErrorCode::noOp;
      if (!deleteEnvelope && !moveEnvelope)
        return fromMutation(std::move(structural.mutation),
                            CommandStatus::unchanged);
      m_historyEnvelope = true;
      if (deleteEnvelope) {
        m_undoSelection = m_originalSelection;
        context.selection.clear();
      }
      m_lifecycle = CommandLifecycleState::executed;
      ++m_stateRevision;
      CommandResult result = fromMutation(std::move(structural.mutation),
                                          CommandStatus::historyEnvelopeNoop);
      result.selectionStatus = deleteEnvelope
                                   ? SelectionRestoreStatus::clearedEmpty
                                   : SelectionRestoreStatus::restored;
      return result;
    }
    if (!structural.mutation.applied())
      return fromMutation(std::move(structural.mutation),
                          CommandStatus::rejected);

    const bool hasDelta = structural.hasDelta();
    if (hasDelta)
      m_delta = std::move(structural.delta);
    const bool selectionPrepared = prepareCreatedSelection(
        context.document, structural.mutation.remap.nodes);
    const SelectionRestoreStatus selectionStatus = publishSelection(
        context, m_executeSelection,
        m_kind == EditorCommandKind::reparentNode);
    m_lifecycle = CommandLifecycleState::executed;
    ++m_stateRevision;
    CommandResult result =
        fromMutation(std::move(structural.mutation), CommandStatus::applied);
    result.selectionStatus = selectionStatus;
    if (!hasDelta || !selectionPrepared)
      result.error = MutationErrorCode::internalError;
    return result;
  }

  [[nodiscard]] CommandResult undo(EditorCommandContext &context) override {
    if (m_lifecycle != CommandLifecycleState::executed)
      return rejected(context, MutationErrorCode::staleTransaction,
                      "structural command is not executed");
    if (m_historyEnvelope) {
      const SelectionRestoreStatus selectionStatus = publishSelection(
          context, m_undoSelection, m_kind == EditorCommandKind::reparentNode);
      m_lifecycle = CommandLifecycleState::undone;
      ++m_stateRevision;
      CommandResult result = envelopeResult(context);
      result.selectionStatus = selectionStatus;
      return result;
    }
    if (!m_delta)
      return rejected(context, MutationErrorCode::invalidSnapshot,
                      "structural command has no delta");
    MutationResult mutation =
        context.mutations.restoreStructural(*m_delta, StructuralState::undo);
    if (!mutation.applied())
      return fromMutation(std::move(mutation), CommandStatus::rejected);
    const SelectionRestoreStatus selectionStatus = publishSelection(
        context, m_undoSelection,
        m_kind == EditorCommandKind::reparentNode);
    m_lifecycle = CommandLifecycleState::undone;
    ++m_stateRevision;
    CommandResult result =
        fromMutation(std::move(mutation), CommandStatus::applied);
    result.selectionStatus = selectionStatus;
    return result;
  }

  [[nodiscard]] CommandResult redo(EditorCommandContext &context) override {
    if (m_lifecycle != CommandLifecycleState::undone)
      return rejected(context, MutationErrorCode::staleTransaction,
                      "structural command is not undone");
    if (m_historyEnvelope) {
      const SelectionRestoreStatus selectionStatus = publishSelection(
          context, m_redoSelection, m_kind == EditorCommandKind::reparentNode);
      m_lifecycle = CommandLifecycleState::executed;
      ++m_stateRevision;
      CommandResult result = envelopeResult(context);
      result.selectionStatus = selectionStatus;
      return result;
    }
    if (!m_delta)
      return rejected(context, MutationErrorCode::invalidSnapshot,
                      "structural command has no delta");
    MutationResult mutation =
        context.mutations.restoreStructural(*m_delta, StructuralState::redo);
    if (!mutation.applied())
      return fromMutation(std::move(mutation), CommandStatus::rejected);
    const SelectionRestoreStatus selectionStatus = publishSelection(
        context, m_redoSelection,
        m_kind == EditorCommandKind::reparentNode);
    m_lifecycle = CommandLifecycleState::executed;
    ++m_stateRevision;
    CommandResult result =
        fromMutation(std::move(mutation), CommandStatus::applied);
    result.selectionStatus = selectionStatus;
    return result;
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

  [[nodiscard]] RetainedCommandInspection
  inspectRetainedState() const override {
    RetainedCommandInspection result;
    result.kind = m_kind;
    result.lifecycle = m_lifecycle;
    result.stateRevision = m_stateRevision;
    result.retainedSnapshotBytes = m_snapshotBytes;
    result.stableTarget = stableTarget();
    result.authority = m_preparedToken.authority;
    result.preparedToken = m_preparedToken;
    result.beforePayload.typeTag = beforeTypeTag();
    appendStructuralRequest(result.beforePayload.canonicalBytes, m_request);
    appendString(result.beforePayload.canonicalBytes, m_description);
    appendU8(result.beforePayload.canonicalBytes,
             m_duplicatePosition ? 1u : 0u);
    if (m_duplicatePosition) {
      appendU64(result.beforePayload.canonicalBytes,
                static_cast<std::uint64_t>(m_duplicatePosition->subtreeSize));
      appendU64(result.beforePayload.canonicalBytes,
                static_cast<std::uint64_t>(m_duplicatePosition->rootRank));
    }
    result.finalPayload.typeTag = finalTypeTag();
    appendOptionalId(result.finalPayload.canonicalBytes, m_createdNode);
    appendU8(result.finalPayload.canonicalBytes, m_historyEnvelope ? 1u : 0u);
    appendU64(result.finalPayload.canonicalBytes, m_snapshotBytes);
    if (m_delta) {
      const StructuralDeltaCanonicalSide undo =
          m_delta->inspectCanonicalSide(StructuralState::undo);
      const StructuralDeltaCanonicalSide redo =
          m_delta->inspectCanonicalSide(StructuralState::redo);
      requireCompleteSide(undo, StructuralState::undo);
      requireCompleteSide(redo, StructuralState::redo);
      result.undoStructuralSide.present = true;
      result.undoStructuralSide.canonicalBytes = undo.canonicalBytes;
      result.redoStructuralSide.present = true;
      result.redoStructuralSide.canonicalBytes = redo.canonicalBytes;
    }
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
    if (m_preparedToken.authority != context.document.documentAuthority())
      return {ExecutedValidationStatus::stale,
              MutationErrorCode::crossDocument};
    if (m_createdNode && !context.document.contains(m_createdNode->untyped()))
      return {ExecutedValidationStatus::valueMismatch,
              MutationErrorCode::staleTransaction};
    return {ExecutedValidationStatus::matches, MutationErrorCode::none};
  }

private:
  static void requireCompleteSide(const StructuralDeltaCanonicalSide &side,
                                  StructuralState expected) {
    if (side.status == StructuralDeltaInspectionStatus::allocationFailed)
      throw std::bad_alloc{};
    if (!side.complete() ||
        side.schemaVersion != StructuralDeltaCanonicalSide::kSchemaVersion ||
        side.side != expected || side.canonicalBytes.empty())
      throw std::logic_error("incomplete structural delta inspection");
  }

  void initializeSelectionEnvelopes() noexcept {
    m_executeSelection = EditorSelectionSnapshot::empty();
    m_undoSelection = EditorSelectionSnapshot::empty();
    m_redoSelection = EditorSelectionSnapshot::empty();
    if (const auto *duplicate = std::get_if<DuplicateNodeRequest>(&m_request)) {
      m_undoSelection =
          nodeSelection(m_preparedToken.authority, duplicate->source);
    } else if (const auto *deleted =
                   std::get_if<DeleteNodeRequest>(&m_request)) {
      m_undoSelection =
          nodeSelection(m_preparedToken.authority, deleted->occurrence);
    } else if (std::holds_alternative<ReparentNodeRequest>(m_request)) {
      m_undoSelection = m_originalSelection;
      // Reparent changes hierarchy order only. The reference keeps the exact
      // selection envelope, including the original occurrence, through
      // Execute, Undo, and Redo.
      m_executeSelection = m_originalSelection;
      m_redoSelection = m_originalSelection;
    }
  }

  [[nodiscard]] bool prepareCreatedSelection(
      const GltfDocument &document,
      const std::vector<MutationRemapEntry> &nodeRemap) noexcept {
    if (m_kind != EditorCommandKind::addChildNode &&
        m_kind != EditorCommandKind::addLight &&
        m_kind != EditorCommandKind::duplicateNode)
      return true;

    std::size_t newCount = 0;
    for (const MutationRemapEntry &entry : nodeRemap)
      if (isNewEntry(entry))
        ++newCount;
    if (newCount == 0)
      return false;

    std::size_t wanted = newCount - 1u;
    SceneId scene;
    std::optional<NodeId> requestedParent;
    if (const auto *request = std::get_if<AddChildNodeRequest>(&m_request)) {
      scene = request->scene;
      requestedParent = request->parent;
    } else if (const auto *request = std::get_if<AddLightRequest>(&m_request)) {
      scene = request->scene;
      requestedParent = request->parent;
    } else {
      const auto &duplicate = std::get<DuplicateNodeRequest>(m_request);
      scene = duplicate.source.scene;
      if (duplicate.parent)
        requestedParent = duplicate.parent->node;
      if (!m_duplicatePosition || m_duplicatePosition->subtreeSize > newCount ||
          m_duplicatePosition->rootRank >= m_duplicatePosition->subtreeSize)
        return false;
      wanted = newCount - m_duplicatePosition->subtreeSize +
               m_duplicatePosition->rootRank;
    }

    std::size_t ordinal = 0;
    std::optional<NodeId> created;
    for (const MutationRemapEntry &entry : nodeRemap) {
      if (!isNewEntry(entry))
        continue;
      if (ordinal == wanted) {
        created = NodeId::from(entry.newId);
        break;
      }
      ++ordinal;
    }
    if (!created)
      return false;

    std::optional<NodeId> actualParent;
    if (document.hasNodeOccurrence(scene, *created, requestedParent)) {
      actualParent = requestedParent;
    } else {
      for (const MutationRemapEntry &entry : nodeRemap) {
        if (!isNewEntry(entry))
          continue;
        const auto candidate = NodeId::from(entry.newId);
        if (candidate &&
            document.hasNodeOccurrence(scene, *created, *candidate)) {
          actualParent = *candidate;
          break;
        }
      }
      if (!actualParent)
        return false;
    }

    m_createdNode = *created;
    m_executeSelection =
        EditorSelectionSnapshot{m_preparedToken.authority,
                                NodeSelection{scene, *created, actualParent}};
    m_redoSelection = m_executeSelection;
    return true;
  }

  [[nodiscard]] SelectionRestoreStatus
  publishSelection(EditorCommandContext &context,
                   const EditorSelectionSnapshot &selection,
                   bool unchanged) const noexcept {
    if (unchanged)
      return SelectionRestoreStatus::restored;
    return context.selection.restore(context.document, selection);
  }

  [[nodiscard]] CommandResult
  envelopeResult(const EditorCommandContext &context) const {
    CommandResult result;
    result.status = CommandStatus::historyEnvelopeNoop;
    result.error = m_kind == EditorCommandKind::deleteNode
                       ? MutationErrorCode::uniqueRootDeleteNoop
                       : MutationErrorCode::noOp;
    result.historyPolicy = HistoryPolicy::undoable;
    result.beforeRevision = context.document.revision();
    result.afterRevision = context.document.revision();
    return result;
  }

  [[nodiscard]] std::optional<EditorObjectId> stableTarget() const noexcept {
    return std::visit(
        [](const auto &value) -> std::optional<EditorObjectId> {
          using Request = std::remove_cvref_t<decltype(value)>;
          if constexpr (std::is_same_v<Request, AddChildNodeRequest>)
            return value.parent.untyped();
          else if constexpr (std::is_same_v<Request, AddLightRequest>)
            return value.parent
                       ? std::optional<EditorObjectId>{value.parent->untyped()}
                       : std::optional<EditorObjectId>{value.scene.untyped()};
          else if constexpr (std::is_same_v<Request, DeleteNodeRequest>)
            return value.occurrence.node.untyped();
          else
            return value.source.node.untyped();
        },
        m_request);
  }

  [[nodiscard]] std::string beforeTypeTag() const {
    switch (m_kind) {
    case EditorCommandKind::addChildNode:
      return "mgif.add-child-node.request.v1";
    case EditorCommandKind::addLight:
      return "mgif.add-light.request.v1";
    case EditorCommandKind::duplicateNode:
      return "mgif.duplicate-node.request.v1";
    case EditorCommandKind::deleteNode:
      return "mgif.delete-node.request.v1";
    case EditorCommandKind::reparentNode:
      return "mgif.reparent-node.request.v1";
    default:
      return "mgif.structural.request.v1";
    }
  }

  [[nodiscard]] std::string finalTypeTag() const {
    switch (m_kind) {
    case EditorCommandKind::addChildNode:
      return "mgif.add-child-node.result.v1";
    case EditorCommandKind::addLight:
      return "mgif.add-light.result.v1";
    case EditorCommandKind::duplicateNode:
      return "mgif.duplicate-node.result.v1";
    case EditorCommandKind::deleteNode:
      return "mgif.delete-node.result.v1";
    case EditorCommandKind::reparentNode:
      return "mgif.reparent-node.result.v1";
    default:
      return "mgif.structural.result.v1";
    }
  }

  EditorCommandKind m_kind;
  StructuralRequest m_request;
  std::unique_ptr<PreparedStructuralMutation> m_prepared;
  std::unique_ptr<StructuralDelta> m_delta;
  std::string m_description;
  MutationToken m_preparedToken{};
  EditorSelectionSnapshot m_originalSelection{};
  EditorSelectionSnapshot m_executeSelection{};
  EditorSelectionSnapshot m_undoSelection{};
  EditorSelectionSnapshot m_redoSelection{};
  std::optional<DuplicateRootPosition> m_duplicatePosition;
  std::optional<NodeId> m_createdNode;
  std::uint64_t m_snapshotBytes{0};
  std::uint64_t m_stateRevision{0};
  CommandLifecycleState m_lifecycle{CommandLifecycleState::prepared};
  bool m_historyEnvelope{false};
};

[[nodiscard]] std::optional<std::uint64_t>
renameSnapshotBytes(const std::string &oldName, const std::string &newName,
                    const std::string &description) noexcept {
  std::uint64_t total = sizeof(RenameNodeCommand);
  if (!addBytes(total, kHierarchyCommandControlCharge) ||
      !addStringBytes(total, oldName) || !addStringBytes(total, newName) ||
      !addStringBytes(total, description))
    return std::nullopt;
  return total;
}

[[nodiscard]] std::optional<std::uint64_t>
structuralSnapshotBytes(const StructuralRequest &request,
                        const std::string &description,
                        std::uint64_t deltaBytes) noexcept {
  std::uint64_t total = sizeof(StructuralCommand);
  if (!addBytes(total, kHierarchyCommandControlCharge) ||
      !addStringBytes(total, description) || !addRequestBytes(total, request) ||
      !addBytes(total, deltaBytes))
    return std::nullopt;
  return total;
}

[[nodiscard]] std::string quotedDescription(std::string_view verb,
                                            std::string_view name) {
  std::string result;
  result.reserve(verb.size() + name.size() + 3u);
  result.append(verb);
  result.push_back(' ');
  result.push_back('\'');
  result.append(name);
  result.push_back('\'');
  return result;
}

[[nodiscard]] CommandPrepareResult
prepareStructuralCommand(EditorCommandContext &context,
                         StructuralRequest request, EditorCommandKind kind,
                         std::string description,
                         std::optional<DuplicateRootPosition> duplicatePosition,
                         EditorFaultInjector *faultInjector) noexcept {
  try {
    if (faultInjector &&
        faultInjector->shouldFail(EditorFaultPoint::allocation))
      return failure(MutationErrorCode::allocationFailed,
                     "hierarchy command allocation fault was injected");

    StructuralRequest preparationRequest = request;
    StructuralPrepareResult prepared =
        context.mutations.prepareStructural(std::move(preparationRequest));
    if (!prepared.succeeded()) {
      CommandPrepareResult result;
      result.error = prepared.error;
      result.message = std::move(prepared.message);
      return result;
    }
    if (prepared.historyPolicy != HistoryPolicy::undoable ||
        prepared.prepared->historyPolicy() != HistoryPolicy::undoable)
      return failure(MutationErrorCode::internalError,
                     "prepared hierarchy mutation has the wrong policy");
    const auto bytes = structuralSnapshotBytes(
        request, description, prepared.prepared->snapshotBytes());
    if (!bytes)
      return failure(MutationErrorCode::snapshotBudgetExceeded,
                     "hierarchy command snapshot byte count overflowed");

    CommandPrepareResult result;
    result.command = std::make_unique<StructuralCommand>(
        kind, std::move(request), std::move(prepared.prepared),
        std::move(description), context.mutations.token(),
        context.selection.snapshot(), duplicatePosition, *bytes);
    return result;
  } catch (const std::bad_alloc &) {
    return failure(MutationErrorCode::allocationFailed,
                   "hierarchy command allocation failed");
  } catch (...) {
    return failure(MutationErrorCode::internalError,
                   "hierarchy command preparation failed");
  }
}

} // namespace

CommandPrepareResult
prepareRenameNodeCommand(EditorCommandContext &context, NodeId node,
                         std::string newName,
                         EditorFaultInjector *faultInjector) noexcept {
  try {
    if (faultInjector &&
        faultInjector->shouldFail(EditorFaultPoint::allocation))
      return failure(MutationErrorCode::allocationFailed,
                     "rename command allocation fault was injected");
    if (node.authority() != context.document.documentAuthority())
      return failure(MutationErrorCode::crossDocument,
                     "rename target belongs to another document");
    if (!context.document.contains(node.untyped()))
      return failure(MutationErrorCode::staleId, "rename target is stale");
    std::string oldName(context.document.objectName(node.untyped()));
    std::string description = quotedDescription("Rename", oldName);
    const auto bytes = renameSnapshotBytes(oldName, newName, description);
    if (!bytes)
      return failure(MutationErrorCode::snapshotBudgetExceeded,
                     "rename command snapshot byte count overflowed");
    CommandPrepareResult result;
    result.command = std::make_unique<RenameNodeCommand>(
        node, std::move(oldName), std::move(newName), std::move(description),
        context.mutations.token(), context.selection.snapshot(), *bytes);
    return result;
  } catch (const std::bad_alloc &) {
    return failure(MutationErrorCode::allocationFailed,
                   "rename command allocation failed");
  } catch (...) {
    return failure(MutationErrorCode::internalError,
                   "rename command preparation failed");
  }
}

CommandPrepareResult
prepareAddChildNodeCommand(EditorCommandContext &context,
                           AddChildNodeRequest request,
                           EditorFaultInjector *faultInjector) noexcept {
  try {
    const std::string_view displayName = request.name.empty()
                                             ? std::string_view{"Node"}
                                             : std::string_view{request.name};
    return prepareStructuralCommand(context,
                                    StructuralRequest{std::move(request)},
                                    EditorCommandKind::addChildNode,
                                    quotedDescription("Add Node", displayName),
                                    std::nullopt, faultInjector);
  } catch (const std::bad_alloc &) {
    return failure(MutationErrorCode::allocationFailed,
                   "add-node command allocation failed");
  } catch (...) {
    return failure(MutationErrorCode::internalError,
                   "add-node command preparation failed");
  }
}

CommandPrepareResult
prepareAddLightCommand(EditorCommandContext &context, AddLightRequest request,
                       EditorFaultInjector *faultInjector) noexcept {
  try {
    std::string verb = "Add ";
    verb.append(request.properties.type);
    verb.append(" Light");
    const std::string description = quotedDescription(verb, request.lightName);
    return prepareStructuralCommand(
        context, StructuralRequest{std::move(request)},
        EditorCommandKind::addLight, description, std::nullopt, faultInjector);
  } catch (const std::bad_alloc &) {
    return failure(MutationErrorCode::allocationFailed,
                   "add-light command allocation failed");
  } catch (...) {
    return failure(MutationErrorCode::internalError,
                   "add-light command preparation failed");
  }
}

CommandPrepareResult
prepareDuplicateNodeCommand(EditorCommandContext &context,
                            DuplicateNodeRequest request,
                            EditorFaultInjector *faultInjector) noexcept {
  try {
    MutationErrorCode occurrenceError =
        validateOccurrence(context, request.source);
    if (occurrenceError != MutationErrorCode::none)
      return occurrenceFailure(
          occurrenceError, occurrenceError == MutationErrorCode::crossDocument
                               ? "duplicate source belongs to another document"
                               : "duplicate source occurrence is stale");
    if (request.parent) {
      occurrenceError = validateOccurrence(context, *request.parent);
      if (occurrenceError != MutationErrorCode::none)
        return occurrenceFailure(
            occurrenceError,
            occurrenceError == MutationErrorCode::crossDocument
                ? "duplicate parent belongs to another document"
                : "duplicate parent occurrence is stale");
    }
    std::string name(
        context.document.objectName(request.source.node.untyped()));
    const auto position =
        duplicateRootPosition(context.document, request.source.node);
    if (!position)
      return failure(MutationErrorCode::staleOccurrence,
                     "duplicate subtree cannot be captured");
    return prepareStructuralCommand(
        context, StructuralRequest{std::move(request)},
        EditorCommandKind::duplicateNode, quotedDescription("Duplicate", name),
        position, faultInjector);
  } catch (const std::bad_alloc &) {
    return failure(MutationErrorCode::allocationFailed,
                   "duplicate command allocation failed");
  } catch (...) {
    return failure(MutationErrorCode::internalError,
                   "duplicate command preparation failed");
  }
}

CommandPrepareResult
prepareDeleteNodeCommand(EditorCommandContext &context,
                         DeleteNodeRequest request,
                         EditorFaultInjector *faultInjector) noexcept {
  try {
    const MutationErrorCode occurrenceError =
        validateOccurrence(context, request.occurrence);
    if (occurrenceError != MutationErrorCode::none)
      return occurrenceFailure(
          occurrenceError, occurrenceError == MutationErrorCode::crossDocument
                               ? "delete source belongs to another document"
                               : "delete source occurrence is stale");
    std::string name(
        context.document.objectName(request.occurrence.node.untyped()));
    return prepareStructuralCommand(
        context, StructuralRequest{std::move(request)},
        EditorCommandKind::deleteNode, quotedDescription("Delete", name),
        std::nullopt, faultInjector);
  } catch (const std::bad_alloc &) {
    return failure(MutationErrorCode::allocationFailed,
                   "delete command allocation failed");
  } catch (...) {
    return failure(MutationErrorCode::internalError,
                   "delete command preparation failed");
  }
}

CommandPrepareResult
prepareReparentNodeCommand(EditorCommandContext &context,
                           ReparentNodeRequest request,
                           EditorFaultInjector *faultInjector) noexcept {
  try {
    MutationErrorCode occurrenceError =
        validateOccurrence(context, request.source);
    if (occurrenceError != MutationErrorCode::none)
      return occurrenceFailure(
          occurrenceError, occurrenceError == MutationErrorCode::crossDocument
                               ? "move source belongs to another document"
                               : "move source occurrence is stale");
    if (request.parent) {
      occurrenceError = validateOccurrence(context, *request.parent);
      if (occurrenceError != MutationErrorCode::none)
        return occurrenceFailure(
            occurrenceError, occurrenceError == MutationErrorCode::crossDocument
                                 ? "move parent belongs to another document"
                                 : "move parent occurrence is stale");
    }
    const SceneId targetScene = reparentTargetScene(request);
    if (targetScene.authority() != context.document.documentAuthority())
      return failure(MutationErrorCode::crossDocument,
                     "move target scene belongs to another document");
    if (!targetScene.valid() ||
        !context.document.contains(targetScene.untyped()))
      return failure(MutationErrorCode::staleOccurrence,
                     "move target scene is stale");
    if (request.parent && request.parent->scene != targetScene)
      return failure(MutationErrorCode::multiScenePolicy,
                     "move target scene differs from its parent occurrence");
    std::string name(
        context.document.objectName(request.source.node.untyped()));
    return prepareStructuralCommand(
        context, StructuralRequest{std::move(request)},
        EditorCommandKind::reparentNode, quotedDescription("Move", name),
        std::nullopt, faultInjector);
  } catch (const std::bad_alloc &) {
    return failure(MutationErrorCode::allocationFailed,
                   "move command allocation failed");
  } catch (...) {
    return failure(MutationErrorCode::internalError,
                   "move command preparation failed");
  }
}

} // namespace demo::editor
