#include "SceneJobSystem.h"

#include <algorithm>
#include <atomic>
#include <condition_variable>
#include <deque>
#include <mutex>
#include <thread>
#include <vector>

namespace demo {

uint32_t SceneJobSystem::defaultWorkerCount()
{
  const uint32_t hardwareThreads = std::thread::hardware_concurrency();
  return std::max(1u, hardwareThreads == 0 ? 4u : hardwareThreads);
}

bool SceneJobSystem::execute(const SceneLoadJobGraph& graph,
                             const JobCallback&       callback,
                             const ExecuteOptions&   options) const
{
  const std::vector<SceneLoadJobNode>& nodes = graph.nodes();
  if(nodes.empty()) {
    return true;
  }

  const uint32_t totalJobs = static_cast<uint32_t>(nodes.size());
  std::vector<uint32_t> indegrees;
  indegrees.reserve(nodes.size());
  for(const SceneLoadJobNode& node : nodes) {
    indegrees.push_back(node.indegree);
  }

  std::deque<uint32_t> readyQueue;
  for(uint32_t index = 0; index < indegrees.size(); ++index) {
    if(indegrees[index] == 0) {
      readyQueue.push_back(index);
    }
  }

  const uint32_t workerCount = std::max(1u, options.workerCount == 0 ? defaultWorkerCount() : options.workerCount);
  std::mutex              mutex;
  std::condition_variable condition;
  std::atomic<uint32_t>   completedJobs{0};
  std::atomic<uint32_t>   runningJobs{0};
  std::atomic<bool>       cancelled{false};
  std::atomic<bool>       failed{false};

  auto shouldCancel = [&]() {
    return cancelled.load(std::memory_order_relaxed)
        || (options.shouldCancel && options.shouldCancel());
  };

  auto worker = [&]() {
    while(true) {
      uint32_t nodeIndex = 0;
      {
        std::unique_lock<std::mutex> lock(mutex);
        condition.wait(lock, [&]() {
          return failed.load(std::memory_order_relaxed)
              || shouldCancel()
              || !readyQueue.empty()
              || (completedJobs.load(std::memory_order_relaxed) == totalJobs
                  && runningJobs.load(std::memory_order_relaxed) == 0);
        });

        if(shouldCancel()) {
          cancelled.store(true, std::memory_order_relaxed);
          return;
        }
        if(failed.load(std::memory_order_relaxed)) {
          return;
        }
        if(readyQueue.empty()) {
          if(completedJobs.load(std::memory_order_relaxed) == totalJobs
             && runningJobs.load(std::memory_order_relaxed) == 0) {
            return;
          }
          continue;
        }

        nodeIndex = readyQueue.front();
        readyQueue.pop_front();
        runningJobs.fetch_add(1, std::memory_order_relaxed);
      }

      try {
        callback(nodes[nodeIndex].job);
      } catch(...) {
        failed.store(true, std::memory_order_relaxed);
      }

      const uint32_t finishedCount = completedJobs.fetch_add(1, std::memory_order_relaxed) + 1;
      {
        std::lock_guard<std::mutex> lock(mutex);
        runningJobs.fetch_sub(1, std::memory_order_relaxed);
        if(!failed.load(std::memory_order_relaxed)) {
          for(const uint32_t dependentIndex : nodes[nodeIndex].dependents) {
            if(dependentIndex >= indegrees.size()) {
              continue;
            }
            if(--indegrees[dependentIndex] == 0) {
              readyQueue.push_back(dependentIndex);
            }
          }
        }
      }

      if(options.onProgress) {
        options.onProgress(finishedCount, totalJobs);
      }
      condition.notify_all();
    }
  };

  const uint32_t launchCount = std::min(workerCount, totalJobs);
  std::vector<std::thread> workers;
  workers.reserve(launchCount);
  for(uint32_t index = 0; index < launchCount; ++index) {
    workers.emplace_back(worker);
  }

  for(std::thread& thread : workers) {
    if(thread.joinable()) {
      thread.join();
    }
  }

  return !failed.load(std::memory_order_relaxed) && !cancelled.load(std::memory_order_relaxed);
}

}  // namespace demo
