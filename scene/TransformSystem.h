#pragma once

#include "SceneAssetView.h"
#include "SceneUploadPlan.h"

#include <vector>

namespace demo {

class TransformSystem
{
public:
  [[nodiscard]] static std::vector<glm::mat4> buildWorldTransforms(const SceneAssetView& asset);
  [[nodiscard]] static InstanceBuildPlan      buildInstances(const SceneAssetView& asset,
                                                             const std::vector<glm::mat4>& worldTransforms);
  [[nodiscard]] static std::vector<InstanceCullRecord> buildCullRecords(const SceneAssetView& asset,
                                                                        const InstanceBuildPlan& instances);
};

}  // namespace demo
