#include "SceneUploadPlanner.h"

#include "TransformSystem.h"

#include <array>
#include <limits>

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

}  // namespace

SceneLoadJobGraph SceneUploadPlanner::buildJobGraph(const SceneAssetView& asset,
                                                    const BuildOptions&   options)
{
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

SceneUploadPlanValidationResult SceneUploadPlanner::validate(const SceneAssetView& asset,
                                                             const SceneUploadPlan& plan)
{
  const SceneAssetValidationResult assetValidation = validateSceneAssetView(asset);
  if(!assetValidation.valid) {
    return {false, assetValidation.error};
  }

  for(const MeshUploadPlan& meshPlan : plan.meshes) {
    if(meshPlan.meshIndex >= asset.meshes.size()) {
      return {false, "mesh upload plan index is out of bounds"};
    }
    if(meshPlan.vertexStride != kSceneUploadVertexStride || meshPlan.vertexCount != asset.meshes[meshPlan.meshIndex].vertexCount
       || meshPlan.indexCount != asset.meshes[meshPlan.meshIndex].indexCount) {
      return {false, "mesh upload plan does not match SceneAsset mesh metadata"};
    }
    if(!sceneRangeFits(meshPlan.vertexSrcOffset, meshPlan.vertexByteSize, asset.vertexPayload.size())
       || !sceneRangeFits(meshPlan.indexSrcOffset, meshPlan.indexByteSize, asset.indexPayload.size())) {
      return {false, "mesh upload plan payload range is out of bounds"};
    }
  }

  for(const TextureUploadPlan& texturePlan : plan.textures) {
    if(texturePlan.textureIndex >= asset.textures.size()) {
      return {false, "texture upload plan index is out of bounds"};
    }
    const SceneTexture& texture = asset.textures[texturePlan.textureIndex];
    if(texturePlan.width != texture.width || texturePlan.height != texture.height || texturePlan.mipLevels != texture.mipLevels) {
      return {false, "texture upload plan does not match SceneAsset texture metadata"};
    }
    if(!sceneRangeFits(texturePlan.payloadSrcOffset, texturePlan.payloadByteSize, asset.texturePayload.size())) {
      return {false, "texture upload plan payload range is out of bounds"};
    }
  }

  for(const MaterialCreatePlan& materialPlan : plan.materials) {
    if(materialPlan.materialIndex >= asset.materials.size()) {
      return {false, "material create plan index is out of bounds"};
    }
    const int32_t textureRefs[] = {
        materialPlan.baseColorTexture,
        materialPlan.normalTexture,
        materialPlan.metallicRoughnessTexture,
        materialPlan.occlusionTexture,
        materialPlan.emissiveTexture,
    };
    for(const int32_t textureRef : textureRefs) {
      if(textureRef >= 0 && static_cast<size_t>(textureRef) >= asset.textures.size()) {
        return {false, "material create plan texture index is out of bounds"};
      }
    }
  }

  if(!plan.transforms.worldTransforms.empty() && plan.transforms.worldTransforms.size() != asset.nodes.size()) {
    return {false, "transform build plan node count does not match SceneAsset"};
  }

  for(const SceneDrawInstance& instance : plan.instances.instances) {
    if(instance.instanceIndex >= plan.instances.instances.size()) {
      return {false, "scene draw instance index is out of bounds"};
    }
    if(instance.nodeIndex >= asset.nodes.size() || instance.meshIndex >= asset.meshes.size()) {
      return {false, "scene draw instance references an invalid node or mesh"};
    }
    if(instance.materialIndex >= asset.materials.size() && !asset.materials.empty()) {
      return {false, "scene draw instance material index is out of bounds"};
    }
  }

  for(const InstanceCullRecord& cullRecord : plan.cullRecords) {
    if(cullRecord.instanceIndex >= plan.instances.instances.size() || cullRecord.meshIndex >= asset.meshes.size()) {
      return {false, "cull record references an invalid instance or mesh"};
    }
  }

  for(const DescriptorUpdatePlan& descriptorPlan : plan.descriptorUpdates) {
    if(descriptorPlan.materialIndex >= asset.materials.size()) {
      return {false, "descriptor update plan material index is out of bounds"};
    }
    for(const int32_t textureRef : descriptorPlan.textureIndices) {
      if(textureRef >= 0 && static_cast<size_t>(textureRef) >= asset.textures.size()) {
        return {false, "descriptor update plan texture index is out of bounds"};
      }
    }
  }

  for(const DrawCommandBuildPlan& drawCommand : plan.drawCommands) {
    if(drawCommand.instanceIndex >= plan.instances.instances.size() || drawCommand.meshIndex >= asset.meshes.size()) {
      return {false, "draw command plan references an invalid instance or mesh"};
    }
    if(drawCommand.materialIndex >= asset.materials.size() && !asset.materials.empty()) {
      return {false, "draw command plan material index is out of bounds"};
    }
  }

  return {};
}

SceneUploadPlanBuildResult SceneUploadPlanner::build(const SceneAssetView& asset,
                                                     const BuildOptions& options) const
{
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
    plan.meshes.push_back(MeshUploadPlan{
        .meshIndex = meshIndex,
        .vertexSrcOffset = mesh.vertexOffset,
        .vertexByteSize = static_cast<uint64_t>(mesh.vertexCount) * kSceneUploadVertexStride,
        .indexSrcOffset = mesh.indexOffset,
        .indexByteSize = static_cast<uint64_t>(mesh.indexCount) * sizeof(uint32_t),
        .vertexCount = mesh.vertexCount,
        .indexCount = mesh.indexCount,
        .vertexStride = kSceneUploadVertexStride,
        .indexType = VK_INDEX_TYPE_UINT32,
        .localBoundsMin = mesh.localBoundsMin,
        .localBoundsMax = mesh.localBoundsMax,
    });
  }

  plan.transforms.worldTransforms = TransformSystem::buildWorldTransforms(asset);
  plan.instances = TransformSystem::buildInstances(asset, plan.transforms.worldTransforms);

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
