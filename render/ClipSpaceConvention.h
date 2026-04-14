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

constexpr bool usesReverseZ(const ProjectionConvention& convention)
{
  return convention.ndcNearZ > convention.ndcFarZ;
}

constexpr ProjectionConvention getProjectionConvention(BackendConvention backend)
{
  switch(backend)
  {
    case BackendConvention::vulkan:
      return ProjectionConvention{.flipY = true, .ndcNearZ = 1.0f, .ndcFarZ = 0.0f};
    case BackendConvention::d3d12:
    case BackendConvention::metal:
      return ProjectionConvention{.flipY = false, .ndcNearZ = 1.0f, .ndcFarZ = 0.0f};
    default:
      return ProjectionConvention{.flipY = true, .ndcNearZ = 1.0f, .ndcFarZ = 0.0f};
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
  const glm::mat4 projection = usesReverseZ(convention)
      ? glm::perspective(fovRadians, aspectRatio, farPlane, nearPlane)
      : glm::perspective(fovRadians, aspectRatio, nearPlane, farPlane);
  return applyProjectionConvention(projection, convention);
}

inline glm::mat4 makeOrthographicProjection(float left,
                                            float right,
                                            float bottom,
                                            float top,
                                            float nearPlane,
                                            float farPlane,
                                            const ProjectionConvention& convention)
{
  const glm::mat4 projection = usesReverseZ(convention)
      ? glm::ortho(left, right, bottom, top, farPlane, nearPlane)
      : glm::ortho(left, right, bottom, top, nearPlane, farPlane);
  return applyProjectionConvention(projection, convention);
}

inline float extractPerspectiveNearPlane(const glm::mat4& projection, const ProjectionConvention& convention)
{
  const float a = projection[2][2];
  const float b = projection[3][2];
  if(usesReverseZ(convention))
  {
    return std::abs(a + 1.0f) > 1e-5f ? b / (a + 1.0f) : 0.1f;
  }
  return std::abs(a) > 1e-5f ? b / a : 0.1f;
}

inline float extractPerspectiveFarPlane(const glm::mat4& projection, const ProjectionConvention& convention)
{
  const float a = projection[2][2];
  const float b = projection[3][2];
  if(usesReverseZ(convention))
  {
    return std::abs(a) > 1e-5f ? b / a : 100.0f;
  }
  return std::abs(a + 1.0f) > 1e-5f ? b / (a + 1.0f) : 100.0f;
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
