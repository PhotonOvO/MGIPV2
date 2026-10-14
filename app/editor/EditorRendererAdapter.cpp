#include "EditorRendererAdapter.h"

#include <mgif/scene_editor/SceneChangeCanonicalizer.h>

#include "../../scene/SceneAssetView.h"
#include "../../scene/SceneUploadPlanner.h"

#include <algorithm>
#include <atomic>
#include <limits>
#include <span>
#include <utility>
#include <vector>

namespace demo::editor::ui {
namespace {

[[nodiscard]] RendererObjectKind
rendererKind(EditorObjectKind kind) noexcept {
  switch (kind) {
  case EditorObjectKind::scene:
  case EditorObjectKind::node:
  case EditorObjectKind::primitive:
    return RendererObjectKind::SceneObject;
  case EditorObjectKind::mesh:
    return RendererObjectKind::Geometry;
  case EditorObjectKind::material:
    return RendererObjectKind::Material;
  case EditorObjectKind::image:
  case EditorObjectKind::texture:
    return RendererObjectKind::Texture;
  case EditorObjectKind::sampler:
    return RendererObjectKind::Sampler;
  case EditorObjectKind::camera:
    return RendererObjectKind::Camera;
  case EditorObjectKind::light:
    return RendererObjectKind::Light;
  case EditorObjectKind::animation:
  case EditorObjectKind::accessor:
  case EditorObjectKind::bufferView:
  case EditorObjectKind::buffer:
  case EditorObjectKind::skin:
    return RendererObjectKind::Descriptor;
  case EditorObjectKind::invalid:
  case EditorObjectKind::count:
    return RendererObjectKind::Descriptor;
  }
  return RendererObjectKind::Descriptor;
}

[[nodiscard]] SceneDigest projectionIdentityDigest(StableSceneId id,
                                                   std::uint64_t runtimeIndex) {
  std::vector<std::uint8_t> bytes;
  bytes.reserve(32u);
  const auto append = [&bytes](std::uint64_t value, unsigned count) {
    for (unsigned index = 0u; index < count; ++index) {
      bytes.push_back(static_cast<std::uint8_t>(value & 0xffu));
      value >>= 8u;
    }
  };
  append(static_cast<std::uint8_t>(id.kind), 1u);
  append(id.authority.value, 8u);
  append(id.slot, 4u);
  append(id.generation, 4u);
  append(runtimeIndex, 8u);
  return SceneChangeCanonicalizer::computeSha256(bytes);
}

[[nodiscard]] SceneCanonicalIdentity
canonicalIdentity(const SceneChangeHeader &header) noexcept {
  return SceneCanonicalIdentity{
      header.authority,
      header.domain,
      header.firstMutationSerial,
      header.lastMutationSerial,
      header.targetDocumentRevision,
      header.targetSceneActivationEpoch,
      header.targetRootExternalStateSerial,
  };
}

[[nodiscard]] std::optional<SceneRuntimeMapping> authorityForFence(
    const SceneRuntimeMapping &source, const RendererTargetFence &fence,
    std::string *error = nullptr) {
  return SceneRuntimeMapping::create(
      SceneRuntimeMappingFence{
          .authority = fence.authority,
          .appliedDocumentRevision = fence.appliedDocumentRevision,
          .activeScene = fence.activeScene,
          .sceneActivationEpoch = fence.sceneActivationEpoch,
          .rootExternalStateSerial = fence.rootExternalStateSerial,
          .projectionDigest = fence.projectionDigest,
          .mappingEpoch = fence.mappingEpoch,
      },
      std::vector<SceneRuntimeMappingEntry>(source.entries().begin(),
                                            source.entries().end()),
      std::vector<SceneRuntimeMappingAlias>(source.aliases().begin(),
                                            source.aliases().end()),
      error);
}

[[nodiscard]] bool validateCameraLightSourceIdentities(
    const SceneProjectionResult &projection, std::string &error) {
  if (!projection.ownerCapability.validate(projection.asset,
                                            projection.mapping, &error)) {
    if (error.empty()) {
      error = "projection owner capability rejected camera/light identity";
    }
    return false;
  }
  error.clear();
  return true;
}

[[nodiscard]] std::string applyFailureMessage(const SceneApplyResult &result) {
  const bool hasRhiMessage =
      !result.rhiResult.succeeded() &&
      result.rhiResult.error.message != nullptr &&
      result.rhiResult.error.message[0] != '\0';
  if (!result.message.empty()) {
    if (!hasRhiMessage ||
        result.message.find(result.rhiResult.error.message) !=
            std::string::npos) {
      return result.message;
    }
    return result.message + ": " + result.rhiResult.error.message;
  }
  return hasRhiMessage ? std::string(result.rhiResult.error.message)
                       : "renderer rejected the editor scene publication";
}

} // namespace

EditorRendererAdapter::EditorRendererAdapter(rhi::BackendType backend) noexcept
    : m_backend(backend) {
  m_mappingState.nextHandleIndices.fill(1u);
}

void EditorRendererAdapter::bindRendererIdentity(
    RendererInstanceId renderer, RendererDeviceId device) noexcept {
  if (m_rendererIdentity == renderer && m_deviceIdentity == device) return;
  m_rendererIdentity = renderer;
  m_deviceIdentity = device;
  m_initialized = false;
  m_requiresAuthoritativeRebuild = false;
  m_activeFence.reset();
  m_activeDocumentDigest = {};
  m_mappingState = {};
  m_mappingState.nextHandleIndices.fill(1u);
  clearPending();
}

bool EditorRendererAdapter::enqueue(
    const EditorController::ProjectionPublication &publication,
    std::string *error, SceneApplyError *typedError) noexcept {
  if (typedError != nullptr) {
    *typedError = SceneApplyError::InvalidState;
  }
  if (!m_rendererIdentity.valid() || !m_deviceIdentity.valid()) {
    if (error != nullptr) *error = "renderer identity is not bound";
    return false;
  }
  if (publication.projection == nullptr) {
    if (error != nullptr) {
      *error = "editor publication has no projection";
    }
    return false;
  }
  if (!publication.projection->error.empty()) {
    if (typedError != nullptr) {
      *typedError = SceneApplyError::InvalidPayload;
    }
    if (error != nullptr) {
      *error = publication.projection->error;
    }
    return false;
  }
  if (!publication.projection->ownerCapability.valid()) {
    if (typedError != nullptr) {
      *typedError = SceneApplyError::StaleObjectGeneration;
    }
    if (error != nullptr) {
      *error = "projection owner capability is absent";
    }
    return false;
  }
  // Validate the immutable owner seal before any refresh classification. Even
  // a UI-only publication must not be allowed to carry a forged projection
  // into a later queued transaction through a reused publication object.
  try {
    std::string identityError;
    if (!validateCameraLightSourceIdentities(*publication.projection,
                                             identityError)) {
      if (typedError != nullptr) {
        *typedError = SceneApplyError::StaleObjectGeneration;
      }
      if (error != nullptr) {
        *error = identityError.empty()
                     ? "projection owner capability rejected publication"
                     : std::move(identityError);
      }
      return false;
    }
  } catch (...) {
    if (typedError != nullptr) {
      *typedError = SceneApplyError::InvalidState;
    }
    if (error != nullptr) {
      *error = "projection owner capability validation failed";
    }
    return false;
  }
  if (publication.refresh == ProjectionRefresh::none ||
      publication.refresh == ProjectionRefresh::selectionOnly) {
    if (typedError != nullptr) {
      *typedError = SceneApplyError::None;
    }
    if (error != nullptr) error->clear();
    return true;
  }
  if (publication.canonical != nullptr &&
      !publication.canonical->succeeded()) {
    if (typedError != nullptr) {
      *typedError = SceneApplyError::InvalidPayload;
    }
    if (error != nullptr) {
      *error = publication.canonical->validation.message.empty()
                   ? "editor publication has invalid canonical evidence"
                   : publication.canonical->validation.message;
    }
    return false;
  }

  const bool authoritativeRebuild =
      publication.refresh == ProjectionRefresh::fullScene &&
      (publication.canonical == nullptr ||
       (publication.canonical->changeSet &&
        (publication.canonical->changeSet->header.domain ==
             SceneChangeDomain::FullSnapshot ||
         publication.canonical->changeSet->header.domain ==
             SceneChangeDomain::SceneActivation)));
  if (m_requiresAuthoritativeRebuild && !authoritativeRebuild) {
    if (error != nullptr) {
      *error =
          "renderer publication is locked until an authoritative rebuild";
    }
    return false;
  }

  try {
    PendingPublication pending;
    pending.projection = *publication.projection;
    pending.refresh = publication.refresh;
    if (publication.canonical != nullptr) {
      pending.canonical = *publication.canonical;
    }
    // A complete unbound projection is authoritative for a new document or an
    // explicit rebuild and safely supersedes older, not-yet-published deltas.
    if (authoritativeRebuild) {
      m_pending.clear();
    }
    m_pending.push_back(std::move(pending));
    if (typedError != nullptr) {
      *typedError = SceneApplyError::None;
    }
    if (error != nullptr) error->clear();
    return true;
  } catch (const std::bad_alloc &) {
    if (typedError != nullptr) {
      *typedError = SceneApplyError::AllocationFailure;
    }
    if (error != nullptr) {
      *error = "out of memory while queuing the editor publication";
    }
  } catch (const std::exception &exception) {
    if (typedError != nullptr) {
      *typedError = SceneApplyError::InvalidState;
    }
    if (error != nullptr) {
      *error = std::string("failed to queue the editor publication: ") +
               exception.what();
    }
  } catch (...) {
    if (typedError != nullptr) {
      *typedError = SceneApplyError::InvalidState;
    }
    if (error != nullptr) {
      *error = "failed to queue the editor publication";
    }
  }
  return false;
}

RendererTargetFence EditorRendererAdapter::targetFence(
    const SceneProjectionResult &projection,
    std::uint64_t mappingEpoch) const noexcept {
  RendererTargetFence fence;
  fence.backend = m_backend;
  fence.capability =
      m_backend == rhi::BackendType::metal
          ? RendererSceneUpdateCapability::Unsupported
          : RendererSceneUpdateCapability::IncrementalAndSnapshot;
  fence.renderer = m_rendererIdentity;
  fence.device = m_deviceIdentity;
  fence.mappingEpoch = mappingEpoch;
  fence.authority = projection.mapping.authority();
  fence.appliedDocumentRevision = projection.documentRevision;
  fence.sceneActivationEpoch = projection.sceneActivationEpoch;
  fence.rootExternalStateSerial =
      projection.mapping.rootExternalStateSerial();
  fence.activeScene = projection.activeScene;
  fence.projectionDigest = projection.projectionDigest;
  return fence;
}

EditorRendererAdapter::MappingBuild EditorRendererAdapter::buildMapping(
    const SceneProjectionResult &projection, const RendererTargetFence &fence,
    const MappingState &baseState, std::uint32_t objectGeneration) const {
  MappingBuild result;
  if (!validateCameraLightSourceIdentities(projection, result.error)) {
    return result;
  }
  result.state = baseState;
  try {
    std::vector<RendererSceneMappingEntry> entries;
    entries.reserve(projection.mapping.entries().size());
    std::vector<StableSceneId> primaryStableIds;
    primaryStableIds.reserve(projection.mapping.entries().size());
    const auto textureProjectionBinding =
        [&](StableSceneId textureId) -> const SceneTextureProjectionBinding * {
          const auto found = std::find_if(
              projection.textureBindings.begin(),
              projection.textureBindings.end(),
              [&](const SceneTextureProjectionBinding &binding) {
                return binding.textureId == textureId;
              });
          return found == projection.textureBindings.end() ? nullptr : &*found;
        };
    const auto samplerProjectionIndex =
        [&](StableSceneId samplerId) -> std::optional<std::uint32_t> {
          const auto found = std::find_if(
              projection.samplerBindings.begin(),
              projection.samplerBindings.end(),
              [&](const SceneSamplerProjectionBinding &binding) {
                return binding.samplerId == samplerId;
              });
          return found == projection.samplerBindings.end()
                     ? std::nullopt
                     : std::optional<std::uint32_t>{static_cast<std::uint32_t>(
                           std::distance(projection.samplerBindings.begin(), found))};
        };
    for (const SceneRuntimeMappingEntry &source :
         projection.mapping.entries()) {
      const StableSceneId stable = stableSceneId(source.documentId);
      if (!stable.valid()) {
        result.error = "projection mapping contains an invalid stable identity";
        return result;
      }
      primaryStableIds.push_back(stable);
      const RendererObjectKind kind = rendererKind(stable.kind);
      RendererObjectHandle handle{};
      const auto existing = result.state.handles.find(stable);
      if (existing != result.state.handles.end()) {
        handle = existing->second;
      } else {
        const std::size_t kindIndex = static_cast<std::size_t>(kind);
        if (kindIndex >= result.state.nextHandleIndices.size() ||
            result.state.nextHandleIndices[kindIndex] ==
                std::numeric_limits<std::uint32_t>::max()) {
          result.error = "renderer object handle space is exhausted";
          return result;
        }
        handle = RendererObjectHandle{
            kind,
            result.state.nextHandleIndices[kindIndex]++,
            objectGeneration,
            fence.renderer.generation,
            fence.device.generation,
        };
        result.state.handles.emplace(stable, handle);
      }

      const bool projected =
          source.sceneAssetIndex != SceneRuntimeMapping::kInvalidIndex;
      std::uint32_t sceneAssetIndex = projected ? source.sceneAssetIndex
                                                : stable.slot;
      std::uint32_t colorSceneAssetIndex = SceneRuntimeMapping::kInvalidIndex;
      std::uint32_t linearSceneAssetIndex = SceneRuntimeMapping::kInvalidIndex;
      if (stable.kind == EditorObjectKind::texture) {
        const SceneTextureProjectionBinding *binding =
            textureProjectionBinding(stable);
        if (binding == nullptr) {
          result.error =
              "renderer mapping has a texture without a typed projection binding";
          return result;
        }
        const auto coordinateInBounds = [&](std::uint32_t coordinate) {
          return coordinate == SceneRuntimeMapping::kInvalidIndex ||
                 coordinate < projection.asset.textures.size();
        };
        if (binding->sceneAssetIndex == SceneRuntimeMapping::kInvalidIndex ||
            binding->sceneAssetIndex >= projection.asset.textures.size() ||
            !coordinateInBounds(binding->colorSceneAssetIndex) ||
            !coordinateInBounds(binding->linearSceneAssetIndex)) {
          result.error =
              "renderer texture projection binding has an invalid SceneAsset coordinate";
          return result;
        }
        sceneAssetIndex = binding->sceneAssetIndex;
        colorSceneAssetIndex = binding->colorSceneAssetIndex;
        linearSceneAssetIndex = binding->linearSceneAssetIndex;
      } else if (stable.kind == EditorObjectKind::sampler) {
        const std::optional<std::uint32_t> samplerIndex =
            samplerProjectionIndex(stable);
        if (!samplerIndex) {
          result.error =
              "renderer mapping has a sampler without a typed projection binding";
          return result;
        }
        sceneAssetIndex = *samplerIndex;
      }
      const std::uint64_t runtimeIndex =
          projected
              ? source.projectionIdentity
              : SceneRuntimeMapping::deriveProjectionIdentity(source.documentId);
      entries.push_back(RendererSceneMappingEntry{
          .stableId = stable,
          .projection = {
              .kind = stable.kind,
              .sceneAssetIndex = sceneAssetIndex,
              .runtimeIndex = runtimeIndex,
              .identityDigest = projectionIdentityDigest(stable, runtimeIndex),
              .colorSceneAssetIndex = colorSceneAssetIndex,
              .linearSceneAssetIndex = linearSceneAssetIndex,
          },
          .rendererHandle = handle,
      });
    }
    std::string authorityError;
    std::optional<SceneRuntimeMapping> authority =
        authorityForFence(projection.mapping, fence, &authorityError);
    if (!authority) {
      result.error = authorityError.empty()
                         ? "failed to bind projection authority to renderer fence"
                         : std::move(authorityError);
      return result;
    }
    result.mapping = RendererSceneMappingSnapshot{
        fence, std::move(entries), std::move(*authority)};
    const RendererSceneMappingValidation validation = result.mapping.validate();
    if (!validation.valid()) {
      result.error = validation.message;
    } else {
      std::sort(primaryStableIds.begin(), primaryStableIds.end());
      for (auto handle = result.state.handles.begin();
           handle != result.state.handles.end();) {
        if (!std::binary_search(primaryStableIds.begin(),
                                primaryStableIds.end(), handle->first)) {
          handle = result.state.handles.erase(handle);
        } else {
          ++handle;
        }
      }
    }
  } catch (const std::bad_alloc &) {
    result.error = "out of memory while building the renderer scene mapping";
  } catch (const std::exception &exception) {
    result.error = std::string("failed to build the renderer scene mapping: ") +
                   exception.what();
  }
  return result;
}

std::optional<SceneUploadPlan> EditorRendererAdapter::buildPlan(
    const SceneProjectionResult &projection, const RendererTargetFence &fence,
    std::string &error) const {
  SceneUploadPlanner planner;
  SceneUploadPlanBuildResult built =
      planner.build(makeSceneAssetView(projection.asset));
  SceneUploadPlanValidationResult binding = bindSceneUploadPlanProjection(
      makeSceneAssetView(projection.asset), projection.mapping, built.plan);
  if (!binding.valid) {
    error = binding.error;
    return std::nullopt;
  }
  built.plan.header = SceneUploadPlanHeader{
      .authority = fence.authority,
      .documentRevision = fence.appliedDocumentRevision,
      .activeScene = fence.activeScene,
      .sceneActivationEpoch = fence.sceneActivationEpoch,
      .rootExternalStateSerial = fence.rootExternalStateSerial,
      .projectionDigest = fence.projectionDigest,
      .mappingEpoch = fence.mappingEpoch,
  };
  const SceneUploadPlanValidationResult validation = SceneUploadPlanner::validate(
      makeSceneAssetView(projection.asset), built.plan);
  if (!validation.valid) {
    error = validation.error;
    return std::nullopt;
  }
  error.clear();
  return std::move(built.plan);
}

SceneCanonicalizationResult EditorRendererAdapter::makeFullSnapshot(
    const RendererTargetFence &baseFence,
    const SceneDigest &baseDocumentDigest,
    const SceneProjectionResult &target, FullRebuildReason reason) const {
  SceneChangeSet input;
  input.header.domain = SceneChangeDomain::FullSnapshot;
  input.header.authority = baseFence.authority;
  input.header.baseDocumentRevision = baseFence.appliedDocumentRevision;
  input.header.targetDocumentRevision = target.documentRevision;
  input.header.baseSceneActivationEpoch = baseFence.sceneActivationEpoch;
  input.header.targetSceneActivationEpoch = target.sceneActivationEpoch;
  input.header.baseRootExternalStateSerial =
      baseFence.rootExternalStateSerial;
  input.header.targetRootExternalStateSerial =
      target.mapping.rootExternalStateSerial();
  input.header.baseActiveScene = baseFence.activeScene;
  input.header.targetActiveScene = target.activeScene;
  input.header.baseDocumentDigest = baseDocumentDigest;
  input.header.targetDocumentDigest = target.documentDigest;
  input.header.baseProjectionDigest = baseFence.projectionDigest;
  input.header.targetProjectionDigest = target.projectionDigest;

  FullSnapshotReference full;
  full.reason = reason;
  full.targetSnapshotDigest = target.immutableSnapshot.contentDigest;
  full.immutableTarget = target.immutableSnapshot;
  full.stableIdRemapComplete = true;
  input.fullSnapshot = std::move(full);
  return SceneChangeCanonicalizer::canonicalize(std::move(input));
}

EditorRendererAdapter::FlushResult EditorRendererAdapter::bootstrap(
    RendererFacade &renderer, PendingPublication pending) noexcept {
  FlushResult result;
  result.consumed = true;
  result.refresh = pending.refresh;
  result.fullSnapshot = true;
  try {
    std::string identityError;
    if (!validateCameraLightSourceIdentities(pending.projection,
                                             identityError)) {
      result.error = SceneApplyError::StaleObjectGeneration;
      result.message = std::move(identityError);
      return result;
    }
    const RendererTargetFence target = targetFence(pending.projection, 2u);
    MappingState emptyState;
    emptyState.nextHandleIndices.fill(1u);
    MappingBuild targetMapping =
        buildMapping(
            pending.projection, target, emptyState,
            m_initialized &&
                    m_objectGeneration != std::numeric_limits<std::uint32_t>::max()
                ? m_objectGeneration + 1u
                : 1u);
    if (!targetMapping.succeeded()) {
      result.message = std::move(targetMapping.error);
      return result;
    }
    SceneProjectionOwnerCapability targetOwner =
        pending.projection.ownerCapability.bindMapping(
            *targetMapping.mapping.projectionAuthority(), &identityError);
    if (!targetOwner.valid()) {
      result.error = SceneApplyError::StaleObjectGeneration;
      result.message = identityError.empty()
                           ? "failed to bind projection owner to target fence"
                           : std::move(identityError);
      return result;
    }

    std::string planError;
    std::optional<SceneUploadPlan> targetPlan =
        buildPlan(pending.projection, target, planError);
    if (!targetPlan) {
      result.message = planError.empty()
                           ? "failed to build the editor scene upload plan"
                           : std::move(planError);
      return result;
    }

    std::vector<std::uint8_t> bootstrapBytes{
        'M', 'G', 'I', 'F', '-', 'E', 'D', 'I', 'T', 'O', 'R', '-', 'B', 'O',
        'O', 'T'};
    bootstrapBytes.insert(bootstrapBytes.end(),
                          pending.projection.projectionDigest.begin(),
                          pending.projection.projectionDigest.end());
    const SceneDigest bootstrapDigest =
        SceneChangeCanonicalizer::computeSha256(bootstrapBytes);
    RendererTargetFence base = target;
    base.mappingEpoch = 1u;
    std::vector<RendererSceneMappingEntry> baseEntries(
        targetMapping.mapping.entries().begin(),
        targetMapping.mapping.entries().end());
    SceneRuntimeMappingFence baseAuthorityFence{
        .authority = base.authority,
        .appliedDocumentRevision = base.appliedDocumentRevision,
        .activeScene = base.activeScene,
        .sceneActivationEpoch = base.sceneActivationEpoch,
        .rootExternalStateSerial = base.rootExternalStateSerial,
        .projectionDigest = base.projectionDigest,
        .mappingEpoch = base.mappingEpoch,
    };
    std::string baseAuthorityError;
    std::optional<SceneRuntimeMapping> baseAuthority =
        SceneRuntimeMapping::create(
            std::move(baseAuthorityFence),
            std::vector<SceneRuntimeMappingEntry>(
                pending.projection.mapping.entries().begin(),
                pending.projection.mapping.entries().end()),
            std::vector<SceneRuntimeMappingAlias>(
                pending.projection.mapping.aliases().begin(),
                pending.projection.mapping.aliases().end()),
            &baseAuthorityError);
    if (!baseAuthority) {
      result.message = baseAuthorityError.empty()
                           ? "failed to derive bootstrap projection authority"
                           : std::move(baseAuthorityError);
      return result;
    }
    RendererSceneMappingSnapshot baseMapping{
        base, std::move(baseEntries), std::move(*baseAuthority)};

    SceneUploadPlan basePlan = *targetPlan;
    basePlan.header.projectionDigest = base.projectionDigest;
    basePlan.header.mappingEpoch = base.mappingEpoch;
    RendererSceneTransactionInitialState initial;
    initial.publication.fence = base;
    initial.publication.mapping = std::move(baseMapping);
    initial.publication.activeGenerationDigest = bootstrapDigest;
    initial.publication.generation = 1u;
    initial.publication.descriptorGeneration = 1u;
    initial.activeAsset = pending.projection.asset;
    initial.activePlan = std::move(basePlan);
    initial.ownerCapability = pending.projection.ownerCapability.bindMapping(
        *initial.publication.mapping.projectionAuthority(), &identityError);
    if (!initial.ownerCapability.valid()) {
      result.error = SceneApplyError::StaleObjectGeneration;
      result.message = identityError.empty()
                           ? "failed to bind projection owner to renderer fence"
                           : std::move(identityError);
      return result;
    }
    if (m_initialized) {
      reset(renderer);
    }
    const RendererSceneTransactionInitializationResult initialized =
        renderer.initializeSceneTransactions(std::move(initial));
    if (!initialized.initialized) {
      result.error = initialized.error;
      result.message = initialized.message.empty()
                           ? "failed to initialize renderer scene transactions"
                           : initialized.message;
      return result;
    }
    m_initialized = true;

    SceneCanonicalizationResult canonical = makeFullSnapshot(
        base, pending.projection.documentDigest, pending.projection,
        FullRebuildReason::ExplicitCallerRequest);
    if (!canonical.succeeded()) {
      result.message = canonical.validation.message.empty()
                           ? "failed to canonicalize the initial scene snapshot"
                           : canonical.validation.message;
      reset(renderer);
      return result;
    }

    ApplyFullSceneSnapshotRequest request;
    request.transaction.expectedTarget = base;
    request.transaction.canonicalIdentity =
        canonicalIdentity(canonical.changeSet->header);
    request.transaction.changes = std::move(*canonical.changeSet);
    request.transaction.canonicalBytes = std::move(canonical.canonicalBytes);
    request.transaction.targetMapping = targetMapping.mapping;
    request.candidateAsset = pending.projection.asset;
    request.candidatePlan = *targetPlan;
    request.ownerCapability = std::move(targetOwner);
    request.transaction.ownerCapability = request.ownerCapability;
    const SceneApplyResult applied = renderer.applyFullSceneSnapshot(request);
    if (!applied.accepted() || !applied.resultingFence) {
      result.error = applied.error;
      result.message = applyFailureMessage(applied);
      reset(renderer);
      return result;
    }
    if (*applied.resultingFence != target) {
      result.message = "renderer accepted an unexpected target fence";
      reset(renderer);
      return result;
    }

    m_activeFence = *applied.resultingFence;
    m_activeDocumentDigest = pending.projection.documentDigest;
    m_activeGenerationDigest =
        request.transaction.changes.fullSnapshot->targetSnapshotDigest;
    m_mappingState = std::move(targetMapping.state);
    result.accepted = true;
    result.error = SceneApplyError::None;
    result.resultingFence = m_activeFence;
    result.projection = std::move(pending.projection);
    result.uploadPlan = std::move(targetPlan);
  } catch (const std::bad_alloc &) {
    result.message = "out of memory while bootstrapping editor scene transactions";
  } catch (const std::exception &exception) {
    result.message = std::string("failed to bootstrap editor scene transactions: ") +
                     exception.what();
  } catch (...) {
    result.message = "failed to bootstrap editor scene transactions";
  }
  if (!result.accepted && m_initialized) {
    reset(renderer);
  }
  return result;
}

EditorRendererAdapter::FlushResult EditorRendererAdapter::apply(
    RendererFacade &renderer, PendingPublication pending) noexcept {
  FlushResult result;
  result.consumed = true;
  result.refresh = pending.refresh;
  try {
    std::string identityError;
    if (!validateCameraLightSourceIdentities(pending.projection,
                                             identityError)) {
      result.error = SceneApplyError::StaleObjectGeneration;
      result.message = std::move(identityError);
      return result;
    }
    if (!m_activeFence) {
      result.message = "renderer scene transaction fence is absent";
      return result;
    }
    SceneCanonicalizationResult synthesized;
    const SceneCanonicalizationResult *canonical = nullptr;
    if (pending.canonical) {
      canonical = &*pending.canonical;
    } else {
      synthesized = makeFullSnapshot(
          *m_activeFence, m_activeDocumentDigest, pending.projection,
          FullRebuildReason::ExplicitCallerRequest);
      canonical = &synthesized;
    }
    if (canonical == nullptr || !canonical->succeeded()) {
      result.message = canonical != nullptr &&
                               !canonical->validation.message.empty()
                           ? canonical->validation.message
                           : "editor scene publication is not canonical";
      return result;
    }
    const SceneChangeHeader &header = canonical->changeSet->header;
    if (header.authority != m_activeFence->authority ||
        pending.projection.mapping.authority() != header.authority ||
        pending.projection.documentRevision != header.targetDocumentRevision ||
        pending.projection.sceneActivationEpoch !=
            header.targetSceneActivationEpoch ||
        pending.projection.mapping.rootExternalStateSerial() !=
            header.targetRootExternalStateSerial ||
        pending.projection.activeScene != header.targetActiveScene ||
        pending.projection.documentDigest != header.targetDocumentDigest ||
        pending.projection.projectionDigest != header.targetProjectionDigest) {
      result.message =
          "editor projection does not match its canonical target envelope";
      return result;
    }
    if (m_activeFence->mappingEpoch ==
        std::numeric_limits<std::uint64_t>::max()) {
      result.message = "renderer scene mapping epoch is exhausted";
      return result;
    }
    const RendererTargetFence target = targetFence(
        pending.projection, m_activeFence->mappingEpoch + 1u);
    MappingBuild targetMapping =
        buildMapping(pending.projection, target, m_mappingState,
                     m_objectGeneration);
    if (!targetMapping.succeeded()) {
      result.message = std::move(targetMapping.error);
      return result;
    }

    ApplySceneChangeRequest transaction;
    transaction.expectedTarget = *m_activeFence;
    transaction.canonicalIdentity = canonicalIdentity(header);
    transaction.changes = *canonical->changeSet;
    transaction.canonicalBytes = canonical->canonicalBytes;
    transaction.targetMapping = targetMapping.mapping;
    transaction.ownerCapability = pending.projection.ownerCapability.bindMapping(
        *transaction.targetMapping.projectionAuthority(), &identityError);
    if (!transaction.ownerCapability.valid()) {
      result.error = SceneApplyError::StaleObjectGeneration;
      result.message = identityError.empty()
                           ? "failed to bind projection owner to transaction fence"
                           : std::move(identityError);
      return result;
    }

    SceneApplyResult applied;
    std::optional<SceneUploadPlan> targetPlan;
    result.fullSnapshot =
        header.domain == SceneChangeDomain::FullSnapshot;
    if (result.fullSnapshot) {
      std::string planError;
      targetPlan = buildPlan(pending.projection, target, planError);
      if (!targetPlan) {
        result.message = planError.empty()
                             ? "failed to build the full-scene upload plan"
                             : std::move(planError);
        return result;
      }
      ApplyFullSceneSnapshotRequest request;
      request.transaction = std::move(transaction);
      request.candidateAsset = pending.projection.asset;
      request.candidatePlan = *targetPlan;
      request.ownerCapability = request.transaction.ownerCapability;
      applied = renderer.applyFullSceneSnapshot(request);
    } else {
      applied = renderer.applySceneChanges(transaction);
    }
    if (!applied.accepted() || !applied.resultingFence) {
      result.error = applied.error;
      // Renderer fallback reasons are diagnostics only. Silently converting a
      // rejected delta into a full rebuild would violate the hot-path contract.
      result.message = applyFailureMessage(applied);
      return result;
    }
    if (*applied.resultingFence != target) {
      result.message = "renderer accepted an unexpected target fence";
      reset(renderer);
      return result;
    }

    m_activeFence = *applied.resultingFence;
    m_activeDocumentDigest = pending.projection.documentDigest;
    m_activeGenerationDigest = canonical->changeSet->fullSnapshot
                                   ? canonical->changeSet->fullSnapshot
                                         ->targetSnapshotDigest
                                   : header.canonicalChangeSetDigest;
    m_mappingState = std::move(targetMapping.state);
    result.accepted = true;
    result.error = SceneApplyError::None;
    result.resultingFence = m_activeFence;
    result.projection = std::move(pending.projection);
    result.uploadPlan = std::move(targetPlan);
  } catch (const std::bad_alloc &) {
    result.message = "out of memory while applying the editor scene publication";
  } catch (const std::exception &exception) {
    result.message = std::string("failed to apply the editor scene publication: ") +
                     exception.what();
  } catch (...) {
    result.message = "failed to apply the editor scene publication";
  }
  return result;
}

EditorRendererAdapter::FlushResult
EditorRendererAdapter::flush(RendererFacade &renderer) noexcept {
  if (m_pending.empty()) {
    return {};
  }
  PendingPublication pending = std::move(m_pending.front());
  m_pending.pop_front();

  const bool authorityChanged =
      m_activeFence &&
      m_activeFence->authority != pending.projection.mapping.authority();
  const bool sceneActivationRebuild =
      pending.canonical && pending.canonical->changeSet &&
      pending.canonical->changeSet->header.domain ==
          SceneChangeDomain::SceneActivation;
  FlushResult result = !m_initialized || authorityChanged ||
                               m_requiresAuthoritativeRebuild ||
                               sceneActivationRebuild
                           ? bootstrap(renderer, std::move(pending))
                           : apply(renderer, std::move(pending));
  if (!result.accepted) {
    // Later publications are based on a target the renderer never accepted.
    // Drop the broken lineage and wait for an explicit authoritative rebuild.
    m_pending.clear();
    m_requiresAuthoritativeRebuild = true;
  } else {
    m_requiresAuthoritativeRebuild = false;
  }
  return result;
}

void EditorRendererAdapter::reset(RendererFacade &renderer) noexcept {
  m_pending.clear();
  if (m_initialized) {
    renderer.resetSceneTransactions();
  }
  m_initialized = false;
  m_requiresAuthoritativeRebuild = false;
  m_activeFence.reset();
  m_activeDocumentDigest = {};
  m_activeGenerationDigest = {};
  m_mappingState.handles.clear();
  m_mappingState.nextHandleIndices.fill(1u);
  if (m_objectGeneration == std::numeric_limits<std::uint32_t>::max()) {
    m_objectGeneration = 1u;
  } else {
    ++m_objectGeneration;
  }
}

void EditorRendererAdapter::clearPending() noexcept { m_pending.clear(); }

} // namespace demo::editor::ui
