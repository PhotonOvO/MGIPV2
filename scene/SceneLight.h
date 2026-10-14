#pragma once

#include <glm/glm.hpp>

#include <cstdint>
#include <limits>
#include <string>

namespace demo {

enum class SceneLightType : uint32_t
{
  directional = 0,
  point = 1,
  spot = 2,
};

struct SceneLight
{
  std::string name;
  int32_t nodeIndex{-1};
  SceneLightType type{SceneLightType::point};
  bool enabled{true};
  glm::vec3 color{1.0f};
  float intensity{1.0f};
  float range{0.0f};
  float innerConeAngle{0.0f};
  float outerConeAngle{0.78539816339f};

  // Projection-owned source identity. The document index proves the source
  // relationship while runtimeIdentity binds the full stable editor ID.
  uint32_t sourceLightIndex{std::numeric_limits<uint32_t>::max()};
  uint64_t runtimeIdentity{0u};
};

}  // namespace demo
