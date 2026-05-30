#include "TransformSystem.h"

#include <glm/gtc/matrix_transform.hpp>

#include <array>
#include <limits>

namespace demo {

namespace {

glm::mat4 composeLocalTransform(const SceneNode& node)
{
  return glm::translate(glm::mat4(1.0f), node.translation)
       * glm::mat4_cast(node.rotation)
       * glm::scale(glm::mat4(1.0f), node.scale);
}

void buildWorldTransformsRecursive(const SceneAssetView& asset,
                                   uint32_t              nodeIndex,
                                   const glm::mat4&      parentTransform,
                                   std::vector<glm::mat4>& outWorldTransforms)
{
  if(nodeIndex >= asset.nodes.size()) {
    return;
  }

  const SceneNode& node = asset.nodes[nodeIndex];
  const glm::mat4  worldTransform = parentTransform * composeLocalTransform(node);
  outWorldTransforms[nodeIndex] = worldTransform;

  for(const uint32_t childIndex : node.children) {
    buildWorldTransformsRecursive(asset, childIndex, worldTransform, outWorldTransforms);
  }
}

void transformBounds(const glm::mat4& worldTransform,
                     const glm::vec3& localMin,
                     const glm::vec3& localMax,
                     glm::vec3&       outWorldMin,
                     glm::vec3&       outWorldMax)
{
  const std::array<glm::vec3, 8> corners = {{
      {localMin.x, localMin.y, localMin.z},
      {localMin.x, localMin.y, localMax.z},
      {localMin.x, localMax.y, localMin.z},
      {localMin.x, localMax.y, localMax.z},
      {localMax.x, localMin.y, localMin.z},
      {localMax.x, localMin.y, localMax.z},
      {localMax.x, localMax.y, localMin.z},
      {localMax.x, localMax.y, localMax.z},
  }};

  outWorldMin = glm::vec3(std::numeric_limits<float>::max());
  outWorldMax = glm::vec3(std::numeric_limits<float>::lowest());

  for(const glm::vec3& corner : corners) {
    const glm::vec3 worldCorner = glm::vec3(worldTransform * glm::vec4(corner, 1.0f));
    outWorldMin = glm::min(outWorldMin, worldCorner);
    outWorldMax = glm::max(outWorldMax, worldCorner);
  }
}

}  // namespace

std::vector<glm::mat4> TransformSystem::buildWorldTransforms(const SceneAssetView& asset)
{
  std::vector<glm::mat4> worldTransforms(asset.nodes.size(), glm::mat4(1.0f));

  for(const uint32_t rootNodeIndex : asset.rootNodes) {
    buildWorldTransformsRecursive(asset, rootNodeIndex, glm::mat4(1.0f), worldTransforms);
  }

  for(uint32_t nodeIndex = 0; nodeIndex < asset.nodes.size(); ++nodeIndex) {
    if(asset.nodes[nodeIndex].parent < 0) {
      buildWorldTransformsRecursive(asset, nodeIndex, glm::mat4(1.0f), worldTransforms);
    }
  }

  return worldTransforms;
}

InstanceBuildPlan TransformSystem::buildInstances(const SceneAssetView&           asset,
                                                  const std::vector<glm::mat4>& worldTransforms)
{
  InstanceBuildPlan instances;

  for(uint32_t nodeIndex = 0; nodeIndex < asset.nodes.size(); ++nodeIndex) {
    if(nodeIndex >= worldTransforms.size()) {
      continue;
    }

    const SceneNode&  node = asset.nodes[nodeIndex];
    const glm::mat4&  worldTransform = worldTransforms[nodeIndex];
    for(const uint32_t meshIndex : node.meshRefs) {
      if(meshIndex >= asset.meshes.size()) {
        continue;
      }

      const SceneMesh& mesh = asset.meshes[meshIndex];
      const uint32_t   instanceIndex = static_cast<uint32_t>(instances.instances.size());
      instances.instances.push_back(SceneDrawInstance{
          .instanceIndex = instanceIndex,
          .nodeIndex = nodeIndex,
          .meshIndex = meshIndex,
          .materialIndex = mesh.materialIndex,
          .worldTransform = worldTransform,
      });
    }
  }

  return instances;
}

std::vector<InstanceCullRecord> TransformSystem::buildCullRecords(const SceneAssetView&      asset,
                                                                  const InstanceBuildPlan& instances)
{
  std::vector<InstanceCullRecord> cullRecords;
  cullRecords.reserve(instances.instances.size());

  for(const SceneDrawInstance& instance : instances.instances) {
    if(instance.meshIndex >= asset.meshes.size()) {
      continue;
    }

    const SceneMesh& mesh = asset.meshes[instance.meshIndex];
    glm::vec3 worldBoundsMin(0.0f);
    glm::vec3 worldBoundsMax(0.0f);
    transformBounds(instance.worldTransform, mesh.localBoundsMin, mesh.localBoundsMax, worldBoundsMin, worldBoundsMax);

    const glm::vec3 center = 0.5f * (worldBoundsMin + worldBoundsMax);
    const float     radius = 0.5f * glm::length(worldBoundsMax - worldBoundsMin);
    cullRecords.push_back(InstanceCullRecord{
        .instanceIndex = instance.instanceIndex,
        .meshIndex = instance.meshIndex,
        .worldBoundsMin = worldBoundsMin,
        .worldBoundsMax = worldBoundsMax,
        .boundingSphere = glm::vec4(center, radius),
    });
  }

  return cullRecords;
}

}  // namespace demo
