#pragma once

#include <mgif/scene_editor/SceneChangeSet.h>

#include <array>
#include <cstddef>
#include <cstdint>
#include <mutex>
#include <optional>

namespace demo::editor {

enum class SceneApplyDisposition : std::uint8_t {
  AcceptedIncremental = 0,
  AcceptedFullSnapshot = 1,
  AlreadyApplied = 2,
  Rejected = 3,
  FailedBeforeSubmit = 4,
  FailedAfterSubmit = 5,
};

enum class SceneApplyError : std::uint8_t {
  None = 0,
  InvalidSchema = 1,
  InvalidDigest = 2,
  InvalidPayload = 3,
  InvalidState = 4,
  AuthorityMismatch = 5,
  StaleRevision = 6,
  RevisionGap = 7,
  SceneActivationMismatch = 8,
  RootSerialMismatch = 9,
  ProjectionMismatch = 10,
  MappingMismatch = 11,
  RendererMismatch = 12,
  DeviceMismatch = 13,
  DeviceLost = 14,
  StaleObjectGeneration = 15,
  UnsupportedCapability = 16,
  AllocationFailure = 17,
  DescriptorStagingFailure = 18,
  RecordingFailure = 19,
  SubmissionFailure = 20,
  PublicationInvariant = 21,
  RetirementEnqueueFailure = 22,
};

inline constexpr std::size_t kSceneApplyErrorCount = 23u;
inline constexpr std::size_t kSceneChangeClassCount = 9u;
inline constexpr std::size_t kFullRebuildReasonCount = 12u;
static_assert(
    static_cast<std::uint8_t>(SceneApplyDisposition::FailedAfterSubmit) == 5u);
static_assert(static_cast<std::uint8_t>(
                  SceneApplyError::RetirementEnqueueFailure) == 22u);

enum class SceneDeviceIdleReason : std::uint8_t {
  Shutdown = 0,
  DeviceLoss = 1,
  SurfaceTeardown = 2,
  ForbiddenOrOther = 3,
};

inline constexpr std::size_t kSceneDeviceIdleReasonCount = 4u;

struct SceneDeltaCurrentFenceSnapshot final {
  EditorDocumentAuthority authority{};
  std::uint64_t appliedDocumentRevision{0};
  std::uint64_t sceneActivationEpoch{0};
  std::uint64_t rootExternalStateSerial{0};
  std::uint64_t mappingEpoch{0};
  std::optional<StableSceneId> activeScene;
  std::uint64_t rendererInstance{0};
  std::uint32_t rendererGeneration{0};
  std::uint64_t deviceInstance{0};
  std::uint32_t deviceGeneration{0};
  SceneDigest activeProjectionDigest{};
  SceneDigest activeGenerationDigest{};

  bool operator==(const SceneDeltaCurrentFenceSnapshot &) const = default;
};

struct SceneDeltaCounterSnapshot final {
  std::uint64_t sceneChangeSetsReceived{0};
  std::uint64_t sceneChangeSetsAlreadyApplied{0};
  std::uint64_t sceneChangeSetsAcceptedIncremental{0};
  std::uint64_t sceneChangeSetsAcceptedFullSnapshot{0};
  std::uint64_t sceneChangeSetsRejected{0};
  std::uint64_t sceneChangeSetsFailedBeforeSubmit{0};
  std::uint64_t sceneChangeSetsFailedAfterSubmit{0};
  std::array<std::uint64_t, kSceneApplyErrorCount> sceneChangeSetsByError{};

  std::uint64_t sceneOperationsReceived{0};
  std::uint64_t sceneOperationsCoalesced{0};
  std::uint64_t sceneOperationsApplied{0};
  std::array<std::uint64_t, kSceneChangeClassCount>
      sceneOperationsReceivedByClass{};
  std::array<std::uint64_t, kSceneChangeClassCount>
      sceneOperationsCoalescedByClass{};
  std::array<std::uint64_t, kSceneChangeClassCount>
      sceneOperationsAppliedByClass{};

  std::uint64_t nodeTransformOpsApplied{0};
  std::uint64_t visibilityOpsApplied{0};
  std::uint64_t materialOpsApplied{0};
  std::uint64_t primitiveBindingOpsApplied{0};
  std::uint64_t cameraOpsApplied{0};
  std::uint64_t lightOpsApplied{0};
  std::uint64_t hierarchyOpsApplied{0};
  std::uint64_t topologyOpsApplied{0};
  std::uint64_t structuralOpsApplied{0};
  std::uint64_t instancesAdded{0};
  std::uint64_t instancesRemoved{0};

  std::uint64_t geometryUploadCalls{0};
  std::uint64_t geometryBytesUploaded{0};
  std::uint64_t geometryRangesUploaded{0};
  std::uint64_t textureUploadCalls{0};
  std::uint64_t textureBytesUploaded{0};
  std::uint64_t textureSubresourcesUploaded{0};
  std::uint64_t otherUploadCalls{0};
  std::uint64_t otherBytesUploaded{0};
  std::uint64_t otherRangesUploaded{0};
  std::uint64_t bufferRangesUploaded{0};
  std::uint64_t resourcesCreated{0};
  std::uint64_t resourcesLogicallyRetired{0};
  std::uint64_t resourcesPhysicallyDestroyed{0};
  std::uint64_t resourcesOrphaned{0};
  std::uint64_t retirementBacklog{0};
  std::uint64_t submissions{0};
  std::uint64_t incrementalSubmissions{0};
  std::uint64_t fullSnapshotSubmissions{0};

  std::uint64_t fullSnapshotCount{0};
  std::array<std::uint64_t, kFullRebuildReasonCount>
      fullSnapshotCountByReason{};
  std::uint64_t rollbackCount{0};
  std::uint64_t allocationFailureCount{0};
  std::uint64_t descriptorStagingFailureCount{0};
  std::uint64_t recordingFailureCount{0};
  std::uint64_t submissionFailureCount{0};
  std::uint64_t publicationFailureCount{0};
  std::uint64_t retirementEnqueueFailureCount{0};
  std::uint64_t partialPublicationCount{0};
  std::uint64_t placeholderResourceCount{0};
  std::uint64_t deviceIdleCalls{0};
  std::array<std::uint64_t, kSceneDeviceIdleReasonCount>
      deviceIdleCallsByReason{};

  SceneDeltaCurrentFenceSnapshot current{};

  bool operator==(const SceneDeltaCounterSnapshot &) const = default;
};

[[nodiscard]] constexpr std::size_t
sceneApplyErrorIndex(SceneApplyError error) noexcept {
  return static_cast<std::size_t>(error);
}

[[nodiscard]] constexpr std::size_t
sceneChangeClassIndex(SceneChangeClass changeClass) noexcept {
  return static_cast<std::size_t>(changeClass);
}

[[nodiscard]] constexpr std::size_t
fullRebuildReasonIndex(FullRebuildReason reason) noexcept {
  return static_cast<std::size_t>(reason);
}

[[nodiscard]] constexpr std::size_t
sceneDeviceIdleReasonIndex(SceneDeviceIdleReason reason) noexcept {
  return static_cast<std::size_t>(reason);
}

[[nodiscard]] const char *
sceneApplyDispositionName(SceneApplyDisposition disposition) noexcept;
[[nodiscard]] const char *sceneApplyErrorName(SceneApplyError error) noexcept;

[[nodiscard]] SceneDeltaCounterSnapshot
sceneDeltaCounterDelta(const SceneDeltaCounterSnapshot &before,
                       const SceneDeltaCounterSnapshot &after) noexcept;
[[nodiscard]] bool sceneDeltaCounterSnapshotIsZero(
    const SceneDeltaCounterSnapshot &snapshot) noexcept;

class SceneDeltaCounters final {
public:
  [[nodiscard]] SceneDeltaCounterSnapshot snapshot() const noexcept;
  void commit(const SceneDeltaCounterSnapshot &snapshot) noexcept;

private:
  mutable std::mutex m_mutex;
  SceneDeltaCounterSnapshot m_snapshot{};
};

} // namespace demo::editor
