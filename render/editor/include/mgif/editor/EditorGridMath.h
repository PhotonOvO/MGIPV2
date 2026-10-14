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

#include <array>
#include <cstddef>
#include <cstdint>
#include <optional>

namespace demo::editor {

inline constexpr float kEditorGridMinimumUnit = 0.001f;
inline constexpr float kEditorGridMaximumUnit = 1000.0f;
inline constexpr std::uint32_t kEditorGridMinimumLineCount = 11u;
inline constexpr std::uint32_t kEditorGridMaximumLineCount = 501u;
inline constexpr std::size_t kEditorGridLevelCount = 4u;
inline constexpr std::size_t kEditorGridVisibleLevelCount = 3u;

enum class EditorGridProjection : std::uint8_t {
  Perspective = 0,
  Orthographic = 1,
};

struct EditorGridMathInput final {
  glm::vec3 cameraPosition{0.0f};

  // This is the camera's world-space +Z/backward basis, matching the fixed
  // reference implementation's inverse-view column and center formula.
  glm::vec3 cameraBackward{0.0f, 0.0f, 1.0f};
  EditorGridProjection projection{EditorGridProjection::Perspective};
  float baseUnit{1.0f};
  std::uint32_t lineCount{151u};
};

struct EditorGridParameters final {
  float baseUnit{1.0f};
  float distanceToFloor{0.001f};
  float level{0.0f};
  float levelFraction{0.0f};
  glm::vec2 center{0.0f};
  glm::vec2 clipExtent{0.0f};
  std::array<float, kEditorGridLevelCount> steps{};
  std::array<glm::vec2, kEditorGridLevelCount> snappedCenters{};
  std::array<std::uint32_t, kEditorGridVisibleLevelCount>
      visibleLevelIndices{};
  std::array<float, kEditorGridVisibleLevelCount> visibleSteps{};
  std::array<glm::vec2, kEditorGridVisibleLevelCount>
      visibleCenters{};
  std::uint32_t coarsestLevelIndex{0u};
  std::uint32_t lineCount{0u};
};

// Finite base units are clamped to the locked [0.001, 1000] UI range. Line
// extents must remain odd and within the reference's [11, 501] range.
[[nodiscard]] std::optional<EditorGridParameters>
computeEditorGridParameters(const EditorGridMathInput &input) noexcept;

} // namespace demo::editor
