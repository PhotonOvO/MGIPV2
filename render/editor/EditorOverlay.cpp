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

#include "include/mgif/editor/EditorOverlay.h"

#include <glm/gtc/matrix_inverse.hpp>

#include <algorithm>
#include <array>
#include <cmath>
#include <limits>
#include <optional>
#include <utility>

namespace demo::editor {
namespace {

constexpr double kVectorEpsilon = 1.0e-12;
constexpr double kBasisDeterminantEpsilon = 1.0e-10;
constexpr double kAffineEpsilon = 1.0e-5;
constexpr double kViewportRoundingTolerance = 0.51;

constexpr std::array<TransformGizmoComponent, kEditorOverlayGizmoHandleCount>
    kOrderedGizmoComponents{
        TransformGizmoComponent::TranslateX,
        TransformGizmoComponent::TranslateY,
        TransformGizmoComponent::TranslateZ,
        TransformGizmoComponent::TranslateXY,
        TransformGizmoComponent::TranslateXZ,
        TransformGizmoComponent::TranslateYZ,
        TransformGizmoComponent::RotateX,
        TransformGizmoComponent::RotateY,
        TransformGizmoComponent::RotateZ,
        TransformGizmoComponent::ScaleX,
        TransformGizmoComponent::ScaleY,
        TransformGizmoComponent::ScaleZ,
        TransformGizmoComponent::ScaleUniform,
    };

[[nodiscard]] bool finite(float value) noexcept { return std::isfinite(value); }

[[nodiscard]] bool finite(const glm::vec2 &value) noexcept {
  return finite(value.x) && finite(value.y);
}

[[nodiscard]] bool finite(const glm::vec3 &value) noexcept {
  return finite(value.x) && finite(value.y) && finite(value.z);
}

[[nodiscard]] bool finite(const glm::vec4 &value) noexcept {
  return finite(value.x) && finite(value.y) && finite(value.z) &&
         finite(value.w);
}

[[nodiscard]] bool finite(const glm::mat4 &value) noexcept {
  for (glm::length_t column = 0; column < 4; ++column) {
    if (!finite(value[column])) {
      return false;
    }
  }
  return true;
}

[[nodiscard]] std::optional<float> checkedFloat(double value) noexcept {
  constexpr double maximum =
      static_cast<double>(std::numeric_limits<float>::max());
  if (!std::isfinite(value) || value < -maximum || value > maximum) {
    return std::nullopt;
  }
  const float narrowed = static_cast<float>(value);
  return finite(narrowed) ? std::optional<float>(narrowed) : std::nullopt;
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
  const double largest =
      std::max({std::abs(wide.x), std::abs(wide.y), std::abs(wide.z)});
  if (!std::isfinite(largest) || largest <= kVectorEpsilon) {
    return std::nullopt;
  }
  const glm::dvec3 scaled = wide / largest;
  const double lengthSquared = glm::dot(scaled, scaled);
  if (!std::isfinite(lengthSquared) ||
      lengthSquared <= kVectorEpsilon * kVectorEpsilon) {
    return std::nullopt;
  }
  const glm::dvec3 result = scaled / std::sqrt(lengthSquared);
  return std::isfinite(result.x) && std::isfinite(result.y) &&
                 std::isfinite(result.z)
             ? std::optional<glm::dvec3>(result)
             : std::nullopt;
}

[[nodiscard]] bool validProjection(EditorGridProjection projection) noexcept {
  return projection == EditorGridProjection::Perspective ||
         projection == EditorGridProjection::Orthographic;
}

[[nodiscard]] bool
validDepthConvention(EditorOverlayDepthConvention convention) noexcept {
  return convention == EditorOverlayDepthConvention::Forward ||
         convention == EditorOverlayDepthConvention::Reverse;
}

[[nodiscard]] bool validGizmoDepthMode(EditorOverlayDepthMode mode) noexcept {
  return mode == EditorOverlayDepthMode::DepthTested ||
         mode == EditorOverlayDepthMode::XRaySolid ||
         mode == EditorOverlayDepthMode::XRayDithered;
}

[[nodiscard]] bool
validInteractionState(EditorOverlayInteractionState state) noexcept {
  switch (state) {
  case EditorOverlayInteractionState::Normal:
  case EditorOverlayInteractionState::Hovered:
  case EditorOverlayInteractionState::Active:
    return true;
  }
  return false;
}

[[nodiscard]] bool validComponent(TransformGizmoComponent component) noexcept {
  return static_cast<std::uint8_t>(component) <=
         static_cast<std::uint8_t>(TransformGizmoComponent::ScaleUniform);
}

[[nodiscard]] TransformGizmoVisibility
visibilityFor(TransformGizmoComponent component) noexcept {
  switch (component) {
  case TransformGizmoComponent::TranslateX:
  case TransformGizmoComponent::TranslateY:
  case TransformGizmoComponent::TranslateZ:
  case TransformGizmoComponent::TranslateXY:
  case TransformGizmoComponent::TranslateXZ:
  case TransformGizmoComponent::TranslateYZ:
    return TransformGizmoVisibility::Translation;
  case TransformGizmoComponent::RotateX:
  case TransformGizmoComponent::RotateY:
  case TransformGizmoComponent::RotateZ:
    return TransformGizmoVisibility::Rotation;
  case TransformGizmoComponent::ScaleX:
  case TransformGizmoComponent::ScaleY:
  case TransformGizmoComponent::ScaleZ:
  case TransformGizmoComponent::ScaleUniform:
    return TransformGizmoVisibility::Scale;
  case TransformGizmoComponent::None:
    return TransformGizmoVisibility::None;
  }
  return TransformGizmoVisibility::None;
}

[[nodiscard]] bool
validVisibility(TransformGizmoVisibility visibility) noexcept {
  const auto raw = static_cast<std::uint8_t>(visibility);
  const auto all = static_cast<std::uint8_t>(TransformGizmoVisibility::All);
  return (raw & static_cast<std::uint8_t>(~all)) == 0u;
}

[[nodiscard]] bool
componentVisible(TransformGizmoComponent component,
                 TransformGizmoVisibility visibility) noexcept {
  const TransformGizmoVisibility required = visibilityFor(component);
  return required != TransformGizmoVisibility::None &&
         hasTransformGizmoVisibility(visibility, required);
}

[[nodiscard]] bool validRect(const EditorOverlayPixelRect &rect,
                             const EditorOverlayPixelExtent &target) noexcept {
  if (rect.extent.width == 0u || rect.extent.height == 0u ||
      target.width == 0u || target.height == 0u ||
      rect.origin.x > target.width || rect.origin.y > target.height) {
    return false;
  }
  return rect.extent.width <= target.width - rect.origin.x &&
         rect.extent.height <= target.height - rect.origin.y;
}

[[nodiscard]] bool
validViewport(const EditorOverlayViewport &viewport) noexcept {
  if (!finite(viewport.logicalExtent) || viewport.logicalExtent.x <= 0.0f ||
      viewport.logicalExtent.y <= 0.0f || !finite(viewport.framebufferScale) ||
      viewport.framebufferScale.x <= 0.0f ||
      viewport.framebufferScale.y <= 0.0f ||
      !validRect(viewport.physicalRenderArea, viewport.colorTargetExtent) ||
      viewport.sceneDepthExtent.width == 0u ||
      viewport.sceneDepthExtent.height == 0u) {
    return false;
  }

  const double expectedWidth = static_cast<double>(viewport.logicalExtent.x) *
                               static_cast<double>(viewport.framebufferScale.x);
  const double expectedHeight =
      static_cast<double>(viewport.logicalExtent.y) *
      static_cast<double>(viewport.framebufferScale.y);
  if (!std::isfinite(expectedWidth) || !std::isfinite(expectedHeight)) {
    return false;
  }
  return std::abs(
             expectedWidth -
             static_cast<double>(viewport.physicalRenderArea.extent.width)) <=
             kViewportRoundingTolerance &&
         std::abs(
             expectedHeight -
             static_cast<double>(viewport.physicalRenderArea.extent.height)) <=
             kViewportRoundingTolerance;
}

[[nodiscard]] std::optional<glm::mat4>
makeGizmoModel(const TransformGizmoFrame &frame, float worldScale) noexcept {
  if (!finite(frame.pivotWorld) || !finite(frame.parentWorld) ||
      !finite(worldScale) || worldScale <= 0.0f ||
      std::abs(static_cast<double>(frame.parentWorld[0][3])) > kAffineEpsilon ||
      std::abs(static_cast<double>(frame.parentWorld[1][3])) > kAffineEpsilon ||
      std::abs(static_cast<double>(frame.parentWorld[2][3])) > kAffineEpsilon ||
      std::abs(static_cast<double>(frame.parentWorld[3][3]) - 1.0) >
          kAffineEpsilon) {
    return std::nullopt;
  }

  std::array<glm::dvec3, 3u> basis{};
  for (glm::length_t column = 0; column < 3; ++column) {
    const auto direction = normalized(glm::vec3(frame.parentWorld[column]));
    if (!direction) {
      return std::nullopt;
    }
    basis[static_cast<std::size_t>(column)] = *direction;
  }
  const glm::dmat3 wideBasis(basis[0], basis[1], basis[2]);
  const double determinant = glm::determinant(wideBasis);
  if (!std::isfinite(determinant) ||
      std::abs(determinant) <= kBasisDeterminantEpsilon) {
    return std::nullopt;
  }

  glm::mat4 model(1.0f);
  for (glm::length_t column = 0; column < 3; ++column) {
    const auto scaled = checkedVector(basis[static_cast<std::size_t>(column)] *
                                      static_cast<double>(worldScale));
    if (!scaled) {
      return std::nullopt;
    }
    model[column] = glm::vec4(*scaled, 0.0f);
  }
  model[3] = glm::vec4(frame.pivotWorld, 1.0f);
  return finite(model) ? std::optional<glm::mat4>(model) : std::nullopt;
}

[[nodiscard]] std::optional<EditorOverlayGizmoPixelScale>
computePixelScale(const EditorOverlayViewport &viewport,
                  const EditorOverlayGizmoInput &input) noexcept {
  if (!validProjection(input.projection) || !finite(input.viewSpaceDepth) ||
      input.viewSpaceDepth <= 0.0f || !finite(input.projectionScaleY) ||
      std::abs(input.projectionScaleY) <= static_cast<float>(kVectorEpsilon) ||
      !finite(input.nominalLogicalPixels) ||
      input.nominalLogicalPixels <= 0.0f) {
    return std::nullopt;
  }

  const double physicalHeight =
      static_cast<double>(viewport.physicalRenderArea.extent.height);
  const double projectionY =
      std::abs(static_cast<double>(input.projectionScaleY));
  const double numerator = input.projection == EditorGridProjection::Perspective
                               ? 2.0 * static_cast<double>(input.viewSpaceDepth)
                               : 2.0;
  const double unitsPerPhysicalPixel =
      numerator / (projectionY * physicalHeight);
  const double targetPhysicalPixels =
      static_cast<double>(input.nominalLogicalPixels) *
      static_cast<double>(viewport.framebufferScale.y);
  const double worldScale = targetPhysicalPixels * unitsPerPhysicalPixel;

  const auto physical = checkedFloat(targetPhysicalPixels);
  const auto units = checkedFloat(unitsPerPhysicalPixel);
  const auto scale = checkedFloat(worldScale);
  if (!physical || !units || !scale || *physical <= 0.0f || *units <= 0.0f ||
      *scale <= 0.0f) {
    return std::nullopt;
  }
  return EditorOverlayGizmoPixelScale{input.nominalLogicalPixels, *physical,
                                      *units, *scale};
}

[[nodiscard]] EditorOverlayGizmoGeometryKind
geometryFor(TransformGizmoComponent component) noexcept {
  switch (component) {
  case TransformGizmoComponent::TranslateX:
  case TransformGizmoComponent::TranslateY:
  case TransformGizmoComponent::TranslateZ:
    return EditorOverlayGizmoGeometryKind::TranslationAxis;
  case TransformGizmoComponent::TranslateXY:
  case TransformGizmoComponent::TranslateXZ:
  case TransformGizmoComponent::TranslateYZ:
    return EditorOverlayGizmoGeometryKind::TranslationPlane;
  case TransformGizmoComponent::RotateX:
  case TransformGizmoComponent::RotateY:
  case TransformGizmoComponent::RotateZ:
    return EditorOverlayGizmoGeometryKind::RotationRing;
  case TransformGizmoComponent::ScaleX:
  case TransformGizmoComponent::ScaleY:
  case TransformGizmoComponent::ScaleZ:
    return EditorOverlayGizmoGeometryKind::ScaleAxis;
  case TransformGizmoComponent::ScaleUniform:
  case TransformGizmoComponent::None:
    return EditorOverlayGizmoGeometryKind::UniformScale;
  }
  return EditorOverlayGizmoGeometryKind::UniformScale;
}

template <typename Payload>
[[nodiscard]] bool
appendCommand(EditorOverlayFrame &frame, EditorOverlayDepthMode depthMode,
              EditorOverlayBlendMode blendMode, bool helperDepthTest,
              bool helperDepthWrite, Payload &&payload) noexcept {
  if (frame.drawCommandCount >= frame.drawCommands.size()) {
    return false;
  }
  EditorOverlayDrawCommand &command =
      frame.drawCommands[frame.drawCommandCount];
  command.order = static_cast<std::uint32_t>(frame.drawCommandCount);
  command.depthMode = depthMode;
  command.blendMode = blendMode;
  command.helperDepthTest = helperDepthTest;
  command.helperDepthWrite = helperDepthWrite;
  command.payload = std::forward<Payload>(payload);
  ++frame.drawCommandCount;
  return true;
}

[[nodiscard]] bool appendGrid(EditorOverlayFrame &frame,
                              const EditorOverlayGridInput &input) noexcept {
  const auto parameters = computeEditorGridParameters(input.math);
  if (!parameters) {
    return false;
  }

  if (!finite(input.math.cameraPosition) ||
      !validProjection(input.math.projection)) {
    return false;
  }

  EditorOverlayGridLinesCommand grid;
  grid.parameters = *parameters;
  grid.projection = input.math.projection;
  grid.cameraPosition = input.math.cameraPosition;
  glm::dvec2 overallMinimum(std::numeric_limits<double>::infinity());
  glm::dvec2 overallMaximum(-std::numeric_limits<double>::infinity());
  constexpr std::array perspectiveAlpha{0.4f, 0.3f, 0.2f};
  const double halfLineCount = static_cast<double>(parameters->lineCount / 2u);
  for (std::size_t index = 0u; index < kEditorGridVisibleLevelCount; ++index) {
    const double extent =
        static_cast<double>(parameters->visibleSteps[index]) * halfLineCount;
    const glm::dvec2 center(parameters->visibleCenters[index]);
    const auto minimum = checkedVector(center - glm::dvec2(extent));
    const auto maximum = checkedVector(center + glm::dvec2(extent));
    if (!minimum || !maximum) {
      return false;
    }
    grid.visibleLevels[index] = EditorOverlayGridLevel{
        parameters->visibleLevelIndices[index],
        parameters->visibleSteps[index],
        parameters->visibleCenters[index],
        *minimum,
        *maximum,
        perspectiveAlpha[index],
    };
    overallMinimum = glm::min(overallMinimum, glm::dvec2(*minimum));
    overallMaximum = glm::max(overallMaximum, glm::dvec2(*maximum));
  }
  const auto minimum = checkedVector(overallMinimum);
  const auto maximum = checkedVector(overallMaximum);
  if (!minimum || !maximum) {
    return false;
  }
  grid.clipMinimum = *minimum;
  grid.clipMaximum = *maximum;
  grid.vertexCount = 12u * parameters->lineCount;
  if (!appendCommand(frame, EditorOverlayDepthMode::SceneOccluded,
                     EditorOverlayBlendMode::Additive, false, false,
                     std::move(grid))) {
    return false;
  }

  EditorOverlayGridAxesCommand axes;
  axes.clipExtent = parameters->clipExtent;
  return appendCommand(frame, EditorOverlayDepthMode::AlwaysVisible,
                       EditorOverlayBlendMode::AlphaOver, false, false,
                       std::move(axes));
}

[[nodiscard]] bool appendGizmo(EditorOverlayFrame &frame,
                               const EditorOverlayGizmoInput &input) noexcept {
  if (!validVisibility(input.visibility) || !validComponent(input.hovered) ||
      !validComponent(input.active) || !validGizmoDepthMode(input.depthMode)) {
    return false;
  }
  if ((input.hovered != TransformGizmoComponent::None &&
       !componentVisible(input.hovered, input.visibility)) ||
      (input.active != TransformGizmoComponent::None &&
       !componentVisible(input.active, input.visibility))) {
    return false;
  }
  if (input.visibility == TransformGizmoVisibility::None) {
    return true;
  }

  const auto pixelScale = computePixelScale(frame.viewport, input);
  if (!pixelScale) {
    return false;
  }
  const auto model = makeGizmoModel(input.frame, pixelScale->worldScale);
  if (!model) {
    return false;
  }
  frame.gizmoPixelScale = *pixelScale;

  for (const TransformGizmoComponent component : kOrderedGizmoComponents) {
    if (!componentVisible(component, input.visibility)) {
      continue;
    }
    const EditorOverlayInteractionState interaction =
        component == input.active    ? EditorOverlayInteractionState::Active
        : component == input.hovered ? EditorOverlayInteractionState::Hovered
                                     : EditorOverlayInteractionState::Normal;
    const auto color = editorOverlayComponentColor(component, interaction);
    if (!color) {
      return false;
    }

    EditorOverlayGizmoHandleCommand handle;
    handle.component = component;
    handle.geometry = geometryFor(component);
    handle.interaction = interaction;
    handle.localAxis = transformGizmoComponentAxis(component);
    handle.color = *color;
    handle.localToWorld = *model;
    if (!finite(handle.localAxis) || !finite(handle.color) ||
        !appendCommand(frame, input.depthMode, EditorOverlayBlendMode::Opaque,
                       true, true, std::move(handle))) {
      return false;
    }
  }
  return true;
}

[[nodiscard]] std::optional<glm::vec4>
baseComponentColor(TransformGizmoComponent component) noexcept {
  switch (component) {
  case TransformGizmoComponent::TranslateX:
  case TransformGizmoComponent::RotateX:
  case TransformGizmoComponent::ScaleX:
    return glm::vec4(0.89f, 0.294f, 0.369f, 1.0f);
  case TransformGizmoComponent::TranslateY:
  case TransformGizmoComponent::RotateY:
  case TransformGizmoComponent::ScaleY:
    return glm::vec4(0.45f, 0.819f, 0.263f, 1.0f);
  case TransformGizmoComponent::TranslateZ:
  case TransformGizmoComponent::RotateZ:
  case TransformGizmoComponent::ScaleZ:
    return glm::vec4(0.25f, 0.451f, 0.851f, 1.0f);
  case TransformGizmoComponent::TranslateXY:
    return glm::vec4(0.78f, 0.78f, 0.25f, 0.45f);
  case TransformGizmoComponent::TranslateXZ:
    return glm::vec4(0.78f, 0.25f, 0.78f, 0.45f);
  case TransformGizmoComponent::TranslateYZ:
    return glm::vec4(0.25f, 0.78f, 0.78f, 0.45f);
  case TransformGizmoComponent::ScaleUniform:
    return glm::vec4(0.65f, 0.65f, 0.65f, 1.0f);
  case TransformGizmoComponent::None:
    return std::nullopt;
  }
  return std::nullopt;
}

} // namespace

std::optional<glm::vec4>
editorOverlayComponentColor(TransformGizmoComponent component,
                            EditorOverlayInteractionState state) noexcept {
  if (!validComponent(component) || !validInteractionState(state)) {
    return std::nullopt;
  }
  auto color = baseComponentColor(component);
  if (!color) {
    return std::nullopt;
  }
  if (state != EditorOverlayInteractionState::Normal) {
    for (glm::length_t channel = 0; channel < 3; ++channel) {
      const float bright = std::clamp((*color)[channel] * 2.5f, 0.0f, 1.0f);
      (*color)[channel] = (*color)[channel] * 0.1f + bright * 0.9f;
    }
  }
  return color;
}

std::optional<EditorOverlayFrame>
buildEditorOverlayFrame(const EditorOverlayInput &input) noexcept {
  if (!validViewport(input.viewport) || !finite(input.camera.viewProjection) ||
      !validDepthConvention(input.camera.depthConvention)) {
    return std::nullopt;
  }

  EditorOverlayFrame frame;
  frame.viewport = input.viewport;
  frame.viewProjection = input.camera.viewProjection;
  frame.depthConvention = input.camera.depthConvention;
  frame.scissor = input.viewport.physicalRenderArea;
  frame.physicalPixelsPerLogicalPixel = input.viewport.framebufferScale;
  const auto depthScale = checkedVector(glm::dvec2(
      static_cast<double>(input.viewport.sceneDepthExtent.width) /
          static_cast<double>(input.viewport.physicalRenderArea.extent.width),
      static_cast<double>(input.viewport.sceneDepthExtent.height) /
          static_cast<double>(
              input.viewport.physicalRenderArea.extent.height)));
  if (!depthScale || depthScale->x <= 0.0f || depthScale->y <= 0.0f) {
    return std::nullopt;
  }
  frame.sceneDepthPixelsPerPhysicalPixel = *depthScale;

  if (input.grid.visible && input.gizmo.visible &&
      input.gizmo.visibility != TransformGizmoVisibility::None &&
      input.grid.math.projection != input.gizmo.projection) {
    return std::nullopt;
  }
  if (input.grid.visible && !appendGrid(frame, input.grid)) {
    return std::nullopt;
  }
  if (input.gizmo.visible && !appendGizmo(frame, input.gizmo)) {
    return std::nullopt;
  }
  return frame;
}

} // namespace demo::editor
