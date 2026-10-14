#include "ScenePicking.h"

#include <algorithm>
#include <cmath>
#include <exception>
#include <utility>

namespace demo::editor {
namespace {

[[nodiscard]] EditorPickResult baseResult(
    const EditorPickRequest &request) noexcept {
  EditorPickResult result;
  result.serial = request.serial;
  result.intent = request.intent;
  result.fence = request.fence;
  result.occurrenceGeneration = request.occurrenceGeneration;
  return result;
}

[[nodiscard]] EditorPickResult miss(const EditorPickRequest &request,
                                    PickResultReason reason) noexcept {
  EditorPickResult result = baseResult(request);
  result.completion = PickCompletion::miss;
  result.reason = reason;
  return result;
}

[[nodiscard]] EditorPickResult error(const EditorPickRequest &request,
                                     PickResultReason reason) noexcept {
  EditorPickResult result = baseResult(request);
  result.completion = PickCompletion::error;
  result.reason = reason == PickResultReason::backendRejected
                      ? reason
                      : PickResultReason::backendFailure;
  return result;
}

[[nodiscard]] bool hitMatchesRay(const EditorPickRequest &request,
                                 const EditorWorldPoint &point,
                                 double distance) noexcept {
  if (!point.finite() || !std::isfinite(distance) || distance <= 0.0) {
    return false;
  }
  const EditorWorldPoint expected{
      request.ray.origin.x + request.ray.direction.x * distance,
      request.ray.origin.y + request.ray.direction.y * distance,
      request.ray.origin.z + request.ray.direction.z * distance};
  const double dx = point.x - expected.x;
  const double dy = point.y - expected.y;
  const double dz = point.z - expected.z;
  const double error_squared = dx * dx + dy * dy + dz * dz;
  const double tolerance = 1.0e-5 * std::max(1.0, distance);
  return std::isfinite(error_squared) &&
         error_squared <= tolerance * tolerance;
}

} // namespace

ScenePicking::ScenePicking(
    IEditorPickingPassBackend &backend,
    IScenePickingTerminalSink &terminalSink) noexcept
    : m_backend(&backend), m_terminalSink(&terminalSink), m_pass(backend) {}

ScenePicking::~ScenePicking() noexcept {
  m_pass.shutdown(this, &ScenePicking::shutdownCompletion);
  if (!m_snapshots.empty()) {
    std::terminate();
  }
}

PickSubmitResult ScenePicking::submit(
    const EditorPickRequest &request,
    SceneRenderOccurrenceSnapshot snapshot) {
  if (!request.valid()) {
    return {PickSubmitStatus::rejectedInvalid, request.serial};
  }
  if (!snapshot || snapshot->generation() != request.occurrenceGeneration) {
    return {PickSubmitStatus::rejectedSnapshot, request.serial};
  }
  if (!sameEditorSceneFence(snapshot->fence(), request.fence.scene)) {
    return {PickSubmitStatus::rejectedFence, request.serial};
  }
  if (request.serial <= m_lastAcceptedSerial) {
    return {PickSubmitStatus::rejectedSerial, request.serial};
  }

  m_snapshots.push_back(PendingSnapshot{request.serial, std::move(snapshot)});
  try {
    const PickSubmitResult submitted = m_pass.submit(request);
    if (!submitted.accepted()) {
      m_snapshots.pop_back();
      return submitted;
    }
  } catch (...) {
    m_snapshots.pop_back();
    throw;
  }
  m_lastAcceptedSerial = request.serial;
  return {PickSubmitStatus::accepted, request.serial};
}

void ScenePicking::poll(std::vector<EditorPickResult> &out) {
  // Reserve before the pass consumes any terminal readback. If allocation
  // fails, every backend completion remains pollable and every snapshot stays
  // retained for the retry.
  out.reserve(out.size() + m_snapshots.size());
  std::vector<EditorPickingPassCompletion> completions;
  completions.reserve(m_snapshots.size());
  m_pass.poll(completions);
  for (const EditorPickingPassCompletion &completion : completions) {
    const std::size_t index = pendingIndex(completion.request.serial);
    if (index == m_snapshots.size() || !m_snapshots[index].snapshot) {
      ++m_stats.ignoredUnknownCompletions;
      continue;
    }
    EditorPickResult result =
        resolve(completion, *m_snapshots[index].snapshot);
    switch (result.completion) {
    case PickCompletion::hit:
      ++m_stats.hitResults;
      break;
    case PickCompletion::miss:
      ++m_stats.missResults;
      break;
    case PickCompletion::error:
      ++m_stats.errorResults;
      break;
    case PickCompletion::cancelled:
      ++m_stats.cancelledResults;
      break;
    }
    out.push_back(std::move(result));
    m_snapshots.erase(m_snapshots.begin() +
                      static_cast<std::ptrdiff_t>(index));
  }
}

void ScenePicking::cancelAuthority(EditorDocumentAuthority authority) {
  m_pass.cancelAuthority(authority);
}

void ScenePicking::cancelAll() { m_pass.cancelAll(); }

EditorPickResult ScenePicking::resolve(
    const EditorPickingPassCompletion &completion,
    const SceneRenderOccurrenceMapping &snapshot) const noexcept {
  const EditorPickRequest &request = completion.request;
  switch (completion.status) {
  case EditorPickingBackendReadbackStatus::cancelled: {
    EditorPickResult result = baseResult(request);
    result.completion = PickCompletion::cancelled;
    result.reason = PickResultReason::cancelled;
    return result;
  }
  case EditorPickingBackendReadbackStatus::error:
    return error(request, completion.reason);
  case EditorPickingBackendReadbackStatus::miss:
    return miss(request, completion.reason == PickResultReason::none
                             ? PickResultReason::background
                             : completion.reason);
  case EditorPickingBackendReadbackStatus::sampled:
    break;
  }

  const std::optional<RenderOccurrenceRecord> record =
      snapshot.resolveFramebufferToken(completion.token);
  if (!record) {
    return miss(request, PickResultReason::invalidToken);
  }
  if (!record->visible) {
    return miss(request, PickResultReason::hiddenOccurrence);
  }
  if (!completion.worldHit || !completion.rayDistance ||
      !hitMatchesRay(request, *completion.worldHit,
                     *completion.rayDistance)) {
    return miss(request, PickResultReason::invalidHit);
  }

  EditorPickResult result = baseResult(request);
  result.completion = PickCompletion::hit;
  result.reason = PickResultReason::none;
  result.occurrence = record->id;
  result.worldHit = completion.worldHit;
  result.rayDistance = completion.rayDistance;
  return result;
}

std::size_t ScenePicking::pendingIndex(std::uint64_t serial) const noexcept {
  for (std::size_t index = 0; index < m_snapshots.size(); ++index) {
    if (m_snapshots[index].serial == serial) {
      return index;
    }
  }
  return m_snapshots.size();
}

std::size_t ScenePicking::pendingCount() const noexcept {
  return m_snapshots.size();
}

const EditorPickingPassStats &ScenePicking::passStats() const noexcept {
  return m_pass.stats();
}

const ScenePickingStats &ScenePicking::stats() const noexcept {
  return m_stats;
}

void ScenePicking::shutdownCompletion(
    void *context,
    const EditorPickingPassCompletion &completion) noexcept {
  if (context == nullptr) {
    std::terminate();
  }
  static_cast<ScenePicking *>(context)->deliverShutdownCompletion(completion);
}

void ScenePicking::deliverShutdownCompletion(
    const EditorPickingPassCompletion &completion) noexcept {
  const std::size_t index = pendingIndex(completion.request.serial);
  if (index == m_snapshots.size() || !m_snapshots[index].snapshot ||
      m_terminalSink == nullptr) {
    std::terminate();
  }
  const EditorPickResult result =
      resolve(completion, *m_snapshots[index].snapshot);
  if (!result.terminalPayloadValid()) {
    std::terminate();
  }
  switch (result.completion) {
  case PickCompletion::hit:
    ++m_stats.hitResults;
    break;
  case PickCompletion::miss:
    ++m_stats.missResults;
    break;
  case PickCompletion::error:
    ++m_stats.errorResults;
    break;
  case PickCompletion::cancelled:
    ++m_stats.cancelledResults;
    break;
  }
  m_terminalSink->onTerminal(result);
  m_snapshots.erase(m_snapshots.begin() +
                    static_cast<std::ptrdiff_t>(index));
}

} // namespace demo::editor
