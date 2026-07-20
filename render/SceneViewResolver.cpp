#include "SceneViewResolver.h"

#include <glm/gtc/constants.hpp>

#include <algorithm>
#include <cmath>

namespace demo {

namespace {

constexpr float kDefaultFiniteCameraFarPlane = 10000.0f;

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
    rhi::Extent2D viewportSize,
    const clipspace::ProjectionConvention& projectionConvention,
    shaderio::CameraUniforms& uniforms)
{
  if(cameras.empty() || viewportSize.width == 0 || viewportSize.height == 0) {
    return false;
  }

  const SceneCamera& camera = cameras.front();
  const glm::mat4* worldTransform = findWorldTransform(camera.nodeIndex, sceneNodes, gltfNodes);
  if(worldTransform == nullptr || !isFiniteMatrix(*worldTransform)) {
    return false;
  }

  const float determinant = glm::determinant(*worldTransform);
  if(!std::isfinite(determinant) || std::abs(determinant) <= 1.0e-8f) {
    return false;
  }

  const float nearPlane = std::max(camera.nearPlane, 1.0e-4f);
  const float farPlane = camera.farPlane > nearPlane
      ? camera.farPlane
      : std::max(kDefaultFiniteCameraFarPlane, nearPlane + 1.0f);

  glm::mat4 projection{1.0f};
  if(camera.type == SceneCameraType::orthographic) {
    const float xmag = std::max(std::abs(camera.horizontalMagnification), 1.0e-4f);
    const float ymag = std::max(std::abs(camera.verticalMagnification), 1.0e-4f);
    projection = clipspace::makeOrthographicProjection(
        -xmag, xmag, -ymag, ymag, nearPlane, farPlane, projectionConvention);
  } else {
    const float viewportAspect = static_cast<float>(viewportSize.width)
                               / static_cast<float>(viewportSize.height);
    const float aspect = camera.aspectRatio > 0.0f ? camera.aspectRatio : viewportAspect;
    const float fieldOfView = glm::clamp(
        camera.verticalFieldOfViewRadians, 1.0e-4f, glm::pi<float>() - 1.0e-4f);
    projection = clipspace::makePerspectiveProjection(
        fieldOfView, std::max(aspect, 1.0e-4f), nearPlane, farPlane, projectionConvention);
  }

  const glm::mat4 view = glm::inverse(*worldTransform);
  if(!isFiniteMatrix(view) || !isFiniteMatrix(projection)) {
    return false;
  }

  populateCameraUniforms(view, projection, glm::vec3((*worldTransform)[3]), uniforms);
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
