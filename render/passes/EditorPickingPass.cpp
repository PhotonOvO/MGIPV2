#include "EditorPickingPass.h"

#include <algorithm>
#include <exception>
#include <limits>

namespace demo::editor {
namespace {

struct NormalizedBackendReadback final {
  EditorPickingBackendReadbackStatus status{
      EditorPickingBackendReadbackStatus::error};
  PickResultReason reason{PickResultReason::backendFailure};
};

[[nodiscard]] bool validMissReason(PickResultReason reason) noexcept {
  switch (reason) {
  case PickResultReason::background:
  case PickResultReason::invalidRequest:
  case PickResultReason::fenceMismatch:
  case PickResultReason::snapshotMismatch:
  case PickResultReason::invalidToken:
  case PickResultReason::hiddenOccurrence:
  case PickResultReason::invalidHit:
    return true;
  case PickResultReason::none:
  case PickResultReason::backendRejected:
  case PickResultReason::backendFailure:
  case PickResultReason::cancelled:
    return false;
  }
  return false;
}

[[nodiscard]] NormalizedBackendReadback normalizeReadback(
    EditorPickingBackendReadbackStatus status,
    PickResultReason reason) noexcept {
  switch (status) {
  case EditorPickingBackendReadbackStatus::sampled:
    return reason == PickResultReason::none
               ? NormalizedBackendReadback{status, reason}
               : NormalizedBackendReadback{};
  case EditorPickingBackendReadbackStatus::miss:
    if (reason == PickResultReason::none) {
      return {status, PickResultReason::background};
    }
    return validMissReason(reason) ? NormalizedBackendReadback{status, reason}
                                   : NormalizedBackendReadback{};
  case EditorPickingBackendReadbackStatus::error:
    return {status, reason == PickResultReason::backendRejected
                        ? reason
                        : PickResultReason::backendFailure};
  case EditorPickingBackendReadbackStatus::cancelled:
    return {status, PickResultReason::cancelled};
  }
  return {};
}

} // namespace

EditorPickingPass::EditorPickingPass(
    IEditorPickingPassBackend &backend) noexcept
    : m_backend(&backend) {}

EditorPickingPass::~EditorPickingPass() noexcept {
  if (!m_pending.empty() || !m_ready.empty()) {
    std::terminate();
  }
}

PickSubmitResult EditorPickingPass::submit(const EditorPickRequest &request) {
  if (m_backend == nullptr || m_shutdown || !request.valid()) {
    ++m_stats.rejectedRequests;
    return {PickSubmitStatus::rejectedInvalid, request.serial};
  }
  if (request.serial <= m_lastAcceptedSerial) {
    ++m_stats.rejectedRequests;
    return {PickSubmitStatus::rejectedSerial, request.serial};
  }

  // Reserve ownership before the backend can create in-flight resources.
  m_pending.push_back(Pending{request, {}, {}});
  ++m_stats.submitCalls;
  const EditorPickingBackendSubmission submission = m_backend->enqueue(request);
  if (!submission.accepted || !submission.ticket.valid() ||
      !submission.submission.isValid()) {
    if (submission.ticket.valid()) {
      m_backend->cancel(submission.ticket);
      if (submission.submission.isValid()) {
        m_backend->retireReadback(submission.ticket, submission.submission);
        ++m_stats.retirementCalls;
      }
    }
    m_pending.pop_back();
    ++m_stats.rejectedRequests;
    return {PickSubmitStatus::rejectedBackend, request.serial};
  }
  if (pendingTicketIndex(submission.ticket) != m_pending.size()) {
    // A duplicate ticket cannot be cancelled without affecting its existing
    // owner. Retire the newly reported submission and reject this request.
    m_backend->retireReadback(submission.ticket, submission.submission);
    ++m_stats.retirementCalls;
    m_pending.pop_back();
    ++m_stats.rejectedRequests;
    return {PickSubmitStatus::rejectedBackend, request.serial};
  }

  Pending &pending = m_pending.back();
  pending.ticket = submission.ticket;
  pending.submission = submission.submission;
  m_lastAcceptedSerial = request.serial;
  ++m_stats.acceptedRequests;
  return {PickSubmitStatus::accepted, request.serial};
}

void EditorPickingPass::poll(
    std::vector<EditorPickingPassCompletion> &out) {
  pump();
  if (m_ready.empty()) {
    return;
  }
  out.reserve(out.size() + m_ready.size());
  for (EditorPickingPassCompletion &completion : m_ready) {
    out.push_back(std::move(completion));
  }
  m_ready.clear();
}

void EditorPickingPass::pump() {
  if (m_backend == nullptr) {
    return;
  }
  // Allocation happens before consuming backend completions, so a failure
  // leaves every request pollable and every readback resource retained.
  m_ready.reserve(m_ready.size() + m_pending.size());
  while (true) {
    const std::optional<EditorPickingBackendReadback> readback =
        m_backend->pollOne();
    if (!readback) {
      break;
    }
    const std::size_t index =
        pendingIndex(readback->ticket, readback->submission);
    if (index == m_pending.size()) {
      ++m_stats.ignoredUnknownOrDuplicateReadbacks;
      continue;
    }

    Pending pending = m_pending[index];
    EditorPickingPassCompletion completion;
    completion.request = pending.request;
    const NormalizedBackendReadback normalized =
        normalizeReadback(readback->status, readback->reason);
    completion.status = normalized.status;
    completion.token = readback->token;
    completion.worldHit = readback->worldHit;
    completion.rayDistance = readback->rayDistance;
    completion.reason = normalized.reason;
    m_ready.push_back(std::move(completion));

    m_backend->retireReadback(pending.ticket, pending.submission);
    ++m_stats.retirementCalls;
    ++m_stats.terminalCompletions;
    if (readback->status == EditorPickingBackendReadbackStatus::cancelled) {
      ++m_stats.cancelledRequests;
    }
    m_pending.erase(m_pending.begin() + static_cast<std::ptrdiff_t>(index));
  }
}

void EditorPickingPass::cancelAuthority(EditorDocumentAuthority authority) {
  if (!authority.valid() || m_backend == nullptr) {
    return;
  }
  std::size_t count = 0;
  for (const Pending &pending : m_pending) {
    if (pending.request.fence.scene.authority == authority) {
      ++count;
    }
  }
  m_ready.reserve(m_ready.size() + count);
  for (std::size_t index = 0; index < m_pending.size();) {
    Pending pending = m_pending[index];
    if (pending.request.fence.scene.authority != authority) {
      ++index;
      continue;
    }
    m_backend->cancel(pending.ticket);
    m_backend->retireReadback(pending.ticket, pending.submission);
    m_ready.push_back(EditorPickingPassCompletion{
        pending.request, EditorPickingBackendReadbackStatus::cancelled, {},
        std::nullopt, std::nullopt, PickResultReason::cancelled});
    ++m_stats.retirementCalls;
    ++m_stats.terminalCompletions;
    ++m_stats.cancelledRequests;
    m_pending.erase(m_pending.begin() + static_cast<std::ptrdiff_t>(index));
  }
}

void EditorPickingPass::cancelAll() {
  if (m_backend == nullptr || m_pending.empty()) {
    return;
  }
  m_ready.reserve(m_ready.size() + m_pending.size());
  for (const Pending &pending : m_pending) {
    m_backend->cancel(pending.ticket);
    m_backend->retireReadback(pending.ticket, pending.submission);
    m_ready.push_back(EditorPickingPassCompletion{
        pending.request, EditorPickingBackendReadbackStatus::cancelled, {},
        std::nullopt, std::nullopt, PickResultReason::cancelled});
    ++m_stats.retirementCalls;
    ++m_stats.terminalCompletions;
    ++m_stats.cancelledRequests;
  }
  m_pending.clear();
}

void EditorPickingPass::shutdown(
    void *context, EditorPickingPassShutdownCallback callback) noexcept {
  if (m_shutdown) {
    return;
  }
  if (callback == nullptr) {
    std::terminate();
  }
  m_shutdown = true;
  for (const EditorPickingPassCompletion &completion : m_ready) {
    callback(context, completion);
  }
  m_ready.clear();
  for (const Pending &pending : m_pending) {
    m_backend->cancel(pending.ticket);
    m_backend->retireReadback(pending.ticket, pending.submission);
    ++m_stats.retirementCalls;
    ++m_stats.terminalCompletions;
    ++m_stats.cancelledRequests;
    const EditorPickingPassCompletion completion{
        pending.request, EditorPickingBackendReadbackStatus::cancelled, {},
        std::nullopt, std::nullopt, PickResultReason::cancelled};
    callback(context, completion);
  }
  m_pending.clear();
}

std::size_t EditorPickingPass::pendingTicketIndex(
    EditorPickingPassTicket ticket) const noexcept {
  for (std::size_t index = 0; index < m_pending.size(); ++index) {
    if (m_pending[index].ticket == ticket) {
      return index;
    }
  }
  return m_pending.size();
}

std::size_t EditorPickingPass::pendingIndex(
    EditorPickingPassTicket ticket,
    rhi::SubmissionToken submission) const noexcept {
  for (std::size_t index = 0; index < m_pending.size(); ++index) {
    if (m_pending[index].ticket == ticket &&
        m_pending[index].submission == submission) {
      return index;
    }
  }
  return m_pending.size();
}

std::size_t EditorPickingPass::pendingCount() const noexcept {
  return m_pending.size();
}

const EditorPickingPassStats &EditorPickingPass::stats() const noexcept {
  return m_stats;
}

} // namespace demo::editor
