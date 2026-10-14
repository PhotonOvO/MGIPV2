#include "SceneDeltaResourceJournal.h"

#include <algorithm>
#include <utility>

namespace demo::editor {

bool SceneDeltaResourceJournal::stage(SceneJournalResource resource) {
  if (m_state != SceneResourceJournalState::Staging || !resource.valid()) {
    return false;
  }
  const bool duplicate = std::any_of(
      m_resources.begin(), m_resources.end(), [&](const auto &existing) {
        return existing.diagnosticId == resource.diagnosticId ||
               existing.handle == resource.handle;
      });
  if (duplicate) {
    return false;
  }
  m_resources.push_back(resource);
  return true;
}

bool SceneDeltaResourceJournal::stage(
    std::span<const SceneJournalResource> resources) {
  if (m_state != SceneResourceJournalState::Staging) {
    return false;
  }
  for (std::size_t index = 0; index < resources.size(); ++index) {
    if (!resources[index].valid() ||
        std::any_of(m_resources.begin(), m_resources.end(),
                    [&](const SceneJournalResource &existing) {
                      return existing.diagnosticId ==
                                 resources[index].diagnosticId ||
                             existing.handle == resources[index].handle;
                    })) {
      return false;
    }
    for (std::size_t prior = 0; prior < index; ++prior) {
      if (resources[prior].diagnosticId == resources[index].diagnosticId ||
          resources[prior].handle == resources[index].handle) {
        return false;
      }
    }
  }
  m_resources.reserve(m_resources.size() + resources.size());
  m_resources.insert(m_resources.end(), resources.begin(), resources.end());
  return true;
}

bool SceneDeltaResourceJournal::adopt(
    std::vector<SceneJournalResource> &resources) noexcept {
  if (m_state != SceneResourceJournalState::Staging || !m_resources.empty()) {
    return false;
  }
  for (std::size_t index = 0; index < resources.size(); ++index) {
    if (!resources[index].valid()) {
      return false;
    }
    for (std::size_t prior = 0; prior < index; ++prior) {
      if (resources[prior].diagnosticId == resources[index].diagnosticId ||
          resources[prior].handle == resources[index].handle) {
        return false;
      }
    }
  }
  m_resources = std::move(resources);
  return true;
}

void SceneDeltaResourceJournal::reserveSubmissions(std::size_t count) {
  if (m_state != SceneResourceJournalState::Staging ||
      !m_submissionTokens.empty()) {
    return;
  }
  m_submissionTokens.reserve(count);
  m_retirementDependencies.reserve(count);
  m_unresolvedSubmissions.reserve(count);
}

bool SceneDeltaResourceJournal::canAdvanceRetirementFrontier(
    rhi::SubmissionToken token) const noexcept {
  if (!token.isValid()) {
    return false;
  }
  const auto existing = std::find_if(
      m_retirementDependencies.begin(), m_retirementDependencies.end(),
      [token](rhi::SubmissionToken candidate) {
        return candidate.queue == token.queue;
      });
  return existing == m_retirementDependencies.end() ||
         existing->value < token.value;
}

void SceneDeltaResourceJournal::advanceRetirementFrontier(
    rhi::SubmissionToken token) noexcept {
  const auto existing = std::find_if(
      m_retirementDependencies.begin(), m_retirementDependencies.end(),
      [token](rhi::SubmissionToken candidate) {
        return candidate.queue == token.queue;
      });
  if (existing == m_retirementDependencies.end()) {
    m_retirementDependencies.push_back(token);
  } else {
    *existing = token;
  }
}

bool SceneDeltaResourceJournal::captureSubmission(rhi::SubmissionToken token) {
  return captureSubmission(
      token, static_cast<std::uint32_t>(m_submissionTokens.size()));
}

bool SceneDeltaResourceJournal::captureSubmission(
    rhi::SubmissionToken token, std::uint32_t submissionIndex) {
  if ((m_state != SceneResourceJournalState::Staging &&
       m_state != SceneResourceJournalState::Submitted) ||
      submissionIndex != m_submissionTokens.size()) {
    return false;
  }

  // Reserve before mutating any vector so allocation failure cannot leave a
  // successfully returned token absent from the journal.
  m_submissionTokens.reserve(m_submissionTokens.size() + 1u);
  m_retirementDependencies.reserve(m_retirementDependencies.size() + 1u);
  m_unresolvedSubmissions.reserve(m_unresolvedSubmissions.size() + 1u);

  const bool accepted = canAdvanceRetirementFrontier(token);
  m_submissionTokens.push_back(token);
  if (accepted) {
    advanceRetirementFrontier(token);
  } else {
    // A successful submit is irrevocable. Every rejected report, including a
    // valid lower/equal token, requires recovery for this exact submission:
    // an earlier same-queue frontier cannot cover work submitted later.
    m_unresolvedSubmissions.push_back({submissionIndex, token});
  }
  m_state = SceneResourceJournalState::Submitted;
  return accepted;
}

std::vector<SceneResourceDiagnosticId>
SceneDeltaResourceJournal::diagnosticIds() const {
  std::vector<SceneResourceDiagnosticId> result;
  result.reserve(m_resources.size());
  for (const SceneJournalResource &resource : m_resources) {
    result.push_back(resource.diagnosticId);
  }
  return result;
}

SceneJournalCleanupResult SceneDeltaResourceJournal::rollbackBeforeSubmit(
    SceneResourceRetirementSink &sink) noexcept {
  SceneJournalCleanupResult result;
  if (m_state != SceneResourceJournalState::Staging) {
    result.rhiResult = rhi::RHIResult::fail(
        rhi::RHIErrorCode::invalidState,
        "pre-submit cleanup requested for a submitted journal");
    result.ownershipRetained = true;
    return result;
  }
  try {
    result.eventuallyDestroyed = diagnosticIds();
    result.rhiResult = sink.destroyUnpublishedResources(m_resources);
    if (!result.rhiResult.succeeded()) {
      result.ownershipRetained = true;
      return result;
    }
    result.physicallyDestroyed = result.eventuallyDestroyed;
    m_resources.clear();
    m_state = SceneResourceJournalState::ImmediateCleanupComplete;
  } catch (...) {
    result.rhiResult = rhi::RHIResult::fail(
        rhi::RHIErrorCode::outOfMemory,
        "pre-submit cleanup diagnostics could not be allocated");
    result.ownershipRetained = true;
  }
  return result;
}

SceneJournalCleanupResult SceneDeltaResourceJournal::rollbackAfterSubmit(
    SceneResourceRetirementSink &sink) noexcept {
  SceneJournalCleanupResult result;
  if (m_state != SceneResourceJournalState::Submitted &&
      m_state != SceneResourceJournalState::DeferredCleanupPending) {
    result.rhiResult = rhi::RHIResult::fail(
        rhi::RHIErrorCode::invalidState,
        "deferred cleanup requested without submission dependencies");
    result.ownershipRetained = true;
    return result;
  }
  try {
    result.logicallyRetired = diagnosticIds();
    result.eventuallyDestroyed = result.logicallyRetired;

    while (!m_unresolvedSubmissions.empty()) {
      const UnresolvedSubmission unresolved =
          m_unresolvedSubmissions.front();
      const rhi::Result<rhi::SubmissionToken> recovered =
          sink.recoverSubmissionDependency(
              m_resources, unresolved.submissionIndex,
              unresolved.reportedToken);
      if (!recovered.succeeded() || !recovered.value.isValid()) {
        result.rhiResult = rhi::RHIResult::fail(
            recovered.succeeded() ? rhi::RHIErrorCode::invalidState
                                  : recovered.error.code,
            recovered.succeeded()
                ? "backend could not recover a valid submission dependency"
                : recovered.error.message);
        result.ownershipRetained = true;
        m_state = SceneResourceJournalState::DeferredCleanupPending;
        return result;
      }
      if (!canAdvanceRetirementFrontier(recovered.value)) {
        result.rhiResult = rhi::RHIResult::fail(
            rhi::RHIErrorCode::invalidState,
            "backend recovery did not produce a strictly advancing queue "
            "frontier");
        result.ownershipRetained = true;
        m_state = SceneResourceJournalState::DeferredCleanupPending;
        return result;
      }
      advanceRetirementFrontier(recovered.value);
      m_unresolvedSubmissions.erase(m_unresolvedSubmissions.begin());
    }

    result.rhiResult =
        sink.enqueueResourceRetirement(m_resources, m_retirementDependencies);
    if (!result.rhiResult.succeeded()) {
      result.ownershipRetained = true;
      m_state = SceneResourceJournalState::DeferredCleanupPending;
      return result;
    }
    m_resources.clear();
    m_state = SceneResourceJournalState::DeferredCleanupComplete;
  } catch (...) {
    result.rhiResult = rhi::RHIResult::fail(
        rhi::RHIErrorCode::outOfMemory,
        "deferred cleanup diagnostics could not be allocated");
    result.ownershipRetained = true;
    m_state = SceneResourceJournalState::DeferredCleanupPending;
  }
  return result;
}

void SceneDeltaResourceJournal::releasePublished() noexcept {
  m_resources.clear();
  m_submissionTokens.clear();
  m_retirementDependencies.clear();
  m_unresolvedSubmissions.clear();
  m_state = SceneResourceJournalState::Published;
}

} // namespace demo::editor
