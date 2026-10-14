#include <mgif/scene_editor/SceneChangeCapture.h>

#include <algorithm>
#include <map>
#include <set>
#include <utility>

namespace demo::editor {
namespace {

[[nodiscard]] SceneChangeValidation failure(SceneChangeValidationError error,
                                             const char *message) {
  SceneChangeValidation value;
  value.error = error;
  value.message = message;
  return value;
}

[[nodiscard]] SceneCanonicalizationResult failed(
    SceneChangeValidation validation) {
  SceneCanonicalizationResult result;
  result.validation = std::move(validation);
  return result;
}

[[nodiscard]] bool snapshotIdentityValid(const SceneCaptureSnapshot &value) {
  return value.authority.valid() && sceneDigestPresent(value.documentDigest) &&
         sceneDigestPresent(value.projectionDigest) &&
         (!value.activeScene ||
          (value.activeScene->valid() &&
           value.activeScene->authority == value.authority &&
           value.activeScene->kind == EditorObjectKind::scene));
}

[[nodiscard]] SceneChangeHeader baseHeader(
    SceneChangeDomain domain, const SceneCaptureSnapshot &before,
    const SceneCaptureSnapshot &after, std::uint64_t firstSerial,
    std::uint64_t lastSerial) {
  SceneChangeHeader header;
  header.domain = domain;
  header.authority = before.authority;
  header.baseDocumentRevision = before.documentRevision;
  header.targetDocumentRevision = after.documentRevision;
  header.baseSceneActivationEpoch = before.sceneActivationEpoch;
  header.targetSceneActivationEpoch = after.sceneActivationEpoch;
  header.baseRootExternalStateSerial = before.rootExternalStateSerial;
  header.targetRootExternalStateSerial = after.rootExternalStateSerial;
  header.baseActiveScene = before.activeScene;
  header.targetActiveScene = after.activeScene;
  header.firstMutationSerial = firstSerial;
  header.lastMutationSerial = lastSerial;
  header.baseDocumentDigest = before.documentDigest;
  header.targetDocumentDigest = after.documentDigest;
  header.baseProjectionDigest = before.projectionDigest;
  header.targetProjectionDigest = after.projectionDigest;
  return header;
}

[[nodiscard]] bool sameTargetEnvelope(const ImmutableSceneSnapshotRef &target,
                                      const SceneCaptureSnapshot &after) {
  return target.authority == after.authority &&
         target.documentRevision == after.documentRevision &&
         target.sceneActivationEpoch == after.sceneActivationEpoch &&
         target.rootExternalStateSerial == after.rootExternalStateSerial &&
         target.activeScene == after.activeScene &&
         target.documentDigest == after.documentDigest &&
         target.projectionDigest == after.projectionDigest;
}

} // namespace

SceneCanonicalizationResult SceneChangeCapture::finalize(
    const SceneCaptureSnapshot &before, const SceneCaptureSnapshot &after,
    std::vector<SceneMutationEvidence> evidence,
    std::optional<FullRebuildReason> incompleteReason) {
  if (!snapshotIdentityValid(before) || !snapshotIdentityValid(after) ||
      before.authority != after.authority) {
    return failed(failure(SceneChangeValidationError::InvalidEnvelope,
                          "capture snapshots have incomplete or conflicting identity"));
  }
  if (evidence.empty()) {
    return failed(failure(SceneChangeValidationError::InvalidSourceMutationChain,
                          "document capture requires at least one source mutation"));
  }

  const SourceMutationFence &first = evidence.front().sourceMutation;
  const SourceMutationFence &last = evidence.back().sourceMutation;
  SceneChangeHeader header =
      baseHeader(SceneChangeDomain::DocumentMutation, before, after,
                 first.mutationToken.serial, last.mutationToken.serial);
  if (first.baseDocumentRevision != before.documentRevision ||
      first.baseDocumentDigest != before.documentDigest ||
      last.targetDocumentRevision != after.documentRevision ||
      last.targetDocumentDigest != after.documentDigest) {
    return failed(failure(SceneChangeValidationError::InvalidSourceMutationChain,
                          "source chain does not bind both capture snapshots"));
  }
  header.mutationToken = last.mutationToken;

  const bool incomplete = std::any_of(
      evidence.begin(), evidence.end(), [](const SceneMutationEvidence &item) {
        return !item.structuralEvidenceComplete;
      });
  if (!incomplete) {
    if (incompleteReason) {
      return failed(failure(SceneChangeValidationError::InvalidFullSnapshot,
                            "complete evidence cannot request a conservative snapshot"));
    }
    return SceneChangeCanonicalizer::canonicalizeEvidence(
        std::move(header), std::move(evidence));
  }
  if (!incompleteReason || !after.immutableTarget ||
      !sameTargetEnvelope(*after.immutableTarget, after)) {
    return failed(failure(SceneChangeValidationError::InvalidFullSnapshot,
                          "incomplete evidence requires a complete immutable target"));
  }

  FullSnapshotReference full;
  full.reason = *incompleteReason;
  full.targetSnapshotDigest = after.immutableTarget->contentDigest;
  full.immutableTarget = *after.immutableTarget;
  full.stableIdRemapComplete = true;
  for (const SceneMutationEvidence &item : evidence) {
    full.stableIdRemap.insert(full.stableIdRemap.end(),
                              item.stableIdRemap.begin(),
                              item.stableIdRemap.end());
  }
  header.domain = SceneChangeDomain::FullSnapshot;
  return SceneChangeCanonicalizer::canonicalizeEvidence(
      std::move(header), std::move(evidence), std::move(full));
}

SceneCanonicalizationResult SceneChangeCapture::finalizeActivation(
    const SceneCaptureSnapshot &before, const SceneCaptureSnapshot &after,
    std::uint64_t transitionSerial) {
  if (!snapshotIdentityValid(before) || !snapshotIdentityValid(after) ||
      before.authority != after.authority || transitionSerial == 0u) {
    return failed(failure(SceneChangeValidationError::InvalidEnvelope,
                          "activation capture identity or serial is invalid"));
  }
  SceneChangeSet input;
  input.header = baseHeader(SceneChangeDomain::SceneActivation, before, after,
                            transitionSerial, transitionSerial);
  return SceneChangeCanonicalizer::canonicalize(std::move(input));
}

SceneCanonicalizationResult SceneChangeCapture::finalizeRootState(
    const SceneCaptureSnapshot &before, const SceneCaptureSnapshot &after,
    const SceneNodeTransformValue &beforeRoot,
    const SceneNodeTransformValue &afterRoot,
    std::uint64_t transitionSerial) {
  if (!snapshotIdentityValid(before) || !snapshotIdentityValid(after) ||
      before.authority != after.authority || transitionSerial == 0u ||
      !after.activeScene) {
    return failed(failure(SceneChangeValidationError::InvalidEnvelope,
                          "root-state capture identity or serial is invalid"));
  }
  SceneChangeSet input;
  input.header = baseHeader(SceneChangeDomain::SceneRootState, before, after,
                            transitionSerial, transitionSerial);
  SceneFieldChange change;
  change.target = *after.activeScene;
  change.changeClass = SceneChangeClass::Transform;
  change.action = SceneChangeAction::Update;
  change.field = SceneField::SceneRootTransform;
  change.mutationSerial = transitionSerial;
  change.beforeValue = beforeRoot;
  change.afterValue = afterRoot;
  input.canonicalChanges.push_back(std::move(change));
  return SceneChangeCanonicalizer::canonicalize(std::move(input));
}

SceneDerivedEffects SceneChangeCapture::deriveHierarchyEffects(
    std::span<const SceneFieldChange> exactChanges,
    std::span<const SceneHierarchyNodeEvidence> hierarchy) {
  std::vector<SceneHierarchyNodeEvidence> normalizedHierarchy(
      hierarchy.begin(), hierarchy.end());
  std::sort(normalizedHierarchy.begin(), normalizedHierarchy.end(),
            [](const SceneHierarchyNodeEvidence &left,
               const SceneHierarchyNodeEvidence &right) {
              return std::tie(left.node, left.parent, left.localVisible) <
                     std::tie(right.node, right.parent, right.localVisible);
            });

  std::map<StableSceneId, std::vector<StableSceneId>> children;
  std::map<StableSceneId, std::optional<StableSceneId>> parents;
  std::set<StableSceneId> known;
  for (const SceneHierarchyNodeEvidence &node : normalizedHierarchy) {
    if (!node.node.valid()) {
      continue;
    }
    known.insert(node.node);
    parents.try_emplace(node.node, node.parent);
  }
  for (const auto &[node, parent] : parents) {
    if (parent && known.contains(*parent)) {
      children[*parent].push_back(node);
    }
  }
  for (auto &[parent, values] : children) {
    (void)parent;
    std::sort(values.begin(), values.end());
    values.erase(std::unique(values.begin(), values.end()), values.end());
  }

  SceneDerivedEffects result;
  std::set<StableSceneId> transformTargets;
  std::set<StableSceneId> visibilityTargets;
  for (const SceneFieldChange &change : exactChanges) {
    if (change.action == SceneChangeAction::Delete ||
        (change.field != SceneField::NodeLocalTransform &&
         change.field != SceneField::NodeLocalVisibility) ||
        !known.contains(change.target)) {
      continue;
    }
    std::vector<StableSceneId> queue = children[change.target];
    std::set<StableSceneId> visited;
    for (std::size_t cursor = 0; cursor < queue.size(); ++cursor) {
      const StableSceneId descendant = queue[cursor];
      if (!visited.insert(descendant).second) {
        continue;
      }
      (change.field == SceneField::NodeLocalTransform ? transformTargets
                                                      : visibilityTargets)
          .insert(descendant);
      const auto found = children.find(descendant);
      if (found != children.end()) {
        queue.insert(queue.end(), found->second.begin(), found->second.end());
      }
    }
  }

  std::map<StableSceneId, std::size_t> incoming;
  for (const StableSceneId node : known) {
    incoming.emplace(node, 0u);
  }
  for (const auto &[node, parent] : parents) {
    if (parent && *parent != node && known.contains(*parent)) {
      ++incoming[node];
    }
  }
  std::set<StableSceneId> ready;
  for (const auto &[node, count] : incoming) {
    if (count == 0u) {
      ready.insert(node);
    }
  }
  std::vector<StableSceneId> hierarchyOrder;
  hierarchyOrder.reserve(known.size());
  std::set<StableSceneId> ordered;
  while (!ready.empty()) {
    const StableSceneId node = *ready.begin();
    ready.erase(ready.begin());
    if (!ordered.insert(node).second) {
      continue;
    }
    hierarchyOrder.push_back(node);
    const auto found = children.find(node);
    if (found == children.end()) {
      continue;
    }
    for (const StableSceneId child : found->second) {
      auto count = incoming.find(child);
      if (count != incoming.end() && count->second != 0u &&
          --count->second == 0u) {
        ready.insert(child);
      }
    }
  }
  // Invalid cyclic evidence has no parent-before-child order. Keep this helper
  // total and deterministic; the canonicalizer remains the rejecting layer.
  for (const StableSceneId node : known) {
    if (!ordered.contains(node)) {
      hierarchyOrder.push_back(node);
    }
  }
  for (const StableSceneId node : hierarchyOrder) {
    if (transformTargets.contains(node)) {
      result.transformTargets.push_back(node);
    }
    if (visibilityTargets.contains(node)) {
      result.visibilityTargets.push_back(node);
    }
  }
  return result;
}

} // namespace demo::editor
