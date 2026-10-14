#include <mgif/scene_editor/commands/ContinuousEdit.h>

#include <mgif/scene_editor/commands/PropertyCommands.h>
#include <mgif/scene_editor/commands/TransformCommands.h>

#include <memory>
#include <new>
#include <utility>

namespace demo::editor {
namespace {

[[nodiscard]] ContinuousValueKind
valueKindFor(ContinuousEditKind kind) noexcept {
  switch (kind) {
  case ContinuousEditKind::inspectorTransform:
  case ContinuousEditKind::gizmoTransform:
    return ContinuousValueKind::nodeTransform;
  case ContinuousEditKind::material:
    return ContinuousValueKind::material;
  case ContinuousEditKind::light:
    return ContinuousValueKind::light;
  }
  return ContinuousValueKind::nodeTransform;
}

} // namespace

ContinuousEditSession::ContinuousEditSession(
    EditorCommandContext &context, EditorHistory &history,
    EditorFaultInjector *faultInjector) noexcept
    : m_context(context), m_history(history), m_faultInjector(faultInjector) {}

ContinuousResult ContinuousEditSession::beginTransform(ContinuousEditKind kind,
                                                       NodeId node) {
  if (kind != ContinuousEditKind::inspectorTransform &&
      kind != ContinuousEditKind::gizmoTransform) {
    return currentResult(ContinuousStatus::rejected,
                         MutationErrorCode::invalidSnapshot);
  }
  if (m_state == ContinuousEditState::previewing) {
    return currentResult(ContinuousStatus::rejected,
                         MutationErrorCode::staleTransaction);
  }
  return beginPrepared(kind, prepareContinuousTransformCommand(
                                 m_context, node, m_faultInjector));
}

ContinuousResult ContinuousEditSession::beginMaterial(MaterialId material) {
  if (m_state == ContinuousEditState::previewing) {
    return currentResult(ContinuousStatus::rejected,
                         MutationErrorCode::staleTransaction);
  }
  return beginPrepared(
      ContinuousEditKind::material,
      prepareContinuousMaterialCommand(m_context, material, m_faultInjector));
}

ContinuousResult ContinuousEditSession::beginLight(LightId light) {
  if (m_state == ContinuousEditState::previewing) {
    return currentResult(ContinuousStatus::rejected,
                         MutationErrorCode::staleTransaction);
  }
  return beginPrepared(
      ContinuousEditKind::light,
      prepareContinuousLightCommand(m_context, light, m_faultInjector));
}

ContinuousResult ContinuousEditSession::preview(const NodeTransform &value) {
  return previewValue(ContinuousCommandValue{value});
}

ContinuousResult ContinuousEditSession::preview(const MaterialSnapshot &value) {
  return previewValue(ContinuousCommandValue{value});
}

ContinuousResult ContinuousEditSession::preview(const LightSnapshot &value) {
  return previewValue(ContinuousCommandValue{value});
}

ContinuousResult
ContinuousEditSession::finishFrame(const ContinuousFrameSignals &signals) {
  if (!active())
    return currentResult(ContinuousStatus::noOp, MutationErrorCode::none);
  if (m_hasFinalizedFrame && signals.applicationFrame <= m_lastFinalizedFrame) {
    return currentResult(ContinuousStatus::noOp, MutationErrorCode::none);
  }
  m_lastFinalizedFrame = signals.applicationFrame;
  m_hasFinalizedFrame = true;

  if (m_state == ContinuousEditState::previewing && !isGizmo() &&
      signals.inspectorDeactivatedAfterEdit) {
    return finalizeCommit();
  }

  if (isGizmo() && signals.escapePressed) {
    abandonRetainingPreview(true);
    return currentResult(ContinuousStatus::abandonedRetainingPreview,
                         MutationErrorCode::none);
  }

  if (m_state == ContinuousEditState::suspendedUnfocused) {
    if (signals.selectionDetached) {
      abandonRetainingPreview(true);
      return currentResult(ContinuousStatus::abandonedRetainingPreview,
                           MutationErrorCode::none);
    }
    if (signals.focusLost || !signals.windowFocused)
      return currentResult(ContinuousStatus::suspended,
                           MutationErrorCode::none);
    return currentResult(ContinuousStatus::noOp, MutationErrorCode::none);
  }

  if (signals.focusLost || !signals.windowFocused) {
    m_state = ContinuousEditState::suspendedUnfocused;
    if (signals.selectionDetached) {
      abandonRetainingPreview(true);
      return currentResult(ContinuousStatus::abandonedRetainingPreview,
                           MutationErrorCode::none);
    }
    return currentResult(ContinuousStatus::suspended, MutationErrorCode::none);
  }

  if (signals.selectionDetached) {
    abandonRetainingPreview(true);
    return currentResult(ContinuousStatus::abandonedRetainingPreview,
                         MutationErrorCode::none);
  }

  if (isGizmo() && signals.physicalReleaseDelivered &&
      !signals.syntheticRelease) {
    return finalizeCommit();
  }

  return currentResult(ContinuousStatus::noOp, MutationErrorCode::none);
}

ContinuousEditState ContinuousEditSession::state() const noexcept {
  return m_state;
}

bool ContinuousEditSession::active() const noexcept {
  return m_state == ContinuousEditState::previewing ||
         m_state == ContinuousEditState::suspendedUnfocused;
}

ContinuousResult
ContinuousEditSession::beginPrepared(ContinuousEditKind kind,
                                     ContinuousCommandPrepareResult prepared) {
  if (!prepared.succeeded()) {
    ContinuousResult result =
        currentResult(ContinuousStatus::rejected, prepared.error);
    result.message = std::move(prepared.message);
    return result;
  }
  if (prepared.command->valueKind() != valueKindFor(kind)) {
    return currentResult(ContinuousStatus::rejected,
                         MutationErrorCode::internalError);
  }

  m_preparedCommand = std::move(prepared.command);
  m_kind = kind;
  m_state = ContinuousEditState::previewing;
  m_lastFinalizedFrame = 0;
  m_hasFinalizedFrame = false;
  return currentResult(ContinuousStatus::began, MutationErrorCode::none);
}

ContinuousResult
ContinuousEditSession::previewValue(const ContinuousCommandValue &value) {
  if (m_state == ContinuousEditState::suspendedUnfocused)
    return currentResult(ContinuousStatus::noOp, MutationErrorCode::none);
  if (m_state != ContinuousEditState::previewing || !m_preparedCommand)
    return currentResult(ContinuousStatus::noOp, MutationErrorCode::none);
  if (value.index() !=
      static_cast<std::size_t>(m_preparedCommand->valueKind())) {
    return currentResult(ContinuousStatus::rejected,
                         MutationErrorCode::invalidSnapshot);
  }

  const SnapshotByteResult bytes =
      m_preparedCommand->prospectiveSnapshotBytes(value);
  if (!bytes.succeeded())
    return currentResult(ContinuousStatus::rejected, bytes.error);
  const HistoryAdmission admission =
      m_history.admitProspectivePush(bytes.bytes);
  if (!admission.admitted)
    return currentResult(ContinuousStatus::rejected, admission.error);

  CommandResult previewResult;
  try {
    previewResult = m_preparedCommand->applyPreview(m_context, value);
  } catch (const std::bad_alloc &) {
    return currentResult(ContinuousStatus::rejected,
                         MutationErrorCode::allocationFailed);
  } catch (...) {
    return currentResult(ContinuousStatus::rejected,
                         MutationErrorCode::internalError);
  }

  ContinuousResult result;
  result.state = m_state;
  result.error = previewResult.error;
  result.message = std::move(previewResult.message);
  if (previewResult.status == CommandStatus::applied) {
    result.status = ContinuousStatus::previewApplied;
    result.error = MutationErrorCode::none;
    result.previewCommand = previewResult;
    return result;
  }
  if (previewResult.status == CommandStatus::unchanged) {
    result.status = ContinuousStatus::previewUnchanged;
    result.error = MutationErrorCode::none;
    return result;
  }

  result.status = ContinuousStatus::rejected;
  if (previewResult.error == MutationErrorCode::staleId ||
      previewResult.error == MutationErrorCode::crossDocument) {
    abandonRetainingPreview(false);
    result.state = m_state;
  }
  return result;
}

ContinuousResult ContinuousEditSession::finalizeCommit() {
  if (!m_preparedCommand)
    return currentResult(ContinuousStatus::noOp, MutationErrorCode::none);

  if (m_preparedCommand->initialEqualsFinal()) {
    m_preparedCommand.reset();
    m_state = ContinuousEditState::committed;
    return currentResult(ContinuousStatus::committed, MutationErrorCode::none);
  }

  if (m_faultInjector != nullptr &&
      m_faultInjector->shouldFail(EditorFaultPoint::preCandidatePublish)) {
    m_preparedCommand.reset();
    m_state = ContinuousEditState::abandoned;
    return currentResult(ContinuousStatus::rejected,
                         MutationErrorCode::internalError);
  }

  HistoryResult history =
      m_history.commitExecuted(std::move(m_preparedCommand));
  ContinuousResult result;
  result.error = history.command.error;
  result.message = std::move(history.command.message);
  if (history.succeeded()) {
    m_state = ContinuousEditState::committed;
    result.status = ContinuousStatus::committed;
    result.error = MutationErrorCode::none;
  } else {
    m_state = ContinuousEditState::abandoned;
    result.status = ContinuousStatus::rejected;
  }
  result.state = m_state;
  result.history.emplace(std::move(history));
  return result;
}

ContinuousResult
ContinuousEditSession::currentResult(ContinuousStatus status,
                                     MutationErrorCode error) const {
  ContinuousResult result;
  result.status = status;
  result.state = m_state;
  result.error = error;
  return result;
}

bool ContinuousEditSession::isGizmo() const noexcept {
  return m_kind == ContinuousEditKind::gizmoTransform;
}

void ContinuousEditSession::abandonRetainingPreview(
    bool clearSelection) noexcept {
  if (clearSelection)
    m_context.selection.clear();
  m_preparedCommand.reset();
  m_state = ContinuousEditState::abandoned;
}

void ContinuousEditSession::abandonForSessionRetirement() noexcept {
  if (active()) {
    m_preparedCommand.reset();
    m_state = ContinuousEditState::abandoned;
  }
}

} // namespace demo::editor
