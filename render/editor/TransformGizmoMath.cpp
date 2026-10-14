/*
 * Copyright (c) 2023-2026, NVIDIA CORPORATION. All rights reserved.
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *     http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 *
 * SPDX-FileCopyrightText: Copyright (c) 2023-2026, NVIDIA CORPORATION.
 * SPDX-License-Identifier: Apache-2.0
 */

#include "include/mgif/editor/TransformGizmoMath.h"

#include <glm/gtc/matrix_inverse.hpp>

#include <algorithm>
#include <array>
#include <cmath>
#include <limits>

namespace demo::editor {
namespace {

constexpr double kNormalizationEpsilon = 1.0e-12;
constexpr double kMatrixDeterminantEpsilon = 1.0e-12;
constexpr double kAffineEpsilon = 1.0e-5;
constexpr double kRadiansToDegrees =
    57.295779513082320876798154814105;

[[nodiscard]] bool finite(float value) noexcept {
  return std::isfinite(value);
}

[[nodiscard]] bool finite(const glm::vec2 &value) noexcept {
  return finite(value.x) && finite(value.y);
}

[[nodiscard]] bool finite(const glm::vec3 &value) noexcept {
  return finite(value.x) && finite(value.y) && finite(value.z);
}

[[nodiscard]] bool finite(const glm::mat4 &value) noexcept {
  for (glm::length_t column = 0; column < 4; ++column) {
    for (glm::length_t row = 0; row < 4; ++row) {
      if (!finite(value[column][row])) {
        return false;
      }
    }
  }
  return true;
}

[[nodiscard]] std::optional<float> checkedFloat(double value) noexcept {
  constexpr double kFloatMaximum =
      static_cast<double>(std::numeric_limits<float>::max());
  if (!std::isfinite(value) || value < -kFloatMaximum || value > kFloatMaximum) {
    return std::nullopt;
  }
  const float converted = static_cast<float>(value);
  if (!finite(converted)) {
    return std::nullopt;
  }
  return converted;
}

[[nodiscard]] std::optional<glm::vec3>
checkedVector(const glm::dvec3 &value) noexcept {
  const auto x = checkedFloat(value.x);
  const auto y = checkedFloat(value.y);
  const auto z = checkedFloat(value.z);
  if (!x || !y || !z) {
    return std::nullopt;
  }
  return glm::vec3(*x, *y, *z);
}

[[nodiscard]] std::optional<glm::dvec3>
normalized(const glm::vec3 &value) noexcept {
  if (!finite(value)) {
    return std::nullopt;
  }

  const glm::dvec3 wide(value);
  const double largest = std::max(
      {std::abs(wide.x), std::abs(wide.y), std::abs(wide.z)});
  if (!std::isfinite(largest) || largest <= kNormalizationEpsilon) {
    return std::nullopt;
  }

  const glm::dvec3 scaled = wide / largest;
  const double lengthSquared = glm::dot(scaled, scaled);
  if (!std::isfinite(lengthSquared) ||
      lengthSquared <= kNormalizationEpsilon * kNormalizationEpsilon) {
    return std::nullopt;
  }

  const glm::dvec3 result = scaled / std::sqrt(lengthSquared);
  if (!std::isfinite(result.x) || !std::isfinite(result.y) ||
      !std::isfinite(result.z)) {
    return std::nullopt;
  }
  return result;
}

[[nodiscard]] bool validAffine(const glm::mat4 &matrix) noexcept {
  return finite(matrix) && std::abs(static_cast<double>(matrix[0][3])) <= kAffineEpsilon &&
         std::abs(static_cast<double>(matrix[1][3])) <= kAffineEpsilon &&
         std::abs(static_cast<double>(matrix[2][3])) <= kAffineEpsilon &&
         std::abs(static_cast<double>(matrix[3][3]) - 1.0) <= kAffineEpsilon;
}

[[nodiscard]] std::optional<glm::dmat3>
inverseParentLinear(const glm::mat4 &parentWorld) noexcept {
  if (!validAffine(parentWorld)) {
    return std::nullopt;
  }

  const glm::dmat3 linear(parentWorld);
  double largest = 0.0;
  for (glm::length_t column = 0; column < 3; ++column) {
    for (glm::length_t row = 0; row < 3; ++row) {
      largest = std::max(largest, std::abs(linear[column][row]));
    }
  }
  if (!std::isfinite(largest) || largest <= kNormalizationEpsilon) {
    return std::nullopt;
  }

  const glm::dmat3 scaled = linear / largest;
  const double determinant = glm::determinant(scaled);
  if (!std::isfinite(determinant) ||
      std::abs(determinant) <= kMatrixDeterminantEpsilon) {
    return std::nullopt;
  }

  const glm::dmat3 inverse = glm::inverse(scaled) / largest;
  for (glm::length_t column = 0; column < 3; ++column) {
    for (glm::length_t row = 0; row < 3; ++row) {
      if (!std::isfinite(inverse[column][row])) {
        return std::nullopt;
      }
    }
  }
  return inverse;
}

[[nodiscard]] std::optional<glm::dmat4>
inverseMatrix(const glm::mat4 &matrix) noexcept {
  if (!finite(matrix)) {
    return std::nullopt;
  }
  const glm::dmat4 wide(matrix);
  const double determinant = glm::determinant(wide);
  if (!std::isfinite(determinant) ||
      std::abs(determinant) <= std::numeric_limits<double>::epsilon()) {
    return std::nullopt;
  }
  const glm::dmat4 inverse = glm::inverse(wide);
  for (glm::length_t column = 0; column < 4; ++column) {
    for (glm::length_t row = 0; row < 4; ++row) {
      if (!std::isfinite(inverse[column][row])) {
        return std::nullopt;
      }
    }
  }
  return inverse;
}

[[nodiscard]] std::optional<glm::vec3>
axisInWorld(const glm::vec3 &localAxis,
            const TransformGizmoFrame &frame) noexcept {
  if (!finite(frame.pivotWorld) || !inverseParentLinear(frame.parentWorld)) {
    return std::nullopt;
  }
  const glm::dvec3 transformed =
      glm::dmat3(frame.parentWorld) * glm::dvec3(localAxis);
  const auto direction = normalized(glm::vec3(transformed));
  if (!direction) {
    return std::nullopt;
  }
  return checkedVector(*direction);
}

[[nodiscard]] std::optional<glm::vec3>
worldDeltaToLocal(const glm::vec3 &worldDelta,
                  const TransformGizmoFrame &frame) noexcept {
  if (!finite(worldDelta)) {
    return std::nullopt;
  }
  const auto inverse = inverseParentLinear(frame.parentWorld);
  if (!inverse) {
    return std::nullopt;
  }
  return checkedVector(*inverse * glm::dvec3(worldDelta));
}

[[nodiscard]] bool isTranslationAxis(TransformGizmoComponent component) noexcept {
  return component == TransformGizmoComponent::TranslateX ||
         component == TransformGizmoComponent::TranslateY ||
         component == TransformGizmoComponent::TranslateZ;
}

[[nodiscard]] bool isTranslationPlane(TransformGizmoComponent component) noexcept {
  return component == TransformGizmoComponent::TranslateXY ||
         component == TransformGizmoComponent::TranslateXZ ||
         component == TransformGizmoComponent::TranslateYZ;
}

[[nodiscard]] bool isRotation(TransformGizmoComponent component) noexcept {
  return component == TransformGizmoComponent::RotateX ||
         component == TransformGizmoComponent::RotateY ||
         component == TransformGizmoComponent::RotateZ;
}

[[nodiscard]] bool isScaleAxis(TransformGizmoComponent component) noexcept {
  return component == TransformGizmoComponent::ScaleX ||
         component == TransformGizmoComponent::ScaleY ||
         component == TransformGizmoComponent::ScaleZ;
}

[[nodiscard]] glm::length_t
componentAxisIndex(TransformGizmoComponent component) noexcept {
  switch (component) {
  case TransformGizmoComponent::TranslateX:
  case TransformGizmoComponent::RotateX:
  case TransformGizmoComponent::ScaleX:
  case TransformGizmoComponent::TranslateYZ:
    return 0;
  case TransformGizmoComponent::TranslateY:
  case TransformGizmoComponent::RotateY:
  case TransformGizmoComponent::ScaleY:
  case TransformGizmoComponent::TranslateXZ:
    return 1;
  default:
    return 2;
  }
}

struct RayBoxHit final {
  double nearDistance{0.0};
  double farDistance{0.0};
};

[[nodiscard]] std::optional<RayBoxHit>
intersectRayBox(const TransformGizmoRay &ray, const glm::vec3 &boundsMin,
                const glm::vec3 &boundsMax) noexcept {
  if (!finite(ray.origin) || !finite(boundsMin) || !finite(boundsMax) ||
      boundsMin.x > boundsMax.x || boundsMin.y > boundsMax.y ||
      boundsMin.z > boundsMax.z) {
    return std::nullopt;
  }
  const auto direction = normalized(ray.direction);
  if (!direction) {
    return std::nullopt;
  }

  const glm::dvec3 origin(ray.origin);
  const glm::dvec3 minimum(boundsMin);
  const glm::dvec3 maximum(boundsMax);
  double nearDistance = 0.0;
  double farDistance = std::numeric_limits<double>::infinity();
  for (glm::length_t axis = 0; axis < 3; ++axis) {
    if (std::abs((*direction)[axis]) <= kNormalizationEpsilon) {
      if (origin[axis] < minimum[axis] || origin[axis] > maximum[axis]) {
        return std::nullopt;
      }
      continue;
    }

    double first = (minimum[axis] - origin[axis]) / (*direction)[axis];
    double second = (maximum[axis] - origin[axis]) / (*direction)[axis];
    if (first > second) {
      std::swap(first, second);
    }
    nearDistance = std::max(nearDistance, first);
    farDistance = std::min(farDistance, second);
    if (farDistance < nearDistance) {
      return std::nullopt;
    }
  }

  if (!std::isfinite(nearDistance) || farDistance < 0.0) {
    return std::nullopt;
  }
  return RayBoxHit{nearDistance, farDistance};
}

[[nodiscard]] std::optional<double>
rayDistanceToBox(const TransformGizmoRay &ray, const glm::vec3 &boundsMin,
                 const glm::vec3 &boundsMax) noexcept {
  const auto direction = normalized(ray.direction);
  if (!direction || !finite(ray.origin) || !finite(boundsMin) ||
      !finite(boundsMax)) {
    return std::nullopt;
  }
  const glm::dvec3 center =
      (glm::dvec3(boundsMin) + glm::dvec3(boundsMax)) * 0.5;
  const glm::dvec3 origin(ray.origin);
  const double rayDistance =
      std::max(0.0, glm::dot(center - origin, *direction));
  const glm::dvec3 closestOnRay = origin + *direction * rayDistance;
  const glm::dvec3 closestOnBox =
      glm::clamp(closestOnRay, glm::dvec3(boundsMin), glm::dvec3(boundsMax));
  const double distance = glm::length(closestOnRay - closestOnBox);
  return std::isfinite(distance) ? std::optional<double>(distance)
                                 : std::nullopt;
}

[[nodiscard]] bool validHitStyle(const TransformGizmoHitStyle &style) noexcept {
  const std::array values{
      style.translationAxisLength,
      style.translationAxisRadius,
      style.translationPlaneOffset,
      style.translationPlaneSize,
      style.translationPlaneHalfThickness,
      style.rotationRingRadius,
      style.rotationRingTolerance,
      style.scaleAxisLength,
      style.scaleAxisHalfExtent,
      style.uniformScaleHalfExtent,
  };
  return std::all_of(values.begin(), values.end(), [](float value) {
    return finite(value) && value > 0.0f;
  });
}

[[nodiscard]] TransformGizmoVisibility
visibilityFor(TransformGizmoComponent component) noexcept {
  if (isTranslationAxis(component) || isTranslationPlane(component)) {
    return TransformGizmoVisibility::Translation;
  }
  if (isRotation(component)) {
    return TransformGizmoVisibility::Rotation;
  }
  if (isScaleAxis(component) ||
      component == TransformGizmoComponent::ScaleUniform) {
    return TransformGizmoVisibility::Scale;
  }
  return TransformGizmoVisibility::None;
}

struct HitCandidate final {
  TransformGizmoComponent component{TransformGizmoComponent::None};
  glm::vec3 boundsMin{0.0f};
  glm::vec3 boundsMax{0.0f};
};

[[nodiscard]] HitCandidate axisCandidate(TransformGizmoComponent component,
                                         float length,
                                         float radius) noexcept {
  glm::vec3 minimum(-radius);
  glm::vec3 maximum(radius);
  maximum[componentAxisIndex(component)] = length;
  return HitCandidate{component, minimum, maximum};
}

[[nodiscard]] HitCandidate planeCandidate(TransformGizmoComponent component,
                                          float offset, float size,
                                          float halfThickness) noexcept {
  const float minimumInPlane = offset - size * 0.5f;
  const float maximumInPlane = offset + size * 0.5f;
  glm::vec3 minimum(minimumInPlane);
  glm::vec3 maximum(maximumInPlane);
  const glm::length_t normalAxis = componentAxisIndex(component);
  minimum[normalAxis] = -halfThickness;
  maximum[normalAxis] = halfThickness;
  return HitCandidate{component, minimum, maximum};
}

[[nodiscard]] HitCandidate ringCandidate(TransformGizmoComponent component,
                                         float radius,
                                         float tolerance) noexcept {
  glm::vec3 minimum(-(radius + tolerance));
  glm::vec3 maximum(radius + tolerance);
  const glm::length_t normalAxis = componentAxisIndex(component);
  minimum[normalAxis] = -tolerance;
  maximum[normalAxis] = tolerance;
  return HitCandidate{component, minimum, maximum};
}

[[nodiscard]] HitCandidate centeredBoxCandidate(
    TransformGizmoComponent component, const glm::vec3 &center,
    float halfExtent) noexcept {
  return HitCandidate{component, center - glm::vec3(halfExtent),
                      center + glm::vec3(halfExtent)};
}

[[nodiscard]] TransformGizmoHitKind
hitKind(TransformGizmoComponent component) noexcept {
  if (isTranslationAxis(component) || isScaleAxis(component)) {
    return TransformGizmoHitKind::Axis;
  }
  if (isTranslationPlane(component)) {
    return TransformGizmoHitKind::Plane;
  }
  if (isRotation(component)) {
    return TransformGizmoHitKind::Ring;
  }
  if (component == TransformGizmoComponent::ScaleUniform) {
    return TransformGizmoHitKind::UniformScale;
  }
  return TransformGizmoHitKind::None;
}

} // namespace

std::optional<TransformGizmoRay> makeTransformGizmoRay(
    const glm::vec2 &mousePosition, const glm::mat4 &viewMatrix,
    const glm::mat4 &projectionMatrix,
    const glm::vec2 &viewportExtent) noexcept {
  if (!finite(mousePosition) || !finite(viewportExtent) ||
      viewportExtent.x <= 0.0f || viewportExtent.y <= 0.0f ||
      !validAffine(viewMatrix)) {
    return std::nullopt;
  }
  const auto inverseView = inverseMatrix(viewMatrix);
  const auto inverseProjection = inverseMatrix(projectionMatrix);
  if (!inverseView || !inverseProjection) {
    return std::nullopt;
  }

  const double ndcX = 2.0 * static_cast<double>(mousePosition.x) /
                          static_cast<double>(viewportExtent.x) -
                      1.0;
  const double ndcY = 2.0 * static_cast<double>(mousePosition.y) /
                          static_cast<double>(viewportExtent.y) -
                      1.0;
  glm::dvec4 startView = *inverseProjection * glm::dvec4(ndcX, ndcY, 0.0, 1.0);
  glm::dvec4 endView = *inverseProjection * glm::dvec4(ndcX, ndcY, 1.0, 1.0);
  if (!std::isfinite(startView.w) || !std::isfinite(endView.w) ||
      std::abs(startView.w) <= kNormalizationEpsilon ||
      std::abs(endView.w) <= kNormalizationEpsilon) {
    return std::nullopt;
  }
  startView /= startView.w;
  endView /= endView.w;

  const glm::dvec4 startWorld = *inverseView * startView;
  const glm::dvec4 endWorld = *inverseView * endView;
  const auto origin = checkedVector(glm::dvec3(startWorld));
  const auto rawDirection = checkedVector(glm::dvec3(endWorld - startWorld));
  if (!origin || !rawDirection) {
    return std::nullopt;
  }
  const auto direction = normalized(*rawDirection);
  if (!direction) {
    return std::nullopt;
  }
  const auto checkedDirection = checkedVector(*direction);
  if (!checkedDirection) {
    return std::nullopt;
  }
  return TransformGizmoRay{*origin, *checkedDirection};
}

std::optional<TransformGizmoRayPlaneHit> intersectTransformGizmoRayPlane(
    const TransformGizmoRay &ray, const glm::vec3 &planePoint,
    const glm::vec3 &planeNormal, float parallelEpsilon) noexcept {
  if (!finite(ray.origin) || !finite(planePoint) || !finite(parallelEpsilon) ||
      parallelEpsilon <= 0.0f) {
    return std::nullopt;
  }
  const auto rayDirection = normalized(ray.direction);
  const auto normal = normalized(planeNormal);
  if (!rayDirection || !normal) {
    return std::nullopt;
  }

  const double denominator = glm::dot(*rayDirection, *normal);
  if (!std::isfinite(denominator) ||
      std::abs(denominator) < static_cast<double>(parallelEpsilon)) {
    return std::nullopt;
  }
  const double distance =
      glm::dot(glm::dvec3(planePoint) - glm::dvec3(ray.origin), *normal) /
      denominator;
  if (!std::isfinite(distance) || distance < 0.0) {
    return std::nullopt;
  }

  const auto checkedDistance = checkedFloat(distance);
  const auto position =
      checkedVector(glm::dvec3(ray.origin) + *rayDirection * distance);
  if (!checkedDistance || !position) {
    return std::nullopt;
  }
  return TransformGizmoRayPlaneHit{*checkedDistance, *position};
}

glm::vec3
transformGizmoComponentAxis(TransformGizmoComponent component) noexcept {
  switch (component) {
  case TransformGizmoComponent::TranslateX:
  case TransformGizmoComponent::RotateX:
  case TransformGizmoComponent::ScaleX:
  case TransformGizmoComponent::TranslateYZ:
    return glm::vec3(1.0f, 0.0f, 0.0f);
  case TransformGizmoComponent::TranslateY:
  case TransformGizmoComponent::RotateY:
  case TransformGizmoComponent::ScaleY:
  case TransformGizmoComponent::TranslateXZ:
    return glm::vec3(0.0f, 1.0f, 0.0f);
  case TransformGizmoComponent::TranslateZ:
  case TransformGizmoComponent::RotateZ:
  case TransformGizmoComponent::ScaleZ:
  case TransformGizmoComponent::TranslateXY:
    return glm::vec3(0.0f, 0.0f, 1.0f);
  default:
    return glm::vec3(0.0f);
  }
}

std::optional<TransformGizmoTranslationDelta>
computeTransformGizmoTranslationDelta(
    TransformGizmoComponent component, const TransformGizmoRay &startRay,
    const TransformGizmoRay &currentRay,
    const glm::vec3 &viewDirectionWorld,
    const TransformGizmoFrame &frame) noexcept {
  if ((!isTranslationAxis(component) && !isTranslationPlane(component)) ||
      !finite(frame.pivotWorld)) {
    return std::nullopt;
  }
  const glm::vec3 localAxis = transformGizmoComponentAxis(component);
  const auto worldAxis = axisInWorld(localAxis, frame);
  if (!worldAxis) {
    return std::nullopt;
  }

  glm::vec3 planeNormal = *worldAxis;
  if (isTranslationAxis(component)) {
    const auto viewDirection = normalized(viewDirectionWorld);
    if (!viewDirection) {
      return std::nullopt;
    }
    const glm::dvec3 projected =
        glm::cross(glm::dvec3(*worldAxis),
                   glm::cross(*viewDirection, glm::dvec3(*worldAxis)));
    const auto projectedFloat = checkedVector(projected);
    if (!projectedFloat) {
      return std::nullopt;
    }
    const auto normalizedPlane = normalized(*projectedFloat);
    if (!normalizedPlane) {
      return std::nullopt;
    }
    const auto checkedPlane = checkedVector(*normalizedPlane);
    if (!checkedPlane) {
      return std::nullopt;
    }
    planeNormal = *checkedPlane;
  }

  const auto startHit = intersectTransformGizmoRayPlane(
      startRay, frame.pivotWorld, planeNormal);
  const auto currentHit = intersectTransformGizmoRayPlane(
      currentRay, frame.pivotWorld, planeNormal);
  if (!startHit || !currentHit) {
    return std::nullopt;
  }

  glm::dvec3 worldDelta =
      glm::dvec3(currentHit->position) - glm::dvec3(startHit->position);
  if (isTranslationAxis(component)) {
    worldDelta = glm::dvec3(*worldAxis) *
                 glm::dot(worldDelta, glm::dvec3(*worldAxis));
  }
  const auto checkedWorldDelta = checkedVector(worldDelta);
  if (!checkedWorldDelta) {
    return std::nullopt;
  }
  const auto localDelta = worldDeltaToLocal(*checkedWorldDelta, frame);
  if (!localDelta) {
    return std::nullopt;
  }
  return TransformGizmoTranslationDelta{*localDelta, *checkedWorldDelta};
}

std::optional<float> computeTransformGizmoRotationDeltaDegrees(
    TransformGizmoComponent component, const TransformGizmoRay &startRay,
    const TransformGizmoRay &currentRay,
    const TransformGizmoFrame &frame) noexcept {
  if (!isRotation(component) || !finite(frame.pivotWorld) ||
      !inverseParentLinear(frame.parentWorld)) {
    return std::nullopt;
  }
  const auto worldAxis =
      axisInWorld(transformGizmoComponentAxis(component), frame);
  if (!worldAxis) {
    return std::nullopt;
  }
  const auto startHit = intersectTransformGizmoRayPlane(
      startRay, frame.pivotWorld, *worldAxis);
  const auto currentHit = intersectTransformGizmoRayPlane(
      currentRay, frame.pivotWorld, *worldAxis);
  if (!startHit || !currentHit) {
    return std::nullopt;
  }

  const glm::vec3 startOffset = startHit->position - frame.pivotWorld;
  const glm::vec3 currentOffset = currentHit->position - frame.pivotWorld;
  const auto startDirection = normalized(startOffset);
  const auto currentDirection = normalized(currentOffset);
  if (!startDirection || !currentDirection) {
    return std::nullopt;
  }

  const double cosine = std::clamp(
      glm::dot(*startDirection, *currentDirection), -1.0, 1.0);
  const double sine = glm::dot(glm::cross(*startDirection, *currentDirection),
                               glm::dvec3(*worldAxis));
  const double degrees = std::atan2(sine, cosine) * kRadiansToDegrees;
  return checkedFloat(degrees);
}

std::optional<TransformGizmoScaleDelta> computeTransformGizmoScaleDelta(
    TransformGizmoComponent component, const glm::vec3 &startScale,
    const glm::vec2 &startMousePosition,
    const glm::vec2 &currentMousePosition, float sensitivity,
    float minimumFactor) noexcept {
  if ((!isScaleAxis(component) &&
       component != TransformGizmoComponent::ScaleUniform) ||
      !finite(startScale) || !finite(startMousePosition) ||
      !finite(currentMousePosition) || !finite(sensitivity) ||
      !finite(minimumFactor) || sensitivity <= 0.0f ||
      minimumFactor <= 0.0f) {
    return std::nullopt;
  }

  const glm::dvec2 mouseDelta =
      glm::dvec2(currentMousePosition) - glm::dvec2(startMousePosition);
  const double factor = std::max(
      static_cast<double>(minimumFactor),
      1.0 + (mouseDelta.x - mouseDelta.y) *
                static_cast<double>(sensitivity));
  const auto checkedFactor = checkedFloat(factor);
  if (!checkedFactor) {
    return std::nullopt;
  }

  glm::dvec3 value(startScale);
  if (component == TransformGizmoComponent::ScaleUniform) {
    value *= factor;
  } else {
    value[componentAxisIndex(component)] *= factor;
  }
  const auto checkedValue = checkedVector(value);
  if (!checkedValue) {
    return std::nullopt;
  }
  const auto delta =
      checkedVector(value - glm::dvec3(startScale));
  if (!delta) {
    return std::nullopt;
  }
  return TransformGizmoScaleDelta{*checkedFactor, *checkedValue, *delta};
}

std::optional<float> snapTransformGizmoValue(float value,
                                              float step) noexcept {
  if (!finite(value) || !finite(step) || step <= 0.0f) {
    return std::nullopt;
  }
  const double quotient =
      static_cast<double>(value) / static_cast<double>(step);
  if (!std::isfinite(quotient)) {
    return std::nullopt;
  }
  return checkedFloat(std::round(quotient) * static_cast<double>(step));
}

std::optional<glm::vec3>
snapTransformGizmoVector(const glm::vec3 &value, float step) noexcept {
  if (!finite(value)) {
    return std::nullopt;
  }
  const auto x = snapTransformGizmoValue(value.x, step);
  const auto y = snapTransformGizmoValue(value.y, step);
  const auto z = snapTransformGizmoValue(value.z, step);
  if (!x || !y || !z) {
    return std::nullopt;
  }
  return glm::vec3(*x, *y, *z);
}

TransformGizmoHit classifyTransformGizmoHit(
    const TransformGizmoRay &localRay, const TransformGizmoHitStyle &style,
    TransformGizmoVisibility visibility) noexcept {
  if (!validHitStyle(style) || !finite(localRay.origin) ||
      !normalized(localRay.direction)) {
    return {};
  }

  const std::array candidates{
      axisCandidate(TransformGizmoComponent::TranslateX,
                    style.translationAxisLength,
                    style.translationAxisRadius),
      axisCandidate(TransformGizmoComponent::TranslateY,
                    style.translationAxisLength,
                    style.translationAxisRadius),
      axisCandidate(TransformGizmoComponent::TranslateZ,
                    style.translationAxisLength,
                    style.translationAxisRadius),
      planeCandidate(TransformGizmoComponent::TranslateXY,
                     style.translationPlaneOffset,
                     style.translationPlaneSize,
                     style.translationPlaneHalfThickness),
      planeCandidate(TransformGizmoComponent::TranslateXZ,
                     style.translationPlaneOffset,
                     style.translationPlaneSize,
                     style.translationPlaneHalfThickness),
      planeCandidate(TransformGizmoComponent::TranslateYZ,
                     style.translationPlaneOffset,
                     style.translationPlaneSize,
                     style.translationPlaneHalfThickness),
      ringCandidate(TransformGizmoComponent::RotateX,
                    style.rotationRingRadius,
                    style.rotationRingTolerance),
      ringCandidate(TransformGizmoComponent::RotateY,
                    style.rotationRingRadius,
                    style.rotationRingTolerance),
      ringCandidate(TransformGizmoComponent::RotateZ,
                    style.rotationRingRadius,
                    style.rotationRingTolerance),
      centeredBoxCandidate(TransformGizmoComponent::ScaleX,
                           glm::vec3(style.scaleAxisLength, 0.0f, 0.0f),
                           style.scaleAxisHalfExtent),
      centeredBoxCandidate(TransformGizmoComponent::ScaleY,
                           glm::vec3(0.0f, style.scaleAxisLength, 0.0f),
                           style.scaleAxisHalfExtent),
      centeredBoxCandidate(TransformGizmoComponent::ScaleZ,
                           glm::vec3(0.0f, 0.0f, style.scaleAxisLength),
                           style.scaleAxisHalfExtent),
      centeredBoxCandidate(TransformGizmoComponent::ScaleUniform,
                           glm::vec3(0.0f),
                           style.uniformScaleHalfExtent),
  };

  TransformGizmoHit best;
  double bestMissDistance = std::numeric_limits<double>::infinity();
  for (const HitCandidate &candidate : candidates) {
    const TransformGizmoVisibility required = visibilityFor(candidate.component);
    if (!hasTransformGizmoVisibility(visibility, required)) {
      continue;
    }
    const auto boxHit =
        intersectRayBox(localRay, candidate.boundsMin, candidate.boundsMax);
    if (!boxHit) {
      continue;
    }

    double rayDistance = boxHit->nearDistance;
    const auto boxMissDistance = rayDistanceToBox(
        localRay, candidate.boundsMin, candidate.boundsMax);
    if (!boxMissDistance) {
      continue;
    }
    double missDistance = *boxMissDistance;
    double pickThreshold = 0.6;
    if (isRotation(candidate.component)) {
      const auto ringHit = intersectTransformGizmoRayPlane(
          localRay, glm::vec3(0.0f),
          transformGizmoComponentAxis(candidate.component));
      if (!ringHit) {
        continue;
      }
      rayDistance = static_cast<double>(ringHit->distance);
      missDistance = std::abs(
          static_cast<double>(glm::length(ringHit->position)) -
          static_cast<double>(style.rotationRingRadius));
      pickThreshold = static_cast<double>(style.rotationRingTolerance);
    } else if (isTranslationPlane(candidate.component)) {
      pickThreshold = 0.2;
    }
    if (!std::isfinite(missDistance) || missDistance >= pickThreshold) {
      continue;
    }

    // Strict comparison preserves the reference's generation-order tie break.
    if (missDistance < bestMissDistance) {
      const auto checkedRayDistance = checkedFloat(rayDistance);
      const auto checkedMiss = checkedFloat(missDistance);
      if (!checkedRayDistance || !checkedMiss) {
        continue;
      }
      best = TransformGizmoHit{candidate.component,
                               hitKind(candidate.component),
                               *checkedRayDistance,
                               *checkedMiss};
      bestMissDistance = missDistance;
    }
  }
  return best;
}

} // namespace demo::editor
