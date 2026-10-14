#pragma once

#include "render/editor/EditorOverlayTypes.h"
#include "render/editor/GizmoGeometry.h"

#include <array>
#include <cstdint>
#include <optional>

namespace demo::editor {

inline constexpr double kGizmoParallelEpsilon = 1.0e-6;
inline constexpr double kGizmoParentConditionLimit = 1.0e10;
inline constexpr double kGizmoScaleSensitivity = 0.005;
inline constexpr double kGizmoMinimumScaleFactor = 0.01;

enum class GizmoVisibility : std::uint8_t {
  none = 0,
  translation = 1u << 0u,
  rotation = 1u << 1u,
  scale = 1u << 2u,
  all = (1u << 0u) | (1u << 1u) | (1u << 2u),
};

[[nodiscard]] constexpr GizmoVisibility operator|(GizmoVisibility left,
                                                   GizmoVisibility right) noexcept {
  return static_cast<GizmoVisibility>(static_cast<std::uint8_t>(left) |
                                      static_cast<std::uint8_t>(right));
}

[[nodiscard]] constexpr bool hasGizmoVisibility(GizmoVisibility value,
                                                 GizmoVisibility flag) noexcept {
  return (static_cast<std::uint8_t>(value) &
          static_cast<std::uint8_t>(flag)) != 0u;
}

struct GizmoRay final {
  OverlayVec3 origin{};
  OverlayVec3 direction{0.0, 0.0, -1.0};

  auto operator<=>(const GizmoRay &) const = default;
};

struct GizmoRayPlaneHit final {
  double distance{0.0};
  OverlayVec3 position{};

  auto operator<=>(const GizmoRayPlaneHit &) const = default;
};

enum class GizmoHitKind : std::uint8_t {
  none = 0,
  axisShaftOrTip,
  translationPlane,
  rotationRing,
  uniformCenter,
};

struct GizmoHitStyle final {
  double axisPickRadius{0.6};
  double translationPlaneTolerance{0.2};
  double rotationRingTolerance{0.2};
  double scaleAxisPickRadius{0.6};
  double uniformCenterPickHalfExtent{0.6};

  auto operator<=>(const GizmoHitStyle &) const = default;
};

struct GizmoHit final {
  GizmoComponent component{GizmoComponent::none};
  GizmoHitKind kind{GizmoHitKind::none};
  double rayDistance{0.0};
  double missDistance{0.0};

  [[nodiscard]] constexpr bool hit() const noexcept {
    return component != GizmoComponent::none;
  }

  auto operator<=>(const GizmoHit &) const = default;
};

struct GizmoLocalTransform final {
  OverlayVec3 translation{};
  OverlayQuaternion rotation{};
  OverlayVec3 scale{1.0, 1.0, 1.0};

  auto operator<=>(const GizmoLocalTransform &) const = default;
};

struct GizmoAttachment final {
  GizmoStableNodeId nodeId{};
  GizmoLocalTransform local{};
  OverlayMatrix4 parentWorld{};
  std::uint64_t documentRevision{0};
  std::uint64_t selectionRevision{0};

  auto operator<=>(const GizmoAttachment &) const = default;
};

struct GizmoBasis final {
  GizmoBasisToken token{GizmoBasisToken::global};
  OverlayVec3 pivotWorld{};
  std::array<OverlayVec3, 3> axes{
      OverlayVec3{1.0, 0.0, 0.0}, OverlayVec3{0.0, 1.0, 0.0},
      OverlayVec3{0.0, 0.0, 1.0}};
  double parentDeterminant{1.0};
  double parentConditionEstimate{1.0};

  auto operator<=>(const GizmoBasis &) const = default;
};

struct GizmoTranslationSolution final {
  OverlayVec3 worldDelta{};
  OverlayVec3 localDelta{};
  OverlayVec3 resultingLocalPosition{};

  auto operator<=>(const GizmoTranslationSolution &) const = default;
};

struct GizmoRotationSolution final {
  double degrees{0.0};
  OverlayVec3 axisWorld{};
  OverlayQuaternion deltaLocal{};
  OverlayQuaternion resultingLocalRotation{};

  auto operator<=>(const GizmoRotationSolution &) const = default;
};

struct GizmoScaleSolution final {
  double factor{1.0};
  OverlayVec3 resultingLocalScale{1.0, 1.0, 1.0};

  auto operator<=>(const GizmoScaleSolution &) const = default;
};

struct GizmoConstantScale final {
  OverlayGenerationBinding generation{};
  double logicalPixels{0.0};
  double worldUnitsPerLogicalPixel{0.0};
  double worldScale{0.0};

  auto operator<=>(const GizmoConstantScale &) const = default;
};

struct GizmoCameraSnapshot final {
  OverlayGenerationBinding generation{};
  OverlayMatrix4 inverseViewProjection{};
  // Vulkan's positive-height viewport maps increasing NDC Y down the screen.
  bool ndcYDown{false};

  auto operator<=>(const GizmoCameraSnapshot &) const = default;
};

struct GizmoViewDepthSnapshot final {
  OverlayGenerationBinding generation{};
  double positiveViewDepth{0.0};

  auto operator<=>(const GizmoViewDepthSnapshot &) const = default;
};

struct GizmoDrawSettings final {
  bool visible{true};
  bool snapEnabled{false};
  bool occlusionDither{false};
  GizmoBasisToken basis{GizmoBasisToken::global};
  GizmoVisibility visibility{GizmoVisibility::all};
  GizmoComponent hovered{GizmoComponent::none};
  GizmoComponent active{GizmoComponent::none};

  auto operator<=>(const GizmoDrawSettings &) const = default;
};

[[nodiscard]] bool finiteOverlayVector(OverlayVec3 value) noexcept;
[[nodiscard]] bool finiteOverlayMatrix(const OverlayMatrix4 &value) noexcept;
[[nodiscard]] bool validGizmoAttachment(const GizmoAttachment &value) noexcept;

[[nodiscard]] std::optional<OverlayMatrix4>
invertOverlayMatrix(const OverlayMatrix4 &value) noexcept;

[[nodiscard]] std::optional<OverlayVec3>
transformOverlayPoint(const OverlayMatrix4 &matrix,
                      OverlayVec3 point) noexcept;

[[nodiscard]] std::optional<OverlayVec3>
transformOverlayDirection(const OverlayMatrix4 &matrix,
                          OverlayVec3 direction) noexcept;

[[nodiscard]] std::optional<GizmoRay>
makeGizmoRay(LogicalViewportPoint pointer,
             const EditorViewportMetrics &metrics,
             const GizmoCameraSnapshot &camera) noexcept;

[[nodiscard]] std::optional<GizmoRayPlaneHit>
intersectGizmoRayPlane(const GizmoRay &ray, OverlayVec3 planePoint,
                       OverlayVec3 planeNormal,
                       double parallelEpsilon = kGizmoParallelEpsilon) noexcept;

[[nodiscard]] GizmoHit classifyGizmoHit(
    const GizmoRay &localRay, const GizmoGeometryAtlas &geometry,
    const GizmoHitStyle &hitStyle = {},
    GizmoVisibility visibility = GizmoVisibility::all) noexcept;

[[nodiscard]] std::optional<GizmoBasis>
deriveGizmoBasis(const GizmoAttachment &attachment,
                 GizmoBasisToken basis = GizmoBasisToken::global) noexcept;

[[nodiscard]] std::optional<OverlayVec3>
worldToParentLocalDirection(const GizmoAttachment &attachment,
                            OverlayVec3 worldDirection) noexcept;

[[nodiscard]] std::optional<OverlayVec3>
parentLocalToWorldDirection(const GizmoAttachment &attachment,
                            OverlayVec3 localDirection) noexcept;

[[nodiscard]] std::optional<GizmoTranslationSolution>
solveGizmoTranslation(GizmoComponent component, const GizmoRay &startRay,
                      const GizmoRay &currentRay,
                      OverlayVec3 viewDirectionWorld,
                      const GizmoAttachment &attachment,
                      GizmoBasisToken basis = GizmoBasisToken::global,
                      std::optional<double> snapStep = std::nullopt) noexcept;

[[nodiscard]] std::optional<GizmoRotationSolution>
solveGizmoRotation(GizmoComponent component, const GizmoRay &startRay,
                   const GizmoRay &currentRay,
                   const GizmoAttachment &attachment,
                   GizmoBasisToken basis = GizmoBasisToken::global,
                   std::optional<double> snapDegrees = std::nullopt) noexcept;

[[nodiscard]] std::optional<GizmoScaleSolution>
solveGizmoScale(GizmoComponent component, OverlayVec3 startingScale,
                OverlayVec2 startPointer, OverlayVec2 currentPointer,
                std::optional<double> snapStep = std::nullopt,
                double sensitivity = kGizmoScaleSensitivity,
                double minimumFactor = kGizmoMinimumScaleFactor) noexcept;

[[nodiscard]] std::optional<double>
snapGizmoScalar(double value, double step) noexcept;

[[nodiscard]] std::optional<OverlayVec3>
snapGizmoVector(OverlayVec3 value, double step) noexcept;

[[nodiscard]] std::optional<GizmoConstantScale>
computeGizmoConstantScale(const EditorViewportMetrics &metrics,
                          const GizmoViewDepthSnapshot &viewDepth,
                          double canonicalVisualDiameter,
                          double nominalLogicalPixels = 50.0) noexcept;

[[nodiscard]] std::optional<GizmoDrawPacket>
buildGizmoDrawPacket(const GizmoGeometryAtlas &geometry,
                     const GizmoAttachment &attachment,
                     const EditorViewportMetrics &metrics,
                     const GizmoViewDepthSnapshot &viewDepth,
                     const GizmoDrawSettings &settings = {}) noexcept;

enum class GizmoInteractionPhase : std::uint8_t {
  detached = 0,
  attachedIdle,
  dragging,
  draggingLatched,
};

struct GizmoAttachmentFence final {
  GizmoStableNodeId nodeId{};
  std::uint64_t documentRevision{0};
  std::uint64_t selectionRevision{0};

  auto operator<=>(const GizmoAttachmentFence &) const = default;
};

struct GizmoInteractionState final {
  GizmoInteractionPhase phase{GizmoInteractionPhase::detached};
  GizmoAttachmentFence fence{};
  std::uint64_t activeTransactionId{0};
  std::uint64_t lastTerminalTransactionId{0};
  GizmoComponent activeComponent{GizmoComponent::none};
  GizmoLocalTransform retainedPreview{};
  bool hasRetainedPreview{false};
  bool captureHeld{false};

  auto operator<=>(const GizmoInteractionState &) const = default;
};

enum class GizmoTerminalEventKind : std::uint8_t {
  deliveredRelease = 0,
  escapePressed,
  focusLost,
  suppressedPhysicalMouseUp,
};

struct GizmoTerminalEvent final {
  GizmoTerminalEventKind kind{GizmoTerminalEventKind::deliveredRelease};
  std::uint64_t sequence{0};

  auto operator<=>(const GizmoTerminalEvent &) const = default;
};

struct GizmoTerminalBatch final {
  std::array<GizmoTerminalEvent, 8> events{};
  std::uint8_t count{0};

  auto operator<=>(const GizmoTerminalBatch &) const = default;
};

struct GizmoActionResolution final {
  GizmoInteractionState state{};
  std::array<GizmoAction, 4> actions{};
  std::uint8_t actionCount{0};

  auto operator<=>(const GizmoActionResolution &) const = default;
};

[[nodiscard]] GizmoInteractionState
attachGizmo(const GizmoAttachment &attachment) noexcept;

[[nodiscard]] GizmoActionResolution
beginGizmoDrag(const GizmoInteractionState &state,
               const GizmoAttachment &attachment,
               std::uint64_t transactionId,
               GizmoComponent component) noexcept;

[[nodiscard]] GizmoActionResolution
updateGizmoPreview(const GizmoInteractionState &state,
                   const GizmoAttachment &attachment,
                   const GizmoLocalTransform &preview) noexcept;

[[nodiscard]] GizmoActionResolution
resolveGizmoTerminalBatch(const GizmoInteractionState &state,
                          const GizmoTerminalBatch &batch) noexcept;

} // namespace demo::editor
