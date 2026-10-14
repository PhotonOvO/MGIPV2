#pragma once

#include "render/editor/EditorViewportMetrics.h"

#include <array>
#include <compare>
#include <cstddef>
#include <cstdint>
#include <type_traits>

namespace demo::editor {

struct OverlayVec2 final {
  double x{0.0};
  double y{0.0};

  auto operator<=>(const OverlayVec2 &) const = default;
};

struct OverlayVec3 final {
  double x{0.0};
  double y{0.0};
  double z{0.0};

  auto operator<=>(const OverlayVec3 &) const = default;
};

struct OverlayQuaternion final {
  double x{0.0};
  double y{0.0};
  double z{0.0};
  double w{1.0};

  auto operator<=>(const OverlayQuaternion &) const = default;
};

// Row-major matrix operating on column vectors. Translation is stored in
// elements 3, 7, and 11.
struct OverlayMatrix4 final {
  std::array<double, 16> elements{
      1.0, 0.0, 0.0, 0.0,
      0.0, 1.0, 0.0, 0.0,
      0.0, 0.0, 1.0, 0.0,
      0.0, 0.0, 0.0, 1.0};

  auto operator<=>(const OverlayMatrix4 &) const = default;
};

struct OverlayBounds final {
  OverlayVec3 minimum{};
  OverlayVec3 maximum{};

  auto operator<=>(const OverlayBounds &) const = default;
};

enum class GizmoComponent : std::uint8_t {
  translateX = 0,
  translateY,
  translateZ,
  translateXY,
  translateXZ,
  translateYZ,
  rotateX,
  rotateY,
  rotateZ,
  scaleX,
  scaleY,
  scaleZ,
  uniformScale,
  none = 0xffu,
};

inline constexpr std::size_t kGizmoComponentCount = 13u;
inline constexpr std::array<GizmoComponent, kGizmoComponentCount>
    kGizmoComponentOrder{
        GizmoComponent::translateX,  GizmoComponent::translateY,
        GizmoComponent::translateZ,  GizmoComponent::translateXY,
        GizmoComponent::translateXZ, GizmoComponent::translateYZ,
        GizmoComponent::rotateX,     GizmoComponent::rotateY,
        GizmoComponent::rotateZ,     GizmoComponent::scaleX,
        GizmoComponent::scaleY,      GizmoComponent::scaleZ,
        GizmoComponent::uniformScale};

[[nodiscard]] constexpr std::size_t
gizmoComponentIndex(GizmoComponent component) noexcept {
  const auto value = static_cast<std::uint8_t>(component);
  return value < kGizmoComponentCount ? static_cast<std::size_t>(value)
                                      : kGizmoComponentCount;
}

enum class GizmoModelToken : std::uint8_t {
  translate = 0,
  rotate,
  scale,
};

enum class GizmoBasisToken : std::uint8_t {
  global = 0,
  parentLocal,
};

enum class GizmoColorToken : std::uint8_t {
  xAxis = 0,
  yAxis,
  zAxis,
  xyPlane,
  xzPlane,
  yzPlane,
  uniform,
};

enum class GizmoVisualStateToken : std::uint8_t {
  normal = 0,
  hovered,
  active,
};

enum class GizmoDepthToken : std::uint8_t {
  xRay = 0,
  occlusionDither,
};

enum class GizmoPlaneToken : std::uint8_t {
  checkerDiscard = 0,
};

enum class GizmoSnapToken : std::uint8_t {
  disabled = 0,
  enabled,
};

struct OverlayGenerationBinding final {
  std::uint64_t viewportGeneration{0};
  EditorProjectionIdentity projection{};
  EditorViewIdentity view{};

  auto operator<=>(const OverlayGenerationBinding &) const = default;
};

struct GizmoStableNodeId final {
  std::uint64_t generation{0};
  std::uint64_t value{0};

  [[nodiscard]] constexpr bool valid() const noexcept {
    return generation != 0 && value != 0;
  }

  auto operator<=>(const GizmoStableNodeId &) const = default;
};

struct GizmoComponentRangePacket final {
  GizmoComponent component{GizmoComponent::none};
  std::uint32_t firstVertex{0};
  std::uint32_t vertexCount{0};
  std::uint32_t firstIndex{0};
  std::uint32_t indexCount{0};
  OverlayBounds localBounds{};

  auto operator<=>(const GizmoComponentRangePacket &) const = default;
};

struct GizmoComponentDrawPacket final {
  GizmoComponentRangePacket range{};
  OverlayMatrix4 model{};
  GizmoModelToken modelToken{GizmoModelToken::translate};
  GizmoColorToken color{GizmoColorToken::uniform};
  GizmoVisualStateToken state{GizmoVisualStateToken::normal};
  bool visible{false};

  auto operator<=>(const GizmoComponentDrawPacket &) const = default;
};

struct GizmoDrawPacket final {
  OverlayGenerationBinding generation{};
  GizmoBasisToken basis{GizmoBasisToken::global};
  GizmoDepthToken depth{GizmoDepthToken::xRay};
  GizmoPlaneToken plane{GizmoPlaneToken::checkerDiscard};
  GizmoSnapToken snap{GizmoSnapToken::disabled};
  double worldScale{0.0};
  double canonicalVisualDiameter{0.0};
  std::array<GizmoComponentDrawPacket, kGizmoComponentCount> components{};

  auto operator<=>(const GizmoDrawPacket &) const = default;
};

enum class GridLineKind : std::uint8_t {
  parallelX = 0,
  parallelZ,
  xAxis,
  zAxis,
};

enum class GridBlendToken : std::uint8_t {
  additive = 0,
  alphaOver,
};

struct GridLinePacket final {
  OverlayVec3 start{};
  OverlayVec3 end{};
  GridLineKind kind{GridLineKind::parallelX};
  GridBlendToken blend{GridBlendToken::additive};
  std::uint8_t lodIndex{0};
  double edgeAlpha{0.0};
  double angleAlpha{0.0};
  double distanceAlpha{0.0};
  double lodAlpha{0.0};
  double alpha{0.0};

  auto operator<=>(const GridLinePacket &) const = default;
};

struct GridLodPacket final {
  std::uint8_t levelIndex{0};
  double step{0.0};
  OverlayVec2 center{};
  double alpha{0.0};

  auto operator<=>(const GridLodPacket &) const = default;
};

struct GridDrawPacket final {
  OverlayGenerationBinding generation{};
  double baseUnit{0.0};
  double distanceToFloor{0.0};
  OverlayVec2 center{};
  std::array<double, 4> decadeSteps{};
  std::array<GridLodPacket, 3> activeLods{};
  std::uint32_t generatedLineCount{0};
  bool enabled{false};

  auto operator<=>(const GridDrawPacket &) const = default;
};

enum class GizmoActionKind : std::uint8_t {
  none = 0,
  beginPreview,
  updatePreview,
  commitPreviewOnce,
  detachRetainingPreview,
  clearSelection,
  latchRetainingPreview,
  releaseCapture,
};

enum class GizmoTerminalToken : std::uint8_t {
  none = 0,
  commitHistoryOnce,
  noHistory,
};

struct GizmoAction final {
  GizmoActionKind kind{GizmoActionKind::none};
  GizmoTerminalToken terminal{GizmoTerminalToken::none};
  std::uint64_t transactionId{0};
  GizmoStableNodeId nodeId{};
  std::uint64_t documentRevision{0};
  std::uint64_t selectionRevision{0};
  GizmoComponent component{GizmoComponent::none};
  OverlayVec3 translation{};
  OverlayQuaternion rotation{};
  OverlayVec3 scale{1.0, 1.0, 1.0};

  auto operator<=>(const GizmoAction &) const = default;
};

static_assert(std::is_trivially_copyable_v<OverlayVec3>);
static_assert(std::is_trivially_copyable_v<OverlayMatrix4>);
static_assert(std::is_trivially_copyable_v<GizmoDrawPacket>);
static_assert(std::is_trivially_copyable_v<GridLinePacket>);
static_assert(std::is_trivially_copyable_v<GizmoAction>);

} // namespace demo::editor
