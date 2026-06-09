#include "ParallelSceneLoader.h"

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

ParallelSceneLoader::BuildResult ParallelSceneLoader::build(const SceneAssetView& asset, const BuildOptions& options) const
{
  BuildResult result;
  result.jobGraph = SceneUploadPlanner::buildJobGraph(asset, options.plannerOptions);

  SceneUploadPlan plan;
  plan.meshes.resize(asset.meshes.size());
  plan.textures.resize(asset.textures.size());
  plan.materials.resize(asset.materials.size());

  std::vector<glm::mat4> worldTransforms;

  SceneJobSystem jobSystem;
  SceneJobSystem::ExecuteOptions executeOptions{};
  executeOptions.workerCount = options.workerCount;
  executeOptions.onProgress = [&](uint32_t completedJobs, uint32_t totalJobs) {
    result.progress.completedJobs = completedJobs;
    result.progress.totalJobs = totalJobs;
    result.progress.percent = totalJobs > 0 ? static_cast<float>(completedJobs) / static_cast<float>(totalJobs) : 1.0f;
    if(options.onProgress) {
      options.onProgress(result.progress);
    }
  };
  executeOptions.shouldCancel = [&]() {
    return options.cancelFlag != nullptr && options.cancelFlag->load(std::memory_order_relaxed);
  };

  const bool success = jobSystem.execute(result.jobGraph, [&](const SceneLoadJob& job) {
    switch(job.type) {
    case SceneLoadJobType::MeshPayload: {
      if(job.index >= asset.meshes.size()) {
        return;
      }
      const SceneMesh& mesh = asset.meshes[job.index];
      plan.meshes[job.index] = MeshUploadPlan{
          .meshIndex = job.index,
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
      };
      break;
    }
    case SceneLoadJobType::TexturePayload: {
      if(job.index >= asset.textures.size()) {
        return;
      }
      const SceneTexture& texture = asset.textures[job.index];
      plan.textures[job.index] = TextureUploadPlan{
          .textureIndex = job.index,
          .format = texture.format,
          .width = texture.width,
          .height = texture.height,
          .mipLevels = texture.mipLevels,
          .payloadKind = texture.isKtx2 ? TexturePayloadKind::Ktx2Container : TexturePayloadKind::RawRgba8,
          .payloadSrcOffset = texture.payloadOffset,
          .payloadByteSize = texture.payloadSize,
      };
      break;
    }
    case SceneLoadJobType::MaterialRecord: {
      if(job.index >= asset.materials.size()) {
        return;
      }
      const SceneMaterial& material = asset.materials[job.index];
      plan.materials[job.index] = MaterialCreatePlan{
          .materialIndex = job.index,
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
      };
      break;
    }
    case SceneLoadJobType::NodeTransform:
      worldTransforms = TransformSystem::buildWorldTransforms(asset);
      plan.transforms.worldTransforms = worldTransforms;
      break;
    case SceneLoadJobType::InstanceBuild:
      plan.instances = TransformSystem::buildInstances(asset, worldTransforms);
      break;
    case SceneLoadJobType::BoundsBuild:
      plan.cullRecords = TransformSystem::buildCullRecords(asset, plan.instances);
      break;
    case SceneLoadJobType::DescriptorPlan: {
      plan.descriptorUpdates.clear();
      plan.descriptorUpdates.reserve(plan.materials.size());
      for(const MaterialCreatePlan& material : plan.materials) {
        plan.descriptorUpdates.push_back(DescriptorUpdatePlan{
            .materialIndex = material.materialIndex,
            .textureIndices = {{
                material.baseColorTexture,
                material.normalTexture,
                material.metallicRoughnessTexture,
                material.occlusionTexture,
                material.emissiveTexture,
            }},
        });
      }
      break;
    }
    case SceneLoadJobType::DrawCommandPlan: {
      plan.drawCommands.clear();
      plan.drawCommands.reserve(plan.instances.instances.size());
      for(const SceneDrawInstance& instance : plan.instances.instances) {
        const int32_t alphaMode = instance.materialIndex >= plan.materials.size()
                                      ? 0
                                      : plan.materials[instance.materialIndex].alphaMode;
        plan.drawCommands.push_back(DrawCommandBuildPlan{
            .instanceIndex = instance.instanceIndex,
            .meshIndex = instance.meshIndex,
            .materialIndex = instance.materialIndex,
            .bucket = classifyDrawBucket(alphaMode),
        });
      }
      break;
    }
    }
  }, executeOptions);

  result.cancelled = !success;
  result.progress.cancelled = !success;
  if(!success) {
    return result;
  }

  result.plan = std::move(plan);
  const std::vector<uint32_t> orderedJobIndices = result.jobGraph.topologicalOrder();
  result.orderedJobs.reserve(orderedJobIndices.size());
  for(const uint32_t jobIndex : orderedJobIndices) {
    if(jobIndex < result.jobGraph.nodes().size()) {
      result.orderedJobs.push_back(result.jobGraph.nodes()[jobIndex].job);
    }
  }

  result.progress.completedJobs = static_cast<uint32_t>(result.orderedJobs.size());
  result.progress.totalJobs = static_cast<uint32_t>(result.orderedJobs.size());
  result.progress.percent = 1.0f;
  return result;
}

}  // namespace demo
