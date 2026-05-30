#pragma once

#include "SceneLoadJobGraph.h"

#include <functional>

namespace demo {

class SceneJobSystem
{
public:
  using JobCallback = std::function<void(const SceneLoadJob&)>;
  using ProgressCallback = std::function<void(uint32_t completedJobs, uint32_t totalJobs)>;
  using CancelCallback = std::function<bool()>;

  struct ExecuteOptions {
    uint32_t workerCount{0};
    ProgressCallback onProgress;
    CancelCallback   shouldCancel;
  };

  [[nodiscard]] static uint32_t defaultWorkerCount();
  [[nodiscard]] bool execute(const SceneLoadJobGraph& graph,
                             const JobCallback&       callback,
                             const ExecuteOptions&   options = {}) const;
};

}  // namespace demo
