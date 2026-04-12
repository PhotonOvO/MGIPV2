#pragma once

#ifndef GLM_FORCE_DEPTH_ZERO_TO_ONE
#define GLM_FORCE_DEPTH_ZERO_TO_ONE
#endif

#include <glm/glm.hpp>
#include <glm/ext/matrix_clip_space.hpp>

namespace demo::clipspace {

enum class BackendConvention
{
  vulkan,
  d3d12,
  metal,
};

struct ProjectionConvention
{
  bool  flipY{false};
  float ndcNearZ{0.0f};
  float ndcFarZ{1.0f};
};

constexpr ProjectionConvention getProjectionConvention(BackendConvention backend)
{
  switch(backend)
  {
    case BackendConvention::vulkan:
      return ProjectionConvention{.flipY = true, .ndcNearZ = 0.0f, .ndcFarZ = 1.0f};
    case BackendConvention::d3d12:
    case BackendConvention::metal:
      return ProjectionConvention{.flipY = false, .ndcNearZ = 0.0f, .ndcFarZ = 1.0f};
    default:
      return ProjectionConvention{.flipY = true, .ndcNearZ = 0.0f, .ndcFarZ = 1.0f};
  }
}

inline glm::mat4 applyProjectionConvention(glm::mat4 projection, const ProjectionConvention& convention)
{
  if(convention.flipY)
  {
    projection[1][1] *= -1.0f;
  }
  return projection;
}

inline glm::mat4 makePerspectiveProjection(float fovRadians,
                                           float aspectRatio,
                                           float nearPlane,
                                           float farPlane,
                                           const ProjectionConvention& convention)
{
  return applyProjectionConvention(glm::perspective(fovRadians, aspectRatio, nearPlane, farPlane), convention);
}

inline glm::mat4 makeOrthographicProjection(float left,
                                            float right,
                                            float bottom,
                                            float top,
                                            float nearPlane,
                                            float farPlane,
                                            const ProjectionConvention& convention)
{
  return applyProjectionConvention(glm::ortho(left, right, bottom, top, nearPlane, farPlane), convention);
}

inline glm::mat4 makeNdcToShadowTextureMatrix(const ProjectionConvention& convention)
{
  glm::mat4 matrix(1.0f);
  matrix[0][0] = 0.5f;
  matrix[1][1] = 0.5f;
  matrix[3][0] = 0.5f;
  matrix[3][1] = 0.5f;

  if(convention.ndcNearZ <= -0.5f)
  {
    matrix[2][2] = 0.5f;
    matrix[3][2] = 0.5f;
  }

  return matrix;
}

}  // namespace demo::clipspace
