#pragma once

#include "SceneAssetView.h"
#include "SceneLoadJobGraph.h"
#include "SceneUploadPlan.h"

#include <vector>

namespace demo {

struct SceneUploadPlanBuildResult {
  SceneUploadPlan plan;
  SceneLoadJobGraph jobGraph;
  std::vector<SceneLoadJob> orderedJobs;
};

class SceneUploadPlanner {
public:
  struct BuildOptions {
    bool buildBounds = true;
    bool buildDescriptorPlans = true;
    bool buildDrawPlans = true;
  };

  [[nodiscard]] static SceneLoadJobGraph buildJobGraph(const SceneAssetView& asset,
                                                       const BuildOptions&   options = {});
  [[nodiscard]] static SceneUploadPlanValidationResult validate(const SceneAssetView& asset,
                                                               const SceneUploadPlan& plan);
  [[nodiscard]] SceneUploadPlanBuildResult build(const SceneAssetView& asset,
                                                 const BuildOptions& options = {}) const;
};

}  // namespace demo
