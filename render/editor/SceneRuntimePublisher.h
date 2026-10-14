#pragma once

#include "../../rhi/RHIBackend.h"
#include "../../rhi/RHIQueue.h"
#include "../../rhi/RHIResult.h"
#include "SceneDeltaCounters.h"
#include "SceneDeltaResourceJournal.h"

#include <mgif/scene_editor/SceneChangeSet.h>
#include <mgif/scene_editor/SceneProjection.h>
#include <mgif/scene_editor/SceneRuntimeMapping.h>

#include <atomic>
#include <compare>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <memory>
#include <mutex>
#include <optional>
#include <span>
#include <string>
#include <vector>

namespace demo::editor {

enum class RendererSceneUpdateCapability : std::uint8_t {
  Unsupported = 0,
  IncrementalAndSnapshot = 1,
  DeviceLost = 2,
};

struct RendererInstanceId final {
  std::uint64_t value{0};
  std::uint32_t generation{0};

  [[nodiscard]] constexpr bool valid() const noexcept {
    return value != 0u && generation != 0u;
  }
  auto operator<=>(const RendererInstanceId &) const = default;
};

struct RendererDeviceId final {
  std::uint64_t value{0};
  std::uint32_t generation{0};

  [[nodiscard]] constexpr bool valid() const noexcept {
    return value != 0u && generation != 0u;
  }
  auto operator<=>(const RendererDeviceId &) const = default;
};

struct RendererTargetFence final {
  rhi::BackendType backend{rhi::BackendType::vulkan};
  RendererSceneUpdateCapability capability{
      RendererSceneUpdateCapability::Unsupported};
  RendererInstanceId renderer{};
  RendererDeviceId device{};
  std::uint64_t mappingEpoch{0};
  EditorDocumentAuthority authority{};
  std::uint64_t appliedDocumentRevision{0};
  std::uint64_t sceneActivationEpoch{0};
  std::uint64_t rootExternalStateSerial{0};
  std::optional<StableSceneId> activeScene;
  SceneDigest projectionDigest{};

  [[nodiscard]] bool valid() const noexcept;
  bool operator==(const RendererTargetFence &) const = default;
};

enum class RendererObjectKind : std::uint8_t {
  SceneObject = 0,
  Material = 1,
  Texture = 2,
  Sampler = 3,
  Camera = 4,
  Light = 5,
  Geometry = 6,
  Descriptor = 7,
};

struct RendererObjectHandle final {
  RendererObjectKind kind{RendererObjectKind::SceneObject};
  std::uint32_t index{0};
  std::uint32_t generation{0};
  std::uint32_t rendererGeneration{0};
  std::uint32_t deviceGeneration{0};

  [[nodiscard]] constexpr bool valid() const noexcept {
    return index != 0u && generation != 0u && rendererGeneration != 0u &&
           deviceGeneration != 0u;
  }
  auto operator<=>(const RendererObjectHandle &) const = default;
};

struct RendererProjectionCoordinate final {
  EditorObjectKind kind{EditorObjectKind::invalid};
  std::uint32_t sceneAssetIndex{std::numeric_limits<std::uint32_t>::max()};
  std::uint64_t runtimeIndex{std::numeric_limits<std::uint64_t>::max()};
  SceneDigest identityDigest{};
  std::uint32_t colorSceneAssetIndex{
      std::numeric_limits<std::uint32_t>::max()};
  std::uint32_t linearSceneAssetIndex{
      std::numeric_limits<std::uint32_t>::max()};

  [[nodiscard]] bool valid() const noexcept;
  [[nodiscard]] std::uint32_t
  sceneAssetIndexFor(SceneTextureSemantic semantic) const noexcept;
  auto operator<=>(const RendererProjectionCoordinate &) const = default;
};

struct RendererSceneMappingEntry final {
  StableSceneId stableId{};
  RendererProjectionCoordinate projection{};
  RendererObjectHandle rendererHandle{};
  std::optional<SceneDigest> resourceContentDigest;
  std::optional<SceneDigest> resourceSchemaDigest;

  bool operator==(const RendererSceneMappingEntry &) const = default;
};

enum class RendererSceneMappingError : std::uint8_t {
  None = 0,
  InvalidFence = 1,
  InvalidEntry = 2,
  AuthorityMismatch = 3,
  RendererGenerationMismatch = 4,
  DeviceGenerationMismatch = 5,
  DuplicateStableId = 6,
  DuplicateProjectionCoordinate = 7,
  DuplicateRendererHandle = 8,
  ReverseLookupMismatch = 9,
  ProjectionAuthorityMismatch = 10,
};

struct RendererSceneMappingValidation final {
  static constexpr std::size_t kNoIndex =
      std::numeric_limits<std::size_t>::max();

  RendererSceneMappingError error{RendererSceneMappingError::None};
  std::size_t entryIndex{kNoIndex};
  std::string message;

  [[nodiscard]] bool valid() const noexcept {
    return error == RendererSceneMappingError::None;
  }
};

class RendererSceneMappingSnapshot final {
public:
  RendererSceneMappingSnapshot() = default;
  RendererSceneMappingSnapshot(RendererTargetFence fence,
                               std::vector<RendererSceneMappingEntry> entries);
  RendererSceneMappingSnapshot(
      RendererTargetFence fence,
      std::vector<RendererSceneMappingEntry> entries,
      SceneRuntimeMapping projectionAuthority);

  [[nodiscard]] RendererSceneMappingValidation validate() const;
  [[nodiscard]] const RendererTargetFence &fence() const noexcept {
    return m_fence;
  }
  [[nodiscard]] std::span<const RendererSceneMappingEntry>
  entries() const noexcept {
    return m_entries;
  }
  [[nodiscard]] const RendererSceneMappingEntry *
  find(StableSceneId stableId) const noexcept;
  [[nodiscard]] const SceneRuntimeMapping *
  projectionAuthority() const noexcept {
    return m_projectionAuthority ? &*m_projectionAuthority : nullptr;
  }
  [[nodiscard]] std::optional<StableSceneId>
  reverseLookup(RendererObjectHandle handle) const noexcept;
  [[nodiscard]] std::optional<StableSceneId>
  reverseLookup(RendererProjectionCoordinate coordinate) const noexcept;

  bool operator==(const RendererSceneMappingSnapshot &) const = default;

private:
  RendererSceneMappingSnapshot(
      RendererTargetFence fence,
      std::vector<RendererSceneMappingEntry> entries,
      std::optional<SceneRuntimeMapping> projectionAuthority);

  RendererTargetFence m_fence{};
  std::vector<RendererSceneMappingEntry> m_entries;
  std::vector<std::size_t> m_rendererReverseOrder;
  std::vector<std::size_t> m_projectionReverseOrder;
  std::optional<SceneRuntimeMapping> m_projectionAuthority;
};

struct SceneCanonicalIdentity final {
  EditorDocumentAuthority authority{};
  SceneChangeDomain domain{SceneChangeDomain::DocumentMutation};
  std::uint64_t firstMutationSerial{0};
  std::uint64_t lastMutationSerial{0};
  std::uint64_t targetDocumentRevision{0};
  std::uint64_t targetSceneActivationEpoch{0};
  std::uint64_t targetRootExternalStateSerial{0};

  [[nodiscard]] bool valid() const noexcept;
  bool operator==(const SceneCanonicalIdentity &) const = default;
};

struct SceneUploadCounterContribution final {
  std::uint64_t geometryUploadCalls{0};
  std::uint64_t geometryBytesUploaded{0};
  std::uint64_t geometryRangesUploaded{0};
  std::uint64_t textureUploadCalls{0};
  std::uint64_t textureBytesUploaded{0};
  std::uint64_t textureSubresourcesUploaded{0};
  std::uint64_t otherUploadCalls{0};
  std::uint64_t otherBytesUploaded{0};
  std::uint64_t otherRangesUploaded{0};
};

enum class ScenePlaceholderPolicy : std::uint8_t {
  Disallow = 0,
  AllowDeclared = 1,
};

struct SceneQueueSubmissionPlan final {
  rhi::QueueClass queueClass{rhi::QueueClass::graphics};
  std::uint64_t diagnosticId{0};

  [[nodiscard]] constexpr bool valid() const noexcept {
    return diagnosticId != 0u;
  }
  bool operator==(const SceneQueueSubmissionPlan &) const = default;
};

struct SceneGenerationStagePlan final {
  std::vector<SceneQueueSubmissionPlan> submissions;
  std::vector<SceneResourceDiagnosticId> retireResourceDiagnosticIds;
  ScenePlaceholderPolicy placeholderPolicy{ScenePlaceholderPolicy::Disallow};
  std::uint32_t placeholderResourceCount{0};
  std::uint64_t coalescedOperationCount{0};
  std::array<std::uint64_t, kSceneChangeClassCount>
      coalescedOperationsByClass{};
  SceneUploadCounterContribution uploads{};
};

struct ApplySceneChangeRequest final {
  RendererTargetFence expectedTarget;
  SceneCanonicalIdentity canonicalIdentity;
  SceneChangeSet changes;
  std::vector<std::uint8_t> canonicalBytes;
  RendererSceneMappingSnapshot targetMapping;
  SceneGenerationStagePlan stagePlan;
  // Immutable projection-owner authority.  Public asset/mapping values are
  // treated as untrusted transport and must be checked against this seal
  // before any renderer transaction work begins.
  SceneProjectionOwnerCapability ownerCapability;
};

struct SceneHandleDiagnostics final {
  std::uint64_t createdCount{0};
  std::uint64_t logicallyRetiredCount{0};
  std::uint64_t eventuallyDestroyedCount{0};
  std::vector<SceneResourceDiagnosticId> created;
  std::vector<SceneResourceDiagnosticId> logicallyRetired;
  std::vector<SceneResourceDiagnosticId> eventuallyDestroyed;
};

struct SceneApplyResult final {
  SceneApplyDisposition disposition{SceneApplyDisposition::Rejected};
  SceneApplyError error{SceneApplyError::InvalidState};
  std::string message;
  rhi::RHIResult rhiResult{};
  SceneCanonicalIdentity canonicalIdentity{};
  SceneDigest canonicalDigest{};
  RendererTargetFence oldFence{};
  std::optional<RendererTargetFence> resultingFence;
  std::vector<rhi::SubmissionToken> queueSubmissionTokens;
  SceneHandleDiagnostics handles;
  SceneDeltaCounterSnapshot countersBefore{};
  SceneDeltaCounterSnapshot countersAfter{};
  SceneDeltaCounterSnapshot counterDelta{};
  std::optional<FullRebuildReason> requestedFullSnapshotReason;
  std::uint32_t placeholderResourceCount{0};

  [[nodiscard]] bool accepted() const noexcept {
    return disposition == SceneApplyDisposition::AcceptedIncremental ||
           disposition == SceneApplyDisposition::AcceptedFullSnapshot ||
           disposition == SceneApplyDisposition::AlreadyApplied;
  }
};

struct SceneRetirementReservation final {
  std::uint64_t value{0};
  [[nodiscard]] constexpr bool valid() const noexcept { return value != 0u; }
};

class SceneTransactionBackend : public SceneResourceRetirementSink {
public:
  ~SceneTransactionBackend() override = default;

  // These calls operate only on unpublished candidate state. Descriptor and
  // handle visibility remains private until publishSceneGeneration performs
  // the single generation swap.
  [[nodiscard]] virtual rhi::RHIResult stageCpuGeneration(
      const SceneChangeSet &changes,
      const RendererSceneMappingSnapshot &targetMapping) noexcept = 0;
  [[nodiscard]] virtual rhi::RHIResult allocateSceneResources(
      const SceneChangeSet &changes,
      std::vector<SceneJournalResource> &createdResources) noexcept = 0;
  [[nodiscard]] virtual rhi::RHIResult stageSceneDescriptors(
      const SceneChangeSet &changes,
      std::span<const SceneJournalResource> createdResources) noexcept = 0;
  [[nodiscard]] virtual rhi::RHIResult recordSceneGeneration(
      const SceneChangeSet &changes,
      std::span<const SceneJournalResource> createdResources) noexcept = 0;
  [[nodiscard]] virtual rhi::Result<rhi::SubmissionToken>
  submitSceneGeneration(const SceneQueueSubmissionPlan &submission,
                        std::uint32_t submissionIndex) noexcept = 0;
  [[nodiscard]] virtual rhi::RHIResult validateBeforePublication(
      const RendererSceneMappingSnapshot &targetMapping) noexcept = 0;
  [[nodiscard]] virtual rhi::Result<SceneRetirementReservation>
  reserveSceneRetirement(
      std::span<const SceneJournalResource> resources,
      std::span<const rhi::SubmissionToken> dependencies) noexcept = 0;
  // A successful reservation owns no logical lifetime transition until this
  // no-fail commit, which is invoked only after publication.
  virtual void
  commitSceneRetirement(SceneRetirementReservation reservation) noexcept = 0;
  virtual void
  cancelSceneRetirement(SceneRetirementReservation reservation) noexcept = 0;
};

struct RendererSceneInitialState final {
  RendererTargetFence fence;
  RendererSceneMappingSnapshot mapping;
  SceneDigest activeGenerationDigest{};
  std::vector<SceneJournalResource> resources;
  std::vector<rhi::SubmissionToken> lastUseSubmissionTokens;
  std::uint64_t generation{1};
  std::uint64_t descriptorGeneration{1};
};

class RendererSceneGeneration final {
public:
  RendererSceneGeneration(
      std::uint64_t generation, std::uint64_t descriptorGeneration,
      RendererTargetFence fence, RendererSceneMappingSnapshot mapping,
      SceneDigest activeGenerationDigest,
      std::vector<SceneJournalResource> resources,
      std::vector<rhi::SubmissionToken> lastUseSubmissionTokens,
      SceneCanonicalIdentity canonicalIdentity, SceneDigest canonicalDigest,
      std::vector<std::uint8_t> canonicalBytes,
      RendererTargetFence canonicalBaseFence);

  [[nodiscard]] std::uint64_t generation() const noexcept {
    return m_generation;
  }
  [[nodiscard]] std::uint64_t descriptorGeneration() const noexcept {
    return m_descriptorGeneration;
  }
  [[nodiscard]] const RendererTargetFence &fence() const noexcept {
    return m_fence;
  }
  [[nodiscard]] const RendererSceneMappingSnapshot &mapping() const noexcept {
    return m_mapping;
  }
  [[nodiscard]] const SceneDigest &activeGenerationDigest() const noexcept {
    return m_activeGenerationDigest;
  }
  [[nodiscard]] std::span<const SceneJournalResource>
  resources() const noexcept {
    return m_resources;
  }
  [[nodiscard]] std::span<const rhi::SubmissionToken>
  lastUseSubmissionTokens() const noexcept {
    return m_lastUseSubmissionTokens;
  }
  [[nodiscard]] const SceneCanonicalIdentity &
  canonicalIdentity() const noexcept {
    return m_canonicalIdentity;
  }
  [[nodiscard]] std::span<const std::uint8_t> canonicalBytes() const noexcept {
    return m_canonicalBytes;
  }
  [[nodiscard]] const SceneDigest &canonicalDigest() const noexcept {
    return m_canonicalDigest;
  }
  [[nodiscard]] const RendererTargetFence &canonicalBaseFence() const noexcept {
    return m_canonicalBaseFence;
  }

private:
  std::uint64_t m_generation{0};
  std::uint64_t m_descriptorGeneration{0};
  RendererTargetFence m_fence{};
  RendererSceneMappingSnapshot m_mapping;
  SceneDigest m_activeGenerationDigest{};
  std::vector<SceneJournalResource> m_resources;
  std::vector<rhi::SubmissionToken> m_lastUseSubmissionTokens;
  SceneCanonicalIdentity m_canonicalIdentity{};
  SceneDigest m_canonicalDigest{};
  std::vector<std::uint8_t> m_canonicalBytes;
  RendererTargetFence m_canonicalBaseFence{};
};

class RendererScenePublisher final {
public:
  RendererScenePublisher(RendererSceneInitialState initialState,
                         SceneTransactionBackend &backend,
                         SceneDeltaCounters &counters);

  [[nodiscard]] SceneApplyResult
  applySceneChanges(const ApplySceneChangeRequest &request) noexcept;
  [[nodiscard]] SceneApplyResult
  applyFullSceneSnapshot(const ApplySceneChangeRequest &request) noexcept;

  // Read-only validation for integration layers that own additional plan or
  // device checks. A returned rejection has zero backend and counter effects;
  // nullopt means the request may proceed to the transactional apply path.
  [[nodiscard]] std::optional<SceneApplyResult>
  preflightSceneChanges(const ApplySceneChangeRequest &request) const noexcept;
  [[nodiscard]] std::optional<SceneApplyResult>
  preflightFullSceneSnapshot(
      const ApplySceneChangeRequest &request) const noexcept;

  [[nodiscard]] std::shared_ptr<const RendererSceneGeneration>
  activeGeneration() const noexcept;
  [[nodiscard]] SceneDeltaCounterSnapshot counters() const noexcept;
  [[nodiscard]] bool hasPendingOrphanRetirement() const noexcept;
  [[nodiscard]] rhi::RHIResult retryPendingOrphanRetirement() noexcept;

private:
  [[nodiscard]] std::optional<SceneApplyResult>
  preflight(const ApplySceneChangeRequest &request,
            SceneChangeDomain requiredDomain) const noexcept;
  [[nodiscard]] rhi::RHIResult retryPendingOrphanRetirementLocked() noexcept;
  [[nodiscard]] SceneApplyResult
  apply(const ApplySceneChangeRequest &request,
        SceneChangeDomain requiredDomain) noexcept;
  [[nodiscard]] std::shared_ptr<const RendererSceneGeneration>
  stageSceneGeneration(
      const RendererSceneGeneration &active,
      const ApplySceneChangeRequest &request,
      RendererTargetFence resultingFence,
      std::vector<SceneJournalResource> resources,
      std::vector<rhi::SubmissionToken> lastUseSubmissionTokens) const;
  [[nodiscard]] std::shared_ptr<const RendererSceneGeneration>
  publishSceneGeneration(
      std::shared_ptr<const RendererSceneGeneration> candidate) noexcept;
  [[nodiscard]] rhi::Result<SceneRetirementReservation> retireSceneGeneration(
      std::span<const SceneJournalResource> resources,
      std::span<const rhi::SubmissionToken> dependencies) noexcept;

  SceneTransactionBackend &m_backend;
  SceneDeltaCounters &m_counters;
  std::atomic<std::shared_ptr<const RendererSceneGeneration>> m_active;
  mutable std::mutex m_applyMutex;
  std::unique_ptr<SceneDeltaResourceJournal> m_pendingOrphan;
};

} // namespace demo::editor
