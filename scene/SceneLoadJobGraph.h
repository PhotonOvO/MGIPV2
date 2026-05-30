#pragma once

#include "SceneLoadJob.h"

#include <cstdint>
#include <deque>
#include <vector>

namespace demo {

struct SceneLoadJobNode {
  SceneLoadJob job{};
  std::vector<uint32_t> dependents;
  uint32_t indegree{0};
};

class SceneLoadJobGraph {
public:
  [[nodiscard]] uint32_t addJob(const SceneLoadJob& job)
  {
    m_nodes.push_back(SceneLoadJobNode{.job = job});
    return static_cast<uint32_t>(m_nodes.size() - 1);
  }

  void addDependency(uint32_t prerequisiteIndex, uint32_t dependentIndex)
  {
    if(prerequisiteIndex >= m_nodes.size() || dependentIndex >= m_nodes.size()) {
      return;
    }

    m_nodes[prerequisiteIndex].dependents.push_back(dependentIndex);
    ++m_nodes[dependentIndex].indegree;
  }

  [[nodiscard]] const std::vector<SceneLoadJobNode>& nodes() const { return m_nodes; }
  [[nodiscard]] bool empty() const { return m_nodes.empty(); }

  [[nodiscard]] std::vector<uint32_t> topologicalOrder() const
  {
    std::vector<uint32_t> order;
    order.reserve(m_nodes.size());

    std::vector<uint32_t> indegrees;
    indegrees.reserve(m_nodes.size());
    for(const SceneLoadJobNode& node : m_nodes) {
      indegrees.push_back(node.indegree);
    }

    std::deque<uint32_t> ready;
    for(uint32_t index = 0; index < indegrees.size(); ++index) {
      if(indegrees[index] == 0) {
        ready.push_back(index);
      }
    }

    while(!ready.empty()) {
      const uint32_t nodeIndex = ready.front();
      ready.pop_front();
      order.push_back(nodeIndex);

      for(const uint32_t dependentIndex : m_nodes[nodeIndex].dependents) {
        if(dependentIndex >= indegrees.size()) {
          continue;
        }

        if(--indegrees[dependentIndex] == 0) {
          ready.push_back(dependentIndex);
        }
      }
    }

    return order;
  }

private:
  std::vector<SceneLoadJobNode> m_nodes;
};

}  // namespace demo
