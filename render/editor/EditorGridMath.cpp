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

#include "include/mgif/editor/EditorGridMath.h"

#include <algorithm>
#include <cmath>
#include <limits>

namespace demo::editor {
namespace {

constexpr double kGridDirectionEpsilon = 1.0e-12;
constexpr double kPerspectiveFloorEpsilon = 1.0e-6;
constexpr double kMaximumGridLevel = 3.999;

[[nodiscard]] bool finite(float value) noexcept {
  return std::isfinite(value);
}

[[nodiscard]] bool finite(const glm::vec3 &value) noexcept {
  return finite(value.x) && finite(value.y) && finite(value.z);
}

[[nodiscard]] std::optional<float> checkedFloat(double value) noexcept {
  constexpr double kFloatMaximum =
      static_cast<double>(std::numeric_limits<float>::max());
  if (!std::isfinite(value) || value < -kFloatMaximum || value > kFloatMaximum) {
    return std::nullopt;
  }
  const float converted = static_cast<float>(value);
  return finite(converted) ? std::optional<float>(converted) : std::nullopt;
}

[[nodiscard]] std::optional<glm::vec2>
checkedVector(const glm::dvec2 &value) noexcept {
  const auto x = checkedFloat(value.x);
  const auto y = checkedFloat(value.y);
  if (!x || !y) {
    return std::nullopt;
  }
  return glm::vec2(*x, *y);
}

[[nodiscard]] std::optional<glm::dvec3>
normalized(const glm::vec3 &value) noexcept {
  if (!finite(value)) {
    return std::nullopt;
  }
  const glm::dvec3 wide(value);
  const double largest = std::max(
      {std::abs(wide.x), std::abs(wide.y), std::abs(wide.z)});
  if (!std::isfinite(largest) || largest <= kGridDirectionEpsilon) {
    return std::nullopt;
  }
  const glm::dvec3 scaled = wide / largest;
  const double lengthSquared = glm::dot(scaled, scaled);
  if (!std::isfinite(lengthSquared) ||
      lengthSquared <= kGridDirectionEpsilon * kGridDirectionEpsilon) {
    return std::nullopt;
  }
  return scaled / std::sqrt(lengthSquared);
}

[[nodiscard]] std::optional<glm::vec2>
snapCenter(const glm::vec2 &center, float step) noexcept {
  if (!finite(center.x) || !finite(center.y) || !finite(step) || step <= 0.0f) {
    return std::nullopt;
  }
  const glm::dvec2 quotient =
      glm::dvec2(center) / static_cast<double>(step);
  return checkedVector(glm::dvec2(std::round(quotient.x),
                                  std::round(quotient.y)) *
                       static_cast<double>(step));
}

} // namespace

std::optional<EditorGridParameters>
computeEditorGridParameters(const EditorGridMathInput &input) noexcept {
  if (!finite(input.cameraPosition) || !finite(input.baseUnit) ||
      input.lineCount < kEditorGridMinimumLineCount ||
      input.lineCount > kEditorGridMaximumLineCount ||
      (input.lineCount & 1u) == 0u) {
    return std::nullopt;
  }
  const auto cameraBackward = normalized(input.cameraBackward);
  if (!cameraBackward) {
    return std::nullopt;
  }

  const double baseUnit = std::clamp(static_cast<double>(input.baseUnit),
                                     static_cast<double>(kEditorGridMinimumUnit),
                                     static_cast<double>(kEditorGridMaximumUnit));
  const double heightAboveFloor =
      std::abs(static_cast<double>(input.cameraPosition.y));
  double distanceToFloor = heightAboveFloor;
  if (input.projection == EditorGridProjection::Perspective) {
    const double vertical = std::abs(cameraBackward->y);
    const double rayDistance =
        vertical >= kPerspectiveFloorEpsilon
            ? std::abs(static_cast<double>(input.cameraPosition.y) /
                       cameraBackward->y)
            : heightAboveFloor;
    const double blend = 1.0 - std::min(1.0, vertical);
    distanceToFloor =
        (1.0 - blend) * rayDistance + blend * heightAboveFloor;
  } else if (input.projection != EditorGridProjection::Orthographic) {
    return std::nullopt;
  }
  distanceToFloor =
      std::max(static_cast<double>(kEditorGridMinimumUnit), distanceToFloor);
  if (!std::isfinite(distanceToFloor)) {
    return std::nullopt;
  }

  const double level = std::clamp(
      std::log10(std::max(distanceToFloor / baseUnit, 0.001)), 0.0,
      kMaximumGridLevel);
  if (!std::isfinite(level)) {
    return std::nullopt;
  }

  glm::dvec2 center;
  if (input.projection == EditorGridProjection::Perspective) {
    const glm::dvec3 floorPoint =
        glm::dvec3(input.cameraPosition) - *cameraBackward * distanceToFloor;
    center = glm::dvec2(floorPoint.x, floorPoint.z);
  } else {
    center = glm::dvec2(input.cameraPosition.x, input.cameraPosition.z);
  }
  const auto checkedCenter = checkedVector(center);
  if (!checkedCenter) {
    return std::nullopt;
  }

  EditorGridParameters result;
  const auto checkedBaseUnit = checkedFloat(baseUnit);
  const auto checkedDistance = checkedFloat(distanceToFloor);
  const auto checkedLevel = checkedFloat(level);
  const auto checkedFraction = checkedFloat(level - std::floor(level));
  if (!checkedBaseUnit || !checkedDistance || !checkedLevel ||
      !checkedFraction) {
    return std::nullopt;
  }
  result.baseUnit = *checkedBaseUnit;
  result.distanceToFloor = *checkedDistance;
  result.level = *checkedLevel;
  result.levelFraction = *checkedFraction;
  result.center = *checkedCenter;
  result.lineCount = input.lineCount;

  double decade = 1.0;
  for (std::size_t index = 0u; index < kEditorGridLevelCount; ++index) {
    const auto step = checkedFloat(baseUnit * decade);
    if (!step) {
      return std::nullopt;
    }
    result.steps[index] = *step;
    const auto snapped = snapCenter(result.center, *step);
    if (!snapped) {
      return std::nullopt;
    }
    result.snappedCenters[index] = *snapped;
    decade *= 10.0;
  }

  const std::uint32_t levelBase =
      static_cast<std::uint32_t>(std::floor(level));
  result.coarsestLevelIndex = std::min(levelBase + 1u, 3u);
  for (std::size_t drawLevel = 0u;
       drawLevel < kEditorGridVisibleLevelCount; ++drawLevel) {
    const int candidate = static_cast<int>(levelBase) + 1 -
                          static_cast<int>(drawLevel);
    const std::uint32_t actualLevel =
        static_cast<std::uint32_t>(std::clamp(candidate, 0, 3));
    result.visibleLevelIndices[drawLevel] = actualLevel;
    result.visibleSteps[drawLevel] = result.steps[actualLevel];
    result.visibleCenters[drawLevel] = result.snappedCenters[actualLevel];
  }

  const double halfLineCount =
      static_cast<double>(input.lineCount / 2u);
  const auto clipExtent = checkedFloat(
      static_cast<double>(result.steps[result.coarsestLevelIndex]) *
      halfLineCount);
  if (!clipExtent) {
    return std::nullopt;
  }
  result.clipExtent = glm::vec2(*clipExtent);
  return result;
}

} // namespace demo::editor
