#pragma once

#include <array>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <span>
#include <string>
#include <string_view>

namespace demo::editor::automation {

class EditorTransactionSmoke final {
public:
  using Digest = std::array<std::uint8_t, 32>;

  enum class Phase : std::uint8_t {
    disabled = 0,
    waitingSceneReady,
    ready,
    mutationSubmitted,
    mutationAcceptedByRenderer,
    undoSubmitted,
    undoObserved,
    redoSubmitted,
    redoObserved,
    saveVerified,
    budgetComplete,
    complete,
    failed,
  };

  enum class Marker : std::uint8_t {
    sceneReady = 0,
    automationStart,
    undoObserved,
    redoObserved,
    saveComplete,
    frameBudgetComplete,
    automationComplete,
  };

  enum class WatchdogStage : std::uint8_t {
    renamePublication = 0,
    undoPublication,
    redoPublication,
    saveVerification,
  };

  struct ProjectionStamp final {
    std::uint64_t documentRevision{0};
    Digest documentDigest{};
    Digest projectionDigest{};
    Digest contentDigest{};
    std::uint64_t mappingEpoch{0};

    bool operator==(const ProjectionStamp&) const = default;
  };

  struct Snapshot final {
    ProjectionStamp projection{};
    std::uint64_t cleanRevision{0};
    std::size_t undoCount{0};
    std::size_t redoCount{0};
  };

  struct DispatchEvidence final {
    bool accepted{false};
    bool expectedRefresh{false};
    bool publicationPending{false};
    std::size_t publicationCount{0};
  };

  struct SaveEvidence final {
    bool dispatchAccepted{false};
    bool targetAbsentAtStart{false};
    bool targetRegularFile{false};
    std::uint64_t targetBytes{0};
    bool reloadSucceeded{false};
    bool semanticEquivalent{false};
    bool reloadedMutationPresent{false};
    bool liveSourceIsTarget{false};
    bool liveDocumentClean{false};
    Snapshot live{};
  };

  explicit EditorTransactionSmoke(bool enabled = false) noexcept
      : m_phase(enabled ? Phase::waitingSceneReady : Phase::disabled) {}

  [[nodiscard]] bool enabled() const noexcept {
    return m_phase != Phase::disabled;
  }

  [[nodiscard]] Phase phase() const noexcept { return m_phase; }
  [[nodiscard]] Phase failurePhase() const noexcept { return m_failurePhase; }
  [[nodiscard]] std::string_view failure() const noexcept { return m_failure; }
  [[nodiscard]] std::span<const Marker> markers() const noexcept {
    return {m_markers.data(), m_markerCount};
  }
  [[nodiscard]] std::uint64_t renderedFrames() const noexcept {
    return m_renderedFrames;
  }
  [[nodiscard]] std::span<const std::chrono::milliseconds>
  completedStageTimings() const noexcept {
    return {m_stageTimings.data(), m_completedStageTimingCount};
  }

  [[nodiscard]] bool observeSceneReady(bool strictSceneReady,
                                       bool rendererReady,
                                       bool publicationQueueEmpty) {
    if(!requirePhase(Phase::waitingSceneReady, "scene-ready was out of order"))
      return false;
    if(!strictSceneReady || !rendererReady || !publicationQueueEmpty)
      return reject("scene-ready predicates were incomplete");
    m_phase = Phase::ready;
    return appendMarker(Marker::sceneReady);
  }

  [[nodiscard]] bool submitMutation(const Snapshot& baseline,
                                    const Snapshot& mutated,
                                    const DispatchEvidence& dispatch,
                                    std::uint64_t frame) {
    if(!requirePhase(Phase::ready, "mutation submission was out of order"))
      return false;
    if(frame != 0u)
      return reject("mutation was not submitted on counted frame 0");
    if(!validStamp(baseline.projection) || baseline.redoCount != 0u ||
       baseline.cleanRevision != baseline.projection.documentRevision)
      return reject("baseline snapshot was not clean and canonical");
    if(!validDispatch(dispatch) || !dispatch.expectedRefresh)
      return reject("rename dispatch or publication evidence was rejected");
    if(!validStamp(mutated.projection) ||
       mutated.cleanRevision != baseline.cleanRevision ||
       mutated.projection.documentRevision <=
           baseline.projection.documentRevision ||
       baseline.projection.documentRevision ==
           std::numeric_limits<std::uint64_t>::max() ||
       mutated.projection.documentRevision !=
           baseline.projection.documentRevision + 1u ||
       mutated.projection.documentDigest == baseline.projection.documentDigest ||
       mutated.projection.projectionDigest ==
           baseline.projection.projectionDigest ||
       mutated.projection.contentDigest == baseline.projection.contentDigest ||
       baseline.projection.mappingEpoch == std::numeric_limits<std::uint64_t>::max() ||
       mutated.projection.mappingEpoch != baseline.projection.mappingEpoch + 1u ||
       mutated.undoCount != baseline.undoCount + 1u ||
       mutated.redoCount != 0u)
      return reject("rename postconditions did not describe one undoable mutation");
    m_baseline = baseline;
    m_mutated = mutated;
    m_expectedProjection = mutated.projection;
    m_submissionFrame = frame;
    m_phase = Phase::mutationSubmitted;
    return appendMarker(Marker::automationStart);
  }

  [[nodiscard]] bool observeMutationAccepted(
      const ProjectionStamp& accepted, std::uint64_t frame) {
    if(!requirePhase(Phase::mutationSubmitted,
                     "mutation renderer acceptance was out of order"))
      return false;
    if(frame != 1u || !publicationWithinBound(frame) ||
       accepted != m_expectedProjection)
      return reject("renderer accepted the wrong or late mutation projection");
    m_phase = Phase::mutationAcceptedByRenderer;
    return true;
  }

  [[nodiscard]] bool submitUndo(const Snapshot& restored,
                                const DispatchEvidence& dispatch,
                                std::uint64_t frame) {
    if(!requirePhase(Phase::mutationAcceptedByRenderer,
                     "undo submission was out of order"))
      return false;
    if(frame != 1u || !validDispatch(dispatch))
      return reject("undo dispatch or publication evidence was rejected");
    if(!validStamp(restored.projection) ||
       restored.cleanRevision != m_baseline.cleanRevision ||
       restored.projection.documentRevision <=
           m_mutated.projection.documentRevision ||
       m_mutated.projection.documentRevision ==
           std::numeric_limits<std::uint64_t>::max() ||
       restored.projection.documentRevision !=
           m_mutated.projection.documentRevision + 1u ||
       restored.projection.documentDigest !=
           m_baseline.projection.documentDigest ||
       m_mutated.projection.mappingEpoch == std::numeric_limits<std::uint64_t>::max() ||
       restored.projection.mappingEpoch != m_mutated.projection.mappingEpoch + 1u ||
       restored.undoCount != m_baseline.undoCount ||
       restored.redoCount != 1u)
      return reject("undo did not restore the baseline semantic state");
    m_undo = restored;
    m_expectedProjection = restored.projection;
    m_submissionFrame = frame;
    m_phase = Phase::undoSubmitted;
    return true;
  }

  [[nodiscard]] bool observeUndoAccepted(const Snapshot& current,
                                         const ProjectionStamp& accepted,
                                         bool originalValuePresent,
                                         std::uint64_t frame) {
    if(!requirePhase(Phase::undoSubmitted,
                     "undo renderer acceptance was out of order"))
      return false;
    if(frame != 2u || !publicationWithinBound(frame) ||
       accepted != m_expectedProjection ||
       current.projection != m_undo.projection ||
       current.projection.documentDigest !=
           m_baseline.projection.documentDigest ||
       current.undoCount != m_baseline.undoCount || current.redoCount != 1u ||
       !originalValuePresent)
      return reject("renderer-observed undo did not restore the baseline");
    m_phase = Phase::undoObserved;
    return appendMarker(Marker::undoObserved);
  }

  [[nodiscard]] bool submitRedo(const Snapshot& reapplied,
                                const DispatchEvidence& dispatch,
                                std::uint64_t frame) {
    if(!requirePhase(Phase::undoObserved, "redo submission was out of order"))
      return false;
    if(frame != 2u || !validDispatch(dispatch))
      return reject("redo dispatch or publication evidence was rejected");
    if(!validStamp(reapplied.projection) ||
       reapplied.cleanRevision != m_baseline.cleanRevision ||
       reapplied.projection.documentRevision <=
           m_undo.projection.documentRevision ||
       m_undo.projection.documentRevision ==
           std::numeric_limits<std::uint64_t>::max() ||
       reapplied.projection.documentRevision !=
           m_undo.projection.documentRevision + 1u ||
       reapplied.projection.documentDigest !=
           m_mutated.projection.documentDigest ||
       m_undo.projection.mappingEpoch == std::numeric_limits<std::uint64_t>::max() ||
       reapplied.projection.mappingEpoch != m_undo.projection.mappingEpoch + 1u ||
       reapplied.undoCount != m_baseline.undoCount + 1u ||
       reapplied.redoCount != 0u)
      return reject("redo did not reapply the mutated semantic state");
    m_redo = reapplied;
    m_expectedProjection = reapplied.projection;
    m_submissionFrame = frame;
    m_phase = Phase::redoSubmitted;
    return true;
  }

  [[nodiscard]] bool observeRedoAccepted(const Snapshot& current,
                                         const ProjectionStamp& accepted,
                                         bool mutatedValuePresent,
                                         std::uint64_t frame) {
    if(!requirePhase(Phase::redoSubmitted,
                     "redo renderer acceptance was out of order"))
      return false;
    if(frame != 3u || !publicationWithinBound(frame) ||
       accepted != m_expectedProjection ||
       current.projection != m_redo.projection ||
       current.projection.documentDigest !=
           m_mutated.projection.documentDigest ||
       current.undoCount != m_baseline.undoCount + 1u ||
       current.redoCount != 0u || !mutatedValuePresent)
      return reject("renderer-observed redo did not reapply the mutation");
    m_phase = Phase::redoObserved;
    return appendMarker(Marker::redoObserved);
  }

  [[nodiscard]] bool verifySave(const SaveEvidence& save,
                                std::uint64_t frame) {
    if(!requirePhase(Phase::redoObserved,
                     "save verification was out of order"))
      return false;
    if(frame != 3u)
      return reject("save verification did not complete on counted frame 3");
    if(!save.dispatchAccepted || !save.targetAbsentAtStart ||
       !save.targetRegularFile || save.targetBytes <= 12u ||
       !save.reloadSucceeded || !save.semanticEquivalent ||
       !save.reloadedMutationPresent || !save.liveSourceIsTarget ||
       !save.liveDocumentClean ||
       save.live.cleanRevision != save.live.projection.documentRevision ||
       save.live.projection != m_redo.projection ||
       save.live.undoCount != m_baseline.undoCount + 1u ||
       save.live.redoCount != 0u)
      return reject("Save As output did not satisfy the persistence contract");
    m_phase = Phase::saveVerified;
    return appendMarker(Marker::saveComplete);
  }

  [[nodiscard]] bool observeRenderedFrame(std::uint64_t frame) {
    if(m_phase == Phase::disabled)
      return true;
    if(m_phase == Phase::failed)
      return false;
    if(m_phase == Phase::complete)
      return reject("a rendered frame arrived after automation completion");
    if(frame != m_renderedFrames || frame >= 40u)
      return reject("counted render frames were duplicated, skipped, or exceeded 40");
    if(waitingForPublication() && !publicationWithinBound(frame))
      return reject("renderer publication acknowledgement timed out");
    ++m_renderedFrames;
    if(frame != 39u)
      return true;
    if(m_phase != Phase::saveVerified)
      return reject("counted frame 39 completed before Save As verification");
    m_phase = Phase::budgetComplete;
    if(!appendMarker(Marker::frameBudgetComplete))
      return false;
    m_phase = Phase::complete;
    return appendMarker(Marker::automationComplete);
  }

  [[nodiscard]] bool checkDeadline(std::chrono::milliseconds elapsed,
                                   std::uint64_t frame) {
    if(m_phase == Phase::disabled || m_phase == Phase::complete)
      return true;
    if(m_phase == Phase::failed)
      return false;
    if(elapsed > std::chrono::seconds(60))
      return reject("editor transaction exceeded its 60-second deadline");
    if(waitingForPublication() && !publicationWithinBound(frame))
      return reject("renderer publication acknowledgement timed out");
    return true;
  }

  // The 60-second watchdog is segmented only at semantic durability
  // boundaries: an exact renderer publication for rename/undo/redo, then the
  // fully reloaded and semantically compared Save As result. Resetting the
  // caller's clock anywhere else would hide synchronous work or weaken the
  // transaction contract.
  [[nodiscard]] bool completeWatchdogStage(
      WatchdogStage stage, std::chrono::milliseconds elapsed,
      std::uint64_t frame) {
    if(!checkDeadline(elapsed, frame))
      return false;
    const std::size_t index = static_cast<std::size_t>(stage);
    if(index != m_completedStageTimingCount || index >= m_stageTimings.size())
      return reject("editor transaction watchdog stages were out of order");
    const Phase requiredPhase = watchdogBoundaryPhase(stage);
    if(m_phase != requiredPhase)
      return reject("editor transaction watchdog reset before a durable boundary");
    m_stageTimings[index] = elapsed;
    ++m_completedStageTimingCount;
    return true;
  }

  [[nodiscard]] bool reject(std::string_view reason) {
    if(m_phase != Phase::failed) {
      m_failurePhase = m_phase;
      m_phase = Phase::failed;
      m_failure.assign(reason);
    }
    return false;
  }

  [[nodiscard]] static constexpr std::string_view markerName(
      Marker marker) noexcept {
    switch(marker) {
      case Marker::sceneReady: return "scene-ready";
      case Marker::automationStart: return "automation-start";
      case Marker::undoObserved: return "undo-observed";
      case Marker::redoObserved: return "redo-observed";
      case Marker::saveComplete: return "save-complete";
      case Marker::frameBudgetComplete: return "frame-budget-complete";
      case Marker::automationComplete: return "automation-complete";
    }
    return {};
  }

  [[nodiscard]] static constexpr std::string_view phaseName(
      Phase phase) noexcept {
    switch(phase) {
      case Phase::disabled: return "disabled";
      case Phase::waitingSceneReady: return "waiting-scene-ready";
      case Phase::ready: return "ready";
      case Phase::mutationSubmitted: return "mutation-submitted";
      case Phase::mutationAcceptedByRenderer:
        return "mutation-accepted-by-renderer";
      case Phase::undoSubmitted: return "undo-submitted";
      case Phase::undoObserved: return "undo-observed";
      case Phase::redoSubmitted: return "redo-submitted";
      case Phase::redoObserved: return "redo-observed";
      case Phase::saveVerified: return "save-verified";
      case Phase::budgetComplete: return "budget-complete";
      case Phase::complete: return "complete";
      case Phase::failed: return "failed";
    }
    return "unknown";
  }

  [[nodiscard]] static constexpr std::string_view watchdogStageName(
      WatchdogStage stage) noexcept {
    switch(stage) {
      case WatchdogStage::renamePublication: return "rename-publication";
      case WatchdogStage::undoPublication: return "undo-publication";
      case WatchdogStage::redoPublication: return "redo-publication";
      case WatchdogStage::saveVerification: return "save-verification";
    }
    return "unknown";
  }

private:
  [[nodiscard]] static bool digestPresent(const Digest& digest) noexcept {
    for(const std::uint8_t value : digest) {
      if(value != 0u)
        return true;
    }
    return false;
  }

  [[nodiscard]] static bool validStamp(const ProjectionStamp& stamp) noexcept {
    return stamp.mappingEpoch != 0u && digestPresent(stamp.documentDigest) &&
           digestPresent(stamp.projectionDigest) &&
           digestPresent(stamp.contentDigest);
  }

  [[nodiscard]] static bool validDispatch(
      const DispatchEvidence& dispatch) noexcept {
    return dispatch.accepted && dispatch.publicationPending &&
           dispatch.publicationCount == 1u;
  }

  [[nodiscard]] bool requirePhase(Phase expected, std::string_view reason) {
    return m_phase == expected ? true : reject(reason);
  }

  [[nodiscard]] bool publicationWithinBound(std::uint64_t frame) const noexcept {
    return frame >= m_submissionFrame && frame - m_submissionFrame <= 3u;
  }

  [[nodiscard]] bool waitingForPublication() const noexcept {
    return m_phase == Phase::mutationSubmitted ||
           m_phase == Phase::undoSubmitted || m_phase == Phase::redoSubmitted;
  }

  [[nodiscard]] static constexpr Phase watchdogBoundaryPhase(
      WatchdogStage stage) noexcept {
    switch(stage) {
      case WatchdogStage::renamePublication:
        return Phase::mutationAcceptedByRenderer;
      case WatchdogStage::undoPublication:
        return Phase::undoObserved;
      case WatchdogStage::redoPublication:
        return Phase::redoObserved;
      case WatchdogStage::saveVerification:
        return Phase::saveVerified;
    }
    return Phase::failed;
  }

  [[nodiscard]] bool appendMarker(Marker marker) {
    if(m_markerCount >= m_markers.size())
      return reject("authoritative marker ledger overflowed");
    if(static_cast<std::size_t>(marker) != m_markerCount)
      return reject("authoritative marker order was violated");
    m_markers[m_markerCount++] = marker;
    return true;
  }

  Phase m_phase{Phase::disabled};
  Phase m_failurePhase{Phase::disabled};
  Snapshot m_baseline{};
  Snapshot m_mutated{};
  Snapshot m_undo{};
  Snapshot m_redo{};
  ProjectionStamp m_expectedProjection{};
  std::uint64_t m_submissionFrame{0};
  std::uint64_t m_renderedFrames{0};
  std::array<Marker, 7> m_markers{};
  std::size_t m_markerCount{0};
  std::array<std::chrono::milliseconds, 4> m_stageTimings{};
  std::size_t m_completedStageTimingCount{0};
  std::string m_failure;
};

} // namespace demo::editor::automation
