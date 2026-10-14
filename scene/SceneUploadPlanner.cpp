#include "SceneUploadPlanner.h"

#include "../common/ProfilerMarkers.h"
#include "TransformSystem.h"

#include <mgif/scene_editor/SceneRuntimeMapping.h>

#include <algorithm>
#include <array>
#include <limits>
#include <optional>
#include <utility>
#include <vector>

namespace demo {

namespace {

SceneDrawBucket classifyDrawBucket(int32_t alphaMode)
{
  switch(alphaMode) {
  case 1:
    return SceneDrawBucket::AlphaMask;
  case 2:
    return SceneDrawBucket::Transparent;
  default:
    return SceneDrawBucket::Opaque;
  }
}

SceneUploadPlanValidationResult invalidPlan(
    SceneUploadPlanError code,
    const char* error,
    size_t recordIndex = SceneUploadPlanValidationResult::kNoRecord,
    std::optional<editor::StableSceneId> sourceId = std::nullopt)
{
  SceneUploadPlanValidationResult result;
  result.valid = false;
  result.code = code;
  result.sourceId = sourceId;
  result.recordIndex = recordIndex;
  result.error = error;
  return result;
}

bool emptyStableId(editor::StableSceneId id)
{
  return id == editor::StableSceneId{};
}

SceneUploadPlanValidationResult validateSourceId(
    editor::StableSceneId id,
    editor::EditorObjectKind expectedKind,
    const SceneUploadPlanHeader& header,
    bool legacy,
    size_t recordIndex)
{
  if(legacy) {
    return emptyStableId(id)
        ? SceneUploadPlanValidationResult{}
        : invalidPlan(SceneUploadPlanError::InvalidEnvelope,
                      "legacy upload plan contains transactional stable IDs",
                      recordIndex,
                      id);
  }
  if(!id.valid()) {
    return invalidPlan(SceneUploadPlanError::MissingStableId,
                       "transactional upload plan record has no stable source ID",
                       recordIndex);
  }
  if(id.kind != expectedKind || id.authority != header.authority) {
    return invalidPlan(SceneUploadPlanError::StableIdMismatch,
                       "transactional upload plan stable ID kind or authority differs",
                       recordIndex,
                       id);
  }
  return {};
}

SceneUploadPlanValidationResult validateDrawableIdentity(
    SceneDrawableSourceIdentity identity,
    const SceneUploadPlanHeader& header,
    size_t recordIndex)
{
  // Pre-P6 transaction fixtures did not carry the redundant active-scene
  // field. Accept that one missing field at validation boundaries; binding and
  // the renderer's private publication copy always materialize it.
  if(emptyStableId(identity.sceneId) && header.activeScene) {
    identity.sceneId = *header.activeScene;
  }
  if(!identity.valid() || !header.activeScene ||
     identity.sceneId != *header.activeScene) {
    return invalidPlan(SceneUploadPlanError::StableIdMismatch,
                       "drawable occurrence identity differs from active scene",
                       recordIndex,
                       identity.primitiveId.valid()
                           ? std::optional{identity.primitiveId}
                           : std::nullopt);
  }
  return {};
}

SceneUploadPlanValidationResult bindSourceId(
    const editor::SceneRuntimeMapping& mapping,
    editor::EditorObjectKind kind,
    uint32_t sceneAssetIndex,
    editor::StableSceneId& destination,
    size_t recordIndex)
{
  const std::optional<editor::EditorObjectId> documentId =
      mapping.documentIdAt(kind, sceneAssetIndex);
  if(!documentId) {
    return invalidPlan(SceneUploadPlanError::MissingStableId,
                       "projection mapping has no stable ID for candidate coordinate",
                       recordIndex);
  }
  const editor::StableSceneId expected = editor::stableSceneId(*documentId);
  if(!emptyStableId(destination) && destination != expected) {
    return invalidPlan(SceneUploadPlanError::StableIdMismatch,
                       "candidate record stable ID differs from projection mapping",
                       recordIndex,
                       destination);
  }
  destination = expected;
  return {};
}

template<typename Record>
const Record* findRecordByIndex(const std::vector<Record>& records,
                                 uint32_t Record::* member,
                                 uint32_t index)
{
  const auto found = std::find_if(records.begin(), records.end(),
                                  [&](const Record& record) {
                                    return record.*member == index;
                                  });
  return found == records.end() ? nullptr : &*found;
}

std::optional<uint32_t> primitiveIndexForOccurrence(
    const SceneAssetView& asset,
    uint32_t nodeIndex,
    uint32_t meshIndex)
{
  std::optional<uint32_t> result;
  for(uint32_t primitiveIndex = 0;
      primitiveIndex < asset.primitives.size(); ++primitiveIndex) {
    const ScenePrimitive& primitive = asset.primitives[primitiveIndex];
    if(primitive.nodeIndex != nodeIndex || primitive.meshIndex != meshIndex) {
      continue;
    }
    if(result) {
      return std::nullopt;
    }
    result = primitiveIndex;
  }
  return result;
}

std::optional<uint32_t> primitiveIndexForMesh(const SceneAssetView& asset,
                                              uint32_t meshIndex)
{
  std::optional<uint32_t> result;
  for(uint32_t primitiveIndex = 0;
      primitiveIndex < asset.primitives.size(); ++primitiveIndex) {
    if(asset.primitives[primitiveIndex].meshIndex != meshIndex) {
      continue;
    }
    if(result) {
      return std::nullopt;
    }
    result = primitiveIndex;
  }
  return result;
}

bool containsIdentity(
    const std::vector<SceneDrawableSourceIdentity>& identities,
    SceneDrawableSourceIdentity identity)
{
  return std::find(identities.begin(), identities.end(), identity) !=
         identities.end();
}

editor::EditorObjectKind requiredResourceObjectKind(SceneRequiredResourceKind kind)
{
  switch(kind) {
  case SceneRequiredResourceKind::Mesh:
    return editor::EditorObjectKind::primitive;
  case SceneRequiredResourceKind::Texture:
    return editor::EditorObjectKind::image;
  case SceneRequiredResourceKind::Material:
    return editor::EditorObjectKind::material;
  }
  return editor::EditorObjectKind::invalid;
}

}  // namespace

SceneLoadJobGraph SceneUploadPlanner::buildJobGraph(const SceneAssetView& asset,
                                                    const BuildOptions&   options)
{
  VKDEMO_CPU_SCOPE("Scene.UploadPlan.BuildJobGraph");
  SceneLoadJobGraph graph;

  std::vector<uint32_t> textureJobIndices;
  textureJobIndices.reserve(asset.textures.size());
  for(uint32_t textureIndex = 0; textureIndex < asset.textures.size(); ++textureIndex) {
    textureJobIndices.push_back(graph.addJob(SceneLoadJob{SceneLoadJobType::TexturePayload, textureIndex}));
  }

  std::vector<uint32_t> materialJobIndices;
  materialJobIndices.reserve(asset.materials.size());
  for(uint32_t materialIndex = 0; materialIndex < asset.materials.size(); ++materialIndex) {
    const SceneMaterial& material = asset.materials[materialIndex];
    const uint32_t materialJobIndex = graph.addJob(SceneLoadJob{SceneLoadJobType::MaterialRecord, materialIndex});
    materialJobIndices.push_back(materialJobIndex);

    const std::array<int32_t, 5> referencedTextures = {{
        material.baseColorTexture,
        material.normalTexture,
        material.metallicRoughnessTexture,
        material.occlusionTexture,
        material.emissiveTexture,
    }};
    for(const int32_t referencedTexture : referencedTextures) {
      if(referencedTexture >= 0 && static_cast<size_t>(referencedTexture) < textureJobIndices.size()) {
        graph.addDependency(textureJobIndices[static_cast<size_t>(referencedTexture)], materialJobIndex);
      }
    }
  }

  std::vector<uint32_t> meshJobIndices;
  meshJobIndices.reserve(asset.meshes.size());
  for(uint32_t meshIndex = 0; meshIndex < asset.meshes.size(); ++meshIndex) {
    meshJobIndices.push_back(graph.addJob(SceneLoadJob{SceneLoadJobType::MeshPayload, meshIndex}));
  }

  const uint32_t nodeTransformJobIndex = graph.addJob(SceneLoadJob{SceneLoadJobType::NodeTransform, 0});
  const uint32_t instanceBuildJobIndex = graph.addJob(SceneLoadJob{SceneLoadJobType::InstanceBuild, 0});
  graph.addDependency(nodeTransformJobIndex, instanceBuildJobIndex);

  uint32_t boundsBuildJobIndex = std::numeric_limits<uint32_t>::max();
  if(options.buildBounds) {
    boundsBuildJobIndex = graph.addJob(SceneLoadJob{SceneLoadJobType::BoundsBuild, 0});
    graph.addDependency(instanceBuildJobIndex, boundsBuildJobIndex);
  }

  uint32_t descriptorPlanJobIndex = std::numeric_limits<uint32_t>::max();
  if(options.buildDescriptorPlans) {
    descriptorPlanJobIndex = graph.addJob(SceneLoadJob{SceneLoadJobType::DescriptorPlan, 0});
    for(const uint32_t materialJobIndex : materialJobIndices) {
      graph.addDependency(materialJobIndex, descriptorPlanJobIndex);
    }
  }

  if(options.buildDrawPlans) {
    const uint32_t drawPlanJobIndex = graph.addJob(SceneLoadJob{SceneLoadJobType::DrawCommandPlan, 0});
    graph.addDependency(instanceBuildJobIndex, drawPlanJobIndex);
    if(boundsBuildJobIndex != std::numeric_limits<uint32_t>::max()) {
      graph.addDependency(boundsBuildJobIndex, drawPlanJobIndex);
    }
    if(descriptorPlanJobIndex != std::numeric_limits<uint32_t>::max()) {
      graph.addDependency(descriptorPlanJobIndex, drawPlanJobIndex);
    }
    for(const uint32_t meshJobIndex : meshJobIndices) {
      graph.addDependency(meshJobIndex, drawPlanJobIndex);
    }
    for(const uint32_t materialJobIndex : materialJobIndices) {
      graph.addDependency(materialJobIndex, drawPlanJobIndex);
    }
  }

  return graph;
}

SceneLoadJobGraph SceneUploadPlanner::buildJobGraph(const SceneAssetView& asset)
{
  return buildJobGraph(asset, BuildOptions{});
}

SceneUploadPlanValidationResult SceneUploadPlanner::validate(const SceneAssetView& asset,
                                                             const SceneUploadPlan& plan)
{
  VKDEMO_CPU_SCOPE("Scene.UploadPlan.Validate");
  const SceneAssetValidationResult assetValidation = validateSceneAssetView(asset);
  if(!assetValidation.valid) {
    SceneUploadPlanValidationResult result =
        invalidPlan(SceneUploadPlanError::InvalidAsset,
                    "SceneAsset validation failed");
    result.error = assetValidation.error;
    return result;
  }

  const bool legacy = plan.header.emptyLegacyEnvelope();
  if(!legacy && !plan.header.valid()) {
    return invalidPlan(SceneUploadPlanError::InvalidEnvelope,
                       "upload plan header is partially populated or invalid");
  }
  if(!legacy && plan.header.documentRevision != asset.documentRevision) {
    return invalidPlan(SceneUploadPlanError::EnvelopeMismatch,
                       "upload plan revision does not match SceneAsset revision");
  }

  std::vector<bool> seenMeshes(asset.meshes.size(), false);
  std::vector<bool> seenMeshPrimitives(asset.primitives.size(), false);
  std::vector<SceneDrawableSourceIdentity> seenMeshIdentities;
  seenMeshIdentities.reserve(plan.meshes.size());
  for(size_t recordIndex = 0; recordIndex < plan.meshes.size(); ++recordIndex) {
    const MeshUploadPlan& meshPlan = plan.meshes[recordIndex];
    const SceneUploadPlanValidationResult idValidation = validateSourceId(
        meshPlan.sourceId, editor::EditorObjectKind::primitive,
        plan.header, legacy, recordIndex);
    if(!idValidation.valid) {
      return idValidation;
    }
    const SceneUploadPlanValidationResult nodeIdValidation = validateSourceId(
        meshPlan.nodeSourceId, editor::EditorObjectKind::node,
        plan.header, legacy, recordIndex);
    if(!nodeIdValidation.valid) {
      return nodeIdValidation;
    }
    if(meshPlan.meshIndex >= asset.meshes.size()) {
      return invalidPlan(SceneUploadPlanError::InvalidRecord,
                         "mesh upload plan index is out of bounds",
                         recordIndex,
                         legacy ? std::nullopt
                                : std::optional{meshPlan.sourceId});
    }
    if(seenMeshes[meshPlan.meshIndex]) {
      return invalidPlan(SceneUploadPlanError::DuplicateStableId,
                         "mesh upload plan repeats a candidate coordinate",
                         recordIndex,
                         legacy ? std::nullopt
                                : std::optional{meshPlan.sourceId});
    }
    seenMeshes[meshPlan.meshIndex] = true;
    if(!legacy) {
      const SceneUploadPlanValidationResult occurrenceValidation =
          validateDrawableIdentity(meshPlan.sourceIdentity(), plan.header,
                                   recordIndex);
      if(!occurrenceValidation.valid) return occurrenceValidation;
      if(meshPlan.primitiveIndex >= asset.primitives.size()) {
        return invalidPlan(
            SceneUploadPlanError::InvalidRecord,
            "mesh upload plan primitive coordinate is out of bounds",
            recordIndex,
            meshPlan.sourceId);
      }
      const ScenePrimitive& primitive = asset.primitives[meshPlan.primitiveIndex];
      const std::optional<uint32_t> resolvedPrimitive =
          primitiveIndexForOccurrence(asset, primitive.nodeIndex,
                                      meshPlan.meshIndex);
      if(primitive.meshIndex != meshPlan.meshIndex || !resolvedPrimitive ||
         *resolvedPrimitive != meshPlan.primitiveIndex) {
        return invalidPlan(
            SceneUploadPlanError::InvalidRecord,
            "mesh upload plan does not identify one unique primitive occurrence",
            recordIndex,
            meshPlan.sourceId);
      }
      if(seenMeshPrimitives[meshPlan.primitiveIndex]) {
        return invalidPlan(SceneUploadPlanError::DuplicateStableId,
                           "mesh upload plan repeats a primitive coordinate",
                           recordIndex,
                           meshPlan.sourceId);
      }
      seenMeshPrimitives[meshPlan.primitiveIndex] = true;
      if(containsIdentity(seenMeshIdentities, meshPlan.sourceIdentity())) {
        return invalidPlan(SceneUploadPlanError::DuplicateStableId,
                           "mesh upload plan repeats a drawable source identity",
                           recordIndex,
                           meshPlan.sourceId);
      }
      seenMeshIdentities.push_back(meshPlan.sourceIdentity());
    }
    if(meshPlan.vertexStride != kSceneUploadVertexStride || meshPlan.vertexCount != asset.meshes[meshPlan.meshIndex].vertexCount
       || meshPlan.indexCount != asset.meshes[meshPlan.meshIndex].indexCount) {
      return invalidPlan(SceneUploadPlanError::InvalidRecord,
                         "mesh upload plan does not match SceneAsset mesh metadata",
                         recordIndex,
                         legacy ? std::nullopt
                                : std::optional{meshPlan.sourceId});
    }
    if(!sceneRangeFits(meshPlan.vertexSrcOffset, meshPlan.vertexByteSize, asset.vertexPayload.size())
       || !sceneRangeFits(meshPlan.indexSrcOffset, meshPlan.indexByteSize, asset.indexPayload.size())) {
      return invalidPlan(SceneUploadPlanError::PayloadRange,
                         "mesh upload plan payload range is out of bounds",
                         recordIndex,
                         legacy ? std::nullopt
                                : std::optional{meshPlan.sourceId});
    }
  }

  std::vector<bool> seenTextures(asset.textures.size(), false);
  for(size_t recordIndex = 0; recordIndex < plan.textures.size(); ++recordIndex) {
    const TextureUploadPlan& texturePlan = plan.textures[recordIndex];
    const SceneUploadPlanValidationResult idValidation = validateSourceId(
        texturePlan.sourceId, editor::EditorObjectKind::image,
        plan.header, legacy, recordIndex);
    if(!idValidation.valid) {
      return idValidation;
    }
    if(texturePlan.textureIndex >= asset.textures.size()) {
      return invalidPlan(SceneUploadPlanError::InvalidRecord,
                         "texture upload plan index is out of bounds",
                         recordIndex,
                         legacy ? std::nullopt
                                : std::optional{texturePlan.sourceId});
    }
    if(seenTextures[texturePlan.textureIndex]) {
      return invalidPlan(SceneUploadPlanError::DuplicateStableId,
                         "texture upload plan repeats a candidate coordinate",
                         recordIndex,
                         legacy ? std::nullopt
                                : std::optional{texturePlan.sourceId});
    }
    seenTextures[texturePlan.textureIndex] = true;
    const SceneTexture& texture = asset.textures[texturePlan.textureIndex];
    if(texturePlan.width != texture.width || texturePlan.height != texture.height || texturePlan.mipLevels != texture.mipLevels) {
      return invalidPlan(SceneUploadPlanError::InvalidRecord,
                         "texture upload plan does not match SceneAsset texture metadata",
                         recordIndex,
                         legacy ? std::nullopt
                                : std::optional{texturePlan.sourceId});
    }
    if(!sceneRangeFits(texturePlan.payloadSrcOffset, texturePlan.payloadByteSize, asset.texturePayload.size())) {
      return invalidPlan(SceneUploadPlanError::PayloadRange,
                         "texture upload plan payload range is out of bounds",
                         recordIndex,
                         legacy ? std::nullopt
                                : std::optional{texturePlan.sourceId});
    }
  }

  std::vector<bool> seenMaterials(asset.materials.size(), false);
  for(size_t recordIndex = 0; recordIndex < plan.materials.size(); ++recordIndex) {
    const MaterialCreatePlan& materialPlan = plan.materials[recordIndex];
    const SceneUploadPlanValidationResult idValidation = validateSourceId(
        materialPlan.sourceId, editor::EditorObjectKind::material,
        plan.header, legacy, recordIndex);
    if(!idValidation.valid) {
      return idValidation;
    }
    if(materialPlan.materialIndex >= asset.materials.size()) {
      return invalidPlan(SceneUploadPlanError::InvalidRecord,
                         "material create plan index is out of bounds",
                         recordIndex,
                         legacy ? std::nullopt
                                : std::optional{materialPlan.sourceId});
    }
    if(seenMaterials[materialPlan.materialIndex]) {
      return invalidPlan(SceneUploadPlanError::DuplicateStableId,
                         "material create plan repeats a candidate coordinate",
                         recordIndex,
                         legacy ? std::nullopt
                                : std::optional{materialPlan.sourceId});
    }
    seenMaterials[materialPlan.materialIndex] = true;
    const int32_t textureRefs[] = {
        materialPlan.baseColorTexture,
        materialPlan.normalTexture,
        materialPlan.metallicRoughnessTexture,
        materialPlan.occlusionTexture,
        materialPlan.emissiveTexture,
    };
    for(const int32_t textureRef : textureRefs) {
      if(textureRef >= 0 && static_cast<size_t>(textureRef) >= asset.textures.size()) {
        return invalidPlan(SceneUploadPlanError::InvalidRecord,
                           "material create plan texture index is out of bounds",
                           recordIndex,
                           legacy ? std::nullopt
                                  : std::optional{materialPlan.sourceId});
      }
    }
  }

  if(!plan.transforms.worldTransforms.empty() && plan.transforms.worldTransforms.size() != asset.nodes.size()) {
    return invalidPlan(SceneUploadPlanError::InvalidRecord,
                       "transform build plan node count does not match SceneAsset");
  }
  if(legacy && !plan.transforms.sourceIds.empty()) {
    return invalidPlan(SceneUploadPlanError::InvalidEnvelope,
                       "legacy transform plan contains transactional stable IDs");
  }
  if(!legacy && plan.transforms.sourceIds.size() != plan.transforms.worldTransforms.size()) {
    return invalidPlan(SceneUploadPlanError::MissingStableId,
                       "transactional transform plan has no parallel stable-ID vector");
  }
  for(size_t recordIndex = 0; recordIndex < plan.transforms.sourceIds.size(); ++recordIndex) {
    const SceneUploadPlanValidationResult idValidation = validateSourceId(
        plan.transforms.sourceIds[recordIndex], editor::EditorObjectKind::node,
        plan.header, legacy, recordIndex);
    if(!idValidation.valid) {
      return idValidation;
    }
  }

  if(!legacy) {
    for(size_t recordIndex = 0; recordIndex < plan.meshes.size(); ++recordIndex) {
      const MeshUploadPlan& meshPlan = plan.meshes[recordIndex];
      const ScenePrimitive& primitive = asset.primitives[meshPlan.primitiveIndex];
      if(primitive.nodeIndex >= plan.transforms.sourceIds.size() ||
         plan.transforms.sourceIds[primitive.nodeIndex] !=
             meshPlan.nodeSourceId) {
        return invalidPlan(
            SceneUploadPlanError::StableIdMismatch,
            "mesh upload plan node source differs from its transform coordinate",
            recordIndex,
            meshPlan.sourceId);
      }
    }
  }

  std::vector<bool> seenInstanceIndices(plan.instances.instances.size(), false);
  std::vector<bool> seenInstancePrimitives(asset.primitives.size(), false);
  std::vector<SceneDrawableSourceIdentity> seenInstanceIdentities;
  seenInstanceIdentities.reserve(plan.instances.instances.size());
  for(size_t recordIndex = 0; recordIndex < plan.instances.instances.size(); ++recordIndex) {
    const SceneDrawInstance& instance = plan.instances.instances[recordIndex];
    const SceneUploadPlanValidationResult idValidation = validateSourceId(
        instance.sourceId, editor::EditorObjectKind::primitive,
        plan.header, legacy, recordIndex);
    if(!idValidation.valid) {
      return idValidation;
    }
    const SceneUploadPlanValidationResult nodeIdValidation = validateSourceId(
        instance.nodeSourceId, editor::EditorObjectKind::node,
        plan.header, legacy, recordIndex);
    if(!nodeIdValidation.valid) {
      return nodeIdValidation;
    }
    if(instance.instanceIndex >= plan.instances.instances.size()) {
      return invalidPlan(SceneUploadPlanError::InvalidRecord,
                         "scene draw instance index is out of bounds",
                         recordIndex,
                         legacy ? std::nullopt
                                 : std::optional{instance.sourceId});
    }
    if(seenInstanceIndices[instance.instanceIndex]) {
      return invalidPlan(SceneUploadPlanError::DuplicateStableId,
                         "scene draw instances repeat an instance coordinate",
                         recordIndex,
                         legacy ? std::nullopt
                                : std::optional{instance.sourceId});
    }
    seenInstanceIndices[instance.instanceIndex] = true;
    if(instance.nodeIndex >= asset.nodes.size() || instance.meshIndex >= asset.meshes.size()) {
      return invalidPlan(SceneUploadPlanError::InvalidRecord,
                         "scene draw instance references an invalid node or mesh",
                         recordIndex,
                         legacy ? std::nullopt
                                 : std::optional{instance.sourceId});
    }
    if(!legacy) {
      const SceneUploadPlanValidationResult occurrenceValidation =
          validateDrawableIdentity(instance.sourceIdentity(), plan.header,
                                   recordIndex);
      if(!occurrenceValidation.valid) return occurrenceValidation;
      if(instance.primitiveIndex >= asset.primitives.size()) {
        return invalidPlan(
            SceneUploadPlanError::InvalidRecord,
            "scene draw instance primitive coordinate is out of bounds",
            recordIndex,
            instance.sourceId);
      }
      const std::optional<uint32_t> resolvedPrimitive =
          primitiveIndexForOccurrence(asset, instance.nodeIndex,
                                      instance.meshIndex);
      if(!resolvedPrimitive || *resolvedPrimitive != instance.primitiveIndex) {
        return invalidPlan(
            SceneUploadPlanError::InvalidRecord,
            "scene draw instance does not identify one unique primitive occurrence",
            recordIndex,
            instance.sourceId);
      }
      if(seenInstancePrimitives[instance.primitiveIndex]) {
        return invalidPlan(SceneUploadPlanError::DuplicateStableId,
                           "scene draw instances repeat a primitive coordinate",
                           recordIndex,
                           instance.sourceId);
      }
      seenInstancePrimitives[instance.primitiveIndex] = true;
      if(containsIdentity(seenInstanceIdentities, instance.sourceIdentity())) {
        return invalidPlan(SceneUploadPlanError::DuplicateStableId,
                           "scene draw instances repeat a drawable source identity",
                           recordIndex,
                           instance.sourceId);
      }
      seenInstanceIdentities.push_back(instance.sourceIdentity());
      if(instance.nodeIndex >= plan.transforms.sourceIds.size() ||
         plan.transforms.sourceIds[instance.nodeIndex] !=
             instance.nodeSourceId) {
        return invalidPlan(
            SceneUploadPlanError::StableIdMismatch,
            "scene draw instance node source differs from its transform coordinate",
            recordIndex,
            instance.sourceId);
      }
      const MeshUploadPlan* meshPlan = findRecordByIndex(
          plan.meshes, &MeshUploadPlan::primitiveIndex,
          instance.primitiveIndex);
      if(meshPlan == nullptr ||
         meshPlan->sourceIdentity() != instance.sourceIdentity() ||
         meshPlan->meshIndex != instance.meshIndex) {
        return invalidPlan(
            SceneUploadPlanError::StableIdMismatch,
            "scene draw instance source differs from its mesh upload occurrence",
            recordIndex,
            instance.sourceId);
      }
    }
    if(instance.materialIndex != std::numeric_limits<uint32_t>::max() &&
       instance.materialIndex >= asset.materials.size()) {
      return invalidPlan(SceneUploadPlanError::InvalidRecord,
                         "scene draw instance material index is out of bounds",
                         recordIndex,
                         legacy ? std::nullopt
                                : std::optional{instance.sourceId});
    }
  }

  for(size_t recordIndex = 0; recordIndex < plan.cullRecords.size(); ++recordIndex) {
    const InstanceCullRecord& cullRecord = plan.cullRecords[recordIndex];
    const SceneUploadPlanValidationResult idValidation = validateSourceId(
        cullRecord.sourceId, editor::EditorObjectKind::primitive,
        plan.header, legacy, recordIndex);
    if(!idValidation.valid) {
      return idValidation;
    }
    const SceneUploadPlanValidationResult nodeIdValidation = validateSourceId(
        cullRecord.nodeSourceId, editor::EditorObjectKind::node,
        plan.header, legacy, recordIndex);
    if(!nodeIdValidation.valid) {
      return nodeIdValidation;
    }
    if(cullRecord.instanceIndex >= plan.instances.instances.size() || cullRecord.meshIndex >= asset.meshes.size()) {
      return invalidPlan(SceneUploadPlanError::InvalidRecord,
                         "cull record references an invalid instance or mesh",
                         recordIndex,
                         legacy ? std::nullopt
                                 : std::optional{cullRecord.sourceId});
    }
    const SceneDrawInstance& instance =
        plan.instances.instances[cullRecord.instanceIndex];
    if(cullRecord.meshIndex != instance.meshIndex) {
      return invalidPlan(SceneUploadPlanError::InvalidRecord,
                         "cull record mesh differs from its instance",
                         recordIndex,
                         legacy ? std::nullopt
                                : std::optional{cullRecord.sourceId});
    }
    if(!legacy &&
       cullRecord.sourceIdentity() != instance.sourceIdentity()) {
      return invalidPlan(SceneUploadPlanError::StableIdMismatch,
                         "cull record source identity differs from its instance",
                         recordIndex,
                         cullRecord.sourceId);
    }
    if(!legacy) {
      const SceneUploadPlanValidationResult occurrenceValidation =
          validateDrawableIdentity(cullRecord.sourceIdentity(), plan.header,
                                   recordIndex);
      if(!occurrenceValidation.valid) return occurrenceValidation;
    }
  }

  for(size_t recordIndex = 0; recordIndex < plan.descriptorUpdates.size(); ++recordIndex) {
    const DescriptorUpdatePlan& descriptorPlan = plan.descriptorUpdates[recordIndex];
    const SceneUploadPlanValidationResult idValidation = validateSourceId(
        descriptorPlan.sourceId, editor::EditorObjectKind::material,
        plan.header, legacy, recordIndex);
    if(!idValidation.valid) {
      return idValidation;
    }
    if(descriptorPlan.materialIndex >= asset.materials.size()) {
      return invalidPlan(SceneUploadPlanError::InvalidRecord,
                         "descriptor update plan material index is out of bounds",
                         recordIndex,
                         legacy ? std::nullopt
                                : std::optional{descriptorPlan.sourceId});
    }
    const MaterialCreatePlan* materialPlan = findRecordByIndex(
        plan.materials, &MaterialCreatePlan::materialIndex,
        descriptorPlan.materialIndex);
    if(!legacy && (materialPlan == nullptr ||
                   materialPlan->sourceId != descriptorPlan.sourceId)) {
      return invalidPlan(SceneUploadPlanError::StableIdMismatch,
                         "descriptor source ID differs from its material",
                         recordIndex,
                         descriptorPlan.sourceId);
    }
    for(const int32_t textureRef : descriptorPlan.textureIndices) {
      if(textureRef >= 0 && static_cast<size_t>(textureRef) >= asset.textures.size()) {
        return invalidPlan(SceneUploadPlanError::InvalidRecord,
                           "descriptor update plan texture index is out of bounds",
                           recordIndex,
                           legacy ? std::nullopt
                                  : std::optional{descriptorPlan.sourceId});
      }
    }
  }

  for(size_t recordIndex = 0; recordIndex < plan.drawCommands.size(); ++recordIndex) {
    const DrawCommandBuildPlan& drawCommand = plan.drawCommands[recordIndex];
    const SceneUploadPlanValidationResult idValidation = validateSourceId(
        drawCommand.sourceId, editor::EditorObjectKind::primitive,
        plan.header, legacy, recordIndex);
    if(!idValidation.valid) {
      return idValidation;
    }
    const SceneUploadPlanValidationResult nodeIdValidation = validateSourceId(
        drawCommand.nodeSourceId, editor::EditorObjectKind::node,
        plan.header, legacy, recordIndex);
    if(!nodeIdValidation.valid) {
      return nodeIdValidation;
    }
    if(drawCommand.instanceIndex >= plan.instances.instances.size() || drawCommand.meshIndex >= asset.meshes.size()) {
      return invalidPlan(SceneUploadPlanError::InvalidRecord,
                         "draw command plan references an invalid instance or mesh",
                         recordIndex,
                         legacy ? std::nullopt
                                 : std::optional{drawCommand.sourceId});
    }
    const SceneDrawInstance& instance =
        plan.instances.instances[drawCommand.instanceIndex];
    if(drawCommand.meshIndex != instance.meshIndex ||
       drawCommand.materialIndex != instance.materialIndex) {
      return invalidPlan(
          SceneUploadPlanError::InvalidRecord,
          "draw command mesh or material differs from its instance",
          recordIndex,
          legacy ? std::nullopt : std::optional{drawCommand.sourceId});
    }
    if(drawCommand.materialIndex != std::numeric_limits<uint32_t>::max() &&
       drawCommand.materialIndex >= asset.materials.size()) {
      return invalidPlan(SceneUploadPlanError::InvalidRecord,
                         "draw command plan material index is out of bounds",
                         recordIndex,
                         legacy ? std::nullopt
                                : std::optional{drawCommand.sourceId});
    }
    if(!legacy &&
       drawCommand.sourceIdentity() != instance.sourceIdentity()) {
      return invalidPlan(SceneUploadPlanError::StableIdMismatch,
                         "draw command source identity differs from its instance",
                         recordIndex,
                         drawCommand.sourceId);
    }
    if(!legacy) {
      const SceneUploadPlanValidationResult occurrenceValidation =
          validateDrawableIdentity(drawCommand.sourceIdentity(), plan.header,
                                   recordIndex);
      if(!occurrenceValidation.valid) return occurrenceValidation;
    }
  }

  if(legacy && !plan.requiredResources.empty()) {
    return invalidPlan(SceneUploadPlanError::InvalidEnvelope,
                       "legacy upload plan contains transactional required resources");
  }
  for(size_t recordIndex = 0; recordIndex < plan.requiredResources.size(); ++recordIndex) {
    const SceneRequiredResource& resource = plan.requiredResources[recordIndex];
    const editor::EditorObjectKind expectedKind =
        requiredResourceObjectKind(resource.kind);
    if(expectedKind == editor::EditorObjectKind::invalid) {
      return invalidPlan(SceneUploadPlanError::InvalidRecord,
                         "required resource kind is invalid",
                         recordIndex,
                         resource.sourceId);
    }
    const SceneUploadPlanValidationResult idValidation = validateSourceId(
        resource.sourceId, expectedKind, plan.header, legacy, recordIndex);
    if(!idValidation.valid) {
      return idValidation;
    }
    if(resource.kind == SceneRequiredResourceKind::Mesh) {
      const SceneUploadPlanValidationResult nodeIdValidation = validateSourceId(
          resource.nodeSourceId, editor::EditorObjectKind::node,
          plan.header, legacy, recordIndex);
      if(!nodeIdValidation.valid) {
        return nodeIdValidation;
      }
      const SceneUploadPlanValidationResult occurrenceValidation =
          validateDrawableIdentity(resource.sourceIdentity(), plan.header,
                                   recordIndex);
      if(!occurrenceValidation.valid) return occurrenceValidation;
    } else if(!emptyStableId(resource.nodeSourceId)) {
      return invalidPlan(
          SceneUploadPlanError::StableIdMismatch,
          "non-mesh required resource contains a drawable node source",
          recordIndex,
          resource.nodeSourceId);
    }
    const bool duplicate = std::any_of(
        plan.requiredResources.begin(),
        plan.requiredResources.begin() + static_cast<std::ptrdiff_t>(recordIndex),
        [&](const SceneRequiredResource& prior) {
          return prior.kind == resource.kind &&
                 (resource.kind == SceneRequiredResourceKind::Mesh
                      ? prior.sourceIdentity() == resource.sourceIdentity()
                      : prior.sourceId == resource.sourceId);
        });
    if(duplicate) {
      return invalidPlan(SceneUploadPlanError::DuplicateStableId,
                         "required resource contains a duplicate stable ID",
                         recordIndex,
                         resource.sourceId);
    }
    const bool exists =
        (resource.kind == SceneRequiredResourceKind::Mesh &&
         std::any_of(plan.meshes.begin(), plan.meshes.end(),
                     [&](const MeshUploadPlan& item) {
                       return item.sourceIdentity() ==
                              resource.sourceIdentity();
                     })) ||
        (resource.kind == SceneRequiredResourceKind::Texture &&
         std::any_of(plan.textures.begin(), plan.textures.end(),
                     [&](const TextureUploadPlan& item) {
                       return item.sourceId == resource.sourceId;
                     })) ||
        (resource.kind == SceneRequiredResourceKind::Material &&
         std::any_of(plan.materials.begin(), plan.materials.end(),
                     [&](const MaterialCreatePlan& item) {
                       return item.sourceId == resource.sourceId;
                     }));
    if(!exists) {
      return invalidPlan(SceneUploadPlanError::MissingRequiredResource,
                         "required resource has no candidate upload record",
                         recordIndex,
                         resource.sourceId);
    }
  }
  if(!legacy) {
    if(plan.meshes.size() != asset.meshes.size() ||
       plan.textures.size() != asset.textures.size() ||
       plan.materials.size() != asset.materials.size()) {
      return invalidPlan(
          SceneUploadPlanError::MissingRequiredResource,
          "transactional upload plan does not cover every candidate resource");
    }
    const size_t expectedRequiredCount =
        plan.meshes.size() + plan.textures.size() + plan.materials.size();
    if(plan.requiredResources.size() != expectedRequiredCount) {
      return invalidPlan(SceneUploadPlanError::MissingRequiredResource,
                         "transactional plan does not declare every required resource");
    }
  }

  return {};
}

SceneUploadPlanValidationResult bindSceneUploadPlanProjection(
    const SceneAssetView& asset,
    const editor::SceneRuntimeMapping& mapping,
    SceneUploadPlan& plan)
{
  const editor::SceneRuntimeMappingFence& mappingFence = mapping.fence();
  const SceneUploadPlanHeader expectedHeader{
      .authority = mappingFence.authority,
      .documentRevision = mappingFence.appliedDocumentRevision,
      .activeScene = mappingFence.activeScene,
      .sceneActivationEpoch = mappingFence.sceneActivationEpoch,
      .rootExternalStateSerial = mappingFence.rootExternalStateSerial,
      .projectionDigest = mappingFence.projectionDigest,
      .mappingEpoch = mappingFence.mappingEpoch,
  };
  if(!expectedHeader.valid()) {
    return invalidPlan(SceneUploadPlanError::InvalidEnvelope,
                       "projection mapping fence is invalid");
  }
  if(expectedHeader.documentRevision != asset.documentRevision) {
    return invalidPlan(SceneUploadPlanError::EnvelopeMismatch,
                       "projection mapping revision does not match SceneAsset");
  }

  SceneUploadPlan candidate = plan;
  if(!candidate.header.emptyLegacyEnvelope() && candidate.header != expectedHeader) {
    return invalidPlan(SceneUploadPlanError::EnvelopeMismatch,
                       "upload plan header differs from projection mapping fence");
  }
  candidate.header = expectedHeader;
  const editor::StableSceneId activeScene =
      expectedHeader.activeScene.value_or(editor::StableSceneId{});

  for(size_t index = 0; index < candidate.meshes.size(); ++index) {
    MeshUploadPlan& mesh = candidate.meshes[index];
    if(mesh.primitiveIndex >= asset.primitives.size()) {
      const std::optional<uint32_t> resolved =
          primitiveIndexForMesh(asset, mesh.meshIndex);
      if(!resolved) {
        return invalidPlan(
            SceneUploadPlanError::InvalidRecord,
            "mesh upload plan has no unique primitive coordinate",
            index);
      }
      mesh.primitiveIndex = *resolved;
    }
    const ScenePrimitive& primitive = asset.primitives[mesh.primitiveIndex];
    const std::optional<uint32_t> resolvedPrimitive =
        primitiveIndexForOccurrence(asset, primitive.nodeIndex,
                                    mesh.meshIndex);
    if(primitive.meshIndex != mesh.meshIndex || !resolvedPrimitive ||
       *resolvedPrimitive != mesh.primitiveIndex) {
      return invalidPlan(
          SceneUploadPlanError::InvalidRecord,
          "mesh upload plan does not identify one unique primitive occurrence",
          index);
    }
    SceneUploadPlanValidationResult result = bindSourceId(
        mapping, editor::EditorObjectKind::primitive,
        mesh.primitiveIndex, mesh.sourceId, index);
    if(!result.valid) {
      return result;
    }
    result = bindSourceId(
        mapping, editor::EditorObjectKind::node, primitive.nodeIndex,
        mesh.nodeSourceId, index);
    if(!result.valid) {
      return result;
    }
  }
  for(size_t index = 0; index < candidate.textures.size(); ++index) {
    SceneUploadPlanValidationResult result = bindSourceId(
        mapping, editor::EditorObjectKind::image,
        candidate.textures[index].textureIndex,
        candidate.textures[index].sourceId, index);
    if(!result.valid) {
      return result;
    }
  }
  for(size_t index = 0; index < candidate.materials.size(); ++index) {
    SceneUploadPlanValidationResult result = bindSourceId(
        mapping, editor::EditorObjectKind::material,
        candidate.materials[index].materialIndex,
        candidate.materials[index].sourceId, index);
    if(!result.valid) {
      return result;
    }
  }

  candidate.transforms.sourceIds.resize(candidate.transforms.worldTransforms.size());
  for(size_t index = 0; index < candidate.transforms.sourceIds.size(); ++index) {
    SceneUploadPlanValidationResult result = bindSourceId(
        mapping, editor::EditorObjectKind::node,
        static_cast<uint32_t>(index), candidate.transforms.sourceIds[index],
        index);
    if(!result.valid) {
      return result;
    }
  }
  for(size_t index = 0; index < candidate.instances.instances.size(); ++index) {
    SceneDrawInstance& instance = candidate.instances.instances[index];
    if(instance.primitiveIndex >= asset.primitives.size()) {
      const std::optional<uint32_t> resolved =
          primitiveIndexForOccurrence(asset, instance.nodeIndex,
                                      instance.meshIndex);
      if(!resolved) {
        return invalidPlan(
            SceneUploadPlanError::InvalidRecord,
            "scene draw instance has no unique primitive coordinate",
            index);
      }
      instance.primitiveIndex = *resolved;
    }
    const std::optional<uint32_t> resolvedPrimitive =
        primitiveIndexForOccurrence(asset, instance.nodeIndex,
                                    instance.meshIndex);
    if(!resolvedPrimitive || *resolvedPrimitive != instance.primitiveIndex) {
      return invalidPlan(
          SceneUploadPlanError::InvalidRecord,
          "scene draw instance does not identify one unique primitive occurrence",
          index);
    }
    SceneUploadPlanValidationResult result = bindSourceId(
        mapping, editor::EditorObjectKind::primitive,
        instance.primitiveIndex,
        instance.sourceId, index);
    if(!result.valid) {
      return result;
    }
    result = bindSourceId(
        mapping, editor::EditorObjectKind::node, instance.nodeIndex,
        instance.nodeSourceId, index);
    if(!result.valid) {
      return result;
    }
    if(!activeScene.valid()) {
      return invalidPlan(SceneUploadPlanError::InvalidEnvelope,
                         "drawable occurrence has no active scene", index,
                         instance.sourceId);
    }
    instance.sceneSourceId = activeScene;
    instance.instanceOrdinal = 0u;
    for(size_t previous = 0; previous < index; ++previous) {
      const SceneDrawInstance& prior = candidate.instances.instances[previous];
      if(prior.sceneSourceId == instance.sceneSourceId &&
         prior.nodeSourceId == instance.nodeSourceId &&
         prior.sourceId == instance.sourceId) {
        ++instance.instanceOrdinal;
      }
    }
  }
  for(size_t index = 0; index < candidate.meshes.size(); ++index) {
    MeshUploadPlan& mesh = candidate.meshes[index];
    const auto found = std::find_if(
        candidate.instances.instances.begin(),
        candidate.instances.instances.end(),
        [&](const SceneDrawInstance& instance) {
          return instance.primitiveIndex == mesh.primitiveIndex &&
                 instance.meshIndex == mesh.meshIndex;
        });
    if(found == candidate.instances.instances.end()) {
      return invalidPlan(SceneUploadPlanError::InvalidRecord,
                         "mesh occurrence has no draw instance", index,
                         mesh.sourceId);
    }
    mesh.sceneSourceId = found->sceneSourceId;
    mesh.instanceOrdinal = found->instanceOrdinal;
  }
  for(size_t index = 0; index < candidate.cullRecords.size(); ++index) {
    InstanceCullRecord& cullRecord = candidate.cullRecords[index];
    if(cullRecord.instanceIndex >= candidate.instances.instances.size()) {
      return invalidPlan(SceneUploadPlanError::InvalidRecord,
                         "cull record references an invalid instance",
                         index);
    }
    const SceneDrawableSourceIdentity expected =
        candidate.instances.instances[cullRecord.instanceIndex].sourceIdentity();
    if((!emptyStableId(cullRecord.sourceId) &&
        cullRecord.sourceId != expected.primitiveId) ||
       (!emptyStableId(cullRecord.nodeSourceId) &&
        cullRecord.nodeSourceId != expected.nodeId)) {
      return invalidPlan(SceneUploadPlanError::StableIdMismatch,
                         "cull record source identity differs from its instance",
                         index,
                         cullRecord.sourceId);
    }
    cullRecord.sourceId = expected.primitiveId;
    cullRecord.nodeSourceId = expected.nodeId;
    cullRecord.sceneSourceId = expected.sceneId;
    cullRecord.instanceOrdinal = expected.instanceOrdinal;
  }
  for(size_t index = 0; index < candidate.descriptorUpdates.size(); ++index) {
    DescriptorUpdatePlan& descriptor = candidate.descriptorUpdates[index];
    SceneUploadPlanValidationResult result = bindSourceId(
        mapping, editor::EditorObjectKind::material,
        descriptor.materialIndex, descriptor.sourceId, index);
    if(!result.valid) {
      return result;
    }
  }
  for(size_t index = 0; index < candidate.drawCommands.size(); ++index) {
    DrawCommandBuildPlan& draw = candidate.drawCommands[index];
    if(draw.instanceIndex >= candidate.instances.instances.size()) {
      return invalidPlan(SceneUploadPlanError::InvalidRecord,
                         "draw command references an invalid instance",
                         index);
    }
    const SceneDrawableSourceIdentity expected =
        candidate.instances.instances[draw.instanceIndex].sourceIdentity();
    if((!emptyStableId(draw.sourceId) &&
        draw.sourceId != expected.primitiveId) ||
       (!emptyStableId(draw.nodeSourceId) &&
        draw.nodeSourceId != expected.nodeId)) {
      return invalidPlan(SceneUploadPlanError::StableIdMismatch,
                         "draw command source identity differs from its instance",
                         index,
                         draw.sourceId);
    }
    draw.sourceId = expected.primitiveId;
    draw.nodeSourceId = expected.nodeId;
    draw.sceneSourceId = expected.sceneId;
    draw.instanceOrdinal = expected.instanceOrdinal;
  }

  if(candidate.requiredResources.empty()) {
    candidate.requiredResources.reserve(
        candidate.meshes.size() + candidate.textures.size() +
        candidate.materials.size());
    for(const MeshUploadPlan& mesh : candidate.meshes) {
      candidate.requiredResources.push_back({
          .sourceId = mesh.sourceId,
          .kind = SceneRequiredResourceKind::Mesh,
          .nodeSourceId = mesh.nodeSourceId,
          .sceneSourceId = mesh.sceneSourceId,
          .instanceOrdinal = mesh.instanceOrdinal,
      });
    }
    for(const TextureUploadPlan& texture : candidate.textures) {
      candidate.requiredResources.push_back({
          .sourceId = texture.sourceId,
          .kind = SceneRequiredResourceKind::Texture,
      });
    }
    for(const MaterialCreatePlan& material : candidate.materials) {
      candidate.requiredResources.push_back({
          .sourceId = material.sourceId,
          .kind = SceneRequiredResourceKind::Material,
      });
    }
  } else {
    std::vector<bool> usedMeshes(candidate.meshes.size(), false);
    for(size_t resourceIndex = 0;
        resourceIndex < candidate.requiredResources.size(); ++resourceIndex) {
      SceneRequiredResource& resource =
          candidate.requiredResources[resourceIndex];
      if(resource.kind != SceneRequiredResourceKind::Mesh) {
        if(!emptyStableId(resource.nodeSourceId) ||
           !emptyStableId(resource.sceneSourceId) ||
           resource.instanceOrdinal != 0u) {
          return invalidPlan(
              SceneUploadPlanError::StableIdMismatch,
              "non-mesh required resource carries drawable occurrence metadata",
              resourceIndex, resource.sourceId);
        }
        continue;
      }

      size_t match = candidate.meshes.size();
      for(size_t meshIndex = 0; meshIndex < candidate.meshes.size();
          ++meshIndex) {
        const MeshUploadPlan& mesh = candidate.meshes[meshIndex];
        if(usedMeshes[meshIndex] || mesh.sourceId != resource.sourceId ||
           (!emptyStableId(resource.nodeSourceId) &&
            resource.nodeSourceId != mesh.nodeSourceId) ||
           (!emptyStableId(resource.sceneSourceId) &&
            resource.sceneSourceId != mesh.sceneSourceId) ||
           (resource.instanceOrdinal != 0u &&
            resource.instanceOrdinal != mesh.instanceOrdinal)) {
          continue;
        }
        match = meshIndex;
        break;
      }
      if(match == candidate.meshes.size()) {
        return invalidPlan(
            SceneUploadPlanError::MissingRequiredResource,
            "required mesh resource has no matching drawable occurrence",
            resourceIndex, resource.sourceId);
      }
      usedMeshes[match] = true;
      const MeshUploadPlan& mesh = candidate.meshes[match];
      resource.nodeSourceId = mesh.nodeSourceId;
      resource.sceneSourceId = mesh.sceneSourceId;
      resource.instanceOrdinal = mesh.instanceOrdinal;
    }
  }
  std::sort(candidate.requiredResources.begin(),
            candidate.requiredResources.end(),
            [](const SceneRequiredResource& left,
               const SceneRequiredResource& right) {
              if(left.kind != right.kind) {
                return left.kind < right.kind;
              }
              if(left.sourceId != right.sourceId) {
                return left.sourceId < right.sourceId;
              }
              if(left.nodeSourceId != right.nodeSourceId) {
                return left.nodeSourceId < right.nodeSourceId;
              }
              if(left.sceneSourceId != right.sceneSourceId) {
                return left.sceneSourceId < right.sceneSourceId;
              }
              return left.instanceOrdinal < right.instanceOrdinal;
            });

  const SceneUploadPlanValidationResult validation =
      SceneUploadPlanner::validate(asset, candidate);
  if(!validation.valid) {
    return validation;
  }
  plan = std::move(candidate);
  return {};
}

SceneUploadPlanBuildResult SceneUploadPlanner::build(const SceneAssetView& asset) const
{
  return build(asset, BuildOptions{});
}

SceneUploadPlanBuildResult SceneUploadPlanner::build(const SceneAssetView& asset,
                                                     const BuildOptions& options) const
{
  VKDEMO_CPU_SCOPE("Scene.UploadPlan.Build");
  SceneUploadPlanBuildResult result;
  SceneUploadPlan& plan = result.plan;
  result.jobGraph = buildJobGraph(asset, options);

  plan.meshes.reserve(asset.meshes.size());
  plan.textures.reserve(asset.textures.size());
  plan.materials.reserve(asset.materials.size());

  for(uint32_t textureIndex = 0; textureIndex < asset.textures.size(); ++textureIndex) {
    const SceneTexture& texture = asset.textures[textureIndex];
    plan.textures.push_back(TextureUploadPlan{
        .textureIndex = textureIndex,
        .format = texture.format,
        .width = texture.width,
        .height = texture.height,
        .mipLevels = texture.mipLevels,
        .payloadKind = texture.isKtx2 ? TexturePayloadKind::Ktx2Container : TexturePayloadKind::RawRgba8,
        .payloadSrcOffset = texture.payloadOffset,
        .payloadByteSize = texture.payloadSize,
    });
  }

  for(uint32_t materialIndex = 0; materialIndex < asset.materials.size(); ++materialIndex) {
    const SceneMaterial& material = asset.materials[materialIndex];
    plan.materials.push_back(MaterialCreatePlan{
        .materialIndex = materialIndex,
        .baseColorFactor = material.baseColorFactor,
        .metallicFactor = material.metallicFactor,
        .roughnessFactor = material.roughnessFactor,
        .normalScale = material.normalScale,
        .occlusionStrength = material.occlusionStrength,
        .emissiveFactor = material.emissiveFactor,
        .baseColorTexture = material.baseColorTexture,
        .normalTexture = material.normalTexture,
        .metallicRoughnessTexture = material.metallicRoughnessTexture,
        .occlusionTexture = material.occlusionTexture,
        .emissiveTexture = material.emissiveTexture,
        .alphaMode = material.alphaMode,
        .alphaCutoff = material.alphaCutoff,
        .materialWorkflow = material.materialWorkflow,
        .doubleSided = material.doubleSided,
    });

    if(options.buildDescriptorPlans) {
      plan.descriptorUpdates.push_back(DescriptorUpdatePlan{
          .materialIndex = materialIndex,
          .textureIndices = {{
              material.baseColorTexture,
              material.normalTexture,
              material.metallicRoughnessTexture,
              material.occlusionTexture,
              material.emissiveTexture,
          }},
      });
    }
  }

  for(uint32_t meshIndex = 0; meshIndex < asset.meshes.size(); ++meshIndex) {
    const SceneMesh& mesh = asset.meshes[meshIndex];
    const std::optional<uint32_t> primitiveIndex =
        primitiveIndexForMesh(asset, meshIndex);
    plan.meshes.push_back(MeshUploadPlan{
        .meshIndex = meshIndex,
        .vertexSrcOffset = mesh.vertexOffset,
        .vertexByteSize = static_cast<uint64_t>(mesh.vertexCount) * kSceneUploadVertexStride,
        .indexSrcOffset = mesh.indexOffset,
        .indexByteSize = static_cast<uint64_t>(mesh.indexCount) * sizeof(uint32_t),
        .vertexCount = mesh.vertexCount,
        .indexCount = mesh.indexCount,
        .vertexStride = kSceneUploadVertexStride,
        .indexType = rhi::IndexFormat::uint32,
        .localBoundsMin = mesh.localBoundsMin,
        .localBoundsMax = mesh.localBoundsMax,
        .primitiveIndex = primitiveIndex.value_or(
            std::numeric_limits<uint32_t>::max()),
    });
  }

  plan.transforms.worldTransforms = TransformSystem::buildWorldTransforms(asset);
  plan.instances = TransformSystem::buildInstances(asset, plan.transforms.worldTransforms);
  for(SceneDrawInstance& instance : plan.instances.instances) {
    const std::optional<uint32_t> primitiveIndex =
        primitiveIndexForOccurrence(asset, instance.nodeIndex,
                                    instance.meshIndex);
    instance.primitiveIndex = primitiveIndex.value_or(
        std::numeric_limits<uint32_t>::max());
  }

  if(options.buildBounds) {
    plan.cullRecords = TransformSystem::buildCullRecords(asset, plan.instances);
  }

  if(options.buildDrawPlans) {
    plan.drawCommands.reserve(plan.instances.instances.size());
    for(const SceneDrawInstance& instance : plan.instances.instances) {
      if(instance.materialIndex >= asset.materials.size()) {
        plan.drawCommands.push_back(DrawCommandBuildPlan{
            .instanceIndex = instance.instanceIndex,
            .meshIndex = instance.meshIndex,
            .materialIndex = instance.materialIndex,
            .bucket = SceneDrawBucket::Opaque,
        });
        continue;
      }

      const SceneMaterial& material = asset.materials[instance.materialIndex];
      plan.drawCommands.push_back(DrawCommandBuildPlan{
          .instanceIndex = instance.instanceIndex,
          .meshIndex = instance.meshIndex,
          .materialIndex = instance.materialIndex,
          .bucket = classifyDrawBucket(material.alphaMode),
      });
    }
  }

  const std::vector<uint32_t> orderedJobIndices = result.jobGraph.topologicalOrder();
  result.orderedJobs.reserve(orderedJobIndices.size());
  for(const uint32_t jobIndex : orderedJobIndices) {
    if(jobIndex < result.jobGraph.nodes().size()) {
      result.orderedJobs.push_back(result.jobGraph.nodes()[jobIndex].job);
    }
  }

  return result;
}

}  // namespace demo
