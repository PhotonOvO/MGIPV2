#pragma once

#include "SceneAssetView.h"
#include "SceneJobSystem.h"
#include "SceneUploadPlanner.h"

#include <atomic>
#include <functional>
#include <vector>

namespace demo {

class ParallelSceneLoader
{
public:
  struct Progress {
    uint32_t completedJobs{0};
    uint32_t totalJobs{0};
    float    percent{0.0f};
    bool     cancelled{false};
  };

  struct BuildOptions {
    SceneUploadPlanner::BuildOptions plannerOptions{};
    uint32_t                         workerCount{0};
    std::function<void(const Progress&)> onProgress;
    std::atomic<bool>*               cancelFlag{nullptr};
  };

  struct BuildResult {
    SceneUploadPlan           plan;
    SceneLoadJobGraph         jobGraph;
    std::vector<SceneLoadJob> orderedJobs;
    Progress                  progress;
    bool                      cancelled{false};
  };

  [[nodiscard]] BuildResult build(const SceneAssetView& asset, const BuildOptions& options = {}) const;
};

}  // namespace demo
