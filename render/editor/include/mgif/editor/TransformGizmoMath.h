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

#pragma once

#include <glm/glm.hpp>

#include <cstdint>
#include <optional>

namespace demo::editor {

inline constexpr float kTransformGizmoParallelEpsilon = 1.0e-6f;
inline constexpr float kTransformGizmoScaleSensitivity = 0.005f;
inline constexpr float kTransformGizmoMinimumScaleFactor = 0.01f;

enum class TransformGizmoComponent : std::uint8_t {
  None = 0,
  TranslateX,
  TranslateY,
  TranslateZ,
  TranslateXY,
  TranslateXZ,
  TranslateYZ,
  RotateX,
  RotateY,
  RotateZ,
  ScaleX,
  ScaleY,
  ScaleZ,
  ScaleUniform,
};

enum class TransformGizmoHitKind : std::uint8_t {
  None = 0,
  Axis,
  Plane,
  Ring,
  UniformScale,
};

enum class TransformGizmoVisibility : std::uint8_t {
  None = 0,
  Translation = 1u << 0u,
  Rotation = 1u << 1u,
  Scale = 1u << 2u,
  All = (1u << 0u) | (1u << 1u) | (1u << 2u),
};

[[nodiscard]] constexpr TransformGizmoVisibility
operator|(TransformGizmoVisibility left,
          TransformGizmoVisibility right) noexcept {
  return static_cast<TransformGizmoVisibility>(
      static_cast<std::uint8_t>(left) | static_cast<std::uint8_t>(right));
}

[[nodiscard]] constexpr bool
hasTransformGizmoVisibility(TransformGizmoVisibility value,
                            TransformGizmoVisibility flag) noexcept {
  return (static_cast<std::uint8_t>(value) &
          static_cast<std::uint8_t>(flag)) != 0u;
}

struct TransformGizmoRay final {
  glm::vec3 origin{0.0f};
  glm::vec3 direction{0.0f, 0.0f, -1.0f};
};

struct TransformGizmoRayPlaneHit final {
  float distance{0.0f};
  glm::vec3 position{0.0f};
};

// ParentWorld supplies the selected node's parent basis. The pivot is already
// in world space, so parent translation is intentionally not used for deltas.
struct TransformGizmoFrame final {
  glm::vec3 pivotWorld{0.0f};
  glm::mat4 parentWorld{1.0f};
};

struct TransformGizmoTranslationDelta final {
  glm::vec3 local{0.0f};
  glm::vec3 world{0.0f};
};

struct TransformGizmoScaleDelta final {
  float factor{1.0f};
  glm::vec3 value{1.0f};
  glm::vec3 delta{0.0f};
};

// Values mirror the reference visual layout after its CPU pick padding.
struct TransformGizmoHitStyle final {
  float translationAxisLength{1.25f};
  float translationAxisRadius{0.11f};
  float translationPlaneOffset{0.5f};
  float translationPlaneSize{0.22f};
  float translationPlaneHalfThickness{0.01f};
  float rotationRingRadius{1.3f};
  float rotationRingTolerance{0.2f};
  float scaleAxisLength{1.67f};
  float scaleAxisHalfExtent{0.125f};
  float uniformScaleHalfExtent{0.2f};
};

struct TransformGizmoHit final {
  TransformGizmoComponent component{TransformGizmoComponent::None};
  TransformGizmoHitKind kind{TransformGizmoHitKind::None};
  float rayDistance{0.0f};
  float missDistance{0.0f};

  [[nodiscard]] constexpr bool hit() const noexcept {
    return component != TransformGizmoComponent::None;
  }
};

// Mouse coordinates use the reference viewport convention: origin at the top
// left, NDC Z in [0, 1], and projection Y already configured by the caller.
[[nodiscard]] std::optional<TransformGizmoRay> makeTransformGizmoRay(
    const glm::vec2 &mousePosition, const glm::mat4 &viewMatrix,
    const glm::mat4 &projectionMatrix,
    const glm::vec2 &viewportExtent) noexcept;

// The returned distance is measured along a normalized ray direction.
[[nodiscard]] std::optional<TransformGizmoRayPlaneHit>
intersectTransformGizmoRayPlane(
    const TransformGizmoRay &ray, const glm::vec3 &planePoint,
    const glm::vec3 &planeNormal,
    float parallelEpsilon = kTransformGizmoParallelEpsilon) noexcept;

// Axis components return their canonical local axis. Plane components return
// the plane normal; components without an axis return zero.
[[nodiscard]] glm::vec3
transformGizmoComponentAxis(TransformGizmoComponent component) noexcept;

[[nodiscard]] std::optional<TransformGizmoTranslationDelta>
computeTransformGizmoTranslationDelta(
    TransformGizmoComponent component,
    const TransformGizmoRay &startRay,
    const TransformGizmoRay &currentRay,
    const glm::vec3 &viewDirectionWorld,
    const TransformGizmoFrame &frame = {}) noexcept;

[[nodiscard]] std::optional<float> computeTransformGizmoRotationDeltaDegrees(
    TransformGizmoComponent component,
    const TransformGizmoRay &startRay,
    const TransformGizmoRay &currentRay,
    const TransformGizmoFrame &frame = {}) noexcept;

[[nodiscard]] std::optional<TransformGizmoScaleDelta>
computeTransformGizmoScaleDelta(
    TransformGizmoComponent component, const glm::vec3 &startScale,
    const glm::vec2 &startMousePosition,
    const glm::vec2 &currentMousePosition,
    float sensitivity = kTransformGizmoScaleSensitivity,
    float minimumFactor = kTransformGizmoMinimumScaleFactor) noexcept;

// std::round semantics are intentional: exact half steps round away from zero,
// matching the locked reference snap behavior.
[[nodiscard]] std::optional<float>
snapTransformGizmoValue(float value, float step) noexcept;
[[nodiscard]] std::optional<glm::vec3>
snapTransformGizmoVector(const glm::vec3 &value, float step) noexcept;

// The ray must be expressed in unscaled gizmo-local coordinates. Invalid rays
// or style extents classify as a deterministic miss.
[[nodiscard]] TransformGizmoHit classifyTransformGizmoHit(
    const TransformGizmoRay &localRay,
    const TransformGizmoHitStyle &style = {},
    TransformGizmoVisibility visibility =
        TransformGizmoVisibility::All) noexcept;

} // namespace demo::editor
