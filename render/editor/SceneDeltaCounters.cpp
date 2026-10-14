#include "SceneDeltaCounters.h"

#include <algorithm>
#include <limits>

namespace demo::editor {
namespace {

[[nodiscard]] constexpr std::uint64_t difference(std::uint64_t before,
                                                 std::uint64_t after) noexcept {
  return after >= before ? after - before : 0u;
}

template <std::size_t Size>
void subtractArray(const std::array<std::uint64_t, Size> &before,
                   const std::array<std::uint64_t, Size> &after,
                   std::array<std::uint64_t, Size> &result) noexcept {
  for (std::size_t index = 0; index < Size; ++index) {
    result[index] = difference(before[index], after[index]);
  }
}

template <std::size_t Size>
[[nodiscard]] bool
arrayIsZero(const std::array<std::uint64_t, Size> &values) noexcept {
  return std::all_of(values.begin(), values.end(),
                     [](std::uint64_t value) { return value == 0u; });
}

} // namespace

const char *
sceneApplyDispositionName(SceneApplyDisposition disposition) noexcept {
  switch (disposition) {
  case SceneApplyDisposition::AcceptedIncremental:
    return "AcceptedIncremental";
  case SceneApplyDisposition::AcceptedFullSnapshot:
    return "AcceptedFullSnapshot";
  case SceneApplyDisposition::AlreadyApplied:
    return "AlreadyApplied";
  case SceneApplyDisposition::Rejected:
    return "Rejected";
  case SceneApplyDisposition::FailedBeforeSubmit:
    return "FailedBeforeSubmit";
  case SceneApplyDisposition::FailedAfterSubmit:
    return "FailedAfterSubmit";
  }
  return "Unknown";
}

const char *sceneApplyErrorName(SceneApplyError error) noexcept {
  switch (error) {
  case SceneApplyError::None:
    return "None";
  case SceneApplyError::InvalidSchema:
    return "InvalidSchema";
  case SceneApplyError::InvalidDigest:
    return "InvalidDigest";
  case SceneApplyError::InvalidPayload:
    return "InvalidPayload";
  case SceneApplyError::InvalidState:
    return "InvalidState";
  case SceneApplyError::AuthorityMismatch:
    return "AuthorityMismatch";
  case SceneApplyError::StaleRevision:
    return "StaleRevision";
  case SceneApplyError::RevisionGap:
    return "RevisionGap";
  case SceneApplyError::SceneActivationMismatch:
    return "SceneActivationMismatch";
  case SceneApplyError::RootSerialMismatch:
    return "RootSerialMismatch";
  case SceneApplyError::ProjectionMismatch:
    return "ProjectionMismatch";
  case SceneApplyError::MappingMismatch:
    return "MappingMismatch";
  case SceneApplyError::RendererMismatch:
    return "RendererMismatch";
  case SceneApplyError::DeviceMismatch:
    return "DeviceMismatch";
  case SceneApplyError::DeviceLost:
    return "DeviceLost";
  case SceneApplyError::StaleObjectGeneration:
    return "StaleObjectGeneration";
  case SceneApplyError::UnsupportedCapability:
    return "UnsupportedCapability";
  case SceneApplyError::AllocationFailure:
    return "AllocationFailure";
  case SceneApplyError::DescriptorStagingFailure:
    return "DescriptorStagingFailure";
  case SceneApplyError::RecordingFailure:
    return "RecordingFailure";
  case SceneApplyError::SubmissionFailure:
    return "SubmissionFailure";
  case SceneApplyError::PublicationInvariant:
    return "PublicationInvariant";
  case SceneApplyError::RetirementEnqueueFailure:
    return "RetirementEnqueueFailure";
  }
  return "Unknown";
}

SceneDeltaCounterSnapshot
sceneDeltaCounterDelta(const SceneDeltaCounterSnapshot &before,
                       const SceneDeltaCounterSnapshot &after) noexcept {
  SceneDeltaCounterSnapshot result{};
#define MGIF_COUNTER_DELTA(field)                                              \
  result.field = difference(before.field, after.field)
  MGIF_COUNTER_DELTA(sceneChangeSetsReceived);
  MGIF_COUNTER_DELTA(sceneChangeSetsAlreadyApplied);
  MGIF_COUNTER_DELTA(sceneChangeSetsAcceptedIncremental);
  MGIF_COUNTER_DELTA(sceneChangeSetsAcceptedFullSnapshot);
  MGIF_COUNTER_DELTA(sceneChangeSetsRejected);
  MGIF_COUNTER_DELTA(sceneChangeSetsFailedBeforeSubmit);
  MGIF_COUNTER_DELTA(sceneChangeSetsFailedAfterSubmit);
  subtractArray(before.sceneChangeSetsByError, after.sceneChangeSetsByError,
                result.sceneChangeSetsByError);
  MGIF_COUNTER_DELTA(sceneOperationsReceived);
  MGIF_COUNTER_DELTA(sceneOperationsCoalesced);
  MGIF_COUNTER_DELTA(sceneOperationsApplied);
  subtractArray(before.sceneOperationsReceivedByClass,
                after.sceneOperationsReceivedByClass,
                result.sceneOperationsReceivedByClass);
  subtractArray(before.sceneOperationsCoalescedByClass,
                after.sceneOperationsCoalescedByClass,
                result.sceneOperationsCoalescedByClass);
  subtractArray(before.sceneOperationsAppliedByClass,
                after.sceneOperationsAppliedByClass,
                result.sceneOperationsAppliedByClass);
  MGIF_COUNTER_DELTA(nodeTransformOpsApplied);
  MGIF_COUNTER_DELTA(visibilityOpsApplied);
  MGIF_COUNTER_DELTA(materialOpsApplied);
  MGIF_COUNTER_DELTA(primitiveBindingOpsApplied);
  MGIF_COUNTER_DELTA(cameraOpsApplied);
  MGIF_COUNTER_DELTA(lightOpsApplied);
  MGIF_COUNTER_DELTA(hierarchyOpsApplied);
  MGIF_COUNTER_DELTA(topologyOpsApplied);
  MGIF_COUNTER_DELTA(structuralOpsApplied);
  MGIF_COUNTER_DELTA(instancesAdded);
  MGIF_COUNTER_DELTA(instancesRemoved);
  MGIF_COUNTER_DELTA(geometryUploadCalls);
  MGIF_COUNTER_DELTA(geometryBytesUploaded);
  MGIF_COUNTER_DELTA(geometryRangesUploaded);
  MGIF_COUNTER_DELTA(textureUploadCalls);
  MGIF_COUNTER_DELTA(textureBytesUploaded);
  MGIF_COUNTER_DELTA(textureSubresourcesUploaded);
  MGIF_COUNTER_DELTA(otherUploadCalls);
  MGIF_COUNTER_DELTA(otherBytesUploaded);
  MGIF_COUNTER_DELTA(otherRangesUploaded);
  MGIF_COUNTER_DELTA(bufferRangesUploaded);
  MGIF_COUNTER_DELTA(resourcesCreated);
  MGIF_COUNTER_DELTA(resourcesLogicallyRetired);
  MGIF_COUNTER_DELTA(resourcesPhysicallyDestroyed);
  MGIF_COUNTER_DELTA(resourcesOrphaned);
  MGIF_COUNTER_DELTA(retirementBacklog);
  MGIF_COUNTER_DELTA(submissions);
  MGIF_COUNTER_DELTA(incrementalSubmissions);
  MGIF_COUNTER_DELTA(fullSnapshotSubmissions);
  MGIF_COUNTER_DELTA(fullSnapshotCount);
  subtractArray(before.fullSnapshotCountByReason,
                after.fullSnapshotCountByReason,
                result.fullSnapshotCountByReason);
  MGIF_COUNTER_DELTA(rollbackCount);
  MGIF_COUNTER_DELTA(allocationFailureCount);
  MGIF_COUNTER_DELTA(descriptorStagingFailureCount);
  MGIF_COUNTER_DELTA(recordingFailureCount);
  MGIF_COUNTER_DELTA(submissionFailureCount);
  MGIF_COUNTER_DELTA(publicationFailureCount);
  MGIF_COUNTER_DELTA(retirementEnqueueFailureCount);
  MGIF_COUNTER_DELTA(partialPublicationCount);
  MGIF_COUNTER_DELTA(placeholderResourceCount);
  MGIF_COUNTER_DELTA(deviceIdleCalls);
  subtractArray(before.deviceIdleCallsByReason, after.deviceIdleCallsByReason,
                result.deviceIdleCallsByReason);
#undef MGIF_COUNTER_DELTA
  if (before.current != after.current) {
    result.current = after.current;
  }
  return result;
}

bool sceneDeltaCounterSnapshotIsZero(
    const SceneDeltaCounterSnapshot &snapshot) noexcept {
  const SceneDeltaCounterSnapshot zero{};
  return snapshot == zero;
}

SceneDeltaCounterSnapshot SceneDeltaCounters::snapshot() const noexcept {
  const std::lock_guard lock{m_mutex};
  return m_snapshot;
}

void SceneDeltaCounters::commit(
    const SceneDeltaCounterSnapshot &snapshot) noexcept {
  const std::lock_guard lock{m_mutex};
  m_snapshot = snapshot;
}

} // namespace demo::editor
