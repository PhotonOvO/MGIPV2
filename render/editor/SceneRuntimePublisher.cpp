#include "SceneRuntimePublisher.h"

#include <mgif/scene_editor/SceneChangeCanonicalizer.h>

#include <algorithm>
#include <limits>
#include <stdexcept>
#include <tuple>
#include <type_traits>
#include <utility>

namespace demo::editor {
namespace {

[[nodiscard]] bool validBackend(rhi::BackendType backend) noexcept {
  return backend >= rhi::BackendType::vulkan &&
         backend <= rhi::BackendType::metal;
}

[[nodiscard]] std::uint64_t
deriveProjectionIdentityFromSource(EditorObjectId documentId) noexcept {
  if (!documentId.valid()) {
    return SceneRuntimeMapping::kInvalidProjectionIdentity;
  }
  std::uint64_t value = 14695981039346656037ull;
  const auto mix = [&value](std::uint64_t part) {
    for (unsigned int shift = 0; shift < 64u; shift += 8u) {
      value ^= (part >> shift) & 0xffu;
      value *= 1099511628211ull;
    }
  };
  mix(static_cast<std::uint64_t>(documentId.kind));
  mix(documentId.authority.value);
  mix(documentId.slot);
  mix(documentId.generation);
  if (value == 0u) {
    return 1u;
  }
  return value == SceneRuntimeMapping::kInvalidProjectionIdentity
             ? value - 1u
             : value;
}

template <typename Enum>
[[nodiscard]] constexpr bool enumInRange(Enum value, Enum maximum) noexcept {
  return static_cast<std::underlying_type_t<Enum>>(value) <=
         static_cast<std::underlying_type_t<Enum>>(maximum);
}

[[nodiscard]] bool
validOptionalDigest(const std::optional<SceneDigest> &digest) noexcept {
  return !digest || sceneDigestPresent(*digest);
}

[[nodiscard]] bool
validResourceSet(std::span<const SceneJournalResource> resources) noexcept {
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
  return true;
}

[[nodiscard]] bool
validSubmissionTokenSet(std::span<const rhi::SubmissionToken> tokens) noexcept {
  for (std::size_t index = 0; index < tokens.size(); ++index) {
    if (!tokens[index].isValid()) {
      return false;
    }
    for (std::size_t prior = 0; prior < index; ++prior) {
      if (tokens[prior].queue == tokens[index].queue) {
        return false;
      }
    }
  }
  return true;
}

[[nodiscard]] SceneApplyError
mapCanonicalError(SceneChangeValidationError error) noexcept {
  switch (error) {
  case SceneChangeValidationError::None:
    return SceneApplyError::None;
  case SceneChangeValidationError::InvalidSchema:
    return SceneApplyError::InvalidSchema;
  case SceneChangeValidationError::InvalidAuthority:
  case SceneChangeValidationError::CrossAuthority:
    return SceneApplyError::AuthorityMismatch;
  case SceneChangeValidationError::InvalidPayload:
  case SceneChangeValidationError::InvalidRange:
  case SceneChangeValidationError::InvalidFieldValue:
  case SceneChangeValidationError::InvalidActionValues:
  case SceneChangeValidationError::FieldClassMismatch:
  case SceneChangeValidationError::InvalidFullSnapshot:
  case SceneChangeValidationError::IncompleteRemap:
    return SceneApplyError::InvalidPayload;
  case SceneChangeValidationError::InvalidContentHash:
  case SceneChangeValidationError::CanonicalDigestMismatch:
    return SceneApplyError::InvalidDigest;
  default:
    return SceneApplyError::InvalidState;
  }
}

[[nodiscard]] std::optional<FullRebuildReason>
fallbackReason(SceneApplyError error) noexcept {
  switch (error) {
  case SceneApplyError::StaleRevision:
  case SceneApplyError::RevisionGap:
    return FullRebuildReason::MissedRevision;
  case SceneApplyError::SceneActivationMismatch:
    return FullRebuildReason::SceneSwitch;
  case SceneApplyError::RootSerialMismatch:
    return FullRebuildReason::ExplicitCallerRequest;
  case SceneApplyError::ProjectionMismatch:
  case SceneApplyError::MappingMismatch:
  case SceneApplyError::RendererMismatch:
  case SceneApplyError::StaleObjectGeneration:
    return FullRebuildReason::MappingMismatch;
  case SceneApplyError::DeviceMismatch:
  case SceneApplyError::DeviceLost:
    return FullRebuildReason::DeviceRecovery;
  case SceneApplyError::UnsupportedCapability:
    return FullRebuildReason::UnsupportedIncrementalCapability;
  case SceneApplyError::PublicationInvariant:
  case SceneApplyError::RetirementEnqueueFailure:
    return FullRebuildReason::ExplicitCallerRequest;
  default:
    return std::nullopt;
  }
}

[[nodiscard]] SceneApplyError
logicalRhiFailure(SceneApplyError stageError,
                  const rhi::RHIResult &result) noexcept {
  return result.error.code == rhi::RHIErrorCode::deviceLost
             ? SceneApplyError::DeviceLost
             : stageError;
}

void setCurrentCounters(SceneDeltaCounterSnapshot &counters,
                        const RendererTargetFence &fence,
                        const SceneDigest &generationDigest) noexcept {
  counters.current.authority = fence.authority;
  counters.current.appliedDocumentRevision = fence.appliedDocumentRevision;
  counters.current.sceneActivationEpoch = fence.sceneActivationEpoch;
  counters.current.rootExternalStateSerial = fence.rootExternalStateSerial;
  counters.current.mappingEpoch = fence.mappingEpoch;
  counters.current.activeScene = fence.activeScene;
  counters.current.rendererInstance = fence.renderer.value;
  counters.current.rendererGeneration = fence.renderer.generation;
  counters.current.deviceInstance = fence.device.value;
  counters.current.deviceGeneration = fence.device.generation;
  counters.current.activeProjectionDigest = fence.projectionDigest;
  counters.current.activeGenerationDigest = generationDigest;
}

void incrementOperationCounter(SceneDeltaCounterSnapshot &counters,
                               const SceneFieldChange &change,
                               bool applied) noexcept {
  const std::size_t index = sceneChangeClassIndex(change.changeClass);
  if (index < counters.sceneOperationsReceivedByClass.size()) {
    ++counters.sceneOperationsReceivedByClass[index];
    if (applied) {
      ++counters.sceneOperationsAppliedByClass[index];
    }
  }
  if (!applied) {
    return;
  }
  switch (change.changeClass) {
  case SceneChangeClass::Transform:
    ++counters.nodeTransformOpsApplied;
    break;
  case SceneChangeClass::MaterialParameters:
    ++counters.materialOpsApplied;
    break;
  case SceneChangeClass::MaterialBinding:
    ++counters.primitiveBindingOpsApplied;
    break;
  case SceneChangeClass::Camera:
    ++counters.cameraOpsApplied;
    break;
  case SceneChangeClass::Light:
    ++counters.lightOpsApplied;
    break;
  case SceneChangeClass::Visibility:
    ++counters.visibilityOpsApplied;
    break;
  case SceneChangeClass::Hierarchy:
    ++counters.hierarchyOpsApplied;
    ++counters.structuralOpsApplied;
    break;
  case SceneChangeClass::Topology:
    ++counters.topologyOpsApplied;
    ++counters.structuralOpsApplied;
    if (change.action == SceneChangeAction::Create) {
      ++counters.instancesAdded;
    } else if (change.action == SceneChangeAction::Delete) {
      ++counters.instancesRemoved;
    }
    break;
  case SceneChangeClass::FullRebuild:
    ++counters.structuralOpsApplied;
    break;
  }
}

void addUploadCounters(SceneDeltaCounterSnapshot &counters,
                       const SceneGenerationStagePlan &plan,
                       bool fullSnapshot) noexcept {
  counters.geometryUploadCalls += plan.uploads.geometryUploadCalls;
  counters.geometryBytesUploaded += plan.uploads.geometryBytesUploaded;
  counters.geometryRangesUploaded += plan.uploads.geometryRangesUploaded;
  counters.textureUploadCalls += plan.uploads.textureUploadCalls;
  counters.textureBytesUploaded += plan.uploads.textureBytesUploaded;
  counters.textureSubresourcesUploaded +=
      plan.uploads.textureSubresourcesUploaded;
  counters.otherUploadCalls += plan.uploads.otherUploadCalls;
  counters.otherBytesUploaded += plan.uploads.otherBytesUploaded;
  counters.otherRangesUploaded += plan.uploads.otherRangesUploaded;
  counters.bufferRangesUploaded +=
      plan.uploads.geometryRangesUploaded + plan.uploads.otherRangesUploaded;
  counters.submissions += plan.submissions.size();
  if (fullSnapshot) {
    counters.fullSnapshotSubmissions += plan.submissions.size();
  } else {
    counters.incrementalSubmissions += plan.submissions.size();
  }
}

[[nodiscard]] bool isContinuousClass(SceneChangeClass changeClass) noexcept {
  switch (changeClass) {
  case SceneChangeClass::Transform:
  case SceneChangeClass::MaterialParameters:
  case SceneChangeClass::MaterialBinding:
  case SceneChangeClass::Camera:
  case SceneChangeClass::Light:
  case SceneChangeClass::Visibility:
    return true;
  default:
    return false;
  }
}

struct ValidationFailure final {
  SceneApplyError error{SceneApplyError::None};
  std::string message;

  [[nodiscard]] bool failed() const noexcept {
    return error != SceneApplyError::None;
  }
};

[[nodiscard]] ValidationFailure
validateExpectedFence(const RendererTargetFence &expected,
                      const RendererTargetFence &current) {
  if (current.capability == RendererSceneUpdateCapability::DeviceLost ||
      expected.capability == RendererSceneUpdateCapability::DeviceLost) {
    return {SceneApplyError::DeviceLost, "renderer device generation is lost"};
  }
  if (current.capability == RendererSceneUpdateCapability::Unsupported ||
      expected.capability == RendererSceneUpdateCapability::Unsupported) {
    return {SceneApplyError::UnsupportedCapability,
            "backend does not advertise scene transaction support"};
  }
  if (expected.backend != current.backend) {
    return {SceneApplyError::RendererMismatch,
            "backend identity does not match the active renderer"};
  }
  if (expected.capability != current.capability) {
    return {SceneApplyError::UnsupportedCapability,
            "backend scene capability does not match the active renderer"};
  }
  if (expected.renderer != current.renderer) {
    return {SceneApplyError::RendererMismatch,
            "renderer instance or generation does not match"};
  }
  if (expected.device != current.device) {
    return {SceneApplyError::DeviceMismatch,
            "device instance or generation does not match"};
  }
  if (expected.mappingEpoch != current.mappingEpoch) {
    return {SceneApplyError::MappingMismatch,
            "renderer mapping epoch does not match"};
  }
  if (expected.authority != current.authority) {
    return {SceneApplyError::AuthorityMismatch,
            "document authority does not match"};
  }
  if (expected.appliedDocumentRevision < current.appliedDocumentRevision) {
    return {SceneApplyError::StaleRevision,
            "document revision is older than the active renderer revision"};
  }
  if (expected.appliedDocumentRevision > current.appliedDocumentRevision) {
    return {SceneApplyError::RevisionGap,
            "document revision is newer than the active renderer revision"};
  }
  if (expected.sceneActivationEpoch != current.sceneActivationEpoch ||
      expected.activeScene != current.activeScene) {
    return {SceneApplyError::SceneActivationMismatch,
            "scene activation fence does not match"};
  }
  if (expected.rootExternalStateSerial != current.rootExternalStateSerial) {
    return {SceneApplyError::RootSerialMismatch,
            "root external-state serial does not match"};
  }
  if (expected.projectionDigest != current.projectionDigest) {
    return {SceneApplyError::ProjectionMismatch,
            "projection digest does not match"};
  }
  return {};
}

[[nodiscard]] RendererTargetFence
targetFenceFor(const RendererTargetFence &current,
               const SceneChangeHeader &header) {
  RendererTargetFence target = current;
  ++target.mappingEpoch;
  target.authority = header.authority;
  target.appliedDocumentRevision = header.targetDocumentRevision;
  target.sceneActivationEpoch = header.targetSceneActivationEpoch;
  target.rootExternalStateSerial = header.targetRootExternalStateSerial;
  target.activeScene = header.targetActiveScene;
  target.projectionDigest = header.targetProjectionDigest;
  return target;
}

[[nodiscard]] ValidationFailure
validateRequestAgainstActive(const ApplySceneChangeRequest &request,
                             const RendererSceneGeneration &active,
                             SceneChangeDomain requiredDomain) {
  if (!request.canonicalIdentity.valid()) {
    return {SceneApplyError::InvalidPayload,
            "canonical change-set identity is incomplete"};
  }
  if (request.changes.header.domain != requiredDomain) {
    return {SceneApplyError::InvalidPayload,
            "request domain does not match the selected apply entry point"};
  }
  if (requiredDomain == SceneChangeDomain::FullSnapshot &&
      (!request.changes.fullSnapshot.has_value() ||
       !request.changes.fullSnapshot->stableIdRemapComplete)) {
    return {SceneApplyError::InvalidPayload,
            "full snapshot stable-ID remap completeness was not asserted"};
  }
  if (request.canonicalBytes.empty()) {
    return {SceneApplyError::InvalidPayload,
            "canonical change-set bytes are absent"};
  }
  const SceneChangeValidation canonical =
      SceneChangeCanonicalizer::validateCanonical(request.changes,
                                                  request.canonicalBytes);
  if (!canonical.valid()) {
    return {mapCanonicalError(canonical.error), canonical.message};
  }
  const SceneChangeHeader &header = request.changes.header;
  if (request.canonicalIdentity.authority != header.authority ||
      request.canonicalIdentity.domain != header.domain ||
      request.canonicalIdentity.firstMutationSerial !=
          header.firstMutationSerial ||
      request.canonicalIdentity.lastMutationSerial !=
          header.lastMutationSerial ||
      request.canonicalIdentity.targetDocumentRevision !=
          header.targetDocumentRevision ||
      request.canonicalIdentity.targetSceneActivationEpoch !=
          header.targetSceneActivationEpoch ||
      request.canonicalIdentity.targetRootExternalStateSerial !=
          header.targetRootExternalStateSerial) {
    return {SceneApplyError::InvalidState,
            "canonical identity does not bind the logical target envelope"};
  }
  ValidationFailure fence =
      validateExpectedFence(request.expectedTarget, active.fence());
  if (fence.failed()) {
    return fence;
  }
  if (header.authority != active.fence().authority) {
    return {SceneApplyError::AuthorityMismatch,
            "change-set authority does not match the active fence"};
  }
  if (header.baseDocumentRevision < active.fence().appliedDocumentRevision) {
    return {SceneApplyError::StaleRevision,
            "change-set base revision is stale"};
  }
  if (header.baseDocumentRevision > active.fence().appliedDocumentRevision) {
    return {SceneApplyError::RevisionGap,
            "change-set base revision leaves a gap"};
  }
  if (header.baseSceneActivationEpoch != active.fence().sceneActivationEpoch ||
      header.baseActiveScene != active.fence().activeScene) {
    return {SceneApplyError::SceneActivationMismatch,
            "change-set scene activation base does not match"};
  }
  if (header.baseRootExternalStateSerial !=
      active.fence().rootExternalStateSerial) {
    return {SceneApplyError::RootSerialMismatch,
            "change-set root serial base does not match"};
  }
  if (header.baseProjectionDigest != active.fence().projectionDigest) {
    return {SceneApplyError::ProjectionMismatch,
            "change-set projection base does not match"};
  }
  if (active.fence().mappingEpoch ==
      std::numeric_limits<std::uint64_t>::max()) {
    return {SceneApplyError::InvalidState, "mapping epoch would overflow"};
  }
  const RendererTargetFence resulting = targetFenceFor(active.fence(), header);
  const ValidationFailure targetFence =
      validateExpectedFence(request.targetMapping.fence(), resulting);
  if (targetFence.failed()) {
    return targetFence;
  }
  const RendererSceneMappingValidation mapping =
      request.targetMapping.validate();
  if (!mapping.valid()) {
    switch (mapping.error) {
    case RendererSceneMappingError::AuthorityMismatch:
      return {SceneApplyError::AuthorityMismatch, mapping.message};
    case RendererSceneMappingError::RendererGenerationMismatch:
      return {SceneApplyError::RendererMismatch, mapping.message};
    case RendererSceneMappingError::DeviceGenerationMismatch:
      return {SceneApplyError::DeviceMismatch, mapping.message};
    case RendererSceneMappingError::InvalidEntry:
      return {SceneApplyError::StaleObjectGeneration, mapping.message};
    default:
      return {SceneApplyError::MappingMismatch, mapping.message};
    }
  }
  for (const SceneFieldChange &change : request.changes.canonicalChanges) {
    const bool existsBefore = active.mapping().find(change.target) != nullptr;
    const bool existsAfter =
        request.targetMapping.find(change.target) != nullptr;
    if ((change.action == SceneChangeAction::Create &&
         (existsBefore || !existsAfter)) ||
        (change.action == SceneChangeAction::Update &&
         (!existsBefore || !existsAfter)) ||
        (change.action == SceneChangeAction::Delete &&
         (!existsBefore || existsAfter))) {
      return {SceneApplyError::StaleObjectGeneration,
              "stable object generation does not match the mapping transition"};
    }
  }
  const bool continuousOnly =
      !request.changes.canonicalChanges.empty() &&
      std::all_of(request.changes.canonicalChanges.begin(),
                  request.changes.canonicalChanges.end(),
                  [](const SceneFieldChange &change) {
                    return isContinuousClass(change.changeClass);
                  });
  if (requiredDomain != SceneChangeDomain::FullSnapshot && continuousOnly &&
      (request.stagePlan.uploads.geometryUploadCalls != 0u ||
       request.stagePlan.uploads.geometryBytesUploaded != 0u ||
       request.stagePlan.uploads.geometryRangesUploaded != 0u ||
       request.stagePlan.uploads.textureUploadCalls != 0u ||
       request.stagePlan.uploads.textureBytesUploaded != 0u ||
       request.stagePlan.uploads.textureSubresourcesUploaded != 0u)) {
    return {SceneApplyError::InvalidPayload,
            "continuous edit requests forbidden geometry or texture upload"};
  }
  if (request.stagePlan.placeholderResourceCount != 0u &&
      request.stagePlan.placeholderPolicy !=
          ScenePlaceholderPolicy::AllowDeclared) {
    return {SceneApplyError::InvalidPayload,
            "placeholder resources were not explicitly allowed before staging"};
  }
  if (!enumInRange(request.stagePlan.placeholderPolicy,
                   ScenePlaceholderPolicy::AllowDeclared) ||
      request.stagePlan.submissions.size() >
          std::numeric_limits<std::uint32_t>::max()) {
    return {SceneApplyError::InvalidPayload,
            "stage plan policy or submission count is invalid"};
  }
  std::uint64_t coalescedByClass = 0u;
  for (const std::uint64_t count :
       request.stagePlan.coalescedOperationsByClass) {
    if (coalescedByClass > std::numeric_limits<std::uint64_t>::max() - count) {
      return {SceneApplyError::InvalidPayload,
              "coalesced operation counters overflow"};
    }
    coalescedByClass += count;
  }
  if (coalescedByClass != request.stagePlan.coalescedOperationCount) {
    return {SceneApplyError::InvalidPayload,
            "coalesced operation total and class buckets differ"};
  }
  for (std::size_t index = 0; index < request.stagePlan.submissions.size();
       ++index) {
    const SceneQueueSubmissionPlan &submission =
        request.stagePlan.submissions[index];
    if (!submission.valid() ||
        !enumInRange(submission.queueClass, rhi::QueueClass::transfer)) {
      return {
          SceneApplyError::InvalidPayload,
          "submission plan contains an invalid queue or diagnostic identity"};
    }
    const bool duplicate =
        std::any_of(request.stagePlan.submissions.begin(),
                    request.stagePlan.submissions.begin() +
                        static_cast<std::ptrdiff_t>(index),
                    [&](const SceneQueueSubmissionPlan &prior) {
                      return prior.diagnosticId == submission.diagnosticId;
                    });
    if (duplicate) {
      return {SceneApplyError::InvalidPayload,
              "submission diagnostic identity is duplicated"};
    }
  }
  for (std::size_t index = 0;
       index < request.stagePlan.retireResourceDiagnosticIds.size(); ++index) {
    const SceneResourceDiagnosticId id =
        request.stagePlan.retireResourceDiagnosticIds[index];
    const bool duplicate =
        std::find(request.stagePlan.retireResourceDiagnosticIds.begin(),
                  request.stagePlan.retireResourceDiagnosticIds.begin() +
                      static_cast<std::ptrdiff_t>(index),
                  id) != request.stagePlan.retireResourceDiagnosticIds.begin() +
                             static_cast<std::ptrdiff_t>(index);
    const bool exists =
        std::any_of(active.resources().begin(), active.resources().end(),
                    [id](const SceneJournalResource &resource) {
                      return resource.diagnosticId == id;
                    });
    if (!id.valid() || duplicate || !exists) {
      return {
          SceneApplyError::InvalidState,
          "retirement plan references an absent or duplicate active resource"};
    }
  }
  return {};
}

[[nodiscard]] std::vector<SceneResourceDiagnosticId>
diagnosticIds(std::span<const SceneJournalResource> resources) {
  std::vector<SceneResourceDiagnosticId> result;
  result.reserve(resources.size());
  for (const SceneJournalResource &resource : resources) {
    result.push_back(resource.diagnosticId);
  }
  return result;
}

[[nodiscard]] bool
containsDiagnosticId(std::span<const SceneResourceDiagnosticId> ids,
                     SceneResourceDiagnosticId candidate) noexcept {
  return std::find(ids.begin(), ids.end(), candidate) != ids.end();
}

[[nodiscard]] std::vector<rhi::SubmissionToken>
mergeSubmissionTokens(std::span<const rhi::SubmissionToken> first,
                      std::span<const rhi::SubmissionToken> second) {
  std::vector<rhi::SubmissionToken> merged;
  merged.reserve(first.size() + second.size());
  const auto record = [&merged](rhi::SubmissionToken token) {
    if (!token.isValid()) {
      return;
    }
    const auto existing = std::find_if(merged.begin(), merged.end(),
                                       [&](rhi::SubmissionToken candidate) {
                                         return candidate.queue == token.queue;
                                       });
    if (existing == merged.end()) {
      merged.push_back(token);
    } else if (existing->value < token.value) {
      *existing = token;
    }
  };
  for (const rhi::SubmissionToken token : first) {
    record(token);
  }
  for (const rhi::SubmissionToken token : second) {
    record(token);
  }
  std::sort(merged.begin(), merged.end(),
            [](rhi::SubmissionToken left, rhi::SubmissionToken right) {
              return std::tie(left.queue.index, left.queue.generation) <
                     std::tie(right.queue.index, right.queue.generation);
            });
  return merged;
}

} // namespace

bool RendererTargetFence::valid() const noexcept {
  const bool validScene =
      !activeScene ||
      (activeScene->valid() && activeScene->kind == EditorObjectKind::scene &&
       activeScene->authority == authority);
  return validBackend(backend) &&
         enumInRange(capability, RendererSceneUpdateCapability::DeviceLost) &&
         renderer.valid() && device.valid() && mappingEpoch != 0u &&
         authority.valid() && validScene &&
         sceneDigestPresent(projectionDigest);
}

bool RendererProjectionCoordinate::valid() const noexcept {
  constexpr std::uint32_t invalidIndex =
      std::numeric_limits<std::uint32_t>::max();
  const bool semanticCoordinatesValid =
      kind == EditorObjectKind::texture ||
      (colorSceneAssetIndex == invalidIndex &&
       linearSceneAssetIndex == invalidIndex);
  return kind > EditorObjectKind::invalid && kind < EditorObjectKind::count &&
         sceneAssetIndex != invalidIndex && semanticCoordinatesValid &&
         sceneDigestPresent(identityDigest);
}

std::uint32_t RendererProjectionCoordinate::sceneAssetIndexFor(
    SceneTextureSemantic semantic) const noexcept {
  constexpr std::uint32_t invalidIndex =
      std::numeric_limits<std::uint32_t>::max();
  std::uint32_t semanticIndex = invalidIndex;
  switch (semantic) {
  case SceneTextureSemantic::BaseColor:
  case SceneTextureSemantic::Emissive:
    semanticIndex = colorSceneAssetIndex;
    break;
  case SceneTextureSemantic::MetallicRoughness:
  case SceneTextureSemantic::Normal:
  case SceneTextureSemantic::Occlusion:
    semanticIndex = linearSceneAssetIndex;
    break;
  default:
    break;
  }
  return semanticIndex != invalidIndex ? semanticIndex : sceneAssetIndex;
}

RendererSceneMappingSnapshot::RendererSceneMappingSnapshot(
    RendererTargetFence fence, std::vector<RendererSceneMappingEntry> entries)
    : RendererSceneMappingSnapshot(std::move(fence), std::move(entries),
                                   std::nullopt) {}

RendererSceneMappingSnapshot::RendererSceneMappingSnapshot(
    RendererTargetFence fence, std::vector<RendererSceneMappingEntry> entries,
    SceneRuntimeMapping projectionAuthority)
    : RendererSceneMappingSnapshot(std::move(fence), std::move(entries),
                                   std::optional<SceneRuntimeMapping>{
                                       std::move(projectionAuthority)}) {}

RendererSceneMappingSnapshot::RendererSceneMappingSnapshot(
    RendererTargetFence fence, std::vector<RendererSceneMappingEntry> entries,
    std::optional<SceneRuntimeMapping> projectionAuthority)
    : m_fence(std::move(fence)), m_entries(std::move(entries)),
      m_projectionAuthority(std::move(projectionAuthority)) {
  std::sort(m_entries.begin(), m_entries.end(),
            [](const auto &left, const auto &right) {
              return left.stableId < right.stableId;
            });
  m_rendererReverseOrder.resize(m_entries.size());
  m_projectionReverseOrder.resize(m_entries.size());
  for (std::size_t index = 0; index < m_entries.size(); ++index) {
    m_rendererReverseOrder[index] = index;
    m_projectionReverseOrder[index] = index;
  }
  std::sort(m_rendererReverseOrder.begin(), m_rendererReverseOrder.end(),
            [this](std::size_t left, std::size_t right) {
              return m_entries[left].rendererHandle <
                     m_entries[right].rendererHandle;
            });
  std::sort(m_projectionReverseOrder.begin(), m_projectionReverseOrder.end(),
            [this](std::size_t left, std::size_t right) {
              return m_entries[left].projection < m_entries[right].projection;
            });
}

RendererSceneMappingValidation RendererSceneMappingSnapshot::validate() const {
  if (!m_fence.valid()) {
    return {RendererSceneMappingError::InvalidFence,
            RendererSceneMappingValidation::kNoIndex,
            "mapping fence is invalid"};
  }
  if (m_rendererReverseOrder.size() != m_entries.size() ||
      m_projectionReverseOrder.size() != m_entries.size()) {
    return {RendererSceneMappingError::ReverseLookupMismatch,
            RendererSceneMappingValidation::kNoIndex,
            "mapping reverse lookup size does not match entries"};
  }
  if (m_projectionAuthority) {
    const SceneRuntimeMappingFence &authorityFence =
        m_projectionAuthority->fence();
    if (authorityFence.authority != m_fence.authority ||
        authorityFence.appliedDocumentRevision !=
            m_fence.appliedDocumentRevision ||
        authorityFence.activeScene != m_fence.activeScene ||
        authorityFence.sceneActivationEpoch != m_fence.sceneActivationEpoch ||
        authorityFence.rootExternalStateSerial !=
            m_fence.rootExternalStateSerial ||
        authorityFence.projectionDigest != m_fence.projectionDigest ||
        authorityFence.mappingEpoch != m_fence.mappingEpoch) {
      return {RendererSceneMappingError::ProjectionAuthorityMismatch,
              RendererSceneMappingValidation::kNoIndex,
              "projection authority fence differs from renderer mapping"};
    }
    for (const SceneRuntimeMappingEntry &entry :
         m_projectionAuthority->entries()) {
      if (entry.sceneAssetIndex != SceneRuntimeMapping::kInvalidIndex &&
          (entry.projectionIdentity !=
               deriveProjectionIdentityFromSource(entry.documentId) ||
           entry.runtimeIndex != entry.projectionIdentity)) {
        return {RendererSceneMappingError::ProjectionAuthorityMismatch,
                RendererSceneMappingValidation::kNoIndex,
                "projection authority identity is not source-derived"};
      }
    }
  }
  for (std::size_t index = 0; index < m_entries.size(); ++index) {
    const RendererSceneMappingEntry &entry = m_entries[index];
    if (!entry.stableId.valid() || !entry.projection.valid() ||
        !entry.rendererHandle.valid() ||
        !enumInRange(entry.rendererHandle.kind,
                     RendererObjectKind::Descriptor) ||
        entry.stableId.kind != entry.projection.kind ||
        entry.resourceContentDigest.has_value() !=
            entry.resourceSchemaDigest.has_value() ||
        !validOptionalDigest(entry.resourceContentDigest) ||
        !validOptionalDigest(entry.resourceSchemaDigest)) {
      return {
          RendererSceneMappingError::InvalidEntry, index,
          "mapping entry identity, coordinate, handle, or digest is invalid"};
    }
    if (entry.stableId.authority != m_fence.authority) {
      return {RendererSceneMappingError::AuthorityMismatch, index,
              "mapping entry authority differs from its fence"};
    }
    if (entry.rendererHandle.rendererGeneration !=
        m_fence.renderer.generation) {
      return {RendererSceneMappingError::RendererGenerationMismatch, index,
              "renderer handle generation differs from its fence"};
    }
    if (entry.rendererHandle.deviceGeneration != m_fence.device.generation) {
      return {RendererSceneMappingError::DeviceGenerationMismatch, index,
              "renderer handle device generation differs from its fence"};
    }
    if (index != 0u && m_entries[index - 1u].stableId == entry.stableId) {
      return {RendererSceneMappingError::DuplicateStableId, index,
              "mapping contains a duplicate stable identity"};
    }
  }
  for (std::size_t order = 0; order < m_entries.size(); ++order) {
    const std::size_t rendererIndex = m_rendererReverseOrder[order];
    const std::size_t projectionIndex = m_projectionReverseOrder[order];
    if (rendererIndex >= m_entries.size() ||
        projectionIndex >= m_entries.size()) {
      return {RendererSceneMappingError::ReverseLookupMismatch, order,
              "mapping reverse lookup contains an out-of-range index"};
    }
    if (order != 0u &&
        m_entries[m_rendererReverseOrder[order - 1u]].rendererHandle ==
            m_entries[rendererIndex].rendererHandle) {
      return {RendererSceneMappingError::DuplicateRendererHandle, rendererIndex,
              "mapping contains a duplicate renderer handle"};
    }
    if (order != 0u &&
        m_entries[m_projectionReverseOrder[order - 1u]].projection ==
            m_entries[projectionIndex].projection) {
      return {RendererSceneMappingError::DuplicateProjectionCoordinate,
              projectionIndex,
              "mapping contains a duplicate projection coordinate"};
    }
  }
  return {};
}

const RendererSceneMappingEntry *
RendererSceneMappingSnapshot::find(StableSceneId stableId) const noexcept {
  const auto found = std::lower_bound(
      m_entries.begin(), m_entries.end(), stableId,
      [](const RendererSceneMappingEntry &entry, StableSceneId candidate) {
        return entry.stableId < candidate;
      });
  return found != m_entries.end() && found->stableId == stableId ? &*found
                                                                 : nullptr;
}

std::optional<StableSceneId> RendererSceneMappingSnapshot::reverseLookup(
    RendererObjectHandle handle) const noexcept {
  const auto found = std::lower_bound(
      m_rendererReverseOrder.begin(), m_rendererReverseOrder.end(), handle,
      [this](std::size_t index, RendererObjectHandle candidate) {
        return m_entries[index].rendererHandle < candidate;
      });
  return found != m_rendererReverseOrder.end() &&
                 m_entries[*found].rendererHandle == handle
             ? std::optional<StableSceneId>{m_entries[*found].stableId}
             : std::nullopt;
}

std::optional<StableSceneId> RendererSceneMappingSnapshot::reverseLookup(
    RendererProjectionCoordinate coordinate) const noexcept {
  const auto found = std::lower_bound(
      m_projectionReverseOrder.begin(), m_projectionReverseOrder.end(),
      coordinate,
      [this](std::size_t index, const RendererProjectionCoordinate &candidate) {
        return m_entries[index].projection < candidate;
      });
  return found != m_projectionReverseOrder.end() &&
                 m_entries[*found].projection == coordinate
             ? std::optional<StableSceneId>{m_entries[*found].stableId}
             : std::nullopt;
}

bool SceneCanonicalIdentity::valid() const noexcept {
  const bool serialsValid =
      (domain == SceneChangeDomain::FullSnapshot && firstMutationSerial == 0u &&
       lastMutationSerial == 0u) ||
      (firstMutationSerial != 0u && lastMutationSerial >= firstMutationSerial);
  return authority.valid() &&
         enumInRange(domain, SceneChangeDomain::FullSnapshot) && serialsValid;
}

RendererSceneGeneration::RendererSceneGeneration(
    std::uint64_t generation, std::uint64_t descriptorGeneration,
    RendererTargetFence fence, RendererSceneMappingSnapshot mapping,
    SceneDigest activeGenerationDigest,
    std::vector<SceneJournalResource> resources,
    std::vector<rhi::SubmissionToken> lastUseSubmissionTokens,
    SceneCanonicalIdentity canonicalIdentity, SceneDigest canonicalDigest,
    std::vector<std::uint8_t> canonicalBytes,
    RendererTargetFence canonicalBaseFence)
    : m_generation(generation), m_descriptorGeneration(descriptorGeneration),
      m_fence(std::move(fence)), m_mapping(std::move(mapping)),
      m_activeGenerationDigest(activeGenerationDigest),
      m_resources(std::move(resources)),
      m_lastUseSubmissionTokens(std::move(lastUseSubmissionTokens)),
      m_canonicalIdentity(canonicalIdentity),
      m_canonicalDigest(canonicalDigest),
      m_canonicalBytes(std::move(canonicalBytes)),
      m_canonicalBaseFence(std::move(canonicalBaseFence)) {}

RendererScenePublisher::RendererScenePublisher(
    RendererSceneInitialState initialState, SceneTransactionBackend &backend,
    SceneDeltaCounters &counters)
    : m_backend(backend), m_counters(counters) {
  if (!initialState.fence.valid() ||
      initialState.mapping.fence() != initialState.fence ||
      !initialState.mapping.validate().valid() ||
      !sceneDigestPresent(initialState.activeGenerationDigest) ||
      !validResourceSet(initialState.resources) ||
      !validSubmissionTokenSet(initialState.lastUseSubmissionTokens) ||
      initialState.generation == 0u ||
      initialState.descriptorGeneration == 0u) {
    throw std::invalid_argument("renderer scene initial state is invalid");
  }
  auto generation = std::make_shared<const RendererSceneGeneration>(
      initialState.generation, initialState.descriptorGeneration,
      initialState.fence, std::move(initialState.mapping),
      initialState.activeGenerationDigest, std::move(initialState.resources),
      std::move(initialState.lastUseSubmissionTokens), SceneCanonicalIdentity{},
      SceneDigest{}, std::vector<std::uint8_t>{}, initialState.fence);
  m_active.store(std::move(generation));
  SceneDeltaCounterSnapshot snapshot = m_counters.snapshot();
  setCurrentCounters(snapshot, initialState.fence,
                     initialState.activeGenerationDigest);
  m_counters.commit(snapshot);
}

std::shared_ptr<const RendererSceneGeneration>
RendererScenePublisher::activeGeneration() const noexcept {
  return m_active.load();
}

SceneDeltaCounterSnapshot RendererScenePublisher::counters() const noexcept {
  return m_counters.snapshot();
}

std::shared_ptr<const RendererSceneGeneration>
RendererScenePublisher::stageSceneGeneration(
    const RendererSceneGeneration &active,
    const ApplySceneChangeRequest &request, RendererTargetFence resultingFence,
    std::vector<SceneJournalResource> resources,
    std::vector<rhi::SubmissionToken> lastUseSubmissionTokens) const {
  const SceneDigest activeGenerationDigest =
      request.changes.fullSnapshot
          ? request.changes.fullSnapshot->targetSnapshotDigest
          : request.changes.header.canonicalChangeSetDigest;
  return std::make_shared<const RendererSceneGeneration>(
      active.generation() + 1u, active.descriptorGeneration() + 1u,
      std::move(resultingFence), request.targetMapping, activeGenerationDigest,
      std::move(resources), std::move(lastUseSubmissionTokens),
      request.canonicalIdentity,
      request.changes.header.canonicalChangeSetDigest, request.canonicalBytes,
      request.expectedTarget);
}

std::shared_ptr<const RendererSceneGeneration>
RendererScenePublisher::publishSceneGeneration(
    std::shared_ptr<const RendererSceneGeneration> candidate) noexcept {
  return m_active.exchange(std::move(candidate));
}

rhi::Result<SceneRetirementReservation>
RendererScenePublisher::retireSceneGeneration(
    std::span<const SceneJournalResource> resources,
    std::span<const rhi::SubmissionToken> dependencies) noexcept {
  return m_backend.reserveSceneRetirement(resources, dependencies);
}

bool RendererScenePublisher::hasPendingOrphanRetirement() const noexcept {
  const std::lock_guard lock{m_applyMutex};
  return static_cast<bool>(m_pendingOrphan);
}

rhi::RHIResult RendererScenePublisher::retryPendingOrphanRetirement() noexcept {
  const std::lock_guard lock{m_applyMutex};
  return retryPendingOrphanRetirementLocked();
}

rhi::RHIResult RendererScenePublisher::retryPendingOrphanRetirementLocked() noexcept {
  if (!m_pendingOrphan) {
    return rhi::RHIResult::ok();
  }
  SceneJournalCleanupResult cleanup =
      m_pendingOrphan->state() == SceneResourceJournalState::Staging
          ? m_pendingOrphan->rollbackBeforeSubmit(m_backend)
          : m_pendingOrphan->rollbackAfterSubmit(m_backend);
  if (cleanup.succeeded()) {
    m_pendingOrphan.reset();
  }
  return cleanup.rhiResult;
}

SceneApplyResult RendererScenePublisher::applySceneChanges(
    const ApplySceneChangeRequest &request) noexcept {
  const SceneChangeDomain domain = request.changes.header.domain;
  return apply(request, domain == SceneChangeDomain::FullSnapshot
                            ? SceneChangeDomain::DocumentMutation
                            : domain);
}

SceneApplyResult RendererScenePublisher::applyFullSceneSnapshot(
    const ApplySceneChangeRequest &request) noexcept {
  return apply(request, SceneChangeDomain::FullSnapshot);
}

std::optional<SceneApplyResult> RendererScenePublisher::preflightSceneChanges(
    const ApplySceneChangeRequest &request) const noexcept {
  const SceneChangeDomain domain = request.changes.header.domain;
  return preflight(request, domain == SceneChangeDomain::FullSnapshot
                                ? SceneChangeDomain::DocumentMutation
                                : domain);
}

std::optional<SceneApplyResult>
RendererScenePublisher::preflightFullSceneSnapshot(
    const ApplySceneChangeRequest &request) const noexcept {
  return preflight(request, SceneChangeDomain::FullSnapshot);
}

std::optional<SceneApplyResult> RendererScenePublisher::preflight(
    const ApplySceneChangeRequest &request,
    SceneChangeDomain requiredDomain) const noexcept {
  const std::lock_guard lock{m_applyMutex};
  SceneApplyResult result;
  const auto active = m_active.load();
  const SceneDeltaCounterSnapshot counters = m_counters.snapshot();
  result.countersBefore = counters;
  result.countersAfter = counters;
  result.canonicalIdentity = request.canonicalIdentity;
  result.canonicalDigest = request.changes.header.canonicalChangeSetDigest;
  result.oldFence = active->fence();
  result.rhiResult = rhi::RHIResult::ok();
  try {
    const ValidationFailure validation =
        validateRequestAgainstActive(request, *active, requiredDomain);
    if (!validation.failed()) {
      return std::nullopt;
    }
    result.disposition = SceneApplyDisposition::Rejected;
    result.error = validation.error;
    result.message = validation.message;
    result.requestedFullSnapshotReason = fallbackReason(validation.error);
    return result;
  } catch (const std::bad_alloc &) {
    result.disposition = SceneApplyDisposition::Rejected;
    result.error = SceneApplyError::AllocationFailure;
    result.message = "request preflight allocation failed";
    return result;
  } catch (...) {
    result.disposition = SceneApplyDisposition::Rejected;
    result.error = SceneApplyError::InvalidState;
    result.message = "request preflight validation failed";
    return result;
  }
}

SceneApplyResult
RendererScenePublisher::apply(const ApplySceneChangeRequest &request,
                              SceneChangeDomain requiredDomain) noexcept {
  const std::lock_guard lock{m_applyMutex};
  SceneApplyResult result;
  const auto active = m_active.load();
  const SceneDeltaCounterSnapshot before = m_counters.snapshot();
  result.countersBefore = before;
  result.countersAfter = before;
  result.canonicalIdentity = request.canonicalIdentity;
  result.canonicalDigest = request.changes.header.canonicalChangeSetDigest;
  result.oldFence = active->fence();
  result.rhiResult = rhi::RHIResult::ok();

  const bool sameIdentity =
      active->canonicalIdentity().valid() &&
      request.canonicalIdentity == active->canonicalIdentity();
  if (sameIdentity) {
    const bool sameBytes =
        request.canonicalBytes.size() == active->canonicalBytes().size() &&
        std::equal(request.canonicalBytes.begin(), request.canonicalBytes.end(),
                   active->canonicalBytes().begin(),
                   active->canonicalBytes().end());
    const bool sameDigest = request.changes.header.canonicalChangeSetDigest ==
                            active->canonicalDigest();
    if (!sameBytes || !sameDigest) {
      result.disposition = SceneApplyDisposition::Rejected;
      result.error = SceneApplyError::InvalidState;
      result.message = "canonical identity was reused with different canonical "
                       "bytes or digest";
      return result;
    }
    if (request.expectedTarget == active->canonicalBaseFence()) {
      result.disposition = SceneApplyDisposition::AlreadyApplied;
      result.error = SceneApplyError::None;
      result.message.clear();
      result.oldFence = active->fence();
      result.resultingFence = active->fence();
      return result;
    }
  }

  auto reject = [&](SceneApplyError error,
                    std::string message) -> SceneApplyResult {
    SceneDeltaCounterSnapshot after = before;
    ++after.sceneChangeSetsReceived;
    ++after.sceneChangeSetsRejected;
    const std::size_t errorIndex = sceneApplyErrorIndex(error);
    if (errorIndex < after.sceneChangeSetsByError.size()) {
      ++after.sceneChangeSetsByError[errorIndex];
    }
    m_counters.commit(after);
    result.disposition = SceneApplyDisposition::Rejected;
    result.error = error;
    result.message = std::move(message);
    result.requestedFullSnapshotReason = fallbackReason(error);
    result.countersAfter = after;
    result.counterDelta = sceneDeltaCounterDelta(before, after);
    return std::move(result);
  };

  if (sameIdentity) {
    return reject(
        SceneApplyError::InvalidState,
        "canonical identity belongs to a different active fence lineage");
  }
  try {
    const ValidationFailure validation =
        validateRequestAgainstActive(request, *active, requiredDomain);
    if (validation.failed()) {
      return reject(validation.error, validation.message);
    }

    // Validate the complete request first. Retrying an orphan can call into
    // the RHI, so stale/gap/mapping/plan rejections must observe no such work.
    if (m_pendingOrphan) {
      const rhi::RHIResult retry = retryPendingOrphanRetirementLocked();
      if (!retry.succeeded()) {
        return reject(SceneApplyError::RetirementEnqueueFailure,
                      "pending orphan retirement still cannot be enqueued");
      }
    }

    auto journal = std::make_unique<SceneDeltaResourceJournal>();
    std::vector<SceneJournalResource> createdResources;
    std::vector<SceneJournalResource> retainedResources;
    std::vector<SceneJournalResource> retiredResources;
    std::vector<SceneResourceDiagnosticId> createdIds;
    std::vector<rhi::SubmissionToken> submissionTokens;
    SceneRetirementReservation retirementReservation{};

    retainedResources.reserve(active->resources().size());
    retiredResources.reserve(
        request.stagePlan.retireResourceDiagnosticIds.size());
    for (const SceneJournalResource &resource : active->resources()) {
      if (containsDiagnosticId(request.stagePlan.retireResourceDiagnosticIds,
                               resource.diagnosticId)) {
        retiredResources.push_back(resource);
      } else {
        retainedResources.push_back(resource);
      }
    }

    auto fail = [&](SceneApplyError error, std::string message,
                    rhi::RHIResult backendResult,
                    bool afterSubmit) -> SceneApplyResult {
      const SceneApplyError primaryError = error;
      SceneJournalCleanupResult cleanup;
      if (!journal->empty()) {
        cleanup = afterSubmit ? journal->rollbackAfterSubmit(m_backend)
                              : journal->rollbackBeforeSubmit(m_backend);
        if (cleanup.ownershipRetained) {
          error = afterSubmit ? SceneApplyError::RetirementEnqueueFailure
                              : SceneApplyError::PublicationInvariant;
          message += afterSubmit
                         ? "; submitted orphan retirement enqueue failed"
                         : "; unpublished resource destruction failed";
          backendResult = cleanup.rhiResult;
          m_pendingOrphan = std::move(journal);
        }
      }
      if (retirementReservation.valid()) {
        m_backend.cancelSceneRetirement(retirementReservation);
      }
      SceneDeltaCounterSnapshot after = before;
      ++after.sceneChangeSetsReceived;
      ++after.rollbackCount;
      if (afterSubmit) {
        ++after.sceneChangeSetsFailedAfterSubmit;
      } else {
        ++after.sceneChangeSetsFailedBeforeSubmit;
      }
      const std::size_t errorIndex = sceneApplyErrorIndex(error);
      if (errorIndex < after.sceneChangeSetsByError.size()) {
        ++after.sceneChangeSetsByError[errorIndex];
      }
      const auto incrementFailureReason =
          [&after](SceneApplyError reason) noexcept {
            switch (reason) {
            case SceneApplyError::AllocationFailure:
              ++after.allocationFailureCount;
              break;
            case SceneApplyError::DescriptorStagingFailure:
              ++after.descriptorStagingFailureCount;
              break;
            case SceneApplyError::RecordingFailure:
              ++after.recordingFailureCount;
              break;
            case SceneApplyError::SubmissionFailure:
              ++after.submissionFailureCount;
              break;
            case SceneApplyError::PublicationInvariant:
              ++after.publicationFailureCount;
              break;
            case SceneApplyError::RetirementEnqueueFailure:
              ++after.retirementEnqueueFailureCount;
              break;
            default:
              break;
            }
          };
      incrementFailureReason(primaryError);
      if (error != primaryError) {
        incrementFailureReason(error);
      }
      after.resourcesCreated += createdIds.size();
      after.resourcesPhysicallyDestroyed += cleanup.physicallyDestroyed.size();
      after.resourcesLogicallyRetired += cleanup.logicallyRetired.size();
      if (afterSubmit) {
        after.resourcesOrphaned += createdIds.size();
        after.retirementBacklog += cleanup.eventuallyDestroyed.size();
        after.submissions += submissionTokens.size();
        if (requiredDomain == SceneChangeDomain::FullSnapshot) {
          after.fullSnapshotSubmissions += submissionTokens.size();
        } else {
          after.incrementalSubmissions += submissionTokens.size();
        }
      }
      m_counters.commit(after);

      result.disposition = afterSubmit
                               ? SceneApplyDisposition::FailedAfterSubmit
                               : SceneApplyDisposition::FailedBeforeSubmit;
      result.error = error;
      result.message = std::move(message);
      result.rhiResult = backendResult;
      result.resultingFence.reset();
      result.queueSubmissionTokens = std::move(submissionTokens);
      result.handles.created = std::move(createdIds);
      result.handles.logicallyRetired = std::move(cleanup.logicallyRetired);
      result.handles.eventuallyDestroyed =
          std::move(cleanup.eventuallyDestroyed);
      result.handles.createdCount = result.handles.created.size();
      result.handles.logicallyRetiredCount =
          result.handles.logicallyRetired.size();
      result.handles.eventuallyDestroyedCount =
          result.handles.eventuallyDestroyed.size();
      result.requestedFullSnapshotReason = fallbackReason(error);
      result.placeholderResourceCount = 0u;
      result.countersAfter = after;
      result.counterDelta = sceneDeltaCounterDelta(before, after);
      return std::move(result);
    };

    rhi::RHIResult backendResult =
        m_backend.stageCpuGeneration(request.changes, request.targetMapping);
    if (!backendResult.succeeded()) {
      return fail(
          logicalRhiFailure(SceneApplyError::AllocationFailure, backendResult),
          "CPU generation staging failed", backendResult, false);
    }

    backendResult =
        m_backend.allocateSceneResources(request.changes, createdResources);
    if (!journal->adopt(createdResources)) {
      try {
        createdIds = diagnosticIds(createdResources);
      } catch (...) {
        // The backend contract is already invalid; cleanup remains mandatory.
      }
      const rhi::RHIResult cleanup =
          m_backend.destroyUnpublishedResources(createdResources);
      SceneDeltaCounterSnapshot after = before;
      ++after.sceneChangeSetsReceived;
      ++after.sceneChangeSetsFailedBeforeSubmit;
      ++after.rollbackCount;
      ++after.publicationFailureCount;
      ++after.sceneChangeSetsByError[sceneApplyErrorIndex(
          SceneApplyError::PublicationInvariant)];
      after.resourcesCreated += createdResources.size();
      if (cleanup.succeeded()) {
        after.resourcesPhysicallyDestroyed += createdResources.size();
      }
      m_counters.commit(after);
      result.disposition = SceneApplyDisposition::FailedBeforeSubmit;
      result.error = SceneApplyError::PublicationInvariant;
      result.message = "backend returned invalid or duplicate staged resources";
      result.rhiResult =
          cleanup.succeeded()
              ? rhi::RHIResult::fail(rhi::RHIErrorCode::invalidState,
                                     "invalid staged resource list")
              : cleanup;
      result.handles.created = std::move(createdIds);
      result.handles.eventuallyDestroyed = result.handles.created;
      result.handles.createdCount = result.handles.created.size();
      result.handles.eventuallyDestroyedCount =
          result.handles.eventuallyDestroyed.size();
      result.countersAfter = after;
      result.counterDelta = sceneDeltaCounterDelta(before, after);
      return result;
    }
    try {
      createdIds = diagnosticIds(journal->resources());
      if (!backendResult.succeeded()) {
        return fail(logicalRhiFailure(SceneApplyError::AllocationFailure,
                                      backendResult),
                    "RHI resource allocation failed", backendResult, false);
      }

      const bool collidesWithActive = std::any_of(
          journal->resources().begin(), journal->resources().end(),
          [&](const SceneJournalResource &created) {
            return std::any_of(
                active->resources().begin(), active->resources().end(),
                [&](const SceneJournalResource &existing) {
                  return existing.diagnosticId == created.diagnosticId ||
                         existing.handle == created.handle;
                });
          });
      if (collidesWithActive) {
        return fail(
            SceneApplyError::PublicationInvariant,
            "staged resource aliases an active handle or diagnostic identity",
            rhi::RHIResult::fail(rhi::RHIErrorCode::invalidHandle,
                                 "staged resource aliases active state"),
            false);
      }

      backendResult = m_backend.stageSceneDescriptors(request.changes,
                                                      journal->resources());
      if (!backendResult.succeeded()) {
        return fail(logicalRhiFailure(SceneApplyError::DescriptorStagingFailure,
                                      backendResult),
                    "descriptor generation staging failed", backendResult,
                    false);
      }

      backendResult = m_backend.recordSceneGeneration(request.changes,
                                                      journal->resources());
      if (!backendResult.succeeded()) {
        std::string message = "scene command recording failed";
        if (backendResult.error.message != nullptr &&
            backendResult.error.message[0] != '\0') {
          message += ": ";
          message += backendResult.error.message;
        }
        return fail(
            logicalRhiFailure(SceneApplyError::RecordingFailure, backendResult),
            std::move(message), backendResult, false);
      }

      submissionTokens.reserve(request.stagePlan.submissions.size());
      journal->reserveSubmissions(request.stagePlan.submissions.size());
      for (std::size_t index = 0; index < request.stagePlan.submissions.size();
           ++index) {
        const rhi::Result<rhi::SubmissionToken> submitted =
            m_backend.submitSceneGeneration(
                request.stagePlan.submissions[index],
                static_cast<std::uint32_t>(index));
        if (!submitted.succeeded()) {
          const SceneApplyError error =
              submitted.error.code == rhi::RHIErrorCode::deviceLost
                  ? SceneApplyError::DeviceLost
                  : SceneApplyError::SubmissionFailure;
          return fail(error, "scene queue submission failed",
                      rhi::RHIResult::fail(submitted.error.code,
                                           submitted.error.message),
                      !submissionTokens.empty());
        }
        // Record the returned token before checking its validity/order. A
        // successful submit is irrevocable; losing this token would make
        // rollback either leak or destroy submitted resources unsafely.
        submissionTokens.push_back(submitted.value);
        if (!journal->captureSubmission(
                submitted.value, static_cast<std::uint32_t>(index))) {
          std::string message =
              "backend returned an invalid or non-monotonic submission token "
              "(queue=";
          message += std::to_string(submitted.value.queue.index);
          message += ':';
          message += std::to_string(submitted.value.queue.generation);
          message += ", value=";
          message += std::to_string(submitted.value.value);
          message += ')';
          return fail(
              SceneApplyError::PublicationInvariant,
              std::move(message),
              rhi::RHIResult::fail(rhi::RHIErrorCode::invalidState,
                                   "invalid submission token"),
              true);
        }
      }

      backendResult =
          m_backend.validateBeforePublication(request.targetMapping);
      if (!backendResult.succeeded()) {
        return fail(logicalRhiFailure(SceneApplyError::PublicationInvariant,
                                      backendResult),
                    "prepublication invariant validation failed", backendResult,
                    !submissionTokens.empty());
      }

      const RendererTargetFence resultingFence =
          targetFenceFor(active->fence(), request.changes.header);
      if (active->generation() == std::numeric_limits<std::uint64_t>::max() ||
          active->descriptorGeneration() ==
              std::numeric_limits<std::uint64_t>::max()) {
        return fail(SceneApplyError::PublicationInvariant,
                    "renderer or descriptor generation would overflow",
                    rhi::RHIResult::fail(rhi::RHIErrorCode::invalidState,
                                         "generation overflow"),
                    !submissionTokens.empty());
      }

      std::shared_ptr<const RendererSceneGeneration> candidate;
      SceneDeltaCounterSnapshot after = before;
      const bool fullSnapshot =
          requiredDomain == SceneChangeDomain::FullSnapshot;
      try {
        std::vector<SceneJournalResource> candidateResources =
            retainedResources;
        candidateResources.reserve(retainedResources.size() +
                                   journal->resources().size());
        candidateResources.insert(candidateResources.end(),
                                  journal->resources().begin(),
                                  journal->resources().end());
        std::vector<rhi::SubmissionToken> candidateDependencies =
            retainedResources.empty()
                ? submissionTokens
                : mergeSubmissionTokens(active->lastUseSubmissionTokens(),
                                        submissionTokens);
        candidate = stageSceneGeneration(*active, request, resultingFence,
                                         std::move(candidateResources),
                                         std::move(candidateDependencies));

        ++after.sceneChangeSetsReceived;
        if (fullSnapshot) {
          ++after.sceneChangeSetsAcceptedFullSnapshot;
          ++after.fullSnapshotCount;
          const FullRebuildReason reason = request.changes.fullSnapshot->reason;
          ++after.fullSnapshotCountByReason[fullRebuildReasonIndex(reason)];
        } else {
          ++after.sceneChangeSetsAcceptedIncremental;
        }
        after.sceneOperationsReceived +=
            request.changes.canonicalChanges.size() +
            request.stagePlan.coalescedOperationCount;
        after.sceneOperationsCoalesced +=
            request.stagePlan.coalescedOperationCount;
        after.sceneOperationsApplied += request.changes.canonicalChanges.size();
        for (const SceneFieldChange &change :
             request.changes.canonicalChanges) {
          incrementOperationCounter(after, change, true);
        }
        for (std::size_t index = 0;
             index < request.stagePlan.coalescedOperationsByClass.size();
             ++index) {
          after.sceneOperationsCoalescedByClass[index] +=
              request.stagePlan.coalescedOperationsByClass[index];
          after.sceneOperationsReceivedByClass[index] +=
              request.stagePlan.coalescedOperationsByClass[index];
        }
        after.resourcesCreated += journal->resources().size();
        after.resourcesLogicallyRetired += retiredResources.size();
        after.retirementBacklog += retiredResources.size();
        after.placeholderResourceCount +=
            request.stagePlan.placeholderResourceCount;
        addUploadCounters(after, request.stagePlan, fullSnapshot);
        setCurrentCounters(after, resultingFence,
                           candidate->activeGenerationDigest());

        result.disposition = fullSnapshot
                                 ? SceneApplyDisposition::AcceptedFullSnapshot
                                 : SceneApplyDisposition::AcceptedIncremental;
        result.error = SceneApplyError::None;
        result.message.clear();
        result.resultingFence = resultingFence;
        result.queueSubmissionTokens = submissionTokens;
        result.handles.created = createdIds;
        result.handles.logicallyRetired = diagnosticIds(retiredResources);
        result.handles.eventuallyDestroyed = result.handles.logicallyRetired;
        result.handles.createdCount = result.handles.created.size();
        result.handles.logicallyRetiredCount =
            result.handles.logicallyRetired.size();
        result.handles.eventuallyDestroyedCount =
            result.handles.eventuallyDestroyed.size();
        result.placeholderResourceCount =
            request.stagePlan.placeholderResourceCount;
        result.countersAfter = after;
        result.counterDelta = sceneDeltaCounterDelta(before, after);
      } catch (const std::bad_alloc &) {
        return fail(SceneApplyError::AllocationFailure,
                    "candidate generation allocation failed after submission",
                    rhi::RHIResult::fail(rhi::RHIErrorCode::outOfMemory,
                                         "candidate allocation failure"),
                    !submissionTokens.empty());
      }

      if (!retiredResources.empty()) {
        const auto reservation = retireSceneGeneration(
            retiredResources, active->lastUseSubmissionTokens());
        if (!reservation.succeeded() || !reservation.value.valid()) {
          const SceneApplyError error =
              reservation.error.code == rhi::RHIErrorCode::deviceLost
                  ? SceneApplyError::DeviceLost
                  : SceneApplyError::RetirementEnqueueFailure;
          return fail(error,
                      "superseded generation retirement reservation failed",
                      rhi::RHIResult::fail(reservation.error.code,
                                           reservation.error.message),
                      !submissionTokens.empty());
        }
        retirementReservation = reservation.value;
      }

      (void)publishSceneGeneration(std::move(candidate));
      journal->releasePublished();
      if (retirementReservation.valid()) {
        m_backend.commitSceneRetirement(retirementReservation);
      }
      m_counters.commit(after);
      return result;
    } catch (const std::bad_alloc &) {
      return fail(SceneApplyError::AllocationFailure,
                  "scene transaction allocation failed after resource staging",
                  rhi::RHIResult::fail(rhi::RHIErrorCode::outOfMemory,
                                       "transaction allocation failure"),
                  !submissionTokens.empty());
    }
  } catch (const std::bad_alloc &) {
    SceneDeltaCounterSnapshot after = before;
    ++after.sceneChangeSetsReceived;
    ++after.sceneChangeSetsFailedBeforeSubmit;
    ++after.rollbackCount;
    ++after.allocationFailureCount;
    ++after.sceneChangeSetsByError[sceneApplyErrorIndex(
        SceneApplyError::AllocationFailure)];
    m_counters.commit(after);
    result.disposition = SceneApplyDisposition::FailedBeforeSubmit;
    result.error = SceneApplyError::AllocationFailure;
    result.message =
        "scene transaction allocation failed before staging completed";
    result.rhiResult = rhi::RHIResult::fail(
        rhi::RHIErrorCode::outOfMemory, "CPU transaction allocation failure");
    result.countersAfter = after;
    result.counterDelta = sceneDeltaCounterDelta(before, after);
    return result;
  } catch (...) {
    SceneDeltaCounterSnapshot after = before;
    ++after.sceneChangeSetsReceived;
    ++after.sceneChangeSetsFailedBeforeSubmit;
    ++after.rollbackCount;
    ++after.sceneChangeSetsByError[sceneApplyErrorIndex(
        SceneApplyError::InvalidState)];
    m_counters.commit(after);
    result.disposition = SceneApplyDisposition::FailedBeforeSubmit;
    result.error = SceneApplyError::InvalidState;
    result.message = "scene transaction encountered an unexpected CPU failure";
    result.countersAfter = after;
    result.counterDelta = sceneDeltaCounterDelta(before, after);
    return result;
  }
}

} // namespace demo::editor
