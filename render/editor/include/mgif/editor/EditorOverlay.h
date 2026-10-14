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

#include <mgif/editor/EditorGridMath.h>
#include <mgif/editor/TransformGizmoMath.h>

#include <glm/glm.hpp>

#include <array>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <span>
#include <variant>

namespace demo::editor {

inline constexpr std::size_t kEditorOverlayGizmoHandleCount = 13u;
inline constexpr std::size_t kEditorOverlayMaximumDrawCommandCount =
    kEditorOverlayGizmoHandleCount + 2u;
inline constexpr float kEditorOverlayNominalGizmoLogicalPixels = 50.0f;
inline constexpr float kEditorOverlayGridLineWidthLogicalPixels = 1.5f;

struct EditorOverlayPixelExtent final {
  std::uint32_t width{0u};
  std::uint32_t height{0u};
};

struct EditorOverlayPixelOffset final {
  std::uint32_t x{0u};
  std::uint32_t y{0u};
};

struct EditorOverlayPixelRect final {
  EditorOverlayPixelOffset origin{};
  EditorOverlayPixelExtent extent{};
};

// The logical and physical values describe the same viewport snapshot.
// physicalRenderArea is also the exact viewport/scissor rectangle a later
// render pass should use. The scene depth target may have a different extent.
struct EditorOverlayViewport final {
  glm::vec2 logicalExtent{0.0f};
  glm::vec2 framebufferScale{1.0f};
  EditorOverlayPixelRect physicalRenderArea{};
  EditorOverlayPixelExtent colorTargetExtent{};
  EditorOverlayPixelExtent sceneDepthExtent{};
};

enum class EditorOverlayDepthMode : std::uint8_t {
  // Grid fragments behind the sampled scene depth are discarded.
  SceneOccluded = 0,
  // The scene depth is ignored, as for the colored grid axes.
  AlwaysVisible,
  // Gizmo fragments behind the scene are discarded; helper depth still
  // controls gizmo self-occlusion through the command flags.
  DepthTested,
  // Occluded gizmo fragments remain solid.
  XRaySolid,
  // Occluded gizmo fragments use a deterministic checker mask.
  XRayDithered,
};

enum class EditorOverlayBlendMode : std::uint8_t {
  Opaque = 0,
  Additive,
  AlphaOver,
};

enum class EditorOverlayDepthConvention : std::uint8_t {
  Forward = 0,
  Reverse,
};

enum class EditorOverlayInteractionState : std::uint8_t {
  Normal = 0,
  Hovered,
  Active,
};

enum class EditorOverlayGizmoGeometryKind : std::uint8_t {
  TranslationAxis = 0,
  TranslationPlane,
  RotationRing,
  ScaleAxis,
  UniformScale,
};

// Dimensions are in canonical gizmo-local units. A geometry atlas can be
// generated once from this stable description and selected by command kind.
struct EditorOverlayGizmoGeometryStyle final {
  float shaftRadius{0.02f};
  float shaftLength{1.0f};
  float coneRadius{0.06f};
  float coneHeight{0.25f};
  std::uint32_t coneSegments{16u};
  float ringRadius{1.3f};
  float ringTubeRadius{0.02f};
  std::uint32_t ringMajorSegments{64u};
  std::uint32_t ringMinorSegments{16u};
  float scaleCubeSize{0.15f};
  float scaleAxisLength{1.67f};
  float uniformScaleMultiplier{2.0f};
  float planeSize{0.22f};
  float planeOffset{0.5f};
  float planeAlpha{0.45f};
};

struct EditorOverlayGridInput final {
  bool visible{false};
  EditorGridMathInput math{};
};

struct EditorOverlayCameraInput final {
  glm::mat4 viewProjection{1.0f};
  EditorOverlayDepthConvention depthConvention{
      EditorOverlayDepthConvention::Reverse};
};

struct EditorOverlayGizmoInput final {
  bool visible{false};
  TransformGizmoFrame frame{};
  TransformGizmoVisibility visibility{TransformGizmoVisibility::All};
  TransformGizmoComponent hovered{TransformGizmoComponent::None};
  TransformGizmoComponent active{TransformGizmoComponent::None};
  EditorGridProjection projection{EditorGridProjection::Perspective};

  // Positive distance from the camera to the pivot. Orthographic scaling is
  // distance-independent, but a positive finite value still proves the pivot
  // is in front of the camera snapshot used by this packet.
  float viewSpaceDepth{1.0f};
  float projectionScaleY{1.0f};
  float nominalLogicalPixels{kEditorOverlayNominalGizmoLogicalPixels};
  EditorOverlayDepthMode depthMode{EditorOverlayDepthMode::XRaySolid};
};

struct EditorOverlayInput final {
  EditorOverlayViewport viewport{};
  EditorOverlayCameraInput camera{};
  EditorOverlayGridInput grid{};
  EditorOverlayGizmoInput gizmo{};
};

struct EditorOverlayGizmoPixelScale final {
  float targetLogicalPixels{0.0f};
  float targetPhysicalPixelsY{0.0f};
  float worldUnitsPerPhysicalPixelY{0.0f};
  float worldScale{0.0f};
};

struct EditorOverlayGridLevel final {
  std::uint32_t levelIndex{0u};
  float step{0.0f};
  glm::vec2 center{0.0f};
  glm::vec2 clipMinimum{0.0f};
  glm::vec2 clipMaximum{0.0f};
  float perspectiveAlpha{0.0f};
};

struct EditorOverlayGridLinesCommand final {
  EditorGridParameters parameters{};
  EditorGridProjection projection{EditorGridProjection::Perspective};
  glm::vec3 cameraPosition{0.0f};
  glm::vec2 clipMinimum{0.0f};
  glm::vec2 clipMaximum{0.0f};
  std::array<EditorOverlayGridLevel, kEditorGridVisibleLevelCount>
      visibleLevels{};
  glm::vec4 minorColor{0.5f, 0.5f, 0.5f, 1.0f};
  glm::vec4 majorColor{0.5f, 0.5f, 0.5f, 1.0f};
  float lineWidthLogicalPixels{kEditorOverlayGridLineWidthLogicalPixels};
  std::uint32_t vertexCount{0u};
};

struct EditorOverlayGridAxesCommand final {
  glm::vec2 clipExtent{0.0f};
  std::array<glm::vec4, 3u> colors{glm::vec4(0.619f, 0.235f, 0.290f, 1.0f),
                                   glm::vec4(0.357f, 0.482f, 0.185f, 1.0f),
                                   glm::vec4(0.231f, 0.357f, 0.518f, 1.0f)};
  float lineWidthLogicalPixels{kEditorOverlayGridLineWidthLogicalPixels};
  std::uint32_t vertexCount{6u};
};

struct EditorOverlayGizmoHandleCommand final {
  TransformGizmoComponent component{TransformGizmoComponent::None};
  EditorOverlayGizmoGeometryKind geometry{
      EditorOverlayGizmoGeometryKind::TranslationAxis};
  EditorOverlayInteractionState interaction{
      EditorOverlayInteractionState::Normal};
  glm::vec3 localAxis{0.0f};
  glm::vec4 color{0.0f};
  glm::mat4 localToWorld{1.0f};
};

using EditorOverlayDrawPayload =
    std::variant<EditorOverlayGridLinesCommand, EditorOverlayGridAxesCommand,
                 EditorOverlayGizmoHandleCommand>;

struct EditorOverlayDrawCommand final {
  std::uint32_t order{0u};
  EditorOverlayDepthMode depthMode{EditorOverlayDepthMode::SceneOccluded};
  EditorOverlayBlendMode blendMode{EditorOverlayBlendMode::Opaque};
  bool helperDepthTest{false};
  bool helperDepthWrite{false};
  EditorOverlayDrawPayload payload{};
};

struct EditorOverlayFrame final {
  EditorOverlayViewport viewport{};
  glm::mat4 viewProjection{1.0f};
  EditorOverlayDepthConvention depthConvention{
      EditorOverlayDepthConvention::Reverse};
  EditorOverlayPixelRect scissor{};
  glm::vec2 physicalPixelsPerLogicalPixel{0.0f};
  glm::vec2 sceneDepthPixelsPerPhysicalPixel{0.0f};
  std::optional<EditorOverlayGizmoPixelScale> gizmoPixelScale;
  EditorOverlayGizmoGeometryStyle gizmoGeometry{};
  std::array<EditorOverlayDrawCommand, kEditorOverlayMaximumDrawCommandCount>
      drawCommands{};
  std::size_t drawCommandCount{0u};

  [[nodiscard]] std::span<const EditorOverlayDrawCommand>
  commands() const noexcept {
    return std::span<const EditorOverlayDrawCommand>(drawCommands.data(),
                                                     drawCommandCount);
  }
};

// Active is a distinct state token but deliberately uses the same visual
// highlight as hover. This avoids inventing a second active palette.
[[nodiscard]] std::optional<glm::vec4>
editorOverlayComponentColor(TransformGizmoComponent component,
                            EditorOverlayInteractionState state =
                                EditorOverlayInteractionState::Normal) noexcept;

// Invalid or internally inconsistent snapshots return no packet. Disabled
// grid/gizmo inputs are ignored, allowing callers to keep inactive settings.
[[nodiscard]] std::optional<EditorOverlayFrame>
buildEditorOverlayFrame(const EditorOverlayInput &input) noexcept;

} // namespace demo::editor
