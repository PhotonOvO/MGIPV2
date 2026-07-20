#pragma once

#include <cstdint>
#include <string>

namespace demo {

enum class SceneCameraType : uint32_t
{
  perspective = 0,
  orthographic = 1,
};

struct SceneCamera
{
  std::string name;
  int32_t nodeIndex{-1};
  SceneCameraType type{SceneCameraType::perspective};

  // Perspective values. aspectRatio == 0 uses the active viewport aspect.
  float aspectRatio{0.0f};
  float verticalFieldOfViewRadians{0.78539816339f};
  float nearPlane{0.1f};
  // glTF allows an omitted perspective zfar. Zero preserves that state and
  // the renderer resolves it to a large finite distance for existing culling.
  float farPlane{0.0f};

  // Orthographic values are the positive half extents from glTF.
  float horizontalMagnification{1.0f};
  float verticalMagnification{1.0f};
};

}  // namespace demo
