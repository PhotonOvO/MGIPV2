#pragma once

#include <mgif/scene_editor/SceneChangeCanonicalizer.h>

#include <cstdint>
#include <optional>
#include <span>
#include <vector>

namespace demo::editor {

// Immutable identity of one document-to-SceneAsset projection. The value owns
// its optional full-snapshot target and contains only document projection data.
struct SceneCaptureSnapshot final {
  EditorDocumentAuthority authority{};
  std::uint64_t documentRevision{0};
  std::uint64_t sceneActivationEpoch{0};
  std::uint64_t rootExternalStateSerial{0};
  std::optional<StableSceneId> activeScene;
  SceneDigest documentDigest{};
  SceneDigest projectionDigest{};
  std::optional<ImmutableSceneSnapshotRef> immutableTarget;

  bool operator==(const SceneCaptureSnapshot &) const = default;
};

struct SceneHierarchyNodeEvidence final {
  StableSceneId node{};
  std::optional<StableSceneId> parent;
  bool localVisible{true};

  bool operator==(const SceneHierarchyNodeEvidence &) const = default;
};

struct SceneDerivedEffects final {
  std::vector<StableSceneId> transformTargets;
  std::vector<StableSceneId> visibilityTargets;

  bool operator==(const SceneDerivedEffects &) const = default;
};

class SceneChangeCapture final {
public:
  // Finalizes one or more adjacent document mutations. Incomplete structural
  // evidence is accepted only with a reason and a complete immutable target.
  [[nodiscard]] static SceneCanonicalizationResult finalize(
      const SceneCaptureSnapshot &before, const SceneCaptureSnapshot &after,
      std::vector<SceneMutationEvidence> evidence,
      std::optional<FullRebuildReason> incompleteReason = std::nullopt);

  // Scene activation and active-root changes are isolated logical domains;
  // neither fabricates a document mutation token or source fence.
  [[nodiscard]] static SceneCanonicalizationResult finalizeActivation(
      const SceneCaptureSnapshot &before, const SceneCaptureSnapshot &after,
      std::uint64_t transitionSerial);
  [[nodiscard]] static SceneCanonicalizationResult finalizeRootState(
      const SceneCaptureSnapshot &before, const SceneCaptureSnapshot &after,
      const SceneNodeTransformValue &beforeRoot,
      const SceneNodeTransformValue &afterRoot,
      std::uint64_t transitionSerial);

  // Adds deterministic parent-before-child transform/visibility declarations
  // for descendants of explicitly changed nodes. Input values remain owned.
  [[nodiscard]] static SceneDerivedEffects deriveHierarchyEffects(
      std::span<const SceneFieldChange> exactChanges,
      std::span<const SceneHierarchyNodeEvidence> hierarchy);
};

} // namespace demo::editor
