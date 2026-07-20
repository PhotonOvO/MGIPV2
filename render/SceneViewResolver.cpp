#include "SceneViewResolver.h"

#include <algorithm>
#include <cmath>

namespace demo {

namespace {

[[nodiscard]] const glm::mat4* findWorldTransform(int32_t nodeIndex,
                                                   std::span<const SceneNode> sceneNodes,
                                                   std::span<const GltfNodeData> gltfNodes)
{
  if(nodeIndex < 0) {
    return nullptr;
  }

  const size_t index = static_cast<size_t>(nodeIndex);
  if(index < sceneNodes.size()) {
    return &sceneNodes[index].worldTransform;
  }
  if(index < gltfNodes.size()) {
    return &gltfNodes[index].worldTransform;
  }
  return nullptr;
}

[[nodiscard]] bool isFiniteMatrix(const glm::mat4& matrix)
{
  for(int column = 0; column < 4; ++column) {
    for(int row = 0; row < 4; ++row) {
      if(!std::isfinite(matrix[column][row])) {
        return false;
      }
    }
  }
  return true;
}

[[nodiscard]] glm::vec3 safeNormalize(const glm::vec3& value, const glm::vec3& fallback)
{
  const float lengthSquared = glm::dot(value, value);
  return lengthSquared > 1.0e-8f ? value * glm::inversesqrt(lengthSquared) : fallback;
}

}  // namespace

void populateCameraUniforms(const glm::mat4& view,
                            const glm::mat4& projection,
                            const glm::vec3& position,
                            shaderio::CameraUniforms& uniforms)
{
  uniforms.view = view;
  uniforms.projection = projection;
  uniforms.viewProjection = projection * view;
  uniforms.inverseViewProjection = glm::inverse(uniforms.viewProjection);
  uniforms.prevView = view;
  uniforms.prevProjection = projection;
  uniforms.prevViewProjection = uniforms.viewProjection;
  uniforms.unjitteredViewProjection = uniforms.viewProjection;
  uniforms.unjitteredInverseViewProjection = uniforms.inverseViewProjection;
  uniforms.prevUnjitteredViewProjection = uniforms.viewProjection;
  uniforms.prevJitteredViewProjection = uniforms.viewProjection;
  uniforms.cameraPosition = position;
  uniforms.shadowConstantBias = 0.0f;
  uniforms.shadowDirectionAndSlopeBias = glm::vec4(0.0f);
}

void populateNavigatedSceneCameraUniforms(
    const shaderio::CameraUniforms& sceneCameraUniforms,
    const glm::mat4& localNavigationView,
    shaderio::CameraUniforms& uniforms)
{
  const glm::mat4 sceneCameraWorld = glm::inverse(sceneCameraUniforms.view);
  const glm::mat4 localNavigationWorld = glm::inverse(localNavigationView);
  const glm::mat4 navigatedCameraWorld = sceneCameraWorld * localNavigationWorld;
  populateCameraUniforms(glm::inverse(navigatedCameraWorld),
                         sceneCameraUniforms.projection,
                         glm::vec3(navigatedCameraWorld[3]),
                         uniforms);
}

bool populatePrimarySceneCameraUniforms(
    std::span<const SceneCamera> cameras,
    std::span<const SceneNode> sceneNodes,
    std::span<const GltfNodeData> gltfNodes,
    const glm::mat4& flightCameraProjection,
    shaderio::CameraUniforms& uniforms)
{
  if(cameras.empty()) {
    return false;
  }

  const SceneCamera& camera = cameras.front();
  const glm::mat4* worldTransform = findWorldTransform(camera.nodeIndex, sceneNodes, gltfNodes);
  if(worldTransform == nullptr || !isFiniteMatrix(*worldTransform)) {
    return false;
  }

  if(!isFiniteMatrix(flightCameraProjection)) {
    return false;
  }

  const glm::vec3 position((*worldTransform)[3]);
  const glm::vec3 forward = safeNormalize(-glm::vec3((*worldTransform)[2]), glm::vec3(0.0f, 0.0f, -1.0f));
  const glm::vec3 importedUp = safeNormalize(glm::vec3((*worldTransform)[1]), glm::vec3(0.0f, 1.0f, 0.0f));
  const glm::vec3 right = safeNormalize(glm::cross(forward, importedUp),
                                        safeNormalize(glm::vec3((*worldTransform)[0]),
                                                      glm::vec3(1.0f, 0.0f, 0.0f)));
  const glm::vec3 up = safeNormalize(glm::cross(right, forward), glm::vec3(0.0f, 1.0f, 0.0f));
  const glm::mat4 view = glm::lookAt(position, position + forward, up);
  if(!isFiniteMatrix(view)) {
    return false;
  }

  populateCameraUniforms(view, flightCameraProjection, position, uniforms);
  return true;
}

bool applyPrimarySceneDirectionalLight(
    std::span<const SceneLight> lights,
    std::span<const SceneNode> sceneNodes,
    std::span<const GltfNodeData> gltfNodes,
    DirectionalLightSettings& settings)
{
  if(lights.empty()) {
    return false;
  }

  settings.color = glm::vec3(0.0f);
  settings.ambient = glm::vec3(0.0f);

  for(const SceneLight& light : lights) {
    if(!light.enabled || light.type != SceneLightType::directional) {
      continue;
    }

    const glm::mat4* worldTransform = findWorldTransform(light.nodeIndex, sceneNodes, gltfNodes);
    if(worldTransform == nullptr) {
      continue;
    }

    settings.direction = safeNormalize(
        glm::vec3(*worldTransform * glm::vec4(0.0f, 0.0f, -1.0f, 0.0f)),
        settings.direction);
    settings.color = light.color * std::max(light.intensity, 0.0f);
    break;
  }

  return true;
}

}  // namespace demo
