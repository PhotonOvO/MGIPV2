#pragma once

#include <cstdint>

namespace demo {

enum class SceneLoadJobType : uint32_t {
  MeshPayload,
  TexturePayload,
  MaterialRecord,
  NodeTransform,
  InstanceBuild,
  BoundsBuild,
  DescriptorPlan,
  DrawCommandPlan,
};

struct SceneLoadJob {
  SceneLoadJobType type{SceneLoadJobType::MeshPayload};
  uint32_t index{0};
};

[[nodiscard]] inline const char* toString(SceneLoadJobType type)
{
  switch(type) {
  case SceneLoadJobType::MeshPayload:
    return "MeshPayload";
  case SceneLoadJobType::TexturePayload:
    return "TexturePayload";
  case SceneLoadJobType::MaterialRecord:
    return "MaterialRecord";
  case SceneLoadJobType::NodeTransform:
    return "NodeTransform";
  case SceneLoadJobType::InstanceBuild:
    return "InstanceBuild";
  case SceneLoadJobType::BoundsBuild:
    return "BoundsBuild";
  case SceneLoadJobType::DescriptorPlan:
    return "DescriptorPlan";
  case SceneLoadJobType::DrawCommandPlan:
    return "DrawCommandPlan";
  }

  return "Unknown";
}

}  // namespace demo
