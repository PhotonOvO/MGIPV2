#pragma once

#include "../../rhi/RHIResourceLifetime.h"
#include "../../rhi/RHIResult.h"

#include <compare>
#include <cstddef>
#include <cstdint>
#include <span>
#include <vector>

namespace demo::editor {

struct SceneResourceDiagnosticId final {
  std::uint64_t value{0};

  [[nodiscard]] constexpr bool valid() const noexcept { return value != 0u; }
  auto operator<=>(const SceneResourceDiagnosticId &) const = default;
};

struct SceneJournalResource final {
  SceneResourceDiagnosticId diagnosticId{};
  rhi::ResourceHandle handle{};

  [[nodiscard]] constexpr bool valid() const noexcept {
    return diagnosticId.valid() && handle.isValid();
  }

  bool operator==(const SceneJournalResource &) const = default;
};

enum class SceneResourceJournalState : std::uint8_t {
  Staging = 0,
  Submitted = 1,
  Published = 2,
  ImmediateCleanupComplete = 3,
  DeferredCleanupComplete = 4,
  DeferredCleanupPending = 5,
};

struct SceneJournalCleanupResult final {
  rhi::RHIResult rhiResult{};
  std::vector<SceneResourceDiagnosticId> logicallyRetired;
  std::vector<SceneResourceDiagnosticId> physicallyDestroyed;
  std::vector<SceneResourceDiagnosticId> eventuallyDestroyed;
  bool ownershipRetained{false};

  [[nodiscard]] bool succeeded() const noexcept {
    return rhiResult.succeeded() && !ownershipRetained;
  }
};

class SceneResourceRetirementSink {
public:
  virtual ~SceneResourceRetirementSink() = default;

  [[nodiscard]] virtual rhi::RHIResult destroyUnpublishedResources(
      std::span<const SceneJournalResource> resources) noexcept = 0;
  [[nodiscard]] virtual rhi::RHIResult enqueueResourceRetirement(
      std::span<const SceneJournalResource> resources,
      std::span<const rhi::SubmissionToken> dependencies) noexcept = 0;
  // A backend that reported success has submitted work even when the token it
  // returned violates the public validity/monotonicity contract. Recover the
  // exact fence dependency for that already-submitted ordinal. The recovered
  // token must cover that submission and strictly advance any same-queue
  // retirement frontier already recorded by this journal; an equal or lower
  // value cannot prove completion of work submitted later. It may also cover
  // later work on the same queue. Recovery must not submit again or wait for
  // the device.
  [[nodiscard]] virtual rhi::Result<rhi::SubmissionToken>
  recoverSubmissionDependency(
      std::span<const SceneJournalResource>,
      std::uint32_t,
      rhi::SubmissionToken) noexcept {
    return rhi::Result<rhi::SubmissionToken>::fail(
        rhi::RHIErrorCode::invalidState,
        "submission dependency recovery is unavailable");
  }
};

class SceneDeltaResourceJournal final {
public:
  [[nodiscard]] bool stage(SceneJournalResource resource);
  [[nodiscard]] bool stage(std::span<const SceneJournalResource> resources);
  [[nodiscard]] bool
  adopt(std::vector<SceneJournalResource> &resources) noexcept;
  void reserveSubmissions(std::size_t count);
  [[nodiscard]] bool captureSubmission(rhi::SubmissionToken token);
  [[nodiscard]] bool captureSubmission(rhi::SubmissionToken token,
                                       std::uint32_t submissionIndex);

  [[nodiscard]] SceneJournalCleanupResult
  rollbackBeforeSubmit(SceneResourceRetirementSink &sink) noexcept;
  [[nodiscard]] SceneJournalCleanupResult
  rollbackAfterSubmit(SceneResourceRetirementSink &sink) noexcept;

  void releasePublished() noexcept;

  [[nodiscard]] SceneResourceJournalState state() const noexcept {
    return m_state;
  }
  [[nodiscard]] bool empty() const noexcept { return m_resources.empty(); }
  [[nodiscard]] std::span<const SceneJournalResource>
  resources() const noexcept {
    return m_resources;
  }
  [[nodiscard]] std::span<const rhi::SubmissionToken>
  submissionTokens() const noexcept {
    return m_submissionTokens;
  }
  [[nodiscard]] std::span<const rhi::SubmissionToken>
  retirementDependencies() const noexcept {
    return m_retirementDependencies;
  }

private:
  struct UnresolvedSubmission final {
    std::uint32_t submissionIndex{0};
    rhi::SubmissionToken reportedToken{};
  };

  [[nodiscard]] std::vector<SceneResourceDiagnosticId> diagnosticIds() const;
  [[nodiscard]] bool canAdvanceRetirementFrontier(
      rhi::SubmissionToken token) const noexcept;
  void advanceRetirementFrontier(rhi::SubmissionToken token) noexcept;

  std::vector<SceneJournalResource> m_resources;
  // Every successful backend return is retained verbatim for diagnostics,
  // including invalid or non-monotonic values.
  std::vector<rhi::SubmissionToken> m_submissionTokens;
  // Retirement uses one strictly monotonic latest token per queue. It is kept
  // separate from diagnostics so a rejected report can be recovered safely.
  std::vector<rhi::SubmissionToken> m_retirementDependencies;
  std::vector<UnresolvedSubmission> m_unresolvedSubmissions;
  SceneResourceJournalState m_state{SceneResourceJournalState::Staging};
};

} // namespace demo::editor
